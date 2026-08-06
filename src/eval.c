#include "yas.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* Forward declaration */

struct EvalRoot {
	YasAtom *expr;
	YasAtom *env;
	YasAtom *stack;
	/* eval_vm()'s output slot. A builtin/native caller re-entering the
	 * evaluator (yas_eval_expr() from TRY/CATCH/FINALLY/INCLUDE, etc.)
	 * routinely holds the *previous* nested call's result live across
	 * the *next* one - unrooted otherwise, since nothing but this
	 * struct can see a plain C `YasAtom result` local. Rooting it here
	 * closes that whole class at once instead of requiring every call
	 * site to push/pop its own result slot. Must be nulled-out (yas_nil)
	 * by eval_vm() before this root is installed - callers pass
	 * uninitialized storage. */
	YasAtom *result;
	struct EvalRoot *prev;
};

static struct EvalRoot *eval_roots = NULL;
static int eval_vm(YasAtom expr, YasAtom env, YasAtom *result, struct EvalRoot *root);

/* Called from data.c's gc_mark_roots() - the live evaluator's roots are
 * pointers to eval_vm()'s own C locals (expr/env/stack/result), not to
 * anything an embedder or a nested yas_eval_expr() caller can see, so
 * they stay a separate root source from the yas_gc_push_root() shadow
 * stack. Two root sources, one collector. */
void yas_eval_mark_roots(void)
{
	struct EvalRoot *r = eval_roots;
	while (r) {
		yas_gc_mark(*r->expr);
		yas_gc_mark(*r->env);
		if (!yas_nilp(*r->stack)) yas_gc_mark(*r->stack);
		yas_gc_mark(*r->result);
		r = r->prev;
	}
}

static int eval_do_return(YasAtom *stack, YasAtom *expr, YasAtom *env, YasAtom *result);
static int eval_do_exec(YasAtom *stack, YasAtom *expr, YasAtom *env);
static int eval_do_bind(YasAtom *stack, YasAtom *expr, YasAtom *env);
static int eval_do_apply(YasAtom *stack, YasAtom *expr, YasAtom *env, YasAtom *result);

YasAtom yas_env_create(YasAtom parent)
{
	return yas_cons(parent, yas_nil);
}

int yas_env_define(YasAtom env, YasAtom symbol, YasAtom value)
{
	YasAtom bs = yas_cdr(env);

	while (!yas_nilp(bs)) {
		YasAtom b = yas_car(bs);
		if (yas_car(b).value.symbol == symbol.value.symbol) {
			yas_set_cdr(b, value);
			return YAS_ERROR_OK;
		}
		bs = yas_cdr(bs);
	}

	yas_set_cdr(env, yas_cons(yas_cons(symbol, value), yas_cdr(env)));

	return YAS_ERROR_OK;
}

int yas_env_get(YasAtom env, YasAtom symbol, YasAtom *result)
{
	YasAtom parent = yas_car(env);
	YasAtom bs = yas_cdr(env);

	while (!yas_nilp(bs)) {
		YasAtom b = yas_car(bs);
		if (yas_car(b).value.symbol == symbol.value.symbol) {
			*result = yas_cdr(b);
			return YAS_ERROR_OK;
		}
		bs = yas_cdr(bs);
	}

	if (yas_nilp(parent))
		return YAS_ERROR_UNBOUND;

	return yas_env_get(parent, symbol, result);
}

YasAtom yas_ctx_exception = { YAS_ATOM_TYPE_NIL };

/* Current file being evaluated (for relative include paths) */
const char *yas_current_file_path = NULL;

/* Resolve a path relative to the current file */
static char *resolve_include_path(const char *include_path)
{
	/* If absolute path or no current file, use as-is */
	if (include_path[0] == '/' || !yas_current_file_path) {
		char *result = malloc(strlen(include_path) + 1);
		strcpy(result, include_path);
		return result;
	}
	
	/* Find directory of current file */
	const char *last_slash = strrchr(yas_current_file_path, '/');
	if (!last_slash) {
		/* Current file has no directory component */
		char *result = malloc(strlen(include_path) + 1);
		strcpy(result, include_path);
		return result;
	}
	
	/* Build path: directory + "/" + include_path */
	size_t dir_len = last_slash - yas_current_file_path + 1;
	size_t total_len = dir_len + strlen(include_path) + 1;
	char *result = malloc(total_len);
	strncpy(result, yas_current_file_path, dir_len);
	result[dir_len] = '\0';
	strcat(result, include_path);
	return result;
}

/* Helper to evaluate include - reads file, parses, evaluates all exprs */
/* Evaluate source string */
int yas_eval_source(const char *text, const char *path, YasAtom env, YasAtom *result)
{
	const char *saved_path = yas_current_file_path;
	yas_current_file_path = path;
	
	const char *p = text;
	YasAtom expr;
	YasErrorContext err = {0};
	err.pos.filename = path;
	err.pos.source = text;
	*result = yas_nil;

	/* `env` and `*result` both span the yas_eval_expr() call below, which
	 * can trigger a nested GC step. For every caller in module.c this
	 * `env` is a bare yas_env_create(yas_nil) temporary that nothing
	 * else references - reachable only transiently as the nested
	 * eval_vm's own `env`, which gets reassigned on closure entry - so
	 * an import that triggers a collection mid-load can free the
	 * top-level env out from under the rest of this loop. `*result`
	 * holds the *previous* form's value, live across the *next*
	 * iteration's call and equally unreachable otherwise (it is not the
	 * nested eval_vm's own result slot - that's `&ignore` below, rooted
	 * separately per call). One root each covers every iteration, since
	 * yas_gc_push_root() tracks the slot's address, not a snapshot. */
	yas_gc_push_root(&env);
	yas_gc_push_root(result);

	while (1) {
		err.code = YAS_ERROR_OK;
		err.message[0] = '\0';

		YasError read_err = yas_read_expr(p, &p, &expr, &err);
		if (read_err == YAS_ERROR_SKIP) {
			/* Feature conditional excluded this expression, continue */
			continue;
		}
		if (read_err) {
			if (err.message[0] != '\0') {
				yas_error_print(&err);
				yas_current_file_path = saved_path;
				yas_gc_pop_roots(2);
				return YAS_ERROR_SYNTAX;
			}
			break;  /* End of file */
		}

		YasAtom ignore;
		YasError eval_err = yas_eval_expr(expr, env, &ignore);
		if (eval_err) {
			yas_current_file_path = saved_path;
			yas_gc_pop_roots(2);
			return eval_err;
		}
		*result = ignore; // result is last expr
	}

	yas_current_file_path = saved_path;
	yas_gc_pop_roots(2);
	return YAS_ERROR_OK;
}

/* Helper to evaluate include - reads file, parses, evaluates all exprs */
int yas_eval_file(const char *path, YasAtom env, YasAtom *result, int case_insensitive)
{
	char *resolved = resolve_include_path(path);
	char *text = yas_slurp(resolved);
	
	if (!text) {
		free(resolved);
		return yas_raise_error("file", "Cannot open file for include");
	}
	
	int err = yas_eval_source(text, resolved, env, result);
	
	free(text);
	free(resolved);
	return err;
}

int yas_env_set(YasAtom env, YasAtom symbol, YasAtom value)
{
	YasAtom parent = yas_car(env);
	YasAtom bs = yas_cdr(env);

	while (!yas_nilp(bs)) {
		YasAtom b = yas_car(bs);
		if (yas_car(b).value.symbol == symbol.value.symbol) {
			yas_set_cdr(b, value);
			return YAS_ERROR_OK;
		}
		bs = yas_cdr(bs);
	}

	if (yas_nilp(parent))
		return YAS_ERROR_UNBOUND;

	return yas_env_set(parent, symbol, value);
}

static int make_closure(YasAtom env, YasAtom args, YasAtom body, YasAtom *result)
{
	YasAtom p;

	if (!yas_listp(body))
		return YAS_ERROR_SYNTAX;

	/* Check argument names are valid:
	 * - Symbol (regular arg or rest)
	 * - Keyword (keyword arg marker)
	 * - Pair with symbol car ([name default])
	 */
	p = args;
	while (!yas_nilp(p)) {
		if (p.type == YAS_ATOM_TYPE_SYMBOL)
			break; /* Rest arg */
		if (p.type != YAS_ATOM_TYPE_PAIR)
			return YAS_ERROR_TYPE;
		
		YasAtom arg_spec = yas_car(p);
		if (arg_spec.type == YAS_ATOM_TYPE_SYMBOL) {
			/* Regular arg */
		} else if (arg_spec.type == YAS_ATOM_TYPE_KEYWORD) {
			/* Keyword marker - ok */
		} else if (arg_spec.type == YAS_ATOM_TYPE_PAIR) {
			/* [name default] - check that car is symbol */
			if (yas_car(arg_spec).type != YAS_ATOM_TYPE_SYMBOL)
				return YAS_ERROR_TYPE;
		} else {
			return YAS_ERROR_TYPE;
		}
		p = yas_cdr(p);
	}

	/* Extract docstring if first body form is a string and there's more body */
	YasAtom docstring = yas_nil;
	if (!yas_nilp(body) && yas_car(body).type == YAS_ATOM_TYPE_STRING && !yas_nilp(yas_cdr(body))) {
		docstring = yas_car(body);
		body = yas_cdr(body);
	}

	/* Structure: (env (args docstring . body)) */
	*result = yas_cons(env, yas_cons(args, yas_cons(docstring, body)));
	result->type = YAS_ATOM_TYPE_CLOSURE;

	return YAS_ERROR_OK;
}

static YasAtom make_frame(YasAtom parent, YasAtom env, YasAtom tail)
{
	return yas_cons(parent,
		yas_cons(env,
		yas_cons(yas_nil, /* op */
		yas_cons(tail,
		yas_cons(yas_nil, /* args */
		yas_cons(yas_nil, /* body */
		yas_nil))))));
}

int eval_do_exec(YasAtom *stack, YasAtom *expr, YasAtom *env)
{
	YasAtom body;

	*env = yas_list_get(*stack, 1);
	body = yas_list_get(*stack, 5);
	*expr = yas_car(body);
	body = yas_cdr(body);
	if (yas_nilp(body)) {
		/* Finished function; pop the stack */
		*stack = yas_car(*stack);
	} else {
		yas_list_set(*stack, 5, body);
	}

	return YAS_ERROR_OK;
}

int eval_do_bind(YasAtom *stack, YasAtom *expr, YasAtom *env)
{
	YasAtom op, args, arg_names, body;

	body = yas_list_get(*stack, 5);
	if (!yas_nilp(body))
		return eval_do_exec(stack, expr, env);

	op = yas_list_get(*stack, 2);
	args = yas_list_get(*stack, 4);

	*env = yas_env_create(yas_car(op));
	arg_names = yas_car(yas_cdr(op));
	/* Structure: (env (args docstring . body)) - skip docstring */
	body = yas_cdr(yas_cdr(yas_cdr(op)));
	yas_list_set(*stack, 1, *env);
	yas_list_set(*stack, 5, body);

	/* Bind the arguments
	 * Supports:
	 * - Regular: x
	 * - Optional with default: [x default]
	 * - Keyword: :key [var default]
	 * - Rest: . rest
	 */
	int in_keywords = 0;
	while (!yas_nilp(arg_names)) {
		/* Rest argument (symbol at end) */
		if (arg_names.type == YAS_ATOM_TYPE_SYMBOL) {
			yas_env_define(*env, arg_names, args);
			args = yas_nil;
			break;
		}

		YasAtom arg_spec = yas_car(arg_names);

		/* Keyword marker - next items are keyword args */
		if (arg_spec.type == YAS_ATOM_TYPE_KEYWORD) {
			in_keywords = 1;
			/* Find matching keyword in args and get its value */
			YasAtom kw = arg_spec;
			arg_names = yas_cdr(arg_names);
			if (yas_nilp(arg_names)) return YAS_ERROR_SYNTAX; /* keyword without var spec */
			
			YasAtom var_spec = yas_car(arg_names);
			YasAtom var_name, default_val = yas_nil;
			
			/* Parse [name default] or just name */
			if (var_spec.type == YAS_ATOM_TYPE_SYMBOL) {
				var_name = var_spec;
			} else if (var_spec.type == YAS_ATOM_TYPE_PAIR) {
				var_name = yas_car(var_spec);
				if (!yas_nilp(yas_cdr(var_spec)))
					default_val = yas_car(yas_cdr(var_spec));
			} else {
				return YAS_ERROR_SYNTAX;
			}
			
			/* Search for keyword in call args */
			YasAtom found_val = yas_nil;
			int found = 0;
			YasAtom search = args;
			YasAtom prev = yas_nil;
			while (!yas_nilp(search)) {
				if (yas_car(search).type == YAS_ATOM_TYPE_KEYWORD &&
				    yas_car(search).value.keyword == kw.value.keyword) {
					/* Found keyword, next is value */
					if (yas_nilp(yas_cdr(search))) return YAS_ERROR_ARGS;
					found_val = yas_car(yas_cdr(search));
					found = 1;
					/* Remove kw and value from args list */
					if (yas_nilp(prev)) {
						args = yas_cdr(yas_cdr(search));
					} else {
						yas_set_cdr(prev, yas_cdr(yas_cdr(search)));
					}
					break;
				}
				prev = search;
				search = yas_cdr(search);
			}
			
			if (found) {
				yas_env_define(*env, var_name, found_val);
			} else {
				yas_env_define(*env, var_name, default_val);
			}
			
			arg_names = yas_cdr(arg_names);
			continue;
		}

		/* Optional argument with default: [name default] */
		if (arg_spec.type == YAS_ATOM_TYPE_PAIR && !in_keywords) {
			YasAtom var_name = yas_car(arg_spec);
			YasAtom default_val = yas_nil;
			if (!yas_nilp(yas_cdr(arg_spec)))
				default_val = yas_car(yas_cdr(arg_spec));
			
			if (yas_nilp(args) || yas_car(args).type == YAS_ATOM_TYPE_KEYWORD) {
				/* No more positional args, use default */
				yas_env_define(*env, var_name, default_val);
			} else {
				yas_env_define(*env, var_name, yas_car(args));
				args = yas_cdr(args);
			}
			arg_names = yas_cdr(arg_names);
			continue;
		}

		/* Regular positional argument */
		if (arg_spec.type == YAS_ATOM_TYPE_SYMBOL) {
			if (yas_nilp(args))
				return YAS_ERROR_ARGS;
			yas_env_define(*env, arg_spec, yas_car(args));
			args = yas_cdr(args);
			arg_names = yas_cdr(arg_names);
			continue;
		}

		return YAS_ERROR_SYNTAX;
	}
	
	/* Check for leftover positional args (keywords are allowed to remain) */
	while (!yas_nilp(args)) {
		if (yas_car(args).type != YAS_ATOM_TYPE_KEYWORD)
			return YAS_ERROR_ARGS;
		args = yas_cdr(args);
		if (!yas_nilp(args)) args = yas_cdr(args); /* Skip value too */
	}

	yas_list_set(*stack, 4, yas_nil);

	return eval_do_exec(stack, expr, env);
}

int eval_do_apply(YasAtom *stack, YasAtom *expr, YasAtom *env, YasAtom *result)
{
	YasAtom op, args;

	op = yas_list_get(*stack, 2);
	args = yas_list_get(*stack, 4);

	if (!yas_nilp(args)) {
		yas_list_reverse(&args);
		yas_list_set(*stack, 4, args);
	}

	if (op.type == YAS_ATOM_TYPE_SYMBOL) {
		if (strcmp(op.value.symbol, "APPLY") == 0) {
			/* Replace the current frame */
			*stack = yas_car(*stack);
			*stack = make_frame(*stack, *env, yas_nil);
			op = yas_car(args);
			args = yas_car(yas_cdr(args));
			if (!yas_listp(args))
				return YAS_ERROR_SYNTAX;

			yas_list_set(*stack, 2, op);
			yas_list_set(*stack, 4, args);
		}
	}

	if (op.type == YAS_ATOM_TYPE_BUILTIN) {
		/* Check for arithmetic metamethods on hash arguments */
		if (!yas_nilp(args) && !yas_nilp(yas_cdr(args))) {
			YasAtom a = yas_car(args);
			YasAtom b = yas_car(yas_cdr(args));
			const char *mm_name = NULL;
			
			/* Determine which metamethod to check based on builtin */
			if (op.value.builtin == yas_builtin_add) mm_name = "add";
			else if (op.value.builtin == yas_builtin_subtract) mm_name = "sub";
			else if (op.value.builtin == yas_builtin_multiply) mm_name = "mul";
			else if (op.value.builtin == yas_builtin_divide) mm_name = "div";
			else if (op.value.builtin == yas_builtin_numeq) mm_name = "eq";
			else if (op.value.builtin == yas_builtin_less) mm_name = "lt";
			
			if (mm_name) {
				YasAtom hash_arg = yas_nil;
				YasAtom other_arg = yas_nil;
				
				/* Check first operand, then second for metamethod */
				if (a.type == YAS_ATOM_TYPE_HASH && yas_has_metamethod(a, mm_name)) {
					hash_arg = a;
					other_arg = b;
				} else if (b.type == YAS_ATOM_TYPE_HASH && yas_has_metamethod(b, mm_name)) {
					hash_arg = b;
					other_arg = a;
				}
				
				if (!yas_nilp(hash_arg)) {
					/* Redirect to metamethod: (metamethod self other) */
					YasAtom mm_fn = yas_get_metamethod(hash_arg, mm_name);
					args = yas_list_create(2, hash_arg, other_arg);
					yas_list_set(*stack, 2, mm_fn);
					yas_list_set(*stack, 4, args);
					
					if (mm_fn.type == YAS_ATOM_TYPE_BUILTIN) {
						*stack = yas_car(*stack);
						*expr = yas_cons(mm_fn, args);
						return YAS_ERROR_OK;
					} else if (mm_fn.type == YAS_ATOM_TYPE_CLOSURE) {
						return eval_do_bind(stack, expr, env);
					} else {
						return YAS_ERROR_TYPE;
					}
				}
			}
		}
		
		/* Check for ~get on hash-ref or ~set on hash-set! */
		if (op.value.builtin == yas_builtin_hash_ref && !yas_nilp(args)) {
			YasAtom hash = yas_car(args);
			if (hash.type == YAS_ATOM_TYPE_HASH && yas_has_metamethod(hash, "get")) {
				YasAtom key = !yas_nilp(yas_cdr(args)) ? yas_car(yas_cdr(args)) : yas_nil;
				/* Check if key exists in hash first - only call ~get if missing */
				YasAtom existing = yas_hash_ref(hash, key, yas_nil);
				if (yas_nilp(existing)) {
					YasAtom mm_fn = yas_get_metamethod(hash, "get");
					args = yas_list_create(2, hash, key);
					yas_list_set(*stack, 2, mm_fn);
					yas_list_set(*stack, 4, args);
					
					if (mm_fn.type == YAS_ATOM_TYPE_CLOSURE) {
						return eval_do_bind(stack, expr, env);
					} else if (mm_fn.type == YAS_ATOM_TYPE_BUILTIN) {
						*stack = yas_car(*stack);
						*expr = yas_cons(mm_fn, args);
						return YAS_ERROR_OK;
					}
				}
			}
		}
		
		if (op.value.builtin == yas_builtin_hash_set && !yas_nilp(args)) {
			YasAtom hash = yas_car(args);
			if (hash.type == YAS_ATOM_TYPE_HASH && yas_has_metamethod(hash, "set")) {
				YasAtom key = !yas_nilp(yas_cdr(args)) ? yas_car(yas_cdr(args)) : yas_nil;
				YasAtom val = !yas_nilp(yas_cdr(yas_cdr(args))) ? yas_car(yas_cdr(yas_cdr(args))) : yas_nil;
				YasAtom mm_fn = yas_get_metamethod(hash, "set");
				args = yas_list_create(3, hash, key, val);
				yas_list_set(*stack, 2, mm_fn);
				yas_list_set(*stack, 4, args);
				
				if (mm_fn.type == YAS_ATOM_TYPE_CLOSURE) {
					return eval_do_bind(stack, expr, env);
				} else if (mm_fn.type == YAS_ATOM_TYPE_BUILTIN) {
					*stack = yas_car(*stack);
					*expr = yas_cons(mm_fn, args);
					return YAS_ERROR_OK;
				}
			}
		}
		
		*stack = yas_car(*stack);
		*expr = yas_cons(op, args);
		return YAS_ERROR_OK;
	} else if (op.type == YAS_ATOM_TYPE_CONTINUATION) {
		/* Invoke continuation: replace stack, return value */
		if (yas_nilp(args) || !yas_nilp(yas_cdr(args)))
			return YAS_ERROR_ARGS;
		
		/* Restore the captured stack */
		stack->type = YAS_ATOM_TYPE_PAIR;
		stack->value.pair = op.value.pair;
		*result = yas_car(args);
		
		if (yas_nilp(*stack))
			return YAS_ERROR_OK;  /* Will exit main loop */
		return eval_do_return(stack, expr, env, result);
	} else if (op.type == YAS_ATOM_TYPE_HASH && yas_has_metamethod(op, "call")) {
		/* Hash has ~call metamethod - invoke it with (self . args) */
		YasAtom call_fn = yas_get_metamethod(op, "call");
		args = yas_cons(op, args);  /* Prepend hash as first arg (self) */
		yas_list_set(*stack, 2, call_fn);
		yas_list_set(*stack, 4, args);
		
		/* Handle based on metamethod type */
		if (call_fn.type == YAS_ATOM_TYPE_BUILTIN) {
			*stack = yas_car(*stack);
			*expr = yas_cons(call_fn, args);
			return YAS_ERROR_OK;
		} else if (call_fn.type == YAS_ATOM_TYPE_CLOSURE) {
			return eval_do_bind(stack, expr, env);
		} else {
			return YAS_ERROR_TYPE;
		}
	} else if (op.type == YAS_ATOM_TYPE_HASH && yas_has_metamethod(op, "missing")) {
		/* Hash has ~missing metamethod - for Ruby-style method dispatch */
		/* args = (method-name arg1 arg2 ...) 
		   Call: (~missing self args)  
		   which is (~missing self (method-name arg1 arg2 ...)) */
		YasAtom missing_fn = yas_get_metamethod(op, "missing");
		YasAtom new_args = yas_list_create(2, op, args);
		yas_list_set(*stack, 2, missing_fn);
		yas_list_set(*stack, 4, new_args);
		
		if (missing_fn.type == YAS_ATOM_TYPE_BUILTIN) {
			*stack = yas_car(*stack);
			*expr = yas_cons(missing_fn, new_args);
			return YAS_ERROR_OK;
		} else if (missing_fn.type == YAS_ATOM_TYPE_CLOSURE) {
			return eval_do_bind(stack, expr, env);
		} else {
			return YAS_ERROR_TYPE;
		}
	} else if (op.type != YAS_ATOM_TYPE_CLOSURE) {
		return YAS_ERROR_TYPE;
	}

	return eval_do_bind(stack, expr, env);
}

int eval_do_return(YasAtom *stack, YasAtom *expr, YasAtom *env, YasAtom *result)
{
	YasAtom op, args, body;

	*env = yas_list_get(*stack, 1);
	op = yas_list_get(*stack, 2);
	body = yas_list_get(*stack, 5);

	if (!yas_nilp(body)) {
		/* Still running a procedure; ignore the result */
		return eval_do_apply(stack, expr, env, result);
	}

	if (yas_nilp(op)) {
		/* Finished evaluating operator */
		op = *result;
		yas_list_set(*stack, 2, op);

		if (op.type == YAS_ATOM_TYPE_MACRO) {
			/* Don't evaluate macro arguments */
			args = yas_list_get(*stack, 3);
			*stack = make_frame(*stack, *env, yas_nil);
			op.type = YAS_ATOM_TYPE_CLOSURE;
			yas_list_set(*stack, 2, op);
			yas_list_set(*stack, 4, args);
			return eval_do_bind(stack, expr, env);
		}
	} else if (op.type == YAS_ATOM_TYPE_SYMBOL) {
		/* Finished working on special form */
		if (strcmp(op.value.symbol, "DEFINE") == 0) {
			YasAtom sym = yas_list_get(*stack, 4);
			(void) yas_env_define(*env, sym, *result);
			*stack = yas_car(*stack);
			*expr = yas_cons(yas_make_sym("QUOTE"), yas_cons(sym, yas_nil));
			return YAS_ERROR_OK;
		} else if (strcmp(op.value.symbol, "SET!") == 0) {
			YasAtom sym = yas_list_get(*stack, 4);
			*stack = yas_car(*stack);
			*expr = yas_cons(yas_make_sym("QUOTE"), yas_cons(sym, yas_nil));
			return yas_env_set(*env, sym, *result);
		} else if (strcmp(op.value.symbol, "IF") == 0) {
			args = yas_list_get(*stack, 3);
			*expr = yas_nilp(*result) ? yas_car(yas_cdr(args)) : yas_car(args);
			*stack = yas_car(*stack);
			return YAS_ERROR_OK;
		} else if (strcmp(op.value.symbol, "MACROEXPAND-1") == 0) {
			/* result contains the evaluated form */
			YasAtom form = *result;
			YasAtom original_env = yas_list_get(*stack, 1);
			*stack = yas_car(*stack);
			
			/* If form is (sym ...) where sym is bound to a macro, expand */
			if (form.type == YAS_ATOM_TYPE_PAIR && yas_car(form).type == YAS_ATOM_TYPE_SYMBOL) {
				YasAtom macro_op;
				if (yas_env_get(original_env, yas_car(form), &macro_op) == YAS_ERROR_OK
				    && macro_op.type == YAS_ATOM_TYPE_MACRO) {
					/* Apply macro: bind args, eval body */
					*stack = make_frame(*stack, original_env, yas_nil);
					macro_op.type = YAS_ATOM_TYPE_CLOSURE;
					yas_list_set(*stack, 2, macro_op);
					yas_list_set(*stack, 4, yas_cdr(form));
					return eval_do_bind(stack, expr, env);
				}
			}
			/* Not a macro call, return unchanged */
			*expr = yas_cons(yas_make_sym("QUOTE"), yas_cons(form, yas_nil));
			return YAS_ERROR_OK;
		} else if (strcmp(op.value.symbol, "CALL/CC") == 0) {
			/* result is the function to call with continuation */
			YasAtom fn = *result;
			YasAtom cont;
			
			/* Pop CALL/CC frame and capture remaining stack as continuation */
			*stack = yas_car(*stack);
			cont.type = YAS_ATOM_TYPE_CONTINUATION;
			cont.value.pair = (*stack).value.pair;
			
			/* Apply fn to the continuation */
			*stack = make_frame(*stack, *env, yas_nil);
			yas_list_set(*stack, 2, fn);
			yas_list_set(*stack, 4, yas_cons(cont, yas_nil));
			return eval_do_bind(stack, expr, env);
		} else {
			goto store_arg;
		}
	} else if (op.type == YAS_ATOM_TYPE_MACRO) {
		/* Finished evaluating macro */
		*expr = *result;
		*stack = yas_car(*stack);
		return YAS_ERROR_OK;
	} else {
	store_arg:
		/* Store evaluated argument */
		args = yas_list_get(*stack, 4);
		yas_list_set(*stack, 4, yas_cons(*result, args));
	}

	args = yas_list_get(*stack, 3);
	if (yas_nilp(args)) {
		/* No more arguments left to evaluate */
		return eval_do_apply(stack, expr, env, result);
	}

	/* Evaluate next argument */
	*expr = yas_car(args);
	yas_list_set(*stack, 3, yas_cdr(args));
	return YAS_ERROR_OK;
}

int yas_eval_expr(YasAtom expr, YasAtom env, YasAtom *result)
{
	struct EvalRoot root;
	root.prev = eval_roots;
	eval_roots = &root;

	/* Balance guard for the yas_gc_push_root()/yas_gc_pop_roots() shadow
	 * stack: every re-entering caller in this file and module.c pushes
	 * and pops in matched pairs, and there is no setjmp/longjmp in the
	 * tree (call/cc is implemented on the VM's own explicit stack), so
	 * every unwind is a plain C return - balance is always achievable.
	 * Catches a future unbalanced push here instead of a slow root-stack
	 * leak. */
	int root_count_before = yas_gc_root_count();

	int err = eval_vm(expr, env, result, &root);

	eval_roots = root.prev;
	assert(yas_gc_root_count() == root_count_before);
	return err;
}

static int eval_vm(YasAtom expr, YasAtom env, YasAtom *result, struct EvalRoot *root)
{
	YasError err = YAS_ERROR_OK;
	YasAtom stack = yas_nil;

	/* Caller-supplied `result` storage is uninitialized (main.c's
	 * `YasAtom result;`, this file's `YasAtom ignore;`/`ignored;`, etc.)
	 * - nil it before rooting so a GC step at the very first safe point
	 * below doesn't mark garbage. */
	*result = yas_nil;

	root->expr = &expr;
	root->env = &env;
	root->stack = &stack;
	root->result = result;

	do {
		/* The evaluator's safe point: eval_roots is valid here and no
		 * builtin/native call is in flight, so this is the only place
		 * a GC step may run (see docs/GC.md). yas_gc_step() is a cheap
		 * no-op unless a cycle is active or the byte threshold has
		 * tripped, so calling it every iteration keeps pauses bounded
		 * (YAS_GC_STEP_LIMIT objects at a time) instead of the old
		 * every-100,000-conses stop-the-world burst. */
		yas_gc_step();

		if (expr.type == YAS_ATOM_TYPE_SYMBOL) {
			err = yas_env_get(env, expr, result);
			if (err == YAS_ERROR_UNBOUND) {
				/* Store the symbol name in exception for better error reporting */
				extern YasAtom yas_ctx_exception;
				char msg[256];
				snprintf(msg, sizeof(msg), "Symbol not bound: %s", expr.value.symbol);
				YasAtom t = yas_make_sym("type-error");
				YasAtom m = yas_make_string(msg);
				yas_ctx_exception = yas_cons(t, yas_cons(m, yas_nil));
			}
		} else if (expr.type != YAS_ATOM_TYPE_PAIR) {
			*result = expr;
		} else if (!yas_listp(expr)) {
			return YAS_ERROR_SYNTAX;
		} else {
			YasAtom op = yas_car(expr);
			YasAtom args = yas_cdr(expr);

			if (op.type == YAS_ATOM_TYPE_SYMBOL) {
				/* Handle special forms */

				if (strcmp(op.value.symbol, "QUOTE") == 0) {
					if (yas_nilp(args) || !yas_nilp(yas_cdr(args)))
						return YAS_ERROR_ARGS;

					*result = yas_car(args);
				} else if (strcmp(op.value.symbol, "DEFINE") == 0) {
					YasAtom sym;

					if (yas_nilp(args) || yas_nilp(yas_cdr(args)))
						return YAS_ERROR_ARGS;

					sym = yas_car(args);
					if (sym.type == YAS_ATOM_TYPE_PAIR) {
						err = make_closure(env, yas_cdr(sym), yas_cdr(args), result);
						sym = yas_car(sym);
						if (sym.type != YAS_ATOM_TYPE_SYMBOL)
							return YAS_ERROR_TYPE;
						(void) yas_env_define(env, sym, *result);
						*result = sym;
					} else if (sym.type == YAS_ATOM_TYPE_SYMBOL) {
						if (!yas_nilp(yas_cdr(yas_cdr(args))))
							return YAS_ERROR_ARGS;
						stack = make_frame(stack, env, yas_nil);
						yas_list_set(stack, 2, op);
						yas_list_set(stack, 4, sym);
						expr = yas_car(yas_cdr(args));
						continue;
					} else {
						return YAS_ERROR_TYPE;
					}
				} else if (strcmp(op.value.symbol, "LAMBDA") == 0) {
					if (yas_nilp(args) || yas_nilp(yas_cdr(args)))
						return YAS_ERROR_ARGS;

					err = make_closure(env, yas_car(args), yas_cdr(args), result);
				} else if (strcmp(op.value.symbol, "IF") == 0) {
					if (yas_nilp(args) || yas_nilp(yas_cdr(args)) || yas_nilp(yas_cdr(yas_cdr(args)))
							|| !yas_nilp(yas_cdr(yas_cdr(yas_cdr(args)))))
						return YAS_ERROR_ARGS;

					stack = make_frame(stack, env, yas_cdr(args));
					yas_list_set(stack, 2, op);
					expr = yas_car(args);
					continue;
				} else if (strcmp(op.value.symbol, "TRY") == 0) {
					if (yas_nilp(args)) return YAS_ERROR_ARGS;
					
					YasAtom try_expr = yas_car(args);
					YasAtom rest = yas_cdr(args);
					
					/* Evaluate try block recursively to catch error */
					/* We recurse here because TRY establishes a dynamic handler boundary */
					err = yas_eval_expr(try_expr, env, result);
					
					if (err != YAS_ERROR_OK) {
						/* Exception thrown or runtime error */
						if (yas_nilp(yas_ctx_exception)) {
							/* Convert code to exception string if not already set */
							const char *msg = "Unknown Error";
							if (err == YAS_ERROR_UNBOUND) msg = "Symbol not bound";
							else if (err == YAS_ERROR_ARGS) msg = "Argument error";
							else if (err == YAS_ERROR_TYPE) msg = "Type error";
							else if (err == YAS_ERROR_SYNTAX) msg = "Syntax error";
							
							yas_ctx_exception = yas_make_string(msg);
						}
						/* Exception thrown */
						int caught = 0;
						YasAtom p = rest;
						while (!yas_nilp(p)) {
							YasAtom clause = yas_car(p);
							if (clause.type == YAS_ATOM_TYPE_PAIR && yas_car(clause).type == YAS_ATOM_TYPE_SYMBOL &&
								strcmp(yas_car(clause).value.symbol, "CATCH") == 0) {
								
								/* (CATCH var_decl . body) */
								YasAtom catch_args = yas_cdr(clause);
								if (yas_nilp(catch_args)) { err = YAS_ERROR_SYNTAX; break; }
								
								YasAtom var_decl = yas_car(catch_args);
								YasAtom catch_body = yas_cdr(catch_args);
								
								YasAtom var = yas_nil;
								YasAtom pred = yas_nil;
								
								/* Parse var declaration: sym or (sym pred) */
								if (var_decl.type == YAS_ATOM_TYPE_SYMBOL) {
									var = var_decl;
								} else if (var_decl.type == YAS_ATOM_TYPE_PAIR) {
									if (yas_nilp(var_decl) || yas_nilp(yas_cdr(var_decl))) { err = YAS_ERROR_SYNTAX; break; }
									var = yas_car(var_decl);
									pred = yas_car(yas_cdr(var_decl));
									if (var.type != YAS_ATOM_TYPE_SYMBOL) { err = YAS_ERROR_TYPE; break; } /* Var must be symbol */
								} else {
									err = YAS_ERROR_SYNTAX;
									break;
								}
								
								/* Bind exception for predicate check and body.
								 * catch_env is a fresh yas_env_create(env) that
								 * nothing else references (env's own binding
								 * list doesn't point back at its children), so
								 * it must stay rooted for as long as it's used
								 * below - through the predicate eval and the
								 * catch body loop, both of which can trigger a
								 * nested GC step. Popped on every exit from
								 * this clause: the predicate-error break, the
								 * matched-and-executed break, and the
								 * not-matched fall-through to the next
								 * clause. */
								YasAtom catch_env = yas_env_create(env);
								yas_env_define(catch_env, var, yas_ctx_exception);
								yas_gc_push_root(&catch_env);

								int match = 1;
								if (!yas_nilp(pred)) {
									/* Check predicate: (pred var) */
									YasAtom pred_expr = yas_cons(pred, yas_cons(var, yas_nil));
									YasAtom pred_res;
									YasError pred_err = yas_eval_expr(pred_expr, catch_env, &pred_res);

									if (pred_err) {
										/* Error evaluating predicate? Propagate it unless it's an exception? */
										/* If predicate throws, we probably abort catch logic and propagate that error */
										err = pred_err;
										yas_gc_pop_roots(1); /* catch_env */
										break;
									}

									if (yas_nilp(pred_res)) match = 0;
								}

								if (match) {
									/* Execute body. last_res carries the
									 * previous body form's value across the
									 * next form's yas_eval_expr() - unrooted
									 * otherwise, since it's a plain C local
									 * distinct from eval_vm's own *result
									 * (only copied into *result after the
									 * loop). */
									YasAtom last_res = yas_nil;
									yas_gc_push_root(&last_res);
									while (!yas_nilp(catch_body)) {
										err = yas_eval_expr(yas_car(catch_body), catch_env, &last_res);
										if (err) break; /* Exception inside catch? */
										catch_body = yas_cdr(catch_body);
									}
									*result = last_res;
									yas_gc_pop_roots(2); /* last_res, catch_env */
									caught = 1;
									(void)caught; /* suppress warning */
									break;
								} else {
									/* Restore exception state for next iteration */
									err = YAS_ERROR_EXCEPTION;
									yas_gc_pop_roots(1); /* catch_env */
								}
							}
							p = yas_cdr(p);
						}
						
						/* If not caught, err remains YAS_ERROR_EXCEPTION (or Syntax error from loop break) */
					}
					
					/* Save error state (either original exception if not caught, or new error from catch, or OK) */
					YasError saved_err = err;
					
					/* Execute FINALLY */
					YasAtom p = rest;
					while (!yas_nilp(p)) {
						YasAtom clause = yas_car(p);
						if (clause.type == YAS_ATOM_TYPE_PAIR && yas_car(clause).type == YAS_ATOM_TYPE_SYMBOL &&
							strcmp(yas_car(clause).value.symbol, "FINALLY") == 0) {
							
							/* (FINALLY . body) */
							YasAtom final_body = yas_cdr(clause);
							YasAtom ignored;
							while (!yas_nilp(final_body)) {
								YasError final_err = yas_eval_expr(yas_car(final_body), env, &ignored);
								if (final_err) {
									/* Exception in finally overrides previous */
									saved_err = final_err;
									break;
								}
								final_body = yas_cdr(final_body);
							}
							break;
						}
						p = yas_cdr(p);
					}
					
					err = saved_err; 
					
					/* Result is set if err==OK. */
					/* If err!=OK, we will break/return error. */
					if (err) break;
				} else if (strcmp(op.value.symbol, "DEFMACRO") == 0) {
					YasAtom name, macro;

					if (yas_nilp(args) || yas_nilp(yas_cdr(args)))
						return YAS_ERROR_ARGS;

					if (yas_car(args).type != YAS_ATOM_TYPE_PAIR)
						return YAS_ERROR_SYNTAX;

					name = yas_car(yas_car(args));
					if (name.type != YAS_ATOM_TYPE_SYMBOL)
						return YAS_ERROR_TYPE;

					err = make_closure(env, yas_cdr(yas_car(args)),
						yas_cdr(args), &macro);
					if (!err) {
						macro.type = YAS_ATOM_TYPE_MACRO;
						*result = name;
						(void) yas_env_define(env, name, macro);
					}
				} else if (strcmp(op.value.symbol, "APPLY") == 0) {
					if (yas_nilp(args) || yas_nilp(yas_cdr(args)) || !yas_nilp(yas_cdr(yas_cdr(args))))
						return YAS_ERROR_ARGS;

					stack = make_frame(stack, env, yas_cdr(args));
					yas_list_set(stack, 2, op);
					expr = yas_car(args);
					continue;
				} else if (strcmp(op.value.symbol, "SET!") == 0) {
					if (yas_nilp(args) || yas_nilp(yas_cdr(args)) || !yas_nilp(yas_cdr(yas_cdr(args))))
						return YAS_ERROR_ARGS;
					if (yas_car(args).type != YAS_ATOM_TYPE_SYMBOL)
						return YAS_ERROR_TYPE;
					stack = make_frame(stack, env, yas_nil);
					yas_list_set(stack, 2, op);
					yas_list_set(stack, 4, yas_car(args));
					expr = yas_car(yas_cdr(args));
					continue;
				} else if (strcmp(op.value.symbol, "MACROEXPAND-1") == 0) {
					if (yas_nilp(args) || !yas_nilp(yas_cdr(args)))
						return YAS_ERROR_ARGS;
					/* Evaluate the argument to get the form */
					stack = make_frame(stack, env, yas_nil);
					yas_list_set(stack, 2, op);
					expr = yas_car(args);
					continue;
				} else if (strcmp(op.value.symbol, "CALL/CC") == 0) {
					if (yas_nilp(args) || !yas_nilp(yas_cdr(args)))
						return YAS_ERROR_ARGS;
					/* Evaluate the function argument */
					stack = make_frame(stack, env, yas_nil);
					yas_list_set(stack, 2, op);
					expr = yas_car(args);
					continue;
				} else if (strcmp(op.value.symbol, "INCLUDE") == 0 ||
				           strcmp(op.value.symbol, "INCLUDE-CI") == 0) {
					/* (include "file.ys" ...) - textual file inclusion */
					if (yas_nilp(args))
						return YAS_ERROR_ARGS;
					
					int case_insensitive = (strcmp(op.value.symbol, "INCLUDE-CI") == 0);
					
					/* Process each file argument */
					YasAtom file_list = args;
					while (!yas_nilp(file_list)) {
						YasAtom file_arg = yas_car(file_list);
						if (file_arg.type != YAS_ATOM_TYPE_STRING)
							return YAS_ERROR_TYPE;
						
						err = yas_eval_file(file_arg.value.string, env, result, case_insensitive);
						if (err)
							return err;
						
						file_list = yas_cdr(file_list);
					}
					/* result is set to the last expression evaluated */
				} else if (strcmp(op.value.symbol, "DEFINE-LIBRARY") == 0) {
					err = yas_module_define_library(args, result);
					if (err) return err;
				} else if (strcmp(op.value.symbol, "IMPORT") == 0) {
					err = yas_module_import(args, env, result);
					if (err) return err;
				} else {
					goto push;
				}
			} else if (op.type == YAS_ATOM_TYPE_BUILTIN) {
				err = (*op.value.builtin)(args, result);
			} else {
			push:
				/* Handle function application */
				stack = make_frame(stack, env, args);
				expr = op;
				continue;
			}
		}

		if (yas_nilp(stack))
			break;

		if (!err)
			err = eval_do_return(&stack, &expr, &env, result);
	} while (!err);

	return err;
}


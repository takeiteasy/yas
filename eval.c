#include "lisp.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* Forward declaration */
static int eval_do_return(Atom *stack, Atom *expr, Atom *env, Atom *result);

Atom env_create(Atom parent)
{
	return cons(parent, nil);
}

int env_define(Atom env, Atom symbol, Atom value)
{
	Atom bs = cdr(env);

	while (!nilp(bs)) {
		Atom b = car(bs);
		if (car(b).value.symbol == symbol.value.symbol) {
			cdr(b) = value;
			return Error_OK;
		}
		bs = cdr(bs);
	}

	cdr(env) = cons(cons(symbol, value), cdr(env));

	return Error_OK;
}

int env_get(Atom env, Atom symbol, Atom *result)
{
	Atom parent = car(env);
	Atom bs = cdr(env);

	while (!nilp(bs)) {
		Atom b = car(bs);
		if (car(b).value.symbol == symbol.value.symbol) {
			*result = cdr(b);
			return Error_OK;
		}
		bs = cdr(bs);
	}

	if (nilp(parent))
		return Error_Unbound;

	return env_get(parent, symbol, result);
}

Atom ctx_exception = { AtomType_Nil };

/* Current file being evaluated (for relative include paths) */
const char *current_file_path = NULL;

/* Resolve a path relative to the current file */
static char *resolve_include_path(const char *include_path)
{
	/* If absolute path or no current file, use as-is */
	if (include_path[0] == '/' || !current_file_path) {
		char *result = malloc(strlen(include_path) + 1);
		strcpy(result, include_path);
		return result;
	}
	
	/* Find directory of current file */
	const char *last_slash = strrchr(current_file_path, '/');
	if (!last_slash) {
		/* Current file has no directory component */
		char *result = malloc(strlen(include_path) + 1);
		strcpy(result, include_path);
		return result;
	}
	
	/* Build path: directory + "/" + include_path */
	size_t dir_len = last_slash - current_file_path + 1;
	size_t total_len = dir_len + strlen(include_path) + 1;
	char *result = malloc(total_len);
	strncpy(result, current_file_path, dir_len);
	result[dir_len] = '\0';
	strcat(result, include_path);
	return result;
}

/* Helper to evaluate include - reads file, parses, evaluates all exprs */
/* Evaluate source string */
int eval_source(const char *text, const char *path, Atom env, Atom *result)
{
	const char *saved_path = current_file_path;
	current_file_path = path;
	
	const char *p = text;
	Atom expr;
	ErrorContext err = {0};
	err.pos.filename = path;
	err.pos.source = text;
	*result = nil;
	
	while (1) {
		err.code = Error_OK;
		err.message[0] = '\0';
		
		Error read_err = read_expr(p, &p, &expr, &err);
		if (read_err) {
			if (err.message[0] != '\0') {
				error_print(&err);
				current_file_path = saved_path;
				return Error_Syntax;
			}
			break;  /* End of file */
		}
		
		Atom ignore;
		Error eval_err = eval_expr(expr, env, &ignore);
		if (eval_err) {
			current_file_path = saved_path;
			return eval_err;
		}
		*result = ignore; // result is last expr
	}
	
	current_file_path = saved_path;
	return Error_OK;
}

/* Helper to evaluate include - reads file, parses, evaluates all exprs */
int eval_file(const char *path, Atom env, Atom *result, int case_insensitive)
{
	char *resolved = resolve_include_path(path);
	char *text = slurp(resolved);
	
	if (!text) {
		free(resolved);
		return raise_error("file", "Cannot open file for include");
	}
	
	int err = eval_source(text, resolved, env, result);
	
	free(text);
	free(resolved);
	return err;
}

int env_set(Atom env, Atom symbol, Atom value)
{
	Atom parent = car(env);
	Atom bs = cdr(env);

	while (!nilp(bs)) {
		Atom b = car(bs);
		if (car(b).value.symbol == symbol.value.symbol) {
			cdr(b) = value;
			return Error_OK;
		}
		bs = cdr(bs);
	}

	if (nilp(parent))
		return Error_Unbound;

	return env_set(parent, symbol, value);
}

int make_closure(Atom env, Atom args, Atom body, Atom *result)
{
	Atom p;

	if (!listp(body))
		return Error_Syntax;

	/* Check argument names are all symbols */
	p = args;
	while (!nilp(p)) {
		if (p.type == AtomType_Symbol)
			break;
		else if (p.type != AtomType_Pair
				|| car(p).type != AtomType_Symbol)
			return Error_Type;
		p = cdr(p);
	}

	*result = cons(env, cons(args, body));
	result->type = AtomType_Closure;

	return Error_OK;
}

Atom make_frame(Atom parent, Atom env, Atom tail)
{
	return cons(parent,
		cons(env,
		cons(nil, /* op */
		cons(tail,
		cons(nil, /* args */
		cons(nil, /* body */
		nil))))));
}

int eval_do_exec(Atom *stack, Atom *expr, Atom *env)
{
	Atom body;

	*env = list_get(*stack, 1);
	body = list_get(*stack, 5);
	*expr = car(body);
	body = cdr(body);
	if (nilp(body)) {
		/* Finished function; pop the stack */
		*stack = car(*stack);
	} else {
		list_set(*stack, 5, body);
	}

	return Error_OK;
}

int eval_do_bind(Atom *stack, Atom *expr, Atom *env)
{
	Atom op, args, arg_names, body;

	body = list_get(*stack, 5);
	if (!nilp(body))
		return eval_do_exec(stack, expr, env);

	op = list_get(*stack, 2);
	args = list_get(*stack, 4);

	*env = env_create(car(op));
	arg_names = car(cdr(op));
	body = cdr(cdr(op));
	list_set(*stack, 1, *env);
	list_set(*stack, 5, body);

	/* Bind the arguments */
	while (!nilp(arg_names)) {
		if (arg_names.type == AtomType_Symbol) {
			env_define(*env, arg_names, args);
			args = nil;
			break;
		}

		if (nilp(args))
			return Error_Args;
		env_define(*env, car(arg_names), car(args));
		arg_names = cdr(arg_names);
		args = cdr(args);
	}
	if (!nilp(args))
		return Error_Args;

	list_set(*stack, 4, nil);

	return eval_do_exec(stack, expr, env);
}

int eval_do_apply(Atom *stack, Atom *expr, Atom *env, Atom *result)
{
	Atom op, args;

	op = list_get(*stack, 2);
	args = list_get(*stack, 4);

	if (!nilp(args)) {
		list_reverse(&args);
		list_set(*stack, 4, args);
	}

	if (op.type == AtomType_Symbol) {
		if (strcmp(op.value.symbol, "APPLY") == 0) {
			/* Replace the current frame */
			*stack = car(*stack);
			*stack = make_frame(*stack, *env, nil);
			op = car(args);
			args = car(cdr(args));
			if (!listp(args))
				return Error_Syntax;

			list_set(*stack, 2, op);
			list_set(*stack, 4, args);
		}
	}

	if (op.type == AtomType_Builtin) {
		/* Check for arithmetic metamethods on hash arguments */
		if (!nilp(args) && !nilp(cdr(args))) {
			Atom a = car(args);
			Atom b = car(cdr(args));
			const char *mm_name = NULL;
			
			/* Determine which metamethod to check based on builtin */
			if (op.value.builtin == builtin_add) mm_name = "add";
			else if (op.value.builtin == builtin_subtract) mm_name = "sub";
			else if (op.value.builtin == builtin_multiply) mm_name = "mul";
			else if (op.value.builtin == builtin_divide) mm_name = "div";
			else if (op.value.builtin == builtin_numeq) mm_name = "eq";
			else if (op.value.builtin == builtin_less) mm_name = "lt";
			
			if (mm_name) {
				Atom hash_arg = nil;
				Atom other_arg = nil;
				
				/* Check first operand, then second for metamethod */
				if (a.type == AtomType_Hash && has_metamethod(a, mm_name)) {
					hash_arg = a;
					other_arg = b;
				} else if (b.type == AtomType_Hash && has_metamethod(b, mm_name)) {
					hash_arg = b;
					other_arg = a;
				}
				
				if (!nilp(hash_arg)) {
					/* Redirect to metamethod: (metamethod self other) */
					Atom mm_fn = get_metamethod(hash_arg, mm_name);
					args = list_create(2, hash_arg, other_arg);
					list_set(*stack, 2, mm_fn);
					list_set(*stack, 4, args);
					
					if (mm_fn.type == AtomType_Builtin) {
						*stack = car(*stack);
						*expr = cons(mm_fn, args);
						return Error_OK;
					} else if (mm_fn.type == AtomType_Closure) {
						return eval_do_bind(stack, expr, env);
					} else {
						return Error_Type;
					}
				}
			}
		}
		
		/* Check for __get on hash-ref or __set on hash-set! */
		if (op.value.builtin == builtin_hash_ref && !nilp(args)) {
			Atom hash = car(args);
			if (hash.type == AtomType_Hash && has_metamethod(hash, "get")) {
				Atom key = !nilp(cdr(args)) ? car(cdr(args)) : nil;
				/* Check if key exists in hash first - only call __get if missing */
				Atom existing = hash_ref(hash, key, nil);
				if (nilp(existing)) {
					Atom mm_fn = get_metamethod(hash, "get");
					args = list_create(2, hash, key);
					list_set(*stack, 2, mm_fn);
					list_set(*stack, 4, args);
					
					if (mm_fn.type == AtomType_Closure) {
						return eval_do_bind(stack, expr, env);
					} else if (mm_fn.type == AtomType_Builtin) {
						*stack = car(*stack);
						*expr = cons(mm_fn, args);
						return Error_OK;
					}
				}
			}
		}
		
		if (op.value.builtin == builtin_hash_set && !nilp(args)) {
			Atom hash = car(args);
			if (hash.type == AtomType_Hash && has_metamethod(hash, "set")) {
				Atom key = !nilp(cdr(args)) ? car(cdr(args)) : nil;
				Atom val = !nilp(cdr(cdr(args))) ? car(cdr(cdr(args))) : nil;
				Atom mm_fn = get_metamethod(hash, "set");
				args = list_create(3, hash, key, val);
				list_set(*stack, 2, mm_fn);
				list_set(*stack, 4, args);
				
				if (mm_fn.type == AtomType_Closure) {
					return eval_do_bind(stack, expr, env);
				} else if (mm_fn.type == AtomType_Builtin) {
					*stack = car(*stack);
					*expr = cons(mm_fn, args);
					return Error_OK;
				}
			}
		}
		
		*stack = car(*stack);
		*expr = cons(op, args);
		return Error_OK;
	} else if (op.type == AtomType_Continuation) {
		/* Invoke continuation: replace stack, return value */
		if (nilp(args) || !nilp(cdr(args)))
			return Error_Args;
		
		/* Restore the captured stack */
		stack->type = AtomType_Pair;
		stack->value.pair = op.value.pair;
		*result = car(args);
		
		if (nilp(*stack))
			return Error_OK;  /* Will exit main loop */
		return eval_do_return(stack, expr, env, result);
	} else if (op.type == AtomType_Hash && has_metamethod(op, "call")) {
		/* Hash has __call metamethod - invoke it with (self . args) */
		Atom call_fn = get_metamethod(op, "call");
		args = cons(op, args);  /* Prepend hash as first arg (self) */
		list_set(*stack, 2, call_fn);
		list_set(*stack, 4, args);
		
		/* Handle based on metamethod type */
		if (call_fn.type == AtomType_Builtin) {
			*stack = car(*stack);
			*expr = cons(call_fn, args);
			return Error_OK;
		} else if (call_fn.type == AtomType_Closure) {
			return eval_do_bind(stack, expr, env);
		} else {
			return Error_Type;
		}
	} else if (op.type == AtomType_Hash && has_metamethod(op, "missing")) {
		/* Hash has __missing metamethod - for Ruby-style method dispatch */
		/* args = (method-name arg1 arg2 ...) 
		   Call: (__missing self args)  
		   which is (__missing self (method-name arg1 arg2 ...)) */
		Atom missing_fn = get_metamethod(op, "missing");
		Atom new_args = list_create(2, op, args);
		list_set(*stack, 2, missing_fn);
		list_set(*stack, 4, new_args);
		
		if (missing_fn.type == AtomType_Builtin) {
			*stack = car(*stack);
			*expr = cons(missing_fn, new_args);
			return Error_OK;
		} else if (missing_fn.type == AtomType_Closure) {
			return eval_do_bind(stack, expr, env);
		} else {
			return Error_Type;
		}
	} else if (op.type != AtomType_Closure) {
		return Error_Type;
	}

	return eval_do_bind(stack, expr, env);
}

int eval_do_return(Atom *stack, Atom *expr, Atom *env, Atom *result)
{
	Atom op, args, body;

	*env = list_get(*stack, 1);
	op = list_get(*stack, 2);
	body = list_get(*stack, 5);

	if (!nilp(body)) {
		/* Still running a procedure; ignore the result */
		return eval_do_apply(stack, expr, env, result);
	}

	if (nilp(op)) {
		/* Finished evaluating operator */
		op = *result;
		list_set(*stack, 2, op);

		if (op.type == AtomType_Macro) {
			/* Don't evaluate macro arguments */
			args = list_get(*stack, 3);
			*stack = make_frame(*stack, *env, nil);
			op.type = AtomType_Closure;
			list_set(*stack, 2, op);
			list_set(*stack, 4, args);
			return eval_do_bind(stack, expr, env);
		}
	} else if (op.type == AtomType_Symbol) {
		/* Finished working on special form */
		if (strcmp(op.value.symbol, "DEFINE") == 0) {
			Atom sym = list_get(*stack, 4);
			(void) env_define(*env, sym, *result);
			*stack = car(*stack);
			*expr = cons(make_sym("QUOTE"), cons(sym, nil));
			return Error_OK;
		} else if (strcmp(op.value.symbol, "SET!") == 0) {
			Atom sym = list_get(*stack, 4);
			*stack = car(*stack);
			*expr = cons(make_sym("QUOTE"), cons(sym, nil));
			return env_set(*env, sym, *result);
		} else if (strcmp(op.value.symbol, "IF") == 0) {
			args = list_get(*stack, 3);
			*expr = nilp(*result) ? car(cdr(args)) : car(args);
			*stack = car(*stack);
			return Error_OK;
		} else if (strcmp(op.value.symbol, "MACROEXPAND-1") == 0) {
			/* result contains the evaluated form */
			Atom form = *result;
			Atom original_env = list_get(*stack, 1);
			*stack = car(*stack);
			
			/* If form is (sym ...) where sym is bound to a macro, expand */
			if (form.type == AtomType_Pair && car(form).type == AtomType_Symbol) {
				Atom macro_op;
				if (env_get(original_env, car(form), &macro_op) == Error_OK
				    && macro_op.type == AtomType_Macro) {
					/* Apply macro: bind args, eval body */
					*stack = make_frame(*stack, original_env, nil);
					macro_op.type = AtomType_Closure;
					list_set(*stack, 2, macro_op);
					list_set(*stack, 4, cdr(form));
					return eval_do_bind(stack, expr, env);
				}
			}
			/* Not a macro call, return unchanged */
			*expr = cons(make_sym("QUOTE"), cons(form, nil));
			return Error_OK;
		} else if (strcmp(op.value.symbol, "CALL/CC") == 0) {
			/* result is the function to call with continuation */
			Atom fn = *result;
			Atom cont;
			
			/* Pop CALL/CC frame and capture remaining stack as continuation */
			*stack = car(*stack);
			cont.type = AtomType_Continuation;
			cont.value.pair = (*stack).value.pair;
			
			/* Apply fn to the continuation */
			*stack = make_frame(*stack, *env, nil);
			list_set(*stack, 2, fn);
			list_set(*stack, 4, cons(cont, nil));
			return eval_do_bind(stack, expr, env);
		} else {
			goto store_arg;
		}
	} else if (op.type == AtomType_Macro) {
		/* Finished evaluating macro */
		*expr = *result;
		*stack = car(*stack);
		return Error_OK;
	} else {
	store_arg:
		/* Store evaluated argument */
		args = list_get(*stack, 4);
		list_set(*stack, 4, cons(*result, args));
	}

	args = list_get(*stack, 3);
	if (nilp(args)) {
		/* No more arguments left to evaluate */
		return eval_do_apply(stack, expr, env, result);
	}

	/* Evaluate next argument */
	*expr = car(args);
	list_set(*stack, 3, cdr(args));
	return Error_OK;
}

int eval_expr(Atom expr, Atom env, Atom *result)
{
	static int count = 0;
	Error err = Error_OK;
	Atom stack = nil;

	do {
		if (++count == 100000) {
			gc_mark(expr);
			gc_mark(env);
			gc_mark(stack);
			gc_mark(ctx_exception);
			gc();
			count = 0;
		}

		if (expr.type == AtomType_Symbol) {
			err = env_get(env, expr, result);
		} else if (expr.type != AtomType_Pair) {
			*result = expr;
		} else if (!listp(expr)) {
			return Error_Syntax;
		} else {
			Atom op = car(expr);
			Atom args = cdr(expr);

			if (op.type == AtomType_Symbol) {
				/* Handle special forms */

				if (strcmp(op.value.symbol, "QUOTE") == 0) {
					if (nilp(args) || !nilp(cdr(args)))
						return Error_Args;

					*result = car(args);
				} else if (strcmp(op.value.symbol, "DEFINE") == 0) {
					Atom sym;

					if (nilp(args) || nilp(cdr(args)))
						return Error_Args;

					sym = car(args);
					if (sym.type == AtomType_Pair) {
						err = make_closure(env, cdr(sym), cdr(args), result);
						sym = car(sym);
						if (sym.type != AtomType_Symbol)
							return Error_Type;
						(void) env_define(env, sym, *result);
						*result = sym;
					} else if (sym.type == AtomType_Symbol) {
						if (!nilp(cdr(cdr(args))))
							return Error_Args;
						stack = make_frame(stack, env, nil);
						list_set(stack, 2, op);
						list_set(stack, 4, sym);
						expr = car(cdr(args));
						continue;
					} else {
						return Error_Type;
					}
				} else if (strcmp(op.value.symbol, "LAMBDA") == 0) {
					if (nilp(args) || nilp(cdr(args)))
						return Error_Args;

					err = make_closure(env, car(args), cdr(args), result);
				} else if (strcmp(op.value.symbol, "IF") == 0) {
					if (nilp(args) || nilp(cdr(args)) || nilp(cdr(cdr(args)))
							|| !nilp(cdr(cdr(cdr(args)))))
						return Error_Args;

					stack = make_frame(stack, env, cdr(args));
					list_set(stack, 2, op);
					expr = car(args);
					continue;
				} else if (strcmp(op.value.symbol, "TRY") == 0) {
					if (nilp(args)) return Error_Args;
					
					Atom try_expr = car(args);
					Atom rest = cdr(args);
					
					/* Evaluate try block recursively to catch error */
					/* We recurse here because TRY establishes a dynamic handler boundary */
					err = eval_expr(try_expr, env, result);
					
					if (err == Error_Exception) {
						/* Exception thrown */
						Atom p = rest;
						while (!nilp(p)) {
							Atom clause = car(p);
							if (clause.type == AtomType_Pair && car(clause).type == AtomType_Symbol &&
								strcmp(car(clause).value.symbol, "CATCH") == 0) {
								
								/* (CATCH var_decl . body) */
								Atom catch_args = cdr(clause);
								if (nilp(catch_args)) { err = Error_Syntax; break; }
								
								Atom var_decl = car(catch_args);
								Atom catch_body = cdr(catch_args);
								
								Atom var = nil;
								Atom pred = nil;
								
								/* Parse var declaration: sym or (sym pred) */
								if (var_decl.type == AtomType_Symbol) {
									var = var_decl;
								} else if (var_decl.type == AtomType_Pair) {
									if (nilp(var_decl) || nilp(cdr(var_decl))) { err = Error_Syntax; break; }
									var = car(var_decl);
									pred = car(cdr(var_decl));
									if (var.type != AtomType_Symbol) { err = Error_Type; break; } /* Var must be symbol */
								} else {
									err = Error_Syntax;
									break;
								}
								
								/* Bind exception for predicate check and body */
								Atom catch_env = env_create(env);
								env_define(catch_env, var, ctx_exception);
								
								int match = 1;
								if (!nilp(pred)) {
									/* Check predicate: (pred var) */
									Atom pred_expr = cons(pred, cons(var, nil));
									Atom pred_res;
									Error pred_err = eval_expr(pred_expr, catch_env, &pred_res);
									
									if (pred_err) {
										/* Error evaluating predicate? Propagate it unless it's an exception? */
										/* If predicate throws, we probably abort catch logic and propagate that error */
										err = pred_err;
										break; 
									}
									
									if (nilp(pred_res)) match = 0;
								}
								
								if (match) {
									/* Execute body */
									Atom last_res = nil;
									while (!nilp(catch_body)) {
										err = eval_expr(car(catch_body), catch_env, &last_res);
										if (err) break; /* Exception inside catch? */
										catch_body = cdr(catch_body);
									}
									*result = last_res;
									caught = 1;
									break;
								} else {
									/* Restore exception state for next iteration */
									err = Error_Exception;
								}
							}
							p = cdr(p);
						}
						
						/* If not caught, err remains Error_Exception (or Syntax error from loop break) */
					}
					
					/* Save error state (either original exception if not caught, or new error from catch, or OK) */
					Error saved_err = err;
					
					/* Execute FINALLY */
					Atom p = rest;
					while (!nilp(p)) {
						Atom clause = car(p);
						if (clause.type == AtomType_Pair && car(clause).type == AtomType_Symbol &&
							strcmp(car(clause).value.symbol, "FINALLY") == 0) {
							
							/* (FINALLY . body) */
							Atom final_body = cdr(clause);
							Atom ignored;
							while (!nilp(final_body)) {
								Error final_err = eval_expr(car(final_body), env, &ignored);
								if (final_err) {
									/* Exception in finally overrides previous */
									saved_err = final_err;
									break;
								}
								final_body = cdr(final_body);
							}
							break;
						}
						p = cdr(p);
					}
					
					err = saved_err; 
					
					/* Result is set if err==OK. */
					/* If err!=OK, we will break/return error. */
					if (err) break;
				} else if (strcmp(op.value.symbol, "DEFMACRO") == 0) {
					Atom name, macro;

					if (nilp(args) || nilp(cdr(args)))
						return Error_Args;

					if (car(args).type != AtomType_Pair)
						return Error_Syntax;

					name = car(car(args));
					if (name.type != AtomType_Symbol)
						return Error_Type;

					err = make_closure(env, cdr(car(args)),
						cdr(args), &macro);
					if (!err) {
						macro.type = AtomType_Macro;
						*result = name;
						(void) env_define(env, name, macro);
					}
				} else if (strcmp(op.value.symbol, "APPLY") == 0) {
					if (nilp(args) || nilp(cdr(args)) || !nilp(cdr(cdr(args))))
						return Error_Args;

					stack = make_frame(stack, env, cdr(args));
					list_set(stack, 2, op);
					expr = car(args);
					continue;
				} else if (strcmp(op.value.symbol, "SET!") == 0) {
					if (nilp(args) || nilp(cdr(args)) || !nilp(cdr(cdr(args))))
						return Error_Args;
					if (car(args).type != AtomType_Symbol)
						return Error_Type;
					stack = make_frame(stack, env, nil);
					list_set(stack, 2, op);
					list_set(stack, 4, car(args));
					expr = car(cdr(args));
					continue;
				} else if (strcmp(op.value.symbol, "MACROEXPAND-1") == 0) {
					if (nilp(args) || !nilp(cdr(args)))
						return Error_Args;
					/* Evaluate the argument to get the form */
					stack = make_frame(stack, env, nil);
					list_set(stack, 2, op);
					expr = car(args);
					continue;
				} else if (strcmp(op.value.symbol, "CALL/CC") == 0) {
					if (nilp(args) || !nilp(cdr(args)))
						return Error_Args;
					/* Evaluate the function argument */
					stack = make_frame(stack, env, nil);
					list_set(stack, 2, op);
					expr = car(args);
					continue;
				} else if (strcmp(op.value.symbol, "INCLUDE") == 0 ||
				           strcmp(op.value.symbol, "INCLUDE-CI") == 0) {
					/* (include "file.gl" ...) - textual file inclusion */
					if (nilp(args))
						return Error_Args;
					
					int case_insensitive = (strcmp(op.value.symbol, "INCLUDE-CI") == 0);
					
					/* Process each file argument */
					Atom file_list = args;
					while (!nilp(file_list)) {
						Atom file_arg = car(file_list);
						if (file_arg.type != AtomType_String)
							return Error_Type;
						
						err = eval_file(file_arg.value.string, env, result, case_insensitive);
						if (err)
							return err;
						
						file_list = cdr(file_list);
					}
					/* result is set to the last expression evaluated */
				} else if (strcmp(op.value.symbol, "DEFINE-LIBRARY") == 0) {
					err = module_define_library(args, result);
					if (err) return err;
				} else if (strcmp(op.value.symbol, "IMPORT") == 0) {
					err = module_import(args, env, result);
					if (err) return err;
				} else {
					goto push;
				}
			} else if (op.type == AtomType_Builtin) {
				err = (*op.value.builtin)(args, result);
			} else {
			push:
				/* Handle function application */
				stack = make_frame(stack, env, args);
				expr = op;
				continue;
			}
		}

		if (nilp(stack))
			break;

		if (!err)
			err = eval_do_return(&stack, &expr, &env, result);
	} while (!err);

	return err;
}


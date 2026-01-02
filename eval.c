#include "lisp.h"
#include <string.h>
#include <stdio.h>

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
						int caught = 0;
						
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


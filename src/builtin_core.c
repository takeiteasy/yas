#include "yas.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

int yas_raise_error(const char *type, const char *msg)
{
	extern const char *yas_current_file_path;
	
	YasAtom t = yas_make_sym(type);
	YasAtom m = yas_make_string(msg);
	
	/* Include file path in error message if available */
	if (yas_current_file_path) {
		char *full_msg = malloc(strlen(msg) + strlen(yas_current_file_path) + 50);
		snprintf(full_msg, strlen(msg) + strlen(yas_current_file_path) + 50, 
		         "%s (in %s)", msg, yas_current_file_path);
		m = yas_make_string(full_msg);
		free(full_msg);
	}
	
	yas_ctx_exception = yas_cons(t, yas_cons(m, yas_nil));
	return YAS_ERROR_EXCEPTION;
}

int yas_builtin_car(YasAtom args, YasAtom *result)
{
	if (yas_nilp(args) || !yas_nilp(yas_cdr(args)))
		return yas_raise_error("args-error", "car: expected 1 argument");

	if (yas_nilp(yas_car(args)))
		*result = yas_nil;
	else if (yas_car(args).type != YAS_ATOM_TYPE_PAIR)
		return yas_raise_error("type-error", "car: argument must be pair");
	else
		*result = yas_car(yas_car(args));

	return YAS_ERROR_OK;
}

int yas_builtin_cdr(YasAtom args, YasAtom *result)
{
	if (yas_nilp(args) || !yas_nilp(yas_cdr(args)))
		return yas_raise_error("args-error", "cdr: expected 1 argument");

	if (yas_nilp(yas_car(args)))
		*result = yas_nil;
	else if (yas_car(args).type != YAS_ATOM_TYPE_PAIR)
		return yas_raise_error("type-error", "cdr: argument must be pair");
	else
		*result = yas_cdr(yas_car(args));

	return YAS_ERROR_OK;
}

int yas_builtin_cons(YasAtom args, YasAtom *result)
{
	if (yas_nilp(args) || yas_nilp(yas_cdr(args)) || !yas_nilp(yas_cdr(yas_cdr(args))))
		return yas_raise_error("args-error", "cons: expected 2 arguments");

	*result = yas_cons(yas_car(args), yas_car(yas_cdr(args)));

	return YAS_ERROR_OK;
}

int yas_builtin_eq(YasAtom args, YasAtom *result)
{
	YasAtom a, b;
	int eq;

	if (yas_nilp(args) || yas_nilp(yas_cdr(args)) || !yas_nilp(yas_cdr(yas_cdr(args))))
		return yas_raise_error("args-error", "eq?: expected 2 arguments");

	a = yas_car(args);
	b = yas_car(yas_cdr(args));

	if (a.type == b.type) {
		switch (a.type) {
		case YAS_ATOM_TYPE_NIL:
			eq = 1;
			break;
		case YAS_ATOM_TYPE_PAIR:
		case YAS_ATOM_TYPE_CLOSURE:
		case YAS_ATOM_TYPE_MACRO:
			eq = (a.value.pair == b.value.pair);
			break;
		case YAS_ATOM_TYPE_SYMBOL:
			eq = (a.value.symbol == b.value.symbol);
			break;
		case YAS_ATOM_TYPE_KEYWORD:
			eq = (a.value.keyword == b.value.keyword);
			break;
		case YAS_ATOM_TYPE_INTEGER:
			eq = (a.value.integer == b.value.integer);
			break;
		case YAS_ATOM_TYPE_BUILTIN:
			eq = (a.value.builtin == b.value.builtin);
			break;
		default:
			eq = 0;
			break;
		}
	} else {
		eq = 0;
	}

	*result = eq ? yas_make_sym("T") : yas_nil;
	return YAS_ERROR_OK;
}

int yas_builtin_pairp(YasAtom args, YasAtom *result)
{
	if (yas_nilp(args) || !yas_nilp(yas_cdr(args)))
		return yas_raise_error("args-error", "pair?: expected 1 argument");

	*result = (yas_car(args).type == YAS_ATOM_TYPE_PAIR) ? yas_make_sym("T") : yas_nil;
	return YAS_ERROR_OK;
}

int yas_builtin_procp(YasAtom args, YasAtom *result)
{
	if (yas_nilp(args) || !yas_nilp(yas_cdr(args)))
		return yas_raise_error("args-error", "procedure?: expected 1 argument");

	*result = (yas_car(args).type == YAS_ATOM_TYPE_BUILTIN
		|| yas_car(args).type == YAS_ATOM_TYPE_CLOSURE) ? yas_make_sym("T") : yas_nil;
	return YAS_ERROR_OK;
}

/* (length list) */
int yas_builtin_list_length(YasAtom args, YasAtom *result)
{
	if (yas_nilp(args) || !yas_nilp(yas_cdr(args)))
		return yas_raise_error("args-error", "length: expected 1 argument");
	YasAtom a = yas_car(args);
	long len = 0;
	while (!yas_nilp(a)) {
		if (a.type != YAS_ATOM_TYPE_PAIR) return yas_raise_error("type-error", "length: improper list");
		len++;
		a = yas_cdr(a);
	}
	*result = yas_make_int(len);
	return YAS_ERROR_OK;
}

/* (append l1 l2 ...) */
int yas_builtin_list_append(YasAtom args, YasAtom *result)
{
	YasAtom head = yas_nil, tail = yas_nil;
	/* `head` accumulates across repeated yas_cons() calls below, same
	 * shape as yas_list_create()'s accumulator (data.c) - root it for
	 * the duration. `args` (this builtin's own input) needs no root:
	 * it's reachable via the evaluator's rooted call-frame args (see
	 * eval_do_apply(), eval.c) for as long as this call is on the
	 * C stack. `tail` needs no separate root once linked in, reachable
	 * by walking cdrs from `head`. */
	yas_gc_push_root(&head);
	YasAtom p = args;
	while (!yas_nilp(p)) {
		YasAtom list = yas_car(p);
		while (!yas_nilp(list)) {
			if (list.type != YAS_ATOM_TYPE_PAIR) {
				yas_gc_pop_roots(1);
				return yas_raise_error("type-error", "append: arguments must be lists");
			}
			YasAtom new_cons = yas_cons(yas_car(list), yas_nil);
			if (yas_nilp(head)) {
				head = tail = new_cons;
			} else {
				yas_set_cdr(tail, new_cons);
				tail = new_cons;
			}
			list = yas_cdr(list);
		}
		p = yas_cdr(p);
	}
	*result = head;
	yas_gc_pop_roots(1);
	return YAS_ERROR_OK;
}

int yas_builtin_throw(YasAtom args, YasAtom *result)
{
	if (yas_nilp(args) || !yas_nilp(yas_cdr(args)))
		return yas_raise_error("args-error", "throw: expected 1 argument");

	yas_ctx_exception = yas_car(args);
	return YAS_ERROR_EXCEPTION;
}

int yas_builtin_error(YasAtom args, YasAtom *result)
{
	if (yas_nilp(args) || !yas_nilp(yas_cdr(args)))
		return yas_raise_error("args-error", "error: expected 1 argument");

	yas_ctx_exception = yas_car(args);
	return YAS_ERROR_EXCEPTION;
}

/* (exit [code]) - Terminate the process with the given status code (default 0) */
int yas_builtin_exit(YasAtom args, YasAtom *result)
{
	long code = 0;

	if (!yas_nilp(args)) {
		if (!yas_nilp(yas_cdr(args)))
			return yas_raise_error("args-error", "exit: expected 0 or 1 arguments");
		if (yas_car(args).type != YAS_ATOM_TYPE_INTEGER)
			return yas_raise_error("type-error", "exit: argument must be an integer");
		code = yas_car(args).value.integer;
	}

	exit((int) code);
}

/* (procedure-doc proc) - Get docstring from procedure/macro */
int yas_builtin_procedure_doc(YasAtom args, YasAtom *result)
{
	if (yas_nilp(args) || !yas_nilp(yas_cdr(args)))
		return yas_raise_error("args-error", "procedure-doc: expected 1 argument");
	
	YasAtom proc = yas_car(args);
	if (proc.type != YAS_ATOM_TYPE_CLOSURE && proc.type != YAS_ATOM_TYPE_MACRO)
		return yas_raise_error("type-error", "procedure-doc: argument must be procedure or macro");
	
	/* Structure: (env (args docstring . body)) */
	*result = yas_car(yas_cdr(yas_cdr(proc)));
	return YAS_ERROR_OK;
}

/* (gc) - run a full collection cycle to completion */
int yas_builtin_gc(YasAtom args, YasAtom *result)
{
	if (!yas_nilp(args))
		return yas_raise_error("args-error", "gc: expected 0 arguments");
	yas_gc();
	*result = yas_nil;
	return YAS_ERROR_OK;
}

/* (gc-start) - begin a cycle without draining it; mainly useful for
 * tests/tools that need to observe the collector mid-cycle. */
int yas_builtin_gc_start(YasAtom args, YasAtom *result)
{
	if (!yas_nilp(args))
		return yas_raise_error("args-error", "gc-start: expected 0 arguments");
	yas_gc_start();
	*result = yas_nil;
	return YAS_ERROR_OK;
}

/* (gc-stats) -> #hash{allocated: <bytes> phase: <int>}
 * phase: 0=idle 1=marking 2=sweeping - see yas_gc_phase() in yas.h. */
int yas_builtin_gc_stats(YasAtom args, YasAtom *result)
{
	if (!yas_nilp(args))
		return yas_raise_error("args-error", "gc-stats: expected 0 arguments");
	YasAtom h = yas_make_hash();
	yas_hash_set(h, yas_make_keyword("ALLOCATED"), yas_make_int((long)yas_gc_allocated()));
	yas_hash_set(h, yas_make_keyword("PHASE"), yas_make_int(yas_gc_phase()));
	*result = h;
	return YAS_ERROR_OK;
}

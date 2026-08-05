#include "lisp.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

int raise_error(const char *type, const char *msg)
{
	extern const char *current_file_path;
	
	Atom t = make_sym(type);
	Atom m = make_string(msg);
	
	/* Include file path in error message if available */
	if (current_file_path) {
		char *full_msg = malloc(strlen(msg) + strlen(current_file_path) + 50);
		snprintf(full_msg, strlen(msg) + strlen(current_file_path) + 50, 
		         "%s (in %s)", msg, current_file_path);
		m = make_string(full_msg);
		free(full_msg);
	}
	
	ctx_exception = cons(t, cons(m, nil));
	return Error_Exception;
}

int builtin_car(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "car: expected 1 argument");

	if (nilp(car(args)))
		*result = nil;
	else if (car(args).type != AtomType_Pair)
		return raise_error("type-error", "car: argument must be pair");
	else
		*result = car(car(args));

	return Error_OK;
}

int builtin_cdr(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "cdr: expected 1 argument");

	if (nilp(car(args)))
		*result = nil;
	else if (car(args).type != AtomType_Pair)
		return raise_error("type-error", "cdr: argument must be pair");
	else
		*result = cdr(car(args));

	return Error_OK;
}

int builtin_cons(Atom args, Atom *result)
{
	if (nilp(args) || nilp(cdr(args)) || !nilp(cdr(cdr(args))))
		return raise_error("args-error", "cons: expected 2 arguments");

	*result = cons(car(args), car(cdr(args)));

	return Error_OK;
}

int builtin_eq(Atom args, Atom *result)
{
	Atom a, b;
	int eq;

	if (nilp(args) || nilp(cdr(args)) || !nilp(cdr(cdr(args))))
		return raise_error("args-error", "eq?: expected 2 arguments");

	a = car(args);
	b = car(cdr(args));

	if (a.type == b.type) {
		switch (a.type) {
		case AtomType_Nil:
			eq = 1;
			break;
		case AtomType_Pair:
		case AtomType_Closure:
		case AtomType_Macro:
			eq = (a.value.pair == b.value.pair);
			break;
		case AtomType_Symbol:
			eq = (a.value.symbol == b.value.symbol);
			break;
		case AtomType_Keyword:
			eq = (a.value.keyword == b.value.keyword);
			break;
		case AtomType_Integer:
			eq = (a.value.integer == b.value.integer);
			break;
		case AtomType_Builtin:
			eq = (a.value.builtin == b.value.builtin);
			break;
		default:
			eq = 0;
			break;
		}
	} else {
		eq = 0;
	}

	*result = eq ? make_sym("T") : nil;
	return Error_OK;
}

int builtin_pairp(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "pair?: expected 1 argument");

	*result = (car(args).type == AtomType_Pair) ? make_sym("T") : nil;
	return Error_OK;
}

int builtin_procp(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "procedure?: expected 1 argument");

	*result = (car(args).type == AtomType_Builtin
		|| car(args).type == AtomType_Closure) ? make_sym("T") : nil;
	return Error_OK;
}

/* (length list) */
int builtin_list_length(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "length: expected 1 argument");
	Atom a = car(args);
	long len = 0;
	while (!nilp(a)) {
		if (a.type != AtomType_Pair) return raise_error("type-error", "length: improper list");
		len++;
		a = cdr(a);
	}
	*result = make_int(len);
	return Error_OK;
}

/* (append l1 l2 ...) */
int builtin_list_append(Atom args, Atom *result)
{
	Atom head = nil, tail = nil;
	Atom p = args;
	while (!nilp(p)) {
		Atom list = car(p);
		while (!nilp(list)) {
			if (list.type != AtomType_Pair)
				return raise_error("type-error", "append: arguments must be lists");
			Atom new_cons = cons(car(list), nil);
			if (nilp(head)) {
				head = tail = new_cons;
			} else {
				cdr(tail) = new_cons;
				tail = new_cons;
			}
			list = cdr(list);
		}
		p = cdr(p);
	}
	*result = head;
	return Error_OK;
}

int builtin_throw(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "throw: expected 1 argument");

	ctx_exception = car(args);
	return Error_Exception;
}

int builtin_error(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "error: expected 1 argument");

	ctx_exception = car(args);
	return Error_Exception;
}

/* (exit [code]) - Terminate the process with the given status code (default 0) */
int builtin_exit(Atom args, Atom *result)
{
	long code = 0;

	if (!nilp(args)) {
		if (!nilp(cdr(args)))
			return raise_error("args-error", "exit: expected 0 or 1 arguments");
		if (car(args).type != AtomType_Integer)
			return raise_error("type-error", "exit: argument must be an integer");
		code = car(args).value.integer;
	}

	exit((int) code);
}

/* (procedure-doc proc) - Get docstring from procedure/macro */
int builtin_procedure_doc(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "procedure-doc: expected 1 argument");
	
	Atom proc = car(args);
	if (proc.type != AtomType_Closure && proc.type != AtomType_Macro)
		return raise_error("type-error", "procedure-doc: argument must be procedure or macro");
	
	/* Structure: (env (args docstring . body)) */
	*result = car(cdr(cdr(proc)));
	return Error_OK;
}

#include "lisp.h"

/* (char? obj) */
int builtin_charp(Atom args, Atom *result) {
	if (nilp(args)) return raise_error("args-error", "char?: expected 1 argument");
	*result = car(args).type == AtomType_Char ? make_sym("T") : nil;
	return Error_OK;
}

/* (char->integer char) */
int builtin_char_to_integer(Atom args, Atom *result) {
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "char->integer: expected 1 argument");
	Atom a = car(args);
	if (a.type != AtomType_Char)
		return raise_error("type-error", "char->integer: argument must be a character");
	*result = make_int(a.value.character);
	return Error_OK;
}

/* (integer->char int) */
int builtin_integer_to_char(Atom args, Atom *result) {
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "integer->char: expected 1 argument");
	Atom a = car(args);
	if (a.type != AtomType_Integer)
		return raise_error("type-error", "integer->char: argument must be an integer");
	*result = make_char((int32_t)a.value.integer);
	return Error_OK;
}

/* (char=? a b) */
int builtin_char_eq(Atom args, Atom *result) {
	if (nilp(args) || nilp(cdr(args)) || !nilp(cdr(cdr(args))))
		return raise_error("args-error", "char=?: expected 2 arguments");
	Atom a = car(args), b = car(cdr(args));
	if (a.type != AtomType_Char || b.type != AtomType_Char)
		return raise_error("type-error", "char=?: arguments must be characters");
	*result = (a.value.character == b.value.character) ? make_sym("T") : nil;
	return Error_OK;
}

/* (char<? a b) */
int builtin_char_lt(Atom args, Atom *result) {
	if (nilp(args) || nilp(cdr(args)) || !nilp(cdr(cdr(args))))
		return raise_error("args-error", "char<?: expected 2 arguments");
	Atom a = car(args), b = car(cdr(args));
	if (a.type != AtomType_Char || b.type != AtomType_Char)
		return raise_error("type-error", "char<?: arguments must be characters");
	*result = (a.value.character < b.value.character) ? make_sym("T") : nil;
	return Error_OK;
}

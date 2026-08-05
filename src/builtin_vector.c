#include "lisp.h"

int builtin_vectorp(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "vector?: expected 1 argument");
	*result = (car(args).type == AtomType_Vector) ? make_sym("T") : nil;
	return Error_OK;
}

/* (vector-length vec) */
int builtin_vector_length(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "vector-length: expected 1 argument");
	Atom a = car(args);
	if (a.type != AtomType_Vector)
		return raise_error("type-error", "vector-length: expected vector");
	*result = make_int(vector_length(a));
	return Error_OK;
}

/* (vector-append v1 v2 ...) */
int builtin_vector_append(Atom args, Atom *result)
{
	size_t total_len = 0;
	Atom p = args;
	while (!nilp(p)) {
		if (car(p).type != AtomType_Vector)
			return raise_error("type-error", "vector-append: arguments must be vectors");
		total_len += vector_length(car(p));
		p = cdr(p);
	}
	*result = make_vector(total_len, nil);
	size_t idx = 0;
	p = args;
	while (!nilp(p)) {
		Atom v = car(p);
		for (size_t i = 0; i < vector_length(v); i++) {
			vector_set(*result, idx++, vector_ref(v, i));
		}
		p = cdr(p);
	}
	return Error_OK;
}

/* Vector operations */
int builtin_make_vector(Atom args, Atom *result)
{
	if (nilp(args))
		return raise_error("args-error", "make-vector: expected size argument");
	
	Atom size_arg = car(args);
	if (size_arg.type != AtomType_Integer)
		return raise_error("type-error", "make-vector: size must be integer");
	
	Atom fill = nil;
	if (!nilp(cdr(args))) {
		fill = car(cdr(args));
		if (!nilp(cdr(cdr(args))))
			return raise_error("args-error", "make-vector: expected 1 or 2 arguments");
	}
	
	*result = make_vector((size_t)size_arg.value.integer, fill);
	return Error_OK;
}

int builtin_vector_ref(Atom args, Atom *result)
{
	if (nilp(args) || nilp(cdr(args)) || !nilp(cdr(cdr(args))))
		return raise_error("args-error", "vector-ref: expected 2 arguments");
	
	Atom vec = car(args);
	Atom idx = car(cdr(args));
	
	if (vec.type != AtomType_Vector || idx.type != AtomType_Integer)
		return raise_error("type-error", "vector-ref: arguments must be vector and integer");
	
	if (idx.value.integer < 0 || (size_t)idx.value.integer >= vector_length(vec))
		return raise_error("args-error", "vector-ref: index out of bounds");
	
	*result = vector_ref(vec, (size_t)idx.value.integer);
	return Error_OK;
}

int builtin_vector_set(Atom args, Atom *result)
{
	if (nilp(args) || nilp(cdr(args)) || nilp(cdr(cdr(args))) ||
	    !nilp(cdr(cdr(cdr(args)))))
		return raise_error("args-error", "vector-set!: expected 3 arguments");
	
	Atom vec = car(args);
	Atom idx = car(cdr(args));
	Atom val = car(cdr(cdr(args)));
	
	if (vec.type != AtomType_Vector || idx.type != AtomType_Integer)
		return raise_error("type-error", "vector-set!: arguments must be vector, integer and value");
	
	if (idx.value.integer < 0 || (size_t)idx.value.integer >= vector_length(vec))
		return raise_error("args-error", "vector-set!: index out of bounds");
	
	vector_set(vec, (size_t)idx.value.integer, val);
	*result = val;
	return Error_OK;
}

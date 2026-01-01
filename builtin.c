#include "lisp.h"
#include <stdlib.h>
#include <string.h>

int builtin_car(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return Error_Args;

	if (nilp(car(args)))
		*result = nil;
	else if (car(args).type != AtomType_Pair)
		return Error_Type;
	else
		*result = car(car(args));

	return Error_OK;
}

int builtin_cdr(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return Error_Args;

	if (nilp(car(args)))
		*result = nil;
	else if (car(args).type != AtomType_Pair)
		return Error_Type;
	else
		*result = cdr(car(args));

	return Error_OK;
}

int builtin_cons(Atom args, Atom *result)
{
	if (nilp(args) || nilp(cdr(args)) || !nilp(cdr(cdr(args))))
		return Error_Args;

	*result = cons(car(args), car(cdr(args)));

	return Error_OK;
}

int builtin_eq(Atom args, Atom *result)
{
	Atom a, b;
	int eq;

	if (nilp(args) || nilp(cdr(args)) || !nilp(cdr(cdr(args))))
		return Error_Args;

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
		case AtomType_Integer:
			eq = (a.value.integer == b.value.integer);
			break;
		case AtomType_Builtin:
			eq = (a.value.builtin == b.value.builtin);
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
		return Error_Args;

	*result = (car(args).type == AtomType_Pair) ? make_sym("T") : nil;
	return Error_OK;
}

int builtin_procp(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return Error_Args;

	*result = (car(args).type == AtomType_Builtin
		|| car(args).type == AtomType_Closure) ? make_sym("T") : nil;
	return Error_OK;
}

int builtin_add(Atom args, Atom *result)
{
	Atom a, b;

	if (nilp(args) || nilp(cdr(args)) || !nilp(cdr(cdr(args))))
		return Error_Args;

	a = car(args);
	b = car(cdr(args));

	if (a.type != AtomType_Integer || b.type != AtomType_Integer)
		return Error_Type;

	*result = make_int(a.value.integer + b.value.integer);

	return Error_OK;
}

int builtin_subtract(Atom args, Atom *result)
{
	Atom a, b;

	if (nilp(args) || nilp(cdr(args)) || !nilp(cdr(cdr(args))))
		return Error_Args;

	a = car(args);
	b = car(cdr(args));

	if (a.type != AtomType_Integer || b.type != AtomType_Integer)
		return Error_Type;

	*result = make_int(a.value.integer - b.value.integer);

	return Error_OK;
}

int builtin_multiply(Atom args, Atom *result)
{
	Atom a, b;

	if (nilp(args) || nilp(cdr(args)) || !nilp(cdr(cdr(args))))
		return Error_Args;

	a = car(args);
	b = car(cdr(args));

	if (a.type != AtomType_Integer || b.type != AtomType_Integer)
		return Error_Type;

	*result = make_int(a.value.integer * b.value.integer);

	return Error_OK;
}

int builtin_divide(Atom args, Atom *result)
{
	Atom a, b;

	if (nilp(args) || nilp(cdr(args)) || !nilp(cdr(cdr(args))))
		return Error_Args;

	a = car(args);
	b = car(cdr(args));

	if (a.type != AtomType_Integer || b.type != AtomType_Integer)
		return Error_Type;

	*result = make_int(a.value.integer / b.value.integer);

	return Error_OK;
}

int builtin_numeq(Atom args, Atom *result)
{
	Atom a, b;

	if (nilp(args) || nilp(cdr(args)) || !nilp(cdr(cdr(args))))
		return Error_Args;

	a = car(args);
	b = car(cdr(args));

	if (a.type != AtomType_Integer || b.type != AtomType_Integer)
		return Error_Type;

	*result = (a.value.integer == b.value.integer) ? make_sym("T") : nil;

	return Error_OK;
}

int builtin_less(Atom args, Atom *result)
{
	Atom a, b;

	if (nilp(args) || nilp(cdr(args)) || !nilp(cdr(cdr(args))))
		return Error_Args;

	a = car(args);
	b = car(cdr(args));

	if (a.type != AtomType_Integer || b.type != AtomType_Integer)
		return Error_Type;

	*result = (a.value.integer < b.value.integer) ? make_sym("T") : nil;

	return Error_OK;
}

/* Type predicates */
int builtin_floatp(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return Error_Args;
	*result = (car(args).type == AtomType_Float) ? make_sym("T") : nil;
	return Error_OK;
}

int builtin_stringp(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return Error_Args;
	*result = (car(args).type == AtomType_String) ? make_sym("T") : nil;
	return Error_OK;
}

int builtin_vectorp(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return Error_Args;
	*result = (car(args).type == AtomType_Vector) ? make_sym("T") : nil;
	return Error_OK;
}

int builtin_hashp(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return Error_Args;
	*result = (car(args).type == AtomType_Hash) ? make_sym("T") : nil;
	return Error_OK;
}

/* Length - works on lists, strings, vectors, hashes */
int builtin_length(Atom args, Atom *result)
{
	Atom a;
	if (nilp(args) || !nilp(cdr(args)))
		return Error_Args;
	
	a = car(args);
	
	switch (a.type) {
	case AtomType_Nil:
		*result = make_int(0);
		break;
	case AtomType_Pair: {
		long len = 0;
		while (!nilp(a)) {
			if (a.type != AtomType_Pair) return Error_Type;
			len++;
			a = cdr(a);
		}
		*result = make_int(len);
		break;
	}
	case AtomType_String:
		*result = make_int(strlen(a.value.string));
		break;
	case AtomType_Vector:
		*result = make_int(vector_length(a));
		break;
	case AtomType_Hash:
		*result = make_int(hash_size(a));
		break;
	default:
		return Error_Type;
	}
	return Error_OK;
}

/* Concat - works on lists, strings, vectors */
int builtin_concat(Atom args, Atom *result)
{
	if (nilp(args))
		return Error_Args;
	
	Atom first = car(args);
	
	if (first.type == AtomType_String) {
		/* String concatenation */
		size_t total_len = 0;
		Atom p = args;
		while (!nilp(p)) {
			if (car(p).type != AtomType_String)
				return Error_Type;
			total_len += strlen(car(p).value.string);
			p = cdr(p);
		}
		char *buf = malloc(total_len + 1);
		buf[0] = '\0';
		p = args;
		while (!nilp(p)) {
			strcat(buf, car(p).value.string);
			p = cdr(p);
		}
		result->type = AtomType_String;
		result->value.string = buf;
	} else if (first.type == AtomType_Pair || first.type == AtomType_Nil) {
		/* List concatenation */
		Atom head = nil, tail = nil;
		Atom p = args;
		while (!nilp(p)) {
			Atom list = car(p);
			while (!nilp(list)) {
				if (list.type != AtomType_Pair)
					return Error_Type;
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
	} else if (first.type == AtomType_Vector) {
		/* Vector concatenation */
		size_t total_len = 0;
		Atom p = args;
		while (!nilp(p)) {
			if (car(p).type != AtomType_Vector)
				return Error_Type;
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
	} else {
		return Error_Type;
	}
	return Error_OK;
}

/* Vector operations */
int builtin_make_vector(Atom args, Atom *result)
{
	if (nilp(args))
		return Error_Args;
	
	Atom size_arg = car(args);
	if (size_arg.type != AtomType_Integer)
		return Error_Type;
	
	Atom fill = nil;
	if (!nilp(cdr(args))) {
		fill = car(cdr(args));
		if (!nilp(cdr(cdr(args))))
			return Error_Args;
	}
	
	*result = make_vector((size_t)size_arg.value.integer, fill);
	return Error_OK;
}

int builtin_vector_ref(Atom args, Atom *result)
{
	if (nilp(args) || nilp(cdr(args)) || !nilp(cdr(cdr(args))))
		return Error_Args;
	
	Atom vec = car(args);
	Atom idx = car(cdr(args));
	
	if (vec.type != AtomType_Vector || idx.type != AtomType_Integer)
		return Error_Type;
	
	if (idx.value.integer < 0 || (size_t)idx.value.integer >= vector_length(vec))
		return Error_Args;
	
	*result = vector_ref(vec, (size_t)idx.value.integer);
	return Error_OK;
}

int builtin_vector_set(Atom args, Atom *result)
{
	if (nilp(args) || nilp(cdr(args)) || nilp(cdr(cdr(args))) ||
	    !nilp(cdr(cdr(cdr(args)))))
		return Error_Args;
	
	Atom vec = car(args);
	Atom idx = car(cdr(args));
	Atom val = car(cdr(cdr(args)));
	
	if (vec.type != AtomType_Vector || idx.type != AtomType_Integer)
		return Error_Type;
	
	if (idx.value.integer < 0 || (size_t)idx.value.integer >= vector_length(vec))
		return Error_Args;
	
	vector_set(vec, (size_t)idx.value.integer, val);
	*result = val;
	return Error_OK;
}

/* Hash operations */
int builtin_make_hash(Atom args, Atom *result)
{
	if (!nilp(args))
		return Error_Args;
	
	*result = make_hash();
	return Error_OK;
}

int builtin_hash_ref(Atom args, Atom *result)
{
	if (nilp(args) || nilp(cdr(args)))
		return Error_Args;
	
	Atom hash = car(args);
	Atom key = car(cdr(args));
	Atom default_val = nil;
	
	if (!nilp(cdr(cdr(args)))) {
		default_val = car(cdr(cdr(args)));
		if (!nilp(cdr(cdr(cdr(args)))))
			return Error_Args;
	}
	
	if (hash.type != AtomType_Hash)
		return Error_Type;
	
	*result = hash_ref(hash, key, default_val);
	return Error_OK;
}

int builtin_hash_set(Atom args, Atom *result)
{
	if (nilp(args) || nilp(cdr(args)) || nilp(cdr(cdr(args))) ||
	    !nilp(cdr(cdr(cdr(args)))))
		return Error_Args;
	
	Atom hash = car(args);
	Atom key = car(cdr(args));
	Atom val = car(cdr(cdr(args)));
	
	if (hash.type != AtomType_Hash)
		return Error_Type;
	
	hash_set(hash, key, val);
	*result = val;
	return Error_OK;
}

int builtin_hash_remove(Atom args, Atom *result)
{
	if (nilp(args) || nilp(cdr(args)) || !nilp(cdr(cdr(args))))
		return Error_Args;
	
	Atom hash = car(args);
	Atom key = car(cdr(args));
	
	if (hash.type != AtomType_Hash)
		return Error_Type;
	
	*result = hash_remove(hash, key) ? make_sym("T") : nil;
	return Error_OK;
}

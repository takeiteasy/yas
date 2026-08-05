#include "lisp.h"

int builtin_hashp(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "hash?: expected 1 argument");
	*result = (car(args).type == AtomType_Hash) ? make_sym("T") : nil;
	return Error_OK;
}

/* (hash-length hash) */
int builtin_hash_length(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "hash-length: expected 1 argument");
	Atom a = car(args);
	if (a.type != AtomType_Hash)
		return raise_error("type-error", "hash-length: expected hash");
	*result = make_int(hash_size(a));
	return Error_OK;
}

/* (hash-join h1 h2 ...) - merges h2 into h1, but since we are functional-ish, 
   lets return NEW hash with all contents. Last one wins. */
int builtin_hash_join(Atom args, Atom *result)
{
	Atom new_h = make_hash();
	Atom p = args;
	while (!nilp(p)) {
		Atom h = car(p);
		if (h.type != AtomType_Hash)
			return raise_error("type-error", "hash-join: arguments must be hashes");
		
		struct Hash *src = h.value.hash;
		for (size_t i = 0; i < src->size; i++) {
			hash_set(new_h, src->keys[i], src->values[i]);
		}
		p = cdr(p);
	}
	*result = new_h;
	return Error_OK;
}

/* Hash operations */
int builtin_make_hash(Atom args, Atom *result)
{
	if (!nilp(args))
		return raise_error("args-error", "make-hash: expected 0 arguments");
	
	*result = make_hash();
	return Error_OK;
}

int builtin_hash_ref(Atom args, Atom *result)
{
	if (nilp(args) || nilp(cdr(args)))
		return raise_error("args-error", "hash-ref: expected at least 2 arguments");
	
	Atom hash = car(args);
	Atom key = car(cdr(args));
	Atom default_val = nil;
	
	if (!nilp(cdr(cdr(args)))) {
		default_val = car(cdr(cdr(args)));
		if (!nilp(cdr(cdr(cdr(args)))))
			return raise_error("args-error", "hash-ref: expected 2 or 3 arguments");
	}
	
	if (hash.type != AtomType_Hash)
		return raise_error("type-error", "hash-ref: first argument must be hash");
	
	*result = hash_ref(hash, key, default_val);
	return Error_OK;
}

int builtin_hash_set(Atom args, Atom *result)
{
	if (nilp(args) || nilp(cdr(args)) || nilp(cdr(cdr(args))) ||
	    !nilp(cdr(cdr(cdr(args)))))
		return raise_error("args-error", "hash-set!: expected 3 arguments");
	
	Atom hash = car(args);
	Atom key = car(cdr(args));
	Atom val = car(cdr(cdr(args)));
	
	if (hash.type != AtomType_Hash)
		return raise_error("type-error", "hash-set!: first argument must be hash");
	
	hash_set(hash, key, val);
	*result = val;
	return Error_OK;
}

int builtin_hash_remove(Atom args, Atom *result)
{
	if (nilp(args) || nilp(cdr(args)) || !nilp(cdr(cdr(args))))
		return raise_error("args-error", "hash-remove!: expected 2 arguments");
	
	Atom hash = car(args);
	Atom key = car(cdr(args));
	
	if (hash.type != AtomType_Hash)
		return raise_error("type-error", "hash-remove!: first argument must be hash");
	
	*result = hash_remove(hash, key) ? make_sym("T") : nil;
	return Error_OK;
}

int builtin_hash_keys(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "hash-keys: expected 1 argument");
	
	Atom hash = car(args);
	if (hash.type != AtomType_Hash)
		return raise_error("type-error", "hash-keys: argument must be hash");
	
	Atom keys = nil;
	struct Hash *h = hash.value.hash;
	
	for (size_t i = 0; i < h->size; i++) {
		keys = cons(h->keys[i], keys);
	}
	
	*result = keys;
	return Error_OK;
}

/* Metatable operations */
int builtin_set_metatable(Atom args, Atom *result)
{
	if (nilp(args) || nilp(cdr(args)) || !nilp(cdr(cdr(args))))
		return raise_error("args-error", "set-metatable!: expected 2 arguments");
	
	Atom hash = car(args);
	Atom mt = car(cdr(args));
	
	if (hash.type != AtomType_Hash)
		return raise_error("type-error", "set-metatable!: first argument must be hash");
	
	if (!nilp(mt) && mt.type != AtomType_Hash)
		return raise_error("type-error", "set-metatable!: second argument must be hash or nil");
	
	if (nilp(mt)) {
		hash.value.hash->metatable = NULL;
	} else {
		hash.value.hash->metatable = mt.value.hash;
	}
	
	*result = hash;
	return Error_OK;
}

int builtin_get_metatable(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "get-metatable: expected 1 argument");
	
	Atom hash = car(args);
	
	if (hash.type != AtomType_Hash)
		return raise_error("type-error", "get-metatable: argument must be hash");
	
	if (hash.value.hash->metatable) {
		result->type = AtomType_Hash;
		result->value.hash = hash.value.hash->metatable;
	} else {
		*result = nil;
	}
	
	return Error_OK;
}

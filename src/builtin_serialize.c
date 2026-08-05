#include "lisp.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Serialization Helpers */

static size_t serialize_calc_len(Atom atom) {
	char buf[64];
	switch (atom.type) {
	case AtomType_Nil:
		return 3; /* "nil" or "()" ? lisd uses nil? lisd.h: LD_PRIMITIVE. 
		             If we want compatible with common lisp usage in lisd, maybe "nil". 
				 But plan says: "Hash Table: (:key1 val1)". 
				 nil is usually printed as NIL. 
				 Let's use "nil" for now. */
		return 3; 
	case AtomType_Pair: {
		/* List -> Vector #(...) */
		size_t len = 2; /* "#(" */
		Atom p = atom;
		int first = 1;
		while (!nilp(p)) {
			if (p.type == AtomType_Pair) {
				if (!first) len++; /* space */
				len += serialize_calc_len(car(p));
				p = cdr(p);
				first = 0;
			} else {
				/* Dotted list - not supported well in lisd array? 
				   Treat as element. */
				if (!first) len += 3; /* " . " */
				len += serialize_calc_len(p);
				break;
			}
		}
		len += 1; /* ")" */
		return len;
	}
	case AtomType_Vector: {
		/* Vector -> Vector #(...) */
		size_t len = 2; /* "#(" */
		for (size_t i = 0; i < atom.value.vector->size; i++) {
			if (i > 0) len++;
			len += serialize_calc_len(atom.value.vector->data[i]);
		}
		len += 1; /* ")" */
		return len;
	}
	case AtomType_Hash: {
		/* Hash -> (:key value ...) */
		size_t len = 1; /* "(" */
		struct Hash *h = atom.value.hash;
		int first = 1;
		for (size_t i = 0; i < h->size; i++) {
			if (!first) len++; /* space */
			len += serialize_calc_len(h->keys[i]);
			len++; /* space */
			len += serialize_calc_len(h->values[i]);
			first = 0;
		}
		len += 1; /* ")" */
		return len;
	}
	case AtomType_Integer:
		snprintf(buf, sizeof(buf), "%ld", atom.value.integer);
		return strlen(buf);
	case AtomType_Float:
		snprintf(buf, sizeof(buf), "%g", atom.value.floatval);
		return strlen(buf);
	case AtomType_String: {
		size_t len = 2; /* quotes */
		for (char *s = atom.value.string; *s; s++) {
			if (*s == '"' || *s == '\\') len++;
			len++;
		}
		return len;
	}
	case AtomType_Symbol:
		return strlen(atom.value.symbol);
	case AtomType_Keyword:
		return strlen(atom.value.keyword) + 1;
	default:
		return 0;
	}
}

static void serialize_write(Atom atom, char *buf, size_t *pos) {
	char tmp[64];
	switch (atom.type) {
	case AtomType_Nil:
		memcpy(buf + *pos, "nil", 3);
		*pos += 3;
		break;
	case AtomType_Pair: {
		/* List -> Vector #(...) */
		memcpy(buf + *pos, "#(", 2);
		*pos += 2;
		Atom p = atom;
		int first = 1;
		while (!nilp(p)) {
			if (p.type == AtomType_Pair) {
				if (!first) buf[(*pos)++] = ' ';
				serialize_write(car(p), buf, pos);
				p = cdr(p);
				first = 0;
			} else {
				if (!first) {
					memcpy(buf + *pos, " . ", 3);
					*pos += 3;
				}
				serialize_write(p, buf, pos);
				break;
			}
		}
		buf[(*pos)++] = ')';
		break;
	}
	case AtomType_Vector: {
		memcpy(buf + *pos, "#(", 2);
		*pos += 2;
		for (size_t i = 0; i < atom.value.vector->size; i++) {
			if (i > 0) buf[(*pos)++] = ' ';
			serialize_write(atom.value.vector->data[i], buf, pos);
		}
		buf[(*pos)++] = ')';
		break;
	}
	case AtomType_Hash: {
		buf[(*pos)++] = '(';
		struct Hash *h = atom.value.hash;
		int first = 1;
		for (size_t i = 0; i < h->size; i++) {
			if (!first) buf[(*pos)++] = ' ';
			serialize_write(h->keys[i], buf, pos);
			buf[(*pos)++] = ' ';
			serialize_write(h->values[i], buf, pos);
			first = 0;
		}
		buf[(*pos)++] = ')';
		break;
	}
	case AtomType_Integer:
		sprintf(tmp, "%ld", atom.value.integer);
		strcpy(buf + *pos, tmp);
		*pos += strlen(tmp);
		break;
	case AtomType_Float:
		sprintf(tmp, "%g", atom.value.floatval);
		strcpy(buf + *pos, tmp);
		*pos += strlen(tmp);
		break;
	case AtomType_String:
		buf[(*pos)++] = '"';
		for (char *s = atom.value.string; *s; s++) {
			if (*s == '"' || *s == '\\') buf[(*pos)++] = '\\';
			buf[(*pos)++] = *s;
		}
		buf[(*pos)++] = '"';
		break;
	case AtomType_Symbol: {
		size_t len = strlen(atom.value.symbol);
		memcpy(buf + *pos, atom.value.symbol, len);
		*pos += len;
		break;
	}
	case AtomType_Keyword:
		buf[(*pos)++] = ':';
		size_t len = strlen(atom.value.keyword);
		memcpy(buf + *pos, atom.value.keyword, len);
		*pos += len;
		break;
	default:
		/* Unserializable types; keep in sync with serialize_calc_len (returns 0) */
		break;
	}
}

int builtin_serialize(Atom args, Atom *result) {
	if (nilp(args) || !nilp(cdr(args)))
		return Error_Args;
	
	Atom obj = car(args);
	size_t len = serialize_calc_len(obj);
	
	char *buf = malloc(len + 1);
	if (!buf) return Error_Exception; /* Should refer to allocation error */
	
	size_t pos = 0;
	serialize_write(obj, buf, &pos);
	buf[pos] = '\0';
	
	*result = make_string(buf);
	free(buf);
	return Error_OK;
}

/* Helper to convert Lists to Hash Tables recursively */
static Atom lisd_convert(Atom atom) {
	if (atom.type == AtomType_Pair) {
		/* Convert List to Hash */
		Atom h = make_hash();
		Atom p = atom;
		while (!nilp(p)) {
			if (p.type != AtomType_Pair) break; /* Should not happen in proper list */
			
			Atom key = car(p);
			p = cdr(p);
			
			Atom val = nil;
			if (!nilp(p)) {
				if (p.type == AtomType_Pair) {
					val = car(p);
					p = cdr(p);
				} else {
					/* Unexpected dotted pair end? Use p as value? */
					val = p;
					p = nil;
				}
			}
			
			/* Recursively convert value */
			val = lisd_convert(val);
			hash_set(h, key, val);
		}
		return h;
	} else if (atom.type == AtomType_Vector) {
		/* Recursively convert elements - Create NEW vector to avoid mutation */
		size_t size = atom.value.vector->size;
		Atom new_vec = make_vector(size, nil);
		for (size_t i = 0; i < size; i++) {
			new_vec.value.vector->data[i] = lisd_convert(atom.value.vector->data[i]);
		}
		return new_vec;
	}
	return atom;
}

int builtin_deserialize(Atom args, Atom *result) {
	if (nilp(args) || !nilp(cdr(args)))
		return Error_Args;
	
	Atom arg = car(args);
	
	if (arg.type == AtomType_String) {
		const char *input = arg.value.string;
		const char *end = input + strlen(input);
		Atom parsed;
		ErrorContext err = {0};
		
		/* Use read_expr to parse the S-expression */
		int status = read_expr(input, &end, &parsed, &err);
		
		if (status != Error_OK) {
			return status;
		}
		
		/* Convert Lists to Hashes */
		*result = lisd_convert(parsed);
	} else {
		/* Argument is already a structure (e.g. quoted list), just convert it */
		*result = lisd_convert(arg);
	}
	
	return Error_OK;
}

/* (read-string str) - Parse an S-expression from a string, returning raw result (no conversion) */
int builtin_read_string(Atom args, Atom *result) {
	if (nilp(args) || !nilp(cdr(args)))
		return Error_Args;
	
	Atom arg = car(args);
	
	if (arg.type != AtomType_String) {
		*result = nil;
		return Error_Type;
	}
	
	const char *input = arg.value.string;
	const char *end = input + strlen(input);
	ErrorContext err = {0};
	
	int status = read_expr(input, &end, result, &err);
	
	if (status != Error_OK) {
		*result = nil;
		return status;
	}
	
	return Error_OK;
}

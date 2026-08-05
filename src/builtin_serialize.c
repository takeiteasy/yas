#include "yas.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Serialization Helpers */

static size_t serialize_calc_len(YasAtom atom) {
	char buf[64];
	switch (atom.type) {
	case YAS_ATOM_TYPE_NIL:
		return 3; /* "nil" or "()" ? lisd uses nil? lisd.h: LD_PRIMITIVE. 
		             If we want compatible with common lisp usage in lisd, maybe "nil". 
				 But plan says: "Hash Table: (:key1 val1)". 
				 nil is usually printed as NIL. 
				 Let's use "nil" for now. */
		return 3; 
	case YAS_ATOM_TYPE_PAIR: {
		/* List -> Vector #(...) */
		size_t len = 2; /* "#(" */
		YasAtom p = atom;
		int first = 1;
		while (!yas_nilp(p)) {
			if (p.type == YAS_ATOM_TYPE_PAIR) {
				if (!first) len++; /* space */
				len += serialize_calc_len(yas_car(p));
				p = yas_cdr(p);
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
	case YAS_ATOM_TYPE_VECTOR: {
		/* Vector -> Vector #(...) */
		size_t len = 2; /* "#(" */
		for (size_t i = 0; i < atom.value.vector->size; i++) {
			if (i > 0) len++;
			len += serialize_calc_len(atom.value.vector->data[i]);
		}
		len += 1; /* ")" */
		return len;
	}
	case YAS_ATOM_TYPE_HASH: {
		/* Hash -> (:key value ...) */
		size_t len = 1; /* "(" */
		struct YasHash *h = atom.value.hash;
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
	case YAS_ATOM_TYPE_INTEGER:
		snprintf(buf, sizeof(buf), "%ld", atom.value.integer);
		return strlen(buf);
	case YAS_ATOM_TYPE_FLOAT:
		snprintf(buf, sizeof(buf), "%g", atom.value.floatval);
		return strlen(buf);
	case YAS_ATOM_TYPE_STRING: {
		size_t len = 2; /* quotes */
		for (char *s = atom.value.string; *s; s++) {
			if (*s == '"' || *s == '\\') len++;
			len++;
		}
		return len;
	}
	case YAS_ATOM_TYPE_SYMBOL:
		return strlen(atom.value.symbol);
	case YAS_ATOM_TYPE_KEYWORD:
		return strlen(atom.value.keyword) + 1;
	default:
		return 0;
	}
}

static void serialize_write(YasAtom atom, char *buf, size_t *pos) {
	char tmp[64];
	switch (atom.type) {
	case YAS_ATOM_TYPE_NIL:
		memcpy(buf + *pos, "nil", 3);
		*pos += 3;
		break;
	case YAS_ATOM_TYPE_PAIR: {
		/* List -> Vector #(...) */
		memcpy(buf + *pos, "#(", 2);
		*pos += 2;
		YasAtom p = atom;
		int first = 1;
		while (!yas_nilp(p)) {
			if (p.type == YAS_ATOM_TYPE_PAIR) {
				if (!first) buf[(*pos)++] = ' ';
				serialize_write(yas_car(p), buf, pos);
				p = yas_cdr(p);
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
	case YAS_ATOM_TYPE_VECTOR: {
		memcpy(buf + *pos, "#(", 2);
		*pos += 2;
		for (size_t i = 0; i < atom.value.vector->size; i++) {
			if (i > 0) buf[(*pos)++] = ' ';
			serialize_write(atom.value.vector->data[i], buf, pos);
		}
		buf[(*pos)++] = ')';
		break;
	}
	case YAS_ATOM_TYPE_HASH: {
		buf[(*pos)++] = '(';
		struct YasHash *h = atom.value.hash;
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
	case YAS_ATOM_TYPE_INTEGER:
		sprintf(tmp, "%ld", atom.value.integer);
		strcpy(buf + *pos, tmp);
		*pos += strlen(tmp);
		break;
	case YAS_ATOM_TYPE_FLOAT:
		sprintf(tmp, "%g", atom.value.floatval);
		strcpy(buf + *pos, tmp);
		*pos += strlen(tmp);
		break;
	case YAS_ATOM_TYPE_STRING:
		buf[(*pos)++] = '"';
		for (char *s = atom.value.string; *s; s++) {
			if (*s == '"' || *s == '\\') buf[(*pos)++] = '\\';
			buf[(*pos)++] = *s;
		}
		buf[(*pos)++] = '"';
		break;
	case YAS_ATOM_TYPE_SYMBOL: {
		size_t len = strlen(atom.value.symbol);
		memcpy(buf + *pos, atom.value.symbol, len);
		*pos += len;
		break;
	}
	case YAS_ATOM_TYPE_KEYWORD:
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

int yas_builtin_serialize(YasAtom args, YasAtom *result) {
	if (yas_nilp(args) || !yas_nilp(yas_cdr(args)))
		return YAS_ERROR_ARGS;
	
	YasAtom obj = yas_car(args);
	size_t len = serialize_calc_len(obj);
	
	char *buf = malloc(len + 1);
	if (!buf) return YAS_ERROR_EXCEPTION; /* Should refer to allocation error */
	
	size_t pos = 0;
	serialize_write(obj, buf, &pos);
	buf[pos] = '\0';
	
	*result = yas_make_string(buf);
	free(buf);
	return YAS_ERROR_OK;
}

/* Helper to convert Lists to Hash Tables recursively */
static YasAtom lisd_convert(YasAtom atom) {
	if (atom.type == YAS_ATOM_TYPE_PAIR) {
		/* Convert List to Hash */
		YasAtom h = yas_make_hash();
		YasAtom p = atom;
		while (!yas_nilp(p)) {
			if (p.type != YAS_ATOM_TYPE_PAIR) break; /* Should not happen in proper list */
			
			YasAtom key = yas_car(p);
			p = yas_cdr(p);
			
			YasAtom val = yas_nil;
			if (!yas_nilp(p)) {
				if (p.type == YAS_ATOM_TYPE_PAIR) {
					val = yas_car(p);
					p = yas_cdr(p);
				} else {
					/* Unexpected dotted pair end? Use p as value? */
					val = p;
					p = yas_nil;
				}
			}
			
			/* Recursively convert value */
			val = lisd_convert(val);
			yas_hash_set(h, key, val);
		}
		return h;
	} else if (atom.type == YAS_ATOM_TYPE_VECTOR) {
		/* Recursively convert elements - Create NEW vector to avoid mutation */
		size_t size = atom.value.vector->size;
		YasAtom new_vec = yas_make_vector(size, yas_nil);
		for (size_t i = 0; i < size; i++) {
			new_vec.value.vector->data[i] = lisd_convert(atom.value.vector->data[i]);
		}
		return new_vec;
	}
	return atom;
}

int yas_builtin_deserialize(YasAtom args, YasAtom *result) {
	if (yas_nilp(args) || !yas_nilp(yas_cdr(args)))
		return YAS_ERROR_ARGS;
	
	YasAtom arg = yas_car(args);
	
	if (arg.type == YAS_ATOM_TYPE_STRING) {
		const char *input = arg.value.string;
		const char *end = input + strlen(input);
		YasAtom parsed;
		YasErrorContext err = {0};
		
		/* Use read_expr to parse the S-expression */
		int status = yas_read_expr(input, &end, &parsed, &err);
		
		if (status != YAS_ERROR_OK) {
			return status;
		}
		
		/* Convert Lists to Hashes */
		*result = lisd_convert(parsed);
	} else {
		/* Argument is already a structure (e.g. quoted list), just convert it */
		*result = lisd_convert(arg);
	}
	
	return YAS_ERROR_OK;
}

/* (read-string str) - Parse an S-expression from a string, returning raw result (no conversion) */
int yas_builtin_read_string(YasAtom args, YasAtom *result) {
	if (yas_nilp(args) || !yas_nilp(yas_cdr(args)))
		return YAS_ERROR_ARGS;
	
	YasAtom arg = yas_car(args);
	
	if (arg.type != YAS_ATOM_TYPE_STRING) {
		*result = yas_nil;
		return YAS_ERROR_TYPE;
	}
	
	const char *input = arg.value.string;
	const char *end = input + strlen(input);
	YasErrorContext err = {0};
	
	int status = yas_read_expr(input, &end, result, &err);
	
	if (status != YAS_ERROR_OK) {
		*result = yas_nil;
		return status;
	}
	
	return YAS_ERROR_OK;
}

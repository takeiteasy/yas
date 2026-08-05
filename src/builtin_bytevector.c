#include "lisp.h"
#include <stdlib.h>
#include <string.h>

/* (make-bytevector size [fill]) */
int builtin_make_bytevector(Atom args, Atom *result) {
	if (nilp(args) || car(args).type != AtomType_Integer) {
		return raise_error("type-error", "make-bytevector expects an integer size");
	}
	
	size_t size = (size_t)car(args).value.integer;
	*result = make_bytevector(size);
	
	/* Optional fill value */
	if (!nilp(cdr(args)) && car(cdr(args)).type == AtomType_Integer) {
		unsigned char fill = (unsigned char)car(cdr(args)).value.integer;
		memset((*result).value.bytevector->data, fill, size);
	}
	
	return Error_OK;
}

/* (bytevector? obj) */
int builtin_bytevectorp(Atom args, Atom *result) {
	if (nilp(args)) return Error_Args;
	*result = car(args).type == AtomType_Bytevector ? make_sym("T") : nil;
	return Error_OK;
}

/* (bytevector-length bv) */
int builtin_bytevector_length(Atom args, Atom *result) {
	if (nilp(args) || car(args).type != AtomType_Bytevector) {
		return raise_error("type-error", "bytevector-length expects a bytevector");
	}
	*result = make_int(bytevector_length(car(args)));
	return Error_OK;
}

/* (bytevector-u8-ref bv index) */
int builtin_bytevector_u8_ref(Atom args, Atom *result) {
	if (nilp(args) || car(args).type != AtomType_Bytevector) {
		return raise_error("type-error", "bytevector-u8-ref expects a bytevector");
	}
	if (nilp(cdr(args)) || car(cdr(args)).type != AtomType_Integer) {
		return raise_error("type-error", "bytevector-u8-ref expects an integer index");
	}
	
	Atom bv = car(args);
	size_t index = (size_t)car(cdr(args)).value.integer;
	
	if (index >= bytevector_length(bv)) {
		return raise_error("range-error", "bytevector index out of bounds");
	}
	
	*result = make_int(bytevector_ref(bv, index));
	return Error_OK;
}

/* (bytevector-u8-set! bv index value) */
int builtin_bytevector_u8_set(Atom args, Atom *result) {
	if (nilp(args) || car(args).type != AtomType_Bytevector) {
		return raise_error("type-error", "bytevector-u8-set! expects a bytevector");
	}
	if (nilp(cdr(args)) || car(cdr(args)).type != AtomType_Integer) {
		return raise_error("type-error", "bytevector-u8-set! expects an integer index");
	}
	if (nilp(cdr(cdr(args))) || car(cdr(cdr(args))).type != AtomType_Integer) {
		return raise_error("type-error", "bytevector-u8-set! expects an integer value");
	}
	
	Atom bv = car(args);
	size_t index = (size_t)car(cdr(args)).value.integer;
	unsigned char value = (unsigned char)car(cdr(cdr(args))).value.integer;
	
	if (index >= bytevector_length(bv)) {
		return raise_error("range-error", "bytevector index out of bounds");
	}
	
	bytevector_set(bv, index, value);
	*result = make_sym("T");
	return Error_OK;
}

/* (bytevector->string bv) - UTF-8 interpretation */
int builtin_bytevector_to_string(Atom args, Atom *result) {
	if (nilp(args) || car(args).type != AtomType_Bytevector) {
		return raise_error("type-error", "bytevector->string expects a bytevector");
	}
	
	struct Bytevector *bv = car(args).value.bytevector;
	*result = make_string_n((const char *)bv->data, bv->size);
	return Error_OK;
}

/* (string->bytevector str) */
int builtin_string_to_bytevector(Atom args, Atom *result) {
	if (nilp(args) || car(args).type != AtomType_String) {
		return raise_error("type-error", "string->bytevector expects a string");
	}
	
	const char *s = car(args).value.string;
	*result = make_bytevector_from_data((const unsigned char *)s, strlen(s));
	return Error_OK;
}

/* (string->bytes str [encoding]) - Convert string to bytevector */
int builtin_string_to_bytes(Atom args, Atom *result) {
	if (nilp(args) || car(args).type != AtomType_String)
		return raise_error("type-error", "string->bytes: first argument must be string");
	
	const char *str = car(args).value.string;
	const char *encoding = "utf-8";  /* Default */
	
	if (!nilp(cdr(args))) {
		Atom enc_arg = car(cdr(args));
		if (enc_arg.type != AtomType_String)
			return raise_error("type-error", "string->bytes: encoding must be string");
		encoding = enc_arg.value.string;
	}
	
	/* UTF-8 is the native format - just copy bytes */
	if (strcmp(encoding, "utf-8") == 0 || strcmp(encoding, "UTF-8") == 0) {
		size_t len = strlen(str);
		*result = make_bytevector_from_data((const unsigned char *)str, len);
	} else if (strcmp(encoding, "ascii") == 0 || strcmp(encoding, "ASCII") == 0) {
		/* ASCII: fail on non-ASCII characters */
		size_t len = strlen(str);
		for (size_t i = 0; i < len; i++) {
			if ((unsigned char)str[i] > 127)
				return raise_error("encoding-error", "string->bytes: non-ASCII character in string");
		}
		*result = make_bytevector_from_data((const unsigned char *)str, len);
	} else if (strcmp(encoding, "latin-1") == 0 || strcmp(encoding, "LATIN-1") == 0 ||
	           strcmp(encoding, "iso-8859-1") == 0 || strcmp(encoding, "ISO-8859-1") == 0) {
		/* Latin-1: encode codepoints 0-255 directly */
		size_t codepoint_count = utf8_strlen(str);
		unsigned char *buf = malloc(codepoint_count);
		const char *p = str;
		for (size_t i = 0; i < codepoint_count; i++) {
			int32_t cp;
			p += utf8_decode(p, &cp);
			if (cp > 255)
				return raise_error("encoding-error", "string->bytes: codepoint > 255 not encodable in latin-1");
			buf[i] = (unsigned char)cp;
		}
		*result = make_bytevector_from_data(buf, codepoint_count);
		free(buf);
	} else {
		return raise_error("value-error", "string->bytes: unknown encoding (use utf-8, ascii, or latin-1)");
	}
	
	return Error_OK;
}

/* (bytes->string bytevector [encoding]) - Convert bytevector to string */
int builtin_bytes_to_string(Atom args, Atom *result) {
	if (nilp(args) || car(args).type != AtomType_Bytevector)
		return raise_error("type-error", "bytes->string: first argument must be bytevector");
	
	struct Bytevector *bv = car(args).value.bytevector;
	const char *encoding = "utf-8";  /* Default */
	
	if (!nilp(cdr(args))) {
		Atom enc_arg = car(cdr(args));
		if (enc_arg.type != AtomType_String)
			return raise_error("type-error", "bytes->string: encoding must be string");
		encoding = enc_arg.value.string;
	}
	
	if (strcmp(encoding, "utf-8") == 0 || strcmp(encoding, "UTF-8") == 0) {
		/* UTF-8: just copy as string */
		*result = make_string_n((const char *)bv->data, bv->size);
	} else if (strcmp(encoding, "ascii") == 0 || strcmp(encoding, "ASCII") == 0) {
		/* ASCII: fail on non-ASCII bytes */
		for (size_t i = 0; i < bv->size; i++) {
			if (bv->data[i] > 127)
				return raise_error("encoding-error", "bytes->string: non-ASCII byte in bytevector");
		}
		*result = make_string_n((const char *)bv->data, bv->size);
	} else if (strcmp(encoding, "latin-1") == 0 || strcmp(encoding, "LATIN-1") == 0 ||
	           strcmp(encoding, "iso-8859-1") == 0 || strcmp(encoding, "ISO-8859-1") == 0) {
		/* Latin-1: each byte is a codepoint, encode to UTF-8 */
		/* Worst case: each byte becomes 2 UTF-8 bytes (for 128-255) */
		char *buf = malloc(bv->size * 2 + 1);
		char *p = buf;
		for (size_t i = 0; i < bv->size; i++) {
			p += utf8_encode(bv->data[i], p);
		}
		*p = '\0';
		*result = make_string(buf);
		free(buf);
	} else {
		return raise_error("value-error", "bytes->string: unknown encoding (use utf-8, ascii, or latin-1)");
	}
	
	return Error_OK;
}

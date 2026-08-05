#include "lisp.h"
#include <stdlib.h>
#include <string.h>

int builtin_stringp(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "string?: expected 1 argument");
	*result = (car(args).type == AtomType_String) ? make_sym("T") : nil;
	return Error_OK;
}

/* (string-length str) */
int builtin_string_length(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "string-length: expected 1 argument");
	Atom a = car(args);
	if (a.type != AtomType_String)
		return raise_error("type-error", "string-length: expected string");
	*result = make_int(utf8_strlen(a.value.string));
	return Error_OK;
}

/* (string-ref str k) - get k-th character (0-indexed) */
int builtin_string_ref(Atom args, Atom *result) {
	if (nilp(args) || nilp(cdr(args)) || !nilp(cdr(cdr(args))))
		return raise_error("args-error", "string-ref: expected 2 arguments");
	
	Atom str = car(args);
	Atom idx = car(cdr(args));
	
	if (str.type != AtomType_String)
		return raise_error("type-error", "string-ref: first argument must be string");
	if (idx.type != AtomType_Integer)
		return raise_error("type-error", "string-ref: second argument must be integer");
	
	int32_t cp = utf8_codepoint_at(str.value.string, (size_t)idx.value.integer);
	if (cp < 0)
		return raise_error("args-error", "string-ref: index out of bounds");
	
	*result = make_char(cp);
	return Error_OK;
}

/* (string=? s1 s2) */
int builtin_string_eq(Atom args, Atom *result) {
	if (nilp(args) || nilp(cdr(args)) || !nilp(cdr(cdr(args))))
		return raise_error("args-error", "string=?: expected 2 arguments");
	
	Atom a = car(args);
	Atom b = car(cdr(args));
	
	if (a.type != AtomType_String || b.type != AtomType_String)
		return raise_error("type-error", "string=?: arguments must be strings");
	
	*result = (strcmp(a.value.string, b.value.string) == 0) ? make_sym("T") : nil;
	return Error_OK;
}

/* (string-byte-length str) - byte length (for I/O) */
int builtin_string_byte_length(Atom args, Atom *result) {
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "string-byte-length: expected 1 argument");
	
	Atom str = car(args);
	if (str.type != AtomType_String)
		return raise_error("type-error", "string-byte-length: argument must be string");
	
	*result = make_int(strlen(str.value.string));
	return Error_OK;
}

/* (string-append s1 s2 ...) */
int builtin_string_append(Atom args, Atom *result)
{
	size_t total_len = 0;
	Atom p = args;
	while (!nilp(p)) {
		if (car(p).type != AtomType_String)
			return raise_error("type-error", "string-append: arguments must be strings");
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
	*result = make_string(buf);
	free(buf);
	return Error_OK;
}

/* (string-upcase str) - convert string to uppercase */
int builtin_string_upcase(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "string-upcase: expected 1 argument");
	
	Atom str = car(args);
	if (str.type != AtomType_String)
		return raise_error("type-error", "string-upcase: expected string");
	
	size_t len = strlen(str.value.string);
	char *buf = malloc(len + 1);
	
	for (size_t i = 0; i <= len; i++) {
		unsigned char c = str.value.string[i];
		if (c >= 'a' && c <= 'z')
			buf[i] = c - 32;
		else
			buf[i] = c;
	}
	
	*result = make_string(buf);
	free(buf);
	return Error_OK;
}

/* (string-downcase str) - convert string to lowercase */
int builtin_string_downcase(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "string-downcase: expected 1 argument");
	
	Atom str = car(args);
	if (str.type != AtomType_String)
		return raise_error("type-error", "string-downcase: expected string");
	
	size_t len = strlen(str.value.string);
	char *buf = malloc(len + 1);
	
	for (size_t i = 0; i <= len; i++) {
		unsigned char c = str.value.string[i];
		if (c >= 'A' && c <= 'Z')
			buf[i] = c + 32;
		else
			buf[i] = c;
	}
	
	*result = make_string(buf);
	free(buf);
	return Error_OK;
}

/* (string-capitalize str) - capitalize first letter of each word */
int builtin_string_capitalize(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "string-capitalize: expected 1 argument");
	
	Atom str = car(args);
	if (str.type != AtomType_String)
		return raise_error("type-error", "string-capitalize: expected string");
	
	size_t len = strlen(str.value.string);
	char *buf = malloc(len + 1);
	
	int word_start = 1;
	for (size_t i = 0; i <= len; i++) {
		unsigned char c = str.value.string[i];
		if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
			word_start = 1;
			buf[i] = c;
		} else if (word_start && c >= 'a' && c <= 'z') {
			buf[i] = c - 32;
			word_start = 0;
		} else if (!word_start && c >= 'A' && c <= 'Z') {
			buf[i] = c + 32;
			word_start = 0;
		} else {
			buf[i] = c;
			if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
				word_start = 0;
		}
	}
	
	*result = make_string(buf);
	free(buf);
	return Error_OK;
}

/* (make-string n [char]) - create string of n copies of char (default space) */
int builtin_make_string(Atom args, Atom *result)
{
	if (nilp(args))
		return raise_error("args-error", "make-string: expected at least 1 argument");
	
	Atom n_arg = car(args);
	if (n_arg.type != AtomType_Integer)
		return raise_error("type-error", "make-string: first argument must be integer");
	
	long n = n_arg.value.integer;
	if (n < 0)
		return raise_error("args-error", "make-string: length must be non-negative");
	
	int32_t cp = ' '; /* default to space */
	if (!nilp(cdr(args))) {
		Atom ch = car(cdr(args));
		if (ch.type == AtomType_Char) {
			cp = ch.value.character;
		} else if (ch.type == AtomType_String && strlen(ch.value.string) > 0) {
			utf8_decode(ch.value.string, &cp);
		} else {
			return raise_error("type-error", "make-string: second argument must be char or string");
		}
	}
	
	/* Encode the character to UTF-8 */
	char encoded[5];
	int char_len = utf8_encode(cp, encoded);
	
	size_t total_len = n * char_len;
	char *buf = malloc(total_len + 1);
	
	for (long i = 0; i < n; i++) {
		memcpy(buf + i * char_len, encoded, char_len);
	}
	buf[total_len] = '\0';
	
	*result = make_string(buf);
	free(buf);
	return Error_OK;
}

/* (substring str start [end]) - extract substring */
int builtin_substring(Atom args, Atom *result)
{
	if (nilp(args) || nilp(cdr(args)))
		return raise_error("args-error", "substring: expected at least 2 arguments");
	
	Atom str = car(args);
	Atom start_arg = car(cdr(args));
	
	if (str.type != AtomType_String)
		return raise_error("type-error", "substring: first argument must be string");
	if (start_arg.type != AtomType_Integer)
		return raise_error("type-error", "substring: second argument must be integer");
	
	size_t str_len = utf8_strlen(str.value.string);
	long start = start_arg.value.integer;
	long end = str_len;
	
	if (!nilp(cdr(cdr(args)))) {
		Atom end_arg = car(cdr(cdr(args)));
		if (end_arg.type != AtomType_Integer)
			return raise_error("type-error", "substring: third argument must be integer");
		end = end_arg.value.integer;
	}
	
	if (start < 0) start = 0;
	if (end > (long)str_len) end = str_len;
	if (start > end) start = end;
	
	/* Find byte positions for UTF-8 string */
	const char *start_ptr = utf8_index_ptr(str.value.string, start);
	const char *end_ptr = utf8_index_ptr(str.value.string, end);
	
	if (!start_ptr || !end_ptr) {
		*result = make_string("");
		return Error_OK;
	}
	
	size_t byte_len = end_ptr - start_ptr;
	*result = make_string_n(start_ptr, byte_len);
	return Error_OK;
}

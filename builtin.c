#include "lisp.h"
#include "utf8.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

int raise_error(const char *type, const char *msg)
{
	Atom t = make_sym(type);
	Atom m = make_string(msg);
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

int builtin_add(Atom args, Atom *result)
{
	Atom a, b;

	if (nilp(args) || nilp(cdr(args)) || !nilp(cdr(cdr(args))))
		return raise_error("args-error", "+: expected 2 arguments");

	a = car(args);
	b = car(cdr(args));

	if (a.type != AtomType_Integer || b.type != AtomType_Integer)
		return raise_error("type-error", "+: operands must be integers");

	*result = make_int(a.value.integer + b.value.integer);

	return Error_OK;
}

int builtin_subtract(Atom args, Atom *result)
{
	Atom a, b;

	if (nilp(args) || nilp(cdr(args)) || !nilp(cdr(cdr(args))))
		return raise_error("args-error", "-: expected 2 arguments");

	a = car(args);
	b = car(cdr(args));

	if (a.type != AtomType_Integer || b.type != AtomType_Integer)
		return raise_error("type-error", "-: operands must be integers");

	*result = make_int(a.value.integer - b.value.integer);

	return Error_OK;
}

int builtin_multiply(Atom args, Atom *result)
{
	Atom a, b;

	if (nilp(args) || nilp(cdr(args)) || !nilp(cdr(cdr(args))))
		return raise_error("args-error", "*: expected 2 arguments");

	a = car(args);
	b = car(cdr(args));

	if (a.type != AtomType_Integer || b.type != AtomType_Integer)
		return raise_error("type-error", "*: operands must be integers");

	*result = make_int(a.value.integer * b.value.integer);

	return Error_OK;
}

int builtin_divide(Atom args, Atom *result)
{
	Atom a, b;

	if (nilp(args) || nilp(cdr(args)) || !nilp(cdr(cdr(args))))
		return raise_error("args-error", "/: expected 2 arguments");

	a = car(args);
	b = car(cdr(args));

	if (a.type != AtomType_Integer || b.type != AtomType_Integer)
		return raise_error("type-error", "/: operands must be integers");
	
	if (b.value.integer == 0)
		return raise_error("arithmetic-error", "/: division by zero");

	*result = make_int(a.value.integer / b.value.integer);

	return Error_OK;
}

int builtin_numeq(Atom args, Atom *result)
{
	Atom a, b;

	if (nilp(args) || nilp(cdr(args)) || !nilp(cdr(cdr(args))))
		return raise_error("args-error", "=: expected 2 arguments");

	a = car(args);
	b = car(cdr(args));

	if (a.type != AtomType_Integer || b.type != AtomType_Integer)
		return raise_error("type-error", "=: operands must be integers");

	*result = (a.value.integer == b.value.integer) ? make_sym("T") : nil;

	return Error_OK;
}

int builtin_less(Atom args, Atom *result)
{
	Atom a, b;

	if (nilp(args) || nilp(cdr(args)) || !nilp(cdr(cdr(args))))
		return raise_error("args-error", "<: expected 2 arguments");

	a = car(args);
	b = car(cdr(args));

	if (a.type != AtomType_Integer || b.type != AtomType_Integer)
		return raise_error("type-error", "<: operands must be integers");

	*result = (a.value.integer < b.value.integer) ? make_sym("T") : nil;

	return Error_OK;
}

/* Type predicates */
int builtin_floatp(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "float?: expected 1 argument");
	*result = (car(args).type == AtomType_Float) ? make_sym("T") : nil;
	return Error_OK;
}

int builtin_stringp(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "string?: expected 1 argument");
	*result = (car(args).type == AtomType_String) ? make_sym("T") : nil;
	return Error_OK;
}

int builtin_vectorp(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "vector?: expected 1 argument");
	*result = (car(args).type == AtomType_Vector) ? make_sym("T") : nil;
	return Error_OK;
}

int builtin_hashp(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "hash?: expected 1 argument");
	*result = (car(args).type == AtomType_Hash) ? make_sym("T") : nil;
	return Error_OK;
}

/* Length - works on lists, strings, vectors, hashes */
int builtin_length(Atom args, Atom *result)
{
	Atom a;
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "length: expected 1 argument");
	
	a = car(args);
	
	switch (a.type) {
	case AtomType_Nil:
		*result = make_int(0);
		break;
	case AtomType_Pair: {
		long len = 0;
		while (!nilp(a)) {
			if (a.type != AtomType_Pair) return raise_error("type-error", "length: improper list");
			len++;
			a = cdr(a);
		}
		*result = make_int(len);
		break;
	}
	case AtomType_String:
		*result = make_int(utf8_strlen(a.value.string));
		break;
	case AtomType_Vector:
		*result = make_int(vector_length(a));
		break;
	case AtomType_Hash:
		*result = make_int(hash_size(a));
		break;
	case AtomType_Bytevector:
		*result = make_int(bytevector_length(a));
		break;
	default:
		return raise_error("type-error", "length: not a sequence");
	}
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

/* Concat - works on lists, strings, vectors */
int builtin_concat(Atom args, Atom *result)
{
	if (nilp(args))
		return raise_error("args-error", "concat: expected at least 1 argument");
	
	Atom first = car(args);
	
	if (first.type == AtomType_String) {
		/* String concatenation */
		size_t total_len = 0;
		Atom p = args;
		while (!nilp(p)) {
			if (car(p).type != AtomType_String)
				return raise_error("type-error", "concat: arguments must be strings");
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
					return raise_error("type-error", "concat: arguments must be lists");
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
				return raise_error("type-error", "concat: arguments must be vectors");
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
		return raise_error("type-error", "concat: first argument must be string, list or vector");
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

int builtin_print(Atom args, Atom *result)
{
	if (nilp(args)) {
		putchar('\n');
		*result = nil;
		return Error_OK;
	}

	while (!nilp(args)) {
		print_expr(car(args));
		if (!nilp(cdr(args)))
			putchar(' ');
		args = cdr(args);
	}
	putchar('\n');
	
	*result = nil;
	return Error_OK;
}

/* (open-input-file path [binary?]) */
int builtin_open_input_file(Atom args, Atom *result) {
	if (nilp(args) || car(args).type != AtomType_String) {
		return raise_error("type-error", "open-input-file expects a string path");
	}
	
	const char *path = car(args).value.string;
	int is_binary = 0;
	
	if (!nilp(cdr(args)) && !nilp(car(cdr(args)))) {
		is_binary = 1;
	}
	
	*result = io_open_file(path, 1, is_binary);
	if (nilp(*result)) {
		return raise_error("io-error", "failed to open file for reading");
	}
	return Error_OK;
}

/* (open-output-file path [binary?]) */
int builtin_open_output_file(Atom args, Atom *result) {
	if (nilp(args) || car(args).type != AtomType_String) {
		return raise_error("type-error", "open-output-file expects a string path");
	}
	
	const char *path = car(args).value.string;
	int is_binary = 0;
	
	if (!nilp(cdr(args)) && !nilp(car(cdr(args)))) {
		is_binary = 1;
	}
	
	*result = io_open_file(path, 0, is_binary);
	if (nilp(*result)) {
		return raise_error("io-error", "failed to open file for writing");
	}
	return Error_OK;
}

/* (close-port port) */
int builtin_close_port(Atom args, Atom *result) {
	if (nilp(args) || car(args).type != AtomType_Port) {
		return raise_error("type-error", "close-port expects a port");
	}
	port_close(car(args));
	*result = make_sym("T");
	return Error_OK;
}

/* (port? obj) */
int builtin_portp(Atom args, Atom *result) {
	if (nilp(args)) return Error_Args;
	*result = car(args).type == AtomType_Port ? make_sym("T") : nil;
	return Error_OK;
}

/* (input-port? port) */
int builtin_input_portp(Atom args, Atom *result) {
	if (nilp(args)) return Error_Args;
	Atom a = car(args);
	*result = (a.type == AtomType_Port && a.value.port->is_input) ? make_sym("T") : nil;
	return Error_OK;
}

/* (output-port? port) */
int builtin_output_portp(Atom args, Atom *result) {
	if (nilp(args)) return Error_Args;
	Atom a = car(args);
	*result = (a.type == AtomType_Port && !a.value.port->is_input) ? make_sym("T") : nil;
	return Error_OK;
}

/* (port-open? port) */
int builtin_port_openp(Atom args, Atom *result) {
	if (nilp(args)) return Error_Args;
	Atom a = car(args);
	*result = (a.type == AtomType_Port && a.value.port->is_open) ? make_sym("T") : nil;
	return Error_OK;
}

/* (read-char port) - reads a full UTF-8 character, returns char type */
int builtin_read_char(Atom args, Atom *result) {
	if (nilp(args) || car(args).type != AtomType_Port) {
		return raise_error("type-error", "read-char expects an input port");
	}
	
	struct Port *p = car(args).value.port;
	if (!p->is_input) {
		return raise_error("io-error", "cannot read from output port");
	}
	
	/* Read first byte to determine UTF-8 sequence length */
	int c = port_getc(p);
	if (c == EOF) {
		*result = make_sym("EOF");
		return Error_OK;
	}
	
	/* Determine UTF-8 sequence length */
	unsigned char first = (unsigned char)c;
	int len = 1;
	if (first >= 0xF0) len = 4;
	else if (first >= 0xE0) len = 3;
	else if (first >= 0xC0) len = 2;
	
	/* Read remaining bytes */
	char buf[5];
	buf[0] = first;
	for (int i = 1; i < len; i++) {
		int b = port_getc(p);
		if (b == EOF) break;
		buf[i] = (char)b;
	}
	buf[len] = '\0';
	
	/* Decode to codepoint */
	int32_t cp;
	utf8_decode(buf, &cp);
	*result = make_char(cp);
	return Error_OK;
}

/* (read-line port) */
int builtin_read_line(Atom args, Atom *result) {
	if (nilp(args) || car(args).type != AtomType_Port) {
		return raise_error("type-error", "read-line expects an input port");
	}
	
	struct Port *p = car(args).value.port;
	if (!p->is_input) {
		return raise_error("io-error", "cannot read from output port");
	}
	
	/* Read line into dynamic buffer */
	size_t capacity = 128;
	size_t len = 0;
	char *buf = malloc(capacity);
	
	int c;
	while ((c = port_getc(p)) != EOF && c != '\n') {
		if (len + 1 >= capacity) {
			capacity *= 2;
			buf = realloc(buf, capacity);
		}
		buf[len++] = (char)c;
	}
	
	if (len == 0 && c == EOF) {
		free(buf);
		*result = make_sym("EOF");
		return Error_OK;
	}
	
	buf[len] = '\0';
	*result = make_string(buf);
	free(buf);
	return Error_OK;
}

/* (read-all port) */
int builtin_read_all(Atom args, Atom *result) {
	if (nilp(args) || car(args).type != AtomType_Port) {
		return raise_error("type-error", "read-all expects an input port");
	}
	
	struct Port *p = car(args).value.port;
	if (!p->is_input || !p->is_open) {
		return raise_error("io-error", "cannot read from output port or closed port");
	}
	
	/* Read all into dynamic buffer */
	size_t capacity = 1024;
	size_t len = 0;
	char *buf = malloc(capacity);
	
	char chunk[1024];
	long n;
	while ((n = port_read(p, chunk, sizeof(chunk))) > 0) {
		if (len + n >= capacity) {
			while (len + n >= capacity) capacity *= 2;
			buf = realloc(buf, capacity);
		}
		memcpy(buf + len, chunk, n);
		len += n;
	}
	
	buf[len] = '\0';
	*result = make_string(buf);
	free(buf);
	return Error_OK;
}

/* (read-byte port) */
int builtin_read_byte(Atom args, Atom *result) {
	if (nilp(args) || car(args).type != AtomType_Port) {
		return raise_error("type-error", "read-byte expects an input port");
	}
	
	struct Port *p = car(args).value.port;
	if (!p->is_input) {
		return raise_error("io-error", "cannot read from output port");
	}
	
	int c = port_getc(p);
	if (c == EOF) {
		*result = make_sym("EOF");
	} else {
		*result = make_int(c);
	}
	return Error_OK;
}

/* (read-bytes port n) */
int builtin_read_bytes(Atom args, Atom *result) {
	if (nilp(args) || car(args).type != AtomType_Port) {
		return raise_error("type-error", "read-bytes expects an input port");
	}
	if (nilp(cdr(args)) || car(cdr(args)).type != AtomType_Integer) {
		return raise_error("type-error", "read-bytes expects count as second argument");
	}
	
	struct Port *p = car(args).value.port;
	long count = car(cdr(args)).value.integer;
	
	if (!p->is_input) {
		return raise_error("io-error", "cannot read from output port");
	}
	
	unsigned char *buf = malloc(count);
	long n = port_read(p, buf, count);
	
	if (n <= 0) {
		free(buf);
		*result = make_sym("EOF");
		return Error_OK;
	}
	
	*result = make_bytevector_from_data(buf, n);
	free(buf);
	return Error_OK;
}


/* (write-char char port) */
int builtin_write_char(Atom args, Atom *result) {
	if (nilp(args) || car(args).type != AtomType_String) {
		return raise_error("type-error", "write-char expects a string (character)");
	}
	if (nilp(cdr(args)) || car(cdr(args)).type != AtomType_Port) {
		return raise_error("type-error", "write-char expects a port as second argument");
	}
	
	const char *s = car(args).value.string;
	struct Port *p = car(cdr(args)).value.port;
	
	if (p->is_input) {
		return raise_error("io-error", "cannot write to input port");
	}
	
	port_write(p, s, 1);
	*result = make_sym("T");
	return Error_OK;
}

/* (write-string string port) */
int builtin_write_string(Atom args, Atom *result) {
	if (nilp(args) || car(args).type != AtomType_String) {
		return raise_error("type-error", "write-string expects a string");
	}
	if (nilp(cdr(args)) || car(cdr(args)).type != AtomType_Port) {
		return raise_error("type-error", "write-string expects a port as second argument");
	}
	
	const char *s = car(args).value.string;
	struct Port *p = car(cdr(args)).value.port;
	
	if (p->is_input) {
		return raise_error("io-error", "cannot write to input port");
	}
	
	port_write(p, s, strlen(s));
	*result = make_sym("T");
	return Error_OK;
}

/* (write-byte byte port) */
int builtin_write_byte(Atom args, Atom *result) {
	if (nilp(args) || car(args).type != AtomType_Integer) {
		return raise_error("type-error", "write-byte expects an integer (0-255)");
	}
	if (nilp(cdr(args)) || car(cdr(args)).type != AtomType_Port) {
		return raise_error("type-error", "write-byte expects a port as second argument");
	}
	
	unsigned char byte = (unsigned char)car(args).value.integer;
	struct Port *p = car(cdr(args)).value.port;
	
	if (p->is_input) {
		return raise_error("io-error", "cannot write to input port");
	}
	
	port_write(p, &byte, 1);
	*result = make_sym("T");
	return Error_OK;
}

/* (write-bytes bytevector port) */
int builtin_write_bytes(Atom args, Atom *result) {
	if (nilp(args) || car(args).type != AtomType_Bytevector) {
		return raise_error("type-error", "write-bytes expects a bytevector");
	}
	if (nilp(cdr(args)) || car(cdr(args)).type != AtomType_Port) {
		return raise_error("type-error", "write-bytes expects a port as second argument");
	}
	
	struct Bytevector *bv = car(args).value.bytevector;
	struct Port *p = car(cdr(args)).value.port;
	
	if (p->is_input) {
		return raise_error("io-error", "cannot write to input port");
	}
	
	port_write(p, bv->data, bv->size);
	*result = make_sym("T");
	return Error_OK;
}

/* (newline port) */
int builtin_newline(Atom args, Atom *result) {
	if (nilp(args) || car(args).type != AtomType_Port) {
		return raise_error("type-error", "newline expects a port");
	}
	
	struct Port *p = car(args).value.port;
	if (p->is_input) {
		return raise_error("io-error", "cannot write to input port");
	}
	
	port_write(p, "\n", 1);
	*result = make_sym("T");
	return Error_OK;
}

/* (flush-port port) */
int builtin_flush_port(Atom args, Atom *result) {
	if (nilp(args) || car(args).type != AtomType_Port) {
		return raise_error("type-error", "flush-port expects a port");
	}
	
	struct Port *p = car(args).value.port;
	port_flush(p);
	
	*result = make_sym("T");
	return Error_OK;
}

/* (file-exists? path) */
int builtin_file_exists(Atom args, Atom *result) {
	if (nilp(args) || car(args).type != AtomType_String) {
		return raise_error("type-error", "file-exists? expects a string path");
	}
	
	const char *path = car(args).value.string;
	if (io_file_exists(path)) {
		*result = make_sym("T");
	} else {
		*result = nil;
	}
	return Error_OK;
}

/* ============================================================
 * BYTEVECTOR BUILTINS
 * ============================================================ */

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

/* ============================================================
 * SANDBOX SCHEME BUILTINS
 * ============================================================ */

/* (io-sandbox-allow! pattern) */
int builtin_io_sandbox_allow(Atom args, Atom *result) {
	if (nilp(args) || car(args).type != AtomType_String) {
		return raise_error("type-error", "io-sandbox-allow! expects a string pattern");
	}
	io_sandbox_allow_path(car(args).value.string);
	*result = make_sym("T");
	return Error_OK;
}

/* (io-sandbox-deny! pattern) */
int builtin_io_sandbox_deny(Atom args, Atom *result) {
	if (nilp(args) || car(args).type != AtomType_String) {
		return raise_error("type-error", "io-sandbox-deny! expects a string pattern");
	}
	io_sandbox_deny_path(car(args).value.string);
	*result = make_sym("T");
	return Error_OK;
}

/* (io-sandbox-mode [mode]) - get or set mode */
int builtin_io_sandbox_mode(Atom args, Atom *result) {
	if (nilp(args)) {
		/* Get mode */
		const char *mode_str;
		switch (io_sandbox_get_mode()) {
		case IO_SANDBOX_UNRESTRICTED: mode_str = "UNRESTRICTED"; break;
		case IO_SANDBOX_RESTRICTED: 
			mode_str = io_sandbox_get_default() ? "RESTRICTED-ALLOW" : "RESTRICTED-DENY"; 
			break;
		case IO_SANDBOX_DISABLED: mode_str = "DISABLED"; break;
		default: mode_str = "UNKNOWN"; break;
		}
		*result = make_sym(mode_str);
	} else {
		/* Set mode */
		Atom mode_arg = car(args);
		if (mode_arg.type != AtomType_Symbol) {
			return raise_error("type-error", "io-sandbox-mode expects a symbol");
		}
		const char *s = mode_arg.value.symbol;
		if (strcmp(s, "UNRESTRICTED") == 0) {
			io_sandbox_init(IO_SANDBOX_UNRESTRICTED);
		} else if (strcmp(s, "RESTRICTED") == 0 || strcmp(s, "RESTRICTED-ALLOW") == 0) {
			io_sandbox_init(IO_SANDBOX_RESTRICTED);
			io_sandbox_set_default(1); /* Allow by default */
		} else if (strcmp(s, "RESTRICTED-DENY") == 0) {
			io_sandbox_init(IO_SANDBOX_RESTRICTED);
			io_sandbox_set_default(0); /* Deny by default */
		} else if (strcmp(s, "DISABLED") == 0) {
			io_sandbox_init(IO_SANDBOX_DISABLED);
		} else {
			return raise_error("value-error", "unknown sandbox mode (use unrestricted, restricted, restricted-allow, restricted-deny, or disabled)");
		}
		*result = mode_arg;
	}
	return Error_OK;
}

/* ============================================================
 * CHARACTER BUILTINS
 * ============================================================ */

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

/* ============================================================
 * STRING/BYTES ENCODING BUILTINS
 * ============================================================ */

#include "utf8.h"

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

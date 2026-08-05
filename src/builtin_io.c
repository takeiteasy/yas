#include "lisp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* (delete-file path) */
int builtin_delete_file(Atom args, Atom *result) {
	if (nilp(args) || car(args).type != AtomType_String) {
		return raise_error("type-error", "delete-file expects a string path");
	}
	
	if (remove(car(args).value.string) == 0) {
		*result = make_sym("T");
		return Error_OK;
	} else {
		return raise_error("io-error", "could not delete file");
	}
}

/* (rename-file old new) */
int builtin_rename_file(Atom args, Atom *result) {
	if (nilp(args) || car(args).type != AtomType_String) {
		return raise_error("type-error", "rename-file expects a string source path");
	}
	if (nilp(cdr(args)) || car(cdr(args)).type != AtomType_String) {
		return raise_error("type-error", "rename-file expects a string destination path");
	}
	
	const char *old_path = car(args).value.string;
	const char *new_path = car(cdr(args)).value.string;
	
	if (rename(old_path, new_path) == 0) {
		*result = make_sym("T");
		return Error_OK;
	} else {
		return raise_error("io-error", "could not rename file");
	}
}

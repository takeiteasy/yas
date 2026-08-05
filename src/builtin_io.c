#include "yas.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* (open-input-file path [binary?]) */
int yas_builtin_open_input_file(YasAtom args, YasAtom *result) {
	if (yas_nilp(args) || yas_car(args).type != YAS_ATOM_TYPE_STRING) {
		return yas_raise_error("type-error", "open-input-file expects a string path");
	}
	
	const char *path = yas_car(args).value.string;
	int is_binary = 0;
	
	if (!yas_nilp(yas_cdr(args)) && !yas_nilp(yas_car(yas_cdr(args)))) {
		is_binary = 1;
	}
	
	*result = yas_io_open_file(path, 1, is_binary);
	if (yas_nilp(*result)) {
		return yas_raise_error("io-error", "failed to open file for reading");
	}
	return YAS_ERROR_OK;
}

/* (open-output-file path [binary?]) */
int yas_builtin_open_output_file(YasAtom args, YasAtom *result) {
	if (yas_nilp(args) || yas_car(args).type != YAS_ATOM_TYPE_STRING) {
		return yas_raise_error("type-error", "open-output-file expects a string path");
	}
	
	const char *path = yas_car(args).value.string;
	int is_binary = 0;
	
	if (!yas_nilp(yas_cdr(args)) && !yas_nilp(yas_car(yas_cdr(args)))) {
		is_binary = 1;
	}
	
	*result = yas_io_open_file(path, 0, is_binary);
	if (yas_nilp(*result)) {
		return yas_raise_error("io-error", "failed to open file for writing");
	}
	return YAS_ERROR_OK;
}

/* (close-port port) */
int yas_builtin_close_port(YasAtom args, YasAtom *result) {
	if (yas_nilp(args) || yas_car(args).type != YAS_ATOM_TYPE_PORT) {
		return yas_raise_error("type-error", "close-port expects a port");
	}
	yas_port_close(yas_car(args));
	*result = yas_make_sym("T");
	return YAS_ERROR_OK;
}

/* (port? obj) */
int yas_builtin_portp(YasAtom args, YasAtom *result) {
	if (yas_nilp(args)) return YAS_ERROR_ARGS;
	*result = yas_car(args).type == YAS_ATOM_TYPE_PORT ? yas_make_sym("T") : yas_nil;
	return YAS_ERROR_OK;
}

/* (input-port? port) */
int yas_builtin_input_portp(YasAtom args, YasAtom *result) {
	if (yas_nilp(args)) return YAS_ERROR_ARGS;
	YasAtom a = yas_car(args);
	*result = (a.type == YAS_ATOM_TYPE_PORT && a.value.port->is_input) ? yas_make_sym("T") : yas_nil;
	return YAS_ERROR_OK;
}

/* (output-port? port) */
int yas_builtin_output_portp(YasAtom args, YasAtom *result) {
	if (yas_nilp(args)) return YAS_ERROR_ARGS;
	YasAtom a = yas_car(args);
	*result = (a.type == YAS_ATOM_TYPE_PORT && !a.value.port->is_input) ? yas_make_sym("T") : yas_nil;
	return YAS_ERROR_OK;
}

/* (port-open? port) */
int yas_builtin_port_openp(YasAtom args, YasAtom *result) {
	if (yas_nilp(args)) return YAS_ERROR_ARGS;
	YasAtom a = yas_car(args);
	*result = (a.type == YAS_ATOM_TYPE_PORT && a.value.port->is_open) ? yas_make_sym("T") : yas_nil;
	return YAS_ERROR_OK;
}

/* (read-char port) - reads a full UTF-8 character, returns char type */
int yas_builtin_read_char(YasAtom args, YasAtom *result) {
	if (yas_nilp(args) || yas_car(args).type != YAS_ATOM_TYPE_PORT) {
		return yas_raise_error("type-error", "read-char expects an input port");
	}
	
	struct YasPort *p = yas_car(args).value.port;
	if (!p->is_input) {
		return yas_raise_error("io-error", "cannot read from output port");
	}
	
	/* Read first byte to determine UTF-8 sequence length */
	int c = yas_port_getc(p);
	if (c == EOF) {
		*result = yas_make_sym("EOF");
		return YAS_ERROR_OK;
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
		int b = yas_port_getc(p);
		if (b == EOF) break;
		buf[i] = (char)b;
	}
	buf[len] = '\0';
	
	/* Decode to codepoint */
	int32_t cp;
	yas_utf8_decode(buf, &cp);
	*result = yas_make_char(cp);
	return YAS_ERROR_OK;
}

/* (read-line port) */
int yas_builtin_read_line(YasAtom args, YasAtom *result) {
	if (yas_nilp(args) || yas_car(args).type != YAS_ATOM_TYPE_PORT) {
		return yas_raise_error("type-error", "read-line expects an input port");
	}
	
	struct YasPort *p = yas_car(args).value.port;
	if (!p->is_input) {
		return yas_raise_error("io-error", "cannot read from output port");
	}
	
	/* Read line into dynamic buffer */
	size_t capacity = 128;
	size_t len = 0;
	char *buf = malloc(capacity);
	
	int c;
	while ((c = yas_port_getc(p)) != EOF && c != '\n') {
		if (len + 1 >= capacity) {
			capacity *= 2;
			buf = realloc(buf, capacity);
		}
		buf[len++] = (char)c;
	}
	
	if (len == 0 && c == EOF) {
		free(buf);
		*result = yas_make_sym("EOF");
		return YAS_ERROR_OK;
	}
	
	buf[len] = '\0';
	*result = yas_make_string(buf);
	free(buf);
	return YAS_ERROR_OK;
}

/* (read-all port) */
int yas_builtin_read_all(YasAtom args, YasAtom *result) {
	if (yas_nilp(args) || yas_car(args).type != YAS_ATOM_TYPE_PORT) {
		return yas_raise_error("type-error", "read-all expects an input port");
	}
	
	struct YasPort *p = yas_car(args).value.port;
	if (!p->is_input || !p->is_open) {
		return yas_raise_error("io-error", "cannot read from output port or closed port");
	}
	
	/* Read all into dynamic buffer */
	size_t capacity = 1024;
	size_t len = 0;
	char *buf = malloc(capacity);
	
	char chunk[1024];
	long n;
	while ((n = yas_port_read(p, chunk, sizeof(chunk))) > 0) {
		if (len + n >= capacity) {
			while (len + n >= capacity) capacity *= 2;
			buf = realloc(buf, capacity);
		}
		memcpy(buf + len, chunk, n);
		len += n;
	}
	
	buf[len] = '\0';
	*result = yas_make_string(buf);
	free(buf);
	return YAS_ERROR_OK;
}

/* (read-byte port) */
int yas_builtin_read_byte(YasAtom args, YasAtom *result) {
	if (yas_nilp(args) || yas_car(args).type != YAS_ATOM_TYPE_PORT) {
		return yas_raise_error("type-error", "read-byte expects an input port");
	}
	
	struct YasPort *p = yas_car(args).value.port;
	if (!p->is_input) {
		return yas_raise_error("io-error", "cannot read from output port");
	}
	
	int c = yas_port_getc(p);
	if (c == EOF) {
		*result = yas_make_sym("EOF");
	} else {
		*result = yas_make_int(c);
	}
	return YAS_ERROR_OK;
}

/* (read-bytes port n) */
int yas_builtin_read_bytes(YasAtom args, YasAtom *result) {
	if (yas_nilp(args) || yas_car(args).type != YAS_ATOM_TYPE_PORT) {
		return yas_raise_error("type-error", "read-bytes expects an input port");
	}
	if (yas_nilp(yas_cdr(args)) || yas_car(yas_cdr(args)).type != YAS_ATOM_TYPE_INTEGER) {
		return yas_raise_error("type-error", "read-bytes expects count as second argument");
	}
	
	struct YasPort *p = yas_car(args).value.port;
	long count = yas_car(yas_cdr(args)).value.integer;
	
	if (!p->is_input) {
		return yas_raise_error("io-error", "cannot read from output port");
	}
	
	unsigned char *buf = malloc(count);
	long n = yas_port_read(p, buf, count);
	
	if (n <= 0) {
		free(buf);
		*result = yas_make_sym("EOF");
		return YAS_ERROR_OK;
	}
	
	*result = yas_make_bytevector_from_data(buf, n);
	free(buf);
	return YAS_ERROR_OK;
}


/* (write-char char port) */
int yas_builtin_write_char(YasAtom args, YasAtom *result) {
	if (yas_nilp(args) || yas_car(args).type != YAS_ATOM_TYPE_STRING) {
		return yas_raise_error("type-error", "write-char expects a string (character)");
	}
	if (yas_nilp(yas_cdr(args)) || yas_car(yas_cdr(args)).type != YAS_ATOM_TYPE_PORT) {
		return yas_raise_error("type-error", "write-char expects a port as second argument");
	}
	
	const char *s = yas_car(args).value.string;
	struct YasPort *p = yas_car(yas_cdr(args)).value.port;
	
	if (p->is_input) {
		return yas_raise_error("io-error", "cannot write to input port");
	}
	
	yas_port_write(p, s, 1);
	*result = yas_make_sym("T");
	return YAS_ERROR_OK;
}

/* (write-string string port) */
int yas_builtin_write_string(YasAtom args, YasAtom *result) {
	if (yas_nilp(args) || yas_car(args).type != YAS_ATOM_TYPE_STRING) {
		return yas_raise_error("type-error", "write-string expects a string");
	}
	if (yas_nilp(yas_cdr(args)) || yas_car(yas_cdr(args)).type != YAS_ATOM_TYPE_PORT) {
		return yas_raise_error("type-error", "write-string expects a port as second argument");
	}
	
	const char *s = yas_car(args).value.string;
	struct YasPort *p = yas_car(yas_cdr(args)).value.port;
	
	if (p->is_input) {
		return yas_raise_error("io-error", "cannot write to input port");
	}
	
	yas_port_write(p, s, strlen(s));
	*result = yas_make_sym("T");
	return YAS_ERROR_OK;
}

/* (write-byte byte port) */
int yas_builtin_write_byte(YasAtom args, YasAtom *result) {
	if (yas_nilp(args) || yas_car(args).type != YAS_ATOM_TYPE_INTEGER) {
		return yas_raise_error("type-error", "write-byte expects an integer (0-255)");
	}
	if (yas_nilp(yas_cdr(args)) || yas_car(yas_cdr(args)).type != YAS_ATOM_TYPE_PORT) {
		return yas_raise_error("type-error", "write-byte expects a port as second argument");
	}
	
	unsigned char byte = (unsigned char)yas_car(args).value.integer;
	struct YasPort *p = yas_car(yas_cdr(args)).value.port;
	
	if (p->is_input) {
		return yas_raise_error("io-error", "cannot write to input port");
	}
	
	yas_port_write(p, &byte, 1);
	*result = yas_make_sym("T");
	return YAS_ERROR_OK;
}

/* (write-bytes bytevector port) */
int yas_builtin_write_bytes(YasAtom args, YasAtom *result) {
	if (yas_nilp(args) || yas_car(args).type != YAS_ATOM_TYPE_BYTEVECTOR) {
		return yas_raise_error("type-error", "write-bytes expects a bytevector");
	}
	if (yas_nilp(yas_cdr(args)) || yas_car(yas_cdr(args)).type != YAS_ATOM_TYPE_PORT) {
		return yas_raise_error("type-error", "write-bytes expects a port as second argument");
	}
	
	struct YasBytevector *bv = yas_car(args).value.bytevector;
	struct YasPort *p = yas_car(yas_cdr(args)).value.port;
	
	if (p->is_input) {
		return yas_raise_error("io-error", "cannot write to input port");
	}
	
	yas_port_write(p, bv->data, bv->size);
	*result = yas_make_sym("T");
	return YAS_ERROR_OK;
}

/* (newline port) */
int yas_builtin_newline(YasAtom args, YasAtom *result) {
	if (yas_nilp(args) || yas_car(args).type != YAS_ATOM_TYPE_PORT) {
		return yas_raise_error("type-error", "newline expects a port");
	}
	
	struct YasPort *p = yas_car(args).value.port;
	if (p->is_input) {
		return yas_raise_error("io-error", "cannot write to input port");
	}
	
	yas_port_write(p, "\n", 1);
	*result = yas_make_sym("T");
	return YAS_ERROR_OK;
}

/* (flush-port port) */
int yas_builtin_flush_port(YasAtom args, YasAtom *result) {
	if (yas_nilp(args) || yas_car(args).type != YAS_ATOM_TYPE_PORT) {
		return yas_raise_error("type-error", "flush-port expects a port");
	}
	
	struct YasPort *p = yas_car(args).value.port;
	yas_port_flush(p);
	
	*result = yas_make_sym("T");
	return YAS_ERROR_OK;
}

/* (file-exists? path) */
int yas_builtin_file_exists(YasAtom args, YasAtom *result) {
	if (yas_nilp(args) || yas_car(args).type != YAS_ATOM_TYPE_STRING) {
		return yas_raise_error("type-error", "file-exists? expects a string path");
	}
	
	const char *path = yas_car(args).value.string;
	if (yas_io_file_exists(path)) {
		*result = yas_make_sym("T");
	} else {
		*result = yas_nil;
	}
	return YAS_ERROR_OK;
}

/* (delete-file path) */
int yas_builtin_delete_file(YasAtom args, YasAtom *result) {
	if (yas_nilp(args) || yas_car(args).type != YAS_ATOM_TYPE_STRING) {
		return yas_raise_error("type-error", "delete-file expects a string path");
	}
	
	if (remove(yas_car(args).value.string) == 0) {
		*result = yas_make_sym("T");
		return YAS_ERROR_OK;
	} else {
		return yas_raise_error("io-error", "could not delete file");
	}
}

/* (rename-file old new) */
int yas_builtin_rename_file(YasAtom args, YasAtom *result) {
	if (yas_nilp(args) || yas_car(args).type != YAS_ATOM_TYPE_STRING) {
		return yas_raise_error("type-error", "rename-file expects a string source path");
	}
	if (yas_nilp(yas_cdr(args)) || yas_car(yas_cdr(args)).type != YAS_ATOM_TYPE_STRING) {
		return yas_raise_error("type-error", "rename-file expects a string destination path");
	}
	
	const char *old_path = yas_car(args).value.string;
	const char *new_path = yas_car(yas_cdr(args)).value.string;
	
	if (rename(old_path, new_path) == 0) {
		*result = yas_make_sym("T");
		return YAS_ERROR_OK;
	} else {
		return yas_raise_error("io-error", "could not rename file");
	}
}

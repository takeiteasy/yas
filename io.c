#include "lisp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fnmatch.h>

/* ============================================================
 * SANDBOX STATE
 * ============================================================ */

static IOSandboxMode sandbox_mode = IO_SANDBOX_UNRESTRICTED;

/* Path patterns for whitelist/blacklist */
#define MAX_PATTERNS 64
static char *allowed_patterns[MAX_PATTERNS];
static int allowed_count = 0;
static char *denied_patterns[MAX_PATTERNS];
static int denied_count = 0;

/* Virtual filesystem providers */
#define MAX_PROVIDERS 16
static IOProvider *providers[MAX_PROVIDERS];
static int provider_count = 0;

/* ============================================================
 * SANDBOX API
 * ============================================================ */

void io_sandbox_init(IOSandboxMode mode) {
	sandbox_mode = mode;
}

IOSandboxMode io_sandbox_get_mode(void) {
	return sandbox_mode;
}

void io_sandbox_allow_path(const char *pattern) {
	if (allowed_count < MAX_PATTERNS) {
		allowed_patterns[allowed_count++] = strdup(pattern);
	}
}

void io_sandbox_deny_path(const char *pattern) {
	if (denied_count < MAX_PATTERNS) {
		denied_patterns[denied_count++] = strdup(pattern);
	}
}

void io_sandbox_clear(void) {
	for (int i = 0; i < allowed_count; i++) {
		free(allowed_patterns[i]);
		allowed_patterns[i] = NULL;
	}
	allowed_count = 0;
	
	for (int i = 0; i < denied_count; i++) {
		free(denied_patterns[i]);
		denied_patterns[i] = NULL;
	}
	denied_count = 0;
}

/* Check if path matches any pattern using glob */
static int matches_any(const char *path, char **patterns, int count) {
	for (int i = 0; i < count; i++) {
		if (fnmatch(patterns[i], path, 0) == 0) {
			return 1;
		}
	}
	return 0;
}

int io_sandbox_check_path(const char *path, int writing) {
	(void)writing; /* May be used for finer control later */
	
	switch (sandbox_mode) {
	case IO_SANDBOX_DISABLED:
		return 0; /* Never allowed */
		
	case IO_SANDBOX_UNRESTRICTED:
		return 1; /* Always allowed */
		
	case IO_SANDBOX_WHITELIST:
		return matches_any(path, allowed_patterns, allowed_count);
		
	case IO_SANDBOX_BLACKLIST:
		return !matches_any(path, denied_patterns, denied_count);
	}
	return 0;
}

/* ============================================================
 * PROVIDER API
 * ============================================================ */

int io_register_provider(IOProvider *provider) {
	if (provider_count >= MAX_PROVIDERS) return -1;
	providers[provider_count++] = provider;
	return provider_count; /* Provider ID (1-based) */
}

void io_unregister_provider(const char *prefix) {
	for (int i = 0; i < provider_count; i++) {
		if (strcmp(providers[i]->prefix, prefix) == 0) {
			/* Shift remaining providers */
			for (int j = i; j < provider_count - 1; j++) {
				providers[j] = providers[j + 1];
			}
			provider_count--;
			return;
		}
	}
}

/* Find provider for a path, return provider ID (0 = stdio) */
static int find_provider(const char *path, const char **subpath) {
	for (int i = 0; i < provider_count; i++) {
		size_t prefix_len = strlen(providers[i]->prefix);
		if (strncmp(path, providers[i]->prefix, prefix_len) == 0) {
			*subpath = path + prefix_len;
			return i + 1; /* 1-based provider ID */
		}
	}
	*subpath = path;
	return 0; /* stdio provider */
}

/* ============================================================
 * PORT OPERATIONS
 * ============================================================ */

Atom make_port(void *handle, const char *path, int is_input, int is_binary, int provider_id) {
	Atom a;
	struct Port *p = malloc(sizeof(struct Port));
	p->handle = handle;
	p->path = strdup(path);
	p->is_input = is_input;
	p->is_binary = is_binary;
	p->is_open = 1;
	p->provider_id = provider_id;
	a.type = AtomType_Port;
	a.value.port = p;
	return a;
}

void port_close(Atom port) {
	if (port.type != AtomType_Port) return;
	struct Port *p = port.value.port;
	if (!p->is_open) return;
	
	if (p->provider_id == 0) {
		/* stdio */
		fclose((FILE *)p->handle);
	} else {
		/* Custom provider */
		IOProvider *prov = providers[p->provider_id - 1];
		if (prov->close) {
			prov->close(p->handle, prov->userdata);
		}
	}
	p->is_open = 0;
}

/* ============================================================
 * BYTEVECTOR OPERATIONS
 * ============================================================ */

Atom make_bytevector(size_t size) {
	Atom a;
	struct Bytevector *bv = malloc(sizeof(struct Bytevector));
	bv->size = size;
	bv->data = calloc(size, 1);
	a.type = AtomType_Bytevector;
	a.value.bytevector = bv;
	return a;
}

Atom make_bytevector_from_data(const unsigned char *data, size_t size) {
	Atom a;
	struct Bytevector *bv = malloc(sizeof(struct Bytevector));
	bv->size = size;
	bv->data = malloc(size);
	memcpy(bv->data, data, size);
	a.type = AtomType_Bytevector;
	a.value.bytevector = bv;
	return a;
}

unsigned char bytevector_ref(Atom bv, size_t index) {
	return bv.value.bytevector->data[index];
}

void bytevector_set(Atom bv, size_t index, unsigned char value) {
	bv.value.bytevector->data[index] = value;
}

size_t bytevector_length(Atom bv) {
	return bv.value.bytevector->size;
}

/* ============================================================
 * PORT BUILTINS
 * ============================================================ */

/* Helper: open a file with sandbox checks */
static Atom io_open_file(const char *path, int is_input, int is_binary) {
	const char *subpath;
	int provider_id = find_provider(path, &subpath);
	
	/* Sandbox check (skip for custom providers - they handle their own security) */
	if (provider_id == 0 && !io_sandbox_check_path(subpath, !is_input)) {
		return nil; /* Access denied */
	}
	
	void *handle;
	if (provider_id == 0) {
		/* stdio */
		const char *mode;
		if (is_input) {
			mode = is_binary ? "rb" : "r";
		} else {
			mode = is_binary ? "wb" : "w";
		}
		handle = fopen(subpath, mode);
	} else {
		/* Custom provider */
		IOProvider *prov = providers[provider_id - 1];
		handle = prov->open(subpath, is_input, is_binary, prov->userdata);
	}
	
	if (!handle) return nil;
	return make_port(handle, path, is_input, is_binary, provider_id);
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

/* ============================================================
 * READ BUILTINS
 * ============================================================ */

/* Helper: read from port */
static long port_read(struct Port *p, void *buf, size_t n) {
	if (!p->is_open) return -1;
	
	if (p->provider_id == 0) {
		return fread(buf, 1, n, (FILE *)p->handle);
	} else {
		IOProvider *prov = providers[p->provider_id - 1];
		return prov->read(p->handle, buf, n, prov->userdata);
	}
}

/* Helper: read a single char */
static int port_getc(struct Port *p) {
	if (!p->is_open) return EOF;
	
	if (p->provider_id == 0) {
		return fgetc((FILE *)p->handle);
	} else {
		unsigned char c;
		IOProvider *prov = providers[p->provider_id - 1];
		long n = prov->read(p->handle, &c, 1, prov->userdata);
		return n == 1 ? c : EOF;
	}
}

/* (read-char port) */
int builtin_read_char(Atom args, Atom *result) {
	if (nilp(args) || car(args).type != AtomType_Port) {
		return raise_error("type-error", "read-char expects an input port");
	}
	
	struct Port *p = car(args).value.port;
	if (!p->is_input) {
		return raise_error("io-error", "cannot read from output port");
	}
	
	int c = port_getc(p);
	if (c == EOF) {
		*result = make_sym("EOF");
	} else {
		char buf[2] = { (char)c, '\0' };
		*result = make_string(buf);
	}
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

/* ============================================================
 * WRITE BUILTINS
 * ============================================================ */

/* Helper: write to port */
static long port_write(struct Port *p, const void *buf, size_t n) {
	if (!p->is_open) return -1;
	
	if (p->provider_id == 0) {
		return fwrite(buf, 1, n, (FILE *)p->handle);
	} else {
		IOProvider *prov = providers[p->provider_id - 1];
		return prov->write(p->handle, buf, n, prov->userdata);
	}
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
	if (p->provider_id == 0 && p->is_open) {
		fflush((FILE *)p->handle);
	}
	/* Custom providers don't have a standard flush */
	
	*result = make_sym("T");
	return Error_OK;
}

/* (file-exists? path) */
int builtin_file_exists(Atom args, Atom *result) {
	if (nilp(args) || car(args).type != AtomType_String) {
		return raise_error("type-error", "file-exists? expects a string path");
	}
	
	const char *path = car(args).value.string;
	const char *subpath;
	int provider_id = find_provider(path, &subpath);
	
	/* Sandbox check for stdio */
	if (provider_id == 0 && !io_sandbox_check_path(subpath, 0)) {
		*result = nil;
		return Error_OK;
	}
	
	if (provider_id == 0) {
		FILE *f = fopen(subpath, "r");
		if (f) {
			fclose(f);
			*result = make_sym("T");
		} else {
			*result = nil;
		}
	} else {
		IOProvider *prov = providers[provider_id - 1];
		if (prov->exists && prov->exists(subpath, prov->userdata)) {
			*result = make_sym("T");
		} else {
			*result = nil;
		}
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
		switch (sandbox_mode) {
		case IO_SANDBOX_UNRESTRICTED: mode_str = "UNRESTRICTED"; break;
		case IO_SANDBOX_WHITELIST: mode_str = "WHITELIST"; break;
		case IO_SANDBOX_BLACKLIST: mode_str = "BLACKLIST"; break;
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
		} else if (strcmp(s, "WHITELIST") == 0) {
			io_sandbox_init(IO_SANDBOX_WHITELIST);
		} else if (strcmp(s, "BLACKLIST") == 0) {
			io_sandbox_init(IO_SANDBOX_BLACKLIST);
		} else if (strcmp(s, "DISABLED") == 0) {
			io_sandbox_init(IO_SANDBOX_DISABLED);
		} else {
			return raise_error("value-error", "unknown sandbox mode");
		}
		*result = mode_arg;
	}
	return Error_OK;
}

#include "lisp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fnmatch.h>

char *slurp(const char *path)
{
	FILE *file;
	char *buf;
	long len;

	file = fopen(path, "r");
	if (!file)
		return NULL;
	fseek(file, 0, SEEK_END);
	len = ftell(file);
	fseek(file, 0, SEEK_SET);

	buf = malloc(len + 1);
	if (!buf)
		return NULL;

	fread(buf, 1, len, file);
	buf[len] = 0;
	fclose(file);

	return buf;
}

static IOSandboxMode sandbox_mode = IO_SANDBOX_UNRESTRICTED;
static int sandbox_default_allow = 1;  /* Default: allow unmatched paths */

/* Path patterns for allow/deny lists */
#define MAX_PATTERNS 64
static char *allowed_patterns[MAX_PATTERNS];
static int allowed_count = 0;
static char *denied_patterns[MAX_PATTERNS];
static int denied_count = 0;

/* Virtual filesystem providers */
#define MAX_PROVIDERS 16
static IOProvider *providers[MAX_PROVIDERS];
static int provider_count = 0;

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

void io_sandbox_set_default(int allow) {
	sandbox_default_allow = allow;
}

int io_sandbox_get_default(void) {
	return sandbox_default_allow;
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
		
	case IO_SANDBOX_RESTRICTED:
		/* Allow list takes priority - explicitly allowed paths always work */
		if (matches_any(path, allowed_patterns, allowed_count)) {
			return 1;
		}
		/* Deny list next - explicitly denied paths always fail */
		if (matches_any(path, denied_patterns, denied_count)) {
			return 0;
		}
		/* Default behavior for unmatched paths */
		return sandbox_default_allow;
	}
	return 0;
}

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

Atom io_open_file(const char *path, int is_input, int is_binary) {
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



long port_read(struct Port *p, void *buf, size_t n) {
	if (!p->is_open) return -1;
	
	if (p->provider_id == 0) {
		return fread(buf, 1, n, (FILE *)p->handle);
	} else {
		IOProvider *prov = providers[p->provider_id - 1];
		return prov->read(p->handle, buf, n, prov->userdata);
	}
}

int port_getc(struct Port *p) {
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



long port_write(struct Port *p, const void *buf, size_t n) {
	if (!p->is_open) return -1;
	
	if (p->provider_id == 0) {
		return fwrite(buf, 1, n, (FILE *)p->handle);
	} else {
		IOProvider *prov = providers[p->provider_id - 1];
		return prov->write(p->handle, buf, n, prov->userdata);
	}
}

void port_flush(struct Port *p) {
	if (p->provider_id == 0 && p->is_open) {
		fflush((FILE *)p->handle);
	}
}

int io_file_exists(const char *path) {
	const char *subpath;
	int provider_id = find_provider(path, &subpath);
	
	/* Sandbox check for stdio */
	if (provider_id == 0 && !io_sandbox_check_path(subpath, 0)) {
		return 0;
	}
	
	if (provider_id == 0) {
		FILE *f = fopen(subpath, "r");
		if (f) {
			fclose(f);
			return 1;
		} else {
			return 0;
		}
	} else {
		IOProvider *prov = providers[provider_id - 1];
		if (prov->exists && prov->exists(subpath, prov->userdata)) {
			return 1;
		} else {
			return 0;
		}
	}
}

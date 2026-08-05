#include "yas.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fnmatch.h>

char *yas_slurp(const char *path)
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

static YasIOSandboxMode sandbox_mode = YAS_IO_SANDBOX_UNRESTRICTED;
static int sandbox_default_allow = 1;  /* Default: allow unmatched paths */

/* Path patterns for allow/deny lists */
#define MAX_PATTERNS 64
static char *allowed_patterns[MAX_PATTERNS];
static int allowed_count = 0;
static char *denied_patterns[MAX_PATTERNS];
static int denied_count = 0;

/* Virtual filesystem providers */
#define MAX_PROVIDERS 16
static YasIOProvider *providers[MAX_PROVIDERS];
static int provider_count = 0;

void yas_io_sandbox_init(YasIOSandboxMode mode) {
	sandbox_mode = mode;
}

YasIOSandboxMode yas_io_sandbox_get_mode(void) {
	return sandbox_mode;
}

void yas_io_sandbox_allow_path(const char *pattern) {
	if (allowed_count < MAX_PATTERNS) {
		allowed_patterns[allowed_count++] = strdup(pattern);
	}
}

void yas_io_sandbox_deny_path(const char *pattern) {
	if (denied_count < MAX_PATTERNS) {
		denied_patterns[denied_count++] = strdup(pattern);
	}
}

void yas_io_sandbox_set_default(int allow) {
	sandbox_default_allow = allow;
}

int yas_io_sandbox_get_default(void) {
	return sandbox_default_allow;
}

void yas_io_sandbox_clear(void) {
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

int yas_io_sandbox_check_path(const char *path, int writing) {
	(void)writing; /* May be used for finer control later */
	
	switch (sandbox_mode) {
	case YAS_IO_SANDBOX_DISABLED:
		return 0; /* Never allowed */
		
	case YAS_IO_SANDBOX_UNRESTRICTED:
		return 1; /* Always allowed */
		
	case YAS_IO_SANDBOX_RESTRICTED:
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

int yas_io_register_provider(YasIOProvider *provider) {
	if (provider_count >= MAX_PROVIDERS) return -1;
	providers[provider_count++] = provider;
	return provider_count; /* Provider ID (1-based) */
}

void yas_io_unregister_provider(const char *prefix) {
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

void yas_port_close(YasAtom port) {
	if (port.type != YAS_ATOM_TYPE_PORT) return;
	struct YasPort *p = port.value.port;
	if (!p->is_open) return;
	
	if (p->provider_id == 0) {
		/* stdio */
		fclose((FILE *)p->handle);
	} else {
		/* Custom provider */
		YasIOProvider *prov = providers[p->provider_id - 1];
		if (prov->close) {
			prov->close(p->handle, prov->userdata);
		}
	}
	p->is_open = 0;
}

YasAtom yas_io_open_file(const char *path, int is_input, int is_binary) {
	const char *subpath;
	int provider_id = find_provider(path, &subpath);
	
	/* Sandbox check (skip for custom providers - they handle their own security) */
	if (provider_id == 0 && !yas_io_sandbox_check_path(subpath, !is_input)) {
		return yas_nil; /* Access denied */
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
		YasIOProvider *prov = providers[provider_id - 1];
		handle = prov->open(subpath, is_input, is_binary, prov->userdata);
	}
	
	if (!handle) return yas_nil;
	return yas_make_port(handle, path, is_input, is_binary, provider_id);
}



long yas_port_read(struct YasPort *p, void *buf, size_t n) {
	if (!p->is_open) return -1;
	
	if (p->provider_id == 0) {
		return fread(buf, 1, n, (FILE *)p->handle);
	} else {
		YasIOProvider *prov = providers[p->provider_id - 1];
		return prov->read(p->handle, buf, n, prov->userdata);
	}
}

int yas_port_getc(struct YasPort *p) {
	if (!p->is_open) return EOF;
	
	if (p->provider_id == 0) {
		return fgetc((FILE *)p->handle);
	} else {
		unsigned char c;
		YasIOProvider *prov = providers[p->provider_id - 1];
		long n = prov->read(p->handle, &c, 1, prov->userdata);
		return n == 1 ? c : EOF;
	}
}



long yas_port_write(struct YasPort *p, const void *buf, size_t n) {
	if (!p->is_open) return -1;
	
	if (p->provider_id == 0) {
		return fwrite(buf, 1, n, (FILE *)p->handle);
	} else {
		YasIOProvider *prov = providers[p->provider_id - 1];
		return prov->write(p->handle, buf, n, prov->userdata);
	}
}

void yas_port_flush(struct YasPort *p) {
	if (p->provider_id == 0 && p->is_open) {
		fflush((FILE *)p->handle);
	}
}

int yas_io_file_exists(const char *path) {
	const char *subpath;
	int provider_id = find_provider(path, &subpath);
	
	/* Sandbox check for stdio */
	if (provider_id == 0 && !yas_io_sandbox_check_path(subpath, 0)) {
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
		YasIOProvider *prov = providers[provider_id - 1];
		if (prov->exists && prov->exists(subpath, prov->userdata)) {
			return 1;
		} else {
			return 0;
		}
	}
}

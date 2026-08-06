#include "yas.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

YasAtom yas_library_registry = { YAS_ATOM_TYPE_NIL };

/* Search paths - modifiable at runtime via *search-paths* */
YasAtom yas_global_search_paths = { YAS_ATOM_TYPE_NIL };

/* Forward declarations */
static YasAtom make_library(YasAtom name);
static int process_import_set(YasAtom import_set, YasAtom target_env);

/* Unified Native Module System */
typedef void (*NativeLoader)(void);

typedef struct {
	const char *name;
	NativeLoader loader;
} NativeModule;

/* Forward declarations */
static YasAtom make_library(YasAtom name);
static int process_import_set(YasAtom import_set, YasAtom target_env);
void yas_load_base_library(void);
void yas_load_hash_library(void);
void yas_load_vector_library(void);
void yas_load_text_library(void);
void yas_load_io_library(void);
void yas_load_math_library(void);

/* Helper to register builtins */
static void reg_builtin(YasAtom env, YasAtom exports, const char *name, YasBuiltin fn) {
	YasAtom sym = yas_make_sym(name);
	yas_env_define(env, sym, yas_make_builtin(fn));
	yas_hash_set(exports, sym, sym);
}

static void reg_sym(YasAtom env, YasAtom exports, const char *name, YasAtom val) {
	YasAtom sym = yas_make_sym(name);
	yas_env_define(env, sym, val);
	yas_hash_set(exports, sym, sym);
}

YasAtom yas_define_native_module(const char *name_str, YasAtom *out_env, YasAtom *out_exports) {
	/* Parse "yas/primitives" -> (YAS PRIMITIVES) */
	YasAtom name = yas_nil;
	YasAtom tail = yas_nil;
	
	char *s = strdup(name_str);
	char *token = strtok(s, "/");
	while (token) {
		/* Uppercase token */
		char *p = token;
		while (*p) {
			if (*p >= 'a' && *p <= 'z') *p -= 32;
			p++;
		}
		
		YasAtom sym = yas_make_sym(token);
		YasAtom node = yas_cons(sym, yas_nil);
		if (yas_nilp(name)) {
			name = tail = node;
		} else {
			yas_set_cdr(tail, node);
			tail = node;
		}
		token = strtok(NULL, "/");
	}
	free(s);
	
	YasAtom lib = make_library(name);
	
	*out_env = yas_hash_ref(lib, yas_make_sym("*env*"), yas_nil);
	*out_exports = yas_hash_ref(lib, yas_make_sym("*exports*"), yas_nil);
	
	YasAtom key = yas_make_string(name_str);
	yas_hash_set(yas_library_registry, key, lib);
	
	return lib;
}

/* Native loaders - merge with Lisp library names */
void yas_load_base_library(void) {
	if (yas_nilp(yas_library_registry)) yas_library_registry = yas_make_hash();
	YasAtom env, exports;
	/* unified: registers to "yas/base" */
	yas_define_native_module("yas/base", &env, &exports);
	
	/* Register core primitives */
	reg_builtin(env, exports, "CAR", yas_builtin_car);
	reg_builtin(env, exports, "CDR", yas_builtin_cdr);
	reg_builtin(env, exports, "CONS", yas_builtin_cons);
	reg_builtin(env, exports, "+", yas_builtin_add);
	reg_builtin(env, exports, "-", yas_builtin_subtract);
	reg_builtin(env, exports, "*", yas_builtin_multiply);
	reg_builtin(env, exports, "/", yas_builtin_divide);
	reg_builtin(env, exports, "=", yas_builtin_numeq);
	reg_builtin(env, exports, "<", yas_builtin_less);
	reg_builtin(env, exports, "EQ?", yas_builtin_eq);
	reg_builtin(env, exports, "PAIR?", yas_builtin_pairp);
	reg_builtin(env, exports, "PROCEDURE?", yas_builtin_procp);
	reg_builtin(env, exports, "FLOAT?", yas_builtin_floatp);
	reg_builtin(env, exports, "INTEGER?", yas_builtin_integerp);
	reg_builtin(env, exports, "STRING?", yas_builtin_stringp);
	reg_builtin(env, exports, "THROW", yas_builtin_throw);
	reg_builtin(env, exports, "ERROR", yas_builtin_error);
	reg_builtin(env, exports, "EXIT", yas_builtin_exit);
	reg_builtin(env, exports, "PRINT", yas_builtin_print);
	reg_builtin(env, exports, "LENGTH", yas_builtin_list_length);
	reg_builtin(env, exports, "APPEND", yas_builtin_list_append);
	reg_sym(env, exports, "T", yas_make_sym("T"));
	reg_builtin(env, exports, "PROCEDURE-DOC", yas_builtin_procedure_doc);

	/* GC control/introspection - see docs/GC.md */
	reg_builtin(env, exports, "GC", yas_builtin_gc);
	reg_builtin(env, exports, "GC-START", yas_builtin_gc_start);
	reg_builtin(env, exports, "GC-STATS", yas_builtin_gc_stats);

	/* Global features list */
	reg_sym(env, exports, "*FEATURES*", yas_global_features);
	
	/* Search path management */
	reg_builtin(env, exports, "GET-SEARCH-PATHS", yas_builtin_get_search_paths);
	reg_builtin(env, exports, "SET-SEARCH-PATHS!", yas_builtin_set_search_paths);
	reg_builtin(env, exports, "ADD-SEARCH-PATH!", yas_builtin_add_search_path);
}

void yas_load_hash_library(void) {
	if (yas_nilp(yas_library_registry)) yas_library_registry = yas_make_hash();
	YasAtom env, exports;
	yas_define_native_module("yas/hash", &env, &exports);	
	
	reg_builtin(env, exports, "HASH?", yas_builtin_hashp);
	reg_builtin(env, exports, "MAKE-HASH", yas_builtin_make_hash);
	reg_builtin(env, exports, "HASH-REF", yas_builtin_hash_ref);
	reg_builtin(env, exports, "HASH-SET!", yas_builtin_hash_set);
	reg_builtin(env, exports, "HASH-REMOVE!", yas_builtin_hash_remove);
	reg_builtin(env, exports, "HASH-LENGTH", yas_builtin_hash_length);
	reg_builtin(env, exports, "HASH-JOIN", yas_builtin_hash_join);
	reg_builtin(env, exports, "HASH-KEYS", yas_builtin_hash_keys);
	
	reg_builtin(env, exports, "SET-METATABLE!", yas_builtin_set_metatable);
	reg_builtin(env, exports, "GET-METATABLE", yas_builtin_get_metatable);
	
	reg_builtin(env, exports, "SERIALIZE", yas_builtin_serialize);
	reg_builtin(env, exports, "DESERIALIZE", yas_builtin_deserialize);
	reg_builtin(env, exports, "READ-STRING", yas_builtin_read_string);
}

void yas_load_vector_library(void) {
	if (yas_nilp(yas_library_registry)) yas_library_registry = yas_make_hash();
	YasAtom env, exports;
	yas_define_native_module("yas/vector", &env, &exports);
	
	reg_builtin(env, exports, "VECTOR?", yas_builtin_vectorp);
	reg_builtin(env, exports, "MAKE-VECTOR", yas_builtin_make_vector);
	reg_builtin(env, exports, "VECTOR-REF", yas_builtin_vector_ref);
	reg_builtin(env, exports, "VECTOR-SET!", yas_builtin_vector_set);
	reg_builtin(env, exports, "VECTOR-LENGTH", yas_builtin_vector_length);
	reg_builtin(env, exports, "VECTOR-APPEND", yas_builtin_vector_append);
}

void yas_load_text_library(void) {
	if (yas_nilp(yas_library_registry)) yas_library_registry = yas_make_hash();
	YasAtom env, exports;
	yas_define_native_module("yas/text", &env, &exports);
	
	reg_builtin(env, exports, "CHAR?", yas_builtin_charp);
	reg_builtin(env, exports, "CHAR->INTEGER", yas_builtin_char_to_integer);
	reg_builtin(env, exports, "INTEGER->CHAR", yas_builtin_integer_to_char);
	reg_builtin(env, exports, "CHAR=?", yas_builtin_char_eq);
	reg_builtin(env, exports, "CHAR<?", yas_builtin_char_lt);
	
	reg_builtin(env, exports, "STRING->BYTES", yas_builtin_string_to_bytes);
	reg_builtin(env, exports, "BYTES->STRING", yas_builtin_bytes_to_string);
	reg_builtin(env, exports, "STRING-REF", yas_builtin_string_ref);
	reg_builtin(env, exports, "STRING=?", yas_builtin_string_eq);
	reg_builtin(env, exports, "STRING-BYTE-LENGTH", yas_builtin_string_byte_length);
	reg_builtin(env, exports, "STRING-LENGTH", yas_builtin_string_length);
	reg_builtin(env, exports, "STRING-APPEND", yas_builtin_string_append);
	reg_builtin(env, exports, "->STRING", yas_builtin_tostring);
	
	/* Format helper builtins */
	reg_builtin(env, exports, "NUMBER->STRING", yas_builtin_number_to_string);
	reg_builtin(env, exports, "STRING-UPCASE", yas_builtin_string_upcase);
	reg_builtin(env, exports, "STRING-DOWNCASE", yas_builtin_string_downcase);
	reg_builtin(env, exports, "STRING-CAPITALIZE", yas_builtin_string_capitalize);
	reg_builtin(env, exports, "MAKE-STRING", yas_builtin_make_string);
	reg_builtin(env, exports, "SUBSTRING", yas_builtin_substring);
	
	reg_builtin(env, exports, "KEYWORD?", yas_builtin_keywordp);
	reg_builtin(env, exports, "KEYWORD->STRING", yas_builtin_keyword_to_string);
	reg_builtin(env, exports, "STRING->KEYWORD", yas_builtin_string_to_keyword);
}

void yas_load_io_library(void) {
	if (yas_nilp(yas_library_registry)) yas_library_registry = yas_make_hash();
	YasAtom env, exports;
	yas_define_native_module("yas/io", &env, &exports);
	
	reg_builtin(env, exports, "OPEN-INPUT-FILE", yas_builtin_open_input_file);
	reg_builtin(env, exports, "OPEN-OUTPUT-FILE", yas_builtin_open_output_file);
	reg_builtin(env, exports, "CLOSE-PORT", yas_builtin_close_port);
	reg_builtin(env, exports, "PORT?", yas_builtin_portp);
	reg_builtin(env, exports, "INPUT-PORT?", yas_builtin_input_portp);
	reg_builtin(env, exports, "OUTPUT-PORT?", yas_builtin_output_portp);
	reg_builtin(env, exports, "PORT-OPEN?", yas_builtin_port_openp);
	reg_builtin(env, exports, "READ-CHAR", yas_builtin_read_char);
	reg_builtin(env, exports, "READ-LINE", yas_builtin_read_line);
	reg_builtin(env, exports, "READ-ALL", yas_builtin_read_all);
	reg_builtin(env, exports, "READ-BYTE", yas_builtin_read_byte);
	reg_builtin(env, exports, "READ-BYTES", yas_builtin_read_bytes);
	reg_builtin(env, exports, "WRITE-CHAR", yas_builtin_write_char);
	reg_builtin(env, exports, "WRITE-STRING", yas_builtin_write_string);
	reg_builtin(env, exports, "WRITE-BYTE", yas_builtin_write_byte);
	reg_builtin(env, exports, "WRITE-BYTES", yas_builtin_write_bytes);
	reg_builtin(env, exports, "NEWLINE", yas_builtin_newline);
	reg_builtin(env, exports, "FLUSH-PORT", yas_builtin_flush_port);
	reg_builtin(env, exports, "FILE-EXISTS?", yas_builtin_file_exists);
	reg_builtin(env, exports, "DELETE-FILE", yas_builtin_delete_file);
	reg_builtin(env, exports, "RENAME-FILE", yas_builtin_rename_file);
	
	reg_builtin(env, exports, "MAKE-BYTEVECTOR", yas_builtin_make_bytevector);
	reg_builtin(env, exports, "BYTEVECTOR?", yas_builtin_bytevectorp);
	reg_builtin(env, exports, "BYTEVECTOR-LENGTH", yas_builtin_bytevector_length);
	reg_builtin(env, exports, "BYTEVECTOR-U8-REF", yas_builtin_bytevector_u8_ref);
	reg_builtin(env, exports, "BYTEVECTOR-U8-SET!", yas_builtin_bytevector_u8_set);
	reg_builtin(env, exports, "BYTEVECTOR->STRING", yas_builtin_bytevector_to_string);
	reg_builtin(env, exports, "STRING->BYTEVECTOR", yas_builtin_string_to_bytevector);
	
	reg_builtin(env, exports, "IO-SANDBOX-ALLOW!", yas_builtin_io_sandbox_allow);
	reg_builtin(env, exports, "IO-SANDBOX-DENY!", yas_builtin_io_sandbox_deny);
	reg_builtin(env, exports, "IO-SANDBOX-MODE", yas_builtin_io_sandbox_mode);
}

void yas_load_math_library(void) {
	if (yas_nilp(yas_library_registry)) yas_library_registry = yas_make_hash();
	YasAtom env, exports;
	yas_define_native_module("yas/math", &env, &exports);
	
	reg_builtin(env, exports, "VEC2?", yas_builtin_vec2p);
	reg_builtin(env, exports, "VEC3?", yas_builtin_vec3p);
	reg_builtin(env, exports, "VEC4?", yas_builtin_vec4p);
	reg_builtin(env, exports, "MAT4?", yas_builtin_mat4p);
	reg_builtin(env, exports, "VEC2", yas_builtin_vec2);
	reg_builtin(env, exports, "VEC3", yas_builtin_vec3);
	reg_builtin(env, exports, "VEC4", yas_builtin_vec4);
	reg_builtin(env, exports, "MAT4-IDENTITY", yas_builtin_mat4_identity);
	reg_builtin(env, exports, "MAT4", yas_builtin_mat4);
	reg_builtin(env, exports, "VEC-X", yas_builtin_vec_x);
	reg_builtin(env, exports, "VEC-Y", yas_builtin_vec_y);
	reg_builtin(env, exports, "VEC-Z", yas_builtin_vec_z);
	reg_builtin(env, exports, "VEC-W", yas_builtin_vec_w);
	reg_builtin(env, exports, "VEC-REF", yas_builtin_vec_ref);
	reg_builtin(env, exports, "VEC-SET!", yas_builtin_vec_set);
	reg_builtin(env, exports, "MAT4*", yas_builtin_mat4_multiply);
	reg_builtin(env, exports, "MAT4-TRANSPOSE", yas_builtin_mat4_transpose);
	reg_builtin(env, exports, "MAT4-INVERSE", yas_builtin_mat4_inverse);
}

#ifdef YAS_FFI
void yas_load_ffi_library(void) {
	if (yas_nilp(yas_library_registry)) yas_library_registry = yas_make_hash();
	YasAtom env, exports;
	yas_define_native_module("yas/ffi", &env, &exports);
	
	reg_builtin(env, exports, "FFI-OPEN", yas_builtin_ffi_open);
	reg_builtin(env, exports, "FFI-CLOSE", yas_builtin_ffi_close);
	reg_builtin(env, exports, "FFI-LOOKUP", yas_builtin_ffi_lookup);
	reg_builtin(env, exports, "FFI-DEFINE", yas_builtin_ffi_define);
	reg_builtin(env, exports, "FFI-CALL", yas_builtin_ffi_call);
	reg_builtin(env, exports, "FFI-MALLOC", yas_builtin_ffi_malloc);
	reg_builtin(env, exports, "FFI-FREE", yas_builtin_ffi_free);
	reg_builtin(env, exports, "FFI-POINTER-REF", yas_builtin_ffi_pointer_ref);
	reg_builtin(env, exports, "FFI-POINTER-SET!", yas_builtin_ffi_pointer_set);
	reg_builtin(env, exports, "FFI-NULL", yas_builtin_ffi_null);
	reg_builtin(env, exports, "FFI-NULL?", yas_builtin_ffi_nullp);

	/* Struct support (internal builtins, wrapped by macros in ffi.ys) */
	reg_builtin(env, exports, "%FFI-DEFSTRUCT", yas_builtin_ffi_defstruct);
	reg_builtin(env, exports, "%MAKE-FFI-STRUCT", yas_builtin_ffi_make_struct);
	reg_builtin(env, exports, "%FFI-STRUCT-REF", yas_builtin_ffi_struct_ref);
	reg_builtin(env, exports, "%FFI-STRUCT-SET!", yas_builtin_ffi_struct_set);
}
#endif

static const NativeModule native_modules[] = {
	{ "yas/base", yas_load_base_library },
	{ "yas/hash", yas_load_hash_library },
	{ "yas/vector", yas_load_vector_library },
	{ "yas/text", yas_load_text_library },
	{ "yas/io", yas_load_io_library },
	{ "yas/math", yas_load_math_library },
#ifdef YAS_FFI
	{ "yas/ffi", yas_load_ffi_library },
#endif
	{ NULL, NULL }
};

/* Public API for embedders: eagerly registers every native module up
 * front (rather than lazily, on first `import`). Not called internally -
 * yas's own import path populates library_registry lazily as libraries
 * are requested, so this branch never runs for the interpreter/AOT paths. */
void yas_load_all_modules(void)
{
	if (yas_nilp(yas_library_registry)) yas_library_registry = yas_make_hash();
	
	/* Initialize default search paths if not set */
	if (yas_nilp(yas_global_search_paths)) {
		yas_global_search_paths = yas_cons(yas_make_string("./lib"), 
		                           yas_cons(yas_make_string("."), yas_nil));
	}
	
	for (int i = 0; native_modules[i].name; i++)
		native_modules[i].loader();
}

/* Convert (example math) -> "example/math" (lowercase) */
char *yas_library_name_to_string(YasAtom name)
{
	char buffer[256];
	buffer[0] = '\0';
	
	YasAtom p = name;
	int first = 1;
	
	while (!yas_nilp(p)) {
		if (p.type != YAS_ATOM_TYPE_PAIR || yas_car(p).type != YAS_ATOM_TYPE_SYMBOL)
			return NULL;
			
		const char *sym = yas_car(p).value.symbol;
		if (!first) strcat(buffer, "/");
		
		/* Append symbol lowercased */
		char *dest = buffer + strlen(buffer);
		for (const char *s = sym; *s; s++) {
			char c = *s;
			if (c >= 'A' && c <= 'Z') c += 32;
			*dest++ = c;
		}
		*dest = '\0';
		
		p = yas_cdr(p);
		first = 0;
	}
	
	return strdup(buffer);
}

/* Find library file in search paths */
static char *find_library_file(const char *lib_path)
{
	char *full_path = malloc(1024);
	YasAtom paths = yas_global_search_paths;
	
	while (!yas_nilp(paths)) {
		YasAtom dir = yas_car(paths);
		if (dir.type == YAS_ATOM_TYPE_STRING) {
			sprintf(full_path, "%s/%s.ys", dir.value.string, lib_path);
			FILE *f = fopen(full_path, "r");
			if (f) {
				fclose(f);
				return full_path;
			}
		}
		paths = yas_cdr(paths);
	}
	
	free(full_path);
	return NULL;
}

/* Create a new library record */
static YasAtom make_library(YasAtom name) {
	YasAtom lib = yas_make_hash();
	yas_hash_set(lib, yas_make_sym("*name*"), name);
	yas_hash_set(lib, yas_make_sym("*env*"), yas_env_create(yas_nil));
	yas_hash_set(lib, yas_make_sym("*exports*"), yas_make_hash()); /* exports map: ext->int */
	return lib;
}

#include "stdlib.h"

YasAtom yas_library_load(YasAtom name_parts)
{
	char *key_str = yas_library_name_to_string(name_parts);
	if (!key_str) return yas_nil;

	if (yas_nilp(yas_library_registry)) yas_library_registry = yas_make_hash();
	
	/* Initialize default search paths if not set */
	if (yas_nilp(yas_global_search_paths)) {
		yas_global_search_paths = yas_cons(yas_make_string("./lib"), 
		                           yas_cons(yas_make_string("."), yas_nil));
	}
	
	YasAtom key = yas_make_string(key_str);
	/* `key` is a fresh, otherwise-unreachable string used purely for
	 * registry lookups below - every branch here calls yas_eval_source/
	 * yas_eval_file at least once to load a dependency (nested library
	 * loading, e.g. (yas test) pulling in (yas math)/(yas vector)/
	 * (yas text) via its own define-library imports), which allocates
	 * enough to legitimately trigger a GC cycle. Without rooting it,
	 * `key` can be collected mid-load and a later allocation can reuse
	 * its memory, so a post-load re-lookup silently compares against a
	 * DIFFERENT library's name string instead of erroring - this is
	 * exactly what was observed: yas_hash_ref(registry, key, ...) here
	 * returning an unrelated library because `key`'s bytes had become
	 * "yas/vector" by the time a nested (yas math)/(yas vector) load
	 * reused its freed memory, while the registry's own correct
	 * "yas/test" entry sat untouched. One root covers the whole
	 * function, including the reassignment further down, since
	 * yas_gc_push_root() tracks the address of the slot, not a
	 * snapshot of its value. */
	yas_gc_push_root(&key);
	YasAtom lib = yas_hash_ref(yas_library_registry, key, yas_nil);

	if (!yas_nilp(lib)) {
		free(key_str);
		yas_gc_pop_roots(1);
		return lib;
	}

	/* Unified Loading: Check native modules first */
	for (int i = 0; native_modules[i].name; i++) {
		if (strcmp(key_str, native_modules[i].name) == 0) {
			/* Found native module. Initialize C part first. */
			native_modules[i].loader();

			/* Get the library object (created by loader) */
			lib = yas_hash_ref(yas_library_registry, key, yas_nil);

			/* Now check for Lisp source in stdlib to merge into this library */
			for (int j = 0; stdlib_files[j].name; j++) {
				if (strcmp(key_str, stdlib_files[j].name) == 0) {
					/* Found Lisp source. Eval into specific lib environment. */
					YasAtom ignore;
					int err = yas_eval_source(stdlib_files[j].source, key_str, yas_env_create(yas_nil), &ignore);

					/* Wait, eval_source evaluates in a FRESH env or passed env?
					   It passes env.
					   But (define-library) itself looks up valid registry/library to populate.
					   So we don't need to pass lib_env. module_define_library handles it.
					*/

					/* We might need to handle errors or just proceed */
					if (err) {
						printf("Error loading Lisp part of native module %s\n", key_str);
					}
					break;
				}
			}
			free(key_str);
			yas_gc_pop_roots(1);
			return lib;
		}
	}

	/* Check for purely Lisp built-in libraries (no C part yet, e.g. math if it was pure scheme) */
	for (int i = 0; stdlib_files[i].name; i++) {
		if (strcmp(key_str, stdlib_files[i].name) == 0) {
			/* Pure Lisp library */
			lib = make_library(name_parts);
			/* We need to set registry BEFORE eval to allow recursive refs if needed?
			   Or define-library does it. define-library does it. */

			/* Unlike the file-load branch below, this `lib` is not
			 * inserted into yas_library_registry before the eval - it's
			 * refetched from the registry afterward instead (line 479),
			 * relying on define-library to have populated it. Until
			 * then it's reachable from nothing, so it must be rooted
			 * across the eval that can trigger a nested GC step. */
			yas_gc_push_root(&lib);
			YasAtom ignore;
			int err = yas_eval_source(stdlib_files[i].source, key_str, yas_env_create(yas_nil), &ignore);
			yas_gc_pop_roots(1); /* lib */

			free(key_str);
			if (err) {
				yas_gc_pop_roots(1); /* key */
				return yas_nil;
			}

			/* Fetch created lib from registry */
			lib = yas_hash_ref(yas_library_registry, key, yas_nil);
			yas_gc_pop_roots(1);
			return lib;
		}
	}

	/* Not loaded. Find file */
	char *path = find_library_file(key_str);
	free(key_str);

	if (!path) {
		printf("Library not found: ");
		yas_print_expr(name_parts);
		printf("\n");
		yas_gc_pop_roots(1);
		return yas_nil;
	}

	/* Create placeholder */
	lib = make_library(name_parts);
	key_str = yas_library_name_to_string(name_parts);
	key = yas_make_string(key_str);
	yas_hash_set(yas_library_registry, key, lib);
	free(key_str);

	/* Load the file - ensure native modules are loaded first */
	if (yas_nilp(yas_library_registry)) {
		yas_load_all_modules();
	}

	/* Convert to absolute path to avoid resolve_include_path mangling it */
	char *abs_path = realpath(path, NULL);
	if (!abs_path) {
		printf("Error: Could not resolve path: %s\n", path);
		free(path);
		yas_gc_pop_roots(1);
		return yas_nil;
	}
	free(path);
	path = abs_path;

	/* Load the file */
	YasAtom ignore;
	int err = yas_eval_file(path, yas_env_create(yas_nil), &ignore, 0);

	if (err) {
		printf("Error loading library file %s: ", path);
		switch (err) {
		case YAS_ERROR_SYNTAX: printf("Syntax error\n"); break;
		case YAS_ERROR_UNBOUND: printf("Symbol not bound\n"); break;
		case YAS_ERROR_ARGS: printf("Argument error\n"); break;
		case YAS_ERROR_TYPE: printf("Type error\n"); break;
		case YAS_ERROR_EXCEPTION:
			printf("Exception: ");
			extern YasAtom yas_ctx_exception;
			yas_print_expr(yas_ctx_exception);
			putchar('\n');
			break;
		default:
			printf("Error code %d\n", err);
		}
		free(path);
		yas_gc_pop_roots(1);
		return yas_nil;
	}

	free(path);
	yas_gc_pop_roots(1);
	return lib;
}

/* Import resolution logic */

/* Resolve import set to a Hash of {name -> value} */
static YasAtom resolve_import_set(YasAtom set) {
	if (set.type == YAS_ATOM_TYPE_PAIR) {
		YasAtom op = yas_car(set);
		if (op.type == YAS_ATOM_TYPE_SYMBOL) {
			const char *s = op.value.symbol;
			if (strcmp(s, "ONLY") == 0) {
				/* (only set id ...) */
				YasAtom child_map = resolve_import_set(yas_car(yas_cdr(set)));
				if (yas_nilp(child_map)) return yas_nil;
				
				YasAtom result = yas_make_hash();
				YasAtom ids = yas_cdr(yas_cdr(set));
				while (!yas_nilp(ids)) {
					YasAtom id = yas_car(ids);
					YasAtom val = yas_hash_ref(child_map, id, yas_nil);
					
					yas_hash_set(result, id, val);
					ids = yas_cdr(ids);
				}
				return result;
			} else if (strcmp(s, "EXCEPT") == 0) {
				/* (except set id ...) */
				YasAtom child_map = resolve_import_set(yas_car(yas_cdr(set)));
				if (yas_nilp(child_map)) return yas_nil;

				YasAtom result = yas_make_hash();
				/* Raw struct YasHash* held across this loop bypasses the
				 * GC header - safe only because nothing in this loop
				 * re-enters the evaluator or otherwise allocates in a
				 * way that could resize/free it today. See the yas
				 * tracker for the ticket tracking a more structural
				 * fix. */
				struct YasHash *h = child_map.value.hash;
				
				/* Collect excepted ids into a list for easier check? Or naive search */
				YasAtom exceptions = yas_cdr(yas_cdr(set));
				
				for (size_t i = 0; i < h->size; i++) {
					YasAtom key = h->keys[i];
					int skip = 0;
					
					YasAtom p = exceptions;
					while (!yas_nilp(p)) {
						if (yas_car(p).value.symbol == key.value.symbol) {
							skip = 1; break;
						}
						p = yas_cdr(p);
					}
					
					if (!skip) {
						yas_hash_set(result, key, h->values[i]);
					}
				}
				return result;
			} else if (strcmp(s, "PREFIX") == 0) {
				/* (prefix set p) */
				YasAtom child_map = resolve_import_set(yas_car(yas_cdr(set)));
				YasAtom prefix = yas_car(yas_cdr(yas_cdr(set)));
				if (prefix.type != YAS_ATOM_TYPE_SYMBOL) return yas_nil;
				
				YasAtom result = yas_make_hash();
				struct YasHash *h = child_map.value.hash;
				
				for (size_t i = 0; i < h->size; i++) {
					YasAtom key = h->keys[i];
					/* Concat prefix + key */
					char buf[256];
					snprintf(buf, sizeof(buf), "%s%s", prefix.value.symbol, key.value.symbol);
					YasAtom new_key = yas_make_sym(buf);
					
					yas_hash_set(result, new_key, h->values[i]);
				}
				return result;
			} else if (strcmp(s, "RENAME") == 0) {
				/* (rename set (old new) ...) */
				YasAtom child_map = resolve_import_set(yas_car(yas_cdr(set)));
				YasAtom renames = yas_cdr(yas_cdr(set));
				
				YasAtom result = yas_make_hash();
				struct YasHash *h = child_map.value.hash;
				
				/* Copy all, applying renames if match */
				/* This approach: iterate original keys, check if renamed. */
				for (size_t i = 0; i < h->size; i++) {
					YasAtom key = h->keys[i];
					YasAtom new_key = key;
					
					YasAtom p = renames;
					while (!yas_nilp(p)) {
						YasAtom pair = yas_car(p); // (old new)
						if (yas_car(pair).value.symbol == key.value.symbol) {
							new_key = yas_car(yas_cdr(pair));
							break;
						}
						p = yas_cdr(p);
					}
					yas_hash_set(result, new_key, h->values[i]);
				}
				return result;
			}
		}
	}
	
	/* Base case: Library name */
	YasAtom lib = yas_library_load(set);
	if (yas_nilp(lib)) return yas_nil;

	YasAtom exports_map = yas_hash_ref(lib, yas_make_sym("*exports*"), yas_nil);
	YasAtom lib_env = yas_hash_ref(lib, yas_make_sym("*env*"), yas_nil);

	YasAtom result = yas_make_hash();
	if (yas_nilp(exports_map) || exports_map.type != YAS_ATOM_TYPE_HASH) return result;
	
	struct YasHash *h = exports_map.value.hash;
	for (size_t i = 0; i < h->size; i++) {
		YasAtom ext = h->keys[i];
		YasAtom internal = h->values[i];
		YasAtom val;
		/* Try internal first (for renames), then external (for direct exports) */
		if (yas_env_get(lib_env, internal, &val) == YAS_ERROR_OK) {
			yas_hash_set(result, ext, val);
		} else if (yas_env_get(lib_env, ext, &val) == YAS_ERROR_OK) {
			/* Direct export - internal == external */
			yas_hash_set(result, ext, val);
		}
	}
	
	return result;
}

static int process_import_set(YasAtom import_set, YasAtom target_env) {
	YasAtom bindings = resolve_import_set(import_set);
	if (yas_nilp(bindings) && !yas_nilp(import_set)) {
		printf("Failed to resolve import set: ");
		yas_print_expr(import_set);
		printf("\n");
		return YAS_ERROR_UNBOUND; 
	}
	if (bindings.type != YAS_ATOM_TYPE_HASH) return YAS_ERROR_TYPE;
	
	struct YasHash *h = bindings.value.hash;
	for (size_t i = 0; i < h->size; i++) {
		YasAtom key = h->keys[i];
		YasAtom val = h->values[i];
		yas_env_define(target_env, key, val);
	}
	return YAS_ERROR_OK;
}

int yas_module_import(YasAtom args, YasAtom env, YasAtom *result) {
	/* (import set1 set2 ...) */
	YasAtom p = args;
	while (!yas_nilp(p)) {
		int err = process_import_set(yas_car(p), env);
		if (err) return err;
		p = yas_cdr(p);
	}
	*result = yas_nil; 
	return YAS_ERROR_OK;
}

int yas_module_define_library(YasAtom args, YasAtom *result) {
	/* (define-library (name) decl ...) */
	if (yas_nilp(args)) return YAS_ERROR_ARGS;
	
	YasAtom name = yas_car(args);
	char *key_str = yas_library_name_to_string(name);
	if (!key_str) return YAS_ERROR_TYPE;
	
	YasAtom key = yas_make_string(key_str);
	free(key_str);
	
	/* Get or create library record */
	YasAtom lib = yas_hash_ref(yas_library_registry, key, yas_nil);
	if (yas_nilp(lib)) {
		lib = make_library(name);
		yas_hash_set(yas_library_registry, key, lib);
	}
	
	YasAtom lib_env = yas_hash_ref(lib, yas_make_sym("*env*"), yas_nil);
	YasAtom lib_exports = yas_hash_ref(lib, yas_make_sym("*exports*"), yas_nil);
	
	YasAtom decls = yas_cdr(args);
	while (!yas_nilp(decls)) {
		YasAtom decl = yas_car(decls);
		if (decl.type != YAS_ATOM_TYPE_PAIR) return YAS_ERROR_SYNTAX;
		
		YasAtom op = yas_car(decl);
		YasAtom dargs = yas_cdr(decl);
		
		if (op.type == YAS_ATOM_TYPE_SYMBOL) {
			if (strcmp(op.value.symbol, "EXPORT") == 0) {
				/* (export id1 (rename int ext) ...) */
				YasAtom p = dargs;
				while (!yas_nilp(p)) {
					YasAtom spec = yas_car(p);
					if (spec.type == YAS_ATOM_TYPE_SYMBOL) {
						yas_hash_set(lib_exports, spec, spec);
					} else if (spec.type == YAS_ATOM_TYPE_PAIR 
							&& yas_car(spec).type == YAS_ATOM_TYPE_SYMBOL
							&& strcmp(yas_car(spec).value.symbol, "RENAME") == 0) {
						/* (rename int ext) */
						YasAtom args = yas_cdr(spec); // (int ext)
						if (!yas_nilp(args) && !yas_nilp(yas_cdr(args))) {
							YasAtom internal = yas_car(args);
							YasAtom external = yas_car(yas_cdr(args));
							yas_hash_set(lib_exports, external, internal);
						}
					}
					p = yas_cdr(p);
				}
			} else if (strcmp(op.value.symbol, "IMPORT") == 0) {
				/* Process imports - these add symbols to lib_env */
				YasAtom import_list = dargs;
				while (!yas_nilp(import_list)) {
					YasAtom import_set = yas_car(import_list);
					int import_err = process_import_set(import_set, lib_env);
					if (import_err) {
						printf("Error importing dependency in library ");
						yas_print_expr(name);
						printf(": ");
						switch (import_err) {
						case YAS_ERROR_UNBOUND: printf("Symbol not bound\n"); break;
						case YAS_ERROR_TYPE: printf("Type error\n"); break;
						case YAS_ERROR_ARGS: printf("Argument error\n"); break;
						default: printf("Error code %d\n", import_err);
						}
						printf("  Import set: ");
						yas_print_expr(import_set);
						printf("\n");
						return import_err;
					}
					import_list = yas_cdr(import_list);
				}
			} else if (strcmp(op.value.symbol, "BEGIN") == 0) {
				YasAtom p = dargs;
				while (!yas_nilp(p)) {
					YasAtom expr = yas_car(p);
					YasAtom ignore;
					int err = yas_eval_expr(expr, lib_env, &ignore);
					if (err) return err;
					p = yas_cdr(p);
				}
			} else if (strcmp(op.value.symbol, "INCLUDE") == 0 ||
					   strcmp(op.value.symbol, "INCLUDE-CI") == 0) {
				/* (include "file") */
				int ci = (strcmp(op.value.symbol, "INCLUDE-CI") == 0);
				YasAtom p = dargs;
				while (!yas_nilp(p)) {
					YasAtom f = yas_car(p);
					if (f.type == YAS_ATOM_TYPE_STRING) {
						YasAtom ignore;
						int err = yas_eval_file(f.value.string, lib_env, &ignore, ci);
						if (err) return err;
					}
					p = yas_cdr(p);
				}
			}
		}
		
		decls = yas_cdr(decls);
	}
	
	*result = name;
	return YAS_ERROR_OK;
}

#include "lisp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Atom library_registry = { AtomType_Nil };

/* Search paths - modifiable at runtime via *search-paths* */
Atom global_search_paths = { AtomType_Nil };

/* Forward declarations */
static Atom make_library(Atom name);
static int process_import_set(Atom import_set, Atom target_env);

/* Unified Native Module System */
typedef void (*NativeLoader)(void);

typedef struct {
	const char *name;
	NativeLoader loader;
} NativeModule;

/* Forward declarations */
static Atom make_library(Atom name);
static int process_import_set(Atom import_set, Atom target_env);
void load_base_library(void);
void load_hash_library(void);
void load_vector_library(void);
void load_text_library(void);
void load_io_library(void);
void load_math_library(void);

/* Helper to register builtins */
static void reg_builtin(Atom env, Atom exports, const char *name, Builtin fn) {
	Atom sym = make_sym(name);
	env_define(env, sym, make_builtin(fn));
	hash_set(exports, sym, sym);
}

static void reg_sym(Atom env, Atom exports, const char *name, Atom val) {
	Atom sym = make_sym(name);
	env_define(env, sym, val);
	hash_set(exports, sym, sym);
}

Atom define_native_module(const char *name_str, Atom *out_env, Atom *out_exports) {
	/* Parse "gold/primitives" -> (GOLD PRIMITIVES) */
	Atom name = nil;
	Atom tail = nil;
	
	char *s = strdup(name_str);
	char *token = strtok(s, "/");
	while (token) {
		/* Uppercase token */
		char *p = token;
		while (*p) {
			if (*p >= 'a' && *p <= 'z') *p -= 32;
			p++;
		}
		
		Atom sym = make_sym(token);
		Atom node = cons(sym, nil);
		if (nilp(name)) {
			name = tail = node;
		} else {
			cdr(tail) = node;
			tail = node;
		}
		token = strtok(NULL, "/");
	}
	free(s);
	
	Atom lib = make_library(name);
	
	*out_env = hash_ref(lib, make_sym("*env*"), nil);
	*out_exports = hash_ref(lib, make_sym("*exports*"), nil);
	
	Atom key = make_string(name_str);
	hash_set(library_registry, key, lib);
	
	return lib;
}

/* Native loaders - merge with Lisp library names */
void load_base_library(void) {
	if (nilp(library_registry)) library_registry = make_hash();
	Atom env, exports;
	/* unified: registers to "gold/base" */
	define_native_module("gold/base", &env, &exports);
	
	/* Register core primitives */
	reg_builtin(env, exports, "CAR", builtin_car);
	reg_builtin(env, exports, "CDR", builtin_cdr);
	reg_builtin(env, exports, "CONS", builtin_cons);
	reg_builtin(env, exports, "+", builtin_add);
	reg_builtin(env, exports, "-", builtin_subtract);
	reg_builtin(env, exports, "*", builtin_multiply);
	reg_builtin(env, exports, "/", builtin_divide);
	reg_builtin(env, exports, "=", builtin_numeq);
	reg_builtin(env, exports, "<", builtin_less);
	reg_builtin(env, exports, "EQ?", builtin_eq);
	reg_builtin(env, exports, "PAIR?", builtin_pairp);
	reg_builtin(env, exports, "PROCEDURE?", builtin_procp);
	reg_builtin(env, exports, "FLOAT?", builtin_floatp);
	reg_builtin(env, exports, "INTEGER?", builtin_integerp);
	reg_builtin(env, exports, "STRING?", builtin_stringp);
	reg_builtin(env, exports, "THROW", builtin_throw);
	reg_builtin(env, exports, "ERROR", builtin_error);
	reg_builtin(env, exports, "PRINT", builtin_print);
	reg_builtin(env, exports, "LENGTH", builtin_list_length);
	reg_builtin(env, exports, "APPEND", builtin_list_append);
	reg_sym(env, exports, "T", make_sym("T"));
	reg_builtin(env, exports, "PROCEDURE-DOC", builtin_procedure_doc);
	
	/* Global features list */
	reg_sym(env, exports, "*FEATURES*", global_features);
	
	/* Search path management */
	reg_builtin(env, exports, "GET-SEARCH-PATHS", builtin_get_search_paths);
	reg_builtin(env, exports, "SET-SEARCH-PATHS!", builtin_set_search_paths);
	reg_builtin(env, exports, "ADD-SEARCH-PATH!", builtin_add_search_path);
}

void load_hash_library(void) {
	if (nilp(library_registry)) library_registry = make_hash();
	Atom env, exports;
	define_native_module("gold/hash", &env, &exports);	
	
	reg_builtin(env, exports, "HASH?", builtin_hashp);
	reg_builtin(env, exports, "MAKE-HASH", builtin_make_hash);
	reg_builtin(env, exports, "HASH-REF", builtin_hash_ref);
	reg_builtin(env, exports, "HASH-SET!", builtin_hash_set);
	reg_builtin(env, exports, "HASH-REMOVE!", builtin_hash_remove);
	reg_builtin(env, exports, "HASH-LENGTH", builtin_hash_length);
	reg_builtin(env, exports, "HASH-JOIN", builtin_hash_join);
	reg_builtin(env, exports, "HASH-KEYS", builtin_hash_keys);
	
	reg_builtin(env, exports, "SET-METATABLE!", builtin_set_metatable);
	reg_builtin(env, exports, "GET-METATABLE", builtin_get_metatable);
	
	reg_builtin(env, exports, "SERIALIZE", builtin_serialize);
	reg_builtin(env, exports, "DESERIALIZE", builtin_deserialize);
	reg_builtin(env, exports, "READ-STRING", builtin_read_string);
}

void load_vector_library(void) {
	if (nilp(library_registry)) library_registry = make_hash();
	Atom env, exports;
	define_native_module("gold/vector", &env, &exports);
	
	reg_builtin(env, exports, "VECTOR?", builtin_vectorp);
	reg_builtin(env, exports, "MAKE-VECTOR", builtin_make_vector);
	reg_builtin(env, exports, "VECTOR-REF", builtin_vector_ref);
	reg_builtin(env, exports, "VECTOR-SET!", builtin_vector_set);
	reg_builtin(env, exports, "VECTOR-LENGTH", builtin_vector_length);
	reg_builtin(env, exports, "VECTOR-APPEND", builtin_vector_append);
}

void load_text_library(void) {
	if (nilp(library_registry)) library_registry = make_hash();
	Atom env, exports;
	define_native_module("gold/text", &env, &exports);
	
	reg_builtin(env, exports, "CHAR?", builtin_charp);
	reg_builtin(env, exports, "CHAR->INTEGER", builtin_char_to_integer);
	reg_builtin(env, exports, "INTEGER->CHAR", builtin_integer_to_char);
	reg_builtin(env, exports, "CHAR=?", builtin_char_eq);
	reg_builtin(env, exports, "CHAR<?", builtin_char_lt);
	
	reg_builtin(env, exports, "STRING->BYTES", builtin_string_to_bytes);
	reg_builtin(env, exports, "BYTES->STRING", builtin_bytes_to_string);
	reg_builtin(env, exports, "STRING-REF", builtin_string_ref);
	reg_builtin(env, exports, "STRING=?", builtin_string_eq);
	reg_builtin(env, exports, "STRING-BYTE-LENGTH", builtin_string_byte_length);
	reg_builtin(env, exports, "STRING-LENGTH", builtin_string_length);
	reg_builtin(env, exports, "STRING-APPEND", builtin_string_append);
	reg_builtin(env, exports, "->STRING", builtin_tostring);
	
	/* Format helper builtins */
	reg_builtin(env, exports, "NUMBER->STRING", builtin_number_to_string);
	reg_builtin(env, exports, "STRING-UPCASE", builtin_string_upcase);
	reg_builtin(env, exports, "STRING-DOWNCASE", builtin_string_downcase);
	reg_builtin(env, exports, "STRING-CAPITALIZE", builtin_string_capitalize);
	reg_builtin(env, exports, "MAKE-STRING", builtin_make_string);
	reg_builtin(env, exports, "SUBSTRING", builtin_substring);
	
	reg_builtin(env, exports, "KEYWORD?", builtin_keywordp);
	reg_builtin(env, exports, "KEYWORD->STRING", builtin_keyword_to_string);
	reg_builtin(env, exports, "STRING->KEYWORD", builtin_string_to_keyword);
}

void load_io_library(void) {
	if (nilp(library_registry)) library_registry = make_hash();
	Atom env, exports;
	define_native_module("gold/io", &env, &exports);
	
	reg_builtin(env, exports, "OPEN-INPUT-FILE", builtin_open_input_file);
	reg_builtin(env, exports, "OPEN-OUTPUT-FILE", builtin_open_output_file);
	reg_builtin(env, exports, "CLOSE-PORT", builtin_close_port);
	reg_builtin(env, exports, "PORT?", builtin_portp);
	reg_builtin(env, exports, "INPUT-PORT?", builtin_input_portp);
	reg_builtin(env, exports, "OUTPUT-PORT?", builtin_output_portp);
	reg_builtin(env, exports, "PORT-OPEN?", builtin_port_openp);
	reg_builtin(env, exports, "READ-CHAR", builtin_read_char);
	reg_builtin(env, exports, "READ-LINE", builtin_read_line);
	reg_builtin(env, exports, "READ-ALL", builtin_read_all);
	reg_builtin(env, exports, "READ-BYTE", builtin_read_byte);
	reg_builtin(env, exports, "READ-BYTES", builtin_read_bytes);
	reg_builtin(env, exports, "WRITE-CHAR", builtin_write_char);
	reg_builtin(env, exports, "WRITE-STRING", builtin_write_string);
	reg_builtin(env, exports, "WRITE-BYTE", builtin_write_byte);
	reg_builtin(env, exports, "WRITE-BYTES", builtin_write_bytes);
	reg_builtin(env, exports, "NEWLINE", builtin_newline);
	reg_builtin(env, exports, "FLUSH-PORT", builtin_flush_port);
	reg_builtin(env, exports, "FILE-EXISTS?", builtin_file_exists);
	reg_builtin(env, exports, "DELETE-FILE", builtin_delete_file);
	reg_builtin(env, exports, "RENAME-FILE", builtin_rename_file);
	
	reg_builtin(env, exports, "MAKE-BYTEVECTOR", builtin_make_bytevector);
	reg_builtin(env, exports, "BYTEVECTOR?", builtin_bytevectorp);
	reg_builtin(env, exports, "BYTEVECTOR-LENGTH", builtin_bytevector_length);
	reg_builtin(env, exports, "BYTEVECTOR-U8-REF", builtin_bytevector_u8_ref);
	reg_builtin(env, exports, "BYTEVECTOR-U8-SET!", builtin_bytevector_u8_set);
	reg_builtin(env, exports, "BYTEVECTOR->STRING", builtin_bytevector_to_string);
	reg_builtin(env, exports, "STRING->BYTEVECTOR", builtin_string_to_bytevector);
	
	reg_builtin(env, exports, "IO-SANDBOX-ALLOW!", builtin_io_sandbox_allow);
	reg_builtin(env, exports, "IO-SANDBOX-DENY!", builtin_io_sandbox_deny);
	reg_builtin(env, exports, "IO-SANDBOX-MODE", builtin_io_sandbox_mode);
}

void load_math_library(void) {
	if (nilp(library_registry)) library_registry = make_hash();
	Atom env, exports;
	define_native_module("gold/math", &env, &exports);
	
	reg_builtin(env, exports, "VEC2?", builtin_vec2p);
	reg_builtin(env, exports, "VEC3?", builtin_vec3p);
	reg_builtin(env, exports, "VEC4?", builtin_vec4p);
	reg_builtin(env, exports, "MAT4?", builtin_mat4p);
	reg_builtin(env, exports, "VEC2", builtin_vec2);
	reg_builtin(env, exports, "VEC3", builtin_vec3);
	reg_builtin(env, exports, "VEC4", builtin_vec4);
	reg_builtin(env, exports, "MAT4-IDENTITY", builtin_mat4_identity);
	reg_builtin(env, exports, "MAT4", builtin_mat4);
	reg_builtin(env, exports, "VEC-X", builtin_vec_x);
	reg_builtin(env, exports, "VEC-Y", builtin_vec_y);
	reg_builtin(env, exports, "VEC-Z", builtin_vec_z);
	reg_builtin(env, exports, "VEC-W", builtin_vec_w);
	reg_builtin(env, exports, "VEC-REF", builtin_vec_ref);
	reg_builtin(env, exports, "VEC-SET!", builtin_vec_set);
	reg_builtin(env, exports, "MAT4*", builtin_mat4_multiply);
	reg_builtin(env, exports, "MAT4-TRANSPOSE", builtin_mat4_transpose);
	reg_builtin(env, exports, "MAT4-INVERSE", builtin_mat4_inverse);
}

#ifdef GOLD_FFI
void load_ffi_library(void) {
	if (nilp(library_registry)) library_registry = make_hash();
	Atom env, exports;
	define_native_module("gold/ffi", &env, &exports);
	
	reg_builtin(env, exports, "FFI-OPEN", builtin_ffi_open);
	reg_builtin(env, exports, "FFI-CLOSE", builtin_ffi_close);
	reg_builtin(env, exports, "FFI-LOOKUP", builtin_ffi_lookup);
	reg_builtin(env, exports, "FFI-DEFINE", builtin_ffi_define);
	reg_builtin(env, exports, "FFI-CALL", builtin_ffi_call);
	reg_builtin(env, exports, "FFI-MALLOC", builtin_ffi_malloc);
	reg_builtin(env, exports, "FFI-FREE", builtin_ffi_free);
	reg_builtin(env, exports, "FFI-POINTER-REF", builtin_ffi_pointer_ref);
	reg_builtin(env, exports, "FFI-POINTER-SET!", builtin_ffi_pointer_set);
	reg_builtin(env, exports, "FFI-NULL", builtin_ffi_null);
	reg_builtin(env, exports, "FFI-NULL?", builtin_ffi_nullp);

	/* Struct support (internal builtins, wrapped by macros in ffi.gl) */
	reg_builtin(env, exports, "%FFI-DEFSTRUCT", builtin_ffi_defstruct);
	reg_builtin(env, exports, "%MAKE-FFI-STRUCT", builtin_ffi_make_struct);
	reg_builtin(env, exports, "%FFI-STRUCT-REF", builtin_ffi_struct_ref);
	reg_builtin(env, exports, "%FFI-STRUCT-SET!", builtin_ffi_struct_set);
}
#endif

static const NativeModule native_modules[] = {
	{ "gold/base", load_base_library },
	{ "gold/hash", load_hash_library },
	{ "gold/vector", load_vector_library },
	{ "gold/text", load_text_library },
	{ "gold/io", load_io_library },
	{ "gold/math", load_math_library },
#ifdef GOLD_FFI
	{ "gold/ffi", load_ffi_library },
#endif
	{ NULL, NULL }
};

void load_all_modules(void)
{
	if (nilp(library_registry)) library_registry = make_hash();
	
	/* Initialize default search paths if not set */
	if (nilp(global_search_paths)) {
		global_search_paths = cons(make_string("./lib"), 
		                           cons(make_string("."), nil));
	}
	
	for (int i = 0; native_modules[i].name; i++)
		native_modules[i].loader();
}

/* Convert (example math) -> "example/math" (lowercase) */
char *library_name_to_string(Atom name)
{
	char buffer[256];
	buffer[0] = '\0';
	
	Atom p = name;
	int first = 1;
	
	while (!nilp(p)) {
		if (p.type != AtomType_Pair || car(p).type != AtomType_Symbol)
			return NULL;
			
		const char *sym = car(p).value.symbol;
		if (!first) strcat(buffer, "/");
		
		/* Append symbol lowercased */
		char *dest = buffer + strlen(buffer);
		for (const char *s = sym; *s; s++) {
			char c = *s;
			if (c >= 'A' && c <= 'Z') c += 32;
			*dest++ = c;
		}
		*dest = '\0';
		
		p = cdr(p);
		first = 0;
	}
	
	return strdup(buffer);
}

/* Find library file in search paths */
static char *find_library_file(const char *lib_path)
{
	char *full_path = malloc(1024);
	Atom paths = global_search_paths;
	
	while (!nilp(paths)) {
		Atom dir = car(paths);
		if (dir.type == AtomType_String) {
			sprintf(full_path, "%s/%s.gl", dir.value.string, lib_path);
			FILE *f = fopen(full_path, "r");
			if (f) {
				fclose(f);
				return full_path;
			}
		}
		paths = cdr(paths);
	}
	
	free(full_path);
	return NULL;
}

/* Create a new library record */
static Atom make_library(Atom name) {
	Atom lib = make_hash();
	hash_set(lib, make_sym("*name*"), name);
	hash_set(lib, make_sym("*env*"), env_create(nil));
	hash_set(lib, make_sym("*exports*"), make_hash()); /* exports map: ext->int */
	return lib;
}

#include "stdlib.h"

Atom library_load(Atom name_parts)
{
	char *key_str = library_name_to_string(name_parts);
	if (!key_str) return nil;
	
	if (nilp(library_registry)) library_registry = make_hash();
	
	/* Initialize default search paths if not set */
	if (nilp(global_search_paths)) {
		global_search_paths = cons(make_string("./lib"), 
		                           cons(make_string("."), nil));
	}
	
	Atom key = make_string(key_str);
	Atom lib = hash_ref(library_registry, key, nil);
	
	if (!nilp(lib)) {
		free(key_str);
		return lib;
	}
	
	/* Unified Loading: Check native modules first */
	for (int i = 0; native_modules[i].name; i++) {
		if (strcmp(key_str, native_modules[i].name) == 0) {
			/* Found native module. Initialize C part first. */
			native_modules[i].loader();
			
			/* Get the library object (created by loader) */
			lib = hash_ref(library_registry, key, nil);
			
			/* Now check for Lisp source in stdlib to merge into this library */
			for (int j = 0; stdlib_files[j].name; j++) {
				if (strcmp(key_str, stdlib_files[j].name) == 0) {
					/* Found Lisp source. Eval into specific lib environment. */
					Atom ignore;
					int err = eval_source(stdlib_files[j].source, key_str, env_create(nil), &ignore);

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
			
			Atom ignore;
			int err = eval_source(stdlib_files[i].source, key_str, env_create(nil), &ignore);
			
			free(key_str);
			if (err) return nil;
			
			/* Fetch created lib from registry */
			lib = hash_ref(library_registry, key, nil);
			return lib;
		}
	}

	/* Not loaded. Find file */
	char *path = find_library_file(key_str);
	free(key_str);
	
	if (!path) {
		printf("Library not found: ");
		print_expr(name_parts);
		printf("\n");
		return nil;
	}
	
	/* Create placeholder */
	lib = make_library(name_parts);
	key_str = library_name_to_string(name_parts);
	key = make_string(key_str); 
	hash_set(library_registry, key, lib);
	free(key_str);
	
	/* Load the file - ensure native modules are loaded first */
	if (nilp(library_registry)) {
		load_all_modules();
	}
	
	/* Convert to absolute path to avoid resolve_include_path mangling it */
	char *abs_path = realpath(path, NULL);
	if (!abs_path) {
		printf("Error: Could not resolve path: %s\n", path);
		free(path);
		return nil;
	}
	free(path);
	path = abs_path;
	
	/* Load the file */
	Atom ignore;
	int err = eval_file(path, env_create(nil), &ignore, 0);
	
	if (err) {
		printf("Error loading library file %s: ", path);
		switch (err) {
		case Error_Syntax: printf("Syntax error\n"); break;
		case Error_Unbound: printf("Symbol not bound\n"); break;
		case Error_Args: printf("Argument error\n"); break;
		case Error_Type: printf("Type error\n"); break;
		case Error_Exception:
			printf("Exception: ");
			extern Atom ctx_exception;
			print_expr(ctx_exception);
			putchar('\n');
			break;
		default:
			printf("Error code %d\n", err);
		}
		free(path);
		return nil;
	}
	
	free(path);
	return lib;
}

/* Import resolution logic */

/* Resolve import set to a Hash of {name -> value} */
static Atom resolve_import_set(Atom set) {
	if (set.type == AtomType_Pair) {
		Atom op = car(set);
		if (op.type == AtomType_Symbol) {
			const char *s = op.value.symbol;
			if (strcmp(s, "ONLY") == 0) {
				/* (only set id ...) */
				Atom child_map = resolve_import_set(car(cdr(set)));
				if (nilp(child_map)) return nil;
				
				Atom result = make_hash();
				Atom ids = cdr(cdr(set));
				while (!nilp(ids)) {
					Atom id = car(ids);
					Atom val = hash_ref(child_map, id, nil);
					
					hash_set(result, id, val);
					ids = cdr(ids);
				}
				return result;
			} else if (strcmp(s, "EXCEPT") == 0) {
				/* (except set id ...) */
				Atom child_map = resolve_import_set(car(cdr(set)));
				if (nilp(child_map)) return nil;
				
				Atom result = make_hash();
				struct Hash *h = child_map.value.hash;
				
				/* Collect excepted ids into a list for easier check? Or naive search */
				Atom exceptions = cdr(cdr(set));
				
				for (size_t i = 0; i < h->size; i++) {
					Atom key = h->keys[i];
					int skip = 0;
					
					Atom p = exceptions;
					while (!nilp(p)) {
						if (car(p).value.symbol == key.value.symbol) {
							skip = 1; break;
						}
						p = cdr(p);
					}
					
					if (!skip) {
						hash_set(result, key, h->values[i]);
					}
				}
				return result;
			} else if (strcmp(s, "PREFIX") == 0) {
				/* (prefix set p) */
				Atom child_map = resolve_import_set(car(cdr(set)));
				Atom prefix = car(cdr(cdr(set)));
				if (prefix.type != AtomType_Symbol) return nil;
				
				Atom result = make_hash();
				struct Hash *h = child_map.value.hash;
				
				for (size_t i = 0; i < h->size; i++) {
					Atom key = h->keys[i];
					/* Concat prefix + key */
					char buf[256];
					snprintf(buf, sizeof(buf), "%s%s", prefix.value.symbol, key.value.symbol);
					Atom new_key = make_sym(buf);
					
					hash_set(result, new_key, h->values[i]);
				}
				return result;
			} else if (strcmp(s, "RENAME") == 0) {
				/* (rename set (old new) ...) */
				Atom child_map = resolve_import_set(car(cdr(set)));
				Atom renames = cdr(cdr(set));
				
				Atom result = make_hash();
				struct Hash *h = child_map.value.hash;
				
				/* Copy all, applying renames if match */
				/* This approach: iterate original keys, check if renamed. */
				for (size_t i = 0; i < h->size; i++) {
					Atom key = h->keys[i];
					Atom new_key = key;
					
					Atom p = renames;
					while (!nilp(p)) {
						Atom pair = car(p); // (old new)
						if (car(pair).value.symbol == key.value.symbol) {
							new_key = car(cdr(pair));
							break;
						}
						p = cdr(p);
					}
					hash_set(result, new_key, h->values[i]);
				}
				return result;
			}
		}
	}
	
	/* Base case: Library name */
	Atom lib = library_load(set);
	if (nilp(lib)) return nil;
	
	Atom exports_map = hash_ref(lib, make_sym("*exports*"), nil);
	Atom lib_env = hash_ref(lib, make_sym("*env*"), nil);
	
	Atom result = make_hash();
	if (nilp(exports_map) || exports_map.type != AtomType_Hash) return result;
	
	struct Hash *h = exports_map.value.hash;
	Atom ffid_sym = make_sym("%FFI-DEFSTRUCT");
	for (size_t i = 0; i < h->size; i++) {
		Atom ext = h->keys[i];
		Atom internal = h->values[i];
		Atom val;
		/* Try internal first (for renames), then external (for direct exports) */
		if (env_get(lib_env, internal, &val) == Error_OK) {
			hash_set(result, ext, val);
		} else if (env_get(lib_env, ext, &val) == Error_OK) {
			/* Direct export - internal == external */
			hash_set(result, ext, val);
		} else {
			/* Export not found - check if it's %FFI-DEFSTRUCT for debugging */
			if (ext.type == AtomType_Symbol && strcmp(ext.value.symbol, "%FFI-DEFSTRUCT") == 0) {
				printf("DEBUG: %FFI-DEFSTRUCT export found but not in library environment\n");
				printf("  Export key: ");
				print_expr(ext);
				printf("\n  Export value: ");
				print_expr(internal);
				printf("\n");
			}
		}
	}
	
	return result;
}

static int process_import_set(Atom import_set, Atom target_env) {
	Atom bindings = resolve_import_set(import_set);
	if (nilp(bindings) && !nilp(import_set)) {
		printf("Failed to resolve import set: ");
		print_expr(import_set);
		printf("\n");
		return Error_Unbound; 
	}
	if (bindings.type != AtomType_Hash) return Error_Type;
	
	struct Hash *h = bindings.value.hash;
	for (size_t i = 0; i < h->size; i++) {
		Atom key = h->keys[i];
		Atom val = h->values[i];
		env_define(target_env, key, val);
		/* Debug: verify %FFI-DEFSTRUCT is being added */
		if (key.type == AtomType_Symbol && strcmp(key.value.symbol, "%FFI-DEFSTRUCT") == 0) {
			Atom check;
			if (env_get(target_env, key, &check) == Error_OK) {
				/* Successfully added */
			} else {
				printf("WARNING: Failed to add %FFI-DEFSTRUCT to target environment\n");
			}
		}
	}
	return Error_OK;
}

int module_import(Atom args, Atom env, Atom *result) {
	/* (import set1 set2 ...) */
	Atom p = args;
	while (!nilp(p)) {
		int err = process_import_set(car(p), env);
		if (err) return err;
		p = cdr(p);
	}
	*result = nil; 
	return Error_OK;
}

int module_define_library(Atom args, Atom *result) {
	/* (define-library (name) decl ...) */
	if (nilp(args)) return Error_Args;
	
	Atom name = car(args);
	char *key_str = library_name_to_string(name);
	if (!key_str) return Error_Type;
	
	Atom key = make_string(key_str);
	free(key_str);
	
	/* Get or create library record */
	Atom lib = hash_ref(library_registry, key, nil);
	if (nilp(lib)) {
		lib = make_library(name);
		hash_set(library_registry, key, lib);
	}
	
	Atom lib_env = hash_ref(lib, make_sym("*env*"), nil);
	Atom lib_exports = hash_ref(lib, make_sym("*exports*"), nil);
	
	Atom decls = cdr(args);
	while (!nilp(decls)) {
		Atom decl = car(decls);
		if (decl.type != AtomType_Pair) return Error_Syntax;
		
		Atom op = car(decl);
		Atom dargs = cdr(decl);
		
		if (op.type == AtomType_Symbol) {
			if (strcmp(op.value.symbol, "EXPORT") == 0) {
				/* (export id1 (rename int ext) ...) */
				Atom p = dargs;
				while (!nilp(p)) {
					Atom spec = car(p);
					if (spec.type == AtomType_Symbol) {
						hash_set(lib_exports, spec, spec);
					} else if (spec.type == AtomType_Pair 
							&& car(spec).type == AtomType_Symbol
							&& strcmp(car(spec).value.symbol, "RENAME") == 0) {
						/* (rename int ext) */
						Atom args = cdr(spec); // (int ext)
						if (!nilp(args) && !nilp(cdr(args))) {
							Atom internal = car(args);
							Atom external = car(cdr(args));
							hash_set(lib_exports, external, internal);
						}
					}
					p = cdr(p);
				}
			} else if (strcmp(op.value.symbol, "IMPORT") == 0) {
				/* Process imports - these add symbols to lib_env */
				Atom import_list = dargs;
				while (!nilp(import_list)) {
					Atom import_set = car(import_list);
					int import_err = process_import_set(import_set, lib_env);
					if (import_err) {
						printf("Error importing dependency in library ");
						print_expr(name);
						printf(": ");
						switch (import_err) {
						case Error_Unbound: printf("Symbol not bound\n"); break;
						case Error_Type: printf("Type error\n"); break;
						case Error_Args: printf("Argument error\n"); break;
						default: printf("Error code %d\n", import_err);
						}
						printf("  Import set: ");
						print_expr(import_set);
						printf("\n");
						return import_err;
					}
					import_list = cdr(import_list);
				}
			} else if (strcmp(op.value.symbol, "BEGIN") == 0) {
				Atom p = dargs;
				while (!nilp(p)) {
					Atom expr = car(p);
					Atom ignore;
					int err = eval_expr(expr, lib_env, &ignore);
					if (err) return err;
					p = cdr(p);
				}
			} else if (strcmp(op.value.symbol, "INCLUDE") == 0 ||
					   strcmp(op.value.symbol, "INCLUDE-CI") == 0) {
				/* (include "file") */
				int ci = (strcmp(op.value.symbol, "INCLUDE-CI") == 0);
				Atom p = dargs;
				while (!nilp(p)) {
					Atom f = car(p);
					if (f.type == AtomType_String) {
						Atom ignore;
						int err = eval_file(f.value.string, lib_env, &ignore, ci);
						if (err) return err;
					}
					p = cdr(p);
				}
			}
		}
		
		decls = cdr(decls);
	}
	
	*result = name;
	return Error_OK;
}

#include "lisp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Atom library_registry = { AtomType_Nil };

/* Search paths */
static const char *search_paths[] = {
	"./lib",
	".",
	NULL
};

/* Forward declarations */
static Atom make_library(Atom name);
static int process_import_set(Atom import_set, Atom target_env);

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

static void create_base_library(void) {
	/* (gold primitives) */
	Atom name = list_create(2, make_sym("GOLD"), make_sym("PRIMITIVES"));
	Atom lib = make_library(name);
	
	Atom env = hash_ref(lib, make_sym("__env__"), nil);
	Atom exports = hash_ref(lib, make_sym("__exports__"), nil);
	
	/* Register everything from main.c */
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
	reg_builtin(env, exports, "STRING?", builtin_stringp);
	reg_builtin(env, exports, "VECTOR?", builtin_vectorp);
	reg_builtin(env, exports, "HASH?", builtin_hashp);
	reg_builtin(env, exports, "LENGTH", builtin_length);
	reg_builtin(env, exports, "CONCAT", builtin_concat);
	reg_builtin(env, exports, "MAKE-VECTOR", builtin_make_vector);
	reg_builtin(env, exports, "VECTOR-REF", builtin_vector_ref);
	reg_builtin(env, exports, "VECTOR-SET!", builtin_vector_set);
	reg_builtin(env, exports, "MAKE-HASH", builtin_make_hash);
	reg_builtin(env, exports, "HASH-REF", builtin_hash_ref);
	reg_builtin(env, exports, "HASH-SET!", builtin_hash_set);
	reg_builtin(env, exports, "HASH-REMOVE!", builtin_hash_remove);
	reg_builtin(env, exports, "THROW", builtin_throw);
	reg_builtin(env, exports, "ERROR", builtin_error);
	reg_builtin(env, exports, "SET-METATABLE!", builtin_set_metatable);
	reg_builtin(env, exports, "GET-METATABLE", builtin_get_metatable);
	reg_builtin(env, exports, "PRINT", builtin_print);
	
	reg_sym(env, exports, "T", make_sym("T"));
	
	Atom key = make_string("gold/primitives");
	hash_set(library_registry, key, lib);
}

void module_init(void)
{
	library_registry = make_hash();
	create_base_library();
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
	const char **dir = search_paths;
	
	while (*dir) {
		sprintf(full_path, "%s/%s.gl", *dir, lib_path);
		FILE *f = fopen(full_path, "r");
		if (f) {
			fclose(f);
			return full_path;
		}
		dir++;
	}
	
	free(full_path);
	return NULL;
}

/* Create a new library record */
static Atom make_library(Atom name) {
	Atom lib = make_hash();
	hash_set(lib, make_sym("__name__"), name);
	hash_set(lib, make_sym("__env__"), env_create(nil));
	hash_set(lib, make_sym("__exports__"), make_hash()); /* exports map: ext->int */
	return lib;
}

#include "stdlib.h"

Atom library_load(Atom name_parts)
{
	char *key_str = library_name_to_string(name_parts);
	if (!key_str) return nil;
	
	Atom key = make_string(key_str);
	Atom lib = hash_ref(library_registry, key, nil);
	
	if (!nilp(lib)) {
		free(key_str);
		return lib;
	}
	
	/* Check for built-in libraries */
	for (int i = 0; stdlib_files[i].name; i++) {
		if (strcmp(key_str, stdlib_files[i].name) == 0) {
			/* Load from embedded source */
			lib = make_library(name_parts);
			hash_set(library_registry, key, lib);
			
			Atom ignore;
			int err = eval_source(stdlib_files[i].source, key_str, env_create(nil), &ignore);
			
			free(key_str);
			if (err) return nil;
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
	
	/* Load the file */
	Atom ignore;
	int err = eval_file(path, env_create(nil), &ignore, 0);
	
	free(path);
	
	if (err) return nil;
	
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
					/* Only include if found? R7RS says error if not found. */
					/* hash_ref returns nil if not found. Nil is valid value? 
					   Actually env stores nil as value. But hash_ref default is nil.
					   We assume libraries don't export 'nil' binding (unbound).
					   But nil IS a value. 
					   We should check existence. hash_ref doesn't strictly distinguish.
					   But wait, values in env are atoms. nil atom is valid.
					   For now assume if key exists we get it.
					   We iterate ids.
					*/
					/* Better: iterate child_map keys and check if in ids? No, iterate ids. */
					/* We risk missing if value is nil (which is valid pair end).
					   But variables usually hold values.
					*/
					hash_set(result, id, val);
					ids = cdr(ids);
				}
				return result;
			} else if (strcmp(s, "EXCEPT") == 0) {
				/* (except set id ...) */
				Atom child_map = resolve_import_set(car(cdr(set)));
				if (nilp(child_map)) return nil;
				
				/* Copy all except ids */
				/* Since we don't have hash_copy, we iterate and filter */
				/* This modifies child_map? NO, we shouldn't modify shared map. */
				/* We need to create NEW map. */
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
	
	Atom exports_map = hash_ref(lib, make_sym("__exports__"), nil);
	Atom lib_env = hash_ref(lib, make_sym("__env__"), nil);
	
	Atom result = make_hash();
	if (nilp(exports_map) || exports_map.type != AtomType_Hash) return result;
	
	struct Hash *h = exports_map.value.hash;
	for (size_t i = 0; i < h->size; i++) {
		Atom ext = h->keys[i];
		Atom internal = h->values[i];
		Atom val;
		if (env_get(lib_env, internal, &val) == Error_OK) {
			hash_set(result, ext, val);
		} else {
			printf("DEBUG: Failed to resolve export: ");
			print_expr(internal);
			printf(" from library ");
			print_expr(hash_ref(lib, make_sym("__name__"), nil));
			printf("\n");
		}
	}
	
	return result;
}

static int process_import_set(Atom import_set, Atom target_env) {
	Atom bindings = resolve_import_set(import_set);
	if (nilp(bindings) && !nilp(import_set)) {
		return Error_Unbound; 
	}
	if (bindings.type != AtomType_Hash) return Error_Type;
	
	struct Hash *h = bindings.value.hash;
	for (size_t i = 0; i < h->size; i++) {
		env_define(target_env, h->keys[i], h->values[i]);
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
	
	Atom lib_env = hash_ref(lib, make_sym("__env__"), nil);
	Atom lib_exports = hash_ref(lib, make_sym("__exports__"), nil);
	
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
				module_import(dargs, lib_env, result);
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

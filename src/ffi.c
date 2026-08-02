/*
 * ffi.c - Foreign Function Interface for YAS Scheme
 * 
 * Provides bindings to libffi for calling C functions from Scheme.
 * This file is only compiled when YAS_FFI is defined.
 */

#ifdef YAS_FFI

#include "lisp.h"
#include <ffi.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ============================================================
 * Library Loading
 * ============================================================ */

void *ffi_load_library(const char *path)
{
	return dlopen(path, RTLD_LAZY);
}

void ffi_close_library(void *handle)
{
	if (handle) dlclose(handle);
}

void *ffi_get_symbol(void *handle, const char *name)
{
	return dlsym(handle, name);
}

/* ============================================================
 * Type Mapping
 * ============================================================ */

/* Map keyword name to YasFFIType */
static YasFFIType keyword_to_type(const char *name)
{
	if (strcmp(name, "VOID") == 0) return YAS_FFI_TYPE_VOID;
	if (strcmp(name, "INT8") == 0) return YAS_FFI_TYPE_INT8;
	if (strcmp(name, "UINT8") == 0) return YAS_FFI_TYPE_UINT8;
	if (strcmp(name, "INT16") == 0) return YAS_FFI_TYPE_INT16;
	if (strcmp(name, "UINT16") == 0) return YAS_FFI_TYPE_UINT16;
	if (strcmp(name, "INT32") == 0 || strcmp(name, "INT") == 0) return YAS_FFI_TYPE_INT32;
	if (strcmp(name, "UINT32") == 0 || strcmp(name, "UINT") == 0) return YAS_FFI_TYPE_UINT32;
	if (strcmp(name, "INT64") == 0 || strcmp(name, "LONG") == 0) return YAS_FFI_TYPE_INT64;
	if (strcmp(name, "UINT64") == 0 || strcmp(name, "ULONG") == 0) return YAS_FFI_TYPE_UINT64;
	if (strcmp(name, "FLOAT") == 0) return YAS_FFI_TYPE_FLOAT;
	if (strcmp(name, "DOUBLE") == 0) return YAS_FFI_TYPE_DOUBLE;
	if (strcmp(name, "POINTER") == 0 || strcmp(name, "PTR") == 0) return YAS_FFI_TYPE_POINTER;
	if (strcmp(name, "STRING") == 0 || strcmp(name, "CSTRING") == 0) return YAS_FFI_TYPE_STRING;
	return YAS_FFI_TYPE_VOID; /* Default */
}

/* Map YasFFIType to libffi type */
static ffi_type *type_to_ffi(YasFFIType t)
{
	switch (t) {
	case YAS_FFI_TYPE_VOID:    return &ffi_type_void;
	case YAS_FFI_TYPE_INT8:    return &ffi_type_sint8;
	case YAS_FFI_TYPE_UINT8:   return &ffi_type_uint8;
	case YAS_FFI_TYPE_INT16:   return &ffi_type_sint16;
	case YAS_FFI_TYPE_UINT16:  return &ffi_type_uint16;
	case YAS_FFI_TYPE_INT32:   return &ffi_type_sint32;
	case YAS_FFI_TYPE_UINT32:  return &ffi_type_uint32;
	case YAS_FFI_TYPE_INT64:   return &ffi_type_sint64;
	case YAS_FFI_TYPE_UINT64:  return &ffi_type_uint64;
	case YAS_FFI_TYPE_FLOAT:   return &ffi_type_float;
	case YAS_FFI_TYPE_DOUBLE:  return &ffi_type_double;
	case YAS_FFI_TYPE_POINTER:
	case YAS_FFI_TYPE_STRING:  return &ffi_type_pointer;
	case YAS_FFI_TYPE_STRUCT:  return &ffi_type_pointer; /* Handled specially */
	}
	return &ffi_type_void;
}

/* ============================================================
 * Struct Type Registry
 * ============================================================ */

/* Global struct type registry */
FFIStructType *ffi_struct_registry = NULL;

/* Look up a struct type by name */
FFIStructType *ffi_lookup_struct_type(const char *name)
{
	FFIStructType *st = ffi_struct_registry;
	while (st) {
		if (strcmp(st->name, name) == 0) {
			return st;
		}
		st = st->next;
	}
	return NULL;
}

/* Resolve a type name (keyword or symbol) to ffi_type and YasFFIType */
static ffi_type *resolve_type(const char *name, YasFFIType *out_type)
{
	/* First check primitive types (keywords) */
	YasFFIType t = keyword_to_type(name);
	if (t != YAS_FFI_TYPE_VOID || strcmp(name, "VOID") == 0) {
		*out_type = t;
		return type_to_ffi(t);
	}

	/* Check struct registry */
	FFIStructType *st = ffi_lookup_struct_type(name);
	if (st) {
		*out_type = YAS_FFI_TYPE_STRUCT;
		return (ffi_type *)st->ffi_struct;
	}

	*out_type = YAS_FFI_TYPE_VOID;
	return &ffi_type_void;
}

/* ============================================================
 * Function Preparation
 * ============================================================ */

struct FFIFunction *ffi_prepare(void *fn_ptr, Atom ret_type, Atom arg_types)
{
	struct FFIFunction *fn = malloc(sizeof(struct FFIFunction));
	memset(fn, 0, sizeof(struct FFIFunction));
	fn->fn_ptr = fn_ptr;

	/* Parse return type (keyword or symbol for structs) */
	fn->ret_type = YAS_FFI_TYPE_VOID;
	fn->ffi_ret_type = &ffi_type_void;
	if (ret_type.type == AtomType_Keyword) {
		fn->ffi_ret_type = resolve_type(ret_type.value.keyword, &fn->ret_type);
	} else if (ret_type.type == AtomType_Symbol) {
		fn->ffi_ret_type = resolve_type(ret_type.value.symbol, &fn->ret_type);
	}

	/* Count argument types */
	fn->arg_count = 0;
	Atom p = arg_types;
	while (!nilp(p)) {
		fn->arg_count++;
		p = cdr(p);
	}

	/* Parse argument types (keywords or symbols for structs) */
	fn->arg_types = NULL;
	fn->ffi_arg_types = NULL;
	if (fn->arg_count > 0) {
		fn->arg_types = malloc(sizeof(YasFFIType) * fn->arg_count);
		fn->ffi_arg_types = malloc(sizeof(ffi_type *) * fn->arg_count);
		p = arg_types;
		for (int i = 0; i < fn->arg_count; i++) {
			Atom t = car(p);
			if (t.type == AtomType_Keyword) {
				fn->ffi_arg_types[i] = resolve_type(t.value.keyword, &fn->arg_types[i]);
			} else if (t.type == AtomType_Symbol) {
				fn->ffi_arg_types[i] = resolve_type(t.value.symbol, &fn->arg_types[i]);
			} else {
				fn->arg_types[i] = YAS_FFI_TYPE_VOID;
				fn->ffi_arg_types[i] = &ffi_type_void;
			}
			p = cdr(p);
		}
	}

	/* Prepare ffi_cif */
	ffi_cif *cif = malloc(sizeof(ffi_cif));

	ffi_status status = ffi_prep_cif(cif, FFI_DEFAULT_ABI,
	                                  fn->arg_count,
	                                  (ffi_type *)fn->ffi_ret_type,
	                                  (ffi_type **)fn->ffi_arg_types);

	if (status != FFI_OK) {
		free(cif);
		if (fn->ffi_arg_types) free(fn->ffi_arg_types);
		if (fn->arg_types) free(fn->arg_types);
		free(fn);
		return NULL;
	}

	fn->cif = cif;

	return fn;
}

void ffi_free_function(struct FFIFunction *fn)
{
	if (!fn) return;
	if (fn->cif) free(fn->cif);
	if (fn->arg_types) free(fn->arg_types);
	if (fn->ffi_arg_types) free(fn->ffi_arg_types);
	free(fn);
}

/* ============================================================
 * Function Calling
 * ============================================================ */

Atom ffi_call_function(struct FFIFunction *fn, Atom args)
{
	if (!fn || !fn->cif) return nil;
	
	ffi_cif *cif = (ffi_cif *)fn->cif;
	
	/* Prepare argument values */
	void **arg_values = NULL;
	void *arg_storage = NULL;  /* Storage for actual values */
	char **string_temps = NULL; /* Temporary strings to free */
	
	if (fn->arg_count > 0) {
		arg_values = malloc(sizeof(void *) * fn->arg_count);
		/* 8 bytes per arg should be enough for any type */
		arg_storage = malloc(8 * fn->arg_count);
		string_temps = calloc(fn->arg_count, sizeof(char *));
		
		Atom p = args;
		for (int i = 0; i < fn->arg_count && !nilp(p); i++) {
			Atom v = car(p);
			void *storage = (char *)arg_storage + (i * 8);
			
			switch (fn->arg_types[i]) {
			case YAS_FFI_TYPE_INT8:
			case YAS_FFI_TYPE_UINT8:
				*(int8_t *)storage = (int8_t)(v.type == AtomType_Integer ? v.value.integer : 0);
				break;
			case YAS_FFI_TYPE_INT16:
			case YAS_FFI_TYPE_UINT16:
				*(int16_t *)storage = (int16_t)(v.type == AtomType_Integer ? v.value.integer : 0);
				break;
			case YAS_FFI_TYPE_INT32:
			case YAS_FFI_TYPE_UINT32:
				*(int32_t *)storage = (int32_t)(v.type == AtomType_Integer ? v.value.integer : 0);
				break;
			case YAS_FFI_TYPE_INT64:
			case YAS_FFI_TYPE_UINT64:
				*(int64_t *)storage = (int64_t)(v.type == AtomType_Integer ? v.value.integer : 0);
				break;
			case YAS_FFI_TYPE_FLOAT:
				*(float *)storage = (float)(v.type == AtomType_Float ? v.value.floatval : 
				                            (v.type == AtomType_Integer ? v.value.integer : 0));
				break;
			case YAS_FFI_TYPE_DOUBLE:
				*(double *)storage = (double)(v.type == AtomType_Float ? v.value.floatval :
				                              (v.type == AtomType_Integer ? v.value.integer : 0));
				break;
			case YAS_FFI_TYPE_STRING:
				if (v.type == AtomType_String) {
					string_temps[i] = strdup(v.value.string);
					*(char **)storage = string_temps[i];
				} else {
					*(char **)storage = NULL;
				}
				break;
			case YAS_FFI_TYPE_POINTER:
				if (v.type == AtomType_FFIPointer) {
					*(void **)storage = v.value.ffi_pointer;
				} else if (v.type == AtomType_String) {
					/* Allow strings as pointers for convenience */
					string_temps[i] = strdup(v.value.string);
					*(void **)storage = string_temps[i];
				} else if (nilp(v)) {
					*(void **)storage = NULL;
				} else {
					*(void **)storage = NULL;
				}
				break;
			case YAS_FFI_TYPE_STRUCT:
				/* For struct-by-value, libffi expects pointer to struct data */
				if (v.type == AtomType_FFIStruct && v.value.ffi_struct) {
					/* Point directly to struct data - libffi copies it */
					arg_values[i] = v.value.ffi_struct->data;
				} else {
					*(void **)storage = NULL;
					arg_values[i] = storage;
				}
				p = cdr(p);
				continue;
			default:
				*(void **)storage = NULL;
			}

			arg_values[i] = storage;
			p = cdr(p);
		}
	}
	
	/* Call the function */
	union {
		int8_t i8;
		int16_t i16;
		int32_t i32;
		int64_t i64;
		float f;
		double d;
		void *ptr;
	} ret_value;
	
	ffi_call(cif, FFI_FN(fn->fn_ptr), &ret_value, arg_values);
	
	/* Free temporary resources */
	if (string_temps) {
		for (int i = 0; i < fn->arg_count; i++) {
			if (string_temps[i]) free(string_temps[i]);
		}
		free(string_temps);
	}
	if (arg_values) free(arg_values);
	if (arg_storage) free(arg_storage);
	
	/* Convert return value to Atom */
	Atom result;
	switch (fn->ret_type) {
	case YAS_FFI_TYPE_VOID:
		result = nil;
		break;
	case YAS_FFI_TYPE_INT8:
	case YAS_FFI_TYPE_UINT8:
		result = make_int(ret_value.i8);
		break;
	case YAS_FFI_TYPE_INT16:
	case YAS_FFI_TYPE_UINT16:
		result = make_int(ret_value.i16);
		break;
	case YAS_FFI_TYPE_INT32:
	case YAS_FFI_TYPE_UINT32:
		result = make_int(ret_value.i32);
		break;
	case YAS_FFI_TYPE_INT64:
	case YAS_FFI_TYPE_UINT64:
		result = make_int(ret_value.i64);
		break;
	case YAS_FFI_TYPE_FLOAT:
		result = make_float(ret_value.f);
		break;
	case YAS_FFI_TYPE_DOUBLE:
		result = make_float(ret_value.d);
		break;
	case YAS_FFI_TYPE_POINTER:
		result.type = AtomType_FFIPointer;
		result.value.ffi_pointer = ret_value.ptr;
		break;
	case YAS_FFI_TYPE_STRING:
		if (ret_value.ptr) {
			result = make_string((char *)ret_value.ptr);
		} else {
			result = nil;
		}
		break;
	case YAS_FFI_TYPE_STRUCT:
		/* Struct by-value return not supported; fail like the nil guard above */
		result = nil;
		break;
	}
	
	return result;
}

/* ============================================================
 * Builtin Functions
 * ============================================================ */

/* (ffi-open path) -> ffi-library */
int builtin_ffi_open(Atom args, Atom *result)
{
	if (nilp(args) || car(args).type != AtomType_String) {
		return raise_error("type-error", "ffi-open requires a string path");
	}
	
	const char *path = car(args).value.string;
	void *handle = ffi_load_library(path);
	
	if (!handle) {
		return raise_error("ffi-error", dlerror());
	}
	
	result->type = AtomType_FFILibrary;
	result->value.ffi_library = handle;
	return Error_OK;
}

/* (ffi-close lib) -> #t */
int builtin_ffi_close(Atom args, Atom *result)
{
	if (nilp(args) || car(args).type != AtomType_FFILibrary) {
		return raise_error("type-error", "ffi-close requires an ffi-library");
	}
	
	ffi_close_library(car(args).value.ffi_library);
	*result = make_sym("T");
	return Error_OK;
}

/* (ffi-lookup lib name) -> ffi-pointer */
int builtin_ffi_lookup(Atom args, Atom *result)
{
	if (nilp(args) || car(args).type != AtomType_FFILibrary) {
		return raise_error("type-error", "ffi-lookup requires an ffi-library");
	}
	if (nilp(cdr(args)) || car(cdr(args)).type != AtomType_String) {
		return raise_error("type-error", "ffi-lookup requires a string name");
	}
	
	void *handle = car(args).value.ffi_library;
	const char *name = car(cdr(args)).value.string;
	
	void *sym = ffi_get_symbol(handle, name);
	if (!sym) {
		return raise_error("ffi-error", dlerror());
	}
	
	result->type = AtomType_FFIPointer;
	result->value.ffi_pointer = sym;
	return Error_OK;
}

/* (ffi-define ptr ret-type (arg-types...)) -> ffi-function */
int builtin_ffi_define(Atom args, Atom *result)
{
	if (nilp(args) || car(args).type != AtomType_FFIPointer) {
		return raise_error("type-error", "ffi-define requires an ffi-pointer");
	}
	if (nilp(cdr(args)) ||
	    (car(cdr(args)).type != AtomType_Keyword && car(cdr(args)).type != AtomType_Symbol)) {
		return raise_error("type-error", "ffi-define requires a return type keyword or struct name");
	}
	
	void *fn_ptr = car(args).value.ffi_pointer;
	Atom ret_type = car(cdr(args));
	Atom arg_types = nilp(cdr(cdr(args))) ? nil : car(cdr(cdr(args)));
	
	struct FFIFunction *fn = ffi_prepare(fn_ptr, ret_type, arg_types);
	if (!fn) {
		return raise_error("ffi-error", "Failed to prepare FFI function");
	}
	
	result->type = AtomType_FFIFunction;
	result->value.ffi_fn = fn;
	return Error_OK;
}

/* (ffi-call fn args...) -> result */
int builtin_ffi_call(Atom args, Atom *result)
{
	if (nilp(args) || car(args).type != AtomType_FFIFunction) {
		return raise_error("type-error", "ffi-call requires an ffi-function");
	}
	
	struct FFIFunction *fn = car(args).value.ffi_fn;
	*result = ffi_call_function(fn, cdr(args));
	return Error_OK;
}

/* (ffi-malloc size) -> ffi-pointer */
int builtin_ffi_malloc(Atom args, Atom *result)
{
	if (nilp(args) || car(args).type != AtomType_Integer) {
		return raise_error("type-error", "ffi-malloc requires an integer size");
	}
	
	size_t size = (size_t)car(args).value.integer;
	void *ptr = malloc(size);
	
	result->type = AtomType_FFIPointer;
	result->value.ffi_pointer = ptr;
	return Error_OK;
}

/* (ffi-free ptr) -> #t */
int builtin_ffi_free(Atom args, Atom *result)
{
	if (nilp(args) || car(args).type != AtomType_FFIPointer) {
		return raise_error("type-error", "ffi-free requires an ffi-pointer");
	}
	
	free(car(args).value.ffi_pointer);
	*result = make_sym("T");
	return Error_OK;
}

/* (ffi-pointer-ref ptr type offset) -> value */
int builtin_ffi_pointer_ref(Atom args, Atom *result)
{
	if (nilp(args) || car(args).type != AtomType_FFIPointer) {
		return raise_error("type-error", "ffi-pointer-ref requires an ffi-pointer");
	}
	if (nilp(cdr(args)) || car(cdr(args)).type != AtomType_Keyword) {
		return raise_error("type-error", "ffi-pointer-ref requires a type keyword");
	}
	if (nilp(cdr(cdr(args))) || car(cdr(cdr(args))).type != AtomType_Integer) {
		return raise_error("type-error", "ffi-pointer-ref requires an integer offset");
	}
	
	void *base = car(args).value.ffi_pointer;
	YasFFIType type = keyword_to_type(car(cdr(args)).value.keyword);
	size_t offset = (size_t)car(cdr(cdr(args))).value.integer;
	
	char *ptr = (char *)base + offset;
	
	switch (type) {
	case YAS_FFI_TYPE_INT8:
		*result = make_int(*(int8_t *)ptr);
		break;
	case YAS_FFI_TYPE_UINT8:
		*result = make_int(*(uint8_t *)ptr);
		break;
	case YAS_FFI_TYPE_INT16:
		*result = make_int(*(int16_t *)ptr);
		break;
	case YAS_FFI_TYPE_UINT16:
		*result = make_int(*(uint16_t *)ptr);
		break;
	case YAS_FFI_TYPE_INT32:
		*result = make_int(*(int32_t *)ptr);
		break;
	case YAS_FFI_TYPE_UINT32:
		*result = make_int(*(uint32_t *)ptr);
		break;
	case YAS_FFI_TYPE_INT64:
		*result = make_int(*(int64_t *)ptr);
		break;
	case YAS_FFI_TYPE_UINT64:
		*result = make_int((long)*(uint64_t *)ptr);
		break;
	case YAS_FFI_TYPE_FLOAT:
		*result = make_float(*(float *)ptr);
		break;
	case YAS_FFI_TYPE_DOUBLE:
		*result = make_float(*(double *)ptr);
		break;
	case YAS_FFI_TYPE_POINTER:
		result->type = AtomType_FFIPointer;
		result->value.ffi_pointer = *(void **)ptr;
		break;
	default:
		*result = nil;
	}
	
	return Error_OK;
}

/* (ffi-pointer-set! ptr type offset value) -> #t */
int builtin_ffi_pointer_set(Atom args, Atom *result)
{
	if (nilp(args) || car(args).type != AtomType_FFIPointer) {
		return raise_error("type-error", "ffi-pointer-set! requires an ffi-pointer");
	}
	if (nilp(cdr(args)) || car(cdr(args)).type != AtomType_Keyword) {
		return raise_error("type-error", "ffi-pointer-set! requires a type keyword");
	}
	if (nilp(cdr(cdr(args))) || car(cdr(cdr(args))).type != AtomType_Integer) {
		return raise_error("type-error", "ffi-pointer-set! requires an integer offset");
	}
	if (nilp(cdr(cdr(cdr(args))))) {
		return raise_error("args-error", "ffi-pointer-set! requires a value");
	}
	
	void *base = car(args).value.ffi_pointer;
	YasFFIType type = keyword_to_type(car(cdr(args)).value.keyword);
	size_t offset = (size_t)car(cdr(cdr(args))).value.integer;
	Atom value = car(cdr(cdr(cdr(args))));
	
	char *ptr = (char *)base + offset;
	
	switch (type) {
	case YAS_FFI_TYPE_INT8:
	case YAS_FFI_TYPE_UINT8:
		*(int8_t *)ptr = (int8_t)(value.type == AtomType_Integer ? value.value.integer : 0);
		break;
	case YAS_FFI_TYPE_INT16:
	case YAS_FFI_TYPE_UINT16:
		*(int16_t *)ptr = (int16_t)(value.type == AtomType_Integer ? value.value.integer : 0);
		break;
	case YAS_FFI_TYPE_INT32:
	case YAS_FFI_TYPE_UINT32:
		*(int32_t *)ptr = (int32_t)(value.type == AtomType_Integer ? value.value.integer : 0);
		break;
	case YAS_FFI_TYPE_INT64:
	case YAS_FFI_TYPE_UINT64:
		*(int64_t *)ptr = (int64_t)(value.type == AtomType_Integer ? value.value.integer : 0);
		break;
	case YAS_FFI_TYPE_FLOAT:
		*(float *)ptr = (float)(value.type == AtomType_Float ? value.value.floatval :
		                        (value.type == AtomType_Integer ? value.value.integer : 0));
		break;
	case YAS_FFI_TYPE_DOUBLE:
		*(double *)ptr = (double)(value.type == AtomType_Float ? value.value.floatval :
		                          (value.type == AtomType_Integer ? value.value.integer : 0));
		break;
	case YAS_FFI_TYPE_POINTER:
		if (value.type == AtomType_FFIPointer) {
			*(void **)ptr = value.value.ffi_pointer;
		}
		break;
	default:
		break;
	}
	
	*result = make_sym("T");
	return Error_OK;
}

/* (ffi-null) -> null pointer */
int builtin_ffi_null(Atom args, Atom *result)
{
	(void)args;
	result->type = AtomType_FFIPointer;
	result->value.ffi_pointer = NULL;
	return Error_OK;
}

/* (ffi-null? ptr) -> boolean */
int builtin_ffi_nullp(Atom args, Atom *result)
{
	if (nilp(args)) {
		*result = make_sym("T");
		return Error_OK;
	}
	
	Atom a = car(args);
	if (a.type == AtomType_FFIPointer && a.value.ffi_pointer == NULL) {
		*result = make_sym("T");
	} else if (nilp(a)) {
		*result = make_sym("T");
	} else {
		*result = nil;
	}
	return Error_OK;
}

/* ============================================================
 * Struct Builtins
 * ============================================================ */

/* (%ffi-defstruct 'name '((field-name type) ...)) -> struct-type-symbol
 * Called by the ffi-defstruct macro which quotes the arguments.
 */
int builtin_ffi_defstruct(Atom args, Atom *result)
{
	if (nilp(args) || car(args).type != AtomType_Symbol) {
		return raise_error("type-error", "%ffi-defstruct requires a symbol name");
	}

	char *name = (char *)car(args).value.symbol;

	/* Second arg is the list of field specs */
	if (nilp(cdr(args))) {
		return raise_error("type-error", "%ffi-defstruct requires a field list");
	}
	Atom field_list = car(cdr(args));

	/* Check if already defined */
	if (ffi_lookup_struct_type(name)) {
		return raise_error("ffi-error", "Struct type already defined");
	}

	/* Allocate struct type */
	FFIStructType *st = calloc(1, sizeof(FFIStructType));
	st->name = strdup(name);

	/* Allocate ffi_type for this struct */
	ffi_type *ffi_struct = malloc(sizeof(ffi_type));
	memset(ffi_struct, 0, sizeof(ffi_type));
	st->ffi_struct = ffi_struct;

	/* Parse fields from the list */
	Atom fields = field_list;
	int i = 0;
	while (!nilp(fields) && i < FFI_STRUCT_MAX_FIELDS) {
		Atom field = car(fields);
		if (field.type != AtomType_Pair) {
			free(st->name);
			free(ffi_struct);
			free(st);
			return raise_error("syntax-error", "ffi-defstruct field must be (name type)");
		}

		Atom field_name = car(field);
		Atom field_type = car(cdr(field));

		if (field_name.type != AtomType_Symbol) {
			free(st->name);
			free(ffi_struct);
			free(st);
			return raise_error("type-error", "Field name must be a symbol");
		}
		if (field_type.type != AtomType_Keyword) {
			free(st->name);
			free(ffi_struct);
			free(st);
			return raise_error("type-error", "Field type must be a keyword");
		}

		st->fields[i].name = strdup(field_name.value.symbol);
		st->fields[i].type = keyword_to_type(field_type.value.keyword);
		st->elements[i] = type_to_ffi(st->fields[i].type);

		fields = cdr(fields);
		i++;
	}
	st->field_count = i;
	st->elements[i] = NULL;  /* Null-terminate for libffi */

	/* Initialize libffi struct type */
	ffi_struct->size = 0;       /* libffi calculates */
	ffi_struct->alignment = 0;  /* libffi calculates */
	ffi_struct->type = FFI_TYPE_STRUCT;
	ffi_struct->elements = (ffi_type **)st->elements;

	/* Force libffi to compute size/alignment by preparing a dummy cif */
	ffi_cif temp_cif;
	ffi_type *temp_args[] = { ffi_struct };
	ffi_prep_cif(&temp_cif, FFI_DEFAULT_ABI, 1, &ffi_type_void, temp_args);
	st->size = ffi_struct->size;

	/* Calculate field offsets */
	size_t offset = 0;
	for (int j = 0; j < st->field_count; j++) {
		ffi_type *elem = (ffi_type *)st->elements[j];
		size_t align = elem->alignment;
		offset = (offset + align - 1) & ~(align - 1);  /* Align */
		st->fields[j].offset = offset;
		offset += elem->size;
	}

	/* Add to registry */
	st->next = ffi_struct_registry;
	ffi_struct_registry = st;

	*result = car(args);  /* Return the struct name symbol */
	return Error_OK;
}

/* (make-ffi-struct type-name values...) -> ffi-struct */
int builtin_ffi_make_struct(Atom args, Atom *result)
{
	if (nilp(args) || car(args).type != AtomType_Symbol) {
		return raise_error("type-error", "make-ffi-struct requires a struct type name");
	}

	const char *name = car(args).value.symbol;
	FFIStructType *st = ffi_lookup_struct_type(name);
	if (!st) {
		return raise_error("ffi-error", "Unknown struct type");
	}

	/* Allocate struct instance */
	FFIStructInstance *inst = malloc(sizeof(FFIStructInstance));
	inst->type = st;
	inst->data = calloc(1, st->size);

	/* Pack field values */
	Atom values = cdr(args);
	for (int i = 0; i < st->field_count && !nilp(values); i++) {
		Atom v = car(values);
		void *field_ptr = (char *)inst->data + st->fields[i].offset;

		switch (st->fields[i].type) {
		case YAS_FFI_TYPE_INT8:
		case YAS_FFI_TYPE_UINT8:
			*(uint8_t *)field_ptr = (uint8_t)(v.type == AtomType_Integer ? v.value.integer : 0);
			break;
		case YAS_FFI_TYPE_INT16:
		case YAS_FFI_TYPE_UINT16:
			*(uint16_t *)field_ptr = (uint16_t)(v.type == AtomType_Integer ? v.value.integer : 0);
			break;
		case YAS_FFI_TYPE_INT32:
		case YAS_FFI_TYPE_UINT32:
			*(uint32_t *)field_ptr = (uint32_t)(v.type == AtomType_Integer ? v.value.integer : 0);
			break;
		case YAS_FFI_TYPE_INT64:
		case YAS_FFI_TYPE_UINT64:
			*(uint64_t *)field_ptr = (uint64_t)(v.type == AtomType_Integer ? v.value.integer : 0);
			break;
		case YAS_FFI_TYPE_FLOAT:
			*(float *)field_ptr = (float)(v.type == AtomType_Float ? v.value.floatval :
			                              (v.type == AtomType_Integer ? v.value.integer : 0));
			break;
		case YAS_FFI_TYPE_DOUBLE:
			*(double *)field_ptr = (double)(v.type == AtomType_Float ? v.value.floatval :
			                                (v.type == AtomType_Integer ? v.value.integer : 0));
			break;
		case YAS_FFI_TYPE_POINTER:
			if (v.type == AtomType_FFIPointer) {
				*(void **)field_ptr = v.value.ffi_pointer;
			} else {
				*(void **)field_ptr = NULL;
			}
			break;
		default:
			break;
		}
		values = cdr(values);
	}

	result->type = AtomType_FFIStruct;
	result->value.ffi_struct = inst;
	return Error_OK;
}

/* (ffi-struct-ref struct field-name) -> value */
int builtin_ffi_struct_ref(Atom args, Atom *result)
{
	if (nilp(args) || car(args).type != AtomType_FFIStruct) {
		return raise_error("type-error", "ffi-struct-ref requires an ffi-struct");
	}
	if (nilp(cdr(args)) || car(cdr(args)).type != AtomType_Symbol) {
		return raise_error("type-error", "ffi-struct-ref requires a field name symbol");
	}

	FFIStructInstance *inst = car(args).value.ffi_struct;
	if (!inst || !inst->type) {
		return raise_error("ffi-error", "Invalid struct instance");
	}

	const char *field_name = car(cdr(args)).value.symbol;
	FFIStructType *st = inst->type;

	/* Find field */
	int field_idx = -1;
	for (int i = 0; i < st->field_count; i++) {
		if (strcmp(st->fields[i].name, field_name) == 0) {
			field_idx = i;
			break;
		}
	}

	if (field_idx < 0) {
		return raise_error("ffi-error", "Unknown field name");
	}

	void *field_ptr = (char *)inst->data + st->fields[field_idx].offset;

	switch (st->fields[field_idx].type) {
	case YAS_FFI_TYPE_INT8:
		*result = make_int(*(int8_t *)field_ptr);
		break;
	case YAS_FFI_TYPE_UINT8:
		*result = make_int(*(uint8_t *)field_ptr);
		break;
	case YAS_FFI_TYPE_INT16:
		*result = make_int(*(int16_t *)field_ptr);
		break;
	case YAS_FFI_TYPE_UINT16:
		*result = make_int(*(uint16_t *)field_ptr);
		break;
	case YAS_FFI_TYPE_INT32:
		*result = make_int(*(int32_t *)field_ptr);
		break;
	case YAS_FFI_TYPE_UINT32:
		*result = make_int(*(uint32_t *)field_ptr);
		break;
	case YAS_FFI_TYPE_INT64:
	case YAS_FFI_TYPE_UINT64:
		*result = make_int(*(int64_t *)field_ptr);
		break;
	case YAS_FFI_TYPE_FLOAT:
		*result = make_float(*(float *)field_ptr);
		break;
	case YAS_FFI_TYPE_DOUBLE:
		*result = make_float(*(double *)field_ptr);
		break;
	case YAS_FFI_TYPE_POINTER:
		result->type = AtomType_FFIPointer;
		result->value.ffi_pointer = *(void **)field_ptr;
		break;
	default:
		*result = nil;
	}

	return Error_OK;
}

/* (ffi-struct-set! struct field-name value) -> value */
int builtin_ffi_struct_set(Atom args, Atom *result)
{
	if (nilp(args) || car(args).type != AtomType_FFIStruct) {
		return raise_error("type-error", "ffi-struct-set! requires an ffi-struct");
	}
	if (nilp(cdr(args)) || car(cdr(args)).type != AtomType_Symbol) {
		return raise_error("type-error", "ffi-struct-set! requires a field name symbol");
	}
	if (nilp(cdr(cdr(args)))) {
		return raise_error("args-error", "ffi-struct-set! requires a value");
	}

	FFIStructInstance *inst = car(args).value.ffi_struct;
	if (!inst || !inst->type) {
		return raise_error("ffi-error", "Invalid struct instance");
	}

	const char *field_name = car(cdr(args)).value.symbol;
	Atom value = car(cdr(cdr(args)));
	FFIStructType *st = inst->type;

	/* Find field */
	int field_idx = -1;
	for (int i = 0; i < st->field_count; i++) {
		if (strcmp(st->fields[i].name, field_name) == 0) {
			field_idx = i;
			break;
		}
	}

	if (field_idx < 0) {
		return raise_error("ffi-error", "Unknown field name");
	}

	void *field_ptr = (char *)inst->data + st->fields[field_idx].offset;

	switch (st->fields[field_idx].type) {
	case YAS_FFI_TYPE_INT8:
	case YAS_FFI_TYPE_UINT8:
		*(uint8_t *)field_ptr = (uint8_t)(value.type == AtomType_Integer ? value.value.integer : 0);
		break;
	case YAS_FFI_TYPE_INT16:
	case YAS_FFI_TYPE_UINT16:
		*(uint16_t *)field_ptr = (uint16_t)(value.type == AtomType_Integer ? value.value.integer : 0);
		break;
	case YAS_FFI_TYPE_INT32:
	case YAS_FFI_TYPE_UINT32:
		*(uint32_t *)field_ptr = (uint32_t)(value.type == AtomType_Integer ? value.value.integer : 0);
		break;
	case YAS_FFI_TYPE_INT64:
	case YAS_FFI_TYPE_UINT64:
		*(uint64_t *)field_ptr = (uint64_t)(value.type == AtomType_Integer ? value.value.integer : 0);
		break;
	case YAS_FFI_TYPE_FLOAT:
		*(float *)field_ptr = (float)(value.type == AtomType_Float ? value.value.floatval :
		                              (value.type == AtomType_Integer ? value.value.integer : 0));
		break;
	case YAS_FFI_TYPE_DOUBLE:
		*(double *)field_ptr = (double)(value.type == AtomType_Float ? value.value.floatval :
		                                (value.type == AtomType_Integer ? value.value.integer : 0));
		break;
	case YAS_FFI_TYPE_POINTER:
		if (value.type == AtomType_FFIPointer) {
			*(void **)field_ptr = value.value.ffi_pointer;
		} else {
			*(void **)field_ptr = NULL;
		}
		break;
	default:
		break;
	}

	*result = value;
	return Error_OK;
}

#endif /* YAS_FFI */

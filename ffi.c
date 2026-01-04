/*
 * ffi.c - Foreign Function Interface for Gold Scheme
 * 
 * Provides bindings to libffi for calling C functions from Scheme.
 * This file is only compiled when GOLD_FFI is defined.
 */

#ifdef GOLD_FFI

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

/* Map keyword name to GoldFFIType */
static GoldFFIType keyword_to_type(const char *name)
{
	if (strcmp(name, "VOID") == 0) return GOLD_FFI_TYPE_VOID;
	if (strcmp(name, "INT8") == 0) return GOLD_FFI_TYPE_INT8;
	if (strcmp(name, "UINT8") == 0) return GOLD_FFI_TYPE_UINT8;
	if (strcmp(name, "INT16") == 0) return GOLD_FFI_TYPE_INT16;
	if (strcmp(name, "UINT16") == 0) return GOLD_FFI_TYPE_UINT16;
	if (strcmp(name, "INT32") == 0 || strcmp(name, "INT") == 0) return GOLD_FFI_TYPE_INT32;
	if (strcmp(name, "UINT32") == 0 || strcmp(name, "UINT") == 0) return GOLD_FFI_TYPE_UINT32;
	if (strcmp(name, "INT64") == 0 || strcmp(name, "LONG") == 0) return GOLD_FFI_TYPE_INT64;
	if (strcmp(name, "UINT64") == 0 || strcmp(name, "ULONG") == 0) return GOLD_FFI_TYPE_UINT64;
	if (strcmp(name, "FLOAT") == 0) return GOLD_FFI_TYPE_FLOAT;
	if (strcmp(name, "DOUBLE") == 0) return GOLD_FFI_TYPE_DOUBLE;
	if (strcmp(name, "POINTER") == 0 || strcmp(name, "PTR") == 0) return GOLD_FFI_TYPE_POINTER;
	if (strcmp(name, "STRING") == 0 || strcmp(name, "CSTRING") == 0) return GOLD_FFI_TYPE_STRING;
	return GOLD_FFI_TYPE_VOID; /* Default */
}

/* Map GoldFFIType to libffi type */
static ffi_type *type_to_ffi(GoldFFIType t)
{
	switch (t) {
	case GOLD_FFI_TYPE_VOID:    return &ffi_type_void;
	case GOLD_FFI_TYPE_INT8:    return &ffi_type_sint8;
	case GOLD_FFI_TYPE_UINT8:   return &ffi_type_uint8;
	case GOLD_FFI_TYPE_INT16:   return &ffi_type_sint16;
	case GOLD_FFI_TYPE_UINT16:  return &ffi_type_uint16;
	case GOLD_FFI_TYPE_INT32:   return &ffi_type_sint32;
	case GOLD_FFI_TYPE_UINT32:  return &ffi_type_uint32;
	case GOLD_FFI_TYPE_INT64:   return &ffi_type_sint64;
	case GOLD_FFI_TYPE_UINT64:  return &ffi_type_uint64;
	case GOLD_FFI_TYPE_FLOAT:   return &ffi_type_float;
	case GOLD_FFI_TYPE_DOUBLE:  return &ffi_type_double;
	case GOLD_FFI_TYPE_POINTER:
	case GOLD_FFI_TYPE_STRING:  return &ffi_type_pointer;
	}
	return &ffi_type_void;
}

/* ============================================================
 * Function Preparation
 * ============================================================ */

struct FFIFunction *ffi_prepare(void *fn_ptr, Atom ret_type, Atom arg_types)
{
	struct FFIFunction *fn = malloc(sizeof(struct FFIFunction));
	fn->fn_ptr = fn_ptr;
	
	/* Parse return type (keyword) */
	fn->ret_type = GOLD_FFI_TYPE_VOID;
	if (ret_type.type == AtomType_Keyword) {
		fn->ret_type = keyword_to_type(ret_type.value.keyword);
	}
	
	/* Count and parse argument types (list of keywords) */
	fn->arg_count = 0;
	Atom p = arg_types;
	while (!nilp(p)) {
		fn->arg_count++;
		p = cdr(p);
	}
	
	fn->arg_types = NULL;
	if (fn->arg_count > 0) {
		fn->arg_types = malloc(sizeof(GoldFFIType) * fn->arg_count);
		p = arg_types;
		for (int i = 0; i < fn->arg_count; i++) {
			Atom t = car(p);
			if (t.type == AtomType_Keyword) {
				fn->arg_types[i] = keyword_to_type(t.value.keyword);
			} else {
				fn->arg_types[i] = GOLD_FFI_TYPE_VOID;
			}
			p = cdr(p);
		}
	}
	
	/* Prepare ffi_cif */
	ffi_cif *cif = malloc(sizeof(ffi_cif));
	ffi_type **ffi_arg_types = NULL;
	
	if (fn->arg_count > 0) {
		ffi_arg_types = malloc(sizeof(ffi_type *) * fn->arg_count);
		for (int i = 0; i < fn->arg_count; i++) {
			ffi_arg_types[i] = type_to_ffi(fn->arg_types[i]);
		}
	}
	
	ffi_status status = ffi_prep_cif(cif, FFI_DEFAULT_ABI, 
	                                  fn->arg_count, 
	                                  type_to_ffi(fn->ret_type),
	                                  ffi_arg_types);
	
	if (status != FFI_OK) {
		free(cif);
		if (ffi_arg_types) free(ffi_arg_types);
		free(fn->arg_types);
		free(fn);
		return NULL;
	}
	
	fn->cif = cif;
	/* Note: ffi_arg_types is now owned by cif in some sense, but we keep it */
	
	return fn;
}

void ffi_free_function(struct FFIFunction *fn)
{
	if (!fn) return;
	if (fn->cif) free(fn->cif);
	if (fn->arg_types) free(fn->arg_types);
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
			case GOLD_FFI_TYPE_INT8:
			case GOLD_FFI_TYPE_UINT8:
				*(int8_t *)storage = (int8_t)(v.type == AtomType_Integer ? v.value.integer : 0);
				break;
			case GOLD_FFI_TYPE_INT16:
			case GOLD_FFI_TYPE_UINT16:
				*(int16_t *)storage = (int16_t)(v.type == AtomType_Integer ? v.value.integer : 0);
				break;
			case GOLD_FFI_TYPE_INT32:
			case GOLD_FFI_TYPE_UINT32:
				*(int32_t *)storage = (int32_t)(v.type == AtomType_Integer ? v.value.integer : 0);
				break;
			case GOLD_FFI_TYPE_INT64:
			case GOLD_FFI_TYPE_UINT64:
				*(int64_t *)storage = (int64_t)(v.type == AtomType_Integer ? v.value.integer : 0);
				break;
			case GOLD_FFI_TYPE_FLOAT:
				*(float *)storage = (float)(v.type == AtomType_Float ? v.value.floatval : 
				                            (v.type == AtomType_Integer ? v.value.integer : 0));
				break;
			case GOLD_FFI_TYPE_DOUBLE:
				*(double *)storage = (double)(v.type == AtomType_Float ? v.value.floatval :
				                              (v.type == AtomType_Integer ? v.value.integer : 0));
				break;
			case GOLD_FFI_TYPE_STRING:
				if (v.type == AtomType_String) {
					string_temps[i] = strdup(v.value.string);
					*(char **)storage = string_temps[i];
				} else {
					*(char **)storage = NULL;
				}
				break;
			case GOLD_FFI_TYPE_POINTER:
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
	case GOLD_FFI_TYPE_VOID:
		result = nil;
		break;
	case GOLD_FFI_TYPE_INT8:
	case GOLD_FFI_TYPE_UINT8:
		result = make_int(ret_value.i8);
		break;
	case GOLD_FFI_TYPE_INT16:
	case GOLD_FFI_TYPE_UINT16:
		result = make_int(ret_value.i16);
		break;
	case GOLD_FFI_TYPE_INT32:
	case GOLD_FFI_TYPE_UINT32:
		result = make_int(ret_value.i32);
		break;
	case GOLD_FFI_TYPE_INT64:
	case GOLD_FFI_TYPE_UINT64:
		result = make_int(ret_value.i64);
		break;
	case GOLD_FFI_TYPE_FLOAT:
		result = make_float(ret_value.f);
		break;
	case GOLD_FFI_TYPE_DOUBLE:
		result = make_float(ret_value.d);
		break;
	case GOLD_FFI_TYPE_POINTER:
		result.type = AtomType_FFIPointer;
		result.value.ffi_pointer = ret_value.ptr;
		break;
	case GOLD_FFI_TYPE_STRING:
		if (ret_value.ptr) {
			result = make_string((char *)ret_value.ptr);
		} else {
			result = nil;
		}
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
	if (nilp(cdr(args)) || car(cdr(args)).type != AtomType_Keyword) {
		return raise_error("type-error", "ffi-define requires a return type keyword");
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
	GoldFFIType type = keyword_to_type(car(cdr(args)).value.keyword);
	size_t offset = (size_t)car(cdr(cdr(args))).value.integer;
	
	char *ptr = (char *)base + offset;
	
	switch (type) {
	case GOLD_FFI_TYPE_INT8:
		*result = make_int(*(int8_t *)ptr);
		break;
	case GOLD_FFI_TYPE_UINT8:
		*result = make_int(*(uint8_t *)ptr);
		break;
	case GOLD_FFI_TYPE_INT16:
		*result = make_int(*(int16_t *)ptr);
		break;
	case GOLD_FFI_TYPE_UINT16:
		*result = make_int(*(uint16_t *)ptr);
		break;
	case GOLD_FFI_TYPE_INT32:
		*result = make_int(*(int32_t *)ptr);
		break;
	case GOLD_FFI_TYPE_UINT32:
		*result = make_int(*(uint32_t *)ptr);
		break;
	case GOLD_FFI_TYPE_INT64:
		*result = make_int(*(int64_t *)ptr);
		break;
	case GOLD_FFI_TYPE_UINT64:
		*result = make_int((long)*(uint64_t *)ptr);
		break;
	case GOLD_FFI_TYPE_FLOAT:
		*result = make_float(*(float *)ptr);
		break;
	case GOLD_FFI_TYPE_DOUBLE:
		*result = make_float(*(double *)ptr);
		break;
	case GOLD_FFI_TYPE_POINTER:
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
	GoldFFIType type = keyword_to_type(car(cdr(args)).value.keyword);
	size_t offset = (size_t)car(cdr(cdr(args))).value.integer;
	Atom value = car(cdr(cdr(cdr(args))));
	
	char *ptr = (char *)base + offset;
	
	switch (type) {
	case GOLD_FFI_TYPE_INT8:
	case GOLD_FFI_TYPE_UINT8:
		*(int8_t *)ptr = (int8_t)(value.type == AtomType_Integer ? value.value.integer : 0);
		break;
	case GOLD_FFI_TYPE_INT16:
	case GOLD_FFI_TYPE_UINT16:
		*(int16_t *)ptr = (int16_t)(value.type == AtomType_Integer ? value.value.integer : 0);
		break;
	case GOLD_FFI_TYPE_INT32:
	case GOLD_FFI_TYPE_UINT32:
		*(int32_t *)ptr = (int32_t)(value.type == AtomType_Integer ? value.value.integer : 0);
		break;
	case GOLD_FFI_TYPE_INT64:
	case GOLD_FFI_TYPE_UINT64:
		*(int64_t *)ptr = (int64_t)(value.type == AtomType_Integer ? value.value.integer : 0);
		break;
	case GOLD_FFI_TYPE_FLOAT:
		*(float *)ptr = (float)(value.type == AtomType_Float ? value.value.floatval :
		                        (value.type == AtomType_Integer ? value.value.integer : 0));
		break;
	case GOLD_FFI_TYPE_DOUBLE:
		*(double *)ptr = (double)(value.type == AtomType_Float ? value.value.floatval :
		                          (value.type == AtomType_Integer ? value.value.integer : 0));
		break;
	case GOLD_FFI_TYPE_POINTER:
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

#endif /* GOLD_FFI */

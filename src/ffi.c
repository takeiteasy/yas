/*
 * ffi.c - Foreign Function Interface for YAS Scheme
 *
 * Provides bindings to libffi for calling C functions from Scheme.
 * This file is only compiled when YAS_FFI is defined.
 */

#ifdef YAS_FFI

#include "yas.h"
#include <ffi.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ============================================================
 * Library Loading
 * ============================================================ */

static void * yas_ffi_load_library_impl(const char *path)
{
    return dlopen(path, RTLD_LAZY);
}

void * yas_ffi_load_library(const char *path)
{
    void * r = yas_ffi_load_library_impl(path);
    return r;
}


static void yas_ffi_close_library_impl(void *handle)
{
    if (handle)
        dlclose(handle);
}

void yas_ffi_close_library(void *handle)
{
    yas_ffi_close_library_impl(handle);
}


static void * yas_ffi_get_symbol_impl(void *handle, const char *name)
{
    return dlsym(handle, name);
}

void * yas_ffi_get_symbol(void *handle, const char *name)
{
    void * r = yas_ffi_get_symbol_impl(handle, name);
    return r;
}


/* ============================================================
 * Type Mapping
 * ============================================================ */

/* Map keyword name to YasFFIType */
static YasFFIType keyword_to_type(const char *name)
{
    if (strcmp(name, "VOID") == 0)
        return YAS_FFI_TYPE_VOID;
    if (strcmp(name, "INT8") == 0)
        return YAS_FFI_TYPE_INT8;
    if (strcmp(name, "UINT8") == 0)
        return YAS_FFI_TYPE_UINT8;
    if (strcmp(name, "INT16") == 0)
        return YAS_FFI_TYPE_INT16;
    if (strcmp(name, "UINT16") == 0)
        return YAS_FFI_TYPE_UINT16;
    if (strcmp(name, "INT32") == 0 || strcmp(name, "INT") == 0)
        return YAS_FFI_TYPE_INT32;
    if (strcmp(name, "UINT32") == 0 || strcmp(name, "UINT") == 0)
        return YAS_FFI_TYPE_UINT32;
    if (strcmp(name, "INT64") == 0 || strcmp(name, "LONG") == 0)
        return YAS_FFI_TYPE_INT64;
    if (strcmp(name, "UINT64") == 0 || strcmp(name, "ULONG") == 0)
        return YAS_FFI_TYPE_UINT64;
    if (strcmp(name, "FLOAT") == 0)
        return YAS_FFI_TYPE_FLOAT;
    if (strcmp(name, "DOUBLE") == 0)
        return YAS_FFI_TYPE_DOUBLE;
    if (strcmp(name, "POINTER") == 0 || strcmp(name, "PTR") == 0)
        return YAS_FFI_TYPE_POINTER;
    if (strcmp(name, "STRING") == 0 || strcmp(name, "CSTRING") == 0)
        return YAS_FFI_TYPE_STRING;
    return YAS_FFI_TYPE_VOID; /* Default */
}

/* Map YasFFIType to libffi type */
static ffi_type *type_to_ffi(YasFFIType t)
{
    switch (t) {
        case YAS_FFI_TYPE_VOID:
            return &ffi_type_void;
        case YAS_FFI_TYPE_INT8:
            return &ffi_type_sint8;
        case YAS_FFI_TYPE_UINT8:
            return &ffi_type_uint8;
        case YAS_FFI_TYPE_INT16:
            return &ffi_type_sint16;
        case YAS_FFI_TYPE_UINT16:
            return &ffi_type_uint16;
        case YAS_FFI_TYPE_INT32:
            return &ffi_type_sint32;
        case YAS_FFI_TYPE_UINT32:
            return &ffi_type_uint32;
        case YAS_FFI_TYPE_INT64:
            return &ffi_type_sint64;
        case YAS_FFI_TYPE_UINT64:
            return &ffi_type_uint64;
        case YAS_FFI_TYPE_FLOAT:
            return &ffi_type_float;
        case YAS_FFI_TYPE_DOUBLE:
            return &ffi_type_double;
        case YAS_FFI_TYPE_POINTER:
        case YAS_FFI_TYPE_STRING:
            return &ffi_type_pointer;
        case YAS_FFI_TYPE_STRUCT:
            return &ffi_type_pointer; /* Handled specially */
    }
    return &ffi_type_void;
}

/* ============================================================
 * Struct Type Registry
 * ============================================================ */

/* Global struct type registry */

/* Look up a struct type by name */
static YasFFIStructType * yas_ffi_lookup_struct_type_impl(const char *name)
{
    YasFFIStructType *st = yas_ffi_struct_registry;
    while (st) {
        if (strcmp(st->name, name) == 0) {
            return st;
        }
        st = st->next;
    }
    return NULL;
}

YasFFIStructType * yas_ffi_lookup_struct_type(const char *name)
{
    YasFFIStructType * r = yas_ffi_lookup_struct_type_impl(name);
    return r;
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
    YasFFIStructType *st = yas_ffi_lookup_struct_type(name);
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

static struct YasFFIFunction * yas_ffi_prepare_impl(void *fn_ptr, YasAtom library, YasAtom ret_type, YasAtom arg_types,
                                                    const char *name)
{
    /* GC-tracked (data.c) like every other heap object - already linked
     * into the unified GC list before this function returns, so on the
     * failure path below we must NOT free() it or its fields by hand:
     * they're left for a later sweep to collect via gc_free_object(),
     * same as if this object had simply become unreachable. */
    struct YasFFIFunction *fn = yas_gc_alloc_ffi_function();
    fn->fn_ptr = fn_ptr;
    /* Keep our own copy of the C symbol name: the caller's pointer
     * usually points into a GC-managed yas string that can die while
     * the function lives. strdup() is plain malloc - no stepping. */
    fn->name = name ? strdup(name) : NULL;
    /* Retain the owning library (nil when prepared from a bare pointer):
     * storing it immediately after allocation - before any further
     * allocating call - means no GC step can observe the field unset.
     * The write barrier keeps the tricolor invariant if a cycle is
     * mid-flight (fresh objects are BLACK mid-cycle, but the stored
     * library atom may still be WHITE), same as yas_cons() et al. */
    fn->library = library;
    {
        YasAtom self;
        self.type = YAS_ATOM_TYPE_FFI_FUNCTION;
        self.value.ffi_fn = fn;
        yas_gc_write_barrier(self, library);
    }

    /* Parse return type (keyword or symbol for structs) */
    fn->ret_type = YAS_FFI_TYPE_VOID;
    fn->ffi_ret_type = &ffi_type_void;
    if (ret_type.type == YAS_ATOM_TYPE_KEYWORD) {
        fn->ffi_ret_type = resolve_type(ret_type.value.keyword, &fn->ret_type);
    } else if (ret_type.type == YAS_ATOM_TYPE_SYMBOL) {
        fn->ffi_ret_type = resolve_type(ret_type.value.symbol, &fn->ret_type);
    }

    /* Count argument types */
    fn->arg_count = 0;
    YasAtom p = arg_types;
    while (!yas_nilp(p)) {
        fn->arg_count++;
        p = yas_cdr(p);
    }

    /* Parse argument types (keywords or symbols for structs) */
    fn->arg_types = NULL;
    fn->ffi_arg_types = NULL;
    if (fn->arg_count > 0) {
        fn->arg_types = malloc(sizeof(YasFFIType) * fn->arg_count);
        fn->ffi_arg_types = malloc(sizeof(ffi_type *) * fn->arg_count);
        p = arg_types;
        for (int i = 0; i < fn->arg_count; i++) {
            YasAtom t = yas_car(p);
            if (t.type == YAS_ATOM_TYPE_KEYWORD) {
                fn->ffi_arg_types[i] = resolve_type(t.value.keyword, &fn->arg_types[i]);
            } else if (t.type == YAS_ATOM_TYPE_SYMBOL) {
                fn->ffi_arg_types[i] = resolve_type(t.value.symbol, &fn->arg_types[i]);
            } else {
                fn->arg_types[i] = YAS_FFI_TYPE_VOID;
                fn->ffi_arg_types[i] = &ffi_type_void;
            }
            p = yas_cdr(p);
        }
    }

    /* Prepare ffi_cif */
    ffi_cif *cif = malloc(sizeof(ffi_cif));

    ffi_status status =
        ffi_prep_cif(cif, FFI_DEFAULT_ABI, fn->arg_count, (ffi_type *)fn->ffi_ret_type, (ffi_type **)fn->ffi_arg_types);

    if (status != FFI_OK) {
        /* Leave fn (and cif) for the GC to reclaim - see the comment
         * where fn was allocated above. */
        free(cif);
        return NULL;
    }

    fn->cif = cif;

    return fn;
}

struct YasFFIFunction * yas_ffi_prepare(void *fn_ptr, YasAtom library, YasAtom ret_type, YasAtom arg_types,
                                        const char *name)
{
    struct YasFFIFunction * r = yas_ffi_prepare_impl(fn_ptr, library, ret_type, arg_types, name);
    return r;
}


/* NOTE: `fn` is GC-tracked (linked into data.c's unified gc_all list at
 * yas_gc_alloc_ffi_function() time). This function is no longer called
 * anywhere internally - data.c's gc_free_object() does this same field
 * cleanup directly when the sweeper reaches an unreachable FFIFunction.
 * Do NOT call this on a still-linked object: it frees the fields but
 * doesn't unlink `fn` from gc_all, so a later sweep would free() it (and
 * its already-freed fields) again. Kept for API compatibility. */
static void yas_ffi_free_function_impl(struct YasFFIFunction *fn)
{
    if (!fn)
        return;
    if (fn->cif)
        free(fn->cif);
    if (fn->arg_types)
        free(fn->arg_types);
    if (fn->ffi_arg_types)
        free(fn->ffi_arg_types);
    if (fn->name)
        free((void *)fn->name);
    free(fn);
}

void yas_ffi_free_function(struct YasFFIFunction *fn)
{
    yas_ffi_free_function_impl(fn);
}


/* ============================================================
 * Function Calling
 * ============================================================ */

static int yas_ffi_call_function_impl(struct YasFFIFunction *fn, YasAtom args, YasAtom *result)
{
    if (!fn || !fn->cif) {
        *result = yas_nil;
        return YAS_ERROR_OK;
    }

    /* Exact arity: the ffi_cif was prepared for exactly arg_count
     * arguments, so anything else is either silently-zero marshaling
     * (too few) or unmarshalable extras. Counted here - not in the
     * ffi-call builtin - so direct application (the evaluator routes
     * through this same path) gets the identical check. */
    size_t argc = 0;
    {
        YasAtom p = args;
        while (p.type == YAS_ATOM_TYPE_PAIR) {
            argc++;
            p = yas_cdr(p);
        }
        if (!yas_nilp(p)) {
            return yas_raise_error("args-error", "foreign call: argument list must be proper");
        }
    }
    if (argc != (size_t)fn->arg_count) {
        char msg[192];
        snprintf(msg, sizeof(msg),
                 "%s expects %d argument%s, got %zu",
                 fn->name ? fn->name : "foreign function",
                 fn->arg_count,
                 fn->arg_count == 1 ? "" : "s",
                 argc);
        return yas_raise_error("args-error", msg);
    }

    ffi_cif *cif = (ffi_cif *)fn->cif;

    /* Prepare argument values */
    void **arg_values = NULL;
    void *arg_storage = NULL;   /* Storage for actual values */
    char **string_temps = NULL; /* Temporary strings to free */

    if (fn->arg_count > 0) {
        arg_values = malloc(sizeof(void *) * fn->arg_count);
        /* 8 bytes per arg should be enough for any type */
        arg_storage = malloc(8 * fn->arg_count);
        string_temps = calloc(fn->arg_count, sizeof(char *));

        YasAtom p = args;
        for (int i = 0; i < fn->arg_count && !yas_nilp(p); i++) {
            YasAtom v = yas_car(p);
            void *storage = (char *)arg_storage + (i * 8);

            switch (fn->arg_types[i]) {
                case YAS_FFI_TYPE_INT8:
                case YAS_FFI_TYPE_UINT8:
                    *(int8_t *)storage = (int8_t)(v.type == YAS_ATOM_TYPE_INTEGER ? v.value.integer : 0);
                    break;
                case YAS_FFI_TYPE_INT16:
                case YAS_FFI_TYPE_UINT16:
                    *(int16_t *)storage = (int16_t)(v.type == YAS_ATOM_TYPE_INTEGER ? v.value.integer : 0);
                    break;
                case YAS_FFI_TYPE_INT32:
                case YAS_FFI_TYPE_UINT32:
                    *(int32_t *)storage = (int32_t)(v.type == YAS_ATOM_TYPE_INTEGER ? v.value.integer : 0);
                    break;
                case YAS_FFI_TYPE_INT64:
                case YAS_FFI_TYPE_UINT64:
                    *(int64_t *)storage = (int64_t)(v.type == YAS_ATOM_TYPE_INTEGER ? v.value.integer : 0);
                    break;
                case YAS_FFI_TYPE_FLOAT:
                    *(float *)storage = (float)(v.type == YAS_ATOM_TYPE_FLOAT
                                                    ? v.value.floatval
                                                    : (v.type == YAS_ATOM_TYPE_INTEGER ? v.value.integer : 0));
                    break;
                case YAS_FFI_TYPE_DOUBLE:
                    *(double *)storage = (double)(v.type == YAS_ATOM_TYPE_FLOAT
                                                      ? v.value.floatval
                                                      : (v.type == YAS_ATOM_TYPE_INTEGER ? v.value.integer : 0));
                    break;
                case YAS_FFI_TYPE_STRING:
                    if (v.type == YAS_ATOM_TYPE_STRING) {
                        string_temps[i] = strdup(v.value.string);
                        *(char **)storage = string_temps[i];
                    } else {
                        *(char **)storage = NULL;
                    }
                    break;
                case YAS_FFI_TYPE_POINTER:
                    if (v.type == YAS_ATOM_TYPE_FFI_POINTER) {
                        *(void **)storage = v.value.ffi_pointer;
                    } else if (v.type == YAS_ATOM_TYPE_STRING) {
                        /* Allow strings as pointers for convenience */
                        string_temps[i] = strdup(v.value.string);
                        *(void **)storage = string_temps[i];
                    } else if (yas_nilp(v)) {
                        *(void **)storage = NULL;
                    } else {
                        *(void **)storage = NULL;
                    }
                    break;
                case YAS_FFI_TYPE_STRUCT:
                    /* For struct-by-value, libffi expects pointer to struct data */
                    if (v.type == YAS_ATOM_TYPE_FFI_STRUCT && v.value.ffi_struct) {
                        /* Point directly to struct data - libffi copies it */
                        arg_values[i] = v.value.ffi_struct->data;
                    } else {
                        *(void **)storage = NULL;
                        arg_values[i] = storage;
                    }
                    p = yas_cdr(p);
                    continue;
                default:
                    *(void **)storage = NULL;
            }

            arg_values[i] = storage;
            p = yas_cdr(p);
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
            if (string_temps[i])
                free(string_temps[i]);
        }
        free(string_temps);
    }
    if (arg_values)
        free(arg_values);
    if (arg_storage)
        free(arg_storage);

    /* Convert return value to Atom */
    switch (fn->ret_type) {
        case YAS_FFI_TYPE_VOID:
            *result = yas_nil;
            break;
        case YAS_FFI_TYPE_INT8:
        case YAS_FFI_TYPE_UINT8:
            *result = yas_make_int(ret_value.i8);
            break;
        case YAS_FFI_TYPE_INT16:
        case YAS_FFI_TYPE_UINT16:
            *result = yas_make_int(ret_value.i16);
            break;
        case YAS_FFI_TYPE_INT32:
        case YAS_FFI_TYPE_UINT32:
            *result = yas_make_int(ret_value.i32);
            break;
        case YAS_FFI_TYPE_INT64:
        case YAS_FFI_TYPE_UINT64:
            *result = yas_make_int(ret_value.i64);
            break;
        case YAS_FFI_TYPE_FLOAT:
            *result = yas_make_float(ret_value.f);
            break;
        case YAS_FFI_TYPE_DOUBLE:
            *result = yas_make_float(ret_value.d);
            break;
        case YAS_FFI_TYPE_POINTER:
            result->type = YAS_ATOM_TYPE_FFI_POINTER;
            result->value.ffi_pointer = ret_value.ptr;
            break;
        case YAS_FFI_TYPE_STRING:
            if (ret_value.ptr) {
                *result = yas_make_string((char *)ret_value.ptr);
            } else {
                *result = yas_nil;
            }
            break;
        case YAS_FFI_TYPE_STRUCT:
            /* Struct by-value return not supported; fail like the nil guard above */
            *result = yas_nil;
            break;
    }

    return YAS_ERROR_OK;
}

int yas_ffi_call_function(struct YasFFIFunction *fn, YasAtom args, YasAtom *result)
{
    return yas_ffi_call_function_impl(fn, args, result);
}


/* ============================================================
 * Builtin Functions
 * ============================================================ */

/* (ffi-open path) -> ffi-library */
static int yas_builtin_ffi_open_impl(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || yas_car(args).type != YAS_ATOM_TYPE_STRING) {
        return yas_raise_error("type-error", "ffi-open requires a string path");
    }

    const char *path = yas_car(args).value.string;
    void *handle = yas_ffi_load_library(path);

    if (!handle) {
        return yas_raise_error("ffi-error", dlerror());
    }

    /* GC-tracked wrapper (data.c): the sweeper dlcloses the handle once
     * no live FFIFunction traces this library and no reference to it
     * remains - see gc_free_object()'s YAS_GC_OBJ_FFI_LIBRARY arm. */
    YasFFILibrary *lib = yas_gc_alloc_ffi_library();
    lib->handle = handle;

    result->type = YAS_ATOM_TYPE_FFI_LIBRARY;
    result->value.ffi_library = lib;
    return YAS_ERROR_OK;
}

int yas_builtin_ffi_open(YasAtom args, YasAtom *result)
{
    int r = yas_builtin_ffi_open_impl(args, result);
    return r;
}


/* (ffi-close lib) -> #t. Explicit early close - otherwise the GC reclaims
 * an unreachable library on its own (see yas_builtin_ffi_open_impl). */
static int yas_builtin_ffi_close_impl(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || yas_car(args).type != YAS_ATOM_TYPE_FFI_LIBRARY) {
        return yas_raise_error("type-error", "ffi-close requires an ffi-library");
    }

    YasFFILibrary *lib = yas_car(args).value.ffi_library;
    if (!lib->handle) {
        return yas_raise_error("ffi-error", "ffi-close: library already closed");
    }

    yas_ffi_close_library(lib->handle);
    /* NULL the handle so the wrapper's eventual sweep is a no-op rather
     * than a double-dlclose (mirrors gc_free_object()'s library arm). */
    lib->handle = NULL;

    *result = yas_make_sym("T");
    return YAS_ERROR_OK;
}

int yas_builtin_ffi_close(YasAtom args, YasAtom *result)
{
    int r = yas_builtin_ffi_close_impl(args, result);
    return r;
}


/* (ffi-lookup lib name) -> ffi-pointer */
static int yas_builtin_ffi_lookup_impl(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || yas_car(args).type != YAS_ATOM_TYPE_FFI_LIBRARY) {
        return yas_raise_error("type-error", "ffi-lookup requires an ffi-library");
    }
    if (yas_nilp(yas_cdr(args)) || yas_car(yas_cdr(args)).type != YAS_ATOM_TYPE_STRING) {
        return yas_raise_error("type-error", "ffi-lookup requires a string name");
    }

    YasFFILibrary *lib = yas_car(args).value.ffi_library;
    if (!lib->handle) {
        return yas_raise_error("ffi-error", "ffi-lookup: library already closed");
    }
    const char *name = yas_car(yas_cdr(args)).value.string;

    void *sym = yas_ffi_get_symbol(lib->handle, name);
    if (!sym) {
        return yas_raise_error("ffi-error", dlerror());
    }

    result->type = YAS_ATOM_TYPE_FFI_POINTER;
    result->value.ffi_pointer = sym;
    return YAS_ERROR_OK;
}

int yas_builtin_ffi_lookup(YasAtom args, YasAtom *result)
{
    int r = yas_builtin_ffi_lookup_impl(args, result);
    return r;
}


/* (ffi-define target ret-type (arg-types...)) -> ffi-function
 *
 * Two forms, dispatched on the first argument's atom type:
 *   (ffi-define lib "c-name" ret-type (arg-types...))
 *     lib is an ffi-library; c-name is looked up in it and the prepared
 *     function RETAINS the library atom (struct YasFFIFunction.library,
 *     traced by the GC) so the library can never be dlclosed out from
 *     under a still-live function. The arg-types list is literal data -
 *     quote it at call sites (the define-foreign macro does).
 *   (ffi-define ptr ret-type (arg-types...))
 *     ptr is a bare ffi-pointer (e.g. a function pointer read out of a
 *     struct field via ffi-pointer-ref); nothing is retained. */
static int yas_builtin_ffi_define_impl(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args)) {
        return yas_raise_error("type-error", "ffi-define requires an ffi-library or ffi-pointer");
    }

    YasAtom first = yas_car(args);
    void *fn_ptr;
    YasAtom library = yas_nil;
    const char *c_name = NULL; /* Symbol name, when extracted from a library */
    int ret_index;

    if (first.type == YAS_ATOM_TYPE_FFI_LIBRARY) {
        YasFFILibrary *lib = first.value.ffi_library;
        if (!lib->handle) {
            return yas_raise_error("ffi-error", "ffi-define: library already closed");
        }
        if (yas_nilp(yas_cdr(args)) || yas_car(yas_cdr(args)).type != YAS_ATOM_TYPE_STRING) {
            return yas_raise_error("type-error",
                                   "ffi-define with an ffi-library requires a string symbol name");
        }

        c_name = yas_car(yas_cdr(args)).value.string;
        fn_ptr = yas_ffi_get_symbol(lib->handle, c_name);
        if (!fn_ptr) {
            return yas_raise_error("ffi-error", dlerror());
        }

        library = first;
        ret_index = 2;
    } else if (first.type == YAS_ATOM_TYPE_FFI_POINTER) {
        fn_ptr = first.value.ffi_pointer;
        ret_index = 1;
    } else {
        return yas_raise_error("type-error", "ffi-define requires an ffi-library or ffi-pointer");
    }

    /* Walk to the return-type argument (position differs per form). */
    YasAtom rest = args;
    for (int i = 0; i < ret_index; i++)
        rest = yas_cdr(rest);
    if (yas_nilp(rest) ||
        (yas_car(rest).type != YAS_ATOM_TYPE_KEYWORD && yas_car(rest).type != YAS_ATOM_TYPE_SYMBOL)) {
        return yas_raise_error("type-error", "ffi-define requires a return type keyword or struct name");
    }

    YasAtom ret_type = yas_car(rest);
    YasAtom tail = yas_cdr(rest);
    YasAtom arg_types = yas_nilp(tail) ? yas_nil : yas_car(tail);

    struct YasFFIFunction *fn = yas_ffi_prepare(fn_ptr, library, ret_type, arg_types, c_name);
    if (!fn) {
        return yas_raise_error("ffi-error", "Failed to prepare FFI function");
    }

    result->type = YAS_ATOM_TYPE_FFI_FUNCTION;
    result->value.ffi_fn = fn;
    return YAS_ERROR_OK;
}

int yas_builtin_ffi_define(YasAtom args, YasAtom *result)
{
    int r = yas_builtin_ffi_define_impl(args, result);
    return r;
}


/* (ffi-call fn args...) -> result */
static int yas_builtin_ffi_call_impl(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || yas_car(args).type != YAS_ATOM_TYPE_FFI_FUNCTION) {
        return yas_raise_error("type-error", "ffi-call requires an ffi-function");
    }

    struct YasFFIFunction *fn = yas_car(args).value.ffi_fn;
    return yas_ffi_call_function(fn, yas_cdr(args), result);
}

int yas_builtin_ffi_call(YasAtom args, YasAtom *result)
{
    int r = yas_builtin_ffi_call_impl(args, result);
    return r;
}


/* (ffi-malloc size) -> ffi-pointer */
static int yas_builtin_ffi_malloc_impl(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || yas_car(args).type != YAS_ATOM_TYPE_INTEGER) {
        return yas_raise_error("type-error", "ffi-malloc requires an integer size");
    }

    size_t size = (size_t)yas_car(args).value.integer;
    void *ptr = malloc(size);

    result->type = YAS_ATOM_TYPE_FFI_POINTER;
    result->value.ffi_pointer = ptr;
    return YAS_ERROR_OK;
}

int yas_builtin_ffi_malloc(YasAtom args, YasAtom *result)
{
    int r = yas_builtin_ffi_malloc_impl(args, result);
    return r;
}


/* (ffi-free ptr) -> #t */
static int yas_builtin_ffi_free_impl(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || yas_car(args).type != YAS_ATOM_TYPE_FFI_POINTER) {
        return yas_raise_error("type-error", "ffi-free requires an ffi-pointer");
    }

    free(yas_car(args).value.ffi_pointer);
    *result = yas_make_sym("T");
    return YAS_ERROR_OK;
}

int yas_builtin_ffi_free(YasAtom args, YasAtom *result)
{
    int r = yas_builtin_ffi_free_impl(args, result);
    return r;
}


/* (ffi-pointer-ref ptr type offset) -> value */
static int yas_builtin_ffi_pointer_ref_impl(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || yas_car(args).type != YAS_ATOM_TYPE_FFI_POINTER) {
        return yas_raise_error("type-error", "ffi-pointer-ref requires an ffi-pointer");
    }
    if (yas_nilp(yas_cdr(args)) || yas_car(yas_cdr(args)).type != YAS_ATOM_TYPE_KEYWORD) {
        return yas_raise_error("type-error", "ffi-pointer-ref requires a type keyword");
    }
    if (yas_nilp(yas_cdr(yas_cdr(args))) || yas_car(yas_cdr(yas_cdr(args))).type != YAS_ATOM_TYPE_INTEGER) {
        return yas_raise_error("type-error", "ffi-pointer-ref requires an integer offset");
    }

    void *base = yas_car(args).value.ffi_pointer;
    YasFFIType type = keyword_to_type(yas_car(yas_cdr(args)).value.keyword);
    size_t offset = (size_t)yas_car(yas_cdr(yas_cdr(args))).value.integer;

    char *ptr = (char *)base + offset;

    switch (type) {
        case YAS_FFI_TYPE_INT8:
            *result = yas_make_int(*(int8_t *)ptr);
            break;
        case YAS_FFI_TYPE_UINT8:
            *result = yas_make_int(*(uint8_t *)ptr);
            break;
        case YAS_FFI_TYPE_INT16:
            *result = yas_make_int(*(int16_t *)ptr);
            break;
        case YAS_FFI_TYPE_UINT16:
            *result = yas_make_int(*(uint16_t *)ptr);
            break;
        case YAS_FFI_TYPE_INT32:
            *result = yas_make_int(*(int32_t *)ptr);
            break;
        case YAS_FFI_TYPE_UINT32:
            *result = yas_make_int(*(uint32_t *)ptr);
            break;
        case YAS_FFI_TYPE_INT64:
            *result = yas_make_int(*(int64_t *)ptr);
            break;
        case YAS_FFI_TYPE_UINT64:
            *result = yas_make_int((long)*(uint64_t *)ptr);
            break;
        case YAS_FFI_TYPE_FLOAT:
            *result = yas_make_float(*(float *)ptr);
            break;
        case YAS_FFI_TYPE_DOUBLE:
            *result = yas_make_float(*(double *)ptr);
            break;
        case YAS_FFI_TYPE_POINTER:
            result->type = YAS_ATOM_TYPE_FFI_POINTER;
            result->value.ffi_pointer = *(void **)ptr;
            break;
        default:
            *result = yas_nil;
    }

    return YAS_ERROR_OK;
}

int yas_builtin_ffi_pointer_ref(YasAtom args, YasAtom *result)
{
    int r = yas_builtin_ffi_pointer_ref_impl(args, result);
    return r;
}


/* (ffi-pointer-set! ptr type offset value) -> #t */
static int yas_builtin_ffi_pointer_set_impl(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || yas_car(args).type != YAS_ATOM_TYPE_FFI_POINTER) {
        return yas_raise_error("type-error", "ffi-pointer-set! requires an ffi-pointer");
    }
    if (yas_nilp(yas_cdr(args)) || yas_car(yas_cdr(args)).type != YAS_ATOM_TYPE_KEYWORD) {
        return yas_raise_error("type-error", "ffi-pointer-set! requires a type keyword");
    }
    if (yas_nilp(yas_cdr(yas_cdr(args))) || yas_car(yas_cdr(yas_cdr(args))).type != YAS_ATOM_TYPE_INTEGER) {
        return yas_raise_error("type-error", "ffi-pointer-set! requires an integer offset");
    }
    if (yas_nilp(yas_cdr(yas_cdr(yas_cdr(args))))) {
        return yas_raise_error("args-error", "ffi-pointer-set! requires a value");
    }

    void *base = yas_car(args).value.ffi_pointer;
    YasFFIType type = keyword_to_type(yas_car(yas_cdr(args)).value.keyword);
    size_t offset = (size_t)yas_car(yas_cdr(yas_cdr(args))).value.integer;
    YasAtom value = yas_car(yas_cdr(yas_cdr(yas_cdr(args))));

    char *ptr = (char *)base + offset;

    switch (type) {
        case YAS_FFI_TYPE_INT8:
        case YAS_FFI_TYPE_UINT8:
            *(int8_t *)ptr = (int8_t)(value.type == YAS_ATOM_TYPE_INTEGER ? value.value.integer : 0);
            break;
        case YAS_FFI_TYPE_INT16:
        case YAS_FFI_TYPE_UINT16:
            *(int16_t *)ptr = (int16_t)(value.type == YAS_ATOM_TYPE_INTEGER ? value.value.integer : 0);
            break;
        case YAS_FFI_TYPE_INT32:
        case YAS_FFI_TYPE_UINT32:
            *(int32_t *)ptr = (int32_t)(value.type == YAS_ATOM_TYPE_INTEGER ? value.value.integer : 0);
            break;
        case YAS_FFI_TYPE_INT64:
        case YAS_FFI_TYPE_UINT64:
            *(int64_t *)ptr = (int64_t)(value.type == YAS_ATOM_TYPE_INTEGER ? value.value.integer : 0);
            break;
        case YAS_FFI_TYPE_FLOAT:
            *(float *)ptr = (float)(value.type == YAS_ATOM_TYPE_FLOAT
                                        ? value.value.floatval
                                        : (value.type == YAS_ATOM_TYPE_INTEGER ? value.value.integer : 0));
            break;
        case YAS_FFI_TYPE_DOUBLE:
            *(double *)ptr = (double)(value.type == YAS_ATOM_TYPE_FLOAT
                                          ? value.value.floatval
                                          : (value.type == YAS_ATOM_TYPE_INTEGER ? value.value.integer : 0));
            break;
        case YAS_FFI_TYPE_POINTER:
            if (value.type == YAS_ATOM_TYPE_FFI_POINTER) {
                *(void **)ptr = value.value.ffi_pointer;
            }
            break;
        default:
            break;
    }

    *result = yas_make_sym("T");
    return YAS_ERROR_OK;
}

int yas_builtin_ffi_pointer_set(YasAtom args, YasAtom *result)
{
    int r = yas_builtin_ffi_pointer_set_impl(args, result);
    return r;
}


/* (ffi-null) -> null pointer */
static int yas_builtin_ffi_null_impl(YasAtom args, YasAtom *result)
{
    (void)args;
    result->type = YAS_ATOM_TYPE_FFI_POINTER;
    result->value.ffi_pointer = NULL;
    return YAS_ERROR_OK;
}

int yas_builtin_ffi_null(YasAtom args, YasAtom *result)
{
    int r = yas_builtin_ffi_null_impl(args, result);
    return r;
}


/* (ffi-null? ptr) -> boolean */
static int yas_builtin_ffi_nullp_impl(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args)) {
        *result = yas_make_sym("T");
        return YAS_ERROR_OK;
    }

    YasAtom a = yas_car(args);
    if (a.type == YAS_ATOM_TYPE_FFI_POINTER && a.value.ffi_pointer == NULL) {
        *result = yas_make_sym("T");
    } else if (yas_nilp(a)) {
        *result = yas_make_sym("T");
    } else {
        *result = yas_nil;
    }
    return YAS_ERROR_OK;
}

int yas_builtin_ffi_nullp(YasAtom args, YasAtom *result)
{
    int r = yas_builtin_ffi_nullp_impl(args, result);
    return r;
}


/* ============================================================
 * Struct Builtins
 * ============================================================ */

/* (%ffi-defstruct 'name '((field-name type) ...)) -> struct-type-symbol
 * Called by the ffi-defstruct macro which quotes the arguments.
 */
static int yas_builtin_ffi_defstruct_impl(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || yas_car(args).type != YAS_ATOM_TYPE_SYMBOL) {
        return yas_raise_error("type-error", "%ffi-defstruct requires a symbol name");
    }

    char *name = (char *)yas_car(args).value.symbol;

    /* Second arg is the list of field specs */
    if (yas_nilp(yas_cdr(args))) {
        return yas_raise_error("type-error", "%ffi-defstruct requires a field list");
    }
    YasAtom field_list = yas_car(yas_cdr(args));

    /* Check if already defined */
    if (yas_ffi_lookup_struct_type(name)) {
        return yas_raise_error("ffi-error", "Struct type already defined");
    }

    /* Allocate struct type */
    YasFFIStructType *st = calloc(1, sizeof(YasFFIStructType));
    st->name = strdup(name);

    /* Allocate ffi_type for this struct */
    ffi_type *ffi_struct = malloc(sizeof(ffi_type));
    memset(ffi_struct, 0, sizeof(ffi_type));
    st->ffi_struct = ffi_struct;

    /* Parse fields from the list */
    YasAtom fields = field_list;
    int i = 0;
    while (!yas_nilp(fields) && i < YAS_FFI_STRUCT_MAX_FIELDS) {
        YasAtom field = yas_car(fields);
        if (field.type != YAS_ATOM_TYPE_PAIR) {
            free(st->name);
            free(ffi_struct);
            free(st);
            return yas_raise_error("syntax-error", "ffi-defstruct field must be (name type)");
        }

        YasAtom field_name = yas_car(field);
        YasAtom field_type = yas_car(yas_cdr(field));

        if (field_name.type != YAS_ATOM_TYPE_SYMBOL) {
            free(st->name);
            free(ffi_struct);
            free(st);
            return yas_raise_error("type-error", "Field name must be a symbol");
        }
        if (field_type.type != YAS_ATOM_TYPE_KEYWORD) {
            free(st->name);
            free(ffi_struct);
            free(st);
            return yas_raise_error("type-error", "Field type must be a keyword");
        }

        st->fields[i].name = strdup(field_name.value.symbol);
        st->fields[i].type = keyword_to_type(field_type.value.keyword);
        st->elements[i] = type_to_ffi(st->fields[i].type);

        fields = yas_cdr(fields);
        i++;
    }
    st->field_count = i;
    st->elements[i] = NULL; /* Null-terminate for libffi */

    /* Initialize libffi struct type */
    ffi_struct->size = 0;      /* libffi calculates */
    ffi_struct->alignment = 0; /* libffi calculates */
    ffi_struct->type = FFI_TYPE_STRUCT;
    ffi_struct->elements = (ffi_type **)st->elements;

    /* Force libffi to compute size/alignment by preparing a dummy cif */
    ffi_cif temp_cif;
    ffi_type *temp_args[] = {ffi_struct};
    ffi_prep_cif(&temp_cif, FFI_DEFAULT_ABI, 1, &ffi_type_void, temp_args);
    st->size = ffi_struct->size;

    /* Calculate field offsets */
    size_t offset = 0;
    for (int j = 0; j < st->field_count; j++) {
        ffi_type *elem = (ffi_type *)st->elements[j];
        size_t align = elem->alignment;
        offset = (offset + align - 1) & ~(align - 1); /* Align */
        st->fields[j].offset = offset;
        offset += elem->size;
    }

    /* Add to registry */
    st->next = yas_ffi_struct_registry;
    yas_ffi_struct_registry = st;

    *result = yas_car(args); /* Return the struct name symbol */
    return YAS_ERROR_OK;
}

int yas_builtin_ffi_defstruct(YasAtom args, YasAtom *result)
{
    int r = yas_builtin_ffi_defstruct_impl(args, result);
    return r;
}


/* (make-ffi-struct type-name values...) -> ffi-struct */
static int yas_builtin_ffi_make_struct_impl(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || yas_car(args).type != YAS_ATOM_TYPE_SYMBOL) {
        return yas_raise_error("type-error", "make-ffi-struct requires a struct type name");
    }

    const char *name = yas_car(args).value.symbol;
    YasFFIStructType *st = yas_ffi_lookup_struct_type(name);
    if (!st) {
        return yas_raise_error("ffi-error", "Unknown struct type");
    }

    /* Allocate struct instance (GC-tracked, see data.c's unified gc_all list) */
    YasFFIStructInstance *inst = yas_gc_alloc_ffi_struct();
    inst->type = st;
    inst->data = calloc(1, st->size);

    /* Pack field values */
    YasAtom values = yas_cdr(args);
    for (int i = 0; i < st->field_count && !yas_nilp(values); i++) {
        YasAtom v = yas_car(values);
        void *field_ptr = (char *)inst->data + st->fields[i].offset;

        switch (st->fields[i].type) {
            case YAS_FFI_TYPE_INT8:
            case YAS_FFI_TYPE_UINT8:
                *(uint8_t *)field_ptr = (uint8_t)(v.type == YAS_ATOM_TYPE_INTEGER ? v.value.integer : 0);
                break;
            case YAS_FFI_TYPE_INT16:
            case YAS_FFI_TYPE_UINT16:
                *(uint16_t *)field_ptr = (uint16_t)(v.type == YAS_ATOM_TYPE_INTEGER ? v.value.integer : 0);
                break;
            case YAS_FFI_TYPE_INT32:
            case YAS_FFI_TYPE_UINT32:
                *(uint32_t *)field_ptr = (uint32_t)(v.type == YAS_ATOM_TYPE_INTEGER ? v.value.integer : 0);
                break;
            case YAS_FFI_TYPE_INT64:
            case YAS_FFI_TYPE_UINT64:
                *(uint64_t *)field_ptr = (uint64_t)(v.type == YAS_ATOM_TYPE_INTEGER ? v.value.integer : 0);
                break;
            case YAS_FFI_TYPE_FLOAT:
                *(float *)field_ptr =
                    (float)(v.type == YAS_ATOM_TYPE_FLOAT ? v.value.floatval
                                                          : (v.type == YAS_ATOM_TYPE_INTEGER ? v.value.integer : 0));
                break;
            case YAS_FFI_TYPE_DOUBLE:
                *(double *)field_ptr =
                    (double)(v.type == YAS_ATOM_TYPE_FLOAT ? v.value.floatval
                                                           : (v.type == YAS_ATOM_TYPE_INTEGER ? v.value.integer : 0));
                break;
            case YAS_FFI_TYPE_POINTER:
                if (v.type == YAS_ATOM_TYPE_FFI_POINTER) {
                    *(void **)field_ptr = v.value.ffi_pointer;
                } else {
                    *(void **)field_ptr = NULL;
                }
                break;
            default:
                break;
        }
        values = yas_cdr(values);
    }

    result->type = YAS_ATOM_TYPE_FFI_STRUCT;
    result->value.ffi_struct = inst;
    return YAS_ERROR_OK;
}

int yas_builtin_ffi_make_struct(YasAtom args, YasAtom *result)
{
    int r = yas_builtin_ffi_make_struct_impl(args, result);
    return r;
}


/* (ffi-struct-ref struct field-name) -> value */
static int yas_builtin_ffi_struct_ref_impl(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || yas_car(args).type != YAS_ATOM_TYPE_FFI_STRUCT) {
        return yas_raise_error("type-error", "ffi-struct-ref requires an ffi-struct");
    }
    if (yas_nilp(yas_cdr(args)) || yas_car(yas_cdr(args)).type != YAS_ATOM_TYPE_SYMBOL) {
        return yas_raise_error("type-error", "ffi-struct-ref requires a field name symbol");
    }

    YasFFIStructInstance *inst = yas_car(args).value.ffi_struct;
    if (!inst || !inst->type) {
        return yas_raise_error("ffi-error", "Invalid struct instance");
    }

    const char *field_name = yas_car(yas_cdr(args)).value.symbol;
    YasFFIStructType *st = inst->type;

    /* Find field */
    int field_idx = -1;
    for (int i = 0; i < st->field_count; i++) {
        if (strcmp(st->fields[i].name, field_name) == 0) {
            field_idx = i;
            break;
        }
    }

    if (field_idx < 0) {
        return yas_raise_error("ffi-error", "Unknown field name");
    }

    void *field_ptr = (char *)inst->data + st->fields[field_idx].offset;

    switch (st->fields[field_idx].type) {
        case YAS_FFI_TYPE_INT8:
            *result = yas_make_int(*(int8_t *)field_ptr);
            break;
        case YAS_FFI_TYPE_UINT8:
            *result = yas_make_int(*(uint8_t *)field_ptr);
            break;
        case YAS_FFI_TYPE_INT16:
            *result = yas_make_int(*(int16_t *)field_ptr);
            break;
        case YAS_FFI_TYPE_UINT16:
            *result = yas_make_int(*(uint16_t *)field_ptr);
            break;
        case YAS_FFI_TYPE_INT32:
            *result = yas_make_int(*(int32_t *)field_ptr);
            break;
        case YAS_FFI_TYPE_UINT32:
            *result = yas_make_int(*(uint32_t *)field_ptr);
            break;
        case YAS_FFI_TYPE_INT64:
        case YAS_FFI_TYPE_UINT64:
            *result = yas_make_int(*(int64_t *)field_ptr);
            break;
        case YAS_FFI_TYPE_FLOAT:
            *result = yas_make_float(*(float *)field_ptr);
            break;
        case YAS_FFI_TYPE_DOUBLE:
            *result = yas_make_float(*(double *)field_ptr);
            break;
        case YAS_FFI_TYPE_POINTER:
            result->type = YAS_ATOM_TYPE_FFI_POINTER;
            result->value.ffi_pointer = *(void **)field_ptr;
            break;
        default:
            *result = yas_nil;
    }

    return YAS_ERROR_OK;
}

int yas_builtin_ffi_struct_ref(YasAtom args, YasAtom *result)
{
    int r = yas_builtin_ffi_struct_ref_impl(args, result);
    return r;
}


/* (ffi-struct-set! struct field-name value) -> value */
static int yas_builtin_ffi_struct_set_impl(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || yas_car(args).type != YAS_ATOM_TYPE_FFI_STRUCT) {
        return yas_raise_error("type-error", "ffi-struct-set! requires an ffi-struct");
    }
    if (yas_nilp(yas_cdr(args)) || yas_car(yas_cdr(args)).type != YAS_ATOM_TYPE_SYMBOL) {
        return yas_raise_error("type-error", "ffi-struct-set! requires a field name symbol");
    }
    if (yas_nilp(yas_cdr(yas_cdr(args)))) {
        return yas_raise_error("args-error", "ffi-struct-set! requires a value");
    }

    YasFFIStructInstance *inst = yas_car(args).value.ffi_struct;
    if (!inst || !inst->type) {
        return yas_raise_error("ffi-error", "Invalid struct instance");
    }

    const char *field_name = yas_car(yas_cdr(args)).value.symbol;
    YasAtom value = yas_car(yas_cdr(yas_cdr(args)));
    YasFFIStructType *st = inst->type;

    /* Find field */
    int field_idx = -1;
    for (int i = 0; i < st->field_count; i++) {
        if (strcmp(st->fields[i].name, field_name) == 0) {
            field_idx = i;
            break;
        }
    }

    if (field_idx < 0) {
        return yas_raise_error("ffi-error", "Unknown field name");
    }

    void *field_ptr = (char *)inst->data + st->fields[field_idx].offset;

    switch (st->fields[field_idx].type) {
        case YAS_FFI_TYPE_INT8:
        case YAS_FFI_TYPE_UINT8:
            *(uint8_t *)field_ptr = (uint8_t)(value.type == YAS_ATOM_TYPE_INTEGER ? value.value.integer : 0);
            break;
        case YAS_FFI_TYPE_INT16:
        case YAS_FFI_TYPE_UINT16:
            *(uint16_t *)field_ptr = (uint16_t)(value.type == YAS_ATOM_TYPE_INTEGER ? value.value.integer : 0);
            break;
        case YAS_FFI_TYPE_INT32:
        case YAS_FFI_TYPE_UINT32:
            *(uint32_t *)field_ptr = (uint32_t)(value.type == YAS_ATOM_TYPE_INTEGER ? value.value.integer : 0);
            break;
        case YAS_FFI_TYPE_INT64:
        case YAS_FFI_TYPE_UINT64:
            *(uint64_t *)field_ptr = (uint64_t)(value.type == YAS_ATOM_TYPE_INTEGER ? value.value.integer : 0);
            break;
        case YAS_FFI_TYPE_FLOAT:
            *(float *)field_ptr = (float)(value.type == YAS_ATOM_TYPE_FLOAT
                                              ? value.value.floatval
                                              : (value.type == YAS_ATOM_TYPE_INTEGER ? value.value.integer : 0));
            break;
        case YAS_FFI_TYPE_DOUBLE:
            *(double *)field_ptr = (double)(value.type == YAS_ATOM_TYPE_FLOAT
                                                ? value.value.floatval
                                                : (value.type == YAS_ATOM_TYPE_INTEGER ? value.value.integer : 0));
            break;
        case YAS_FFI_TYPE_POINTER:
            if (value.type == YAS_ATOM_TYPE_FFI_POINTER) {
                *(void **)field_ptr = value.value.ffi_pointer;
            } else {
                *(void **)field_ptr = NULL;
            }
            break;
        default:
            break;
    }

    *result = value;
    return YAS_ERROR_OK;
}

int yas_builtin_ffi_struct_set(YasAtom args, YasAtom *result)
{
    int r = yas_builtin_ffi_struct_set_impl(args, result);
    return r;
}


#endif /* YAS_FFI */

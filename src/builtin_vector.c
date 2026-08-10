#include "yas.h"

int yas_builtin_vectorp(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || !yas_nilp(yas_cdr(args)))
        return yas_raise_error("args-error", "vector?: expected 1 argument");
    *result = (yas_car(args).type == YAS_ATOM_TYPE_VECTOR) ? yas_make_sym("T") : yas_nil;
    return YAS_ERROR_OK;
}

/* (vector-length vec) */
int yas_builtin_vector_length(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || !yas_nilp(yas_cdr(args)))
        return yas_raise_error("args-error", "vector-length: expected 1 argument");
    YasAtom a = yas_car(args);
    if (a.type != YAS_ATOM_TYPE_VECTOR)
        return yas_raise_error("type-error", "vector-length: expected vector");
    *result = yas_make_int(yas_vector_length(a));
    return YAS_ERROR_OK;
}

/* (vector-append v1 v2 ...) */
int yas_builtin_vector_append(YasAtom args, YasAtom *result)
{
    size_t total_len = 0;
    YasAtom p = args;
    while (!yas_nilp(p)) {
        if (yas_car(p).type != YAS_ATOM_TYPE_VECTOR)
            return yas_raise_error("type-error", "vector-append: arguments must be vectors");
        total_len += yas_vector_length(yas_car(p));
        p = yas_cdr(p);
    }
    *result = yas_make_vector(total_len, yas_nil);
    size_t idx = 0;
    p = args;
    while (!yas_nilp(p)) {
        YasAtom v = yas_car(p);
        for (size_t i = 0; i < yas_vector_length(v); i++) {
            yas_vector_set(*result, idx++, yas_vector_ref(v, i));
        }
        p = yas_cdr(p);
    }
    return YAS_ERROR_OK;
}

/* Vector operations */
int yas_builtin_make_vector(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args))
        return yas_raise_error("args-error", "make-vector: expected size argument");

    YasAtom size_arg = yas_car(args);
    if (size_arg.type != YAS_ATOM_TYPE_INTEGER)
        return yas_raise_error("type-error", "make-vector: size must be integer");

    YasAtom fill = yas_nil;
    if (!yas_nilp(yas_cdr(args))) {
        fill = yas_car(yas_cdr(args));
        if (!yas_nilp(yas_cdr(yas_cdr(args))))
            return yas_raise_error("args-error", "make-vector: expected 1 or 2 arguments");
    }

    *result = yas_make_vector((size_t)size_arg.value.integer, fill);
    return YAS_ERROR_OK;
}

int yas_builtin_vector_ref(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || yas_nilp(yas_cdr(args)) || !yas_nilp(yas_cdr(yas_cdr(args))))
        return yas_raise_error("args-error", "vector-ref: expected 2 arguments");

    YasAtom vec = yas_car(args);
    YasAtom idx = yas_car(yas_cdr(args));

    if (vec.type != YAS_ATOM_TYPE_VECTOR || idx.type != YAS_ATOM_TYPE_INTEGER)
        return yas_raise_error("type-error", "vector-ref: arguments must be vector and integer");

    if (idx.value.integer < 0 || (size_t)idx.value.integer >= yas_vector_length(vec))
        return yas_raise_error("args-error", "vector-ref: index out of bounds");

    *result = yas_vector_ref(vec, (size_t)idx.value.integer);
    return YAS_ERROR_OK;
}

int yas_builtin_vector_set(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || yas_nilp(yas_cdr(args)) || yas_nilp(yas_cdr(yas_cdr(args))) ||
        !yas_nilp(yas_cdr(yas_cdr(yas_cdr(args)))))
        return yas_raise_error("args-error", "vector-set!: expected 3 arguments");

    YasAtom vec = yas_car(args);
    YasAtom idx = yas_car(yas_cdr(args));
    YasAtom val = yas_car(yas_cdr(yas_cdr(args)));

    if (vec.type != YAS_ATOM_TYPE_VECTOR || idx.type != YAS_ATOM_TYPE_INTEGER)
        return yas_raise_error("type-error", "vector-set!: arguments must be vector, integer and value");

    if (idx.value.integer < 0 || (size_t)idx.value.integer >= yas_vector_length(vec))
        return yas_raise_error("args-error", "vector-set!: index out of bounds");

    yas_vector_set(vec, (size_t)idx.value.integer, val);
    *result = val;
    return YAS_ERROR_OK;
}

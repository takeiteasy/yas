#include "yas.h"

/* (char? obj) */
static int yas_builtin_charp_impl(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args))
        return yas_raise_error("args-error", "char?: expected 1 argument");
    *result = yas_car(args).type == YAS_ATOM_TYPE_CHAR ? yas_make_sym("T") : yas_nil;
    return YAS_ERROR_OK;
}

int yas_builtin_charp(YasAtom args, YasAtom *result)
{
    yas_gil_enter();
    int r = yas_builtin_charp_impl(args, result);
    yas_gil_leave();
    return r;
}


/* (char->integer char) */
static int yas_builtin_char_to_integer_impl(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || !yas_nilp(yas_cdr(args)))
        return yas_raise_error("args-error", "char->integer: expected 1 argument");
    YasAtom a = yas_car(args);
    if (a.type != YAS_ATOM_TYPE_CHAR)
        return yas_raise_error("type-error", "char->integer: argument must be a character");
    *result = yas_make_int(a.value.character);
    return YAS_ERROR_OK;
}

int yas_builtin_char_to_integer(YasAtom args, YasAtom *result)
{
    yas_gil_enter();
    int r = yas_builtin_char_to_integer_impl(args, result);
    yas_gil_leave();
    return r;
}


/* (integer->char int) */
static int yas_builtin_integer_to_char_impl(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || !yas_nilp(yas_cdr(args)))
        return yas_raise_error("args-error", "integer->char: expected 1 argument");
    YasAtom a = yas_car(args);
    if (a.type != YAS_ATOM_TYPE_INTEGER)
        return yas_raise_error("type-error", "integer->char: argument must be an integer");
    *result = yas_make_char((int32_t)a.value.integer);
    return YAS_ERROR_OK;
}

int yas_builtin_integer_to_char(YasAtom args, YasAtom *result)
{
    yas_gil_enter();
    int r = yas_builtin_integer_to_char_impl(args, result);
    yas_gil_leave();
    return r;
}


/* (char=? a b) */
static int yas_builtin_char_eq_impl(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || yas_nilp(yas_cdr(args)) || !yas_nilp(yas_cdr(yas_cdr(args))))
        return yas_raise_error("args-error", "char=?: expected 2 arguments");
    YasAtom a = yas_car(args), b = yas_car(yas_cdr(args));
    if (a.type != YAS_ATOM_TYPE_CHAR || b.type != YAS_ATOM_TYPE_CHAR)
        return yas_raise_error("type-error", "char=?: arguments must be characters");
    *result = (a.value.character == b.value.character) ? yas_make_sym("T") : yas_nil;
    return YAS_ERROR_OK;
}

int yas_builtin_char_eq(YasAtom args, YasAtom *result)
{
    yas_gil_enter();
    int r = yas_builtin_char_eq_impl(args, result);
    yas_gil_leave();
    return r;
}


/* (char<? a b) */
static int yas_builtin_char_lt_impl(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || yas_nilp(yas_cdr(args)) || !yas_nilp(yas_cdr(yas_cdr(args))))
        return yas_raise_error("args-error", "char<?: expected 2 arguments");
    YasAtom a = yas_car(args), b = yas_car(yas_cdr(args));
    if (a.type != YAS_ATOM_TYPE_CHAR || b.type != YAS_ATOM_TYPE_CHAR)
        return yas_raise_error("type-error", "char<?: arguments must be characters");
    *result = (a.value.character < b.value.character) ? yas_make_sym("T") : yas_nil;
    return YAS_ERROR_OK;
}

int yas_builtin_char_lt(YasAtom args, YasAtom *result)
{
    yas_gil_enter();
    int r = yas_builtin_char_lt_impl(args, result);
    yas_gil_leave();
    return r;
}


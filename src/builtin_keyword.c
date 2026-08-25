#include "yas.h"
#include <stdlib.h>
#include <string.h>

/* (keyword? obj) */
static int yas_builtin_keywordp_impl(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || !yas_nilp(yas_cdr(args)))
        return yas_raise_error("args-error", "keyword?: expected 1 argument");
    *result = yas_car(args).type == YAS_ATOM_TYPE_KEYWORD ? yas_make_sym("T") : yas_nil;
    return YAS_ERROR_OK;
}

int yas_builtin_keywordp(YasAtom args, YasAtom *result)
{
    int r = yas_builtin_keywordp_impl(args, result);
    return r;
}


/* (keyword->string kw) */
static int yas_builtin_keyword_to_string_impl(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || !yas_nilp(yas_cdr(args)))
        return yas_raise_error("args-error", "keyword->string: expected 1 argument");
    if (yas_car(args).type != YAS_ATOM_TYPE_KEYWORD)
        return yas_raise_error("type-error", "keyword->string: argument must be keyword");
    *result = yas_make_string(yas_car(args).value.keyword);
    return YAS_ERROR_OK;
}

int yas_builtin_keyword_to_string(YasAtom args, YasAtom *result)
{
    int r = yas_builtin_keyword_to_string_impl(args, result);
    return r;
}


/* (string->keyword str) */
static int yas_builtin_string_to_keyword_impl(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || !yas_nilp(yas_cdr(args)))
        return yas_raise_error("args-error", "string->keyword: expected 1 argument");
    if (yas_car(args).type != YAS_ATOM_TYPE_STRING)
        return yas_raise_error("type-error", "string->keyword: argument must be string");

    /* Convert to uppercase for consistency */
    char *buf = strdup(yas_car(args).value.string);
    for (char *p = buf; *p; p++) {
        if (*p >= 'a' && *p <= 'z')
            *p -= 32;
    }
    *result = yas_make_keyword(buf);
    free(buf);
    return YAS_ERROR_OK;
}

int yas_builtin_string_to_keyword(YasAtom args, YasAtom *result)
{
    int r = yas_builtin_string_to_keyword_impl(args, result);
    return r;
}


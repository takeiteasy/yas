#include "yas.h"
#include <stdlib.h>
#include <string.h>

int yas_builtin_stringp(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || !yas_nilp(yas_cdr(args)))
        return yas_raise_error("args-error", "string?: expected 1 argument");
    *result = (yas_car(args).type == YAS_ATOM_TYPE_STRING) ? yas_make_sym("T") : yas_nil;
    return YAS_ERROR_OK;
}

/* (string-length str) */
int yas_builtin_string_length(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || !yas_nilp(yas_cdr(args)))
        return yas_raise_error("args-error", "string-length: expected 1 argument");
    YasAtom a = yas_car(args);
    if (a.type != YAS_ATOM_TYPE_STRING)
        return yas_raise_error("type-error", "string-length: expected string");
    *result = yas_make_int(yas_utf8_strlen(a.value.string));
    return YAS_ERROR_OK;
}

/* (string-ref str k) - get k-th character (0-indexed) */
int yas_builtin_string_ref(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || yas_nilp(yas_cdr(args)) || !yas_nilp(yas_cdr(yas_cdr(args))))
        return yas_raise_error("args-error", "string-ref: expected 2 arguments");

    YasAtom str = yas_car(args);
    YasAtom idx = yas_car(yas_cdr(args));

    if (str.type != YAS_ATOM_TYPE_STRING)
        return yas_raise_error("type-error", "string-ref: first argument must be string");
    if (idx.type != YAS_ATOM_TYPE_INTEGER)
        return yas_raise_error("type-error", "string-ref: second argument must be integer");

    int32_t cp = yas_utf8_codepoint_at(str.value.string, (size_t)idx.value.integer);
    if (cp < 0)
        return yas_raise_error("args-error", "string-ref: index out of bounds");

    *result = yas_make_char(cp);
    return YAS_ERROR_OK;
}

/* (string=? s1 s2) */
int yas_builtin_string_eq(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || yas_nilp(yas_cdr(args)) || !yas_nilp(yas_cdr(yas_cdr(args))))
        return yas_raise_error("args-error", "string=?: expected 2 arguments");

    YasAtom a = yas_car(args);
    YasAtom b = yas_car(yas_cdr(args));

    if (a.type != YAS_ATOM_TYPE_STRING || b.type != YAS_ATOM_TYPE_STRING)
        return yas_raise_error("type-error", "string=?: arguments must be strings");

    *result = (strcmp(a.value.string, b.value.string) == 0) ? yas_make_sym("T") : yas_nil;
    return YAS_ERROR_OK;
}

/* (string-byte-length str) - byte length (for I/O) */
int yas_builtin_string_byte_length(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || !yas_nilp(yas_cdr(args)))
        return yas_raise_error("args-error", "string-byte-length: expected 1 argument");

    YasAtom str = yas_car(args);
    if (str.type != YAS_ATOM_TYPE_STRING)
        return yas_raise_error("type-error", "string-byte-length: argument must be string");

    *result = yas_make_int(strlen(str.value.string));
    return YAS_ERROR_OK;
}

/* (string-append s1 s2 ...) */
int yas_builtin_string_append(YasAtom args, YasAtom *result)
{
    size_t total_len = 0;
    YasAtom p = args;
    while (!yas_nilp(p)) {
        if (yas_car(p).type != YAS_ATOM_TYPE_STRING)
            return yas_raise_error("type-error", "string-append: arguments must be strings");
        total_len += strlen(yas_car(p).value.string);
        p = yas_cdr(p);
    }
    char *buf = malloc(total_len + 1);
    buf[0] = '\0';
    p = args;
    while (!yas_nilp(p)) {
        strcat(buf, yas_car(p).value.string);
        p = yas_cdr(p);
    }
    *result = yas_make_string(buf);
    free(buf);
    return YAS_ERROR_OK;
}

/* (string-upcase str) - convert string to uppercase */
int yas_builtin_string_upcase(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || !yas_nilp(yas_cdr(args)))
        return yas_raise_error("args-error", "string-upcase: expected 1 argument");

    YasAtom str = yas_car(args);
    if (str.type != YAS_ATOM_TYPE_STRING)
        return yas_raise_error("type-error", "string-upcase: expected string");

    size_t len = strlen(str.value.string);
    char *buf = malloc(len + 1);

    for (size_t i = 0; i <= len; i++) {
        unsigned char c = str.value.string[i];
        if (c >= 'a' && c <= 'z')
            buf[i] = c - 32;
        else
            buf[i] = c;
    }

    *result = yas_make_string(buf);
    free(buf);
    return YAS_ERROR_OK;
}

/* (string-downcase str) - convert string to lowercase */
int yas_builtin_string_downcase(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || !yas_nilp(yas_cdr(args)))
        return yas_raise_error("args-error", "string-downcase: expected 1 argument");

    YasAtom str = yas_car(args);
    if (str.type != YAS_ATOM_TYPE_STRING)
        return yas_raise_error("type-error", "string-downcase: expected string");

    size_t len = strlen(str.value.string);
    char *buf = malloc(len + 1);

    for (size_t i = 0; i <= len; i++) {
        unsigned char c = str.value.string[i];
        if (c >= 'A' && c <= 'Z')
            buf[i] = c + 32;
        else
            buf[i] = c;
    }

    *result = yas_make_string(buf);
    free(buf);
    return YAS_ERROR_OK;
}

/* (string-capitalize str) - capitalize first letter of each word */
int yas_builtin_string_capitalize(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || !yas_nilp(yas_cdr(args)))
        return yas_raise_error("args-error", "string-capitalize: expected 1 argument");

    YasAtom str = yas_car(args);
    if (str.type != YAS_ATOM_TYPE_STRING)
        return yas_raise_error("type-error", "string-capitalize: expected string");

    size_t len = strlen(str.value.string);
    char *buf = malloc(len + 1);

    int word_start = 1;
    for (size_t i = 0; i <= len; i++) {
        unsigned char c = str.value.string[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            word_start = 1;
            buf[i] = c;
        } else if (word_start && c >= 'a' && c <= 'z') {
            buf[i] = c - 32;
            word_start = 0;
        } else if (!word_start && c >= 'A' && c <= 'Z') {
            buf[i] = c + 32;
            word_start = 0;
        } else {
            buf[i] = c;
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
                word_start = 0;
        }
    }

    *result = yas_make_string(buf);
    free(buf);
    return YAS_ERROR_OK;
}

/* (make-string n [char]) - create string of n copies of char (default space) */
int yas_builtin_make_string(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args))
        return yas_raise_error("args-error", "make-string: expected at least 1 argument");

    YasAtom n_arg = yas_car(args);
    if (n_arg.type != YAS_ATOM_TYPE_INTEGER)
        return yas_raise_error("type-error", "make-string: first argument must be integer");

    long n = n_arg.value.integer;
    if (n < 0)
        return yas_raise_error("args-error", "make-string: length must be non-negative");

    int32_t cp = ' '; /* default to space */
    if (!yas_nilp(yas_cdr(args))) {
        YasAtom ch = yas_car(yas_cdr(args));
        if (ch.type == YAS_ATOM_TYPE_CHAR) {
            cp = ch.value.character;
        } else if (ch.type == YAS_ATOM_TYPE_STRING && strlen(ch.value.string) > 0) {
            yas_utf8_decode(ch.value.string, &cp);
        } else {
            return yas_raise_error("type-error", "make-string: second argument must be char or string");
        }
    }

    /* Encode the character to UTF-8 */
    char encoded[5];
    int char_len = yas_utf8_encode(cp, encoded);

    size_t total_len = n * char_len;
    char *buf = malloc(total_len + 1);

    for (long i = 0; i < n; i++) {
        memcpy(buf + i * char_len, encoded, char_len);
    }
    buf[total_len] = '\0';

    *result = yas_make_string(buf);
    free(buf);
    return YAS_ERROR_OK;
}

/* (substring str start [end]) - extract substring */
int yas_builtin_substring(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || yas_nilp(yas_cdr(args)))
        return yas_raise_error("args-error", "substring: expected at least 2 arguments");

    YasAtom str = yas_car(args);
    YasAtom start_arg = yas_car(yas_cdr(args));

    if (str.type != YAS_ATOM_TYPE_STRING)
        return yas_raise_error("type-error", "substring: first argument must be string");
    if (start_arg.type != YAS_ATOM_TYPE_INTEGER)
        return yas_raise_error("type-error", "substring: second argument must be integer");

    size_t str_len = yas_utf8_strlen(str.value.string);
    long start = start_arg.value.integer;
    long end = str_len;

    if (!yas_nilp(yas_cdr(yas_cdr(args)))) {
        YasAtom end_arg = yas_car(yas_cdr(yas_cdr(args)));
        if (end_arg.type != YAS_ATOM_TYPE_INTEGER)
            return yas_raise_error("type-error", "substring: third argument must be integer");
        end = end_arg.value.integer;
    }

    if (start < 0)
        start = 0;
    if (end > (long)str_len)
        end = str_len;
    if (start > end)
        start = end;

    /* Find byte positions for UTF-8 string */
    const char *start_ptr = yas_utf8_index_ptr(str.value.string, start);
    const char *end_ptr = yas_utf8_index_ptr(str.value.string, end);

    if (!start_ptr || !end_ptr) {
        *result = yas_make_string("");
        return YAS_ERROR_OK;
    }

    size_t byte_len = end_ptr - start_ptr;
    *result = yas_make_string_n(start_ptr, byte_len);
    return YAS_ERROR_OK;
}

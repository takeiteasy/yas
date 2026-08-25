#include "yas.h"
#include <stdlib.h>
#include <string.h>

/* (make-bytevector size [fill]) */
static int yas_builtin_make_bytevector_impl(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || yas_car(args).type != YAS_ATOM_TYPE_INTEGER) {
        return yas_raise_error("type-error", "make-bytevector expects an integer size");
    }

    size_t size = (size_t)yas_car(args).value.integer;
    *result = yas_make_bytevector(size);

    /* Optional fill value */
    if (!yas_nilp(yas_cdr(args)) && yas_car(yas_cdr(args)).type == YAS_ATOM_TYPE_INTEGER) {
        unsigned char fill = (unsigned char)yas_car(yas_cdr(args)).value.integer;
        memset((*result).value.bytevector->data, fill, size);
    }

    return YAS_ERROR_OK;
}

int yas_builtin_make_bytevector(YasAtom args, YasAtom *result)
{
    yas_gil_enter();
    int r = yas_builtin_make_bytevector_impl(args, result);
    yas_gil_leave();
    return r;
}


/* (bytevector? obj) */
static int yas_builtin_bytevectorp_impl(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args))
        return YAS_ERROR_ARGS;
    *result = yas_car(args).type == YAS_ATOM_TYPE_BYTEVECTOR ? yas_make_sym("T") : yas_nil;
    return YAS_ERROR_OK;
}

int yas_builtin_bytevectorp(YasAtom args, YasAtom *result)
{
    yas_gil_enter();
    int r = yas_builtin_bytevectorp_impl(args, result);
    yas_gil_leave();
    return r;
}


/* (bytevector-length bv) */
static int yas_builtin_bytevector_length_impl(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || yas_car(args).type != YAS_ATOM_TYPE_BYTEVECTOR) {
        return yas_raise_error("type-error", "bytevector-length expects a bytevector");
    }
    *result = yas_make_int(yas_bytevector_length(yas_car(args)));
    return YAS_ERROR_OK;
}

int yas_builtin_bytevector_length(YasAtom args, YasAtom *result)
{
    yas_gil_enter();
    int r = yas_builtin_bytevector_length_impl(args, result);
    yas_gil_leave();
    return r;
}


/* (bytevector-u8-ref bv index) */
static int yas_builtin_bytevector_u8_ref_impl(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || yas_car(args).type != YAS_ATOM_TYPE_BYTEVECTOR) {
        return yas_raise_error("type-error", "bytevector-u8-ref expects a bytevector");
    }
    if (yas_nilp(yas_cdr(args)) || yas_car(yas_cdr(args)).type != YAS_ATOM_TYPE_INTEGER) {
        return yas_raise_error("type-error", "bytevector-u8-ref expects an integer index");
    }

    YasAtom bv = yas_car(args);
    size_t index = (size_t)yas_car(yas_cdr(args)).value.integer;

    if (index >= yas_bytevector_length(bv)) {
        return yas_raise_error("range-error", "bytevector index out of bounds");
    }

    *result = yas_make_int(yas_bytevector_ref(bv, index));
    return YAS_ERROR_OK;
}

int yas_builtin_bytevector_u8_ref(YasAtom args, YasAtom *result)
{
    yas_gil_enter();
    int r = yas_builtin_bytevector_u8_ref_impl(args, result);
    yas_gil_leave();
    return r;
}


/* (bytevector-u8-set! bv index value) */
static int yas_builtin_bytevector_u8_set_impl(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || yas_car(args).type != YAS_ATOM_TYPE_BYTEVECTOR) {
        return yas_raise_error("type-error", "bytevector-u8-set! expects a bytevector");
    }
    if (yas_nilp(yas_cdr(args)) || yas_car(yas_cdr(args)).type != YAS_ATOM_TYPE_INTEGER) {
        return yas_raise_error("type-error", "bytevector-u8-set! expects an integer index");
    }
    if (yas_nilp(yas_cdr(yas_cdr(args))) || yas_car(yas_cdr(yas_cdr(args))).type != YAS_ATOM_TYPE_INTEGER) {
        return yas_raise_error("type-error", "bytevector-u8-set! expects an integer value");
    }

    YasAtom bv = yas_car(args);
    size_t index = (size_t)yas_car(yas_cdr(args)).value.integer;
    unsigned char value = (unsigned char)yas_car(yas_cdr(yas_cdr(args))).value.integer;

    if (index >= yas_bytevector_length(bv)) {
        return yas_raise_error("range-error", "bytevector index out of bounds");
    }

    yas_bytevector_set(bv, index, value);
    *result = yas_make_sym("T");
    return YAS_ERROR_OK;
}

int yas_builtin_bytevector_u8_set(YasAtom args, YasAtom *result)
{
    yas_gil_enter();
    int r = yas_builtin_bytevector_u8_set_impl(args, result);
    yas_gil_leave();
    return r;
}


/* (bytevector->string bv) - UTF-8 interpretation */
static int yas_builtin_bytevector_to_string_impl(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || yas_car(args).type != YAS_ATOM_TYPE_BYTEVECTOR) {
        return yas_raise_error("type-error", "bytevector->string expects a bytevector");
    }

    struct YasBytevector *bv = yas_car(args).value.bytevector;
    *result = yas_make_string_n((const char *)bv->data, bv->size);
    return YAS_ERROR_OK;
}

int yas_builtin_bytevector_to_string(YasAtom args, YasAtom *result)
{
    yas_gil_enter();
    int r = yas_builtin_bytevector_to_string_impl(args, result);
    yas_gil_leave();
    return r;
}


/* (string->bytevector str) */
static int yas_builtin_string_to_bytevector_impl(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || yas_car(args).type != YAS_ATOM_TYPE_STRING) {
        return yas_raise_error("type-error", "string->bytevector expects a string");
    }

    const char *s = yas_car(args).value.string;
    *result = yas_make_bytevector_from_data((const unsigned char *)s, strlen(s));
    return YAS_ERROR_OK;
}

int yas_builtin_string_to_bytevector(YasAtom args, YasAtom *result)
{
    yas_gil_enter();
    int r = yas_builtin_string_to_bytevector_impl(args, result);
    yas_gil_leave();
    return r;
}


/* (string->bytes str [encoding]) - Convert string to bytevector */
static int yas_builtin_string_to_bytes_impl(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || yas_car(args).type != YAS_ATOM_TYPE_STRING)
        return yas_raise_error("type-error", "string->bytes: first argument must be string");

    const char *str = yas_car(args).value.string;
    const char *encoding = "utf-8"; /* Default */

    if (!yas_nilp(yas_cdr(args))) {
        YasAtom enc_arg = yas_car(yas_cdr(args));
        if (enc_arg.type != YAS_ATOM_TYPE_STRING)
            return yas_raise_error("type-error", "string->bytes: encoding must be string");
        encoding = enc_arg.value.string;
    }

    /* UTF-8 is the native format - just copy bytes */
    if (strcmp(encoding, "utf-8") == 0 || strcmp(encoding, "UTF-8") == 0) {
        size_t len = strlen(str);
        *result = yas_make_bytevector_from_data((const unsigned char *)str, len);
    } else if (strcmp(encoding, "ascii") == 0 || strcmp(encoding, "ASCII") == 0) {
        /* ASCII: fail on non-ASCII characters */
        size_t len = strlen(str);
        for (size_t i = 0; i < len; i++) {
            if ((unsigned char)str[i] > 127)
                return yas_raise_error("encoding-error", "string->bytes: non-ASCII character in string");
        }
        *result = yas_make_bytevector_from_data((const unsigned char *)str, len);
    } else if (strcmp(encoding, "latin-1") == 0 || strcmp(encoding, "LATIN-1") == 0 ||
               strcmp(encoding, "iso-8859-1") == 0 || strcmp(encoding, "ISO-8859-1") == 0) {
        /* Latin-1: encode codepoints 0-255 directly */
        size_t codepoint_count = yas_utf8_strlen(str);
        unsigned char *buf = malloc(codepoint_count);
        const char *p = str;
        for (size_t i = 0; i < codepoint_count; i++) {
            int32_t cp;
            p += yas_utf8_decode(p, &cp);
            if (cp > 255)
                return yas_raise_error("encoding-error", "string->bytes: codepoint > 255 not encodable in latin-1");
            buf[i] = (unsigned char)cp;
        }
        *result = yas_make_bytevector_from_data(buf, codepoint_count);
        free(buf);
    } else {
        return yas_raise_error("value-error", "string->bytes: unknown encoding (use utf-8, ascii, or latin-1)");
    }

    return YAS_ERROR_OK;
}

int yas_builtin_string_to_bytes(YasAtom args, YasAtom *result)
{
    yas_gil_enter();
    int r = yas_builtin_string_to_bytes_impl(args, result);
    yas_gil_leave();
    return r;
}


/* (bytes->string bytevector [encoding]) - Convert bytevector to string */
static int yas_builtin_bytes_to_string_impl(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || yas_car(args).type != YAS_ATOM_TYPE_BYTEVECTOR)
        return yas_raise_error("type-error", "bytes->string: first argument must be bytevector");

    struct YasBytevector *bv = yas_car(args).value.bytevector;
    const char *encoding = "utf-8"; /* Default */

    if (!yas_nilp(yas_cdr(args))) {
        YasAtom enc_arg = yas_car(yas_cdr(args));
        if (enc_arg.type != YAS_ATOM_TYPE_STRING)
            return yas_raise_error("type-error", "bytes->string: encoding must be string");
        encoding = enc_arg.value.string;
    }

    if (strcmp(encoding, "utf-8") == 0 || strcmp(encoding, "UTF-8") == 0) {
        /* UTF-8: just copy as string */
        *result = yas_make_string_n((const char *)bv->data, bv->size);
    } else if (strcmp(encoding, "ascii") == 0 || strcmp(encoding, "ASCII") == 0) {
        /* ASCII: fail on non-ASCII bytes */
        for (size_t i = 0; i < bv->size; i++) {
            if (bv->data[i] > 127)
                return yas_raise_error("encoding-error", "bytes->string: non-ASCII byte in bytevector");
        }
        *result = yas_make_string_n((const char *)bv->data, bv->size);
    } else if (strcmp(encoding, "latin-1") == 0 || strcmp(encoding, "LATIN-1") == 0 ||
               strcmp(encoding, "iso-8859-1") == 0 || strcmp(encoding, "ISO-8859-1") == 0) {
        /* Latin-1: each byte is a codepoint, encode to UTF-8 */
        /* Worst case: each byte becomes 2 UTF-8 bytes (for 128-255) */
        char *buf = malloc(bv->size * 2 + 1);
        char *p = buf;
        for (size_t i = 0; i < bv->size; i++) {
            p += yas_utf8_encode(bv->data[i], p);
        }
        *p = '\0';
        *result = yas_make_string(buf);
        free(buf);
    } else {
        return yas_raise_error("value-error", "bytes->string: unknown encoding (use utf-8, ascii, or latin-1)");
    }

    return YAS_ERROR_OK;
}

int yas_builtin_bytes_to_string(YasAtom args, YasAtom *result)
{
    yas_gil_enter();
    int r = yas_builtin_bytes_to_string_impl(args, result);
    yas_gil_leave();
    return r;
}


#include "yas.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void yas_print_expr_impl(YasAtom atom)
{
    switch (atom.type) {
        default:
        case YAS_ATOM_TYPE_NIL:
            printf("NIL");
            break;
        case YAS_ATOM_TYPE_PAIR:
            putchar('(');
            yas_print_expr(yas_car(atom));
            atom = yas_cdr(atom);
            while (!yas_nilp(atom)) {
                if (atom.type == YAS_ATOM_TYPE_PAIR) {
                    putchar(' ');
                    yas_print_expr(yas_car(atom));
                    atom = yas_cdr(atom);
                } else {
                    printf(" . ");
                    yas_print_expr(atom);
                    break;
                }
            }
            putchar(')');
            break;
        case YAS_ATOM_TYPE_SYMBOL:
            printf("%s", atom.value.symbol);
            break;
        case YAS_ATOM_TYPE_INTEGER:
            printf("%ld", atom.value.integer);
            break;
        case YAS_ATOM_TYPE_BUILTIN:
            printf("#<BUILTIN:%p>", atom.value.builtin);
            break;
        case YAS_ATOM_TYPE_CLOSURE:
            printf("#<CLOSURE:%p>", atom.value.pair);
            break;
        case YAS_ATOM_TYPE_MACRO:
            printf("#<MACRO:%p>", atom.value.pair);
            break;
        case YAS_ATOM_TYPE_CONTINUATION:
            printf("#<CONTINUATION>");
            break;
        case YAS_ATOM_TYPE_ENV:
            printf("#<ENVIRONMENT:%p>", (void *)atom.value.env);
            break;
        case YAS_ATOM_TYPE_FRAME:
            printf("#<FRAME:%p>", (void *)atom.value.frame);
            break;
        case YAS_ATOM_TYPE_FLOAT:
            printf("%g", atom.value.floatval);
            break;
        case YAS_ATOM_TYPE_STRING:
            putchar('"');
            for (char *s = atom.value.string; *s; s++) {
                if (*s == '"' || *s == '\\')
                    putchar('\\');
                putchar(*s);
            }
            putchar('"');
            break;
        case YAS_ATOM_TYPE_VECTOR:
            printf("#(");
            for (size_t i = 0; i < atom.value.vector->size; i++) {
                if (i > 0)
                    putchar(' ');
                yas_print_expr(atom.value.vector->data[i]);
            }
            putchar(')');
            break;
        case YAS_ATOM_TYPE_HASH:
            printf("#<HASH:%zu>", atom.value.hash->size);
            break;
        case YAS_ATOM_TYPE_PORT:
            printf("#<PORT %s %s%s>", atom.value.port->is_input ? "input" : "output", atom.value.port->path,
                   atom.value.port->is_open ? "" : " (closed)");
            break;
        case YAS_ATOM_TYPE_BYTEVECTOR:
            printf("#<BYTEVECTOR:%zu>", atom.value.bytevector->size);
            break;
        case YAS_ATOM_TYPE_CHAR: {
            int32_t cp = atom.value.character;
            if (cp == '\n')
                printf("#\\newline");
            else if (cp == ' ')
                printf("#\\space");
            else if (cp == '\t')
                printf("#\\tab");
            else if (cp == '\r')
                printf("#\\return");
            else if (cp < 0x80 && cp >= 0x21 && cp <= 0x7E)
                printf("#\\%c", (char)cp);
            else
                printf("#\\x%X", cp);
            break;
        }
        case YAS_ATOM_TYPE_KEYWORD:
            printf(":%s", atom.value.keyword);
            break;
        case YAS_ATOM_TYPE_VEC2: {
#ifdef YAS_SIMD_SCALAR
            printf("#vec(%g %g)", atom.value.vec->data[0], atom.value.vec->data[1]);
#else
            float vals[4];
#ifdef YAS_SIMD_X86
            _mm_storeu_ps(vals, atom.value.vec->v);
#elif defined(YAS_SIMD_ARM_NEON)
            vst1q_f32(vals, atom.value.vec->v);
#elif defined(YAS_SIMD_WASM)
            wasm_v128_store(vals, atom.value.vec->v);
#endif
            printf("#vec(%g %g)", vals[0], vals[1]);
#endif
            break;
        }
        case YAS_ATOM_TYPE_VEC3: {
#ifdef YAS_SIMD_SCALAR
            printf("#vec(%g %g %g)", atom.value.vec->data[0], atom.value.vec->data[1], atom.value.vec->data[2]);
#else
            float vals[4];
#ifdef YAS_SIMD_X86
            _mm_storeu_ps(vals, atom.value.vec->v);
#elif defined(YAS_SIMD_ARM_NEON)
            vst1q_f32(vals, atom.value.vec->v);
#elif defined(YAS_SIMD_WASM)
            wasm_v128_store(vals, atom.value.vec->v);
#endif
            printf("#vec(%g %g %g)", vals[0], vals[1], vals[2]);
#endif
            break;
        }
        case YAS_ATOM_TYPE_VEC4: {
#ifdef YAS_SIMD_SCALAR
            printf("#vec(%g %g %g %g)", atom.value.vec->data[0], atom.value.vec->data[1], atom.value.vec->data[2],
                   atom.value.vec->data[3]);
#else
            float vals[4];
#ifdef YAS_SIMD_X86
            _mm_storeu_ps(vals, atom.value.vec->v);
#elif defined(YAS_SIMD_ARM_NEON)
            vst1q_f32(vals, atom.value.vec->v);
#elif defined(YAS_SIMD_WASM)
            wasm_v128_store(vals, atom.value.vec->v);
#endif
            printf("#vec(%g %g %g %g)", vals[0], vals[1], vals[2], vals[3]);
#endif
            break;
        }
        case YAS_ATOM_TYPE_MAT4: {
            printf("#mat(");
            for (int c = 0; c < 4; c++) {
#ifdef YAS_SIMD_SCALAR
                for (int r = 0; r < 4; r++) {
                    if (c > 0 || r > 0)
                        printf(" ");
                    printf("%g", atom.value.mat4->columns[c].data[r]);
                }
#else
                float vals[4];
#ifdef YAS_SIMD_X86
                _mm_storeu_ps(vals, atom.value.mat4->columns[c].v);
#elif defined(YAS_SIMD_ARM_NEON)
                vst1q_f32(vals, atom.value.mat4->columns[c].v);
#elif defined(YAS_SIMD_WASM)
                wasm_v128_store(vals, atom.value.mat4->columns[c].v);
#endif
                for (int r = 0; r < 4; r++) {
                    if (c > 0 || r > 0)
                        printf(" ");
                    printf("%g", vals[r]);
                }
#endif
            }
            printf(")");
            break;
        }
#ifdef YAS_FFI
        case YAS_ATOM_TYPE_FFI_LIBRARY:
            printf("#<FFI-LIBRARY:%p>", atom.value.ffi_library ? atom.value.ffi_library->handle : NULL);
            break;
        case YAS_ATOM_TYPE_FFI_POINTER:
            printf("#<FFI-POINTER:%p>", atom.value.ffi_pointer);
            break;
        case YAS_ATOM_TYPE_FFI_FUNCTION:
            printf("#<FFI-FUNCTION:%p>", atom.value.ffi_fn);
            break;
        case YAS_ATOM_TYPE_FFI_STRUCT:
            if (atom.value.ffi_struct && atom.value.ffi_struct->type) {
                printf("#<FFI-STRUCT:%s>", atom.value.ffi_struct->type->name);
            } else {
                printf("#<FFI-STRUCT:nil>");
            }
            break;
#endif
    }
}

void yas_print_expr(YasAtom atom)
{
    yas_print_expr_impl(atom);
}


/* Calculate length needed for sprint_expr */
static size_t sprint_expr_len(YasAtom atom)
{
    char buf[64];
    switch (atom.type) {
        default:
        case YAS_ATOM_TYPE_NIL:
            return 3; /* "NIL" */
        case YAS_ATOM_TYPE_PAIR: {
            size_t len = 1; /* '(' */
            YasAtom a = atom;
            while (!yas_nilp(a)) {
                if (a.type == YAS_ATOM_TYPE_PAIR) {
                    if (a.value.pair != atom.value.pair)
                        len++; /* space */
                    len += sprint_expr_len(yas_car(a));
                    a = yas_cdr(a);
                } else {
                    len += 3; /* " . " */
                    len += sprint_expr_len(a);
                    break;
                }
            }
            return len + 1; /* ')' */
        }
        case YAS_ATOM_TYPE_SYMBOL:
            return strlen(atom.value.symbol);
        case YAS_ATOM_TYPE_INTEGER:
            snprintf(buf, sizeof(buf), "%ld", atom.value.integer);
            return strlen(buf);
        case YAS_ATOM_TYPE_FLOAT:
            snprintf(buf, sizeof(buf), "%g", atom.value.floatval);
            return strlen(buf);
        case YAS_ATOM_TYPE_STRING:
            return strlen(atom.value.string); /* For ->string, just the raw string */
        case YAS_ATOM_TYPE_CHAR: {
            char tmp[8];
            return yas_utf8_encode(atom.value.character, tmp);
        }
        case YAS_ATOM_TYPE_KEYWORD:
            return strlen(atom.value.keyword) + 1; /* ':' + name */
        case YAS_ATOM_TYPE_VECTOR: {
            size_t len = 2; /* "#(" */
            for (size_t i = 0; i < atom.value.vector->size; i++) {
                if (i > 0)
                    len++;
                len += sprint_expr_len(atom.value.vector->data[i]);
            }
            return len + 1;
        }
        case YAS_ATOM_TYPE_HASH:
            snprintf(buf, sizeof(buf), "#<HASH:%zu>", atom.value.hash->size);
            return strlen(buf);
        case YAS_ATOM_TYPE_BUILTIN:
        case YAS_ATOM_TYPE_CLOSURE:
        case YAS_ATOM_TYPE_MACRO:
        case YAS_ATOM_TYPE_CONTINUATION:
        case YAS_ATOM_TYPE_ENV:
        case YAS_ATOM_TYPE_FRAME:
            return 20; /* estimate for #<...> forms */
        case YAS_ATOM_TYPE_PORT:
            return 40;
        case YAS_ATOM_TYPE_BYTEVECTOR:
            return 30;
        case YAS_ATOM_TYPE_VEC2:
        case YAS_ATOM_TYPE_VEC3:
        case YAS_ATOM_TYPE_VEC4:
            return 60;
        case YAS_ATOM_TYPE_MAT4:
            return 200;
    }
}

/* Sprint atom to buffer, return chars written */
static size_t sprint_expr_to(YasAtom atom, char *buf, size_t pos)
{
    char tmp[64];
    int n;
    switch (atom.type) {
        default:
        case YAS_ATOM_TYPE_NIL:
            memcpy(buf + pos, "NIL", 3);
            return pos + 3;
        case YAS_ATOM_TYPE_PAIR: {
            buf[pos++] = '(';
            YasAtom a = atom;
            int first = 1;
            while (!yas_nilp(a)) {
                if (a.type == YAS_ATOM_TYPE_PAIR) {
                    if (!first)
                        buf[pos++] = ' ';
                    pos = sprint_expr_to(yas_car(a), buf, pos);
                    a = yas_cdr(a);
                    first = 0;
                } else {
                    memcpy(buf + pos, " . ", 3);
                    pos += 3;
                    pos = sprint_expr_to(a, buf, pos);
                    break;
                }
            }
            buf[pos++] = ')';
            return pos;
        }
        case YAS_ATOM_TYPE_SYMBOL:
            n = strlen(atom.value.symbol);
            memcpy(buf + pos, atom.value.symbol, n);
            return pos + n;
        case YAS_ATOM_TYPE_INTEGER:
            n = snprintf(tmp, sizeof(tmp), "%ld", atom.value.integer);
            memcpy(buf + pos, tmp, n);
            return pos + n;
        case YAS_ATOM_TYPE_FLOAT:
            n = snprintf(tmp, sizeof(tmp), "%g", atom.value.floatval);
            memcpy(buf + pos, tmp, n);
            return pos + n;
        case YAS_ATOM_TYPE_STRING:
            n = strlen(atom.value.string);
            memcpy(buf + pos, atom.value.string, n);
            return pos + n;
        case YAS_ATOM_TYPE_CHAR:
            n = yas_utf8_encode(atom.value.character, buf + pos);
            return pos + n;
        case YAS_ATOM_TYPE_KEYWORD:
            buf[pos++] = ':';
            n = strlen(atom.value.keyword);
            memcpy(buf + pos, atom.value.keyword, n);
            return pos + n;
        case YAS_ATOM_TYPE_VECTOR: {
            buf[pos++] = '#';
            buf[pos++] = '(';
            for (size_t i = 0; i < atom.value.vector->size; i++) {
                if (i > 0)
                    buf[pos++] = ' ';
                pos = sprint_expr_to(atom.value.vector->data[i], buf, pos);
            }
            buf[pos++] = ')';
            return pos;
        }
        case YAS_ATOM_TYPE_HASH:
            n = snprintf(tmp, sizeof(tmp), "#<HASH:%zu>", atom.value.hash->size);
            memcpy(buf + pos, tmp, n);
            return pos + n;
        case YAS_ATOM_TYPE_BUILTIN:
            n = snprintf(tmp, sizeof(tmp), "#<BUILTIN>");
            memcpy(buf + pos, tmp, n);
            return pos + n;
        case YAS_ATOM_TYPE_CLOSURE:
            n = snprintf(tmp, sizeof(tmp), "#<CLOSURE>");
            memcpy(buf + pos, tmp, n);
            return pos + n;
        case YAS_ATOM_TYPE_MACRO:
            n = snprintf(tmp, sizeof(tmp), "#<MACRO>");
            memcpy(buf + pos, tmp, n);
            return pos + n;
        case YAS_ATOM_TYPE_CONTINUATION:
            n = snprintf(tmp, sizeof(tmp), "#<CONTINUATION>");
            memcpy(buf + pos, tmp, n);
            return pos + n;
        case YAS_ATOM_TYPE_ENV:
            n = snprintf(tmp, sizeof(tmp), "#<ENVIRONMENT>");
            memcpy(buf + pos, tmp, n);
            return pos + n;
        case YAS_ATOM_TYPE_FRAME:
            n = snprintf(tmp, sizeof(tmp), "#<FRAME>");
            memcpy(buf + pos, tmp, n);
            return pos + n;
        case YAS_ATOM_TYPE_PORT:
            n = snprintf(tmp, sizeof(tmp), "#<PORT>");
            memcpy(buf + pos, tmp, n);
            return pos + n;
        case YAS_ATOM_TYPE_BYTEVECTOR:
            n = snprintf(tmp, sizeof(tmp), "#<BYTEVECTOR:%zu>", atom.value.bytevector->size);
            memcpy(buf + pos, tmp, n);
            return pos + n;
        case YAS_ATOM_TYPE_VEC2:
        case YAS_ATOM_TYPE_VEC3:
        case YAS_ATOM_TYPE_VEC4: {
            float vals[4];
#ifdef YAS_SIMD_SCALAR
            for (int i = 0; i < 4; i++)
                vals[i] = atom.value.vec->data[i];
#else
#ifdef YAS_SIMD_X86
            _mm_storeu_ps(vals, atom.value.vec->v);
#elif defined(YAS_SIMD_ARM_NEON)
            vst1q_f32(vals, atom.value.vec->v);
#elif defined(YAS_SIMD_WASM)
            wasm_v128_store(vals, atom.value.vec->v);
#endif
#endif
            if (atom.type == YAS_ATOM_TYPE_VEC2)
                n = snprintf(tmp, sizeof(tmp), "#vec(%g %g)", vals[0], vals[1]);
            else if (atom.type == YAS_ATOM_TYPE_VEC3)
                n = snprintf(tmp, sizeof(tmp), "#vec(%g %g %g)", vals[0], vals[1], vals[2]);
            else
                n = snprintf(tmp, sizeof(tmp), "#vec(%g %g %g %g)", vals[0], vals[1], vals[2], vals[3]);
            memcpy(buf + pos, tmp, n);
            return pos + n;
        }
        case YAS_ATOM_TYPE_MAT4:
            n = snprintf(tmp, sizeof(tmp), "#<MAT4>");
            memcpy(buf + pos, tmp, n);
            return pos + n;
    }
}

/* Convert atom to string - returns newly allocated string */
static char * yas_sprint_expr_impl(YasAtom atom)
{
    size_t len = sprint_expr_len(atom);
    char *buf = malloc(len + 1);
    size_t end = sprint_expr_to(atom, buf, 0);
    buf[end] = '\0';
    return buf;
}

char * yas_sprint_expr(YasAtom atom)
{
    char * r = yas_sprint_expr_impl(atom);
    return r;
}


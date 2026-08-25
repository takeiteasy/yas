#include "yas.h"
#include <string.h>
#include <stdio.h>

static inline yas_vec4_t vec4_splat(float val)
{
    yas_vec4_t result;
#ifdef YAS_SIMD_X86
    result.v = _mm_set1_ps(val);
#elif defined(YAS_SIMD_ARM_NEON)
    result.v = vdupq_n_f32(val);
#elif defined(YAS_SIMD_WASM)
    result.v = wasm_f32x4_splat(val);
#else
    result.data[0] = result.data[1] = result.data[2] = result.data[3] = val;
#endif
    return result;
}

static inline void vec4_store(float *ptr, yas_vec4_t v)
{
#ifdef YAS_SIMD_X86
    _mm_storeu_ps(ptr, v.v);
#elif defined(YAS_SIMD_ARM_NEON)
    vst1q_f32(ptr, v.v);
#elif defined(YAS_SIMD_WASM)
    wasm_v128_store(ptr, v.v);
#else
    ptr[0] = v.data[0];
    ptr[1] = v.data[1];
    ptr[2] = v.data[2];
    ptr[3] = v.data[3];
#endif
}

static inline yas_vec4_t vec4_add(yas_vec4_t a, yas_vec4_t b)
{
    yas_vec4_t result;
#ifdef YAS_SIMD_X86
    result.v = _mm_add_ps(a.v, b.v);
#elif defined(YAS_SIMD_ARM_NEON)
    result.v = vaddq_f32(a.v, b.v);
#elif defined(YAS_SIMD_WASM)
    result.v = wasm_f32x4_add(a.v, b.v);
#else
    for (int i = 0; i < 4; i++)
        result.data[i] = a.data[i] + b.data[i];
#endif
    return result;
}

static inline yas_vec4_t vec4_sub(yas_vec4_t a, yas_vec4_t b)
{
    yas_vec4_t result;
#ifdef YAS_SIMD_X86
    result.v = _mm_sub_ps(a.v, b.v);
#elif defined(YAS_SIMD_ARM_NEON)
    result.v = vsubq_f32(a.v, b.v);
#elif defined(YAS_SIMD_WASM)
    result.v = wasm_f32x4_sub(a.v, b.v);
#else
    for (int i = 0; i < 4; i++)
        result.data[i] = a.data[i] - b.data[i];
#endif
    return result;
}

static inline yas_vec4_t vec4_mul(yas_vec4_t a, yas_vec4_t b)
{
    yas_vec4_t result;
#ifdef YAS_SIMD_X86
    result.v = _mm_mul_ps(a.v, b.v);
#elif defined(YAS_SIMD_ARM_NEON)
    result.v = vmulq_f32(a.v, b.v);
#elif defined(YAS_SIMD_WASM)
    result.v = wasm_f32x4_mul(a.v, b.v);
#else
    for (int i = 0; i < 4; i++)
        result.data[i] = a.data[i] * b.data[i];
#endif
    return result;
}

static inline yas_vec4_t vec4_div(yas_vec4_t a, yas_vec4_t b)
{
    yas_vec4_t result;
#ifdef YAS_SIMD_X86
    result.v = _mm_div_ps(a.v, b.v);
#elif defined(YAS_SIMD_ARM_NEON)
    result.v = vdivq_f32(a.v, b.v);
#elif defined(YAS_SIMD_WASM)
    result.v = wasm_f32x4_div(a.v, b.v);
#else
    for (int i = 0; i < 4; i++)
        result.data[i] = a.data[i] / b.data[i];
#endif
    return result;
}

/* Helper: check if type is any vec type */
static int is_vec_type(int type)
{
    return type == YAS_ATOM_TYPE_VEC2 || type == YAS_ATOM_TYPE_VEC3 || type == YAS_ATOM_TYPE_VEC4;
}

/* Helper: get float value from number */
static float num_to_float(YasAtom a)
{
    if (a.type == YAS_ATOM_TYPE_INTEGER)
        return (float)a.value.integer;
    return (float)a.value.floatval;
}

/* Helper: check if type is numeric */
static int is_numeric(int type)
{
    return type == YAS_ATOM_TYPE_INTEGER || type == YAS_ATOM_TYPE_FLOAT;
}

static int yas_builtin_add_impl(YasAtom args, YasAtom *result)
{
    YasAtom a, b;

    if (yas_nilp(args) || yas_nilp(yas_cdr(args)) || !yas_nilp(yas_cdr(yas_cdr(args))))
        return yas_raise_error("args-error", "+: expected 2 arguments");

    a = yas_car(args);
    b = yas_car(yas_cdr(args));

    /* Vector + Vector */
    if (is_vec_type(a.type) && is_vec_type(b.type)) {
        if (a.type != b.type)
            return yas_raise_error("type-error", "+: vectors must have same dimension");

        yas_vec4_t rv = vec4_add(*a.value.vec, *b.value.vec);
        float vals[4];
        vec4_store(vals, rv);
        if (a.type == YAS_ATOM_TYPE_VEC2)
            *result = yas_make_vec2(vals[0], vals[1]);
        else if (a.type == YAS_ATOM_TYPE_VEC3)
            *result = yas_make_vec3(vals[0], vals[1], vals[2]);
        else
            *result = yas_make_vec4(vals[0], vals[1], vals[2], vals[3]);
        return YAS_ERROR_OK;
    }

    /* Vector + Scalar */
    if (is_vec_type(a.type) && is_numeric(b.type)) {
        float s = num_to_float(b);
        yas_vec4_t sv = vec4_splat(s);
        yas_vec4_t rv = vec4_add(*a.value.vec, sv);
        float vals[4];
        vec4_store(vals, rv);
        if (a.type == YAS_ATOM_TYPE_VEC2)
            *result = yas_make_vec2(vals[0], vals[1]);
        else if (a.type == YAS_ATOM_TYPE_VEC3)
            *result = yas_make_vec3(vals[0], vals[1], vals[2]);
        else
            *result = yas_make_vec4(vals[0], vals[1], vals[2], vals[3]);
        return YAS_ERROR_OK;
    }

    /* Scalar + Vector */
    if (is_numeric(a.type) && is_vec_type(b.type)) {
        float s = num_to_float(a);
        yas_vec4_t sv = vec4_splat(s);
        yas_vec4_t rv = vec4_add(sv, *b.value.vec);
        float vals[4];
        vec4_store(vals, rv);
        if (b.type == YAS_ATOM_TYPE_VEC2)
            *result = yas_make_vec2(vals[0], vals[1]);
        else if (b.type == YAS_ATOM_TYPE_VEC3)
            *result = yas_make_vec3(vals[0], vals[1], vals[2]);
        else
            *result = yas_make_vec4(vals[0], vals[1], vals[2], vals[3]);
        return YAS_ERROR_OK;
    }

    /* Integer + Integer */
    if (a.type == YAS_ATOM_TYPE_INTEGER && b.type == YAS_ATOM_TYPE_INTEGER) {
        *result = yas_make_int(a.value.integer + b.value.integer);
        return YAS_ERROR_OK;
    }

    /* Float arithmetic (if either is float) */
    if (is_numeric(a.type) && is_numeric(b.type)) {
        double av = (a.type == YAS_ATOM_TYPE_INTEGER) ? (double)a.value.integer : a.value.floatval;
        double bv = (b.type == YAS_ATOM_TYPE_INTEGER) ? (double)b.value.integer : b.value.floatval;
        *result = yas_make_float(av + bv);
        return YAS_ERROR_OK;
    }

    return yas_raise_error("type-error", "+: operands must be numbers or vectors");
}

int yas_builtin_add(YasAtom args, YasAtom *result)
{
    int r = yas_builtin_add_impl(args, result);
    return r;
}


static int yas_builtin_subtract_impl(YasAtom args, YasAtom *result)
{
    YasAtom a, b;

    if (yas_nilp(args) || yas_nilp(yas_cdr(args)) || !yas_nilp(yas_cdr(yas_cdr(args))))
        return yas_raise_error("args-error", "-: expected 2 arguments");

    a = yas_car(args);
    b = yas_car(yas_cdr(args));

    /* Vector - Vector */
    if (is_vec_type(a.type) && is_vec_type(b.type)) {
        if (a.type != b.type)
            return yas_raise_error("type-error", "-: vectors must have same dimension");

        yas_vec4_t rv = vec4_sub(*a.value.vec, *b.value.vec);
        float vals[4];
        vec4_store(vals, rv);
        if (a.type == YAS_ATOM_TYPE_VEC2)
            *result = yas_make_vec2(vals[0], vals[1]);
        else if (a.type == YAS_ATOM_TYPE_VEC3)
            *result = yas_make_vec3(vals[0], vals[1], vals[2]);
        else
            *result = yas_make_vec4(vals[0], vals[1], vals[2], vals[3]);
        return YAS_ERROR_OK;
    }

    /* Vector - Scalar */
    if (is_vec_type(a.type) && is_numeric(b.type)) {
        float s = num_to_float(b);
        yas_vec4_t sv = vec4_splat(s);
        yas_vec4_t rv = vec4_sub(*a.value.vec, sv);
        float vals[4];
        vec4_store(vals, rv);
        if (a.type == YAS_ATOM_TYPE_VEC2)
            *result = yas_make_vec2(vals[0], vals[1]);
        else if (a.type == YAS_ATOM_TYPE_VEC3)
            *result = yas_make_vec3(vals[0], vals[1], vals[2]);
        else
            *result = yas_make_vec4(vals[0], vals[1], vals[2], vals[3]);
        return YAS_ERROR_OK;
    }

    /* Integer - Integer */
    if (a.type == YAS_ATOM_TYPE_INTEGER && b.type == YAS_ATOM_TYPE_INTEGER) {
        *result = yas_make_int(a.value.integer - b.value.integer);
        return YAS_ERROR_OK;
    }

    /* Float arithmetic */
    if (is_numeric(a.type) && is_numeric(b.type)) {
        double av = (a.type == YAS_ATOM_TYPE_INTEGER) ? (double)a.value.integer : a.value.floatval;
        double bv = (b.type == YAS_ATOM_TYPE_INTEGER) ? (double)b.value.integer : b.value.floatval;
        *result = yas_make_float(av - bv);
        return YAS_ERROR_OK;
    }

    return yas_raise_error("type-error", "-: operands must be numbers or vectors");
}

int yas_builtin_subtract(YasAtom args, YasAtom *result)
{
    int r = yas_builtin_subtract_impl(args, result);
    return r;
}


static int yas_builtin_multiply_impl(YasAtom args, YasAtom *result)
{
    YasAtom a, b;

    if (yas_nilp(args) || yas_nilp(yas_cdr(args)) || !yas_nilp(yas_cdr(yas_cdr(args))))
        return yas_raise_error("args-error", "*: expected 2 arguments");

    a = yas_car(args);
    b = yas_car(yas_cdr(args));

    /* Vector * Vector (component-wise) */
    if (is_vec_type(a.type) && is_vec_type(b.type)) {
        if (a.type != b.type)
            return yas_raise_error("type-error", "*: vectors must have same dimension");

        yas_vec4_t rv = vec4_mul(*a.value.vec, *b.value.vec);
        float vals[4];
        vec4_store(vals, rv);
        if (a.type == YAS_ATOM_TYPE_VEC2)
            *result = yas_make_vec2(vals[0], vals[1]);
        else if (a.type == YAS_ATOM_TYPE_VEC3)
            *result = yas_make_vec3(vals[0], vals[1], vals[2]);
        else
            *result = yas_make_vec4(vals[0], vals[1], vals[2], vals[3]);
        return YAS_ERROR_OK;
    }

    /* Vector * Scalar */
    if (is_vec_type(a.type) && is_numeric(b.type)) {
        float s = num_to_float(b);
        yas_vec4_t sv = vec4_splat(s);
        yas_vec4_t rv = vec4_mul(*a.value.vec, sv);
        float vals[4];
        vec4_store(vals, rv);
        if (a.type == YAS_ATOM_TYPE_VEC2)
            *result = yas_make_vec2(vals[0], vals[1]);
        else if (a.type == YAS_ATOM_TYPE_VEC3)
            *result = yas_make_vec3(vals[0], vals[1], vals[2]);
        else
            *result = yas_make_vec4(vals[0], vals[1], vals[2], vals[3]);
        return YAS_ERROR_OK;
    }

    /* Scalar * Vector */
    if (is_numeric(a.type) && is_vec_type(b.type)) {
        float s = num_to_float(a);
        yas_vec4_t sv = vec4_splat(s);
        yas_vec4_t rv = vec4_mul(sv, *b.value.vec);
        float vals[4];
        vec4_store(vals, rv);
        if (b.type == YAS_ATOM_TYPE_VEC2)
            *result = yas_make_vec2(vals[0], vals[1]);
        else if (b.type == YAS_ATOM_TYPE_VEC3)
            *result = yas_make_vec3(vals[0], vals[1], vals[2]);
        else
            *result = yas_make_vec4(vals[0], vals[1], vals[2], vals[3]);
        return YAS_ERROR_OK;
    }

    /* Integer * Integer */
    if (a.type == YAS_ATOM_TYPE_INTEGER && b.type == YAS_ATOM_TYPE_INTEGER) {
        *result = yas_make_int(a.value.integer * b.value.integer);
        return YAS_ERROR_OK;
    }

    /* Float arithmetic */
    if (is_numeric(a.type) && is_numeric(b.type)) {
        double av = (a.type == YAS_ATOM_TYPE_INTEGER) ? (double)a.value.integer : a.value.floatval;
        double bv = (b.type == YAS_ATOM_TYPE_INTEGER) ? (double)b.value.integer : b.value.floatval;
        *result = yas_make_float(av * bv);
        return YAS_ERROR_OK;
    }

    return yas_raise_error("type-error", "*: operands must be numbers or vectors");
}

int yas_builtin_multiply(YasAtom args, YasAtom *result)
{
    int r = yas_builtin_multiply_impl(args, result);
    return r;
}


static int yas_builtin_divide_impl(YasAtom args, YasAtom *result)
{
    YasAtom a, b;

    if (yas_nilp(args) || yas_nilp(yas_cdr(args)) || !yas_nilp(yas_cdr(yas_cdr(args))))
        return yas_raise_error("args-error", "/: expected 2 arguments");

    a = yas_car(args);
    b = yas_car(yas_cdr(args));

    /* Vector / Vector (component-wise) */
    if (is_vec_type(a.type) && is_vec_type(b.type)) {
        if (a.type != b.type)
            return yas_raise_error("type-error", "/: vectors must have same dimension");

        yas_vec4_t rv = vec4_div(*a.value.vec, *b.value.vec);
        float vals[4];
        vec4_store(vals, rv);
        if (a.type == YAS_ATOM_TYPE_VEC2)
            *result = yas_make_vec2(vals[0], vals[1]);
        else if (a.type == YAS_ATOM_TYPE_VEC3)
            *result = yas_make_vec3(vals[0], vals[1], vals[2]);
        else
            *result = yas_make_vec4(vals[0], vals[1], vals[2], vals[3]);
        return YAS_ERROR_OK;
    }

    /* Vector / Scalar */
    if (is_vec_type(a.type) && is_numeric(b.type)) {
        float s = num_to_float(b);
        if (s == 0.0f)
            return yas_raise_error("arithmetic-error", "/: division by zero");
        yas_vec4_t sv = vec4_splat(s);
        yas_vec4_t rv = vec4_div(*a.value.vec, sv);
        float vals[4];
        vec4_store(vals, rv);
        if (a.type == YAS_ATOM_TYPE_VEC2)
            *result = yas_make_vec2(vals[0], vals[1]);
        else if (a.type == YAS_ATOM_TYPE_VEC3)
            *result = yas_make_vec3(vals[0], vals[1], vals[2]);
        else
            *result = yas_make_vec4(vals[0], vals[1], vals[2], vals[3]);
        return YAS_ERROR_OK;
    }

    /* Integer / Integer */
    if (a.type == YAS_ATOM_TYPE_INTEGER && b.type == YAS_ATOM_TYPE_INTEGER) {
        if (b.value.integer == 0)
            return yas_raise_error("arithmetic-error", "/: division by zero");
        *result = yas_make_int(a.value.integer / b.value.integer);
        return YAS_ERROR_OK;
    }

    /* Float arithmetic */
    if (is_numeric(a.type) && is_numeric(b.type)) {
        double av = (a.type == YAS_ATOM_TYPE_INTEGER) ? (double)a.value.integer : a.value.floatval;
        double bv = (b.type == YAS_ATOM_TYPE_INTEGER) ? (double)b.value.integer : b.value.floatval;
        if (bv == 0.0)
            return yas_raise_error("arithmetic-error", "/: division by zero");
        *result = yas_make_float(av / bv);
        return YAS_ERROR_OK;
    }

    return yas_raise_error("type-error", "/: operands must be numbers or vectors");
}

int yas_builtin_divide(YasAtom args, YasAtom *result)
{
    int r = yas_builtin_divide_impl(args, result);
    return r;
}


static int yas_builtin_numeq_impl(YasAtom args, YasAtom *result)
{
    YasAtom a, b;

    if (yas_nilp(args) || yas_nilp(yas_cdr(args)) || !yas_nilp(yas_cdr(yas_cdr(args))))
        return yas_raise_error("args-error", "=: expected 2 arguments");

    a = yas_car(args);
    b = yas_car(yas_cdr(args));

    /* Integer == Integer */
    if (a.type == YAS_ATOM_TYPE_INTEGER && b.type == YAS_ATOM_TYPE_INTEGER) {
        *result = (a.value.integer == b.value.integer) ? yas_make_sym("T") : yas_nil;
        return YAS_ERROR_OK;
    }

    /* Float comparison (if either is float or integer) */
    if (is_numeric(a.type) && is_numeric(b.type)) {
        double av = (a.type == YAS_ATOM_TYPE_INTEGER) ? (double)a.value.integer : a.value.floatval;
        double bv = (b.type == YAS_ATOM_TYPE_INTEGER) ? (double)b.value.integer : b.value.floatval;
        *result = (av == bv) ? yas_make_sym("T") : yas_nil;
        return YAS_ERROR_OK;
    }

    return yas_raise_error("type-error", "=: operands must be numbers");
}

int yas_builtin_numeq(YasAtom args, YasAtom *result)
{
    int r = yas_builtin_numeq_impl(args, result);
    return r;
}


static int yas_builtin_less_impl(YasAtom args, YasAtom *result)
{
    YasAtom a, b;

    if (yas_nilp(args) || yas_nilp(yas_cdr(args)) || !yas_nilp(yas_cdr(yas_cdr(args))))
        return yas_raise_error("args-error", "<: expected 2 arguments");

    a = yas_car(args);
    b = yas_car(yas_cdr(args));

    /* Integer < Integer */
    if (a.type == YAS_ATOM_TYPE_INTEGER && b.type == YAS_ATOM_TYPE_INTEGER) {
        *result = (a.value.integer < b.value.integer) ? yas_make_sym("T") : yas_nil;
        return YAS_ERROR_OK;
    }

    /* Float comparison (if either is float or integer) - matches
     * yas_builtin_numeq's mixed int/float handling above. Pre-existing gap
     * found while adding (current-time): `<` (and `<=`/`>`/`>=`, which
     * base.ys derives from it) rejected floats even though `+ - * / =` all
     * accept them. */
    if (is_numeric(a.type) && is_numeric(b.type)) {
        double av = (a.type == YAS_ATOM_TYPE_INTEGER) ? (double)a.value.integer : a.value.floatval;
        double bv = (b.type == YAS_ATOM_TYPE_INTEGER) ? (double)b.value.integer : b.value.floatval;
        *result = (av < bv) ? yas_make_sym("T") : yas_nil;
        return YAS_ERROR_OK;
    }

    return yas_raise_error("type-error", "<: operands must be numbers");
}

int yas_builtin_less(YasAtom args, YasAtom *result)
{
    int r = yas_builtin_less_impl(args, result);
    return r;
}


/* Type predicates */
static int yas_builtin_floatp_impl(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || !yas_nilp(yas_cdr(args)))
        return yas_raise_error("args-error", "float?: expected 1 argument");
    *result = (yas_car(args).type == YAS_ATOM_TYPE_FLOAT) ? yas_make_sym("T") : yas_nil;
    return YAS_ERROR_OK;
}

int yas_builtin_floatp(YasAtom args, YasAtom *result)
{
    int r = yas_builtin_floatp_impl(args, result);
    return r;
}


static int yas_builtin_integerp_impl(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || !yas_nilp(yas_cdr(args)))
        return yas_raise_error("args-error", "integer?: expected 1 argument");
    *result = (yas_car(args).type == YAS_ATOM_TYPE_INTEGER) ? yas_make_sym("T") : yas_nil;
    return YAS_ERROR_OK;
}

int yas_builtin_integerp(YasAtom args, YasAtom *result)
{
    int r = yas_builtin_integerp_impl(args, result);
    return r;
}


/* ============================================================
 * SIMD VECTOR/MATRIX BUILTINS
 * ============================================================ */

/* Helper to extract float from vec component */
static float vec_get_component(YasAtom vec, int idx)
{
#ifdef YAS_SIMD_SCALAR
    return vec.value.vec->data[idx];
#else
    float vals[4];
#ifdef YAS_SIMD_X86
    _mm_storeu_ps(vals, vec.value.vec->v);
#elif defined(YAS_SIMD_ARM_NEON)
    vst1q_f32(vals, vec.value.vec->v);
#elif defined(YAS_SIMD_WASM)
    wasm_v128_store(vals, vec.value.vec->v);
#endif
    return vals[idx];
#endif
}

/* Type predicates */
static int yas_builtin_vec2p_impl(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || !yas_nilp(yas_cdr(args)))
        return yas_raise_error("args-error", "vec2?: expected 1 argument");
    *result = (yas_car(args).type == YAS_ATOM_TYPE_VEC2) ? yas_make_sym("T") : yas_nil;
    return YAS_ERROR_OK;
}

int yas_builtin_vec2p(YasAtom args, YasAtom *result)
{
    int r = yas_builtin_vec2p_impl(args, result);
    return r;
}


static int yas_builtin_vec3p_impl(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || !yas_nilp(yas_cdr(args)))
        return yas_raise_error("args-error", "vec3?: expected 1 argument");
    *result = (yas_car(args).type == YAS_ATOM_TYPE_VEC3) ? yas_make_sym("T") : yas_nil;
    return YAS_ERROR_OK;
}

int yas_builtin_vec3p(YasAtom args, YasAtom *result)
{
    int r = yas_builtin_vec3p_impl(args, result);
    return r;
}


static int yas_builtin_vec4p_impl(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || !yas_nilp(yas_cdr(args)))
        return yas_raise_error("args-error", "vec4?: expected 1 argument");
    *result = (yas_car(args).type == YAS_ATOM_TYPE_VEC4) ? yas_make_sym("T") : yas_nil;
    return YAS_ERROR_OK;
}

int yas_builtin_vec4p(YasAtom args, YasAtom *result)
{
    int r = yas_builtin_vec4p_impl(args, result);
    return r;
}


static int yas_builtin_mat4p_impl(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || !yas_nilp(yas_cdr(args)))
        return yas_raise_error("args-error", "mat4?: expected 1 argument");
    *result = (yas_car(args).type == YAS_ATOM_TYPE_MAT4) ? yas_make_sym("T") : yas_nil;
    return YAS_ERROR_OK;
}

int yas_builtin_mat4p(YasAtom args, YasAtom *result)
{
    int r = yas_builtin_mat4p_impl(args, result);
    return r;
}


/* Constructors */
static int yas_builtin_vec2_impl(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || yas_nilp(yas_cdr(args)) || !yas_nilp(yas_cdr(yas_cdr(args))))
        return yas_raise_error("args-error", "vec2: expected 2 arguments");

    YasAtom x = yas_car(args), y = yas_car(yas_cdr(args));
    float fx, fy;

    if (x.type == YAS_ATOM_TYPE_INTEGER)
        fx = (float)x.value.integer;
    else if (x.type == YAS_ATOM_TYPE_FLOAT)
        fx = (float)x.value.floatval;
    else
        return yas_raise_error("type-error", "vec2: arguments must be numbers");

    if (y.type == YAS_ATOM_TYPE_INTEGER)
        fy = (float)y.value.integer;
    else if (y.type == YAS_ATOM_TYPE_FLOAT)
        fy = (float)y.value.floatval;
    else
        return yas_raise_error("type-error", "vec2: arguments must be numbers");

    *result = yas_make_vec2(fx, fy);
    return YAS_ERROR_OK;
}

int yas_builtin_vec2(YasAtom args, YasAtom *result)
{
    int r = yas_builtin_vec2_impl(args, result);
    return r;
}


static int yas_builtin_vec3_impl(YasAtom args, YasAtom *result)
{
    YasAtom x = yas_car(args), y = yas_car(yas_cdr(args)), z = yas_car(yas_cdr(yas_cdr(args)));
    float fx, fy, fz;

    if (x.type == YAS_ATOM_TYPE_INTEGER)
        fx = (float)x.value.integer;
    else if (x.type == YAS_ATOM_TYPE_FLOAT)
        fx = (float)x.value.floatval;
    else
        return yas_raise_error("type-error", "vec3: arguments must be numbers");

    if (y.type == YAS_ATOM_TYPE_INTEGER)
        fy = (float)y.value.integer;
    else if (y.type == YAS_ATOM_TYPE_FLOAT)
        fy = (float)y.value.floatval;
    else
        return yas_raise_error("type-error", "vec3: arguments must be numbers");

    if (z.type == YAS_ATOM_TYPE_INTEGER)
        fz = (float)z.value.integer;
    else if (z.type == YAS_ATOM_TYPE_FLOAT)
        fz = (float)z.value.floatval;
    else
        return yas_raise_error("type-error", "vec3: arguments must be numbers");

    *result = yas_make_vec3(fx, fy, fz);
    return YAS_ERROR_OK;
}

int yas_builtin_vec3(YasAtom args, YasAtom *result)
{
    int r = yas_builtin_vec3_impl(args, result);
    return r;
}


static int yas_builtin_vec4_impl(YasAtom args, YasAtom *result)
{
    YasAtom x = yas_car(args), y = yas_car(yas_cdr(args)), z = yas_car(yas_cdr(yas_cdr(args))),
            w = yas_car(yas_cdr(yas_cdr(yas_cdr(args))));
    float fx, fy, fz, fw;

    if (x.type == YAS_ATOM_TYPE_INTEGER)
        fx = (float)x.value.integer;
    else if (x.type == YAS_ATOM_TYPE_FLOAT)
        fx = (float)x.value.floatval;
    else
        return yas_raise_error("type-error", "vec4: arguments must be numbers");

    if (y.type == YAS_ATOM_TYPE_INTEGER)
        fy = (float)y.value.integer;
    else if (y.type == YAS_ATOM_TYPE_FLOAT)
        fy = (float)y.value.floatval;
    else
        return yas_raise_error("type-error", "vec4: arguments must be numbers");

    if (z.type == YAS_ATOM_TYPE_INTEGER)
        fz = (float)z.value.integer;
    else if (z.type == YAS_ATOM_TYPE_FLOAT)
        fz = (float)z.value.floatval;
    else
        return yas_raise_error("type-error", "vec4: arguments must be numbers");

    if (w.type == YAS_ATOM_TYPE_INTEGER)
        fw = (float)w.value.integer;
    else if (w.type == YAS_ATOM_TYPE_FLOAT)
        fw = (float)w.value.floatval;
    else
        return yas_raise_error("type-error", "vec4: arguments must be numbers");

    *result = yas_make_vec4(fx, fy, fz, fw);
    return YAS_ERROR_OK;
}

int yas_builtin_vec4(YasAtom args, YasAtom *result)
{
    int r = yas_builtin_vec4_impl(args, result);
    return r;
}


static int yas_builtin_mat4_identity_impl(YasAtom args, YasAtom *result)
{
    if (!yas_nilp(args))
        return yas_raise_error("args-error", "mat4-identity: expected 0 arguments");
    *result = yas_make_mat4_identity();
    return YAS_ERROR_OK;
}

int yas_builtin_mat4_identity(YasAtom args, YasAtom *result)
{
    int r = yas_builtin_mat4_identity_impl(args, result);
    return r;
}


/* Component accessors */
static int yas_builtin_vec_x_impl(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || !yas_nilp(yas_cdr(args)))
        return yas_raise_error("args-error", "vec-x: expected 1 argument");
    YasAtom v = yas_car(args);
    if (v.type != YAS_ATOM_TYPE_VEC2 && v.type != YAS_ATOM_TYPE_VEC3 && v.type != YAS_ATOM_TYPE_VEC4)
        return yas_raise_error("type-error", "vec-x: argument must be a vector");
    *result = yas_make_float(vec_get_component(v, 0));
    return YAS_ERROR_OK;
}

int yas_builtin_vec_x(YasAtom args, YasAtom *result)
{
    int r = yas_builtin_vec_x_impl(args, result);
    return r;
}


static int yas_builtin_vec_y_impl(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || !yas_nilp(yas_cdr(args)))
        return yas_raise_error("args-error", "vec-y: expected 1 argument");
    YasAtom v = yas_car(args);
    if (v.type != YAS_ATOM_TYPE_VEC2 && v.type != YAS_ATOM_TYPE_VEC3 && v.type != YAS_ATOM_TYPE_VEC4)
        return yas_raise_error("type-error", "vec-y: argument must be a vector");
    *result = yas_make_float(vec_get_component(v, 1));
    return YAS_ERROR_OK;
}

int yas_builtin_vec_y(YasAtom args, YasAtom *result)
{
    int r = yas_builtin_vec_y_impl(args, result);
    return r;
}


static int yas_builtin_vec_z_impl(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || !yas_nilp(yas_cdr(args)))
        return yas_raise_error("args-error", "vec-z: expected 1 argument");
    YasAtom v = yas_car(args);
    if (v.type != YAS_ATOM_TYPE_VEC3 && v.type != YAS_ATOM_TYPE_VEC4)
        return yas_raise_error("type-error", "vec-z: argument must be vec3 or vec4");
    *result = yas_make_float(vec_get_component(v, 2));
    return YAS_ERROR_OK;
}

int yas_builtin_vec_z(YasAtom args, YasAtom *result)
{
    int r = yas_builtin_vec_z_impl(args, result);
    return r;
}


static int yas_builtin_vec_w_impl(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || !yas_nilp(yas_cdr(args)))
        return yas_raise_error("args-error", "vec-w: expected 1 argument");
    YasAtom v = yas_car(args);
    if (v.type != YAS_ATOM_TYPE_VEC4)
        return yas_raise_error("type-error", "vec-w: argument must be vec4");
    *result = yas_make_float(vec_get_component(v, 3));
    return YAS_ERROR_OK;
}

int yas_builtin_vec_w(YasAtom args, YasAtom *result)
{
    int r = yas_builtin_vec_w_impl(args, result);
    return r;
}


/* (vec-ref v idx) - get component by index */
static int yas_builtin_vec_ref_impl(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || yas_nilp(yas_cdr(args)) || !yas_nilp(yas_cdr(yas_cdr(args))))
        return yas_raise_error("args-error", "vec-ref: expected 2 arguments");

    YasAtom v = yas_car(args), idx_atom = yas_car(yas_cdr(args));
    if (v.type != YAS_ATOM_TYPE_VEC2 && v.type != YAS_ATOM_TYPE_VEC3 && v.type != YAS_ATOM_TYPE_VEC4)
        return yas_raise_error("type-error", "vec-ref: first argument must be a vector");
    if (idx_atom.type != YAS_ATOM_TYPE_INTEGER)
        return yas_raise_error("type-error", "vec-ref: index must be integer");

    int idx = (int)idx_atom.value.integer;
    int max = (v.type == YAS_ATOM_TYPE_VEC2) ? 2 : (v.type == YAS_ATOM_TYPE_VEC3) ? 3 : 4;
    if (idx < 0 || idx >= max)
        return yas_raise_error("range-error", "vec-ref: index out of bounds");

    *result = yas_make_float(vec_get_component(v, idx));
    return YAS_ERROR_OK;
}

int yas_builtin_vec_ref(YasAtom args, YasAtom *result)
{
    int r = yas_builtin_vec_ref_impl(args, result);
    return r;
}


/* Helper to set a vector component */
static void vec_set_component(YasAtom vec, int idx, float val)
{
    float vals[4];
    vec4_store(vals, *vec.value.vec);
    vals[idx] = val;
    /* Recreate the SIMD vector */
#ifdef YAS_SIMD_X86
    vec.value.vec->v = _mm_loadu_ps(vals);
#elif defined(YAS_SIMD_ARM_NEON)
    vec.value.vec->v = vld1q_f32(vals);
#elif defined(YAS_SIMD_WASM)
    vec.value.vec->v = wasm_v128_load(vals);
#else
    for (int i = 0; i < 4; i++)
        vec.value.vec->data[i] = vals[i];
#endif
}

/* (vec-set! v idx value) - set component by index (mutates in place) */
static int yas_builtin_vec_set_impl(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || yas_nilp(yas_cdr(args)) || yas_nilp(yas_cdr(yas_cdr(args))) ||
        !yas_nilp(yas_cdr(yas_cdr(yas_cdr(args)))))
        return yas_raise_error("args-error", "vec-set!: expected 3 arguments");

    YasAtom v = yas_car(args), idx_atom = yas_car(yas_cdr(args)), val_atom = yas_car(yas_cdr(yas_cdr(args)));
    if (v.type != YAS_ATOM_TYPE_VEC2 && v.type != YAS_ATOM_TYPE_VEC3 && v.type != YAS_ATOM_TYPE_VEC4)
        return yas_raise_error("type-error", "vec-set!: first argument must be a vector");
    if (idx_atom.type != YAS_ATOM_TYPE_INTEGER)
        return yas_raise_error("type-error", "vec-set!: index must be integer");
    if (!is_numeric(val_atom.type))
        return yas_raise_error("type-error", "vec-set!: value must be a number");

    int idx = (int)idx_atom.value.integer;
    int max = (v.type == YAS_ATOM_TYPE_VEC2) ? 2 : (v.type == YAS_ATOM_TYPE_VEC3) ? 3 : 4;
    if (idx < 0 || idx >= max)
        return yas_raise_error("range-error", "vec-set!: index out of bounds");

    float val = num_to_float(val_atom);
    vec_set_component(v, idx, val);
    *result = v;
    return YAS_ERROR_OK;
}

int yas_builtin_vec_set(YasAtom args, YasAtom *result)
{
    int r = yas_builtin_vec_set_impl(args, result);
    return r;
}


/* mat4 multiply: multiply two 4x4 matrices */
static int yas_builtin_mat4_multiply_impl(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || yas_nilp(yas_cdr(args)) || !yas_nilp(yas_cdr(yas_cdr(args))))
        return yas_raise_error("args-error", "mat4*: expected 2 arguments");

    YasAtom a = yas_car(args), b = yas_car(yas_cdr(args));
    if (a.type != YAS_ATOM_TYPE_MAT4 || b.type != YAS_ATOM_TYPE_MAT4)
        return yas_raise_error("type-error", "mat4*: arguments must be mat4");

    /* Result matrix */
    YasAtom r = yas_make_mat4_identity();

    /* Column-major matrix multiplication: C = A * B */
    /* C[col] = A * B[col] for each column */
    for (int col = 0; col < 4; col++) {
        float bc[4];
        vec4_store(bc, b.value.mat4->columns[col]);

        float rc[4] = {0, 0, 0, 0};
        for (int i = 0; i < 4; i++) {
            float ac[4];
            vec4_store(ac, a.value.mat4->columns[i]);
            for (int row = 0; row < 4; row++) {
                rc[row] += ac[row] * bc[i];
            }
        }

#ifdef YAS_SIMD_X86
        r.value.mat4->columns[col].v = _mm_loadu_ps(rc);
#elif defined(YAS_SIMD_ARM_NEON)
        r.value.mat4->columns[col].v = vld1q_f32(rc);
#elif defined(YAS_SIMD_WASM)
        r.value.mat4->columns[col].v = wasm_v128_load(rc);
#else
        for (int j = 0; j < 4; j++)
            r.value.mat4->columns[col].data[j] = rc[j];
#endif
    }

    *result = r;
    return YAS_ERROR_OK;
}

int yas_builtin_mat4_multiply(YasAtom args, YasAtom *result)
{
    int r = yas_builtin_mat4_multiply_impl(args, result);
    return r;
}


/* (mat4 e0 ... e15) - Create mat4 from 16 floats (column-major) */
static int yas_builtin_mat4_impl(YasAtom args, YasAtom *result)
{
    float vals[16];
    YasAtom p = args;
    for (int i = 0; i < 16; i++) {
        if (yas_nilp(p))
            return yas_raise_error("args-error", "mat4: expected 16 arguments");
        YasAtom val = yas_car(p);
        if (val.type == YAS_ATOM_TYPE_INTEGER)
            vals[i] = (float)val.value.integer;
        else if (val.type == YAS_ATOM_TYPE_FLOAT)
            vals[i] = (float)val.value.floatval;
        else
            return yas_raise_error("type-error", "mat4: expected numbers");
        p = yas_cdr(p);
    }
    *result = yas_make_mat4_from_floats(vals);
    return YAS_ERROR_OK;
}

int yas_builtin_mat4(YasAtom args, YasAtom *result)
{
    int r = yas_builtin_mat4_impl(args, result);
    return r;
}


/* mat4 transpose */
static int yas_builtin_mat4_transpose_impl(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || !yas_nilp(yas_cdr(args)))
        return yas_raise_error("args-error", "mat4-transpose: expected 1 argument");

    YasAtom m = yas_car(args);
    if (m.type != YAS_ATOM_TYPE_MAT4)
        return yas_raise_error("type-error", "mat4-transpose: argument must be mat4");

    YasAtom r = yas_make_mat4_identity();

    /* Transpose: swap rows and columns */
    for (int col = 0; col < 4; col++) {
        float mc[4];
        vec4_store(mc, m.value.mat4->columns[col]);
        for (int row = 0; row < 4; row++) {
            float rc[4];
            vec4_store(rc, r.value.mat4->columns[row]);
            rc[col] = mc[row];
#ifdef YAS_SIMD_X86
            r.value.mat4->columns[row].v = _mm_loadu_ps(rc);
#elif defined(YAS_SIMD_ARM_NEON)
            r.value.mat4->columns[row].v = vld1q_f32(rc);
#elif defined(YAS_SIMD_WASM)
            r.value.mat4->columns[row].v = wasm_v128_load(rc);
#else
            for (int j = 0; j < 4; j++)
                r.value.mat4->columns[row].data[j] = rc[j];
#endif
        }
    }

    *result = r;
    return YAS_ERROR_OK;
}

int yas_builtin_mat4_transpose(YasAtom args, YasAtom *result)
{
    int r = yas_builtin_mat4_transpose_impl(args, result);
    return r;
}


/* mat4 inverse - uses standard 4x4 matrix inversion algorithm */
static int yas_builtin_mat4_inverse_impl(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || !yas_nilp(yas_cdr(args)))
        return yas_raise_error("args-error", "mat4-inverse: expected 1 argument");

    YasAtom m = yas_car(args);
    if (m.type != YAS_ATOM_TYPE_MAT4)
        return yas_raise_error("type-error", "mat4-inverse: argument must be mat4");

    /* Extract matrix elements (column-major) */
    float a[16];
    for (int c = 0; c < 4; c++) {
        float mc[4];
        vec4_store(mc, m.value.mat4->columns[c]);
        for (int r = 0; r < 4; r++) {
            a[c * 4 + r] = mc[r];
        }
    }

    float inv[16];

    /* Calculate cofactors */
    inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14] +
             a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
    inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14] -
             a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
    inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13] +
             a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
    inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13] -
              a[12] * a[5] * a[10] + a[12] * a[6] * a[9];

    inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14] -
             a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
    inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14] +
             a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
    inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13] -
             a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
    inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] + a[8] * a[2] * a[13] +
              a[12] * a[1] * a[10] - a[12] * a[2] * a[9];

    inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] +
             a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
    inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14] -
             a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
    inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13] +
              a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
    inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] - a[4] * a[2] * a[13] -
              a[12] * a[1] * a[6] + a[12] * a[2] * a[5];

    inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] - a[5] * a[3] * a[10] -
             a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
    inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] +
             a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
    inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] -
              a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
    inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] + a[8] * a[1] * a[6] -
              a[8] * a[2] * a[5];

    float det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];

    if (det == 0.0f) {
        return yas_raise_error("arithmetic-error", "mat4-inverse: matrix is not invertible (determinant is 0)");
    }

    float inv_det = 1.0f / det;
    for (int i = 0; i < 16; i++) {
        inv[i] *= inv_det;
    }

    *result = yas_make_mat4_from_floats(inv);
    return YAS_ERROR_OK;
}

int yas_builtin_mat4_inverse(YasAtom args, YasAtom *result)
{
    int r = yas_builtin_mat4_inverse_impl(args, result);
    return r;
}


/* ============================================================
 * Format String Helper Builtins
 * ============================================================ */

/* (number->string n [radix]) - convert number to string with optional radix */
static int yas_builtin_number_to_string_impl(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args))
        return yas_raise_error("args-error", "number->string: expected at least 1 argument");

    YasAtom num = yas_car(args);
    int radix = 10;

    if (!yas_nilp(yas_cdr(args))) {
        YasAtom r = yas_car(yas_cdr(args));
        if (r.type != YAS_ATOM_TYPE_INTEGER)
            return yas_raise_error("type-error", "number->string: radix must be integer");
        radix = (int)r.value.integer;
        if (radix < 2 || radix > 36)
            return yas_raise_error("args-error", "number->string: radix must be 2-36");
    }

    char buf[128];

    if (num.type == YAS_ATOM_TYPE_INTEGER) {
        long val = num.value.integer;
        int negative = val < 0;
        if (negative)
            val = -val;

        if (radix == 10) {
            snprintf(buf, sizeof(buf), "%ld", num.value.integer);
        } else {
            /* Convert to string with arbitrary radix */
            static const char digits[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
            char tmp[65];
            int i = 64;
            tmp[i] = '\0';

            if (val == 0) {
                tmp[--i] = '0';
            } else {
                while (val > 0) {
                    tmp[--i] = digits[val % radix];
                    val /= radix;
                }
            }
            if (negative)
                tmp[--i] = '-';
            strcpy(buf, &tmp[i]);
        }
    } else if (num.type == YAS_ATOM_TYPE_FLOAT) {
        snprintf(buf, sizeof(buf), "%g", num.value.floatval);
    } else {
        return yas_raise_error("type-error", "number->string: expected number");
    }

    *result = yas_make_string(buf);
    return YAS_ERROR_OK;
}

int yas_builtin_number_to_string(YasAtom args, YasAtom *result)
{
    int r = yas_builtin_number_to_string_impl(args, result);
    return r;
}


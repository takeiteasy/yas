#include "lisp.h"
#include <string.h>
#include <stdio.h>

static inline vec4_t vec4_splat(float val) {
    vec4_t result;
#ifdef SIMD_X86
    result.v = _mm_set1_ps(val);
#elif defined(SIMD_ARM_NEON)
    result.v = vdupq_n_f32(val);
#elif defined(SIMD_WASM)
    result.v = wasm_f32x4_splat(val);
#else
    result.data[0] = result.data[1] = result.data[2] = result.data[3] = val;
#endif
    return result;
}

static inline void vec4_store(float* ptr, vec4_t v) {
#ifdef SIMD_X86
    _mm_storeu_ps(ptr, v.v);
#elif defined(SIMD_ARM_NEON)
    vst1q_f32(ptr, v.v);
#elif defined(SIMD_WASM)
    wasm_v128_store(ptr, v.v);
#else
    ptr[0] = v.data[0]; ptr[1] = v.data[1];
    ptr[2] = v.data[2]; ptr[3] = v.data[3];
#endif
}

static inline vec4_t vec4_add(vec4_t a, vec4_t b) {
    vec4_t result;
#ifdef SIMD_X86
    result.v = _mm_add_ps(a.v, b.v);
#elif defined(SIMD_ARM_NEON)
    result.v = vaddq_f32(a.v, b.v);
#elif defined(SIMD_WASM)
    result.v = wasm_f32x4_add(a.v, b.v);
#else
    for (int i = 0; i < 4; i++) result.data[i] = a.data[i] + b.data[i];
#endif
    return result;
}

static inline vec4_t vec4_sub(vec4_t a, vec4_t b) {
    vec4_t result;
#ifdef SIMD_X86
    result.v = _mm_sub_ps(a.v, b.v);
#elif defined(SIMD_ARM_NEON)
    result.v = vsubq_f32(a.v, b.v);
#elif defined(SIMD_WASM)
    result.v = wasm_f32x4_sub(a.v, b.v);
#else
    for (int i = 0; i < 4; i++) result.data[i] = a.data[i] - b.data[i];
#endif
    return result;
}

static inline vec4_t vec4_mul(vec4_t a, vec4_t b) {
    vec4_t result;
#ifdef SIMD_X86
    result.v = _mm_mul_ps(a.v, b.v);
#elif defined(SIMD_ARM_NEON)
    result.v = vmulq_f32(a.v, b.v);
#elif defined(SIMD_WASM)
    result.v = wasm_f32x4_mul(a.v, b.v);
#else
    for (int i = 0; i < 4; i++) result.data[i] = a.data[i] * b.data[i];
#endif
    return result;
}

static inline vec4_t vec4_div(vec4_t a, vec4_t b) {
    vec4_t result;
#ifdef SIMD_X86
    result.v = _mm_div_ps(a.v, b.v);
#elif defined(SIMD_ARM_NEON)
    result.v = vdivq_f32(a.v, b.v);
#elif defined(SIMD_WASM)
    result.v = wasm_f32x4_div(a.v, b.v);
#else
    for (int i = 0; i < 4; i++) result.data[i] = a.data[i] / b.data[i];
#endif
    return result;
}

/* Helper: check if type is any vec type */
static int is_vec_type(int type)
{
	return type == AtomType_Vec2 || type == AtomType_Vec3 || type == AtomType_Vec4;
}

/* Helper: get float value from number */
static float num_to_float(Atom a)
{
	if (a.type == AtomType_Integer) return (float)a.value.integer;
	return (float)a.value.floatval;
}

/* Helper: check if type is numeric */
static int is_numeric(int type)
{
	return type == AtomType_Integer || type == AtomType_Float;
}

int builtin_add(Atom args, Atom *result)
{
	Atom a, b;

	if (nilp(args) || nilp(cdr(args)) || !nilp(cdr(cdr(args))))
		return raise_error("args-error", "+: expected 2 arguments");

	a = car(args);
	b = car(cdr(args));

	/* Vector + Vector */
	if (is_vec_type(a.type) && is_vec_type(b.type)) {
		if (a.type != b.type)
			return raise_error("type-error", "+: vectors must have same dimension");
		
		vec4_t rv = vec4_add(*a.value.vec, *b.value.vec);
		float vals[4];
		vec4_store(vals, rv);
		if (a.type == AtomType_Vec2)
			*result = make_vec2(vals[0], vals[1]);
		else if (a.type == AtomType_Vec3)
			*result = make_vec3(vals[0], vals[1], vals[2]);
		else
			*result = make_vec4(vals[0], vals[1], vals[2], vals[3]);
		return Error_OK;
	}
	
	/* Vector + Scalar */
	if (is_vec_type(a.type) && is_numeric(b.type)) {
		float s = num_to_float(b);
		vec4_t sv = vec4_splat(s);
		vec4_t rv = vec4_add(*a.value.vec, sv);
		float vals[4];
		vec4_store(vals, rv);
		if (a.type == AtomType_Vec2)
			*result = make_vec2(vals[0], vals[1]);
		else if (a.type == AtomType_Vec3)
			*result = make_vec3(vals[0], vals[1], vals[2]);
		else
			*result = make_vec4(vals[0], vals[1], vals[2], vals[3]);
		return Error_OK;
	}
	
	/* Scalar + Vector */
	if (is_numeric(a.type) && is_vec_type(b.type)) {
		float s = num_to_float(a);
		vec4_t sv = vec4_splat(s);
		vec4_t rv = vec4_add(sv, *b.value.vec);
		float vals[4];
		vec4_store(vals, rv);
		if (b.type == AtomType_Vec2)
			*result = make_vec2(vals[0], vals[1]);
		else if (b.type == AtomType_Vec3)
			*result = make_vec3(vals[0], vals[1], vals[2]);
		else
			*result = make_vec4(vals[0], vals[1], vals[2], vals[3]);
		return Error_OK;
	}
	
	/* Integer + Integer */
	if (a.type == AtomType_Integer && b.type == AtomType_Integer) {
		*result = make_int(a.value.integer + b.value.integer);
		return Error_OK;
	}
	
	/* Float arithmetic (if either is float) */
	if (is_numeric(a.type) && is_numeric(b.type)) {
		double av = (a.type == AtomType_Integer) ? (double)a.value.integer : a.value.floatval;
		double bv = (b.type == AtomType_Integer) ? (double)b.value.integer : b.value.floatval;
		*result = make_float(av + bv);
		return Error_OK;
	}

	return raise_error("type-error", "+: operands must be numbers or vectors");
}


int builtin_subtract(Atom args, Atom *result)
{
	Atom a, b;

	if (nilp(args) || nilp(cdr(args)) || !nilp(cdr(cdr(args))))
		return raise_error("args-error", "-: expected 2 arguments");

	a = car(args);
	b = car(cdr(args));

	/* Vector - Vector */
	if (is_vec_type(a.type) && is_vec_type(b.type)) {
		if (a.type != b.type)
			return raise_error("type-error", "-: vectors must have same dimension");
		
		vec4_t rv = vec4_sub(*a.value.vec, *b.value.vec);
		float vals[4];
		vec4_store(vals, rv);
		if (a.type == AtomType_Vec2)
			*result = make_vec2(vals[0], vals[1]);
		else if (a.type == AtomType_Vec3)
			*result = make_vec3(vals[0], vals[1], vals[2]);
		else
			*result = make_vec4(vals[0], vals[1], vals[2], vals[3]);
		return Error_OK;
	}
	
	/* Vector - Scalar */
	if (is_vec_type(a.type) && is_numeric(b.type)) {
		float s = num_to_float(b);
		vec4_t sv = vec4_splat(s);
		vec4_t rv = vec4_sub(*a.value.vec, sv);
		float vals[4];
		vec4_store(vals, rv);
		if (a.type == AtomType_Vec2)
			*result = make_vec2(vals[0], vals[1]);
		else if (a.type == AtomType_Vec3)
			*result = make_vec3(vals[0], vals[1], vals[2]);
		else
			*result = make_vec4(vals[0], vals[1], vals[2], vals[3]);
		return Error_OK;
	}
	
	/* Integer - Integer */
	if (a.type == AtomType_Integer && b.type == AtomType_Integer) {
		*result = make_int(a.value.integer - b.value.integer);
		return Error_OK;
	}
	
	/* Float arithmetic */
	if (is_numeric(a.type) && is_numeric(b.type)) {
		double av = (a.type == AtomType_Integer) ? (double)a.value.integer : a.value.floatval;
		double bv = (b.type == AtomType_Integer) ? (double)b.value.integer : b.value.floatval;
		*result = make_float(av - bv);
		return Error_OK;
	}

	return raise_error("type-error", "-: operands must be numbers or vectors");
}


int builtin_multiply(Atom args, Atom *result)
{
	Atom a, b;

	if (nilp(args) || nilp(cdr(args)) || !nilp(cdr(cdr(args))))
		return raise_error("args-error", "*: expected 2 arguments");

	a = car(args);
	b = car(cdr(args));

	/* Vector * Vector (component-wise) */
	if (is_vec_type(a.type) && is_vec_type(b.type)) {
		if (a.type != b.type)
			return raise_error("type-error", "*: vectors must have same dimension");
		
		vec4_t rv = vec4_mul(*a.value.vec, *b.value.vec);
		float vals[4];
		vec4_store(vals, rv);
		if (a.type == AtomType_Vec2)
			*result = make_vec2(vals[0], vals[1]);
		else if (a.type == AtomType_Vec3)
			*result = make_vec3(vals[0], vals[1], vals[2]);
		else
			*result = make_vec4(vals[0], vals[1], vals[2], vals[3]);
		return Error_OK;
	}
	
	/* Vector * Scalar */
	if (is_vec_type(a.type) && is_numeric(b.type)) {
		float s = num_to_float(b);
		vec4_t sv = vec4_splat(s);
		vec4_t rv = vec4_mul(*a.value.vec, sv);
		float vals[4];
		vec4_store(vals, rv);
		if (a.type == AtomType_Vec2)
			*result = make_vec2(vals[0], vals[1]);
		else if (a.type == AtomType_Vec3)
			*result = make_vec3(vals[0], vals[1], vals[2]);
		else
			*result = make_vec4(vals[0], vals[1], vals[2], vals[3]);
		return Error_OK;
	}
	
	/* Scalar * Vector */
	if (is_numeric(a.type) && is_vec_type(b.type)) {
		float s = num_to_float(a);
		vec4_t sv = vec4_splat(s);
		vec4_t rv = vec4_mul(sv, *b.value.vec);
		float vals[4];
		vec4_store(vals, rv);
		if (b.type == AtomType_Vec2)
			*result = make_vec2(vals[0], vals[1]);
		else if (b.type == AtomType_Vec3)
			*result = make_vec3(vals[0], vals[1], vals[2]);
		else
			*result = make_vec4(vals[0], vals[1], vals[2], vals[3]);
		return Error_OK;
	}
	
	/* Integer * Integer */
	if (a.type == AtomType_Integer && b.type == AtomType_Integer) {
		*result = make_int(a.value.integer * b.value.integer);
		return Error_OK;
	}
	
	/* Float arithmetic */
	if (is_numeric(a.type) && is_numeric(b.type)) {
		double av = (a.type == AtomType_Integer) ? (double)a.value.integer : a.value.floatval;
		double bv = (b.type == AtomType_Integer) ? (double)b.value.integer : b.value.floatval;
		*result = make_float(av * bv);
		return Error_OK;
	}

	return raise_error("type-error", "*: operands must be numbers or vectors");
}


int builtin_divide(Atom args, Atom *result)
{
	Atom a, b;

	if (nilp(args) || nilp(cdr(args)) || !nilp(cdr(cdr(args))))
		return raise_error("args-error", "/: expected 2 arguments");

	a = car(args);
	b = car(cdr(args));

	/* Vector / Vector (component-wise) */
	if (is_vec_type(a.type) && is_vec_type(b.type)) {
		if (a.type != b.type)
			return raise_error("type-error", "/: vectors must have same dimension");
		
		vec4_t rv = vec4_div(*a.value.vec, *b.value.vec);
		float vals[4];
		vec4_store(vals, rv);
		if (a.type == AtomType_Vec2)
			*result = make_vec2(vals[0], vals[1]);
		else if (a.type == AtomType_Vec3)
			*result = make_vec3(vals[0], vals[1], vals[2]);
		else
			*result = make_vec4(vals[0], vals[1], vals[2], vals[3]);
		return Error_OK;
	}
	
	/* Vector / Scalar */
	if (is_vec_type(a.type) && is_numeric(b.type)) {
		float s = num_to_float(b);
		if (s == 0.0f)
			return raise_error("arithmetic-error", "/: division by zero");
		vec4_t sv = vec4_splat(s);
		vec4_t rv = vec4_div(*a.value.vec, sv);
		float vals[4];
		vec4_store(vals, rv);
		if (a.type == AtomType_Vec2)
			*result = make_vec2(vals[0], vals[1]);
		else if (a.type == AtomType_Vec3)
			*result = make_vec3(vals[0], vals[1], vals[2]);
		else
			*result = make_vec4(vals[0], vals[1], vals[2], vals[3]);
		return Error_OK;
	}
	
	/* Integer / Integer */
	if (a.type == AtomType_Integer && b.type == AtomType_Integer) {
		if (b.value.integer == 0)
			return raise_error("arithmetic-error", "/: division by zero");
		*result = make_int(a.value.integer / b.value.integer);
		return Error_OK;
	}
	
	/* Float arithmetic */
	if (is_numeric(a.type) && is_numeric(b.type)) {
		double av = (a.type == AtomType_Integer) ? (double)a.value.integer : a.value.floatval;
		double bv = (b.type == AtomType_Integer) ? (double)b.value.integer : b.value.floatval;
		if (bv == 0.0)
			return raise_error("arithmetic-error", "/: division by zero");
		*result = make_float(av / bv);
		return Error_OK;
	}

	return raise_error("type-error", "/: operands must be numbers or vectors");
}


int builtin_numeq(Atom args, Atom *result)
{
	Atom a, b;

	if (nilp(args) || nilp(cdr(args)) || !nilp(cdr(cdr(args))))
		return raise_error("args-error", "=: expected 2 arguments");

	a = car(args);
	b = car(cdr(args));

	/* Integer == Integer */
	if (a.type == AtomType_Integer && b.type == AtomType_Integer) {
		*result = (a.value.integer == b.value.integer) ? make_sym("T") : nil;
		return Error_OK;
	}
	
	/* Float comparison (if either is float or integer) */
	if (is_numeric(a.type) && is_numeric(b.type)) {
		double av = (a.type == AtomType_Integer) ? (double)a.value.integer : a.value.floatval;
		double bv = (b.type == AtomType_Integer) ? (double)b.value.integer : b.value.floatval;
		*result = (av == bv) ? make_sym("T") : nil;
		return Error_OK;
	}

	return raise_error("type-error", "=: operands must be numbers");
}

int builtin_less(Atom args, Atom *result)
{
	Atom a, b;

	if (nilp(args) || nilp(cdr(args)) || !nilp(cdr(cdr(args))))
		return raise_error("args-error", "<: expected 2 arguments");

	a = car(args);
	b = car(cdr(args));

	if (a.type != AtomType_Integer || b.type != AtomType_Integer)
		return raise_error("type-error", "<: operands must be integers");

	*result = (a.value.integer < b.value.integer) ? make_sym("T") : nil;

	return Error_OK;
}

/* Type predicates */
int builtin_floatp(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "float?: expected 1 argument");
	*result = (car(args).type == AtomType_Float) ? make_sym("T") : nil;
	return Error_OK;
}

int builtin_integerp(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "integer?: expected 1 argument");
	*result = (car(args).type == AtomType_Integer) ? make_sym("T") : nil;
	return Error_OK;
}

/* ============================================================
 * SIMD VECTOR/MATRIX BUILTINS
 * ============================================================ */

/* Helper to extract float from vec component */
static float vec_get_component(Atom vec, int idx)
{
#ifdef SIMD_SCALAR
	return vec.value.vec->data[idx];
#else
	float vals[4];
	#ifdef SIMD_X86
	_mm_storeu_ps(vals, vec.value.vec->v);
	#elif defined(SIMD_ARM_NEON)
	vst1q_f32(vals, vec.value.vec->v);
	#elif defined(SIMD_WASM)
	wasm_v128_store(vals, vec.value.vec->v);
	#endif
	return vals[idx];
#endif
}

/* Type predicates */
int builtin_vec2p(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "vec2?: expected 1 argument");
	*result = (car(args).type == AtomType_Vec2) ? make_sym("T") : nil;
	return Error_OK;
}

int builtin_vec3p(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "vec3?: expected 1 argument");
	*result = (car(args).type == AtomType_Vec3) ? make_sym("T") : nil;
	return Error_OK;
}

int builtin_vec4p(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "vec4?: expected 1 argument");
	*result = (car(args).type == AtomType_Vec4) ? make_sym("T") : nil;
	return Error_OK;
}

int builtin_mat4p(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "mat4?: expected 1 argument");
	*result = (car(args).type == AtomType_Mat4) ? make_sym("T") : nil;
	return Error_OK;
}

/* Constructors */
int builtin_vec2(Atom args, Atom *result)
{
	if (nilp(args) || nilp(cdr(args)) || !nilp(cdr(cdr(args))))
		return raise_error("args-error", "vec2: expected 2 arguments");
	
	Atom x = car(args), y = car(cdr(args));
	float fx, fy;
	
	if (x.type == AtomType_Integer) fx = (float)x.value.integer;
	else if (x.type == AtomType_Float) fx = (float)x.value.floatval;
	else return raise_error("type-error", "vec2: arguments must be numbers");
	
	if (y.type == AtomType_Integer) fy = (float)y.value.integer;
	else if (y.type == AtomType_Float) fy = (float)y.value.floatval;
	else return raise_error("type-error", "vec2: arguments must be numbers");
	
	*result = make_vec2(fx, fy);
	return Error_OK;
}

int builtin_vec3(Atom args, Atom *result)
{
	Atom x = car(args), y = car(cdr(args)), z = car(cdr(cdr(args)));
	float fx, fy, fz;
	
	if (x.type == AtomType_Integer) fx = (float)x.value.integer;
	else if (x.type == AtomType_Float) fx = (float)x.value.floatval;
	else return raise_error("type-error", "vec3: arguments must be numbers");
	
	if (y.type == AtomType_Integer) fy = (float)y.value.integer;
	else if (y.type == AtomType_Float) fy = (float)y.value.floatval;
	else return raise_error("type-error", "vec3: arguments must be numbers");
	
	if (z.type == AtomType_Integer) fz = (float)z.value.integer;
	else if (z.type == AtomType_Float) fz = (float)z.value.floatval;
	else return raise_error("type-error", "vec3: arguments must be numbers");
	
	*result = make_vec3(fx, fy, fz);
	return Error_OK;
}

int builtin_vec4(Atom args, Atom *result)
{
	Atom x = car(args), y = car(cdr(args)), z = car(cdr(cdr(args))), w = car(cdr(cdr(cdr(args))));
	float fx, fy, fz, fw;
	
	if (x.type == AtomType_Integer) fx = (float)x.value.integer;
	else if (x.type == AtomType_Float) fx = (float)x.value.floatval;
	else return raise_error("type-error", "vec4: arguments must be numbers");
	
	if (y.type == AtomType_Integer) fy = (float)y.value.integer;
	else if (y.type == AtomType_Float) fy = (float)y.value.floatval;
	else return raise_error("type-error", "vec4: arguments must be numbers");
	
	if (z.type == AtomType_Integer) fz = (float)z.value.integer;
	else if (z.type == AtomType_Float) fz = (float)z.value.floatval;
	else return raise_error("type-error", "vec4: arguments must be numbers");
	
	if (w.type == AtomType_Integer) fw = (float)w.value.integer;
	else if (w.type == AtomType_Float) fw = (float)w.value.floatval;
	else return raise_error("type-error", "vec4: arguments must be numbers");
	
	*result = make_vec4(fx, fy, fz, fw);
	return Error_OK;
}

int builtin_mat4_identity(Atom args, Atom *result)
{
	if (!nilp(args))
		return raise_error("args-error", "mat4-identity: expected 0 arguments");
	*result = make_mat4_identity();
	return Error_OK;
}

/* Component accessors */
int builtin_vec_x(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "vec-x: expected 1 argument");
	Atom v = car(args);
	if (v.type != AtomType_Vec2 && v.type != AtomType_Vec3 && v.type != AtomType_Vec4)
		return raise_error("type-error", "vec-x: argument must be a vector");
	*result = make_float(vec_get_component(v, 0));
	return Error_OK;
}

int builtin_vec_y(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "vec-y: expected 1 argument");
	Atom v = car(args);
	if (v.type != AtomType_Vec2 && v.type != AtomType_Vec3 && v.type != AtomType_Vec4)
		return raise_error("type-error", "vec-y: argument must be a vector");
	*result = make_float(vec_get_component(v, 1));
	return Error_OK;
}

int builtin_vec_z(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "vec-z: expected 1 argument");
	Atom v = car(args);
	if (v.type != AtomType_Vec3 && v.type != AtomType_Vec4)
		return raise_error("type-error", "vec-z: argument must be vec3 or vec4");
	*result = make_float(vec_get_component(v, 2));
	return Error_OK;
}

int builtin_vec_w(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "vec-w: expected 1 argument");
	Atom v = car(args);
	if (v.type != AtomType_Vec4)
		return raise_error("type-error", "vec-w: argument must be vec4");
	*result = make_float(vec_get_component(v, 3));
	return Error_OK;
}

/* (vec-ref v idx) - get component by index */
int builtin_vec_ref(Atom args, Atom *result)
{
	if (nilp(args) || nilp(cdr(args)) || !nilp(cdr(cdr(args))))
		return raise_error("args-error", "vec-ref: expected 2 arguments");
	
	Atom v = car(args), idx_atom = car(cdr(args));
	if (v.type != AtomType_Vec2 && v.type != AtomType_Vec3 && v.type != AtomType_Vec4)
		return raise_error("type-error", "vec-ref: first argument must be a vector");
	if (idx_atom.type != AtomType_Integer)
		return raise_error("type-error", "vec-ref: index must be integer");
	
	int idx = (int)idx_atom.value.integer;
	int max = (v.type == AtomType_Vec2) ? 2 : (v.type == AtomType_Vec3) ? 3 : 4;
	if (idx < 0 || idx >= max)
		return raise_error("range-error", "vec-ref: index out of bounds");
	
	*result = make_float(vec_get_component(v, idx));
	return Error_OK;
}

/* Helper to set a vector component */
static void vec_set_component(Atom vec, int idx, float val)
{
	float vals[4];
	vec4_store(vals, *vec.value.vec);
	vals[idx] = val;
	/* Recreate the SIMD vector */
#ifdef SIMD_X86
	vec.value.vec->v = _mm_loadu_ps(vals);
#elif defined(SIMD_ARM_NEON)
	vec.value.vec->v = vld1q_f32(vals);
#elif defined(SIMD_WASM)
	vec.value.vec->v = wasm_v128_load(vals);
#else
	for (int i = 0; i < 4; i++) vec.value.vec->data[i] = vals[i];
#endif
}

/* (vec-set! v idx value) - set component by index (mutates in place) */
int builtin_vec_set(Atom args, Atom *result)
{
	if (nilp(args) || nilp(cdr(args)) || nilp(cdr(cdr(args))) || !nilp(cdr(cdr(cdr(args)))))
		return raise_error("args-error", "vec-set!: expected 3 arguments");
	
	Atom v = car(args), idx_atom = car(cdr(args)), val_atom = car(cdr(cdr(args)));
	if (v.type != AtomType_Vec2 && v.type != AtomType_Vec3 && v.type != AtomType_Vec4)
		return raise_error("type-error", "vec-set!: first argument must be a vector");
	if (idx_atom.type != AtomType_Integer)
		return raise_error("type-error", "vec-set!: index must be integer");
	if (!is_numeric(val_atom.type))
		return raise_error("type-error", "vec-set!: value must be a number");
	
	int idx = (int)idx_atom.value.integer;
	int max = (v.type == AtomType_Vec2) ? 2 : (v.type == AtomType_Vec3) ? 3 : 4;
	if (idx < 0 || idx >= max)
		return raise_error("range-error", "vec-set!: index out of bounds");
	
	float val = num_to_float(val_atom);
	vec_set_component(v, idx, val);
	*result = v;
	return Error_OK;
}

/* mat4 multiply: multiply two 4x4 matrices */
int builtin_mat4_multiply(Atom args, Atom *result)
{
	if (nilp(args) || nilp(cdr(args)) || !nilp(cdr(cdr(args))))
		return raise_error("args-error", "mat4*: expected 2 arguments");
	
	Atom a = car(args), b = car(cdr(args));
	if (a.type != AtomType_Mat4 || b.type != AtomType_Mat4)
		return raise_error("type-error", "mat4*: arguments must be mat4");
	
	/* Result matrix */
	Atom r = make_mat4_identity();
	
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
		
#ifdef SIMD_X86
		r.value.mat4->columns[col].v = _mm_loadu_ps(rc);
#elif defined(SIMD_ARM_NEON)
		r.value.mat4->columns[col].v = vld1q_f32(rc);
#elif defined(SIMD_WASM)
		r.value.mat4->columns[col].v = wasm_v128_load(rc);
#else
		for (int j = 0; j < 4; j++) r.value.mat4->columns[col].data[j] = rc[j];
#endif
	}
	
	*result = r;
	return Error_OK;
}


/* (mat4 e0 ... e15) - Create mat4 from 16 floats (column-major) */
int builtin_mat4(Atom args, Atom *result)
{
	float vals[16];
	Atom p = args;
	for (int i = 0; i < 16; i++) {
		if (nilp(p)) return raise_error("args-error", "mat4: expected 16 arguments");
		Atom val = car(p);
		if (val.type == AtomType_Integer) vals[i] = (float)val.value.integer;
		else if (val.type == AtomType_Float) vals[i] = (float)val.value.floatval;
		else return raise_error("type-error", "mat4: expected numbers");
		p = cdr(p);
	}
	*result = make_mat4_from_floats(vals);
	return Error_OK;
}

/* mat4 transpose */
int builtin_mat4_transpose(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "mat4-transpose: expected 1 argument");
	
	Atom m = car(args);
	if (m.type != AtomType_Mat4)
		return raise_error("type-error", "mat4-transpose: argument must be mat4");
	
	Atom r = make_mat4_identity();
	
	/* Transpose: swap rows and columns */
	for (int col = 0; col < 4; col++) {
		float mc[4];
		vec4_store(mc, m.value.mat4->columns[col]);
		for (int row = 0; row < 4; row++) {
			float rc[4];
			vec4_store(rc, r.value.mat4->columns[row]);
			rc[col] = mc[row];
#ifdef SIMD_X86
			r.value.mat4->columns[row].v = _mm_loadu_ps(rc);
#elif defined(SIMD_ARM_NEON)
			r.value.mat4->columns[row].v = vld1q_f32(rc);
#elif defined(SIMD_WASM)
			r.value.mat4->columns[row].v = wasm_v128_load(rc);
#else
			for (int j = 0; j < 4; j++) r.value.mat4->columns[row].data[j] = rc[j];
#endif
		}
	}
	
	*result = r;
	return Error_OK;
}

/* mat4 inverse - uses standard 4x4 matrix inversion algorithm */
int builtin_mat4_inverse(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "mat4-inverse: expected 1 argument");
	
	Atom m = car(args);
	if (m.type != AtomType_Mat4)
		return raise_error("type-error", "mat4-inverse: argument must be mat4");
	
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
	inv[0] = a[5]*a[10]*a[15] - a[5]*a[11]*a[14] - a[9]*a[6]*a[15]
	       + a[9]*a[7]*a[14] + a[13]*a[6]*a[11] - a[13]*a[7]*a[10];
	inv[4] = -a[4]*a[10]*a[15] + a[4]*a[11]*a[14] + a[8]*a[6]*a[15]
	       - a[8]*a[7]*a[14] - a[12]*a[6]*a[11] + a[12]*a[7]*a[10];
	inv[8] = a[4]*a[9]*a[15] - a[4]*a[11]*a[13] - a[8]*a[5]*a[15]
	       + a[8]*a[7]*a[13] + a[12]*a[5]*a[11] - a[12]*a[7]*a[9];
	inv[12] = -a[4]*a[9]*a[14] + a[4]*a[10]*a[13] + a[8]*a[5]*a[14]
	        - a[8]*a[6]*a[13] - a[12]*a[5]*a[10] + a[12]*a[6]*a[9];
	
	inv[1] = -a[1]*a[10]*a[15] + a[1]*a[11]*a[14] + a[9]*a[2]*a[15]
	       - a[9]*a[3]*a[14] - a[13]*a[2]*a[11] + a[13]*a[3]*a[10];
	inv[5] = a[0]*a[10]*a[15] - a[0]*a[11]*a[14] - a[8]*a[2]*a[15]
	       + a[8]*a[3]*a[14] + a[12]*a[2]*a[11] - a[12]*a[3]*a[10];
	inv[9] = -a[0]*a[9]*a[15] + a[0]*a[11]*a[13] + a[8]*a[1]*a[15]
	       - a[8]*a[3]*a[13] - a[12]*a[1]*a[11] + a[12]*a[3]*a[9];
	inv[13] = a[0]*a[9]*a[14] - a[0]*a[10]*a[13] - a[8]*a[1]*a[14]
	        + a[8]*a[2]*a[13] + a[12]*a[1]*a[10] - a[12]*a[2]*a[9];
	
	inv[2] = a[1]*a[6]*a[15] - a[1]*a[7]*a[14] - a[5]*a[2]*a[15]
	       + a[5]*a[3]*a[14] + a[13]*a[2]*a[7] - a[13]*a[3]*a[6];
	inv[6] = -a[0]*a[6]*a[15] + a[0]*a[7]*a[14] + a[4]*a[2]*a[15]
	       - a[4]*a[3]*a[14] - a[12]*a[2]*a[7] + a[12]*a[3]*a[6];
	inv[10] = a[0]*a[5]*a[15] - a[0]*a[7]*a[13] - a[4]*a[1]*a[15]
	        + a[4]*a[3]*a[13] + a[12]*a[1]*a[7] - a[12]*a[3]*a[5];
	inv[14] = -a[0]*a[5]*a[14] + a[0]*a[6]*a[13] + a[4]*a[1]*a[14]
	        - a[4]*a[2]*a[13] - a[12]*a[1]*a[6] + a[12]*a[2]*a[5];
	
	inv[3] = -a[1]*a[6]*a[11] + a[1]*a[7]*a[10] + a[5]*a[2]*a[11]
	       - a[5]*a[3]*a[10] - a[9]*a[2]*a[7] + a[9]*a[3]*a[6];
	inv[7] = a[0]*a[6]*a[11] - a[0]*a[7]*a[10] - a[4]*a[2]*a[11]
	       + a[4]*a[3]*a[10] + a[8]*a[2]*a[7] - a[8]*a[3]*a[6];
	inv[11] = -a[0]*a[5]*a[11] + a[0]*a[7]*a[9] + a[4]*a[1]*a[11]
	        - a[4]*a[3]*a[9] - a[8]*a[1]*a[7] + a[8]*a[3]*a[5];
	inv[15] = a[0]*a[5]*a[10] - a[0]*a[6]*a[9] - a[4]*a[1]*a[10]
	        + a[4]*a[2]*a[9] + a[8]*a[1]*a[6] - a[8]*a[2]*a[5];
	
	float det = a[0]*inv[0] + a[1]*inv[4] + a[2]*inv[8] + a[3]*inv[12];
	
	if (det == 0.0f) {
		return raise_error("arithmetic-error", "mat4-inverse: matrix is not invertible (determinant is 0)");
	}
	
	float inv_det = 1.0f / det;
	for (int i = 0; i < 16; i++) {
		inv[i] *= inv_det;
	}
	
	*result = make_mat4_from_floats(inv);
	return Error_OK;
}

/* ============================================================
 * Format String Helper Builtins
 * ============================================================ */

/* (number->string n [radix]) - convert number to string with optional radix */
int builtin_number_to_string(Atom args, Atom *result)
{
	if (nilp(args))
		return raise_error("args-error", "number->string: expected at least 1 argument");
	
	Atom num = car(args);
	int radix = 10;
	
	if (!nilp(cdr(args))) {
		Atom r = car(cdr(args));
		if (r.type != AtomType_Integer)
			return raise_error("type-error", "number->string: radix must be integer");
		radix = (int)r.value.integer;
		if (radix < 2 || radix > 36)
			return raise_error("args-error", "number->string: radix must be 2-36");
	}
	
	char buf[128];
	
	if (num.type == AtomType_Integer) {
		long val = num.value.integer;
		int negative = val < 0;
		if (negative) val = -val;
		
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
			if (negative) tmp[--i] = '-';
			strcpy(buf, &tmp[i]);
		}
	} else if (num.type == AtomType_Float) {
		snprintf(buf, sizeof(buf), "%g", num.value.floatval);
	} else {
		return raise_error("type-error", "number->string: expected number");
	}
	
	*result = make_string(buf);
	return Error_OK;
}

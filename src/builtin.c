#include "lisp.h"
#include <stdlib.h>
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

int raise_error(const char *type, const char *msg)
{
	extern const char *current_file_path;
	
	Atom t = make_sym(type);
	Atom m = make_string(msg);
	
	/* Include file path in error message if available */
	if (current_file_path) {
		char *full_msg = malloc(strlen(msg) + strlen(current_file_path) + 50);
		snprintf(full_msg, strlen(msg) + strlen(current_file_path) + 50, 
		         "%s (in %s)", msg, current_file_path);
		m = make_string(full_msg);
		free(full_msg);
	}
	
	ctx_exception = cons(t, cons(m, nil));
	return Error_Exception;
}

int builtin_car(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "car: expected 1 argument");

	if (nilp(car(args)))
		*result = nil;
	else if (car(args).type != AtomType_Pair)
		return raise_error("type-error", "car: argument must be pair");
	else
		*result = car(car(args));

	return Error_OK;
}

int builtin_cdr(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "cdr: expected 1 argument");

	if (nilp(car(args)))
		*result = nil;
	else if (car(args).type != AtomType_Pair)
		return raise_error("type-error", "cdr: argument must be pair");
	else
		*result = cdr(car(args));

	return Error_OK;
}

int builtin_cons(Atom args, Atom *result)
{
	if (nilp(args) || nilp(cdr(args)) || !nilp(cdr(cdr(args))))
		return raise_error("args-error", "cons: expected 2 arguments");

	*result = cons(car(args), car(cdr(args)));

	return Error_OK;
}

int builtin_eq(Atom args, Atom *result)
{
	Atom a, b;
	int eq;

	if (nilp(args) || nilp(cdr(args)) || !nilp(cdr(cdr(args))))
		return raise_error("args-error", "eq?: expected 2 arguments");

	a = car(args);
	b = car(cdr(args));

	if (a.type == b.type) {
		switch (a.type) {
		case AtomType_Nil:
			eq = 1;
			break;
		case AtomType_Pair:
		case AtomType_Closure:
		case AtomType_Macro:
			eq = (a.value.pair == b.value.pair);
			break;
		case AtomType_Symbol:
			eq = (a.value.symbol == b.value.symbol);
			break;
		case AtomType_Keyword:
			eq = (a.value.keyword == b.value.keyword);
			break;
		case AtomType_Integer:
			eq = (a.value.integer == b.value.integer);
			break;
		case AtomType_Builtin:
			eq = (a.value.builtin == b.value.builtin);
			break;
		default:
			eq = 0;
			break;
		}
	} else {
		eq = 0;
	}

	*result = eq ? make_sym("T") : nil;
	return Error_OK;
}

int builtin_pairp(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "pair?: expected 1 argument");

	*result = (car(args).type == AtomType_Pair) ? make_sym("T") : nil;
	return Error_OK;
}

int builtin_procp(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "procedure?: expected 1 argument");

	*result = (car(args).type == AtomType_Builtin
		|| car(args).type == AtomType_Closure) ? make_sym("T") : nil;
	return Error_OK;
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

int builtin_stringp(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "string?: expected 1 argument");
	*result = (car(args).type == AtomType_String) ? make_sym("T") : nil;
	return Error_OK;
}

int builtin_vectorp(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "vector?: expected 1 argument");
	*result = (car(args).type == AtomType_Vector) ? make_sym("T") : nil;
	return Error_OK;
}

int builtin_hashp(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "hash?: expected 1 argument");
	*result = (car(args).type == AtomType_Hash) ? make_sym("T") : nil;
	return Error_OK;
}

/* (string-length str) */
int builtin_string_length(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "string-length: expected 1 argument");
	Atom a = car(args);
	if (a.type != AtomType_String)
		return raise_error("type-error", "string-length: expected string");
	*result = make_int(utf8_strlen(a.value.string));
	return Error_OK;
}

/* (vector-length vec) */
int builtin_vector_length(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "vector-length: expected 1 argument");
	Atom a = car(args);
	if (a.type != AtomType_Vector)
		return raise_error("type-error", "vector-length: expected vector");
	*result = make_int(vector_length(a));
	return Error_OK;
}

/* (hash-length hash) */
int builtin_hash_length(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "hash-length: expected 1 argument");
	Atom a = car(args);
	if (a.type != AtomType_Hash)
		return raise_error("type-error", "hash-length: expected hash");
	*result = make_int(hash_size(a));
	return Error_OK;
}

/* (length list) */
int builtin_list_length(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "length: expected 1 argument");
	Atom a = car(args);
	long len = 0;
	while (!nilp(a)) {
		if (a.type != AtomType_Pair) return raise_error("type-error", "length: improper list");
		len++;
		a = cdr(a);
	}
	*result = make_int(len);
	return Error_OK;
}

/* (string-ref str k) - get k-th character (0-indexed) */
int builtin_string_ref(Atom args, Atom *result) {
	if (nilp(args) || nilp(cdr(args)) || !nilp(cdr(cdr(args))))
		return raise_error("args-error", "string-ref: expected 2 arguments");
	
	Atom str = car(args);
	Atom idx = car(cdr(args));
	
	if (str.type != AtomType_String)
		return raise_error("type-error", "string-ref: first argument must be string");
	if (idx.type != AtomType_Integer)
		return raise_error("type-error", "string-ref: second argument must be integer");
	
	int32_t cp = utf8_codepoint_at(str.value.string, (size_t)idx.value.integer);
	if (cp < 0)
		return raise_error("args-error", "string-ref: index out of bounds");
	
	*result = make_char(cp);
	return Error_OK;
}

/* (string=? s1 s2) */
int builtin_string_eq(Atom args, Atom *result) {
	if (nilp(args) || nilp(cdr(args)) || !nilp(cdr(cdr(args))))
		return raise_error("args-error", "string=?: expected 2 arguments");
	
	Atom a = car(args);
	Atom b = car(cdr(args));
	
	if (a.type != AtomType_String || b.type != AtomType_String)
		return raise_error("type-error", "string=?: arguments must be strings");
	
	*result = (strcmp(a.value.string, b.value.string) == 0) ? make_sym("T") : nil;
	return Error_OK;
}

/* (string-byte-length str) - byte length (for I/O) */
int builtin_string_byte_length(Atom args, Atom *result) {
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "string-byte-length: expected 1 argument");
	
	Atom str = car(args);
	if (str.type != AtomType_String)
		return raise_error("type-error", "string-byte-length: argument must be string");
	
	*result = make_int(strlen(str.value.string));
	return Error_OK;
}

/* (string-append s1 s2 ...) */
int builtin_string_append(Atom args, Atom *result)
{
	size_t total_len = 0;
	Atom p = args;
	while (!nilp(p)) {
		if (car(p).type != AtomType_String)
			return raise_error("type-error", "string-append: arguments must be strings");
		total_len += strlen(car(p).value.string);
		p = cdr(p);
	}
	char *buf = malloc(total_len + 1);
	buf[0] = '\0';
	p = args;
	while (!nilp(p)) {
		strcat(buf, car(p).value.string);
		p = cdr(p);
	}
	result->type = AtomType_String;
	result->value.string = buf;
	return Error_OK;
}

/* (append l1 l2 ...) */
int builtin_list_append(Atom args, Atom *result)
{
	Atom head = nil, tail = nil;
	Atom p = args;
	while (!nilp(p)) {
		Atom list = car(p);
		while (!nilp(list)) {
			if (list.type != AtomType_Pair)
				return raise_error("type-error", "append: arguments must be lists");
			Atom new_cons = cons(car(list), nil);
			if (nilp(head)) {
				head = tail = new_cons;
			} else {
				cdr(tail) = new_cons;
				tail = new_cons;
			}
			list = cdr(list);
		}
		p = cdr(p);
	}
	*result = head;
	return Error_OK;
}

/* (vector-append v1 v2 ...) */
int builtin_vector_append(Atom args, Atom *result)
{
	size_t total_len = 0;
	Atom p = args;
	while (!nilp(p)) {
		if (car(p).type != AtomType_Vector)
			return raise_error("type-error", "vector-append: arguments must be vectors");
		total_len += vector_length(car(p));
		p = cdr(p);
	}
	*result = make_vector(total_len, nil);
	size_t idx = 0;
	p = args;
	while (!nilp(p)) {
		Atom v = car(p);
		for (size_t i = 0; i < vector_length(v); i++) {
			vector_set(*result, idx++, vector_ref(v, i));
		}
		p = cdr(p);
	}
	return Error_OK;
}

/* (hash-join h1 h2 ...) - merges h2 into h1, but since we are functional-ish, 
   lets return NEW hash with all contents. Last one wins. */
int builtin_hash_join(Atom args, Atom *result)
{
	Atom new_h = make_hash();
	Atom p = args;
	while (!nilp(p)) {
		Atom h = car(p);
		if (h.type != AtomType_Hash)
			return raise_error("type-error", "hash-join: arguments must be hashes");
		
		struct Hash *src = h.value.hash;
		for (size_t i = 0; i < src->size; i++) {
			hash_set(new_h, src->keys[i], src->values[i]);
		}
		p = cdr(p);
	}
	*result = new_h;
	return Error_OK;
}

/* Vector operations */
int builtin_make_vector(Atom args, Atom *result)
{
	if (nilp(args))
		return raise_error("args-error", "make-vector: expected size argument");
	
	Atom size_arg = car(args);
	if (size_arg.type != AtomType_Integer)
		return raise_error("type-error", "make-vector: size must be integer");
	
	Atom fill = nil;
	if (!nilp(cdr(args))) {
		fill = car(cdr(args));
		if (!nilp(cdr(cdr(args))))
			return raise_error("args-error", "make-vector: expected 1 or 2 arguments");
	}
	
	*result = make_vector((size_t)size_arg.value.integer, fill);
	return Error_OK;
}

int builtin_vector_ref(Atom args, Atom *result)
{
	if (nilp(args) || nilp(cdr(args)) || !nilp(cdr(cdr(args))))
		return raise_error("args-error", "vector-ref: expected 2 arguments");
	
	Atom vec = car(args);
	Atom idx = car(cdr(args));
	
	if (vec.type != AtomType_Vector || idx.type != AtomType_Integer)
		return raise_error("type-error", "vector-ref: arguments must be vector and integer");
	
	if (idx.value.integer < 0 || (size_t)idx.value.integer >= vector_length(vec))
		return raise_error("args-error", "vector-ref: index out of bounds");
	
	*result = vector_ref(vec, (size_t)idx.value.integer);
	return Error_OK;
}

int builtin_vector_set(Atom args, Atom *result)
{
	if (nilp(args) || nilp(cdr(args)) || nilp(cdr(cdr(args))) ||
	    !nilp(cdr(cdr(cdr(args)))))
		return raise_error("args-error", "vector-set!: expected 3 arguments");
	
	Atom vec = car(args);
	Atom idx = car(cdr(args));
	Atom val = car(cdr(cdr(args)));
	
	if (vec.type != AtomType_Vector || idx.type != AtomType_Integer)
		return raise_error("type-error", "vector-set!: arguments must be vector, integer and value");
	
	if (idx.value.integer < 0 || (size_t)idx.value.integer >= vector_length(vec))
		return raise_error("args-error", "vector-set!: index out of bounds");
	
	vector_set(vec, (size_t)idx.value.integer, val);
	*result = val;
	return Error_OK;
}

/* Hash operations */
int builtin_make_hash(Atom args, Atom *result)
{
	if (!nilp(args))
		return raise_error("args-error", "make-hash: expected 0 arguments");
	
	*result = make_hash();
	return Error_OK;
}

int builtin_hash_ref(Atom args, Atom *result)
{
	if (nilp(args) || nilp(cdr(args)))
		return raise_error("args-error", "hash-ref: expected at least 2 arguments");
	
	Atom hash = car(args);
	Atom key = car(cdr(args));
	Atom default_val = nil;
	
	if (!nilp(cdr(cdr(args)))) {
		default_val = car(cdr(cdr(args)));
		if (!nilp(cdr(cdr(cdr(args)))))
			return raise_error("args-error", "hash-ref: expected 2 or 3 arguments");
	}
	
	if (hash.type != AtomType_Hash)
		return raise_error("type-error", "hash-ref: first argument must be hash");
	
	*result = hash_ref(hash, key, default_val);
	return Error_OK;
}

int builtin_hash_set(Atom args, Atom *result)
{
	if (nilp(args) || nilp(cdr(args)) || nilp(cdr(cdr(args))) ||
	    !nilp(cdr(cdr(cdr(args)))))
		return raise_error("args-error", "hash-set!: expected 3 arguments");
	
	Atom hash = car(args);
	Atom key = car(cdr(args));
	Atom val = car(cdr(cdr(args)));
	
	if (hash.type != AtomType_Hash)
		return raise_error("type-error", "hash-set!: first argument must be hash");
	
	hash_set(hash, key, val);
	*result = val;
	return Error_OK;
}

int builtin_hash_remove(Atom args, Atom *result)
{
	if (nilp(args) || nilp(cdr(args)) || !nilp(cdr(cdr(args))))
		return raise_error("args-error", "hash-remove!: expected 2 arguments");
	
	Atom hash = car(args);
	Atom key = car(cdr(args));
	
	if (hash.type != AtomType_Hash)
		return raise_error("type-error", "hash-remove!: first argument must be hash");
	
	*result = hash_remove(hash, key) ? make_sym("T") : nil;
	return Error_OK;
}

int builtin_hash_keys(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "hash-keys: expected 1 argument");
	
	Atom hash = car(args);
	if (hash.type != AtomType_Hash)
		return raise_error("type-error", "hash-keys: argument must be hash");
	
	Atom keys = nil;
	struct Hash *h = hash.value.hash;
	
	for (size_t i = 0; i < h->size; i++) {
		keys = cons(h->keys[i], keys);
	}
	
	*result = keys;
	return Error_OK;
}

int builtin_throw(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "throw: expected 1 argument");

	ctx_exception = car(args);
	return Error_Exception;
}

int builtin_error(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "error: expected 1 argument");

	ctx_exception = car(args);
	return Error_Exception;
}

/* Metatable operations */
int builtin_set_metatable(Atom args, Atom *result)
{
	if (nilp(args) || nilp(cdr(args)) || !nilp(cdr(cdr(args))))
		return raise_error("args-error", "set-metatable!: expected 2 arguments");
	
	Atom hash = car(args);
	Atom mt = car(cdr(args));
	
	if (hash.type != AtomType_Hash)
		return raise_error("type-error", "set-metatable!: first argument must be hash");
	
	if (!nilp(mt) && mt.type != AtomType_Hash)
		return raise_error("type-error", "set-metatable!: second argument must be hash or nil");
	
	if (nilp(mt)) {
		hash.value.hash->metatable = NULL;
	} else {
		hash.value.hash->metatable = mt.value.hash;
	}
	
	*result = hash;
	return Error_OK;
}

int builtin_get_metatable(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "get-metatable: expected 1 argument");
	
	Atom hash = car(args);
	
	if (hash.type != AtomType_Hash)
		return raise_error("type-error", "get-metatable: argument must be hash");
	
	if (hash.value.hash->metatable) {
		result->type = AtomType_Hash;
		result->value.hash = hash.value.hash->metatable;
	} else {
		*result = nil;
	}
	
	return Error_OK;
}


/* Serialization Helpers */

static size_t serialize_calc_len(Atom atom) {
	char buf[64];
	switch (atom.type) {
	case AtomType_Nil:
		return 3; /* "nil" or "()" ? lisd uses nil? lisd.h: LD_PRIMITIVE. 
		             If we want compatible with common lisp usage in lisd, maybe "nil". 
				 But plan says: "Hash Table: (:key1 val1)". 
				 nil is usually printed as NIL. 
				 Let's use "nil" for now. */
		return 3; 
	case AtomType_Pair: {
		/* List -> Vector #(...) */
		size_t len = 2; /* "#(" */
		Atom p = atom;
		int first = 1;
		while (!nilp(p)) {
			if (p.type == AtomType_Pair) {
				if (!first) len++; /* space */
				len += serialize_calc_len(car(p));
				p = cdr(p);
				first = 0;
			} else {
				/* Dotted list - not supported well in lisd array? 
				   Treat as element. */
				if (!first) len += 3; /* " . " */
				len += serialize_calc_len(p);
				break;
			}
		}
		len += 1; /* ")" */
		return len;
	}
	case AtomType_Vector: {
		/* Vector -> Vector #(...) */
		size_t len = 2; /* "#(" */
		for (size_t i = 0; i < atom.value.vector->size; i++) {
			if (i > 0) len++;
			len += serialize_calc_len(atom.value.vector->data[i]);
		}
		len += 1; /* ")" */
		return len;
	}
	case AtomType_Hash: {
		/* Hash -> (:key value ...) */
		size_t len = 1; /* "(" */
		struct Hash *h = atom.value.hash;
		int first = 1;
		for (size_t i = 0; i < h->size; i++) {
			if (!first) len++; /* space */
			len += serialize_calc_len(h->keys[i]);
			len++; /* space */
			len += serialize_calc_len(h->values[i]);
			first = 0;
		}
		len += 1; /* ")" */
		return len;
	}
	case AtomType_Integer:
		snprintf(buf, sizeof(buf), "%ld", atom.value.integer);
		return strlen(buf);
	case AtomType_Float:
		snprintf(buf, sizeof(buf), "%g", atom.value.floatval);
		return strlen(buf);
	case AtomType_String: {
		size_t len = 2; /* quotes */
		for (char *s = atom.value.string; *s; s++) {
			if (*s == '"' || *s == '\\') len++;
			len++;
		}
		return len;
	}
	case AtomType_Symbol:
		return strlen(atom.value.symbol);
	case AtomType_Keyword:
		return strlen(atom.value.keyword) + 1;
	default:
		return 0;
	}
}

static void serialize_write(Atom atom, char *buf, size_t *pos) {
	char tmp[64];
	switch (atom.type) {
	case AtomType_Nil:
		memcpy(buf + *pos, "nil", 3);
		*pos += 3;
		break;
	case AtomType_Pair: {
		/* List -> Vector #(...) */
		memcpy(buf + *pos, "#(", 2);
		*pos += 2;
		Atom p = atom;
		int first = 1;
		while (!nilp(p)) {
			if (p.type == AtomType_Pair) {
				if (!first) buf[(*pos)++] = ' ';
				serialize_write(car(p), buf, pos);
				p = cdr(p);
				first = 0;
			} else {
				if (!first) {
					memcpy(buf + *pos, " . ", 3);
					*pos += 3;
				}
				serialize_write(p, buf, pos);
				break;
			}
		}
		buf[(*pos)++] = ')';
		break;
	}
	case AtomType_Vector: {
		memcpy(buf + *pos, "#(", 2);
		*pos += 2;
		for (size_t i = 0; i < atom.value.vector->size; i++) {
			if (i > 0) buf[(*pos)++] = ' ';
			serialize_write(atom.value.vector->data[i], buf, pos);
		}
		buf[(*pos)++] = ')';
		break;
	}
	case AtomType_Hash: {
		buf[(*pos)++] = '(';
		struct Hash *h = atom.value.hash;
		int first = 1;
		for (size_t i = 0; i < h->size; i++) {
			if (!first) buf[(*pos)++] = ' ';
			serialize_write(h->keys[i], buf, pos);
			buf[(*pos)++] = ' ';
			serialize_write(h->values[i], buf, pos);
			first = 0;
		}
		buf[(*pos)++] = ')';
		break;
	}
	case AtomType_Integer:
		sprintf(tmp, "%ld", atom.value.integer);
		strcpy(buf + *pos, tmp);
		*pos += strlen(tmp);
		break;
	case AtomType_Float:
		sprintf(tmp, "%g", atom.value.floatval);
		strcpy(buf + *pos, tmp);
		*pos += strlen(tmp);
		break;
	case AtomType_String:
		buf[(*pos)++] = '"';
		for (char *s = atom.value.string; *s; s++) {
			if (*s == '"' || *s == '\\') buf[(*pos)++] = '\\';
			buf[(*pos)++] = *s;
		}
		buf[(*pos)++] = '"';
		break;
	case AtomType_Symbol: {
		size_t len = strlen(atom.value.symbol);
		memcpy(buf + *pos, atom.value.symbol, len);
		*pos += len;
		break;
	}
	case AtomType_Keyword:
		buf[(*pos)++] = ':';
		size_t len = strlen(atom.value.keyword);
		memcpy(buf + *pos, atom.value.keyword, len);
		*pos += len;
		break;
	default:
		/* Unserializable types; keep in sync with serialize_calc_len (returns 0) */
		break;
	}
}

int builtin_serialize(Atom args, Atom *result) {
	if (nilp(args) || !nilp(cdr(args)))
		return Error_Args;
	
	Atom obj = car(args);
	size_t len = serialize_calc_len(obj);
	
	char *buf = malloc(len + 1);
	if (!buf) return Error_Exception; /* Should refer to allocation error */
	
	size_t pos = 0;
	serialize_write(obj, buf, &pos);
	buf[pos] = '\0';
	
	*result = make_string(buf);
	free(buf);
	return Error_OK;
}

/* Helper to convert Lists to Hash Tables recursively */
static Atom lisd_convert(Atom atom) {
	if (atom.type == AtomType_Pair) {
		/* Convert List to Hash */
		Atom h = make_hash();
		Atom p = atom;
		while (!nilp(p)) {
			if (p.type != AtomType_Pair) break; /* Should not happen in proper list */
			
			Atom key = car(p);
			p = cdr(p);
			
			Atom val = nil;
			if (!nilp(p)) {
				if (p.type == AtomType_Pair) {
					val = car(p);
					p = cdr(p);
				} else {
					/* Unexpected dotted pair end? Use p as value? */
					val = p;
					p = nil;
				}
			}
			
			/* Recursively convert value */
			val = lisd_convert(val);
			hash_set(h, key, val);
		}
		return h;
	} else if (atom.type == AtomType_Vector) {
		/* Recursively convert elements - Create NEW vector to avoid mutation */
		size_t size = atom.value.vector->size;
		Atom new_vec = make_vector(size, nil);
		for (size_t i = 0; i < size; i++) {
			new_vec.value.vector->data[i] = lisd_convert(atom.value.vector->data[i]);
		}
		return new_vec;
	}
	return atom;
}

int builtin_deserialize(Atom args, Atom *result) {
	if (nilp(args) || !nilp(cdr(args)))
		return Error_Args;
	
	Atom arg = car(args);
	
	if (arg.type == AtomType_String) {
		const char *input = arg.value.string;
		const char *end = input + strlen(input);
		Atom parsed;
		ErrorContext err = {0};
		
		/* Use read_expr to parse the S-expression */
		int status = read_expr(input, &end, &parsed, &err);
		
		if (status != Error_OK) {
			return status;
		}
		
		/* Convert Lists to Hashes */
		*result = lisd_convert(parsed);
	} else {
		/* Argument is already a structure (e.g. quoted list), just convert it */
		*result = lisd_convert(arg);
	}
	
	return Error_OK;
}

/* (read-string str) - Parse an S-expression from a string, returning raw result (no conversion) */
int builtin_read_string(Atom args, Atom *result) {
	if (nilp(args) || !nilp(cdr(args)))
		return Error_Args;
	
	Atom arg = car(args);
	
	if (arg.type != AtomType_String) {
		*result = nil;
		return Error_Type;
	}
	
	const char *input = arg.value.string;
	const char *end = input + strlen(input);
	ErrorContext err = {0};
	
	int status = read_expr(input, &end, result, &err);
	
	if (status != Error_OK) {
		*result = nil;
		return status;
	}
	
	return Error_OK;
}

int builtin_print(Atom args, Atom *result)

{
	if (nilp(args)) {
		putchar('\n');
		*result = nil;
		return Error_OK;
	}

	while (!nilp(args)) {
		print_expr(car(args));
		if (!nilp(cdr(args)))
			putchar(' ');
		args = cdr(args);
	}
	putchar('\n');
	
	*result = nil;
	return Error_OK;
}

/* (->string value) - convert any value to string */
int builtin_tostring(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "->string: expected 1 argument");
	
	char *str = sprint_expr(car(args));
	*result = make_string(str);
	free(str);
	return Error_OK;
}

/* (open-input-file path [binary?]) */
int builtin_open_input_file(Atom args, Atom *result) {
	if (nilp(args) || car(args).type != AtomType_String) {
		return raise_error("type-error", "open-input-file expects a string path");
	}
	
	const char *path = car(args).value.string;
	int is_binary = 0;
	
	if (!nilp(cdr(args)) && !nilp(car(cdr(args)))) {
		is_binary = 1;
	}
	
	*result = io_open_file(path, 1, is_binary);
	if (nilp(*result)) {
		return raise_error("io-error", "failed to open file for reading");
	}
	return Error_OK;
}

/* (open-output-file path [binary?]) */
int builtin_open_output_file(Atom args, Atom *result) {
	if (nilp(args) || car(args).type != AtomType_String) {
		return raise_error("type-error", "open-output-file expects a string path");
	}
	
	const char *path = car(args).value.string;
	int is_binary = 0;
	
	if (!nilp(cdr(args)) && !nilp(car(cdr(args)))) {
		is_binary = 1;
	}
	
	*result = io_open_file(path, 0, is_binary);
	if (nilp(*result)) {
		return raise_error("io-error", "failed to open file for writing");
	}
	return Error_OK;
}

/* (close-port port) */
int builtin_close_port(Atom args, Atom *result) {
	if (nilp(args) || car(args).type != AtomType_Port) {
		return raise_error("type-error", "close-port expects a port");
	}
	port_close(car(args));
	*result = make_sym("T");
	return Error_OK;
}

/* (port? obj) */
int builtin_portp(Atom args, Atom *result) {
	if (nilp(args)) return Error_Args;
	*result = car(args).type == AtomType_Port ? make_sym("T") : nil;
	return Error_OK;
}

/* (input-port? port) */
int builtin_input_portp(Atom args, Atom *result) {
	if (nilp(args)) return Error_Args;
	Atom a = car(args);
	*result = (a.type == AtomType_Port && a.value.port->is_input) ? make_sym("T") : nil;
	return Error_OK;
}

/* (output-port? port) */
int builtin_output_portp(Atom args, Atom *result) {
	if (nilp(args)) return Error_Args;
	Atom a = car(args);
	*result = (a.type == AtomType_Port && !a.value.port->is_input) ? make_sym("T") : nil;
	return Error_OK;
}

/* (port-open? port) */
int builtin_port_openp(Atom args, Atom *result) {
	if (nilp(args)) return Error_Args;
	Atom a = car(args);
	*result = (a.type == AtomType_Port && a.value.port->is_open) ? make_sym("T") : nil;
	return Error_OK;
}

/* (read-char port) - reads a full UTF-8 character, returns char type */
int builtin_read_char(Atom args, Atom *result) {
	if (nilp(args) || car(args).type != AtomType_Port) {
		return raise_error("type-error", "read-char expects an input port");
	}
	
	struct Port *p = car(args).value.port;
	if (!p->is_input) {
		return raise_error("io-error", "cannot read from output port");
	}
	
	/* Read first byte to determine UTF-8 sequence length */
	int c = port_getc(p);
	if (c == EOF) {
		*result = make_sym("EOF");
		return Error_OK;
	}
	
	/* Determine UTF-8 sequence length */
	unsigned char first = (unsigned char)c;
	int len = 1;
	if (first >= 0xF0) len = 4;
	else if (first >= 0xE0) len = 3;
	else if (first >= 0xC0) len = 2;
	
	/* Read remaining bytes */
	char buf[5];
	buf[0] = first;
	for (int i = 1; i < len; i++) {
		int b = port_getc(p);
		if (b == EOF) break;
		buf[i] = (char)b;
	}
	buf[len] = '\0';
	
	/* Decode to codepoint */
	int32_t cp;
	utf8_decode(buf, &cp);
	*result = make_char(cp);
	return Error_OK;
}

/* (read-line port) */
int builtin_read_line(Atom args, Atom *result) {
	if (nilp(args) || car(args).type != AtomType_Port) {
		return raise_error("type-error", "read-line expects an input port");
	}
	
	struct Port *p = car(args).value.port;
	if (!p->is_input) {
		return raise_error("io-error", "cannot read from output port");
	}
	
	/* Read line into dynamic buffer */
	size_t capacity = 128;
	size_t len = 0;
	char *buf = malloc(capacity);
	
	int c;
	while ((c = port_getc(p)) != EOF && c != '\n') {
		if (len + 1 >= capacity) {
			capacity *= 2;
			buf = realloc(buf, capacity);
		}
		buf[len++] = (char)c;
	}
	
	if (len == 0 && c == EOF) {
		free(buf);
		*result = make_sym("EOF");
		return Error_OK;
	}
	
	buf[len] = '\0';
	*result = make_string(buf);
	free(buf);
	return Error_OK;
}

/* (read-all port) */
int builtin_read_all(Atom args, Atom *result) {
	if (nilp(args) || car(args).type != AtomType_Port) {
		return raise_error("type-error", "read-all expects an input port");
	}
	
	struct Port *p = car(args).value.port;
	if (!p->is_input || !p->is_open) {
		return raise_error("io-error", "cannot read from output port or closed port");
	}
	
	/* Read all into dynamic buffer */
	size_t capacity = 1024;
	size_t len = 0;
	char *buf = malloc(capacity);
	
	char chunk[1024];
	long n;
	while ((n = port_read(p, chunk, sizeof(chunk))) > 0) {
		if (len + n >= capacity) {
			while (len + n >= capacity) capacity *= 2;
			buf = realloc(buf, capacity);
		}
		memcpy(buf + len, chunk, n);
		len += n;
	}
	
	buf[len] = '\0';
	*result = make_string(buf);
	free(buf);
	return Error_OK;
}

/* (read-byte port) */
int builtin_read_byte(Atom args, Atom *result) {
	if (nilp(args) || car(args).type != AtomType_Port) {
		return raise_error("type-error", "read-byte expects an input port");
	}
	
	struct Port *p = car(args).value.port;
	if (!p->is_input) {
		return raise_error("io-error", "cannot read from output port");
	}
	
	int c = port_getc(p);
	if (c == EOF) {
		*result = make_sym("EOF");
	} else {
		*result = make_int(c);
	}
	return Error_OK;
}

/* (read-bytes port n) */
int builtin_read_bytes(Atom args, Atom *result) {
	if (nilp(args) || car(args).type != AtomType_Port) {
		return raise_error("type-error", "read-bytes expects an input port");
	}
	if (nilp(cdr(args)) || car(cdr(args)).type != AtomType_Integer) {
		return raise_error("type-error", "read-bytes expects count as second argument");
	}
	
	struct Port *p = car(args).value.port;
	long count = car(cdr(args)).value.integer;
	
	if (!p->is_input) {
		return raise_error("io-error", "cannot read from output port");
	}
	
	unsigned char *buf = malloc(count);
	long n = port_read(p, buf, count);
	
	if (n <= 0) {
		free(buf);
		*result = make_sym("EOF");
		return Error_OK;
	}
	
	*result = make_bytevector_from_data(buf, n);
	free(buf);
	return Error_OK;
}


/* (write-char char port) */
int builtin_write_char(Atom args, Atom *result) {
	if (nilp(args) || car(args).type != AtomType_String) {
		return raise_error("type-error", "write-char expects a string (character)");
	}
	if (nilp(cdr(args)) || car(cdr(args)).type != AtomType_Port) {
		return raise_error("type-error", "write-char expects a port as second argument");
	}
	
	const char *s = car(args).value.string;
	struct Port *p = car(cdr(args)).value.port;
	
	if (p->is_input) {
		return raise_error("io-error", "cannot write to input port");
	}
	
	port_write(p, s, 1);
	*result = make_sym("T");
	return Error_OK;
}

/* (write-string string port) */
int builtin_write_string(Atom args, Atom *result) {
	if (nilp(args) || car(args).type != AtomType_String) {
		return raise_error("type-error", "write-string expects a string");
	}
	if (nilp(cdr(args)) || car(cdr(args)).type != AtomType_Port) {
		return raise_error("type-error", "write-string expects a port as second argument");
	}
	
	const char *s = car(args).value.string;
	struct Port *p = car(cdr(args)).value.port;
	
	if (p->is_input) {
		return raise_error("io-error", "cannot write to input port");
	}
	
	port_write(p, s, strlen(s));
	*result = make_sym("T");
	return Error_OK;
}

/* (write-byte byte port) */
int builtin_write_byte(Atom args, Atom *result) {
	if (nilp(args) || car(args).type != AtomType_Integer) {
		return raise_error("type-error", "write-byte expects an integer (0-255)");
	}
	if (nilp(cdr(args)) || car(cdr(args)).type != AtomType_Port) {
		return raise_error("type-error", "write-byte expects a port as second argument");
	}
	
	unsigned char byte = (unsigned char)car(args).value.integer;
	struct Port *p = car(cdr(args)).value.port;
	
	if (p->is_input) {
		return raise_error("io-error", "cannot write to input port");
	}
	
	port_write(p, &byte, 1);
	*result = make_sym("T");
	return Error_OK;
}

/* (write-bytes bytevector port) */
int builtin_write_bytes(Atom args, Atom *result) {
	if (nilp(args) || car(args).type != AtomType_Bytevector) {
		return raise_error("type-error", "write-bytes expects a bytevector");
	}
	if (nilp(cdr(args)) || car(cdr(args)).type != AtomType_Port) {
		return raise_error("type-error", "write-bytes expects a port as second argument");
	}
	
	struct Bytevector *bv = car(args).value.bytevector;
	struct Port *p = car(cdr(args)).value.port;
	
	if (p->is_input) {
		return raise_error("io-error", "cannot write to input port");
	}
	
	port_write(p, bv->data, bv->size);
	*result = make_sym("T");
	return Error_OK;
}

/* (newline port) */
int builtin_newline(Atom args, Atom *result) {
	if (nilp(args) || car(args).type != AtomType_Port) {
		return raise_error("type-error", "newline expects a port");
	}
	
	struct Port *p = car(args).value.port;
	if (p->is_input) {
		return raise_error("io-error", "cannot write to input port");
	}
	
	port_write(p, "\n", 1);
	*result = make_sym("T");
	return Error_OK;
}

/* (flush-port port) */
int builtin_flush_port(Atom args, Atom *result) {
	if (nilp(args) || car(args).type != AtomType_Port) {
		return raise_error("type-error", "flush-port expects a port");
	}
	
	struct Port *p = car(args).value.port;
	port_flush(p);
	
	*result = make_sym("T");
	return Error_OK;
}

/* (file-exists? path) */
int builtin_file_exists(Atom args, Atom *result) {
	if (nilp(args) || car(args).type != AtomType_String) {
		return raise_error("type-error", "file-exists? expects a string path");
	}
	
	const char *path = car(args).value.string;
	if (io_file_exists(path)) {
		*result = make_sym("T");
	} else {
		*result = nil;
	}
	return Error_OK;
}

/* (delete-file path) */
int builtin_delete_file(Atom args, Atom *result) {
	if (nilp(args) || car(args).type != AtomType_String) {
		return raise_error("type-error", "delete-file expects a string path");
	}
	
	if (remove(car(args).value.string) == 0) {
		*result = make_sym("T");
		return Error_OK;
	} else {
		return raise_error("io-error", "could not delete file");
	}
}

/* (rename-file old new) */
int builtin_rename_file(Atom args, Atom *result) {
	if (nilp(args) || car(args).type != AtomType_String) {
		return raise_error("type-error", "rename-file expects a string source path");
	}
	if (nilp(cdr(args)) || car(cdr(args)).type != AtomType_String) {
		return raise_error("type-error", "rename-file expects a string destination path");
	}
	
	const char *old_path = car(args).value.string;
	const char *new_path = car(cdr(args)).value.string;
	
	if (rename(old_path, new_path) == 0) {
		*result = make_sym("T");
		return Error_OK;
	} else {
		return raise_error("io-error", "could not rename file");
	}
}

/* ============================================================
 * BYTEVECTOR BUILTINS
 * ============================================================ */

/* (make-bytevector size [fill]) */
int builtin_make_bytevector(Atom args, Atom *result) {
	if (nilp(args) || car(args).type != AtomType_Integer) {
		return raise_error("type-error", "make-bytevector expects an integer size");
	}
	
	size_t size = (size_t)car(args).value.integer;
	*result = make_bytevector(size);
	
	/* Optional fill value */
	if (!nilp(cdr(args)) && car(cdr(args)).type == AtomType_Integer) {
		unsigned char fill = (unsigned char)car(cdr(args)).value.integer;
		memset((*result).value.bytevector->data, fill, size);
	}
	
	return Error_OK;
}

/* (bytevector? obj) */
int builtin_bytevectorp(Atom args, Atom *result) {
	if (nilp(args)) return Error_Args;
	*result = car(args).type == AtomType_Bytevector ? make_sym("T") : nil;
	return Error_OK;
}

/* (bytevector-length bv) */
int builtin_bytevector_length(Atom args, Atom *result) {
	if (nilp(args) || car(args).type != AtomType_Bytevector) {
		return raise_error("type-error", "bytevector-length expects a bytevector");
	}
	*result = make_int(bytevector_length(car(args)));
	return Error_OK;
}

/* (bytevector-u8-ref bv index) */
int builtin_bytevector_u8_ref(Atom args, Atom *result) {
	if (nilp(args) || car(args).type != AtomType_Bytevector) {
		return raise_error("type-error", "bytevector-u8-ref expects a bytevector");
	}
	if (nilp(cdr(args)) || car(cdr(args)).type != AtomType_Integer) {
		return raise_error("type-error", "bytevector-u8-ref expects an integer index");
	}
	
	Atom bv = car(args);
	size_t index = (size_t)car(cdr(args)).value.integer;
	
	if (index >= bytevector_length(bv)) {
		return raise_error("range-error", "bytevector index out of bounds");
	}
	
	*result = make_int(bytevector_ref(bv, index));
	return Error_OK;
}

/* (bytevector-u8-set! bv index value) */
int builtin_bytevector_u8_set(Atom args, Atom *result) {
	if (nilp(args) || car(args).type != AtomType_Bytevector) {
		return raise_error("type-error", "bytevector-u8-set! expects a bytevector");
	}
	if (nilp(cdr(args)) || car(cdr(args)).type != AtomType_Integer) {
		return raise_error("type-error", "bytevector-u8-set! expects an integer index");
	}
	if (nilp(cdr(cdr(args))) || car(cdr(cdr(args))).type != AtomType_Integer) {
		return raise_error("type-error", "bytevector-u8-set! expects an integer value");
	}
	
	Atom bv = car(args);
	size_t index = (size_t)car(cdr(args)).value.integer;
	unsigned char value = (unsigned char)car(cdr(cdr(args))).value.integer;
	
	if (index >= bytevector_length(bv)) {
		return raise_error("range-error", "bytevector index out of bounds");
	}
	
	bytevector_set(bv, index, value);
	*result = make_sym("T");
	return Error_OK;
}

/* (bytevector->string bv) - UTF-8 interpretation */
int builtin_bytevector_to_string(Atom args, Atom *result) {
	if (nilp(args) || car(args).type != AtomType_Bytevector) {
		return raise_error("type-error", "bytevector->string expects a bytevector");
	}
	
	struct Bytevector *bv = car(args).value.bytevector;
	*result = make_string_n((const char *)bv->data, bv->size);
	return Error_OK;
}

/* (string->bytevector str) */
int builtin_string_to_bytevector(Atom args, Atom *result) {
	if (nilp(args) || car(args).type != AtomType_String) {
		return raise_error("type-error", "string->bytevector expects a string");
	}
	
	const char *s = car(args).value.string;
	*result = make_bytevector_from_data((const unsigned char *)s, strlen(s));
	return Error_OK;
}

/* ============================================================
 * SANDBOX SCHEME BUILTINS
 * ============================================================ */

/* (io-sandbox-allow! pattern) */
int builtin_io_sandbox_allow(Atom args, Atom *result) {
	if (nilp(args) || car(args).type != AtomType_String) {
		return raise_error("type-error", "io-sandbox-allow! expects a string pattern");
	}
	io_sandbox_allow_path(car(args).value.string);
	*result = make_sym("T");
	return Error_OK;
}

/* (io-sandbox-deny! pattern) */
int builtin_io_sandbox_deny(Atom args, Atom *result) {
	if (nilp(args) || car(args).type != AtomType_String) {
		return raise_error("type-error", "io-sandbox-deny! expects a string pattern");
	}
	io_sandbox_deny_path(car(args).value.string);
	*result = make_sym("T");
	return Error_OK;
}

/* (io-sandbox-mode [mode]) - get or set mode */
int builtin_io_sandbox_mode(Atom args, Atom *result) {
	if (nilp(args)) {
		/* Get mode */
		const char *mode_str;
		switch (io_sandbox_get_mode()) {
		case IO_SANDBOX_UNRESTRICTED: mode_str = "UNRESTRICTED"; break;
		case IO_SANDBOX_RESTRICTED: 
			mode_str = io_sandbox_get_default() ? "RESTRICTED-ALLOW" : "RESTRICTED-DENY"; 
			break;
		case IO_SANDBOX_DISABLED: mode_str = "DISABLED"; break;
		default: mode_str = "UNKNOWN"; break;
		}
		*result = make_sym(mode_str);
	} else {
		/* Set mode */
		Atom mode_arg = car(args);
		if (mode_arg.type != AtomType_Symbol) {
			return raise_error("type-error", "io-sandbox-mode expects a symbol");
		}
		const char *s = mode_arg.value.symbol;
		if (strcmp(s, "UNRESTRICTED") == 0) {
			io_sandbox_init(IO_SANDBOX_UNRESTRICTED);
		} else if (strcmp(s, "RESTRICTED") == 0 || strcmp(s, "RESTRICTED-ALLOW") == 0) {
			io_sandbox_init(IO_SANDBOX_RESTRICTED);
			io_sandbox_set_default(1); /* Allow by default */
		} else if (strcmp(s, "RESTRICTED-DENY") == 0) {
			io_sandbox_init(IO_SANDBOX_RESTRICTED);
			io_sandbox_set_default(0); /* Deny by default */
		} else if (strcmp(s, "DISABLED") == 0) {
			io_sandbox_init(IO_SANDBOX_DISABLED);
		} else {
			return raise_error("value-error", "unknown sandbox mode (use unrestricted, restricted, restricted-allow, restricted-deny, or disabled)");
		}
		*result = mode_arg;
	}
	return Error_OK;
}

/* ============================================================
 * CHARACTER BUILTINS
 * ============================================================ */

/* (char? obj) */
int builtin_charp(Atom args, Atom *result) {
	if (nilp(args)) return raise_error("args-error", "char?: expected 1 argument");
	*result = car(args).type == AtomType_Char ? make_sym("T") : nil;
	return Error_OK;
}

/* (char->integer char) */
int builtin_char_to_integer(Atom args, Atom *result) {
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "char->integer: expected 1 argument");
	Atom a = car(args);
	if (a.type != AtomType_Char)
		return raise_error("type-error", "char->integer: argument must be a character");
	*result = make_int(a.value.character);
	return Error_OK;
}

/* (integer->char int) */
int builtin_integer_to_char(Atom args, Atom *result) {
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "integer->char: expected 1 argument");
	Atom a = car(args);
	if (a.type != AtomType_Integer)
		return raise_error("type-error", "integer->char: argument must be an integer");
	*result = make_char((int32_t)a.value.integer);
	return Error_OK;
}

/* (char=? a b) */
int builtin_char_eq(Atom args, Atom *result) {
	if (nilp(args) || nilp(cdr(args)) || !nilp(cdr(cdr(args))))
		return raise_error("args-error", "char=?: expected 2 arguments");
	Atom a = car(args), b = car(cdr(args));
	if (a.type != AtomType_Char || b.type != AtomType_Char)
		return raise_error("type-error", "char=?: arguments must be characters");
	*result = (a.value.character == b.value.character) ? make_sym("T") : nil;
	return Error_OK;
}

/* (char<? a b) */
int builtin_char_lt(Atom args, Atom *result) {
	if (nilp(args) || nilp(cdr(args)) || !nilp(cdr(cdr(args))))
		return raise_error("args-error", "char<?: expected 2 arguments");
	Atom a = car(args), b = car(cdr(args));
	if (a.type != AtomType_Char || b.type != AtomType_Char)
		return raise_error("type-error", "char<?: arguments must be characters");
	*result = (a.value.character < b.value.character) ? make_sym("T") : nil;
	return Error_OK;
}

/* ============================================================
 * STRING/BYTES ENCODING BUILTINS
 * ============================================================ */

/* (string->bytes str [encoding]) - Convert string to bytevector */
int builtin_string_to_bytes(Atom args, Atom *result) {
	if (nilp(args) || car(args).type != AtomType_String)
		return raise_error("type-error", "string->bytes: first argument must be string");
	
	const char *str = car(args).value.string;
	const char *encoding = "utf-8";  /* Default */
	
	if (!nilp(cdr(args))) {
		Atom enc_arg = car(cdr(args));
		if (enc_arg.type != AtomType_String)
			return raise_error("type-error", "string->bytes: encoding must be string");
		encoding = enc_arg.value.string;
	}
	
	/* UTF-8 is the native format - just copy bytes */
	if (strcmp(encoding, "utf-8") == 0 || strcmp(encoding, "UTF-8") == 0) {
		size_t len = strlen(str);
		*result = make_bytevector_from_data((const unsigned char *)str, len);
	} else if (strcmp(encoding, "ascii") == 0 || strcmp(encoding, "ASCII") == 0) {
		/* ASCII: fail on non-ASCII characters */
		size_t len = strlen(str);
		for (size_t i = 0; i < len; i++) {
			if ((unsigned char)str[i] > 127)
				return raise_error("encoding-error", "string->bytes: non-ASCII character in string");
		}
		*result = make_bytevector_from_data((const unsigned char *)str, len);
	} else if (strcmp(encoding, "latin-1") == 0 || strcmp(encoding, "LATIN-1") == 0 ||
	           strcmp(encoding, "iso-8859-1") == 0 || strcmp(encoding, "ISO-8859-1") == 0) {
		/* Latin-1: encode codepoints 0-255 directly */
		size_t codepoint_count = utf8_strlen(str);
		unsigned char *buf = malloc(codepoint_count);
		const char *p = str;
		for (size_t i = 0; i < codepoint_count; i++) {
			int32_t cp;
			p += utf8_decode(p, &cp);
			if (cp > 255)
				return raise_error("encoding-error", "string->bytes: codepoint > 255 not encodable in latin-1");
			buf[i] = (unsigned char)cp;
		}
		*result = make_bytevector_from_data(buf, codepoint_count);
		free(buf);
	} else {
		return raise_error("value-error", "string->bytes: unknown encoding (use utf-8, ascii, or latin-1)");
	}
	
	return Error_OK;
}

/* (bytes->string bytevector [encoding]) - Convert bytevector to string */
int builtin_bytes_to_string(Atom args, Atom *result) {
	if (nilp(args) || car(args).type != AtomType_Bytevector)
		return raise_error("type-error", "bytes->string: first argument must be bytevector");
	
	struct Bytevector *bv = car(args).value.bytevector;
	const char *encoding = "utf-8";  /* Default */
	
	if (!nilp(cdr(args))) {
		Atom enc_arg = car(cdr(args));
		if (enc_arg.type != AtomType_String)
			return raise_error("type-error", "bytes->string: encoding must be string");
		encoding = enc_arg.value.string;
	}
	
	if (strcmp(encoding, "utf-8") == 0 || strcmp(encoding, "UTF-8") == 0) {
		/* UTF-8: just copy as string */
		*result = make_string_n((const char *)bv->data, bv->size);
	} else if (strcmp(encoding, "ascii") == 0 || strcmp(encoding, "ASCII") == 0) {
		/* ASCII: fail on non-ASCII bytes */
		for (size_t i = 0; i < bv->size; i++) {
			if (bv->data[i] > 127)
				return raise_error("encoding-error", "bytes->string: non-ASCII byte in bytevector");
		}
		*result = make_string_n((const char *)bv->data, bv->size);
	} else if (strcmp(encoding, "latin-1") == 0 || strcmp(encoding, "LATIN-1") == 0 ||
	           strcmp(encoding, "iso-8859-1") == 0 || strcmp(encoding, "ISO-8859-1") == 0) {
		/* Latin-1: each byte is a codepoint, encode to UTF-8 */
		/* Worst case: each byte becomes 2 UTF-8 bytes (for 128-255) */
		char *buf = malloc(bv->size * 2 + 1);
		char *p = buf;
		for (size_t i = 0; i < bv->size; i++) {
			p += utf8_encode(bv->data[i], p);
		}
		*p = '\0';
		*result = make_string(buf);
		free(buf);
	} else {
		return raise_error("value-error", "bytes->string: unknown encoding (use utf-8, ascii, or latin-1)");
	}
	
	return Error_OK;
}

/* ============================================================
 * KEYWORD BUILTINS
 * ============================================================ */

/* (keyword? obj) */
int builtin_keywordp(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "keyword?: expected 1 argument");
	*result = car(args).type == AtomType_Keyword ? make_sym("T") : nil;
	return Error_OK;
}

/* (keyword->string kw) */
int builtin_keyword_to_string(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "keyword->string: expected 1 argument");
	if (car(args).type != AtomType_Keyword)
		return raise_error("type-error", "keyword->string: argument must be keyword");
	*result = make_string(car(args).value.keyword);
	return Error_OK;
}

/* (string->keyword str) */
int builtin_string_to_keyword(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "string->keyword: expected 1 argument");
	if (car(args).type != AtomType_String)
		return raise_error("type-error", "string->keyword: argument must be string");
	
	/* Convert to uppercase for consistency */
	char *buf = strdup(car(args).value.string);
	for (char *p = buf; *p; p++) {
		if (*p >= 'a' && *p <= 'z') *p -= 32;
	}
	*result = make_keyword(buf);
	free(buf);
	return Error_OK;
}

/* ============================================================
 * PROCEDURE BUILTINS
 * ============================================================ */

/* (procedure-doc proc) - Get docstring from procedure/macro */
int builtin_procedure_doc(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "procedure-doc: expected 1 argument");
	
	Atom proc = car(args);
	if (proc.type != AtomType_Closure && proc.type != AtomType_Macro)
		return raise_error("type-error", "procedure-doc: argument must be procedure or macro");
	
	/* Structure: (env (args docstring . body)) */
	*result = car(cdr(cdr(proc)));
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

/* Search path management */

int builtin_get_search_paths(Atom args, Atom *result)
{
	if (!nilp(args))
		return raise_error("args-error", "get-search-paths: expected 0 arguments");
	*result = global_search_paths;
	return Error_OK;
}

int builtin_set_search_paths(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "set-search-paths!: expected 1 argument");
	Atom paths = car(args);
	/* Validate it's a list of strings */
	Atom p = paths;
	while (!nilp(p)) {
		if (p.type != AtomType_Pair)
			return raise_error("type-error", "set-search-paths!: expected list of strings");
		if (car(p).type != AtomType_String)
			return raise_error("type-error", "set-search-paths!: paths must be strings");
		p = cdr(p);
	}
	global_search_paths = paths;
	*result = paths;
	return Error_OK;
}

int builtin_add_search_path(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "add-search-path!: expected 1 argument");
	Atom path = car(args);
	if (path.type != AtomType_String)
		return raise_error("type-error", "add-search-path!: path must be string");
	global_search_paths = cons(path, global_search_paths);
	*result = global_search_paths;
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

/* (string-upcase str) - convert string to uppercase */
int builtin_string_upcase(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "string-upcase: expected 1 argument");
	
	Atom str = car(args);
	if (str.type != AtomType_String)
		return raise_error("type-error", "string-upcase: expected string");
	
	size_t len = strlen(str.value.string);
	char *buf = malloc(len + 1);
	
	for (size_t i = 0; i <= len; i++) {
		unsigned char c = str.value.string[i];
		if (c >= 'a' && c <= 'z')
			buf[i] = c - 32;
		else
			buf[i] = c;
	}
	
	*result = make_string(buf);
	free(buf);
	return Error_OK;
}

/* (string-downcase str) - convert string to lowercase */
int builtin_string_downcase(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "string-downcase: expected 1 argument");
	
	Atom str = car(args);
	if (str.type != AtomType_String)
		return raise_error("type-error", "string-downcase: expected string");
	
	size_t len = strlen(str.value.string);
	char *buf = malloc(len + 1);
	
	for (size_t i = 0; i <= len; i++) {
		unsigned char c = str.value.string[i];
		if (c >= 'A' && c <= 'Z')
			buf[i] = c + 32;
		else
			buf[i] = c;
	}
	
	*result = make_string(buf);
	free(buf);
	return Error_OK;
}

/* (string-capitalize str) - capitalize first letter of each word */
int builtin_string_capitalize(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "string-capitalize: expected 1 argument");
	
	Atom str = car(args);
	if (str.type != AtomType_String)
		return raise_error("type-error", "string-capitalize: expected string");
	
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
	
	*result = make_string(buf);
	free(buf);
	return Error_OK;
}

/* (make-string n [char]) - create string of n copies of char (default space) */
int builtin_make_string(Atom args, Atom *result)
{
	if (nilp(args))
		return raise_error("args-error", "make-string: expected at least 1 argument");
	
	Atom n_arg = car(args);
	if (n_arg.type != AtomType_Integer)
		return raise_error("type-error", "make-string: first argument must be integer");
	
	long n = n_arg.value.integer;
	if (n < 0)
		return raise_error("args-error", "make-string: length must be non-negative");
	
	int32_t cp = ' '; /* default to space */
	if (!nilp(cdr(args))) {
		Atom ch = car(cdr(args));
		if (ch.type == AtomType_Char) {
			cp = ch.value.character;
		} else if (ch.type == AtomType_String && strlen(ch.value.string) > 0) {
			utf8_decode(ch.value.string, &cp);
		} else {
			return raise_error("type-error", "make-string: second argument must be char or string");
		}
	}
	
	/* Encode the character to UTF-8 */
	char encoded[5];
	int char_len = utf8_encode(cp, encoded);
	
	size_t total_len = n * char_len;
	char *buf = malloc(total_len + 1);
	
	for (long i = 0; i < n; i++) {
		memcpy(buf + i * char_len, encoded, char_len);
	}
	buf[total_len] = '\0';
	
	*result = make_string(buf);
	free(buf);
	return Error_OK;
}

/* (substring str start [end]) - extract substring */
int builtin_substring(Atom args, Atom *result)
{
	if (nilp(args) || nilp(cdr(args)))
		return raise_error("args-error", "substring: expected at least 2 arguments");
	
	Atom str = car(args);
	Atom start_arg = car(cdr(args));
	
	if (str.type != AtomType_String)
		return raise_error("type-error", "substring: first argument must be string");
	if (start_arg.type != AtomType_Integer)
		return raise_error("type-error", "substring: second argument must be integer");
	
	size_t str_len = utf8_strlen(str.value.string);
	long start = start_arg.value.integer;
	long end = str_len;
	
	if (!nilp(cdr(cdr(args)))) {
		Atom end_arg = car(cdr(cdr(args)));
		if (end_arg.type != AtomType_Integer)
			return raise_error("type-error", "substring: third argument must be integer");
		end = end_arg.value.integer;
	}
	
	if (start < 0) start = 0;
	if (end > (long)str_len) end = str_len;
	if (start > end) start = end;
	
	/* Find byte positions for UTF-8 string */
	const char *start_ptr = utf8_index_ptr(str.value.string, start);
	const char *end_ptr = utf8_index_ptr(str.value.string, end);
	
	if (!start_ptr || !end_ptr) {
		*result = make_string("");
		return Error_OK;
	}
	
	size_t byte_len = end_ptr - start_ptr;
	*result = make_string_n(start_ptr, byte_len);
	return Error_OK;
}


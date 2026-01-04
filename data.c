#include "lisp.h"
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct Allocation {
	struct Pair pair;
	unsigned int mark : 1;
	struct Allocation *next;
};

struct Allocation *global_allocations = NULL;

Atom cons(Atom car_val, Atom cdr_val)
{
	struct Allocation *a;
	Atom p;

	a = malloc(sizeof(struct Allocation));
	a->mark = 0;
	a->next = global_allocations;
	global_allocations = a;

	p.type = AtomType_Pair;
	p.value.pair = &a->pair;

	car(p) = car_val;
	cdr(p) = cdr_val;

	return p;
}

Atom make_int(long x)
{
	Atom a;
	a.type = AtomType_Integer;
	a.value.integer = x;
	return a;
}

static Atom sym_table = { AtomType_Nil };

Atom make_sym(const char *s)
{
	Atom a, p;

	p = sym_table;
	while (!nilp(p)) {
		a = car(p);
		if (strcmp(a.value.symbol, s) == 0)
			return a;
		p = cdr(p);
	}

	a.type = AtomType_Symbol;
	a.value.symbol = strdup(s);
	sym_table = cons(a, sym_table);

	return a;
}

static Atom kw_table = { AtomType_Nil };

Atom make_keyword(const char *s)
{
	Atom a, p;

	p = kw_table;
	while (!nilp(p)) {
		a = car(p);
		if (strcmp(a.value.keyword, s) == 0)
			return a;
		p = cdr(p);
	}

	a.type = AtomType_Keyword;
	a.value.keyword = strdup(s);
	kw_table = cons(a, kw_table);

	return a;
}

Atom make_builtin(Builtin fn)
{
	Atom a;
	a.type = AtomType_Builtin;
	a.value.builtin = fn;
	return a;
}

Atom make_float(double x)
{
	Atom a;
	a.type = AtomType_Float;
	a.value.floatval = x;
	return a;
}

Atom make_char(int32_t codepoint)
{
	Atom a;
	a.type = AtomType_Char;
	a.value.character = codepoint;
	return a;
}

Atom make_string(const char *s)
{
	Atom a;
	a.type = AtomType_String;
	a.value.string = strdup(s);
	return a;
}

Atom make_string_n(const char *s, size_t len)
{
	Atom a;
	a.type = AtomType_String;
	a.value.string = malloc(len + 1);
	memcpy(a.value.string, s, len);
	a.value.string[len] = '\0';
	return a;
}

Atom make_vector(size_t size, Atom fill)
{
	Atom a;
	struct Vector *v = malloc(sizeof(struct Vector));
	v->size = size;
	v->capacity = size > 0 ? size : 4;
	v->data = malloc(sizeof(Atom) * v->capacity);
	for (size_t i = 0; i < size; i++) {
		v->data[i] = fill;
	}
	a.type = AtomType_Vector;
	a.value.vector = v;
	return a;
}

Atom make_hash(void)
{
	Atom a;
	struct Hash *h = malloc(sizeof(struct Hash));
	h->size = 0;
	h->capacity = 16;
	h->keys = malloc(sizeof(Atom) * h->capacity);
	h->values = malloc(sizeof(Atom) * h->capacity);
	h->metatable = NULL;
	a.type = AtomType_Hash;
	a.value.hash = h;
	return a;
}

/* Vector operations */
Atom vector_ref(Atom vec, size_t index)
{
	return vec.value.vector->data[index];
}

void vector_set(Atom vec, size_t index, Atom value)
{
	vec.value.vector->data[index] = value;
}

size_t vector_length(Atom vec)
{
	return vec.value.vector->size;
}

/* Hash operations - linear probing for simplicity */
static int atom_equal(Atom a, Atom b)
{
	if (a.type != b.type) return 0;
	switch (a.type) {
	case AtomType_Nil: return 1;
	case AtomType_Symbol: return a.value.symbol == b.value.symbol;
	case AtomType_Keyword: return a.value.keyword == b.value.keyword;
	case AtomType_Integer: return a.value.integer == b.value.integer;
	case AtomType_Float: return a.value.floatval == b.value.floatval;
	case AtomType_String: return strcmp(a.value.string, b.value.string) == 0;
	default: return a.value.pair == b.value.pair;
	}
}

Atom hash_ref(Atom hash, Atom key, Atom default_val)
{
	struct Hash *h = hash.value.hash;
	for (size_t i = 0; i < h->size; i++) {
		if (atom_equal(h->keys[i], key))
			return h->values[i];
	}
	return default_val;
}

void hash_set(Atom hash, Atom key, Atom value)
{
	struct Hash *h = hash.value.hash;
	/* Check if key exists */
	for (size_t i = 0; i < h->size; i++) {
		if (atom_equal(h->keys[i], key)) {
			h->values[i] = value;
			return;
		}
	}
	/* Add new key */
	if (h->size >= h->capacity) {
		h->capacity *= 2;
		h->keys = realloc(h->keys, sizeof(Atom) * h->capacity);
		h->values = realloc(h->values, sizeof(Atom) * h->capacity);
	}
	h->keys[h->size] = key;
	h->values[h->size] = value;
	h->size++;
}

int hash_remove(Atom hash, Atom key)
{
	struct Hash *h = hash.value.hash;
	for (size_t i = 0; i < h->size; i++) {
		if (atom_equal(h->keys[i], key)) {
			/* Move last element to this position */
			h->size--;
			if (i < h->size) {
				h->keys[i] = h->keys[h->size];
				h->values[i] = h->values[h->size];
			}
			return 1;
		}
	}
	return 0;
}

size_t hash_size(Atom hash)
{
	return hash.value.hash->size;
}

/* Metatable operations */
Atom get_metamethod(Atom hash, const char *name)
{
	struct Hash *h = hash.value.hash;
	if (!h->metatable) return nil;
	
	/* Build symbol name with ~ prefix (uppercase to match reader) */
	char sym_name[64];
	snprintf(sym_name, sizeof(sym_name), "~%s", name);
	/* Convert to uppercase */
	for (char *p = sym_name; *p; p++) {
		if (*p >= 'a' && *p <= 'z') *p -= 32;
	}
	Atom key = make_sym(sym_name);
	
	/* Look up in metatable */
	Atom mt;
	mt.type = AtomType_Hash;
	mt.value.hash = h->metatable;
	return hash_ref(mt, key, nil);
}

int has_metamethod(Atom hash, const char *name)
{
	return !nilp(get_metamethod(hash, name));
}

int listp(Atom expr)
{
	while (!nilp(expr)) {
		if (expr.type != AtomType_Pair)
			return 0;
		expr = cdr(expr);
	}
	return 1;
}

Atom copy_list(Atom list)
{
	Atom a, p;

	if (nilp(list))
		return nil;

	a = cons(car(list), nil);
	p = a;
	list = cdr(list);

	while (!nilp(list)) {
		cdr(p) = cons(car(list), nil);
		p = cdr(p);
		list = cdr(list);
	}

	return a;
}

Atom list_create(int n, ...)
{
	va_list ap;
	Atom list = nil;

	va_start(ap, n);
	while (n--) {
		Atom item = va_arg(ap, Atom);
		list = cons(item, list);
	}
	va_end(ap);

	list_reverse(&list);
	return list;
}

Atom list_get(Atom list, int k)
{
	while (k--)
		list = cdr(list);
	return car(list);
}

void list_set(Atom list, int k, Atom value)
{
	while (k--)
		list = cdr(list);
	car(list) = value;
}

void list_reverse(Atom *list)
{
	Atom tail = nil;
	while (!nilp(*list)) {
		Atom p = cdr(*list);
		cdr(*list) = tail;
		tail = *list;
		*list = p;
	}
	*list = tail;
}

void gc_mark(Atom root)
{
	struct Allocation *a;

	if (!(root.type == AtomType_Pair
		|| root.type == AtomType_Closure
		|| root.type == AtomType_Macro))
		return;

	a = (struct Allocation *)
		((char *) root.value.pair
			- offsetof(struct Allocation, pair));

	if (a->mark)
		return;

	a->mark = 1;

	gc_mark(car(root));
	gc_mark(cdr(root));
}

void gc()
{
	struct Allocation *a, **p;

	gc_mark(sym_table);
	gc_mark(kw_table);

	/* Free unmarked allocations */
	p = &global_allocations;
	while (*p != NULL) {
		a = *p;
		if (!a->mark) {
			*p = a->next;
			free(a);
		} else {
			p = &a->next;
		}
	}

	/* Clear marks */
	a = global_allocations;
	while (a != NULL) {
		a->mark = 0;
		a = a->next;
	}
}

/* Port operations */
Atom make_port(void *handle, const char *path, int is_input, int is_binary, int provider_id)
{
	Atom a;
	struct Port *p = malloc(sizeof(struct Port));
	p->handle = handle;
	p->path = strdup(path);
	p->is_input = is_input;
	p->is_binary = is_binary;
	p->is_open = 1;
	p->provider_id = provider_id;
	a.type = AtomType_Port;
	a.value.port = p;
	return a;
}

/* Bytevector operations */
Atom make_bytevector(size_t size)
{
	Atom a;
	struct Bytevector *bv = malloc(sizeof(struct Bytevector));
	bv->size = size;
	bv->data = calloc(size, 1);
	a.type = AtomType_Bytevector;
	a.value.bytevector = bv;
	return a;
}

Atom make_bytevector_from_data(const unsigned char *data, size_t size)
{
	Atom a;
	struct Bytevector *bv = malloc(sizeof(struct Bytevector));
	bv->size = size;
	bv->data = malloc(size);
	memcpy(bv->data, data, size);
	a.type = AtomType_Bytevector;
	a.value.bytevector = bv;
	return a;
}

unsigned char bytevector_ref(Atom bv, size_t index)
{
	return bv.value.bytevector->data[index];
}

void bytevector_set(Atom bv, size_t index, unsigned char value)
{
	bv.value.bytevector->data[index] = value;
}

size_t bytevector_length(Atom bv)
{
	return bv.value.bytevector->size;
}

/* SIMD Vector/Matrix constructors */
Atom make_vec2(float x, float y)
{
	Atom a;
	a.type = AtomType_Vec2;
	a.value.vec = malloc(sizeof(vec4_t));
#ifdef SIMD_X86
	a.value.vec->v = _mm_set_ps(0, 0, y, x);
#elif defined(SIMD_ARM_NEON)
	float vals[4] = {x, y, 0, 0};
	a.value.vec->v = vld1q_f32(vals);
#elif defined(SIMD_WASM)
	a.value.vec->v = wasm_f32x4_make(x, y, 0, 0);
#else
	a.value.vec->data[0] = x;
	a.value.vec->data[1] = y;
	a.value.vec->data[2] = 0;
	a.value.vec->data[3] = 0;
#endif
	return a;
}

Atom make_vec3(float x, float y, float z)
{
	Atom a;
	a.type = AtomType_Vec3;
	a.value.vec = malloc(sizeof(vec4_t));
#ifdef SIMD_X86
	a.value.vec->v = _mm_set_ps(0, z, y, x);
#elif defined(SIMD_ARM_NEON)
	float vals[4] = {x, y, z, 0};
	a.value.vec->v = vld1q_f32(vals);
#elif defined(SIMD_WASM)
	a.value.vec->v = wasm_f32x4_make(x, y, z, 0);
#else
	a.value.vec->data[0] = x;
	a.value.vec->data[1] = y;
	a.value.vec->data[2] = z;
	a.value.vec->data[3] = 0;
#endif
	return a;
}

Atom make_vec4(float x, float y, float z, float w)
{
	Atom a;
	a.type = AtomType_Vec4;
	a.value.vec = malloc(sizeof(vec4_t));
#ifdef SIMD_X86
	a.value.vec->v = _mm_set_ps(w, z, y, x);
#elif defined(SIMD_ARM_NEON)
	float vals[4] = {x, y, z, w};
	a.value.vec->v = vld1q_f32(vals);
#elif defined(SIMD_WASM)
	a.value.vec->v = wasm_f32x4_make(x, y, z, w);
#else
	a.value.vec->data[0] = x;
	a.value.vec->data[1] = y;
	a.value.vec->data[2] = z;
	a.value.vec->data[3] = w;
#endif
	return a;
}

Atom make_mat4_identity(void)
{
	Atom a;
	a.type = AtomType_Mat4;
	a.value.mat4 = malloc(sizeof(mat4_t));
	/* Column-major identity matrix */
	for (int c = 0; c < 4; c++) {
#ifdef SIMD_X86
		a.value.mat4->columns[c].v = _mm_set_ps(
			c == 3 ? 1.0f : 0.0f,
			c == 2 ? 1.0f : 0.0f,
			c == 1 ? 1.0f : 0.0f,
			c == 0 ? 1.0f : 0.0f);
#elif defined(SIMD_ARM_NEON)
		float vals[4] = {
			c == 0 ? 1.0f : 0.0f,
			c == 1 ? 1.0f : 0.0f,
			c == 2 ? 1.0f : 0.0f,
			c == 3 ? 1.0f : 0.0f};
		a.value.mat4->columns[c].v = vld1q_f32(vals);
#elif defined(SIMD_WASM)
		a.value.mat4->columns[c].v = wasm_f32x4_make(
			c == 0 ? 1.0f : 0.0f,
			c == 1 ? 1.0f : 0.0f,
			c == 2 ? 1.0f : 0.0f,
			c == 3 ? 1.0f : 0.0f);
#else
		a.value.mat4->columns[c].data[0] = (c == 0) ? 1.0f : 0.0f;
		a.value.mat4->columns[c].data[1] = (c == 1) ? 1.0f : 0.0f;
		a.value.mat4->columns[c].data[2] = (c == 2) ? 1.0f : 0.0f;
		a.value.mat4->columns[c].data[3] = (c == 3) ? 1.0f : 0.0f;
#endif
	}
	return a;
}

Atom make_mat4_from_floats(const float *values)
{
	Atom a;
	a.type = AtomType_Mat4;
	a.value.mat4 = malloc(sizeof(mat4_t));
	/* Values are column-major: first 4 = column 0, etc. */
	for (int c = 0; c < 4; c++) {
#ifdef SIMD_X86
		a.value.mat4->columns[c].v = _mm_loadu_ps(values + c * 4);
#elif defined(SIMD_ARM_NEON)
		a.value.mat4->columns[c].v = vld1q_f32(values + c * 4);
#elif defined(SIMD_WASM)
		a.value.mat4->columns[c].v = wasm_v128_load(values + c * 4);
#else
		for (int r = 0; r < 4; r++) {
			a.value.mat4->columns[c].data[r] = values[c * 4 + r];
		}
#endif
	}
	return a;
}

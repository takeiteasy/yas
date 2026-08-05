#include "yas.h"
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct Allocation {
	struct YasPair pair;
	unsigned int mark : 1;
	struct Allocation *next;
};

static struct Allocation *global_allocations = NULL;
static struct YasVector *global_vectors = NULL;
static struct YasHash *global_hashes = NULL;
static struct YasStringAlloc *global_strings = NULL;
static struct YasPort *global_ports = NULL;
static struct YasBytevector *global_bytevectors = NULL;
static struct YasVecAlloc *global_vecs = NULL;
static struct YasMat4Alloc *global_mats = NULL;
#ifdef YAS_FFI
struct YasFFIFunction *yas_global_ffi_functions = NULL;
YasFFIStructInstance *yas_global_ffi_structs = NULL;
#endif

YasAtom yas_cons(YasAtom car_val, YasAtom cdr_val)
{
	struct Allocation *a;
	YasAtom p;

	a = malloc(sizeof(struct Allocation));
	a->mark = 0;
	a->next = global_allocations;
	global_allocations = a;

	p.type = YAS_ATOM_TYPE_PAIR;
	p.value.pair = &a->pair;

	yas_car(p) = car_val;
	yas_cdr(p) = cdr_val;

	return p;
}

YasAtom yas_make_int(long x)
{
	YasAtom a;
	a.type = YAS_ATOM_TYPE_INTEGER;
	a.value.integer = x;
	return a;
}

static YasAtom sym_table = { YAS_ATOM_TYPE_NIL };

YasAtom yas_make_sym(const char *s)
{
	YasAtom a, p;

	p = sym_table;
	while (!yas_nilp(p)) {
		a = yas_car(p);
		if (strcmp(a.value.symbol, s) == 0)
			return a;
		p = yas_cdr(p);
	}

	a.type = YAS_ATOM_TYPE_SYMBOL;
	a.value.symbol = strdup(s);
	sym_table = yas_cons(a, sym_table);

	return a;
}

static YasAtom kw_table = { YAS_ATOM_TYPE_NIL };

YasAtom yas_make_keyword(const char *s)
{
	YasAtom a, p;

	p = kw_table;
	while (!yas_nilp(p)) {
		a = yas_car(p);
		if (strcmp(a.value.keyword, s) == 0)
			return a;
		p = yas_cdr(p);
	}

	a.type = YAS_ATOM_TYPE_KEYWORD;
	a.value.keyword = strdup(s);
	kw_table = yas_cons(a, kw_table);

	return a;
}

YasAtom yas_make_builtin(YasBuiltin fn)
{
	YasAtom a;
	a.type = YAS_ATOM_TYPE_BUILTIN;
	a.value.builtin = fn;
	return a;
}

YasAtom yas_make_float(double x)
{
	YasAtom a;
	a.type = YAS_ATOM_TYPE_FLOAT;
	a.value.floatval = x;
	return a;
}

YasAtom yas_make_char(int32_t codepoint)
{
	YasAtom a;
	a.type = YAS_ATOM_TYPE_CHAR;
	a.value.character = codepoint;
	return a;
}

static char *gc_alloc_string(size_t len)
{
	struct YasStringAlloc *sa = malloc(sizeof(struct YasStringAlloc) + len + 1);
	sa->mark = 0;
	sa->next = global_strings;
	global_strings = sa;
	return sa->data;
}

YasAtom yas_make_string(const char *s)
{
	YasAtom a;
	size_t len = strlen(s);
	a.type = YAS_ATOM_TYPE_STRING;
	a.value.string = gc_alloc_string(len);
	memcpy(a.value.string, s, len + 1);
	return a;
}

YasAtom yas_make_string_n(const char *s, size_t len)
{
	YasAtom a;
	a.type = YAS_ATOM_TYPE_STRING;
	a.value.string = gc_alloc_string(len);
	memcpy(a.value.string, s, len);
	a.value.string[len] = '\0';
	return a;
}

YasAtom yas_make_vector(size_t size, YasAtom fill)
{
	YasAtom a;
	struct YasVector *v = malloc(sizeof(struct YasVector));
	v->size = size;
	v->capacity = size > 0 ? size : 4;
	v->data = malloc(sizeof(YasAtom) * v->capacity);
	for (size_t i = 0; i < size; i++) {
		v->data[i] = fill;
	}
	a.type = YAS_ATOM_TYPE_VECTOR;
	a.value.vector = v;
	
	v->mark = 0;
	v->next = global_vectors;
	global_vectors = v;
	
	return a;
}

YasAtom yas_make_hash(void)
{
	YasAtom a;
	struct YasHash *h = malloc(sizeof(struct YasHash));
	h->size = 0;
	h->capacity = 16;
	h->keys = malloc(sizeof(YasAtom) * h->capacity);
	h->values = malloc(sizeof(YasAtom) * h->capacity);
	h->metatable = NULL;
	a.type = YAS_ATOM_TYPE_HASH;
	a.value.hash = h;
	
	h->mark = 0;
	h->next = global_hashes;
	global_hashes = h;
	
	return a;
}

/* Vector operations */
YasAtom yas_vector_ref(YasAtom vec, size_t index)
{
	return vec.value.vector->data[index];
}

void yas_vector_set(YasAtom vec, size_t index, YasAtom value)
{
	vec.value.vector->data[index] = value;
}

size_t yas_vector_length(YasAtom vec)
{
	return vec.value.vector->size;
}

/* Hash operations - linear probing for simplicity */
static int atom_equal(YasAtom a, YasAtom b)
{
	if (a.type != b.type) return 0;
	switch (a.type) {
	case YAS_ATOM_TYPE_NIL: return 1;
	case YAS_ATOM_TYPE_SYMBOL: return a.value.symbol == b.value.symbol;
	case YAS_ATOM_TYPE_KEYWORD: return a.value.keyword == b.value.keyword;
	case YAS_ATOM_TYPE_INTEGER: return a.value.integer == b.value.integer;
	case YAS_ATOM_TYPE_FLOAT: return a.value.floatval == b.value.floatval;
	case YAS_ATOM_TYPE_STRING: return strcmp(a.value.string, b.value.string) == 0;
	default: return a.value.pair == b.value.pair;
	}
}

YasAtom yas_hash_ref(YasAtom hash, YasAtom key, YasAtom default_val)
{
	struct YasHash *h = hash.value.hash;
	for (size_t i = 0; i < h->size; i++) {
		if (atom_equal(h->keys[i], key))
			return h->values[i];
	}
	return default_val;
}

void yas_hash_set(YasAtom hash, YasAtom key, YasAtom value)
{
	struct YasHash *h = hash.value.hash;
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
		h->keys = realloc(h->keys, sizeof(YasAtom) * h->capacity);
		h->values = realloc(h->values, sizeof(YasAtom) * h->capacity);
	}
	h->keys[h->size] = key;
	h->values[h->size] = value;
	h->size++;
}

int yas_hash_remove(YasAtom hash, YasAtom key)
{
	struct YasHash *h = hash.value.hash;
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

size_t yas_hash_size(YasAtom hash)
{
	return hash.value.hash->size;
}

/* Metatable operations */
YasAtom yas_get_metamethod(YasAtom hash, const char *name)
{
	struct YasHash *h = hash.value.hash;
	if (!h->metatable) return yas_nil;
	
	/* Use keyword for metamethod lookup (uppercase to match reader) */
	char kw_name[64];
	snprintf(kw_name, sizeof(kw_name), "%s", name);
	/* Convert to uppercase */
	for (char *p = kw_name; *p; p++) {
		if (*p >= 'a' && *p <= 'z') *p -= 32;
	}
	YasAtom key = yas_make_keyword(kw_name);
	
	/* Look up in metatable */
	YasAtom mt;
	mt.type = YAS_ATOM_TYPE_HASH;
	mt.value.hash = h->metatable;
	return yas_hash_ref(mt, key, yas_nil);
}

int yas_has_metamethod(YasAtom hash, const char *name)
{
	return !yas_nilp(yas_get_metamethod(hash, name));
}

int yas_listp(YasAtom expr)
{
	while (!yas_nilp(expr)) {
		if (expr.type != YAS_ATOM_TYPE_PAIR)
			return 0;
		expr = yas_cdr(expr);
	}
	return 1;
}

YasAtom yas_copy_list(YasAtom list)
{
	YasAtom a, p;

	if (yas_nilp(list))
		return yas_nil;

	a = yas_cons(yas_car(list), yas_nil);
	p = a;
	list = yas_cdr(list);

	while (!yas_nilp(list)) {
		yas_cdr(p) = yas_cons(yas_car(list), yas_nil);
		p = yas_cdr(p);
		list = yas_cdr(list);
	}

	return a;
}

YasAtom yas_list_create(int n, ...)
{
	va_list ap;
	YasAtom list = yas_nil;

	va_start(ap, n);
	while (n--) {
		YasAtom item = va_arg(ap, YasAtom);
		list = yas_cons(item, list);
	}
	va_end(ap);

	yas_list_reverse(&list);
	return list;
}

YasAtom yas_list_get(YasAtom list, int k)
{
	while (k--)
		list = yas_cdr(list);
	return yas_car(list);
}

void yas_list_set(YasAtom list, int k, YasAtom value)
{
	while (k--)
		list = yas_cdr(list);
	yas_car(list) = value;
}

void yas_list_reverse(YasAtom *list)
{
	YasAtom tail = yas_nil;
	while (!yas_nilp(*list)) {
		YasAtom p = yas_cdr(*list);
		yas_cdr(*list) = tail;
		tail = *list;
		*list = p;
	}
	*list = tail;
}

void yas_gc_mark(YasAtom root)
{
	struct Allocation *a;

	if (root.type == YAS_ATOM_TYPE_VECTOR) {
		if (root.value.vector->mark) return;
		root.value.vector->mark = 1;
		for (size_t i = 0; i < root.value.vector->size; i++) {
			yas_gc_mark(root.value.vector->data[i]);
		}
		return;
	}

	if (root.type == YAS_ATOM_TYPE_HASH) {
		if (root.value.hash->mark) return;
		root.value.hash->mark = 1;
		if (root.value.hash->metatable) {
			YasAtom mt; 
			mt.type = YAS_ATOM_TYPE_HASH; 
			mt.value.hash = root.value.hash->metatable;
			yas_gc_mark(mt);
		}
		for (size_t i = 0; i < root.value.hash->size; i++) {
			yas_gc_mark(root.value.hash->keys[i]);
			yas_gc_mark(root.value.hash->values[i]);
		}
		return;
	}

	if (root.type == YAS_ATOM_TYPE_STRING) {
		struct YasStringAlloc *sa = (struct YasStringAlloc *)
			((char *) root.value.string
				- offsetof(struct YasStringAlloc, data));
		sa->mark = 1;
		return;
	}

	if (root.type == YAS_ATOM_TYPE_PORT) {
		root.value.port->mark = 1;
		return;
	}

	if (root.type == YAS_ATOM_TYPE_BYTEVECTOR) {
		root.value.bytevector->mark = 1;
		return;
	}

	if (root.type == YAS_ATOM_TYPE_VEC2 || root.type == YAS_ATOM_TYPE_VEC3 || root.type == YAS_ATOM_TYPE_VEC4) {
		struct YasVecAlloc *va = (struct YasVecAlloc *)
			((char *) root.value.vec
				- offsetof(struct YasVecAlloc, v));
		va->mark = 1;
		return;
	}

	if (root.type == YAS_ATOM_TYPE_MAT4) {
		struct YasMat4Alloc *ma = (struct YasMat4Alloc *)
			((char *) root.value.mat4
				- offsetof(struct YasMat4Alloc, m));
		ma->mark = 1;
		return;
	}

#ifdef YAS_FFI
	if (root.type == YAS_ATOM_TYPE_FFI_FUNCTION) {
		if (root.value.ffi_fn) root.value.ffi_fn->mark = 1;
		return;
	}

	if (root.type == YAS_ATOM_TYPE_FFI_STRUCT) {
		if (root.value.ffi_struct) root.value.ffi_struct->mark = 1;
		return;
	}
#endif

	if (!(root.type == YAS_ATOM_TYPE_PAIR
		|| root.type == YAS_ATOM_TYPE_CLOSURE
		|| root.type == YAS_ATOM_TYPE_MACRO
		|| root.type == YAS_ATOM_TYPE_CONTINUATION))
		return;

	a = (struct Allocation *)
		((char *) root.value.pair
			- offsetof(struct Allocation, pair));

	if (a->mark)
		return;

	a->mark = 1;

	yas_gc_mark(yas_car(root));
	yas_gc_mark(yas_cdr(root));
}

void yas_gc()
{
	struct Allocation *a, **p;

	yas_gc_mark(sym_table);
	yas_gc_mark(kw_table);
	
	/* Mark global roots */
	yas_gc_mark(yas_library_registry);
	yas_gc_mark(yas_global_search_paths);
	yas_gc_mark(yas_global_features);

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
	
	/* Sweep Vectors */
	struct YasVector **pv = &global_vectors;
	while (*pv != NULL) {
		struct YasVector *v = *pv;
		if (!v->mark) {
			*pv = v->next;
			free(v->data);
			free(v);
		} else {
			v->mark = 0;
			pv = &v->next;
		}
	}
	
	/* Sweep Hashes */
	struct YasHash **ph = &global_hashes;
	while (*ph != NULL) {
		struct YasHash *h = *ph;
		if (!h->mark) {
			*ph = h->next;
			free(h->keys);
			free(h->values);
			free(h);
		} else {
			h->mark = 0;
			ph = &h->next;
		}
	}

	/* Sweep Strings */
	struct YasStringAlloc **ps = &global_strings;
	while (*ps != NULL) {
		struct YasStringAlloc *sa = *ps;
		if (!sa->mark) {
			*ps = sa->next;
			free(sa);
		} else {
			sa->mark = 0;
			ps = &sa->next;
		}
	}

	/* Sweep Ports (unmarked but still open ports are closed before freeing) */
	struct YasPort **pp = &global_ports;
	while (*pp != NULL) {
		struct YasPort *port = *pp;
		if (!port->mark) {
			*pp = port->next;
			if (port->is_open) {
				YasAtom tmp;
				tmp.type = YAS_ATOM_TYPE_PORT;
				tmp.value.port = port;
				yas_port_close(tmp);
			}
			free(port->path);
			free(port);
		} else {
			port->mark = 0;
			pp = &port->next;
		}
	}

	/* Sweep Bytevectors */
	struct YasBytevector **pbv = &global_bytevectors;
	while (*pbv != NULL) {
		struct YasBytevector *bv = *pbv;
		if (!bv->mark) {
			*pbv = bv->next;
			free(bv->data);
			free(bv);
		} else {
			bv->mark = 0;
			pbv = &bv->next;
		}
	}

	/* Sweep Vec2/Vec3/Vec4 */
	struct YasVecAlloc **pva = &global_vecs;
	while (*pva != NULL) {
		struct YasVecAlloc *va = *pva;
		if (!va->mark) {
			*pva = va->next;
			free(va);
		} else {
			va->mark = 0;
			pva = &va->next;
		}
	}

	/* Sweep Mat4 */
	struct YasMat4Alloc **pma = &global_mats;
	while (*pma != NULL) {
		struct YasMat4Alloc *ma = *pma;
		if (!ma->mark) {
			*pma = ma->next;
			free(ma);
		} else {
			ma->mark = 0;
			pma = &ma->next;
		}
	}

#ifdef YAS_FFI
	/* Sweep FFIFunctions */
	struct YasFFIFunction **pfn = &yas_global_ffi_functions;
	while (*pfn != NULL) {
		struct YasFFIFunction *fn = *pfn;
		if (!fn->mark) {
			*pfn = fn->next;
			yas_ffi_free_function(fn);
		} else {
			fn->mark = 0;
			pfn = &fn->next;
		}
	}

	/* Sweep FFIStruct instances (the type registry entry is not owned here) */
	YasFFIStructInstance **pfs = &yas_global_ffi_structs;
	while (*pfs != NULL) {
		YasFFIStructInstance *inst = *pfs;
		if (!inst->mark) {
			*pfs = inst->next;
			free(inst->data);
			free(inst);
		} else {
			inst->mark = 0;
			pfs = &inst->next;
		}
	}
#endif
}

/* Port operations */
YasAtom yas_make_port(void *handle, const char *path, int is_input, int is_binary, int provider_id)
{
	YasAtom a;
	struct YasPort *p = malloc(sizeof(struct YasPort));
	p->handle = handle;
	p->path = strdup(path);
	p->is_input = is_input;
	p->is_binary = is_binary;
	p->is_open = 1;
	p->provider_id = provider_id;
	a.type = YAS_ATOM_TYPE_PORT;
	a.value.port = p;

	p->mark = 0;
	p->next = global_ports;
	global_ports = p;

	return a;
}

/* Bytevector operations */
YasAtom yas_make_bytevector(size_t size)
{
	YasAtom a;
	struct YasBytevector *bv = malloc(sizeof(struct YasBytevector));
	bv->size = size;
	bv->data = calloc(size, 1);
	a.type = YAS_ATOM_TYPE_BYTEVECTOR;
	a.value.bytevector = bv;

	bv->mark = 0;
	bv->next = global_bytevectors;
	global_bytevectors = bv;

	return a;
}

YasAtom yas_make_bytevector_from_data(const unsigned char *data, size_t size)
{
	YasAtom a;
	struct YasBytevector *bv = malloc(sizeof(struct YasBytevector));
	bv->size = size;
	bv->data = malloc(size);
	memcpy(bv->data, data, size);
	a.type = YAS_ATOM_TYPE_BYTEVECTOR;
	a.value.bytevector = bv;

	bv->mark = 0;
	bv->next = global_bytevectors;
	global_bytevectors = bv;

	return a;
}

unsigned char yas_bytevector_ref(YasAtom bv, size_t index)
{
	return bv.value.bytevector->data[index];
}

void yas_bytevector_set(YasAtom bv, size_t index, unsigned char value)
{
	bv.value.bytevector->data[index] = value;
}

size_t yas_bytevector_length(YasAtom bv)
{
	return bv.value.bytevector->size;
}

/* SIMD Vector/Matrix constructors */
static yas_vec4_t *gc_alloc_vec(void)
{
	struct YasVecAlloc *va = malloc(sizeof(struct YasVecAlloc));
	va->mark = 0;
	va->next = global_vecs;
	global_vecs = va;
	return &va->v;
}

static yas_mat4_t *gc_alloc_mat4(void)
{
	struct YasMat4Alloc *ma = malloc(sizeof(struct YasMat4Alloc));
	ma->mark = 0;
	ma->next = global_mats;
	global_mats = ma;
	return &ma->m;
}

YasAtom yas_make_vec2(float x, float y)
{
	YasAtom a;
	a.type = YAS_ATOM_TYPE_VEC2;
	a.value.vec = gc_alloc_vec();
#ifdef YAS_SIMD_X86
	a.value.vec->v = _mm_set_ps(0, 0, y, x);
#elif defined(YAS_SIMD_ARM_NEON)
	float vals[4] = {x, y, 0, 0};
	a.value.vec->v = vld1q_f32(vals);
#elif defined(YAS_SIMD_WASM)
	a.value.vec->v = wasm_f32x4_make(x, y, 0, 0);
#else
	a.value.vec->data[0] = x;
	a.value.vec->data[1] = y;
	a.value.vec->data[2] = 0;
	a.value.vec->data[3] = 0;
#endif
	return a;
}

YasAtom yas_make_vec3(float x, float y, float z)
{
	YasAtom a;
	a.type = YAS_ATOM_TYPE_VEC3;
	a.value.vec = gc_alloc_vec();
#ifdef YAS_SIMD_X86
	a.value.vec->v = _mm_set_ps(0, z, y, x);
#elif defined(YAS_SIMD_ARM_NEON)
	float vals[4] = {x, y, z, 0};
	a.value.vec->v = vld1q_f32(vals);
#elif defined(YAS_SIMD_WASM)
	a.value.vec->v = wasm_f32x4_make(x, y, z, 0);
#else
	a.value.vec->data[0] = x;
	a.value.vec->data[1] = y;
	a.value.vec->data[2] = z;
	a.value.vec->data[3] = 0;
#endif
	return a;
}

YasAtom yas_make_vec4(float x, float y, float z, float w)
{
	YasAtom a;
	a.type = YAS_ATOM_TYPE_VEC4;
	a.value.vec = gc_alloc_vec();
#ifdef YAS_SIMD_X86
	a.value.vec->v = _mm_set_ps(w, z, y, x);
#elif defined(YAS_SIMD_ARM_NEON)
	float vals[4] = {x, y, z, w};
	a.value.vec->v = vld1q_f32(vals);
#elif defined(YAS_SIMD_WASM)
	a.value.vec->v = wasm_f32x4_make(x, y, z, w);
#else
	a.value.vec->data[0] = x;
	a.value.vec->data[1] = y;
	a.value.vec->data[2] = z;
	a.value.vec->data[3] = w;
#endif
	return a;
}

YasAtom yas_make_mat4_identity(void)
{
	YasAtom a;
	a.type = YAS_ATOM_TYPE_MAT4;
	a.value.mat4 = gc_alloc_mat4();
	/* Column-major identity matrix */
	for (int c = 0; c < 4; c++) {
#ifdef YAS_SIMD_X86
		a.value.mat4->columns[c].v = _mm_set_ps(
			c == 3 ? 1.0f : 0.0f,
			c == 2 ? 1.0f : 0.0f,
			c == 1 ? 1.0f : 0.0f,
			c == 0 ? 1.0f : 0.0f);
#elif defined(YAS_SIMD_ARM_NEON)
		float vals[4] = {
			c == 0 ? 1.0f : 0.0f,
			c == 1 ? 1.0f : 0.0f,
			c == 2 ? 1.0f : 0.0f,
			c == 3 ? 1.0f : 0.0f};
		a.value.mat4->columns[c].v = vld1q_f32(vals);
#elif defined(YAS_SIMD_WASM)
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

YasAtom yas_make_mat4_from_floats(const float *values)
{
	YasAtom a;
	a.type = YAS_ATOM_TYPE_MAT4;
	a.value.mat4 = gc_alloc_mat4();
	/* Values are column-major: first 4 = column 0, etc. */
	for (int c = 0; c < 4; c++) {
#ifdef YAS_SIMD_X86
		a.value.mat4->columns[c].v = _mm_loadu_ps(values + c * 4);
#elif defined(YAS_SIMD_ARM_NEON)
		a.value.mat4->columns[c].v = vld1q_f32(values + c * 4);
#elif defined(YAS_SIMD_WASM)
		a.value.mat4->columns[c].v = wasm_v128_load(values + c * 4);
#else
		for (int r = 0; r < 4; r++) {
			a.value.mat4->columns[c].data[r] = values[c * 4 + r];
		}
#endif
	}
	return a;
}

/* ============================================================
 * GLOBAL FEATURES SYSTEM
 * ============================================================ */

YasAtom yas_global_features = { YAS_ATOM_TYPE_NIL };

void yas_init_features(void)
{
	yas_global_features = yas_nil;

	/* Platform detection */
#if defined(__APPLE__)
	yas_global_features = yas_cons(yas_make_sym("MACOS"), yas_global_features);
	yas_global_features = yas_cons(yas_make_sym("UNIX"), yas_global_features);
#elif defined(_WIN32)
	yas_global_features = yas_cons(yas_make_sym("WINDOWS"), yas_global_features);
#elif defined(__linux__)
	yas_global_features = yas_cons(yas_make_sym("LINUX"), yas_global_features);
	yas_global_features = yas_cons(yas_make_sym("UNIX"), yas_global_features);
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
	yas_global_features = yas_cons(yas_make_sym("BSD"), yas_global_features);
	yas_global_features = yas_cons(yas_make_sym("UNIX"), yas_global_features);
#endif

	/* Architecture detection */
#if defined(__x86_64__) || defined(_M_X64)
	yas_global_features = yas_cons(yas_make_sym("X86-64"), yas_global_features);
#elif defined(__i386__) || defined(_M_IX86)
	yas_global_features = yas_cons(yas_make_sym("X86"), yas_global_features);
#elif defined(__aarch64__) || defined(_M_ARM64)
	yas_global_features = yas_cons(yas_make_sym("ARM64"), yas_global_features);
#elif defined(__arm__) || defined(_M_ARM)
	yas_global_features = yas_cons(yas_make_sym("ARM"), yas_global_features);
#elif defined(__wasm__)
	yas_global_features = yas_cons(yas_make_sym("WASM"), yas_global_features);
#endif

	/* Optional features - compile-time flags */
#ifdef YAS_FFI
	yas_global_features = yas_cons(yas_make_sym("FFI"), yas_global_features);
#endif
}

int yas_has_feature(const char *name)
{
	YasAtom p = yas_global_features;
	while (!yas_nilp(p)) {
		if (yas_car(p).type == YAS_ATOM_TYPE_SYMBOL &&
		    strcmp(yas_car(p).value.symbol, name) == 0) {
			return 1;
		}
		p = yas_cdr(p);
	}
	return 0;
}

/* Evaluate compound feature expressions like (or macos linux), (and x y), (not x) */
int yas_eval_feature_expr(YasAtom expr)
{
	/* Simple symbol */
	if (expr.type == YAS_ATOM_TYPE_SYMBOL) {
		return yas_has_feature(expr.value.symbol);
	}

	/* Compound expression */
	if (expr.type == YAS_ATOM_TYPE_PAIR && !yas_nilp(expr)) {
		YasAtom op = yas_car(expr);
		YasAtom args = yas_cdr(expr);

		if (op.type == YAS_ATOM_TYPE_SYMBOL) {
			/* (or ...) - true if any arg is true */
			if (strcmp(op.value.symbol, "OR") == 0) {
				while (!yas_nilp(args)) {
					if (yas_eval_feature_expr(yas_car(args)))
						return 1;
					args = yas_cdr(args);
				}
				return 0;
			}
			/* (and ...) - true if all args are true */
			if (strcmp(op.value.symbol, "AND") == 0) {
				while (!yas_nilp(args)) {
					if (!yas_eval_feature_expr(yas_car(args)))
						return 0;
					args = yas_cdr(args);
				}
				return 1;
			}
			/* (not x) - true if x is false */
			if (strcmp(op.value.symbol, "NOT") == 0) {
				if (!yas_nilp(args)) {
					return !yas_eval_feature_expr(yas_car(args));
				}
				return 0;
			}
			/* Single symbol in parens like (macos) */
			if (yas_nilp(args)) {
				return yas_has_feature(op.value.symbol);
			}
		}
	}

	return 0;
}


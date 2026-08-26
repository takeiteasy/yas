#include "marshal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Neutral value marshaler - see marshal.h for the contract and
 * docs/CONCURRENCY.md ("Value transfer") for the design. Two passes
 * over the same node vocabulary:
 *
 *   marshal:   source-context YasAtom tree -> neutral nodes. One
 *              open-addressed map keyed on SOURCE object identity
 *              dedups DAG sharing and terminates cycles in a single
 *              mechanism: a node is registered in the map before its
 *              children are walked, so re-entering an object finds the
 *              existing (possibly still incomplete) node.
 *
 *   unmarshal: neutral nodes -> bound-context values. The memo lives
 *              in the node itself (rebuilt/rebuilt_atom), also
 *              registered before children are rebuilt; containers are
 *              pre-allocated empty and filled afterwards so a cycle's
 *              back-edge observes the real container, not nil. All
 *              locals held across allocating calls are rooted per
 *              docs/GC.md ("accumulator loops" hazard shape).
 *
 * Both walks recurse. Depth is bounded by source structure depth like
 * every other recursive walk in the interpreter (reader, printer, GC
 * scan); an explicit worklist would lift that ceiling if it ever
 * matters in practice. */

/* ── Pointer-keyed map (marshal-phase memo) ────────────────────────── */

struct SrcMap {
    void **keys;
    struct YasMarshalNode **vals;
    size_t capacity; /* power of two */
    size_t count;
};

struct FreeSet {
    struct YasMarshalNode **nodes;
    size_t count;
    size_t capacity;
};

static void marshal_free_walk(struct YasMarshalNode *n, struct FreeSet *seen);

static size_t ptr_hash(const void *p)
{
    uintptr_t x = (uintptr_t)p;
    x ^= x >> 13;
    x *= (uintptr_t)0xff51afd7ed558ccdULL;
    x ^= x >> 13;
    return (size_t)x;
}

static int src_map_init(struct SrcMap *m)
{
    m->capacity = 64;
    m->count = 0;
    m->keys = calloc(m->capacity, sizeof(void *));
    m->vals = calloc(m->capacity, sizeof(struct YasMarshalNode *));
    return m->keys && m->vals;
}

static void src_map_put(struct SrcMap *m, void *key, struct YasMarshalNode *val)
{
    size_t mask = m->capacity - 1;
    size_t i = ptr_hash(key) & mask;
    while (m->keys[i]) {
        if (m->keys[i] == key) {
            m->vals[i] = val;
            return;
        }
        i = (i + 1) & mask;
    }
    m->keys[i] = key;
    m->vals[i] = val;
    m->count++;
}

static struct YasMarshalNode *src_map_get(struct SrcMap *m, void *key)
{
    if (!m->capacity)
        return NULL;
    size_t mask = m->capacity - 1;
    size_t i = ptr_hash(key) & mask;
    while (m->keys[i]) {
        if (m->keys[i] == key)
            return m->vals[i];
        i = (i + 1) & mask;
    }
    return NULL;
}

/* Grow at 70% load, rehashing everything (same policy as data.c's
 * intern tables). */
static int src_map_maybe_grow(struct SrcMap *m)
{
    if ((m->count + 1) * 10 < m->capacity * 7)
        return 1;

    struct SrcMap old = *m;
    m->capacity = old.capacity * 2;
    m->count = 0;
    m->keys = calloc(m->capacity, sizeof(void *));
    m->vals = calloc(m->capacity, sizeof(struct YasMarshalNode *));
    if (!m->keys || !m->vals) {
        free(m->keys);
        free(m->vals);
        *m = old;
        return 0;
    }
    for (size_t i = 0; i < old.capacity; i++)
        if (old.keys[i])
            src_map_put(m, old.keys[i], old.vals[i]);
    free(old.keys);
    free(old.vals);
    return 1;
}

/* ── Node allocation ───────────────────────────────────────────────── */

static struct YasMarshalNode *node_new(unsigned char kind)
{
    /* calloc: rebuilt flag and the union start zeroed. */
    struct YasMarshalNode *n = calloc(1, sizeof(struct YasMarshalNode));
    if (n)
        n->kind = kind;
    return n;
}

/* ── Marshal phase ─────────────────────────────────────────────────── */

static const char *atom_type_name(YasAtom v)
{
    switch (v.type) {
        case YAS_ATOM_TYPE_BUILTIN:
        case YAS_ATOM_TYPE_CLOSURE:
        case YAS_ATOM_TYPE_MACRO:
            return "procedure";
        case YAS_ATOM_TYPE_CONTINUATION:
            return "continuation";
        case YAS_ATOM_TYPE_PORT:
            return "port";
        case YAS_ATOM_TYPE_ENV:
            return "environment";
        case YAS_ATOM_TYPE_FRAME:
            return "control frame";
#ifdef YAS_FFI
        case YAS_ATOM_TYPE_FFI_LIBRARY:
        case YAS_ATOM_TYPE_FFI_POINTER:
        case YAS_ATOM_TYPE_FFI_FUNCTION:
        case YAS_ATOM_TYPE_FFI_STRUCT:
            return "ffi value";
#endif
        default:
            return "value of unsupported type";
    }
}

static struct YasMarshalNode *marshal_walk(YasAtom v, struct SrcMap *seen);

/* Marshal n atoms into a parallel node array. On any failure returns
 * NULL having freed only the array itself - every node built so far is
 * reachable from `seen` (all containers register themselves), so
 * yas_marshal()'s single top-level unwind reclaims the whole partial
 * tree; yas_marshal_free()'s freed guard makes that overlap safe. */
static struct YasMarshalNode **marshal_node_array(YasAtom *atoms, size_t n, struct SrcMap *seen)
{
    struct YasMarshalNode **out = malloc((n ? n : 1) * sizeof(struct YasMarshalNode *));
    if (!out)
        return NULL;
    for (size_t i = 0; i < n; i++) {
        out[i] = marshal_walk(atoms[i], seen);
        if (!out[i]) {
            free(out);
            return NULL;
        }
    }
    return out;
}

static struct YasMarshalNode *marshal_walk(YasAtom v, struct SrcMap *seen)
{
    switch (v.type) {
        case YAS_ATOM_TYPE_NIL:
            return node_new(YNM_NIL);
        case YAS_ATOM_TYPE_INTEGER: {
            struct YasMarshalNode *n = node_new(YNM_INTEGER);
            if (n)
                n->u.integer = v.value.integer;
            return n;
        }
        case YAS_ATOM_TYPE_FLOAT: {
            struct YasMarshalNode *n = node_new(YNM_FLOAT);
            if (n)
                memcpy(&n->u.float_bits, &v.value.floatval, sizeof(n->u.float_bits));
            return n;
        }
        case YAS_ATOM_TYPE_CHAR: {
            struct YasMarshalNode *n = node_new(YNM_CHAR);
            if (n)
                n->u.character = v.value.character;
            return n;
        }
        case YAS_ATOM_TYPE_SYMBOL:
        case YAS_ATOM_TYPE_KEYWORD: {
            struct YasMarshalNode *n = node_new(v.type == YAS_ATOM_TYPE_SYMBOL ? YNM_SYMBOL : YNM_KEYWORD);
            if (!n)
                return NULL;
            n->u.name = strdup(v.value.symbol);
            if (!n->u.name) {
                yas_marshal_free(n);
                return NULL;
            }
            return n;
        }
        case YAS_ATOM_TYPE_STRING: {
            struct YasMarshalNode *n = node_new(YNM_STRING);
            if (!n)
                return NULL;
            n->u.string.len = strlen(v.value.string);
            n->u.string.bytes = malloc(n->u.string.len + 1);
            if (!n->u.string.bytes) {
                yas_marshal_free(n);
                return NULL;
            }
            memcpy(n->u.string.bytes, v.value.string, n->u.string.len + 1);
            return n;
        }
        case YAS_ATOM_TYPE_BYTEVECTOR: {
            struct YasBytevector *bv = v.value.bytevector;
            struct YasMarshalNode *n = node_new(YNM_BYTEVECTOR);
            if (!n)
                return NULL;
            n->u.bytevector.len = bv->size;
            n->u.bytevector.bytes = malloc(bv->size ? bv->size : 1);
            if (!n->u.bytevector.bytes) {
                yas_marshal_free(n);
                return NULL;
            }
            memcpy(n->u.bytevector.bytes, bv->data, bv->size);
            return n;
        }
        case YAS_ATOM_TYPE_VEC2:
        case YAS_ATOM_TYPE_VEC3:
        case YAS_ATOM_TYPE_VEC4: {
            unsigned char kind = v.type == YAS_ATOM_TYPE_VEC2     ? YNM_VEC2
                                 : v.type == YAS_ATOM_TYPE_VEC3   ? YNM_VEC3
                                                                  : YNM_VEC4;
            struct YasMarshalNode *n = node_new(kind);
            if (!n)
                return NULL;
            memcpy(n->u.vec, v.value.vec, sizeof(float) * 4);
            return n;
        }
        case YAS_ATOM_TYPE_MAT4: {
            struct YasMarshalNode *n = node_new(YNM_MAT4);
            if (!n)
                return NULL;
            memcpy(n->u.mat, v.value.mat4, sizeof(float) * 16);
            return n;
        }
        case YAS_ATOM_TYPE_PAIR: {
            struct YasMarshalNode *hit = src_map_get(seen, v.value.pair);
            if (hit)
                return hit;
            if (!src_map_maybe_grow(seen))
                return NULL;
            struct YasMarshalNode *n = node_new(YNM_PAIR);
            if (!n)
                return NULL;
            /* Register before descending: cycles terminate here. */
            src_map_put(seen, v.value.pair, n);
            n->u.pair.car = marshal_walk(yas_car(v), seen);
            if (!n->u.pair.car)
                return NULL;
            n->u.pair.cdr = marshal_walk(yas_cdr(v), seen);
            if (!n->u.pair.cdr)
                return NULL;
            return n;
        }
        case YAS_ATOM_TYPE_VECTOR: {
            struct YasVector *vec = v.value.vector;
            struct YasMarshalNode *hit = src_map_get(seen, vec);
            if (hit)
                return hit;
            if (!src_map_maybe_grow(seen))
                return NULL;
            struct YasMarshalNode *n = node_new(YNM_VECTOR);
            if (!n)
                return NULL;
            src_map_put(seen, vec, n);
            n->u.vector.n = vec->size;
            n->u.vector.items = marshal_node_array(vec->data, vec->size, seen);
            if (!n->u.vector.items) {
                yas_marshal_free(n);
                return NULL;
            }
            return n;
        }
        case YAS_ATOM_TYPE_HASH: {
            struct YasHash *h = v.value.hash;
            struct YasMarshalNode *hit = src_map_get(seen, h);
            if (hit)
                return hit;
            if (!src_map_maybe_grow(seen))
                return NULL;
            struct YasMarshalNode *n = node_new(YNM_HASH);
            if (!n)
                return NULL;
            src_map_put(seen, h, n);
            n->u.hash.n = h->size;
            n->u.hash.keys = marshal_node_array(h->keys, h->size, seen);
            if (!n->u.hash.keys) {
                yas_marshal_free(n);
                return NULL;
            }
            n->u.hash.vals = marshal_node_array(h->values, h->size, seen);
            if (!n->u.hash.vals) {
                free(n->u.hash.keys);
                yas_marshal_free(n);
                return NULL;
            }
            if (h->metatable) {
                YasAtom mt;
                mt.type = YAS_ATOM_TYPE_HASH;
                mt.value.hash = h->metatable;
                n->u.hash.metatable = marshal_walk(mt, seen);
                if (!n->u.hash.metatable) {
                    free(n->u.hash.keys);
                    free(n->u.hash.vals);
                    yas_marshal_free(n);
                    return NULL;
                }
            }
            return n;
        }
        default: {
            char msg[128];
            snprintf(msg, sizeof(msg),
                     "cannot transfer a %s across contexts "
                     "(only data types cross)",
                     atom_type_name(v));
            yas_raise_error("type-error", msg);
            return NULL;
        }
    }
}

struct YasMarshalNode *yas_marshal(YasAtom v)
{
    struct SrcMap seen = {0};
    if (!src_map_init(&seen)) {
        yas_raise_error("memory", "marshal: out of memory");
        return NULL;
    }
    struct YasMarshalNode *root = marshal_walk(v, &seen);
    if (!root) {
        /* Unwind whatever partial subtree was built before the error. */
        struct FreeSet freed = {0};
        for (size_t i = 0; i < seen.capacity; i++)
            if (seen.keys[i])
                marshal_free_walk(seen.vals[i], &freed);
        free(freed.nodes);
    }
    free(seen.keys);
    free(seen.vals);
    return root;
}

/* ── Unmarshal phase ───────────────────────────────────────────────── */

static int unmarshal_walk(struct YasMarshalNode *n, YasAtom *out)
{
    if (n->rebuilt) {
        *out = n->rebuilt_atom;
        return YAS_ERROR_OK;
    }

    switch (n->kind) {
        case YNM_NIL:
            *out = yas_nil;
            break;
        case YNM_INTEGER:
            *out = yas_make_int(n->u.integer);
            break;
        case YNM_FLOAT: {
            double d;
            memcpy(&d, &n->u.float_bits, sizeof(d));
            *out = yas_make_float(d);
            break;
        }
        case YNM_CHAR:
            *out = yas_make_char(n->u.character);
            break;
        case YNM_SYMBOL:
            *out = yas_make_sym(n->u.name); /* re-interns in this context */
            break;
        case YNM_KEYWORD:
            *out = yas_make_keyword(n->u.name);
            break;
        case YNM_STRING:
            *out = yas_make_string_n(n->u.string.bytes, n->u.string.len);
            break;
        case YNM_BYTEVECTOR:
            *out = yas_make_bytevector_from_data(n->u.bytevector.bytes,
                                                 n->u.bytevector.len);
            break;
        case YNM_VEC2:
            *out = yas_make_vec2(n->u.vec[0], n->u.vec[1]);
            break;
        case YNM_VEC3:
            *out = yas_make_vec3(n->u.vec[0], n->u.vec[1], n->u.vec[2]);
            break;
        case YNM_VEC4:
            *out = yas_make_vec4(n->u.vec[0], n->u.vec[1], n->u.vec[2],
                                 n->u.vec[3]);
            break;
        case YNM_MAT4:
            *out = yas_make_mat4_from_floats(n->u.mat);
            break;
        case YNM_PAIR: {
            /* Pre-allocate, register the memo, then fill - a cyclic
             * back-edge among the children must find this exact cell,
             * not recurse into building it again. */
            YasAtom cell = yas_cons(yas_nil, yas_nil);
            n->rebuilt_atom = cell;
            n->rebuilt = 1;

            YasAtom car_a = yas_nil, cdr_a = yas_nil;
            yas_gc_push_root(&cell);
            yas_gc_push_root(&car_a);
            yas_gc_push_root(&cdr_a);
            int rc = unmarshal_walk(n->u.pair.car, &car_a);
            if (rc == YAS_ERROR_OK)
                rc = unmarshal_walk(n->u.pair.cdr, &cdr_a);
            if (rc == YAS_ERROR_OK) {
                yas_set_car(cell, car_a);
                yas_set_cdr(cell, cdr_a);
            }
            yas_gc_pop_roots(3);
            if (rc != YAS_ERROR_OK)
                return rc;
            *out = cell;
            return YAS_ERROR_OK;
        }
        case YNM_VECTOR: {
            YasAtom vec = yas_make_vector(n->u.vector.n, yas_nil);
            n->rebuilt_atom = vec;
            n->rebuilt = 1;

            yas_gc_push_root(&vec);
            int rc = YAS_ERROR_OK;
            for (size_t i = 0; i < n->u.vector.n && rc == YAS_ERROR_OK; i++) {
                YasAtom item = yas_nil;
                yas_gc_push_root(&item);
                rc = unmarshal_walk(n->u.vector.items[i], &item);
                if (rc == YAS_ERROR_OK)
                    yas_vector_set(vec, i, item);
                yas_gc_pop_roots(1);
            }
            yas_gc_pop_roots(1);
            if (rc != YAS_ERROR_OK)
                return rc;
            *out = vec;
            return YAS_ERROR_OK;
        }
        case YNM_HASH: {
            YasAtom hash = yas_make_hash();
            n->rebuilt_atom = hash;
            n->rebuilt = 1;

            yas_gc_push_root(&hash);
            int rc = YAS_ERROR_OK;
            for (size_t i = 0; i < n->u.hash.n && rc == YAS_ERROR_OK; i++) {
                YasAtom k = yas_nil, val = yas_nil;
                yas_gc_push_root(&k);
                yas_gc_push_root(&val);
                rc = unmarshal_walk(n->u.hash.keys[i], &k);
                if (rc == YAS_ERROR_OK)
                    rc = unmarshal_walk(n->u.hash.vals[i], &val);
                if (rc == YAS_ERROR_OK)
                    yas_hash_set(hash, k, val);
                yas_gc_pop_roots(2);
            }
            if (rc == YAS_ERROR_OK && n->u.hash.metatable) {
                YasAtom mt = yas_nil;
                yas_gc_push_root(&mt);
                rc = unmarshal_walk(n->u.hash.metatable, &mt);
                if (rc == YAS_ERROR_OK) {
                    /* Raw-pointer store like set-metatable! - barrier
                     * required (docs/GC.md write barrier). */
                    hash.value.hash->metatable = mt.value.hash;
                    yas_gc_write_barrier(hash, mt);
                }
                yas_gc_pop_roots(1);
            }
            yas_gc_pop_roots(1);
            if (rc != YAS_ERROR_OK)
                return rc;
            *out = hash;
            return YAS_ERROR_OK;
        }
        default:
            yas_raise_error("type-error", "marshal: corrupt neutral tree");
            return YAS_ERROR_EXCEPTION;
    }

    n->rebuilt_atom = *out;
    n->rebuilt = 1;
    return YAS_ERROR_OK;
}

int yas_unmarshal(struct YasMarshalNode *n, YasAtom *out)
{
    return unmarshal_walk(n, out);
}

/* ── Free ──────────────────────────────────────────────────────────── */

static int free_set_add(struct FreeSet *set, struct YasMarshalNode *n)
{
    for (size_t i = 0; i < set->count; i++)
        if (set->nodes[i] == n)
            return 0;
    if (set->count == set->capacity) {
        size_t capacity = set->capacity ? set->capacity * 2 : 64;
        struct YasMarshalNode **nodes =
            realloc(set->nodes, capacity * sizeof(*nodes));
        if (!nodes)
            return -1;
        set->nodes = nodes;
        set->capacity = capacity;
    }
    set->nodes[set->count++] = n;
    return 1;
}

static void marshal_free_walk(struct YasMarshalNode *n, struct FreeSet *seen)
{
    if (!n || free_set_add(seen, n) <= 0)
        return;
    switch (n->kind) {
        case YNM_SYMBOL:
        case YNM_KEYWORD:
            free(n->u.name);
            break;
        case YNM_STRING:
            free(n->u.string.bytes);
            break;
        case YNM_BYTEVECTOR:
            free(n->u.bytevector.bytes);
            break;
        case YNM_PAIR:
            marshal_free_walk(n->u.pair.car, seen);
            marshal_free_walk(n->u.pair.cdr, seen);
            break;
        case YNM_VECTOR:
            for (size_t i = 0; i < n->u.vector.n; i++)
                marshal_free_walk(n->u.vector.items[i], seen);
            free(n->u.vector.items);
            break;
        case YNM_HASH:
            for (size_t i = 0; i < n->u.hash.n; i++) {
                marshal_free_walk(n->u.hash.keys[i], seen);
                marshal_free_walk(n->u.hash.vals[i], seen);
            }
            free(n->u.hash.keys);
            free(n->u.hash.vals);
            marshal_free_walk(n->u.hash.metatable, seen);
            break;
        default:
            break;
    }
    free(n);
}

void yas_marshal_free(struct YasMarshalNode *n)
{
    struct FreeSet seen = {0};
    marshal_free_walk(n, &seen);
    free(seen.nodes);
}

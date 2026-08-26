/* PRIVATE header - interpreter sources only, never installed.
 *
 * Neutral value marshaler: deep-copies a yas value into a malloc-owned
 * tree of plain C structs ("marshal phase") and rebuilds an equivalent
 * value in whatever context is bound during the rebuild ("unmarshal
 * phase"). This is the transfer codec behind cross-context value
 * movement (docs/CONCURRENCY.md "Value transfer"): payloads never touch
 * either heap, so no GC object is ever shared between contexts and no
 * cross-heap rooting exists.
 *
 * Contract:
 *   - yas_marshal() reads the value in the CURRENTLY BOUND context; it
 *     performs no GC-heap allocation, so the source graph needs no
 *     extra rooting while it runs (the collector is non-moving).
 *     Returns NULL only after raising a catchable "type-error" (value
 *     outside the transferable type matrix) or "memory" error; a NULL
 *     return leaves no tree behind. Channel handles are added by the
 *     channel primitive alongside its atom type.
 *   - yas_unmarshal() interns symbols/keywords into the CURRENTLY BOUND
 *     context and gc_alloc()s there - the caller must have pushed a
 *     root for `*out` (docs/GC.md root API) before calling, exactly as
 *     for any constructor call whose result is held across further
 *     allocations.
 *   - Trees are context-independent: both contexts involved may be
 *     created/destroyed around a tree freely.
 *   - Structure sharing within one value survives as sharing (a DAG
 *     stays a DAG, verified by eq? identity after unmarshal), and
 *     cyclic structure round-trips: each direction keeps one memo per
 *     object (source-pointer map on marshal, rebuilt_atom slot on
 *     unmarshal), registered before recursing into children.
 */
#ifndef YAS_MARSHAL_H
#define YAS_MARSHAL_H

#include "yas.h"

typedef enum {
    YNM_NIL,
    YNM_INTEGER,
    YNM_FLOAT,
    YNM_CHAR,
    YNM_SYMBOL,
    YNM_KEYWORD,
    YNM_STRING,
    YNM_BYTEVECTOR,
    YNM_VEC2,
    YNM_VEC3,
    YNM_VEC4,
    YNM_MAT4,
    YNM_PAIR,
    YNM_VECTOR,
    YNM_HASH
} YasMarshalKind;

struct YasMarshalNode {
    unsigned char kind; /* YasMarshalKind */
    /* Unmarshal memo: set before children are rebuilt, so re-entering
     * this node mid-rebuild (a cycle) finds the container already
     * registered instead of recursing forever. */
    int rebuilt;
    YasAtom rebuilt_atom;
    union {
        long integer;
        uint64_t float_bits; /* raw bits - bit-exact across contexts */
        int32_t character;
        char *name; /* symbol / keyword (malloc'd copy) */
        struct {
            char *bytes;
            size_t len;
        } string;
        struct {
            unsigned char *bytes;
            size_t len;
        } bytevector;
        float vec[4];
        float mat[16];
        struct {
            struct YasMarshalNode *car, *cdr;
        } pair;
        struct {
            struct YasMarshalNode **items;
            size_t n;
        } vector;
        struct {
            struct YasMarshalNode **keys, **vals;
            size_t n;
            struct YasMarshalNode *metatable; /* NULL when none */
        } hash;
    } u;
};

/* Marshal `v` (bound context's representation) into a neutral tree.
 * Raises + returns NULL on untransferable types or OOM. */
struct YasMarshalNode *yas_marshal(YasAtom v);

/* Rebuild a neutral tree as a live value in the bound context. Caller
 * roots `*out`. Returns YAS_ERROR_OK or raises + returns the error. */
int yas_unmarshal(struct YasMarshalNode *n, YasAtom *out);

/* Free a neutral tree (safe on shared/cyclic trees). */
void yas_marshal_free(struct YasMarshalNode *n);

#endif /* YAS_MARSHAL_H */

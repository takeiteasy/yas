/* PRIVATE header - included by the interpreter sources only, never installed.
 *
 * struct YasContext: everything the interpreter mutates at runtime,
 * per context (ticket #47 milestone 2). Public code sees an opaque
 * handle (yas.h); this header exposes the layout so the interpreter
 * internals can reach their state through the thread-local
 * `yas_ctx` binding.
 *
 * Ownership rules:
 *   - A context owns its whole heap, intern tables, root stack and
 *     registries. Atoms NEVER cross contexts - equality, interning and
 *     identity are all context-local facts by construction.
 *   - ONE thread at a time drives a bound context. The evaluator's
 *     EvalRoot chain and the shadow root stack live in here and are
 *     LIFO windows of the owning thread; concurrent use of one context
 *     is unsupported (bind a second context instead).
 *   - yas_context_create()/yas_context_destroy() take the process-wide
 *     interpreter lock (src/thread.c); everything else runs lock-free
 *     because nothing is shared.
 */
#ifndef YAS_CONTEXT_H
#define YAS_CONTEXT_H

#include "yas.h"

/* Collector phase machine (was file-private in data.c). */
typedef enum { YAS_GC_IDLE, YAS_GC_MARKING, YAS_GC_SWEEPING } YasGCPhase;

/* Intern record (was file-private in data.c): interned name plus the
 * root-frame lookup cache backing yas_env_get/yas_env_set (#45).
 * `name` is a flexible array member; atom.value.symbol points directly
 * at it. Records are immortal within their context. */
struct YasSymbol {
    struct YasEnv *cache_env;
    size_t cache_slot;
    unsigned char shadowed;
    char name[];
};

struct YasInternTable {
    struct YasSymbol **slots;
    size_t capacity;
    size_t count;
};

/* Per-thread-root-stack, now per-context: the shadow root stack of the
 * thread bound to this context (see docs/GC.md). */
struct YasRootStack {
    YasAtom **slots;
    int count;
    int cap;
};

/* Evaluator control-stack root chain (was file-private in eval.c). */
struct EvalRoot {
    YasAtom *expr;
    YasAtom *env;
    YasAtom *stack;
    YasAtom *result;
    struct EvalRoot *prev;
};

/* Macro expansion memo entry (#64, was file-private in data.c). */
struct YasMacroMemoEntry {
    struct YasPair *key; /* NULL = empty, tombstone = removed (cast 1) */
    YasAtom macro;
    YasAtom expansion;
};

struct YasContext {
    /* ── GC core ── */
    struct YasGCHeader *gc_all;
    YasGCPhase gc_phase;
    size_t gc_allocated;
    size_t gc_threshold;      /* 0 = "not yet initialized" */
    struct YasGCHeader **gc_gray;
    int gc_gray_count;
    int gc_gray_cap;
    struct YasGCHeader *gc_sweep_prev;
    size_t gc_debt;
    size_t gc_debt_quantum;   /* 0 = "not yet read from env" */
    int gc_stepping;
    size_t gc_min_threshold;  /* 0 = "not yet read from env" */
    long gc_stress_bytes;     /* -1 = "not yet read from env"; 0 = unset */

#ifndef YAS_NO_GC_POOL
    /* Size-class free lists (#44), per context like everything else. */
    struct YasGCHeader *gc_pool[YAS_GC_OBJ_COUNT];
    int gc_pool_count[YAS_GC_OBJ_COUNT];
    int gc_pool_puts_since_trim[YAS_GC_OBJ_COUNT];
#endif

    /* ── Roots ── */
    struct YasRootStack roots;

    /* ── Intern tables (symbols are context-local identities) ── */
    struct YasInternTable sym_intern;
    struct YasInternTable kw_intern;

    /* ── Macro expansion memo (#64) ── */
    struct YasMacroMemoEntry *macro_memo_slots;
    size_t macro_memo_capacity;
    size_t macro_memo_used;

    /* ── Evaluator ambient state ── */
    struct EvalRoot *eval_roots;
    YasAtom exception;        /* yas_ctx_exception */
    const char *current_file_path; /* yas_current_file_path */
    int error_line, error_col; /* Position of the form being evaluated
                                * (yas_ctx_error_line/col; written only
                                * in YAS_SRCPOS builds) */

    /* ── Modules / features / search paths ── */
    YasAtom library_registry;      /* yas_library_registry */
    YasAtom search_paths;          /* yas_global_search_paths */
    YasAtom features;              /* yas_global_features */

    /* ── I/O sandbox + virtual filesystem providers (io.c) ── */
    int sandbox_mode;         /* YasIOSandboxMode */
    int sandbox_default_allow;
    char *allowed_patterns[64];
    int allowed_count;
    char *denied_patterns[64];
    int denied_count;
    YasIOProvider *providers[16];
    int provider_count;

#ifdef YAS_FFI
    /* ── FFI struct-type registry (ffi.c) ── */
    struct YasFFIStructType *ffi_struct_registry;
#endif
};

/* Thread-local binding: the context the calling thread operates on.
 * Set by yas_context_bind() (and implicitly by create); asserted non-
 * NULL wherever interpreter state is first touched. */
extern YAS_TLS YasContext *yas_ctx;

/* data.c: destroy-path object release (see context.c). */
void yas_gc_context_free_object(struct YasGCHeader *obj);

#endif /* YAS_CONTEXT_H */

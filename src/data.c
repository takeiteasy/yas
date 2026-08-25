#include "yas.h"
#include "context.h"
#include <assert.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ══════════════════════════════════════════════════════════════════
   Unified incremental tricolor mark-sweep GC.

   One intrusive list (`gc_all`), threaded through the `struct
   YasGCHeader` embedded in every heap type, replaces the ten
   near-identical mark/next + sweep-loop copies this file used to have.
   See docs/GC.md for the full design: the tricolor invariant,
   allocation-triggered stepping (gc_alloc() paces steps via a
   byte-proportional debt counter), the allocate-during-sweep hazard and
   how insertion position avoids it, and the write barrier.
   ══════════════════════════════════════════════════════════════════ */

/* Pair/Closure/Macro/Continuation share this allocation - the atom's
 * `pair` pointer points at the embedded `struct YasPair`, not at the
 * allocation base, so recovering the header needs offsetof (see
 * atom_to_header below). `gc` is still first at the C level; the offset
 * is nonzero only because `pair` isn't. */
struct Allocation {
    struct YasGCHeader gc;
    struct YasPair pair;
};

typedef YasGCPhase GCPhase;

#define YAS_GC_INIT_THRESHOLD (256 * 1024) /* bytes */
#define YAS_GC_STEP_LIMIT 512              /* objects per bounded step */

/* ── Per-context state (ticket #47 milestone 2) ─────────────────────
 * Everything below used to be file-scope statics; it now lives in
 * struct YasContext (context.h), reached through the thread-local
 * `yas_ctx` binding. These shorthands keep the function bodies
 * byte-identical to the single-context era - the collector reads and
 * mutates exactly what it always did, just scoped to whichever context
 * the calling thread has bound. */
#define gc_all (yas_ctx->gc_all)
#define gc_phase (yas_ctx->gc_phase)
#define gc_allocated (yas_ctx->gc_allocated)
#define gc_threshold (yas_ctx->gc_threshold)
#define gc_gray (yas_ctx->gc_gray)
#define gc_gray_count (yas_ctx->gc_gray_count)
#define gc_gray_cap (yas_ctx->gc_gray_cap)
#define gc_sweep_prev (yas_ctx->gc_sweep_prev)
#define gc_debt (yas_ctx->gc_debt)
#define gc_debt_quantum (yas_ctx->gc_debt_quantum)
#define gc_stepping (yas_ctx->gc_stepping)
#define gc_min_threshold (yas_ctx->gc_min_threshold)
#define gc_stress_bytes (yas_ctx->gc_stress_bytes)

/* ── Allocation-triggered stepping ───────────────────────────────────
 * gc_alloc() (below) paces yas_gc_step() itself instead of relying on
 * eval_vm()'s loop to call it once per iteration - a single builtin or
 * reader call that allocates heavily used to run to completion with no
 * collector progress at all, so the "incremental" pause bound only held
 * between VM iterations, not within one. See docs/GC.md.
 *
 * gc_debt accumulates bytes allocated (gc_alloc() and gc_account_extra()
 * both feed it - the latter matters because vector/hash/bytevector
 * growth can be the dominant source of allocated bytes without a single
 * gc_alloc() call to hang a step off of) and triggers one bounded step
 * every YAS_GC_DEBT_QUANTUM bytes. Pacing arithmetic: a cons cell
 * (struct Allocation = YasGCHeader + YasPair) is 56 bytes on a typical
 * LP64 build, so one quantum is roughly 73 conses; YAS_GC_STEP_LIMIT
 * (512 objects marked per step) is ~7x that - comfortably enough for
 * marking to outpace allocation. Revisit this comment if either
 * constant changes. */
#define YAS_GC_DEBT_QUANTUM_DEFAULT (4 * 1024) /* bytes allocated per step */

/* GC_STRESS mode (make test-gc-stress / YAS_GC_STRESS=1) is built on
 * top of the *ordinary* debt-paced yas_gc_step() call below, not a
 * separate code path - two designs were tried and rejected first:
 *
 *   - Running a full yas_gc() every Nth allocation always ends back at
 *     IDLE, which colors every subsequent object WHITE regardless of
 *     whether a cycle is conceptually active - defeating allocate-black
 *     (gc_alloc()'s `obj->color = ... YAS_GC_BLACK`) and manufacturing
 *     use-after-frees for values that were never actually unsafe under
 *     the collector's real invariant (an object only needs to survive
 *     to the *next cycle boundary*, not to the next allocation).
 *   - Forcing yas_gc_start() (bypassing the byte threshold) on every
 *     single allocation preserves that invariant, but is O(heap) per
 *     allocation whenever the live heap is small enough for one bounded
 *     step to complete an entire cycle - which, empirically, is most of
 *     the time for a small/medium heap (measured: a ~170KB library
 *     bootstrap didn't finish in 2 minutes). Quadratic, not usable.
 *
 * What actually works: keep the normal, already-correct pacing path
 * (which respects allocate-black because it never forces more than one
 * bounded step, and never resets to IDLE except by naturally finishing
 * a sweep) and just make the *threshold* for starting a new cycle tiny,
 * via gc_min_threshold_get() below, instead of the normal 256KB floor.
 * YAS_GC_STRESS=<n> is a one-var convenience that sets both the min
 * threshold and the debt quantum to `n` bytes unless either is
 * overridden explicitly via YAS_GC_MIN_THRESHOLD/YAS_GC_DEBT_QUANTUM. */

static long gc_stress_get(void)
{
    if (gc_stress_bytes < 0) {
        const char *env = getenv("YAS_GC_STRESS");
        long v = env ? atol(env) : 0;
        gc_stress_bytes = (v > 0) ? v : 0;
    }
    return gc_stress_bytes;
}

static size_t gc_min_threshold_get(void)
{
    if (gc_min_threshold == 0) {
        const char *env = getenv("YAS_GC_MIN_THRESHOLD");
        long v = env ? atol(env) : 0;
        if (v > 0) {
            gc_min_threshold = (size_t)v;
        } else {
            long stress = gc_stress_get();
            gc_min_threshold = (stress > 0) ? (size_t)stress : YAS_GC_INIT_THRESHOLD;
        }
    }
    return gc_min_threshold;
}

static size_t gc_debt_quantum_get(void)
{
    if (gc_debt_quantum == 0) {
        const char *env = getenv("YAS_GC_DEBT_QUANTUM");
        long v = env ? atol(env) : 0;
        if (v > 0) {
            gc_debt_quantum = (size_t)v;
        } else {
            long stress = gc_stress_get();
            gc_debt_quantum = (stress > 0) ? (size_t)stress : YAS_GC_DEBT_QUANTUM_DEFAULT;
        }
    }
    return gc_debt_quantum;
}

/* Shared by gc_alloc() and gc_account_extra(): advance the debt counter
 * by `size` bytes and, if due, run one paced step. gc_stepping guards
 * against re-entering gc_alloc() from inside a step; today nothing a
 * step does (gc_mark_step/gc_sweep_step use plain realloc/free, never
 * gc_alloc()) can trigger that, so this is insurance against a future
 * change, not a live fix. */
static void gc_pace(size_t size)
{
    if (gc_stepping)
        return;

    gc_debt += size;
    if (gc_debt >= gc_debt_quantum_get()) {
        gc_debt = 0;
        gc_stepping = 1;
        yas_gc_step();
        gc_stepping = 0;
    }
}

/* ── Symbol/keyword interning ────────────────────────────────────────
 * Open-addressed tables keyed by string content, probed by
 * yas_make_sym()/yas_make_keyword() further down this file. Plain
 * malloc, not GC-managed - the interned records are immortal for the
 * process lifetime (nothing ever un-interns a symbol), so there is
 * nothing for the collector to trace here and no GC root needed (see
 * gc_mark_roots() below, which used to mark a cons-list version of
 * these tables).
 *
 * Each slot is a `struct YasSymbol *`, not a bare `char *`: `name` is a
 * flexible array member holding the interned text, and `atom.value.symbol`
 * / `atom.value.keyword` point directly at `name` - every existing use of
 * `value.symbol` (string ops, pointer-identity comparison) is unaffected,
 * since `name`'s address is exactly what a plain interned `char *` used to
 * be. `sym_rec()` recovers the enclosing record from that pointer via
 * `offsetof`. The extra fields (`cache_env`/`cache_slot`/`shadowed`) back
 * the root-frame lookup cache in yas_env_get()/yas_env_set() (ticket #45)
 * - `kw_intern` entries never populate them. */
static struct YasSymbol *sym_rec(const char *name)
{
    return (struct YasSymbol *)((char *)name - offsetof(struct YasSymbol, name));
}

/* The tables live in the context (context.h); symbols are context-local
 * identities. */
#define sym_intern (yas_ctx->sym_intern)
#define kw_intern (yas_ctx->kw_intern)

/* Empty-slot sentinel for YasHash's open-addressed index (yas.h) -
 * forward-declared here since yas_make_hash() (needs it to initialize a
 * fresh index) is defined before yas_hash_set() and friends, where the
 * index probing logic itself lives. */
#define YAS_HASH_INDEX_EMPTY ((size_t)-1)

static size_t fnv1a_hash(const char *s)
{
    size_t h = 2166136261u;
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 16777619u;
    }
    return h;
}

/* Insert `rec` (already known absent) into `t` without taking ownership -
 * caller passes a record it already owns (a fresh allocation or one being
 * relocated by intern_grow()). */
static void intern_insert(struct YasInternTable *t, struct YasSymbol *rec)
{
    size_t mask = t->capacity - 1;
    size_t i = fnv1a_hash(rec->name) & mask;
    while (t->slots[i])
        i = (i + 1) & mask;
    t->slots[i] = rec;
    t->count++;
}

static void intern_grow(struct YasInternTable *t)
{
    size_t old_capacity = t->capacity;
    struct YasSymbol **old_slots = t->slots;

    t->capacity = old_capacity ? old_capacity * 2 : 256;
    t->slots = calloc(t->capacity, sizeof(struct YasSymbol *));
    t->count = 0;

    for (size_t i = 0; i < old_capacity; i++)
        if (old_slots[i])
            intern_insert(t, old_slots[i]);
    free(old_slots);
}

/* Probe `t` for `s`; return the interned name pointer on a hit, NULL on a
 * miss. Grows (rehashing every live entry) at 70% load before an
 * insert would cross it - open addressing with no deletes, so no
 * tombstones to worry about. */
static char *intern_find_or_insert(struct YasInternTable *t, const char *s)
{
    if (t->capacity == 0)
        intern_grow(t);

    size_t mask = t->capacity - 1;
    size_t i = fnv1a_hash(s) & mask;
    while (t->slots[i]) {
        if (strcmp(t->slots[i]->name, s) == 0)
            return t->slots[i]->name;
        i = (i + 1) & mask;
    }

    if ((t->count + 1) * 10 >= t->capacity * 7) {
        intern_grow(t);
        /* Table moved/rehashed - re-probe from scratch. */
        mask = t->capacity - 1;
        i = fnv1a_hash(s) & mask;
        while (t->slots[i])
            i = (i + 1) & mask;
    }

    size_t len = strlen(s);
    struct YasSymbol *rec = malloc(sizeof(struct YasSymbol) + len + 1);
    rec->cache_env = NULL;
    rec->cache_slot = 0;
    rec->shadowed = 0;
    memcpy(rec->name, s, len + 1);
    intern_insert(t, rec);
    return rec->name;
}

/* Root-registration API (see yas.h and docs/GC.md). The shadow stack is
 * owned by the bound CONTEXT (context.h) - the LIFO of the one thread
 * driving it. Milestone 1 made these stacks per-thread after a shared
 * global LIFO let interleaved push/pop windows strip other threads'
 * entries under serialized concurrency (live objects unregistered ->
 * swept mid-use; stale slot pointers -> garbage in gc_mark_roots());
 * milestone 2 goes further and scopes them to the context, which also
 * means destroy() reclaims them with everything else. */
static void gc_free_object(struct YasGCHeader *obj, size_t *freed);

/* Destroy-path helper (src/context.c): release one heap object plus its
 * external buffers. Poolable blocks go through the pool like any sweep
 * and the caller drains the buckets afterwards - same code path as a
 * normal collection, no special cases. */
void yas_gc_context_free_object(struct YasGCHeader *obj)
{
    size_t freed = 0;
    gc_free_object(obj, &freed);
}

static void yas_gc_push_root_impl(YasAtom *slot)
{
    struct YasRootStack *st = &yas_ctx->roots;
    if (!slot)
        return;
    if (st->count >= st->cap) {
        int new_cap = st->cap ? st->cap * 2 : 16;
        YasAtom **tmp = realloc(st->slots, (size_t)new_cap * sizeof(YasAtom *));
        if (!tmp)
            return; /* OOM: drop the root; caller must keep it reachable another way */
        st->slots = tmp;
        st->cap = new_cap;
    }
    st->slots[st->count++] = slot;
}

void yas_gc_push_root(YasAtom *slot)
{
    yas_gc_push_root_impl(slot);
}


static void yas_gc_pop_roots_impl(int n)
{
    struct YasRootStack *st = &yas_ctx->roots;
    st->count -= n;
    if (st->count < 0)
        st->count = 0;
}

void yas_gc_pop_roots(int n)
{
    yas_gc_pop_roots_impl(n);
}


static int yas_gc_root_count_impl(void)
{
    return yas_ctx->roots.count;
}

int yas_gc_root_count(void)
{
    int r = yas_gc_root_count_impl();
    return r;
}


static int yas_gc_phase_impl(void)
{
    return (int)gc_phase;
}

int yas_gc_phase(void)
{
    int r = yas_gc_phase_impl();
    return r;
}


static size_t yas_gc_allocated_impl(void)
{
    return gc_allocated;
}

size_t yas_gc_allocated(void)
{
    size_t r = yas_gc_allocated_impl();
    return r;
}


/* eval.c walks its own EvalRoot chain (the live evaluator's C-stack
 * pointers) - kept separate from the shadow stack above since eval_vm's
 * roots point at *its own* locals (expr/env/stack), not at anything a
 * caller of yas_eval_expr can see. Two root sources, one collector. */
void yas_eval_mark_roots(void);

/* ── Header recovery ──────────────────────────────────────────────── */

static struct YasGCHeader *atom_to_header(YasAtom a)
{
    switch (a.type) {
        case YAS_ATOM_TYPE_PAIR:
        case YAS_ATOM_TYPE_CLOSURE:
        case YAS_ATOM_TYPE_MACRO: {
            if (!a.value.pair)
                return NULL;
            struct Allocation *alloc = (struct Allocation *)((char *)a.value.pair - offsetof(struct Allocation, pair));
            return &alloc->gc;
        }
        case YAS_ATOM_TYPE_VECTOR:
            return a.value.vector ? &a.value.vector->gc : NULL;
        case YAS_ATOM_TYPE_HASH:
            return a.value.hash ? &a.value.hash->gc : NULL;
        case YAS_ATOM_TYPE_ENV:
            return a.value.env ? &a.value.env->gc : NULL;
        /* A continuation is a captured control-stack frame chain, retagged - see
         * struct YasFrame (yas.h) and eval.c's CALL/CC capture/restore. */
        case YAS_ATOM_TYPE_FRAME:
        case YAS_ATOM_TYPE_CONTINUATION:
            return a.value.frame ? &a.value.frame->gc : NULL;
        case YAS_ATOM_TYPE_STRING: {
            if (!a.value.string)
                return NULL;
            struct YasStringAlloc *sa =
                (struct YasStringAlloc *)((char *)a.value.string - offsetof(struct YasStringAlloc, data));
            return &sa->gc;
        }
        case YAS_ATOM_TYPE_PORT:
            return a.value.port ? &a.value.port->gc : NULL;
        case YAS_ATOM_TYPE_BYTEVECTOR:
            return a.value.bytevector ? &a.value.bytevector->gc : NULL;
        case YAS_ATOM_TYPE_VEC2:
        case YAS_ATOM_TYPE_VEC3:
        case YAS_ATOM_TYPE_VEC4: {
            if (!a.value.vec)
                return NULL;
            struct YasVecAlloc *va = (struct YasVecAlloc *)((char *)a.value.vec - offsetof(struct YasVecAlloc, v));
            return &va->gc;
        }
        case YAS_ATOM_TYPE_MAT4: {
            if (!a.value.mat4)
                return NULL;
            struct YasMat4Alloc *ma = (struct YasMat4Alloc *)((char *)a.value.mat4 - offsetof(struct YasMat4Alloc, m));
            return &ma->gc;
        }
#ifdef YAS_FFI
        case YAS_ATOM_TYPE_FFI_FUNCTION:
            return a.value.ffi_fn ? &a.value.ffi_fn->gc : NULL;
        case YAS_ATOM_TYPE_FFI_STRUCT:
            return a.value.ffi_struct ? &a.value.ffi_struct->gc : NULL;
#endif
        default:
            return NULL; /* nil, symbol, integer, float, char, keyword, builtin: not heap-tracked */
    }
}

/* ── Macro expansion memo (#64) ──────────────────────────────────────
 * A source macro call site's expansion is a deterministic function of
 * its argument forms and its binding, but eval.c used to re-run the
 * macro's closure and rebuild the expansion from scratch on every single
 * evaluation of that call site - inside a loop body (nearly all yas
 * control flow - `when`, `cond`, `let`, `while`, `dotimes`, ... - is a
 * `defmacro`, see docs/ARCHITECTURE.md), that meant reallocating the
 * same control-flow skeleton every iteration.
 *
 * Open-addressed, keyed on the call form's argument-spine cell identity
 * (`struct YasPair *`) plus the macro binding's identity - see
 * eval_do_return() in eval.c for how those are obtained and why they're
 * stable per call site. Weakly keyed: entries don't keep their key pair
 * alive, and `YAS_GC_FLAG_MACRO_MEMO_KEY` on the key's YasGCHeader lets
 * gc_free_object() drop the entry in O(1) when the key is actually swept
 * (macro_memo_remove() below), rather than this table scanning itself on
 * every free. Tombstones (`MACRO_MEMO_TOMBSTONE`) mark a removed slot so
 * open-addressed probing still finds entries past it. */
#define macro_memo_slots (yas_ctx->macro_memo_slots)
#define macro_memo_capacity (yas_ctx->macro_memo_capacity)
#define macro_memo_used (yas_ctx->macro_memo_used)

#define MACRO_MEMO_TOMBSTONE ((struct YasPair *)1)

static size_t ptr_hash(const void *p)
{
    uintptr_t x = (uintptr_t)p;
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    return (size_t)x;
}

/* Insert `key`/`macro`/`expansion` into `slots` (already known absent),
 * used both for a fresh insert and for relocating a live entry during
 * grow - does not touch macro_memo_used, callers own that. */
static void macro_memo_insert(struct YasMacroMemoEntry *slots, size_t capacity, struct YasPair *key, YasAtom macro,
                               YasAtom expansion)
{
    size_t mask = capacity - 1;
    size_t i = ptr_hash(key) & mask;
    while (slots[i].key && slots[i].key != MACRO_MEMO_TOMBSTONE)
        i = (i + 1) & mask;
    slots[i].key = key;
    slots[i].macro = macro;
    slots[i].expansion = expansion;
}

static void macro_memo_grow(void)
{
    size_t old_capacity = macro_memo_capacity;
    struct YasMacroMemoEntry *old_slots = macro_memo_slots;

    macro_memo_capacity = old_capacity ? old_capacity * 2 : 64;
    macro_memo_slots = calloc(macro_memo_capacity, sizeof(struct YasMacroMemoEntry));
    macro_memo_used = 0;

    for (size_t i = 0; i < old_capacity; i++) {
        if (old_slots[i].key && old_slots[i].key != MACRO_MEMO_TOMBSTONE) {
            macro_memo_insert(macro_memo_slots, macro_memo_capacity, old_slots[i].key, old_slots[i].macro,
                               old_slots[i].expansion);
            macro_memo_used++;
        }
    }
    free(old_slots);
}

static int yas_macro_memo_get_impl(YasAtom args_cell, YasAtom macro, YasAtom *expansion)
{
    if (macro_memo_capacity == 0 || args_cell.type != YAS_ATOM_TYPE_PAIR)
        return 0;

    struct YasPair *key = args_cell.value.pair;
    size_t mask = macro_memo_capacity - 1;
    size_t i = ptr_hash(key) & mask;
    while (macro_memo_slots[i].key) {
        if (macro_memo_slots[i].key == key && macro_memo_slots[i].macro.value.pair == macro.value.pair) {
            *expansion = macro_memo_slots[i].expansion;
            return 1;
        }
        i = (i + 1) & mask;
    }
    return 0;
}

int yas_macro_memo_get(YasAtom args_cell, YasAtom macro, YasAtom *expansion)
{
    int r = yas_macro_memo_get_impl(args_cell, macro, expansion);
    return r;
}


static void yas_macro_memo_put_impl(YasAtom args_cell, YasAtom macro, YasAtom expansion)
{
    if (args_cell.type != YAS_ATOM_TYPE_PAIR)
        return;

    /* Grow (rehashing every live entry, dropping tombstones) at 70% load
     * before an insert would cross it - same policy as the symbol intern
     * table above. */
    if (macro_memo_capacity == 0 || (macro_memo_used + 1) * 10 >= macro_memo_capacity * 7)
        macro_memo_grow();

    struct YasPair *key = args_cell.value.pair;
    size_t mask = macro_memo_capacity - 1;
    size_t i = ptr_hash(key) & mask;
    long first_tombstone = -1;
    while (macro_memo_slots[i].key) {
        if (macro_memo_slots[i].key == key && macro_memo_slots[i].macro.value.pair == macro.value.pair) {
            /* Same call site, same macro binding - refresh in place
             * (e.g. re-inserting after a lookup miss raced a grow). */
            macro_memo_slots[i].expansion = expansion;
            goto marked;
        }
        if (macro_memo_slots[i].key == MACRO_MEMO_TOMBSTONE && first_tombstone < 0)
            first_tombstone = (long)i;
        i = (i + 1) & mask;
    }
    i = first_tombstone >= 0 ? (size_t)first_tombstone : i;
    macro_memo_slots[i].key = key;
    macro_memo_slots[i].macro = macro;
    macro_memo_slots[i].expansion = expansion;
    macro_memo_used++;

    {
        struct YasGCHeader *kh = atom_to_header(args_cell);
        if (kh)
            kh->flags |= YAS_GC_FLAG_MACRO_MEMO_KEY;
    }

marked:
    /* Mid-cycle insert: `expansion` (and, in principle, `macro`, though
     * that's already reachable via the live binding) may be pre-existing
     * WHITE structure this cycle's marking hasn't reached yet, not
     * necessarily freshly consed BLACK-on-alloc structure - grey it now
     * so the memo doesn't hold the only live reference to something
     * still eligible for this cycle's sweep. gc_mark_roots() covers every
     * live entry on the next full pass (including the atomic pre-sweep
     * re-mark, see yas_gc_step()'s MARKING case) regardless; this closes
     * the gap between "inserted" and "next gc_mark_roots() call" for a
     * cycle already past its own root-marking pass. No-op (besides the
     * WHITE check inside yas_gc_mark()) outside an active cycle. */
    if (gc_phase != YAS_GC_IDLE) {
        yas_gc_mark(macro);
        yas_gc_mark(expansion);
    }
}

void yas_macro_memo_put(YasAtom args_cell, YasAtom macro, YasAtom expansion)
{
    yas_macro_memo_put_impl(args_cell, macro, expansion);
}


/* Called from gc_free_object()'s PAIR case when `obj`'s
 * YAS_GC_FLAG_MACRO_MEMO_KEY bit is set - `key` is about to be freed (or
 * recycled into the free-list pool), so drop its memo entry now rather
 * than leaving a dangling pointer that a future, unrelated pair could
 * collide with under the same hashed slot. Tombstones instead of
 * compacting, matching the open-addressing contract every other prober
 * in this table relies on. */
static void macro_memo_remove(struct YasPair *key)
{
    if (macro_memo_capacity == 0)
        return;

    size_t mask = macro_memo_capacity - 1;
    size_t i = ptr_hash(key) & mask;
    while (macro_memo_slots[i].key) {
        if (macro_memo_slots[i].key == key) {
            macro_memo_slots[i].key = MACRO_MEMO_TOMBSTONE;
            macro_memo_slots[i].macro = yas_nil;
            macro_memo_slots[i].expansion = yas_nil;
            return;
        }
        i = (i + 1) & mask;
    }
}

/* Marks every live entry's `macro` and `expansion` - strong values, weak
 * key (see the struct comment above): an entry's key is never marked
 * here, so a key with no other reference dies normally and takes the
 * entry with it via macro_memo_remove(). */
static void macro_memo_mark_roots(void)
{
    for (size_t i = 0; i < macro_memo_capacity; i++) {
        if (macro_memo_slots[i].key && macro_memo_slots[i].key != MACRO_MEMO_TOMBSTONE) {
            yas_gc_mark(macro_memo_slots[i].macro);
            yas_gc_mark(macro_memo_slots[i].expansion);
        }
    }
}

/* ── Free-list pool (ticket #44) ─────────────────────────────────────
 * gc_alloc() calls calloc()/gc_free_object() calls free() per object -
 * general-purpose malloc bookkeeping/locking overhead that's wasted on
 * fixed-size, high-frequency allocations like cons cells and frames. A
 * size-class free list, bucketed by obj_type (most types have a single
 * fixed sizeof), removes most of that per-call cost: gc_sweep_step()
 * pushes a freed WHITE object's header back onto its bucket instead of
 * free()ing it, and gc_alloc() pops from the bucket before falling back
 * to calloc(). Exact-size reuse means no rounding and no change to
 * backing_size accounting - a recycled block is the same size it always
 * was.
 *
 * Excluded: STRING (struct YasStringAlloc has a trailing flexible array
 * member, so its allocation size varies with string length - a pooled
 * block sized for one string is generally wrong for the next) and the
 * two FFI types (rare enough not to be worth a bucket).
 *
 * YAS_NO_GC_POOL (set by `make YAS_ASAN=1` - see the Makefile) disables
 * this file-wide: a pooled block is freed and reused without ever
 * calling free() on the same address twice from ASan's point of view,
 * which suppresses genuine use-after-free detection on every recycled
 * object - exactly the audit this build exists to run. */
#ifndef YAS_NO_GC_POOL
#define YAS_GC_POOL_CAP 1024                   /* per-bucket cap; sweep bare free()s past it */
#define YAS_GC_POOL_TRIM_HIGH_WATER 128        /* only trim a bucket comfortably above normal churn */
#define YAS_GC_POOL_TRIM_MAX YAS_GC_STEP_LIMIT /* bound trim work to one step's worth, per cycle end */

#define gc_pool (yas_ctx->gc_pool)
#define gc_pool_count (yas_ctx->gc_pool_count)
/* Blocks pushed onto each bucket since the last gc_pool_trim() call - see
 * gc_pool_trim()'s comment for how this drives actual draining instead of
 * just capping at the high-water mark. */
#define gc_pool_puts_since_trim (yas_ctx->gc_pool_puts_since_trim)

static int gc_obj_type_poolable(YasGCObjType type)
{
    switch (type) {
        case YAS_GC_OBJ_PAIR:
        case YAS_GC_OBJ_VECTOR:
        case YAS_GC_OBJ_HASH:
        case YAS_GC_OBJ_PORT:
        case YAS_GC_OBJ_BYTEVECTOR:
        case YAS_GC_OBJ_VEC:
        case YAS_GC_OBJ_MAT4:
        case YAS_GC_OBJ_ENV:
        case YAS_GC_OBJ_FRAME:
            return 1;
        default:
            return 0; /* STRING (variable size), the FFI types, COUNT */
    }
}

/* Cycle-end trim: the cap alone only bounds a single allocation burst, not
 * long-term idle memory - a bucket that merely stays *capped* at the
 * high-water mark forever (rather than draining) would not actually solve
 * the allocate-then-stop case the ticket asks for, since gc_pool_count[]
 * is a LIFO stack (gc_free_object() pushes the head, gc_alloc() pops the
 * head) and repeatedly trimming from that same head only ever removes the
 * *newest* arrivals, never touching the oldest ones sitting at the tail -
 * a bucket above the high-water mark would settle at exactly
 * YAS_GC_POOL_TRIM_HIGH_WATER blocks and stay there indefinitely,
 * draining nothing further.
 *
 * gc_pool_puts_since_trim[] fixes that: a bucket that received zero pushes
 * since the *previous* trim call means nothing of that type has been
 * freed since - i.e. that type has gone idle - so this drains it
 * completely instead of only capping it. A bucket still under active
 * churn (puts > 0) is left at the cap, since freeing it now would just be
 * immediately reallocated. Bounded to YAS_GC_POOL_TRIM_MAX frees total so
 * this can't turn a step-limited sweep into a free() storm -
 * gc_sweep_step()'s cycle-end branch that calls this runs on essentially
 * every allocation under YAS_GC_STRESS=1's shrunk threshold. Only ever
 * bare free()s a block: gc_free_object() already released its external
 * buffers (v->data, h->keys, ...) before pushing it onto the pool, so
 * calling gc_free_object() again here would double-free them. */
static void gc_pool_trim(void)
{
    int budget = YAS_GC_POOL_TRIM_MAX;
    for (int t = 0; t < YAS_GC_OBJ_COUNT && budget > 0; t++) {
        int target = gc_pool_puts_since_trim[t] > 0 ? YAS_GC_POOL_TRIM_HIGH_WATER : 0;
        while (gc_pool_count[t] > target && budget > 0) {
            struct YasGCHeader *obj = gc_pool[t];
            gc_pool[t] = obj->next;
            gc_pool_count[t]--;
            free(obj);
            budget--;
        }
        gc_pool_puts_since_trim[t] = 0;
    }
}
#endif

/* ── Allocation ───────────────────────────────────────────────────── */

static void *gc_alloc(size_t size, YasGCObjType type)
{
    /* Pace *before* allocating, not after: `obj` doesn't exist yet at
     * this point, so a step - even a full YAS_GC_STRESS collection -
     * cannot possibly free the object this very call is about to create.
     * (Pacing after linking+coloring was tried and reverted - it does
     * NOT help: under YAS_GC_STRESS, the collector always ends a cycle
     * back at IDLE, so a freshly-linked object is colored WHITE just
     * like any other IDLE-time allocation, and a pace call immediately
     * afterward can sweep *this* object before its car/cdr/fields are
     * ever populated - an immediate use-after-free on the object under
     * construction, worse than the hazard this ordering is meant to
     * avoid.) The hazard this leaves is exactly the one docs/GC.md
     * documents: *other*, already-returned, still-unrooted objects a
     * caller is holding across this call. That's the intended audit
     * target, not a bug in this function. */
    gc_pace(size);

    struct YasGCHeader *obj = NULL;
#ifndef YAS_NO_GC_POOL
    /* Pop a recycled block before calloc()ing a fresh one. gc_pace()
     * above already ran, so a reused block counts toward the debt
     * counter the same as a fresh allocation would - it's the same
     * live-byte pressure either way. memset() first, before any header
     * field below is written, since the tempting reordering (init header,
     * then memset the payload) would zero the header this function is
     * about to populate; recycling loses calloc()'s zero-init, and
     * several constructors rely on their fields starting NIL/0 (see
     * yas_make_frame()'s comment on this same point). */
    if (gc_obj_type_poolable(type) && gc_pool[type]) {
        obj = gc_pool[type];
        gc_pool[type] = obj->next;
        gc_pool_count[type]--;
        memset(obj, 0, size);
    }
#endif
    if (!obj) {
        obj = calloc(1, size);
        if (!obj)
            return NULL;
    }
    obj->obj_type = (unsigned char)type;
    /* New objects during an active cycle are BLACK so a caller who
     * hasn't rooted them yet doesn't lose them to the current sweep -
     * constructors write-barrier every reference they store into a
     * fresh object for exactly this reason (see yas_cons() etc. below).
     * Always inserting at the head is safe regardless of sweep progress:
     * whether or not this cycle's sweep cursor ever reaches this object,
     * yas_gc_start() bulk-clears every object in gc_all back to WHITE at
     * the *next* cycle's start (see there), so a BLACK object never gets
     * "stuck" unscannable the way it would if survivors' colors were
     * reset incrementally during sweep instead. */
    obj->color = (gc_phase != YAS_GC_IDLE) ? YAS_GC_BLACK : YAS_GC_WHITE;
    obj->next = gc_all;
    gc_all = obj;
    obj->backing_size = size;
    gc_allocated += size;
    return obj;
}

/* Account an external buffer (vector/hash/bytevector data, etc.) owned by
 * `obj` but allocated separately from the gc_alloc() call that created it
 * - so both the live-bytes counter and the amount returned to it on
 * sweep (gc_free_object()'s `*freed`) stay accurate. Growth via realloc
 * (e.g. yas_hash_set()'s capacity doubling) is intentionally not re-
 * accounted here: gc_allocated is a pacing heuristic for when to start a
 * cycle, not an exact accounting ledger. */
static void gc_account_extra(struct YasGCHeader *obj, size_t delta)
{
    obj->backing_size += delta;
    gc_allocated += delta;
    /* Feeds the same pacing counter gc_alloc() does - vector/hash/
     * bytevector growth can be the dominant source of allocated bytes
     * without a single gc_alloc() call to hang a step off of, so
     * without this a program churning large buffers would starve
     * marking mid-cycle. */
    gc_pace(delta);
}

/* ── Write barrier ────────────────────────────────────────────────── */

static void gc_write_barrier_headers(struct YasGCHeader *container, struct YasGCHeader *child)
{
    if (!container || !child)
        return;
    if (container->color == YAS_GC_BLACK && child->color == YAS_GC_WHITE) {
        container->color = YAS_GC_GRAY;
        if (gc_gray_count >= gc_gray_cap) {
            int new_cap = gc_gray_cap ? gc_gray_cap * 2 : 64;
            struct YasGCHeader **tmp = realloc(gc_gray, (size_t)new_cap * sizeof(struct YasGCHeader *));
            if (!tmp)
                return; /* OOM: drop the re-gray; container may be swept early */
            gc_gray = tmp;
            gc_gray_cap = new_cap;
        }
        gc_gray[gc_gray_count++] = container;
    }
}

/* container/child are the atoms holding the reference being stored, e.g.
 * yas_gc_write_barrier(pair_atom, new_cdr_atom). Exposed (not static) for
 * builtin_hash.c's set-metatable!, which stores a raw `struct YasHash *`
 * rather than going through yas_set_car/yas_set_cdr. */
static void yas_gc_write_barrier_impl(YasAtom container, YasAtom child)
{
    gc_write_barrier_headers(atom_to_header(container), atom_to_header(child));
}

void yas_gc_write_barrier(YasAtom container, YasAtom child)
{
    yas_gc_write_barrier_impl(container, child);
}


static void yas_set_car_impl(YasAtom p, YasAtom v)
{
    yas_car(p) = v;
    yas_gc_write_barrier(p, v);
}

void yas_set_car(YasAtom p, YasAtom v)
{
    yas_set_car_impl(p, v);
}


static void yas_set_cdr_impl(YasAtom p, YasAtom v)
{
    yas_cdr(p) = v;
    yas_gc_write_barrier(p, v);
}

void yas_set_cdr(YasAtom p, YasAtom v)
{
    yas_set_cdr_impl(p, v);
}


/* ── Marking ──────────────────────────────────────────────────────── */

static void gc_push_gray(struct YasGCHeader *obj)
{
    if (gc_gray_count >= gc_gray_cap) {
        int new_cap = gc_gray_cap ? gc_gray_cap * 2 : 64;
        struct YasGCHeader **tmp = realloc(gc_gray, (size_t)new_cap * sizeof(struct YasGCHeader *));
        if (!tmp)
            return; /* OOM: drop the push; object may be swept, but better than crash */
        gc_gray = tmp;
        gc_gray_cap = new_cap;
    }
    gc_gray[gc_gray_count++] = obj;
}

/* Grays and enqueues `root`. NOT a full trace - see the contract note on
 * yas_gc_mark() in yas.h. */
static void yas_gc_mark_impl(YasAtom root)
{
    struct YasGCHeader *h = atom_to_header(root);
    if (!h || h->color != YAS_GC_WHITE)
        return;
    h->color = YAS_GC_GRAY;
    gc_push_gray(h);
}

void yas_gc_mark(YasAtom root)
{
    yas_gc_mark_impl(root);
}


static void gc_mark_roots(void)
{
    yas_gc_mark(yas_library_registry);
    yas_gc_mark(yas_global_search_paths);
    yas_gc_mark(yas_global_features);
    yas_gc_mark(yas_ctx_exception);

    /* The bound context's shadow root stack. Collection only ever runs
     * on the context's owning thread, so it cannot mutate mid-walk. */
    {
        struct YasRootStack *st = &yas_ctx->roots;
        for (int i = 0; i < st->count; i++)
            yas_gc_mark_impl(*st->slots[i]);
    }

    yas_eval_mark_roots();
    macro_memo_mark_roots();
}

static void gc_scan_object(struct YasGCHeader *obj)
{
    switch ((YasGCObjType)obj->obj_type) {
        case YAS_GC_OBJ_PAIR: {
            struct Allocation *a = (struct Allocation *)obj;
            YasAtom p;
            p.type = YAS_ATOM_TYPE_PAIR;
            p.value.pair = &a->pair;
            yas_gc_mark(yas_car(p));
            yas_gc_mark(yas_cdr(p));
            break;
        }
        case YAS_GC_OBJ_VECTOR: {
            struct YasVector *v = (struct YasVector *)obj;
            for (size_t i = 0; i < v->size; i++)
                yas_gc_mark(v->data[i]);
            break;
        }
        case YAS_GC_OBJ_HASH: {
            struct YasHash *h = (struct YasHash *)obj;
            if (h->metatable) {
                YasAtom mt;
                mt.type = YAS_ATOM_TYPE_HASH;
                mt.value.hash = h->metatable;
                yas_gc_mark(mt);
            }
            for (size_t i = 0; i < h->size; i++) {
                yas_gc_mark(h->keys[i]);
                yas_gc_mark(h->values[i]);
            }
            break;
        }
        case YAS_GC_OBJ_ENV: {
            struct YasEnv *e = (struct YasEnv *)obj;
            yas_gc_mark(e->parent);
            for (size_t i = 0; i < e->capacity; i++) {
                if (yas_nilp(e->syms[i]))
                    continue;
                yas_gc_mark(e->syms[i]);
                yas_gc_mark(e->vals[i]);
            }
            break;
        }
        case YAS_GC_OBJ_FRAME: {
            struct YasFrame *f = (struct YasFrame *)obj;
            yas_gc_mark(f->parent);
            yas_gc_mark(f->env);
            yas_gc_mark(f->op);
            yas_gc_mark(f->tail);
            yas_gc_mark(f->args);
            yas_gc_mark(f->body);
            break;
        }
        case YAS_GC_OBJ_STRING:
        case YAS_GC_OBJ_PORT:
        case YAS_GC_OBJ_BYTEVECTOR:
        case YAS_GC_OBJ_VEC:
        case YAS_GC_OBJ_MAT4:
#ifdef YAS_FFI
        case YAS_GC_OBJ_FFI_FUNCTION:
        case YAS_GC_OBJ_FFI_STRUCT:
#endif
            /* Leaves: no YasAtom children. */
            break;
        case YAS_GC_OBJ_COUNT:
            /* Sentinel, not a real object type - never stored in obj_type.
             * Named explicitly (rather than a `default:`) so -Wswitch keeps
             * catching a genuinely unhandled type added above this line. */
            break;
    }
    obj->color = YAS_GC_BLACK;
}

static void gc_mark_step(int limit)
{
    int work = 0;
    while (gc_gray_count > 0 && work < limit) {
        struct YasGCHeader *obj = gc_gray[--gc_gray_count];
        gc_scan_object(obj);
        work++;
    }
}

/* ── Freeing ──────────────────────────────────────────────────────── */

static void gc_free_object(struct YasGCHeader *obj, size_t *freed)
{
    switch ((YasGCObjType)obj->obj_type) {
        case YAS_GC_OBJ_PAIR:
            /* car/cdr are YasAtoms, not owned pointers - nothing extra to free. */
            if (obj->flags & YAS_GC_FLAG_MACRO_MEMO_KEY) {
                /* This pair is a live key in the macro expansion memo
                 * (#64) - drop its entry now, before the free-list pool
                 * (or a bare free()) can hand this same address out
                 * again under a fresh identity. gc_alloc()'s memset()
                 * on reuse already clears `flags`, so a recycled pair
                 * never re-triggers this; the explicit clear here is
                 * just so a pooled-but-not-yet-reused header doesn't
                 * keep stale bits set. */
                struct Allocation *a = (struct Allocation *)obj;
                macro_memo_remove(&a->pair);
                obj->flags &= (unsigned char)~YAS_GC_FLAG_MACRO_MEMO_KEY;
            }
            break;
        case YAS_GC_OBJ_VECTOR: {
            struct YasVector *v = (struct YasVector *)obj;
            free(v->data);
            v->data = NULL; /* pool invariant: no dangling buffer pointer while pooled */
            break;
        }
        case YAS_GC_OBJ_HASH: {
            struct YasHash *h = (struct YasHash *)obj;
            free(h->keys);
            free(h->values);
            free(h->index);
            h->keys = NULL;
            h->values = NULL;
            h->index = NULL;
            break;
        }
        case YAS_GC_OBJ_ENV: {
            struct YasEnv *e = (struct YasEnv *)obj;
            free(e->syms);
            free(e->vals);
            e->syms = NULL;
            e->vals = NULL;
            break;
        }
        case YAS_GC_OBJ_FRAME:
            /* All six fields are YasAtoms, not owned pointers - nothing extra
             * to free (mirrors YAS_GC_OBJ_PAIR above). */
            break;
        case YAS_GC_OBJ_STRING:
            /* Single allocation (header + trailing data) - nothing extra. */
            break;
        case YAS_GC_OBJ_PORT: {
            struct YasPort *p = (struct YasPort *)obj;
            if (p->is_open) {
                YasAtom tmp;
                tmp.type = YAS_ATOM_TYPE_PORT;
                tmp.value.port = p;
                yas_port_close(tmp);
            }
            free(p->path);
            p->path = NULL;
            break;
        }
        case YAS_GC_OBJ_BYTEVECTOR: {
            struct YasBytevector *bv = (struct YasBytevector *)obj;
            free(bv->data);
            bv->data = NULL;
            break;
        }
        case YAS_GC_OBJ_VEC:
        case YAS_GC_OBJ_MAT4:
            /* Single allocation - nothing extra. */
            break;
#ifdef YAS_FFI
        case YAS_GC_OBJ_FFI_FUNCTION: {
            struct YasFFIFunction *fn = (struct YasFFIFunction *)obj;
            if (fn->cif)
                free(fn->cif);
            if (fn->arg_types)
                free(fn->arg_types);
            if (fn->ffi_arg_types)
                free(fn->ffi_arg_types);
            break;
        }
        case YAS_GC_OBJ_FFI_STRUCT: {
            YasFFIStructInstance *inst = (YasFFIStructInstance *)obj;
            free(inst->data);
            break;
        }
#endif
        case YAS_GC_OBJ_COUNT:
            /* Sentinel, not a real object type - never stored in obj_type.
             * See gc_scan_object()'s matching arm. */
            break;
    }
    *freed += obj->backing_size;
#ifndef YAS_NO_GC_POOL
    if (gc_obj_type_poolable((YasGCObjType)obj->obj_type) && gc_pool_count[obj->obj_type] < YAS_GC_POOL_CAP) {
        obj->next = gc_pool[obj->obj_type];
        gc_pool[obj->obj_type] = obj;
        gc_pool_count[obj->obj_type]++;
        gc_pool_puts_since_trim[obj->obj_type]++;
        return;
    }
#endif
    free(obj);
}

#ifndef NDEBUG
/* Cycle-end invariant: the gray worklist must be empty once a cycle
 * finishes (nothing left un-scanned). Survivors sit at BLACK, not WHITE,
 * between cycles - see the comment on gc_sweep_step()'s keep branch for
 * why sweep no longer resets them, and yas_gc_start() for where they get
 * cleared back to WHITE at the next cycle's start. */
static void gc_check_idle_invariant(void)
{
    assert(gc_gray_count == 0);
}
#endif

static void gc_sweep_step(int limit)
{
    int work = 0;
    struct YasGCHeader **prev = gc_sweep_prev ? &gc_sweep_prev->next : &gc_all;

    while (*prev && work < limit) {
        struct YasGCHeader *obj = *prev;
        if (obj->color == YAS_GC_WHITE) {
            *prev = obj->next;
            size_t freed = 0;
            gc_free_object(obj, &freed);
            if (gc_allocated >= freed)
                gc_allocated -= freed;
            else
                gc_allocated = 0;
        } else {
            /* Kept - but deliberately NOT reset to WHITE here. If we
             * did, and a write barrier later (still mid-sweep) rescued
             * a reference to this already-passed object, marking it
             * would recolor it BLACK with nothing left to ever reset
             * it back - it would carry BLACK into the next cycle and
             * never be scanned again (the very
             * "allocate-during-sweep"-shaped hazard docs/GC.md warns
             * about, just via rescue instead of fresh allocation).
             * Leaving survivors BLACK until yas_gc_start()'s bulk
             * clear sidesteps that: a WHITE object encountered anytime
             * before the next cycle's clear is unambiguously "unseen
             * this cycle", full stop, regardless of whether the sweep
             * cursor has passed its list position. */
            gc_sweep_prev = obj;
            prev = &obj->next;
        }
        work++;
    }

    if (!*prev) {
        gc_phase = YAS_GC_IDLE;
        gc_sweep_prev = NULL;
        gc_threshold = gc_allocated * 2;
        if (gc_threshold < gc_min_threshold_get())
            gc_threshold = gc_min_threshold_get();
#ifndef NDEBUG
        gc_check_idle_invariant();
#endif
#ifndef YAS_NO_GC_POOL
        /* Drain surplus pooled blocks now that a cycle has actually
         * finished, not on every step - see gc_pool_trim()'s comment for
         * why this is bounded rather than a full drain. */
        gc_pool_trim();
#endif
    }
}

/* ── Public entry points ──────────────────────────────────────────── */

static void yas_gc_start_impl(void)
{
    if (gc_phase != YAS_GC_IDLE)
        return;
    /* Bulk-clear every survivor back to WHITE before marking. Cheap
     * (a byte write per live object, no allocation/freeing) and avoids
     * the alternating-white-color bookkeeping production collectors use
     * to skip this pass - a deliberate simplicity/perf trade-off for
     * this GC. See gc_sweep_step()'s keep branch for why survivors are
     * BLACK (not WHITE) going into this call. */
    for (struct YasGCHeader *h = gc_all; h; h = h->next)
        h->color = YAS_GC_WHITE;

    gc_phase = YAS_GC_MARKING;
    gc_mark_roots();
}

void yas_gc_start(void)
{
    yas_gc_start_impl();
}


/* One bounded slice of whichever phase is active; a no-op (besides the
 * threshold check) when IDLE and under budget. Paced by gc_alloc()/
 * gc_account_extra() via gc_pace() above - every YAS_GC_DEBT_QUANTUM
 * bytes allocated gets one call here, so allocation itself is what
 * advances the collector (see docs/GC.md, "Allocation-triggered
 * stepping"). Also callable directly - (gc-start) exposes yas_gc_start()
 * and (gc)/yas_gc() drain to completion via the loops below, both of
 * which can still be invoked from arbitrary builtin context. */
static void yas_gc_step_impl(void)
{
    switch (gc_phase) {
        case YAS_GC_IDLE:
            if (gc_threshold == 0)
                gc_threshold = gc_min_threshold_get();
            if (gc_allocated <= gc_threshold)
                return;
            yas_gc_start();
            gc_mark_step(YAS_GC_STEP_LIMIT);
            break;
        case YAS_GC_MARKING:
            gc_mark_step(YAS_GC_STEP_LIMIT);
            if (gc_gray_count == 0) {
                /* Atomic phase: the C-stack roots (eval_roots' expr/env/
                 * stack pointers, the shadow stack) may have been
                 * reassigned many times across the safe points this cycle
                 * spanned, so re-mark once more and fully drain before
                 * sweeping - this never persists across a yas_gc_step()
                 * call, so no allocation can race it. */
                gc_mark_roots();
                while (gc_gray_count > 0)
                    gc_mark_step(1 << 20);
                gc_phase = YAS_GC_SWEEPING;
                gc_sweep_prev = NULL;
                gc_sweep_step(YAS_GC_STEP_LIMIT);
            }
            break;
        case YAS_GC_SWEEPING:
            /* The write barrier can still fire during SWEEPING: live code
             * keeps running between steps, and can store a reference to an
             * object this cycle's marking never reached (still WHITE) into
             * an already-BLACK container - e.g. resurrecting a value that
             * was unreachable at the atomic snapshot via a fresh binding.
             * MARKING is over, so nothing else will ever pop and scan that
             * re-grayed container; drain it here first so gc_scan_object()
             * marks the rescued object non-white *before* the sweep cursor
             * can reach and free it. Without this, the assertion in
             * gc_check_idle_invariant() (gc_gray_count == 0 at cycle end)
             * fires - or worse, without the assertion, the object is
             * silently freed out from under a live container. */
            if (gc_gray_count > 0)
                gc_mark_step(1 << 20);
            gc_sweep_step(YAS_GC_STEP_LIMIT);
            break;
    }
}

void yas_gc_step(void)
{
    yas_gc_step_impl();
}


static void yas_gc_impl(void)
{
    if (gc_phase == YAS_GC_IDLE) {
        if (gc_allocated == 0)
            return;
        yas_gc_start();
    }
    while (gc_phase == YAS_GC_MARKING)
        yas_gc_step();
    while (gc_phase == YAS_GC_SWEEPING)
        yas_gc_step();
}

void yas_gc(void)
{
    yas_gc_impl();
}


/* ── Constructors ─────────────────────────────────────────────────── */

static YasAtom yas_cons_impl(YasAtom car_val, YasAtom cdr_val)
{
    struct Allocation *a = gc_alloc(sizeof(struct Allocation), YAS_GC_OBJ_PAIR);
    YasAtom p;

    p.type = YAS_ATOM_TYPE_PAIR;
    p.value.pair = &a->pair;

    yas_set_car(p, car_val);
    yas_set_cdr(p, cdr_val);

    return p;
}

YasAtom yas_cons(YasAtom car_val, YasAtom cdr_val)
{
    YasAtom r = yas_cons_impl(car_val, cdr_val);
    return r;
}


/* eval.c's control-stack frame constructor (ticket #40) - one gc_alloc()
 * instead of make_frame()'s former six yas_cons() calls. `op`/`args`/`body`
 * start NIL: gc_alloc() calloc()s (or, once pooled, memset()s - see
 * gc_alloc()) the backing storage, and YAS_ATOM_TYPE_NIL is 0, so a
 * zero-initialized YasAtom is already a valid nil atom - no explicit init
 * needed. No yas_gc_push_root() here: unlike yas_make_vector()/yas_env_create()
 * this constructor never malloc()s an external buffer or otherwise calls
 * anything that can itself trigger a gc_alloc() between this allocation and
 * the barriered stores below, so `f` can't be swept out from under us. */
static YasAtom yas_make_frame_impl(YasAtom parent, YasAtom env, YasAtom tail)
{
    struct YasFrame *f = gc_alloc(sizeof(struct YasFrame), YAS_GC_OBJ_FRAME);
    YasAtom a;

    a.type = YAS_ATOM_TYPE_FRAME;
    a.value.frame = f;

    f->parent = parent;
    yas_gc_write_barrier(a, parent);
    f->env = env;
    yas_gc_write_barrier(a, env);
    f->tail = tail;
    yas_gc_write_barrier(a, tail);

    return a;
}

YasAtom yas_make_frame(YasAtom parent, YasAtom env, YasAtom tail)
{
    YasAtom r = yas_make_frame_impl(parent, env, tail);
    return r;
}


YasAtom yas_make_int(long x)
{
    YasAtom a;
    a.type = YAS_ATOM_TYPE_INTEGER;
    a.value.integer = x;
    return a;
}

static YasAtom yas_make_sym_impl(const char *s)
{
    YasAtom a;
    a.type = YAS_ATOM_TYPE_SYMBOL;
    a.value.symbol = intern_find_or_insert(&sym_intern, s);
    return a;
}

YasAtom yas_make_sym(const char *s)
{
    YasAtom r = yas_make_sym_impl(s);
    return r;
}


static YasAtom yas_make_keyword_impl(const char *s)
{
    YasAtom a;
    a.type = YAS_ATOM_TYPE_KEYWORD;
    a.value.keyword = intern_find_or_insert(&kw_intern, s);
    return a;
}

YasAtom yas_make_keyword(const char *s)
{
    YasAtom r = yas_make_keyword_impl(s);
    return r;
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

static YasAtom yas_make_string_impl(const char *s)
{
    YasAtom a;
    size_t len = strlen(s);
    struct YasStringAlloc *sa = gc_alloc(sizeof(struct YasStringAlloc) + len + 1, YAS_GC_OBJ_STRING);
    memcpy(sa->data, s, len + 1);
    a.type = YAS_ATOM_TYPE_STRING;
    a.value.string = sa->data;
    return a;
}

YasAtom yas_make_string(const char *s)
{
    YasAtom r = yas_make_string_impl(s);
    return r;
}


static YasAtom yas_make_string_n_impl(const char *s, size_t len)
{
    YasAtom a;
    struct YasStringAlloc *sa = gc_alloc(sizeof(struct YasStringAlloc) + len + 1, YAS_GC_OBJ_STRING);
    if (s)
        memcpy(sa->data, s, len);
    sa->data[len] = '\0';
    a.type = YAS_ATOM_TYPE_STRING;
    a.value.string = sa->data;
    return a;
}

YasAtom yas_make_string_n(const char *s, size_t len)
{
    YasAtom r = yas_make_string_n_impl(s, len);
    return r;
}


static YasAtom yas_make_vector_impl(size_t size, YasAtom fill)
{
    YasAtom a;
    struct YasVector *v = gc_alloc(sizeof(struct YasVector), YAS_GC_OBJ_VECTOR);
    a.type = YAS_ATOM_TYPE_VECTOR;
    a.value.vector = v;
    /* `v` is fully linked into gc_all and colored by gc_alloc() above,
     * but nothing marks it reachable yet - it's not returned to our
     * caller until this function returns. Ordinarily allocate-black
     * would protect it for the rest of an already-active cycle, but if
     * *this* gc_alloc() is what happened to run while IDLE (v colored
     * WHITE) and gc_account_extra() below is what starts the next cycle
     * (e.g. the very first collection of the process, when the live
     * heap is small enough for a single bounded step to complete an
     * entire mark+sweep), v was never inside an active cycle at all -
     * it would be swept in the very call that starts one. Root `a` to
     * cover that window. */
    yas_gc_push_root(&a);

    /* v->size is left at 0 (calloc's zero-init, via gc_alloc()) until
     * the fill loop below has written a valid YasAtom into every slot -
     * gc_scan_object() walks exactly v->data[0..v->size), and setting
     * v->size before v->data is malloc'd/filled would let a step scan
     * uninitialized/garbage bytes as YasAtoms. */
    v->capacity = size > 0 ? size : 4;
    v->data = malloc(sizeof(YasAtom) * v->capacity);
    gc_account_extra(&v->gc, sizeof(YasAtom) * v->capacity);
    for (size_t i = 0; i < size; i++) {
        v->data[i] = fill;
    }
    v->size = size;

    if (size > 0)
        yas_gc_write_barrier(a, fill);

    yas_gc_pop_roots(1);
    return a;
}

YasAtom yas_make_vector(size_t size, YasAtom fill)
{
    YasAtom r = yas_make_vector_impl(size, fill);
    return r;
}


static YasAtom yas_make_hash_impl(void)
{
    YasAtom a;
    struct YasHash *h = gc_alloc(sizeof(struct YasHash), YAS_GC_OBJ_HASH);
    a.type = YAS_ATOM_TYPE_HASH;
    a.value.hash = h;
    /* Same rationale as yas_make_vector() above. */
    yas_gc_push_root(&a);

    h->size = 0;
    h->capacity = 16;
    h->keys = malloc(sizeof(YasAtom) * h->capacity);
    h->values = malloc(sizeof(YasAtom) * h->capacity);
    gc_account_extra(&h->gc, sizeof(YasAtom) * h->capacity * 2);
    h->metatable = NULL;

    h->index_capacity = h->capacity * 2;
    h->index = malloc(sizeof(size_t) * h->index_capacity);
    for (size_t i = 0; i < h->index_capacity; i++)
        h->index[i] = YAS_HASH_INDEX_EMPTY;
    gc_account_extra(&h->gc, sizeof(size_t) * h->index_capacity);

    yas_gc_pop_roots(1);
    return a;
}

YasAtom yas_make_hash(void)
{
    YasAtom r = yas_make_hash_impl();
    return r;
}


/* ── Environments ─────────────────────────────────────────────────── */

/* Symbols are interned (see yas_make_sym() above), so `sym.value.symbol`
 * pointer identity is the key - a fast integer hash over the pointer
 * bits, not a string hash. Mask, not modulo: `capacity` is always a
 * power of two. */
static size_t env_hash_ptr(const void *p)
{
    uintptr_t x = (uintptr_t)p;
    x ^= x >> 13;
    x *= (uintptr_t)0xff51afd7ed558ccdULL;
    x ^= x >> 13;
    return (size_t)x;
}

/* Probe env `e` for `sym`. Returns the occupied slot on a hit, or the
 * first empty slot found on a miss (`e->syms[slot]` distinguishes the
 * two - it is NIL only on a miss, since a symbol atom is never NIL).
 * Open addressing, no deletes, so no tombstones to skip. */
static size_t env_probe(struct YasEnv *e, YasAtom sym)
{
    size_t mask = e->capacity - 1;
    size_t i = env_hash_ptr(sym.value.symbol) & mask;
    while (!yas_nilp(e->syms[i])) {
        if (e->syms[i].value.symbol == sym.value.symbol)
            return i;
        i = (i + 1) & mask;
    }
    return i;
}

/* Doubles capacity and rehashes every occupied slot. Mirrors
 * yas_hash_set()'s capacity doubling / intern_grow() (above) - grow
 * before crossing 70% load, not after, so env_probe() never has to
 * search a full table. */
static void env_grow(struct YasEnv *e)
{
    size_t old_capacity = e->capacity;
    YasAtom *old_syms = e->syms;
    YasAtom *old_vals = e->vals;

    e->capacity = old_capacity * 2;
    e->syms = malloc(sizeof(YasAtom) * e->capacity);
    e->vals = malloc(sizeof(YasAtom) * e->capacity);
    for (size_t i = 0; i < e->capacity; i++) {
        e->syms[i] = yas_nil;
        e->vals[i] = yas_nil;
    }
    gc_account_extra(&e->gc, sizeof(YasAtom) * (e->capacity - old_capacity) * 2);

    for (size_t i = 0; i < old_capacity; i++) {
        if (yas_nilp(old_syms[i]))
            continue;
        size_t slot = env_probe(e, old_syms[i]);
        e->syms[slot] = old_syms[i];
        e->vals[slot] = old_vals[i];
    }
    free(old_syms);
    free(old_vals);
}

static YasAtom yas_env_create_impl(YasAtom parent)
{
    YasAtom a;
    struct YasEnv *e = gc_alloc(sizeof(struct YasEnv), YAS_GC_OBJ_ENV);
    a.type = YAS_ATOM_TYPE_ENV;
    a.value.env = e;
    /* Same rationale as yas_make_vector()/yas_make_hash() above: `e` is
     * BLACK if allocated mid-cycle (gc_alloc()'s allocate-black), and the
     * malloc()s/write barrier below can themselves step the GC before
     * this constructor returns. */
    yas_gc_push_root(&a);

    e->parent = yas_nil;
    e->count = 0;
    e->capacity = 8;
    e->syms = malloc(sizeof(YasAtom) * e->capacity);
    e->vals = malloc(sizeof(YasAtom) * e->capacity);
    for (size_t i = 0; i < e->capacity; i++) {
        e->syms[i] = yas_nil;
        e->vals[i] = yas_nil;
    }
    gc_account_extra(&e->gc, sizeof(YasAtom) * e->capacity * 2);

    e->parent = parent;
    e->root = yas_nilp(parent) ? e : parent.value.env->root;
    yas_gc_write_barrier(a, parent);

    yas_gc_pop_roots(1);
    return a;
}

YasAtom yas_env_create(YasAtom parent)
{
    YasAtom r = yas_env_create_impl(parent);
    return r;
}


static int yas_env_define_impl(YasAtom env, YasAtom symbol, YasAtom value)
{
    struct YasEnv *e = env.value.env;
    size_t slot = env_probe(e, symbol);

    /* Any insert into a non-root frame permanently disqualifies this
     * symbol from the fast path below (ticket #45) - set before the
     * insert so the flag is live before the frame's body runs. Monotone,
     * never cleared: a false "shadowed" just means an extra parent-chain
     * walk, never a wrong value. */
    if (e != e->root)
        sym_rec(symbol.value.symbol)->shadowed = 1;

    if (!yas_nilp(e->syms[slot])) {
        e->vals[slot] = value;
        yas_gc_write_barrier(env, value);
        return YAS_ERROR_OK;
    }

    if ((e->count + 1) * 10 >= e->capacity * 7) {
        env_grow(e);
        slot = env_probe(e, symbol);
    }

    e->syms[slot] = symbol;
    e->vals[slot] = value;
    e->count++;
    yas_gc_write_barrier(env, symbol);
    yas_gc_write_barrier(env, value);

    return YAS_ERROR_OK;
}

int yas_env_define(YasAtom env, YasAtom symbol, YasAtom value)
{
    int r = yas_env_define_impl(env, symbol, value);
    return r;
}


static int yas_env_get_impl(YasAtom env, YasAtom symbol, YasAtom *result)
{
    /* Root-frame fast path (ticket #45): a symbol never yas_env_define()'d
     * into a non-root frame always resolves at the chain's root, so cache
     * (root, slot) behind the interned symbol and skip both the
     * frame-by-frame walk and the per-frame probe on a hit. The cached
     * slot is re-validated against the live table on every use - this is
     * what makes env_grow()'s rehash (which moves every slot) and the
     * free-list pool's env-address recycling (ticket #44) safe without a
     * generation counter: a stale/foreign slot either fails the symbol
     * check or the `cache_slot < capacity` bounds check, and falls back
     * to a fresh probe. */
    struct YasSymbol *s = sym_rec(symbol.value.symbol);
    if (!s->shadowed) {
        struct YasEnv *r = env.value.env->root;
        if (s->cache_env == r && s->cache_slot < r->capacity &&
            r->syms[s->cache_slot].value.symbol == symbol.value.symbol) {
            *result = r->vals[s->cache_slot];
            return YAS_ERROR_OK;
        }
        size_t slot = env_probe(r, symbol);
        if (yas_nilp(r->syms[slot]))
            return YAS_ERROR_UNBOUND; /* sound: not shadowed anywhere */
        s->cache_env = r;
        s->cache_slot = slot;
        *result = r->vals[slot];
        return YAS_ERROR_OK;
    }

    struct YasEnv *e = env.value.env;
    for (;;) {
        size_t slot = env_probe(e, symbol);
        if (!yas_nilp(e->syms[slot])) {
            *result = e->vals[slot];
            return YAS_ERROR_OK;
        }
        if (yas_nilp(e->parent))
            return YAS_ERROR_UNBOUND;
        e = e->parent.value.env;
    }
}

int yas_env_get(YasAtom env, YasAtom symbol, YasAtom *result)
{
    int r = yas_env_get_impl(env, symbol, result);
    return r;
}


static int yas_env_set_impl(YasAtom env, YasAtom symbol, YasAtom value)
{
    struct YasSymbol *s = sym_rec(symbol.value.symbol);
    struct YasEnv *start = env.value.env;

    /* Root-frame fast path (ticket #45), gated for module boundaries:
     * an unshadowed symbol lives at the chain's root alone - but for a
     * module env that root is the ONE SHARED yas/base frame, and a
     * plain write-through would publish the mutation to every module in
     * the process. Only take the fast write when the caller is the root
     * itself, or the root is nobody's shared layer (top-level script
     * envs, which flat-copy base); anything else falls through to the
     * boundary-aware walk below. */
    if (!s->shadowed && (start == start->root || !start->root->module_boundary)) {
        struct YasEnv *r = start->root;
        if (s->cache_env == r && s->cache_slot < r->capacity &&
            r->syms[s->cache_slot].value.symbol == symbol.value.symbol) {
            r->vals[s->cache_slot] = value;
            yas_gc_write_barrier((YasAtom){.type = YAS_ATOM_TYPE_ENV, .value.env = r}, value);
            return YAS_ERROR_OK;
        }
        size_t slot = env_probe(r, symbol);
        if (yas_nilp(r->syms[slot]))
            return YAS_ERROR_UNBOUND;
        s->cache_env = r;
        s->cache_slot = slot;
        r->vals[slot] = value;
        yas_gc_write_barrier((YasAtom){.type = YAS_ATOM_TYPE_ENV, .value.env = r}, value);
        return YAS_ERROR_OK;
    }

    struct YasEnv *e = start;
    for (;;) {
        size_t slot = env_probe(e, symbol);
        if (!yas_nilp(e->syms[slot])) {
            YasAtom eatom = {.type = YAS_ATOM_TYPE_ENV, .value.env = e};
            e->vals[slot] = value;
            yas_gc_write_barrier(eatom, value);
            return YAS_ERROR_OK;
        }
        if (e->module_boundary) {
            /* Terminal frame for set!: the name wasn't owned here, so
             * it is inherited from the shared layer behind this frame -
             * shadow-copy the new value into THIS frame instead of
             * writing through (the old behavior corrupted base for every
             * module at once). A name bound nowhere on the far side
             * keeps raising UNBOUND, so set! never becomes definition. */
            if (!yas_nilp(e->parent)) {
                struct YasEnv *b = e->parent.value.env;
                size_t bslot = env_probe(b, symbol);
                if (!yas_nilp(b->syms[bslot])) {
                    YasAtom eatom = {.type = YAS_ATOM_TYPE_ENV, .value.env = e};
                    return yas_env_define_impl(eatom, symbol, value);
                }
            }
            return YAS_ERROR_UNBOUND;
        }
        if (yas_nilp(e->parent))
            return YAS_ERROR_UNBOUND;
        e = e->parent.value.env;
    }
}

int yas_env_set(YasAtom env, YasAtom symbol, YasAtom value)
{
    int r = yas_env_set_impl(env, symbol, value);
    return r;
}


/* Collect every symbol bound in this exact frame (not the parent chain)
 * into a freshly consed list. The module system's import uses this to
 * enumerate a module's public surface; nothing else should rely on slot
 * ordering. The accumulator grows through one yas_cons() per binding -
 * the standard accumulator-loop hazard (docs/GC.md) - so it stays rooted
 * for the whole walk; each slot's symbol is fetched before the cons that
 * might step the collector. */
static YasAtom yas_env_keys_impl(YasAtom env)
{
    struct YasEnv *e = env.value.env;
    YasAtom acc = yas_nil;
    yas_gc_push_root(&acc);
    for (size_t i = 0; i < e->capacity; i++) {
        if (yas_nilp(e->syms[i]))
            continue;
        acc = yas_cons(e->syms[i], acc);
    }
    yas_gc_pop_roots(1); /* acc */
    return acc;
}

YasAtom yas_env_keys(YasAtom env)
{
    YasAtom r = yas_env_keys_impl(env);
    return r;
}


/* Vector operations */
static YasAtom yas_vector_ref_impl(YasAtom vec, size_t index)
{
    return vec.value.vector->data[index];
}

YasAtom yas_vector_ref(YasAtom vec, size_t index)
{
    YasAtom r = yas_vector_ref_impl(vec, index);
    return r;
}


static void yas_vector_set_impl(YasAtom vec, size_t index, YasAtom value)
{
    vec.value.vector->data[index] = value;
    yas_gc_write_barrier(vec, value);
}

void yas_vector_set(YasAtom vec, size_t index, YasAtom value)
{
    yas_vector_set_impl(vec, index, value);
}


static size_t yas_vector_length_impl(YasAtom vec)
{
    return vec.value.vector->size;
}

size_t yas_vector_length(YasAtom vec)
{
    size_t r = yas_vector_length_impl(vec);
    return r;
}


/* Hash operations - dense keys[]/values[] arrays (unchanged: iteration
 * order and yas_hash_remove()'s swap-last-into-hole compaction, both
 * relied on elsewhere - gc_scan_object(), hash-keys, module.c's export
 * resolution - aren't a contract this index needs to preserve, but the
 * dense storage itself is), plus an open-addressed `index` (yas.h) of
 * slot numbers giving O(1) amortized lookup instead of an O(n) scan. */
static int atom_equal(YasAtom a, YasAtom b)
{
    if (a.type != b.type)
        return 0;
    switch (a.type) {
        case YAS_ATOM_TYPE_NIL:
            return 1;
        case YAS_ATOM_TYPE_SYMBOL:
            return a.value.symbol == b.value.symbol;
        case YAS_ATOM_TYPE_KEYWORD:
            return a.value.keyword == b.value.keyword;
        case YAS_ATOM_TYPE_INTEGER:
            return a.value.integer == b.value.integer;
        case YAS_ATOM_TYPE_FLOAT:
            return a.value.floatval == b.value.floatval;
        case YAS_ATOM_TYPE_STRING:
            return strcmp(a.value.string, b.value.string) == 0;
        default:
            return a.value.pair == b.value.pair;
    }
}

/* Must agree with atom_equal() above exactly, key type by key type -
 * two atoms atom_equal() calls equal must hash identically. Reuses
 * env_hash_ptr()/fnv1a_hash() (both defined earlier in this file, for
 * the environment index and the symbol/keyword intern tables
 * respectively) rather than a third hash implementation. Integer/float
 * route their bit pattern through env_hash_ptr() as a pointer value -
 * never dereferenced, just reusing its mixing step. */
static size_t atom_hash(YasAtom a)
{
    switch (a.type) {
        case YAS_ATOM_TYPE_NIL:
            return 0;
        case YAS_ATOM_TYPE_SYMBOL:
            return env_hash_ptr(a.value.symbol);
        case YAS_ATOM_TYPE_KEYWORD:
            return env_hash_ptr(a.value.keyword);
        case YAS_ATOM_TYPE_INTEGER:
            return env_hash_ptr((const void *)(uintptr_t)a.value.integer);
        case YAS_ATOM_TYPE_FLOAT: {
            uint64_t bits;
            memcpy(&bits, &a.value.floatval, sizeof(bits));
            return env_hash_ptr((const void *)(uintptr_t)bits);
        }
        case YAS_ATOM_TYPE_STRING:
            return fnv1a_hash(a.value.string);
        default:
            return env_hash_ptr(a.value.pair);
    }
}

/* Probe `h->index` for `key` (already-computed `key_hash`). Returns the
 * occupied slot on a hit, or the first empty slot on a miss -
 * `h->index[slot] == YAS_HASH_INDEX_EMPTY` distinguishes the two. Open
 * addressing, no deletes (yas_hash_remove() rebuilds wholesale instead
 * of leaving a tombstone - see there). */
static size_t hash_index_probe(struct YasHash *h, YasAtom key, size_t key_hash)
{
    size_t mask = h->index_capacity - 1;
    size_t i = key_hash & mask;
    while (h->index[i] != YAS_HASH_INDEX_EMPTY) {
        if (atom_equal(h->keys[h->index[i]], key))
            return i;
        i = (i + 1) & mask;
    }
    return i;
}

/* Rebuilds `h->index` from scratch against the current `h->keys`/
 * `h->size` - used both after growing `index_capacity` (yas_hash_set())
 * and after yas_hash_remove()'s compaction shifts a slot number. Every
 * key in `h->keys[0..size)` is already known unique, so no equal-key
 * collision handling is needed here beyond ordinary open-addressing
 * probing. */
static void hash_index_rebuild(struct YasHash *h)
{
    for (size_t i = 0; i < h->index_capacity; i++)
        h->index[i] = YAS_HASH_INDEX_EMPTY;
    for (size_t i = 0; i < h->size; i++) {
        size_t slot = hash_index_probe(h, h->keys[i], atom_hash(h->keys[i]));
        h->index[slot] = i;
    }
}

static YasAtom yas_hash_ref_impl(YasAtom hash, YasAtom key, YasAtom default_val)
{
    struct YasHash *h = hash.value.hash;
    size_t slot = hash_index_probe(h, key, atom_hash(key));
    if (h->index[slot] != YAS_HASH_INDEX_EMPTY)
        return h->values[h->index[slot]];
    return default_val;
}

YasAtom yas_hash_ref(YasAtom hash, YasAtom key, YasAtom default_val)
{
    YasAtom r = yas_hash_ref_impl(hash, key, default_val);
    return r;
}


static void yas_hash_set_impl(YasAtom hash, YasAtom key, YasAtom value)
{
    struct YasHash *h = hash.value.hash;
    size_t key_hash = atom_hash(key);
    size_t slot = hash_index_probe(h, key, key_hash);

    if (h->index[slot] != YAS_HASH_INDEX_EMPTY) {
        h->values[h->index[slot]] = value;
        yas_gc_write_barrier(hash, value);
        return;
    }

    /* Add new key */
    if (h->size >= h->capacity) {
        h->capacity *= 2;
        h->keys = realloc(h->keys, sizeof(YasAtom) * h->capacity);
        h->values = realloc(h->values, sizeof(YasAtom) * h->capacity);

        /* index_capacity tracks capacity * 2 (see yas.h) - rebuild
         * against the new, larger table rather than growing in place,
         * since every existing slot's probe sequence changes with the
         * mask. */
        free(h->index);
        h->index_capacity = h->capacity * 2;
        h->index = malloc(sizeof(size_t) * h->index_capacity);
        hash_index_rebuild(h);
        slot = hash_index_probe(h, key, key_hash);
    }

    h->keys[h->size] = key;
    h->values[h->size] = value;
    h->index[slot] = h->size;
    h->size++;
    yas_gc_write_barrier(hash, key);
    yas_gc_write_barrier(hash, value);
}

void yas_hash_set(YasAtom hash, YasAtom key, YasAtom value)
{
    yas_hash_set_impl(hash, key, value);
}


static int yas_hash_remove_impl(YasAtom hash, YasAtom key)
{
    struct YasHash *h = hash.value.hash;
    size_t slot = hash_index_probe(h, key, atom_hash(key));
    if (h->index[slot] == YAS_HASH_INDEX_EMPTY)
        return 0;

    size_t i = h->index[slot];
    /* Move last element to this position. Both the moved key and value
     * were already reachable through this same hash before the move,
     * so no new white child is introduced - no write barrier needed
     * here. */
    h->size--;
    if (i < h->size) {
        h->keys[i] = h->keys[h->size];
        h->values[i] = h->values[h->size];
    }
    /* The move above changed which slot number names the relocated
     * entry, and open addressing has no delete - cheapest correct fix
     * is a wholesale rebuild rather than tombstone bookkeeping. Removal
     * is rare relative to ref/set, so this is not the hot path. */
    hash_index_rebuild(h);
    return 1;
}

int yas_hash_remove(YasAtom hash, YasAtom key)
{
    int r = yas_hash_remove_impl(hash, key);
    return r;
}


static size_t yas_hash_size_impl(YasAtom hash)
{
    return hash.value.hash->size;
}

size_t yas_hash_size(YasAtom hash)
{
    size_t r = yas_hash_size_impl(hash);
    return r;
}


/* Metatable operations */
static YasAtom yas_get_metamethod_impl(YasAtom hash, const char *name)
{
    struct YasHash *h = hash.value.hash;
    if (!h->metatable)
        return yas_nil;

    /* Use keyword for metamethod lookup (uppercase to match reader) */
    char kw_name[64];
    snprintf(kw_name, sizeof(kw_name), "%s", name);
    /* Convert to uppercase */
    for (char *p = kw_name; *p; p++) {
        if (*p >= 'a' && *p <= 'z')
            *p -= 32;
    }
    YasAtom key = yas_make_keyword(kw_name);

    /* Look up in metatable */
    YasAtom mt;
    mt.type = YAS_ATOM_TYPE_HASH;
    mt.value.hash = h->metatable;
    return yas_hash_ref(mt, key, yas_nil);
}

YasAtom yas_get_metamethod(YasAtom hash, const char *name)
{
    YasAtom r = yas_get_metamethod_impl(hash, name);
    return r;
}


static int yas_has_metamethod_impl(YasAtom hash, const char *name)
{
    return !yas_nilp(yas_get_metamethod(hash, name));
}

int yas_has_metamethod(YasAtom hash, const char *name)
{
    int r = yas_has_metamethod_impl(hash, name);
    return r;
}


static int yas_listp_impl(YasAtom expr)
{
    while (!yas_nilp(expr)) {
        if (expr.type != YAS_ATOM_TYPE_PAIR)
            return 0;
        expr = yas_cdr(expr);
    }
    return 1;
}

int yas_listp(YasAtom expr)
{
    int r = yas_listp_impl(expr);
    return r;
}


static YasAtom yas_copy_list_impl(YasAtom list)
{
    YasAtom a, p;

    if (yas_nilp(list))
        return yas_nil;

    a = yas_cons(yas_car(list), yas_nil);
    /* `a` (the new list's head) is a fresh, otherwise-unreachable local
     * built up across further yas_cons() calls below - root it for the
     * loop's duration. `p` needs no separate root: it's always reachable
     * by walking cdrs from `a`, since each iteration's yas_set_cdr()
     * links the newly-consed node in before `p` ever advances past it.
     * `list` (the *source* being copied) is this function's parameter,
     * not a value it created - whether it needs protection is the
     * caller's responsibility, per docs/GC.md's "Callers not affected". */
    yas_gc_push_root(&a);
    p = a;
    list = yas_cdr(list);

    while (!yas_nilp(list)) {
        yas_set_cdr(p, yas_cons(yas_car(list), yas_nil));
        p = yas_cdr(p);
        list = yas_cdr(list);
    }

    yas_gc_pop_roots(1);
    return a;
}

YasAtom yas_copy_list(YasAtom list)
{
    YasAtom r = yas_copy_list_impl(list);
    return r;
}


/* Variadic, so the standard impl+wrapper shape can't just forward the
 * argument list - the va_list walk moves into yas_list_create_v() and the
 * locking wrapper does the single va_start/va_end around it. */
static YasAtom yas_list_create_v(int n, va_list ap)
{
    YasAtom list = yas_nil;

    /* `list` accumulates across repeated yas_cons() calls below - each
     * one is a fresh, otherwise-unreachable pair until the *next*
     * iteration links it in, so root the slot for the whole loop (one
     * push covers every reassignment - yas_gc_push_root() tracks the
     * slot's address, not a snapshot). This is not hypothetical: two
     * items are already enough to reproduce a real use-after-free here
     * (main.c's own top-level yas/base import, at process startup
     * before the heap is large enough for bounded stepping to make this
     * astronomically unlikely instead of merely unlikely). */
    yas_gc_push_root(&list);
    while (n--) {
        YasAtom item = va_arg(ap, YasAtom);
        list = yas_cons(item, list);
    }

    yas_list_reverse(&list);
    yas_gc_pop_roots(1);
    return list;
}

YasAtom yas_list_create(int n, ...)
{
    va_list ap;
    YasAtom r;
    va_start(ap, n);
    r = yas_list_create_v(n, ap);
    va_end(ap);
    return r;
}

static YasAtom yas_list_get_impl(YasAtom list, int k)
{
    while (k--)
        list = yas_cdr(list);
    return yas_car(list);
}

YasAtom yas_list_get(YasAtom list, int k)
{
    YasAtom r = yas_list_get_impl(list, k);
    return r;
}


static void yas_list_set_impl(YasAtom list, int k, YasAtom value)
{
    while (k--)
        list = yas_cdr(list);
    yas_set_car(list, value);
}

void yas_list_set(YasAtom list, int k, YasAtom value)
{
    yas_list_set_impl(list, k, value);
}


static void yas_list_reverse_impl(YasAtom *list)
{
    YasAtom tail = yas_nil;
    while (!yas_nilp(*list)) {
        YasAtom p = yas_cdr(*list);
        yas_set_cdr(*list, tail);
        tail = *list;
        *list = p;
    }
    *list = tail;
}

void yas_list_reverse(YasAtom *list)
{
    yas_list_reverse_impl(list);
}


/* Port operations */
static YasAtom yas_make_port_impl(void *handle, const char *path, int is_input, int is_binary, int provider_id)
{
    YasAtom a;
    struct YasPort *p = gc_alloc(sizeof(struct YasPort), YAS_GC_OBJ_PORT);
    p->handle = handle;
    p->path = strdup(path);
    p->is_input = is_input;
    p->is_binary = is_binary;
    p->is_open = 1;
    p->provider_id = provider_id;
    a.type = YAS_ATOM_TYPE_PORT;
    a.value.port = p;

    return a;
}

YasAtom yas_make_port(void *handle, const char *path, int is_input, int is_binary, int provider_id)
{
    YasAtom r = yas_make_port_impl(handle, path, is_input, is_binary, provider_id);
    return r;
}


#ifdef YAS_FFI
/* FFIFunction/FFIStructInstance allocation, for ffi.c - these two types
 * are tracked in the same unified GC list as everything else rather than
 * their own global lists (see yas.h). ffi.c fills in the fields; these
 * just get a zeroed, GC-tracked block of the right size. */
static struct YasFFIFunction * yas_gc_alloc_ffi_function_impl(void)
{
    return gc_alloc(sizeof(struct YasFFIFunction), YAS_GC_OBJ_FFI_FUNCTION);
}

struct YasFFIFunction * yas_gc_alloc_ffi_function(void)
{
    struct YasFFIFunction * r = yas_gc_alloc_ffi_function_impl();
    return r;
}


static YasFFIStructInstance * yas_gc_alloc_ffi_struct_impl(void)
{
    return gc_alloc(sizeof(YasFFIStructInstance), YAS_GC_OBJ_FFI_STRUCT);
}

YasFFIStructInstance * yas_gc_alloc_ffi_struct(void)
{
    YasFFIStructInstance * r = yas_gc_alloc_ffi_struct_impl();
    return r;
}

#endif

/* Bytevector operations */
static YasAtom yas_make_bytevector_impl(size_t size)
{
    YasAtom a;
    struct YasBytevector *bv = gc_alloc(sizeof(struct YasBytevector), YAS_GC_OBJ_BYTEVECTOR);
    a.type = YAS_ATOM_TYPE_BYTEVECTOR;
    a.value.bytevector = bv;
    /* Same rationale as yas_make_vector() above: `bv` itself (not its
     * contents - bytevectors are GC leaves, gc_scan_object() never reads
     * bv->data) needs protecting across gc_account_extra() below. */
    yas_gc_push_root(&a);

    bv->size = size;
    bv->data = calloc(size, 1);
    gc_account_extra(&bv->gc, size);

    yas_gc_pop_roots(1);
    return a;
}

YasAtom yas_make_bytevector(size_t size)
{
    YasAtom r = yas_make_bytevector_impl(size);
    return r;
}


static YasAtom yas_make_bytevector_from_data_impl(const unsigned char *data, size_t size)
{
    YasAtom a;
    struct YasBytevector *bv = gc_alloc(sizeof(struct YasBytevector), YAS_GC_OBJ_BYTEVECTOR);
    a.type = YAS_ATOM_TYPE_BYTEVECTOR;
    a.value.bytevector = bv;
    /* Same rationale as yas_make_bytevector() above. */
    yas_gc_push_root(&a);

    bv->size = size;
    bv->data = malloc(size);
    memcpy(bv->data, data, size);
    gc_account_extra(&bv->gc, size);

    yas_gc_pop_roots(1);
    return a;
}

YasAtom yas_make_bytevector_from_data(const unsigned char *data, size_t size)
{
    YasAtom r = yas_make_bytevector_from_data_impl(data, size);
    return r;
}


static unsigned char yas_bytevector_ref_impl(YasAtom bv, size_t index)
{
    return bv.value.bytevector->data[index];
}

unsigned char yas_bytevector_ref(YasAtom bv, size_t index)
{
    unsigned char r = yas_bytevector_ref_impl(bv, index);
    return r;
}


static void yas_bytevector_set_impl(YasAtom bv, size_t index, unsigned char value)
{
    bv.value.bytevector->data[index] = value;
}

void yas_bytevector_set(YasAtom bv, size_t index, unsigned char value)
{
    yas_bytevector_set_impl(bv, index, value);
}


static size_t yas_bytevector_length_impl(YasAtom bv)
{
    return bv.value.bytevector->size;
}

size_t yas_bytevector_length(YasAtom bv)
{
    size_t r = yas_bytevector_length_impl(bv);
    return r;
}


/* SIMD Vector/Matrix constructors */
static yas_vec4_t *gc_alloc_vec(void)
{
    struct YasVecAlloc *va = gc_alloc(sizeof(struct YasVecAlloc), YAS_GC_OBJ_VEC);
    return &va->v;
}

static yas_mat4_t *gc_alloc_mat4(void)
{
    struct YasMat4Alloc *ma = gc_alloc(sizeof(struct YasMat4Alloc), YAS_GC_OBJ_MAT4);
    return &ma->m;
}

static YasAtom yas_make_vec2_impl(float x, float y)
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

YasAtom yas_make_vec2(float x, float y)
{
    YasAtom r = yas_make_vec2_impl(x, y);
    return r;
}


static YasAtom yas_make_vec3_impl(float x, float y, float z)
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

YasAtom yas_make_vec3(float x, float y, float z)
{
    YasAtom r = yas_make_vec3_impl(x, y, z);
    return r;
}


static YasAtom yas_make_vec4_impl(float x, float y, float z, float w)
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

YasAtom yas_make_vec4(float x, float y, float z, float w)
{
    YasAtom r = yas_make_vec4_impl(x, y, z, w);
    return r;
}


static YasAtom yas_make_mat4_identity_impl(void)
{
    YasAtom a;
    a.type = YAS_ATOM_TYPE_MAT4;
    a.value.mat4 = gc_alloc_mat4();
    /* Column-major identity matrix */
    for (int c = 0; c < 4; c++) {
#ifdef YAS_SIMD_X86
        a.value.mat4->columns[c].v =
            _mm_set_ps(c == 3 ? 1.0f : 0.0f, c == 2 ? 1.0f : 0.0f, c == 1 ? 1.0f : 0.0f, c == 0 ? 1.0f : 0.0f);
#elif defined(YAS_SIMD_ARM_NEON)
        float vals[4] = {c == 0 ? 1.0f : 0.0f, c == 1 ? 1.0f : 0.0f, c == 2 ? 1.0f : 0.0f, c == 3 ? 1.0f : 0.0f};
        a.value.mat4->columns[c].v = vld1q_f32(vals);
#elif defined(YAS_SIMD_WASM)
        a.value.mat4->columns[c].v =
            wasm_f32x4_make(c == 0 ? 1.0f : 0.0f, c == 1 ? 1.0f : 0.0f, c == 2 ? 1.0f : 0.0f, c == 3 ? 1.0f : 0.0f);
#else
        a.value.mat4->columns[c].data[0] = (c == 0) ? 1.0f : 0.0f;
        a.value.mat4->columns[c].data[1] = (c == 1) ? 1.0f : 0.0f;
        a.value.mat4->columns[c].data[2] = (c == 2) ? 1.0f : 0.0f;
        a.value.mat4->columns[c].data[3] = (c == 3) ? 1.0f : 0.0f;
#endif
    }
    return a;
}

YasAtom yas_make_mat4_identity(void)
{
    YasAtom r = yas_make_mat4_identity_impl();
    return r;
}


static YasAtom yas_make_mat4_from_floats_impl(const float *values)
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

YasAtom yas_make_mat4_from_floats(const float *values)
{
    YasAtom r = yas_make_mat4_from_floats_impl(values);
    return r;
}


/* ============================================================
 * GLOBAL FEATURES SYSTEM
 * ============================================================ */

static void yas_init_features_impl(void)
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

void yas_init_features(void)
{
    yas_init_features_impl();
}


static int yas_has_feature_impl(const char *name)
{
    YasAtom p = yas_global_features;
    while (!yas_nilp(p)) {
        if (yas_car(p).type == YAS_ATOM_TYPE_SYMBOL && strcmp(yas_car(p).value.symbol, name) == 0) {
            return 1;
        }
        p = yas_cdr(p);
    }
    return 0;
}

int yas_has_feature(const char *name)
{
    int r = yas_has_feature_impl(name);
    return r;
}


/* Evaluate compound feature expressions like (or macos linux), (and x y), (not x) */
static int yas_eval_feature_expr_impl(YasAtom expr)
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

int yas_eval_feature_expr(YasAtom expr)
{
    int r = yas_eval_feature_expr_impl(expr);
    return r;
}


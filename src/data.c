#include "yas.h"
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

typedef enum {
	YAS_GC_IDLE,
	YAS_GC_MARKING,
	YAS_GC_SWEEPING
} GCPhase;

#define YAS_GC_INIT_THRESHOLD (256 * 1024)  /* bytes */
#define YAS_GC_STEP_LIMIT 512               /* objects per bounded step */

static struct YasGCHeader *gc_all = NULL;
static GCPhase gc_phase = YAS_GC_IDLE;
static size_t gc_allocated = 0;
static size_t gc_threshold = 0;   /* 0 = "not yet initialized", see gc_min_threshold_get() */

static struct YasGCHeader **gc_gray = NULL;
static int gc_gray_count = 0;
static int gc_gray_cap = 0;

static struct YasGCHeader *gc_sweep_prev = NULL;

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
#define YAS_GC_DEBT_QUANTUM_DEFAULT (4 * 1024)  /* bytes allocated per step */
static size_t gc_debt = 0;
static size_t gc_debt_quantum = 0;   /* 0 = "not yet read from env" */
static int    gc_stepping = 0;       /* re-entrancy guard, see gc_alloc() */

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
static size_t gc_min_threshold = 0;  /* 0 = "not yet read from env" */
static long   gc_stress_bytes = -1;  /* -1 = "not yet read from env"; 0 = unset */

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

/* Interning tables, needed by gc_mark_roots() below - forward-declared
 * here since they're otherwise only used starting from yas_make_sym()/
 * yas_make_keyword() further down this file. */
static YasAtom sym_table = { YAS_ATOM_TYPE_NIL };
static YasAtom kw_table = { YAS_ATOM_TYPE_NIL };

/* Root-registration API (see yas.h and docs/GC.md). A stack of pointers
 * to caller-owned YasAtom slots, scanned in gc_mark_roots(). */
static YasAtom **gc_roots = NULL;
static int gc_root_count = 0;
static int gc_root_cap = 0;

void yas_gc_push_root(YasAtom *slot)
{
	if (!slot) return;
	if (gc_root_count >= gc_root_cap) {
		int new_cap = gc_root_cap ? gc_root_cap * 2 : 16;
		YasAtom **tmp = realloc(gc_roots, (size_t)new_cap * sizeof(YasAtom *));
		if (!tmp) return; /* OOM: drop the root; caller must keep it reachable another way */
		gc_roots = tmp;
		gc_root_cap = new_cap;
	}
	gc_roots[gc_root_count++] = slot;
}

void yas_gc_pop_roots(int n)
{
	gc_root_count -= n;
	if (gc_root_count < 0)
		gc_root_count = 0;
}

int yas_gc_root_count(void)
{
	return gc_root_count;
}

int yas_gc_phase(void)
{
	return (int)gc_phase;
}

size_t yas_gc_allocated(void)
{
	return gc_allocated;
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
	case YAS_ATOM_TYPE_MACRO:
	case YAS_ATOM_TYPE_CONTINUATION: {
		if (!a.value.pair) return NULL;
		struct Allocation *alloc = (struct Allocation *)
			((char *)a.value.pair - offsetof(struct Allocation, pair));
		return &alloc->gc;
	}
	case YAS_ATOM_TYPE_VECTOR:
		return a.value.vector ? &a.value.vector->gc : NULL;
	case YAS_ATOM_TYPE_HASH:
		return a.value.hash ? &a.value.hash->gc : NULL;
	case YAS_ATOM_TYPE_STRING: {
		if (!a.value.string) return NULL;
		struct YasStringAlloc *sa = (struct YasStringAlloc *)
			((char *)a.value.string - offsetof(struct YasStringAlloc, data));
		return &sa->gc;
	}
	case YAS_ATOM_TYPE_PORT:
		return a.value.port ? &a.value.port->gc : NULL;
	case YAS_ATOM_TYPE_BYTEVECTOR:
		return a.value.bytevector ? &a.value.bytevector->gc : NULL;
	case YAS_ATOM_TYPE_VEC2:
	case YAS_ATOM_TYPE_VEC3:
	case YAS_ATOM_TYPE_VEC4: {
		if (!a.value.vec) return NULL;
		struct YasVecAlloc *va = (struct YasVecAlloc *)
			((char *)a.value.vec - offsetof(struct YasVecAlloc, v));
		return &va->gc;
	}
	case YAS_ATOM_TYPE_MAT4: {
		if (!a.value.mat4) return NULL;
		struct YasMat4Alloc *ma = (struct YasMat4Alloc *)
			((char *)a.value.mat4 - offsetof(struct YasMat4Alloc, m));
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

	struct YasGCHeader *obj = calloc(1, size);
	if (!obj)
		return NULL;
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
			if (!tmp) return; /* OOM: drop the re-gray; container may be swept early */
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
void yas_gc_write_barrier(YasAtom container, YasAtom child)
{
	gc_write_barrier_headers(atom_to_header(container), atom_to_header(child));
}

void yas_set_car(YasAtom p, YasAtom v)
{
	yas_car(p) = v;
	yas_gc_write_barrier(p, v);
}

void yas_set_cdr(YasAtom p, YasAtom v)
{
	yas_cdr(p) = v;
	yas_gc_write_barrier(p, v);
}

/* ── Marking ──────────────────────────────────────────────────────── */

static void gc_push_gray(struct YasGCHeader *obj)
{
	if (gc_gray_count >= gc_gray_cap) {
		int new_cap = gc_gray_cap ? gc_gray_cap * 2 : 64;
		struct YasGCHeader **tmp = realloc(gc_gray, (size_t)new_cap * sizeof(struct YasGCHeader *));
		if (!tmp) return; /* OOM: drop the push; object may be swept, but better than crash */
		gc_gray = tmp;
		gc_gray_cap = new_cap;
	}
	gc_gray[gc_gray_count++] = obj;
}

/* Grays and enqueues `root`. NOT a full trace - see the contract note on
 * yas_gc_mark() in yas.h. */
void yas_gc_mark(YasAtom root)
{
	struct YasGCHeader *h = atom_to_header(root);
	if (!h || h->color != YAS_GC_WHITE)
		return;
	h->color = YAS_GC_GRAY;
	gc_push_gray(h);
}

static void gc_mark_roots(void)
{
	yas_gc_mark(sym_table);
	yas_gc_mark(kw_table);
	yas_gc_mark(yas_library_registry);
	yas_gc_mark(yas_global_search_paths);
	yas_gc_mark(yas_global_features);
	yas_gc_mark(yas_ctx_exception);

	for (int i = 0; i < gc_root_count; i++)
		yas_gc_mark(*gc_roots[i]);

	yas_eval_mark_roots();
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
		break;
	case YAS_GC_OBJ_VECTOR: {
		struct YasVector *v = (struct YasVector *)obj;
		free(v->data);
		break;
	}
	case YAS_GC_OBJ_HASH: {
		struct YasHash *h = (struct YasHash *)obj;
		free(h->keys);
		free(h->values);
		break;
	}
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
		break;
	}
	case YAS_GC_OBJ_BYTEVECTOR: {
		struct YasBytevector *bv = (struct YasBytevector *)obj;
		free(bv->data);
		break;
	}
	case YAS_GC_OBJ_VEC:
	case YAS_GC_OBJ_MAT4:
		/* Single allocation - nothing extra. */
		break;
#ifdef YAS_FFI
	case YAS_GC_OBJ_FFI_FUNCTION: {
		struct YasFFIFunction *fn = (struct YasFFIFunction *)obj;
		if (fn->cif) free(fn->cif);
		if (fn->arg_types) free(fn->arg_types);
		if (fn->ffi_arg_types) free(fn->ffi_arg_types);
		break;
	}
	case YAS_GC_OBJ_FFI_STRUCT: {
		YasFFIStructInstance *inst = (YasFFIStructInstance *)obj;
		free(inst->data);
		break;
	}
#endif
	}
	*freed += obj->backing_size;
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
	}
}

/* ── Public entry points ──────────────────────────────────────────── */

void yas_gc_start(void)
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

/* One bounded slice of whichever phase is active; a no-op (besides the
 * threshold check) when IDLE and under budget. Paced by gc_alloc()/
 * gc_account_extra() via gc_pace() above - every YAS_GC_DEBT_QUANTUM
 * bytes allocated gets one call here, so allocation itself is what
 * advances the collector (see docs/GC.md, "Allocation-triggered
 * stepping"). Also callable directly - (gc-start) exposes yas_gc_start()
 * and (gc)/yas_gc() drain to completion via the loops below, both of
 * which can still be invoked from arbitrary builtin context. */
void yas_gc_step(void)
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

void yas_gc(void)
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

/* ── Constructors ─────────────────────────────────────────────────── */

YasAtom yas_cons(YasAtom car_val, YasAtom cdr_val)
{
	struct Allocation *a = gc_alloc(sizeof(struct Allocation), YAS_GC_OBJ_PAIR);
	YasAtom p;

	p.type = YAS_ATOM_TYPE_PAIR;
	p.value.pair = &a->pair;

	yas_set_car(p, car_val);
	yas_set_cdr(p, cdr_val);

	return p;
}

YasAtom yas_make_int(long x)
{
	YasAtom a;
	a.type = YAS_ATOM_TYPE_INTEGER;
	a.value.integer = x;
	return a;
}

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

YasAtom yas_make_string(const char *s)
{
	YasAtom a;
	size_t len = strlen(s);
	struct YasStringAlloc *sa = gc_alloc(sizeof(struct YasStringAlloc) + len + 1, YAS_GC_OBJ_STRING);
	memcpy(sa->data, s, len + 1);
	a.type = YAS_ATOM_TYPE_STRING;
	a.value.string = sa->data;
	return a;
}

YasAtom yas_make_string_n(const char *s, size_t len)
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

YasAtom yas_make_vector(size_t size, YasAtom fill)
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

YasAtom yas_make_hash(void)
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

	yas_gc_pop_roots(1);
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
	yas_gc_write_barrier(vec, value);
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
			yas_gc_write_barrier(hash, value);
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
	yas_gc_write_barrier(hash, key);
	yas_gc_write_barrier(hash, value);
}

int yas_hash_remove(YasAtom hash, YasAtom key)
{
	struct YasHash *h = hash.value.hash;
	for (size_t i = 0; i < h->size; i++) {
		if (atom_equal(h->keys[i], key)) {
			/* Move last element to this position. Both the moved key
			 * and value were already reachable through this same hash
			 * before the move, so no new white child is introduced -
			 * no write barrier needed here. */
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

YasAtom yas_list_create(int n, ...)
{
	va_list ap;
	YasAtom list = yas_nil;

	/* `list` accumulates across repeated yas_cons() calls below - each
	 * one is a fresh, otherwise-unreachable pair until the *next*
	 * iteration links it in, so root the slot for the whole loop (one
	 * push covers every reassignment - yas_gc_push_root() tracks the
	 * slot's address, not a snapshot). This is not hypothetical: two
	 * items are already enough to reproduce a real use-after-free here
	 * (main.c's own top-level (yas base) import, at process startup
	 * before the heap is large enough for bounded stepping to make this
	 * astronomically unlikely instead of merely unlikely). */
	yas_gc_push_root(&list);
	va_start(ap, n);
	while (n--) {
		YasAtom item = va_arg(ap, YasAtom);
		list = yas_cons(item, list);
	}
	va_end(ap);

	yas_list_reverse(&list);
	yas_gc_pop_roots(1);
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
	yas_set_car(list, value);
}

void yas_list_reverse(YasAtom *list)
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

/* Port operations */
YasAtom yas_make_port(void *handle, const char *path, int is_input, int is_binary, int provider_id)
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

#ifdef YAS_FFI
/* FFIFunction/FFIStructInstance allocation, for ffi.c - these two types
 * are tracked in the same unified GC list as everything else rather than
 * their own global lists (see yas.h). ffi.c fills in the fields; these
 * just get a zeroed, GC-tracked block of the right size. */
struct YasFFIFunction *yas_gc_alloc_ffi_function(void)
{
	return gc_alloc(sizeof(struct YasFFIFunction), YAS_GC_OBJ_FFI_FUNCTION);
}

YasFFIStructInstance *yas_gc_alloc_ffi_struct(void)
{
	return gc_alloc(sizeof(YasFFIStructInstance), YAS_GC_OBJ_FFI_STRUCT);
}
#endif

/* Bytevector operations */
YasAtom yas_make_bytevector(size_t size)
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

YasAtom yas_make_bytevector_from_data(const unsigned char *data, size_t size)
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
	struct YasVecAlloc *va = gc_alloc(sizeof(struct YasVecAlloc), YAS_GC_OBJ_VEC);
	return &va->v;
}

static yas_mat4_t *gc_alloc_mat4(void)
{
	struct YasMat4Alloc *ma = gc_alloc(sizeof(struct YasMat4Alloc), YAS_GC_OBJ_MAT4);
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

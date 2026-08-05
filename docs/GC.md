# GC

yas uses a unified incremental tricolor mark-sweep collector. This
document covers the design; see [ARCHITECTURE.md](ARCHITECTURE.md#gc) for
a short summary and [EMBEDDING.md](EMBEDDING.md#gc) for the embedder-facing
root API.

## One header, one list

Every heap-allocated object type (Pair, Vector, Hash, String, Port,
Bytevector, Vec2/3/4, Mat4, and - under `YAS_FFI` - `YasFFIFunction`/
`YasFFIStructInstance`) embeds a `struct YasGCHeader`:

```c
struct YasGCHeader {
	struct YasGCHeader *next;
	unsigned char color;        /* YasGCColor: WHITE/GRAY/BLACK */
	unsigned char obj_type;     /* YasGCObjType */
	size_t backing_size;        /* bytes owned - struct + any external buffer */
};
```

threaded through a single intrusive list (`gc_all` in `data.c`), replacing
what used to be ten near-identical `mark`/`next` pairs and matching
sweep loops (one per type). `gc_alloc()`/`gc_scan_object()`/
`gc_free_object()` dispatch on `obj_type` instead.

**Header recovery differs by type**, because the `YasAtom` union doesn't
always point at the allocation's start:

- Vector, Hash, Port, Bytevector, and the two FFI types: the atom's
  pointer *is* the allocation's address, so `gc` is the struct's first
  member and a plain cast recovers it.
- Pair (and Closure/Macro/Continuation, which share the same underlying
  allocation - see [ARCHITECTURE.md](ARCHITECTURE.md#value-representation)),
  String, Vec2/3/4, and Mat4: the atom points *into* the allocation (at
  the embedded `YasPair`, or at a trailing/embedded data field), so
  recovery uses `offsetof` against a small wrapper struct (`Allocation`,
  `YasStringAlloc`, `YasVecAlloc`, `YasMat4Alloc`), the same trick the
  original per-type `mark`/`next` headers already used.

`atom_to_header()` in `data.c` centralizes both cases behind one switch.

## Tricolor invariant and the four-step cycle

- **WHITE** - not yet reached this cycle; swept (freed) if still WHITE
  when the sweeper reaches it.
- **GRAY** - reached, queued on the gray worklist, not yet scanned.
- **BLACK** - scanned; its children have themselves been grayed/marked.

A cycle runs `IDLE → MARKING → (atomic re-mark + drain) → SWEEPING →
IDLE`, each step bounded to `YAS_GC_STEP_LIMIT` objects of work
(`yas_gc_step()`):

- **MARKING**: grays roots, then repeatedly pops the gray worklist and
  scans (`gc_scan_object()`), which marks each scanned object's children
  and sets it BLACK.
- **Atomic phase**: once the gray worklist empties, roots are re-marked
  and drained to completion *within the same `yas_gc_step()` call* -
  never persisting across a call boundary, so no allocation can race it.
  This exists because the cycle spans many evaluator safe points, and the
  evaluator's own roots (`expr`/`env`/`stack`, reassigned constantly as
  `eval_vm()` runs) can only be trusted as of *this* snapshot, not the one
  at cycle start. Because it never persists, `yas_gc_phase()` only ever
  reports IDLE/MARKING/SWEEPING to callers - there is no externally
  observable "atomic" phase.
- **SWEEPING**: walks the entire `gc_all` list once per cycle, freeing
  WHITE objects and leaving everything else untouched (see "survivor
  colors" below).

### Why steps run at the evaluator safe point, not inside `gc_alloc()`

do2 (the sibling project this collector's shape is ported from) steps the
GC directly inside its allocator. Doing that here would mean a collection
could complete in the middle of *any* builtin - and roughly twenty
`builtin_*.c` files hold `YasAtom` C locals across further allocating
calls without being rooted (that's the normal, expected style for a
builtin that isn't itself re-entering the evaluator). Retrofitting
`yas_gc_push_root()` through every builtin is a much larger audit and
explicitly out of scope for this port.

Instead, `yas_gc_step()` is called once per `eval_vm()` loop iteration
(`eval.c`) - the same place the old counter-triggered stop-the-world
collector used to fire, except now every iteration takes one bounded
slice instead of waiting for a 100,000-cons counter and then doing the
whole mark+sweep in one burst. `eval_roots` is valid at that point and no
builtin call is "in flight" (a builtin either returns without further
evaluation, or itself calls back into `yas_eval_expr()`, which pushes its
own `EvalRoot` and gets its own safe points).

This does **not** fully close the "unrooted local across an allocating
call" hazard: a builtin that calls `yas_eval_expr()` recursively (a real
and existing pattern - see `process_import_set()` in `module.c`) enters a
nested `eval_vm()` that *does* step the GC, while the *outer* builtin's
own `YasAtom` C locals remain unrooted. This is not a regression - the old
counter-based collector had the identical exposure, just far less likely
to trigger given its coarser, rarer firing. Fixing it is a separate,
larger audit (see "Future work" below); the specific instance that
surfaced during this port (`yas_library_load()`'s own lookup key, held
across nested library loads) is fixed - see "A worked example" below.

## Survivor colors and the allocate-during-sweep hazard

An early version of this collector reset each surviving (non-WHITE)
object back to WHITE the moment the sweeper visited and kept it, mirroring
how many simpler mark-sweep collectors work. That's unsound here: the
write barrier (below) can still fire *after* the sweeper has already
passed an object's position in the list, re-graying its container and
(when that gray gets drained) marking the already-visited object BLACK
again - a color the sweeper will never revisit this cycle, since its
cursor has moved on. That object then carries BLACK into the *next*
cycle, where `yas_gc_mark()`'s `color != WHITE` check makes it a silent
no-op: the object is never scanned, so anything reachable only through it
is missed and incorrectly swept.

The fix: **survivors are left BLACK, not reset to WHITE, during sweep.**
Colors are bulk-cleared to WHITE only once, in `yas_gc_start()`, before
marking begins for the *next* cycle - a single linear pass over `gc_all`
(a byte write per live object, no allocation or freeing). This means:

- A WHITE object encountered anywhere before the next cycle's clear is
  unambiguously "unreached so far this cycle" - regardless of whether the
  sweep cursor has passed its list position - which is exactly what the
  write barrier and `gc_alloc()` need to reason about correctly.
- New objects are always inserted at the head of `gc_all` regardless of
  the current phase - there's no need for phase-aware insertion, because
  the bulk clear picks up every object equally at the next cycle's start
  regardless of where it sits in the list or when it was allocated.

This is a deliberate simplicity/perf trade-off against the alternating-
white-color bookkeeping production collectors (Lua, do2) use to avoid the
O(n) clear pass. Worth revisiting if profiling shows it matters.

## Write barrier

New objects allocated mid-cycle are colored BLACK (`gc_phase != IDLE`) so
a caller who hasn't rooted them yet doesn't lose them to the current
sweep - but that only holds if every reference a constructor stores into
a fresh BLACK object is itself protected. `yas_gc_write_barrier(container,
child)` does this: if `container` is BLACK and `child` is WHITE, it
re-grays `container` (pushes it back onto the gray worklist), so a later
scan re-discovers `child` and marks it non-white.

Call sites:

- `yas_set_car()`/`yas_set_cdr()` - the barriered counterparts to the
  `yas_car`/`yas_cdr` macros. Any code that *assigns* through car/cdr
  (rather than only reading) must use these; the macros stay read-only.
  `yas_cons()` uses them for its own initial car/cdr store, since a
  freshly-allocated BLACK pair has exactly the same hazard as one mutated
  later.
- `yas_vector_set()`, `yas_hash_set()` (both the update and insert paths).
  `yas_hash_remove()`'s move-last-element-into-the-gap reshuffle does
  *not* need a barrier: both relocated values were already reachable
  through the same hash before the move.
- `set-metatable!` (`builtin_hash.c`) - stores a raw `struct YasHash *`
  rather than a `YasAtom` field, so it calls `yas_gc_write_barrier()`
  directly instead of going through one of the helpers above.
- `yas_make_vector()`'s fill value (one barrier call covers every slot,
  since it's the same atom repeated).

**The barrier still matters during SWEEPING, not just MARKING.** Live
code keeps running between `yas_gc_step()` calls, and can legitimately
store a reference to an object this cycle's marking never reached (still
WHITE - e.g. resurrecting a value that was unreachable at the atomic
snapshot via a fresh binding) into an already-BLACK container. Once
MARKING is over, nothing else will ever pop and scan a re-grayed
container - so `yas_gc_step()`'s SWEEPING case explicitly drains any
pending gray entries (`gc_mark_step()` with a large limit) *before*
continuing the bounded sweep, ensuring a rescued object is marked
non-white before the sweep cursor can reach and free it.

## Root-registration API

`EvalRoot`/`eval_roots` (`eval.c`) remain exactly as they were - they root
pointers to `eval_vm()`'s *own* C locals (`expr`, `env`, and the
continuation-passing `stack`), which a caller of `yas_eval_expr()` can't
see or reuse. That's a separate root source from the shadow stack below;
`gc_mark_roots()` (`data.c`) walks both.

```c
void yas_gc_push_root(YasAtom *slot);   /* register &slot as a root */
void yas_gc_pop_roots(int n);           /* pop the last n roots (LIFO) */
int  yas_gc_root_count(void);           /* for push/pop balance asserts */
```

Slot-based, not value-copy: a root is the *address* of a `YasAtom`
variable, not a snapshot of its value, so it keeps protecting whatever
that variable currently holds even if the caller reassigns it. Use this
whenever a native builtin, an embedder, or (in the future) AOT-generated
code holds a `YasAtom` local across any call that might allocate.

```c
YasAtom slot = yas_make_string("...");
yas_gc_push_root(&slot);
/* ... calls that might allocate/step the GC ... */
yas_gc_pop_roots(1);
```

Two other entry points exist mainly for tests and tools that need to
observe or force a cycle rather than wait for the byte threshold:

```c
void yas_gc_start(void);   /* force IDLE -> MARKING, mark roots, don't drain */
void yas_gc(void);         /* run a full cycle to completion */
void yas_gc_step(void);    /* one bounded slice - what eval_vm() calls */
int    yas_gc_phase(void);      /* 0 idle, 1 marking, 2 sweeping */
size_t yas_gc_allocated(void);  /* live bytes tracked */
```

Exposed to Lisp as `(gc)`, `(gc-start)`, and `(gc-stats)` (returns a hash
with `:ALLOCATED`/`:PHASE`) - see `docs/LANGUAGE_REFERENCE.md`.

**`yas_gc_mark()`'s contract**: it grays and enqueues an object - it is
*not* "mark this subgraph right now". Outside an active cycle (IDLE) it's
a no-op. This is a behavior change from the old stop-the-world collector,
where `yas_gc_mark()` recursively traced the whole subgraph immediately;
any embedder calling it directly (rather than through the root API above)
should account for the new, lazier contract.

### A worked example: the bug this API exists to prevent

While porting this collector, `yas_library_load()` (`module.c`) turned up
a real instance of exactly the hazard #4 describes. It computes a lookup
key:

```c
YasAtom key = yas_make_string(key_str);
YasAtom lib = yas_hash_ref(yas_library_registry, key, yas_nil);
```

then, for a pure-Lisp library, evaluates the library's source (which may
itself load further nested libraries - e.g. `(yas test)`'s own
`define-library` imports `(yas math)`, `(yas vector)`, `(yas text)`) before
using `key` *again* to re-fetch the now-populated entry. `key` was never
stored anywhere - purely a local lookup value - so with the incremental
collector's much lower, byte-based threshold, it was possible for a GC
cycle to run to completion during the nested load, freeing `key`'s
string. A later allocation (another library's name string) then reused
that exact memory address, and the post-load re-lookup compared `key`
against the registry using bytes that now spelled a *different* library's
name - silently returning the wrong library instead of erroring. The fix
is a single `yas_gc_push_root(&key)` for the function's duration. `main.c`
has two similar top-level cases (the REPL/CLI's top-level environment and
its initial `(import (yas base))` argument list), both now rooted the same
way.

### Callers not affected

Not every `YasAtom` held across an evaluating call needs a root - only
ones with no *other* reachability path. Reachability generally comes from
one of:

- The active `eval_vm()`'s own `expr`/`env`/`stack` (protected via
  `EvalRoot` automatically, as long as the C variable in question is
  still whatever those point to, or reachable transitively from it, at
  the moment a GC step runs).
- Already-anchored global state - `yas_library_registry`,
  `yas_global_search_paths`, `yas_global_features`, `sym_table`/
  `kw_table` (interning tables).

For example, a freshly created library environment (`lib_env`) is safe to
use across nested evaluation immediately after `yas_hash_set(registry,
key, lib)` registers its owning library, because from that point on it's
reachable via `yas_library_registry → lib → lib_env`. It's only values
with *no* such path - like the lookup key above - that need an explicit
root.

## FFI exclusions

`YasFFIFunction` and `YasFFIStructInstance` are GC-tracked like everything
else. `FFILibrary` (a `dlopen` handle) and `FFIPointer` (a raw `void*`)
deliberately are **not**: ownership of a bare pointer can't be inferred
(it may be a `dlsym` result, an interior pointer into a struct, or a
genuinely owned `ffi-malloc` block), and closing a library when its atom
becomes unreachable could invalidate a still-live `YasFFIFunction`
extracted from it (it holds no back-reference to its library). Both stay
explicitly managed via `ffi-close`/`ffi-free`.

## Testing

- `tests/gc_test.c` (`make test-gc`) - C-level: root protection actually
  survives real collection, push/pop balance, the write barrier firing
  during MARKING and during SWEEPING specifically, and a
  million-element list to confirm `yas_gc_mark()`'s non-recursive marker
  doesn't overflow the C stack the way the old recursive one could.
- `aux/gc_continuation_stress.ys` - a continuation reachable only via a
  binding (not the live evaluator stack) survives GC, exercising that
  `YAS_ATOM_TYPE_CONTINUATION` is traced the same as Pair/Closure/Macro
  (they share the same underlying allocation).
- `aux/gc_incremental_stress.ys` - ordinary interpreted code (closures,
  strings, vectors, hashes, lists) held live across heavy allocation
  churn, exercising many real incremental cycles under normal safe-point
  stepping rather than a single controlled phase transition.

## Future work

- **Root builtin locals held across nested `yas_eval_expr()`.** As noted
  above, a builtin that itself calls back into the evaluator (`module.c`'s
  import/library-loading machinery is the known example) can have a GC
  step fire mid-call while its own C locals are unrooted. Not a
  regression from this port, but the root API now exists to fix it
  properly - needs an audit of `yas_eval_expr()` callers from builtin
  context.
- **Allocation-triggered incrementality.** Steps currently only run at
  `eval_vm()`'s safe point; moving them into `gc_alloc()` itself (as do2
  does) would give tighter pause bounds, but depends on the audit above
  landing first - otherwise a step could fire inside any builtin's
  unrooted-local window.
- **Avoid the O(n) per-cycle color-clear** in `yas_gc_start()` via an
  alternating-white-color scheme, if profiling shows it matters.

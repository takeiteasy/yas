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
  This exists because the cycle spans many separate `yas_gc_step()` calls,
  and the evaluator's own roots (`expr`/`env`/`stack`, reassigned
  constantly as `eval_vm()` runs) can only be trusted as of *this*
  snapshot, not the one at cycle start. Because it never persists, `yas_gc_phase()` only ever
  reports IDLE/MARKING/SWEEPING to callers - there is no externally
  observable "atomic" phase.
- **SWEEPING**: walks the entire `gc_all` list once per cycle, freeing
  WHITE objects and leaving everything else untouched (see "survivor
  colors" below).

### Allocation-triggered stepping

`gc_alloc()` (`data.c`) paces `yas_gc_step()` itself - allocation is what
advances the collector, not `eval_vm()`'s loop. do2 (the sibling project
this collector's shape is ported from) does the same. An earlier version
of this port stepped only once per `eval_vm()` loop iteration instead,
specifically to avoid the audit this section describes; that tradeoff is
gone now that the audit has been done (see "The rooting contract" below).

**Pacing**: `gc_pace()` (`data.c`) accumulates a byte counter (`gc_debt`,
fed by both `gc_alloc()` and `gc_account_extra()` - the latter matters
because vector/hash/bytevector growth can be the dominant source of
allocated bytes without a single `gc_alloc()` call to hang a step off of)
and runs one bounded `yas_gc_step()` every `YAS_GC_DEBT_QUANTUM` bytes
(default 4KB, overridable via that env var). Pacing arithmetic: a cons
cell (`struct Allocation` = `YasGCHeader` + `YasPair`) is 56 bytes on a
typical LP64 build, so one quantum is roughly 73 conses; `YAS_GC_STEP_LIMIT`
(512 objects marked per step) is ~7x that - comfortably enough for
marking to outpace allocation.

`gc_pace()` is called from *inside* `gc_alloc()`, before the `calloc` -
not after. The object being allocated doesn't exist yet at that point, so
a step (even one that completes a whole cycle) cannot possibly free the
very object this call is about to create; the risk that ordering avoids
is real, not theoretical - pacing *after* linking and coloring the new
object was tried and produces an immediate use-after-free on the object
under construction, once the collector has already been told a cycle is
active (see the "Testing" section's note on `YAS_GC_STRESS`, which is
exactly when this matters). A `gc_stepping` re-entrancy guard prevents a
step's own internals from re-triggering `gc_pace()` - today nothing a
step does (`gc_mark_step`/`gc_sweep_step` use plain `realloc`/`free`,
never `gc_alloc()`) can hit that, so it's insurance against a future
change rather than a live fix.

`eval_vm()`'s loop no longer calls `yas_gc_step()` at all; `EvalRoot`
(`expr`/`env`/`stack`/`result`) is still a root source, just no longer
the pacing site.

#### The rooting contract

Since a collection can now complete during *any* allocating call, not
just at `eval_vm()`'s old safe point, the rooting contract is broader
than it used to be: **any C function - builtin, reader, evaluator
internals - that holds a `YasAtom` local across a further allocating call
needs to root it**, unless that value is already reachable another way
(see "Callers not affected" below). This was audited across `data.c`,
`read.c`, `eval.c`, `module.c`, and the `builtin_*.c` files; the two
concrete shapes found repeatedly:

- **Accumulator hazard** - a loop building a list/string by holding a
  head/tail pair across repeated allocating calls (`yas_list_create()`,
  `yas_copy_list()`, `read_list()`, `yas_builtin_list_append()`'s
  `append`, `yas_define_native_module()`'s name-parsing loop, and others)
  - fixed by rooting the head for the loop's duration; the tail needs no
  separate root once linked in, since it's reachable by walking cdrs from
  the (rooted) head.
- **Nested-temporary hazard** - `f(alloc_a(), alloc_b())`: whichever of
  `alloc_a()`/`alloc_b()` runs first produces an unrooted C temporary
  that survives only as an argument-evaluation-order artifact until `f`
  itself is called, and `f`'s own `gc_alloc()` can step the GC before
  linking that temporary in (`yas_env_define()`'s binding cons,
  `yas_library_load()`'s default search-path construction, `read.c`'s
  quote-wrapper construction and string-interpolation parts list, and
  others) - fixed by naming and rooting each intermediate value instead
  of nesting the expressions.

A narrower, non-obvious third case: a constructor's *own* freshly
allocated object can need rooting internally, if the object is created
while the collector is IDLE (colored WHITE) and a *later* allocation in
the same function is what starts the next cycle - for a small enough live
heap, a single bounded step can complete an entire mark+sweep, so the
object is swept before its own constructor ever returns it anywhere.
`yas_make_vector()`/`yas_make_hash()`/`yas_make_bytevector*()` all hit
this between their initial `gc_alloc()` and the `gc_account_extra()` call
that grows their external buffer, and root themselves across that window.

One bound that keeps the contract usable rather than infinite: a
builtin's `args` (and everything reachable through them) arrive already
rooted via the evaluator's own call-frame stack (`eval_do_apply()`,
`eval.c`, reachable through `EvalRoot->stack`) - only values a function
*creates itself* and then holds across another allocation need explicit
rooting. See "Callers not affected" below for the general version of this
reasoning.

A closely related, pre-existing bug class surfaced by the same audit:
raw `yas_cdr(p) = x` / `*result = x` assignments that write into
GC-tracked pair storage without going through `yas_set_car`/`yas_set_cdr`
or an explicit `yas_gc_write_barrier()` call bypass the write barrier
entirely (see "Write barrier" below). `read_list()` had two such
assignments building its result list; `read_expr_internal()`'s
quote/quasiquote/unquote branches write through `&yas_car(yas_cdr(*result))`
- a raw pointer that aliases into an existing pair's car slot - which is
exactly the same hazard by a different route. Both are fixed. These were
dormant before this change (a collection could not previously land inside
a single reader call), not something allocation-triggered stepping
introduced.

`(gc)`/`(gc-start)` (`yas_gc()`/`yas_gc_start()`, exposed as builtins in
`builtin_core.c`) can still force a collection from *builtin* context at
any call depth - true before this change and still true now, just no
longer the only way a collection can start mid-builtin.

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
- `read_expr_internal()`'s quote/quasiquote/unquote branches (`read.c`) -
  the recursive parse writes its result through
  `&yas_car(yas_cdr(*result))`, a raw pointer into the freshly-allocated
  wrapper pair's car slot rather than through `yas_set_car()`, so the
  quote branch calls `yas_gc_write_barrier()` explicitly once the
  recursive read succeeds. This is the general shape to watch for beyond
  the helpers above: any raw pointer write through a caller-supplied
  `YasAtom*` that happens to alias into existing pair storage bypasses
  the barrier exactly like a raw `yas_cdr(p) = x` assignment would (see
  "Allocation-triggered stepping" above - `read_list()` had two such
  bypasses, now converted to `yas_set_cdr()`).

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

`EvalRoot`/`eval_roots` (`eval.c`) root pointers to `eval_vm()`'s *own* C
locals - `expr`, `env`, the continuation-passing `stack`, and its output
slot `result` - which a caller of `yas_eval_expr()` can't see or reuse.
That's a separate root source from the shadow stack below;
`gc_mark_roots()` (`data.c`) walks both. Rooting `result` closes a whole
class of hazard by itself: any re-entering caller that holds the
*previous* nested `yas_eval_expr()` call's output live across the *next*
one (`TRY`'s try-block result staying live across `FINALLY`, a source
loop's last-form value staying live across the next form) is protected
automatically, with no per-call-site `push_root`/`pop_roots` needed. It
must be nulled to `yas_nil` before this root is installed, since callers
pass uninitialized storage.

```c
void yas_gc_push_root(YasAtom *slot);   /* register &slot as a root */
void yas_gc_pop_roots(int n);           /* pop the last n roots (LIFO) */
int  yas_gc_root_count(void);           /* for push/pop balance asserts */
```

`yas_eval_expr()` asserts `yas_gc_root_count()` is unchanged across its
own call, catching any future unbalanced `push_root`/`pop_roots` pair as
a test failure. This is safe to rely on: there is no `setjmp`/`longjmp`
in the tree, and `call/cc` is implemented on the VM's own explicit stack,
so every unwind (including a thrown exception) is a plain C `return` -
push/pop balance is always achievable.

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

The rest of the audit this motivated turned up the same shape in a few
more places, all fixed with the same pattern:

- `yas_eval_source()`'s own `env` parameter and `result` output slot
  (`eval.c`) - for every `module.c` caller, `env` is a bare
  `yas_env_create(yas_nil)` temporary that nothing else references, and
  the loop's `*result` holds the previous top-level form's value live
  across the next form's evaluation. This is the highest-frequency
  instance: it runs on every library load.
- The `TRY`/`CATCH` clause's bound-exception environment (`catch_env`,
  `eval_vm()` in `eval.c`) - a fresh `yas_env_create(env)` with nothing
  pointing back at it, rooted for the duration of the predicate check and
  the catch body, popped on every exit from the clause (matched, not
  matched, or a predicate error).
- `yas_library_load()`'s pure-Lisp-library branch (`module.c`) - unlike
  the file-load branch, `lib` isn't inserted into `yas_library_registry`
  before evaluating the library's source, so it's unreachable across that
  eval until re-fetched afterward.

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

- `aux/gc_test.c` (`make test-gc`) - C-level: root protection actually
  survives real collection, push/pop balance, the write barrier firing
  during MARKING and during SWEEPING specifically, a million-element list
  to confirm `yas_gc_mark()`'s non-recursive marker doesn't overflow the
  C stack the way the old recursive one could, `yas_eval_source()`'s
  fresh-`env` rooting (a source string that imports `(yas base)`, forces
  a collection with `(gc)`, then `define`s into that same env - only
  reachable from C, since the vulnerable env only ever exists on the
  `yas_library_load()` path), and three cases specific to allocation-
  triggered stepping: a rooted value surviving `gc_alloc()`'s own
  automatic pacing with no explicit `yas_gc()`/`yas_gc_step()` call at
  all, `yas_gc_allocated()` staying bounded across a long churn loop
  (pacing actually keeping up with allocation, not just eventually
  running), and heavy allocation while a cycle is already active still
  reaching IDLE (the `gc_stepping` re-entrancy guard not stalling
  progress).
- `suite.ys` ("GC rooting across nested eval") - Lisp-level: a `TRY`
  whose `FINALLY` clause calls `(gc)`, checking the try-block's result
  survives; a `CATCH` whose first body form recurses deeply enough to
  cross the byte threshold and drive a full automatic cycle to
  completion while `catch_env` is the *only* thing keeping the bound
  exception variable reachable. Both allocate the object under test
  while the collector is IDLE and force the cycle *afterward* -
  `(gc-start)` immediately followed by allocation would not catch a
  regression here, since `gc_alloc()` colors new objects BLACK (and
  therefore survives) while a cycle is already active.
- `suite.ys` ("allocation-triggered stepping mid-builtin") - a single
  `append` call spanning several thousand conses, entirely inside one
  builtin C call with no return to `eval_vm()`'s loop in between and no
  explicit `(gc)`/`(gc-start)` - exercises the mid-builtin collection
  window that could never occur before allocation-triggered stepping.
- `aux/gc_continuation_stress.ys` - a continuation reachable only via a
  binding (not the live evaluator stack) survives GC, exercising that
  `YAS_ATOM_TYPE_CONTINUATION` is traced the same as Pair/Closure/Macro
  (they share the same underlying allocation).
- `aux/gc_incremental_stress.ys` - ordinary interpreted code (closures,
  strings, vectors, hashes, lists) held live across heavy allocation
  churn, exercising many real incremental cycles under normal safe-point
  stepping rather than a single controlled phase transition.
- `aux/gc_alloc_stress.ys` + `make test-gc-stress` - the audit gate for
  allocation-triggered stepping itself. `YAS_GC_STRESS=<n>` (`data.c`)
  shrinks both the cycle-start threshold and the debt quantum to `n`
  bytes via `gc_min_threshold_get()`/`gc_debt_quantum_get()` (env
  overridable individually as `YAS_GC_MIN_THRESHOLD`/
  `YAS_GC_DEBT_QUANTUM`), instead of the ~256KB/4KB defaults - so cycles
  start and advance on essentially every allocation, using the exact same
  pacing code path production does. `make test-gc-stress` runs
  `suite.ys` + every `aux/*.ys` script + `aux/gc_test.c` at `n=1`; add
  `YAS_ASAN=1` so a surviving bug reports a stack trace naming the exact
  function.

  Two other designs were tried first and rejected - worth knowing if
  revisiting this: running a *full* `yas_gc()` to completion on every
  Nth allocation always ends back at IDLE, which colors every subsequent
  object WHITE and defeats allocate-black, manufacturing use-after-frees
  for values that were never actually unsafe (an object only needs to
  survive to the next *cycle boundary*, not to the next allocation).
  Forcing `yas_gc_start()` (bypassing the threshold) on every single
  allocation preserves that invariant but is O(heap) per allocation
  whenever the heap is small enough for one bounded step to complete an
  entire cycle - which is most of the time for a small/medium heap
  (measured: a ~170KB library bootstrap didn't finish in 2 minutes).
  Shrinking the threshold instead reuses the ordinary, already-correct
  pacing path, so it's neither unsound nor quadratic.

## Future work

- **Avoid the O(n) per-cycle color-clear** in `yas_gc_start()` via an
  alternating-white-color scheme, if profiling shows it matters.

# Benchmarking

`aux/` holds slow stress/benchmark/fuzz scripts that don't belong in
`suite.ys`/`make test` (see [BUILDING.md](BUILDING.md#directory-layout)).
This document covers the benchmark half of that convention.

## Naming convention

| pattern | kind | run via |
|---|---|---|
| `aux/*_stress.ys` | slow stress/regression scripts | `make stress-test`, `make test-gc-stress` |
| `aux/bench_*.ys` | self-timed in-language micro-benchmarks | `make bench` |
| `aux/bench/` | cross-language benchmark harness + workloads | `make bench` |
| `aux/fuzz_*.ys` | **reserved** - no fuzzing harness exists yet | - |
| `aux/*.c` | C-level tests | `make test-gc` |

`make bench` is informational, not a regression gate: it isn't part of
`make check`, and only fails if a *present* runtime produces the wrong
output (a bug, not a timing regression).

## Cross-language harness (`aux/bench/`)

```sh
make bench                                          # everything
python3 aux/bench/bench.py                          # same, directly
python3 aux/bench/bench.py --workload fib --runs 5  # one workload, best-of-5
```

Each workload (`aux/bench/workloads/<name>/<name>.<ext>`) is implemented
once per language with identical parameters and a stdout the driver
(`bench.py`) knows in advance - a broken variant can't post a fake fast
time by silently computing the wrong answer. A runtime that isn't
installed (`ruby`, `lua`, ...) is reported `skipped` and does not
fail the run; only a *present* runtime producing the wrong output does.

Seven workloads today:

- **`fib`** - naive recursive `fib(24)`, call/arithmetic-heavy.
- **`alloc`** - build and sum a 50000-element linked list, allocation/GC-
  pressure-heavy (a plain array in the non-yas variants would test
  something different, so each variant builds an actual linked
  list/cons-cell chain).
- **`string`** - build a string via 6000 rounds of `string-append` +
  `number->string`, checksummed via `substring`/`string=?` on each small
  per-round chunk (never the growing accumulator - see the "UTF-8 string
  ops are O(n)" note below).
- **`hash`** - insert 6000 integer keys, remove every odd one, sum every
  key's value back out by lookup; the checksum combines that sum with the
  final table size so both the insert/remove and lookup paths matter.
- **`loop`** - a small (800-deep) non-tail recursive sum next to a much
  larger (300000-deep) tail-recursive one, to see whether TCO (or its
  absence) shows up in the timing.
- **`closure`** - `map`/`filter`/`foldl` over a 40000-element list, each
  step calling through a closure and allocating a fresh list.
- **`array`** - sieve of Eratosthenes over a 3000-element vector,
  `vector-ref`/`vector-set!` indexed access rather than cons-cell or hash
  churn.

Each row also gets a `python3`, `ruby`, `lua` (interpreted) and `c`
(native, `cc -O2`) variant, plus a `startup` row measuring empty-program
interpreter startup cost in isolation - printed as its own column, never
subtracted from a workload's total (subtracting two noisy wall-clock
measurements would invent precision, not add it).

**`string`/`hash`/`array` need an explicit `(import ...)`**, unlike
`fib`/`alloc`/`loop`/`closure`. `main.c` only auto-imports `(yas base)`
into every top-level environment (see `CLAUDE.md`) - `(yas text)`,
`(yas hash)`, and `(yas vector)` are separate native modules, so those
three workloads' numbers also include evaluating the relevant slice of
`std/yas/text.ys`/`hash.ys`/`vector.ys` at import time, the same caveat
`(yas base)` gets below. That's still the representative case for real
`./yas` usage of those features, just not a raw-C-primitive number.

**`string-ref`/`string-length` are UTF-8 and walk the string, O(n) per
call** (`yas_utf8_codepoint_at`/`yas_utf8_strlen` in `src/utf8.c`), so an
indexed per-character loop over a long string is O(n²) in yas but O(n) in
the other languages - that would measure the gap between those two
complexities, not string-op cost. `string.ys` avoids this by only ever
calling `substring`/`string=?` on the small per-round chunk, never the
growing accumulator.

**The `yas` under test is the default optimized build** (`-O2 -g`, see
[BUILDING.md](BUILDING.md) - about 2.2x faster than the old `-O0` default
for `fib(24)`, for no source change). To compare optimization levels,
rebuild between runs: `make clean && make OPT=<level> && make bench` -
there's no opt-level sweep inside `bench.py` itself.

**The yas numbers include `(yas base)`.** `main.c` imports `(yas base)`
into every top-level environment unconditionally (see `CLAUDE.md`), so
even though the workload `.ys` files have no explicit `(import ...)`,
`+ - * /` there are still `(yas base)`'s variadic closures over the C
primitives, not the primitives directly. That's deliberate: ordinary
`./yas` usage always has base loaded, so this is the representative
number, not a raw-VM one.

## Results

Sample `make bench` output, one machine (macOS/arm64), default `-O2`
build, best-of-3. Not a committed regression baseline - re-run `make
bench` for current numbers on your own hardware; this is a snapshot to
give a sense of scale, not a number to diff against.

**TL;DR:** yas is roughly 2 orders of magnitude slower than Python/Ruby/
Lua on every workload here, and roughly 3 orders of magnitude slower than
native C - expected for a tree-walking interpreter with no JIT (the
default `-O2` build this doc's numbers already reflect is the biggest
single lever found so far - see [BUILDING.md](BUILDING.md)). `array`
(the sieve, dominated by `while`/`dotimes` macro-expansion overhead per
iteration) and `loop`'s tail-recursive leg are the widest gaps to the
other interpreted languages; `alloc` and `fib` are the narrowest.

The `yas` column below has three sub-columns tracking two follow-up
optimizations to the GC/allocation machinery that `sample` profiling
showed dominating every workload here (see tickets #40/#44): **base**
(before either change), **+#40** (control-stack frames collapsed from
6 cons cells per push to 1 GC object, `src/eval.c`/`struct YasFrame`),
and **+#44** (the free-list pool on top of that, `gc_alloc()`/
`gc_sweep_step()` in `src/data.c`). #40 is the dominant lever (~45-50%
wall-time reduction on its own, since it cuts allocations-per-push
6-to-1); #44's *additional* win on top of #40 is smaller than it would
have measured in isolation (~10-15% further on most workloads, roughly
noise on `alloc`/`closure`) precisely because #40 already removed five
of every six frame allocations #44 would otherwise be making cheaper -
expected, not a sign #44 underperforms.

| workload | yas (base) | yas (+#40) | yas (+#44) | python3 | ruby | lua | c |
|---|---|---|---|---|---|---|---|
| `fib` (n=24) | 1.174s | 0.612s | 0.531s | 0.017s | 0.057s | 0.006s | 0.002s |
| `alloc` (n=50000) | 0.728s | 0.417s | 0.393s | 0.018s | 0.059s | 0.008s | 0.003s |
| `string` (n=6000) | 2.271s | 1.203s | 1.012s | 0.015s | 0.073s | 0.010s | 0.011s |
| `hash` (n=6000) | 2.728s | 1.423s | 1.270s | 0.012s | 0.053s | 0.003s | 0.002s |
| `loop` (800 / 300000) | 3.290s | 1.675s | 1.461s | 0.024s | 0.062s | 0.006s | 0.002s |
| `closure` (n=40000) | 2.365s | 1.327s | 1.396s | 0.019s | 0.055s | 0.006s | 0.002s |
| `array` (n=3000) | 3.585s | 1.875s | 1.654s | 0.013s | 0.060s | 0.004s | 0.002s |
| `startup` | 0.004s | 0.003s | 0.003s | 0.011s | 0.052s | 0.003s | - |

`closure`'s `+#44` column landing slightly above `+#40` is within the
noise of a single best-of-3 run, not a regression - the pool changes
nothing about closure/env allocation shape, only where the backing bytes
come from.

**Ticket #58 note**: faithful (snapshot-per-invocation) continuations add
one cons per argument to every application - `eval_do_apply()` now
reverses a copy of the evaluated-args list instead of rewiring the shared
spine in place, so continuation snapshots can't be scrambled mid-spine -
and make capture/invoke O(live stack depth) in frame allocations. The
table's workloads use no continuations and their arg lists are short; no
column above has been re-measured for this change.

## Self-timed scripts (`aux/bench_*.ys`)

For benchmarks that don't need cross-language comparison, use
`(current-time)` (docs/LANGUAGE_REFERENCE.md) directly in a `bench_*.ys`
script - see `aux/bench_alloc.ys`. `make bench` runs every
`aux/bench_*.ys` after the cross-language harness.

## Fuzzing (reserved, not implemented)

`aux/fuzz_*.ys` is reserved for future reader/evaluator fuzzing (random or
property-based input generation against `read_expr()`/the evaluator - see
`src/read.c`/`src/eval.c`, which have no fuzz coverage today). Deciding a
randomness source and corpus/seed strategy is tracked separately; nothing
under that name exists yet.

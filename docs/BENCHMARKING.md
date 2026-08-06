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
python3 aux/bench/bench.py --no-aot                 # skip the AOT row
```

Each workload (`aux/bench/workloads/<name>/<name>.<ext>`) is implemented
once per language with identical parameters and a stdout the driver
(`bench.py`) knows in advance - a broken variant can't post a fake fast
time by silently computing the wrong answer, the same idea as `make
test-aot` checking `build/fib`'s exact output. A runtime that isn't
installed (`ruby`, `lua`, `cccc`, ...) is reported `skipped` and does not
fail the run; only a *present* runtime producing the wrong output does.

Two workloads today:

- **`fib`** - naive recursive `fib(24)`, call/arithmetic-heavy.
- **`alloc`** - build and sum a 50000-element linked list, allocation/GC-
  pressure-heavy (a plain array in the non-yas variants would test
  something different, so each variant builds an actual linked
  list/cons-cell chain).

Each row also gets a `python3`, `ruby`, `lua` (interpreted) and `c`
(native, `cc -O2`) variant, plus a `startup` row measuring empty-program
interpreter startup cost in isolation - printed as its own column, never
subtracted from a workload's total (subtracting two noisy wall-clock
measurements would invent precision, not add it).

**The `yas` under test is the default optimized build** (`-O2 -g`, see
[BUILDING.md](BUILDING.md) - ticket #38 found this ~2.2x faster than the
old `-O0` default for `fib(24)`, for no source change). To compare
optimization levels, rebuild between runs: `make clean && make OPT=<level>
&& make bench` - there's no opt-level sweep inside `bench.py` itself.

**The yas numbers include `(yas base)`.** `main.c` imports `(yas base)`
into every top-level environment unconditionally (interpreter and REPL,
not the separate AOT front end - see `CLAUDE.md`), so even though the
workload `.ys` files have no explicit `(import ...)`, `+ - * /` there are
still `(yas base)`'s variadic closures over the C primitives, not the
primitives directly. That's deliberate: ordinary `./yas` usage always has
base loaded, so this is the representative number, not a raw-VM one.

### AOT row (placeholder)

The `fib (n=30, AOT)` row is a stopgap, not a finished comparison: the AOT
pipeline only handles the subset of yas in `aot/examples/fib.ys`
(`fib(30)`, no imports), and this repo's `make aot` is hardwired to that
example. The row shells out to the existing `make aot` and times its
output against a native-C `fib(30)` - a different input from the `fib`
workload's `fib(24)` above, so the two are not comparable to each other,
only "yas AOT" vs "native C" at n=30 is. A follow-up ticket tracks
replacing this once the AOT pipeline covers more of the language.

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

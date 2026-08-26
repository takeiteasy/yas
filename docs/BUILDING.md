# Building

## Interpreter

```sh
make            # builds ./yas, optimized (-O2 -g) by default
make OPT=-O3    # build at a different optimization level
make YAS_DEBUG=1 # build unoptimized (-O0 -g) for stepping through a crash
make test       # runs suite.ys, gates on its process exit code
make test-gc    # C-level GC regression tests (aux/gc_test.c) - see GC.md
make stress-test    # runs the slow stress/regression scripts in aux/*_stress.ys
make test-gc-stress # allocation-triggered GC stepping audit gate - see GC.md
make bench          # cross-language + self-timed benchmarks - see BENCHMARKING.md
make fuzz           # reader/evaluator fuzzing - see FUZZING.md
make clean
```

Requires a C99 compiler and `libreadline`. Sources are in `src/`;
`src/stdlib.h` is generated from `std/yas/*.ys` by `embed.py` and is not
tracked in git.

The default build is optimized (`OPT ?= -O2` in the Makefile, `-g` kept
for usable stack traces) - `fib(24)` interpreted runs about 2.2x faster
at `-O2` than at `-O0`, for no source change (see
[BENCHMARKING.md](BENCHMARKING.md)). `-O3` was A/B'd against `-O2` and
showed no further improvement on this benchmark, so `-O2` is the default;
override with `make OPT=-O3` (or any other level) to try it yourself.
`make YAS_DEBUG=1` forces `-O0 -g` back, for contributors who want
unoptimized stack traces while debugging a crash. **Run `make clean`
whenever `OPT`, `YAS_DEBUG`, `YAS_ASAN`, or `YAS_FFI` changes between
builds** - the object files only depend on headers, not on these flags,
so a build with a flag flipped but no `make clean` first will silently
relink stale `.o` files instead of rebuilding with the new flags.

`aux/` holds slow interpreted stress/benchmark/fuzz scripts that aren't
part of the `make test` gate (`suite.ys` should stay fast), plus the
C-level GC regression tests (`aux/gc_test.c`, `make test-gc`). Naming
convention (see [BENCHMARKING.md](BENCHMARKING.md) for the full picture):
`aux/*_stress.ys` for stress/regression scripts (`make stress-test`),
`aux/bench_*.ys` + `aux/bench/` for benchmarks (`make bench`), and
`aux/fuzz/` + `aux/fuzz_harness.c` for reader/evaluator fuzzing
(`make fuzz`, [FUZZING.md](FUZZING.md)). Run any script individually
with `./yas aux/<script>.ys`.

### GC stress testing

```sh
make test-gc-stress               # suite.ys + aux/*_stress.ys + gc_test.c, YAS_GC_STRESS=1
make test-gc-stress YAS_ASAN=1    # same, built with AddressSanitizer
```

`YAS_GC_STRESS=<n>` (any positive integer; `test-gc-stress` uses `1`)
shrinks the collector's cycle-start threshold and step quantum to `n`
bytes (default ~256KB / 4KB), so a collection cycle starts and advances
on essentially every allocation instead of only once the heap has grown
substantially - turning a one-in-a-million unrooted-local race into
something the ordinary test suite hits every run. `YAS_GC_MIN_THRESHOLD`
and `YAS_GC_DEBT_QUANTUM` override the two independently if finer control
is needed. See [GC.md](GC.md#testing) for what this is auditing and why
it's built this way (a cheaper, unsound "run a full collection every
allocation" design was tried and rejected first).

`make YAS_ASAN=1` also defines `YAS_NO_GC_POOL`, which disables the
free-list pool `gc_alloc()`/`gc_sweep_step()` recycle fixed-size objects
through (cons cells, environments, control-stack frames, ...; see
[GC.md](GC.md#free-list-pool)) - a pooled block reuses an address without
ever calling `free()` on it, which would suppress the use-after-free
detection `YAS_ASAN=1` exists to provide. `YAS_NO_GC_POOL=1` can be set
standalone too, without `YAS_ASAN`, to isolate whether a bug is
pool-related.

### FFI support

```sh
make YAS_FFI=1
```

Links `libffi` (on macOS, auto-detected via `brew --prefix libffi`).
Off by default.

## Directory layout

```
src/            interpreter sources
std/yas/*.ys    stdlib, embedded into src/stdlib.h at build time
aux/            slow stress/benchmark/fuzz scripts - *_stress.ys (make
                stress-test), bench_*.ys + bench/ (make bench), fuzz/
                (make fuzz) - plus the C-level GC regression tests
                (gc_test.c, make test-gc); see BENCHMARKING.md and
                FUZZING.md
docs/           this directory
suite.ys        interpreter test suite (make test)
```

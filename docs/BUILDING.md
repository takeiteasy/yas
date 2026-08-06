# Building

## Interpreter

```sh
make            # builds ./yas
make test       # runs suite.ys, gates on its process exit code
make test-gc    # C-level GC regression tests (aux/gc_test.c) - see GC.md
make stress-test    # runs the slow stress/benchmark scripts in aux/
make test-gc-stress # allocation-triggered GC stepping audit gate - see GC.md
make clean
```

Requires a C99 compiler and `libreadline`. Sources are in `src/`;
`src/stdlib.h` is generated from `std/yas/*.ys` by `embed.py` and is not
tracked in git.

`aux/` holds slow interpreted stress/benchmark/fuzz scripts that aren't
part of the `make test` gate (`suite.ys` should stay fast) - run
individually with `./yas aux/<script>.ys` or all together with `make
stress-test` - plus the C-level GC regression tests (`aux/gc_test.c`,
`make test-gc`).

### GC stress testing

```sh
make test-gc-stress               # suite.ys + aux/*.ys + gc_test.c, YAS_GC_STRESS=1
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

### FFI support

```sh
make YAS_FFI=1
```

Links `libffi` (on macOS, auto-detected via `brew --prefix libffi`).
Off by default.

## AOT compilation

See [AOT.md](AOT.md) for the design. Requires
[cccc](https://github.com) on `$PATH`; override with `CCCC=/path/to/cccc`
if it isn't.

```sh
make libyas               # shared lib: reader + plan compiler, dlopen'd by cccc
make aot                   # builds aot/examples/fib.ys -> build/fib
./build/fib                 # fib(30) = 832040
make aot-gen                # dump the expanded C to build/fib.expanded.c
make test-aot                # plan_test.c (plain C, no cccc) + build/fib's output
make check                   # test + test-gc + test-aot (test-gc-stress is separate, slower)
```

To compile a different `.ys` file, override `AOT_OUT` too so the result
doesn't land on `build/fib` (which `test-aot` always rebuilds and checks
against the default `fib` example, regardless of these overrides):

```sh
make aot AOT_SRC=aot/examples/misc.ys AOT_MAIN=path/to/your_main.c AOT_OUT=misc
```

`AOT_MAIN` is a plain C file providing `main()`; whatever functions the
`.ys` file defines are available to it under their mangled names (see
[AOT.md](AOT.md#name-mangling)). Avoid a runtime `#include <stdio.h>` in
it - see [AOT.md](AOT.md#cccc-issues-found-along-the-way), point 9.

## Directory layout

```
src/            interpreter + AOT compiler sources
std/yas/*.ys    stdlib, embedded into src/stdlib.h at build time
aot/            AOT pass: aot.c (cccc comptime side), examples/, tests/
aux/            slow stress/benchmark/fuzz scripts (make stress-test),
                plus the C-level GC regression tests (gc_test.c, make test-gc)
docs/           this directory
suite.ys        interpreter test suite (make test)
```

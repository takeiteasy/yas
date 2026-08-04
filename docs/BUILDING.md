# Building

## Interpreter

```sh
make            # builds ./yas
make test       # runs suite.ys, gates on ALL TESTS PASSED
make clean
```

Requires a C99 compiler and `libreadline`. Sources are in `src/`;
`src/stdlib.h` is generated from `std/yas/*.ys` by `embed.py` and is not
tracked in git.

### FFI support

```sh
make YAS_FFI=1
```

Links `libffi` (on macOS, auto-detected via `brew --prefix libffi`).
Off by default.

## AOT compilation

See [AOT.md](AOT.md) for the design. Requires a
[cccc](https://github.com) checkout as a sibling directory (`../cccc` by
default; override with `CCCC_DIR=/path/to/cccc`).

```sh
make libyas               # shared lib: reader + plan compiler, dlopen'd by cccc
make aot                   # builds aot/examples/fib.ys -> build/fib
./build/fib                 # fib(30) = 832040
make aot-gen                # dump the expanded C to build/fib.expanded.c
make test-aot                # plan_test.c (plain C, no cccc) + build/fib's output
make check                   # test + test-aot
```

To compile a different `.ys` file:

```sh
make aot AOT_SRC=aot/examples/misc.ys AOT_MAIN=path/to/your_main.c
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
docs/           this directory
suite.ys        interpreter test suite (make test)
```

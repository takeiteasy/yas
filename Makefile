# aot/plan.c is the AOT compiler proper (see aot/plan.h and docs/AOT.md) -
# ordinary plain C, built into `yas`/libyas alongside everything in src/,
# just kept out of src/ since it's conceptually part of the AOT pass, not
# the interpreter. Not picked up by the src/*.c wildcard, so listed
# explicitly.
sources=$(wildcard src/*.c) aot/plan.c

# Default to an optimized build (-O2): the interpreter is ~2.2x faster
# than at -O0 for no source change - see docs/BENCHMARKING.md. -g is kept
# so a release build still produces usable stack traces. `make
# YAS_DEBUG=1` gives the old unoptimized build
# back for stepping through a crash; `make OPT=-O3` (or any other level)
# overrides directly. Recursive `CFLAGS =` (not `CFLAGS=...`) so `make
# OPT=...` composes instead of needing a full CFLAGS override. Changing
# OPT/YAS_DEBUG/YAS_ASAN/YAS_FFI between builds needs `make clean` first -
# $(objects) only depends on headers, not on these flags, so stale .o
# files get silently relinked otherwise.
OPT ?= -O2
ifdef YAS_DEBUG
OPT = -O0
endif
CFLAGS = -Wall $(OPT) -g --std=c99 -D_GNU_SOURCE -iquote src
LDFLAGS=-lreadline

# Optional FFI support: make YAS_FFI=1
ifdef YAS_FFI
CFLAGS += -DYAS_FFI
# macOS: Homebrew libffi location
ifeq ($(shell uname),Darwin)
FFI_PREFIX := $(shell brew --prefix libffi 2>/dev/null)
ifneq ($(FFI_PREFIX),)
CFLAGS += -I$(FFI_PREFIX)/include
LDFLAGS += -L$(FFI_PREFIX)/lib
endif
endif
LDFLAGS += -lffi
endif

# Optional AddressSanitizer build: make YAS_ASAN=1. Paired with
# YAS_GC_STRESS (see data.c) as the audit tool for allocation-triggered
# GC stepping - see docs/GC.md "Testing" and `test-gc-stress` below.
ifdef YAS_ASAN
CFLAGS += -fsanitize=address -fno-omit-frame-pointer
LDFLAGS += -fsanitize=address
endif

objects=$(sources:.c=.o)

yas: $(objects)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(objects): $(wildcard src/*.h) aot/plan.h

src/stdlib.h: embed.py $(wildcard std/yas/*.ys)
	python3 embed.py $@ $(wildcard std/yas/*.ys)

src/module.o: src/stdlib.h
src/module.pic.o: src/stdlib.h

# libyas: the AOT plan compiler (aot/plan.c) exposed as a shared library,
# so cccc's comptime pass can dlopen it. Everything except main.c - a
# library has no business with the REPL/CLI entry point.
lib_sources=$(filter-out src/main.c,$(sources))
lib_objects=$(lib_sources:.c=.pic.o)

ifeq ($(shell uname),Darwin)
libyas=libyas.dylib
LIBFLAGS=-dynamiclib
else
libyas=libyas.so
LIBFLAGS=-shared
endif

src/%.pic.o: src/%.c
	$(CC) $(CFLAGS) -fPIC -c -o $@ $<

aot/%.pic.o: aot/%.c
	$(CC) $(CFLAGS) -fPIC -c -o $@ $<

$(libyas): $(lib_objects)
	$(CC) $(CFLAGS) $(LIBFLAGS) -o $@ $^ $(filter-out -lreadline,$(LDFLAGS))

.PHONY: libyas
libyas: $(libyas)

.PHONY: test
test: yas
	@./yas suite.ys; \
	status=$$?; \
	if [ $$status -ne 0 ]; then \
		echo "make test: FAILED"; \
		exit 1; \
	fi; \
	echo "make test: PASSED"

# C-level GC regression tests (aux/gc_test.c) - things suite.ys can't
# reach: the root-registration API and collector internals that need
# controlled stepping (write barrier mid-cycle, deep-list marking).
.PHONY: test-gc
test-gc: $(libyas)
	@mkdir -p build
	$(CC) -std=c99 -Wall -D_GNU_SOURCE -iquote src -o build/gc_test aux/gc_test.c -L. -lyas -Wl,-rpath,.
	./build/gc_test

# aux/ holds slow stress/benchmark/fuzz scripts (plus gc_test.c above)
# that aren't part of the regular `make test` gate. Naming convention
# (docs/BENCHMARKING.md, ticket #29): `*_stress.ys` here, `bench_*.ys` +
# `bench/` for benchmarks (see `bench` target below), `fuzz_*.ys` reserved
# for future fuzzing. Only `*_stress.ys` is filtered by this target (and
# test-gc-stress's YAS_GC_STRESS=1 loop below) - deliberately, not just a
# glob-tightening: a `bench_*.ys` timing script under YAS_GC_STRESS=1's
# 1-byte step quantum would be meaningless, and stress-test's job is
# regression coverage, not timing. Run individually as needed, e.g.:
#   ./yas aux/gc_continuation_stress.ys
.PHONY: stress-test
stress-test: yas
	@for f in aux/*_stress.ys; do \
		echo "=== $$f ==="; \
		./yas $$f || exit 1; \
	done

# Cross-language + self-timed benchmarks (docs/BENCHMARKING.md) - not part
# of `make check`, results are informational, not a pass/fail regression
# gate (aux/bench/bench.py only fails on a *present* runtime producing
# wrong output, never on being slow).
.PHONY: bench
bench: yas
	@python3 aux/bench/bench.py
	@for f in aux/bench_*.ys; do \
		echo "=== $$f ==="; \
		./yas $$f || exit 1; \
	done

# The audit gate for allocation-triggered GC stepping (docs/GC.md,
# "Allocation-triggered stepping"): YAS_GC_STRESS=1 shrinks the cycle-
# start threshold and step quantum to 1 byte (see data.c's
# gc_min_threshold_get()/gc_debt_quantum_get()), so a collection cycle
# starts and advances on essentially every allocation instead of only
# once ~256KB has accumulated - turning a one-in-a-million unrooted-
# local race into something suite.ys/aux/*_stress.ys/gc_alloc_stress.ys hit
# every run. Pair with `make test-gc-stress YAS_ASAN=1` so a surviving
# bug reports a stack trace naming the exact function instead of an
# opaque crash. Only *_stress.ys, not bench_*.ys - a 1-byte step quantum
# would make a timing script's numbers meaningless (see `stress-test`'s
# comment above for the naming convention this depends on).
.PHONY: test-gc-stress
test-gc-stress: yas $(libyas)
	@echo "=== suite.ys (YAS_GC_STRESS=1) ==="
	@YAS_GC_STRESS=1 ./yas suite.ys || exit 1
	@for f in aux/*_stress.ys; do \
		echo "=== $$f (YAS_GC_STRESS=1) ==="; \
		YAS_GC_STRESS=1 ./yas $$f || exit 1; \
	done
	@mkdir -p build
	@echo "=== aux/gc_test.c (YAS_GC_STRESS=1) ==="
	$(CC) -std=c99 -Wall -D_GNU_SOURCE -iquote src -o build/gc_test aux/gc_test.c -L. -lyas -Wl,-rpath,.
	@YAS_GC_STRESS=1 ./build/gc_test || exit 1
	@echo "make test-gc-stress: PASSED"

# ============================================================
# AOT: lowers a .ys file to C at compile time via cccc's comptime pass +
# reflection API. See docs/AOT.md for the design and aot/aot.c for the
# implementation notes (several cccc quirks/bugs worked around there).
#
# Requires cccc (https://github.com) on $PATH; override with
# `make aot CCCC=/path/to/cccc` if it isn't. cccc is run directly from
# this repo's own root (ordinary relative paths below) -- it used to need
# `cd` into cccc's own checkout because its bundled headers only resolved
# relative to its own CWD, fixed upstream in cccc #891 (headers now
# resolve from any CWD with no flags).
# ============================================================
CCCC ?= cccc

AOT_SRC ?= aot/examples/fib.ys
AOT_MAIN ?= aot/fib_main.c
# Output basename under build/ - defaults to the fib example. Override
# alongside AOT_SRC/AOT_MAIN when building a different .ys file, so it
# doesn't collide with build/fib (which `test-aot` below always checks
# against the *default* fib example, regardless of these overrides).
AOT_OUT ?= fib

# aot/fib_main.c and aot/aot.c are passed to cccc as two positional source
# files, not concatenated or #include'd: cccc #890 fixed a comptime function
# losing sight of file-scope statics declared alongside it across an
# #include boundary, but -c=native has a separate, narrower gap (cccc #896)
# where its temp-file re-emission chokes on a #include'd file that itself
# uses #include @comptime syntax - passing both files as separate positional
# arguments avoids that entirely, since cccc merges and preprocesses them
# itself before any native serialization happens. $(AOT_MAIN) must come
# first so its #include <stdio.h> is the one cccc captures and re-emits for
# the native compiler. YAS_LIBYAS_PATH and YAS_AOT_SOURCE_PATH arrive via
# `-D`, not a source #define: cccc's comptime pass deliberately doesn't
# forward source-file #defines into comptime function bodies (see cccc's
# docs/MACROS.md, "Pre-parse macro declaration context"), but a
# command-line `-D` value is the documented exception, so it's the correct
# escape hatch for a build-machine-specific path like this one.
#
# Relative, not absolute: aot_bind()'s dlopen() runs at cccc's own compile
# time (it's [[cccc::comptime]]), with cccc's CWD being wherever `make aot`
# invokes it from -- this repo's root, below -- so "./$(libyas)" resolves
# correctly without needing an absolute path.
.PHONY: aot
aot: aot/aot.c $(AOT_SRC) $(AOT_MAIN) $(libyas)
	@mkdir -p build
	$(CCCC) -c=native -DYAS_LIBYAS_PATH='"./$(libyas)"' -DYAS_AOT_SOURCE_PATH='"$(AOT_SRC)"' -o build/$(AOT_OUT) $(AOT_MAIN) aot/aot.c

.PHONY: aot-gen
aot-gen: aot/aot.c $(AOT_SRC) $(AOT_MAIN) $(libyas)
	@mkdir -p build
	$(CCCC) -G -DYAS_LIBYAS_PATH='"./$(libyas)"' -DYAS_AOT_SOURCE_PATH='"$(AOT_SRC)"' -o build/$(AOT_OUT).expanded.c $(AOT_MAIN) aot/aot.c
	@echo "generated C: build/$(AOT_OUT).expanded.c"

.PHONY: test-aot
# Pinned to the default fib example regardless of ambient AOT_SRC/AOT_MAIN/
# AOT_OUT overrides in the environment - this is a fixed regression check,
# not a generic "test whatever `make aot` was last pointed at" target.
test-aot: libyas
	$(MAKE) aot AOT_SRC=aot/examples/fib.ys AOT_MAIN=aot/fib_main.c AOT_OUT=fib
	$(CC) -std=c99 -Wall -D_GNU_SOURCE -iquote src -iquote aot -o build/plan_test aot/tests/plan_test.c -L. -lyas -Wl,-rpath,.
	./build/plan_test
	@out=$$(./build/fib); \
	rc=$$?; \
	if [ "$$rc" != "0" ] || [ "$$out" != "fib(30) = 832040" ]; then \
		echo "make test-aot: FAILED (build/fib exit $$rc, output '$$out', want exit 0 and 'fib(30) = 832040')"; \
		exit 1; \
	fi; \
	echo "PASS $$out"; \
	echo "make test-aot: PASSED"

.PHONY: check
check: test test-gc test-aot

.PHONY: clean
clean:
	$(RM) $(objects) $(lib_objects) yas libyas.dylib libyas.so src/stdlib.h .test-output.log
	$(RM) -r build

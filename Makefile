sources=$(wildcard src/*.c)

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
# -lpthread: the interpreter lock (src/thread.c, ticket #47 milestone 1).
# A no-op link stub on Darwin (pthread lives in libSystem) but required
# on Linux/glibc.
LDFLAGS=-lreadline -lpthread

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
# Also disables the GC free-list pool (YAS_NO_GC_POOL, data.c) - a pooled
# block is freed and reused without ever calling free() on the same
# address twice from ASan's point of view, which would suppress the very
# use-after-free detection this build exists to run.
ifdef YAS_ASAN
CFLAGS += -fsanitize=address -fno-omit-frame-pointer -DYAS_NO_GC_POOL
LDFLAGS += -fsanitize=address
endif

objects=$(sources:.c=.o)

yas: $(objects)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(objects): $(wildcard src/*.h)

src/stdlib.h: embed.py $(wildcard std/yas/*.ys)
	python3 embed.py $@ $(wildcard std/yas/*.ys)

src/module.o: src/stdlib.h
src/module.pic.o: src/stdlib.h

# libyas: the interpreter core (everything except main.c) exposed as a
# shared library - used by the C-level GC tests (aux/gc_test.c) that need
# the plain-C API but no REPL/CLI entry point.
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

$(libyas): $(lib_objects)
	$(CC) $(CFLAGS) $(LIBFLAGS) -o $@ $^ $(filter-out -lreadline,$(LDFLAGS))

.PHONY: libyas
libyas: $(libyas)

.PHONY: test
# Runs the .ys suite, the module-system tests, and aux/thread_test.c (the
# GIL concurrency regression, ticket #47 milestone 1) against libyas.
test: yas $(libyas)
	@./yas suite.ys; status=$$?; \
	./yas aux/module_test.ys; status2=$$?; \
	mkdir -p build; \
	$(CC) -std=c99 -Wall -D_GNU_SOURCE -iquote src -o build/thread_test aux/thread_test.c -L. -lyas -Wl,-rpath,. || exit 1; \
	./build/thread_test; status3=$$?; \
	if [ $$status -ne 0 ] || [ $$status2 -ne 0 ] || [ $$status3 -ne 0 ]; then \
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
# `bench/` for benchmarks (see `bench` target below), and `fuzz_*.ys` +
# `aux/fuzz/` for reader/evaluator fuzzing (see `fuzz` target below,
# docs/FUZZING.md). Only `*_stress.ys` is filtered by this target (and
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
	@echo "=== aux/module_test.ys (YAS_GC_STRESS=1) ==="
	@YAS_GC_STRESS=1 ./yas aux/module_test.ys || exit 1
	@mkdir -p build
	$(CC) -std=c99 -Wall -D_GNU_SOURCE -iquote src -o build/thread_test aux/thread_test.c -L. -lyas -Wl,-rpath,.
	@echo "=== aux/thread_test.c (YAS_GC_STRESS=1) ==="
	@YAS_GC_STRESS=1 ./build/thread_test || exit 1
	@for f in aux/*_stress.ys; do \
		echo "=== $$f (YAS_GC_STRESS=1) ==="; \
		YAS_GC_STRESS=1 ./yas $$f || exit 1; \
	done
	@mkdir -p build
	@echo "=== aux/gc_test.c (YAS_GC_STRESS=1) ==="
	$(CC) -std=c99 -Wall -D_GNU_SOURCE -iquote src -o build/gc_test aux/gc_test.c -L. -lyas -Wl,-rpath,.
	@YAS_GC_STRESS=1 ./build/gc_test || exit 1
	@echo "make test-gc-stress: PASSED"

# Reader/evaluator fuzzing (docs/FUZZING.md) - aux/fuzz/fuzz.py generates
# seeded cases, build/fuzz_harness runs them in-process against libyas.
# Informational gate like `bench`: NOT part of `make check` (a hang in
# fuzzed code is correct semantics, so "pass" just means no crashes).
# Overrides: make fuzz FUZZ_CASES=50000 FUZZ_SEED=42 FUZZ_MODE=grammar.
# Pair with the GC stress runtime for deeper sessions:
#   make clean && make YAS_GC_STRESS=1 && make fuzz
# and with YAS_ASAN=1 likewise (both need `make clean` between flag
# changes - see the comment at the top). Crash artifacts land in
# gitignored fuzz-findings/.
.PHONY: fuzz
fuzz: yas $(libyas)
	@mkdir -p build
	$(CC) -std=c99 -Wall -D_GNU_SOURCE -iquote src -o build/fuzz_harness aux/fuzz_harness.c -L. -lyas -Wl,-rpath,.
	python3 aux/fuzz/fuzz.py \
		$(if $(FUZZ_CASES),--cases $(FUZZ_CASES)) \
		$(if $(FUZZ_SEED),--seed $(FUZZ_SEED)) \
		$(if $(FUZZ_MODE),--mode $(FUZZ_MODE))

.PHONY: check
check: test test-gc

.PHONY: clean
clean:
	$(RM) $(objects) $(lib_objects) yas libyas.dylib libyas.so src/stdlib.h .test-output.log
	$(RM) -r build

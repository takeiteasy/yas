sources=$(wildcard src/*.c)

CFLAGS=-Wall -O0 -g --std=c99 -D_GNU_SOURCE
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

objects=$(sources:.c=.o)

yas: $(objects)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(objects): $(wildcard src/*.h)

src/stdlib.h: embed.py $(wildcard std/yas/*.ys)
	python3 embed.py $@ $(wildcard std/yas/*.ys)

src/module.o: src/stdlib.h
src/module.pic.o: src/stdlib.h

# libyas: the reader (aot_shim.c) exposed as a shared library, so cccc's
# comptime pass can dlopen it. Everything except main.c - a library has no
# business with the REPL/CLI entry point.
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
test: yas
	@./yas suite.ys > .test-output.log 2>&1; \
	cat .test-output.log; \
	if grep -q "SOME TESTS FAILED" .test-output.log || ! grep -q "ALL TESTS PASSED" .test-output.log; then \
		rm -f .test-output.log; \
		echo "make test: FAILED"; \
		exit 1; \
	fi; \
	rm -f .test-output.log; \
	echo "make test: PASSED"

# ============================================================
# AOT: lowers a .ys file to C at compile time via cccc's comptime pass +
# reflection API. See docs/AOT.md for the design and aot/aot.c for the
# implementation notes (several cccc quirks/bugs worked around there).
#
# Requires cccc (https://github.com - sibling checkout by default;
# override with `make aot CCCC_DIR=/path/to/cccc`). cccc is run directly
# from this repo's own root (ordinary relative paths below) -- it used to
# need `cd $(CCCC_DIR)` because its bundled headers only resolved relative
# to its own CWD, fixed upstream in cccc #891 (headers now resolve from
# any CWD with no flags).
# ============================================================
CCCC_DIR ?= $(abspath ../cccc)
CCCC ?= $(CCCC_DIR)/cccc

AOT_SRC ?= aot/examples/fib.ys
AOT_MAIN ?= aot/fib_main.c

# Substitute the libyas path into aot.c (plain text, before cccc ever sees
# it - see the comment above the dlopen call in aot/aot.c for why this
# can't be a #define read inside the comptime function body), then
# concatenate the YAS_AOT_SOURCE_PATH #define + aot.c + the host "main"
# file into one physical source file. Also plain concatenation, not an
# #include: a global-generation comptime function loses sight of its own
# file-scope statics when invoked across an #include boundary, even though
# the preprocessed token stream is identical either way (see the note near
# the top of aot/aot.c). `make aot` is therefore the only supported way to
# build an AOT example - not a direct cccc invocation.
#
# Relative, not absolute: aot_bind()'s dlopen() runs at cccc's own compile
# time (it's [[cccc::comptime]]), with cccc's CWD being wherever `make aot`
# invokes it from -- this repo's root, below -- so "./$(libyas)" resolves
# correctly without needing an absolute path.
build/aot_gen.c: aot/aot.c src/aot_plan.h src/aot_plan_ops.h $(libyas)
	@mkdir -p build
	sed -e "s#AOT_LIBYAS_PATH#./$(libyas)#" aot/aot.c > $@

build/fib.gen.c: build/aot_gen.c $(AOT_MAIN)
	@mkdir -p build
	{ echo '#define YAS_AOT_SOURCE_PATH "$(AOT_SRC)"'; \
	  cat build/aot_gen.c $(AOT_MAIN); } > $@

.PHONY: aot
aot: build/fib.gen.c
	$(CCCC) -c=native -o build/fib build/fib.gen.c

.PHONY: aot-gen
aot-gen: build/fib.gen.c
	$(CCCC) -G -o build/fib.expanded.c build/fib.gen.c
	@echo "generated C: build/fib.expanded.c"

.PHONY: test-aot
test-aot: libyas aot
	$(CC) -std=c99 -Wall -D_GNU_SOURCE -iquote src -o build/plan_test aot/tests/plan_test.c -L. -lyas -Wl,-rpath,.
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
check: test test-aot

.PHONY: clean
clean:
	$(RM) $(objects) $(lib_objects) yas libyas.dylib libyas.so src/stdlib.h .test-output.log
	$(RM) -r build

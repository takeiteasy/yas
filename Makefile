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

.PHONY: clean
clean:
	$(RM) $(objects) yas src/stdlib.h .test-output.log

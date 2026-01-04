sources=$(wildcard *.c)

CFLAGS=-Wall -O0 -g --std=c99 -D_GNU_SOURCE
LDFLAGS=-lreadline

# Optional FFI support: make GOLD_FFI=1
ifdef GOLD_FFI
CFLAGS += -DGOLD_FFI
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

gold: $(objects)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(objects): $(wildcard *.h)

stdlib.h: embed.py $(wildcard std/gold/*.gl)
	python3 embed.py $@ $(wildcard std/gold/*.gl)

module.o: stdlib.h

.PHONY: clean
clean:
	$(RM) *.o gold stdlib.h 


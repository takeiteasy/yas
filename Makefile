sources=$(wildcard *.c)

CFLAGS=-Wall -O0 -g --std=c99 -D_GNU_SOURCE
LDFLAGS=-lreadline

objects=$(sources:.c=.o)

gold: $(objects)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(objects): $(wildcard *.h)

.PHONY: clean
clean:
	$(RM) *.o gold 


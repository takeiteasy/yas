sources=$(wildcard *.c)

CFLAGS=-Wall -O0 -g --std=c99 -D_GNU_SOURCE
LDFLAGS=-lreadline

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


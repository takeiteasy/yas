#include <stdio.h>
#include <stdlib.h>

typedef struct Cons {
	long n;
	struct Cons *rest;
} Cons;

static Cons *build(long n, Cons *acc)
{
	while (n != 0) {
		Cons *c = malloc(sizeof(Cons));
		c->n = n;
		c->rest = acc;
		acc = c;
		n--;
	}
	return acc;
}

static long suml(Cons *lst, long acc)
{
	while (lst) {
		acc += lst->n;
		lst = lst->rest;
	}
	return acc;
}

int main(void)
{
	printf("%ld\n", suml(build(50000, NULL), 0));
	return 0;
}

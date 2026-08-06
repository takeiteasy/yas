#include <stdio.h>
#include <stdlib.h>

/* C has no closures over local state in the same sense - map/filter/
 * foldl are modeled with function pointers over a heap array, the
 * closest same-shape equivalent. */
typedef long (*unary_fn)(long);
typedef int (*pred_fn)(long);
typedef long (*binop_fn)(long, long);

static long square(long x) { return x * x; }
static int above(long x) { return x > 400000000; }
static long add(long a, long x) { return a + x; }

static long *map(unary_fn fn, long *in, long n)
{
	long *out = malloc(n * sizeof(long));
	for (long i = 0; i < n; i++) out[i] = fn(in[i]);
	return out;
}

static long *filter(pred_fn pred, long *in, long n, long *out_n)
{
	long *out = malloc(n * sizeof(long));
	long count = 0;
	for (long i = 0; i < n; i++)
		if (pred(in[i])) out[count++] = in[i];
	*out_n = count;
	return out;
}

static long foldl(binop_fn fn, long init, long *in, long n)
{
	long acc = init;
	for (long i = 0; i < n; i++) acc = fn(acc, in[i]);
	return acc;
}

int main(void)
{
	long n = 40000;
	long *lst = malloc(n * sizeof(long));
	for (long i = 0; i < n; i++) lst[i] = i + 1;

	long *squared = map(square, lst, n);
	long big_n;
	long *big = filter(above, squared, n, &big_n);
	printf("%ld\n", foldl(add, 0, big, big_n));

	free(lst);
	free(squared);
	free(big);
	return 0;
}

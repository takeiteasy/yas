#include <stdio.h>
#include <stdlib.h>

int main(void)
{
	long n = 3000;
	/* Allocated and filled at runtime, not a static array (which the
	 * compiler could reason about at compile time) - matches every
	 * other variant here actually allocating its vector/array/table at
	 * runtime, the same reasoning alloc.c gives for using malloc'd
	 * cons cells over a flat array. */
	int *sieve = calloc(n, sizeof(int));
	sieve[0] = 1;
	sieve[1] = 1;

	for (long i = 0; i < n; i++) {
		if (sieve[i] == 0) {
			long j = i * i;
			while (j < n) {
				sieve[j] = 1;
				j += i;
			}
		}
	}

	long count = 0;
	for (long i = 0; i < n; i++)
		if (sieve[i] == 0)
			count++;

	printf("%ld\n", count);
	free(sieve);
	return 0;
}

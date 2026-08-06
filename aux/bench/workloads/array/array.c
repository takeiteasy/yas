#include <stdio.h>

int main(void)
{
	long n = 3000;
	static int sieve[3000];
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
	return 0;
}

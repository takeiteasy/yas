#include <stdio.h>

static long sum_nontail(long n)
{
	if (n == 0) return 0;
	return n + sum_nontail(n - 1);
}

static long sum_tail(long n, long acc)
{
	while (n != 0) {
		acc += n;
		n--;
	}
	return acc;
}

int main(void)
{
	long non_tail_n = 800;
	long tail_n = 300000;
	printf("%ld\n", sum_nontail(non_tail_n) + sum_tail(tail_n, 0));
	return 0;
}

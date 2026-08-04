/* Trailer concatenated after aot.c by `make aot` - see the Makefile and
 * the note at the top of aot.c on why this can't be a separate
 * `#include`d file. Plain runtime #include <stdio.h> works here (cccc
 * #891 fixed the FILE/fpos_t typedef collision this used to hit), so the
 * result is printed directly instead of being squeezed through the
 * process exit code. `make test-aot` checks this exact output; run
 * `./build/fib` directly to see it yourself. */
#include <stdio.h>

int main(void)
{
	printf("fib(30) = %d\n", (int)fib(30));
	return 0;
}

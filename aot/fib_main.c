/* Trailer concatenated after aot.c by `make aot` - see the Makefile and
 * the note at the top of aot.c on why this can't be a separate
 * `#include`d file. No runtime #include here, deliberately: cccc's
 * real-system-header support is a work in progress (a plain runtime
 * `#include <stdio.h>` collides its own stdio.h polyfill with the real
 * one - "typedef redefinition: FILE/fpos_t" - across several
 * --sysroot/--use-system-headers/--no-builtin-includes combinations
 * tried; see docs/AOT.md). The result is reported through the process
 * exit code instead: fib(13)=233 fits a byte, so the exit code *is* the
 * value, not a truncation of one - run `./build/fib; echo $?` (`make aot`
 * does this for you). `make test-aot` cross-checks the untruncated
 * fib(30) against both the interpreter and the plain-C plan_test
 * harness, which have no such restriction. */
int main(void)
{
	return (int)fib(13);
}

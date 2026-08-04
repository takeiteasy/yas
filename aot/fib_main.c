/* Host "main" file for the AOT pass - passed to cccc alongside aot/aot.c
 * as a second positional source file (see the Makefile's `aot` target),
 * not via `#include`: cccc #890 fixed a comptime function losing sight of
 * file-scope statics declared alongside it across an `#include` boundary,
 * so an `#include "aot.c"` here would now work too for the plain bytecode
 * path, but `-c=native`'s temp-file re-emission chokes on aot.c's
 * `#include @comptime` lines when they arrive through a re-emitted
 * `#include` (cccc #896) - passing both files as separate positional
 * arguments sidesteps that by having cccc itself merge and preprocess
 * them before any native serialization happens. Plain runtime
 * #include <stdio.h> works here (cccc #891 fixed the FILE/fpos_t typedef
 * collision this used to hit), so the result is printed directly instead
 * of being squeezed through the process exit code. `make test-aot` checks
 * this exact output; run `./build/fib` directly to see it yourself. */
#include <stdio.h>

#ifndef YAS_AOT_SOURCE_PATH
#error "define YAS_AOT_SOURCE_PATH via -D - see the Makefile's aot target"
#endif

yas_aot_compile(YAS_AOT_SOURCE_PATH);

int main(void)
{
	printf("fib(30) = %d\n", (int)fib(30));
	return 0;
}

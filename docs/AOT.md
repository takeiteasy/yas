# AOT compilation

yas can lower a restricted subset of itself to plain C **at compile time**,
using [cccc](https://github.com)'s comptime pass and reflection API. The
result is an ordinary C function - no interpreter, no `Atom`, no GC - that
the system compiler optimizes and links like any other code.

```scheme
(define (fib n)
  (if (< n 2)
      n
      (+ (fib (- n 1)) (fib (- n 2)))))
```

lowers to:

```c
long fib(long n) {
    return n < 2 ? n : fib(n - 1) + fib(n - 2);
}
```

`fib(30)`: ~31.4s interpreted, ~0.01s AOT-compiled.

## Quick start

```sh
make libyas     # shared lib exposing the reader + plan compiler
make aot        # builds aot/examples/fib.ys -> build/fib
./build/fib; echo $?    # 233 == fib(13)
make aot-gen    # dump the expanded C to build/fib.expanded.c
make test-aot   # plan_test.c (no cccc) + build/fib's exit code
```

Requires a [cccc](https://github.com) checkout as a sibling directory
(`../cccc`) or `make aot CCCC_DIR=/path/to/cccc`.

## Supported subset

- `(define (name params...) body...)` - function definitions
- `(define name literal)` - scalar int/float constants
- `if`, `let`, `let*`, `begin`
- `+ - * /` (n-ary), `< > <= >= =` (n-ary), `and`/`or`/`not`
- integer and float literals, parameter references, let-bound references
- direct and mutual recursion between AOT-compiled functions

Every value is an unboxed C `long` or `double` - never an `Atom`. A whole
function is either all-`long` or all-`double`: if any part of its body is a
float literal, a float constant, or a call to a function already inferred
`double`, the *entire* function (return type and every parameter) is
`double`. This is deliberately simpler than per-expression inference; mixed
int/float code still works via automatic promotion (matching yas's
interpreted arithmetic), just at the whole-function granularity.

**Not supported** - each produces a `MacroErrorAt` naming the form, never
silently wrong code: closures and higher-order functions, `lambda` outside
`define`, `set!`, strings/symbols/pairs/vectors/hashes at runtime, `import`,
`defmacro`, `quasiquote`, `call/cc`, `try`, FFI, variadic/keyword lambda
lists, computed constant initializers.

**Do not `(import (yas base))` in AOT input.** `std/yas/base.ys` rebinds
`+ - * /` as variadic closures over `foldl`; this pass treats them as C
primitives instead. Under `(import (yas base))` the interpreted `+` also
carries variadic identity and metamethod dispatch (see
[ARCHITECTURE.md](ARCHITECTURE.md#metamethod-dispatch)) that plain C
arithmetic doesn't reproduce. AOT input is self-contained `.ys` with no
imports.

## Name mangling

The reader uppercases every symbol (see [ARCHITECTURE.md](ARCHITECTURE.md)),
so mangling starts from e.g. `FIB-HELPER?` and produces the C identifier:

| yas character | C |
|---|---|
| `A`-`Z` | lowercased |
| `-` | `_` |
| `?` | `_p` |
| `!` | `_bang` |
| `*` | `_star` |
| `/` | `_slash` |
| `>` | `_gt` |
| `<` | `_lt` |
| `=` | `_eq` |
| anything else | `_` |
| leading digit | prefixed with `_` |

`fib-helper?` -> `fib_helper_p`. Collisions after mangling are not currently
detected (ticket).

## Architecture: why this isn't one comptime pass

The obvious design - read the `.ys` file and build the C AST in one pass,
entirely inside cccc's comptime VM - was the first thing tried, and it hit a
string of VM-scale bugs: crashes and "undefined variable" errors that
appeared only once enough comptime-side code existed, and that shrank away
in every minimal reproduction. Chasing them one at a time did not converge.

The actual design splits the work by *where it can safely run*:

- **`src/aot_plan.c`** - ordinary C, compiled by the system compiler, with
  none of cccc's constraints. Does everything: reads the file (via
  `src/aot_shim.c`'s reader front end), mangles names, infers long-vs-double
  per function, resolves `let`/`let*` bindings to numbered local slots, and
  flattens each function body into a flat **postfix op list**
  (`src/aot_plan_ops.h`) - `PUSH_INT`, `PUSH_PARAM`, `BINOP`, `COND`,
  `CALL`, `CAST_LONG`, `SEQ`, etc. Fully unit-testable with a plain `cc`
  and no cccc in the loop: `aot/tests/plan_test.c` builds a small in-memory
  AST from the op list and evaluates it lazily (real recursion, real
  short-circuiting on `COND` - see the comment at the top of that file for
  why a *naive* eager interpreter of the flat list sends `fib` into
  unbounded recursion, and why that's specific to eager evaluation, not a
  flaw in the op-list design itself).

- **`aot/aot.c`** - the only part that runs inside cccc's comptime VM.
  `dlopen`s `libyas` (built by `make libyas`) to reach both the reader and
  the plan compiler, then replays each function's flat op list through a
  small, non-recursive stack machine that pushes/pops cccc reflection
  `Node *` instead of values - building an AST, not evaluating anything.
  `COND` becomes a real C ternary (`MakeCond`); its *generated code*
  short-circuits at the AOT-compiled program's own runtime, which is what
  makes recursive functions with a base case terminate correctly.

This keeps the comptime-side surface area small and flat, which is what
actually made it work reliably.

## cccc issues found along the way

`cccc` is explicitly marked work-in-progress. Building this pass surfaced
several VM-level issues worth reporting upstream; each is reproduced as a
small standalone case during development (not currently checked into this
repo - recreate from the description if needed) and documented at its
trigger site in `aot/aot.c` / `src/aot_shim.h`:

1. **Forward-declared comptime functions crash the VM.** A
   `[[cccc::comptime]]` function prototype, forward-declared and defined
   later (needed for mutual recursion between two comptime *helper*
   functions - not the generated C, which has no such restriction via
   `FunctionPrototype`), reliably SIGSEGVs cccc itself. Minimal repro is
   two functions, `is_even`/`is_odd`, each calling the other, one
   forward-declared. Worked around by never forward-declaring a comptime
   function - restructuring so mutually-recursive logic is inlined instead.

2. **Indirect calls through a `dlsym`'d function pointer crash at runtime
   if the cast happens inline at the call site** -
   `((int(*)(int,int))dlsym(h,"f"))(a,b)` compiles but crashes when run,
   regardless of block (`#pragma cccc comptime`) vs. attribute
   (`[[cccc::comptime]]`) style, or call-site vs. global-generation
   category. Assigning the cast result to a named variable first, then
   calling through the name, works reliably. Always do the latter.

3. **Function-pointer `typedef`s declared *inside* comptime-executed code**
   (a `#pragma cccc comptime begin/end` block, or a `[[cccc::comptime]]`
   function body) **are rejected at compile time** - "pointer/string
   variables are not supported yet (ticket #188 scope: int/float/struct
   only)". Declaring the typedef at plain file scope avoids it; plain
   file-scope declarations are part of the "conservative snapshot"
   comptime code can see (per cccc's own docs on global-generation macros).

4. **String values referenced via anything other than a literal or a local
   variable, inside a comptime function body, are unreliable** - not just
   the ticket #188 case above. A file-scope `const char *` global, or even
   an object-like `#define` (`#define X "s"` then `helper(X)`), used as an
   argument somewhere in a comptime call chain produces either an
   "undefined variable" error or an outright crash, inconsistently,
   despite ordinary C preprocessing already having replaced the macro with
   a literal before any parsing should occur. A plain string literal or a
   local `const char *` variable both work reliably. This is why
   `aot/aot.c` picks its `dlopen` path via two whole function definitions
   selected by a file-scope `#ifdef`/`#else`/`#endif`, rather than a
   `#define` referenced from inside one function.

5. **A `#define`d integer constant used as a local array size inside a
   comptime function body** is sometimes rejected as an "undefined
   variable" too (`int floats[AOT_MAX_SCOPE];` where `AOT_MAX_SCOPE` is a
   plain `#define`), even though the identical macro works fine in a
   *global* array declaration (`static AotFn g_fns[AOT_MAX_FNS];`,
   file scope, not inside any function body). Worked around by using
   literal integers directly inside function bodies.

6. **`#if`/`#else`/`#endif` directly inside a `#pragma cccc comptime
   begin/end` block corrupts the block's own begin/end tracking**, turning
   its closing `#pragma ... end` into a "stray end without matching begin"
   error. Plain `#ifdef`/`#endif` with no `#else` is fine. Not directly
   relevant to the final design (which doesn't use the block form at all),
   kept here since it was found along the way.

7. **A global-generation entry point (`void f(char *path)`, invoked as a
   bare `f("...");` statement) loses sight of its own file-scope statics
   once invoked across an `#include` boundary** - even though the
   preprocessed token stream is byte-identical to writing the call
   directly in the same physical file. Reproduced directly: appending
   `yas_aot_compile("...");` to the end of `aot.c` itself compiles and
   runs; the identical call one `#include "aot.c"` away, in a separate
   driver file, fails with `undefined variable 'plan_fn'` (a file-scope
   static declared earlier in `aot.c`). This is why the actual
   `yas_aot_compile(...)` invocation lives at the bottom of `aot/aot.c`
   itself, and why `make aot` builds by *concatenating* `aot.c` with a
   host "main" file (`aot/fib_main.c`) rather than having the host file
   `#include` it.

8. **cccc's own bundled headers (`implicit-reflection.h` -> `stdbool.h`
   etc.) are only found when cccc's process CWD is its own repository
   root**, not relative to the compiled file's location or an absolute
   `-I`/`--sysroot`/`--use-system-headers`/`--no-builtin-includes`
   combination tried. This is why `make aot` does `cd $(CCCC_DIR) &&
   ./cccc ...` and passes only absolute paths - both for the source files
   handed to cccc and for the `libyas`/`.ys` paths `dlopen`ed from inside
   the comptime pass (a CWD-relative `dlopen("./libyas.dylib")` would
   otherwise look for it next to cccc, not next to yas).

9. **A plain runtime `#include <stdio.h>` collides cccc's own bundled
   `stdio.h` polyfill with the real system one** ("typedef redefinition:
   `FILE`"/`fpos_t`"), with or without `--sysroot`/`--use-system-headers`.
   Not worked around (not needed): `aot/fib_main.c` reports its result
   through the process exit code instead of `printf`, which needs no
   headers at all.

None of these affect the *generated* program - only the comptime tooling
used to produce it. Points 7-9 are arguably the most disruptive for
reusing this pattern elsewhere, since they constrain how any cccc-based
build must be invoked, not just what comptime code may contain.

## Future work

See the tracker (labels `aot`, `compiler`) for the itemized backlog:
GC root/shadow-stack API so a future `Atom`-level lowering can hold live
values; `Atom`-level lowering beyond the numeric subset; closures and
higher-order functions; a standalone macro-expansion pass (macros are
currently runtime and per-environment - see
[ARCHITECTURE.md](ARCHITECTURE.md)); metamethod dispatch for lowered
arithmetic; `call/cc`/`try` under AOT; richer (per-expression, not
per-function) type inference; name-collision detection after mangling.

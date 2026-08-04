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
./build/fib     # fib(30) = 832040
make aot-gen    # dump the expanded C to build/fib.expanded.c
make test-aot   # plan_test.c (no cccc) + build/fib's output
```

Requires [cccc](https://github.com) on `$PATH`; override with
`make aot CCCC=/path/to/cccc` if it isn't.

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

2. **Fixed upstream (cccc #885).** Indirect calls through a `dlsym`'d
   function pointer used to crash at runtime if the cast happened inline
   at the call site - `((int(*)(int,int))dlsym(h,"f"))(a,b))` compiled but
   crashed when run. `aot/aot.c` still assigns each dlsym result to a
   named variable before calling through it, simply because that reads
   better with sixteen of them, not because the inline form is unsafe now.

3. **Fixed upstream (cccc #886).** A function-pointer `typedef` declared
   *inside* comptime-executed code (a `#pragma cccc comptime begin/end`
   block, or a `[[cccc::comptime]]` function body) used to be rejected at
   compile time - "pointer/string variables are not supported yet (ticket
   #188 scope: int/float/struct only)" - because the dispatcher that
   decides "function, variable, or something else" had no case for a type
   declaration. `aot/aot.c`'s `fp_*` typedefs still live at plain file
   scope, but only because the file-scope statics they type (`plan_fn`,
   `plan_error`, ...) need to be there regardless of this fix.

4. **Fixed upstream (cccc #887), in part.** An identifier referenced
   inside a comptime function body that cccc's parser can't resolve -
   including a `#define`d string used as an argument in a comptime call
   chain, or a file-scope `const char *` global - used to sometimes crash
   the host (SIGSEGV) instead of producing a clean "undefined variable"
   compile error; that crash is what's fixed. The underlying restriction
   is not a bug and is not fixed: cccc's comptime pass deliberately
   isolates the comptime compile from the runtime translation unit's
   `#define`s (see cccc's docs/MACROS.md, "Pre-parse macro declaration
   context") - a `#define`, even though ordinary C preprocessing would
   already have replaced it with a literal everywhere else in the file, is
   simply invisible inside a comptime function body unless the header
   defining it is routed `@shared`, the value is passed via `-D`, `#define
   @shared` opts in that one macro in place (cccc #888 - there is an
   escape hatch for a source `#define` now, contrary to what this section
   used to say), or `--comptime-include-all` is used. A file-scope
   *initialized* global (`const char *g = "...";`) remains unsupported
   outright (no escape hatch) - filed upstream as a separate follow-up.
   `aot/aot.c` still picks its `dlopen` path via `-D YAS_LIBYAS_PATH=...`
   rather than a source `#define` (see the Makefile's `aot` target) - not
   because no mechanism exists, but because the value is build-machine-
   specific (a relative `libyas.dylib`/`libyas.so` path chosen by `make`),
   which is exactly what `-D` is for.

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

7. **Fixed upstream (cccc #890).** A global-generation entry point (`void
   f(char *path)`, invoked as a bare `f("...");` statement) used to lose
   sight of its own file-scope statics once invoked across an `#include`
   boundary, even though the preprocessed token stream is byte-identical
   to writing the call directly in the same physical file. Reproduced
   directly: appending `yas_aot_compile("...");` to the end of `aot.c`
   itself compiled and ran; the identical call one `#include "aot.c"`
   away, in a separate driver file, failed with `undefined variable
   'plan_fn'` (a file-scope static declared earlier in `aot.c`). `aot.c`
   and `aot/fib_main.c` no longer need to be concatenated into one
   physical source to work around this - see cccc's docs/MACROS.md,
   "Pre-parse macro declaration context", for the fixed rule. `make aot`
   passes the two files to cccc as separate positional arguments rather
   than using `#include` between them, for an unrelated reason: see point
   10 below.

8. **Fixed upstream (cccc #891).** cccc's own bundled headers
   (`implicit-reflection.h` -> `stdbool.h` etc.) used to be found only
   when cccc's process CWD was its own repository root, not relative to
   the compiled file's location or any `-I`/`--sysroot`/
   `--use-system-headers`/`--no-builtin-includes` combination. `make aot`
   used to work around this with `cd $(CCCC_DIR) && ./cccc ...` and
   absolute paths everywhere - both for the source files handed to cccc
   and for the `libyas`/`.ys` paths `dlopen`ed from inside the comptime
   pass. Now that cccc resolves its own headers from any CWD with no
   flags, `make aot` runs `$(CCCC)` directly from this repo's root with
   ordinary relative paths, and `aot_bind()`'s `dlopen("./libyas.dylib")`
   resolves correctly since that's now cccc's own CWD when it runs.

9. **Fixed upstream (cccc #891).** A plain runtime `#include <stdio.h>`
   used to collide cccc's own bundled `stdio.h` polyfill with the real
   system one ("typedef redefinition: `FILE`"/`fpos_t`"), with or without
   `--sysroot`/`--use-system-headers`. `aot/fib_main.c` now uses a plain
   `#include <stdio.h>` and `printf`s the untruncated `fib(30)` result
   directly, instead of squeezing `fib(13)` through the process exit code.

10. **`-c=native` re-emits a `#include`d file verbatim, which breaks if
    that file itself uses `#include @comptime`/`#include @shared` syntax
    (cccc #896, open).** `-c=native` serializes the runtime TU to a temp C
    file for the system compiler and re-emits the primary file's own
    `#include` directives into it verbatim. That's fine for an ordinary
    header, but `aot/aot.c` uses `#include @comptime` (cccc-only syntax,
    never valid to a system compiler) - so `aot/fib_main.c #include
    "aot.c"` compiled and ran fine as ordinary bytecode (cccc #890 is
    fixed), but broke under `-c=native` specifically, with the system
    compiler failing on `aot.c`'s `#include @comptime` lines once they
    were re-emitted into the temp file. Worked around by passing
    `aot/fib_main.c` and `aot/aot.c` to cccc as two separate positional
    source files instead of using `#include` between them at all (see the
    Makefile's `aot` target) - cccc merges and fully preprocesses both
    files itself before any native serialization happens, so no
    `#include` referencing a `@comptime`-using file ever reaches the
    system compiler. `$(AOT_MAIN)` must be listed first so its `#include
    <stdio.h>` is the one cccc captures and re-emits.

None of these affect the *generated* program - only the comptime tooling
used to produce it. Points 7-9 were the most disruptive for reusing this
pattern elsewhere while they lasted, since they constrained how any
cccc-based build had to be invoked, not just what comptime code could
contain; 8 and 9 no longer apply as of cccc #891, and 7 no longer applies
as of cccc #890. Point 10 is a narrower, `-c=native`-specific gap with a
one-line workaround (positional files instead of `#include`), not a
blocker for the design.

## Future work

See the tracker (labels `aot`, `compiler`) for the itemized backlog:
GC root/shadow-stack API so a future `Atom`-level lowering can hold live
values; `Atom`-level lowering beyond the numeric subset; closures and
higher-order functions; a standalone macro-expansion pass (macros are
currently runtime and per-environment - see
[ARCHITECTURE.md](ARCHITECTURE.md)); metamethod dispatch for lowered
arithmetic; `call/cc`/`try` under AOT; richer (per-expression, not
per-function) type inference; name-collision detection after mangling.

# Embedding

There is no `yas_state_t` and no init/destroy pair - all interpreter state
lives in file-scope globals (symbol/keyword tables, the allocation lists
the GC sweeps, the module registry, search paths, the current exception).
Only one yas instance can exist per process. Embedding is a small, fixed
sequence, taken from `src/main.c`:

```c
#include "lisp.h"

init_features();                 /* platform/feature list for #+/#- readers */

Atom env = env_create(nil);

/* Most programs will want (yas base) - car/cdr/define/lambda/etc: */
Atom yas_base = list_create(2, make_sym("YAS"), make_sym("BASE"));
Atom ignore;
module_import(list_create(1, yas_base), env, &ignore);
```

## Evaluating code

```c
int eval_expr(Atom expr, Atom env, Atom *result);
int eval_file(const char *path, Atom env, Atom *result, int case_insensitive);
int eval_source(const char *text, const char *path, Atom env, Atom *result);
void load_file(Atom env, const char *path);   /* silent unless a top-level form errors */
```

Both `eval_source()` and `load_file()`'s reader loop treat a top-level
`#+feature`/`#-feature` reader conditional that excludes an expression
(`Error_Skip`) as "skip this form, keep reading" rather than end-of-file,
so reader conditionals anywhere in the source are handled correctly by
either entry point.

## Registering native functions

There is no public "define a native function" convenience - `main.c`'s
own `reg_builtin`/`reg_sym` helpers are `static` to `module.c`. From
outside, define directly:

```c
int my_native(Atom args, Atom *result) {
    /* args is a list; unpack with car/cdr */
    *result = make_int(42);
    return Error_OK;
}

env_define(env, make_sym("MY-FUNC"), make_builtin(my_native));
```

Remember the reader uppercases symbols (see
[ARCHITECTURE.md](ARCHITECTURE.md)) - `make_sym("MY-FUNC")`, not
`make_sym("my-func")`, is what `(my-func)` in yas source will resolve to.

To expose a whole native module (like the built-in `yas/hash`, `yas/io`,
etc.), see `define_native_module()` and the `load_*_library()` functions
in `module.c` for the pattern: create an environment, `env_define()` each
export into it, register the export names in a `Hash`, and add an entry
to `native_modules[]`.

## Error handling

```c
typedef enum {
	Error_OK = 0, Error_Syntax, Error_Unbound,
	Error_Args, Error_Type, Error_Exception, Error_Skip
} Error;

extern Atom ctx_exception;   /* the thrown value, when eval_expr returns Error_Exception */
int raise_error(const char *type, const char *msg);
```

`Error_Skip` is reader-internal (see above) - a real evaluation never
returns it.

## I/O sandbox

For embedding untrusted scripts:

```c
void io_sandbox_init(IOSandboxMode mode);   /* UNRESTRICTED | RESTRICTED | DISABLED */
void io_sandbox_allow_path(const char *pattern);
void io_sandbox_deny_path(const char *pattern);
void io_sandbox_set_default(int allow);     /* default for unmatched paths in RESTRICTED */
```

Virtual filesystems (asset bundles, zip archives, etc.) plug in via
`io_register_provider(IOProvider *)` - an open/close/read/write/exists
vtable keyed by a URI prefix (e.g. `"asset://"`). See `lisp.h`'s
`IOProvider` struct.

## GC

There is no root-registration API for embedders (see
[ARCHITECTURE.md](ARCHITECTURE.md#gc)) - `eval_roots` is internal to
`eval.c`. Any `Atom` an embedder holds outside of a value already reachable
through `env` or the current evaluation is not protected from collection.
Keep host-side references short-lived, or re-derive them from `env` rather
than caching across calls that might allocate.

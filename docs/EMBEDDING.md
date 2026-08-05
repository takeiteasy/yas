# Embedding

There is no `yas_state_t` and no init/destroy pair - all interpreter state
lives in file-scope globals (symbol/keyword tables, the allocation lists
the GC sweeps, the module registry, search paths, the current exception).
Only one yas instance can exist per process. Embedding is a small, fixed
sequence, taken from `src/main.c`:

```c
#include "yas.h"

yas_init_features();             /* platform/feature list for #+/#- readers */

YasAtom env = yas_env_create(yas_nil);

/* Most programs will want (yas base) - car/cdr/define/lambda/etc: */
YasAtom yas_base = yas_list_create(2, yas_make_sym("YAS"), yas_make_sym("BASE"));
YasAtom ignore;
yas_module_import(yas_list_create(1, yas_base), env, &ignore);
```

## Evaluating code

```c
int yas_eval_expr(YasAtom expr, YasAtom env, YasAtom *result);
int yas_eval_file(const char *path, YasAtom env, YasAtom *result, int case_insensitive);
int yas_eval_source(const char *text, const char *path, YasAtom env, YasAtom *result);
void yas_load_file(YasAtom env, const char *path);   /* silent unless a top-level form errors */
```

Both `yas_eval_source()` and `yas_load_file()`'s reader loop treat a
top-level `#+feature`/`#-feature` reader conditional that excludes an
expression (`YAS_ERROR_SKIP`) as "skip this form, keep reading" rather
than end-of-file, so reader conditionals anywhere in the source are
handled correctly by either entry point.

## Registering native functions

There is no public "define a native function" convenience - `main.c`'s
own `reg_builtin`/`reg_sym` helpers are `static` to `module.c`. From
outside, define directly:

```c
int my_native(YasAtom args, YasAtom *result) {
    /* args is a list; unpack with yas_car/yas_cdr */
    *result = yas_make_int(42);
    return YAS_ERROR_OK;
}

yas_env_define(env, yas_make_sym("MY-FUNC"), yas_make_builtin(my_native));
```

Remember the reader uppercases symbols (see
[ARCHITECTURE.md](ARCHITECTURE.md)) - `yas_make_sym("MY-FUNC")`, not
`yas_make_sym("my-func")`, is what `(my-func)` in yas source will resolve
to.

To expose a whole native module (like the built-in `yas/hash`, `yas/io`,
etc.), see `yas_define_native_module()` and the `yas_load_*_library()`
functions in `module.c` for the pattern: create an environment,
`yas_env_define()` each export into it, register the export names in a
`YasHash`, and add an entry to `native_modules[]`.

## Error handling

```c
typedef enum {
	YAS_ERROR_OK = 0, YAS_ERROR_SYNTAX, YAS_ERROR_UNBOUND,
	YAS_ERROR_ARGS, YAS_ERROR_TYPE, YAS_ERROR_EXCEPTION, YAS_ERROR_SKIP
} YasError;

extern YasAtom yas_ctx_exception;   /* the thrown value, when yas_eval_expr returns YAS_ERROR_EXCEPTION */
int yas_raise_error(const char *type, const char *msg);
```

`YAS_ERROR_SKIP` is reader-internal (see above) - a real evaluation never
returns it.

## I/O sandbox

For embedding untrusted scripts:

```c
void yas_io_sandbox_init(YasIOSandboxMode mode);   /* UNRESTRICTED | RESTRICTED | DISABLED */
void yas_io_sandbox_allow_path(const char *pattern);
void yas_io_sandbox_deny_path(const char *pattern);
void yas_io_sandbox_set_default(int allow);     /* default for unmatched paths in RESTRICTED */
```

Virtual filesystems (asset bundles, zip archives, etc.) plug in via
`yas_io_register_provider(YasIOProvider *)` - an open/close/read/write/exists
vtable keyed by a URI prefix (e.g. `"asset://"`). See `yas.h`'s
`YasIOProvider` struct.

## GC

There is no root-registration API for embedders (see
[ARCHITECTURE.md](ARCHITECTURE.md#gc)) - `eval_roots` is internal to
`eval.c`. Any `YasAtom` an embedder holds outside of a value already
reachable through `env` or the current evaluation is not protected from
collection. Keep host-side references short-lived, or re-derive them from
`env` rather than caching across calls that might allocate.

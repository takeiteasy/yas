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
yas_gc_push_root(&env);          /* see GC.md - nothing else references env yet */

/* Most programs will want yas/base - car/cdr/define/lambda/etc: */
YasAtom import_args = yas_list_create(1, yas_make_sym("YAS/BASE"));
yas_gc_push_root(&import_args);
YasAtom ignore;
yas_module_import(import_args, env, &ignore);
yas_gc_pop_roots(1);              /* import_args no longer needed; env stays rooted */
```

Alternatively, call `yas_load_all_modules()` to eagerly load every
native module (C part plus its embedded Lisp layer) without going
through `import`.

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
functions in `module.c` for the pattern: call
`yas_define_native_module(name, parent_env, &env)` to register a module
record and get its environment (pass the `yas/base` module env as
`parent_env` so its bindings are visible to your module's users, or NIL
for a fresh root), then `yas_env_define()` each binding into it - no
separate export table is needed; a module's surface is simply what its
environment frame holds. Add an entry to `native_modules[]` with your
loader function.

## Error handling

```c
typedef enum {
	YAS_ERROR_OK = 0, YAS_ERROR_SYNTAX, YAS_ERROR_UNBOUND,
	YAS_ERROR_ARGS, YAS_ERROR_TYPE, YAS_ERROR_EXCEPTION, YAS_ERROR_SKIP
} YasError;

extern YasAtom yas_ctx_exception;   /* the thrown value, when yas_eval_expr returns YAS_ERROR_EXCEPTION */
int yas_raise_error(const char *type, const char *msg);
void yas_clear_exception(void);
```

`yas_ctx_exception` holds the value of the exception currently
propagating or being handled - it is set by explicit raises (`throw`,
`error`, `yas_raise_error()`) and cleared when a `catch` clause takes
ownership or a consumer reports the error. Treat it as valid only
immediately after a failing call: once you have read (printed, logged,
inspected) the value accompanying `YAS_ERROR_EXCEPTION`, call
`yas_clear_exception()` before evaluating anything else, or a later
handler with no exception object of its own could be handed your stale
one.

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

Full design in [GC.md](GC.md). The part that matters for embedders: any
`YasAtom` you hold that isn't already reachable through `env` (or the
current evaluation's live state) needs an explicit root before making any
call that might allocate:

```c
YasAtom env = yas_env_create(yas_nil);
yas_gc_push_root(&env);   /* nothing else references this yet */

yas_module_import(import_args, env, &ignore);   /* may allocate/collect */

/* ... use env for the embedding's lifetime ... */
yas_gc_pop_roots(1);      /* if/when env is no longer needed */
```

`main.c` does exactly this for its own top-level `env` and the argument
list passed to the initial `yas/base` import - see
[GC.md](GC.md#a-worked-example-the-bug-this-api-exists-to-prevent) for
the bug this fixed. If you don't root a value and don't otherwise keep it
reachable, don't assume it survives past the next allocating call - re-derive it from `env` instead of caching it.

```c
void yas_gc_push_root(YasAtom *slot);   /* register &slot as a root */
void yas_gc_pop_roots(int n);           /* pop the last n roots (LIFO) */
```

`(gc)` forces a full collection from Lisp, `(gc-stats)` reports live bytes
and phase - useful when testing that your own embedding roots things
correctly.

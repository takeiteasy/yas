#include "yas.h"
#include <pthread.h>

/* ══════════════════════════════════════════════════════════════════
   Global interpreter lock ("GIL").

   Interpreter state (GC heap/intern tables/root stacks/evaluator
   globals/module registry/sandbox config) is still process-global, so
   every public API entry that touches it takes this ONE recursive
   mutex for its whole body: threads get correctness immediately and
   true parallelism never (that is milestone 2's per-context isolation,
   which demotes this lock to context create/teardown bookkeeping).

   Nesting is the common case - yas_eval_expr() holds the lock while a
   builtin calls yas_cons(), which locks again - so acquisition goes
   through a thread-local depth counter first: the outermost enter()
   does the real pthread_mutex_lock(), everything nested below it just
   bumps an int. Same thread always owns the mutex across the nesting,
   so no other thread can interleave between depth changes; a plain
   non-recursive mutex plus this counter is therefore exactly as strong
   as PTHREAD_MUTEX_RECURSIVE without its per-call cost on the hot
   cons/vector paths.

   There is no setjmp/longjmp anywhere in yas (see eval.c), so leave()
   cannot be skipped by an unwind - every entry point releases on all
   of its return paths by construction.
   ══════════════════════════════════════════════════════════════════ */

static pthread_mutex_t gil = PTHREAD_MUTEX_INITIALIZER;
static YAS_TLS int gil_depth = 0;

void yas_gil_enter(void)
{
    if (gil_depth++ == 0)
        pthread_mutex_lock(&gil);
}

void yas_gil_leave(void)
{
    if (--gil_depth == 0)
        pthread_mutex_unlock(&gil);
}

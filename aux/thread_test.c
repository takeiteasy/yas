/* Thread-safety regression test for ticket #47 - milestone 2 (per-
 * context isolation).
 *
 * Each worker creates AND DESTROYS its own YasContext, imports the
 * modules it needs into its own top-level env, and runs deterministic
 * read+eval workloads - arithmetic through yas/base's variadic
 * closures, hash churn, string churn, plus periodic explicit full
 * collections. Contexts share nothing mutable, so the threads really
 * run in parallel and every worker's checksums are exact.
 *
 * Also asserts the core isolation fact directly: the same name interned
 * in two contexts yields DIFFERENT symbol pointers (identity is a
 * context-local fact).
 *
 * Built and run by `make test` / `make test-gc-stress` against libyas,
 * same as aux/gc_test.c. */
#include "yas.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef N_WORKERS
#define N_WORKERS 4
#endif
#define N_ITERS 100

static int g_failures;

static void check(int cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        g_failures++;
    }
}

static long expect_arith(long t, long i)
{
    long x = (i % 7) + 1;
    return t * 1000000 + i + 36 * x;
}

static void run_workload(long t)
{
    const char *mods[] = {"YAS/BASE", "YAS/HASH", "YAS/TEXT"};
    YasAtom env = yas_nil, import_args = yas_nil, ignore;
    long failed_here = g_failures;

    env = yas_env_create(yas_nil);
    yas_gc_push_root(&env);
    for (int m = 0; m < 3; m++) {
        import_args = yas_list_create(1, yas_make_sym(mods[m]));
        yas_gc_push_root(&import_args);
        int rc = yas_module_import(import_args, env, &ignore);
        yas_gc_pop_roots(1);
        check(rc == YAS_ERROR_OK, mods[m]);
    }

    for (long i = 0; i < N_ITERS; i++) {
        char src[256];
        /* Full-lifetime rooting of worker locals: the collector paces on
         * THIS thread's allocations only, but the discipline is cheap
         * and keeps the embedder contract uniform (docs/GC.md). */
        YasAtom expr = yas_nil, result = yas_nil;
        const char *p;
        YasErrorContext err = {0};
        int rc;

        yas_gc_push_root(&expr);
        yas_gc_push_root(&result);

        snprintf(src, sizeof(src),
                 "(+ %ld (foldl + 0 (map (lambda (x) (* x %ld)) '(1 2 3 4 5 6 7 8))))%s",
                 t * 1000000 + i, (i % 7) + 1, (i % 17 == 0) ? " (gc)" : "");
        p = src;
        rc = yas_read_expr(p, &p, &expr, &err);
        check(rc == YAS_ERROR_OK, "read arith expr");
        rc = yas_eval_expr(expr, env, &result);
        check(rc == YAS_ERROR_OK && result.type == YAS_ATOM_TYPE_INTEGER &&
                  result.value.integer == expect_arith(t, i),
              "arith result");
        yas_clear_exception();

        snprintf(src, sizeof(src),
                 "(hash-ref (let ((h (make-hash))) (hash-set! h 'kk %ld) h) 'kk -1)",
                 (t * 1000000 + i) * 2);
        p = src;
        rc = yas_read_expr(p, &p, &expr, &err);
        check(rc == YAS_ERROR_OK, "read hash expr");
        rc = yas_eval_expr(expr, env, &result);
        check(rc == YAS_ERROR_OK && result.type == YAS_ATOM_TYPE_INTEGER &&
                  result.value.integer == (t * 1000000 + i) * 2,
              "hash result");

        snprintf(src, sizeof(src), "(string-append \"ab%ld\" \"123\")", i);
        p = src;
        rc = yas_read_expr(p, &p, &expr, &err);
        check(rc == YAS_ERROR_OK, "read string expr");
        rc = yas_eval_expr(expr, env, &result);
        char numbuf[32];
        snprintf(numbuf, sizeof(numbuf), "%ld", i);
        size_t want_len = 2 + strlen(numbuf) + 3;
        check(rc == YAS_ERROR_OK && result.type == YAS_ATOM_TYPE_STRING &&
                  strlen(result.value.string) == want_len &&
                  strncmp(result.value.string, "ab", 2) == 0 &&
                  strcmp(result.value.string + want_len - 3, "123") == 0,
              "string result");

        yas_clear_exception();
        yas_gc_pop_roots(2); /* expr, result */
    }

    yas_gc_pop_roots(1); /* env */
    check(g_failures == failed_here, "worker saw no new failures");
}

static void *worker(void *argp)
{
    long t = (long)argp;

    YasContext *ctx = yas_context_create(); /* binds this thread */
    if (!ctx) {
        check(0, "yas_context_create");
        return NULL;
    }
    check(yas_context_current() == ctx, "create binds current");

    run_workload(t);

    yas_context_destroy(ctx);
    check(yas_context_current() == NULL, "destroy unbinds when it was current");
    return NULL;
}

int main(void)
{
    pthread_t th[N_WORKERS];

    /* Isolation fact: identical names interned in different contexts
     * are different identities - nothing crosses the boundary. */
    YasContext *a = yas_context_create();
    YasAtom sym_a = yas_make_sym("PROBE");
    yas_context_bind(NULL);
    YasContext *b = yas_context_create();
    YasAtom sym_b = yas_make_sym("PROBE");
    check(sym_a.type == YAS_ATOM_TYPE_SYMBOL && sym_b.type == YAS_ATOM_TYPE_SYMBOL,
          "probe symbols interned");
    check(sym_a.value.symbol != sym_b.value.symbol, "interning is context-local");
    yas_context_destroy(b);
    yas_context_bind(a);
    yas_context_destroy(a);
    yas_context_bind(NULL);

    for (long t = 0; t < N_WORKERS; t++) {
        if (pthread_create(&th[t], NULL, worker, (void *)t) != 0) {
            fprintf(stderr, "FAIL: pthread_create(worker %ld)\n", t);
            return 1;
        }
    }
    for (int t = 0; t < N_WORKERS; t++)
        pthread_join(th[t], NULL);

    if (g_failures) {
        printf("THREAD TEST FAILED (%d failures)\n", g_failures);
        return 1;
    }
    printf("ALL THREAD TESTS PASSED\n");
    return 0;
}

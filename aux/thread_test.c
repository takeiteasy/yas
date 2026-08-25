/* Thread-safety regression test for ticket #47 milestone 1 (the GIL).
 *
 * N worker pthreads hammer the plain-C embedder API against ONE shared
 * top-level environment while a dedicated thread forces full collection
 * cycles in parallel - all of it process-global state today, so any
 * missed lock shows up as torn heaps, corrupted intern tables or wrong
 * arithmetic almost immediately. Every worker's expressions are built
 * from its own thread number and iteration counter and return values
 * the test can recompute exactly in C, so the final checksums are
 * deterministic regardless of interleaving (which the GIL serializes
 * completely anyway - this milestone buys correctness, not parallelism;
 * milestone 2's per-context isolation gets the real concurrency).
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
#define N_ITERS 150

static YasAtom g_env;
static volatile int g_stop = 0;
static int g_failures = 0;

static void check(int cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        g_failures++;
    }
}

static void check_long(int cond, const char *what, long t, long i, long got, long want)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s (t=%ld i=%ld got=%ld want=%ld)\n", what, t, i, got, want);
        g_failures++;
    }
}

/* The three expression families below return integers recomputable in C:
 *   arith: t*1000000 + i + 36*X          where X = (i % 7) + 1
 *   hash:  (t*1000000 + i) * 2
 *   str:   5 + digits(i)
 */
static long expect_arith(long t, long i)
{
    long x = (i % 7) + 1;
    return t * 1000000 + i + 36 * x;
}

static void *worker(void *argp)
{
    long t = (long)argp;
    YasAtom root = g_env;

    for (long i = 0; i < N_ITERS; i++) {
        char src[256];
        /* Milestone-1 embedder contract (docs/EMBEDDING.md "Threads"):
         * with the GIL, OTHER threads' collections run between our own
         * locked calls - so every atom local is rooted for its WHOLE
         * lifetime here, not merely across our next allocating call. */
        YasAtom expr = yas_nil, result = yas_nil;
        const char *p;
        YasErrorContext err = {0};
        int rc;

        yas_gc_push_root(&root);
        yas_gc_push_root(&expr);
        yas_gc_push_root(&result);

        snprintf(src, sizeof(src),
                 "(+ %ld (foldl + 0 (map (lambda (x) (* x %ld)) '(1 2 3 4 5 6 7 8))))",
                 t * 1000000 + i, (i % 7) + 1);
        p = src;
        rc = yas_read_expr(p, &p, &expr, &err);
        check(rc == YAS_ERROR_OK, "read arith expr");
        rc = yas_eval_expr(expr, root, &result);
        check_long(rc == YAS_ERROR_OK && result.type == YAS_ATOM_TYPE_INTEGER &&
                       result.value.integer == expect_arith(t, i),
                   "arith result", t, i,
                   rc == YAS_ERROR_OK && result.type == YAS_ATOM_TYPE_INTEGER ? (long)result.value.integer : rc,
                   expect_arith(t, i));

        snprintf(src, sizeof(src),
                 "(hash-ref (let ((h (make-hash))) (hash-set! h 'kk %ld) h) 'kk -1)",
                 (t * 1000000 + i) * 2);
        p = src;
        rc = yas_read_expr(p, &p, &expr, &err);
        check(rc == YAS_ERROR_OK, "read hash expr");
        rc = yas_eval_expr(expr, root, &result);
        check_long(rc == YAS_ERROR_OK && result.type == YAS_ATOM_TYPE_INTEGER &&
                       result.value.integer == (t * 1000000 + i) * 2,
                   "hash result", t, i,
                   rc == YAS_ERROR_OK && result.type == YAS_ATOM_TYPE_INTEGER ? (long)result.value.integer : rc,
                   (t * 1000000 + i) * 2);

        snprintf(src, sizeof(src), "(string-append \"ab%ld\" \"123\")", i);
        p = src;
        rc = yas_read_expr(p, &p, &expr, &err);
        check(rc == YAS_ERROR_OK, "read string expr");
        rc = yas_eval_expr(expr, root, &result);
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
        yas_gc_pop_roots(1); /* root */
    }
    return NULL;
}

static void *gc_hammer(void *arg)
{
    (void)arg;
    while (!g_stop) {
        yas_gc();
        if (yas_gc_allocated() == 0 && yas_gc_phase() != 0)
            check(0, "gc phase/allocated invariant");
    }
    return NULL;
}

int main(void)
{
    pthread_t workers[N_WORKERS], hammer;
    YasAtom import_args, ignore;

    yas_init_features();

    g_env = yas_env_create(yas_nil);
    yas_gc_push_root(&g_env); /* process-lifetime, like main.c */

    const char *mods[] = {"YAS/BASE", "YAS/HASH", "YAS/TEXT"};
    for (int m = 0; m < 3; m++) {
        import_args = yas_list_create(1, yas_make_sym(mods[m]));
        yas_gc_push_root(&import_args);
        int rc = yas_module_import(import_args, g_env, &ignore);
        yas_gc_pop_roots(1);
        check(rc == YAS_ERROR_OK, mods[m]);
    }

    if (!getenv("YAS_THREAD_TEST_NO_HAMMER")) {
        if (pthread_create(&hammer, NULL, gc_hammer, NULL) != 0) {
            fprintf(stderr, "FAIL: pthread_create(gc_hammer)\n");
            return 1;
        }
    }
    for (long t = 0; t < N_WORKERS; t++) {
        if (pthread_create(&workers[t], NULL, worker, (void *)t) != 0) {
            fprintf(stderr, "FAIL: pthread_create(worker %ld)\n", t);
            return 1;
        }
    }
    for (int t = 0; t < N_WORKERS; t++)
        pthread_join(workers[t], NULL);
    if (!getenv("YAS_THREAD_TEST_NO_HAMMER")) {
        g_stop = 1;
        pthread_join(hammer, NULL);
    }

    yas_clear_exception();
    if (g_failures) {
        printf("THREAD TEST FAILED (%d failures)\n", g_failures);
        return 1;
    }
    printf("ALL THREAD TESTS PASSED\n");
    return 0;
}

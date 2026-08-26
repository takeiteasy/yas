/* Neutral-marshaler round-trip tests (cross-context value transfer,
 * docs/CONCURRENCY.md "Value transfer").
 *
 * Deterministic section: builds a payload graph in context A covering
 * the transferable type matrix - scalars, bit-exact floats (denormal /
 * +inf / -0.0 compared as raw bits), UTF-8 strings, bytevectors,
 * vec/mat components, dotted pairs, vectors, hashes with a metatable,
 * empty containers, shared substructure (one source pair referenced
 * twice must rebuild as ONE pair, verified by pointer identity and a
 * through-both-references mutation) and a cdr-cycle - marshals it,
 * DESTROYS context A, rebuilds in a fresh context B and verifies
 * content, re-interned-symbol identity and structure. Then asserts the
 * rejected half of the type matrix raises a catchable type-error
 * naming the type (builtin procedure, closure, port, environment).
 *
 * Deep-list section: a 10k-deep chain round-trips with an exact
 * checksum (main thread - the recursive walk shares the interpreter's
 * C-stack depth assumptions).
 *
 * Concurrency section: N workers each build a worker-specific graph in
 * their own context, marshal it, destroy that context, rebuild in a
 * fresh one and verify exact checksums - proving trees are independent
 * of any context's lifetime while collections step concurrently.
 *
 * Rooting follows aux/thread_test.c's embedder discipline: every local
 * holding a heap object is registered before any further allocating
 * call, so this harness itself is clean under YAS_GC_STRESS=1.
 *
 * Built and run by `make test` / `make test-gc-stress` against libyas,
 * alongside aux/thread_test.c. */
#include "yas.h"
#include "marshal.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef N_WORKERS
#define N_WORKERS 4
#endif
#ifndef W_ITERS
#define W_ITERS 25
#endif
#define DEEP_LEN 10000

static int g_failures;

static void check(int cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        g_failures++;
    }
}

/* Raw-bit constructor for awkward doubles (no libm dependency). */
static double bits_to_double(uint64_t bits)
{
    double d;
    memcpy(&d, &bits, sizeof(d));
    return d;
}

static int atom_is_string(YasAtom a, const char *s)
{
    return a.type == YAS_ATOM_TYPE_STRING && strcmp(a.value.string, s) == 0;
}

/* yas_vec4_t may be a SIMD vector - copy out to plain floats. */
static void vec_lanes(YasAtom v, float lanes[4])
{
    memcpy(lanes, v.value.vec, sizeof(float) * 4);
}

/* ── Deterministic section ─────────────────────────────────────────── */

static void run_deterministic(void)
{
    YasContext *a = yas_context_create(); /* binds */

    /* Create-and-root every heap value BEFORE assembling containers -
     * an unrooted fresh string inside a yas_list_create() argument list
     * would be sweepable mid-construction under GC stress. */
    YasAtom shared = yas_cons(yas_make_int(42), yas_nil);
    yas_gc_push_root(&shared);
    YasAtom parent = yas_list_create(2, shared, shared);
    yas_gc_push_root(&parent);
    YasAtom ring = yas_cons(yas_make_int(1), yas_nil);
    yas_gc_push_root(&ring);
    yas_set_cdr(ring, ring);
    YasAtom dotted_inner = yas_list_create(2, yas_make_int(1), yas_make_int(2));
    yas_gc_push_root(&dotted_inner);
    YasAtom dotted = yas_cons(dotted_inner, yas_make_int(9));
    yas_gc_push_root(&dotted);
    YasAtom s_invec = yas_make_string("in-vec");
    yas_gc_push_root(&s_invec);
    YasAtom inner_vec = yas_list_create(3, yas_make_int(7),
                                        yas_make_sym("VECSYM"), s_invec);
    yas_gc_push_root(&inner_vec);
    YasAtom mt = yas_make_hash();
    yas_gc_push_root(&mt);
    YasAtom mm_val = yas_make_string("mm-add");
    yas_gc_push_root(&mm_val);
    yas_hash_set(mt, yas_make_keyword("ADD"), mm_val);
    YasAtom h = yas_make_hash();
    yas_gc_push_root(&h);
    yas_hash_set(h, yas_make_int(1), yas_make_string("one"));
    yas_hash_set(h, yas_make_sym("KEY"), yas_make_int(22));
    yas_hash_set(h, yas_make_string("skey"), yas_make_float(2.5));
    yas_hash_set(h, yas_make_char('c'), yas_make_int(33));
    h.value.hash->metatable = mt.value.hash;
    yas_gc_write_barrier(h, mt);
    YasAtom v3 = yas_make_vec3(1.5f, -2.25f, 3.125f);
    yas_gc_push_root(&v3);
    float mat_vals[16] = {1.f, 2.f, 3.f, 4.f, 5.f, 6.f, 7.f, 8.f,
                          9.f, 10.f, 11.f, 12.f, 13.f, 14.f, 15.f, 16.f};
    YasAtom m4 = yas_make_mat4_from_floats(mat_vals);
    yas_gc_push_root(&m4);
    unsigned char bv_bytes[4] = {0, 255, 128, 7};
    YasAtom bv = yas_make_bytevector_from_data(bv_bytes, 4);
    yas_gc_push_root(&bv);
    YasAtom estr =
        yas_make_string("h\xc3\xa9llo \"quoted\" back\\slash \xe2\x9c\x93");
    yas_gc_push_root(&estr);

    double awkward_bits[3];
    YasAtom fdenorm = yas_make_float(bits_to_double(1)); /* smallest denormal */
    memcpy(&awkward_bits[0], &fdenorm.value.floatval, 8);
    YasAtom finf = yas_make_float(bits_to_double(0x7ff0000000000000ULL));
    memcpy(&awkward_bits[1], &finf.value.floatval, 8);
    YasAtom fnegz = yas_make_float(bits_to_double(0x8000000000000000ULL));
    memcpy(&awkward_bits[2], &fnegz.value.floatval, 8);

    YasAtom empty_vec = yas_make_vector(0, yas_nil);
    yas_gc_push_root(&empty_vec);
    YasAtom empty_hash = yas_make_hash();
    yas_gc_push_root(&empty_hash);

    /* Spine order is load-bearing - the checks below index it. */
    YasAtom payload = yas_list_create(
        21, yas_make_int(123), yas_make_int(-456), yas_make_float(0.5),
        fdenorm, finf, fnegz, yas_make_char('A'), yas_make_char(0x1F600),
        yas_make_sym("MARSHAL_PROBE"), yas_make_keyword("KW_PROBE"), estr,
        bv, v3, m4, dotted, parent, ring, empty_vec, inner_vec, h,
        empty_hash);
    yas_gc_push_root(&payload);

    struct YasMarshalNode *tree = yas_marshal(payload);
    check(tree != NULL, "payload marshals");

    yas_gc_pop_roots(16); /* source graph */
    yas_context_destroy(a); /* source graph gone; the tree must stand alone */

    YasContext *b = yas_context_create();
    YasAtom out = yas_nil;
    yas_gc_push_root(&out);
    if (!tree || yas_unmarshal(tree, &out) != YAS_ERROR_OK) {
        check(0, "payload unmarshals");
    } else {
        YasAtom e;
        e = yas_list_get(out, 0);
        check(e.type == YAS_ATOM_TYPE_INTEGER && e.value.integer == 123, "int");
        e = yas_list_get(out, 1);
        check(e.type == YAS_ATOM_TYPE_INTEGER && e.value.integer == -456,
              "negative int");
        e = yas_list_get(out, 2);
        check(e.type == YAS_ATOM_TYPE_FLOAT && e.value.floatval == 0.5, "float");
        e = yas_list_get(out, 3);
        check(e.type == YAS_ATOM_TYPE_FLOAT &&
                  memcmp(&e.value.floatval, &awkward_bits[0], 8) == 0,
              "denormal float bit-exact");
        e = yas_list_get(out, 4);
        check(e.type == YAS_ATOM_TYPE_FLOAT &&
                  memcmp(&e.value.floatval, &awkward_bits[1], 8) == 0,
              "+inf float bit-exact");
        e = yas_list_get(out, 5);
        check(e.type == YAS_ATOM_TYPE_FLOAT &&
                  memcmp(&e.value.floatval, &awkward_bits[2], 8) == 0,
              "-0.0 float bit-exact");
        e = yas_list_get(out, 6);
        check(e.type == YAS_ATOM_TYPE_CHAR && e.value.character == 'A', "char");
        e = yas_list_get(out, 7);
        check(e.type == YAS_ATOM_TYPE_CHAR && e.value.character == 0x1F600,
              "wide char");

        e = yas_list_get(out, 8);
        check(e.type == YAS_ATOM_TYPE_SYMBOL &&
                  strcmp(e.value.symbol, "MARSHAL_PROBE") == 0 &&
                  e.value.symbol == yas_make_sym("MARSHAL_PROBE").value.symbol,
              "symbol re-interned to one instance");
        e = yas_list_get(out, 9);
        check(e.type == YAS_ATOM_TYPE_KEYWORD &&
                  e.value.keyword == yas_make_keyword("KW_PROBE").value.keyword,
              "keyword re-interned");

        check(atom_is_string(
                  yas_list_get(out, 10),
                  "h\xc3\xa9llo \"quoted\" back\\slash \xe2\x9c\x93"),
              "utf8/escaped string bytes verbatim");

        e = yas_list_get(out, 11);
        check(e.type == YAS_ATOM_TYPE_BYTEVECTOR &&
                  yas_bytevector_length(e) == 4 &&
                  yas_bytevector_ref(e, 0) == 0 &&
                  yas_bytevector_ref(e, 1) == 255 &&
                  yas_bytevector_ref(e, 2) == 128 &&
                  yas_bytevector_ref(e, 3) == 7,
              "bytevector bytes verbatim");

        e = yas_list_get(out, 12);
        {
            float lanes[4];
            vec_lanes(e, lanes);
            check(e.type == YAS_ATOM_TYPE_VEC3 && lanes[0] == 1.5f &&
                      lanes[1] == -2.25f && lanes[2] == 3.125f,
                  "vec3 components");
        }
        e = yas_list_get(out, 13);
        check(e.type == YAS_ATOM_TYPE_MAT4 &&
                  memcmp(e.value.mat4->columns, mat_vals, sizeof(float) * 16) == 0,
              "mat4 components bit-exact");

        e = yas_list_get(out, 14);
        check(e.type == YAS_ATOM_TYPE_PAIR &&
                  e.value.pair->atom[1].type == YAS_ATOM_TYPE_INTEGER &&
                  e.value.pair->atom[1].value.integer == 9 &&
                  e.value.pair->atom[0].type == YAS_ATOM_TYPE_PAIR &&
                  yas_car(yas_car(e)).value.integer == 1 &&
                   yas_car(yas_cdr(yas_car(e))).value.integer == 2,
              "dotted pair shape preserved");

        e = yas_list_get(out, 15);
        {
            YasAtom first = yas_car(e), second = yas_car(yas_cdr(e));
            check(first.type == YAS_ATOM_TYPE_PAIR &&
                      first.value.pair != NULL &&
                      first.value.pair == second.value.pair,
                  "shared substructure rebuilt as one object");
            yas_set_car(first, yas_make_int(999));
            check(second.value.pair->atom[0].value.integer == 999,
                  "mutation visible through both references");
        }

        e = yas_list_get(out, 16);
        check(e.type == YAS_ATOM_TYPE_PAIR &&
                  yas_cdr(e).value.pair == e.value.pair,
              "cdr-cycle survives");

        e = yas_list_get(out, 17);
        check(e.type == YAS_ATOM_TYPE_VECTOR && yas_vector_length(e) == 0,
              "empty vector");

        e = yas_list_get(out, 18);
        check(e.type == YAS_ATOM_TYPE_PAIR && yas_listp(e) &&
                  yas_list_get(e, 0).value.integer == 7 &&
                  strcmp(yas_list_get(e, 1).value.symbol, "VECSYM") == 0 &&
                  atom_is_string(yas_list_get(e, 2), "in-vec"),
              "inner list contents");

        e = yas_list_get(out, 19);
        check(e.type == YAS_ATOM_TYPE_HASH && yas_hash_size(e) == 4,
              "hash row count");
        {
            YasAtom k = yas_make_int(1);
            check(atom_is_string(yas_hash_ref(e, k, yas_nil), "one"),
                  "hash int key");
            k = yas_make_sym("KEY");
            check(yas_hash_ref(e, k, yas_nil).value.integer == 22,
                  "hash sym key");
            k = yas_make_string("skey");
            check(yas_hash_ref(e, k, yas_nil).value.floatval == 2.5,
                  "hash str key");
            k = yas_make_char('c');
            check(yas_hash_ref(e, k, yas_nil).value.integer == 33,
                  "hash char key");
            check(atom_is_string(yas_get_metamethod(e, "add"), "mm-add"),
                  "metatable transferred");
        }

        e = yas_list_get(out, 20);
        check(e.type == YAS_ATOM_TYPE_HASH && yas_hash_size(e) == 0,
              "empty hash");

        if (tree)
            yas_marshal_free(tree);
    }
    yas_gc_pop_roots(1); /* out */
    yas_context_destroy(b);
}

/* ── Rejected types ────────────────────────────────────────────────── */

static int raised_type_error(void)
{
    /* yas_raise_error stores the exception as a (type message) pair;
     * the type symbol keeps the spelling passed to raise (this path
     * bypasses the reader, so no uppercasing applies). */
    YasAtom ex = yas_ctx_exception;
    int ok = !yas_nilp(ex) && ex.type == YAS_ATOM_TYPE_PAIR &&
             ex.value.pair->atom[0].type == YAS_ATOM_TYPE_SYMBOL &&
             strcmp(ex.value.pair->atom[0].value.symbol, "type-error") == 0;
    yas_clear_exception();
    return ok;
}

static void run_rejected_types(void)
{
    YasContext *ctx = yas_context_create();

    YasAtom bad = yas_make_builtin(yas_builtin_car);
    check(yas_marshal(bad) == NULL && raised_type_error(), "builtin rejected");

    /* A closure, via a minimal base-env eval. */
    YasAtom env = yas_env_create(yas_nil);
    YasAtom import_args = yas_list_create(1, yas_make_sym("YAS/BASE"));
    YasAtom ignore, expr = yas_nil, result = yas_nil;
    YasErrorContext err = {0};
    yas_gc_push_root(&env);
    yas_gc_push_root(&import_args);
    yas_gc_push_root(&expr);
    yas_gc_push_root(&result);
    if (yas_module_import(import_args, env, &ignore) != YAS_ERROR_OK) {
        check(0, "import yas/base for closure test");
    } else {
        const char *src = "(lambda (x) x)";
        check(yas_read_expr(src, &src, &expr, &err) == YAS_ERROR_OK,
              "read lambda");
        check(yas_eval_expr(expr, env, &result) == YAS_ERROR_OK, "eval lambda");
        check(result.type == YAS_ATOM_TYPE_CLOSURE, "got closure");
        check(yas_marshal(result) == NULL && raised_type_error(),
              "closure rejected");
    }
    yas_clear_exception();
    yas_gc_pop_roots(3); /* import_args, expr, result; retain env below */

    YasAtom port = yas_make_port(NULL, "<test>", 1, 0, 0);
    yas_gc_push_root(&port);
    check(yas_marshal(port) == NULL && raised_type_error(), "port rejected");
    yas_gc_pop_roots(1);

    bad = env;
    check(yas_marshal(bad) == NULL && raised_type_error(),
          "environment rejected");

    yas_gc_pop_roots(1); /* env */
    yas_context_destroy(ctx);
}

/* ── Deep structure ────────────────────────────────────────────────── */

static void run_deep_list(void)
{
    YasContext *a = yas_context_create();

    YasAtom lst = yas_nil;
    yas_gc_push_root(&lst);
    for (long i = DEEP_LEN; i > 0; i--)
        lst = yas_cons(yas_make_int(i), lst);

    struct YasMarshalNode *tree = yas_marshal(lst);
    check(tree != NULL, "deep list marshals");
    long expected_sum = (long)DEEP_LEN * (DEEP_LEN + 1) / 2;
    yas_gc_pop_roots(1); /* lst */
    yas_context_destroy(a);

    YasContext *b = yas_context_create();
    YasAtom out = yas_nil;
    yas_gc_push_root(&out);
    if (!tree || yas_unmarshal(tree, &out) != YAS_ERROR_OK) {
        check(0, "deep list unmarshals");
    } else {
        long sum = 0, n = 0;
        for (YasAtom p = out; p.type == YAS_ATOM_TYPE_PAIR; p = yas_cdr(p)) {
            sum += yas_car(p).value.integer;
            n++;
        }
        check(n == DEEP_LEN && sum == expected_sum, "deep list round-trip");
    }
    if (tree)
        yas_marshal_free(tree);
    yas_gc_pop_roots(1); /* out */
    yas_context_destroy(b);
}

/* ── Concurrent section ────────────────────────────────────────────── */

static void *worker(void *argp)
{
    long t = (long)argp;
    long failed_here = g_failures;

    for (int iter = 0; iter < W_ITERS; iter++) {
        YasContext *a = yas_context_create();
        if (!a) {
            check(0, "worker context create");
            return NULL;
        }

        /* Graph: hash of 8 rows keyed by computed integers, each row's
         * value a pair sharing ONE common tail cell (DAG), plus a small
         * cdr-ring. */
        YasAtom shared_tail = yas_cons(yas_make_int((int)(t * 777)), yas_nil);
        YasAtom map = yas_make_hash();
        YasAtom ring = yas_cons(yas_make_sym("RING"), yas_nil);
        yas_set_cdr(ring, ring);
        yas_gc_push_root(&shared_tail);
        yas_gc_push_root(&map);
        yas_gc_push_root(&ring);
        long want_sum = 0;
        for (int i = 0; i < 8; i++) {
            long kv = t * 10000 + iter * 100 + i;
            YasAtom val = yas_cons(yas_make_int(kv * 3), shared_tail);
            yas_gc_push_root(&val);
            yas_hash_set(map, yas_make_int(kv), val);
            yas_gc_pop_roots(1);
            want_sum += kv * 3;
        }
        YasAtom bundle = yas_list_create(2, map, ring);
        yas_gc_push_root(&bundle);

        struct YasMarshalNode *tree = yas_marshal(bundle);

        yas_gc_pop_roots(4); /* bundle, ring, map, shared_tail */
        yas_context_destroy(a); /* kill the source mid-life of the tree */

        YasContext *b = yas_context_create();
        YasAtom out = yas_nil;
        yas_gc_push_root(&out);
        if (!tree || yas_unmarshal(tree, &out) != YAS_ERROR_OK) {
            check(0, "worker unmarshal");
        } else {
            YasAtom rmap = yas_car(out), rring = yas_car(yas_cdr(out));
            check(rmap.type == YAS_ATOM_TYPE_HASH && rmap.value.hash->size == 8,
                  "worker rows");
            long got_sum = 0;
            struct YasHash *h = rmap.value.hash;
            for (size_t i = 0; i < h->size; i++)
                got_sum += yas_car(h->values[i]).value.integer;
            check(got_sum == want_sum, "worker checksum");
            check(rring.type == YAS_ATOM_TYPE_PAIR &&
                      yas_cdr(rring).value.pair == rring.value.pair,
                  "worker ring");
            /* Sharing: every row's rebuilt tail is the same pair. */
            int all_share = h->size > 0;
            for (size_t i = 1; all_share && i < h->size; i++)
                all_share = yas_cdr(h->values[0]).value.pair ==
                            yas_cdr(h->values[i]).value.pair;
            check(all_share, "worker DAG sharing preserved");
        }
        if (tree)
            yas_marshal_free(tree);
        yas_gc_pop_roots(1); /* out */
        yas_context_destroy(b);

    }

    check(g_failures == failed_here, "worker saw no new failures");
    return NULL;
}

int main(void)
{
    run_deterministic();
    run_rejected_types();
    run_deep_list();

    pthread_t th[N_WORKERS];
    for (long t = 0; t < N_WORKERS; t++) {
        if (pthread_create(&th[t], NULL, worker, (void *)t) != 0) {
            fprintf(stderr, "FAIL: pthread_create(worker %ld)\n", t);
            return 1;
        }
    }
    for (int t = 0; t < N_WORKERS; t++)
        pthread_join(th[t], NULL);

    if (g_failures) {
        printf("MARSHAL TEST FAILED (%d failures)\n", g_failures);
        return 1;
    }
    printf("ALL MARSHAL TESTS PASSED\n");
    return 0;
}

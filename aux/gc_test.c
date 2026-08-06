/* gc_test.c - C-level regression tests for the unified incremental
 * tricolor GC (src/data.c) and the root-registration API (yas.h).
 *
 * Exercises what suite.ys can't reach: the plain-C API a native builtin
 * or embedder uses (yas_gc_push_root/pop_roots), and collector internals
 * that need many objects and controlled stepping to hit (the write
 * barrier firing mid-cycle, the allocate-during-sweep hazard). See
 * docs/GC.md for the design these tests are checking.
 *
 * Build: cc -std=c99 -Wall -D_GNU_SOURCE -iquote src -o build/gc_test \
 *          tests/gc_test.c -L. -lyas -Wl,-rpath,. (see the Makefile's
 * `test-gc` target)
 */
#include "yas.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		failures++; \
	} \
} while (0)

/* Allocate n throwaway conses to pressure the allocator without rooting
 * them - they should all be reclaimable. */
static void churn(int n)
{
	for (int i = 0; i < n; i++)
		(void)yas_cons(yas_make_int(i), yas_nil);
}

/* 1. A root actually protects its slot's object across real collection:
 * without yas_gc_push_root, this string would be freed once enough
 * unrelated allocation crosses the threshold and a full cycle runs. */
static void test_root_protection(void)
{
	YasAtom slot = yas_make_string("hello-root-test");
	yas_gc_push_root(&slot);

	churn(50000);
	yas_gc();
	yas_gc();
	yas_gc();

	CHECK(slot.type == YAS_ATOM_TYPE_STRING);
	CHECK(strcmp(slot.value.string, "hello-root-test") == 0);

	yas_gc_pop_roots(1);
}

/* 2. Push/pop are a balanced stack. */
static void test_root_balance(void)
{
	int start = yas_gc_root_count();

	YasAtom a = yas_make_int(1);
	YasAtom b = yas_make_int(2);
	YasAtom c = yas_make_int(3);
	yas_gc_push_root(&a);
	yas_gc_push_root(&b);
	yas_gc_push_root(&c);
	CHECK(yas_gc_root_count() == start + 3);

	yas_gc_pop_roots(3);
	CHECK(yas_gc_root_count() == start);
}

/* Build a long rooted list, forcing a cycle spanning multiple
 * yas_gc_step() calls (each bounded to YAS_GC_STEP_LIMIT work items),
 * so the test can catch the collector mid-phase instead of it always
 * completing within a single yas_gc_step() call. */
static YasAtom build_long_rooted_list(int n)
{
	YasAtom list = yas_nil;
	for (int i = 0; i < n; i++)
		list = yas_cons(yas_make_int(i), list);
	return list;
}

/* 3. Write barrier fires while the cycle is in MARKING: a container the
 * root marker already reached (BLACK) gets a fresh, otherwise-unreachable
 * child attached mid-cycle. Without the barrier, gc_scan_object() has
 * already looked at this container's old fields and won't revisit it, so
 * the child - genuinely unreached by this cycle's root trace - would be
 * swept as garbage even though it's now live. */
static void test_write_barrier_during_marking(void)
{
	YasAtom filler = build_long_rooted_list(5000);
	yas_gc_push_root(&filler);

	YasAtom container = yas_cons(yas_make_int(0), yas_nil);
	yas_gc_push_root(&container);

	yas_gc_start();
	CHECK(yas_gc_phase() != 0);

	/* Advance a few bounded steps without necessarily finishing -
	 * the filler list is large enough that MARKING alone should take
	 * more than one step. */
	int phase = yas_gc_phase();
	for (int i = 0; i < 3 && phase == 1; i++)
		yas_gc_step();

	YasAtom child = yas_make_string("rescued-during-marking");
	yas_set_cdr(container, child);

	while (yas_gc_phase() != 0)
		yas_gc_step();

	CHECK(yas_cdr(container).type == YAS_ATOM_TYPE_STRING);
	CHECK(strcmp(yas_cdr(container).value.string, "rescued-during-marking") == 0);

	yas_gc_pop_roots(2);
}

/* 4. Write barrier fires while the cycle is in SWEEPING - the case
 * data.c's yas_gc_step() specifically drains gray for (see the comment
 * there: "MARKING is over, so nothing else will ever pop and scan that
 * re-grayed container"). Without that drain, this is exactly the
 * scenario that either trips the gc_gray_count==0 assertion at cycle end
 * or, worse, silently frees a live child. */
static void test_write_barrier_during_sweeping(void)
{
	YasAtom filler = build_long_rooted_list(5000);
	yas_gc_push_root(&filler);

	YasAtom container = yas_cons(yas_make_int(0), yas_nil);
	yas_gc_push_root(&container);

	yas_gc_start();
	/* Drain MARKING (and its atomic root re-mark) so we land in
	 * SWEEPING; guard against the tiny-heap case where it finishes
	 * outright. */
	while (yas_gc_phase() == 1)
		yas_gc_step();

	if (yas_gc_phase() == 2) {
		YasAtom child = yas_make_string("rescued-during-sweeping");
		yas_set_cdr(container, child);
	}

	while (yas_gc_phase() != 0)
		yas_gc_step();

	CHECK(yas_cdr(container).type == YAS_ATOM_TYPE_STRING);
	CHECK(strcmp(yas_cdr(container).value.string, "rescued-during-sweeping") == 0);

	yas_gc_pop_roots(2);
}

/* 5. yas_gc_mark() must not recurse per list element - a naive recursive
 * marker overflows the C stack on a long enough list. This is the same
 * shape aux/gc_incremental_stress.ys exercises from Lisp; this version
 * pins the exact length and asserts on completion rather than crash. */
static void test_deep_list_no_stack_overflow(void)
{
	const int n = 1000000;
	YasAtom list = build_long_rooted_list(n);
	yas_gc_push_root(&list);

	yas_gc();

	int count = 0;
	YasAtom p = list;
	while (!yas_nilp(p)) {
		count++;
		p = yas_cdr(p);
	}
	CHECK(count == n);
	/* Head of the list was consed last (build_long_rooted_list prepends),
	 * so it holds n-1. */
	CHECK(yas_car(list).value.integer == n - 1);

	yas_gc_pop_roots(1);
}

/* 6. yas_eval_source()'s `env` parameter, when called with a fresh
 * yas_env_create(yas_nil) the way yas_library_load() does for every
 * native-module-plus-Lisp-source and pure-Lisp library load
 * (src/module.c), is reachable from nothing else. If a source string
 * triggers a full collection mid-evaluation (here, via (gc) called from
 * builtin context - see docs/GC.md's note that (gc)/(gc-start) can force
 * a cycle at any depth, not just at eval_vm's automatic safe point), the
 * unrooted env is swept before the next top-level form (define) tries to
 * bind into it. suite.ys can't reach this: the vulnerable env only ever
 * exists on the yas_library_load() path, where no user code can inject
 * a (gc) between forms. */
static void test_eval_source_roots_fresh_env(void)
{
	YasAtom result;
	/* (import (yas base)) both binds `gc` into this fresh env (so a
	 * later builtin-context collection can be forced at all) and, via
	 * yas_library_load()'s own nested eval, allocates enough that a real
	 * cycle can start - then (gc) forces it to completion right inside
	 * this env's unrooted window before the following (define) needs it
	 * to still be reachable. */
	int err = yas_eval_source(
		"(import (yas base)) (gc) (define x 1) x", "<gc_test>",
		yas_env_create(yas_nil), &result);

	CHECK(err == YAS_ERROR_OK);
	CHECK(result.type == YAS_ATOM_TYPE_INTEGER);
	CHECK(result.value.integer == 1);
}

int main(void)
{
	test_root_protection();
	test_root_balance();
	test_write_barrier_during_marking();
	test_write_barrier_during_sweeping();
	test_deep_list_no_stack_overflow();
	test_eval_source_roots_fresh_env();

	if (failures) {
		fprintf(stderr, "gc_test: %d FAILURE(S)\n", failures);
		return 1;
	}
	printf("gc_test: ALL TESTS PASSED\n");
	return 0;
}

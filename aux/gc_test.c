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
 *          aux/gc_test.c -L. -lyas -Wl,-rpath,. (see the Makefile's
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

/* 7. gc_alloc() itself paces steps now (docs/GC.md, "Allocation-triggered
 * stepping") - unlike test_root_protection() above, this test never calls
 * yas_gc()/yas_gc_start()/yas_gc_step() explicitly. churn() alone, given
 * enough iterations to cross the default byte threshold, must trigger
 * enough automatic collector progress from inside gc_alloc() that an
 * unrooted value gets swept while a rooted one survives - proving the
 * pacing added to gc_alloc()/gc_account_extra() actually runs a real
 * builtin/allocator would exercise, not just the explicit-call API. */
static void test_gc_alloc_paces_itself(void)
{
	YasAtom rooted = yas_make_string("survives-automatic-pacing");
	yas_gc_push_root(&rooted);

	/* Plenty to cross the default ~256KB threshold (a cons cell is 56
	 * bytes) several times over via gc_alloc()'s own internal pacing
	 * alone. */
	churn(200000);

	CHECK(rooted.type == YAS_ATOM_TYPE_STRING);
	CHECK(strcmp(rooted.value.string, "survives-automatic-pacing") == 0);
	/* Automatic pacing must have actually run at least one cycle by now -
	 * otherwise this test isn't exercising anything different from
	 * test_root_protection()'s explicit yas_gc() calls. */
	CHECK(yas_gc_allocated() < 200000 * 56);

	yas_gc_pop_roots(1);
}

/* 8. Pacing must keep up with allocation, not just eventually run: the
 * live-bytes counter should stay bounded (proportional to what's actually
 * rooted, not to total allocation volume) across a long churn loop -
 * otherwise "gc_alloc() paces itself" could still mean an effectively
 * unbounded heap if steps fall behind allocation. */
static void test_pacing_keeps_up_with_allocation(void)
{
	YasAtom held = yas_make_int(0);
	yas_gc_push_root(&held);

	size_t max_seen = 0;
	for (int batch = 0; batch < 20; batch++) {
		churn(20000);
		size_t allocated = yas_gc_allocated();
		if (allocated > max_seen)
			max_seen = allocated;
	}

	/* Total garbage generated was ~20 * 20000 * 56 bytes (~22MB); if
	 * pacing kept up, live bytes tracked at any snapshot should stay a
	 * small fraction of that - a generous bound (2MB) that's still far
	 * below "the collector never ran". */
	CHECK(max_seen < 2 * 1024 * 1024);

	yas_gc_pop_roots(1);
}

/* 9. gc_stepping re-entrancy guard (data.c's gc_pace()): force a cycle
 * active, then allocate heavily *while* it's active, from inside what
 * looks like (to gc_alloc()) a step already in progress is not the case
 * here - this just confirms ordinary heavy allocation during an
 * already-active, externally-started cycle still terminates cleanly and
 * reaches IDLE, rather than the guard causing stepping to silently stop
 * making progress once a cycle happens to already be running. */
static void test_allocation_during_active_cycle_terminates(void)
{
	YasAtom held = yas_make_int(0);
	yas_gc_push_root(&held);

	yas_gc_start();
	CHECK(yas_gc_phase() != 0);

	churn(50000);

	/* Heavy allocation during an active cycle must not prevent it from
	 * ever finishing - drain explicitly and confirm IDLE is reachable. */
	while (yas_gc_phase() != 0)
		yas_gc_step();
	CHECK(yas_gc_phase() == 0);

	yas_gc_pop_roots(1);
}

/* 10. Environment frames (struct YasEnv, yas.h) are a GC heap type now -
 * this is the C-level check that YAS_GC_OBJ_ENV's gc_scan_object() arm
 * (data.c) actually marks `parent` and every occupied slot, and that the
 * write barrier fired by yas_env_define()/yas_env_create() (also data.c)
 * protects a frame's bindings once the container is BLACK mid-cycle.
 * Without either, this drops bindings the moment a cycle runs while the
 * chain is rooted only at its innermost frame - exactly the shape
 * closures and nested lets produce, which suite.ys can't force a
 * controlled collection in the middle of. Also exercises env_grow()
 * (data.c): 50 bindings per frame crosses the 8-slot initial capacity's
 * 70% rehash threshold several times over. */
static void test_env_chain_survives_gc(void)
{
	const int depth = 200;
	const int bindings_per_frame = 50;

	YasAtom root_env = yas_env_create(yas_nil);
	yas_gc_push_root(&root_env);

	YasAtom env = root_env;
	for (int d = 0; d < depth; d++) {
		env = yas_env_create(env);
		/* Re-root: env_create()'s own internal root only covers its own
		 * call, and this loop's `env` is the sole reference to the chain
		 * built so far - rooting just the innermost frame is enough,
		 * since every ancestor is reachable by walking `parent`. */
		yas_gc_pop_roots(1);
		yas_gc_push_root(&env);

		for (int b = 0; b < bindings_per_frame; b++) {
			char name[32];
			snprintf(name, sizeof(name), "V-%d-%d", d, b);
			yas_env_define(env, yas_make_sym(name), yas_make_int(d * 1000 + b));
		}
	}

	churn(50000);
	yas_gc();
	yas_gc();
	yas_gc();

	int ok = 1;
	for (int d = 0; d < depth && ok; d++) {
		for (int b = 0; b < bindings_per_frame && ok; b++) {
			char name[32];
			snprintf(name, sizeof(name), "V-%d-%d", d, b);
			YasAtom result;
			int err = yas_env_get(env, yas_make_sym(name), &result);
			if (err != YAS_ERROR_OK
			    || result.type != YAS_ATOM_TYPE_INTEGER
			    || result.value.integer != d * 1000 + b) {
				ok = 0;
			}
		}
	}
	CHECK(ok);

	/* An unbound name at the root parent still reports UNBOUND, not a
	 * stale/garbage hit - confirms the write barrier didn't over-protect
	 * (i.e. isn't masking a broken parent chain). */
	YasAtom unused;
	CHECK(yas_env_get(env, yas_make_sym("NO-SUCH-BINDING"), &unused) == YAS_ERROR_UNBOUND);

	yas_gc_pop_roots(1);
}

/* 11. Control-stack frames (struct YasFrame, yas.h - ticket #40) are a GC
 * heap type - this is the C-level check that YAS_GC_OBJ_FRAME's
 * gc_scan_object() arm (data.c) actually marks a frame's fields, mirroring
 * test_env_chain_survives_gc() above. yas_make_frame() (data.c) is the
 * only field-setting API this file has access to (frame_get()/frame_set()
 * are eval.c-internal static inlines, not part of the public yas.h
 * surface), so this exercises `parent`/`env`/`tail` - the three fields it
 * threads through the write barrier - by chaining frames and storing a
 * distinct heap string in each frame's `env` slot and a distinct heap
 * cons in each frame's `tail` slot, then forcing several full collections
 * and confirming every frame's data is still intact. Without a
 * YAS_GC_OBJ_FRAME arm in gc_scan_object(), this drops data the moment a
 * cycle runs while the chain is rooted only at its innermost frame - the
 * exact shape a captured continuation produces (see
 * aux/gc_continuation_stress.ys for the .ys-level equivalent). */
static void test_frame_chain_survives_gc(void)
{
	const int depth = 200;

	YasAtom frame = yas_make_frame(yas_nil, yas_nil, yas_nil);
	yas_gc_push_root(&frame);

	for (int d = 1; d < depth; d++) {
		char name[32];
		snprintf(name, sizeof(name), "FRAME-ENV-%d", d);
		YasAtom env_marker = yas_make_string(name);
		YasAtom tail_marker = yas_cons(yas_make_int(d), yas_nil);

		frame = yas_make_frame(frame, env_marker, tail_marker);
		/* Re-root: same discipline as test_env_chain_survives_gc() -
		 * this loop's `frame` is the sole reference to the chain built
		 * so far, and every ancestor is reachable by walking `parent`. */
		yas_gc_pop_roots(1);
		yas_gc_push_root(&frame);
	}

	churn(50000);
	yas_gc();
	yas_gc();
	yas_gc();

	int ok = 1;
	YasAtom f = frame;
	for (int d = depth - 1; d >= 1 && ok; d--) {
		if (f.type != YAS_ATOM_TYPE_FRAME || !f.value.frame) { ok = 0; break; }
		YasAtom env_marker = f.value.frame->env;
		YasAtom tail_marker = f.value.frame->tail;
		char expected[32];
		snprintf(expected, sizeof(expected), "FRAME-ENV-%d", d);
		if (env_marker.type != YAS_ATOM_TYPE_STRING
		    || strcmp(env_marker.value.string, expected) != 0)
			ok = 0;
		if (tail_marker.type != YAS_ATOM_TYPE_PAIR
		    || yas_car(tail_marker).type != YAS_ATOM_TYPE_INTEGER
		    || yas_car(tail_marker).value.integer != d)
			ok = 0;
		f = f.value.frame->parent;
	}
	/* Walked all the way back to the root frame (its own parent is nil). */
	if (ok && (f.type != YAS_ATOM_TYPE_FRAME || !f.value.frame
	           || !yas_nilp(f.value.frame->parent)))
		ok = 0;
	CHECK(ok);

	yas_gc_pop_roots(1);
}

int main(void)
{
	test_root_protection();
	test_root_balance();
	test_write_barrier_during_marking();
	test_write_barrier_during_sweeping();
	test_deep_list_no_stack_overflow();
	test_eval_source_roots_fresh_env();
	test_gc_alloc_paces_itself();
	test_pacing_keeps_up_with_allocation();
	test_allocation_during_active_cycle_terminates();
	test_env_chain_survives_gc();
	test_frame_chain_survives_gc();

	if (failures) {
		fprintf(stderr, "gc_test: %d FAILURE(S)\n", failures);
		return 1;
	}
	printf("gc_test: ALL TESTS PASSED\n");
	return 0;
}

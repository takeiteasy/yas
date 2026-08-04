/* plan_test.c - plain-C reference interpreter for the plan op stream
 * produced by aot_plan.c, used to validate the plan compiler (mangling,
 * type inference, let/let* scoping, op emission) with no cccc involved.
 *
 * This exists because a purely-comptime debugging loop is expensive (each
 * iteration needs a full cccc invocation); the actual compiler logic in
 * aot_plan.c is ordinary, plain-C-testable code, and this is where it
 * should be developed and regression-tested. aot/aot.c's replay loop
 * (real cccc reflection Node* construction) is a thin, mechanical
 * translation of the exact same two-pass shape used here: consume the
 * flat op list left-to-right, maintaining a stack of *tree nodes* (not
 * values) - build() below - then evaluate that tree lazily (eval()).
 *
 * The two-pass split matters: a naive single-pass interpreter that pushes
 * *computed values* instead of tree nodes evaluates both branches of
 * every COND unconditionally (postfix has no short-circuiting on its
 * own), which sends any recursive function with a base case - fib
 * included - into unbounded recursion. cccc's real replay doesn't have
 * this problem because MakeCond builds a C ternary `Node*`; the ternary's
 * *generated code* short-circuits at the AOT-compiled program's own
 * runtime, same as build()+eval() does here.
 *
 * Build: cc -std=c99 -D_GNU_SOURCE -Isrc -o build/plan_test \
 *          aot/tests/plan_test.c -L. -lyas -Wl,-rpath,.
 * (see the Makefile's `test-aot` target)
 */
#include "aot_plan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* value type used only by the eval() lazy-evaluation pass below */

/* ---- pass 1: flat ops -> tree, via the same push-onto-a-stack shape a
 * postfix-to-AST conversion always has (aot/aot.c's replay loop does the
 * identical walk, pushing cccc Node* instead of Ast*). ---- */

typedef struct Ast {
	int kind; /* YasOpKind */
	int a, b;
	long ival;
	double fval;
	struct Ast *kids[3];  /* COND: cond,then,else. BINOP: l,r. UNOP/CAST_*: x. CALL: unused (see args). */
	struct Ast **args;    /* CALL args, and SEQ operands */
	int nargs;
} Ast;

static Ast *ast_new(int kind, int a, int b, long ival, double fval)
{
	Ast *n = calloc(1, sizeof(Ast));
	n->kind = kind; n->a = a; n->b = b; n->ival = ival; n->fval = fval;
	return n;
}

static Ast *build(struct YasPlan *p, int fn)
{
	int op_count = yas_plan_op_count(p, fn);
	Ast *stack[256];
	int sp = 0;

	for (int i = 0; i < op_count; i++) {
		int kind = yas_plan_op_kind(p, fn, i);
		int a = yas_plan_op_a(p, fn, i);
		int b = yas_plan_op_b(p, fn, i);
		switch (kind) {
		case YAS_OP_PUSH_INT:
			stack[sp++] = ast_new(kind, 0, 0, yas_plan_op_i(p, fn, i), 0);
			break;
		case YAS_OP_PUSH_FLOAT:
			stack[sp++] = ast_new(kind, 0, 0, 0, yas_plan_op_f(p, fn, i));
			break;
		case YAS_OP_PUSH_PARAM:
		case YAS_OP_PUSH_LOCAL:
			stack[sp++] = ast_new(kind, a, 0, 0, 0);
			break;
		case YAS_OP_STORE_LOCAL: {
			Ast *n = ast_new(kind, a, 0, 0, 0);
			n->kids[0] = stack[sp - 1]; /* value; STORE_LOCAL's own result === its operand's value */
			stack[sp - 1] = n;
			break;
		}
		case YAS_OP_BINOP: {
			Ast *n = ast_new(kind, a, 0, 0, 0);
			n->kids[1] = stack[--sp];
			n->kids[0] = stack[--sp];
			stack[sp++] = n;
			break;
		}
		case YAS_OP_UNOP: {
			Ast *n = ast_new(kind, a, 0, 0, 0);
			n->kids[0] = stack[--sp];
			stack[sp++] = n;
			break;
		}
		case YAS_OP_COND: {
			Ast *n = ast_new(kind, 0, 0, 0, 0);
			n->kids[2] = stack[--sp];
			n->kids[1] = stack[--sp];
			n->kids[0] = stack[--sp];
			stack[sp++] = n;
			break;
		}
		case YAS_OP_CALL: {
			Ast *n = ast_new(kind, a, b, 0, 0);
			n->nargs = b;
			n->args = calloc((size_t)b, sizeof(Ast *));
			for (int k = b - 1; k >= 0; k--) n->args[k] = stack[--sp];
			stack[sp++] = n;
			break;
		}
		case YAS_OP_CAST_LONG:
		case YAS_OP_CAST_DOUBLE: {
			Ast *n = ast_new(kind, 0, 0, 0, 0);
			n->kids[0] = stack[--sp];
			stack[sp++] = n;
			break;
		}
		case YAS_OP_SEQ: {
			Ast *n = ast_new(kind, a, 0, 0, 0);
			n->nargs = a;
			n->args = calloc((size_t)a, sizeof(Ast *));
			for (int k = a - 1; k >= 0; k--) n->args[k] = stack[--sp];
			stack[sp++] = n;
			break;
		}
		default:
			fprintf(stderr, "plan_test: unknown op kind %d\n", kind);
			break;
		}
	}
	return stack[sp - 1];
}

/* ---- pass 2: lazy evaluation of the tree. Real recursion, real
 * short-circuiting on COND - this is what makes fib(n) terminate. ---- */

typedef struct { int is_float; long i; double f; } Val;

static Val vint(long i) { Val v; v.is_float = 0; v.i = i; v.f = 0; return v; }
static Val vfloat(double f) { Val v; v.is_float = 1; v.i = 0; v.f = f; return v; }
static double as_f(Val v) { return v.is_float ? v.f : (double)v.i; }
static long   as_i(Val v) { return v.is_float ? (long)v.f : v.i; }
static int truthy(Val v) { return v.is_float ? v.f != 0.0 : v.i != 0; }

static Val call_fn(struct YasPlan *p, int fn, Val *args);

static Val eval(struct YasPlan *p, Ast *n, Val *params, Val *locals)
{
	switch (n->kind) {
	case YAS_OP_PUSH_INT:   return vint(n->ival);
	case YAS_OP_PUSH_FLOAT: return vfloat(n->fval);
	case YAS_OP_PUSH_PARAM: return params[n->a];
	case YAS_OP_PUSH_LOCAL: return locals[n->a];
	case YAS_OP_STORE_LOCAL: {
		Val v = eval(p, n->kids[0], params, locals);
		locals[n->a] = v;
		return v;
	}
	case YAS_OP_BINOP: {
		Val l = eval(p, n->kids[0], params, locals);
		Val r = eval(p, n->kids[1], params, locals);
		int is_f = l.is_float || r.is_float;
		switch (n->a) {
		case YAS_BINOP_ADD: return is_f ? vfloat(as_f(l) + as_f(r)) : vint(as_i(l) + as_i(r));
		case YAS_BINOP_SUB: return is_f ? vfloat(as_f(l) - as_f(r)) : vint(as_i(l) - as_i(r));
		case YAS_BINOP_MUL: return is_f ? vfloat(as_f(l) * as_f(r)) : vint(as_i(l) * as_i(r));
		case YAS_BINOP_DIV: return is_f ? vfloat(as_f(l) / as_f(r)) : vint(as_i(l) / as_i(r));
		case YAS_BINOP_LT:  return vint(is_f ? as_f(l) <  as_f(r) : as_i(l) <  as_i(r));
		case YAS_BINOP_LE:  return vint(is_f ? as_f(l) <= as_f(r) : as_i(l) <= as_i(r));
		case YAS_BINOP_EQ:  return vint(is_f ? as_f(l) == as_f(r) : as_i(l) == as_i(r));
		case YAS_BINOP_LOGAND: return vint(truthy(l) && truthy(r));
		case YAS_BINOP_LOGOR:  return vint(truthy(l) || truthy(r));
		}
		return vint(0);
	}
	case YAS_OP_UNOP: {
		Val v = eval(p, n->kids[0], params, locals);
		if (n->a == YAS_UNOP_NEG) return v.is_float ? vfloat(-v.f) : vint(-v.i);
		return vint(!truthy(v));
	}
	case YAS_OP_COND: {
		Val c = eval(p, n->kids[0], params, locals);
		return truthy(c) ? eval(p, n->kids[1], params, locals)
		                  : eval(p, n->kids[2], params, locals);
	}
	case YAS_OP_CALL: {
		Val cargs[16];
		for (int i = 0; i < n->nargs; i++) cargs[i] = eval(p, n->args[i], params, locals);
		return call_fn(p, n->a, cargs);
	}
	case YAS_OP_CAST_LONG:   return vint(as_i(eval(p, n->kids[0], params, locals)));
	case YAS_OP_CAST_DOUBLE: return vfloat(as_f(eval(p, n->kids[0], params, locals)));
	case YAS_OP_SEQ: {
		Val last = vint(0);
		for (int i = 0; i < n->nargs; i++) last = eval(p, n->args[i], params, locals);
		return last;
	}
	}
	fprintf(stderr, "plan_test: unknown ast kind %d\n", n->kind);
	return vint(0);
}

static Val call_fn(struct YasPlan *p, int fn, Val *args)
{
	Val locals[64];
	int local_count = yas_plan_fn_local_count(p, fn);
	for (int i = 0; i < local_count; i++)
		locals[i] = yas_plan_fn_local_is_float(p, fn, i) ? vfloat(0) : vint(0);

	/* Built fresh per call: aot_plan.c owns the op list, this is a cheap
	 * re-walk of it, not a cache invalidation concern. A real embedding
	 * would build once and reuse - not needed for this test harness. */
	Ast *tree = build(p, fn);
	return eval(p, tree, args, locals);
}

static int find_fn(struct YasPlan *p, const char *name)
{
	for (int i = 0; i < yas_plan_fn_count(p); i++)
		if (strcmp(yas_plan_fn_name(p, i), name) == 0) return i;
	return -1;
}

int main(void)
{
	int fails = 0;

	/* fib */
	{
		struct YasPlan *p = yas_aot_plan("aot/examples/fib.ys");
		const char *err = yas_plan_error(p);
		if (err) { fprintf(stderr, "FAIL fib.ys: %s\n", err); fails++; }
		else {
			int fn = find_fn(p, "fib");
			Val r = call_fn(p, fn, (Val[]){ vint(10) });
			long got = as_i(r);
			if (got != 55) { fprintf(stderr, "FAIL fib(10): got %ld want 55\n", got); fails++; }
			else printf("PASS fib(10) = %ld\n", got);

			if (as_i(call_fn(p, fn, (Val[]){ vint(1) })) != 1) { fprintf(stderr, "FAIL fib(1)\n"); fails++; }
			if (as_i(call_fn(p, fn, (Val[]){ vint(0) })) != 0) { fprintf(stderr, "FAIL fib(0)\n"); fails++; }
		}
		yas_plan_free(p);
	}

	/* misc: let, let*, float constants/promotion, mutual recursion */
	{
		struct YasPlan *p = yas_aot_plan("aot/examples/misc.ys");
		const char *err = yas_plan_error(p);
		if (err) { fprintf(stderr, "FAIL misc.ys: %s\n", err); fails++; yas_plan_free(p); goto done; }

		int fn;

		fn = find_fn(p, "let_test");
		{ long got = as_i(call_fn(p, fn, (Val[]){ vint(5) }));
		  if (got != 16) { fprintf(stderr, "FAIL let-test(5): got %ld want 16\n", got); fails++; }
		  else printf("PASS let-test(5) = %ld\n", got); }

		fn = find_fn(p, "let_star_test");
		{ long got = as_i(call_fn(p, fn, (Val[]){ vint(5) }));
		  if (got != 18) { fprintf(stderr, "FAIL let-star-test(5): got %ld want 18\n", got); fails++; }
		  else printf("PASS let-star-test(5) = %ld\n", got); }

		fn = find_fn(p, "circle_area");
		if (!yas_plan_fn_is_float(p, fn)) { fprintf(stderr, "FAIL circle-area: expected float inference\n"); fails++; }
		{ double got = as_f(call_fn(p, fn, (Val[]){ vfloat(2.0) }));
		  if (got < 12.56 || got > 12.57) { fprintf(stderr, "FAIL circle-area(2.0): got %f want ~12.566\n", got); fails++; }
		  else printf("PASS circle-area(2.0) = %f\n", got); }

		fn = find_fn(p, "mutual_even");
		{ long got = as_i(call_fn(p, fn, (Val[]){ vint(10) }));
		  if (got != 1) { fprintf(stderr, "FAIL mutual-even(10): got %ld want 1\n", got); fails++; }
		  else printf("PASS mutual-even(10) = %ld\n", got); }

		fn = find_fn(p, "mutual_odd");
		{ long got = as_i(call_fn(p, fn, (Val[]){ vint(7) }));
		  if (got != 1) { fprintf(stderr, "FAIL mutual-odd(7): got %ld want 1\n", got); fails++; }
		  else printf("PASS mutual-odd(7) = %ld\n", got); }

		yas_plan_free(p);
	}

done:
	if (fails) { printf("%d check(s) FAILED\n", fails); return 1; }
	printf("ALL PLAN TESTS PASSED\n");
	return 0;
}

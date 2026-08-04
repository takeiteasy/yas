/* aot.c - cccc comptime replay loop for the yas AOT pass.
 *
 * All the actual compiler work - reading, mangling, type inference,
 * let/let* scope resolution, flattening each function body to a postfix
 * op list - happens in ordinary C in src/aot_plan.c, tested independently
 * of cccc (aot/tests/plan_test.c). This file's only job is to walk that
 * flat op list once per function and rebuild it as cccc reflection
 * `Node *` trees via a small, non-recursive stack machine, then hand the
 * finished tree to `FunctionSetBody`.
 *
 * That split exists because an earlier version of this file did
 * everything - reading, lowering, AST-building - inside cccc's comptime
 * VM in one pass, and that ran into a string of VM-scale bugs (crashes
 * and "undefined variable" errors that appeared only once enough
 * comptime-side code existed, and that every minimal reproduction failed
 * to reproduce). Keeping this file small and flat is what makes it work;
 * see docs/AOT.md for the design writeup and the cccc issues found along
 * the way. In particular, everything below sticks to patterns verified to
 * be safe:
 *
 *   - `Obj *` / `Node *` / `Type *` values never live in a file-scope
 *     global or struct field - only in this function's own locals.
 *   - The entry point (`yas_aot_compile`) is global-generation style
 *     (`void`, taking a `char *path`, invoked as a bare statement at file
 *     scope - see aot/fib_main.c), which correctly auto-stringifies its
 *     call-site argument into an ordinary local parameter. A
 *     `Node *`-returning call-site-expansion entry was tried first
 *     specifically to dodge macro/global string values, but that traded
 *     one bug for a worse one - see the next point.
 *   - No `#define`d string value is ever referenced *inside* a
 *     `[[cccc::comptime]]` function body (as opposed to a plain
 *     parameter, which is fine): cccc's comptime pass deliberately does
 *     not forward source-file `#define`s into comptime function bodies
 *     (see cccc's docs/MACROS.md, "Pre-parse macro declaration context").
 *     `-D`/command-line defines are the documented exception - they
 *     survive comptime isolation - which is why `AOT_LIBYAS_PATH` (below,
 *     now `YAS_LIBYAS_PATH`) arrives via `-D` rather than a source
 *     `#define`.
 *   - No recursive or mutually-recursive `[[cccc::comptime]]` helper
 *     functions, and no forward declarations of one - cccc's comptime VM
 *     crashes (SIGSEGV) on a forward-declared comptime function used
 *     before its definition, reproduced as a two-line minimal case. This
 *     file's comptime functions are only ever called in sequence, never
 *     each other, and never before their own definition.
 *
 * Two earlier workarounds this file used to need no longer apply, fixed
 * upstream in cccc: indirect calls through a dlsym'd function pointer via
 * an inline cast used to crash at runtime (cccc #885) -- every dlsym
 * result below is still assigned to a named variable before use, simply
 * because that reads better, not because the inline form is unsafe now --
 * and a function-pointer typedef declared inside comptime-executed code
 * used to be rejected as though it were a comptime pointer *variable*
 * (cccc #886) -- the `fp_*` typedefs below stay at file scope regardless,
 * because the file-scope statics they type need them there.
 */

#include @comptime "../src/aot_plan.h"
#include @comptime <dlfcn.h>

typedef struct YasPlan *(*fp_plan)(const char *);
typedef const char     *(*fp_error)(struct YasPlan *);
typedef void            (*fp_free)(struct YasPlan *);
typedef int              (*fp_fn_count)(struct YasPlan *);
typedef const char      *(*fp_fn_name)(struct YasPlan *, int);
typedef int              (*fp_fn_is_float)(struct YasPlan *, int);
typedef int              (*fp_fn_param_count)(struct YasPlan *, int);
typedef const char      *(*fp_fn_param_name)(struct YasPlan *, int, int);
typedef int              (*fp_fn_local_count)(struct YasPlan *, int);
typedef int              (*fp_fn_local_is_float)(struct YasPlan *, int, int);
typedef int              (*fp_op_count)(struct YasPlan *, int);
typedef int              (*fp_op_kind)(struct YasPlan *, int, int);
typedef int              (*fp_op_a)(struct YasPlan *, int, int);
typedef int              (*fp_op_b)(struct YasPlan *, int, int);
typedef long              (*fp_op_i)(struct YasPlan *, int, int);
typedef double            (*fp_op_f)(struct YasPlan *, int, int);

static fp_plan               plan_fn;
static fp_error               plan_error;
static fp_free                 plan_free;
static fp_fn_count             plan_fn_count;
static fp_fn_name              plan_fn_name;
static fp_fn_is_float          plan_fn_is_float;
static fp_fn_param_count       plan_fn_param_count;
static fp_fn_param_name        plan_fn_param_name;
static fp_fn_local_count       plan_fn_local_count;
static fp_fn_local_is_float    plan_fn_local_is_float;
static fp_op_count             plan_op_count;
static fp_op_kind              plan_op_kind;
static fp_op_a                 plan_op_a;
static fp_op_b                 plan_op_b;
static fp_op_i                 plan_op_i;
static fp_op_f                 plan_op_f;

[[cccc::comptime]]
int aot_bind(void)
{
	/* YAS_LIBYAS_PATH: the build's relative, per-platform libyas path
	 * ("./libyas.dylib" / "./libyas.so"), supplied via `make aot`'s `-D`
	 * flag (see the file-level comment above on why a source #define
	 * can't be read from inside this function instead). This runs at
	 * cccc's own compile time (aot_bind is [[cccc::comptime]]), so it
	 * resolves relative to cccc's CWD, which is this repo's root -- see
	 * the Makefile's `aot` target. See docs/AOT.md. */
	void *h = dlopen(YAS_LIBYAS_PATH, RTLD_NOW);
	if (!h) return 0;
	plan_fn                 = (fp_plan)dlsym(h, "yas_aot_plan");
	plan_error               = (fp_error)dlsym(h, "yas_plan_error");
	plan_free                 = (fp_free)dlsym(h, "yas_plan_free");
	plan_fn_count             = (fp_fn_count)dlsym(h, "yas_plan_fn_count");
	plan_fn_name              = (fp_fn_name)dlsym(h, "yas_plan_fn_name");
	plan_fn_is_float          = (fp_fn_is_float)dlsym(h, "yas_plan_fn_is_float");
	plan_fn_param_count       = (fp_fn_param_count)dlsym(h, "yas_plan_fn_param_count");
	plan_fn_param_name        = (fp_fn_param_name)dlsym(h, "yas_plan_fn_param_name");
	plan_fn_local_count       = (fp_fn_local_count)dlsym(h, "yas_plan_fn_local_count");
	plan_fn_local_is_float    = (fp_fn_local_is_float)dlsym(h, "yas_plan_fn_local_is_float");
	plan_op_count              = (fp_op_count)dlsym(h, "yas_plan_op_count");
	plan_op_kind               = (fp_op_kind)dlsym(h, "yas_plan_op_kind");
	plan_op_a                  = (fp_op_a)dlsym(h, "yas_plan_op_a");
	plan_op_b                  = (fp_op_b)dlsym(h, "yas_plan_op_b");
	plan_op_i                  = (fp_op_i)dlsym(h, "yas_plan_op_i");
	plan_op_f                  = (fp_op_f)dlsym(h, "yas_plan_op_f");
	return plan_fn && plan_error && plan_free && plan_fn_count && plan_fn_name
	    && plan_fn_is_float && plan_fn_param_count && plan_fn_param_name
	    && plan_fn_local_count && plan_fn_local_is_float && plan_op_count
	    && plan_op_kind && plan_op_a && plan_op_b && plan_op_i && plan_op_f;
}

/* Global-generation macro: called at file scope (see aot/fib_main.c) as a
 * bare `yas_aot_compile("...");` statement, runs before the main parse. `path`
 * arrives de-stringified (quotes stripped by cccc) as an ordinary
 * function parameter - a ordinary local, not a macro or a global, so none
 * of the string-value restriction above applies to it. */
[[cccc::comptime]]
void yas_aot_compile(char *path)
{
	if (!aot_bind()) {
		MacroErrorAt(0, "yas-aot: could not dlopen libyas - build it first "
		                 "with `make libyas` (see docs/AOT.md)");
		return;
	}

	struct YasPlan *plan = plan_fn(path);
	const char *err = plan_error(plan);
	if (err) {
		MacroErrorAt(0, "yas-aot: %s", err);
		return;
	}

	int fn_count = plan_fn_count(plan);

	/* pass 1: prototypes, so mutual recursion resolves */
	for (int fni = 0; fni < fn_count; fni++) {
		Type *ret_ty = plan_fn_is_float(plan, fni) ? GetType("double") : GetType("long");
		Obj *proto = FunctionPrototype(plan_fn_name(plan, fni), ret_ty);
		int param_count = plan_fn_param_count(plan, fni);
		for (int pi = 0; pi < param_count; pi++)
			FunctionAddParam(proto, plan_fn_param_name(plan, fni, pi), ret_ty);
		PublishNode(proto);
	}

	/* pass 2: bodies - each is a small, non-recursive stack machine over
	 * the flat op list, exactly mirroring aot_plan.c/plan_test.c's build()
	 * pass but pushing cccc Node* instead of a plain-C Ast*. COND builds a
	 * real ternary (ND_COND) whose *generated code* short-circuits at the
	 * AOT-compiled program's own runtime - construction here never
	 * evaluates either branch, so there's no risk of the eager-evaluation
	 * infinite-recursion trap plan_test.c's first draft hit. */
	for (int fni = 0; fni < fn_count; fni++) {
		Type *ret_ty = plan_fn_is_float(plan, fni) ? GetType("double") : GetType("long");
		Obj *fnobj = MakeFunction(plan_fn_name(plan, fni), ret_ty);
		WithFn(fnobj) {
			int param_count = plan_fn_param_count(plan, fni);
			Node *params[16];
			for (int pi = 0; pi < param_count; pi++)
				params[pi] = MakeParamRef(fnobj, plan_fn_param_name(plan, fni, pi));

			int local_count = plan_fn_local_count(plan, fni);
			Node *locals[64];
			for (int li = 0; li < local_count; li++)
				locals[li] = MakeLocalVarUnique(plan_fn_local_is_float(plan, fni, li)
				                                 ? GetType("double") : GetType("long"));

			Node *stack[256];
			int sp = 0;
			int op_count = plan_op_count(plan, fni);
			for (int i = 0; i < op_count; i++) {
				int kind = plan_op_kind(plan, fni, i);
				int a = plan_op_a(plan, fni, i);
				int b = plan_op_b(plan, fni, i);

				if (kind == YAS_OP_PUSH_INT) {
					stack[sp++] = MakeIntLiteral(plan_op_i(plan, fni, i));
				} else if (kind == YAS_OP_PUSH_FLOAT) {
					stack[sp++] = MakeFloatLiteral(plan_op_f(plan, fni, i));
				} else if (kind == YAS_OP_PUSH_PARAM) {
					stack[sp++] = params[a];
				} else if (kind == YAS_OP_PUSH_LOCAL) {
					stack[sp++] = locals[a];
				} else if (kind == YAS_OP_STORE_LOCAL) {
					stack[sp - 1] = MakeAssign(locals[a], stack[sp - 1]);
				} else if (kind == YAS_OP_BINOP) {
					Node *r = stack[--sp];
					Node *l = stack[--sp];
					int nk = a == YAS_BINOP_ADD ? NK_ADD
					       : a == YAS_BINOP_SUB ? NK_SUB
					       : a == YAS_BINOP_MUL ? NK_MUL
					       : a == YAS_BINOP_DIV ? NK_DIV
					       : a == YAS_BINOP_LT ? NK_LT
					       : a == YAS_BINOP_LE ? NK_LE
					       : a == YAS_BINOP_EQ ? NK_EQ
					       : a == YAS_BINOP_LOGAND ? NK_LOGAND
					       : NK_LOGOR;
					stack[sp++] = MakeBinary(nk, l, r);
				} else if (kind == YAS_OP_UNOP) {
					Node *x = stack[--sp];
					stack[sp++] = MakeUnary(a == YAS_UNOP_NEG ? NK_NEG : NK_NOT, x);
				} else if (kind == YAS_OP_COND) {
					Node *e = stack[--sp];
					Node *t = stack[--sp];
					Node *c = stack[--sp];
					stack[sp++] = MakeCond(c, t, e);
				} else if (kind == YAS_OP_CALL) {
					Node *args[16];
					for (int k = b - 1; k >= 0; k--) args[k] = stack[--sp];
					stack[sp++] = MakeFuncCall(MakeVarRef(plan_fn_name(plan, a)), args, b);
				} else if (kind == YAS_OP_CAST_LONG) {
					stack[sp++] = MakeCast(stack[--sp], GetType("long"));
				} else if (kind == YAS_OP_CAST_DOUBLE) {
					stack[sp++] = MakeCast(stack[--sp], GetType("double"));
				} else if (kind == YAS_OP_SEQ) {
					Node *acc = stack[sp - 1];
					for (int k = 2; k <= a; k++) acc = MakeComma(stack[sp - k], acc);
					sp -= a;
					stack[sp++] = acc;
				}
			}

			FunctionSetBody(fnobj, MakeReturn(stack[sp - 1]));
		}
	}

	plan_free(plan);
}

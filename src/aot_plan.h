/* aot_plan.h
 *
 * The yas AOT compiler proper: parses a .ys file (via aot_plan.c's own
 * boxed-Atom reader front end), mangles names, infers long-vs-double per
 * function, resolves let/let* bindings to numbered local slots, and
 * flattens every function body into a linear list of stack-machine ops.
 *
 * This is ordinary C - compiled and testable with a plain `cc`, no cccc
 * involved. That's deliberate: an earlier version of this pass did all of
 * this *inside* cccc's comptime VM (walking yas forms and recursively
 * building cccc reflection Node* trees in the same pass), and hit a string
 * of VM-scale bugs - crashes and "undefined variable" errors that
 * appeared only once enough comptime-side code existed, disappeared in
 * every minimal reproduction, and were plainly not going to be resolved by
 * finding "the next" construct to avoid within the VM. Moving all the
 * actual compiler logic out here and reducing the comptime side (aot.c) to
 * a small, flat, non-recursive replay loop over the plan below sidesteps
 * that class of problem entirely - see docs/AOT.md for the design writeup
 * and the cccc issues found along the way.
 *
 * Built into both `yas` and `libyas`. Comptime code dlopens libyas and
 * drives this API through dlsym - only pointers/long/double/int/const
 * char* ever cross that boundary. This header is deliberately
 * dependency-free (no yas YasAtom/yas.h types, no cccc types) so aot/aot.c
 * can `#include @comptime` it directly.
 */
#ifndef YAS_AOT_PLAN_H
#define YAS_AOT_PLAN_H

#ifdef __cplusplus
extern "C" {
#endif

struct YasPlan; /* one compiled file: a table of functions, each a flat op list */

/* NULL only on inability to even allocate; check yas_plan_error() first -
 * a non-NULL plan can still carry an error (unsupported input). */
struct YasPlan *yas_aot_plan(const char *path);
const char     *yas_plan_error(struct YasPlan *p); /* NULL on success */
void            yas_plan_free(struct YasPlan *p);

int         yas_plan_fn_count(struct YasPlan *p);
const char *yas_plan_fn_name(struct YasPlan *p, int fn);        /* mangled C name */
int         yas_plan_fn_is_float(struct YasPlan *p, int fn);    /* return + every param's type */
int         yas_plan_fn_param_count(struct YasPlan *p, int fn);
const char *yas_plan_fn_param_name(struct YasPlan *p, int fn, int param); /* mangled */

/* let/let* locals, numbered per-function. Declared once up front by the
 * replay loop (MakeLocalVarUnique), each with its own inferred type -
 * independent of the function's own is_float (see docs/AOT.md). */
int yas_plan_fn_local_count(struct YasPlan *p, int fn);
int yas_plan_fn_local_is_float(struct YasPlan *p, int fn, int slot);

/* The flat op list: a postfix (stack-machine) encoding of the function
 * body. The replay loop pushes/pops cccc Node* per YasOpKind below. */
int yas_plan_op_count(struct YasPlan *p, int fn);
int yas_plan_op_kind(struct YasPlan *p, int fn, int i);  /* YasOpKind */
int yas_plan_op_a(struct YasPlan *p, int fn, int i);     /* generic int operand */
int yas_plan_op_b(struct YasPlan *p, int fn, int i);     /* second int operand (CALL's argc) */
long   yas_plan_op_i(struct YasPlan *p, int fn, int i);  /* PUSH_INT literal */
double yas_plan_op_f(struct YasPlan *p, int fn, int i);  /* PUSH_FLOAT literal */

/* The flat stack-machine opcode set: emitted by aot_plan.c, replayed by
 * aot/aot.c (cccc comptime code) into cccc reflection Node* trees. Kept
 * dependency-free for the same @comptime-include reason as the rest of
 * this header.
 *
 * Every op pushes exactly one value except STORE_LOCAL (pushes the
 * assignment expression, so it composes into a SEQ) - the stack is never
 * popped without something replacing it, which is what lets a flat list
 * stand in for a tree without ever needing recursion on the replay side. */
typedef enum {
	YAS_OP_PUSH_INT,     /* i: literal value                         -> push */
	YAS_OP_PUSH_FLOAT,   /* f: literal value                         -> push */
	YAS_OP_PUSH_PARAM,   /* a: param index                           -> push */
	YAS_OP_PUSH_LOCAL,   /* a: local slot index                      -> push */
	YAS_OP_STORE_LOCAL,  /* a: local slot index; pop value            -> push (local = value) */
	YAS_OP_BINOP,        /* a: YasBinOp;                pop 2 (l, r) -> push (l op r) */
	YAS_OP_UNOP,         /* a: YasUnOp;                 pop 1        -> push (op x) */
	YAS_OP_COND,         /* pop 3 (cond, then, else), in that push order -> push ternary */
	YAS_OP_CALL,         /* a: callee fn index, b: argc; pop argc     -> push call(args) */
	YAS_OP_CAST_LONG,    /* pop 1 -> push (long)x */
	YAS_OP_CAST_DOUBLE,  /* pop 1 -> push (double)x */
	YAS_OP_SEQ           /* a: n; pop n, comma-chain keeping the last -> push */
} YasOpKind;

typedef enum {
	YAS_BINOP_ADD, YAS_BINOP_SUB, YAS_BINOP_MUL, YAS_BINOP_DIV,
	YAS_BINOP_LT, YAS_BINOP_LE, YAS_BINOP_EQ,
	YAS_BINOP_LOGAND, YAS_BINOP_LOGOR
} YasBinOp;

typedef enum {
	YAS_UNOP_NEG, YAS_UNOP_NOT
} YasUnOp;

#ifdef __cplusplus
}
#endif

#endif /* YAS_AOT_PLAN_H */

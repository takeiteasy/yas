/* aot_plan.h
 *
 * The yas AOT compiler proper: parses a .ys file (via aot_shim.h's reader
 * front end), mangles names, infers long-vs-double per function, resolves
 * let/let* bindings to numbered local slots, and flattens every function
 * body into a linear list of stack-machine ops.
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
 * a small, flat, non-recursive replay loop over the plan produced below
 * sidesteps that class of problem entirely - see docs/AOT.md for the
 * design writeup and the cccc issues found along the way.
 *
 * Built into both `yas` and `libyas`, same as aot_shim. Comptime code
 * dlopens libyas and drives this API through dlsym, exactly like the
 * reader shim - only pointers/long/double/int/const char* cross that
 * boundary, for the same by-value-Atom reasons documented in aot_shim.h.
 */
#ifndef YAS_AOT_PLAN_H
#define YAS_AOT_PLAN_H

#ifdef __cplusplus
extern "C" {
#endif

struct YasPlan; /* one compiled file: a table of functions, each a flat op list */

/* NULL only on inability to even allocate; check yas_plan_error() first -
 * a non-NULL plan can still carry an error (unsupported input, same as
 * yas_aot_read_file()'s convention). */
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
 * body. The replay loop pushes/pops cccc Node* per YasOpKind; see
 * aot_plan_ops.h for the opcode set and operand meanings. */
int yas_plan_op_count(struct YasPlan *p, int fn);
int yas_plan_op_kind(struct YasPlan *p, int fn, int i);  /* YasOpKind */
int yas_plan_op_a(struct YasPlan *p, int fn, int i);     /* generic int operand */
int yas_plan_op_b(struct YasPlan *p, int fn, int i);     /* second int operand (CALL's argc) */
long   yas_plan_op_i(struct YasPlan *p, int fn, int i);  /* PUSH_INT literal */
double yas_plan_op_f(struct YasPlan *p, int fn, int i);  /* PUSH_FLOAT literal */

#ifdef __cplusplus
}
#endif

#endif /* YAS_AOT_PLAN_H */

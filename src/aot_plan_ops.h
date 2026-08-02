/* aot_plan_ops.h - the flat stack-machine opcode set shared between
 * aot_plan.c (which emits it, in ordinary C) and aot/aot.c (cccc comptime
 * code, which replays it into cccc reflection Node* trees). Kept
 * dependency-free (no cccc types, no yas Atom types) so both sides can
 * include it without pulling in anything else.
 *
 * Every op pushes exactly one value except STORE_LOCAL (pushes the
 * assignment expression, so it composes into a SEQ) - the stack is never
 * popped without something replacing it, which is what lets a flat list
 * stand in for a tree without ever needing recursion on the replay side.
 */
#ifndef YAS_AOT_PLAN_OPS_H
#define YAS_AOT_PLAN_OPS_H

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

#endif /* YAS_AOT_PLAN_OPS_H */

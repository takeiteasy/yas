/* aot_plan.c - see aot_plan.h for the why. Ordinary C; test with a plain
 * `cc` + libyas, no cccc involved (see aot/tests/plan_test.c). */
#include "aot_plan.h"
#include "yas.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FNS     64
#define MAX_PARAMS  16
#define MAX_LOCALS  64
#define MAX_CONSTS  32
#define MAX_SCOPE   (MAX_PARAMS + MAX_LOCALS)

/* ---- boxed-Atom reader front end ----
 *
 * A pointer-only view onto the yas reader, kept private to this file.
 * `YasAtom` (yas.h) is a 16-byte union passed by value; this pass never
 * needs it directly, so every form gets boxed on the heap and only
 * pointers/long/double/const char* flow through the rest of aot_plan.c
 * below. That's not a hard requirement of *this* file (it's ordinary C,
 * not comptime code - see aot_plan.h), but keeping the boundary here
 * means the exact same reader front end could still be reused by a
 * comptime-side caller later without redesigning it, and it keeps
 * yas.h's platform SIMD intrinic includes (xmmintrin.h / arm_neon.h /
 * ...) contained to this one translation unit instead of leaking into
 * aot_plan.h's public surface.
 *
 * Note: `YasForm`/`YasForms` are forward-declared as plain `struct X`
 * opaque handles below (not `typedef struct X X;`) even though they're
 * private to this file - matching the convention the rest of the AOT
 * pass uses for anything that might ever be @shared into a comptime TU,
 * so this stays safe to lift back out into its own header if a second
 * caller ever needs it. See docs/AOT.md for the full write-up of why
 * that idiom matters here specifically. */

typedef enum {
	YAS_FORM_NIL,
	YAS_FORM_PAIR,
	YAS_FORM_SYMBOL,
	YAS_FORM_INTEGER,
	YAS_FORM_FLOAT,
	YAS_FORM_STRING,
	YAS_FORM_CHAR,
	YAS_FORM_KEYWORD,
	YAS_FORM_OTHER /* vectors/hashes/etc: valid as a literal reader form but
	                * not meaningful to the AOT lowerer */
} YasFormKind;

struct YasForm {
	YasAtom atom;
};

struct YasForms {
	struct YasForm **items;
	int count;
	int capacity;
	char *error; /* NULL on success */
};

static struct YasForm *box(YasAtom a)
{
	struct YasForm *f = malloc(sizeof(struct YasForm));
	f->atom = a;
	return f;
}

static void forms_push(struct YasForms *forms, YasAtom a)
{
	if (forms->count == forms->capacity) {
		forms->capacity = forms->capacity ? forms->capacity * 2 : 16;
		forms->items = realloc(forms->items, sizeof(struct YasForm *) * forms->capacity);
	}
	forms->items[forms->count++] = box(a);
}

/* Read every top-level form from `path`. Returns NULL only on failure to
 * open the file; a read syntax error is reported via yas_aot_error()
 * instead, matching main.c's load_file() loop (including its handling of
 * YAS_ERROR_SKIP for reader-conditional-excluded forms, which yas_eval_source()
 * gets wrong - see docs/ARCHITECTURE.md). */
static struct YasForms *yas_aot_read_file(const char *path)
{
	struct YasForms *forms = calloc(1, sizeof(struct YasForms));

	char *text = yas_slurp(path);
	if (!text) {
		forms->error = malloc(256);
		snprintf(forms->error, 256, "could not read file: %s", path);
		return forms;
	}

	const char *p = text;
	YasErrorContext err = {0};
	err.pos.filename = path;
	err.pos.source = text;

	while (1) {
		err.code = YAS_ERROR_OK;
		err.message[0] = '\0';

		YasAtom expr;
		YasError read_err = yas_read_expr(p, &p, &expr, &err);
		if (read_err == YAS_ERROR_SKIP) {
			/* Reader conditional excluded this form (#+feature / #-feature).
			 * Unlike eval_source(), we must not treat this as EOF. */
			continue;
		}
		if (read_err) {
			if (err.message[0] != '\0') {
				forms->error = malloc(YAS_ERROR_MSG_MAX + 64);
				snprintf(forms->error, YAS_ERROR_MSG_MAX + 64, "%s:%d:%d: %s",
				         path, err.pos.line, err.pos.column, err.message);
			}
			break;
		}

		forms_push(forms, expr);
	}

	free(text);
	return forms;
}

static const char *yas_aot_error(struct YasForms *forms) { return forms->error; }
static int yas_aot_count(struct YasForms *forms) { return forms->count; }

static struct YasForm *yas_aot_at(struct YasForms *forms, int index)
{
	if (index < 0 || index >= forms->count) return NULL;
	return forms->items[index];
}

static void yas_aot_free(struct YasForms *forms)
{
	if (!forms) return;
	for (int i = 0; i < forms->count; i++) free(forms->items[i]);
	free(forms->items);
	free(forms->error);
	free(forms);
}

static YasFormKind yas_aot_kind(struct YasForm *form)
{
	if (!form) return YAS_FORM_NIL;
	switch (form->atom.type) {
	case YAS_ATOM_TYPE_NIL:     return YAS_FORM_NIL;
	case YAS_ATOM_TYPE_PAIR:    return YAS_FORM_PAIR;
	case YAS_ATOM_TYPE_SYMBOL:  return YAS_FORM_SYMBOL;
	case YAS_ATOM_TYPE_INTEGER: return YAS_FORM_INTEGER;
	case YAS_ATOM_TYPE_FLOAT:   return YAS_FORM_FLOAT;
	case YAS_ATOM_TYPE_STRING:  return YAS_FORM_STRING;
	case YAS_ATOM_TYPE_CHAR:    return YAS_FORM_CHAR;
	case YAS_ATOM_TYPE_KEYWORD: return YAS_FORM_KEYWORD;
	default:               return YAS_FORM_OTHER;
	}
}

static struct YasForm *yas_aot_car(struct YasForm *form)
{
	if (!form || form->atom.type != YAS_ATOM_TYPE_PAIR) return NULL;
	return box(yas_car(form->atom));
}

static struct YasForm *yas_aot_cdr(struct YasForm *form)
{
	if (!form || form->atom.type != YAS_ATOM_TYPE_PAIR) return NULL;
	return box(yas_cdr(form->atom));
}

static long yas_aot_int(struct YasForm *form)
{
	return form && form->atom.type == YAS_ATOM_TYPE_INTEGER ? form->atom.value.integer : 0;
}

static double yas_aot_float(struct YasForm *form)
{
	return form && form->atom.type == YAS_ATOM_TYPE_FLOAT ? form->atom.value.floatval : 0.0;
}

/* Interned, uppercase (the reader upcases all symbols - see read.c). Valid
 * for the process lifetime; do not free. */
static const char *yas_aot_sym(struct YasForm *form)
{
	return form && form->atom.type == YAS_ATOM_TYPE_SYMBOL ? form->atom.value.symbol : NULL;
}

/* ---- the AOT compiler proper (see aot_plan.h) ---- */

typedef struct {
	int kind;   /* YasOpKind */
	int a, b;
	long ival;
	double fval;
} Op;

typedef struct {
	char lisp_name[64];
	int  is_float;
	long ival;
	double dval;
} PlanConst;

typedef struct {
	char lisp_name[64];
	char c_name[96];
	int  param_count;
	char param_lisp[MAX_PARAMS][64];
	char param_c[MAX_PARAMS][96];
	struct YasForm *body; /* list of body forms (implicit begin) */
	int  is_float;        /* whole-function type: 0 = long, 1 = double */

	int local_count;
	int local_is_float[MAX_LOCALS];

	Op *ops;
	int op_count;
	int op_cap;
} PlanFn;

struct YasPlan {
	PlanFn fns[MAX_FNS];
	int fn_count;
	PlanConst consts[MAX_CONSTS];
	int const_count;
	char *error;
	struct YasForms *forms; /* kept alive: PlanFn.body points into it */
};

/* scope during lowering: either a parameter (PUSH_PARAM) or a let/let*
 * local (PUSH_LOCAL/STORE_LOCAL). Plain C, plain recursion - this is not
 * running inside cccc's comptime VM, so none of aot.c's constraints apply
 * here. */
typedef struct {
	char name[64];
	int is_param;   /* 1 = param index in `index`, 0 = local slot in `index` */
	int index;
	int is_float;
} Scope;

static void set_error(struct YasPlan *p, const char *fmt, ...)
{
	if (p->error) return; /* keep the first error */
	char *buf = malloc(512);
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, 512, fmt, ap);
	va_end(ap);
	p->error = buf;
}

static void emit(PlanFn *fn, int kind, int a, int b, long ival, double fval)
{
	if (fn->op_count == fn->op_cap) {
		fn->op_cap = fn->op_cap ? fn->op_cap * 2 : 32;
		fn->ops = realloc(fn->ops, sizeof(Op) * (size_t)fn->op_cap);
	}
	Op *op = &fn->ops[fn->op_count++];
	op->kind = kind; op->a = a; op->b = b; op->ival = ival; op->fval = fval;
}

/* ---- name mangling: lowercase; '-' -> '_'; '?' -> "_p"; '!' -> "_bang";
 * '*' -> "_star"; '/' -> "_slash"; '>' -> "_gt"; '<' -> "_lt"; '=' -> "_eq";
 * everything else unrecognized -> '_'; leading digit gets a '_' prefix.
 * See docs/AOT.md. ---- */
static void mangle(const char *name, char *out, int outsz)
{
	int oi = 0;
	for (int i = 0; name[i] && oi < outsz - 8; i++) {
		char c = name[i];
		if (c >= 'A' && c <= 'Z')      out[oi++] = (char)(c - 'A' + 'a');
		else if (c == '-')              out[oi++] = '_';
		else if (c >= '0' && c <= '9')  out[oi++] = c;
		else if (c == '_')              out[oi++] = c;
		else if (c == '?')            { out[oi++] = '_'; out[oi++] = 'p'; }
		else if (c == '!')            { const char *s = "_bang";  while (*s) out[oi++] = *s++; }
		else if (c == '*')            { const char *s = "_star";  while (*s) out[oi++] = *s++; }
		else if (c == '/')            { const char *s = "_slash"; while (*s) out[oi++] = *s++; }
		else if (c == '>')            { const char *s = "_gt";    while (*s) out[oi++] = *s++; }
		else if (c == '<')            { const char *s = "_lt";    while (*s) out[oi++] = *s++; }
		else if (c == '=')            { const char *s = "_eq";    while (*s) out[oi++] = *s++; }
		else                             out[oi++] = '_';
	}
	if (oi == 0) out[oi++] = '_';
	out[oi] = '\0';
	if (out[0] >= '0' && out[0] <= '9') {
		memmove(out + 1, out, (size_t)oi + 1);
		out[0] = '_';
	}
}

static struct YasForm *nth_cdr(struct YasForm *f, int n)
{
	while (n-- > 0 && f) f = yas_aot_cdr(f);
	return f;
}

static struct YasForm *nth(struct YasForm *f, int n)
{
	struct YasForm *c = nth_cdr(f, n);
	return c ? yas_aot_car(c) : NULL;
}

static int list_len(struct YasForm *f)
{
	int n = 0;
	while (f && yas_aot_kind(f) == YAS_FORM_PAIR) { n++; f = yas_aot_cdr(f); }
	return n;
}

static int find_fn(struct YasPlan *p, const char *name)
{
	for (int i = 0; i < p->fn_count; i++)
		if (strcmp(p->fns[i].lisp_name, name) == 0) return i;
	return -1;
}

static int find_const(struct YasPlan *p, const char *name)
{
	for (int i = 0; i < p->const_count; i++)
		if (strcmp(p->consts[i].lisp_name, name) == 0) return i;
	return -1;
}

/* ---- type inference: a function is `double` if any part of its body is a
 * float literal, or calls a function already known to be `double`. Fixed
 * point over the whole file. Deliberately this simple - see docs/AOT.md. ---- */

static int expr_is_float(struct YasPlan *p, struct YasForm *e)
{
	if (!e) return 0;
	YasFormKind k = yas_aot_kind(e);
	if (k == YAS_FORM_FLOAT) return 1;
	if (k == YAS_FORM_SYMBOL) {
		int ci = find_const(p, yas_aot_sym(e));
		return ci >= 0 && p->consts[ci].is_float;
	}
	if (k != YAS_FORM_PAIR) return 0;

	struct YasForm *opf = yas_aot_car(e);
	const char *op = yas_aot_kind(opf) == YAS_FORM_SYMBOL ? yas_aot_sym(opf) : NULL;
	if (!op) return 0;

	if (strcmp(op, "IF") == 0)
		return expr_is_float(p, nth(e, 2)) || expr_is_float(p, nth(e, 3));

	if (strcmp(op, "BEGIN") == 0) {
		int r = 0;
		for (struct YasForm *b = yas_aot_cdr(e); b && yas_aot_kind(b) == YAS_FORM_PAIR; b = yas_aot_cdr(b))
			if (expr_is_float(p, yas_aot_car(b))) r = 1;
		return r;
	}

	if (strcmp(op, "LET") == 0 || strcmp(op, "LET*") == 0) {
		int r = 0;
		for (struct YasForm *b = nth(e, 1); b && yas_aot_kind(b) == YAS_FORM_PAIR; b = yas_aot_cdr(b))
			if (expr_is_float(p, nth(yas_aot_car(b), 1))) r = 1;
		for (struct YasForm *b = nth_cdr(e, 2); b && yas_aot_kind(b) == YAS_FORM_PAIR; b = yas_aot_cdr(b))
			if (expr_is_float(p, yas_aot_car(b))) r = 1;
		return r;
	}

	if (strcmp(op, "+") == 0 || strcmp(op, "-") == 0 ||
	    strcmp(op, "*") == 0 || strcmp(op, "/") == 0) {
		for (struct YasForm *a = yas_aot_cdr(e); a && yas_aot_kind(a) == YAS_FORM_PAIR; a = yas_aot_cdr(a))
			if (expr_is_float(p, yas_aot_car(a))) return 1;
		return 0;
	}

	/* comparisons/booleans always yield 0/1 (long) in this subset */
	if (strcmp(op, "<") == 0 || strcmp(op, ">") == 0 || strcmp(op, "<=") == 0 ||
	    strcmp(op, ">=") == 0 || strcmp(op, "=") == 0 || strcmp(op, "AND") == 0 ||
	    strcmp(op, "OR") == 0 || strcmp(op, "NOT") == 0)
		return 0;

	int idx = find_fn(p, op);
	return idx >= 0 && p->fns[idx].is_float;
}

static void infer_types(struct YasPlan *p)
{
	int changed = 1;
	while (changed) {
		changed = 0;
		for (int i = 0; i < p->fn_count; i++) {
			int r = 0;
			for (struct YasForm *b = p->fns[i].body; b && yas_aot_kind(b) == YAS_FORM_PAIR; b = yas_aot_cdr(b))
				if (expr_is_float(p, yas_aot_car(b))) r = 1;
			if (!p->fns[i].is_float && r) { p->fns[i].is_float = 1; changed = 1; }
		}
	}
}

/* ---- lowering: emits a flat op list per function. `scope` covers params
 * (fixed for the whole function) plus whatever let/let* locals are
 * currently in view; `*scope_count` grows/shrinks as LET/LET* push and
 * pop their bindings, same shape as a normal lexical-scope stack. ---- */

static void lower_expr(struct YasPlan *p, PlanFn *fn, struct YasForm *e,
                        Scope *scope, int *scope_count, int *out_is_float);

static void coerce(PlanFn *fn, int is_float, int want_float)
{
	if (want_float && !is_float) emit(fn, YAS_OP_CAST_DOUBLE, 0, 0, 0, 0);
	else if (!want_float && is_float) emit(fn, YAS_OP_CAST_LONG, 0, 0, 0, 0);
}

/* lower a list of forms (implicit begin), keeping only the last value -
 * used for BEGIN, LET/LET* bodies, and top-level function bodies. */
static void lower_seq(struct YasPlan *p, PlanFn *fn, struct YasForm *body,
                       Scope *scope, int *scope_count, int *out_is_float)
{
	int n = 0, last_float = 0;
	for (struct YasForm *b = body; b && yas_aot_kind(b) == YAS_FORM_PAIR; b = yas_aot_cdr(b)) {
		int f;
		lower_expr(p, fn, yas_aot_car(b), scope, scope_count, &f);
		last_float = f;
		n++;
	}
	if (n == 0) { emit(fn, YAS_OP_PUSH_INT, 0, 0, 0, 0); *out_is_float = 0; return; }
	if (n > 1) emit(fn, YAS_OP_SEQ, n, 0, 0, 0);
	*out_is_float = last_float;
}

static void lower_expr(struct YasPlan *p, PlanFn *fn, struct YasForm *e,
                        Scope *scope, int *scope_count, int *out_is_float)
{
	if (p->error) return;
	YasFormKind k = yas_aot_kind(e);

	if (k == YAS_FORM_INTEGER) {
		emit(fn, YAS_OP_PUSH_INT, 0, 0, yas_aot_int(e), 0);
		*out_is_float = 0;
		return;
	}
	if (k == YAS_FORM_FLOAT) {
		emit(fn, YAS_OP_PUSH_FLOAT, 0, 0, 0, yas_aot_float(e));
		*out_is_float = 1;
		return;
	}
	if (k == YAS_FORM_SYMBOL) {
		const char *name = yas_aot_sym(e);
		for (int i = *scope_count - 1; i >= 0; i--) {
			if (strcmp(scope[i].name, name) == 0) {
				emit(fn, scope[i].is_param ? YAS_OP_PUSH_PARAM : YAS_OP_PUSH_LOCAL,
				     scope[i].index, 0, 0, 0);
				*out_is_float = scope[i].is_float;
				return;
			}
		}
		int ci = find_const(p, name);
		if (ci >= 0) {
			if (p->consts[ci].is_float) emit(fn, YAS_OP_PUSH_FLOAT, 0, 0, 0, p->consts[ci].dval);
			else emit(fn, YAS_OP_PUSH_INT, 0, 0, p->consts[ci].ival, 0);
			*out_is_float = p->consts[ci].is_float;
			return;
		}
		set_error(p, "yas-aot: unbound variable %s (the AOT subset has no "
		             "closures or globals besides AOT-compiled defines) - "
		             "see docs/AOT.md", name);
		*out_is_float = 0;
		return;
	}
	if (k != YAS_FORM_PAIR) {
		set_error(p, "yas-aot: unsupported literal in the AOT subset "
		             "(strings/vectors/hashes are not supported) - see docs/AOT.md");
		*out_is_float = 0;
		return;
	}

	struct YasForm *opf = yas_aot_car(e);
	const char *op = yas_aot_kind(opf) == YAS_FORM_SYMBOL ? yas_aot_sym(opf) : NULL;
	if (!op) {
		set_error(p, "yas-aot: call target must be a symbol in the AOT subset");
		*out_is_float = 0;
		return;
	}

	if (strcmp(op, "IF") == 0) {
		struct YasForm *then_form = nth(e, 2);
		struct YasForm *else_form = nth(e, 3);
		/* Precompute the result type via the pure static analysis pass
		 * (expr_is_float has no side effects / emits no ops), so each
		 * branch's cast can be emitted right after it's pushed - a postfix
		 * stream has no way to go back and patch an earlier push once the
		 * next one has started. */
		int rf = expr_is_float(p, then_form) || (else_form && expr_is_float(p, else_form));

		int cf;
		lower_expr(p, fn, nth(e, 1), scope, scope_count, &cf);

		int tf;
		lower_expr(p, fn, then_form, scope, scope_count, &tf);
		coerce(fn, tf, rf);

		if (else_form) {
			int ef;
			lower_expr(p, fn, else_form, scope, scope_count, &ef);
			coerce(fn, ef, rf);
		} else {
			emit(fn, rf ? YAS_OP_PUSH_FLOAT : YAS_OP_PUSH_INT, 0, 0, 0, 0);
		}

		emit(fn, YAS_OP_COND, 0, 0, 0, 0);
		*out_is_float = rf;
		return;
	}

	if (strcmp(op, "BEGIN") == 0) {
		lower_seq(p, fn, yas_aot_cdr(e), scope, scope_count, out_is_float);
		return;
	}

	if (strcmp(op, "LET") == 0 || strcmp(op, "LET*") == 0) {
		int is_star = strcmp(op, "LET*") == 0;
		struct YasForm *bindings = nth(e, 1);
		int saved_scope = *scope_count;
		int n = 0;
		int slots[MAX_LOCALS];
		char names[MAX_LOCALS][64];
		int floats[MAX_LOCALS];

		for (struct YasForm *b = bindings; b && yas_aot_kind(b) == YAS_FORM_PAIR && n < MAX_LOCALS; b = yas_aot_cdr(b)) {
			struct YasForm *binding = yas_aot_car(b);
			struct YasForm *name_form = yas_aot_car(binding);
			const char *vname = yas_aot_kind(name_form) == YAS_FORM_SYMBOL ? yas_aot_sym(name_form) : NULL;
			if (!vname) { set_error(p, "yas-aot: let binding name must be a symbol"); *out_is_float = 0; return; }

			int init_float;
			lower_expr(p, fn, nth(binding, 1), scope, scope_count, &init_float);
			if (p->error) return;

			if (fn->local_count >= MAX_LOCALS) { set_error(p, "yas-aot: too many let locals (max %d)", MAX_LOCALS); return; }
			int slot = fn->local_count++;
			fn->local_is_float[slot] = init_float;
			emit(fn, YAS_OP_STORE_LOCAL, slot, 0, 0, 0);

			strncpy(names[n], vname, 63);
			slots[n] = slot;
			floats[n] = init_float;

			if (is_star) {
				/* let*: visible to subsequent inits immediately */
				strncpy(scope[*scope_count].name, vname, 63);
				scope[*scope_count].is_param = 0;
				scope[*scope_count].index = slot;
				scope[*scope_count].is_float = init_float;
				(*scope_count)++;
			}
			n++;
		}
		if (!is_star) {
			/* let: parallel - push all bindings now, after every init was
			 * evaluated against the outer scope only. */
			for (int i = 0; i < n; i++) {
				strncpy(scope[saved_scope + i].name, names[i], 63);
				scope[saved_scope + i].is_param = 0;
				scope[saved_scope + i].index = slots[i];
				scope[saved_scope + i].is_float = floats[i];
			}
			*scope_count = saved_scope + n;
		}

		int bf;
		lower_seq(p, fn, nth_cdr(e, 2), scope, scope_count, &bf);
		*scope_count = saved_scope;

		if (n > 0) emit(fn, YAS_OP_SEQ, n + 1, 0, 0, 0);
		*out_is_float = bf;
		return;
	}

	if (strcmp(op, "+") == 0 || strcmp(op, "-") == 0 ||
	    strcmp(op, "*") == 0 || strcmp(op, "/") == 0) {
		struct YasForm *args = yas_aot_cdr(e);
		int argc = list_len(args);
		if (argc == 0) { set_error(p, "yas-aot: %s needs at least 1 argument", op); *out_is_float = 0; return; }
		if (argc > 16) { set_error(p, "yas-aot: %s has too many arguments (max 16)", op); *out_is_float = 0; return; }

		struct YasForm *argv[16];
		int i = 0;
		for (struct YasForm *a = args; a && yas_aot_kind(a) == YAS_FORM_PAIR; a = yas_aot_cdr(a)) argv[i++] = yas_aot_car(a);

		/* Precompute the result type (pure static analysis, no emission),
		 * same reasoning as IF above: a postfix stream can't patch an
		 * earlier push once later ones start, so every operand needs its
		 * cast decided and emitted immediately after it's pushed. */
		int rf = 0;
		for (int j = 0; j < argc; j++) if (expr_is_float(p, argv[j])) rf = 1;

		int binop = strcmp(op, "+") == 0 ? YAS_BINOP_ADD
		          : strcmp(op, "-") == 0 ? YAS_BINOP_SUB
		          : strcmp(op, "*") == 0 ? YAS_BINOP_MUL : YAS_BINOP_DIV;

		if (argc == 1 && strcmp(op, "-") == 0) {
			int f; lower_expr(p, fn, argv[0], scope, scope_count, &f); coerce(fn, f, rf);
			emit(fn, YAS_OP_UNOP, YAS_UNOP_NEG, 0, 0, 0);
		} else if (argc == 1 && strcmp(op, "/") == 0) {
			/* (/ x) => 1/x: push the reciprocal's numerator first, then x,
			 * then divide - ordinary postfix, no special support needed. */
			emit(fn, rf ? YAS_OP_PUSH_FLOAT : YAS_OP_PUSH_INT, 0, 0, 1, 1.0);
			int f; lower_expr(p, fn, argv[0], scope, scope_count, &f); coerce(fn, f, rf);
			emit(fn, YAS_OP_BINOP, YAS_BINOP_DIV, 0, 0, 0);
		} else if (argc == 1) {
			int f; lower_expr(p, fn, argv[0], scope, scope_count, &f); coerce(fn, f, rf);
		} else {
			int f; lower_expr(p, fn, argv[0], scope, scope_count, &f); coerce(fn, f, rf);
			for (int j = 1; j < argc; j++) {
				lower_expr(p, fn, argv[j], scope, scope_count, &f); coerce(fn, f, rf);
				emit(fn, YAS_OP_BINOP, binop, 0, 0, 0);
			}
		}
		*out_is_float = rf;
		return;
	}

	if (strcmp(op, "<") == 0 || strcmp(op, ">") == 0 || strcmp(op, "<=") == 0 ||
	    strcmp(op, ">=") == 0 || strcmp(op, "=") == 0) {
		struct YasForm *args = yas_aot_cdr(e);
		int argc = list_len(args);
		if (argc < 2) { set_error(p, "yas-aot: %s needs at least 2 arguments", op); *out_is_float = 0; return; }
		if (argc > 16) { set_error(p, "yas-aot: %s has too many arguments (max 16)", op); *out_is_float = 0; return; }

		struct YasForm *argv[16];
		int i = 0;
		for (struct YasForm *a = args; a && yas_aot_kind(a) == YAS_FORM_PAIR; a = yas_aot_cdr(a)) argv[i++] = yas_aot_car(a);

		int cf = 0;
		for (int j = 0; j < argc; j++) if (expr_is_float(p, argv[j])) cf = 1;

		int binop = strcmp(op, "<") == 0 || strcmp(op, ">") == 0 ? YAS_BINOP_LT
		          : strcmp(op, "<=") == 0 || strcmp(op, ">=") == 0 ? YAS_BINOP_LE
		          : YAS_BINOP_EQ;
		int swapped = strcmp(op, ">") == 0 || strcmp(op, ">=") == 0;

		/* Chain via re-lowering: each adjacent pair is lowered fresh (each
		 * interior operand gets lowered twice, once per pair it's in) and
		 * compared, then combined with the running result via LOGAND. A
		 * stack machine has no random access into earlier pushes without a
		 * DUP op, and this subset doesn't need one badly enough to add it -
		 * cheap since these are just AST rebuilds, not runtime work. */
		for (int j = 0; j < argc - 1; j++) {
			struct YasForm *lhs = swapped ? argv[j + 1] : argv[j];
			struct YasForm *rhs = swapped ? argv[j] : argv[j + 1];
			int f;
			lower_expr(p, fn, lhs, scope, scope_count, &f); coerce(fn, f, cf);
			lower_expr(p, fn, rhs, scope, scope_count, &f); coerce(fn, f, cf);
			emit(fn, YAS_OP_BINOP, binop, 0, 0, 0);
			if (j > 0) emit(fn, YAS_OP_BINOP, YAS_BINOP_LOGAND, 0, 0, 0);
		}
		*out_is_float = 0;
		return;
	}

	if (strcmp(op, "AND") == 0 || strcmp(op, "OR") == 0) {
		int binop = strcmp(op, "AND") == 0 ? YAS_BINOP_LOGAND : YAS_BINOP_LOGOR;
		struct YasForm *args = yas_aot_cdr(e);
		int argc = list_len(args);
		*out_is_float = 0;
		if (argc == 0) { emit(fn, YAS_OP_PUSH_INT, 0, 0, strcmp(op, "AND") == 0 ? 1 : 0, 0); return; }
		int i = 0;
		for (struct YasForm *a = args; a && yas_aot_kind(a) == YAS_FORM_PAIR && i < 16; a = yas_aot_cdr(a), i++) {
			int f;
			lower_expr(p, fn, yas_aot_car(a), scope, scope_count, &f);
			if (i > 0) emit(fn, YAS_OP_BINOP, binop, 0, 0, 0);
		}
		return;
	}

	if (strcmp(op, "NOT") == 0) {
		int f;
		lower_expr(p, fn, nth(e, 1), scope, scope_count, &f);
		emit(fn, YAS_OP_UNOP, YAS_UNOP_NOT, 0, 0, 0);
		*out_is_float = 0;
		return;
	}

	int idx = find_fn(p, op);
	if (idx < 0) {
		set_error(p, "yas-aot: unsupported form or unknown function `%s` "
		             "(the AOT subset supports define/if/let/let*/begin, "
		             "+ - * /, < > <= >= =, and/or/not, and calls to other "
		             "AOT-compiled functions) - see docs/AOT.md", op);
		*out_is_float = 0;
		return;
	}
	PlanFn *callee = &p->fns[idx];
	struct YasForm *args = yas_aot_cdr(e);
	int argc = list_len(args);
	if (argc != callee->param_count) {
		set_error(p, "yas-aot: %s expects %d argument(s), got %d",
		          callee->lisp_name, callee->param_count, argc);
		*out_is_float = 0;
		return;
	}
	int i = 0;
	for (struct YasForm *a = args; a && yas_aot_kind(a) == YAS_FORM_PAIR && i < 16; a = yas_aot_cdr(a), i++) {
		int f;
		lower_expr(p, fn, yas_aot_car(a), scope, scope_count, &f);
		coerce(fn, f, callee->is_float);
	}
	emit(fn, YAS_OP_CALL, idx, argc, 0, 0);
	*out_is_float = callee->is_float;
}

struct YasPlan *yas_aot_plan(const char *path)
{
	struct YasPlan *p = calloc(1, sizeof(struct YasPlan));
	p->forms = yas_aot_read_file(path);
	const char *err = yas_aot_error(p->forms);
	if (err) { set_error(p, "yas-aot: %s", err); return p; }

	int n = yas_aot_count(p->forms);
	for (int i = 0; i < n; i++) {
		struct YasForm *top = yas_aot_at(p->forms, i);
		if (yas_aot_kind(top) != YAS_FORM_PAIR) {
			set_error(p, "yas-aot: unsupported top-level form (only DEFINE is "
			             "supported) - see docs/AOT.md");
			return p;
		}
		struct YasForm *op = yas_aot_car(top);
		if (yas_aot_kind(op) != YAS_FORM_SYMBOL || strcmp(yas_aot_sym(op), "DEFINE") != 0) {
			set_error(p, "yas-aot: only top-level DEFINE forms are supported "
			             "in the AOT subset - see docs/AOT.md");
			return p;
		}

		struct YasForm *target = nth(top, 1);

		if (yas_aot_kind(target) == YAS_FORM_SYMBOL) {
			if (p->const_count >= MAX_CONSTS) { set_error(p, "yas-aot: too many constants (max %d)", MAX_CONSTS); return p; }
			struct YasForm *val = nth(top, 2);
			YasFormKind vk = yas_aot_kind(val);
			if (vk != YAS_FORM_INTEGER && vk != YAS_FORM_FLOAT) {
				set_error(p, "yas-aot: constant `%s` must be a literal number "
				             "(computed globals are not supported) - see docs/AOT.md",
				          yas_aot_sym(target));
				return p;
			}
			PlanConst *c = &p->consts[p->const_count++];
			strncpy(c->lisp_name, yas_aot_sym(target), 63);
			c->is_float = vk == YAS_FORM_FLOAT;
			if (c->is_float) c->dval = yas_aot_float(val); else c->ival = yas_aot_int(val);
			continue;
		}

		if (yas_aot_kind(target) != YAS_FORM_PAIR) {
			set_error(p, "yas-aot: unsupported define target - see docs/AOT.md");
			return p;
		}

		if (p->fn_count >= MAX_FNS) { set_error(p, "yas-aot: too many functions (max %d)", MAX_FNS); return p; }
		PlanFn *fn = &p->fns[p->fn_count++];
		struct YasForm *name_form = yas_aot_car(target);
		const char *lisp_name = yas_aot_kind(name_form) == YAS_FORM_SYMBOL ? yas_aot_sym(name_form) : NULL;
		if (!lisp_name) { set_error(p, "yas-aot: function name must be a symbol"); return p; }
		strncpy(fn->lisp_name, lisp_name, 63);
		mangle(lisp_name, fn->c_name, sizeof(fn->c_name));

		struct YasForm *param = yas_aot_cdr(target);
		while (param && yas_aot_kind(param) == YAS_FORM_PAIR) {
			struct YasForm *pn = yas_aot_car(param);
			const char *pname = yas_aot_kind(pn) == YAS_FORM_SYMBOL ? yas_aot_sym(pn) : NULL;
			if (!pname) {
				set_error(p, "yas-aot: unsupported lambda-list in %s (only plain "
				             "positional parameters are supported) - see docs/AOT.md", lisp_name);
				return p;
			}
			if (fn->param_count >= MAX_PARAMS) {
				set_error(p, "yas-aot: %s has too many parameters (max %d)", lisp_name, MAX_PARAMS);
				return p;
			}
			strncpy(fn->param_lisp[fn->param_count], pname, 63);
			mangle(pname, fn->param_c[fn->param_count], sizeof(fn->param_c[0]));
			fn->param_count++;
			param = yas_aot_cdr(param);
		}
		if (param && yas_aot_kind(param) != YAS_FORM_NIL) {
			set_error(p, "yas-aot: variadic/rest parameters in %s are not "
			             "supported in the AOT subset - see docs/AOT.md", lisp_name);
			return p;
		}

		fn->body = nth_cdr(top, 2);
	}

	infer_types(p);

	for (int i = 0; i < p->fn_count; i++) {
		PlanFn *fn = &p->fns[i];
		Scope scope[MAX_SCOPE];
		int scope_count = 0;
		for (int j = 0; j < fn->param_count; j++) {
			strncpy(scope[scope_count].name, fn->param_lisp[j], 63);
			scope[scope_count].is_param = 1;
			scope[scope_count].index = j;
			scope[scope_count].is_float = fn->is_float;
			scope_count++;
		}
		int bf;
		lower_seq(p, fn, fn->body, scope, &scope_count, &bf);
		coerce(fn, bf, fn->is_float);
		if (p->error) return p;
	}

	return p;
}

const char *yas_plan_error(struct YasPlan *p) { return p->error; }

void yas_plan_free(struct YasPlan *p)
{
	if (!p) return;
	for (int i = 0; i < p->fn_count; i++) free(p->fns[i].ops);
	yas_aot_free(p->forms);
	free(p->error);
	free(p);
}

int yas_plan_fn_count(struct YasPlan *p) { return p->fn_count; }
const char *yas_plan_fn_name(struct YasPlan *p, int fn) { return p->fns[fn].c_name; }
int yas_plan_fn_is_float(struct YasPlan *p, int fn) { return p->fns[fn].is_float; }
int yas_plan_fn_param_count(struct YasPlan *p, int fn) { return p->fns[fn].param_count; }
const char *yas_plan_fn_param_name(struct YasPlan *p, int fn, int param) { return p->fns[fn].param_c[param]; }
int yas_plan_fn_local_count(struct YasPlan *p, int fn) { return p->fns[fn].local_count; }
int yas_plan_fn_local_is_float(struct YasPlan *p, int fn, int slot) { return p->fns[fn].local_is_float[slot]; }

int yas_plan_op_count(struct YasPlan *p, int fn) { return p->fns[fn].op_count; }
int yas_plan_op_kind(struct YasPlan *p, int fn, int i) { return p->fns[fn].ops[i].kind; }
int yas_plan_op_a(struct YasPlan *p, int fn, int i) { return p->fns[fn].ops[i].a; }
int yas_plan_op_b(struct YasPlan *p, int fn, int i) { return p->fns[fn].ops[i].b; }
long yas_plan_op_i(struct YasPlan *p, int fn, int i) { return p->fns[fn].ops[i].ival; }
double yas_plan_op_f(struct YasPlan *p, int fn, int i) { return p->fns[fn].ops[i].fval; }

#include "yas.h"
#include "context.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* Forward declaration */

/* The EvalRoot chain lives in the bound context (context.h) - LIFO
 * windows of the thread driving it, so no cross-thread interleaving is
 * possible by construction. */
#define eval_roots (yas_ctx->eval_roots)
static int eval_vm(YasAtom expr, YasAtom env, YasAtom *result, struct EvalRoot *root);

/* Called from data.c's gc_mark_roots() - the live evaluator's roots are
 * pointers to eval_vm()'s own C locals (expr/env/stack/result), not to
 * anything an embedder or a nested yas_eval_expr() caller can see, so
 * they stay a separate root source from the yas_gc_push_root() shadow
 * stack. Two root sources, one collector. */
static void yas_eval_mark_roots_impl(void)
{
    struct EvalRoot *r = eval_roots;
    while (r) {
        yas_gc_mark(*r->expr);
        yas_gc_mark(*r->env);
        if (!yas_nilp(*r->stack))
            yas_gc_mark(*r->stack);
        yas_gc_mark(*r->result);
        r = r->prev;
    }
}

void yas_eval_mark_roots(void)
{
    yas_eval_mark_roots_impl();
}


static int eval_do_return(YasAtom *stack, YasAtom *expr, YasAtom *env, YasAtom *result);
static int eval_do_exec(YasAtom *stack, YasAtom *expr, YasAtom *env);
static int eval_do_bind(YasAtom *stack, YasAtom *expr, YasAtom *env);
static int eval_do_apply(YasAtom *stack, YasAtom *expr, YasAtom *env, YasAtom *result);

/* yas_env_create()/yas_env_define()/yas_env_get()/yas_env_set() now live
 * in data.c, alongside yas_make_hash()/gc_alloc()/gc_account_extra() -
 * the hashed-frame representation (struct YasEnv, yas.h) needs those
 * GC-internal statics, which aren't visible outside data.c. See
 * docs/ARCHITECTURE.md's "Environments" section. */


/* Current file being evaluated (for relative include paths) */

/* Resolve a path relative to the current file */
static char *resolve_include_path(const char *include_path)
{
    /* If absolute path or no current file, use as-is */
    if (include_path[0] == '/' || !yas_current_file_path) {
        char *result = malloc(strlen(include_path) + 1);
        strcpy(result, include_path);
        return result;
    }

    /* Find directory of current file */
    const char *last_slash = strrchr(yas_current_file_path, '/');
    if (!last_slash) {
        /* Current file has no directory component */
        char *result = malloc(strlen(include_path) + 1);
        strcpy(result, include_path);
        return result;
    }

    /* Build path: directory + "/" + include_path */
    size_t dir_len = last_slash - yas_current_file_path + 1;
    size_t total_len = dir_len + strlen(include_path) + 1;
    char *result = malloc(total_len);
    strncpy(result, yas_current_file_path, dir_len);
    result[dir_len] = '\0';
    strcat(result, include_path);
    return result;
}

/* Helper to evaluate include - reads file, parses, evaluates all exprs */
/* Evaluate source string */
static int yas_eval_source_impl(const char *text, const char *path, YasAtom env, YasAtom *result)
{
    const char *saved_path = yas_current_file_path;
    yas_current_file_path = path;

    const char *p = text;
    YasAtom expr = yas_nil;
    YasErrorContext err = {0};
    err.pos.filename = path;
    err.pos.source = text;
    *result = yas_nil;

    /* `env` and `*result` both span the yas_eval_expr() call below, which
     * can trigger a nested GC step. For every caller in module.c this
     * `env` is a bare yas_env_create(yas_nil) temporary that nothing
     * else references - reachable only transiently as the nested
     * eval_vm's own `env`, which gets reassigned on closure entry - so
     * an import that triggers a collection mid-load can free the
     * top-level env out from under the rest of this loop. `*result`
     * holds the *previous* form's value, live across the *next*
     * iteration's call and equally unreachable otherwise (it is not the
     * nested eval_vm's own result slot - that's `&ignore` below, rooted
     * separately per call). One root each covers every iteration, since
     * yas_gc_push_root() tracks the slot's address, not a snapshot.
     *
     * `expr` also needs its own root: yas_read_expr() (read.c)
     * accumulates it across many allocating calls (read_list()'s own
     * unrooted accumulator, one yas_cons() per list element) with no
     * protection of its own before this function ever sees it - a large
     * top-level form is exactly the kind
     * of long allocation run that can trip the collector mid-parse. */
    yas_gc_push_root(&env);
    yas_gc_push_root(result);
    yas_gc_push_root(&expr);

    while (1) {
        err.code = YAS_ERROR_OK;
        err.message[0] = '\0';

        /* Balance check for the reader's own push_root()/pop_roots()
         * pairs (read.c) - read_list() and read_expr_internal()'s
         * quote/quasiquote/unquote/string-interpolation/#hash(/#(
         * branches each push and pop around their own locals, several
         * with multiple early-return error paths. A parse failure
         * exercises those error paths far less often than a successful
         * one, so verify balance on every call, not just success. */
        int reader_roots_before = yas_gc_root_count();
        YasError read_err = yas_read_expr(p, &p, &expr, &err);
        assert(yas_gc_root_count() == reader_roots_before);
        if (read_err == YAS_ERROR_SKIP) {
            /* Feature conditional excluded this expression, continue */
            continue;
        }
        if (read_err) {
            if (err.message[0] != '\0') {
                yas_error_print(&err);
                yas_current_file_path = saved_path;
                yas_gc_pop_roots(3);
                return YAS_ERROR_SYNTAX;
            }
            break; /* End of file */
        }

        YasAtom ignore;
        YasError eval_err = yas_eval_expr(expr, env, &ignore);
        if (eval_err) {
            yas_current_file_path = saved_path;
            yas_gc_pop_roots(3);
            return eval_err;
        }
        *result = ignore; // result is last expr
    }

    yas_current_file_path = saved_path;
    yas_gc_pop_roots(3);
    return YAS_ERROR_OK;
}

int yas_eval_source(const char *text, const char *path, YasAtom env, YasAtom *result)
{
    int r = yas_eval_source_impl(text, path, env, result);
    return r;
}


/* Helper to evaluate include - reads file, parses, evaluates all exprs */
static int yas_eval_file_impl(const char *path, YasAtom env, YasAtom *result, int case_insensitive)
{
    char *resolved = resolve_include_path(path);
    char *text = yas_slurp(resolved);

    if (!text) {
        free(resolved);
        return yas_raise_error("file", "Cannot open file for include");
    }

    int err = yas_eval_source(text, resolved, env, result);

    free(text);
    free(resolved);
    return err;
}

int yas_eval_file(const char *path, YasAtom env, YasAtom *result, int case_insensitive)
{
    int r = yas_eval_file_impl(path, env, result, case_insensitive);
    return r;
}


static int make_closure(YasAtom env, YasAtom args, YasAtom body, YasAtom *result)
{
    YasAtom p;

    /* No more up-front yas_listp(body) here (#63) - that walked the
     * whole body spine on every LAMBDA/DEFMACRO/closure-shape DEFINE
     * evaluation, directly in the `closure` bench workload. `body` is
     * yas_cdr(args) at every call site below, and the caller's
     * arity_ok(args, 2, -1) already established `args` and `body` are
     * both pairs (docstring check just below relies on that); a body
     * cell past the first is now validated incrementally, one form per
     * call, by eval_do_exec() as the closure actually runs. */

    /* Check argument names are valid:
     * - Symbol (regular arg or rest)
     * - Keyword (keyword arg marker)
     * - Pair with symbol car ([name default])
     */
    p = args;
    while (!yas_nilp(p)) {
        if (p.type == YAS_ATOM_TYPE_SYMBOL)
            break; /* Rest arg */
        if (p.type != YAS_ATOM_TYPE_PAIR)
            return YAS_ERROR_TYPE;

        YasAtom arg_spec = yas_car(p);
        if (arg_spec.type == YAS_ATOM_TYPE_SYMBOL) {
            /* Regular arg */
        } else if (arg_spec.type == YAS_ATOM_TYPE_KEYWORD) {
            /* Keyword marker - ok */
        } else if (arg_spec.type == YAS_ATOM_TYPE_PAIR) {
            /* [name default] - check that car is symbol */
            if (yas_car(arg_spec).type != YAS_ATOM_TYPE_SYMBOL)
                return YAS_ERROR_TYPE;
        } else {
            return YAS_ERROR_TYPE;
        }
        p = yas_cdr(p);
    }

    /* Extract docstring if first body form is a string and there's more body */
    YasAtom docstring = yas_nil;
    if (body.type == YAS_ATOM_TYPE_PAIR && yas_car(body).type == YAS_ATOM_TYPE_STRING &&
        !yas_nilp(yas_cdr(body))) {
        docstring = yas_car(body);
        body = yas_cdr(body);
    }

    /* Structure: (env (args docstring . body)) */
    *result = yas_cons(env, yas_cons(args, yas_cons(docstring, body)));
    result->type = YAS_ATOM_TYPE_CLOSURE;

    return YAS_ERROR_OK;
}

static YasAtom make_frame(YasAtom parent, YasAtom env, YasAtom tail)
{
    return yas_make_frame(parent, env, tail);
}

/* Control-frame field access (ticket #40 - see struct YasFrame, yas.h).
 * Named slots replace make_frame()'s former cons-list indices 0-5, so the
 * diff at every call site below is a mechanical rename:
 * yas_list_get(*stack, k) -> frame_get(*stack, FRAME_*),
 * yas_list_set(*stack, k, v) -> frame_set(*stack, FRAME_*, v). Slot 0
 * (parent) is only ever popped via yas_car(*stack) in the old code, never
 * read through yas_list_get, so it gets no FRAME_PARENT get-site rename -
 * only the pop sites change, to frame_get(*stack, FRAME_PARENT).
 *
 * frame_set() always goes through yas_gc_write_barrier() - a direct field
 * store (`f.value.frame->op = v;`) would silently bypass it, since only
 * yas_set_car/yas_set_cdr and the *_write_barrier() calls implement that
 * contract (see yas.h's comment on yas_set_car/yas_set_cdr and
 * docs/GC.md's "Write barrier"). Both functions take a compile-time
 * constant `slot` at every call site, so the switch folds to a direct
 * field access once inlined - no runtime dispatch cost. */
enum { FRAME_PARENT = 0, FRAME_ENV = 1, FRAME_OP = 2, FRAME_TAIL = 3, FRAME_ARGS = 4, FRAME_BODY = 5 };

static inline YasAtom frame_get(YasAtom f, int slot)
{
    struct YasFrame *fr = f.value.frame;
    switch (slot) {
        case FRAME_PARENT:
            return fr->parent;
        case FRAME_ENV:
            return fr->env;
        case FRAME_OP:
            return fr->op;
        case FRAME_TAIL:
            return fr->tail;
        case FRAME_ARGS:
            return fr->args;
        default:
            return fr->body;
    }
}

static inline void frame_set(YasAtom f, int slot, YasAtom v)
{
    struct YasFrame *fr = f.value.frame;
    switch (slot) {
        case FRAME_PARENT:
            fr->parent = v;
            break;
        case FRAME_ENV:
            fr->env = v;
            break;
        case FRAME_OP:
            fr->op = v;
            break;
        case FRAME_TAIL:
            fr->tail = v;
            break;
        case FRAME_ARGS:
            fr->args = v;
            break;
        default:
            fr->body = v;
            break;
    }
    yas_gc_write_barrier(f, v);
}

/* Deep-copy a control-stack frame chain (ticket #58). Each link becomes a
 * fresh struct YasFrame whose six fields are copied verbatim through
 * frame_get()/frame_set() - so every barriered-store and GC-scan contract
 * the live stack relies on applies to the copy automatically. The lists
 * held in tail/args/body are shared, not copied: the evaluator only ever
 * walks those spines or prepends fresh cells onto args (store_arg below),
 * never mutates existing pairs - apply-time's former in-place
 * yas_list_reverse() being the one exception, now a copying reverse for
 * exactly this reason. Environments are shared too, deliberately:
 * mutation of an environment is observed across continuation invocations
 * in standard Scheme.
 *
 * GC: the cursors are otherwise-unreachable locals kept live across
 * further gc_alloc() calls (allocation-triggered stepping may collect at
 * any yas_make_frame()), so they're rooted for the walk's duration -
 * docs/GC.md's "accumulator loop" hazard shape. Each fresh frame `f` is
 * unlinked only between its own yas_make_frame() and the frame_set()s
 * that follow; none of those allocate, so it needs no root of its own.
 * Returns yas_nil for an empty/NULL chain (the whole-top-level-form
 * capture case; see the CONTINUATION-apply branch in eval_do_apply()). */
static YasAtom clone_frame_chain(YasAtom src)
{
    YasAtom dst = yas_nil;  /* clone head - the topmost (newest) frame */
    YasAtom prev = yas_nil; /* most recently cloned frame */

    if (src.type != YAS_ATOM_TYPE_FRAME || !src.value.frame)
        return yas_nil;

    yas_gc_push_root(&dst);
    yas_gc_push_root(&prev);
    yas_gc_push_root(&src);

    /* Walk the source top-down (head -> parents) and link each clone as
     * the PREVIOUS clone's parent, mirroring the source's direction:
     * building the other way (parent = previous clone) would leave the
     * head's parent NIL and strand every deeper frame. The bottom-most
     * clone keeps a NIL parent, exactly like the source's bottom. */
    while (src.type == YAS_ATOM_TYPE_FRAME && src.value.frame) {
        YasAtom f = make_frame(yas_nil,
                               frame_get(src, FRAME_ENV),
                               frame_get(src, FRAME_TAIL));
        frame_set(f, FRAME_OP, frame_get(src, FRAME_OP));
        frame_set(f, FRAME_ARGS, frame_get(src, FRAME_ARGS));
        frame_set(f, FRAME_BODY, frame_get(src, FRAME_BODY));
        if (yas_nilp(prev))
            dst = f;
        else
            frame_set(prev, FRAME_PARENT, f);
        prev = f;
        src = frame_get(src, FRAME_PARENT);
    }

    yas_gc_pop_roots(3); /* src, prev, dst */
    return dst;
}

/* Bounded arity + spine-properness check for a special form's argument
 * list `args` (e.g. yas_cdr(expr)), replacing the O(len) yas_listp(expr)
 * that used to guard every call node up front (#63). Verifies the first
 * `min` cells are pairs (so `args` has at least `min` elements); when
 * `max < 0` that's all it checks - a special form whose tail is an
 * unbounded body (LAMBDA, DEFMACRO, TRY, INCLUDE) relies on a separate,
 * incremental check further down that spine instead (eval_do_exec() for
 * closure/macro bodies, the TRY/INCLUDE clause loops below) rather than
 * this walking the rest of it. When `max >= 0`, additionally verifies the
 * spine terminates in NIL at exactly `max` elements (so min==max means
 * "exactly N"). Either way this never walks past `max` (or `min`) cells,
 * so it stays O(1) for every fixed-arity form on the hot path. */
static int arity_ok(YasAtom args, int min, int max)
{
    YasAtom p = args;
    int count = 0;

    while (count < min) {
        if (p.type != YAS_ATOM_TYPE_PAIR)
            return 0;
        p = yas_cdr(p);
        count++;
    }
    if (max < 0)
        return 1;
    while (count < max) {
        if (yas_nilp(p))
            return 1;
        if (p.type != YAS_ATOM_TYPE_PAIR)
            return 0;
        p = yas_cdr(p);
        count++;
    }
    return yas_nilp(p);
}

int eval_do_exec(YasAtom *stack, YasAtom *expr, YasAtom *env)
{
    YasAtom body;

    *env = frame_get(*stack, FRAME_ENV);
    body = frame_get(*stack, FRAME_BODY);
    /* Walks one body form per call, across many calls as the closure's
     * body drains - this is where an improper body spine (formerly
     * rejected up front by make_closure()'s yas_listp(body), #63) now
     * gets caught, one cell at a time, instead of all at once on every
     * `lambda` evaluation. */
    if (body.type != YAS_ATOM_TYPE_PAIR)
        return YAS_ERROR_SYNTAX;
    *expr = yas_car(body);
    body = yas_cdr(body);
    if (yas_nilp(body)) {
        /* Finished function; pop the stack */
        *stack = frame_get(*stack, FRAME_PARENT);
    } else {
        frame_set(*stack, FRAME_BODY, body);
    }

    return YAS_ERROR_OK;
}

int eval_do_bind(YasAtom *stack, YasAtom *expr, YasAtom *env)
{
    YasAtom op, args, arg_names, body;

    body = frame_get(*stack, FRAME_BODY);
    if (!yas_nilp(body))
        return eval_do_exec(stack, expr, env);

    op = frame_get(*stack, FRAME_OP);
    args = frame_get(*stack, FRAME_ARGS);

    *env = yas_env_create(yas_car(op));
    arg_names = yas_car(yas_cdr(op));
    /* Structure: (env (args docstring . body)) - skip docstring */
    body = yas_cdr(yas_cdr(yas_cdr(op)));
    frame_set(*stack, FRAME_ENV, *env);
    frame_set(*stack, FRAME_BODY, body);

    /* Bind the arguments
     * Supports:
     * - Regular: x
     * - Optional with default: [x default]
     * - Keyword: :key [var default]
     * - Rest: . rest
     */
    int in_keywords = 0;
    /* Keyword matching splices matched :keyword/value pairs out of
     * `args`, but `args` is not always owned by this frame (#66): a
     * macro call's FRAME_ARGS is the source form's own argument spine
     * (aliased by FRAME_TAIL, and the #64 expansion-memo key), a
     * MACROEXPAND-1 call binds yas_cdr(form) of arbitrary evaluated
     * data, and APPLY binds the caller's raw argument list. Splicing
     * any of those corrupts user-visible structure. So the first time
     * a splice is about to fire, take one private copy (COW) and splice
     * only that from then on - calls that bind no keywords never pay
     * for it, and no caller has to know whether its list is safe to
     * mutate. */
    int args_copied = 0;
    while (!yas_nilp(arg_names)) {
        /* Rest argument (symbol at end) */
        if (arg_names.type == YAS_ATOM_TYPE_SYMBOL) {
            yas_env_define(*env, arg_names, args);
            args = yas_nil;
            break;
        }

        YasAtom arg_spec = yas_car(arg_names);

        /* `args` is proper-by-construction for an ordinary closure call
         * (eval_do_apply()'s yas_list_reverse() output), but for a macro
         * call it's the raw source argument spine (eval_do_return()'s
         * MACRO branch sets FRAME_ARGS straight from FRAME_TAIL, #63/#64)
         * - guard it here instead of relying on the up-front whole-spine
         * check this ticket removes. */
        if (!yas_nilp(args) && args.type != YAS_ATOM_TYPE_PAIR)
            return YAS_ERROR_SYNTAX;

        /* Keyword marker - next items are keyword args */
        if (arg_spec.type == YAS_ATOM_TYPE_KEYWORD) {
            in_keywords = 1;
            /* Find matching keyword in args and get its value */
            YasAtom kw = arg_spec;
            arg_names = yas_cdr(arg_names);
            if (yas_nilp(arg_names))
                return YAS_ERROR_SYNTAX; /* keyword without var spec */

            YasAtom var_spec = yas_car(arg_names);
            YasAtom var_name, default_val = yas_nil;

            /* Parse [name default] or just name */
            if (var_spec.type == YAS_ATOM_TYPE_SYMBOL) {
                var_name = var_spec;
            } else if (var_spec.type == YAS_ATOM_TYPE_PAIR) {
                var_name = yas_car(var_spec);
                if (!yas_nilp(yas_cdr(var_spec)))
                    default_val = yas_car(yas_cdr(var_spec));
            } else {
                return YAS_ERROR_SYNTAX;
            }

            /* Search for keyword in call args */
            /* First keyword spec to run against a non-empty args list?
             * Take one private copy (COW, #66): the search/splice below
             * removes matched :keyword/value pairs via yas_set_cdr(),
             * but `args` is not always owned by this frame - a macro
             * call's FRAME_ARGS is the source form's own argument spine
             * (aliased by FRAME_TAIL and used as the #64 expansion-memo
             * key), MACROEXPAND-1 binds yas_cdr(form) of arbitrary
             * evaluated data, and APPLY binds the caller's raw argument
             * list. Splicing any of those corrupts user-visible
             * structure. Calls that bind no keywords never pay for the
             * copy, and no caller has to know whether its list is safe
             * to mutate. */
            if (!args_copied && !yas_nilp(args)) {
                if (!yas_listp(args))
                    return YAS_ERROR_SYNTAX;
                /* Old `args` stays reachable through FRAME_ARGS during
                 * the copying allocations; the private copy itself is
                 * rooted across them, then anchored on the frame so
                 * later keyword iterations' allocating yas_env_define()
                 * calls can't collect it while only this C local holds
                 * it. */
                yas_gc_push_root(&args);
                args = yas_copy_list(args);
                yas_gc_pop_roots(1);
                frame_set(*stack, FRAME_ARGS, args);
                args_copied = 1;
            }
            YasAtom found_val = yas_nil;
            int found = 0;
            YasAtom search = args;
            YasAtom prev = yas_nil;
            while (search.type == YAS_ATOM_TYPE_PAIR) {
                if (yas_car(search).type == YAS_ATOM_TYPE_KEYWORD &&
                    yas_car(search).value.keyword == kw.value.keyword) {
                    /* Found keyword, next is value */
                    if (yas_nilp(yas_cdr(search)))
                        return YAS_ERROR_ARGS;
                    found_val = yas_car(yas_cdr(search));
                    found = 1;
                    /* Remove kw and value from args list. */
                    if (yas_nilp(prev)) {
                        args = yas_cdr(yas_cdr(search));
                    } else {
                        yas_set_cdr(prev, yas_cdr(yas_cdr(search)));
                    }
                    break;
                }
                prev = search;
                search = yas_cdr(search);
            }

            if (found) {
                yas_env_define(*env, var_name, found_val);
            } else {
                yas_env_define(*env, var_name, default_val);
            }

            arg_names = yas_cdr(arg_names);
            continue;
        }

        /* Optional argument with default: [name default] */
        if (arg_spec.type == YAS_ATOM_TYPE_PAIR && !in_keywords) {
            YasAtom var_name = yas_car(arg_spec);
            YasAtom default_val = yas_nil;
            if (!yas_nilp(yas_cdr(arg_spec)))
                default_val = yas_car(yas_cdr(arg_spec));

            if (yas_nilp(args) || yas_car(args).type == YAS_ATOM_TYPE_KEYWORD) {
                /* No more positional args, use default */
                yas_env_define(*env, var_name, default_val);
            } else {
                yas_env_define(*env, var_name, yas_car(args));
                args = yas_cdr(args);
            }
            arg_names = yas_cdr(arg_names);
            continue;
        }

        /* Regular positional argument */
        if (arg_spec.type == YAS_ATOM_TYPE_SYMBOL) {
            if (yas_nilp(args))
                return YAS_ERROR_ARGS;
            yas_env_define(*env, arg_spec, yas_car(args));
            args = yas_cdr(args);
            arg_names = yas_cdr(arg_names);
            continue;
        }

        return YAS_ERROR_SYNTAX;
    }

    /* Check for leftover positional args (keywords are allowed to remain) */
    while (!yas_nilp(args)) {
        if (args.type != YAS_ATOM_TYPE_PAIR)
            return YAS_ERROR_SYNTAX;
        if (yas_car(args).type != YAS_ATOM_TYPE_KEYWORD)
            return YAS_ERROR_ARGS;
        args = yas_cdr(args);
        if (!yas_nilp(args)) {
            if (args.type != YAS_ATOM_TYPE_PAIR)
                return YAS_ERROR_SYNTAX;
            args = yas_cdr(args); /* Skip value too */
        }
    }

    frame_set(*stack, FRAME_ARGS, yas_nil);

    return eval_do_exec(stack, expr, env);
}

int eval_do_apply(YasAtom *stack, YasAtom *expr, YasAtom *env, YasAtom *result)
{
    YasAtom op, args;

    op = frame_get(*stack, FRAME_OP);
    args = frame_get(*stack, FRAME_ARGS);

    if (!yas_nilp(args)) {
        /* Copy, then reverse the copy: yas_list_reverse() rewires cdrs
         * in place, which would scramble the shared arg spine of any
         * continuation snapshot whose frames were captured while this
         * application was mid-argument-evaluation (ticket #58; see
         * clone_frame_chain() for why frame-held spines must be
         * immutable once linked). */
        args = yas_copy_list(args);
        yas_list_reverse(&args);
        frame_set(*stack, FRAME_ARGS, args);
    }

    if (op.type == YAS_ATOM_TYPE_SYMBOL) {
        if (strcmp(op.value.symbol, "APPLY") == 0) {
            /* Replace the current frame */
            *stack = frame_get(*stack, FRAME_PARENT);
            *stack = make_frame(*stack, *env, yas_nil);
            op = yas_car(args);
            args = yas_car(yas_cdr(args));
            /* Unlike a source call form's argument spine (#63), this is
             * a genuine runtime value - the second argument to `apply`,
             * arbitrary user data that has never been validated as a
             * proper list. yas_listp() stays here; there's no later
             * per-cell consumption point to push this check down to,
             * and `apply` isn't the hot path #63 targets. */
            if (!yas_listp(args))
                return YAS_ERROR_SYNTAX;

            frame_set(*stack, FRAME_OP, op);
            frame_set(*stack, FRAME_ARGS, args);
        }
    }

    if (op.type == YAS_ATOM_TYPE_BUILTIN) {
        /* Check for arithmetic metamethods on hash arguments */
        if (!yas_nilp(args) && !yas_nilp(yas_cdr(args))) {
            YasAtom a = yas_car(args);
            YasAtom b = yas_car(yas_cdr(args));
            const char *mm_name = NULL;

            /* Determine which metamethod to check based on builtin */
            if (op.value.builtin == yas_builtin_add)
                mm_name = "add";
            else if (op.value.builtin == yas_builtin_subtract)
                mm_name = "sub";
            else if (op.value.builtin == yas_builtin_multiply)
                mm_name = "mul";
            else if (op.value.builtin == yas_builtin_divide)
                mm_name = "div";
            else if (op.value.builtin == yas_builtin_numeq)
                mm_name = "eq";
            else if (op.value.builtin == yas_builtin_less)
                mm_name = "lt";

            if (mm_name) {
                YasAtom hash_arg = yas_nil;
                YasAtom other_arg = yas_nil;

                /* Check first operand, then second for metamethod */
                if (a.type == YAS_ATOM_TYPE_HASH && yas_has_metamethod(a, mm_name)) {
                    hash_arg = a;
                    other_arg = b;
                } else if (b.type == YAS_ATOM_TYPE_HASH && yas_has_metamethod(b, mm_name)) {
                    hash_arg = b;
                    other_arg = a;
                }

                if (!yas_nilp(hash_arg)) {
                    /* Redirect to metamethod: (metamethod self other) */
                    YasAtom mm_fn = yas_get_metamethod(hash_arg, mm_name);
                    args = yas_list_create(2, hash_arg, other_arg);
                    frame_set(*stack, FRAME_OP, mm_fn);
                    frame_set(*stack, FRAME_ARGS, args);

                    if (mm_fn.type == YAS_ATOM_TYPE_BUILTIN) {
                        *stack = frame_get(*stack, FRAME_PARENT);
                        *expr = yas_cons(mm_fn, args);
                        return YAS_ERROR_OK;
                    } else if (mm_fn.type == YAS_ATOM_TYPE_CLOSURE) {
                        return eval_do_bind(stack, expr, env);
                    } else {
                        return YAS_ERROR_TYPE;
                    }
                }
            }
        }

        /* Check for ~get on hash-ref or ~set on hash-set! */
        if (op.value.builtin == yas_builtin_hash_ref && !yas_nilp(args)) {
            YasAtom hash = yas_car(args);
            if (hash.type == YAS_ATOM_TYPE_HASH && yas_has_metamethod(hash, "get")) {
                YasAtom key = !yas_nilp(yas_cdr(args)) ? yas_car(yas_cdr(args)) : yas_nil;
                /* Check if key exists in hash first - only call ~get if missing */
                YasAtom existing = yas_hash_ref(hash, key, yas_nil);
                if (yas_nilp(existing)) {
                    YasAtom mm_fn = yas_get_metamethod(hash, "get");
                    args = yas_list_create(2, hash, key);
                    frame_set(*stack, FRAME_OP, mm_fn);
                    frame_set(*stack, FRAME_ARGS, args);

                    if (mm_fn.type == YAS_ATOM_TYPE_CLOSURE) {
                        return eval_do_bind(stack, expr, env);
                    } else if (mm_fn.type == YAS_ATOM_TYPE_BUILTIN) {
                        *stack = frame_get(*stack, FRAME_PARENT);
                        *expr = yas_cons(mm_fn, args);
                        return YAS_ERROR_OK;
                    }
                }
            }
        }

        if (op.value.builtin == yas_builtin_hash_set && !yas_nilp(args)) {
            YasAtom hash = yas_car(args);
            if (hash.type == YAS_ATOM_TYPE_HASH && yas_has_metamethod(hash, "set")) {
                YasAtom key = !yas_nilp(yas_cdr(args)) ? yas_car(yas_cdr(args)) : yas_nil;
                YasAtom val = !yas_nilp(yas_cdr(yas_cdr(args))) ? yas_car(yas_cdr(yas_cdr(args))) : yas_nil;
                YasAtom mm_fn = yas_get_metamethod(hash, "set");
                args = yas_list_create(3, hash, key, val);
                frame_set(*stack, FRAME_OP, mm_fn);
                frame_set(*stack, FRAME_ARGS, args);

                if (mm_fn.type == YAS_ATOM_TYPE_CLOSURE) {
                    return eval_do_bind(stack, expr, env);
                } else if (mm_fn.type == YAS_ATOM_TYPE_BUILTIN) {
                    *stack = frame_get(*stack, FRAME_PARENT);
                    *expr = yas_cons(mm_fn, args);
                    return YAS_ERROR_OK;
                }
            }
        }

        *stack = frame_get(*stack, FRAME_PARENT);
        *expr = yas_cons(op, args);
        return YAS_ERROR_OK;
    } else if (op.type == YAS_ATOM_TYPE_CONTINUATION) {
        /* Invoke continuation: replace stack with a fresh clone of the
         * captured chain, return value */
        if (yas_nilp(args) || !yas_nilp(yas_cdr(args)))
            return YAS_ERROR_ARGS;

        /* Restore a per-invocation clone of the captured stack (ticket
         * #58): the continuation holds a private capture-time snapshot,
         * and each invocation gets its own copy, so a resumed run
         * advancing frame positions in place can never perturb any
         * other invocation - or the snapshot itself - making repeated
         * invocation genuinely re-entrant. The clone's spines/env are
         * shared with the snapshot (see clone_frame_chain()).
         *
         * A captured continuation's frame pointer is NULL when call/cc
         * was invoked with nothing left on the stack below it (e.g.
         * `(call/cc (lambda (k) (k 1)))` as a whole top-level form).
         * Restoring to an explicit yas_nil in that case (instead of a
         * FRAME-tagged atom wrapping NULL) is what makes the
         * yas_nilp(*stack) check below actually fire. */
        if (!op.value.frame) {
            *stack = yas_nil;
        } else {
            YasAtom saved;
            saved.type = YAS_ATOM_TYPE_FRAME;
            saved.value.frame = op.value.frame;
            *stack = clone_frame_chain(saved);
        }
        *result = yas_car(args);

        if (yas_nilp(*stack))
            return YAS_ERROR_OK; /* Will exit main loop */
        return eval_do_return(stack, expr, env, result);
    } else if (op.type == YAS_ATOM_TYPE_HASH && yas_has_metamethod(op, "call")) {
        /* Hash has ~call metamethod - invoke it with (self . args) */
        YasAtom call_fn = yas_get_metamethod(op, "call");
        args = yas_cons(op, args); /* Prepend hash as first arg (self) */
        frame_set(*stack, FRAME_OP, call_fn);
        frame_set(*stack, FRAME_ARGS, args);

        /* Handle based on metamethod type */
        if (call_fn.type == YAS_ATOM_TYPE_BUILTIN) {
            *stack = frame_get(*stack, FRAME_PARENT);
            *expr = yas_cons(call_fn, args);
            return YAS_ERROR_OK;
        } else if (call_fn.type == YAS_ATOM_TYPE_CLOSURE) {
            return eval_do_bind(stack, expr, env);
        } else {
            return YAS_ERROR_TYPE;
        }
    } else if (op.type == YAS_ATOM_TYPE_HASH && yas_has_metamethod(op, "missing")) {
        /* Hash has ~missing metamethod - for Ruby-style method dispatch */
        /* args = (method-name arg1 arg2 ...)
           Call: (~missing self args)
           which is (~missing self (method-name arg1 arg2 ...)) */
        YasAtom missing_fn = yas_get_metamethod(op, "missing");
        YasAtom new_args = yas_list_create(2, op, args);
        frame_set(*stack, FRAME_OP, missing_fn);
        frame_set(*stack, FRAME_ARGS, new_args);

        if (missing_fn.type == YAS_ATOM_TYPE_BUILTIN) {
            *stack = frame_get(*stack, FRAME_PARENT);
            *expr = yas_cons(missing_fn, new_args);
            return YAS_ERROR_OK;
        } else if (missing_fn.type == YAS_ATOM_TYPE_CLOSURE) {
            return eval_do_bind(stack, expr, env);
        } else {
            return YAS_ERROR_TYPE;
        }
    } else if (op.type != YAS_ATOM_TYPE_CLOSURE) {
        return YAS_ERROR_TYPE;
    }

    return eval_do_bind(stack, expr, env);
}

int eval_do_return(YasAtom *stack, YasAtom *expr, YasAtom *env, YasAtom *result)
{
    YasAtom op, args, body;

    *env = frame_get(*stack, FRAME_ENV);
    op = frame_get(*stack, FRAME_OP);
    body = frame_get(*stack, FRAME_BODY);

    if (!yas_nilp(body)) {
        /* Still running a procedure; ignore the result */
        return eval_do_apply(stack, expr, env, result);
    }

    if (yas_nilp(op)) {
        /* Finished evaluating operator */
        op = *result;
        frame_set(*stack, FRAME_OP, op);

        if (op.type == YAS_ATOM_TYPE_MACRO) {
            /* Don't evaluate macro arguments */
            args = frame_get(*stack, FRAME_TAIL);

            /* #64: this call site may already have an expansion cached
             * from a previous evaluation - `args` (yas_cdr(call-form))
             * is a stable identity per call site, so a hit here skips
             * running the macro's closure entirely. See
             * yas_macro_memo_get()/put() (data.c) and
             * docs/ARCHITECTURE.md's "Evaluator" section. A nil `args`
             * (a zero-argument macro call) has no per-call-site identity
             * to key on and always misses. */
            YasAtom cached;
            if (!yas_nilp(args) && yas_macro_memo_get(args, op, &cached)) {
                *expr = cached;
                *stack = frame_get(*stack, FRAME_PARENT);
                return YAS_ERROR_OK;
            }

            *stack = make_frame(*stack, *env, yas_nil);
            op.type = YAS_ATOM_TYPE_CLOSURE;
            frame_set(*stack, FRAME_OP, op);
            frame_set(*stack, FRAME_ARGS, args);
            return eval_do_bind(stack, expr, env);
        }
    } else if (op.type == YAS_ATOM_TYPE_SYMBOL) {
        /* Finished working on special form */
        if (strcmp(op.value.symbol, "DEFINE") == 0) {
            YasAtom sym = frame_get(*stack, FRAME_ARGS);
            (void)yas_env_define(*env, sym, *result);
            *stack = frame_get(*stack, FRAME_PARENT);
            *expr = yas_cons(yas_make_sym("QUOTE"), yas_cons(sym, yas_nil));
            return YAS_ERROR_OK;
        } else if (strcmp(op.value.symbol, "SET!") == 0) {
            YasAtom sym = frame_get(*stack, FRAME_ARGS);
            *stack = frame_get(*stack, FRAME_PARENT);
            *expr = yas_cons(yas_make_sym("QUOTE"), yas_cons(sym, yas_nil));
            return yas_env_set(*env, sym, *result);
        } else if (strcmp(op.value.symbol, "IF") == 0) {
            args = frame_get(*stack, FRAME_TAIL);
            *expr = yas_nilp(*result) ? yas_car(yas_cdr(args)) : yas_car(args);
            *stack = frame_get(*stack, FRAME_PARENT);
            return YAS_ERROR_OK;
        } else if (strcmp(op.value.symbol, "MACROEXPAND-1") == 0) {
            /* result contains the evaluated form */
            YasAtom form = *result;
            YasAtom original_env = frame_get(*stack, FRAME_ENV);
            *stack = frame_get(*stack, FRAME_PARENT);

            /* If form is (sym ...) where sym is bound to a macro, expand */
            if (form.type == YAS_ATOM_TYPE_PAIR && yas_car(form).type == YAS_ATOM_TYPE_SYMBOL) {
                YasAtom macro_op;
                if (yas_env_get(original_env, yas_car(form), &macro_op) == YAS_ERROR_OK &&
                    macro_op.type == YAS_ATOM_TYPE_MACRO) {
                    /* Apply macro: bind args, eval body */
                    *stack = make_frame(*stack, original_env, yas_nil);
                    macro_op.type = YAS_ATOM_TYPE_CLOSURE;
                    frame_set(*stack, FRAME_OP, macro_op);
                    frame_set(*stack, FRAME_ARGS, yas_cdr(form));
                    return eval_do_bind(stack, expr, env);
                }
            }
            /* Not a macro call, return unchanged */
            *expr = yas_cons(yas_make_sym("QUOTE"), yas_cons(form, yas_nil));
            return YAS_ERROR_OK;
        } else if (strcmp(op.value.symbol, "CALL/CC") == 0) {
            /* result is the function to call with continuation */
            YasAtom fn = *result;
            YasAtom cont;

            /* Pop CALL/CC frame and capture the remaining stack as the
             * continuation - a *snapshot*: clone_frame_chain() copies
             * every frame, so positions frozen here can't be perturbed
             * by the ordinary (non-continuation) execution path going on
             * to advance or pop the live frames after this point (ticket
             * #58). NULL when nothing is left on the stack (see the
             * CONTINUATION-apply case in eval_do_apply() for how that's
             * handled on invoke). */
            *stack = frame_get(*stack, FRAME_PARENT);
            cont.type = YAS_ATOM_TYPE_CONTINUATION;
            if (yas_nilp(*stack))
                cont.value.frame = NULL;
            else
                cont.value.frame = clone_frame_chain(*stack).value.frame;

            /* Apply fn to the continuation. `cont` is otherwise
             * unreachable across the allocations below - unlike the
             * pre-#58 alias it shares no structure with the rooted live
             * stack - so root it until yas_cons() links it into the new
             * frame's args. */
            yas_gc_push_root(&cont);
            *stack = make_frame(*stack, *env, yas_nil);
            frame_set(*stack, FRAME_OP, fn);
            frame_set(*stack, FRAME_ARGS, yas_cons(cont, yas_nil));
            yas_gc_pop_roots(1); /* cont */
            return eval_do_bind(stack, expr, env);
        } else {
            goto store_arg;
        }
    } else if (op.type == YAS_ATOM_TYPE_MACRO) {
        /* Finished evaluating macro - `*stack` is still the call-site
         * frame (the one pushed at `push:` whose FRAME_OP matched
         * YAS_ATOM_TYPE_MACRO above): the child frame pushed for the
         * expansion carries the macro retagged to CLOSURE and is popped
         * by eval_do_exec() when its body drains, restoring `*stack` to
         * this frame before eval_do_return() is called again. FRAME_TAIL
         * on this frame is therefore the exact same cell used as the
         * memo key on the lookup side above - it's only ever reassigned
         * by the store_arg path below, which this branch never takes.
         * #64: record the expansion for future evaluations of this call
         * site before discarding it. */
        YasAtom call_args = frame_get(*stack, FRAME_TAIL);
        if (!yas_nilp(call_args))
            yas_macro_memo_put(call_args, op, *result);
        *expr = *result;
        *stack = frame_get(*stack, FRAME_PARENT);
        return YAS_ERROR_OK;
    } else {
    store_arg:
        /* Store evaluated argument */
        args = frame_get(*stack, FRAME_ARGS);
        frame_set(*stack, FRAME_ARGS, yas_cons(*result, args));
    }

    args = frame_get(*stack, FRAME_TAIL);
    if (yas_nilp(args)) {
        /* No more arguments left to evaluate */
        return eval_do_apply(stack, expr, env, result);
    }
    /* `args` is the source call form's own argument spine (FRAME_TAIL was
     * seeded straight from yas_cdr(expr) at push-time), walked one cell
     * per evaluated argument - this is exactly the invariant the old
     * up-front yas_listp(expr) check (#63) used to guarantee. Catch an
     * improper spine here instead, at the point it's actually consumed. */
    if (args.type != YAS_ATOM_TYPE_PAIR)
        return YAS_ERROR_SYNTAX;

    /* Evaluate next argument */
    *expr = yas_car(args);
    frame_set(*stack, FRAME_TAIL, yas_cdr(args));
    return YAS_ERROR_OK;
}

static int yas_eval_expr_impl(YasAtom expr, YasAtom env, YasAtom *result)
{
    struct EvalRoot root;
    root.prev = eval_roots;
    eval_roots = &root;

    /* Balance guard for the yas_gc_push_root()/yas_gc_pop_roots() shadow
     * stack: every re-entering caller in this file and module.c pushes
     * and pops in matched pairs, and there is no setjmp/longjmp in the
     * tree (call/cc is implemented on the VM's own explicit stack), so
     * every unwind is a plain C return - balance is always achievable.
     * Catches a future unbalanced push here instead of a slow root-stack
     * leak. */
    int root_count_before = yas_gc_root_count();

    int err = eval_vm(expr, env, result, &root);

    eval_roots = root.prev;
    assert(yas_gc_root_count() == root_count_before);
    return err;
}

int yas_eval_expr(YasAtom expr, YasAtom env, YasAtom *result)
{
    int r = yas_eval_expr_impl(expr, env, result);
    return r;
}


static int eval_vm(YasAtom expr, YasAtom env, YasAtom *result, struct EvalRoot *root)
{
    YasError err = YAS_ERROR_OK;
    YasAtom stack = yas_nil;

    /* Caller-supplied `result` storage is uninitialized (main.c's
     * `YasAtom result;`, this file's `YasAtom ignore;`/`ignored;`, etc.)
     * - nil it before rooting so the very first allocation below (which
     * can now step the GC directly - see docs/GC.md, "Allocation-
     * triggered stepping") doesn't mark garbage. */
    *result = yas_nil;

    root->expr = &expr;
    root->env = &env;
    root->stack = &stack;
    root->result = result;

    do {
        /* No per-iteration yas_gc_step() call here anymore: gc_alloc()
         * itself now paces steps (a byte-proportional debt counter, see
         * data.c), so allocation is what advances the collector rather
         * than the evaluator's loop. `expr`/`env`/`stack`/`result` stay
         * rooted via `root` (EvalRoot) regardless of where in this loop
         * a step happens to land. */

        if (expr.type == YAS_ATOM_TYPE_SYMBOL) {
            err = yas_env_get(env, expr, result);
            if (err == YAS_ERROR_UNBOUND) {
                /* Store the symbol name in exception for better error reporting */
                extern YasAtom yas_ctx_exception;
                char msg[256];
                snprintf(msg, sizeof(msg), "Symbol not bound: %s", expr.value.symbol);
                YasAtom t = yas_make_sym("type-error");
                YasAtom m = yas_make_string(msg);
                yas_ctx_exception = yas_cons(t, yas_cons(m, yas_nil));
            }
        } else if (expr.type != YAS_ATOM_TYPE_PAIR) {
            *result = expr;
        } else {
            /* No more up-front yas_listp(expr) here (#63): that used to
             * walk the whole spine to prove it terminates in NIL before
             * anything else happened, on every call/special-form node.
             * Properness is now checked incrementally, at each site that
             * actually consumes another cell - arity_ok() below for
             * special forms, eval_do_return()'s TAIL advance for
             * ordinary call arguments, eval_do_exec()/eval_do_bind() for
             * bodies. Consequence: an improper form is now diagnosed
             * when the offending cell is reached rather than up front,
             * so e.g. `(f a . b)` may evaluate `a` (side effects
             * included) before erroring, and the resulting error code
             * may be ARGS/TYPE rather than always SYNTAX - see
             * docs/ARCHITECTURE.md's "Evaluator" section. */
            YasAtom op = yas_car(expr);
            YasAtom args = yas_cdr(expr);

            if (op.type == YAS_ATOM_TYPE_SYMBOL) {
                /* Handle special forms */

                if (strcmp(op.value.symbol, "QUOTE") == 0) {
                    if (!arity_ok(args, 1, 1))
                        return YAS_ERROR_ARGS;

                    *result = yas_car(args);
                } else if (strcmp(op.value.symbol, "DEFINE") == 0) {
                    YasAtom sym;

                    if (!arity_ok(args, 2, -1))
                        return YAS_ERROR_ARGS;

                    sym = yas_car(args);
                    if (sym.type == YAS_ATOM_TYPE_PAIR) {
                        err = make_closure(env, yas_cdr(sym), yas_cdr(args), result);
                        sym = yas_car(sym);
                        if (sym.type != YAS_ATOM_TYPE_SYMBOL)
                            return YAS_ERROR_TYPE;
                        (void)yas_env_define(env, sym, *result);
                        *result = sym;
                    } else if (sym.type == YAS_ATOM_TYPE_SYMBOL) {
                        if (!yas_nilp(yas_cdr(yas_cdr(args))))
                            return YAS_ERROR_ARGS;
                        stack = make_frame(stack, env, yas_nil);
                        frame_set(stack, FRAME_OP, op);
                        frame_set(stack, FRAME_ARGS, sym);
                        expr = yas_car(yas_cdr(args));
                        continue;
                    } else {
                        return YAS_ERROR_TYPE;
                    }
                } else if (strcmp(op.value.symbol, "LAMBDA") == 0) {
                    if (!arity_ok(args, 2, -1))
                        return YAS_ERROR_ARGS;

                    err = make_closure(env, yas_car(args), yas_cdr(args), result);
                } else if (strcmp(op.value.symbol, "IF") == 0) {
                    if (!arity_ok(args, 3, 3))
                        return YAS_ERROR_ARGS;

                    stack = make_frame(stack, env, yas_cdr(args));
                    frame_set(stack, FRAME_OP, op);
                    expr = yas_car(args);
                    continue;
                } else if (strcmp(op.value.symbol, "TRY") == 0) {
                    if (!arity_ok(args, 1, -1))
                        return YAS_ERROR_ARGS;

                    YasAtom try_expr = yas_car(args);
                    YasAtom rest = yas_cdr(args);

                    /* Evaluate try block recursively to catch error */
                    /* We recurse here because TRY establishes a dynamic handler boundary */
                    err = yas_eval_expr(try_expr, env, result);

                    if (err != YAS_ERROR_OK) {
                        /* Exception thrown or runtime error.
                         *
                         * Dispatch model (#65/#69): detach the pending
                         * exception from the global for the whole clause
                         * walk. Clauses bind from the local `pending`
                         * snapshot and the global stays nil throughout, so
                         * anything raised while evaluating a predicate or a
                         * catch body installs itself unambiguously as the
                         * global's new content - it can neither be confused
                         * with the value being dispatched nor leave that
                         * value stale behind a code-only failure (#69's
                         * pred_err corner). The snapshot goes back to the
                         * global only if this TRY finishes without any
                         * clause handling the error: uncaught errors must
                         * keep carrying their object outward, while every
                         * superseding or malformed delivery abandons it. */
                        YasAtom pending = yas_ctx_exception;
                        yas_clear_exception();
                        /* Rooted before anything below can allocate: once
                         * detached above, `pending` is referenced only by
                         * this slot until a clause's catch_env takes over
                         * (the enclosing TRY form's own spine keeps the
                         * clause cells alive via expr, but nothing else
                         * references the exception value itself). */
                        yas_gc_push_root(&pending);

                        if (yas_nilp(pending)) {
                            /* Code-only failure (SYNTAX/ARGS/TYPE/UNBOUND
                             * with no explicit object installed by the
                             * raiser): convert the code to a message string
                             * for the clauses to bind. */
                            const char *msg = "Unknown Error";
                            if (err == YAS_ERROR_UNBOUND)
                                msg = "Symbol not bound";
                            else if (err == YAS_ERROR_ARGS)
                                msg = "Argument error";
                            else if (err == YAS_ERROR_TYPE)
                                msg = "Type error";
                            else if (err == YAS_ERROR_SYNTAX)
                                msg = "Syntax error";

                            pending = yas_make_string(msg);
                        }
                        /* Exception thrown */
                        int caught = 0;
                        YasAtom p = rest;
                        /* `p` walks the same top-level spine `expr`'s
                         * yas_listp() check used to guarantee proper
                         * (#63) - check the cell type explicitly instead
                         * of trusting yas_nilp() alone to signal the
                         * end. */
                        while (p.type == YAS_ATOM_TYPE_PAIR) {
                            YasAtom clause = yas_car(p);
                            if (clause.type == YAS_ATOM_TYPE_PAIR && yas_car(clause).type == YAS_ATOM_TYPE_SYMBOL &&
                                strcmp(yas_car(clause).value.symbol, "CATCH") == 0) {

                                /* (CATCH var_decl . body) */
                                YasAtom catch_args = yas_cdr(clause);
                                if (yas_nilp(catch_args)) {
                                    /* Malformed clause: delivery of the
                                     * dispatched value failed structurally,
                                     * so abandon it (#65) - the SYNTAX error
                                     * leaving this TRY describes itself via
                                     * an outer synthesis instead of carrying
                                     * the abandoned value. */
                                    pending = yas_nil;
                                    err = YAS_ERROR_SYNTAX;
                                    break;
                                }

                                YasAtom var_decl = yas_car(catch_args);
                                YasAtom catch_body = yas_cdr(catch_args);

                                YasAtom var = yas_nil;
                                YasAtom pred = yas_nil;

                                /* Parse var declaration: sym or (sym pred) */
                                if (var_decl.type == YAS_ATOM_TYPE_SYMBOL) {
                                    var = var_decl;
                                } else if (var_decl.type == YAS_ATOM_TYPE_PAIR) {
                                    if (yas_nilp(var_decl) || yas_nilp(yas_cdr(var_decl))) {
                                        /* Same delivery-abandoned case as
                                         * the catch_args check above. */
                                        pending = yas_nil;
                                        err = YAS_ERROR_SYNTAX;
                                        break;
                                    }
                                    var = yas_car(var_decl);
                                    pred = yas_car(yas_cdr(var_decl));
                                    if (var.type != YAS_ATOM_TYPE_SYMBOL) {
                                        pending = yas_nil;
                                        err = YAS_ERROR_TYPE;
                                        break;
                                    } /* Var must be symbol */
                                } else {
                                    pending = yas_nil;
                                    err = YAS_ERROR_SYNTAX;
                                    break;
                                }

                                /* Bind the dispatched value for predicate
                                 * check and body. catch_env is a fresh
                                 * yas_env_create(env) that nothing else
                                 * references (env's own binding list doesn't
                                 * point back at its children), so it must
                                 * stay rooted for as long as it's used
                                 * below - through the predicate eval and the
                                 * catch body loop, both of which can trigger
                                 * a nested GC step. Popped on every exit from
                                 * this clause: the predicate-error break, the
                                 * matched-and-executed break, and the
                                 * not-matched fall-through to the next
                                 * clause. */
                                YasAtom catch_env = yas_env_create(env);
                                yas_env_define(catch_env, var, pending);
                                yas_gc_push_root(&catch_env);

                                int match = 1;
                                if (!yas_nilp(pred)) {
                                    /* Check predicate: (pred var) */
                                    YasAtom pred_expr = yas_cons(pred, yas_cons(var, yas_nil));
                                    YasAtom pred_res;
                                    YasError pred_err = yas_eval_expr(pred_expr, catch_env, &pred_res);

                                    if (pred_err) {
                                        /* Error evaluating predicate?
                                         * Propagate it - and abandon the
                                         * dispatched value (#69): whatever
                                         * went wrong inside the pred has
                                         * already installed its own exception
                                         * object if it raised explicitly (a
                                         * throw, an unbound symbol), and left
                                         * the global nil if it failed
                                         * code-only, so the outward error
                                         * always describes the pred's own
                                         * failure rather than dragging the
                                         * abandoned value along. */
                                        pending = yas_nil;
                                        err = pred_err;
                                        yas_gc_pop_roots(1); /* catch_env */
                                        break;
                                    }

                                    if (yas_nilp(pred_res))
                                        match = 0;
                                }

                                if (match) {
                                    /* Execute body. last_res carries the
                                     * previous body form's value across the
                                     * next form's yas_eval_expr() - unrooted
                                     * otherwise, since it's a plain C local
                                     * distinct from eval_vm's own *result
                                     * (only copied into *result after the
                                     * loop). */
                                    YasAtom last_res = yas_nil;
                                    yas_gc_push_root(&last_res);
                                    while (!yas_nilp(catch_body)) {
                                        err = yas_eval_expr(yas_car(catch_body), catch_env, &last_res);
                                        if (err)
                                            break; /* Exception inside catch? */
                                        catch_body = yas_cdr(catch_body);
                                    }
                                    *result = last_res;
                                    yas_gc_pop_roots(2); /* last_res, catch_env */
                                    caught = 1;
                                    break;
                                } else {
                                    /* Predicate declined - keep `pending`
                                     * intact for the next clause, re-arming
                                     * the dispatched error code. */
                                    err = YAS_ERROR_EXCEPTION;
                                    yas_gc_pop_roots(1); /* catch_env */
                                }
                            }
                            p = yas_cdr(p);
                        }

                        /* Unhandled passthrough: put the dispatched value
                         * back so the error leaves this TRY carrying the
                         * same object it arrived with (nested-TRY
                         * passthrough). Handled-or-superseded exits nil'd
                         * `pending` above - the guard keeps their hands off
                         * the global, which by then holds either the
                         * superseding raise's own fresh object or nil (for
                         * an outward code-only synthesis). All paths
                         * through the clause walk converge on this single
                         * pop. */
                        if (!caught && !yas_nilp(pending))
                            yas_ctx_exception = pending;
                        yas_gc_pop_roots(1); /* pending */

                        /* If not caught, err remains YAS_ERROR_EXCEPTION (or Syntax error from loop break) */
                    }

                    /* Save error state (either original exception if not caught, or new error from catch, or OK) */
                    YasError saved_err = err;

                    /* Execute FINALLY */
                    YasAtom p = rest;
                    while (p.type == YAS_ATOM_TYPE_PAIR) {
                        YasAtom clause = yas_car(p);
                        if (clause.type == YAS_ATOM_TYPE_PAIR && yas_car(clause).type == YAS_ATOM_TYPE_SYMBOL &&
                            strcmp(yas_car(clause).value.symbol, "FINALLY") == 0) {

                            /* (FINALLY . body) */
                            YasAtom final_body = yas_cdr(clause);
                            YasAtom ignored;
                            while (!yas_nilp(final_body)) {
                                YasError final_err = yas_eval_expr(yas_car(final_body), env, &ignored);
                                if (final_err) {
                                    /* Exception in finally overrides previous */
                                    saved_err = final_err;
                                    break;
                                }
                                final_body = yas_cdr(final_body);
                            }
                            break;
                        }
                        p = yas_cdr(p);
                    }

                    err = saved_err;

                    /* Result is set if err==OK. */
                    /* If err!=OK, we will break/return error. */
                    if (err)
                        break;
                } else if (strcmp(op.value.symbol, "DEFMACRO") == 0) {
                    YasAtom name, macro;

                    if (!arity_ok(args, 2, -1))
                        return YAS_ERROR_ARGS;

                    if (yas_car(args).type != YAS_ATOM_TYPE_PAIR)
                        return YAS_ERROR_SYNTAX;

                    name = yas_car(yas_car(args));
                    if (name.type != YAS_ATOM_TYPE_SYMBOL)
                        return YAS_ERROR_TYPE;

                    err = make_closure(env, yas_cdr(yas_car(args)), yas_cdr(args), &macro);
                    if (!err) {
                        macro.type = YAS_ATOM_TYPE_MACRO;
                        *result = name;
                        (void)yas_env_define(env, name, macro);
                    }
                } else if (strcmp(op.value.symbol, "APPLY") == 0) {
                    if (!arity_ok(args, 2, 2))
                        return YAS_ERROR_ARGS;

                    stack = make_frame(stack, env, yas_cdr(args));
                    frame_set(stack, FRAME_OP, op);
                    expr = yas_car(args);
                    continue;
                } else if (strcmp(op.value.symbol, "SET!") == 0) {
                    if (!arity_ok(args, 2, 2))
                        return YAS_ERROR_ARGS;
                    if (yas_car(args).type != YAS_ATOM_TYPE_SYMBOL)
                        return YAS_ERROR_TYPE;
                    stack = make_frame(stack, env, yas_nil);
                    frame_set(stack, FRAME_OP, op);
                    frame_set(stack, FRAME_ARGS, yas_car(args));
                    expr = yas_car(yas_cdr(args));
                    continue;
                } else if (strcmp(op.value.symbol, "MACROEXPAND-1") == 0) {
                    if (!arity_ok(args, 1, 1))
                        return YAS_ERROR_ARGS;
                    /* Evaluate the argument to get the form */
                    stack = make_frame(stack, env, yas_nil);
                    frame_set(stack, FRAME_OP, op);
                    expr = yas_car(args);
                    continue;
                } else if (strcmp(op.value.symbol, "CALL/CC") == 0) {
                    if (!arity_ok(args, 1, 1))
                        return YAS_ERROR_ARGS;
                    /* Evaluate the function argument */
                    stack = make_frame(stack, env, yas_nil);
                    frame_set(stack, FRAME_OP, op);
                    expr = yas_car(args);
                    continue;
                } else if (strcmp(op.value.symbol, "INCLUDE") == 0 || strcmp(op.value.symbol, "INCLUDE-CI") == 0) {
                    /* (include "file.ys" ...) - textual file inclusion */
                    if (!arity_ok(args, 1, -1))
                        return YAS_ERROR_ARGS;

                    int case_insensitive = (strcmp(op.value.symbol, "INCLUDE-CI") == 0);

                    /* Process each file argument */
                    YasAtom file_list = args;
                    while (file_list.type == YAS_ATOM_TYPE_PAIR) {
                        YasAtom file_arg = yas_car(file_list);
                        if (file_arg.type != YAS_ATOM_TYPE_STRING)
                            return YAS_ERROR_TYPE;

                        err = yas_eval_file(file_arg.value.string, env, result, case_insensitive);
                        if (err)
                            return err;

                        file_list = yas_cdr(file_list);
                    }
                    if (!yas_nilp(file_list))
                        return YAS_ERROR_SYNTAX;
                    /* result is set to the last expression evaluated */
                } else if (strcmp(op.value.symbol, "IMPORT") == 0) {
                    err = yas_module_import(args, env, result);
                    if (err)
                        return err;
                } else {
                    goto push;
                }
            } else if (op.type == YAS_ATOM_TYPE_BUILTIN) {
                /* `args` is proper by construction on the ordinary path
                 * (eval_do_apply()'s yas_cons(builtin, args)), which is
                 * why builtin_*.c walks it unchecked - but a hand-built
                 * form can reach here directly, e.g. `(eval (cons car
                 * 1))` gives a BUILTIN car and a non-pair cdr. An O(1)
                 * nil-or-pair check catches that immediate case without
                 * reinstating a yas_listp() walk on every builtin call
                 * (#63); a non-pair cell further down a hand-built args
                 * spine - `(eval (cons car (cons 1 2)))` - stays
                 * undetected and is undefined behaviour, documented in
                 * docs/ARCHITECTURE.md. */
                if (!yas_nilp(args) && args.type != YAS_ATOM_TYPE_PAIR)
                    return YAS_ERROR_SYNTAX;
                err = (*op.value.builtin)(args, result);
            } else {
            push:
                /* Handle function application */
                stack = make_frame(stack, env, args);
                expr = op;
                continue;
            }
        }

        if (yas_nilp(stack))
            break;

        if (!err)
            err = eval_do_return(&stack, &expr, &env, result);
    } while (!err);

    return err;
}


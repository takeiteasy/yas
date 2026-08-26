#include "context.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ══════════════════════════════════════════════════════════════════
   Context lifecycle (ticket #47 milestone 2). See context.h for the
   ownership rules and yas.h for the public contract.
   ══════════════════════════════════════════════════════════════════ */

YAS_TLS YasContext *yas_ctx = NULL;

YasContext *yas_context_current(void)
{
    return yas_ctx;
}

void yas_context_bind(YasContext *ctx)
{
    yas_ctx = ctx;
}

/* Accessor slots behind the backwards-compatible macros in yas.h. */
YasAtom *yas_context_exception(void)
{
    return &yas_ctx->exception;
}

const char **yas_context_current_file_path_slot(void)
{
    return &yas_ctx->current_file_path;
}

#ifdef YAS_SRCPOS
int *yas_context_error_line_slot(void)
{
    return &yas_ctx->error_line;
}

int *yas_context_error_col_slot(void)
{
    return &yas_ctx->error_col;
}
#endif

YasAtom *yas_context_library_registry_slot(void)
{
    return &yas_ctx->library_registry;
}

YasAtom *yas_context_search_paths_slot(void)
{
    return &yas_ctx->search_paths;
}

YasAtom *yas_context_features_slot(void)
{
    return &yas_ctx->features;
}

#ifdef YAS_FFI
struct YasFFIStructType **yas_context_ffi_registry_slot(void)
{
    return &yas_ctx->ffi_struct_registry;
}
#endif

YasContext *yas_context_create(void)
{
    YasContext *prev = yas_ctx;
    YasContext *ctx = calloc(1, sizeof(struct YasContext));
    if (!ctx)
        return NULL;

    /* calloc zeroing gives every field the same "lazy" start the old
     * file-scope statics had: threshold/quantum/min_threshold 0 and
     * stress_bytes -1 mean "sample env on first use", tables empty,
     * atoms NIL (YAS_ATOM_TYPE_NIL is 0), sandbox UNRESTRICTED (mode 0).
     * The one non-zero default: unmatched paths are ALLOWED. */
    ctx->sandbox_default_allow = 1;

    /* Bind while constructing so the feature-list allocation lands in
     * this context's heap; restore whatever the caller had bound. */
    yas_ctx = ctx;
    yas_init_features();

    yas_gil_enter();
    yas_ctx = prev ? prev : ctx; /* keep new context bound if none was */
    /* The lock now only guards process-wide bookkeeping around context
     * creation/teardown - there is no shared interpreter state left to
     * protect (ticket #47 milestone 2). */
    yas_gil_leave();

    return ctx;
}

/* Free every intern record in a table (records are plain malloc'd, not
 * GC-managed) and the table's slot array itself. */
static void context_free_intern_table(struct YasInternTable *t)
{
    if (!t->slots)
        return;
    for (size_t i = 0; i < t->capacity; i++)
        free(t->slots[i]);
    free(t->slots);
    t->slots = NULL;
    t->capacity = t->count = 0;
}

void yas_context_destroy(YasContext *ctx)
{
    if (!ctx)
        return;

    yas_gil_enter();
    YasContext *prev = yas_ctx;
    yas_ctx = ctx; /* collection helpers below resolve through yas_ctx */

    /* Free the whole heap: every object on the intrusive list, buffers
     * included. No marking needed - nothing survives its context.
     * Poolable objects land on the free-list pool as usual and the
     * drain loop right after releases those blocks for real. Open
     * ports close via gc_free_object's PORT case. */
    struct YasGCHeader *h = ctx->gc_all;
    while (h) {
        struct YasGCHeader *next = h->next;
        yas_gc_context_free_object(h);
        h = next;
    }
#ifndef YAS_NO_GC_POOL
    for (int t = 0; t < YAS_GC_OBJ_COUNT; t++) {
        struct YasGCHeader *p = ctx->gc_pool[t];
        while (p) {
            struct YasGCHeader *n = p->next;
            free(p);
            p = n;
        }
    }
#endif

    context_free_intern_table(&ctx->sym_intern);
    context_free_intern_table(&ctx->kw_intern);

    free(ctx->macro_memo_slots);
    free(ctx->gc_gray);
    free(ctx->roots.slots);

    for (int i = 0; i < ctx->allowed_count; i++)
        free(ctx->allowed_patterns[i]);
    for (int i = 0; i < ctx->denied_count; i++)
        free(ctx->denied_patterns[i]);
    /* providers[] holds caller-owned structs - not freed here. */

    free(ctx);
    yas_ctx = prev == ctx ? NULL : prev;
    yas_gil_leave();
}

#include "yas.h"

int yas_builtin_hashp(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || !yas_nilp(yas_cdr(args)))
        return yas_raise_error("args-error", "hash?: expected 1 argument");
    *result = (yas_car(args).type == YAS_ATOM_TYPE_HASH) ? yas_make_sym("T") : yas_nil;
    return YAS_ERROR_OK;
}

/* (hash-length hash) */
int yas_builtin_hash_length(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || !yas_nilp(yas_cdr(args)))
        return yas_raise_error("args-error", "hash-length: expected 1 argument");
    YasAtom a = yas_car(args);
    if (a.type != YAS_ATOM_TYPE_HASH)
        return yas_raise_error("type-error", "hash-length: expected hash");
    *result = yas_make_int(yas_hash_size(a));
    return YAS_ERROR_OK;
}

/* (hash-join h1 h2 ...) - merges h2 into h1, but since we are functional-ish,
   lets return NEW hash with all contents. Last one wins. */
int yas_builtin_hash_join(YasAtom args, YasAtom *result)
{
    YasAtom new_h = yas_make_hash();
    YasAtom p = args;
    while (!yas_nilp(p)) {
        YasAtom h = yas_car(p);
        if (h.type != YAS_ATOM_TYPE_HASH)
            return yas_raise_error("type-error", "hash-join: arguments must be hashes");

        struct YasHash *src = h.value.hash;
        for (size_t i = 0; i < src->size; i++) {
            yas_hash_set(new_h, src->keys[i], src->values[i]);
        }
        p = yas_cdr(p);
    }
    *result = new_h;
    return YAS_ERROR_OK;
}

/* Hash operations */
int yas_builtin_make_hash(YasAtom args, YasAtom *result)
{
    if (!yas_nilp(args))
        return yas_raise_error("args-error", "make-hash: expected 0 arguments");

    *result = yas_make_hash();
    return YAS_ERROR_OK;
}

int yas_builtin_hash_ref(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || yas_nilp(yas_cdr(args)))
        return yas_raise_error("args-error", "hash-ref: expected at least 2 arguments");

    YasAtom hash = yas_car(args);
    YasAtom key = yas_car(yas_cdr(args));
    YasAtom default_val = yas_nil;

    if (!yas_nilp(yas_cdr(yas_cdr(args)))) {
        default_val = yas_car(yas_cdr(yas_cdr(args)));
        if (!yas_nilp(yas_cdr(yas_cdr(yas_cdr(args)))))
            return yas_raise_error("args-error", "hash-ref: expected 2 or 3 arguments");
    }

    if (hash.type != YAS_ATOM_TYPE_HASH)
        return yas_raise_error("type-error", "hash-ref: first argument must be hash");

    *result = yas_hash_ref(hash, key, default_val);
    return YAS_ERROR_OK;
}

int yas_builtin_hash_set(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || yas_nilp(yas_cdr(args)) || yas_nilp(yas_cdr(yas_cdr(args))) ||
        !yas_nilp(yas_cdr(yas_cdr(yas_cdr(args)))))
        return yas_raise_error("args-error", "hash-set!: expected 3 arguments");

    YasAtom hash = yas_car(args);
    YasAtom key = yas_car(yas_cdr(args));
    YasAtom val = yas_car(yas_cdr(yas_cdr(args)));

    if (hash.type != YAS_ATOM_TYPE_HASH)
        return yas_raise_error("type-error", "hash-set!: first argument must be hash");

    yas_hash_set(hash, key, val);
    *result = val;
    return YAS_ERROR_OK;
}

int yas_builtin_hash_remove(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || yas_nilp(yas_cdr(args)) || !yas_nilp(yas_cdr(yas_cdr(args))))
        return yas_raise_error("args-error", "hash-remove!: expected 2 arguments");

    YasAtom hash = yas_car(args);
    YasAtom key = yas_car(yas_cdr(args));

    if (hash.type != YAS_ATOM_TYPE_HASH)
        return yas_raise_error("type-error", "hash-remove!: first argument must be hash");

    *result = yas_hash_remove(hash, key) ? yas_make_sym("T") : yas_nil;
    return YAS_ERROR_OK;
}

int yas_builtin_hash_keys(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || !yas_nilp(yas_cdr(args)))
        return yas_raise_error("args-error", "hash-keys: expected 1 argument");

    YasAtom hash = yas_car(args);
    if (hash.type != YAS_ATOM_TYPE_HASH)
        return yas_raise_error("type-error", "hash-keys: argument must be hash");

    YasAtom keys = yas_nil;
    struct YasHash *h = hash.value.hash;

    for (size_t i = 0; i < h->size; i++) {
        keys = yas_cons(h->keys[i], keys);
    }

    *result = keys;
    return YAS_ERROR_OK;
}

/* Metatable operations */
int yas_builtin_set_metatable(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || yas_nilp(yas_cdr(args)) || !yas_nilp(yas_cdr(yas_cdr(args))))
        return yas_raise_error("args-error", "set-metatable!: expected 2 arguments");

    YasAtom hash = yas_car(args);
    YasAtom mt = yas_car(yas_cdr(args));

    if (hash.type != YAS_ATOM_TYPE_HASH)
        return yas_raise_error("type-error", "set-metatable!: first argument must be hash");

    if (!yas_nilp(mt) && mt.type != YAS_ATOM_TYPE_HASH)
        return yas_raise_error("type-error", "set-metatable!: second argument must be hash or nil");

    if (yas_nilp(mt)) {
        hash.value.hash->metatable = NULL;
    } else {
        hash.value.hash->metatable = mt.value.hash;
        yas_gc_write_barrier(hash, mt);
    }

    *result = hash;
    return YAS_ERROR_OK;
}

int yas_builtin_get_metatable(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || !yas_nilp(yas_cdr(args)))
        return yas_raise_error("args-error", "get-metatable: expected 1 argument");

    YasAtom hash = yas_car(args);

    if (hash.type != YAS_ATOM_TYPE_HASH)
        return yas_raise_error("type-error", "get-metatable: argument must be hash");

    if (hash.value.hash->metatable) {
        result->type = YAS_ATOM_TYPE_HASH;
        result->value.hash = hash.value.hash->metatable;
    } else {
        *result = yas_nil;
    }

    return YAS_ERROR_OK;
}

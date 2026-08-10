#include "yas.h"

int yas_builtin_get_search_paths(YasAtom args, YasAtom *result)
{
    if (!yas_nilp(args))
        return yas_raise_error("args-error", "get-search-paths: expected 0 arguments");
    *result = yas_global_search_paths;
    return YAS_ERROR_OK;
}

int yas_builtin_set_search_paths(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || !yas_nilp(yas_cdr(args)))
        return yas_raise_error("args-error", "set-search-paths!: expected 1 argument");
    YasAtom paths = yas_car(args);
    /* Validate it's a list of strings */
    YasAtom p = paths;
    while (!yas_nilp(p)) {
        if (p.type != YAS_ATOM_TYPE_PAIR)
            return yas_raise_error("type-error", "set-search-paths!: expected list of strings");
        if (yas_car(p).type != YAS_ATOM_TYPE_STRING)
            return yas_raise_error("type-error", "set-search-paths!: paths must be strings");
        p = yas_cdr(p);
    }
    yas_global_search_paths = paths;
    *result = paths;
    return YAS_ERROR_OK;
}

int yas_builtin_add_search_path(YasAtom args, YasAtom *result)
{
    if (yas_nilp(args) || !yas_nilp(yas_cdr(args)))
        return yas_raise_error("args-error", "add-search-path!: expected 1 argument");
    YasAtom path = yas_car(args);
    if (path.type != YAS_ATOM_TYPE_STRING)
        return yas_raise_error("type-error", "add-search-path!: path must be string");
    yas_global_search_paths = yas_cons(path, yas_global_search_paths);
    *result = yas_global_search_paths;
    return YAS_ERROR_OK;
}

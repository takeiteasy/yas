#include "lisp.h"

int builtin_get_search_paths(Atom args, Atom *result)
{
	if (!nilp(args))
		return raise_error("args-error", "get-search-paths: expected 0 arguments");
	*result = global_search_paths;
	return Error_OK;
}

int builtin_set_search_paths(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "set-search-paths!: expected 1 argument");
	Atom paths = car(args);
	/* Validate it's a list of strings */
	Atom p = paths;
	while (!nilp(p)) {
		if (p.type != AtomType_Pair)
			return raise_error("type-error", "set-search-paths!: expected list of strings");
		if (car(p).type != AtomType_String)
			return raise_error("type-error", "set-search-paths!: paths must be strings");
		p = cdr(p);
	}
	global_search_paths = paths;
	*result = paths;
	return Error_OK;
}

int builtin_add_search_path(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "add-search-path!: expected 1 argument");
	Atom path = car(args);
	if (path.type != AtomType_String)
		return raise_error("type-error", "add-search-path!: path must be string");
	global_search_paths = cons(path, global_search_paths);
	*result = global_search_paths;
	return Error_OK;
}

#include "lisp.h"
#include <stdlib.h>
#include <string.h>

/* (keyword? obj) */
int builtin_keywordp(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "keyword?: expected 1 argument");
	*result = car(args).type == AtomType_Keyword ? make_sym("T") : nil;
	return Error_OK;
}

/* (keyword->string kw) */
int builtin_keyword_to_string(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "keyword->string: expected 1 argument");
	if (car(args).type != AtomType_Keyword)
		return raise_error("type-error", "keyword->string: argument must be keyword");
	*result = make_string(car(args).value.keyword);
	return Error_OK;
}

/* (string->keyword str) */
int builtin_string_to_keyword(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "string->keyword: expected 1 argument");
	if (car(args).type != AtomType_String)
		return raise_error("type-error", "string->keyword: argument must be string");
	
	/* Convert to uppercase for consistency */
	char *buf = strdup(car(args).value.string);
	for (char *p = buf; *p; p++) {
		if (*p >= 'a' && *p <= 'z') *p -= 32;
	}
	*result = make_keyword(buf);
	free(buf);
	return Error_OK;
}

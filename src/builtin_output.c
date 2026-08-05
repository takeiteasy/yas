#include "lisp.h"
#include <stdio.h>
#include <stdlib.h>

int builtin_print(Atom args, Atom *result)

{
	if (nilp(args)) {
		putchar('\n');
		*result = nil;
		return Error_OK;
	}

	while (!nilp(args)) {
		print_expr(car(args));
		if (!nilp(cdr(args)))
			putchar(' ');
		args = cdr(args);
	}
	putchar('\n');
	
	*result = nil;
	return Error_OK;
}

/* (->string value) - convert any value to string */
int builtin_tostring(Atom args, Atom *result)
{
	if (nilp(args) || !nilp(cdr(args)))
		return raise_error("args-error", "->string: expected 1 argument");
	
	char *str = sprint_expr(car(args));
	*result = make_string(str);
	free(str);
	return Error_OK;
}

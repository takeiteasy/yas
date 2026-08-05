#include "yas.h"
#include <stdio.h>
#include <stdlib.h>

int yas_builtin_print(YasAtom args, YasAtom *result)

{
	if (yas_nilp(args)) {
		putchar('\n');
		*result = yas_nil;
		return YAS_ERROR_OK;
	}

	while (!yas_nilp(args)) {
		yas_print_expr(yas_car(args));
		if (!yas_nilp(yas_cdr(args)))
			putchar(' ');
		args = yas_cdr(args);
	}
	putchar('\n');
	
	*result = yas_nil;
	return YAS_ERROR_OK;
}

/* (->string value) - convert any value to string */
int yas_builtin_tostring(YasAtom args, YasAtom *result)
{
	if (yas_nilp(args) || !yas_nilp(yas_cdr(args)))
		return yas_raise_error("args-error", "->string: expected 1 argument");
	
	char *str = yas_sprint_expr(yas_car(args));
	*result = yas_make_string(str);
	free(str);
	return YAS_ERROR_OK;
}

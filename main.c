#include "lisp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <readline/readline.h>

char *slurp(const char *path)
{
	FILE *file;
	char *buf;
	long len;

	file = fopen(path, "r");
	if (!file)
		return NULL;
	fseek(file, 0, SEEK_END);
	len = ftell(file);
	fseek(file, 0, SEEK_SET);

	buf = malloc(len + 1);
	if (!buf)
		return NULL;

	fread(buf, 1, len, file);
	buf[len] = 0;
	fclose(file);

	return buf;
}

void load_file(Atom env, const char *path)
{
	char *text;

	printf("Reading %s...\n", path);
	text = slurp(path);
	if (text) {
		const char *p = text;
		Atom expr;
		ErrorContext err = {0};
		err.pos.filename = path;
		err.pos.source = text;

		while (1) {
			err.code = Error_OK;
			err.message[0] = '\0';

			Error read_err = read_expr(p, &p, &expr, &err);
			if (read_err) {
				if (err.message[0] != '\0') {
					/* Real error, not just end of file */
					error_print(&err);
				}
				break;
			}

			Atom result;
			Error eval_err = eval_expr(expr, env, &result);
			if (eval_err) {
				printf("Error evaluating expression:\n\t");
				print_expr(expr);
				putchar('\n');
			} else {
				print_expr(result);
				putchar('\n');
			}
		}
		free(text);
	}
}

int main(int argc, char **argv)
{
	Atom env;
	char *input;

	env = env_create(nil);

	/* Set up the initial environment */
	env_define(env, make_sym("CAR"), make_builtin(builtin_car));
	env_define(env, make_sym("CDR"), make_builtin(builtin_cdr));
	env_define(env, make_sym("CONS"), make_builtin(builtin_cons));
	env_define(env, make_sym("+"), make_builtin(builtin_add));
	env_define(env, make_sym("-"), make_builtin(builtin_subtract));
	env_define(env, make_sym("*"), make_builtin(builtin_multiply));
	env_define(env, make_sym("/"), make_builtin(builtin_divide));
	env_define(env, make_sym("T"), make_sym("T"));
	env_define(env, make_sym("="), make_builtin(builtin_numeq));
	env_define(env, make_sym("<"), make_builtin(builtin_less));
	env_define(env, make_sym("EQ?"), make_builtin(builtin_eq));
	env_define(env, make_sym("PAIR?"), make_builtin(builtin_pairp));
	env_define(env, make_sym("PROCEDURE?"), make_builtin(builtin_procp));
	env_define(env, make_sym("FLOAT?"), make_builtin(builtin_floatp));
	env_define(env, make_sym("STRING?"), make_builtin(builtin_stringp));
	env_define(env, make_sym("VECTOR?"), make_builtin(builtin_vectorp));
	env_define(env, make_sym("HASH?"), make_builtin(builtin_hashp));
	env_define(env, make_sym("LENGTH"), make_builtin(builtin_length));
	env_define(env, make_sym("CONCAT"), make_builtin(builtin_concat));
	env_define(env, make_sym("MAKE-VECTOR"), make_builtin(builtin_make_vector));
	env_define(env, make_sym("VECTOR-REF"), make_builtin(builtin_vector_ref));
	env_define(env, make_sym("VECTOR-SET!"), make_builtin(builtin_vector_set));
	env_define(env, make_sym("MAKE-HASH"), make_builtin(builtin_make_hash));
	env_define(env, make_sym("HASH-REF"), make_builtin(builtin_hash_ref));
	env_define(env, make_sym("HASH-SET!"), make_builtin(builtin_hash_set));
	env_define(env, make_sym("HASH-REMOVE!"), make_builtin(builtin_hash_remove));

	/* Exception builtins */
	env_define(env, make_sym("THROW"), make_builtin(builtin_throw));
	env_define(env, make_sym("ERROR"), make_builtin(builtin_error));

	load_file(env, "library.lisp");

	/* Main loop */
	while ((input = readline("> ")) != NULL) {
		ErrorContext err = {0};
		err.pos.filename = "<stdin>";
		err.pos.source = input;

		const char *p = input;
		Error read_err;
		Atom expr, result;

		read_err = read_expr(p, &p, &expr, &err);

		if (read_err) {
			error_print(&err);
			free(input);
			continue;
		}

		Error eval_err = eval_expr(expr, env, &result);

		switch (eval_err) {
		case Error_OK:
			print_expr(result);
			putchar('\n');
			break;
		case Error_Syntax:
			puts("Syntax error");
			break;
		case Error_Unbound:
			puts("Symbol not bound");
			break;
		case Error_Args:
			puts("Wrong number of arguments");
			break;
		case Error_Type:
			puts("Wrong type");
			break;
		case Error_Exception:
			printf("Uncaught exception: ");
			print_expr(ctx_exception);
			putchar('\n');
			break;
		}

		free(input);
	}

	return 0;
}

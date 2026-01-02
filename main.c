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
				printf("Error evaluating expression:\n");
				switch (eval_err) {
				case Error_Syntax: puts("Syntax error"); break;
				case Error_Unbound: puts("Symbol not bound"); break;
				case Error_Args: puts("Wrong number of arguments"); break;
				case Error_Type: puts("Wrong type"); break;
				case Error_Exception:
					printf("Uncaught exception: ");
					print_expr(ctx_exception);
					putchar('\n');
					break;
				default:
					printf("Unknown error %d\n", eval_err);
				}
				printf("\t");
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

	/* Import (gold base) */
	module_init();
	Atom gold_base = list_create(2, make_sym("GOLD"), make_sym("BASE"));
	Atom import_args = list_create(1, gold_base);
	Atom ignore;
	module_import(import_args, env, &ignore);

	if (argc > 1) {
		for (int i = 1; i < argc; i++)
			load_file(env, argv[i]);
		return 0;
	}

	printf("(gold repl v0.0.1 :)\n\n");

	/* Main loop */
	while ((input = readline("> ")) != NULL) {
		ErrorContext err = {0};
		err.pos.filename = "<stdin>";
		err.pos.source = input;

		const char *p = input;
		Error read_err;
		Atom expr, result;

		if (input[0] == '\0') {
			free(input);
			continue;
		} else if (input[0] == '/') {
			const char *iinput = input + 1;
			if (strncmp(iinput, "exit", 4) == 0 ||
				strncmp(iinput, "quit", 4) == 0) {
				free(input);
				return 0;
			}
		}

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

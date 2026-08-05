#include "lisp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <readline/readline.h>

/* Returns 0 if every top-level form in the file read and evaluated
 * cleanly, non-zero if a syntax or eval error occurred anywhere - callers
 * use this to set the process exit code. */
int load_file(Atom env, const char *path)
{
	char *text;
	int had_error = 0;

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
			if (read_err == Error_Skip) {
				/* Feature conditional excluded this expression, continue */
				continue;
			}
			if (read_err) {
				if (err.message[0] != '\0') {
					/* Real error, not just end of file */
					error_print(&err);
					had_error = 1;
				}
				break;
			}

			Atom result;
			Error eval_err = eval_expr(expr, env, &result);
			if (eval_err) {
				had_error = 1;
				extern const char *current_file_path;
				printf("Error evaluating expression");
				if (current_file_path) {
					printf(" in %s", current_file_path);
				}
				printf(":\n");
				switch (eval_err) {
				case Error_Syntax: puts("Syntax error"); break;
				case Error_Unbound:
					if (!nilp(ctx_exception) && ctx_exception.type == AtomType_Pair) {
						Atom msg = car(cdr(ctx_exception));
						if (msg.type == AtomType_String) {
							printf("%s\n", msg.value.string);
						} else {
							puts("Symbol not bound");
						}
					} else {
						puts("Symbol not bound");
					}
					break;
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
				printf("  Code: ");
				print_expr(expr);
				putchar('\n');
			}
			/* Unlike the REPL, running a file does not auto-print each
			 * top-level result - scripts print explicitly if they want
			 * output. */
		}
		free(text);
	} else {
		had_error = 1;
	}

	return had_error;
}

int main(int argc, char **argv)
{
	Atom env;
	char *input;

	/* Initialize features before anything else */
	init_features();

	env = env_create(nil);

	/* Import (yas base) */
	Atom yas_base = list_create(2, make_sym("YAS"), make_sym("BASE"));
	Atom import_args = list_create(1, yas_base);
	Atom ignore;
	module_import(import_args, env, &ignore);

	if (argc > 1) {
		int had_error = 0;
		for (int i = 1; i < argc; i++) {
			if (load_file(env, argv[i]))
				had_error = 1;
		}
		return had_error ? 1 : 0;
	}

	printf("(yas repl v0.0.1 :)\n\n");

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

		if (read_err == Error_Skip) {
			/* Feature conditional excluded this expression */
			free(input);
			continue;
		}
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
		case Error_Skip:
			/* Should not reach here - handled earlier */
			break;
		}

		free(input);
	}

	return 0;
}

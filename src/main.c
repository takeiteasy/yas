#include "yas.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <readline/readline.h>

/* Returns 0 if every top-level form in the file read and evaluated
 * cleanly, non-zero if a syntax or eval error occurred anywhere - callers
 * use this to set the process exit code. */
int yas_load_file(YasAtom env, const char *path)
{
    char *text;
    int had_error = 0;

    text = yas_slurp(path);
    if (text) {
        const char *p = text;
        YasAtom expr;
        YasErrorContext err = {0};
        err.pos.filename = path;
        err.pos.source = text;

        while (1) {
            err.code = YAS_ERROR_OK;
            err.message[0] = '\0';

            YasError read_err = yas_read_expr(p, &p, &expr, &err);
            if (read_err == YAS_ERROR_SKIP) {
                /* Feature conditional excluded this expression, continue */
                continue;
            }
            if (read_err) {
                if (err.message[0] != '\0') {
                    /* Real error, not just end of file */
                    yas_error_print(&err);
                    had_error = 1;
                }
                break;
            }

            YasAtom result;
            YasError eval_err = yas_eval_expr(expr, env, &result);
            if (eval_err) {
                had_error = 1;
                extern const char *yas_current_file_path;
                printf("Error evaluating expression");
                if (yas_current_file_path) {
                    printf(" in %s", yas_current_file_path);
                }
                printf(":\n");
                switch (eval_err) {
                    case YAS_ERROR_SYNTAX:
                        puts("Syntax error");
                        break;
                    case YAS_ERROR_UNBOUND:
                        if (!yas_nilp(yas_ctx_exception) && yas_ctx_exception.type == YAS_ATOM_TYPE_PAIR) {
                            YasAtom msg = yas_car(yas_cdr(yas_ctx_exception));
                            if (msg.type == YAS_ATOM_TYPE_STRING) {
                                printf("%s\n", msg.value.string);
                            } else {
                                puts("Symbol not bound");
                            }
                        } else {
                            puts("Symbol not bound");
                        }
                        break;
                    case YAS_ERROR_ARGS:
                        puts("Wrong number of arguments");
                        break;
                    case YAS_ERROR_TYPE:
                        puts("Wrong type");
                        break;
                    case YAS_ERROR_EXCEPTION:
                        printf("Uncaught exception: ");
                        yas_print_expr(yas_ctx_exception);
                        putchar('\n');
                        break;
                    default:
                        printf("Unknown error %d\n", eval_err);
                }
                printf("  Code: ");
                yas_print_expr(expr);
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
    YasAtom env;
    char *input;

    /* Initialize features before anything else */
    yas_init_features();

    env = yas_env_create(yas_nil);
    /* Root the top-level environment for the rest of the process's
     * lifetime: unlike a library's own env (reachable via
     * yas_library_registry once registered), nothing else references
     * this one, and importing (yas base) below evaluates real library
     * forms - allocating enough to legitimately trigger a GC cycle -
     * before `env` ever gets any bindings of its own. Without this, an
     * early collection frees the pair out from under it. See yas.h's
     * yas_gc_push_root()/docs/EMBEDDING.md. */
    yas_gc_push_root(&env);

    /* Import (yas base) */
    YasAtom yas_base = yas_list_create(2, yas_make_sym("YAS"), yas_make_sym("BASE"));
    /* `yas_base` is a fresh, otherwise-unreachable local held across the
     * further allocating yas_list_create(1, yas_base) call right below -
     * root it for that one call. */
    yas_gc_push_root(&yas_base);
    YasAtom import_args = yas_list_create(1, yas_base);
    yas_gc_pop_roots(1); /* yas_base */
    YasAtom ignore;
    /* Same reasoning as `env` above: import_args is a fresh, otherwise-
     * unreachable list that yas_module_import walks across the same
     * allocating library-load call. */
    yas_gc_push_root(&import_args);
    yas_module_import(import_args, env, &ignore);
    yas_gc_pop_roots(1);

    if (argc > 1) {
        int had_error = 0;
        for (int i = 1; i < argc; i++) {
            if (yas_load_file(env, argv[i]))
                had_error = 1;
        }
        return had_error ? 1 : 0;
    }

    printf("(yas repl v0.0.1 :)\n\n");

    /* Main loop */
    while ((input = readline("> ")) != NULL) {
        YasErrorContext err = {0};
        err.pos.filename = "<stdin>";
        err.pos.source = input;

        const char *p = input;
        YasError read_err;
        YasAtom expr, result;

        if (input[0] == '\0') {
            free(input);
            continue;
        } else if (input[0] == '/') {
            const char *iinput = input + 1;
            if (strncmp(iinput, "exit", 4) == 0 || strncmp(iinput, "quit", 4) == 0) {
                free(input);
                return 0;
            }
        }

        read_err = yas_read_expr(p, &p, &expr, &err);

        if (read_err == YAS_ERROR_SKIP) {
            /* Feature conditional excluded this expression */
            free(input);
            continue;
        }
        if (read_err) {
            yas_error_print(&err);
            free(input);
            continue;
        }

        YasError eval_err = yas_eval_expr(expr, env, &result);

        switch (eval_err) {
            case YAS_ERROR_OK:
                yas_print_expr(result);
                putchar('\n');
                break;
            case YAS_ERROR_SYNTAX:
                puts("Syntax error");
                break;
            case YAS_ERROR_UNBOUND:
                puts("Symbol not bound");
                break;
            case YAS_ERROR_ARGS:
                puts("Wrong number of arguments");
                break;
            case YAS_ERROR_TYPE:
                puts("Wrong type");
                break;
            case YAS_ERROR_EXCEPTION:
                printf("Uncaught exception: ");
                yas_print_expr(yas_ctx_exception);
                putchar('\n');
                break;
            case YAS_ERROR_SKIP:
                /* Should not reach here - handled earlier */
                break;
        }

        free(input);
    }

    return 0;
}

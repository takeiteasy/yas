#include "lisp.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Compute line and column from source start to current position */
static void compute_pos(const char *source, const char *current,
                        const char *filename, ReaderPos *pos)
{
	pos->source = source;
	pos->filename = filename;
	pos->line = 1;
	pos->column = 1;

	for (const char *p = source; p < current; p++) {
		if (*p == '\n') {
			pos->line++;
			pos->column = 1;
		} else {
			pos->column++;
		}
	}
}

/* Set error with position and message */
static void set_error(ErrorContext *err, Error code, const char *source,
                      const char *pos_ptr, const char *filename,
                      const char *message)
{
	if (!err) return;
	err->code = code;
	compute_pos(source, pos_ptr, filename, &err->pos);
	snprintf(err->message, ERROR_MSG_MAX, "%s", message);
}

/* Print error with position and context */
void error_print(ErrorContext *err)
{
	if (!err || err->code == Error_OK) return;

	/* Print location and message */
	printf("%s:%d:%d: error: %s\n",
	       err->pos.filename ? err->pos.filename : "<unknown>",
	       err->pos.line, err->pos.column, err->message);

	/* Find and print the offending line */
	if (err->pos.source) {
		const char *line_start = err->pos.source;
		const char *p = err->pos.source;

		/* Find start of error line */
		int current_line = 1;
		while (current_line < err->pos.line && *p) {
			if (*p == '\n') {
				current_line++;
				line_start = p + 1;
			}
			p++;
		}

		/* Find end of line */
		const char *line_end = line_start;
		while (*line_end && *line_end != '\n') line_end++;

		/* Print the line */
		printf("    ");
		for (p = line_start; p < line_end; p++) {
			putchar(*p);
		}
		putchar('\n');

		/* Print caret pointing to error */
		printf("    ");
		for (int i = 1; i < err->pos.column; i++) {
			putchar(' ');
		}
		printf("^\n");
	}
}

static int lex(const char *str, const char **start, const char **end,
               const char *source, const char *filename, ErrorContext *err)
{
	const char *ws = " \t\n";
	const char *delim = "(); \t\n";
	const char *prefix = "()'`";

	str += strspn(str, ws);

	if (str[0] == '\0') {
		*start = *end = NULL;
		/* Don't set error message here - caller provides context */
		return Error_Syntax;
	}

	*start = str;

	if (strchr(prefix, str[0]) != NULL)
		*end = str + 1;
	else if (str[0] == ',')
		*end = str + (str[1] == '@' ? 2 : 1);
	else if (str[0] == ';') {
		str = strchr(str, '\n');
		if (!str) {
			*start = *end = NULL;
			set_error(err, Error_Syntax, source, str, filename,
			          "Unexpected end of input after comment");
			return Error_Syntax;
		}
		return lex(str, start, end, source, filename, err);
	} else
		*end = str + strcspn(str, delim);

	return Error_OK;
}

static int parse_simple(const char *start, const char *end, Atom *result,
                        const char *source, const char *filename, ErrorContext *err)
{
	char *buf, *p;

	/* Is it an integer? */
	long val = strtol(start, &p, 10);
	if (p == end) {
		result->type = AtomType_Integer;
		result->value.integer = val;
		return Error_OK;
	}

	/* NIL or symbol */
	buf = malloc(end - start + 1);
	p = buf;
	while (start != end)
		*p++ = toupper(*start), ++start;
	*p = '\0';

	if (strcmp(buf, "NIL") == 0)
		*result = nil;
	else
		*result = make_sym(buf);

	free(buf);

	return Error_OK;
}

static int read_list(const char *start, const char **end, Atom *result,
                     const char *source, const char *filename, ErrorContext *err)
{
	Atom p;

	*end = start;
	p = *result = nil;

	for (;;) {
		const char *token;
		Atom item;
		Error e;

		e = lex(*end, &token, end, source, filename, err);
		if (e) {
			/* If lex failed (likely EOF), provide better error message */
			if (err && e == Error_Syntax) {
				set_error(err, Error_Syntax, source, start - 1, filename,
				          "Unterminated list - missing ')'");
			}
			return e;
		}

		if (token[0] == ')')
			return Error_OK;

		if (token[0] == '.' && *end - token == 1) {
			/* Improper list */
			if (nilp(p)) {
				set_error(err, Error_Syntax, source, token, filename,
				          "Unexpected '.' at start of list");
				return Error_Syntax;
			}

			e = read_expr(*end, end, &item, err);
			if (e)
				return e;

			cdr(p) = item;

			/* Read the closing ')' */
			e = lex(*end, &token, end, source, filename, err);
			if (e) {
				set_error(err, Error_Syntax, source, *end, filename,
				          "Expected ')' after dotted pair");
				return e;
			}
			if (token[0] != ')') {
				set_error(err, Error_Syntax, source, token, filename,
				          "Only one expression allowed after '.'");
				return Error_Syntax;
			}

			return Error_OK;
		}

		e = read_expr(token, end, &item, err);
		if (e)
			return e;

		if (nilp(p)) {
			/* First item */
			*result = cons(item, nil);
			p = *result;
		} else {
			cdr(p) = cons(item, nil);
			p = cdr(p);
		}
	}
}

/* Internal implementation with source tracking */
static int read_expr_internal(const char *input, const char **end, Atom *result,
                              const char *source, const char *filename, ErrorContext *err)
{
	const char *token;
	Error e;

	e = lex(input, &token, end, source, filename, err);
	if (e)
		return e;

	if (token[0] == '(') {
		return read_list(*end, end, result, source, filename, err);
	} else if (token[0] == ')') {
		set_error(err, Error_Syntax, source, token, filename,
		          "Unexpected ')'");
		return Error_Syntax;
	} else if (token[0] == '\'') {
		*result = cons(make_sym("QUOTE"), cons(nil, nil));
		e = read_expr_internal(*end, end, &car(cdr(*result)), source, filename, err);
		if (e && err) {
			set_error(err, Error_Syntax, source, token, filename,
			          "Expected expression after quote");
		}
		return e;
	} else if (token[0] == '`') {
		*result = cons(make_sym("QUASIQUOTE"), cons(nil, nil));
		e = read_expr_internal(*end, end, &car(cdr(*result)), source, filename, err);
		if (e && err) {
			set_error(err, Error_Syntax, source, token, filename,
			          "Expected expression after quasiquote");
		}
		return e;
	} else if (token[0] == ',') {
		*result = cons(make_sym(
			token[1] == '@' ? "UNQUOTE-SPLICING" : "UNQUOTE"),
			cons(nil, nil));
		e = read_expr_internal(*end, end, &car(cdr(*result)), source, filename, err);
		if (e && err) {
			set_error(err, Error_Syntax, source, token, filename,
			          "Expected expression after unquote");
		}
		return e;
	} else {
		return parse_simple(token, *end, result, source, filename, err);
	}
}

/* Public interface - source is the input itself */
int read_expr(const char *input, const char **end, Atom *result, ErrorContext *err)
{
	const char *filename = err && err->pos.filename ? err->pos.filename : "<stdin>";
	const char *source = err && err->pos.source ? err->pos.source : input;
	return read_expr_internal(input, end, result, source, filename, err);
}

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
	const char *delim = "(); \t\n\"";
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
	else if (str[0] == '"')
		*end = str + 1;  /* String opening quote */
	else if (str[0] == '#' && str[1] == '(')
		*end = str + 2;  /* Vector literal #( */
	else if (str[0] == '#' && str[1] == '\\') {
		/* Character literal #\x or #\newline etc */
		const char *p = str + 2;
		if (*p == 'x' || *p == 'u' || *p == 'U') {
			/* Hex/unicode escape - read hex digits */
			p++;
			while (isxdigit(*p)) p++;
		} else if (isalpha(*p)) {
			/* Named char or single char */
			while (isalpha(*p)) p++;
		} else if (*p) {
			/* Single character */
			p++;
		}
		*end = p;
	}
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

	/* Check if it looks like a number (starts with digit, or - followed by digit) */
	int is_number = isdigit(start[0]) || 
		(start[0] == '-' && end - start > 1 && isdigit(start[1])) ||
		(start[0] == '.' && end - start > 1 && isdigit(start[1]));

	if (is_number) {
		/* Check if it's a float (contains .) */
		int has_dot = 0;
		for (const char *c = start; c < end; c++) {
			if (*c == '.') { has_dot = 1; break; }
		}
		
		if (has_dot) {
			double fval = strtod(start, &p);
			if (p == end) {
				*result = make_float(fval);
				return Error_OK;
			}
		} else {
			long val = strtol(start, &p, 10);
			if (p == end) {
				result->type = AtomType_Integer;
				result->value.integer = val;
				return Error_OK;
			}
		}
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
	} else if (token[0] == '"') {
		/* String literal */
		const char *str_start = *end;
		const char *p = str_start;
		size_t len = 0;
		
		/* Count length and find end */
		while (*p && *p != '"') {
			if (*p == '\\' && p[1]) p++;
			p++;
			len++;
		}
		if (!*p) {
			set_error(err, Error_Syntax, source, token, filename,
			          "Unterminated string");
			return Error_Syntax;
		}
		
		/* Build string with escape handling */
		char *buf = malloc(len + 1);
		char *dst = buf;
		p = str_start;
		while (*p != '"') {
			if (*p == '\\') {
				p++;
				switch (*p) {
				case 'n': *dst++ = '\n'; break;
				case 't': *dst++ = '\t'; break;
				case 'r': *dst++ = '\r'; break;
				case '\\': *dst++ = '\\'; break;
				case '"': *dst++ = '"'; break;
				default: *dst++ = *p; break;
				}
			} else {
				*dst++ = *p;
			}
			p++;
		}
		*dst = '\0';
		*end = p + 1;  /* Skip closing quote */
		
		result->type = AtomType_String;
		result->value.string = buf;
		return Error_OK;
	} else if (token[0] == '#' && token[1] == '(') {
		/* Vector literal #(1 2 3) */
		Atom list_result;
		e = read_list(*end, end, &list_result, source, filename, err);
		if (e) return e;
		
		/* Count list length */
		size_t len = 0;
		Atom p = list_result;
		while (!nilp(p)) { len++; p = cdr(p); }
		
		/* Create vector from list */
		*result = make_vector(len, nil);
		p = list_result;
		for (size_t i = 0; i < len; i++) {
			vector_set(*result, i, car(p));
			p = cdr(p);
		}
		return Error_OK;
	} else if (token[0] == '#' && token[1] == '\\') {
		/* Character literal */
		const char *start = token + 2;
		size_t len = *end - start;
		int32_t cp;
		
		if (len == 0) {
			set_error(err, Error_Syntax, source, token, filename,
			          "Empty character literal");
			return Error_Syntax;
		}
		
		/* Named characters */
		if (len == 7 && strncmp(start, "newline", 7) == 0) {
			cp = '\n';
		} else if (len == 5 && strncmp(start, "space", 5) == 0) {
			cp = ' ';
		} else if (len == 3 && strncmp(start, "tab", 3) == 0) {
			cp = '\t';
		} else if (len == 6 && strncmp(start, "return", 6) == 0) {
			cp = '\r';
		} else if (start[0] == 'x' && len > 1) {
			/* Hex escape #\xNN */
			char *endp;
			cp = strtol(start + 1, &endp, 16);
			if (endp != *end) {
				set_error(err, Error_Syntax, source, token, filename,
				          "Invalid hex character literal");
				return Error_Syntax;
			}
		} else if (start[0] == 'u' && len == 5) {
			/* Unicode escape #\uNNNN */
			char *endp;
			cp = strtol(start + 1, &endp, 16);
			if (endp != *end) {
				set_error(err, Error_Syntax, source, token, filename,
				          "Invalid unicode character literal");
				return Error_Syntax;
			}
		} else if (start[0] == 'U' && len == 9) {
			/* Unicode escape #\UNNNNNNNN */
			char *endp;
			cp = strtol(start + 1, &endp, 16);
			if (endp != *end) {
				set_error(err, Error_Syntax, source, token, filename,
				          "Invalid unicode character literal");
				return Error_Syntax;
			}
		} else if (len == 1) {
			/* Single character */
			cp = (unsigned char)start[0];
		} else {
			/* Try UTF-8 decode for multi-byte chars like #\λ */
			int n = utf8_decode(start, &cp);
			if (n != (int)len) {
				set_error(err, Error_Syntax, source, token, filename,
				          "Unknown character name");
				return Error_Syntax;
			}
		}
		
		*result = make_char(cp);
		return Error_OK;
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

#include "yas.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Compute line and column from source start to current position */
static void compute_pos(const char *source, const char *current,
                        const char *filename, YasReaderPos *pos)
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
static void set_error(YasErrorContext *err, YasError code, const char *source,
                      const char *pos_ptr, const char *filename,
                      const char *message)
{
	if (!err) return;
	err->code = code;
	compute_pos(source, pos_ptr, filename, &err->pos);
	snprintf(err->message, YAS_ERROR_MSG_MAX, "%s", message);
}

/* Print error with position and context */
void yas_error_print(YasErrorContext *err)
{
	if (!err || err->code == YAS_ERROR_OK) return;

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
               const char *source, const char *filename, YasErrorContext *err)
{
	const char *ws = " \t\n";
	const char *delim = "(); \t\n\"";
	const char *prefix = "()'`";

	str += strspn(str, ws);

	if (str[0] == '\0') {
		*start = *end = NULL;
		/* Don't set error message here - caller provides context */
		return YAS_ERROR_SYNTAX;
	}

	*start = str;

	if (strchr(prefix, str[0]) != NULL)
		*end = str + 1;
	else if (str[0] == '"')
		*end = str + 1;  /* String opening quote */
	else if (str[0] == '#' && str[1] == '(')
		*end = str + 2;  /* Vector literal #( */
	else if (str[0] == '#' && strncmp(str + 1, "vec(", 4) == 0)
		*end = str + 5;  /* #vec( literal */
	else if (str[0] == '#' && strncmp(str + 1, "mat(", 4) == 0)
		*end = str + 5;  /* #mat( literal */
	else if (str[0] == '#' && strncmp(str + 1, "hash(", 5) == 0)
		*end = str + 6;  /* #hash( literal */
	else if (str[0] == '#' && (str[1] == '+' || str[1] == '-'))
		*end = str + 2;  /* Reader conditionals #+ #- */
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
			set_error(err, YAS_ERROR_SYNTAX, source, str, filename,
			          "Unexpected end of input after comment");
			return YAS_ERROR_SYNTAX;
		}
		return lex(str, start, end, source, filename, err);
	} else
		*end = str + strcspn(str, delim);

	return YAS_ERROR_OK;
}

static int parse_simple(const char *start, const char *end, YasAtom *result,
                        const char *source, const char *filename, YasErrorContext *err)
{
	char *buf, *p;

	/* Check if it looks like a number (starts with digit, or - followed by digit) */
	int is_number = isdigit(start[0]) || 
		(start[0] == '-' && end - start > 1 && isdigit(start[1])) ||
		(start[0] == '.' && end - start > 1 && isdigit(start[1]));

	if (is_number) {
		/* Check for hex (0x) or binary (0b) prefix */
		if (start[0] == '0' && end - start > 2) {
			if (start[1] == 'x' || start[1] == 'X') {
				long val = strtol(start + 2, &p, 16);
				if (p == end) {
					result->type = YAS_ATOM_TYPE_INTEGER;
					result->value.integer = val;
					return YAS_ERROR_OK;
				}
			} else if (start[1] == 'b' || start[1] == 'B') {
				long val = strtol(start + 2, &p, 2);
				if (p == end) {
					result->type = YAS_ATOM_TYPE_INTEGER;
					result->value.integer = val;
					return YAS_ERROR_OK;
				}
			}
		}

		/* Check if it's a float (contains .) */
		int has_dot = 0;
		for (const char *c = start; c < end; c++) {
			if (*c == '.') { has_dot = 1; break; }
		}
		
		if (has_dot) {
			double fval = strtod(start, &p);
			if (p == end) {
				*result = yas_make_float(fval);
				return YAS_ERROR_OK;
			}
		} else {
			long val = strtol(start, &p, 10);
			if (p == end) {
				result->type = YAS_ATOM_TYPE_INTEGER;
				result->value.integer = val;
				return YAS_ERROR_OK;
			}
		}
	}

	/* Check for keyword (starts with :) */
	if (start[0] == ':' && end - start > 1) {
		buf = malloc(end - start);
		p = buf;
		start++;  /* Skip the colon */
		while (start != end)
			*p++ = toupper(*start), ++start;
		*p = '\0';
		*result = yas_make_keyword(buf);
		free(buf);
		return YAS_ERROR_OK;
	}

	/* NIL or symbol */
	buf = malloc(end - start + 1);
	p = buf;
	while (start != end)
		*p++ = toupper(*start), ++start;
	*p = '\0';

	if (strcmp(buf, "NIL") == 0)
		*result = yas_nil;
	else
		*result = yas_make_sym(buf);

	free(buf);

	return YAS_ERROR_OK;
}

static int read_list(const char *start, const char **end, YasAtom *result,
                     const char *source, const char *filename, YasErrorContext *err)
{
	YasAtom p;

	*end = start;
	p = *result = yas_nil;

	/* `*result` (the list being built) accumulates across repeated
	 * yas_cons() calls below and has no protection of its own before
	 * this function ever sees it (a caller several frames up may root
	 * its own copy, e.g. yas_eval_source()'s `expr`, but every level of
	 * *nested* list/quote reading introduces its own fresh, unrooted
	 * `result`/`item` locals) - root `*result` for this whole call. `p`
	 * needs no separate root: once linked in, it's always reachable by
	 * walking cdrs from `*result`. */
	yas_gc_push_root(result);

	for (;;) {
		const char *token;
		YasAtom item = yas_nil;
		YasError e;

		e = lex(*end, &token, end, source, filename, err);
		if (e) {
			/* If lex failed (likely EOF), provide better error message */
			if (err && e == YAS_ERROR_SYNTAX) {
				set_error(err, YAS_ERROR_SYNTAX, source, start - 1, filename,
				          "Unterminated list - missing ')'");
			}
			yas_gc_pop_roots(1);
			return e;
		}

		if (token[0] == ')') {
			yas_gc_pop_roots(1);
			return YAS_ERROR_OK;
		}

		if (token[0] == '.' && *end - token == 1) {
			/* Improper list */
			if (yas_nilp(p)) {
				set_error(err, YAS_ERROR_SYNTAX, source, token, filename,
				          "Unexpected '.' at start of list");
				yas_gc_pop_roots(1);
				return YAS_ERROR_SYNTAX;
			}

			e = yas_read_expr(*end, end, &item, err);
			if (e) {
				yas_gc_pop_roots(1);
				return e;
			}

			/* yas_set_cdr(), not the raw yas_cdr() assignment macro -
			 * this store needs the write barrier like any other (see
			 * yas.h's yas_set_car/yas_set_cdr contract). */
			yas_set_cdr(p, item);

			/* Read the closing ')' */
			e = lex(*end, &token, end, source, filename, err);
			if (e) {
				set_error(err, YAS_ERROR_SYNTAX, source, *end, filename,
				          "Expected ')' after dotted pair");
				yas_gc_pop_roots(1);
				return e;
			}
			if (token[0] != ')') {
				set_error(err, YAS_ERROR_SYNTAX, source, token, filename,
				          "Only one expression allowed after '.'");
				yas_gc_pop_roots(1);
				return YAS_ERROR_SYNTAX;
			}

			yas_gc_pop_roots(1);
			return YAS_ERROR_OK;
		}

		e = yas_read_expr(token, end, &item, err);
		if (e == YAS_ERROR_SKIP) {
			/* Feature conditional excluded this expression, continue reading */
			continue;
		}
		if (e) {
			yas_gc_pop_roots(1);
			return e;
		}

		/* `item` is a fresh, otherwise-unreachable local held across
		 * the further allocating yas_cons() call right below - root it
		 * for that one call. */
		yas_gc_push_root(&item);
		if (yas_nilp(p)) {
			/* First item */
			*result = yas_cons(item, yas_nil);
			p = *result;
		} else {
			YasAtom node = yas_cons(item, yas_nil);
			yas_set_cdr(p, node);
			p = node;
		}
		yas_gc_pop_roots(1); /* item */
	}
}

/* Internal implementation with source tracking */
static int read_expr_internal(const char *input, const char **end, YasAtom *result,
                              const char *source, const char *filename, YasErrorContext *err)
{
	const char *token;
	YasError e;

	e = lex(input, &token, end, source, filename, err);
	if (e)
		return e;

	if (token[0] == '(') {
		return read_list(*end, end, result, source, filename, err);
	} else if (token[0] == ')') {
		set_error(err, YAS_ERROR_SYNTAX, source, token, filename,
		          "Unexpected ')'");
		return YAS_ERROR_SYNTAX;
	} else if (token[0] == '\'') {
		/* `*result` (the (QUOTE ...) wrapper) is a fresh, otherwise-
		 * unreachable local across the recursive read_expr_internal()
		 * call below, which allocates heavily for a nested form - root
		 * it. The recursive call writes its result through
		 * &yas_car(yas_cdr(*result)) - a raw pointer into the wrapper
		 * pair's car slot, not through yas_set_car() - so if that pair
		 * was allocated BLACK (an active cycle, see gc_alloc()) this
		 * bypasses the write barrier exactly like a raw yas_cdr()
		 * assignment would; apply it explicitly once the recursive read
		 * succeeds. */
		*result = yas_cons(yas_make_sym("QUOTE"), yas_cons(yas_nil, yas_nil));
		yas_gc_push_root(result);
		e = read_expr_internal(*end, end, &yas_car(yas_cdr(*result)), source, filename, err);
		if (!e)
			yas_gc_write_barrier(yas_cdr(*result), yas_car(yas_cdr(*result)));
		yas_gc_pop_roots(1);
		if (e && err) {
			set_error(err, YAS_ERROR_SYNTAX, source, token, filename,
			          "Expected expression after quote");
		}
		return e;
	} else if (token[0] == '`') {
		/* Same rationale as the quote branch above. */
		*result = yas_cons(yas_make_sym("QUASIQUOTE"), yas_cons(yas_nil, yas_nil));
		yas_gc_push_root(result);
		e = read_expr_internal(*end, end, &yas_car(yas_cdr(*result)), source, filename, err);
		if (!e)
			yas_gc_write_barrier(yas_cdr(*result), yas_car(yas_cdr(*result)));
		yas_gc_pop_roots(1);
		if (e && err) {
			set_error(err, YAS_ERROR_SYNTAX, source, token, filename,
			          "Expected expression after quasiquote");
		}
		return e;
	} else if (token[0] == ',') {
		/* Same rationale as the quote branch above. */
		*result = yas_cons(yas_make_sym(
			token[1] == '@' ? "UNQUOTE-SPLICING" : "UNQUOTE"),
			yas_cons(yas_nil, yas_nil));
		yas_gc_push_root(result);
		e = read_expr_internal(*end, end, &yas_car(yas_cdr(*result)), source, filename, err);
		if (!e)
			yas_gc_write_barrier(yas_cdr(*result), yas_car(yas_cdr(*result)));
		yas_gc_pop_roots(1);
		if (e && err) {
			set_error(err, YAS_ERROR_SYNTAX, source, token, filename,
			          "Expected expression after unquote");
		}
		return e;
	} else if (token[0] == '"') {
		/* String literal with interpolation support: "Hello {{name}}" */
		const char *str_start = *end;
		const char *p = str_start;
		
		/* First pass: check if there's any interpolation */
		int has_interp = 0;
		while (*p && *p != '"') {
			if (*p == '\\' && p[1]) {
				p += 2;
			} else if (p[0] == '{' && p[1] == '{') {
				has_interp = 1;
				break;
			} else {
				p++;
			}
		}
		
		if (!has_interp) {
			/* Simple string - no interpolation */
			p = str_start;
			size_t len = 0;
			while (*p && *p != '"') {
				if (*p == '\\' && p[1]) p++;
				p++;
				len++;
			}
			if (!*p) {
				set_error(err, YAS_ERROR_SYNTAX, source, token, filename,
				          "Unterminated string");
				return YAS_ERROR_SYNTAX;
			}
			
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
			*end = p + 1;

			*result = yas_make_string(buf);
			free(buf);
			return YAS_ERROR_OK;
		}
		
		/* Has interpolation - build (string-append ...) form */
		YasAtom parts = yas_nil;
		YasAtom parts_tail = yas_nil;
		/* `parts` accumulates across repeated yas_cons() calls in the
		 * loop below, same shape as read_list()'s own accumulator -
		 * root it for the duration. `parts_tail` needs no separate
		 * root once linked in (reachable by walking cdrs from `parts`),
		 * matching the same established pattern. */
		yas_gc_push_root(&parts);
		p = str_start;

		while (*p && *p != '"') {
			if (p[0] == '{' && p[1] == '{') {
				/* Interpolation - parse expression */
				const char *expr_start = p + 2;
				const char *expr_end = expr_start;
				int depth = 1;
				
				/* Find matching }} */
				while (*expr_end && depth > 0) {
					if (expr_end[0] == '{' && expr_end[1] == '{') {
						depth++;
						expr_end += 2;
					} else if (expr_end[0] == '}' && expr_end[1] == '}') {
						depth--;
						if (depth > 0) expr_end += 2;
					} else {
						expr_end++;
					}
				}
				
				if (depth != 0) {
					set_error(err, YAS_ERROR_SYNTAX, source, p, filename,
					          "Unterminated interpolation - missing }}");
					yas_gc_pop_roots(1); /* parts */
					return YAS_ERROR_SYNTAX;
				}

				/* Parse the expression */
				YasAtom expr = yas_nil;
				const char *parse_end;
				e = read_expr_internal(expr_start, &parse_end, &expr, source, filename, err);
				if (e) {
					yas_gc_pop_roots(1); /* parts */
					return e;
				}

				/* `expr`/`expr_list`/`tostr_call` are fresh, otherwise-
				 * unreachable locals held across further allocating
				 * yas_cons() calls - root each in turn. (Was a single
				 * nested expression - yas_cons(sym, yas_cons(expr,
				 * nil)) - same Class-B shape as module.c's default
				 * search-paths fix.) */
				yas_gc_push_root(&expr);
				YasAtom expr_list = yas_cons(expr, yas_nil);
				yas_gc_pop_roots(1); /* expr */
				yas_gc_push_root(&expr_list);
				/* Wrap in (->string expr) */
				YasAtom tostr_call = yas_cons(yas_make_sym("->STRING"), expr_list);
				yas_gc_pop_roots(1); /* expr_list */
				yas_gc_push_root(&tostr_call);

				/* Add to parts list */
				YasAtom new_cell = yas_cons(tostr_call, yas_nil);
				yas_gc_pop_roots(1); /* tostr_call */
				if (yas_nilp(parts)) {
					parts = new_cell;
					parts_tail = new_cell;
				} else {
					yas_set_cdr(parts_tail, new_cell);
					parts_tail = new_cell;
				}

				p = expr_end + 2;  /* Skip }} */
			} else {
				/* Literal string segment */
				const char *seg_start = p;
				size_t seg_len = 0;
				
				/* Scan until {{ or end of string */
				while (*p && *p != '"' && !(p[0] == '{' && p[1] == '{')) {
					if (*p == '\\' && p[1]) {
						p += 2;
						seg_len++;
					} else {
						p++;
						seg_len++;
					}
				}
				
				if (seg_len > 0) {
					/* Build the string segment */
					char *seg_buf = malloc(seg_len + 1);
					char *dst = seg_buf;
					const char *sp = seg_start;
					while (sp < p) {
						if (*sp == '\\') {
							sp++;
							switch (*sp) {
							case 'n': *dst++ = '\n'; break;
							case 't': *dst++ = '\t'; break;
							case 'r': *dst++ = '\r'; break;
							case '\\': *dst++ = '\\'; break;
							case '"': *dst++ = '"'; break;
							default: *dst++ = *sp; break;
							}
						} else {
							*dst++ = *sp;
						}
						sp++;
					}
					*dst = '\0';
					
					YasAtom str_atom = yas_make_string(seg_buf);
					free(seg_buf);

					/* `str_atom` held across the further allocating
					 * yas_cons() call right below - root it. */
					yas_gc_push_root(&str_atom);
					YasAtom new_cell = yas_cons(str_atom, yas_nil);
					yas_gc_pop_roots(1); /* str_atom */
					if (yas_nilp(parts)) {
						parts = new_cell;
						parts_tail = new_cell;
					} else {
						yas_set_cdr(parts_tail, new_cell);
						parts_tail = new_cell;
					}
				}
			}
		}

		if (*p != '"') {
			set_error(err, YAS_ERROR_SYNTAX, source, token, filename,
			          "Unterminated string");
			yas_gc_pop_roots(1); /* parts */
			return YAS_ERROR_SYNTAX;
		}

		*end = p + 1;  /* Skip closing quote */

		/* Build (string-append part1 part2 ...) */
		*result = yas_cons(yas_make_sym("STRING-APPEND"), parts);
		yas_gc_pop_roots(1); /* parts */
		return YAS_ERROR_OK;
	} else if (token[0] == '#' && token[1] == '(') {
		/* Vector literal #(1 2 3) */
		YasAtom list_result;
		e = read_list(*end, end, &list_result, source, filename, err);
		if (e) return e;

		/* `list_result` is a fresh, otherwise-unreachable local held
		 * across the further allocating yas_make_vector() call below -
		 * root it until every element has been copied out. */
		yas_gc_push_root(&list_result);

		/* Count list length */
		size_t len = 0;
		YasAtom p = list_result;
		while (!yas_nilp(p)) { len++; p = yas_cdr(p); }

		/* Create vector from list */
		*result = yas_make_vector(len, yas_nil);
		p = list_result;
		for (size_t i = 0; i < len; i++) {
			yas_vector_set(*result, i, yas_car(p));
			p = yas_cdr(p);
		}
		yas_gc_pop_roots(1);
		return YAS_ERROR_OK;
	} else if (token[0] == '#' && strncmp(token, "#vec(", 5) == 0) {
		/* SIMD vector literal #vec(1 2) or #vec(1 2 3) or #vec(1 2 3 4) */
		YasAtom list_result;
		e = read_list(*end, end, &list_result, source, filename, err);
		if (e) return e;
		
		/* Count list length and collect floats */
		size_t len = 0;
		YasAtom p = list_result;
		while (!yas_nilp(p)) { len++; p = yas_cdr(p); }
		
		/* Convert list elements to floats */
		float vals[4] = {0, 0, 0, 0};
		p = list_result;
		for (size_t i = 0; i < len && i < 4; i++) {
			YasAtom a = yas_car(p);
			if (a.type == YAS_ATOM_TYPE_INTEGER)
				vals[i] = (float)a.value.integer;
			else if (a.type == YAS_ATOM_TYPE_FLOAT)
				vals[i] = (float)a.value.floatval;
			else {
				set_error(err, YAS_ERROR_TYPE, source, token, filename,
				          "#vec elements must be numbers");
				return YAS_ERROR_TYPE;
			}
			p = yas_cdr(p);
		}
		
		/* Create vec based on element count */
		if (len == 2)
			*result = yas_make_vec2(vals[0], vals[1]);
		else if (len == 3)
			*result = yas_make_vec3(vals[0], vals[1], vals[2]);
		else if (len == 4)
			*result = yas_make_vec4(vals[0], vals[1], vals[2], vals[3]);
		else {
			set_error(err, YAS_ERROR_SYNTAX, source, token, filename,
			          "#vec requires 2, 3, or 4 elements");
			return YAS_ERROR_SYNTAX;
		}
		return YAS_ERROR_OK;
	} else if (token[0] == '#' && strncmp(token, "#mat(", 5) == 0) {
		/* mat4 literal #mat(m00 m01 ... m33) - 16 column-major floats */
		YasAtom list_result;
		e = read_list(*end, end, &list_result, source, filename, err);
		if (e) return e;
		
		/* Count list length */
		size_t len = 0;
		YasAtom p = list_result;
		while (!yas_nilp(p)) { len++; p = yas_cdr(p); }
		
		if (len != 16) {
			set_error(err, YAS_ERROR_SYNTAX, source, token, filename,
			          "#mat requires exactly 16 elements");
			return YAS_ERROR_SYNTAX;
		}
		
		/* Convert elements to floats */
		float vals[16];
		p = list_result;
		for (size_t i = 0; i < 16; i++) {
			YasAtom a = yas_car(p);
			if (a.type == YAS_ATOM_TYPE_INTEGER)
				vals[i] = (float)a.value.integer;
			else if (a.type == YAS_ATOM_TYPE_FLOAT)
				vals[i] = (float)a.value.floatval;
			else {
				set_error(err, YAS_ERROR_TYPE, source, token, filename,
				          "#mat elements must be numbers");
				return YAS_ERROR_TYPE;
			}
			p = yas_cdr(p);
		}
		
		*result = yas_make_mat4_from_floats(vals);
		return YAS_ERROR_OK;
	} else if (token[0] == '#' && strncmp(token, "#hash(", 6) == 0) {
		/* Hash literal #hash(:key val ...) or #hash(sym val ...) */
		YasAtom list_result;
		e = read_list(*end, end, &list_result, source, filename, err);
		if (e) return e;

		/* `list_result` is a fresh, otherwise-unreachable local held
		 * across the further allocating yas_make_hash()/yas_make_sym()
		 * calls below - root it until the loop is done walking it.
		 * (`*result` itself needs no extra root: writing through it
		 * updates whatever slot our own caller already rooted.) */
		yas_gc_push_root(&list_result);
		*result = yas_make_hash();
		YasAtom p = list_result;
		while (!yas_nilp(p) && !yas_nilp(yas_cdr(p))) {
			YasAtom key = yas_car(p);
			YasAtom val = yas_car(yas_cdr(p));
			/* Convert keywords to symbols for hash keys */
			if (key.type == YAS_ATOM_TYPE_KEYWORD) {
				key = yas_make_sym(key.value.keyword);
			}
			yas_hash_set(*result, key, val);
			p = yas_cdr(yas_cdr(p));
		}
		yas_gc_pop_roots(1);
		return YAS_ERROR_OK;
	} else if (token[0] == '#' && (token[1] == '+' || token[1] == '-')) {
		/* Reader conditionals: #+(feature expr) or #-(feature expr) */
		int include_if_present = (token[1] == '+');
		
		/* Read the feature name (symbol, or a compound list expression -
		 * see below) */
		YasAtom feature = yas_nil;
		e = read_expr_internal(*end, end, &feature, source, filename, err);
		if (e) return e;

		/* `feature` can be a compound list expression ("(or macos
		 * linux)"), not just a bare symbol - root it across the further
		 * allocating read_expr_internal() call below. */
		yas_gc_push_root(&feature);

		/* Check if feature is present - supports compound expressions like (or macos linux) */
		int feature_present = yas_eval_feature_expr(feature);

		/* Decide whether to include the following expression */
		int include_expr = (include_if_present && feature_present) ||
		                   (!include_if_present && !feature_present);

		/* Read the expression (we must consume it either way) */
		YasAtom expr = yas_nil;
		e = read_expr_internal(*end, end, &expr, source, filename, err);
		yas_gc_pop_roots(1); /* feature */
		if (e) return e;

		if (include_expr) {
			*result = expr;
			return YAS_ERROR_OK;
		} else {
			/* Signal to caller that this expression should be skipped */
			return YAS_ERROR_SKIP;
		}
	} else if (token[0] == '#' && token[1] == '\\') {
		/* Character literal */
		const char *start = token + 2;
		size_t len = *end - start;
		int32_t cp;
		
		if (len == 0) {
			set_error(err, YAS_ERROR_SYNTAX, source, token, filename,
			          "Empty character literal");
			return YAS_ERROR_SYNTAX;
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
				set_error(err, YAS_ERROR_SYNTAX, source, token, filename,
				          "Invalid hex character literal");
				return YAS_ERROR_SYNTAX;
			}
		} else if (start[0] == 'u' && len == 5) {
			/* Unicode escape #\uNNNN */
			char *endp;
			cp = strtol(start + 1, &endp, 16);
			if (endp != *end) {
				set_error(err, YAS_ERROR_SYNTAX, source, token, filename,
				          "Invalid unicode character literal");
				return YAS_ERROR_SYNTAX;
			}
		} else if (start[0] == 'U' && len == 9) {
			/* Unicode escape #\UNNNNNNNN */
			char *endp;
			cp = strtol(start + 1, &endp, 16);
			if (endp != *end) {
				set_error(err, YAS_ERROR_SYNTAX, source, token, filename,
				          "Invalid unicode character literal");
				return YAS_ERROR_SYNTAX;
			}
		} else if (len == 1) {
			/* Single character */
			cp = (unsigned char)start[0];
		} else {
			/* Try UTF-8 decode for multi-byte chars like #\λ */
			int n = yas_utf8_decode(start, &cp);
			if (n != (int)len) {
				set_error(err, YAS_ERROR_SYNTAX, source, token, filename,
				          "Unknown character name");
				return YAS_ERROR_SYNTAX;
			}
		}
		
		*result = yas_make_char(cp);
		return YAS_ERROR_OK;
	} else {
		return parse_simple(token, *end, result, source, filename, err);
	}
}

/* Public interface - source is the input itself */
int yas_read_expr(const char *input, const char **end, YasAtom *result, YasErrorContext *err)
{
	const char *filename = err && err->pos.filename ? err->pos.filename : "<stdin>";
	const char *source = err && err->pos.source ? err->pos.source : input;
	return read_expr_internal(input, end, result, source, filename, err);
}

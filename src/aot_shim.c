/* aot_shim.c - see aot_shim.h for the why. */
#include "aot_shim.h"
#include "lisp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct YasForm {
	Atom atom;
};

struct YasForms {
	struct YasForm **items;
	int count;
	int capacity;
	char *error; /* NULL on success */
};

static struct YasForm *box(Atom a)
{
	struct YasForm *f = malloc(sizeof(struct YasForm));
	f->atom = a;
	return f;
}

static void push(struct YasForms *forms, Atom a)
{
	if (forms->count == forms->capacity) {
		forms->capacity = forms->capacity ? forms->capacity * 2 : 16;
		forms->items = realloc(forms->items, sizeof(struct YasForm *) * forms->capacity);
	}
	forms->items[forms->count++] = box(a);
}

struct YasForms *yas_aot_read_file(const char *path)
{
	struct YasForms *forms = calloc(1, sizeof(struct YasForms));

	char *text = slurp(path);
	if (!text) {
		forms->error = malloc(256);
		snprintf(forms->error, 256, "could not read file: %s", path);
		return forms;
	}

	const char *p = text;
	ErrorContext err = {0};
	err.pos.filename = path;
	err.pos.source = text;

	while (1) {
		err.code = Error_OK;
		err.message[0] = '\0';

		Atom expr;
		Error read_err = read_expr(p, &p, &expr, &err);
		if (read_err == Error_Skip) {
			/* Reader conditional excluded this form (#+feature / #-feature).
			 * Unlike eval_source(), we must not treat this as EOF. */
			continue;
		}
		if (read_err) {
			if (err.message[0] != '\0') {
				forms->error = malloc(ERROR_MSG_MAX + 64);
				snprintf(forms->error, ERROR_MSG_MAX + 64, "%s:%d:%d: %s",
				         path, err.pos.line, err.pos.column, err.message);
			}
			break;
		}

		push(forms, expr);
	}

	free(text);
	return forms;
}

const char *yas_aot_error(struct YasForms *forms)
{
	return forms->error;
}

int yas_aot_count(struct YasForms *forms)
{
	return forms->count;
}

struct YasForm *yas_aot_at(struct YasForms *forms, int index)
{
	if (index < 0 || index >= forms->count) return NULL;
	return forms->items[index];
}

void yas_aot_free(struct YasForms *forms)
{
	if (!forms) return;
	for (int i = 0; i < forms->count; i++) free(forms->items[i]);
	free(forms->items);
	free(forms->error);
	free(forms);
}

YasFormKind yas_aot_kind(struct YasForm *form)
{
	if (!form) return YAS_FORM_NIL;
	switch (form->atom.type) {
	case AtomType_Nil:     return YAS_FORM_NIL;
	case AtomType_Pair:    return YAS_FORM_PAIR;
	case AtomType_Symbol:  return YAS_FORM_SYMBOL;
	case AtomType_Integer: return YAS_FORM_INTEGER;
	case AtomType_Float:   return YAS_FORM_FLOAT;
	case AtomType_String:  return YAS_FORM_STRING;
	case AtomType_Char:    return YAS_FORM_CHAR;
	case AtomType_Keyword: return YAS_FORM_KEYWORD;
	default:               return YAS_FORM_OTHER;
	}
}

struct YasForm *yas_aot_car(struct YasForm *form)
{
	if (!form || form->atom.type != AtomType_Pair) return NULL;
	return box(car(form->atom));
}

struct YasForm *yas_aot_cdr(struct YasForm *form)
{
	if (!form || form->atom.type != AtomType_Pair) return NULL;
	return box(cdr(form->atom));
}

long yas_aot_int(struct YasForm *form)
{
	return form && form->atom.type == AtomType_Integer ? form->atom.value.integer : 0;
}

double yas_aot_float(struct YasForm *form)
{
	return form && form->atom.type == AtomType_Float ? form->atom.value.floatval : 0.0;
}

const char *yas_aot_sym(struct YasForm *form)
{
	return form && form->atom.type == AtomType_Symbol ? form->atom.value.symbol : NULL;
}

const char *yas_aot_string(struct YasForm *form)
{
	return form && form->atom.type == AtomType_String ? form->atom.value.string : NULL;
}

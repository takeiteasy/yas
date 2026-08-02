/* aot_shim.h
 *
 * Pointer-only front end onto the yas reader, built for cccc's comptime
 * pass. `struct Atom` (lisp.h) is a 16-byte union passed by value; pushing
 * that through cccc's comptime FFI is unnecessary risk, so this shim boxes
 * every Atom on the heap and only ever exchanges pointers, `long`, `double`,
 * `int` and `const char *` across the boundary.
 *
 * This header intentionally does NOT include lisp.h: lisp.h pulls in
 * platform SIMD intrinsic headers (xmmintrin.h / arm_neon.h / ...) which
 * have no business anywhere near a comptime parse. YasFormKind below is a
 * stable subset of AtomType, independent of lisp.h's enum ordering and of
 * whether YAS_FFI is defined.
 *
 * Built into both `yas` and `libyas` (see Makefile). Comptime code dlopens
 * libyas and calls these entry points through dlsym.
 *
 * Note: opaque handles are bare `struct YasForms *` / `struct YasForm *`,
 * not typedef'd (no `typedef struct X X;`). cccc's comptime pass
 * special-cases that idiom and reinjects it as `typedef struct AttrTarget
 * X;` when a header using it is @shared into the runtime TU, which then
 * conflicts with the real definition here. Plain forward declarations avoid
 * it - see docs/AOT.md for the write-up.
 */
#ifndef YAS_AOT_SHIM_H
#define YAS_AOT_SHIM_H

#ifdef __cplusplus
extern "C" {
#endif

struct YasForms; /* owns an arena of boxed forms read from one file */
struct YasForm;  /* one boxed Atom; only valid while its YasForms is alive */

typedef enum {
	YAS_FORM_NIL,
	YAS_FORM_PAIR,
	YAS_FORM_SYMBOL,
	YAS_FORM_INTEGER,
	YAS_FORM_FLOAT,
	YAS_FORM_STRING,
	YAS_FORM_CHAR,
	YAS_FORM_KEYWORD,
	YAS_FORM_OTHER /* vectors/hashes/etc: valid as a literal reader form but
	                * not meaningful to the AOT lowerer */
} YasFormKind;

/* Read every top-level form from `path`. Returns NULL only on failure to
 * open the file; a read syntax error is reported via yas_aot_error()
 * instead, matching main.c's load_file() loop (including its handling of
 * Error_Skip for reader-conditional-excluded forms, which eval_source()
 * gets wrong - see docs/ARCHITECTURE.md). */
struct YasForms *yas_aot_read_file(const char *path);

/* NULL if reading succeeded; a human-readable message otherwise. */
const char *yas_aot_error(struct YasForms *forms);

int             yas_aot_count(struct YasForms *forms);
struct YasForm *yas_aot_at(struct YasForms *forms, int index);
void            yas_aot_free(struct YasForms *forms);

YasFormKind     yas_aot_kind(struct YasForm *form);
struct YasForm *yas_aot_car(struct YasForm *form);
struct YasForm *yas_aot_cdr(struct YasForm *form);
long            yas_aot_int(struct YasForm *form);
double          yas_aot_float(struct YasForm *form);
/* Interned, uppercase (the reader upcases all symbols - see read.c). Valid
 * for the process lifetime; do not free. */
const char *yas_aot_sym(struct YasForm *form);
const char *yas_aot_string(struct YasForm *form);

#ifdef __cplusplus
}
#endif

#endif /* YAS_AOT_SHIM_H */

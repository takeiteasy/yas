#include <stddef.h>

typedef enum {
	Error_OK = 0,
	Error_Syntax,
	Error_Unbound,
	Error_Args,
	Error_Type,
	Error_Exception
} Error;

/* Reader position tracking */
typedef struct {
	const char *filename;     /* Source filename (or "<stdin>") */
	const char *source;       /* Original source string start */
	int line;                 /* Current line (1-indexed) */
	int column;               /* Current column (1-indexed) */
} ReaderPos;

/* Error context for detailed messages */
#define ERROR_MSG_MAX 256

typedef struct {
	Error code;
	ReaderPos pos;
	char message[ERROR_MSG_MAX];
} ErrorContext;

struct Atom;
struct Vector;
struct Hash;

typedef int (*Builtin)(struct Atom args, struct Atom *result);

struct Atom {
	enum {
		AtomType_Nil,
		AtomType_Pair,
		AtomType_Symbol,
		AtomType_Integer,
		AtomType_Float,
		AtomType_String,
		AtomType_Vector,
		AtomType_Hash,
		AtomType_Builtin,
		AtomType_Closure,
		AtomType_Macro,
		AtomType_Continuation
	} type;

	union {
		struct Pair *pair;
		const char *symbol;
		long integer;
		double floatval;
		char *string;
		struct Vector *vector;
		struct Hash *hash;
		Builtin builtin;
	} value;
};

struct Pair {
	struct Atom atom[2];
};

struct Vector {
	struct Atom *data;
	size_t size;
	size_t capacity;
};

struct Hash {
	struct Atom *keys;
	struct Atom *values;
	size_t size;
	size_t capacity;
};

typedef struct Atom Atom;

#define car(p) ((p).value.pair->atom[0])
#define cdr(p) ((p).value.pair->atom[1])
#define nilp(atom) ((atom).type == AtomType_Nil)

static const Atom nil = { AtomType_Nil };

/* READER */

int read_expr(const char *input, const char **end, Atom *result, ErrorContext *err);
void error_print(ErrorContext *err);


/* PRINTER */

void print_expr(Atom atom);

/* Exceptions */
extern Atom ctx_exception;

int builtin_throw(Atom args, Atom *result);
int builtin_error(Atom args, Atom *result);

int raise_error(const char *type, const char *msg);

/* EVALUATOR */

Atom env_create(Atom parent);
int env_define(Atom env, Atom symbol, Atom value);
int env_get(Atom env, Atom symbol, Atom *result);
int env_set(Atom env, Atom symbol, Atom value);
int eval_expr(Atom expr, Atom env, Atom *result);

/* DATA */

Atom cons(Atom car_val, Atom cdr_val);
Atom make_int(long x);
Atom make_float(double x);
Atom make_string(const char *s);
Atom make_string_n(const char *s, size_t len);
Atom make_vector(size_t size, Atom fill);
Atom make_hash(void);
Atom make_sym(const char *s);
Atom make_builtin(Builtin fn);
int listp(Atom expr);
Atom copy_list(Atom list);
Atom list_create(int n, ...);
Atom list_get(Atom list, int k);
void list_set(Atom list, int k, Atom value);
void list_reverse(Atom *list);
void gc_mark(Atom root);
void gc();

/* Vector operations */
Atom vector_ref(Atom vec, size_t index);
void vector_set(Atom vec, size_t index, Atom value);
size_t vector_length(Atom vec);

/* Hash operations */
Atom hash_ref(Atom hash, Atom key, Atom default_val);
void hash_set(Atom hash, Atom key, Atom value);
int hash_remove(Atom hash, Atom key);
size_t hash_size(Atom hash);

/* Metatable operations */
Atom get_metamethod(Atom hash, const char *name);
int has_metamethod(Atom hash, const char *name);

/* BUILTINS */

int builtin_car(Atom args, Atom *result);
int builtin_cdr(Atom args, Atom *result);
int builtin_cons(Atom args, Atom *result);
int builtin_eq(Atom args, Atom *result);
int builtin_pairp(Atom args, Atom *result);
int builtin_procp(Atom args, Atom *result);
int builtin_add(Atom args, Atom *result);
int builtin_subtract(Atom args, Atom *result);
int builtin_multiply(Atom args, Atom *result);
int builtin_divide(Atom args, Atom *result);
int builtin_numeq(Atom args, Atom *result);
int builtin_less(Atom args, Atom *result);

/* New type builtins */
int builtin_floatp(Atom args, Atom *result);
int builtin_stringp(Atom args, Atom *result);
int builtin_vectorp(Atom args, Atom *result);
int builtin_hashp(Atom args, Atom *result);
int builtin_length(Atom args, Atom *result);
int builtin_concat(Atom args, Atom *result);
int builtin_make_vector(Atom args, Atom *result);
int builtin_vector_ref(Atom args, Atom *result);
int builtin_vector_set(Atom args, Atom *result);
int builtin_make_hash(Atom args, Atom *result);
int builtin_hash_ref(Atom args, Atom *result);
int builtin_hash_set(Atom args, Atom *result);
int builtin_hash_remove(Atom args, Atom *result);

/* Metatable builtins */
int builtin_set_metatable(Atom args, Atom *result);
int builtin_get_metatable(Atom args, Atom *result);

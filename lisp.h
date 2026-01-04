#include <stddef.h>
#include <stdint.h>

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
struct Port;
struct Bytevector;

/* Platform detection */
#if defined(__wasm__) && defined(__wasm_simd128__)
#define SIMD_WASM
#include <wasm_simd128.h>
#elif defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#define SIMD_X86
#include <xmmintrin.h>  /* SSE */
#elif defined(__ARM_NEON) || defined(__aarch64__)
#define SIMD_ARM_NEON
#include <arm_neon.h>
#else
#define SIMD_SCALAR
#endif

typedef struct {
#ifdef SIMD_X86
    __m128 v;
#elif defined(SIMD_ARM_NEON)
    float32x4_t v;
#elif defined(SIMD_WASM)
    v128_t v;
#else
    float data[4];
#endif
} vec4_t;

typedef vec4_t vec2_t;
typedef vec4_t vec3_t;

typedef struct {
	vec4_t m;
} mat2_t;

typedef struct {
	vec4_t columns[3];
} mat3_t;

typedef struct {
	vec4_t columns[4];
} mat4_t;

/* Inline SIMD operations */
static inline vec4_t vec4_splat(float val) {
    vec4_t result;
#ifdef SIMD_X86
    result.v = _mm_set1_ps(val);
#elif defined(SIMD_ARM_NEON)
    result.v = vdupq_n_f32(val);
#elif defined(SIMD_WASM)
    result.v = wasm_f32x4_splat(val);
#else
    result.data[0] = result.data[1] = result.data[2] = result.data[3] = val;
#endif
    return result;
}

static inline void vec4_store(float* ptr, vec4_t v) {
#ifdef SIMD_X86
    _mm_storeu_ps(ptr, v.v);
#elif defined(SIMD_ARM_NEON)
    vst1q_f32(ptr, v.v);
#elif defined(SIMD_WASM)
    wasm_v128_store(ptr, v.v);
#else
    ptr[0] = v.data[0]; ptr[1] = v.data[1];
    ptr[2] = v.data[2]; ptr[3] = v.data[3];
#endif
}

static inline vec4_t vec4_add(vec4_t a, vec4_t b) {
    vec4_t result;
#ifdef SIMD_X86
    result.v = _mm_add_ps(a.v, b.v);
#elif defined(SIMD_ARM_NEON)
    result.v = vaddq_f32(a.v, b.v);
#elif defined(SIMD_WASM)
    result.v = wasm_f32x4_add(a.v, b.v);
#else
    for (int i = 0; i < 4; i++) result.data[i] = a.data[i] + b.data[i];
#endif
    return result;
}

static inline vec4_t vec4_sub(vec4_t a, vec4_t b) {
    vec4_t result;
#ifdef SIMD_X86
    result.v = _mm_sub_ps(a.v, b.v);
#elif defined(SIMD_ARM_NEON)
    result.v = vsubq_f32(a.v, b.v);
#elif defined(SIMD_WASM)
    result.v = wasm_f32x4_sub(a.v, b.v);
#else
    for (int i = 0; i < 4; i++) result.data[i] = a.data[i] - b.data[i];
#endif
    return result;
}

static inline vec4_t vec4_mul(vec4_t a, vec4_t b) {
    vec4_t result;
#ifdef SIMD_X86
    result.v = _mm_mul_ps(a.v, b.v);
#elif defined(SIMD_ARM_NEON)
    result.v = vmulq_f32(a.v, b.v);
#elif defined(SIMD_WASM)
    result.v = wasm_f32x4_mul(a.v, b.v);
#else
    for (int i = 0; i < 4; i++) result.data[i] = a.data[i] * b.data[i];
#endif
    return result;
}

static inline vec4_t vec4_div(vec4_t a, vec4_t b) {
    vec4_t result;
#ifdef SIMD_X86
    result.v = _mm_div_ps(a.v, b.v);
#elif defined(SIMD_ARM_NEON)
    result.v = vdivq_f32(a.v, b.v);
#elif defined(SIMD_WASM)
    result.v = wasm_f32x4_div(a.v, b.v);
#else
    for (int i = 0; i < 4; i++) result.data[i] = a.data[i] / b.data[i];
#endif
    return result;
}

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
		AtomType_Port,
		AtomType_Bytevector,
		AtomType_Builtin,
		AtomType_Closure,
		AtomType_Macro,
		AtomType_Continuation,
		AtomType_Char,
		AtomType_Keyword,
		AtomType_Vec2,
		AtomType_Vec3,
		AtomType_Vec4,
		AtomType_Mat4
	} type;

	union {
		struct Pair *pair;
		const char *symbol;
		long integer;
		double floatval;
		char *string;
		struct Vector *vector;
		struct Hash *hash;
		struct Port *port;
		struct Bytevector *bytevector;
		Builtin builtin;
		int32_t character;  /* Unicode codepoint */
		const char *keyword;  /* Interned keyword name (without :) */
		vec4_t *vec;         /* Used for vec2, vec3, vec4 (SIMD aligned) */
		mat4_t *mat4;        /* 4x4 matrix */
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
	struct Hash *metatable;  /* Metatable for operator overloading */
};

struct Port {
	void *handle;           /* FILE* or custom provider handle */
	char *path;
	int is_input;
	int is_binary;
	int is_open;
	int provider_id;        /* 0 = stdio, >0 = custom provider */
};

struct Bytevector {
	unsigned char *data;
	size_t size;
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
Atom make_char(int32_t codepoint);
Atom make_string(const char *s);
Atom make_string_n(const char *s, size_t len);
Atom make_vector(size_t size, Atom fill);
Atom make_hash(void);
Atom make_sym(const char *s);
Atom make_keyword(const char *s);
Atom make_builtin(Builtin fn);
Atom make_vec2(float x, float y);
Atom make_vec3(float x, float y, float z);
Atom make_vec4(float x, float y, float z, float w);
Atom make_mat4_identity(void);
Atom make_mat4_from_floats(const float *values);
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
int builtin_integerp(Atom args, Atom *result);
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

/* SIMD Vector/Matrix builtins */
int builtin_vec2p(Atom args, Atom *result);
int builtin_vec3p(Atom args, Atom *result);
int builtin_vec4p(Atom args, Atom *result);
int builtin_mat4p(Atom args, Atom *result);
int builtin_vec2(Atom args, Atom *result);
int builtin_vec3(Atom args, Atom *result);
int builtin_vec4(Atom args, Atom *result);
int builtin_mat4_identity(Atom args, Atom *result);
int builtin_vec_x(Atom args, Atom *result);
int builtin_vec_y(Atom args, Atom *result);
int builtin_vec_z(Atom args, Atom *result);
int builtin_vec_w(Atom args, Atom *result);
int builtin_vec_ref(Atom args, Atom *result);
int builtin_vec_set(Atom args, Atom *result);
int builtin_mat4_multiply(Atom args, Atom *result);
int builtin_mat4_transpose(Atom args, Atom *result);
int builtin_mat4_inverse(Atom args, Atom *result);

/* Output builtins */
int builtin_print(Atom args, Atom *result);

/* File I/O */
char *slurp(const char *path);

/* Include tracking for relative paths */
extern const char *current_file_path;

/* Module system */
extern Atom library_registry;
void module_init(void);
Atom library_load(Atom name_parts);
char *library_name_to_string(Atom name_parts);

/* Module special forms */
int module_define_library(Atom args, Atom *result);
int module_import(Atom args, Atom env, Atom *result); /* File loading */
int eval_file(const char *path, Atom env, Atom *result, int case_insensitive);
int eval_source(const char *text, const char *path, Atom env, Atom *result);
void load_file(Atom env, const char *path);

/* ============================================================
 * I/O SANDBOX SYSTEM
 * ============================================================ */

/* Sandbox modes */
typedef enum {
	IO_SANDBOX_UNRESTRICTED,  /* Default: no restrictions */
	IO_SANDBOX_RESTRICTED,    /* Allow/deny lists apply; default_allow controls unmatched */
	IO_SANDBOX_DISABLED       /* All I/O operations fail */
} IOSandboxMode;

/* Virtual filesystem provider callbacks */
typedef struct {
	const char *prefix;       /* e.g., "asset://" or "zip://" */
	void *userdata;
	/* Open file, return handle (NULL on failure) */
	void *(*open)(const char *path, int is_input, int is_binary, void *userdata);
	/* Close file */
	void (*close)(void *handle, void *userdata);
	/* Read up to n bytes, return bytes read (-1 on error) */
	long (*read)(void *handle, void *buf, size_t n, void *userdata);
	/* Write n bytes, return bytes written (-1 on error) */
	long (*write)(void *handle, const void *buf, size_t n, void *userdata);
	/* Check if file exists (0 = no, 1 = yes) */
	int (*exists)(const char *path, void *userdata);
} IOProvider;

/* Sandbox C API (for embedding) */
void io_sandbox_init(IOSandboxMode mode);
IOSandboxMode io_sandbox_get_mode(void);
void io_sandbox_allow_path(const char *pattern);
void io_sandbox_deny_path(const char *pattern);
void io_sandbox_set_default(int allow);  /* Set default for unmatched paths in RESTRICTED mode */
int  io_sandbox_get_default(void);
void io_sandbox_clear(void);
int  io_sandbox_check_path(const char *path, int writing);

/* Provider system (for virtual filesystems) */
int  io_register_provider(IOProvider *provider);
void io_unregister_provider(const char *prefix);

/* Port operations */
Atom make_port(void *handle, const char *path, int is_input, int is_binary, int provider_id);
void port_close(Atom port);
Atom io_open_file(const char *path, int is_input, int is_binary);
long port_read(struct Port *p, void *buf, size_t n);
long port_write(struct Port *p, const void *buf, size_t n);
int  port_getc(struct Port *p);
void port_flush(struct Port *p);
int  io_file_exists(const char *path);

/* Bytevector operations */
Atom make_bytevector(size_t size);
Atom make_bytevector_from_data(const unsigned char *data, size_t size);
unsigned char bytevector_ref(Atom bv, size_t index);
void bytevector_set(Atom bv, size_t index, unsigned char value);
size_t bytevector_length(Atom bv);

/* Port builtins */
int builtin_open_input_file(Atom args, Atom *result);
int builtin_open_output_file(Atom args, Atom *result);
int builtin_close_port(Atom args, Atom *result);
int builtin_portp(Atom args, Atom *result);
int builtin_input_portp(Atom args, Atom *result);
int builtin_output_portp(Atom args, Atom *result);
int builtin_port_openp(Atom args, Atom *result);
int builtin_read_char(Atom args, Atom *result);
int builtin_read_line(Atom args, Atom *result);
int builtin_read_all(Atom args, Atom *result);
int builtin_read_byte(Atom args, Atom *result);
int builtin_read_bytes(Atom args, Atom *result);
int builtin_write_char(Atom args, Atom *result);
int builtin_write_string(Atom args, Atom *result);
int builtin_write_byte(Atom args, Atom *result);
int builtin_write_bytes(Atom args, Atom *result);
int builtin_newline(Atom args, Atom *result);
int builtin_flush_port(Atom args, Atom *result);
int builtin_file_exists(Atom args, Atom *result);

/* Bytevector builtins */
int builtin_make_bytevector(Atom args, Atom *result);
int builtin_bytevectorp(Atom args, Atom *result);
int builtin_bytevector_length(Atom args, Atom *result);
int builtin_bytevector_u8_ref(Atom args, Atom *result);
int builtin_bytevector_u8_set(Atom args, Atom *result);
int builtin_bytevector_to_string(Atom args, Atom *result);
int builtin_string_to_bytevector(Atom args, Atom *result);

/* Sandbox scheme builtins */
int builtin_io_sandbox_allow(Atom args, Atom *result);
int builtin_io_sandbox_deny(Atom args, Atom *result);
int builtin_io_sandbox_mode(Atom args, Atom *result);

/* Character builtins */
int builtin_charp(Atom args, Atom *result);
int builtin_char_to_integer(Atom args, Atom *result);
int builtin_integer_to_char(Atom args, Atom *result);
int builtin_char_eq(Atom args, Atom *result);
int builtin_char_lt(Atom args, Atom *result);

/* String/Bytes encoding builtins */
int builtin_string_to_bytes(Atom args, Atom *result);
int builtin_bytes_to_string(Atom args, Atom *result);
int builtin_string_ref(Atom args, Atom *result);
int builtin_string_eq(Atom args, Atom *result);
int builtin_string_byte_length(Atom args, Atom *result);

/* Keyword builtins */
int builtin_keywordp(Atom args, Atom *result);
int builtin_keyword_to_string(Atom args, Atom *result);
int builtin_string_to_keyword(Atom args, Atom *result);

/* Procedure introspection */
int builtin_procedure_doc(Atom args, Atom *result);

/* Decode a single UTF-8 character, return bytes consumed (0 on end/error) */
int utf8_decode(const char *s, int32_t *cp);
/* Encode a codepoint to UTF-8, return bytes written (1-4) */
int utf8_encode(int32_t cp, char *out);
/* Count codepoints in UTF-8 string */
size_t utf8_strlen(const char *s);
/* Get codepoint at index, returns -1 if out of bounds */
int32_t utf8_codepoint_at(const char *s, size_t index);
/* Get pointer to codepoint at index, returns NULL if out of bounds */
const char *utf8_index_ptr(const char *s, size_t index);

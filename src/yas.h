#ifndef YAS_H
#define YAS_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
	YAS_ERROR_OK = 0,
	YAS_ERROR_SYNTAX,
	YAS_ERROR_UNBOUND,
	YAS_ERROR_ARGS,
	YAS_ERROR_TYPE,
	YAS_ERROR_EXCEPTION,
	YAS_ERROR_SKIP  /* Used internally by reader to skip feature-excluded expressions */
} YasError;

/* Reader position tracking */
typedef struct {
	const char *filename;     /* Source filename (or "<stdin>") */
	const char *source;       /* Original source string start */
	int line;                 /* Current line (1-indexed) */
	int column;               /* Current column (1-indexed) */
} YasReaderPos;

/* Error context for detailed messages */
#define YAS_ERROR_MSG_MAX 256

typedef struct {
	YasError code;
	YasReaderPos pos;
	char message[YAS_ERROR_MSG_MAX];
} YasErrorContext;

struct YasAtom;
struct YasVector;
struct YasHash;
struct YasPort;
struct YasBytevector;

/* Platform detection */
#if defined(__wasm__) && defined(__wasm_simd128__)
#define YAS_SIMD_WASM
#include <wasm_simd128.h>
#elif defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#define YAS_SIMD_X86
#include <xmmintrin.h>  /* SSE */
#elif defined(__ARM_NEON) || defined(__aarch64__)
#define YAS_SIMD_ARM_NEON
#include <arm_neon.h>
#else
#define YAS_SIMD_SCALAR
#endif

typedef struct {
#ifdef YAS_SIMD_X86
    __m128 v;
#elif defined(YAS_SIMD_ARM_NEON)
    float32x4_t v;
#elif defined(YAS_SIMD_WASM)
    v128_t v;
#else
    float data[4];
#endif
} yas_vec4_t;

typedef yas_vec4_t yas_vec2_t;
typedef yas_vec4_t yas_vec3_t;

typedef struct {
	yas_vec4_t m;
} yas_mat2_t;

typedef struct {
	yas_vec4_t columns[3];
} yas_mat3_t;

typedef struct {
	yas_vec4_t columns[4];
} yas_mat4_t;

typedef int (*YasBuiltin)(struct YasAtom args, struct YasAtom *result);

struct YasAtom {
	enum {
		YAS_ATOM_TYPE_NIL,
		YAS_ATOM_TYPE_PAIR,
		YAS_ATOM_TYPE_SYMBOL,
		YAS_ATOM_TYPE_INTEGER,
		YAS_ATOM_TYPE_FLOAT,
		YAS_ATOM_TYPE_STRING,
		YAS_ATOM_TYPE_VECTOR,
		YAS_ATOM_TYPE_HASH,
		YAS_ATOM_TYPE_PORT,
		YAS_ATOM_TYPE_BYTEVECTOR,
		YAS_ATOM_TYPE_BUILTIN,
		YAS_ATOM_TYPE_CLOSURE,
		YAS_ATOM_TYPE_MACRO,
		YAS_ATOM_TYPE_CONTINUATION,
		YAS_ATOM_TYPE_CHAR,
		YAS_ATOM_TYPE_KEYWORD,
		YAS_ATOM_TYPE_VEC2,
		YAS_ATOM_TYPE_VEC3,
		YAS_ATOM_TYPE_VEC4,
		YAS_ATOM_TYPE_MAT4,
#ifdef YAS_FFI
		YAS_ATOM_TYPE_FFI_LIBRARY,   /* dlopen handle */
		YAS_ATOM_TYPE_FFI_POINTER,   /* Raw C pointer */
		YAS_ATOM_TYPE_FFI_FUNCTION,  /* Prepared ffi function */
		YAS_ATOM_TYPE_FFI_STRUCT     /* Struct instance (packed bytes) */
#endif
	} type;

	union {
		struct YasPair *pair;
		const char *symbol;
		long integer;
		double floatval;
		char *string;
		struct YasVector *vector;
		struct YasHash *hash;
		struct YasPort *port;
		struct YasBytevector *bytevector;
		YasBuiltin builtin;
		int32_t character;  /* Unicode codepoint */
		const char *keyword;  /* Interned keyword name (without :) */
		yas_vec4_t *vec;         /* Used for vec2, vec3, vec4 (SIMD aligned) */
		yas_mat4_t *mat4;        /* 4x4 matrix */
#ifdef YAS_FFI
		void *ffi_library;           /* dlopen handle */
		void *ffi_pointer;           /* raw C pointer */
		struct YasFFIFunction *ffi_fn;  /* prepared ffi function */
		struct YasFFIStructInstance *ffi_struct; /* struct instance */
#endif
	} value;
};

struct YasPair {
	struct YasAtom atom[2];
};

struct YasVector {
	struct YasAtom *data;
	size_t size;
	size_t capacity;
	int mark;
	struct YasVector *next;
};

struct YasHash {
	struct YasAtom *keys;
	struct YasAtom *values;
	size_t size;
	size_t capacity;
	struct YasHash *metatable;  /* Metatable for operator overloading */
	int mark;
	struct YasHash *next;
};

struct YasPort {
	void *handle;           /* FILE* or custom provider handle */
	char *path;
	int is_input;
	int is_binary;
	int is_open;
	int provider_id;        /* 0 = stdio, >0 = custom provider */
	int mark;
	struct YasPort *next;
};

struct YasBytevector {
	unsigned char *data;
	size_t size;
	int mark;
	struct YasBytevector *next;
};

/* GC headers for otherwise-bare-pointer atom payloads (String, Vec2/3/4,
 * Mat4). The atom stores a pointer into the header's trailing/embedded
 * data; gc_mark() recovers the header via offsetof, mirroring how `cons`
 * recovers `struct Allocation` from a `struct Pair *`. */
struct YasStringAlloc {
	int mark;
	struct YasStringAlloc *next;
	char data[];
};

struct YasVecAlloc {
	int mark;
	struct YasVecAlloc *next;
	yas_vec4_t v;
};

struct YasMat4Alloc {
	int mark;
	struct YasMat4Alloc *next;
	yas_mat4_t m;
};

typedef struct YasAtom YasAtom;

#define yas_car(p) ((p).value.pair->atom[0])
#define yas_cdr(p) ((p).value.pair->atom[1])
#define yas_nilp(atom) ((atom).type == YAS_ATOM_TYPE_NIL)

static const YasAtom yas_nil = { YAS_ATOM_TYPE_NIL };

/* READER */

int yas_read_expr(const char *input, const char **end, YasAtom *result, YasErrorContext *err);
void yas_error_print(YasErrorContext *err);


/* PRINTER */

void yas_print_expr(YasAtom atom);
char *yas_sprint_expr(YasAtom atom);

/* Exceptions */
extern YasAtom yas_ctx_exception;

int yas_builtin_throw(YasAtom args, YasAtom *result);
int yas_builtin_error(YasAtom args, YasAtom *result);
int yas_builtin_exit(YasAtom args, YasAtom *result);

int yas_raise_error(const char *type, const char *msg);

/* EVALUATOR */

YasAtom yas_env_create(YasAtom parent);
int yas_env_define(YasAtom env, YasAtom symbol, YasAtom value);
int yas_env_get(YasAtom env, YasAtom symbol, YasAtom *result);
int yas_env_set(YasAtom env, YasAtom symbol, YasAtom value);
int yas_eval_expr(YasAtom expr, YasAtom env, YasAtom *result);

/* DATA */

YasAtom yas_cons(YasAtom car_val, YasAtom cdr_val);
YasAtom yas_make_int(long x);
YasAtom yas_make_float(double x);
YasAtom yas_make_char(int32_t codepoint);
YasAtom yas_make_string(const char *s);
YasAtom yas_make_string_n(const char *s, size_t len);
YasAtom yas_make_vector(size_t size, YasAtom fill);
YasAtom yas_make_hash(void);
YasAtom yas_make_sym(const char *s);
YasAtom yas_make_keyword(const char *s);
YasAtom yas_make_builtin(YasBuiltin fn);
YasAtom yas_make_vec2(float x, float y);
YasAtom yas_make_vec3(float x, float y, float z);
YasAtom yas_make_vec4(float x, float y, float z, float w);
YasAtom yas_make_mat4_identity(void);
YasAtom yas_make_mat4_from_floats(const float *values);
int yas_listp(YasAtom expr);
YasAtom yas_copy_list(YasAtom list);
YasAtom yas_list_create(int n, ...);
YasAtom yas_list_get(YasAtom list, int k);
void yas_list_set(YasAtom list, int k, YasAtom value);
void yas_list_reverse(YasAtom *list);
void yas_gc_mark(YasAtom root);
void yas_gc();

/* Vector operations */
YasAtom yas_vector_ref(YasAtom vec, size_t index);
void yas_vector_set(YasAtom vec, size_t index, YasAtom value);
size_t yas_vector_length(YasAtom vec);

/* Hash operations */
YasAtom yas_hash_ref(YasAtom hash, YasAtom key, YasAtom default_val);
void yas_hash_set(YasAtom hash, YasAtom key, YasAtom value);
int yas_hash_remove(YasAtom hash, YasAtom key);
size_t yas_hash_size(YasAtom hash);

/* Metatable operations */
YasAtom yas_get_metamethod(YasAtom hash, const char *name);
int yas_has_metamethod(YasAtom hash, const char *name);

/* BUILTINS */

int yas_builtin_car(YasAtom args, YasAtom *result);
int yas_builtin_cdr(YasAtom args, YasAtom *result);
int yas_builtin_cons(YasAtom args, YasAtom *result);
int yas_builtin_eq(YasAtom args, YasAtom *result);
int yas_builtin_pairp(YasAtom args, YasAtom *result);
int yas_builtin_procp(YasAtom args, YasAtom *result);
int yas_builtin_add(YasAtom args, YasAtom *result);
int yas_builtin_subtract(YasAtom args, YasAtom *result);
int yas_builtin_multiply(YasAtom args, YasAtom *result);
int yas_builtin_divide(YasAtom args, YasAtom *result);
int yas_builtin_numeq(YasAtom args, YasAtom *result);
int yas_builtin_less(YasAtom args, YasAtom *result);

/* New type builtins */
int yas_builtin_floatp(YasAtom args, YasAtom *result);
int yas_builtin_integerp(YasAtom args, YasAtom *result);
int yas_builtin_stringp(YasAtom args, YasAtom *result);
int yas_builtin_vectorp(YasAtom args, YasAtom *result);
int yas_builtin_hashp(YasAtom args, YasAtom *result);
int yas_builtin_string_length(YasAtom args, YasAtom *result);
int yas_builtin_vector_length(YasAtom args, YasAtom *result);
int yas_builtin_hash_length(YasAtom args, YasAtom *result);
int yas_builtin_list_length(YasAtom args, YasAtom *result);
int yas_builtin_string_append(YasAtom args, YasAtom *result);
int yas_builtin_vector_append(YasAtom args, YasAtom *result);
int yas_builtin_list_append(YasAtom args, YasAtom *result);
int yas_builtin_hash_join(YasAtom args, YasAtom *result);
int yas_builtin_make_vector(YasAtom args, YasAtom *result);
int yas_builtin_vector_ref(YasAtom args, YasAtom *result);
int yas_builtin_vector_set(YasAtom args, YasAtom *result);
int yas_builtin_make_hash(YasAtom args, YasAtom *result);
int yas_builtin_hash_ref(YasAtom args, YasAtom *result);
int yas_builtin_hash_set(YasAtom args, YasAtom *result);
int yas_builtin_hash_remove(YasAtom args, YasAtom *result);
int yas_builtin_hash_keys(YasAtom args, YasAtom *result);

/* Metatable builtins */
int yas_builtin_set_metatable(YasAtom args, YasAtom *result);
int yas_builtin_get_metatable(YasAtom args, YasAtom *result);

/* SIMD Vector/Matrix builtins */
int yas_builtin_vec2p(YasAtom args, YasAtom *result);
int yas_builtin_vec3p(YasAtom args, YasAtom *result);
int yas_builtin_vec4p(YasAtom args, YasAtom *result);
int yas_builtin_mat4p(YasAtom args, YasAtom *result);
int yas_builtin_vec2(YasAtom args, YasAtom *result);
int yas_builtin_vec3(YasAtom args, YasAtom *result);
int yas_builtin_vec4(YasAtom args, YasAtom *result);
int yas_builtin_mat4(YasAtom args, YasAtom *result);
int yas_builtin_mat4_identity(YasAtom args, YasAtom *result);
int yas_builtin_vec_x(YasAtom args, YasAtom *result);
int yas_builtin_vec_y(YasAtom args, YasAtom *result);
int yas_builtin_vec_z(YasAtom args, YasAtom *result);
int yas_builtin_vec_w(YasAtom args, YasAtom *result);
int yas_builtin_vec_ref(YasAtom args, YasAtom *result);
int yas_builtin_vec_set(YasAtom args, YasAtom *result);
int yas_builtin_mat4_multiply(YasAtom args, YasAtom *result);
int yas_builtin_mat4_transpose(YasAtom args, YasAtom *result);
int yas_builtin_mat4_inverse(YasAtom args, YasAtom *result);

/* Output builtins */
int yas_builtin_print(YasAtom args, YasAtom *result);
int yas_builtin_tostring(YasAtom args, YasAtom *result);

/* File I/O */
char *yas_slurp(const char *path);

/* Include tracking for relative paths */
extern const char *yas_current_file_path;

/* Global features list for platform/feature detection */
extern YasAtom yas_global_features;
extern YasAtom yas_global_search_paths;
void yas_init_features(void);
int yas_has_feature(const char *name);
int yas_eval_feature_expr(YasAtom expr);

/* Module system */
extern YasAtom yas_library_registry;
void yas_load_all_modules(void);
void yas_load_base_library(void);
void yas_load_hash_library(void);
void yas_load_vector_library(void);
void yas_load_text_library(void);
void yas_load_io_library(void);
void yas_load_math_library(void);
YasAtom yas_define_native_module(const char *name_str, YasAtom *out_env, YasAtom *out_exports);
YasAtom yas_library_load(YasAtom name_parts);
char *yas_library_name_to_string(YasAtom name_parts);

/* Module special forms */
int yas_module_define_library(YasAtom args, YasAtom *result);
int yas_module_import(YasAtom args, YasAtom env, YasAtom *result); /* File loading */
int yas_eval_file(const char *path, YasAtom env, YasAtom *result, int case_insensitive);
int yas_eval_source(const char *text, const char *path, YasAtom env, YasAtom *result);
int yas_load_file(YasAtom env, const char *path);

/* ============================================================
 * I/O SANDBOX SYSTEM
 * ============================================================ */

/* Sandbox modes */
typedef enum {
	YAS_IO_SANDBOX_UNRESTRICTED,  /* Default: no restrictions */
	YAS_IO_SANDBOX_RESTRICTED,    /* Allow/deny lists apply; default_allow controls unmatched */
	YAS_IO_SANDBOX_DISABLED       /* All I/O operations fail */
} YasIOSandboxMode;

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
} YasIOProvider;

/* Sandbox C API (for embedding) */
void yas_io_sandbox_init(YasIOSandboxMode mode);
YasIOSandboxMode yas_io_sandbox_get_mode(void);
void yas_io_sandbox_allow_path(const char *pattern);
void yas_io_sandbox_deny_path(const char *pattern);
void yas_io_sandbox_set_default(int allow);  /* Set default for unmatched paths in RESTRICTED mode */
int  yas_io_sandbox_get_default(void);
void yas_io_sandbox_clear(void);
int  yas_io_sandbox_check_path(const char *path, int writing);

/* Provider system (for virtual filesystems) */
int  yas_io_register_provider(YasIOProvider *provider);
void yas_io_unregister_provider(const char *prefix);

/* Port operations */
YasAtom yas_make_port(void *handle, const char *path, int is_input, int is_binary, int provider_id);
void yas_port_close(YasAtom port);
YasAtom yas_io_open_file(const char *path, int is_input, int is_binary);
long yas_port_read(struct YasPort *p, void *buf, size_t n);
long yas_port_write(struct YasPort *p, const void *buf, size_t n);
int  yas_port_getc(struct YasPort *p);
void yas_port_flush(struct YasPort *p);
int  yas_io_file_exists(const char *path);

/* Bytevector operations */
YasAtom yas_make_bytevector(size_t size);
YasAtom yas_make_bytevector_from_data(const unsigned char *data, size_t size);
unsigned char yas_bytevector_ref(YasAtom bv, size_t index);
void yas_bytevector_set(YasAtom bv, size_t index, unsigned char value);
size_t yas_bytevector_length(YasAtom bv);

/* Port builtins */
int yas_builtin_open_input_file(YasAtom args, YasAtom *result);
int yas_builtin_open_output_file(YasAtom args, YasAtom *result);
int yas_builtin_close_port(YasAtom args, YasAtom *result);
int yas_builtin_portp(YasAtom args, YasAtom *result);
int yas_builtin_input_portp(YasAtom args, YasAtom *result);
int yas_builtin_output_portp(YasAtom args, YasAtom *result);
int yas_builtin_port_openp(YasAtom args, YasAtom *result);
int yas_builtin_read_char(YasAtom args, YasAtom *result);
int yas_builtin_read_line(YasAtom args, YasAtom *result);
int yas_builtin_read_all(YasAtom args, YasAtom *result);
int yas_builtin_read_byte(YasAtom args, YasAtom *result);
int yas_builtin_read_bytes(YasAtom args, YasAtom *result);
int yas_builtin_write_char(YasAtom args, YasAtom *result);
int yas_builtin_write_string(YasAtom args, YasAtom *result);
int yas_builtin_write_byte(YasAtom args, YasAtom *result);
int yas_builtin_write_bytes(YasAtom args, YasAtom *result);
int yas_builtin_newline(YasAtom args, YasAtom *result);
int yas_builtin_flush_port(YasAtom args, YasAtom *result);
int yas_builtin_file_exists(YasAtom args, YasAtom *result);
int yas_builtin_delete_file(YasAtom args, YasAtom *result);
int yas_builtin_rename_file(YasAtom args, YasAtom *result);

/* Bytevector builtins */
int yas_builtin_make_bytevector(YasAtom args, YasAtom *result);
int yas_builtin_bytevectorp(YasAtom args, YasAtom *result);
int yas_builtin_bytevector_length(YasAtom args, YasAtom *result);
int yas_builtin_bytevector_u8_ref(YasAtom args, YasAtom *result);
int yas_builtin_bytevector_u8_set(YasAtom args, YasAtom *result);
int yas_builtin_bytevector_to_string(YasAtom args, YasAtom *result);
int yas_builtin_string_to_bytevector(YasAtom args, YasAtom *result);

/* Sandbox scheme builtins */
int yas_builtin_io_sandbox_allow(YasAtom args, YasAtom *result);
int yas_builtin_io_sandbox_deny(YasAtom args, YasAtom *result);
int yas_builtin_io_sandbox_mode(YasAtom args, YasAtom *result);

/* Character builtins */
int yas_builtin_charp(YasAtom args, YasAtom *result);
int yas_builtin_char_to_integer(YasAtom args, YasAtom *result);
int yas_builtin_integer_to_char(YasAtom args, YasAtom *result);
int yas_builtin_char_eq(YasAtom args, YasAtom *result);
int yas_builtin_char_lt(YasAtom args, YasAtom *result);

/* String/Bytes encoding builtins */
int yas_builtin_string_to_bytes(YasAtom args, YasAtom *result);
int yas_builtin_bytes_to_string(YasAtom args, YasAtom *result);
int yas_builtin_string_ref(YasAtom args, YasAtom *result);
int yas_builtin_string_eq(YasAtom args, YasAtom *result);
int yas_builtin_string_byte_length(YasAtom args, YasAtom *result);

/* Keyword builtins */
int yas_builtin_keywordp(YasAtom args, YasAtom *result);
int yas_builtin_keyword_to_string(YasAtom args, YasAtom *result);
int yas_builtin_string_to_keyword(YasAtom args, YasAtom *result);

/* Procedure introspection */
int yas_builtin_procedure_doc(YasAtom args, YasAtom *result);

/* Decode a single UTF-8 character, return bytes consumed (0 on end/error) */
int yas_utf8_decode(const char *s, int32_t *cp);
/* Encode a codepoint to UTF-8, return bytes written (1-4) */
int yas_utf8_encode(int32_t cp, char *out);
/* Count codepoints in UTF-8 string */
size_t yas_utf8_strlen(const char *s);
/* Get codepoint at index, returns -1 if out of bounds */
int32_t yas_utf8_codepoint_at(const char *s, size_t index);
/* Get pointer to codepoint at index, returns NULL if out of bounds */
const char *yas_utf8_index_ptr(const char *s, size_t index);

/* Serialization */
int yas_builtin_serialize(YasAtom args, YasAtom *result);
int yas_builtin_deserialize(YasAtom args, YasAtom *result);
int yas_builtin_read_string(YasAtom args, YasAtom *result);

/* Search paths */
int yas_builtin_get_search_paths(YasAtom args, YasAtom *result);
int yas_builtin_set_search_paths(YasAtom args, YasAtom *result);
int yas_builtin_add_search_path(YasAtom args, YasAtom *result);

/* Format helper builtins */
int yas_builtin_number_to_string(YasAtom args, YasAtom *result);
int yas_builtin_string_upcase(YasAtom args, YasAtom *result);
int yas_builtin_string_downcase(YasAtom args, YasAtom *result);
int yas_builtin_string_capitalize(YasAtom args, YasAtom *result);
int yas_builtin_make_string(YasAtom args, YasAtom *result);
int yas_builtin_substring(YasAtom args, YasAtom *result);

/* ============================================================
 * FFI (Foreign Function Interface) - Optional
 * ============================================================ */
#ifdef YAS_FFI

/* FFI Type descriptors for marshaling */
typedef enum {
	YAS_FFI_TYPE_VOID,
	YAS_FFI_TYPE_INT8, YAS_FFI_TYPE_UINT8,
	YAS_FFI_TYPE_INT16, YAS_FFI_TYPE_UINT16,
	YAS_FFI_TYPE_INT32, YAS_FFI_TYPE_UINT32,
	YAS_FFI_TYPE_INT64, YAS_FFI_TYPE_UINT64,
	YAS_FFI_TYPE_FLOAT, YAS_FFI_TYPE_DOUBLE,
	YAS_FFI_TYPE_POINTER, YAS_FFI_TYPE_STRING,
	YAS_FFI_TYPE_STRUCT   /* Struct by value */
} YasFFIType;

/* Maximum fields per struct */
#define YAS_FFI_STRUCT_MAX_FIELDS 32

/* Struct field definition */
typedef struct YasFFIStructField {
	char *name;            /* Field name */
	YasFFIType type;      /* Field type */
	size_t offset;         /* Byte offset in struct */
} YasFFIStructField;

/* Struct type definition (registered via ffi-defstruct) */
typedef struct YasFFIStructType {
	char *name;                                    /* Struct name (e.g., "Color") */
	void *ffi_struct;                              /* ffi_type* for this struct */
	void *elements[YAS_FFI_STRUCT_MAX_FIELDS + 1];     /* ffi_type** element array */
	YasFFIStructField fields[YAS_FFI_STRUCT_MAX_FIELDS];  /* Field metadata */
	int field_count;                               /* Number of fields */
	size_t size;                                   /* Total size in bytes */
	struct YasFFIStructType *next;                    /* Linked list for registry */
} YasFFIStructType;

/* Struct instance (created via make-ffi-struct) */
typedef struct YasFFIStructInstance {
	YasFFIStructType *type;   /* Pointer to struct type definition */
	void *data;            /* Pointer to packed struct data */
	int mark;
	struct YasFFIStructInstance *next;
} YasFFIStructInstance;

/* Global struct type registry */
extern YasFFIStructType *yas_ffi_struct_registry;

/* Struct type lookup */
YasFFIStructType *yas_ffi_lookup_struct_type(const char *name);

/* Forward declaration for ffi_cif */
struct YasFFIFunction {
	void *fn_ptr;           /* Function pointer */
	void *cif;              /* ffi_cif* (opaque) */
	YasFFIType ret_type;
	YasFFIType *arg_types;
	void **ffi_arg_types;   /* ffi_type** for each arg (needed for structs) */
	void *ffi_ret_type;     /* ffi_type* for return (needed for structs) */
	int arg_count;
	int mark;
	struct YasFFIFunction *next;
};

/* GC-tracked lists for FFIFunction/FFIStructInstance (data.c sweeps these).
 * FFILibrary (dlopen handle) and FFIPointer (raw void*) are deliberately
 * NOT tracked here: ownership of a raw pointer can't be inferred (it may be
 * a dlsym result, an interior pointer into a struct, or a genuinely owned
 * ffi-malloc block), and closing a library when its atom becomes
 * unreachable could invalidate a still-live FFIFunction extracted from it.
 * Both remain explicitly managed via ffi-close / ffi-free. */
extern struct YasFFIFunction *yas_global_ffi_functions;
extern YasFFIStructInstance *yas_global_ffi_structs;

/* FFI core functions (ffi.c) */
void *yas_ffi_load_library(const char *path);
void yas_ffi_close_library(void *handle);
void *yas_ffi_get_symbol(void *handle, const char *name);
struct YasFFIFunction *yas_ffi_prepare(void *fn_ptr, YasAtom ret_type, YasAtom arg_types);
YasAtom yas_ffi_call_function(struct YasFFIFunction *fn, YasAtom args);
void yas_ffi_free_function(struct YasFFIFunction *fn);

/* FFI builtins */
int yas_builtin_ffi_open(YasAtom args, YasAtom *result);
int yas_builtin_ffi_close(YasAtom args, YasAtom *result);
int yas_builtin_ffi_lookup(YasAtom args, YasAtom *result);
int yas_builtin_ffi_define(YasAtom args, YasAtom *result);
int yas_builtin_ffi_call(YasAtom args, YasAtom *result);
int yas_builtin_ffi_malloc(YasAtom args, YasAtom *result);
int yas_builtin_ffi_free(YasAtom args, YasAtom *result);
int yas_builtin_ffi_pointer_ref(YasAtom args, YasAtom *result);
int yas_builtin_ffi_pointer_set(YasAtom args, YasAtom *result);
int yas_builtin_ffi_null(YasAtom args, YasAtom *result);
int yas_builtin_ffi_nullp(YasAtom args, YasAtom *result);

/* FFI struct builtins */
int yas_builtin_ffi_defstruct(YasAtom args, YasAtom *result);
int yas_builtin_ffi_make_struct(YasAtom args, YasAtom *result);
int yas_builtin_ffi_struct_ref(YasAtom args, YasAtom *result);
int yas_builtin_ffi_struct_set(YasAtom args, YasAtom *result);

/* FFI module loader */
void yas_load_ffi_library(void);

#endif /* YAS_FFI */

/* Global roots for GC */
extern YasAtom yas_library_registry;
extern YasAtom yas_global_search_paths;
extern YasAtom yas_global_features;

#endif /* YAS_H */

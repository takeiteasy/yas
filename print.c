#include "lisp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void print_expr(Atom atom)
{
	switch (atom.type) {
	default:
	case AtomType_Nil:
		printf("NIL");
		break;
	case AtomType_Pair:
		putchar('(');
		print_expr(car(atom));
		atom = cdr(atom);
		while (!nilp(atom)) {
			if (atom.type == AtomType_Pair) {
				putchar(' ');
				print_expr(car(atom));
				atom = cdr(atom);
			} else {
				printf(" . ");
				print_expr(atom);
				break;
			}
		}
		putchar(')');
		break;
	case AtomType_Symbol:
		printf("%s", atom.value.symbol);
		break;
	case AtomType_Integer:
		printf("%ld", atom.value.integer);
		break;
	case AtomType_Builtin:
		printf("#<BUILTIN:%p>", atom.value.builtin);
		break;
	case AtomType_Closure:
		printf("#<CLOSURE:%p>", atom.value.pair);
		break;
	case AtomType_Macro:
		printf("#<MACRO:%p>", atom.value.pair);
		break;
	case AtomType_Continuation:
		printf("#<CONTINUATION>");
		break;
	case AtomType_Float:
		printf("%g", atom.value.floatval);
		break;
	case AtomType_String:
		putchar('"');
		for (char *s = atom.value.string; *s; s++) {
			if (*s == '"' || *s == '\\')
				putchar('\\');
			putchar(*s);
		}
		putchar('"');
		break;
	case AtomType_Vector:
		printf("#(");
		for (size_t i = 0; i < atom.value.vector->size; i++) {
			if (i > 0) putchar(' ');
			print_expr(atom.value.vector->data[i]);
		}
		putchar(')');
		break;
	case AtomType_Hash:
		printf("#<HASH:%zu>", atom.value.hash->size);
		break;
	case AtomType_Port:
		printf("#<PORT %s %s%s>",
			atom.value.port->is_input ? "input" : "output",
			atom.value.port->path,
			atom.value.port->is_open ? "" : " (closed)");
		break;
	case AtomType_Bytevector:
		printf("#<BYTEVECTOR:%zu>", atom.value.bytevector->size);
		break;
	case AtomType_Char: {
		int32_t cp = atom.value.character;
		if (cp == '\n')
			printf("#\\newline");
		else if (cp == ' ')
			printf("#\\space");
		else if (cp == '\t')
			printf("#\\tab");
		else if (cp == '\r')
			printf("#\\return");
		else if (cp < 0x80 && cp >= 0x21 && cp <= 0x7E)
			printf("#\\%c", (char)cp);
		else
			printf("#\\x%X", cp);
		break;
	}
	case AtomType_Keyword:
		printf(":%s", atom.value.keyword);
		break;
	case AtomType_Vec2: {
#ifdef SIMD_SCALAR
		printf("#vec(%g %g)", atom.value.vec->data[0], atom.value.vec->data[1]);
#else
		float vals[4];
		#ifdef SIMD_X86
		_mm_storeu_ps(vals, atom.value.vec->v);
		#elif defined(SIMD_ARM_NEON)
		vst1q_f32(vals, atom.value.vec->v);
		#elif defined(SIMD_WASM)
		wasm_v128_store(vals, atom.value.vec->v);
		#endif
		printf("#vec(%g %g)", vals[0], vals[1]);
#endif
		break;
	}
	case AtomType_Vec3: {
#ifdef SIMD_SCALAR
		printf("#vec(%g %g %g)", atom.value.vec->data[0], atom.value.vec->data[1], atom.value.vec->data[2]);
#else
		float vals[4];
		#ifdef SIMD_X86
		_mm_storeu_ps(vals, atom.value.vec->v);
		#elif defined(SIMD_ARM_NEON)
		vst1q_f32(vals, atom.value.vec->v);
		#elif defined(SIMD_WASM)
		wasm_v128_store(vals, atom.value.vec->v);
		#endif
		printf("#vec(%g %g %g)", vals[0], vals[1], vals[2]);
#endif
		break;
	}
	case AtomType_Vec4: {
#ifdef SIMD_SCALAR
		printf("#vec(%g %g %g %g)", atom.value.vec->data[0], atom.value.vec->data[1], 
		       atom.value.vec->data[2], atom.value.vec->data[3]);
#else
		float vals[4];
		#ifdef SIMD_X86
		_mm_storeu_ps(vals, atom.value.vec->v);
		#elif defined(SIMD_ARM_NEON)
		vst1q_f32(vals, atom.value.vec->v);
		#elif defined(SIMD_WASM)
		wasm_v128_store(vals, atom.value.vec->v);
		#endif
		printf("#vec(%g %g %g %g)", vals[0], vals[1], vals[2], vals[3]);
#endif
		break;
	}
	case AtomType_Mat4: {
		printf("#mat(");
		for (int c = 0; c < 4; c++) {
#ifdef SIMD_SCALAR
			for (int r = 0; r < 4; r++) {
				if (c > 0 || r > 0) printf(" ");
				printf("%g", atom.value.mat4->columns[c].data[r]);
			}
#else
			float vals[4];
			#ifdef SIMD_X86
			_mm_storeu_ps(vals, atom.value.mat4->columns[c].v);
			#elif defined(SIMD_ARM_NEON)
			vst1q_f32(vals, atom.value.mat4->columns[c].v);
			#elif defined(SIMD_WASM)
			wasm_v128_store(vals, atom.value.mat4->columns[c].v);
			#endif
			for (int r = 0; r < 4; r++) {
				if (c > 0 || r > 0) printf(" ");
				printf("%g", vals[r]);
			}
#endif
		}
		printf(")");
		break;
	}
#ifdef GOLD_FFI
	case AtomType_FFILibrary:
		printf("#<FFI-LIBRARY:%p>", atom.value.ffi_library);
		break;
	case AtomType_FFIPointer:
		printf("#<FFI-POINTER:%p>", atom.value.ffi_pointer);
		break;
	case AtomType_FFIFunction:
		printf("#<FFI-FUNCTION:%p>", atom.value.ffi_fn);
		break;
	case AtomType_FFIStruct:
		if (atom.value.ffi_struct && atom.value.ffi_struct->type) {
			printf("#<FFI-STRUCT:%s>", atom.value.ffi_struct->type->name);
		} else {
			printf("#<FFI-STRUCT:nil>");
		}
		break;
#endif
	}
}

/* Calculate length needed for sprint_expr */
static size_t sprint_expr_len(Atom atom)
{
	char buf[64];
	switch (atom.type) {
	default:
	case AtomType_Nil:
		return 3;  /* "NIL" */
	case AtomType_Pair: {
		size_t len = 1;  /* '(' */
		Atom a = atom;
		while (!nilp(a)) {
			if (a.type == AtomType_Pair) {
				if (a.value.pair != atom.value.pair) len++;  /* space */
				len += sprint_expr_len(car(a));
				a = cdr(a);
			} else {
				len += 3;  /* " . " */
				len += sprint_expr_len(a);
				break;
			}
		}
		return len + 1;  /* ')' */
	}
	case AtomType_Symbol:
		return strlen(atom.value.symbol);
	case AtomType_Integer:
		snprintf(buf, sizeof(buf), "%ld", atom.value.integer);
		return strlen(buf);
	case AtomType_Float:
		snprintf(buf, sizeof(buf), "%g", atom.value.floatval);
		return strlen(buf);
	case AtomType_String:
		return strlen(atom.value.string);  /* For ->string, just the raw string */
	case AtomType_Char: {
		char tmp[8];
		return utf8_encode(atom.value.character, tmp);
	}
	case AtomType_Keyword:
		return strlen(atom.value.keyword) + 1;  /* ':' + name */
	case AtomType_Vector: {
		size_t len = 2;  /* "#(" */
		for (size_t i = 0; i < atom.value.vector->size; i++) {
			if (i > 0) len++;
			len += sprint_expr_len(atom.value.vector->data[i]);
		}
		return len + 1;
	}
	case AtomType_Hash:
		snprintf(buf, sizeof(buf), "#<HASH:%zu>", atom.value.hash->size);
		return strlen(buf);
	case AtomType_Builtin:
	case AtomType_Closure:
	case AtomType_Macro:
	case AtomType_Continuation:
		return 20;  /* estimate for #<...> forms */
	case AtomType_Port:
		return 40;
	case AtomType_Bytevector:
		return 30;
	case AtomType_Vec2:
	case AtomType_Vec3:
	case AtomType_Vec4:
		return 60;
	case AtomType_Mat4:
		return 200;
	}
}

/* Sprint atom to buffer, return chars written */
static size_t sprint_expr_to(Atom atom, char *buf, size_t pos)
{
	char tmp[64];
	int n;
	switch (atom.type) {
	default:
	case AtomType_Nil:
		memcpy(buf + pos, "NIL", 3);
		return pos + 3;
	case AtomType_Pair: {
		buf[pos++] = '(';
		Atom a = atom;
		int first = 1;
		while (!nilp(a)) {
			if (a.type == AtomType_Pair) {
				if (!first) buf[pos++] = ' ';
				pos = sprint_expr_to(car(a), buf, pos);
				a = cdr(a);
				first = 0;
			} else {
				memcpy(buf + pos, " . ", 3);
				pos += 3;
				pos = sprint_expr_to(a, buf, pos);
				break;
			}
		}
		buf[pos++] = ')';
		return pos;
	}
	case AtomType_Symbol:
		n = strlen(atom.value.symbol);
		memcpy(buf + pos, atom.value.symbol, n);
		return pos + n;
	case AtomType_Integer:
		n = snprintf(tmp, sizeof(tmp), "%ld", atom.value.integer);
		memcpy(buf + pos, tmp, n);
		return pos + n;
	case AtomType_Float:
		n = snprintf(tmp, sizeof(tmp), "%g", atom.value.floatval);
		memcpy(buf + pos, tmp, n);
		return pos + n;
	case AtomType_String:
		n = strlen(atom.value.string);
		memcpy(buf + pos, atom.value.string, n);
		return pos + n;
	case AtomType_Char:
		n = utf8_encode(atom.value.character, buf + pos);
		return pos + n;
	case AtomType_Keyword:
		buf[pos++] = ':';
		n = strlen(atom.value.keyword);
		memcpy(buf + pos, atom.value.keyword, n);
		return pos + n;
	case AtomType_Vector: {
		buf[pos++] = '#';
		buf[pos++] = '(';
		for (size_t i = 0; i < atom.value.vector->size; i++) {
			if (i > 0) buf[pos++] = ' ';
			pos = sprint_expr_to(atom.value.vector->data[i], buf, pos);
		}
		buf[pos++] = ')';
		return pos;
	}
	case AtomType_Hash:
		n = snprintf(tmp, sizeof(tmp), "#<HASH:%zu>", atom.value.hash->size);
		memcpy(buf + pos, tmp, n);
		return pos + n;
	case AtomType_Builtin:
		n = snprintf(tmp, sizeof(tmp), "#<BUILTIN>");
		memcpy(buf + pos, tmp, n);
		return pos + n;
	case AtomType_Closure:
		n = snprintf(tmp, sizeof(tmp), "#<CLOSURE>");
		memcpy(buf + pos, tmp, n);
		return pos + n;
	case AtomType_Macro:
		n = snprintf(tmp, sizeof(tmp), "#<MACRO>");
		memcpy(buf + pos, tmp, n);
		return pos + n;
	case AtomType_Continuation:
		n = snprintf(tmp, sizeof(tmp), "#<CONTINUATION>");
		memcpy(buf + pos, tmp, n);
		return pos + n;
	case AtomType_Port:
		n = snprintf(tmp, sizeof(tmp), "#<PORT>");
		memcpy(buf + pos, tmp, n);
		return pos + n;
	case AtomType_Bytevector:
		n = snprintf(tmp, sizeof(tmp), "#<BYTEVECTOR:%zu>", atom.value.bytevector->size);
		memcpy(buf + pos, tmp, n);
		return pos + n;
	case AtomType_Vec2:
	case AtomType_Vec3:
	case AtomType_Vec4: {
		float vals[4];
#ifdef SIMD_SCALAR
		for (int i = 0; i < 4; i++) vals[i] = atom.value.vec->data[i];
#else
	#ifdef SIMD_X86
		_mm_storeu_ps(vals, atom.value.vec->v);
	#elif defined(SIMD_ARM_NEON)
		vst1q_f32(vals, atom.value.vec->v);
	#elif defined(SIMD_WASM)
		wasm_v128_store(vals, atom.value.vec->v);
	#endif
#endif
		if (atom.type == AtomType_Vec2)
			n = snprintf(tmp, sizeof(tmp), "#vec(%g %g)", vals[0], vals[1]);
		else if (atom.type == AtomType_Vec3)
			n = snprintf(tmp, sizeof(tmp), "#vec(%g %g %g)", vals[0], vals[1], vals[2]);
		else
			n = snprintf(tmp, sizeof(tmp), "#vec(%g %g %g %g)", vals[0], vals[1], vals[2], vals[3]);
		memcpy(buf + pos, tmp, n);
		return pos + n;
	}
	case AtomType_Mat4:
		n = snprintf(tmp, sizeof(tmp), "#<MAT4>");
		memcpy(buf + pos, tmp, n);
		return pos + n;
	}
}

/* Convert atom to string - returns newly allocated string */
char *sprint_expr(Atom atom)
{
	size_t len = sprint_expr_len(atom);
	char *buf = malloc(len + 1);
	size_t end = sprint_expr_to(atom, buf, 0);
	buf[end] = '\0';
	return buf;
}

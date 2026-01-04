#include "lisp.h"
#include <stdio.h>

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
	}
}

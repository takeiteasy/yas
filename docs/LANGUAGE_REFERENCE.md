# Language reference

For a tutorial-style introduction see [GETTING_STARTED.md](GETTING_STARTED.md).
For interpreter internals (how these are implemented) see
[ARCHITECTURE.md](ARCHITECTURE.md).

## Reading yas source

- **All symbols are read as uppercase.** `car` and `CAR` are the same
  symbol. Case doesn't matter in source, but error messages and
  `write`/`print`-style output show the uppercase form.
- Numbers: decimal, `0x` hex, `0b` binary; a `.` makes a literal a float.
- `'x` reads as `(quote x)`; `` `x `` as `(quasiquote x)`;  `,x`/`,@x` as
  `(unquote x)`/`(unquote-splicing x)`.
- Comments: `;` to end of line.
- String interpolation: `"Hello {{name}}"` expands at read time to
  `(string-append "Hello " name)`.

### Literals

| Syntax | Value |
|---|---|
| `42`, `0x2a`, `0b101010` | integer |
| `3.14` | float |
| `"text"` | string |
| `#\a`, `#\newline`, `#\x41` | character |
| `:keyword` | keyword |
| `#(1 2 3)` | vector |
| `#vec(x y)`, `#vec(x y z)`, `#vec(x y z w)` | vec2/vec3/vec4 |
| `#mat(...)` | mat4 |
| `#hash(:a 1 :b 2)` | hash (keyword keys become symbol keys) |
| `nil`, `t` | false/empty-list, true |

### Reader conditionals

```scheme
#+macos (print "on macos")
#-macos (print "not on macos")
```

Evaluated against the running platform's feature list (`MACOS`/`LINUX`/
`WINDOWS`/`BSD`, `UNIX`, an architecture name, and `FFI` when built with
`YAS_FFI=1`) via `*features*`. `(or ...)`/`(and ...)`/`(not ...)` compose
inside the conditional: `#+(or macos linux) ...`.

## Special forms

These are recognized directly by the evaluator (not macros - see
[ARCHITECTURE.md](ARCHITECTURE.md#evaluator) for why that distinction
matters, e.g. for the AOT pass):

| Form | |
|---|---|
| `(quote x)` | `'x` |
| `(define name value)` / `(define (name args...) body...)` | |
| `(lambda (args...) body...)` | see [Lambda lists](#lambda-lists) |
| `(if cond then else)` | `else` optional |
| `(try body... (catch e handler...) (finally cleanup...))` | see [Exceptions](#exceptions) |
| `(defmacro (name args...) body...)` | |
| `(apply proc args-list)` | |
| `(set! name value)` | |
| `(macroexpand-1 form)` | expand one step; `(yas base)`'s `macroexpand` loops this to a fixed point |
| `(call/cc (lambda (k) ...))` | see [call/cc](#callcc) |
| `(include "file")`, `(include-ci "file")` | textual inclusion |
| `(define-library (name...) ...)`, `(import ...)` | see [Modules](#modules) |

Everything else that reads like syntax - `begin`, `let`, `let*`, `cond`,
`and`, `or`, `when`, `unless`, `quasiquote`, `while`, `dotimes`, `dolist`,
`for`, `assert` - is a `defmacro` in `(yas base)`, expanded at evaluation
time, not compiled separately. They behave like special forms in practice
but can in principle be shadowed or redefined, unlike the ones above.

## Lambda lists

Richer than plain Scheme:

```scheme
(lambda (a b) ...)                        ; positional
(lambda (a [b 10]) ...)                    ; b optional, defaults to 10
(lambda (a :kw [b 5]) ...)                  ; keyword arg :kw, bound to b, default 5
(lambda (a . rest) ...)                     ; rest args collected into a list
(lambda (a) "docstring" a)                  ; leading string + more body = docstring
```

Call keyword arguments as `(f 1 :kw 2)`. `procedure-doc` retrieves a
function's docstring (`nil` if none).

## Exceptions

```scheme
(try
  (throw 'my-error)              ; or (error "message")
  (catch e
    (print "caught:" e))
  (finally
    (print "always runs")))
```

`throw` raises any value; `error` raises a string. `catch` binds the
raised value to `e`. `finally` always runs, whether or not an exception
was caught. Note: `try` recurses into the evaluator (a real C-stack
boundary) - tail calls don't propagate through it, and a continuation
captured inside a `try` can't be invoked across it (see
[ARCHITECTURE.md](ARCHITECTURE.md#evaluator)).

## call/cc

```scheme
(call/cc (lambda (k) (k 42)))                    ; => 42
(+ 1 (call/cc (lambda (k) (+ 2 (k 3)))))            ; => 4, k escapes the (+ 2 ...)
```

Continuations are **re-entrant**, not one-shot: a captured continuation
can be invoked more than once, including after the `call/cc` call that
created it has already returned (see
[ARCHITECTURE.md](ARCHITECTURE.md#callcc)).

## Modules

Import one or more libraries:

```scheme
(import (yas base) (yas math))
(import (only (yas math) sqrt abs))
(import (except (yas base) map))
(import (prefix (yas math) m/))          ; m/sqrt, m/abs, ...
(import (rename (yas math) (sqrt square-root)))
```

Define your own:

```scheme
(define-library (my lib)
  (export greet)
  (import (yas base))
  (begin
    (define (greet name) (print "hi" name))))
```

Imports copy each exported binding directly into the importing
environment - there's no namespace object at runtime, so `m/sqrt` (from a
`prefix` import) is just an ordinary symbol, not a member access.

### Standard modules

- **`(yas base)`** - `car cdr cons eq? pair? procedure? float? string?
  length append throw error print t caar/cadr/.../cdddr foldl foldr list
  unary-map map quasiquote and begin cond let let* or unless when assert
  + - * / = < <= > >= for-each list-ref list-tail reverse list? not null?
  macroexpand dotimes dolist while for equal? *features*`
- **`(yas hash)`** - `make-hash hash-ref hash-set! hash-remove!
  hash-length hash-join hash-keys hash? hash-update! alist->hash
  hash->alist hash-for-each hash-map hash-merge set-metatable!
  get-metatable callable? serialize deserialize`
- **`(yas vector)`** - `make-vector vector-ref vector-set! vector-length
  vector-append vector? vector-map vector-for-each vector->list
  list->vector vector-fold`
- **`(yas text)`** - `string? string-length string-ref string=?
  string-append string-byte-length ->string number->string string-upcase
  string-downcase string-capitalize make-string substring keyword?
  keyword->string string->keyword`
- **`(yas io)`** - `open-input-file open-output-file close-port port?
  input-port? output-port? port-open? read-char read-line read-all
  read-byte read-bytes write-char write-string write-byte write-bytes
  newline flush-port file-exists? delete-file rename-file copy-file
  path-join path-dirname path-basename path-extension make-bytevector
  bytevector? bytevector-length bytevector-u8-ref bytevector-u8-set!`
- **`(yas math)`** - `*pi* *tau* *e* vec2?/vec3?/vec4?/mat4? vec2/vec3/
  vec4/mat4 mat4-identity mat4* mat4-inverse mat4-transpose vec-x/y/z/w
  vec-ref vec-set! vec-dot vec-cross vec-length vec-normalize vec-lerp
  vec-distance radians->degrees degrees->radians sqrt abs floor ceil
  round sin cos tan asin acos atan atan2 pow exp log clamp gcd lcm min
  max negative? odd? positive? quotient remainder zero? modulo even?
  number? mat4-translate/scale/rotate-x/rotate-y/rotate-z mat4-ortho
  mat4-perspective mat4-look-at`. Beyond vec/mat, these are interpreted
  Lisp, not C builtins (see [ARCHITECTURE.md](ARCHITECTURE.md)).
- **`(yas format)`** - `format`, a `~`-directive string formatter (see
  [Format directives](#format-directives)).
- **`(yas test)`** - `check test run-tests` and supporting state; see
  [Testing](GETTING_STARTED.md#testing).
- **`(yas ffi)`** - only under `YAS_FFI=1`; see [BUILDING.md](BUILDING.md).

### Format directives

`(format nil "~A + ~A = ~A" 2 3 5)` -> `"2 + 3 = 5"`. `nil` as the first
argument returns a string instead of printing.

| Directive | Meaning |
|---|---|
| `~A` | aesthetic (display, no quotes) |
| `~S` | s-expression (write, with quotes) |
| `~D` | decimal |
| `~X` | hexadecimal |
| `~O` | octal |
| `~B` | binary |
| `~R` | radix - spelled out (`~R` -> "forty-two") or roman (`~@R`) |
| `~:R` | ordinal ("third") |
| `~C` | character |
| `~P` | plural suffix (`~D item~P`) |
| `~T` | tab |
| `~F` | fixed-point float |
| `~@D` | signed decimal (leading `+`) |
| `~:D` | decimal with thousands separators |
| `~~` | literal `~` |

## Metatables (operator overloading)

Hashes can carry a metatable that intercepts arithmetic, comparison,
indexing, and calls:

```scheme
(define mt (make-hash))
(hash-set! mt :add (lambda (self other) ...))      ; self + other
(hash-set! mt :call (lambda (self . args) ...))      ; (self args...)
(hash-set! mt :missing (lambda (self key) ...))       ; hash-ref miss
(set-metatable! obj mt)
```

See `eval_do_apply` in [ARCHITECTURE.md](ARCHITECTURE.md#metamethod-dispatch)
for exactly which builtins this intercepts, and note that anything
calling the underlying `Builtin` function pointer directly (rather than
through the evaluator) bypasses it.

## SIMD types

`vec2`/`vec3`/`vec4`/`mat4` are native SIMD-backed types (SSE/NEON/WASM
depending on platform), not built from lists or vectors:

```scheme
#vec(1 2 3)                 ; vec3 literal
(vec3? #vec(1 2 3))          ; t
(vec-x #vec(10 20))           ; 10.0
(vec-dot v1 v2)
(mat4-identity)
(mat4* m1 m2)
```

## Serialization

`(yas hash)` exports `serialize`/`deserialize` for a yas-native binary
format; `(yas text)`/`(yas io)` cover string/byte-level I/O. See
`std/yas/hash.ys` for the wire format if you need to interoperate with it
from outside yas.

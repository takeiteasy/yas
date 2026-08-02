# Getting started

## Build and run

```sh
make               # builds ./yas
./yas              # REPL
./yas script.ys     # run a file
```

See [BUILDING.md](BUILDING.md) for test/FFI/AOT build targets.

## The REPL

```
$ ./yas
(yas repl v0.0.1 :)

> (+ 1 2)
3
> /exit
```

`/exit` or `/quit` leaves the REPL.

## A first program

yas ships almost nothing without an explicit import - `car`, `cdr`,
`define`, `lambda`, `if`, arithmetic, `let`, `cond`, and the rest of what
feels like "the language" live in the `(yas base)` module:

```scheme
(import (yas base))

(define (greet name)
  (print "Hello," name))

(greet "world")
```

Running a file from the command line auto-imports `(yas base)` for you
(see `main.c`); code loaded other ways (via `eval_source`, or a library's
own body) does not, and must import it explicitly.

## Core syntax

```scheme
;; define
(define x 10)
(define (add a b) (+ a b))

;; lambda, closures
(define make-adder (lambda (n) (lambda (x) (+ x n))))
(define add5 (make-adder 5))
(add5 10)                          ; => 15

;; conditionals
(if (> x 5) "big" "small")
(cond ((< x 0) "negative")
      ((= x 0) "zero")
      (else "positive"))
(when (> x 5) (print "big"))
(unless (> x 5) (print "not big"))

;; let / let*
(let ((a 1) (b 2)) (+ a b))          ; parallel binding
(let* ((a 1) (b (+ a 1))) b)          ; sequential binding, b sees a

;; loops
(dotimes (i 5) (print i))
(dolist (x '(1 2 3)) (print x))
(while (< x 10) (set! x (+ x 1)))
(for (init cond step) body...)

;; lists
(list 1 2 3)
(car '(1 2 3))    ; 1
(cdr '(1 2 3))    ; (2 3)
(map (lambda (x) (* x x)) '(1 2 3))    ; (1 4 9)
(foldl + 0 '(1 2 3))                    ; 6
```

Lambda lists support optional (`[x default]`) and keyword (`:kw [var
default]`) parameters and a `. rest` tail - see
[LANGUAGE_REFERENCE.md](LANGUAGE_REFERENCE.md#lambda-lists).

## Modules

```scheme
(import (yas base) (yas math) (yas hash))
```

Available modules: `(yas base)`, `(yas hash)`, `(yas vector)`, `(yas
text)`, `(yas io)`, `(yas math)`, `(yas format)`, `(yas test)`, and `(yas
ffi)` (only under `YAS_FFI=1` builds). See
[LANGUAGE_REFERENCE.md](LANGUAGE_REFERENCE.md#modules) for what each
exports.

```scheme
(import (only (yas math) sqrt abs))         ; just these names
(import (except (yas base) map))             ; everything but this
(import (prefix (yas math) m/))               ; m/sqrt, m/abs, ...
(import (rename (yas math) (sqrt square-root)))
```

## Error handling

```scheme
(try
  (throw "custom-error" "something went wrong")
  (catch e
    (print "caught:" e))
  (finally
    (print "cleanup")))
```

## Testing

```scheme
(import (yas base) (yas test))

(test "addition"
  (check (+ 1 1) 2)
  (check (* 2 3) 6))

(run-tests)
```

`run-tests` prints a pass/fail summary but does not set a process exit
code ([ticket](https://todo.sr.ht/~takeiteasy/yas/9)) - `make test` works
around this by grepping the printed summary. See `suite.ys` for a full
example test file, and `(yas test)` in
[LANGUAGE_REFERENCE.md](LANGUAGE_REFERENCE.md#yas-test) for `check`'s
comparison rules.

## Next steps

- [Language reference](LANGUAGE_REFERENCE.md) - full syntax and stdlib
- [Embedding](EMBEDDING.md) - using yas as a C library
- [AOT compilation](AOT.md) - compiling a numeric subset to native C
- [Architecture](ARCHITECTURE.md) - how the interpreter works internally

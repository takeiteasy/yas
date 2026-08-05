# yas

**Y**as **A**in't **S**cheme, or **Y**et **A**nother **S**cheme, is a small
embeddable Scheme-like interpreter geared towards game development. It also
compiles a restricted numeric subset of itself to native C at compile time,
via [cccc](https://github.com)'s comptime pass and reflection API.

```sh
make && make test    # build the interpreter, run the test suite
./yas suite.ys        # or run it directly
```

```scheme
(define (fib n)
  (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2)))))
(print (fib 20))
```

## Docs

- [Getting started](docs/GETTING_STARTED.md) - a first program, REPL, modules
- [Language reference](docs/LANGUAGE_REFERENCE.md) - syntax and stdlib
- [Architecture](docs/ARCHITECTURE.md) - value representation, evaluator,
  GC, module system
- [GC](docs/GC.md) - the incremental tricolor collector and root API
- [Building](docs/BUILDING.md) - build targets, FFI, AOT
- [Embedding](docs/EMBEDDING.md) - the C API for host programs
- [AOT compilation](docs/AOT.md) - lowering yas to C at compile time

## License

```text
yas
Copyright (C) 2025 George Watson

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program. If not, see <https://www.gnu.org/licenses/>.
```

# Fuzzing

`aux/fuzz_*.ys` / `aux/fuzz/` hold the reader/evaluator fuzzing half of
the `aux/` convention (see [BENCHMARKING.md](BENCHMARKING.md)). It
targets `yas_read_expr()` (`src/read.c`) and `yas_eval_expr()`
(`src/eval.c`) with generated input, hunting for crashes, wild reads,
and hangs.

```sh
make fuzz                                              # 5000 mixed cases (~10s)
python3 aux/fuzz/fuzz.py --seed 42 --cases 50000       # deeper session
python3 aux/fuzz/fuzz.py --mode grammar --cases 1000   # one generator
python3 aux/fuzz/fuzz.py --self-test                   # robustness checks
```

Not part of `make check`: like [bench](BENCHMARKING.md) this is an
informational gate, not a pass/fail regression test - a pass means "no
crashes found", never "no bugs exist". Deeper sessions pair well with
the stress runtimes (`make clean` between flag changes, as always):

```sh
make clean && make YAS_GC_STRESS=1 && make fuzz    # GC-stepping audit
make clean && make YAS_ASAN=1 && make fuzz         # ASan build
```

## Architecture

Two pieces:

- **`aux/fuzz_harness.c`** - a small C runner built against `libyas`
  (same pattern as `aux/gc_test.c`). For each case file it creates a
  fresh context, imports `yas/base`, disables the I/O sandbox (fuzzed
  input must never touch the real filesystem), and runs the same
  top-level read/eval loop as `main.c`'s file loader, `Error_Skip`
  semantics included. Per-case read/eval errors are expected noise.
  A watchdog (`SIGALRM`) re-arms per case; crash handlers dump a C
  backtrace naming the offending file.
- **`aux/fuzz/fuzz.py`** - the seeded generator/driver. It writes cases
  under `build/fuzz/cases/`, runs them through the harness in batches
  (default 100/process), and on any batch death recursively bisects to
  isolate every offending single case.

## Generators

`--mode` picks one, or `mix` (default) round-robins all three:

| mode | what it does |
|---|---|
| `grammar` | grammar-aware generation over the yas token alphabet: lists/brackets, dotted pairs, quotes/quasiquote, string escapes and `{{expr}}` interpolation, char literals, numeric formats, vectors/`#hash(`/`#vec(`/`#mat(` literals, reader conditionals, comments, plus definitions and applications over a curated safe-symbol pool |
| `mutate` | byte-level mutations (bit flips, splices between seeds, chunk duplication/truncation, structural-token injection) of the valid-form seed corpus in `aux/fuzz/corpus/*.ys` |
| `bytes` | raw random-ish bytes, biased toward printable/structural characters |

The safe-symbol pool deliberately excludes everything that touches the
filesystem, FFI, or process state; the disabled sandbox is the belt to
the corpus' braces, not a license to include those names.

## Determinism and reproduction

A run is a pure function of its inputs: given the same `--seed`,
corpus, generators, and yas build, the case sequence is identical
(`--self-test` checks this). Cases are numbered in the run order and
written to `build/fuzz/cases/seed<SEED>_case<N>.ys`.

Any single case can be replayed without the driver:

```sh
./yas <case-file>                    # ordinary interpreter
build/fuzz_harness <case-file>       # harness (watchdog + backtraces)
```

## What counts as a finding

| outcome | classification | where it goes |
|---|---|---|
| signal death, ASan report, C backtrace, or an exit status stolen by fuzzed code escaping through `(exit ...)` | **crash** - always fails the run | `fuzz-findings/` |
| watchdog/driver deadline hit (default 5s/case) | **timeout** - reported, does *not* fail the run | `fuzz-findings/timeouts/` |
| per-case syntax/eval errors, unbound symbols, wrong types | expected noise | nowhere |

Timeouts are quarantined rather than failed because an eternal loop
like `(while 'never)` is *correct* yas semantics - the fuzzer writes
those on purpose-adjacent paths constantly, and each one costs its
watchdog budget to confirm. A genuine evaluator wedge looks the same
from outside; triage is manual: replay the quarantined case and judge.

Crash triage is also manual: replay the saved `.ys` under the harness
(and under `YAS_GC_STRESS=1` / ASan builds if it survives plain
builds), minimize by deleting lines, then either fix or file a ticket
with the artifact attached. Note `make clean` wipes `build/` - crash
artifacts live in gitignored top-level `fuzz-findings/` precisely so
they survive that.

## Track record

First sessions of this harness found two real interpreter bugs within
its first ~10k generated cases, both fixed:

- dotted tails inside structured literals (`#hash(:k v . 2)` and
  friends) walked the tail payload as if it were a pair - a
  wild-pointer read;
- a runtime-computed APPLY (`((quote apply) 1)`) bypassed the source
  form's arity gate and dereferenced NULL.

Both shapes are now permanent suite regressions ("Improper structured
literals", "Runtime-computed apply" tests in `suite.ys`), and hostile
inputs exercising them are pinned in `--self-test`.

## Limitations

- Not coverage-guided: generation is blind to which branches the
  interpreter took. A libFuzzer/AFL target wired to the same harness
  logic would be the natural follow-up if blind throughput stops
  finding anything.
- Throughput is bounded by fresh-context-per-case isolation (~1k
  cases/s here) - deliberate; shared-context batching would let one
  case's state poison another's verdict.
- The watchdog can't distinguish a wedged evaluator from a legitimate
  eternal loop; see the triage note above.

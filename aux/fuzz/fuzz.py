#!/usr/bin/env python3
"""Reader/evaluator fuzzer for aux/fuzz_*.ys (docs/FUZZING.md).

Generates deterministic fuzz cases from a seed, runs them through the C
harness (build/fuzz_harness, see aux/fuzz_harness.c) in batches, and
isolates any case that crashes, hangs, or kills the batch early.

Usage:
    make fuzz                                          # 5000 mixed cases
    python3 aux/fuzz/fuzz.py --seed 1234 --cases 50000 # deeper session
    python3 aux/fuzz/fuzz.py --mode grammar --cases 1000
    python3 aux/fuzz/fuzz.py --self-test               # robustness checks

Modes (--mode):
    grammar  grammar-aware generation over the yas token alphabet,
             bounded nesting depth, safe-symbol pool for applications
    mutate   byte-level mutations (flip/splice/truncate/...) of the
             hand-written valid seed corpus in aux/fuzz/corpus/
    bytes    raw random-ish bytes, biased toward printable/structural
    mix      round-robin thirds of all three (default)

A FINDING is a case whose processing kills the harness: signal death,
an ASAN report, or fuzzed code escaping through (exit ...). Ordinary
per-case read/eval errors are expected noise, never findings. Watchdog
timeouts are quarantined separately (fuzz-findings/timeouts/): an
eternal loop like (while 'never) is correct yas semantics, not a bug.
Crash artifacts land in fuzz-findings/ - each is a plain .ys file
reproducible with `./yas <file>` (or build/fuzz_harness <file>).

Given the same --seed and the same yas build, a run is fully
deterministic: identical cases in identical order.
"""
import argparse
import shutil
import subprocess
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
HARNESS = REPO_ROOT / "build" / "fuzz_harness"
CORPUS_DIR = Path(__file__).resolve().parent / "corpus"
CASES_DIR = REPO_ROOT / "build" / "fuzz" / "cases"
FINDINGS_DIR = REPO_ROOT / "fuzz-findings"  # gitignored, survives `make clean`
TIMEOUTS_DIR = FINDINGS_DIR / "timeouts"

# Symbols the generator may emit in application position: pure compute
# and control flow only. Deliberately excludes everything that touches
# the filesystem (open/delete/rename), FFI, or process state - the
# harness also runs with the I/O sandbox disabled, but generated cases
# shouldn't rely on that belt alone.
SAFE_SYMBOLS = [
    "car", "cdr", "cons", "list", "append", "length", "list-ref",
    "+", "-", "*", "/", "=", "<", ">", "<=", ">=",
    "eq?", "equal?", "pair?", "null?", "list?", "not",
    "integer?", "float?", "string?", "symbol?", "vector?",
    "hash?", "char?", "keyword?", "procedure?", "boolean?", "nil?",
    "string-append", "substring", "string-length", "string->symbol",
    "symbol->string", "number->string", "string->number",
    "string=?", "string<?", "upcase", "downcase",
    "make-vector", "vector-ref", "vector-set!", "vector-length",
    "make-hash", "hash-set!", "hash-ref", "hash-remove!", "hash-keys",
    "hash->list", "list->hash", "list->vector", "vector->list",
    "bytevector", "bytevector-length", "bytevector-u8-ref",
    "abs", "min", "max", "modulo", "quotient", "remainder",
    "floor", "ceiling", "round", "sqrt", "expt", "exact->inexact",
    "map", "filter", "foldl", "foldr", "apply", "for-each",
    "member", "assoc", "reverse", "sort", "range", "iota",
    "display", "newline", "write", "read-string",
    "call/cc", "error", "throw", "try", "catch", "eval", "gc",
    "gensym", "void", "boolean?", "zero?", "positive?", "negative?",
]

SPECIAL_FORMS = ["if", "when", "unless", "begin", "and", "or",
                 "cond", "case", "let", "let*", "letrec", "while"]

FEATURE_NAMES = ["macos", "linux", "unix", "windows", "arm64", "x86_64",
                 "ffi", "(or macos linux)", "(and unix arm64)"]

STRUCTURAL_TOKENS = [b"(", b")", b"[", b"]", b"'", b"`", b",", b",@",
                     b'"', b"#(", b"#\\", b";", b"#+", b"#-", b".",
                     b"#hash(", b"#vec(", b"#mat(", b"}}", b"{{"]


class Gen:
    """Seeded case generator. One instance per run; the shared rng makes
    the whole case sequence a pure function of the seed."""

    def __init__(self, rng, corpus):
        self.rng = rng
        self.corpus = corpus  # list of bytes

    # ── grammar mode ────────────────────────────────────────────────
    def atom(self):
        r = self.rng.random()
        if r < 0.22:
            return str(self.rng.randint(-10**6, 10**6))
        if r < 0.30:
            return repr(round(self.rng.uniform(-1000, 1000), 6))
        if r < 0.34:
            return "0x%x" % self.rng.getrandbits(20)
        if r < 0.38:
            return "0b" + format(self.rng.getrandbits(12), "b")
        if r < 0.48:
            # Real symbol (application fodder) or garbage identifier -
            # the reader uppercases everything either way.
            if self.rng.random() < 0.6:
                return self.rng.choice(SAFE_SYMBOLS)
            n = self.rng.randint(1, 8)
            alphabet = "abcdefghijklmnopqrstuvwxyz-*!?<>+=/.0123456789"
            return "".join(self.rng.choice(alphabet) for _ in range(n))
        if r < 0.54:
            n = self.rng.randint(1, 6)
            alphabet = "abcdefghijklmnopqrstuvwxyz0123456789-_"
            return ":" + "".join(self.rng.choice(alphabet) for _ in range(n))
        if r < 0.66:
            return self.string_lit()
        if r < 0.72:
            return self.char_lit()
        if r < 0.76:
            n = self.rng.randint(0, 4)
            return "#(%s)" % " ".join(str(self.rng.randint(0, 99)) for _ in range(n))
        if r < 0.78:
            return "#nil" if self.rng.random() < 0.5 else "#t"
        return self.rng.choice(SAFE_SYMBOLS)

    def string_lit(self):
        pieces = []
        for _ in range(self.rng.randint(1, 3)):
            kind = self.rng.random()
            if kind < 0.5:
                n = self.rng.randint(0, 12)
                alphabet = "abcdefXYZ .,!0123456789"
                pieces.append("".join(self.rng.choice(alphabet) for _ in range(n)))
            elif kind < 0.75:
                pieces.append(self.rng.choice([r"\n", r"\t", r"\\", r"\"", r"\q"]))
            else:
                # Interpolation - exercises read.c's {{expr}} scanner
                inner = self.rng.choice(["x", "(+ 1 2)", "", "{{y}}", '"s"', ":k"])
                pieces.append("{{%s}}" % inner)
        body = "".join(pieces).replace('"', r"\"")
        return '"%s"' % body

    def char_lit(self):
        return self.rng.choice(
            ["#\\a", "#\\Z", "#\\space", "#\\newline", "#\\tab",
             "#\\x41", "#\\u00e9", "#\\U0001F600", "#\\(", "#\\\\",
             "#\\x", "#\\u12", "#\\q"])

    def expr(self, depth, allow_defs=True):
        if depth <= 0 or self.rng.random() < 0.35:
            return self.atom()
        r = self.rng.random()
        d = depth - 1
        if r < 0.34:
            # Plain list, sometimes dotted
            n = self.rng.randint(0, 4)
            parts = [self.expr(d, False) for _ in range(n)]
            if parts and self.rng.random() < 0.12:
                return "(%s . %s)" % (" ".join(parts), self.expr(d, False))
            return "(%s)" % " ".join(parts)
        if r < 0.42:
            # Application over the safe pool
            n = self.rng.randint(0, 3)
            args = " ".join(self.expr(d, False) for _ in range(n))
            return "(%s %s)" % (self.rng.choice(SAFE_SYMBOLS), args)
        if r < 0.52 and allow_defs:
            return self.special_form(d)
        if r < 0.58:
            q = self.rng.choice(["'", "`", ",", ",@"])
            return "%s%s" % (q, self.expr(d, False))
        if r < 0.63:
            return "#(%s)" % " ".join(self.expr(d, False) for _ in range(self.rng.randint(0, 4)))
        if r < 0.67:
            pairs = " ".join(self.expr(d, False) for _ in range(self.rng.randint(0, 5)))
            return "#hash(%s)" % pairs
        if r < 0.70:
            nums = [str(round(self.rng.uniform(-9, 9), 2)) for _ in range(self.rng.choice([2, 3, 4]))]
            return "#vec(%s)" % " ".join(nums)
        if r < 0.72:
            nums = " ".join(str(self.rng.randint(-9, 9)) for _ in range(16))
            return "#mat(%s)" % nums
        if r < 0.77:
            # Reader conditional - feature expr then payload
            feat = self.rng.choice(FEATURE_NAMES)
            sign = self.rng.choice(["#+", "#-"])
            return "%s(%s) %s" % (sign, feat, self.expr(d, False))
        if r < 0.80:
            return "[%s]" % " ".join(self.expr(d, False) for _ in range(self.rng.randint(0, 3)))
        if r < 0.83:
            return "; %s\n%s" % (self.rng.choice(["note", "x", ""], ),
                                 self.expr(d, False))
        return self.atom()

    def special_form(self, d):
        form = self.rng.choice(SPECIAL_FORMS)
        a, b = self.expr(d, False), self.expr(d, False)
        if form == "if":
            return "(if %s %s%s)" % (a, b,
                                     "" if self.rng.random() < 0.5 else " " + self.expr(d, False))
        if form in ("when", "unless", "while"):
            # `while` gets an immediately-failing condition: any
            # constant-truthy one is a legitimate infinite loop (correct
            # yas semantics), and watchdog quarantine noise isn't worth
            # that shape's coverage - mutations still produce it anyway.
            cond = ("(< x 0)" if form == "while" else a)
            return "(%s %s %s)" % (form, cond, b)
        if form in ("and", "or"):
            return "(%s %s %s)" % (form, a, b)
        if form == "cond":
            return "(cond (%s %s) (else %s))" % (a, b, self.expr(d, False))
        if form == "case":
            return "(case %s ((1 2) %s) (else %s))" % (a, b, self.expr(d, False))
        if form in ("let", "let*", "letrec"):
            return "(%s ((x %s) (y %s)) %s)" % (form, a, b, self.expr(d, False))
        return "(begin %s %s)" % (a, b)

    def top_level(self):
        r = self.rng.random()
        if r < 0.18:
            name = self.rng.choice(SAFE_SYMBOLS).replace("?", "").replace("!", "")
            return "(define %s %s)" % (name, self.expr(4, False))
        if r < 0.28:
            arg = self.rng.choice(["x", "y", "args"])
            return ("(define (%s %s) %s)"
                    % (self.rng.choice(["f", "g", "helper"]), arg, self.expr(4, False)))
        if r < 0.36:
            return ("(defmacro m%s () %s)"
                    % (self.rng.randint(0, 99), self.expr(4, False)))
        if r < 0.44:
            return "(lambda (x) %s)" % self.expr(4, False)
        return self.expr(6)

    def case_text(self):
        forms = []
        for _ in range(self.rng.randint(1, 6)):
            forms.append(self.top_level())
            if self.rng.random() < 0.15:
                forms.append("; separator\n")
        return "\n".join(forms) + "\n"

    # ── mutation + bytes modes ──────────────────────────────────────
    def mutate_case(self):
        base = bytearray(self.rng.choice(self.corpus))
        for _ in range(self.rng.randint(1, 8)):
            if not base:
                break
            op = self.rng.random()
            pos = self.rng.randrange(len(base))
            if op < 0.25:
                base[pos] ^= 1 << self.rng.randrange(8)
            elif op < 0.45:
                base[pos] = self.rng.randrange(256)
            elif op < 0.60:
                ln = min(self.rng.randint(1, 24), len(base) - pos)
                del base[pos:pos + ln]
            elif op < 0.75 and pos + 4 < len(base):
                ln = min(self.rng.randint(1, 24), len(base) - pos)
                base[pos:pos] = base[pos:pos + ln]  # duplicate a chunk
            elif op < 0.90:
                tok = self.rng.choice(STRUCTURAL_TOKENS)
                base[pos:pos] = tok
            else:
                other = self.rng.choice(self.corpus)
                cut = self.rng.randrange(len(other) + 1) if other else 0
                base[pos:pos] = other[:cut]  # splice from another seed
        return bytes(base)

    def bytes_case(self):
        n = self.rng.randint(1, 1024)
        if self.rng.random() < 0.7:
            # Biased toward printable + structural characters: pure
            # uniform bytes almost always die in lex's whitespace scan
            # having tested nothing.
            pool = (b"()[]'`\",;#. \n\t0123456789abcXYZ+-*/\\#"
                    b"!?$%&:=<>{}|~^_@" + b"".join(STRUCTURAL_TOKENS))
            return bytes(self.rng.choice(pool) for _ in range(n))
        return bytes(self.rng.randrange(256) for _ in range(n))

    def next_case(self, mode):
        if mode == "mix":
            mode = self.rng.choice(["grammar", "mutate", "bytes"])
        if mode == "grammar":
            return self.case_text().encode()
        if mode == "mutate":
            return self.mutate_case()
        return self.bytes_case()


# ── harness driving ──────────────────────────────────────────────────
#
# Status taxonomy (drives what happens to an isolated case):
#   ok      - every case processed cleanly (per-case read/eval errors are
#             expected noise)
#   crash   - signal death, ASAN report, arbitrary exit code from fuzzed
#             code escaping through (exit ...), or missing DONE marker -
#             THE finding class, always fails the run
#   timeout - watchdog/driver deadline hit - almost always a legitimately
#             infinite loop the generator wrote ((while :kw ...) is valid,
#             eternal yas); quarantined under build/fuzz/timeouts/ and
#             reported, but does NOT fail the run

def classify(proc_stderr, returncode):
    if b"TIMEOUT (watchdog)" in proc_stderr:
        return "timeout"
    return "crash"


def run_batch(files, timeout_secs):
    """Run the harness once over `files`. Returns (status, stderr).
    `ok` requires BOTH exit status 0 and the trailing DONE marker:
    fuzzed code can escape through (exit 0), which looks clean without
    the marker."""
    cmd = [str(HARNESS), "--timeout", str(timeout_secs)] + [str(f) for f in files]
    budget = timeout_secs * len(files) + 15
    try:
        proc = subprocess.run(cmd, capture_output=True, timeout=budget)
    except subprocess.TimeoutExpired as e:
        return "timeout", (e.stderr or b"") + b"\n(driver-level timeout)"
    if proc.returncode == 0 and any(l.startswith(b"DONE ") for l in proc.stderr.splitlines()):
        return "ok", proc.stderr
    return classify(proc.stderr, proc.returncode), proc.stderr


def isolate_all(batch, timeout_secs):
    """A batch died: recursively bisect it, collecting EVERY offending
    case (not just the first - a hang early in a batch must not mask a
    crash later in it). Uses a shortened deadline: infinite loops and
    wedges reveal themselves immediately, and 5s-per-single would make
    each hung batch cost minutes."""
    iso_timeout = max(1, timeout_secs // 3)
    if len(batch) == 1:
        status, _ = run_batch(batch, iso_timeout)
        return [] if status == "ok" else [(batch[0], status)]
    mid = len(batch) // 2
    culprits = []
    for half in (batch[:mid], batch[mid:]):
        status, _ = run_batch(half, iso_timeout)
        if status != "ok":
            culprits += isolate_all(half, timeout_secs)
    return culprits


def main():
    ap = argparse.ArgumentParser(description="yas reader/evaluator fuzzer")
    ap.add_argument("--seed", type=int, default=1, help="PRNG seed (default 1)")
    ap.add_argument("--cases", type=int, default=5000, help="cases to run (default 5000)")
    ap.add_argument("--mode", choices=["mix", "grammar", "mutate", "bytes"], default="mix")
    ap.add_argument("--batch", type=int, default=100, help="cases per harness invocation")
    ap.add_argument("--timeout", type=int, default=5, help="watchdog secs per case")
    ap.add_argument("--corpus-dir", type=Path, default=CORPUS_DIR)
    ap.add_argument("--self-test", action="store_true",
                    help="run fixed hostile inputs + determinism checks, no fuzzing")
    args = ap.parse_args()

    if args.self_test:
        sys.exit(self_test(args))

    if not HARNESS.exists():
        sys.exit(f"fuzz.py: {HARNESS} not found - build it first with `make fuzz`")

    corpus = sorted(args.corpus_dir.glob("*.ys"))
    if args.mode in ("mutate", "mix") and not corpus:
        sys.exit(f"fuzz.py: no seed corpus (*.ys) in {args.corpus_dir}")
    corpus_bytes = [f.read_bytes() for f in corpus]

    CASES_DIR.mkdir(parents=True, exist_ok=True)
    FINDINGS_DIR.mkdir(parents=True, exist_ok=True)
    TIMEOUTS_DIR.mkdir(parents=True, exist_ok=True)

    import random
    gen = Gen(random.Random(args.seed), corpus_bytes)

    started = time.time()
    crashes = []
    timeouts = []
    i = 0
    while i < args.cases:
        n = min(args.batch, args.cases - i)
        batch = []
        for j in range(n):
            data = gen.next_case(args.mode)
            path = CASES_DIR / f"seed{args.seed}_case{i + j:06d}.ys"
            path.write_bytes(data)
            batch.append(path)
        status, err = run_batch(batch, args.timeout)
        if status != "ok":
            # Bisection re-runs sub-batches, so a case that never got its
            # turn in the failed invocation still runs here; generation
            # order (and thus reproducibility) is unaffected.
            for culprit, kind in isolate_all(batch, args.timeout):
                dest_dir = TIMEOUTS_DIR if kind == "timeout" else FINDINGS_DIR
                dest = dest_dir / culprit.name
                shutil.copy(culprit, dest)
                (dest_dir / (culprit.name + ".stderr")).write_bytes(err)
                if kind == "timeout":
                    timeouts.append(culprit.name)
                    print(f"timeout: {culprit.name} (quarantined in fuzz-findings/timeouts/)", flush=True)
                else:
                    crashes.append(culprit.name)
                    print(f"FINDING: {culprit.name} (saved to fuzz-findings/)", flush=True)
        i += n

    dt = time.time() - started
    print(f"seed={args.seed} mode={args.mode}: {args.cases} cases in {dt:.1f}s "
          f"({args.cases / max(dt, 0.001):.0f}/s), "
          f"{len(crashes)} crash(es), {len(timeouts)} timeout(s)")
    if crashes:
        print("crash artifacts in fuzz-findings/ - reproduce with "
              "./yas <file> or build/fuzz_harness <file>")
        return 1
    return 0


# ── self-test ────────────────────────────────────────────────────────

HOSTILE_CASES = {
    # The historical stack-smash shapes: must now be clean syntax errors
    "deep-parens": b"(" * 100000 + b")" * 100000,
    "deep-brackets": b"[" * 100000,
    "deep-quote": b"'" * 200000 + b"x",
    "deep-quasiquote": b"`" * 200000 + b"x",
    "deep-comment-run": b";c\n" * 200000,
    "unterminated-list": b"(+ 1 2",
    "unterminated-string": b'"abc',
    "bad-char-literal": b"#\\zzzzz9",
    "interp-abuse": b'"{{{{{{{{{{{{{{{{{{{{{{{{}}}}}}}}}}}}}}}}}}}}}}}}}"\n',
    "high-bytes": bytes(range(128, 256)) * 64,
    "dot-abuse": b"(. . . .)\n",
    "hash-soup": b"#vec(#hash(#mat(#(" * 500,
}


def self_test(args):
    """Fixed hostile inputs must produce clean errors, never crashes;
    generation must be deterministic for a given seed. Needs the harness
    built (make fuzz)."""
    if not HARNESS.exists():
        sys.exit(f"fuzz.py: {HARNESS} not found - build it first with `make fuzz`")
    CASES_DIR.mkdir(parents=True, exist_ok=True)
    failed = 0
    for name, data in HOSTILE_CASES.items():
        path = CASES_DIR / f"selftest_{name}.ys"
        path.write_bytes(data)
        ok, err = run_batch([path], args.timeout)
        status = "PASS" if ok else "FAIL"
        if not ok:
            failed += 1
        print(f"  [{status}] self-test {name}")

    import random
    corpus_bytes = [f.read_bytes() for f in sorted(CORPUS_DIR.glob("*.ys"))]
    g1 = Gen(random.Random(7), corpus_bytes)
    g2 = Gen(random.Random(7), corpus_bytes)
    det = all(g1.next_case("mix") == g2.next_case("mix") for _ in range(200))
    print(f"  [{'PASS' if det else 'FAIL'}] generation determinism (seed 7, 200 cases)")
    return 1 if (failed or not det) else 0


if __name__ == "__main__":
    sys.exit(main())

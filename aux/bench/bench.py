#!/usr/bin/env python3
"""Cross-language micro-benchmark driver for aux/bench/ (ticket #29,
docs/BENCHMARKING.md).

Times the same small workload across yas and a few reference languages, and
(as a WIP placeholder - see bench_aot() below) yas's AOT-compiled output
against native C.

Usage:
    make bench
    python3 aux/bench/bench.py [--runs N] [--workload NAME ...] [--no-aot]

Exit status: 0 if every *present* runtime's output matched what the
workload expects (a missing runtime is reported as "skipped", not a
failure); 1 if a present runtime produced the wrong output.
"""
import argparse
import shutil
import subprocess
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
YAS_BIN = REPO_ROOT / "yas"
WORKLOADS_DIR = Path(__file__).resolve().parent / "workloads"
BUILD_DIR = REPO_ROOT / "build"  # gitignored - compiled variants land here

# Each workload's expected stdout is hardcoded here (not derived from any
# one variant's output) so a broken variant can't post a fake fast time by
# silently producing wrong results - same idea as the Makefile's `test-aot`
# checking build/fib's exact output.
WORKLOADS = {
    "fib": {"expected": "46368\n"},
    "alloc": {"expected": "1250025000\n"},
}

# (runtime name, file extension, argv-builder for a given workload path).
INTERPRETED_VARIANTS = [
    ("yas", "ys", lambda p: [str(YAS_BIN), str(p)]),
    ("python3", "py", lambda p: ["python3", str(p)]),
    ("ruby", "rb", lambda p: ["ruby", str(p)]),
    ("lua", "lua", lambda p: ["lua", str(p)]),
]


def which_ok(cmd):
    return shutil.which(cmd) is not None


def best_of(argv, runs, expect_stdout=None, cwd=None):
    """Run argv `runs` times, return (best_seconds, stdout_of_last_run).
    Raises RuntimeError if the process fails, or if expect_stdout is given
    and doesn't match."""
    best = None
    out = None
    for _ in range(runs):
        start = time.perf_counter()
        proc = subprocess.run(argv, cwd=cwd, capture_output=True, text=True)
        elapsed = time.perf_counter() - start
        if proc.returncode != 0:
            raise RuntimeError(
                f"{' '.join(argv)} exited {proc.returncode}: {proc.stderr.strip()}"
            )
        out = proc.stdout
        if best is None or elapsed < best:
            best = elapsed
    if expect_stdout is not None and out != expect_stdout:
        raise RuntimeError(f"{' '.join(argv)}: expected {expect_stdout!r}, got {out!r}")
    return best, out


def bench_interpreted(workload, runs, results):
    expected = WORKLOADS[workload]["expected"]
    for name, ext, make_argv in INTERPRETED_VARIANTS:
        path = WORKLOADS_DIR / workload / f"{workload}.{ext}"
        if not path.exists():
            continue
        available = YAS_BIN.exists() if name == "yas" else which_ok(name)
        if not available:
            results.append((workload, name, "skipped", None))
            continue
        try:
            elapsed, _ = best_of(make_argv(path), runs, expect_stdout=expected)
            results.append((workload, name, f"{elapsed:.3f}s", None))
        except RuntimeError as e:
            results.append((workload, name, "FAILED", str(e)))


def bench_c(workload, runs, results):
    expected = WORKLOADS[workload]["expected"]
    src = WORKLOADS_DIR / workload / f"{workload}.c"
    if not src.exists():
        return
    if not which_ok("cc"):
        results.append((workload, "c", "skipped", None))
        return
    BUILD_DIR.mkdir(exist_ok=True)
    out_bin = BUILD_DIR / f"bench_{workload}_c"
    compile_proc = subprocess.run(
        ["cc", "-O2", "-o", str(out_bin), str(src)], capture_output=True, text=True
    )
    if compile_proc.returncode != 0:
        results.append((workload, "c", "FAILED", compile_proc.stderr.strip()))
        return
    try:
        elapsed, _ = best_of([str(out_bin)], runs, expect_stdout=expected)
        results.append((workload, "c", f"{elapsed:.3f}s", None))
    except RuntimeError as e:
        results.append((workload, "c", "FAILED", str(e)))


# --- AOT row -------------------------------------------------------------
# PLACEHOLDER - tracked by a follow-up ticket to ticket #29 ("replace the
# placeholder AOT bench row with a real AOT-vs-C comparison"). The AOT
# pipeline currently only handles the subset of yas exercised by
# aot/examples/fib.ys (fib(30), no imports - see docs/AOT.md), and this
# repo's `make aot` is hardwired to that example and aot/fib_main.c. Rather
# than teach the AOT pass a second entry point just for this bench, this row
# shells out to the existing `make aot` and times its output
# ("fib(30) = 832040", NOT the fib workload's fib(24) = 46368 above - the two
# are not comparable inputs, only "yas AOT" vs "native C" at the *same*
# n=30 is). Replace with a dedicated AOT bench target once the pipeline
# covers more of the language.
AOT_WORKLOAD = "fib (n=30, AOT)"
AOT_EXPECTED = "fib(30) = 832040\n"
# Native C reference at the same n=30, kept inline (not a tracked file)
# since it exists only to give this one placeholder row something to
# compare against - aux/bench/workloads/fib/fib.c is the real, tracked
# native-C reference for the fib workload proper (n=24).
AOT_C_REF_SRC = (
    "#include <stdio.h>\n"
    "static long fib(long n){return n<2?n:fib(n-1)+fib(n-2);}\n"
    'int main(void){printf("fib(30) = %ld\\n", fib(30));return 0;}\n'
)


def bench_aot(runs, results):
    if not which_ok("cccc"):
        results.append((AOT_WORKLOAD, "yas-aot", "skipped", None))
        results.append((AOT_WORKLOAD, "c", "skipped", None))
        return

    make_proc = subprocess.run(["make", "aot"], cwd=REPO_ROOT, capture_output=True, text=True)
    if make_proc.returncode != 0:
        results.append((AOT_WORKLOAD, "yas-aot", "FAILED", make_proc.stderr.strip()))
    else:
        try:
            elapsed, _ = best_of([str(BUILD_DIR / "fib")], runs, expect_stdout=AOT_EXPECTED)
            results.append((AOT_WORKLOAD, "yas-aot", f"{elapsed:.3f}s", None))
        except RuntimeError as e:
            results.append((AOT_WORKLOAD, "yas-aot", "FAILED", str(e)))

    if not which_ok("cc"):
        results.append((AOT_WORKLOAD, "c", "skipped", None))
        return
    BUILD_DIR.mkdir(exist_ok=True)
    tmp_c = BUILD_DIR / "bench_fib30_ref.c"
    tmp_bin = BUILD_DIR / "bench_fib30_ref"
    tmp_c.write_text(AOT_C_REF_SRC)
    compile_proc = subprocess.run(
        ["cc", "-O2", "-o", str(tmp_bin), str(tmp_c)], capture_output=True, text=True
    )
    if compile_proc.returncode != 0:
        results.append((AOT_WORKLOAD, "c", "FAILED", compile_proc.stderr.strip()))
        return
    try:
        elapsed, _ = best_of([str(tmp_bin)], runs, expect_stdout=AOT_EXPECTED)
        results.append((AOT_WORKLOAD, "c", f"{elapsed:.3f}s", None))
    except RuntimeError as e:
        results.append((AOT_WORKLOAD, "c", "FAILED", str(e)))


def bench_startup(runs, results):
    """Empty-program startup cost per runtime - its own column, never
    subtracted from a workload's total (two noisy measurements subtracted
    would just invent precision)."""
    empty_ys = WORKLOADS_DIR / "empty.ys"
    baselines = [
        ("yas", [str(YAS_BIN), str(empty_ys)], YAS_BIN.exists()),
        ("python3", ["python3", "-c", "pass"], which_ok("python3")),
        ("ruby", ["ruby", "-e", ""], which_ok("ruby")),
        ("lua", ["lua", "-e", ""], which_ok("lua")),
    ]
    for name, argv, available in baselines:
        if not available:
            results.append(("startup", name, "skipped", None))
            continue
        try:
            elapsed, _ = best_of(argv, runs)
            results.append(("startup", name, f"{elapsed:.3f}s", None))
        except RuntimeError as e:
            results.append(("startup", name, "FAILED", str(e)))


def print_table(results):
    rows = [("workload", "variant", "best-of-N", "note")] + [
        (w, v, t, (n or "")[:60]) for w, v, t, n in results
    ]
    widths = [max(len(r[i]) for r in rows) for i in range(4)]
    for i, row in enumerate(rows):
        print("  ".join(cell.ljust(widths[j]) for j, cell in enumerate(row)))
        if i == 0:
            print("  ".join("-" * w for w in widths))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runs", type=int, default=3, help="best-of-N (default: 3)")
    parser.add_argument(
        "--workload", action="append", choices=sorted(WORKLOADS), default=None,
        help="restrict to one or more workloads (default: all)",
    )
    parser.add_argument("--no-aot", action="store_true", help="skip the placeholder AOT row")
    args = parser.parse_args()

    workloads = args.workload or sorted(WORKLOADS)
    results = []

    bench_startup(args.runs, results)
    for w in workloads:
        bench_interpreted(w, args.runs, results)
        bench_c(w, args.runs, results)
    if "fib" in workloads and not args.no_aot:
        bench_aot(args.runs, results)

    print_table(results)
    failed = any(status == "FAILED" for _, _, status, _ in results)
    if failed:
        print("\nbench.py: FAILED (a present runtime produced the wrong output)")
        return 1
    print("\nbench.py: done")
    return 0


if __name__ == "__main__":
    sys.exit(main())

/* fuzz_harness - in-process runner for reader/evaluator fuzz cases
 * (docs/FUZZING.md, the C half of the aux/fuzz_*.ys convention).
 *
 * Usage: build/fuzz_harness [--timeout SECS] [--verbose] <case-file>...
 *
 * For each case file: create a fresh context, import yas/base, disable
 * the I/O sandbox (fuzzed input must never touch the real filesystem),
 * then run the same top-level read/eval loop as main.c's load_file() -
 * including YAS_ERROR_SKIP semantics for #+/#- conditionals.
 *
 * Design contract with the Python driver (aux/fuzz/fuzz.py):
 *   - Per-case read/eval errors are EXPECTED noise: they are counted,
 *     not reported (unless --verbose), and never affect the exit status.
 *   - A finding is anything that kills the process early: signal death,
 *     an ASAN report, a watchdog timeout, or fuzzed code calling (exit).
 *   - So the harness prints "DONE <cases> <errors>" to stderr after the
 *     last case; a driver that doesn't see that line knows this batch
 *     died inside some case and bisects by re-running sub-batches.
 *   - The watchdog re-arms per case; SIGALRM reports the offending file
 *     via write() (async-signal-safe) and _exit()s.
 *
 * stdout is redirected to /dev/null unless --verbose: fuzzed code calls
 * print/display freely, and that output would drown an ASAN report.
 *
 * Build: cc -std=c99 -Wall -D_GNU_SOURCE -iquote src -o build/fuzz_harness
 *          aux/fuzz_harness.c -L. -lyas -Wl,-rpath,.   (the Makefile's
 *        `fuzz` target)
 */
#include "yas.h"

#include <execinfo.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *g_current_case = "(none)";
static int g_verbose = 0;
static YasContext *g_ctx = NULL;

/* Crash reporter: name the case, dump a C backtrace, die. Not
 * async-signal-safe in the strict sense (backtrace_symbols_fd may
 * allocate), but this only runs on a faulting path that is about to
 * _exit anyway - and a symbolic trace beats a bare exit code when
 * triaging a finding. */
static void crash_handler(int sig)
{
    static const char msg[] = "\nfuzz_harness: FATAL SIGNAL in case: ";
    ssize_t r1 = write(STDERR_FILENO, msg, sizeof(msg) - 1);
    const char *p = g_current_case;
    while (*p)
        p++;
    ssize_t r2 = write(STDERR_FILENO, g_current_case, (size_t)(p - g_current_case));
    ssize_t r3 = write(STDERR_FILENO, "\n", 1);
    (void)r1;
    (void)r2;
    (void)r3;
    void *bt[32];
    int n = backtrace(bt, 32);
    backtrace_symbols_fd(bt, n, STDERR_FILENO);
    _exit(139);
}

/* Async-signal-safe: write()s the current case path, then _exit(). The
 * driver treats exit code 2 as "watchdog fired on this exact file". */
static void watchdog_handler(int sig)
{
    (void)sig;
    static const char msg[] = "\nfuzz_harness: TIMEOUT (watchdog) in case: ";
    ssize_t r1 = write(STDERR_FILENO, msg, sizeof(msg) - 1);
    const char *p = g_current_case;
    while (*p)
        p++;
    ssize_t r2 = write(STDERR_FILENO, g_current_case, (size_t)(p - g_current_case));
    ssize_t r3 = write(STDERR_FILENO, "\n", 1);
    (void)r1;
    (void)r2;
    (void)r3;
    _exit(2);
}

/* Run one case file in a fresh context. Returns 0 on clean processing
 * (read/eval errors included - they are expected), nonzero only if the
 * file couldn't be read at all. */
static int run_case(const char *path)
{
    YasAtom env;
    char *text;

    g_ctx = yas_context_create(); /* creates AND binds */

    /* Fuzzed input must not touch the real filesystem even if it mutates
     * into an (open-input-file ...) form seeded from corpus content. */
    yas_io_sandbox_init(YAS_IO_SANDBOX_DISABLED);

    text = yas_slurp(path);
    if (!text) {
        fprintf(stderr, "fuzz_harness: cannot read %s\n", path);
        return 1;
    }

    env = yas_env_create(yas_nil);
    yas_gc_push_root(&env);

    /* Same builtins layer every top-level env gets under ./yas (main.c),
     * so eval-side coverage includes yas/base's variadic closures over
     * the arithmetic primitives, macros, etc. */
    YasAtom import_args = yas_list_create(1, yas_make_sym("YAS/BASE"));
    YasAtom ignore;
    yas_gc_push_root(&import_args);
    yas_module_import(import_args, env, &ignore);
    yas_gc_pop_roots(1);

    const char *saved_path = yas_current_file_path;
    yas_current_file_path = path;
    const char *p = text;
    while (1) {
        YasErrorContext err = {0};
        err.pos.filename = path;
        err.pos.source = text;

        YasAtom expr = yas_nil;
        YasError read_err = yas_read_expr(p, &p, &expr, &err);
        if (read_err == YAS_ERROR_SKIP)
            continue; /* feature conditional excluded this form */
        if (read_err) {
            if (err.message[0] != '\0' && g_verbose)
                fprintf(stderr, "  read error in %s: %s\n", path, err.message);
            break; /* empty message = end of input */
        }

        /* Root both locals across yas_eval_expr(): unlike main.c we make
         * no assumption about which allocations the evaluator performs
         * before it installs its own roots (docs/GC.md). */
        yas_gc_push_root(&expr);
        YasAtom result = yas_nil;
        yas_gc_push_root(&result);
        YasError eval_err = yas_eval_expr(expr, env, &result);
        yas_gc_pop_roots(2);
        if (eval_err) {
            yas_clear_exception(); /* expected noise; consume it */
            if (g_verbose)
                fprintf(stderr, "  eval error %d in %s\n", (int)eval_err, path);
        }
    }

    yas_current_file_path = saved_path;
    yas_gc_pop_roots(1); /* env */
    free(text);
    yas_context_destroy(g_ctx);
    g_ctx = NULL;
    return 0; /* read/eval errors are expected noise; never fail a batch */
}

int main(int argc, char **argv)
{
    int timeout_secs = 5;
    int nfiles = 0;
    const char **files = malloc(sizeof(char *) * (size_t)argc);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc) {
            timeout_secs = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--verbose") == 0) {
            g_verbose = 1;
        } else {
            files[nfiles++] = argv[i];
        }
    }
    if (nfiles == 0) {
        fprintf(stderr, "usage: fuzz_harness [--timeout SECS] [--verbose] <case-file>...\n");
        return 1;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = watchdog_handler;
    sigaction(SIGALRM, &sa, NULL);
    sa.sa_handler = crash_handler;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);

    /* Silence fuzzed code's print/display output unless asked for - an
     * ASAN report or crash trace must be findable in stderr/stdout. */
    if (!g_verbose && freopen("/dev/null", "w", stdout) == NULL) {
        fprintf(stderr, "fuzz_harness: cannot silence stdout\n");
        return 1;
    }

    for (int i = 0; i < nfiles; i++) {
        g_current_case = files[i];
        alarm((unsigned)timeout_secs);
        run_case(files[i]);
        alarm(0);
    }

    fprintf(stderr, "DONE %d\n", nfiles);
    free(files);
    return 0;
}

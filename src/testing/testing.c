/* The test runner: Go's src/testing/testing.go, benchmark.go and fuzz.go.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "corpus.h"
#include "match.h"

#include "burrow/chan.h"
#include "burrow/context.h"
#include "burrow/core.h"
#include "burrow/defer.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/func.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/note.h"
#include "burrow/pal.h"
#include "burrow/panic.h"
#include "burrow/proc.h"
#include "burrow/sched.h"
#include "burrow/slice.h"
#include "burrow/strconv.h"
#include "burrow/sync.h"
#include "burrow/testing.h"
#include "burrow/thread.h"
#include "burrow/time.h"
#include "burrow/type.h"
#include "burrow/utf8.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---------------------------------------------------------------- buffers */

/* An allocation the runner cannot go on without. Most of the calls that need
 * memory have no Error to hand it back through, and Go's runtime ends a test
 * binary that runs out of memory, so this panics. */
static void *must_alloc(Alloc *a, size_t size, size_t align) {
    void *p = mem_alloc(a, size, align);
    if (p == NULL)
        panic_str(BURROW_S("testing: out of memory"));
    return p;
}

static void *must_realloc(Alloc *a, void *old, size_t old_size, size_t size,
                          size_t align) {
    void *p = mem_realloc(a, old, old_size, size, align);
    if (p == NULL)
        panic_str(BURROW_S("testing: out of memory"));
    return p;
}

typedef struct Buf {
    Byte *p;
    Int len;
    Int cap;
} Buf;

static void buf_append(Buf *b, const void *p, Int n) {
    if (n <= 0)
        return;
    if (b->len + n > b->cap) {
        Int ncap = b->cap == 0 ? 64 : b->cap;
        while (ncap < b->len + n)
            ncap *= 2;
        b->p = (Byte *)must_realloc(heap_allocator(), b->p, (size_t)b->cap,
                                    (size_t)ncap, 1);
        b->cap = ncap;
    }
    memcpy(b->p + b->len, p, (size_t)n);
    b->len += n;
}

static void buf_str(Buf *b, Str s) {
    buf_append(b, s.p, s.len);
}

static void buf_free(Buf *b) {
    if (b->p != NULL)
        mem_free(heap_allocator(), b->p, (size_t)b->cap, 1);
    *b = (Buf){0};
}

static void buf_release(void *b) {
    buf_free((Buf *)b);
}

/* Frees a name that full_name cloned onto the heap. */
static void str_release(Str s) {
    if (s.len > 0)
        mem_free(heap_allocator(), (void *)(uintptr_t)s.p, (size_t)s.len, 1);
}

static Str buf_view(const Buf *b) {
    return str_from_bytes(b->p, b->len);
}

/* ------------------------------------------------------------- durations
 *
 * time.Duration's String, Round and ParseDuration, which the flags and the
 * timeout message need and which time does not have yet. */

static int frac(char *buf, int w, uint64_t *v, int prec) {
    bool print = false;
    for (int i = 0; i < prec; i++) {
        uint64_t digit = *v % 10;
        print = print || digit != 0;
        if (print)
            buf[--w] = (char)('0' + digit);
        *v /= 10;
    }
    if (print)
        buf[--w] = '.';
    return w;
}

static int uint_digits(char *buf, int w, uint64_t v) {
    if (v == 0) {
        buf[--w] = '0';
        return w;
    }
    while (v > 0) {
        buf[--w] = (char)('0' + v % 10);
        v /= 10;
    }
    return w;
}

/* Duration.String, into the 32 bytes at buf. */
static Str dur_string(int64_t d, char buf[32]) {
    int w = 32;
    uint64_t u = (uint64_t)d;
    bool neg = d < 0;
    if (neg)
        u = 0 - u;
    if (u < (uint64_t)TIME_SECOND) {
        int prec;
        buf[--w] = 's';
        w--;
        if (u == 0) {
            buf[w] = '0';
            return str_from_bytes((const Byte *)buf + w, 32 - w);
        }
        if (u < (uint64_t)TIME_MICROSECOND) {
            prec = 0;
            buf[w] = 'n';
        } else if (u < (uint64_t)TIME_MILLISECOND) {
            prec = 3;
            w--;
            /* U+00B5, as Go writes it. Two bytes of a buffer, not a string.
             * NOLINTNEXTLINE(bugprone-not-null-terminated-result) */
            memcpy(buf + w, "\xC2\xB5", 2);
        } else {
            prec = 6;
            buf[w] = 'm';
        }
        w = frac(buf, w, &u, prec);
        w = uint_digits(buf, w, u);
    } else {
        buf[--w] = 's';
        w = frac(buf, w, &u, 9);
        w = uint_digits(buf, w, u % 60);
        u /= 60;
        if (u > 0) {
            buf[--w] = 'm';
            w = uint_digits(buf, w, u % 60);
            u /= 60;
            if (u > 0) {
                buf[--w] = 'h';
                w = uint_digits(buf, w, u);
            }
        }
    }
    if (neg)
        buf[--w] = '-';
    return str_from_bytes((const Byte *)buf + w, 32 - w);
}

/* Duration.Round for a positive m. */
static int64_t dur_round(int64_t d, int64_t m) {
    int64_t r = d % m;
    if (d < 0) {
        r = -r;
        if ((uint64_t)r + (uint64_t)r < (uint64_t)m)
            return d + r;
        int64_t d1 = d - m + r;
        return d1 < d ? d1 : INT64_MIN;
    }
    if ((uint64_t)r + (uint64_t)r < (uint64_t)m)
        return d - r;
    if (d <= INT64_MAX - (m - r))
        return d + m - r;
    return INT64_MAX;
}

/* time.ParseDuration, answering only whether it worked, since the flag
 * package throws the message away and says "parse error". */
static bool dur_parse(Str s, int64_t *out) {
    const uint64_t limit = (uint64_t)1 << 63;
    uint64_t d = 0;
    bool neg = false;
    Int i = 0;
    if (s.len > 0 && (s.p[0] == '-' || s.p[0] == '+')) {
        neg = s.p[0] == '-';
        i = 1;
    }
    if (s.len - i == 1 && s.p[i] == '0') {
        *out = 0;
        return true;
    }
    if (i == s.len)
        return false;
    while (i < s.len) {
        uint64_t v = 0;
        uint64_t f = 0;
        double scale = 1;
        if (!(s.p[i] == '.' || (s.p[i] >= '0' && s.p[i] <= '9')))
            return false;
        Int pl = i;
        for (; i < s.len && s.p[i] >= '0' && s.p[i] <= '9'; i++) {
            if (v > limit / 10)
                return false;
            v = v * 10 + (uint64_t)(s.p[i] - '0');
            if (v > limit)
                return false;
        }
        bool pre = pl != i;
        bool post = false;
        if (i < s.len && s.p[i] == '.') {
            i++;
            Int fl = i;
            bool overflow = false;
            for (; i < s.len && s.p[i] >= '0' && s.p[i] <= '9'; i++) {
                if (overflow)
                    continue;
                if (f > (limit - 1) / 10) {
                    overflow = true;
                    continue;
                }
                uint64_t y = f * 10 + (uint64_t)(s.p[i] - '0');
                if (y > limit) {
                    overflow = true;
                    continue;
                }
                f = y;
                scale *= 10;
            }
            post = fl != i;
        }
        if (!pre && !post)
            return false;
        Int us = i;
        while (i < s.len && s.p[i] != '.' && !(s.p[i] >= '0' && s.p[i] <= '9'))
            i++;
        if (us == i)
            return false;
        Str u = str_from_bytes(s.p + us, i - us);
        uint64_t unit;
        if (str_eq(u, BURROW_S("ns")))
            unit = 1;
        else if (str_eq(u, BURROW_S("us")) || str_eq(u, BURROW_S("\xC2\xB5s")) ||
                 str_eq(u, BURROW_S("\xCE\xBCs")))
            unit = 1000;
        else if (str_eq(u, BURROW_S("ms")))
            unit = 1000000;
        else if (str_eq(u, BURROW_S("s")))
            unit = 1000000000;
        else if (str_eq(u, BURROW_S("m")))
            unit = 60000000000ULL;
        else if (str_eq(u, BURROW_S("h")))
            unit = 3600000000000ULL;
        else
            return false;
        if (v > limit / unit)
            return false;
        v *= unit;
        if (f > 0) {
            v += (uint64_t)((double)f * ((double)unit / scale));
            if (v > limit)
                return false;
        }
        d += v;
        if (d > limit)
            return false;
    }
    if (neg) {
        *out = (int64_t)(0 - d);
        return true;
    }
    if (d > limit - 1)
        return false;
    *out = (int64_t)d;
    return true;
}

/* fmtDuration, "87.00s". */
static Str fmt_dur(Alloc *a, int64_t d) {
    int64_t whole = d / TIME_SECOND;
    double secs = (double)whole + (double)(d % TIME_SECOND) / 1e9;
    return fmt_sprintf_v(a, "%.2fs", secs);
}

/* ------------------------------------------------------------------ flags
 *
 * The flags a Go test binary registers, with Go's usage text, so that -h
 * prints what Go prints. The ones for profiles, coverage and fuzzing are
 * accepted and do nothing. */

enum {
    FK_BOOL,
    FK_STRING,
    FK_INT,
    FK_UINT,
    FK_DURATION,
    FK_DURCOUNT, /* testing's durationOrCountFlag */
    FK_CHATTY
};

enum {
    F_ARTIFACTS,
    F_BENCH,
    F_BENCHMEM,
    F_BENCHTIME,
    F_BLOCKPROFILE,
    F_BLOCKPROFILERATE,
    F_COUNT,
    F_COVERPROFILE,
    F_CPU,
    F_CPUPROFILE,
    F_FAILFAST,
    F_FULLPATH,
    F_FUZZ,
    F_FUZZCACHEDIR,
    F_FUZZMINIMIZETIME,
    F_FUZZTIME,
    F_FUZZWORKER,
    F_GOCOVERDIR,
    F_LIST,
    F_MEMPROFILE,
    F_MEMPROFILERATE,
    F_MUTEXPROFILE,
    F_MUTEXPROFILEFRACTION,
    F_OUTPUTDIR,
    F_PANICONEXIT0,
    F_PARALLEL,
    F_RUN,
    F_SHORT,
    F_SHUFFLE,
    F_SKIP,
    F_TESTLOGFILE,
    F_TIMEOUT,
    F_TRACE,
    F_V,
    F_COUNT_OF_FLAGS
};

typedef struct FlagDef {
    const char *name;
    const char *def; /* NULL for the parallel default, which is computed */
    const char *usage;
    int kind;
    bool allow_zero;
} FlagDef;

/* Sorted by name, which is the order -h prints them in. */
static const FlagDef flag_defs[F_COUNT_OF_FLAGS] = {
    {"test.artifacts", "false", "store test artifacts in test.,outputdir", FK_BOOL,
     false},
    {"test.bench", "", "run only benchmarks matching `regexp`", FK_STRING, false},
    {"test.benchmem", "false", "print memory allocations for benchmarks", FK_BOOL,
     false},
    {"test.benchtime", "1s",
     "run each benchmark for duration `d` or N times if `d` is of the form Nx",
     FK_DURCOUNT, false},
    {"test.blockprofile", "", "write a goroutine blocking profile to `file`", FK_STRING,
     false},
    {"test.blockprofilerate", "1",
     "set blocking profile `rate` (see runtime.SetBlockProfileRate)", FK_INT, false},
    {"test.count", "1", "run tests and benchmarks `n` times", FK_UINT, false},
    {"test.coverprofile", "", "write a coverage profile to `file`", FK_STRING, false},
    {"test.cpu", "", "comma-separated `list` of cpu counts to run each test with",
     FK_STRING, false},
    {"test.cpuprofile", "", "write a cpu profile to `file`", FK_STRING, false},
    {"test.failfast", "false", "do not start new tests after the first test failure",
     FK_BOOL, false},
    {"test.fullpath", "false", "show full file names in error messages", FK_BOOL,
     false},
    {"test.fuzz", "", "run the fuzz test matching `regexp`", FK_STRING, false},
    {"test.fuzzcachedir", "",
     "directory where interesting fuzzing inputs are stored (for use only by cmd/go)",
     FK_STRING, false},
    {"test.fuzzminimizetime", "1m0s",
     "time to spend minimizing a value after finding a failing input", FK_DURCOUNT,
     true},
    {"test.fuzztime", "0s", "time to spend fuzzing; default is to run indefinitely",
     FK_DURCOUNT, false},
    {"test.fuzzworker", "false",
     "coordinate with the parent process to fuzz random values (for use only by "
     "cmd/go)",
     FK_BOOL, false},
    {"test.gocoverdir", "", "write coverage intermediate files to this directory",
     FK_STRING, false},
    {"test.list", "",
     "list tests, examples, and benchmarks matching `regexp` then exit", FK_STRING,
     false},
    {"test.memprofile", "", "write an allocation profile to `file`", FK_STRING, false},
    {"test.memprofilerate", "0",
     "set memory allocation profiling `rate` (see runtime.MemProfileRate)", FK_INT,
     false},
    {"test.mutexprofile", "",
     "write a mutex contention profile to the named file after execution", FK_STRING,
     false},
    {"test.mutexprofilefraction", "1",
     "if >= 0, calls runtime.SetMutexProfileFraction()", FK_INT, false},
    {"test.outputdir", "", "write profiles to `dir`", FK_STRING, false},
    {"test.paniconexit0", "false", "panic on call to os.Exit(0)", FK_BOOL, false},
    {"test.parallel", NULL, "run at most `n` tests in parallel", FK_INT, false},
    {"test.run", "", "run only tests and examples matching `regexp`", FK_STRING, false},
    {"test.short", "false", "run smaller test suite to save time", FK_BOOL, false},
    {"test.shuffle", "off", "randomize the execution order of tests and benchmarks",
     FK_STRING, false},
    {"test.skip", "", "do not list or run tests matching `regexp`", FK_STRING, false},
    {"test.testlogfile", "", "write test action log to `file` (for use only by cmd/go)",
     FK_STRING, false},
    {"test.timeout", "0s",
     "panic test binary after duration `d` (default 0, timeout disabled)", FK_DURATION,
     false},
    {"test.trace", "", "write an execution trace to `file`", FK_STRING, false},
    {"test.v", "false", "verbose: print additional output", FK_CHATTY, false},
};

typedef struct FlagValue {
    bool b;
    Str s;
    int64_t i;
    uint64_t u;
    int64_t d;
    int64_t n; /* FK_DURCOUNT: the count, when it was given as Nx */
    bool json; /* FK_CHATTY */
} FlagValue;

/* ------------------------------------------------------------------ types */

typedef struct Chatty {
    SyncMutex mu;
    Buf last_name;
    bool json;
} Chatty;

typedef struct TestState {
    burrow__TestingMatcher *match;
    int64_t deadline;
    bool bare;
    SyncMutex mu;
    Chan *start_parallel;
    int running;
    int num_waiting;
    int max_parallel;
} TestState;

struct TestingT {
    SyncMutex mu;
    Str name;
    TestingT *parent;
    int level;
    TestState *tstate;
    Chatty *chatty;

    Buf output;
    Buf partial;
    bool has_o; /* Go's c.o, which the root does not have */

    TestingT **sub;
    Int nsub;
    Int csub;
    Func *cleanups;
    Int ncleanups;
    Int ccleanups;

    bool cleanup_started;
    bool has_sub;
    bool ran;
    bool failed;
    bool skipped;
    bool done;
    bool finished;
    bool is_parallel;

    int64_t start;
    int64_t duration;

    Context ctx;
    ContextCancelFunc cancel;

    Chan *barrier; /* NULL in bare mode, which is what makes Parallel do nothing */
    Chan *signal;
    bool signal_value;
    bool bench; /* the common part of a TestingB */
    /* Set on a fuzz target's T while a seed runs, which is when most of F's
     * methods are off limits. */
    bool in_fuzz_fn;

    /* Who is running the test function, so that FailNow can tell whether it
     * was called there, and what it panicked with. */
    uintptr_t runner_key;
    bool in_runner;
    bool has_err;
    Any err;

    /* Go's running map, for the timeout message. */
    bool running;
    int64_t running_since;
};

struct TestingM {
    TestingMatchString match;
    TestingInternalTest *tests;
    Int ntests;
    TestingInternalBenchmark *benchmarks;
    Int nbenchmarks;
    TestingInternalFuzzTarget *fuzz_targets;
    Int nfuzz_targets;
    TestingInternalExample *examples;
    Int nexamples;
    int num_run;
    int exit_code;
    bool bare;
};

/* ---------------------------------------------------------- package state
 *
 * Go's package level variables: the flags, the running map, the failure
 * count for -failfast. */
typedef struct Package {
    int argc;
    char **argv;
    bool init_ran;
    bool parsed;
    char parallel_def[24];
    FlagValue flags[F_COUNT_OF_FLAGS];

    /* Every T of the run, for FailNow's question and the timeout message,
     * and so that they can all be freed at the end. */
    SyncMutex reg_mu;
    TestingT **all;
    Int nall;
    Int call;
    uint32_t num_failed;

    int *cpus;
    Int ncpus;
    bool have_examples;

    /* Set once a test has panicked and the panic is on its way out, so that a
     * runner it passes on the way treats it as somebody else's. */
    bool repanicking;
    Any repanic;

    /* Go's benchmarkLock, and who is inside runN, which is the benchmark's
     * answer to FailNow's question. A stack, because B.Run lets go of the lock
     * while the parent's runN is still under way. */
    SyncMutex bench_mu;
    uintptr_t *bench_keys;
    Int nbench_keys;
    Int cbench_keys;
    bool labels_done;
    int bench_procs;   /* the -test.cpu value the benchmark is named for */
    int heap_counting; /* how many benchmark runs want the heap counted */
} Package;

static Package pkg;

static FILE *err_out(void) {
    return pkg.flags[F_V].json ? stdout : stderr;
}

static void write_to(FILE *f, Str s) {
    if (s.len > 0)
        fwrite(s.p, 1, (size_t)s.len, f);
    fflush(f);
}

/* ------------------------------------------------------------ flag parsing */

static Str cstr(const char *s) {
    return str_from_cstr(s);
}

static Str flag_default(int i) {
    return flag_defs[i].def != NULL ? cstr(flag_defs[i].def) : cstr(pkg.parallel_def);
}

/* What the flag's Set method says, or an empty string when it took. */
static const char *flag_set(int i, Str v, FlagValue *fv, Alloc *a, Str *msg) {
    Error err = {0};
    switch (flag_defs[i].kind) {
    case FK_BOOL: {
        bool b = strconv_parse_bool(v, &err);
        if (err.vt != NULL)
            return "parse error";
        fv->b = b;
        return NULL;
    }
    case FK_STRING:
        fv->s = v;
        return NULL;
    case FK_INT: {
        int64_t n = strconv_parse_int(v, 0, 0, &err);
        if (err.vt != NULL)
            return errors_is(err, strconv_err_range) ? "value out of range"
                                                     : "parse error";
        fv->i = n;
        return NULL;
    }
    case FK_UINT: {
        uint64_t n = strconv_parse_uint(v, 0, 0, &err);
        if (err.vt != NULL)
            return errors_is(err, strconv_err_range) ? "value out of range"
                                                     : "parse error";
        fv->u = n;
        return NULL;
    }
    case FK_DURATION: {
        int64_t d;
        if (!dur_parse(v, &d))
            return "parse error";
        fv->d = d;
        return NULL;
    }
    case FK_DURCOUNT: {
        if (v.len > 0 && v.p[v.len - 1] == 'x') {
            int64_t n = strconv_parse_int(str_from_bytes(v.p, v.len - 1), 10, 0, &err);
            if (err.vt != NULL || n < 0 || (!flag_defs[i].allow_zero && n == 0))
                return "invalid count";
            fv->d = 0;
            fv->n = n;
            return NULL;
        }
        int64_t d;
        if (!dur_parse(v, &d) || d < 0 || (!flag_defs[i].allow_zero && d == 0))
            return "invalid duration";
        fv->d = d;
        fv->n = 0;
        return NULL;
    }
    default: /* FK_CHATTY */
        if (str_eq(v, BURROW_S("true")) || str_eq(v, BURROW_S("test2json"))) {
            fv->b = true;
            fv->json = str_eq(v, BURROW_S("test2json"));
            return NULL;
        }
        if (str_eq(v, BURROW_S("false"))) {
            fv->b = false;
            fv->json = false;
            return NULL;
        }
        *msg = fmt_sprintf_v(a, "invalid flag -test.v=%s", v);
        return "";
    }
}

static bool flag_is_bool(int i) {
    return flag_defs[i].kind == FK_BOOL || flag_defs[i].kind == FK_CHATTY;
}

/* The zero value's String, which decides whether -h shows the default. */
static const char *flag_zero(int kind) {
    switch (kind) {
    case FK_BOOL:
    case FK_CHATTY:
        return "false";
    case FK_STRING:
        return "";
    case FK_INT:
    case FK_UINT:
        return "0";
    default:
        return "0s";
    }
}

static int flag_cmp(const void *x, const void *y) {
    return strcmp(flag_defs[*(const int *)x].name, flag_defs[*(const int *)y].name);
}

/* flag.PrintDefaults, and the Usage line in front of it. */
static void usage(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Buf b = {0};
    const char *prog = pkg.argc > 0 && pkg.argv != NULL ? pkg.argv[0] : "";
    buf_str(&b, fmt_sprintf_v(a, "Usage of %s:\n", prog));
    int order[F_COUNT_OF_FLAGS];
    for (int i = 0; i < F_COUNT_OF_FLAGS; i++)
        order[i] = i;
    qsort(order, F_COUNT_OF_FLAGS, sizeof order[0], flag_cmp);
    for (int k = 0; k < F_COUNT_OF_FLAGS; k++) {
        int i = order[k];
        const FlagDef *f = &flag_defs[i];
        buf_str(&b, BURROW_S("  -"));
        buf_str(&b, cstr(f->name));

        /* The back quoted word in the usage names the argument, and the
         * quotes come out. */
        Str use = cstr(f->usage);
        Str arg = BURROW_S("");
        Buf plain = {0};
        Int q1 = -1;
        Int q2 = -1;
        for (Int j = 0; j < use.len; j++) {
            if (use.p[j] == '`') {
                if (q1 < 0) {
                    q1 = j;
                } else {
                    q2 = j;
                    break;
                }
            }
        }
        if (q2 > 0) {
            arg = str_from_bytes(use.p + q1 + 1, q2 - q1 - 1);
            buf_append(&plain, use.p, q1);
            buf_str(&plain, arg);
            buf_append(&plain, use.p + q2 + 1, use.len - q2 - 1);
        } else {
            buf_str(&plain, use);
            switch (f->kind) {
            case FK_STRING:
                arg = BURROW_S("string");
                break;
            case FK_INT:
                arg = BURROW_S("int");
                break;
            case FK_UINT:
                arg = BURROW_S("uint");
                break;
            case FK_DURATION:
                arg = BURROW_S("duration");
                break;
            case FK_DURCOUNT:
                arg = BURROW_S("value");
                break;
            default:
                break;
            }
        }
        if (arg.len > 0) {
            buf_str(&b, BURROW_S(" "));
            buf_str(&b, arg);
        }
        buf_str(&b, BURROW_S("\n    \t"));
        for (Int j = 0; j < plain.len; j++) {
            if (plain.p[j] == '\n')
                buf_str(&b, BURROW_S("\n    \t"));
            else
                buf_append(&b, &plain.p[j], 1);
        }
        buf_free(&plain);
        Str def = flag_default(i);
        if (!str_eq(def, cstr(flag_zero(f->kind)))) {
            if (f->kind == FK_STRING)
                buf_str(&b, fmt_sprintf_v(a, " (default %q)", def));
            else
                buf_str(&b, fmt_sprintf_v(a, " (default %s)", def));
        }
        buf_str(&b, BURROW_S("\n"));
    }
    write_to(err_out(), buf_view(&b));
    buf_free(&b);
    arena_free(&ar);
}

BURROW_NORETURN static void flag_fail(Str msg) {
    Buf b = {0};
    buf_str(&b, msg);
    buf_str(&b, BURROW_S("\n"));
    write_to(err_out(), buf_view(&b));
    buf_free(&b);
    usage();
    exit(2);
}

static int flag_lookup(Str name) {
    for (int i = 0; i < F_COUNT_OF_FLAGS; i++) {
        if (str_eq(name, cstr(flag_defs[i].name)))
            return i;
    }
    return -1;
}

static void flags_reset(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Str ignored;
    for (int i = 0; i < F_COUNT_OF_FLAGS; i++) {
        pkg.flags[i] = (FlagValue){0};
        flag_set(i, flag_default(i), &pkg.flags[i], arena_allocator(&ar), &ignored);
    }
    arena_free(&ar);
}

/* flag.Parse over the command line testing_init was given. The -test.
 * prefix may be left off, since that is how people type them. */
static void flags_parse(void) {
    pkg.parsed = true;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (int k = 1; k < pkg.argc; k++) {
        Str s = cstr(pkg.argv[k]);
        if (s.len < 2 || s.p[0] != '-')
            break;
        Int minuses = 1;
        if (s.p[1] == '-') {
            minuses++;
            if (s.len == 2)
                break;
        }
        Str name = str_from_bytes(s.p + minuses, s.len - minuses);
        if (name.len == 0 || name.p[0] == '-' || name.p[0] == '=')
            flag_fail(fmt_sprintf_v(a, "bad flag syntax: %s", s));
        bool has_value = false;
        Str value = BURROW_S("");
        for (Int j = 1; j < name.len; j++) {
            if (name.p[j] == '=') {
                value = str_from_bytes(name.p + j + 1, name.len - j - 1);
                name = str_from_bytes(name.p, j);
                has_value = true;
                break;
            }
        }
        int i = flag_lookup(name);
        if (i < 0)
            i = flag_lookup(fmt_sprintf_v(a, "test.%s", name));
        if (i < 0) {
            if (str_eq(name, BURROW_S("help")) || str_eq(name, BURROW_S("h"))) {
                usage();
                exit(0);
            }
            flag_fail(fmt_sprintf_v(a, "flag provided but not defined: -%s", name));
        }
        Str msg = BURROW_S("");
        const char *why;
        if (flag_is_bool(i)) {
            if (has_value) {
                why = flag_set(i, value, &pkg.flags[i], a, &msg);
                if (why != NULL)
                    flag_fail(fmt_sprintf_v(a, "invalid boolean value %q for -%s: %s",
                                            value, name,
                                            msg.len > 0 ? msg : cstr(why)));
            } else {
                why = flag_set(i, BURROW_S("true"), &pkg.flags[i], a, &msg);
                if (why != NULL)
                    flag_fail(fmt_sprintf_v(a, "invalid boolean flag %s: %s", name,
                                            msg.len > 0 ? msg : cstr(why)));
            }
            continue;
        }
        if (!has_value && k + 1 < pkg.argc) {
            has_value = true;
            value = cstr(pkg.argv[++k]);
        }
        if (!has_value)
            flag_fail(fmt_sprintf_v(a, "flag needs an argument: -%s", name));
        why = flag_set(i, value, &pkg.flags[i], a, &msg);
        if (why != NULL)
            flag_fail(fmt_sprintf_v(a, "invalid value %q for flag -%s: %s", value, name,
                                    msg.len > 0 ? msg : cstr(why)));
    }
    arena_free(&ar);
}

/* ------------------------------------------------------------ the package */

void testing_init(int argc, char **argv) {
    pkg.argc = argc;
    pkg.argv = argv;
    if (pkg.init_ran)
        return;
    pkg.init_ran = true;
    snprintf(pkg.parallel_def, sizeof pkg.parallel_def, "%d", runtime_gomaxprocs(0));
    flags_reset();
}

bool testing_short(void) {
    if (!pkg.init_ran)
        panic_str(BURROW_S("testing: Short called before Init"));
    if (!pkg.parsed)
        panic_str(BURROW_S("testing: Short called before Parse"));
    return pkg.flags[F_SHORT].b;
}

bool testing_verbose(void) {
    if (!pkg.parsed)
        panic_str(BURROW_S("testing: Verbose called before Parse"));
    return pkg.flags[F_V].b;
}

bool testing_testing(void) {
    return pkg.init_ran;
}

Str testing_cover_mode(void) {
    return BURROW_S("");
}

double testing_coverage(void) {
    return 0;
}

static bool should_fail_fast(void) {
    sync_mutex_lock(&pkg.reg_mu);
    bool stop = pkg.flags[F_FAILFAST].b && pkg.num_failed > 0;
    sync_mutex_unlock(&pkg.reg_mu);
    return stop;
}

/* The goroutine this is, or the thread when it is not on one. */
static uintptr_t current_key(void) {
    burrow__G *g = burrow__curg();
    if (g != NULL)
        return (uintptr_t)g;
    return (uintptr_t)(burrow__thread_self() << 1 | 1);
}

static void set_running(TestingT *t, bool on) {
    sync_mutex_lock(&pkg.reg_mu);
    t->running = on;
    if (on)
        t->running_since = burrow_nanotime();
    sync_mutex_unlock(&pkg.reg_mu);
}

/* ---------------------------------------------------------- stopping early
 *
 * What FailNow and SkipNow do where Go calls runtime.Goexit. On the goroutine
 * running a test, or the thread in bare mode, it is a panic with a value only
 * this file makes, and the runner catches it. Anywhere else it is Goexit. */

typedef struct Stop {
    Byte unused;
} Stop;

static const Type stop_desc = {
    BURROW_S_INIT("stop"),
    BURROW_S_INIT("testing"),
    KIND_STRUCT,
    (uint32_t)sizeof(Stop),
    (uint16_t)_Alignof(Stop),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

static bool on_runner(void) {
    uintptr_t key = current_key();
    bool found = false;
    sync_mutex_lock(&pkg.reg_mu);
    for (Int i = 0; i < pkg.nall && !found; i++)
        found = pkg.all[i]->in_runner && pkg.all[i]->runner_key == key;
    for (Int i = 0; i < pkg.nbench_keys && !found; i++)
        found = pkg.bench_keys[i] == key;
    sync_mutex_unlock(&pkg.reg_mu);
    return found;
}

BURROW_NORETURN static void stop_test(void) {
    if (burrow__curg() == NULL || on_runner()) {
        Stop s = {0};
        panic((Any){&stop_desc, &s});
    }
    runtime_goexit();
}

static bool is_stop(Any p) {
    return p.t == &stop_desc;
}

/* A panic value kept past the catch block that caught it. */
static Any keep_panic(Any p) {
    if (p.t == NULL) {
        Error e =
            errors_new(heap_allocator(), BURROW_S("panic called with nil argument"));
        return any_box(heap_allocator(), BURROW_ANY_OF(e));
    }
    return any_box(heap_allocator(), p);
}

/* -------------------------------------------------------------- printing */

static void chatty_write(Chatty *p, Str msg) {
    Buf b = {0};
    if (p->json)
        buf_str(&b, BURROW_S("\x16"));
    buf_str(&b, msg);
    write_to(stdout, buf_view(&b));
    buf_free(&b);
}

/* chattyPrinter.Updatef, with the message already formatted. */
static void chatty_update(Chatty *p, Str name, Str msg) {
    sync_mutex_lock(&p->mu);
    p->last_name.len = 0;
    buf_str(&p->last_name, name);
    chatty_write(p, msg);
    sync_mutex_unlock(&p->mu);
}

static void chatty_updatef(Chatty *p, Str name, const char *format, Str arg) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    chatty_update(p, name, fmt_sprintf_v(arena_allocator(&ar), format, arg));
    arena_free(&ar);
}

/* chattyPrinter.Printf, which says whose output this is when it changed. */
static void chatty_printf(Chatty *p, Str name, Str msg) {
    sync_mutex_lock(&p->mu);
    if (p->last_name.len == 0) {
        buf_str(&p->last_name, name);
    } else if (!str_eq(buf_view(&p->last_name), name)) {
        Arena ar;
        arena_init(&ar, NULL, 0);
        chatty_write(p, fmt_sprintf_v(arena_allocator(&ar), "=== NAME  %s\n", name));
        arena_free(&ar);
        p->last_name.len = 0;
        buf_str(&p->last_name, name);
    }
    write_to(stdout, msg);
    sync_mutex_unlock(&p->mu);
}

/* indenter.Write into c's output, with c->mu held. */
static void indent_into(TestingT *c, Str b) {
    while (b.len > 0) {
        Int end = 0;
        while (end < b.len && b.p[end] != '\n')
            end++;
        if (end < b.len)
            end++;
        Str line = str_from_bytes(b.p, end);
        if (b.p[0] == 0x16) {
            buf_append(&c->output, line.p, 1);
            line = str_from_bytes(line.p + 1, line.len - 1);
        }
        buf_str(&c->output, BURROW_S("    "));
        buf_str(&c->output, line);
        b = str_from_bytes(b.p + end, b.len - end);
    }
}

/* Writes to c's w, which is stdout for the root and the indenter for the
 * rest, with c->mu held. */
static void write_w(TestingT *c, Str s) {
    if (c->parent == NULL)
        write_to(stdout, s);
    else
        indent_into(c, s);
}

/* flushToParent, with the header already formatted. */
static void flush_to_parent(TestingT *c, Str header) {
    TestingT *p = c->parent;
    sync_mutex_lock(&p->mu);
    sync_mutex_lock(&c->mu);
    Buf msg = {0};
    buf_str(&msg, header);
    buf_append(&msg, c->output.p, c->output.len);
    c->output.len = 0;
    if (c->chatty != NULL && (p->parent == NULL || c->chatty->json)) {
        chatty_update(c->chatty, c->name, buf_view(&msg));
    } else {
        if (c->chatty != NULL && c->chatty->json) {
            Buf framed = {0};
            buf_str(&framed, BURROW_S("\x16"));
            buf_str(&framed, buf_view(&msg));
            write_w(p, buf_view(&framed));
            buf_free(&framed);
        } else {
            write_w(p, buf_view(&msg));
        }
    }
    buf_free(&msg);
    sync_mutex_unlock(&c->mu);
    sync_mutex_unlock(&p->mu);
}

static TestingT *destination(TestingT *c) {
    for (TestingT *n = c; n != NULL; n = n->parent) {
        sync_mutex_lock(&n->mu);
        bool done = n->done;
        sync_mutex_unlock(&n->mu);
        if (!done)
            return n;
    }
    return NULL;
}

BURROW_NORETURN static void panic_after(const char *format, Str name, Str extra) {
    Alloc *a = error_allocator();
    panic_str(fmt_sprintf_v(a, format, name, extra));
}

/* checkFuzzFn. */
static void check_fuzz_fn(TestingT *t, const char *name) {
    if (t->in_fuzz_fn) {
        Str n = str_from_cstr(name);
        panic_str(fmt_sprintf_v(
            error_allocator(),
            "testing: f.%s was called inside the fuzz target, use t.%s instead", n, n));
    }
}

static bool is_mark(Byte b) {
    return b == 0x16 || b == 0x0F || b == 0x0E || b == 0x1B;
}

/* outputWriter.writeLine, with c->mu held. */
static void write_line(TestingT *c, Str b, bool err_begin, bool err_end) {
    if (c->done || c->chatty == NULL) {
        buf_str(&c->output, BURROW_S("    "));
        buf_str(&c->output, b);
        return;
    }
    Buf line = {0};
    bool json = c->chatty->json;
    if (err_begin && json)
        buf_str(&line, BURROW_S("\x0f"));
    buf_str(&line, BURROW_S("    "));
    Str tail = BURROW_S("");
    if (err_end && json && b.len > 0 && b.p[b.len - 1] == '\n') {
        b = str_from_bytes(b.p, b.len - 1);
        tail = BURROW_S("\n");
    }
    for (Int i = 0; i < b.len; i++) {
        if (is_mark(b.p[i]))
            buf_str(&line, BURROW_S("\x1b"));
        buf_append(&line, &b.p[i], 1);
    }
    if (err_end && json)
        buf_str(&line, BURROW_S("\x0e"));
    buf_str(&line, tail);
    /* Benchmarks print no === CONT lines, so they skip the printer and go
     * straight to stdout. */
    if (c->bench)
        write_to(stdout, buf_view(&line));
    else
        chatty_printf(c->chatty, c->name, buf_view(&line));
    buf_free(&line);
}

/* outputWriter.write. */
static void o_write(TestingT *c, Str p, bool is_err) {
    if (!c->has_o)
        return;
    if (destination(c) == NULL)
        panic_after("Write called after %s has completed%s", c->name, BURROW_S(""));
    sync_mutex_lock(&c->mu);
    Int lines = 0;
    for (Int i = 0; i < p.len; i++)
        lines += p.p[i] == '\n';
    Int k = 0;
    while (k < lines) {
        Int end = 0;
        while (p.p[end] != '\n')
            end++;
        end++;
        Str line = str_from_bytes(p.p, end);
        if (k == 0 && c->partial.len > 0) {
            Buf joined = {0};
            buf_append(&joined, c->partial.p, c->partial.len);
            buf_str(&joined, line);
            c->partial.len = 0;
            write_line(c, buf_view(&joined), is_err && k == 0,
                       is_err && k == lines - 1);
            buf_free(&joined);
        } else {
            write_line(c, line, is_err && k == 0, is_err && k == lines - 1);
        }
        p = str_from_bytes(p.p + end, p.len - end);
        k++;
    }
    buf_str(&c->partial, p);
    sync_mutex_unlock(&c->mu);
}

static void flush_partial(TestingT *c) {
    sync_mutex_lock(&c->mu);
    bool partial = c->has_o && c->partial.len > 0;
    sync_mutex_unlock(&c->mu);
    if (partial)
        o_write(c, BURROW_S("\n"), false);
}

static Str base_name(const char *file) {
    Str s = cstr(file);
    if (pkg.flags[F_FULLPATH].b)
        return s;
    Int i = s.len;
    while (i > 0 && s.p[i - 1] != '/' && s.p[i - 1] != '\\')
        i--;
    return str_from_bytes(s.p + i, s.len - i);
}

/* common.log. */
static void log_text(TestingT *c, const char *file, int line, Str s, bool is_err) {
    if (s.len > 0 && s.p[s.len - 1] == '\n')
        s.len--;
    Buf b = {0};
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    if (file == NULL)
        buf_str(&b, BURROW_S("???:1: "));
    else
        buf_str(&b, fmt_sprintf_v(a, "%s:%d: ", base_name(file), line > 0 ? line : 1));
    Int body = b.len;
    for (Int i = 0; i < s.len; i++) {
        buf_append(&b, &s.p[i], 1);
        if (s.p[i] == '\n')
            buf_str(&b, BURROW_S("    "));
    }
    buf_str(&b, BURROW_S("\n"));
    arena_free(&ar);

    TestingT *n = destination(c);
    if (n == NULL) {
        Str text =
            str_clone(error_allocator(), str_from_bytes(b.p + body, b.len - body));
        buf_free(&b);
        panic_after("Log in goroutine after %s has completed: %s", c->name, text);
    }
    flush_partial(n);
    o_write(n, buf_view(&b), is_err);
    buf_free(&b);
}

/* -------------------------------------------------------------- T methods */

Str testing_t_name(TestingT *t) {
    return t->name;
}

void testing_t_fail(TestingT *t) {
    if (t->parent != NULL)
        testing_t_fail(t->parent);
    sync_mutex_lock(&t->mu);
    if (t->done) {
        sync_mutex_unlock(&t->mu);
        panic_after("Fail in goroutine after %s has completed%s", t->name,
                    BURROW_S(""));
    }
    t->failed = true;
    sync_mutex_unlock(&t->mu);
}

bool testing_t_failed(TestingT *t) {
    sync_mutex_lock(&t->mu);
    bool failed = t->failed;
    sync_mutex_unlock(&t->mu);
    return failed;
}

void testing_t_fail_now(TestingT *t) {
    check_fuzz_fn(t, "FailNow");
    testing_t_fail(t);
    sync_mutex_lock(&t->mu);
    t->finished = true;
    sync_mutex_unlock(&t->mu);
    stop_test();
}

void testing_t_skip_now(TestingT *t) {
    check_fuzz_fn(t, "SkipNow");
    sync_mutex_lock(&t->mu);
    t->skipped = true;
    t->finished = true;
    sync_mutex_unlock(&t->mu);
    stop_test();
}

bool testing_t_skipped(TestingT *t) {
    sync_mutex_lock(&t->mu);
    bool skipped = t->skipped;
    sync_mutex_unlock(&t->mu);
    return skipped;
}

void testing_t_helper(TestingT *t) {
    (void)t;
}

void testing_t_cleanup(TestingT *t, Func f) {
    check_fuzz_fn(t, "Cleanup");
    sync_mutex_lock(&t->mu);
    if (t->ncleanups == t->ccleanups) {
        Int ncap = t->ccleanups == 0 ? 4 : t->ccleanups * 2;
        t->cleanups = (Func *)must_realloc(heap_allocator(), t->cleanups,
                                           (size_t)t->ccleanups * sizeof(Func),
                                           (size_t)ncap * sizeof(Func), _Alignof(Func));
        t->ccleanups = ncap;
    }
    t->cleanups[t->ncleanups++] = f;
    sync_mutex_unlock(&t->mu);
}

Context testing_t_context(TestingT *t) {
    check_fuzz_fn(t, "Context");
    return t->ctx;
}

bool testing_t_deadline(TestingT *t, int64_t *when) {
    *when = t->tstate->deadline;
    return t->tstate->deadline != 0;
}

static Int output_write(void *self, Slice p, Error *err) {
    if (err != NULL)
        *err = (Error){0};
    o_write((TestingT *)self, str_from_bytes((const Byte *)p.p, p.len), false);
    return p.len;
}

static const IoWriterVT output_vt = {NULL, output_write};

IoWriter testing_t_output(TestingT *t) {
    check_fuzz_fn(t, "Output");
    TestingT *n = destination(t);
    if (n == NULL)
        panic_after("Output called after %s has completed%s", t->name, BURROW_S(""));
    return (IoWriter){&output_vt, n};
}

static bool is_space_rune(Rune r) {
    switch (r) {
    case '\t':
    case '\n':
    case '\v':
    case '\f':
    case '\r':
    case ' ':
    case 0x85:
    case 0xA0:
    case 0x1680:
    case 0x2028:
    case 0x2029:
    case 0x202f:
    case 0x205f:
    case 0x3000:
        return true;
    default:
        return r >= 0x2000 && r <= 0x200a;
    }
}

void testing_t_attr(TestingT *t, Str key, Str value) {
    for (Int i = 0; i < key.len;) {
        Int size = 0;
        Rune r =
            utf8_decode_rune_in_string(str_from_bytes(key.p + i, key.len - i), &size);
        if (is_space_rune(r)) {
            testing_t_errorf_v(t, "disallowed whitespace in attribute key %q", key);
            return;
        }
        i += size;
    }
    for (Int i = 0; i < value.len; i++) {
        if (value.p[i] == '\r' || value.p[i] == '\n') {
            testing_t_errorf_v(t, "disallowed newline in attribute value %q", value);
            return;
        }
    }
    if (t->chatty == NULL)
        return;
    Arena ar;
    arena_init(&ar, NULL, 0);
    chatty_update(t->chatty, t->name,
                  fmt_sprintf_v(arena_allocator(&ar), "=== ATTR  %s %v %v\n", t->name,
                                key, value));
    arena_free(&ar);
}

/* The name check_fuzz_fn gives each log kind. */
static const char *log_name(burrow__TestingLogKind kind, bool f) {
    switch (kind) {
    case BURROW__TESTING_ERROR:
        return f ? "Errorf" : "Error";
    case BURROW__TESTING_FATAL:
        return f ? "Fatalf" : "Fatal";
    case BURROW__TESTING_SKIP:
        return f ? "Skipf" : "Skip";
    case BURROW__TESTING_LOG:
    default:
        return f ? "Logf" : "Log";
    }
}

/* What Log and the rest end with. */
static void after_log(TestingT *t, burrow__TestingLogKind kind) {
    switch (kind) {
    case BURROW__TESTING_ERROR:
        testing_t_fail(t);
        break;
    case BURROW__TESTING_FATAL:
        testing_t_fail_now(t);
    case BURROW__TESTING_SKIP:
        testing_t_skip_now(t);
    case BURROW__TESTING_LOG:
    default:
        break;
    }
}

void burrow__testing_t_logln(TestingT *t, const char *file, int line,
                             burrow__TestingLogKind kind, Slice args) {
    check_fuzz_fn(t, log_name(kind, false));
    Arena ar;
    arena_init(&ar, NULL, 0);
    Str s = fmt_sprintln(arena_allocator(&ar), args);
    Buf b = {0};
    buf_str(&b, s);
    arena_free(&ar);
    bool is_err = kind == BURROW__TESTING_ERROR || kind == BURROW__TESTING_FATAL;
    BURROW_SCOPE {
        BURROW_DEFER(buf_release, &b);
        log_text(t, file, line, buf_view(&b), is_err);
    }
    BURROW_SCOPE_END;
    after_log(t, kind);
}

void burrow__testing_t_logf(TestingT *t, const char *file, int line,
                            burrow__TestingLogKind kind, Str format, Slice args) {
    check_fuzz_fn(t, log_name(kind, true));
    Arena ar;
    arena_init(&ar, NULL, 0);
    Str s = fmt_sprintf(arena_allocator(&ar), format, args);
    Buf b = {0};
    buf_str(&b, s);
    arena_free(&ar);
    bool is_err = kind == BURROW__TESTING_ERROR || kind == BURROW__TESTING_FATAL;
    BURROW_SCOPE {
        BURROW_DEFER(buf_release, &b);
        log_text(t, file, line, buf_view(&b), is_err);
    }
    BURROW_SCOPE_END;
    after_log(t, kind);
}

/* The functions behind the macros, for a caller that took the address. */
void(testing_t_log)(TestingT *t, Slice args) {
    burrow__testing_t_logln(t, NULL, 0, BURROW__TESTING_LOG, args);
}

void(testing_t_logf)(TestingT *t, Str format, Slice args) {
    burrow__testing_t_logf(t, NULL, 0, BURROW__TESTING_LOG, format, args);
}

void(testing_t_error)(TestingT *t, Slice args) {
    burrow__testing_t_logln(t, NULL, 0, BURROW__TESTING_ERROR, args);
}

void(testing_t_errorf)(TestingT *t, Str format, Slice args) {
    burrow__testing_t_logf(t, NULL, 0, BURROW__TESTING_ERROR, format, args);
}

void(testing_t_fatal)(TestingT *t, Slice args) {
    burrow__testing_t_logln(t, NULL, 0, BURROW__TESTING_LOG, args);
    testing_t_fail_now(t);
}

void(testing_t_fatalf)(TestingT *t, Str format, Slice args) {
    burrow__testing_t_logf(t, NULL, 0, BURROW__TESTING_LOG, format, args);
    testing_t_fail_now(t);
}

void(testing_t_skip)(TestingT *t, Slice args) {
    burrow__testing_t_logln(t, NULL, 0, BURROW__TESTING_LOG, args);
    testing_t_skip_now(t);
}

void(testing_t_skipf)(TestingT *t, Str format, Slice args) {
    burrow__testing_t_logf(t, NULL, 0, BURROW__TESTING_LOG, format, args);
    testing_t_skip_now(t);
}

/* ---------------------------------------------------------------- TB */

static void tb_cleanup(void *self, Func f) {
    testing_t_cleanup((TestingT *)self, f);
}
static Context tb_context(void *self) {
    return testing_t_context((TestingT *)self);
}
static void tb_error(void *self, Slice args) {
    (testing_t_error)((TestingT *)self, args);
}
static void tb_errorf(void *self, Str format, Slice args) {
    (testing_t_errorf)((TestingT *)self, format, args);
}
static void tb_fail(void *self) {
    testing_t_fail((TestingT *)self);
}
static void tb_fail_now(void *self) {
    testing_t_fail_now((TestingT *)self);
}
static bool tb_failed(void *self) {
    return testing_t_failed((TestingT *)self);
}
static void tb_fatal(void *self, Slice args) {
    (testing_t_fatal)((TestingT *)self, args);
}
static void tb_fatalf(void *self, Str format, Slice args) {
    (testing_t_fatalf)((TestingT *)self, format, args);
}
static void tb_helper(void *self) {
    testing_t_helper((TestingT *)self);
}
static void tb_log(void *self, Slice args) {
    (testing_t_log)((TestingT *)self, args);
}
static void tb_logf(void *self, Str format, Slice args) {
    (testing_t_logf)((TestingT *)self, format, args);
}
static Str tb_name(void *self) {
    return testing_t_name((TestingT *)self);
}
static IoWriter tb_output(void *self) {
    return testing_t_output((TestingT *)self);
}
static void tb_attr(void *self, Str key, Str value) {
    testing_t_attr((TestingT *)self, key, value);
}
static void tb_skip(void *self, Slice args) {
    (testing_t_skip)((TestingT *)self, args);
}
static void tb_skip_now(void *self) {
    testing_t_skip_now((TestingT *)self);
}
static void tb_skipf(void *self, Str format, Slice args) {
    (testing_t_skipf)((TestingT *)self, format, args);
}
static bool tb_skipped(void *self) {
    return testing_t_skipped((TestingT *)self);
}

static const TestingTBVT t_tb_vt = {
    NULL,      tb_cleanup, tb_context, tb_error,    tb_errorf, tb_fail,    tb_fail_now,
    tb_failed, tb_fatal,   tb_fatalf,  tb_helper,   tb_log,    tb_logf,    tb_name,
    tb_output, tb_attr,    tb_skip,    tb_skip_now, tb_skipf,  tb_skipped,
};

TestingTB testing_t_as_testing_tb(TestingT *t) {
    return (TestingTB){&t_tb_vt, t};
}

/* ------------------------------------------------------------- testState */

static TestState *tstate_new(int max_parallel, burrow__TestingMatcher *m, bool bare) {
    TestState *s = (TestState *)must_alloc(heap_allocator(), sizeof(TestState),
                                           _Alignof(TestState));
    *s = (TestState){0};
    s->match = m;
    s->bare = bare;
    s->max_parallel = max_parallel;
    s->running = 1;
    if (!bare)
        s->start_parallel = chan_make(heap_allocator(), TYPE_BOOL, 0);
    return s;
}

static void tstate_free(TestState *s) {
    if (s->start_parallel != NULL)
        chan_free(s->start_parallel);
    burrow__testing_matcher_free(s->match);
    mem_free(heap_allocator(), s, sizeof(TestState), _Alignof(TestState));
}

static void wait_parallel(TestState *s) {
    sync_mutex_lock(&s->mu);
    if (s->running < s->max_parallel) {
        s->running++;
        sync_mutex_unlock(&s->mu);
        return;
    }
    s->num_waiting++;
    sync_mutex_unlock(&s->mu);
    bool v;
    chan_recv(s->start_parallel, &v);
}

static void release(TestState *s) {
    sync_mutex_lock(&s->mu);
    if (s->num_waiting == 0) {
        s->running--;
        sync_mutex_unlock(&s->mu);
        return;
    }
    s->num_waiting--;
    sync_mutex_unlock(&s->mu);
    bool v = true;
    chan_send(s->start_parallel, &v);
}

/* ------------------------------------------------------------ making a T */

static TestingT *t_new(TestingT *parent, Str name, TestState *s, Chatty *chatty) {
    TestingT *t =
        (TestingT *)must_alloc(heap_allocator(), sizeof(TestingT), _Alignof(TestingT));
    *t = (TestingT){0};
    t->name = name;
    t->parent = parent;
    t->level = parent != NULL ? parent->level + 1 : 0;
    t->tstate = s;
    t->chatty = chatty;
    t->has_o = parent != NULL;
    t->ctx = context_with_cancel(heap_allocator(), context_background(), &t->cancel);
    if (!s->bare) {
        t->barrier = chan_make(heap_allocator(), TYPE_BOOL, 0);
        t->signal = chan_make(heap_allocator(), TYPE_BOOL, 1);
    }
    sync_mutex_lock(&pkg.reg_mu);
    if (pkg.nall == pkg.call) {
        Int ncap = pkg.call == 0 ? 16 : pkg.call * 2;
        pkg.all = (TestingT **)must_realloc(
            heap_allocator(), (void *)pkg.all, (size_t)pkg.call * sizeof(TestingT *),
            (size_t)ncap * sizeof(TestingT *), _Alignof(TestingT *));
        pkg.call = ncap;
    }
    pkg.all[pkg.nall++] = t;
    sync_mutex_unlock(&pkg.reg_mu);
    return t;
}

/* Everything a T holds, less the T itself, which a B has inside it. */
static void t_release(TestingT *t) {
    context_release(t->ctx);
    if (t->barrier != NULL)
        chan_free(t->barrier);
    if (t->signal != NULL)
        chan_free(t->signal);
    buf_free(&t->output);
    buf_free(&t->partial);
    if (t->sub != NULL)
        mem_free(heap_allocator(), (void *)t->sub, (size_t)t->csub * sizeof(TestingT *),
                 _Alignof(TestingT *));
    if (t->cleanups != NULL)
        mem_free(heap_allocator(), t->cleanups, (size_t)t->ccleanups * sizeof(Func),
                 _Alignof(Func));
    str_release(t->name);
}

static void t_free(TestingT *t) {
    t_release(t);
    mem_free(heap_allocator(), t, sizeof(TestingT), _Alignof(TestingT));
}

static void free_all(void) {
    sync_mutex_lock(&pkg.reg_mu);
    TestingT **all = pkg.all;
    Int n = pkg.nall;
    Int cap = pkg.call;
    pkg.all = NULL;
    pkg.nall = 0;
    pkg.call = 0;
    sync_mutex_unlock(&pkg.reg_mu);
    for (Int i = 0; i < n; i++)
        t_free(all[i]);
    if (all != NULL)
        mem_free(heap_allocator(), (void *)all, (size_t)cap * sizeof(TestingT *),
                 _Alignof(TestingT *));
}

/* ------------------------------------------------------------- the runner */

/* runCleanup. Answers whether a cleanup panicked, and with what. */
static bool run_cleanup(TestingT *t, Any *out) {
    sync_mutex_lock(&t->mu);
    t->cleanup_started = true;
    sync_mutex_unlock(&t->mu);
    BURROW_CALLF0(t->cancel);
    volatile bool got = false;
    for (;;) {
        Func f = {0};
        sync_mutex_lock(&t->mu);
        if (t->ncleanups > 0)
            f = t->cleanups[--t->ncleanups];
        sync_mutex_unlock(&t->mu);
        if (BURROW_FUNC_IS_NIL(f))
            break;
        BURROW_TRY {
            BURROW_CALLF0(f);
        }
        BURROW_CATCH(p) {
            if (pkg.repanicking)
                panic(pkg.repanic);
            if (!is_stop(p)) {
                *out = keep_panic(p);
                got = true;
            }
        }
        BURROW_TRY_END;
    }
    sync_mutex_lock(&t->mu);
    t->cleanup_started = false;
    sync_mutex_unlock(&t->mu);
    return got;
}

static void report(TestingT *t) {
    if (t->parent == NULL)
        return;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str d = fmt_dur(a, t->duration);
    const char *format = "--- %s: %s (%s)\n";
    if (testing_t_failed(t))
        flush_to_parent(t, fmt_sprintf_v(a, format, "FAIL", t->name, d));
    else if (t->chatty != NULL)
        flush_to_parent(t,
                        fmt_sprintf_v(a, format, testing_t_skipped(t) ? "SKIP" : "PASS",
                                      t->name, d));
    arena_free(&ar);
}

static void set_ran(TestingT *t) {
    if (t->parent != NULL)
        set_ran(t->parent);
    sync_mutex_lock(&t->mu);
    t->ran = true;
    sync_mutex_unlock(&t->mu);
}

/* doPanic: report the failure all the way up, then let the panic go on and
 * end the process, which is what Go does with a panicking test. */
BURROW_NORETURN static void do_panic(TestingT *t, Any err) {
    testing_t_fail(t);
    Any r;
    if (run_cleanup(t, &r))
        testing_t_logf_v(t, "cleanup panicked with %v", r);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (TestingT *root = t; root->parent != NULL; root = root->parent) {
        sync_mutex_lock(&root->mu);
        root->duration += burrow_nanotime() - root->start;
        int64_t d = root->duration;
        sync_mutex_unlock(&root->mu);
        flush_partial(root);
        flush_to_parent(
            root, fmt_sprintf_v(a, "--- FAIL: %s (%s)\n", root->name, fmt_dur(a, d)));
        if (run_cleanup(root->parent, &r)) {
            sync_mutex_lock(&root->parent->mu);
            write_w(root->parent, fmt_sprintf_v(a, "cleanup panicked with %v", r));
            sync_mutex_unlock(&root->parent->mu);
        }
    }
    arena_free(&ar);
    pkg.repanicking = true;
    pkg.repanic = err;
    panic(err);
}

/* The deferred half of tRunner. */
static void runner_finish(TestingT *t) {
    bool signal = true;
    sync_mutex_lock(&t->mu);
    bool finished = t->finished;
    sync_mutex_unlock(&t->mu);
    bool has_err = t->has_err;
    Any err = t->err;
    if (!finished && !has_err) {
        Error nil_err = errors_new(
            heap_allocator(), BURROW_S("test executed panic(nil) or runtime.Goexit"));
        err = any_box(heap_allocator(), BURROW_ANY_OF(nil_err));
        has_err = true;
        for (TestingT *p = t->parent; p != NULL; p = p->parent) {
            sync_mutex_lock(&p->mu);
            finished = p->finished;
            sync_mutex_unlock(&p->mu);
            if (finished) {
                if (!t->is_parallel) {
                    testing_t_errorf_v(
                        t, "%v: subtest may have called FailNow on a parent test", err);
                    has_err = false;
                }
                signal = false;
                break;
            }
        }
    }
    if (has_err)
        do_panic(t, err);

    t->duration += burrow_nanotime() - t->start;

    if (t->nsub > 0) {
        release(t->tstate);
        set_running(t, false);
        chan_close(t->barrier);
        for (Int i = 0; i < t->nsub; i++) {
            bool v;
            chan_recv(t->sub[i]->signal, &v);
        }
        int64_t cleanup_start = burrow_nanotime();
        set_running(t, true);
        Any r;
        bool panicked = run_cleanup(t, &r);
        t->duration += burrow_nanotime() - cleanup_start;
        if (panicked)
            do_panic(t, r);
        if (!t->is_parallel)
            wait_parallel(t->tstate);
    } else if (t->is_parallel) {
        release(t->tstate);
    }
    for (TestingT *root = t; root->parent != NULL; root = root->parent)
        flush_partial(root);

    if (testing_t_failed(t)) {
        sync_mutex_lock(&pkg.reg_mu);
        pkg.num_failed++;
        sync_mutex_unlock(&pkg.reg_mu);
    }

    report(t);

    sync_mutex_lock(&t->mu);
    t->done = true;
    bool has_sub = t->has_sub;
    sync_mutex_unlock(&t->mu);
    if (t->parent != NULL && !has_sub)
        set_ran(t);

    sync_mutex_lock(&pkg.reg_mu);
    t->in_runner = false;
    t->running = false;
    sync_mutex_unlock(&pkg.reg_mu);
    if (t->signal != NULL)
        chan_send(t->signal, &signal);
    else
        t->signal_value = signal;
}

/* Runs if the test's goroutine ends without the runner getting to the end,
 * which is runtime_goexit called on it directly. Go treats that as a panic
 * and so the process ends here the same way. The flag lives on the runner's
 * stack rather than in the T, because once runner_finish has signalled the
 * parent the T can be freed before this runs. */
typedef struct Guard {
    TestingT *t;
    volatile bool returned;
} Guard;

static void goexit_guard(void *arg) {
    Guard *g = (Guard *)arg;
    if (g->returned || pkg.repanicking || panic_value().t != NULL)
        return;
    TestingT *t = g->t;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (TestingT *root = t; root->parent != NULL; root = root->parent) {
        int64_t d = root->duration + burrow_nanotime() - root->start;
        flush_partial(root);
        flush_to_parent(
            root, fmt_sprintf_v(a, "--- FAIL: %s (%s)\n", root->name, fmt_dur(a, d)));
    }
    arena_free(&ar);
    write_to(stderr, BURROW_S("panic: test executed panic(nil) or runtime.Goexit\n"));
    _Exit(2);
}

/* tRunner. */
static void t_runner(TestingT *t, TestingTFunc fn) {
    Guard guard = {t, false};
    BURROW_SCOPE {
        BURROW_DEFER(goexit_guard, &guard);
        sync_mutex_lock(&pkg.reg_mu);
        t->runner_key = current_key();
        t->in_runner = true;
        sync_mutex_unlock(&pkg.reg_mu);
        BURROW_TRY {
            t->start = burrow_nanotime();
            BURROW_CALLF(fn, t);
            sync_mutex_lock(&t->mu);
            t->finished = true;
            sync_mutex_unlock(&t->mu);
        }
        BURROW_CATCH(p) {
            if (pkg.repanicking)
                panic(pkg.repanic);
            if (!is_stop(p)) {
                t->err = keep_panic(p);
                t->has_err = true;
            }
        }
        BURROW_TRY_END;
        if (t->nsub == 0) {
            Any r;
            if (run_cleanup(t, &r)) {
                t->err = r;
                t->has_err = true;
            }
        }
        guard.returned = true;
        runner_finish(t);
    }
    BURROW_SCOPE_END;
}

typedef struct RunArg {
    TestingT *t;
    TestingTFunc f;
} RunArg;

static void run_goroutine(void *env) {
    RunArg arg = *(RunArg *)env;
    mem_free(heap_allocator(), env, sizeof(RunArg), _Alignof(RunArg));
    t_runner(arg.t, arg.f);
}

bool testing_t_run(TestingT *t, Str name, TestingTFunc f) {
    sync_mutex_lock(&t->mu);
    if (t->cleanup_started) {
        sync_mutex_unlock(&t->mu);
        panic_str(BURROW_S("testing: t.Run called during t.Cleanup"));
    }
    t->has_sub = true;
    sync_mutex_unlock(&t->mu);

    bool ok;
    bool partial;
    Str test_name = burrow__testing_full_name(
        t->tstate->match, t->level > 0 ? &t->name : NULL, name, &ok, &partial);
    if (!ok || should_fail_fast()) {
        str_release(test_name);
        return true;
    }
    TestingT *c = t_new(t, test_name, t->tstate, t->chatty);
    if (c->chatty != NULL)
        chatty_updatef(c->chatty, c->name, "=== RUN   %s\n", c->name);
    set_running(c, true);

    bool signal;
    if (c->signal != NULL) {
        RunArg *arg =
            (RunArg *)must_alloc(heap_allocator(), sizeof(RunArg), _Alignof(RunArg));
        arg->t = c;
        arg->f = f;
        if (!go(BURROW_FN(Func, run_goroutine, arg))) {
            mem_free(heap_allocator(), arg, sizeof(RunArg), _Alignof(RunArg));
            panic_str(BURROW_S("testing: cannot start the goroutine for a test"));
        }
        chan_recv(c->signal, &signal);
    } else {
        t_runner(c, f);
        signal = c->signal_value;
    }
    if (!signal)
        stop_test();

    if (c->chatty != NULL && c->chatty->json)
        chatty_updatef(c->chatty, t->name, "=== NAME  %s\n", t->name);
    return !testing_t_failed(c);
}

void testing_t_parallel(TestingT *t) {
    if (t->is_parallel)
        panic_str(BURROW_S("testing: t.Parallel called multiple times"));
    if (t->parent == NULL || t->parent->barrier == NULL)
        return;
    t->is_parallel = true;
    t->duration += burrow_nanotime() - t->start;

    TestingT *p = t->parent;
    sync_mutex_lock(&p->mu);
    if (p->nsub == p->csub) {
        Int ncap = p->csub == 0 ? 4 : p->csub * 2;
        p->sub = (TestingT **)must_realloc(
            heap_allocator(), (void *)p->sub, (size_t)p->csub * sizeof(TestingT *),
            (size_t)ncap * sizeof(TestingT *), _Alignof(TestingT *));
        p->csub = ncap;
    }
    p->sub[p->nsub++] = t;
    sync_mutex_unlock(&p->mu);

    if (t->chatty != NULL)
        chatty_updatef(t->chatty, t->name, "=== PAUSE %s\n", t->name);
    set_running(t, false);

    bool v = true;
    chan_send(t->signal, &v);
    chan_recv(p->barrier, &v);
    wait_parallel(t->tstate);

    if (t->chatty != NULL)
        chatty_updatef(t->chatty, t->name, "=== CONT  %s\n", t->name);
    set_running(t, true);
    t->start = burrow_nanotime();
}

/* ----------------------------------------------------------- fuzz targets
 *
 * Go's src/testing/fuzz.go, the part that runs without -test.fuzz: a target
 * adds its seeds, hands over the function, and each seed runs as a subtest
 * named after its place in the corpus. */

/* corpusEntry. The values are copies the F owns. */
typedef struct FuzzEntry {
    Str path;
    Any *values;
    Int n;
} FuzzEntry;

/* What a seed's subtest needs, kept apart from the corpus so that it stays
 * put while the corpus grows. */
typedef struct FuzzSeed {
    TestingF *f;
    Int i;
} FuzzSeed;

struct TestingF {
    TestingT *common;
    FuzzEntry *corpus;
    Int ncorpus;
    Int ccorpus;
    FuzzSeed *seeds;
    Int nseeds;
    TestingFuzzFunc fn;
    bool fuzz_called;
};

/* supportedTypes, answering with the descriptor a value of type t is kept
 * as, or NULL. Any []uint8 is Go's []byte, whichever descriptor it came in
 * with. */
static const Type *fuzz_type(const Type *t) {
    if (t == NULL)
        return NULL;
    if (t->kind == KIND_SLICE && t->name.len == 0 && t->elem == TYPE_UINT8)
        return TYPE_BYTES;
    if (t == TYPE_STRING || t == TYPE_BOOL || t == TYPE_FLOAT32 || t == TYPE_FLOAT64 ||
        t == TYPE_INT || t == TYPE_INT8 || t == TYPE_INT16 || t == TYPE_INT32 ||
        t == TYPE_INT64 || t == TYPE_UINT || t == TYPE_UINT8 || t == TYPE_UINT16 ||
        t == TYPE_UINT32 || t == TYPE_UINT64)
        return t;
    return NULL;
}

static void fuzz_entry_free(FuzzEntry *e) {
    for (Int i = 0; i < e->n; i++)
        burrow__testing_value_free(e->values[i]);
    if (e->values != NULL)
        mem_free(heap_allocator(), e->values, (size_t)e->n * sizeof(Any),
                 _Alignof(Any));
    str_release(e->path);
}

static void f_free(void *env) {
    TestingF *f = (TestingF *)env;
    Alloc *h = heap_allocator();
    for (Int i = 0; i < f->ncorpus; i++)
        fuzz_entry_free(&f->corpus[i]);
    if (f->corpus != NULL)
        mem_free(h, f->corpus, (size_t)f->ccorpus * sizeof(FuzzEntry),
                 _Alignof(FuzzEntry));
    if (f->seeds != NULL)
        mem_free(h, f->seeds, (size_t)f->nseeds * sizeof(FuzzSeed), _Alignof(FuzzSeed));
    mem_free(h, f, sizeof(TestingF), _Alignof(TestingF));
}

/* A type as reflect.Type's String spells it, which is what %T prints. Only
 * the descriptor is looked at for the types fuzzing allows, and the zeroes
 * are there for the ones it does not. */
static Str type_string(Alloc *a, const Type *t) {
    static const uint64_t zero[4] = {0};
    Any v = {t, (void *)(uintptr_t)zero};
    return fmt_sprintf_v(a, "%T", v);
}

/* The "[int string]" CheckCorpus prints for a list of types. */
static Str type_list(Alloc *a, const Any *values, const Type *const *types, Int n) {
    Buf b = {0};
    buf_str(&b, BURROW_S("["));
    for (Int i = 0; i < n; i++) {
        if (i > 0)
            buf_str(&b, BURROW_S(" "));
        buf_str(&b, values != NULL ? fmt_sprintf_v(a, "%T", values[i])
                                   : type_string(a, types[i]));
    }
    buf_str(&b, BURROW_S("]"));
    Str s = str_clone(a, buf_view(&b));
    buf_free(&b);
    return s;
}

static void corpus_push(TestingF *f, FuzzEntry e) {
    if (f->ncorpus == f->ccorpus) {
        Int ncap = f->ccorpus == 0 ? 4 : f->ccorpus * 2;
        f->corpus = (FuzzEntry *)must_realloc(
            heap_allocator(), f->corpus, (size_t)f->ccorpus * sizeof(FuzzEntry),
            (size_t)ncap * sizeof(FuzzEntry), _Alignof(FuzzEntry));
        f->ccorpus = ncap;
    }
    f->corpus[f->ncorpus++] = e;
}

void testing_f_add(TestingF *f, Slice args) {
    const Any *v = (const Any *)args.p;
    for (Int i = 0; i < args.len; i++)
        if (fuzz_type(v[i].t) == NULL)
            panic_str(fmt_sprintf_v(error_allocator(),
                                    "testing: unsupported type to Add %T", v[i]));
    Alloc *h = heap_allocator();
    Any *values = NULL;
    if (args.len > 0) {
        values = (Any *)must_alloc(h, (size_t)args.len * sizeof(Any), _Alignof(Any));
        for (Int i = 0; i < args.len; i++)
            values[i] = burrow__testing_value_copy(fuzz_type(v[i].t), v[i].data);
    }
    Str path = str_clone(h, fmt_sprintf_v(error_allocator(), "seed#%d", f->ncorpus));
    corpus_push(f, (FuzzEntry){path, values, args.len});
}

/* CheckCorpus. Answers with the error's text, or an empty string. */
static Str check_corpus(Alloc *a, const FuzzEntry *e, const Type *const *types,
                        Int ntypes) {
    if (e->n != ntypes)
        return fmt_sprintf_v(a, "wrong number of values in corpus entry: %d, want %d",
                             e->n, ntypes);
    for (Int i = 0; i < ntypes; i++)
        if (e->values[i].t != types[i])
            return fmt_sprintf_v(a, "mismatched types in corpus entry: %s, want %s",
                                 type_list(a, e->values, NULL, e->n),
                                 type_list(a, NULL, types, ntypes));
    return BURROW_S("");
}

/* corpusDir, and the separator filepath.Join puts between its parts. */
#if defined(BURROW_OS_WINDOWS)
#define FUZZ_SEP "\\"
#else
#define FUZZ_SEP "/"
#endif

/* A path for the PAL, which wants it NUL terminated. */
static char *path_cstr(Str s) {
    char *p = (char *)must_alloc(heap_allocator(), (size_t)s.len + 1, 1);
    memcpy(p, s.p, (size_t)s.len);
    p[s.len] = '\0';
    return p;
}

static void path_cstr_free(char *p) {
    mem_free(heap_allocator(), p, strlen(p) + 1, 1);
}

/* os.ReadFile, answering with the PalErrno and the operation that failed. */
static bool read_whole_file(const char *path, Buf *out, PalErrno *err,
                            const char **op) {
    *op = "open";
    int64_t fd = pal_open(path, PAL_O_RDONLY, 0, err);
    if (fd == PAL_INVALID_HANDLE)
        return false;
    *op = "read";
    Byte chunk[4096];
    for (;;) {
        int64_t n = pal_read(fd, chunk, (int64_t)sizeof chunk, err);
        if (n < 0) {
            pal_close(fd, NULL);
            return false;
        }
        if (n == 0)
            break;
        buf_append(out, chunk, (Int)n);
    }
    pal_close(fd, NULL);
    return true;
}

static int corpus_name_cmp(Str a, Str b) {
    Int n = a.len < b.len ? a.len : b.len;
    int c = n > 0 ? memcmp(a.p, b.p, (size_t)n) : 0;
    if (c != 0)
        return c;
    return a.len < b.len ? -1 : a.len > b.len ? 1 : 0;
}

/* The names in dir that are not directories, sorted, as os.ReadDir gives
 * them. Answers false with *err set when the directory cannot be read. */
static bool read_dir_names(const char *dir, Str **names, Int *n, PalErrno *err) {
    Alloc *h = heap_allocator();
    *names = NULL;
    *n = 0;
    int64_t fd = pal_open(dir, PAL_O_RDONLY | PAL_O_DIRECTORY, 0, err);
    if (fd == PAL_INVALID_HANDLE)
        return false;
    PalDir *d = (PalDir *)must_alloc(h, sizeof(PalDir), _Alignof(PalDir));
    d->fd = fd;
    PalDirEntry *e =
        (PalDirEntry *)must_alloc(h, sizeof(PalDirEntry), _Alignof(PalDirEntry));
    Int cap = 0;
    *err = PAL_OK;
    while (pal_readdir(d, e, err)) {
        Str name = str_from_bytes(e->name, e->name_len);
        uint32_t type = e->type;
        if (type == 0) {
            /* The filesystem did not say, so ask, which is what DirEntry's
             * IsDir does on such a filesystem. */
            Str full = fmt_sprintf_v(error_allocator(), "%s" FUZZ_SEP "%s", dir, name);
            char *p = path_cstr(full);
            PalStat st;
            if (pal_lstat(p, &st, NULL))
                type = st.mode & PAL_S_IFMT;
            path_cstr_free(p);
        }
        if (type == PAL_S_IFDIR)
            continue;
        if (*n == cap) {
            Int ncap = cap == 0 ? 8 : cap * 2;
            *names = (Str *)must_realloc(h, *names, (size_t)cap * sizeof(Str),
                                         (size_t)ncap * sizeof(Str), _Alignof(Str));
            cap = ncap;
        }
        (*names)[(*n)++] = str_clone(h, name);
    }
    pal_close(fd, NULL);
    mem_free(h, e, sizeof(PalDirEntry), _Alignof(PalDirEntry));
    mem_free(h, d, sizeof(PalDir), _Alignof(PalDir));
    if (*err != PAL_OK) {
        for (Int i = 0; i < *n; i++)
            str_release((*names)[i]);
        if (*names != NULL)
            mem_free(h, *names, (size_t)cap * sizeof(Str), _Alignof(Str));
        *names = NULL;
        *n = 0;
        return false;
    }
    for (Int i = 1; i < *n; i++)
        for (Int j = i; j > 0 && corpus_name_cmp((*names)[j - 1], (*names)[j]) > 0;
             j--) {
            Str t = (*names)[j];
            (*names)[j] = (*names)[j - 1];
            (*names)[j - 1] = t;
        }
    if (*names != NULL && *n < cap)
        *names = (Str *)must_realloc(h, *names, (size_t)cap * sizeof(Str),
                                     (size_t)*n * sizeof(Str), _Alignof(Str));
    return true;
}

/* internal/fuzz's ReadCorpus, for testdata/fuzz/<target>. What it reads goes
 * onto the end of f's corpus. Answers with the error F.Fuzz fails with, or an
 * empty string. A missing directory is no corpus and no error. */
static Str read_corpus(TestingF *f, const Type *const *types, Int ntypes) {
    Alloc *a = error_allocator();
    Alloc *h = heap_allocator();
    Str dir = fmt_sprintf_v(a, "testdata" FUZZ_SEP "fuzz" FUZZ_SEP "%s",
                            testing_t_name(f->common));
    char *cdir = path_cstr(dir);
    Str *names;
    Int n;
    PalErrno err;
    bool ok = read_dir_names(cdir, &names, &n, &err);
    path_cstr_free(cdir);
    if (!ok) {
        if (err == PAL_ENOENT)
            return BURROW_S("");
        return fmt_sprintf_v(a, "reading seed corpus from testdata: open %s: %s", dir,
                             pal_errno_string(err));
    }

    Buf errs = {0};
    Str fail = BURROW_S("");
    for (Int i = 0; i < n; i++) {
        Str path = fmt_sprintf_v(a, "%s" FUZZ_SEP "%s", dir, names[i]);
        char *cpath = path_cstr(path);
        Buf data = {0};
        const char *op;
        bool read = read_whole_file(cpath, &data, &err, &op);
        path_cstr_free(cpath);
        if (!read) {
            buf_free(&data);
            fail = fmt_sprintf_v(a, "failed to read corpus file: %s %s: %s", op, path,
                                 pal_errno_string(err));
            break;
        }
        Any *vals;
        Int nvals;
        Str why;
        bool parsed =
            burrow__testing_corpus_unmarshal(a, buf_view(&data), &vals, &nvals, &why);
        buf_free(&data);
        if (!parsed) {
            why = fmt_sprintf_v(a, "unmarshal: %s", why);
        } else {
            FuzzEntry e = {path, vals, nvals};
            why = check_corpus(a, &e, types, ntypes);
            if (why.len > 0)
                burrow__testing_values_free(vals, nvals);
            else
                corpus_push(f, (FuzzEntry){str_clone(h, path), vals, nvals});
        }
        if (why.len > 0) {
            if (errs.len > 0)
                buf_str(&errs, BURROW_S("\n"));
            buf_str(&errs, fmt_sprintf_v(a, "%q: %s", path, why));
        }
    }
    for (Int i = 0; i < n; i++)
        str_release(names[i]);
    if (names != NULL)
        mem_free(h, names, (size_t)n * sizeof(Str), _Alignof(Str));
    if (fail.len == 0 && errs.len > 0)
        fail = str_clone(a, buf_view(&errs));
    buf_free(&errs);
    return fail;
}

/* filepath.Base, for the name a corpus entry's subtest runs under. */
static Str path_base(Str p) {
    Int i = p.len;
    while (i > 0 && p.p[i - 1] != '/' && p.p[i - 1] != '\\')
        i--;
    return str_from_bytes(p.p + i, p.len - i);
}

static void seed_body(void *env, TestingT *t) {
    FuzzSeed *s = (FuzzSeed *)env;
    FuzzEntry *e = &s->f->corpus[s->i];
    BURROW_CALLF(s->f->fn, t, slice_from(e->values, e->n, e->n, TYPE_ANY));
}

void burrow__testing_f_fuzz(TestingF *f, const char *file, int line, TestingFuzzFunc ff,
                            Slice types) {
    if (f->fuzz_called)
        panic_str(BURROW_S("testing: F.Fuzz called more than once"));
    f->fuzz_called = true;
    if (testing_t_failed(f->common))
        return;
    if (BURROW_FUNC_IS_NIL(ff))
        panic_str(BURROW_S("testing: F.Fuzz must receive a function"));
    if (types.len < 1)
        panic_str(BURROW_S("testing: fuzz target must receive at least two arguments, "
                           "where the first argument is a *T"));

    Alloc *h = heap_allocator();
    const Type **ts = (const Type **)must_alloc(
        h, (size_t)types.len * sizeof(const Type *), _Alignof(const Type *));
    for (Int i = 0; i < types.len; i++) {
        const Type *t = ((const Type *const *)types.p)[i];
        ts[i] = fuzz_type(t);
        if (ts[i] == NULL) {
            mem_free(h, (void *)ts, (size_t)types.len * sizeof(const Type *),
                     _Alignof(const Type *));
            panic_str(fmt_sprintf_v(error_allocator(),
                                    "testing: unsupported type for fuzzing %s",
                                    type_string(error_allocator(), t)));
        }
    }

    Str err = BURROW_S("");
    for (Int i = 0; i < f->ncorpus && err.len == 0; i++)
        err = check_corpus(error_allocator(), &f->corpus[i], ts, types.len);
    /* The seed corpus in testdata, which has to match the types as well. */
    if (err.len == 0)
        err = read_corpus(f, ts, types.len);
    mem_free(h, (void *)ts, (size_t)types.len * sizeof(const Type *),
             _Alignof(const Type *));
    if (err.len > 0)
        burrow__testing_t_logln(f->common, file, line, BURROW__TESTING_FATAL,
                                BURROW__FMT_ARGS(BURROW_ANY_OF, err));

    f->fn = ff;
    if (f->ncorpus > 0) {
        f->seeds = (FuzzSeed *)must_alloc(h, (size_t)f->ncorpus * sizeof(FuzzSeed),
                                          _Alignof(FuzzSeed));
        f->nseeds = f->ncorpus;
    }
    for (Int i = 0; i < f->nseeds; i++) {
        f->seeds[i] = (FuzzSeed){f, i};
        f->common->in_fuzz_fn = true;
        testing_t_run(f->common, path_base(f->corpus[i].path),
                      BURROW_FN(TestingTFunc, seed_body, &f->seeds[i]));
        f->common->in_fuzz_fn = false;
    }
}

void *burrow__testing_fuzz_arg(Slice args, Int i, const Type *want) {
    if (i < 0 || i >= args.len)
        panic_str(fmt_sprintf_v(error_allocator(),
                                "testing: fuzz argument %d out of range with %d values",
                                i, args.len));
    Any v = ((const Any *)args.p)[i];
    if (fuzz_type(want) != v.t)
        panic_str(fmt_sprintf_v(error_allocator(),
                                "testing: fuzz argument %d is %T, not %s", i, v,
                                type_string(error_allocator(), want)));
    return v.data;
}

TestingT *burrow__testing_f_t(TestingF *f) {
    return f->common;
}

Str testing_f_name(TestingF *f) {
    return testing_t_name(f->common);
}

void testing_f_fail(TestingF *f) {
    check_fuzz_fn(f->common, "Fail");
    testing_t_fail(f->common);
}

bool testing_f_failed(TestingF *f) {
    return testing_t_failed(f->common);
}

void testing_f_fail_now(TestingF *f) {
    testing_t_fail_now(f->common);
}

void testing_f_skip_now(TestingF *f) {
    testing_t_skip_now(f->common);
}

bool testing_f_skipped(TestingF *f) {
    check_fuzz_fn(f->common, "Skipped");
    return testing_t_skipped(f->common);
}

void testing_f_helper(TestingF *f) {
    check_fuzz_fn(f->common, "Helper");
}

void testing_f_cleanup(TestingF *f, Func fn) {
    testing_t_cleanup(f->common, fn);
}

Context testing_f_context(TestingF *f) {
    return testing_t_context(f->common);
}

IoWriter testing_f_output(TestingF *f) {
    return testing_t_output(f->common);
}

void testing_f_attr(TestingF *f, Str key, Str value) {
    testing_t_attr(f->common, key, value);
}

TestingTB testing_f_as_testing_tb(TestingF *f) {
    return testing_t_as_testing_tb(f->common);
}

/* fRunner, less what the T's runner already does. The F lives until the T's
 * cleanups run, which is after any seed that went parallel has finished. */
static void fuzz_target_body(void *env, TestingT *t) {
    const TestingInternalFuzzTarget *ft = (const TestingInternalFuzzTarget *)env;
    TestingF *f =
        (TestingF *)must_alloc(heap_allocator(), sizeof(TestingF), _Alignof(TestingF));
    *f = (TestingF){0};
    f->common = t;
    testing_t_cleanup(t, BURROW_FN(Func, f_free, f));
    set_ran(t);
    BURROW_CALLF(ft->fn, f);
    if (!f->fuzz_called && !testing_t_skipped(t) && !testing_t_failed(t))
        testing_t_error_v(t, "returned without calling F.Fuzz, F.Fail, or F.Skip");
}

/* ---------------------------------------------------------------- tables */

#define INTERNAL_TYPE(var, T, name)                                                    \
    static const Type var = {                                                          \
        BURROW_S_INIT(name),                                                           \
        BURROW_S_INIT("testing"),                                                      \
        KIND_STRUCT,                                                                   \
        (uint32_t)sizeof(T),                                                           \
        (uint16_t)_Alignof(T),                                                         \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
        NULL,                                                                          \
        NULL,                                                                          \
        NULL,                                                                          \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
    }

INTERNAL_TYPE(internal_test_desc, TestingInternalTest, "InternalTest");
INTERNAL_TYPE(internal_benchmark_desc, TestingInternalBenchmark, "InternalBenchmark");
INTERNAL_TYPE(internal_fuzz_target_desc, TestingInternalFuzzTarget,
              "InternalFuzzTarget");
INTERNAL_TYPE(internal_example_desc, TestingInternalExample, "InternalExample");

const Type *const TYPE_TESTING_INTERNAL_TEST = &internal_test_desc;
const Type *const TYPE_TESTING_INTERNAL_BENCHMARK = &internal_benchmark_desc;
const Type *const TYPE_TESTING_INTERNAL_FUZZ_TARGET = &internal_fuzz_target_desc;
const Type *const TYPE_TESTING_INTERNAL_EXAMPLE = &internal_example_desc;

/* A copy the M owns, since -test.shuffle reorders it and the caller's table
 * is usually const. */
static void *copy_table(Slice s, size_t size, Int *n) {
    *n = s.len;
    if (s.len == 0)
        return NULL;
    void *p = must_alloc(heap_allocator(), (size_t)s.len * size, 8);
    memcpy(p, s.p, (size_t)s.len * size);
    return p;
}

TestingM *testing_main_start(TestingMatchString match, Slice tests, Slice benchmarks,
                             Slice fuzz_targets, Slice examples) {
    if (!pkg.init_ran)
        testing_init(0, NULL);
    TestingM *m =
        (TestingM *)must_alloc(heap_allocator(), sizeof(TestingM), _Alignof(TestingM));
    *m = (TestingM){0};
    m->match = match;
    m->tests = (TestingInternalTest *)copy_table(tests, sizeof(TestingInternalTest),
                                                 &m->ntests);
    m->benchmarks = (TestingInternalBenchmark *)copy_table(
        benchmarks, sizeof(TestingInternalBenchmark), &m->nbenchmarks);
    m->fuzz_targets = (TestingInternalFuzzTarget *)copy_table(
        fuzz_targets, sizeof(TestingInternalFuzzTarget), &m->nfuzz_targets);
    m->examples = (TestingInternalExample *)copy_table(
        examples, sizeof(TestingInternalExample), &m->nexamples);
    return m;
}

void testing_m_free(TestingM *m) {
    if (m == NULL)
        return;
    Alloc *a = heap_allocator();
    if (m->tests != NULL)
        mem_free(a, m->tests, (size_t)m->ntests * sizeof(TestingInternalTest), 8);
    if (m->benchmarks != NULL)
        mem_free(a, m->benchmarks,
                 (size_t)m->nbenchmarks * sizeof(TestingInternalBenchmark), 8);
    if (m->fuzz_targets != NULL)
        mem_free(a, m->fuzz_targets,
                 (size_t)m->nfuzz_targets * sizeof(TestingInternalFuzzTarget), 8);
    if (m->examples != NULL)
        mem_free(a, m->examples, (size_t)m->nexamples * sizeof(TestingInternalExample),
                 8);
    mem_free(a, m, sizeof(TestingM), _Alignof(TestingM));
}

void testing_m_set_bare(TestingM *m, bool bare) {
    m->bare = bare;
}

/* ---------------------------------------------------------------- alarm */

typedef struct Alarm {
    burrow__Thread th;
    burrow__Note note;
    bool on;
} Alarm;

static Alarm timeout_alarm;

static int name_cmp(const void *x, const void *y) {
    const Str *a = (const Str *)x;
    const Str *b = (const Str *)y;
    Int n = a->len < b->len ? a->len : b->len;
    int c = n > 0 ? memcmp(a->p, b->p, (size_t)n) : 0;
    if (c != 0)
        return c;
    return a->len < b->len ? -1 : a->len > b->len;
}

static void alarm_main(void *arg) {
    (void)arg;
    int64_t timeout = pkg.flags[F_TIMEOUT].d;
    if (burrow__note_sleep_timeout(&timeout_alarm.note, timeout))
        return;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    char db[32];
    Buf b = {0};
    buf_str(&b, fmt_sprintf_v(a, "panic: test timed out after %s",
                              dur_string(timeout, db)));
    sync_mutex_lock(&pkg.reg_mu);
    Str *list =
        (Str *)must_alloc(a, (size_t)(pkg.nall + 1) * sizeof(Str), _Alignof(Str));
    Int n = 0;
    int64_t now = burrow_nanotime();
    for (Int i = 0; i < pkg.nall; i++) {
        TestingT *t = pkg.all[i];
        if (!t->running || t->parent == NULL)
            continue;
        char eb[32];
        list[n++] = fmt_sprintf_v(
            a, "%s (%s)", t->name,
            dur_string(dur_round(now - t->running_since, TIME_SECOND), eb));
    }
    sync_mutex_unlock(&pkg.reg_mu);
    qsort(list, (size_t)n, sizeof(Str), name_cmp);
    if (n > 0)
        buf_str(&b, BURROW_S("\nrunning tests:"));
    for (Int i = 0; i < n; i++) {
        buf_str(&b, BURROW_S("\n\t"));
        buf_str(&b, list[i]);
    }
    buf_str(&b, BURROW_S("\n"));
    write_to(stderr, buf_view(&b));
    _Exit(2);
}

static int64_t start_alarm(void) {
    int64_t timeout = pkg.flags[F_TIMEOUT].d;
    if (timeout <= 0)
        return 0;
    int64_t deadline = burrow_nanotime() + timeout;
    burrow__note_init_transient(&timeout_alarm.note);
    timeout_alarm.on = burrow__thread_start(&timeout_alarm.th, alarm_main, NULL, 0);
    return deadline;
}

static void stop_alarm(void) {
    if (!timeout_alarm.on)
        return;
    burrow__note_wake(&timeout_alarm.note);
    burrow__thread_join(&timeout_alarm.th);
    burrow__note_free(&timeout_alarm.note);
    timeout_alarm.on = false;
}

/* ---------------------------------------------------------------- running */

static void parse_cpu_list(void) {
    if (pkg.cpus != NULL)
        mem_free(heap_allocator(), pkg.cpus, (size_t)pkg.ncpus * sizeof(int),
                 _Alignof(int));
    pkg.cpus = NULL;
    pkg.ncpus = 0;
    Str s = pkg.flags[F_CPU].s;
    Int count = 1;
    for (Int i = 0; i < s.len; i++)
        count += s.p[i] == ',';
    pkg.cpus =
        (int *)must_alloc(heap_allocator(), (size_t)count * sizeof(int), _Alignof(int));
    Int from = 0;
    for (Int i = 0; i <= s.len; i++) {
        if (i < s.len && s.p[i] != ',')
            continue;
        Str val = str_from_bytes(s.p + from, i - from);
        from = i + 1;
        while (val.len > 0 && is_space_rune(val.p[0]))
            val = str_from_bytes(val.p + 1, val.len - 1);
        while (val.len > 0 && is_space_rune(val.p[val.len - 1]))
            val.len--;
        if (val.len == 0)
            continue;
        Error err = {0};
        Int cpu = strconv_atoi(val, &err);
        if (err.vt != NULL || cpu <= 0 || cpu > 1 << 20) {
            Arena ar;
            arena_init(&ar, NULL, 0);
            write_to(err_out(),
                     fmt_sprintf_v(arena_allocator(&ar),
                                   "testing: invalid value %q for -test.cpu\n", val));
            arena_free(&ar);
            exit(1);
        }
        pkg.cpus[pkg.ncpus++] = (int)cpu;
    }
    if (pkg.ncpus == 0)
        pkg.cpus[pkg.ncpus++] = runtime_gomaxprocs(0);
}

/* One of tests and fuzz is empty: a pass runs the tests or the seed corpus
 * of the fuzz targets, which Go runs as a pass of their own after the tests. */
typedef struct RunState {
    TestingMatchString match;
    const TestingInternalTest *tests;
    Int ntests;
    const TestingInternalFuzzTarget *fuzz;
    Int nfuzz;
    int64_t deadline;
    bool bare;
    bool ran;
    bool ok;
} RunState;

static void run_all(void *env, TestingT *t) {
    RunState *rs = (RunState *)env;
    for (Int i = 0; i < rs->ntests; i++)
        testing_t_run(t, rs->tests[i].name, rs->tests[i].f);
    for (Int i = 0; i < rs->nfuzz; i++)
        testing_t_run(
            t, rs->fuzz[i].name,
            BURROW_FN(TestingTFunc, fuzz_target_body, (void *)(uintptr_t)&rs->fuzz[i]));
}

/* One pass for one -test.cpu value: -test.count runs of every test. */
static void run_pass(void *env) {
    RunState *rs = (RunState *)env;
    for (uint64_t i = 0; i < pkg.flags[F_COUNT].u; i++) {
        if (should_fail_fast())
            break;
        if (i > 0 && !rs->ran)
            break;
        burrow__TestingMatcher *m = burrow__testing_matcher_new(
            rs->match, pkg.flags[F_RUN].s, BURROW_S("-test.run"), pkg.flags[F_SKIP].s);
        TestState *s = tstate_new((int)pkg.flags[F_PARALLEL].i, m, rs->bare);
        s->deadline = rs->deadline;
        Chatty *chatty = NULL;
        if (pkg.flags[F_V].b) {
            chatty = (Chatty *)must_alloc(heap_allocator(), sizeof(Chatty),
                                          _Alignof(Chatty));
            *chatty = (Chatty){0};
            chatty->json = pkg.flags[F_V].json;
        }
        TestingT *t = t_new(NULL, BURROW_S(""), s, chatty);
        t_runner(t, BURROW_FN(TestingTFunc, run_all, rs));
        rs->ok = rs->ok && !testing_t_failed(t);
        rs->ran = rs->ran || t->ran;
        free_all();
        tstate_free(s);
        if (chatty != NULL) {
            buf_free(&chatty->last_name);
            mem_free(heap_allocator(), chatty, sizeof(Chatty), _Alignof(Chatty));
        }
    }
}

static bool run_passes(RunState *rs) {
    for (Int k = 0; k < pkg.ncpus; k++) {
        if (!rs->bare && burrow__curg() == NULL) {
            runtime_gomaxprocs(pkg.cpus[k]);
            runtime_main(BURROW_FN(Func, run_pass, rs));
        } else {
            run_pass(rs);
        }
    }
    return rs->ok;
}

static bool run_tests(TestingMatchString match, const TestingInternalTest *tests,
                      Int ntests, int64_t deadline, bool bare, bool *ran) {
    RunState rs = {match, tests, ntests, NULL, 0, deadline, bare, false, true};
    bool ok = run_passes(&rs);
    *ran = rs.ran;
    return ok;
}

/* runFuzzTests: the seed corpus of every fuzz target, as tests. */
static bool run_fuzz_tests(TestingMatchString match,
                           const TestingInternalFuzzTarget *fuzz, Int nfuzz,
                           int64_t deadline, bool bare, bool *ran) {
    *ran = false;
    if (nfuzz == 0 || pkg.flags[F_FUZZWORKER].b)
        return true;
    RunState rs = {match, NULL, 0, fuzz, nfuzz, deadline, bare, false, true};
    bool ok = run_passes(&rs);
    *ran = rs.ran;
    return ok;
}

bool testing_run_tests(TestingMatchString match, Slice tests) {
    if (!pkg.parsed)
        flags_parse();
    if (pkg.cpus == NULL)
        parse_cpu_list();
    int64_t deadline = 0;
    if (pkg.flags[F_TIMEOUT].d > 0)
        deadline = burrow_nanotime() + pkg.flags[F_TIMEOUT].d;
    bool ran;
    bool ok = run_tests(match, (const TestingInternalTest *)tests.p, tests.len,
                        deadline, false, &ran);
    if (!ran && !pkg.have_examples)
        write_to(err_out(), BURROW_S("testing: warning: no tests to run\n"));
    return ok;
}

/* --------------------------------------------------------------- examples
 *
 * Go's src/testing/example.go and run_example.go. An example runs with the
 * process's standard output captured, and passes when what it printed is the
 * output it was listed with, both trimmed of white space at the ends first.
 * An example that panics fails, and then the panic carries on up and ends the
 * run, as it does in Go. */

/* unicode.IsSpace. */
static bool ex_space(Rune r) {
    switch (r) {
    case '\t':
    case '\n':
    case '\v':
    case '\f':
    case '\r':
    case ' ':
    case 0x85:
    case 0xA0:
    case 0x1680:
    case 0x2028:
    case 0x2029:
    case 0x202F:
    case 0x205F:
    case 0x3000:
        return true;
    default:
        return r >= 0x2000 && r <= 0x200A;
    }
}

/* strings.TrimSpace. */
static Str ex_trim(Str s) {
    while (s.len > 0) {
        Int n;
        if (!ex_space(utf8_decode_rune_in_string(s, &n)))
            break;
        s = str_from_bytes(s.p + n, s.len - n);
    }
    while (s.len > 0) {
        Int n;
        if (!ex_space(utf8_decode_last_rune_in_string(s, &n)))
            break;
        s.len -= n;
    }
    return s;
}

#if defined(BURROW_OS_WINDOWS)
/* strings.ReplaceAll(s, "\r\n", "\n"), which Go does to both sides on
 * Windows. */
static Str ex_crlf(Alloc *a, Str s) {
    Byte *p = (Byte *)must_alloc(a, (size_t)s.len + 1, 1);
    Int n = 0;
    for (Int i = 0; i < s.len; i++) {
        if (s.p[i] == '\r' && i + 1 < s.len && s.p[i + 1] == '\n')
            continue;
        p[n++] = (Byte)s.p[i];
    }
    return str_from_bytes((const char *)p, n);
}
#endif

/* The lines of s, split on newlines, in sorted order. */
static Str *ex_lines(Alloc *a, Str s, Int *n) {
    Int count = 1;
    for (Int i = 0; i < s.len; i++)
        if (s.p[i] == '\n')
            count++;
    Str *lines = (Str *)must_alloc(a, (size_t)count * sizeof(Str), _Alignof(Str));
    Int k = 0;
    Int start = 0;
    for (Int i = 0; i <= s.len; i++) {
        if (i < s.len && s.p[i] != '\n')
            continue;
        Str line = str_from_bytes(s.p + start, i - start);
        Int j = k++;
        while (j > 0 && str_cmp(lines[j - 1], line) > 0) {
            lines[j] = lines[j - 1];
            j--;
        }
        lines[j] = line;
        start = i + 1;
    }
    *n = count;
    return lines;
}

/* sortLines(a) == sortLines(b). The lines have no newlines in them, so two
 * joins are equal exactly when the sorted lists are. */
static bool ex_same_lines(Alloc *a, Str x, Str y) {
    Int nx;
    Int ny;
    Str *lx = ex_lines(a, x, &nx);
    Str *ly = ex_lines(a, y, &ny);
    if (nx != ny)
        return false;
    for (Int i = 0; i < nx; i++)
        if (!str_eq(lx[i], ly[i]))
            return false;
    return true;
}

static void ex_sink(void *env, const void *p, int64_t n) {
    buf_append((Buf *)env, p, (Int)n);
}

typedef struct ExampleState {
    Buf out;
    bool finished;
    bool has_panic;
    Any perr;
} ExampleState;

/* runExample and processRunResult, which say whether eg passed. */
static bool run_example(const TestingInternalExample *eg) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    bool chatty = pkg.flags[F_V].b;
    const char *prefix = pkg.flags[F_V].json ? "\x16" : "";
    if (chatty)
        write_to(stdout, fmt_sprintf_v(a, "%s=== RUN   %s\n", prefix, eg->name));

    ExampleState st = {0};
    ExampleState *sp = &st;
    PalStdoutCapture cap;
    PalErrno perr = PAL_OK;
    if (!pal_stdout_capture_begin(&cap, ex_sink, &sp->out, &perr)) {
        write_to(stderr, fmt_sprintf_v(a, "%s\n", pal_errno_string(perr)));
        exit(1);
    }
    int64_t start = burrow_nanotime();
    BURROW_TRY {
        BURROW_CALLF0(eg->f);
        sp->finished = true;
    }
    BURROW_CATCH(p) {
        if (pkg.repanicking)
            panic(pkg.repanic);
        sp->perr = keep_panic(p);
        sp->has_panic = true;
    }
    BURROW_TRY_END;
    int64_t spent = burrow_nanotime() - start;
    if (!pal_stdout_capture_end(&cap, &perr)) {
        write_to(stderr, fmt_sprintf_v(a, "testing: copying pipe: %s\n",
                                       pal_errno_string(perr)));
        exit(1);
    }

    Str stdout_s = buf_view(&sp->out);
    Str got = ex_trim(stdout_s);
    Str want = ex_trim(eg->output);
#if defined(BURROW_OS_WINDOWS)
    got = ex_crlf(a, got);
    want = ex_crlf(a, want);
#endif
    Str fail = BURROW_STR_EMPTY;
    if (eg->unordered) {
        if (!ex_same_lines(a, got, want) && !sp->has_panic)
            fail = fmt_sprintf_v(a, "got:\n%s\nwant (unordered):\n%s\n", stdout_s,
                                 eg->output);
    } else if (!str_eq(got, want) && !sp->has_panic) {
        fail = fmt_sprintf_v(a, "got:\n%s\nwant:\n%s\n", got, want);
    }
    bool passed = true;
    Str dstr = fmt_dur(a, spent);
    if (fail.len > 0 || !sp->finished || sp->has_panic) {
        write_to(stdout, fmt_sprintf_v(a, "%s--- FAIL: %s (%s)\n%s", prefix, eg->name,
                                       dstr, fail));
        passed = false;
    } else if (chatty) {
        write_to(stdout,
                 fmt_sprintf_v(a, "%s--- PASS: %s (%s)\n", prefix, eg->name, dstr));
    }
    if (chatty && pkg.flags[F_V].json)
        write_to(stdout, fmt_sprintf_v(a, "%s=== NAME   \n", prefix));
    buf_free(&sp->out);
    arena_free(&ar);
    if (sp->has_panic)
        panic(sp->perr);
    return passed;
}

typedef struct ExampleRun {
    TestingMatchString match;
    const TestingInternalExample *examples;
    Int n;
    bool ran;
    bool ok;
} ExampleRun;

static void examples_body(void *env) {
    ExampleRun *er = (ExampleRun *)env;
    burrow__TestingMatcher *m = burrow__testing_matcher_new(
        er->match, pkg.flags[F_RUN].s, BURROW_S("-test.run"), pkg.flags[F_SKIP].s);
    for (Int i = 0; i < er->n; i++) {
        bool matched;
        bool partial;
        str_release(burrow__testing_full_name(m, NULL, er->examples[i].name, &matched,
                                              &partial));
        if (!matched)
            continue;
        er->ran = true;
        if (!run_example(&er->examples[i]))
            er->ok = false;
    }
    burrow__testing_matcher_free(m);
}

/* runExamples. Examples run on the main goroutine in Go, so here they run in
 * one of their own like a test does, unless the run is bare. */
static bool run_examples(TestingMatchString match, const TestingInternalExample *egs,
                         Int n, bool bare, bool *ran) {
    ExampleRun er = {match, egs, n, false, true};
    if (n > 0) {
        if (!bare && burrow__curg() == NULL)
            runtime_main(BURROW_FN(Func, examples_body, &er));
        else
            examples_body(&er);
    }
    *ran = er.ran;
    return er.ok;
}

/* ------------------------------------------------------------- benchmarks
 *
 * Go's src/testing/benchmark.go. A B is a T's common part with the timer and
 * the counts beside it, and the T functions above work on it unchanged. What
 * a B does differently is where it runs: runN on a goroutine of its own, or on
 * the calling thread in bare mode, with the benchmark lock held while the
 * function runs so that two benchmarks never overlap. */

#define LOOP_POISON_TIMER ((uint64_t)1 << 63)
#define LOOP_POISON_MASK (~(((uint64_t)1 << 62) - 1))
#define MAX_BENCH_PREDICT_ITERS 1000000000

typedef struct BenchTime {
    int64_t d;
    int64_t n;
} BenchTime;

typedef struct BenchState {
    burrow__TestingMatcher *match;
    int max_len; /* the longest name, for the padding */
    int ext_len; /* the longest -N suffix */
} BenchState;

struct TestingB {
    burrow__TestingBHead head; /* first, where testing_b_loop looks for it */
    TestingT common;
    BenchState *bstate;
    int64_t previous_n;
    int64_t previous_duration;
    TestingBFunc bench_func;
    BenchTime bench_time;
    int64_t bytes;
    bool missing_bytes;
    bool timer_on;
    bool show_alloc_result;
    bool loop_done;
    bool discard; /* testing_benchmark's, which prints nothing */
    TestingBenchmarkResult result;
    int parallelism;
    uint64_t start_allocs;
    uint64_t start_bytes;
    uint64_t net_allocs;
    uint64_t net_bytes;

    /* Go's extra map, kept sorted by unit, with the units on the heap. */
    TestingMetric *extra;
    Int nextra;
    Int cextra;

    /* The sub-benchmarks this one started, which it frees with itself. */
    TestingB **kids;
    Int nkids;
    Int ckids;
};

struct TestingPB {
    uint64_t *global_n;
    uint64_t grain;
    uint64_t cache;
    uint64_t bn;
};

TestingT *burrow__testing_b_t(TestingB *b) {
    return &b->common;
}

static void bench_count_allocs(bool on) {
    sync_mutex_lock(&pkg.reg_mu);
    pkg.heap_counting += on ? 1 : -1;
    burrow__heap_count(pkg.heap_counting > 0);
    sync_mutex_unlock(&pkg.reg_mu);
}

/* -test.benchtime, which testing_benchmark reads too. Go's Benchmark sees
 * whatever flag.Parse left there, and the nearest thing here is parsing the
 * command line testing_init was given, if it was given one. */
static BenchTime bench_time_flag(void) {
    if (!pkg.init_ran)
        return (BenchTime){TIME_SECOND, 0};
    if (!pkg.parsed)
        flags_parse();
    return (BenchTime){pkg.flags[F_BENCHTIME].d, pkg.flags[F_BENCHTIME].n};
}

static const char *chatty_prefix(Chatty *c) {
    return c != NULL && c->json ? "\x16" : "";
}

static void b_write(TestingB *b, Str s) {
    if (!b->discard)
        write_to(stdout, s);
}

static TestingB *b_new(TestingB *parent, Str name, Chatty *chatty, TestingBFunc f,
                       BenchTime bt, BenchState *bs, bool bare, bool discard) {
    TestingB *b =
        (TestingB *)must_alloc(heap_allocator(), sizeof(TestingB), _Alignof(TestingB));
    *b = (TestingB){0};
    TestingT *c = &b->common;
    c->name = name;
    c->parent = parent != NULL ? &parent->common : NULL;
    c->level = parent != NULL ? parent->common.level + 1 : 0;
    c->chatty = chatty;
    c->has_o = true;
    c->bench = true;
    c->ctx = context_with_cancel(heap_allocator(), context_background(), &c->cancel);
    if (!bare)
        c->signal = chan_make(heap_allocator(), TYPE_BOOL, 1);
    b->bench_func = f;
    b->bench_time = bt;
    b->bstate = bs;
    b->discard = discard;
    if (parent != NULL) {
        if (parent->nkids == parent->ckids) {
            Int ncap = parent->ckids == 0 ? 4 : parent->ckids * 2;
            parent->kids = (TestingB **)must_realloc(
                heap_allocator(), (void *)parent->kids,
                (size_t)parent->ckids * sizeof(TestingB *),
                (size_t)ncap * sizeof(TestingB *), _Alignof(TestingB *));
            parent->ckids = ncap;
        }
        parent->kids[parent->nkids++] = b;
    }
    return b;
}

static void metrics_free(TestingMetric *m, Int n, Int cap) {
    for (Int i = 0; i < n; i++)
        str_release(m[i].unit);
    if (m != NULL)
        mem_free(heap_allocator(), m, (size_t)cap * sizeof(TestingMetric),
                 _Alignof(TestingMetric));
}

static void b_free(TestingB *b) {
    if (b == NULL)
        return;
    for (Int i = 0; i < b->nkids; i++)
        b_free(b->kids[i]);
    if (b->kids != NULL)
        mem_free(heap_allocator(), (void *)b->kids,
                 (size_t)b->ckids * sizeof(TestingB *), _Alignof(TestingB *));
    metrics_free(b->extra, b->nextra, b->cextra);
    testing_benchmark_result_free(&b->result);
    t_release(&b->common);
    mem_free(heap_allocator(), b, sizeof(TestingB), _Alignof(TestingB));
}

/* --------------------------------------------------------------- the timer */

void testing_b_start_timer(TestingB *b) {
    if (!b->timer_on) {
        burrow__heap_counts(&b->start_allocs, &b->start_bytes);
        b->common.start = burrow_nanotime();
        b->timer_on = true;
        b->head.loop_i &= ~LOOP_POISON_TIMER;
    }
}

void testing_b_stop_timer(TestingB *b) {
    if (b->timer_on) {
        b->common.duration += burrow_nanotime() - b->common.start;
        uint64_t allocs;
        uint64_t bytes;
        burrow__heap_counts(&allocs, &bytes);
        b->net_allocs += allocs - b->start_allocs;
        b->net_bytes += bytes - b->start_bytes;
        b->timer_on = false;
        b->head.loop_i |= LOOP_POISON_TIMER;
    }
}

void testing_b_reset_timer(TestingB *b) {
    for (Int i = 0; i < b->nextra; i++)
        str_release(b->extra[i].unit);
    b->nextra = 0;
    if (b->timer_on) {
        burrow__heap_counts(&b->start_allocs, &b->start_bytes);
        b->common.start = burrow_nanotime();
    }
    b->common.duration = 0;
    b->net_allocs = 0;
    b->net_bytes = 0;
}

void testing_b_set_bytes(TestingB *b, int64_t n) {
    b->bytes = n;
}

void testing_b_report_allocs(TestingB *b) {
    b->show_alloc_result = true;
}

int64_t testing_b_elapsed(TestingB *b) {
    int64_t d = b->common.duration;
    if (b->timer_on)
        d += burrow_nanotime() - b->common.start;
    return d;
}

static Int metric_find(const TestingMetric *m, Int n, Str unit, bool *found) {
    Int lo = 0;
    Int hi = n;
    while (lo < hi) {
        Int mid = lo + (hi - lo) / 2;
        int c = name_cmp(&m[mid].unit, &unit);
        if (c == 0) {
            *found = true;
            return mid;
        }
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    *found = false;
    return lo;
}

void testing_b_report_metric(TestingB *b, double n, Str unit) {
    if (unit.len == 0)
        panic_str(BURROW_S("metric unit must not be empty"));
    for (Int i = 0; i < unit.len;) {
        Int size = 0;
        Rune r =
            utf8_decode_rune_in_string(str_from_bytes(unit.p + i, unit.len - i), &size);
        if (is_space_rune(r))
            panic_str(BURROW_S("metric unit must not contain whitespace"));
        i += size;
    }
    bool found;
    Int at = metric_find(b->extra, b->nextra, unit, &found);
    if (found) {
        b->extra[at].value = n;
        return;
    }
    if (b->nextra == b->cextra) {
        Int ncap = b->cextra == 0 ? 16 : b->cextra * 2;
        b->extra = (TestingMetric *)must_realloc(
            heap_allocator(), b->extra, (size_t)b->cextra * sizeof(TestingMetric),
            (size_t)ncap * sizeof(TestingMetric), _Alignof(TestingMetric));
        b->cextra = ncap;
    }
    memmove(b->extra + at + 1, b->extra + at,
            (size_t)(b->nextra - at) * sizeof(TestingMetric));
    b->extra[at] = (TestingMetric){str_clone(heap_allocator(), unit), n};
    if (b->extra[at].unit.p == NULL)
        panic_str(BURROW_S("testing: out of memory"));
    b->nextra++;
}

/* The metrics as a result owns them: an array of exactly the right size. */
static void metrics_take(TestingB *b, TestingBenchmarkResult *r) {
    r->extra = NULL;
    r->nextra = b->nextra;
    if (b->nextra > 0) {
        r->extra = (TestingMetric *)must_alloc(
            heap_allocator(), (size_t)b->nextra * sizeof(TestingMetric),
            _Alignof(TestingMetric));
        memcpy(r->extra, b->extra, (size_t)b->nextra * sizeof(TestingMetric));
    }
    b->nextra = 0;
}

/* ------------------------------------------------------------------ runN */

static void bench_key_push(void) {
    uintptr_t key = current_key();
    sync_mutex_lock(&pkg.reg_mu);
    if (pkg.nbench_keys == pkg.cbench_keys) {
        Int ncap = pkg.cbench_keys == 0 ? 8 : pkg.cbench_keys * 2;
        pkg.bench_keys = (uintptr_t *)must_realloc(
            heap_allocator(), pkg.bench_keys,
            (size_t)pkg.cbench_keys * sizeof(uintptr_t),
            (size_t)ncap * sizeof(uintptr_t), _Alignof(uintptr_t));
        pkg.cbench_keys = ncap;
    }
    pkg.bench_keys[pkg.nbench_keys++] = key;
    sync_mutex_unlock(&pkg.reg_mu);
}

static void bench_key_pop(void) {
    uintptr_t key = current_key();
    sync_mutex_lock(&pkg.reg_mu);
    for (Int i = pkg.nbench_keys - 1; i >= 0; i--) {
        if (pkg.bench_keys[i] == key) {
            memmove(pkg.bench_keys + i, pkg.bench_keys + i + 1,
                    (size_t)(pkg.nbench_keys - i - 1) * sizeof(uintptr_t));
            pkg.nbench_keys--;
            break;
        }
    }
    if (pkg.nbench_keys == 0 && pkg.bench_keys != NULL) {
        mem_free(heap_allocator(), pkg.bench_keys,
                 (size_t)pkg.cbench_keys * sizeof(uintptr_t), _Alignof(uintptr_t));
        pkg.bench_keys = NULL;
        pkg.cbench_keys = 0;
    }
    sync_mutex_unlock(&pkg.reg_mu);
}

/* What runN keeps across the TRY. The deferred half takes its address, so it
 * lives in memory and nothing in it is lost to the longjmp. */
typedef struct RunN {
    TestingB *b;
    bool returned;
    bool stopped;
    bool has_panic;
    Any perr;
} RunN;

/* The deferred half of runN, which also runs when the benchmark calls
 * runtime_goexit itself. */
static void run_n_exit(void *arg) {
    RunN *r = (RunN *)arg;
    if (!r->returned) {
        Any ignored;
        run_cleanup(&r->b->common, &ignored);
    }
    bench_key_pop();
    sync_mutex_unlock(&pkg.bench_mu);
}

/* runN. Answers false when the benchmark stopped itself with FailNow or
 * SkipNow, which in Go ends the goroutine and so everything after runN too. */
static bool run_n(TestingB *b, int64_t n) {
    RunN st = {b, false, false, false, {0}};
    RunN *rn = &st;
    sync_mutex_lock(&pkg.bench_mu);
    BURROW_SCOPE {
        BURROW_DEFER(run_n_exit, rn);
        bench_key_push();
        TestingT *c = &b->common;
        context_release(c->ctx);
        c->ctx =
            context_with_cancel(heap_allocator(), context_background(), &c->cancel);
        b->head.n = (Int)n;
        b->head.loop_n = 0;
        b->head.loop_i = 0;
        b->loop_done = false;
        b->parallelism = 1;
        testing_b_reset_timer(b);
        testing_b_start_timer(b);
        BURROW_TRY {
            BURROW_CALLF(b->bench_func, b);
        }
        BURROW_CATCH(p) {
            if (pkg.repanicking)
                panic(pkg.repanic);
            if (is_stop(p)) {
                rn->stopped = true;
            } else {
                rn->perr = keep_panic(p);
                rn->has_panic = true;
            }
        }
        BURROW_TRY_END;
        if (!rn->stopped && !rn->has_panic) {
            testing_b_stop_timer(b);
            b->previous_n = n;
            b->previous_duration = c->duration;
            if (b->head.loop_n > 0 && !b->loop_done && !testing_t_failed(c))
                burrow__testing_t_logln(
                    c, __FILE__, __LINE__, BURROW__TESTING_ERROR,
                    BURROW__FMT_ARGS(BURROW_ANY_OF,
                                     "benchmark function returned without B.Loop() == "
                                     "false (break or return in loop?)"));
        }
        Any r;
        if (run_cleanup(c, &r) && !rn->has_panic) {
            rn->perr = r;
            rn->has_panic = true;
        }
        rn->returned = true;
    }
    BURROW_SCOPE_END;
    if (rn->has_panic)
        panic(rn->perr);
    return !rn->stopped;
}

static void b_signal(void *arg) {
    TestingB *b = (TestingB *)arg;
    bool v = true;
    chan_send(b->common.signal, &v);
}

/* Runs fn(b) on a goroutine and waits for it, or in line in bare mode. */
static void b_go(TestingB *b, void (*fn)(void *)) {
    if (b->common.signal == NULL) {
        fn(b);
        return;
    }
    if (!go(BURROW_FN(Func, fn, b)))
        panic_str(BURROW_S("testing: cannot start the goroutine for a benchmark"));
    bool v;
    chan_recv(b->common.signal, &v);
}

static void run1_body(void *arg) {
    TestingB *b = (TestingB *)arg;
    if (b->common.signal == NULL) {
        run_n(b, 1);
        return;
    }
    BURROW_SCOPE {
        BURROW_DEFER(b_signal, b);
        run_n(b, 1);
    }
    BURROW_SCOPE_END;
}

/* trimOutput: at most ten lines of what the benchmark logged. */
static void trim_output(TestingT *c) {
    int lines = 0;
    for (Int j = 0; j < c->output.len; j++) {
        if (c->output.p[j] != '\n')
            continue;
        if (++lines >= 10) {
            c->output.len = j;
            buf_str(&c->output, BURROW_S("\n\t... [output truncated]\n"));
            break;
        }
    }
}

/* "--- TAG: name\n" and then what was logged. */
static void b_report(TestingB *b, const char *tag, Str name) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Buf out = {0};
    buf_str(&out, fmt_sprintf_v(arena_allocator(&ar), "%s--- %s: %s\n",
                                chatty_prefix(b->common.chatty), tag, name));
    arena_free(&ar);
    buf_append(&out, b->common.output.p, b->common.output.len);
    b_write(b, buf_view(&out));
    buf_free(&out);
}

/* run1: one iteration first, to find out whether the benchmark has
 * sub-benchmarks, which then do the measuring instead. */
static bool run1(TestingB *b) {
    BenchState *s = b->bstate;
    if (s != NULL) {
        int n = (int)b->common.name.len + s->ext_len + 1;
        if (n > s->max_len)
            s->max_len = n + 8;
    }
    b_go(b, run1_body);
    TestingT *c = &b->common;
    if (testing_t_failed(c)) {
        b_report(b, "FAIL", c->name);
        return false;
    }
    sync_mutex_lock(&c->mu);
    bool finished = c->finished;
    bool has_sub = c->has_sub;
    sync_mutex_unlock(&c->mu);
    if (has_sub || finished) {
        if (c->chatty != NULL && (c->output.len > 0 || finished)) {
            trim_output(c);
            b_report(b, testing_t_skipped(c) ? "SKIP" : "BENCH", c->name);
        }
        return false;
    }
    return true;
}

/* The goos, goarch and cpu lines, once per process. */
static void print_labels(TestingB *b, bool to_stdout) {
    sync_mutex_lock(&pkg.reg_mu);
    bool done = pkg.labels_done;
    pkg.labels_done = true;
    sync_mutex_unlock(&pkg.reg_mu);
    if (done || (!to_stdout && b->discard))
        return;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Buf out = {0};
    buf_str(&out, fmt_sprintf_v(a, "goos: %s\ngoarch: %s\n", cstr(burrow_os_name()),
                                cstr(burrow_arch_name())));
    char cpu[256];
    if (pal_cpu_name(cpu, (int64_t)sizeof cpu) > 0)
        buf_str(&out, fmt_sprintf_v(a, "cpu: %s\n", cstr(cpu)));
    write_to(stdout, buf_view(&out));
    buf_free(&out);
    arena_free(&ar);
}

static int64_t predict_n(int64_t goalns, int64_t prev_iters, int64_t prevns,
                         int64_t last) {
    if (prevns == 0)
        prevns = 1;
    /* goalns * prev_iters can overflow for a fast benchmark and a long
     * -benchtime, which Go lets wrap, and so does this, without the undefined
     * behaviour. */
    int64_t n = (int64_t)((uint64_t)goalns * (uint64_t)prev_iters) / prevns;
    n += n / 5;
    if (last <= INT64_MAX / 100 && n > 100 * last)
        n = 100 * last;
    if (n < last + 1)
        n = last + 1;
    if (n > MAX_BENCH_PREDICT_ITERS)
        n = MAX_BENCH_PREDICT_ITERS;
    return n;
}

/* launch: run the benchmark with more and more iterations until it takes
 * -test.benchtime, or the number of times given as Nx. */
static void launch(TestingB *b) {
    TestingT *c = &b->common;
    if (b->head.loop_n == 0) {
        if (b->bench_time.n > 0) {
            if (b->bench_time.n > 1 && !run_n(b, b->bench_time.n))
                return;
        } else {
            int64_t d = b->bench_time.d;
            for (int64_t n = 1;
                 !testing_t_failed(c) && c->duration < d && n < 1000000000;) {
                int64_t last = n;
                n = predict_n(d, (int64_t)b->head.n, c->duration, last);
                if (!run_n(b, n))
                    return;
            }
        }
    }
    testing_benchmark_result_free(&b->result);
    b->result = (TestingBenchmarkResult){
        b->head.n, c->duration, b->bytes, b->net_allocs, b->net_bytes, NULL, 0,
    };
    metrics_take(b, &b->result);
}

static void launch_body(void *arg) {
    TestingB *b = (TestingB *)arg;
    if (b->common.signal == NULL) {
        launch(b);
        return;
    }
    BURROW_SCOPE {
        BURROW_DEFER(b_signal, b);
        launch(b);
    }
    BURROW_SCOPE_END;
}

static TestingBenchmarkResult do_bench(TestingB *b) {
    b_go(b, launch_body);
    return b->result;
}

/* benchmarkName: the name with the -test.cpu value after it. */
static Str benchmark_name(Alloc *a, Str name, int n) {
    if (n != 1)
        return fmt_sprintf_v(a, "%s-%d", name, n);
    return name;
}

/* processBench: the benchmark for each -test.cpu value, -test.count times,
 * printing Go's line for each. */
static void process_bench(BenchState *s, TestingB *b) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Chatty *chatty = b->common.chatty;
    const char *prefix = chatty_prefix(chatty);
    for (Int i = 0; i < pkg.ncpus; i++) {
        for (uint64_t j = 0; j < pkg.flags[F_COUNT].u; j++) {
            int procs = pkg.cpus[i];
            pkg.bench_procs = procs;
            Str bench_name = benchmark_name(a, b->common.name, procs);
            if (chatty == NULL)
                b_write(b, fmt_sprintf_v(a, "%-*s\t", s->max_len, bench_name));
            TestingB *cur = b;
            TestingB *again = NULL;
            if (i > 0 || j > 0) {
                again = b_new(NULL, str_clone(heap_allocator(), b->common.name), chatty,
                              b->bench_func, b->bench_time, NULL,
                              b->common.signal == NULL, b->discard);
                cur = again;
                run1(cur);
            }
            TestingBenchmarkResult r = do_bench(cur);
            TestingT *c = &cur->common;
            if (testing_t_failed(c)) {
                b_report(cur, "FAIL", bench_name);
                b_free(again);
                continue;
            }
            Buf line = {0};
            if (chatty != NULL)
                buf_str(&line, fmt_sprintf_v(a, "%-*s\t", s->max_len, bench_name));
            buf_str(&line, testing_benchmark_result_string(a, r));
            if (pkg.flags[F_BENCHMEM].b || cur->show_alloc_result) {
                buf_str(&line, BURROW_S("\t"));
                buf_str(&line, testing_benchmark_result_mem_string(a, r));
            }
            buf_str(&line, BURROW_S("\n"));
            b_write(cur, buf_view(&line));
            buf_free(&line);
            if (c->output.len > 0) {
                trim_output(c);
                b_report(cur, "BENCH", bench_name);
            }
            if (chatty != NULL && chatty->json)
                chatty_updatef(chatty, BURROW_S(""), "=== NAME  %s\n", BURROW_S(""));
            b_free(again);
        }
    }
    (void)prefix;
    arena_free(&ar);
}

static void b_run_body(TestingB *b) {
    print_labels(b, false);
    if (b->bstate != NULL)
        process_bench(b->bstate, b);
    else
        do_bench(b);
}

static void bench_relock(void *arg) {
    (void)arg;
    sync_mutex_lock(&pkg.bench_mu);
}

/* add: a parent's result is the sum of its children's per-op figures. */
static void b_add(TestingB *b, TestingBenchmarkResult other) {
    TestingBenchmarkResult *r = &b->result;
    r->n = 1;
    r->t += testing_benchmark_result_ns_per_op(other);
    if (other.bytes == 0) {
        b->missing_bytes = true;
        r->bytes = 0;
    }
    if (!b->missing_bytes)
        r->bytes += other.bytes;
    r->mem_allocs += (uint64_t)testing_benchmark_result_allocs_per_op(other);
    r->mem_bytes += (uint64_t)testing_benchmark_result_alloced_bytes_per_op(other);
}

bool testing_b_run(TestingB *b, Str name, TestingBFunc f) {
    TestingT *c = &b->common;
    sync_mutex_lock(&c->mu);
    c->has_sub = true;
    sync_mutex_unlock(&c->mu);
    volatile bool result = true;
    sync_mutex_unlock(&pkg.bench_mu);
    BURROW_SCOPE {
        BURROW_DEFER(bench_relock, NULL);
        bool ok = true;
        bool partial = false;
        Str bench_name;
        if (b->bstate != NULL)
            bench_name = burrow__testing_full_name(
                b->bstate->match, c->level > 0 ? &c->name : NULL, name, &ok, &partial);
        else
            bench_name = str_clone(heap_allocator(), c->name);
        if (!ok) {
            str_release(bench_name);
        } else {
            TestingB *sub = b_new(b, bench_name, c->chatty, f, b->bench_time, b->bstate,
                                  c->signal == NULL, b->discard);
            if (partial)
                sub->common.has_sub = true;
            if (c->chatty != NULL) {
                print_labels(b, true);
                if (c->chatty->json)
                    chatty_updatef(c->chatty, bench_name, "=== RUN   %s\n", bench_name);
                Buf line = {0};
                buf_str(&line, bench_name);
                buf_str(&line, BURROW_S("\n"));
                write_to(stdout, buf_view(&line));
                buf_free(&line);
            }
            if (run1(sub))
                b_run_body(sub);
            b_add(b, sub->result);
            result = !testing_t_failed(&sub->common);
        }
    }
    BURROW_SCOPE_END;
    return result;
}

/* ------------------------------------------------------------------- Loop */

/* stopOrScaleBLoop. */
static bool stop_or_scale(TestingB *b) {
    int64_t t = testing_b_elapsed(b);
    if (t >= b->bench_time.d)
        return false;
    int64_t prev_iters = (int64_t)b->head.loop_n;
    b->head.loop_n = (uint64_t)predict_n(b->bench_time.d, prev_iters, t, prev_iters);
    if ((b->head.loop_n & LOOP_POISON_MASK) != 0)
        panic_str(BURROW_S("loop iteration target overflow"));
    return (uint64_t)prev_iters < b->head.loop_n;
}

bool burrow__testing_b_loop_slow(TestingB *b) {
    if (!b->timer_on)
        burrow__testing_t_logln(
            &b->common, __FILE__, __LINE__, BURROW__TESTING_FATAL,
            BURROW__FMT_ARGS(BURROW_ANY_OF, "B.Loop called with timer stopped"));
    if ((b->head.loop_i & LOOP_POISON_MASK) != 0)
        panic_str(fmt_sprintf_v(error_allocator(), "unknown loop stop condition: %#x",
                                b->head.loop_i));
    if (b->head.loop_n == 0) {
        b->head.loop_n = b->bench_time.n > 0 ? (uint64_t)b->bench_time.n : 1;
        b->head.n = 0;
        testing_b_reset_timer(b);
        b->head.loop_i++;
        return true;
    }
    bool more;
    if (b->bench_time.n > 0) {
        if (b->head.loop_i != (uint64_t)b->bench_time.n)
            panic_str(fmt_sprintf_v(error_allocator(),
                                    "iteration count %d < fixed target %d",
                                    b->head.loop_i, b->bench_time.n));
        more = false;
    } else {
        more = stop_or_scale(b);
    }
    if (!more) {
        testing_b_stop_timer(b);
        b->head.n = (Int)b->head.loop_n;
        b->loop_done = true;
        return false;
    }
    b->head.loop_i++;
    return true;
}

/* ------------------------------------------------------------ RunParallel */

bool testing_pb_next(TestingPB *pb) {
    if (pb->cache == 0) {
        uint64_t n = burrow__atomic_add_u64(pb->global_n, pb->grain) + pb->grain;
        if (n <= pb->bn)
            pb->cache = pb->grain;
        else if (n < pb->bn + pb->grain)
            pb->cache = pb->bn + pb->grain - n;
        else
            return false;
    }
    pb->cache--;
    return true;
}

typedef struct Parallel {
    SyncWaitGroup wg;
    uint64_t n;
    uint64_t grain;
    uint64_t bn;
    TestingPBFunc body;
} Parallel;

static void parallel_done(void *arg) {
    sync_wait_group_done((SyncWaitGroup *)arg);
}

static void parallel_worker(void *env) {
    Parallel *par = (Parallel *)env;
    BURROW_SCOPE {
        BURROW_DEFER(parallel_done, &par->wg);
        TestingPB pb = {&par->n, par->grain, 0, par->bn};
        BURROW_CALLF(par->body, &pb);
    }
    BURROW_SCOPE_END;
}

void testing_b_run_parallel(TestingB *b, TestingPBFunc body) {
    if (b->head.n == 0)
        return;
    uint64_t grain = 0;
    if (b->previous_n > 0 && b->previous_duration > 0)
        grain = 100000 * (uint64_t)b->previous_n / (uint64_t)b->previous_duration;
    if (grain < 1)
        grain = 1;
    if (grain > 10000)
        grain = 10000;
    Parallel par = {0};
    par.grain = grain;
    par.bn = (uint64_t)b->head.n;
    par.body = body;
    int procs = pkg.bench_procs > 0 ? pkg.bench_procs : runtime_gomaxprocs(0);
    int num = b->parallelism * procs;
    if (b->common.signal == NULL) {
        /* Bare mode has no goroutines, so the bodies take turns. */
        for (int p = 0; p < num; p++) {
            TestingPB pb = {&par.n, grain, 0, par.bn};
            BURROW_CALLF(body, &pb);
        }
    } else {
        sync_wait_group_add(&par.wg, num);
        for (int p = 0; p < num; p++)
            if (!go(BURROW_FN(Func, parallel_worker, &par)))
                panic_str(BURROW_S("testing: cannot start a RunParallel goroutine"));
        sync_wait_group_wait(&par.wg);
    }
    if (burrow__atomic_load_u64(&par.n) <= par.bn && !testing_t_failed(&b->common))
        burrow__testing_t_logln(
            &b->common, __FILE__, __LINE__, BURROW__TESTING_FATAL,
            BURROW__FMT_ARGS(BURROW_ANY_OF,
                             "RunParallel: body exited without pb.Next() == false"));
}

void testing_b_set_parallelism(TestingB *b, int p) {
    if (p >= 1)
        b->parallelism = p;
}

/* ---------------------------------------------------------- B's T methods */

Str testing_b_name(TestingB *b) {
    return b->common.name;
}
void testing_b_fail(TestingB *b) {
    testing_t_fail(&b->common);
}
bool testing_b_failed(TestingB *b) {
    return testing_t_failed(&b->common);
}
void testing_b_fail_now(TestingB *b) {
    testing_t_fail_now(&b->common);
}
void testing_b_skip_now(TestingB *b) {
    testing_t_skip_now(&b->common);
}
bool testing_b_skipped(TestingB *b) {
    return testing_t_skipped(&b->common);
}
void testing_b_helper(TestingB *b) {
    (void)b;
}
void testing_b_cleanup(TestingB *b, Func f) {
    testing_t_cleanup(&b->common, f);
}
Context testing_b_context(TestingB *b) {
    return b->common.ctx;
}
IoWriter testing_b_output(TestingB *b) {
    return testing_t_output(&b->common);
}
void testing_b_attr(TestingB *b, Str key, Str value) {
    testing_t_attr(&b->common, key, value);
}
TestingTB testing_b_as_testing_tb(TestingB *b) {
    return (TestingTB){&t_tb_vt, &b->common};
}

/* --------------------------------------------------------- BenchmarkResult */

static bool result_extra(TestingBenchmarkResult r, const char *unit, double *v) {
    return testing_benchmark_result_extra(r, cstr(unit), v);
}

bool testing_benchmark_result_extra(TestingBenchmarkResult r, Str unit, double *value) {
    bool found;
    Int at = metric_find(r.extra, r.nextra, unit, &found);
    if (found && value != NULL)
        *value = r.extra[at].value;
    return found;
}

void testing_benchmark_result_free(TestingBenchmarkResult *r) {
    metrics_free(r->extra, r->nextra, r->nextra);
    r->extra = NULL;
    r->nextra = 0;
}

int64_t testing_benchmark_result_ns_per_op(TestingBenchmarkResult r) {
    double v;
    if (result_extra(r, "ns/op", &v))
        return (int64_t)v;
    if (r.n <= 0)
        return 0;
    return r.t / (int64_t)r.n;
}

static double mb_per_sec(TestingBenchmarkResult r) {
    double v;
    if (result_extra(r, "MB/s", &v))
        return v;
    if (r.bytes <= 0 || r.t <= 0 || r.n <= 0)
        return 0;
    return ((double)r.bytes * (double)r.n / 1e6) / ((double)r.t / 1e9);
}

int64_t testing_benchmark_result_allocs_per_op(TestingBenchmarkResult r) {
    double v;
    if (result_extra(r, "allocs/op", &v))
        return (int64_t)v;
    if (r.n <= 0)
        return 0;
    return (int64_t)r.mem_allocs / (int64_t)r.n;
}

int64_t testing_benchmark_result_alloced_bytes_per_op(TestingBenchmarkResult r) {
    double v;
    if (result_extra(r, "B/op", &v))
        return (int64_t)v;
    if (r.n <= 0)
        return 0;
    return (int64_t)r.mem_bytes / (int64_t)r.n;
}

/* prettyPrint: enough digits that small numbers keep three significant ones,
 * right aligned so the columns line up. */
static void pretty_print(Buf *b, Alloc *a, double x, Str unit) {
    double y = x < 0 ? -x : x;
    const char *format;
    if (y == 0 || y >= 999.95)
        format = "%10.0f %s";
    else if (y >= 99.995)
        format = "%12.1f %s";
    else if (y >= 9.9995)
        format = "%13.2f %s";
    else if (y >= 0.99995)
        format = "%14.3f %s";
    else if (y >= 0.099995)
        format = "%15.4f %s";
    else if (y >= 0.0099995)
        format = "%16.5f %s";
    else if (y >= 0.00099995)
        format = "%17.6f %s";
    else
        format = "%18.7f %s";
    buf_str(b, fmt_sprintf_v(a, format, x, unit));
}

Str testing_benchmark_result_string(Alloc *a, TestingBenchmarkResult r) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *t = arena_allocator(&ar);
    Buf b = {0};
    buf_str(&b, fmt_sprintf_v(t, "%8d", r.n));
    double ns;
    if (!result_extra(r, "ns/op", &ns))
        ns = (double)r.t / (double)r.n;
    if (ns != 0) {
        buf_str(&b, BURROW_S("\t"));
        pretty_print(&b, t, ns, BURROW_S("ns/op"));
    }
    double mbs = mb_per_sec(r);
    if (mbs != 0)
        buf_str(&b, fmt_sprintf_v(t, "\t%7.2f MB/s", mbs));
    for (Int i = 0; i < r.nextra; i++) {
        Str k = r.extra[i].unit;
        if (str_eq(k, BURROW_S("ns/op")) || str_eq(k, BURROW_S("MB/s")) ||
            str_eq(k, BURROW_S("B/op")) || str_eq(k, BURROW_S("allocs/op")))
            continue;
        buf_str(&b, BURROW_S("\t"));
        pretty_print(&b, t, r.extra[i].value, k);
    }
    Str out = str_clone(a, buf_view(&b));
    buf_free(&b);
    arena_free(&ar);
    return out;
}

Str testing_benchmark_result_mem_string(Alloc *a, TestingBenchmarkResult r) {
    return fmt_sprintf_v(a, "%8d B/op\t%8d allocs/op",
                         testing_benchmark_result_alloced_bytes_per_op(r),
                         testing_benchmark_result_allocs_per_op(r));
}

/* ------------------------------------------------------------- the runner */

typedef struct BenchRun {
    TestingMatchString match;
    const TestingInternalBenchmark *benchmarks;
    Int n;
    bool bare;
    bool ok;
    TestingBFunc single; /* testing_benchmark's */
    TestingBenchmarkResult result;
} BenchRun;

static void bench_all(void *env, TestingB *b) {
    BenchRun *br = (BenchRun *)env;
    for (Int i = 0; i < br->n; i++)
        testing_b_run(b, br->benchmarks[i].name, br->benchmarks[i].f);
}

/* runBenchmarks, from the point where the scheduler is running. */
static void bench_pass(void *env) {
    BenchRun *br = (BenchRun *)env;
    int maxprocs = 1;
    for (Int i = 0; i < pkg.ncpus; i++)
        if (pkg.cpus[i] > maxprocs)
            maxprocs = pkg.cpus[i];
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BenchState s = {0};
    s.match = burrow__testing_matcher_new(br->match, pkg.flags[F_BENCH].s,
                                          BURROW_S("-test.bench"), pkg.flags[F_SKIP].s);
    s.ext_len = (int)benchmark_name(a, BURROW_S(""), maxprocs).len;
    TestingInternalBenchmark *bs = NULL;
    Int nbs = 0;
    if (br->n > 0)
        bs = (TestingInternalBenchmark *)must_alloc(
            a, (size_t)br->n * sizeof(TestingInternalBenchmark),
            _Alignof(TestingInternalBenchmark));
    for (Int i = 0; i < br->n; i++) {
        bool ok;
        bool partial;
        Str full = burrow__testing_full_name(s.match, NULL, br->benchmarks[i].name, &ok,
                                             &partial);
        str_release(full);
        if (!ok)
            continue;
        bs[nbs++] = br->benchmarks[i];
        int l = (int)benchmark_name(a, br->benchmarks[i].name, maxprocs).len +
                s.ext_len + 1;
        if (l > s.max_len)
            s.max_len = l;
    }
    BenchRun all = *br;
    all.benchmarks = bs;
    all.n = nbs;
    Chatty *chatty = NULL;
    if (pkg.flags[F_V].b) {
        chatty =
            (Chatty *)must_alloc(heap_allocator(), sizeof(Chatty), _Alignof(Chatty));
        *chatty = (Chatty){0};
        chatty->json = pkg.flags[F_V].json;
    }
    TestingB *main_b = b_new(NULL, str_clone(heap_allocator(), BURROW_S("Main")),
                             chatty, BURROW_FN(TestingBFunc, bench_all, &all),
                             bench_time_flag(), &s, br->bare, false);
    bench_count_allocs(true);
    run_n(main_b, 1);
    bench_count_allocs(false);
    br->ok = !testing_t_failed(&main_b->common);
    b_free(main_b);
    if (chatty != NULL) {
        buf_free(&chatty->last_name);
        mem_free(heap_allocator(), chatty, sizeof(Chatty), _Alignof(Chatty));
    }
    burrow__testing_matcher_free(s.match);
    arena_free(&ar);
}

static bool run_benchmarks(TestingMatchString match, const TestingInternalBenchmark *bs,
                           Int n, bool bare) {
    if (pkg.flags[F_BENCH].s.len == 0)
        return true;
    if (pkg.cpus == NULL)
        parse_cpu_list();
    BenchRun br = {0};
    br.match = match;
    br.benchmarks = bs;
    br.n = n;
    br.bare = bare;
    br.ok = true;
    if (!bare && burrow__curg() == NULL) {
        int maxprocs = 1;
        for (Int i = 0; i < pkg.ncpus; i++)
            if (pkg.cpus[i] > maxprocs)
                maxprocs = pkg.cpus[i];
        runtime_gomaxprocs(maxprocs);
        runtime_main(BURROW_FN(Func, bench_pass, &br));
    } else {
        bench_pass(&br);
    }
    pkg.bench_procs = 0;
    return br.ok;
}

void testing_run_benchmarks(TestingMatchString match, Slice benchmarks) {
    if (!pkg.parsed)
        flags_parse();
    run_benchmarks(match, (const TestingInternalBenchmark *)benchmarks.p,
                   benchmarks.len, false);
}

static void benchmark_one(void *env) {
    BenchRun *br = (BenchRun *)env;
    TestingB *b = b_new(NULL, BURROW_S(""), NULL, br->single, bench_time_flag(), NULL,
                        burrow__curg() == NULL, true);
    bench_count_allocs(true);
    if (run1(b))
        b_run_body(b);
    bench_count_allocs(false);
    br->result = b->result;
    b->result = (TestingBenchmarkResult){0};
    b_free(b);
}

TestingBenchmarkResult testing_benchmark(TestingBFunc f) {
    BenchRun br = {0};
    br.single = f;
    if (burrow__curg() == NULL)
        runtime_main(BURROW_FN(Func, benchmark_one, &br));
    else
        benchmark_one(&br);
    return br.result;
}

static void list_tests(TestingM *m) {
    Str pat = pkg.flags[F_LIST].s;
    Error err = {0};
    burrow__testing_call_match(m->match, pat, BURROW_S("non-empty"), &err);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    if (err.vt != NULL) {
        write_to(err_out(),
                 fmt_sprintf_v(a, "testing: invalid regexp in -test.list (%q): %s\n",
                               pat, error_text(err)));
        exit(1);
    }
    Buf b = {0};
    for (Int i = 0; i < m->ntests + m->nbenchmarks + m->nfuzz_targets + m->nexamples;
         i++) {
        Str name;
        Int j = i;
        Int tb = m->ntests + m->nbenchmarks;
        Int tbf = tb + m->nfuzz_targets;
        if (j < m->ntests)
            name = m->tests[j].name;
        else if (j < tb)
            name = m->benchmarks[j - m->ntests].name;
        else if (j < tbf)
            name = m->fuzz_targets[j - tb].name;
        else
            name = m->examples[j - tbf].name;
        if (burrow__testing_call_match(m->match, pat, name, &err)) {
            buf_str(&b, name);
            buf_str(&b, BURROW_S("\n"));
        }
    }
    write_to(stdout, buf_view(&b));
    buf_free(&b);
    arena_free(&ar);
}

static uint64_t splitmix(uint64_t *s) {
    uint64_t z = (*s += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

static void shuffle_table(void *base, Int n, size_t size, uint64_t *state) {
    Byte tmp[64];
    for (Int i = n - 1; i > 0; i--) {
        Int j = (Int)(splitmix(state) % (uint64_t)(i + 1));
        Byte *x = (Byte *)base + (size_t)i * size;
        Byte *y = (Byte *)base + (size_t)j * size;
        memcpy(tmp, x, size);
        memcpy(x, y, size);
        memcpy(y, tmp, size);
    }
}

int testing_m_run(TestingM *m) {
    m->num_run++;
    if (!pkg.parsed)
        flags_parse();

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    const char *prefix = pkg.flags[F_V].json ? "\x16" : "";

    if (pkg.flags[F_PARALLEL].i < 1) {
        write_to(err_out(),
                 BURROW_S("testing: -parallel can only be given a positive integer\n"));
        usage();
        m->exit_code = 2;
        goto out;
    }
    if (pkg.flags[F_FUZZ].s.len > 0 && pkg.flags[F_FUZZCACHEDIR].s.len == 0) {
        write_to(
            err_out(),
            BURROW_S("testing: -test.fuzzcachedir must be set if -test.fuzz is set\n"));
        usage();
        m->exit_code = 2;
        goto out;
    }
    if (pkg.flags[F_LIST].s.len > 0) {
        list_tests(m);
        m->exit_code = 0;
        goto out;
    }
    if (!str_eq(pkg.flags[F_SHUFFLE].s, BURROW_S("off"))) {
        int64_t n;
        if (str_eq(pkg.flags[F_SHUFFLE].s, BURROW_S("on"))) {
            struct timespec ts;
            timespec_get(&ts, TIME_UTC);
            n = (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
        } else {
            Error err = {0};
            n = strconv_parse_int(pkg.flags[F_SHUFFLE].s, 10, 64, &err);
            if (err.vt != NULL) {
                write_to(err_out(),
                         fmt_sprintf_v(
                             a,
                             "testing: -shuffle should be \"off\", \"on\", or a valid "
                             "integer: %v\n",
                             err));
                m->exit_code = 2;
                goto out;
            }
        }
        write_to(stdout, fmt_sprintf_v(a, "-test.shuffle %d\n", n));
        uint64_t state = (uint64_t)n;
        shuffle_table(m->tests, m->ntests, sizeof(TestingInternalTest), &state);
        shuffle_table(m->benchmarks, m->nbenchmarks, sizeof(TestingInternalBenchmark),
                      &state);
    }

    parse_cpu_list();

    {
        int64_t deadline = start_alarm();
        pkg.have_examples = m->nexamples > 0;
        bool ran;
        bool ok = run_tests(m->match, m->tests, m->ntests, deadline, m->bare, &ran);
        bool fuzz_ran;
        bool fuzz_ok = run_fuzz_tests(m->match, m->fuzz_targets, m->nfuzz_targets,
                                      deadline, m->bare, &fuzz_ran);
        bool example_ran;
        bool example_ok =
            run_examples(m->match, m->examples, m->nexamples, m->bare, &example_ran);
        stop_alarm();
        if (!ran && !fuzz_ran && !example_ran && pkg.flags[F_BENCH].s.len == 0 &&
            pkg.flags[F_FUZZ].s.len == 0)
            write_to(err_out(), BURROW_S("testing: warning: no tests to run\n"));
        if (!ok || !fuzz_ok || !example_ok ||
            !run_benchmarks(m->match, m->benchmarks, m->nbenchmarks, m->bare)) {
            write_to(stdout, fmt_sprintf_v(a, "%sFAIL\n", prefix));
            m->exit_code = 1;
            goto out;
        }
    }
    m->exit_code = 0;
    write_to(stdout, fmt_sprintf_v(a, "%sPASS\n", prefix));
out:
    arena_free(&ar);
    return m->exit_code;
}

void testing_main(TestingMatchString match, Slice tests, Slice benchmarks,
                  Slice examples) {
    TestingM *m =
        testing_main_start(match, tests, benchmarks,
                           slice_nil(TYPE_TESTING_INTERNAL_FUZZ_TARGET), examples);
    int code = testing_m_run(m);
    testing_m_free(m);
    exit(code);
}

/* The functions TESTING_MAIN's table points at take a TestingT or a TestingB,
 * and these call them through the right type, with the entry as the env. */
static void entry_test(void *env, TestingT *t) {
    const burrow__TestingEntry *e = (const burrow__TestingEntry *)env;
    ((void (*)(TestingT *))e->fn)(t);
}

static void entry_benchmark(void *env, TestingB *b) {
    const burrow__TestingEntry *e = (const burrow__TestingEntry *)env;
    ((void (*)(TestingB *))e->fn)(b);
}

static void entry_fuzz(void *env, TestingF *f) {
    const burrow__TestingEntry *e = (const burrow__TestingEntry *)env;
    ((void (*)(TestingF *))e->fn)(f);
}

static void entry_example(void *env) {
    const burrow__TestingEntry *e = (const burrow__TestingEntry *)env;
    e->fn();
}

int burrow__testing_main(int argc, char **argv, const burrow__TestingEntry *entries,
                         Int n, bool bare, int (*main_fn)(TestingM *m)) {
    Alloc *h = heap_allocator();
    TestingInternalTest *tests = NULL;
    TestingInternalBenchmark *benchmarks = NULL;
    TestingInternalExample *examples = NULL;
    TestingInternalFuzzTarget *fuzz = NULL;
    if (n > 0) {
        fuzz = (TestingInternalFuzzTarget *)must_alloc(
            h, (size_t)n * sizeof(TestingInternalFuzzTarget),
            _Alignof(TestingInternalFuzzTarget));
        examples = (TestingInternalExample *)must_alloc(
            h, (size_t)n * sizeof(TestingInternalExample),
            _Alignof(TestingInternalExample));
        tests = (TestingInternalTest *)must_alloc(
            h, (size_t)n * sizeof(TestingInternalTest), _Alignof(TestingInternalTest));
        benchmarks = (TestingInternalBenchmark *)must_alloc(
            h, (size_t)n * sizeof(TestingInternalBenchmark),
            _Alignof(TestingInternalBenchmark));
    }
    Int ntests = 0;
    Int nbenchmarks = 0;
    Int nexamples = 0;
    Int nfuzz = 0;
    for (Int i = 0; i < n; i++) {
        const burrow__TestingEntry *e = &entries[i];
        if (e->kind == BURROW__TESTING_KIND_EXAMPLE) {
            /* go test leaves an example with no output comment out of the
             * table, compiled and never run, and so does this. */
            if (e->output != NULL)
                examples[nexamples++] = (TestingInternalExample){
                    e->name, BURROW_FN(Func, entry_example, (void *)(uintptr_t)e),
                    str_from_cstr(e->output), e->unordered};
        } else if (e->kind == BURROW__TESTING_KIND_FUZZ)
            fuzz[nfuzz++] = (TestingInternalFuzzTarget){
                e->name, BURROW_FN(TestingFFunc, entry_fuzz, (void *)(uintptr_t)e)};
        else if (e->kind == BURROW__TESTING_KIND_BENCHMARK)
            benchmarks[nbenchmarks++] = (TestingInternalBenchmark){
                e->name,
                BURROW_FN(TestingBFunc, entry_benchmark, (void *)(uintptr_t)e)};
        else
            tests[ntests++] = (TestingInternalTest){
                e->name, BURROW_FN(TestingTFunc, entry_test, (void *)(uintptr_t)e)};
    }
    testing_init(argc, argv);
    TestingM *m = testing_main_start(
        (TestingMatchString){NULL, NULL},
        slice_from(tests, ntests, ntests, TYPE_TESTING_INTERNAL_TEST),
        slice_from(benchmarks, nbenchmarks, nbenchmarks,
                   TYPE_TESTING_INTERNAL_BENCHMARK),
        slice_from(fuzz, nfuzz, nfuzz, TYPE_TESTING_INTERNAL_FUZZ_TARGET),
        slice_from(examples, nexamples, nexamples, TYPE_TESTING_INTERNAL_EXAMPLE));
    testing_m_set_bare(m, bare);
    int code = main_fn(m);
    testing_m_free(m);
    if (n > 0) {
        mem_free(h, tests, (size_t)n * sizeof(TestingInternalTest),
                 _Alignof(TestingInternalTest));
        mem_free(h, benchmarks, (size_t)n * sizeof(TestingInternalBenchmark),
                 _Alignof(TestingInternalBenchmark));
        mem_free(h, examples, (size_t)n * sizeof(TestingInternalExample),
                 _Alignof(TestingInternalExample));
        mem_free(h, fuzz, (size_t)n * sizeof(TestingInternalFuzzTarget),
                 _Alignof(TestingInternalFuzzTarget));
    }
    return code;
}

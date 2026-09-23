/* fmt: the scanning half, checked against what Go's scanning does.
 *
 * scan_gen.h holds every table in scan_test.go with Go's answer for each: the
 * count, the error text and what each operand holds afterwards. Each case runs
 * once over the string and once through an IoReader that hands back a single
 * byte per read, which is the slowest path a scan can take. This file adds the
 * two Scanner types those tables use and the cases Go checks by hand.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/core.h"
#include "burrow/declare.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/func.h"
#include "burrow/iface.h"
#include "burrow/io.h"
#include "burrow/mem/arena.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include "harness.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static Arena ar;
static Alloc *a;

static void setup(void) {
    arena_init(&ar, NULL, 0);
    a = arena_allocator(&ar);
}

static void teardown(void) {
    arena_free(&ar);
}

BURROW_SLICE_TYPE(ByteSlice, uint8_t);

/* ------------------------------------------------------------------ Xs */

/* Go's Xs, a string that accepts any non-empty run of the verb character. */
#define XS_FIELDS(F, T) F(T, Str, s, "")
BURROW_STRUCT_DECL(Xs, XS_FIELDS);

static bool is_verb(void *env, Rune r) {
    return r == *(Rune *)env;
}

static Error xs_scan(Xs *x, FmtScanState st, Rune verb) {
    Error err = BURROW_NO_ERROR;
    Slice tok = st.vt->token(st.data, true, BURROW_FN(RuneFunc, is_verb, &verb), &err);
    if (BURROW_FAILED(err))
        return err;
    const Byte *p = tok.p;
    Int n = tok.len;
    bool ok = n > 0;
    for (Int i = 0; i < n && ok; i++)
        ok = (Rune)p[i] == verb;
    if (!ok)
        return errors_new(error_allocator(), BURROW_S("syntax error for xs"));
    x->s = str_clone(a, (Str){p, n});
    return BURROW_NO_ERROR;
}

#define XS_SIG_Scan(IN, OUT) IN(0, FmtScanState) IN(1, Rune) OUT(Error)
#define XS_METHODS(M, T) M(T, Scan, xs_scan, XS_SIG_Scan)
BURROW_STRUCT_DEFINE_METHODS(Xs, XS_FIELDS, XS_METHODS);

/* ----------------------------------------------------------- IntString */

/* Go's IntString, an integer followed at once by a string, which scans the
 * integer with a scan of its own nested inside the outer one. */
#define INT_STRING_FIELDS(F, T)                                                        \
    F(T, Int, i, "")                                                                   \
    F(T, Str, s, "")
BURROW_STRUCT_DECL(IntString, INT_STRING_FIELDS);

static Error int_string_scan(IntString *s, FmtScanState st, Rune verb) {
    (void)verb;
    Error err = BURROW_NO_ERROR;
    Any args[] = {BURROW_ANY(TYPE_INT, &s->i)};
    fmt_fscan(a, fmt_scan_state_reader(&st), slice_from(args, 1, 1, TYPE_ANY), &err);
    if (BURROW_FAILED(err))
        return err;
    Slice tok = st.vt->token(st.data, true, (RuneFunc){NULL, NULL}, &err);
    if (BURROW_FAILED(err))
        return err;
    s->s = str_clone(a, (Str){tok.p, tok.len});
    return BURROW_NO_ERROR;
}

#define INT_STRING_SIG_Scan(IN, OUT) IN(0, FmtScanState) IN(1, Rune) OUT(Error)
#define INT_STRING_METHODS(M, T) M(T, Scan, int_string_scan, INT_STRING_SIG_Scan)
BURROW_STRUCT_DEFINE_METHODS(IntString, INT_STRING_FIELDS, INT_STRING_METHODS);

/* ------------------------------------------------------ the one-byte reader */

typedef struct OneByte {
    Str s;
    Int pos;
} OneByte;

static Int one_byte_read(void *self, Slice p, Error *err) {
    OneByte *r = self;
    if (r->pos >= r->s.len) {
        *err = io_eof;
        return 0;
    }
    if (p.len == 0)
        return 0;
    ((Byte *)p.p)[0] = r->s.p[r->pos++];
    return 1;
}

/* A Str as a C string for the harness, in a buffer that lasts until the next
 * call. */
static const char *cs(Str s) {
    static char buf[4][256];
    static int k;
    char *b = buf[k++ % 4];
    size_t n = (size_t)s.len < sizeof buf[0] - 1 ? (size_t)s.len : sizeof buf[0] - 1;
    if (n > 0)
        memcpy(b, s.p, n);
    b[n] = '\0';
    return b;
}

static const IoReaderVT one_byte_vt = {NULL, one_byte_read};

/* ---------------------------------------------------------- the Go tables */

typedef enum ScanMode { SM_SCAN, SM_SCANLN, SM_SCANF } ScanMode;

typedef enum ScanKind {
    SC_BOOL,
    SC_INT,
    SC_INT8,
    SC_INT16,
    SC_INT32,
    SC_INT64,
    SC_UINT,
    SC_UINT8,
    SC_UINT16,
    SC_UINT32,
    SC_UINT64,
    SC_UINTPTR,
    SC_FLOAT32,
    SC_FLOAT64,
    SC_COMPLEX64,
    SC_COMPLEX128,
    SC_STRING,
    SC_BYTES,
    SC_XS,
    SC_INTSTRING
} ScanKind;

typedef struct ScanCase {
    ScanMode mode;
    Str format;
    Str text;
    int first;
    int n;
    Int count;
    Str err;
} ScanCase;

typedef struct ScanTarget {
    ScanKind kind;
    /* Go's renamedInt and the rest, a type of the kind that is not the builtin
     * itself. */
    bool renamed;
    Str want;
} ScanTarget;

#include "scan_gen.h"

typedef union ScanValue {
    bool b;
    Int i;
    int8_t i8;
    int16_t i16;
    int32_t i32;
    int64_t i64;
    Uint u;
    uint8_t u8;
    uint16_t u16;
    uint32_t u32;
    uint64_t u64;
    Uintptr up;
    float f32;
    double f64;
    Complex64 c64;
    Complex128 c128;
    Str s;
    Slice sl;
    Xs xs;
    IntString is;
} ScanValue;

static const Type *kind_type(ScanKind k) {
    switch (k) {
    case SC_BOOL:
        return TYPE_BOOL;
    case SC_INT:
        return TYPE_INT;
    case SC_INT8:
        return TYPE_INT8;
    case SC_INT16:
        return TYPE_INT16;
    case SC_INT32:
        return TYPE_INT32;
    case SC_INT64:
        return TYPE_INT64;
    case SC_UINT:
        return TYPE_UINT;
    case SC_UINT8:
        return TYPE_UINT8;
    case SC_UINT16:
        return TYPE_UINT16;
    case SC_UINT32:
        return TYPE_UINT32;
    case SC_UINT64:
        return TYPE_UINT64;
    case SC_UINTPTR:
        return TYPE_UINTPTR;
    case SC_FLOAT32:
        return TYPE_FLOAT32;
    case SC_FLOAT64:
        return TYPE_FLOAT64;
    case SC_COMPLEX64:
        return TYPE_COMPLEX64;
    case SC_COMPLEX128:
        return TYPE_COMPLEX128;
    case SC_STRING:
        return TYPE_STRING;
    case SC_BYTES:
        return &burrow_type_ByteSlice;
    case SC_XS:
        return TYPE_OF(Xs);
    case SC_INTSTRING:
        return TYPE_OF(IntString);
    default:
        return NULL;
    }
}

/* What an operand holds, printed the way the generator printed Go's. */
static Str show(const ScanTarget *t, ScanValue *v) {
    switch (t->kind) {
    case SC_XS:
        return fmt_sprintf_v(a, "%q", v->xs.s);
    case SC_INTSTRING:
        return fmt_sprintf_v(a, "{%d %q}", v->is.i, v->is.s);
    case SC_BOOL:
    case SC_INT:
    case SC_INT8:
    case SC_INT16:
    case SC_INT32:
    case SC_INT64:
    case SC_UINT:
    case SC_UINT8:
    case SC_UINT16:
    case SC_UINT32:
    case SC_UINT64:
    case SC_UINTPTR:
    case SC_FLOAT32:
    case SC_FLOAT64:
    case SC_COMPLEX64:
    case SC_COMPLEX128:
    case SC_STRING:
    case SC_BYTES:
    default: {
        Any one[] = {BURROW_ANY(kind_type(t->kind), v)};
        return fmt_sprintf(a, BURROW_S("%#v"), slice_from(one, 1, 1, TYPE_ANY));
    }
    }
}

static void print_quoted(const char *label, Str s) {
    fprintf(stderr, " %s \"", label);
    for (Int i = 0; i < s.len; i++) {
        Byte c = s.p[i];
        if (c >= 0x20 && c < 0x7f && c != '"' && c != '\\')
            fputc(c, stderr);
        else
            fprintf(stderr, "\\x%02x", c);
    }
    fputc('"', stderr);
}

static const char *mode_name(ScanMode m) {
    switch (m) {
    case SM_SCAN:
        return "scan";
    case SM_SCANLN:
        return "scanln";
    case SM_SCANF:
        return "scanf";
    default:
        return "?";
    }
}

enum { MAXOPS = 4 };

static void run_case(size_t k, const ScanCase *c, bool reader) {
    ScanValue vals[MAXOPS];
    Type renamed[MAXOPS];
    Any args[MAXOPS];
    memset(vals, 0, sizeof vals);
    CHECK(c->n <= MAXOPS);
    for (int i = 0; i < c->n && i < MAXOPS; i++) {
        const ScanTarget *t = &scan_targets[c->first + i];
        const Type *ty = kind_type(t->kind);
        if (t->renamed) {
            renamed[i] = *ty;
            ty = &renamed[i];
        }
        args[i] = BURROW_ANY(ty, &vals[i]);
    }
    Slice sl = slice_from(args, c->n, c->n, TYPE_ANY);
    Error err = BURROW_NO_ERROR;
    Int n;
    OneByte ob = {c->text, 0};
    IoReader r = {&one_byte_vt, &ob};
    switch (c->mode) {
    case SM_SCAN:
        n = reader ? fmt_fscan(a, r, sl, &err) : fmt_sscan(a, c->text, sl, &err);
        break;
    case SM_SCANLN:
        n = reader ? fmt_fscanln(a, r, sl, &err) : fmt_sscanln(a, c->text, sl, &err);
        break;
    case SM_SCANF:
    default:
        n = reader ? fmt_fscanf(a, r, c->format, sl, &err)
                   : fmt_sscanf(a, c->text, c->format, sl, &err);
        break;
    }
    Str etext = BURROW_FAILED(err) ? error_text(err) : BURROW_S("");
    bool ok = n == c->count && str_eq(etext, c->err);
    Str got[MAXOPS];
    for (int i = 0; i < c->n && i < MAXOPS; i++) {
        got[i] = show(&scan_targets[c->first + i], &vals[i]);
        ok = ok && str_eq(got[i], scan_targets[c->first + i].want);
    }
    harness_checks++;
    if (ok)
        return;
    harness_failures++;
    fprintf(stderr, "case %zu %s%s:", k, mode_name(c->mode), reader ? " (reader)" : "");
    print_quoted("format", c->format);
    print_quoted("text", c->text);
    fprintf(stderr, "\n    n %lld want %lld", (long long)n, (long long)c->count);
    print_quoted("err", etext);
    print_quoted("want", c->err);
    fputc('\n', stderr);
    for (int i = 0; i < c->n && i < MAXOPS; i++) {
        fprintf(stderr, "   ");
        print_quoted("got", got[i]);
        print_quoted("want", scan_targets[c->first + i].want);
        fputc('\n', stderr);
    }
}

TEST(go_scan_tests) {
    for (size_t k = 0; k < sizeof scan_cases / sizeof scan_cases[0]; k++) {
        run_case(k, &scan_cases[k], false);
        run_case(k, &scan_cases[k], true);
    }
}

/* ------------------------------------------------------- by hand */

TEST(nan_and_inf) {
    static const char *const nans[] = {"nan", "NAN", "NaN"};
    for (size_t i = 0; i < sizeof nans / sizeof nans[0]; i++) {
        float f32 = 0;
        double f64 = 0;
        Error err = BURROW_NO_ERROR;
        Str in = str_from_cstr(nans[i]);
        CHECK_INT_EQ(fmt_sscan_v(a, &err, in, &f32), 1);
        CHECK(BURROW_OK(err));
        CHECK(isnan(f32));
        CHECK_INT_EQ(fmt_sscan_v(a, &err, in, &f64), 1);
        CHECK(isnan(f64));
    }
    static const char *const infs[] = {"inf",  "+inf", "-inf", "INF", "-INF",
                                       "+INF", "Inf",  "-Inf", "+Inf"};
    for (size_t i = 0; i < sizeof infs / sizeof infs[0]; i++) {
        float f32 = 0;
        double f64 = 0;
        Error err = BURROW_NO_ERROR;
        Str in = str_from_cstr(infs[i]);
        CHECK_INT_EQ(fmt_sscan_v(a, &err, in, &f32), 1);
        CHECK(BURROW_OK(err));
        CHECK(isinf(f32));
        CHECK_INT_EQ(fmt_sscan_v(a, &err, in, &f64), 1);
        CHECK(isinf(f64));
        CHECK((infs[i][0] == '-') == (f64 < 0));
    }
}

TEST(variadic_macros) {
    Int i = 0;
    Str s = {0};
    double f = 0;
    Error err = BURROW_NO_ERROR;
    CHECK_INT_EQ(fmt_sscan_v(a, &err, BURROW_S("12 abc 2.5"), &i, &s, &f), 3);
    CHECK(BURROW_OK(err));
    CHECK_INT_EQ(i, 12);
    CHECK_STR_EQ(cs(s), "abc");
    CHECK(f == 2.5);

    int small = 0;
    unsigned long ul = 0;
    CHECK_INT_EQ(fmt_sscanf_v(a, &err, BURROW_S("7:8"), "%d:%d", &small, &ul), 2);
    CHECK_INT_EQ(small, 7);
    CHECK_INT_EQ((Int)ul, 8);

    Slice b = slice_nil(TYPE_BYTE);
    CHECK_INT_EQ(fmt_sscanln_v(a, &err, BURROW_S("hello\n"), &b), 1);
    CHECK_STR_EQ(cs(((Str){b.p, b.len})), "hello");

    Xs x = {0};
    CHECK_INT_EQ(
        fmt_sscanf_v(a, &err, BURROW_S("  vvv "), "%v", BURROW_ANY(TYPE_OF(Xs), &x)),
        1);
    CHECK_STR_EQ(cs(x.s), "vvv");

    OneByte ob = {BURROW_S("41 42\n"), 0};
    IoReader r = {&one_byte_vt, &ob};
    int8_t p = 0;
    uint16_t q = 0;
    CHECK_INT_EQ(fmt_fscanln_v(a, &err, r, &p, &q), 2);
    CHECK_INT_EQ(p, 41);
    CHECK_INT_EQ(q, 42);
}

TEST(errors) {
    Int i = 0;
    Error err = BURROW_NO_ERROR;
    Any nil[] = {{NULL, NULL}};
    CHECK_INT_EQ(fmt_sscan(a, BURROW_S("1"), slice_from(nil, 1, 1, TYPE_ANY), &err), 0);
    CHECK_STR_EQ(cs(error_text(err)), "can't scan type: <nil>");

    Any bad[] = {BURROW_ANY(TYPE_STRING, &i)};
    bad[0].t = TYPE_UNSAFE_POINTER;
    err = BURROW_NO_ERROR;
    CHECK_INT_EQ(fmt_sscan(a, BURROW_S("1"), slice_from(bad, 1, 1, TYPE_ANY), &err), 0);
    CHECK_STR_EQ(cs(error_text(err)), "can't scan type: *unsafe.Pointer");

    err = BURROW_NO_ERROR;
    CHECK_INT_EQ(fmt_sscan_v(a, &err, BURROW_S("x"), &i), 0);
    CHECK_STR_EQ(cs(error_text(err)), "expected integer");
}

/* An input longer than any buffer the scanner starts with. */
TEST(long_tokens) {
    enum { N = 10000 };
    static char text[N + 1];
    memset(text, 'z', N);
    Str s = {0};
    Error err = BURROW_NO_ERROR;
    CHECK_INT_EQ(fmt_sscan_v(a, &err, ((Str){(const Byte *)text, N}), &s), 1);
    CHECK_INT_EQ(s.len, N);
    CHECK(s.len == N && s.p[N - 1] == 'z');
}

int main(void) {
    setup();
    RUN(go_scan_tests);
    RUN(nan_and_inf);
    RUN(variadic_macros);
    RUN(errors);
    RUN(long_tokens);
    teardown();
    return harness_report("scan");
}

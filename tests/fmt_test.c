/* fmt: the printing half, checked against what Go prints for the same thing.
 *
 * Every expected string here is Go's output for the same format and the same
 * value, and most are lifted from fmt_test.go. The generated table in
 * fmt_gen.h covers the long tail of flags and verbs; this file covers what the
 * table cannot express, which is structs, methods, maps, errors and the
 * variadic macros.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/core.h"
#include "burrow/declare.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/iface.h"
#include "burrow/io.h"
#include "burrow/map.h"
#include "burrow/mem/arena.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include "check.h"

#include <math.h>
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

/* A Str as a C string for CHECK_STR_EQ, in a buffer that lasts until the next
 * call. Output with a NUL in it is checked some other way. */
static const char *cs(Str s) {
    static char buf[4096];
    size_t n = (size_t)s.len < sizeof buf - 1 ? (size_t)s.len : sizeof buf - 1;
    if (n > 0)
        memcpy(buf, s.p, n);
    buf[n] = '\0';
    return buf;
}

#define SP(...) cs(fmt_sprintf_v(a, __VA_ARGS__))

/* ------------------------------------------------------------------ types */

#define POINT_FIELDS(F, T)                                                             \
    F(T, Int, X, "")                                                                   \
    F(T, Int, Y, "")
BURROW_STRUCT(Point, POINT_FIELDS);

#define MIXED_FIELDS(F, T)                                                             \
    F(T, Int, a, "")                                                                   \
    F(T, Str, B, "json:\"b\"")                                                         \
    F(T, double, C, "")
BURROW_STRUCT(Mixed, MIXED_FIELDS);

BURROW_PTR_TYPE(PointPtr, Point);
BURROW_SLICE_TYPE(ByteSlice, uint8_t);

/* Go's []byte, the type a Slice of bytes has when it is passed through Any. */
#define bytes_slice_type burrow_type_ByteSlice
BURROW_PTR_TYPE(IntPtr, Int);

/* A Stringer. */
#define NAMED_FIELDS(F, T) F(T, Int, N, "")
BURROW_STRUCT_DECL(Named, NAMED_FIELDS);

static Str named_string(Named *n) {
    return n->N == 1 ? BURROW_S("one") : BURROW_S("many");
}

#define NAMED_SIG_String(IN, OUT) OUT(Str)
#define NAMED_METHODS(M, T) M(T, String, named_string, NAMED_SIG_String)
BURROW_STRUCT_DEFINE_METHODS(Named, NAMED_FIELDS, NAMED_METHODS);

/* A struct with a Stringer inside, which Go calls at depth one. */
#define OUTER_FIELDS(F, T)                                                             \
    F(T, Named, In, "")                                                                \
    F(T, Named, in, "")
BURROW_STRUCT(Outer, OUTER_FIELDS);

/* A GoStringer with a String too. */
#define GOS_FIELDS(F, T) F(T, Int, V, "")
BURROW_STRUCT_DECL(Gos, GOS_FIELDS);

static Str gos_go_string(Gos *g) {
    (void)g;
    return BURROW_S("GoString(Gos)");
}

static Str gos_string(Gos *g) {
    (void)g;
    return BURROW_S("String(Gos)");
}

#define GOS_SIG_Str(IN, OUT) OUT(Str)
#define GOS_METHODS(M, T)                                                              \
    M(T, GoString, gos_go_string, GOS_SIG_Str)                                         \
    M(T, String, gos_string, GOS_SIG_Str)
BURROW_STRUCT_DEFINE_METHODS(Gos, GOS_FIELDS, GOS_METHODS);

/* A Formatter, Go's F type from fmt_test.go, which prints <verb=x>. */
#define FMTR_FIELDS(F, T) F(T, Int, V, "")
BURROW_STRUCT_DECL(Fmtr, FMTR_FIELDS);

static void fmtr_format(Fmtr *f, FmtState s, Rune verb) {
    (void)f;
    Str out = fmt_sprintf_v(a, "<%c=F(%d)>", verb, f->V);
    s.vt->write(s.data,
                slice_from((void *)(uintptr_t)out.p, out.len, out.len, TYPE_BYTE),
                NULL);
}

#define FMTR_SIG_Format(IN, OUT)                                                       \
    IN(0, FmtState)                                                                    \
    IN(1, Rune)
#define FMTR_METHODS(M, T) M(T, Format, fmtr_format, FMTR_SIG_Format)
BURROW_STRUCT_DEFINE_METHODS(Fmtr, FMTR_FIELDS, FMTR_METHODS);

/* A Stringer that panics, Go's Panic type. */
#define PANICKY_FIELDS(F, T) F(T, Int, V, "")
BURROW_STRUCT_DECL(Panicky, PANICKY_FIELDS);

static Str panicky_string(Panicky *p) {
    (void)p;
    panic_str(BURROW_S("oops"));
}

#define PANICKY_SIG_String(IN, OUT) OUT(Str)
#define PANICKY_METHODS(M, T) M(T, String, panicky_string, PANICKY_SIG_String)
BURROW_STRUCT_DEFINE_METHODS(Panicky, PANICKY_FIELDS, PANICKY_METHODS);

/* ------------------------------------------------------------------ basics */

static void TestIntegers(TestingT *t) {
    CHECK_STR_EQ(SP("%d", 12345), "12345");
    CHECK_STR_EQ(SP("%v", -12345), "-12345");
    CHECK_STR_EQ(SP("%5d", 12), "   12");
    CHECK_STR_EQ(SP("%-5d|", 12), "12   |");
    CHECK_STR_EQ(SP("%05d", -12), "-0012");
    CHECK_STR_EQ(SP("%+d", 12), "+12");
    CHECK_STR_EQ(SP("% d", 12), " 12");
    CHECK_STR_EQ(SP("%x", 255), "ff");
    CHECK_STR_EQ(SP("%X", 255), "FF");
    CHECK_STR_EQ(SP("%#x", 255), "0xff");
    CHECK_STR_EQ(SP("%o", 8), "10");
    CHECK_STR_EQ(SP("%O", 8), "0o10");
    CHECK_STR_EQ(SP("%b", 5), "101");
    CHECK_STR_EQ(SP("%.3d", 7), "007");
    CHECK_STR_EQ(SP("%c", 'x'), "x");
    CHECK_STR_EQ(SP("%q", 'x'), "'x'");
    CHECK_STR_EQ(SP("%U", 0x1F600), "U+1F600");
    CHECK_STR_EQ(SP("%#U", 0x263A), "U+263A '\xe2\x98\xba'");
    CHECK_STR_EQ(SP("%d", (int8_t)-128), "-128");
    CHECK_STR_EQ(SP("%d", (uint64_t)18446744073709551615ULL), "18446744073709551615");
    CHECK_STR_EQ(SP("%x", (int64_t)-1), "-1");
    CHECK_STR_EQ(SP("%#v", (Uint)255), "0xff");
    CHECK_STR_EQ(SP("%#v", 255), "255");
}

static void TestFloats(TestingT *t) {
    CHECK_STR_EQ(SP("%v", 1.0), "1");
    CHECK_STR_EQ(SP("%v", 0.1f), "0.1");
    CHECK_STR_EQ(SP("%v", 1e21), "1e+21");
    CHECK_STR_EQ(SP("%f", 3.14159), "3.141590");
    CHECK_STR_EQ(SP("%.2f", 3.14159), "3.14");
    CHECK_STR_EQ(SP("%8.3f", -3.14159), "  -3.142");
    CHECK_STR_EQ(SP("%e", 1234.5678), "1.234568e+03");
    CHECK_STR_EQ(SP("%g", 1234.5678), "1234.5678");
    CHECK_STR_EQ(SP("%#g", 1.0), "1.00000");
    CHECK_STR_EQ(SP("%x", 1.0), "0x1p+00");
    CHECK_STR_EQ(SP("%+.3e", 0.0), "+0.000e+00");
    CHECK_STR_EQ(SP("%v", INFINITY), "+Inf");
    CHECK_STR_EQ(SP("%v", -INFINITY), "-Inf");
    CHECK_STR_EQ(SP("%v", (double)NAN), "NaN");
    CHECK_STR_EQ(SP("%05v", (double)NAN), "  NaN");
    Complex128 c1 = {1, 2}, c2 = {1, -2};
    Complex64 c3 = {0.1f, 0};
    CHECK_STR_EQ(SP("%v", c1), "(1+2i)");
    CHECK_STR_EQ(SP("%.2f", c2), "(1.00-2.00i)");
    CHECK_STR_EQ(SP("%v", c3), "(0.1+0i)");
}

static void TestStringsAndBools(TestingT *t) {
    CHECK_STR_EQ(SP("%s", "abc"), "abc");
    CHECK_STR_EQ(SP("%v", BURROW_S("abc")), "abc");
    CHECK_STR_EQ(SP("%q", "abc"), "\"abc\"");
    CHECK_STR_EQ(SP("%#q", "abc"), "`abc`");
    CHECK_STR_EQ(SP("%x", "xyz"), "78797a");
    CHECK_STR_EQ(SP("% X", "xyz"), "78 79 7A");
    CHECK_STR_EQ(SP("%.2s", "abc"), "ab");
    CHECK_STR_EQ(SP("%5s|", "ab"), "   ab|");
    CHECK_STR_EQ(SP("%-5s|", "ab"), "ab   |");
    CHECK_STR_EQ(SP("%#v", "a\n"), "\"a\\n\"");
    CHECK_STR_EQ(SP("%t", (bool)true), "true");
    CHECK_STR_EQ(SP("%v", (bool)false), "false");
    CHECK_STR_EQ(SP("%%"), "%");
    CHECK_STR_EQ(SP("plain"), "plain");
}

static void TestBadFormats(TestingT *t) {
    CHECK_STR_EQ(SP("%d"), "%!d(MISSING)");
    CHECK_STR_EQ(SP("%d", "hi"), "%!d(string=hi)");
    CHECK_STR_EQ(SP("%z", 3), "%!z(int=3)");
    CHECK_STR_EQ(SP("%s", 3), "%!s(int=3)");
    CHECK_STR_EQ(SP("%d %d", 1), "1 %!d(MISSING)");
    CHECK_STR_EQ(SP("%d", 1, 2), "1%!(EXTRA int=2)");
    CHECK_STR_EQ(SP("%d", 1, "x", 2.5), "1%!(EXTRA string=x, float64=2.5)");
    CHECK_STR_EQ(SP("%"), "%!(NOVERB)");
    CHECK_STR_EQ(SP("%[3]d", 1, 2), "%!d(BADINDEX)");
    CHECK_STR_EQ(SP("%[2]d %[1]d", 1, 2), "2 1");
    CHECK_STR_EQ(SP("%[1]d %[1]x", 10), "10 a");
    CHECK_STR_EQ(SP("%*d", 5, 1), "    1");
    CHECK_STR_EQ(SP("%-*d|", 3, 1), "1  |");
    CHECK_STR_EQ(SP("%*d", -3, 1), "1  ");
    CHECK_STR_EQ(SP("%.*f", 2, 3.14159), "3.14");
    CHECK_STR_EQ(SP("%*d", "x", 1), "%!(BADWIDTH)1");
    CHECK_STR_EQ(SP("%.*d", "x", 1), "%!(BADPREC)1");
    CHECK_STR_EQ(SP("%!", 1), "%!!(int=1)");
    CHECK_STR_EQ(SP("%v", ((Any){NULL, NULL})), "<nil>");
    CHECK_STR_EQ(SP("%d", ((Any){NULL, NULL})), "%!d(<nil>)");
}

/* ------------------------------------------------------------ composites */

static void TestSlicesAndBytes(TestingT *t) {
    Int xs[] = {1, 2, 3};
    Slice s = slice_from(xs, 3, 3, TYPE_INT);
    CHECK_STR_EQ(SP("%v", s), "[1 2 3]");
    CHECK_STR_EQ(SP("%d", s), "[1 2 3]");
    CHECK_STR_EQ(SP("%x", s), "[1 2 3]");
    CHECK_STR_EQ(SP("%#v", s), "[]int{1, 2, 3}");
    CHECK_STR_EQ(SP("%#v", slice_nil(TYPE_INT)), "[]int(nil)");
    CHECK_STR_EQ(SP("%v", slice_nil(TYPE_INT)), "[]");

    Byte bs[] = {'h', 'i'};
    Slice b = slice_from(bs, 2, 2, TYPE_BYTE);
    CHECK_STR_EQ(SP("%v", b), "[104 105]");
    CHECK_STR_EQ(SP("%s", b), "hi");
    CHECK_STR_EQ(SP("%q", b), "\"hi\"");
    CHECK_STR_EQ(SP("%x", b), "6869");
    CHECK_STR_EQ(SP("%#v", b), "[]byte{0x68, 0x69}");
    CHECK_STR_EQ(SP("%T", b), "[]uint8");

    Str strs[] = {BURROW_S("a"), BURROW_S("b")};
    CHECK_STR_EQ(SP("%q", slice_from(strs, 2, 2, TYPE_STRING)), "[\"a\" \"b\"]");
    CHECK_STR_EQ(SP("%T", slice_from(strs, 2, 2, TYPE_STRING)), "[]string");
}

static void TestStructs(TestingT *t) {
    Point p = {1, 2};
    Any v = BURROW_ANY(TYPE_OF(Point), &p);
    CHECK_STR_EQ(SP("%v", v), "{1 2}");
    CHECK_STR_EQ(SP("%+v", v), "{X:1 Y:2}");
    CHECK_STR_EQ(SP("%#v", v), "Point{X:1, Y:2}");
    CHECK_STR_EQ(SP("%T", v), "Point");
    CHECK_STR_EQ(SP("%d", v), "{1 2}");
    CHECK_STR_EQ(SP("%x", v), "{1 2}");

    Mixed m = {7, BURROW_S("b"), 2.5};
    Any mv = BURROW_ANY(TYPE_OF(Mixed), &m);
    CHECK_STR_EQ(SP("%v", mv), "{7 b 2.5}");
    CHECK_STR_EQ(SP("%+v", mv), "{a:7 B:b C:2.5}");
    CHECK_STR_EQ(SP("%#v", mv), "Mixed{a:7, B:\"b\", C:2.5}");

    /* A pointer to a struct at the top level prints as & and the struct. */
    Point *pp = &p;
    CHECK_STR_EQ(SP("%v", BURROW_ANY(TYPE_OF(PointPtr), &pp)), "&{1 2}");
    CHECK_STR_EQ(SP("%+v", BURROW_ANY(TYPE_OF(PointPtr), &pp)), "&{X:1 Y:2}");
    CHECK_STR_EQ(SP("%T", BURROW_ANY(TYPE_OF(PointPtr), &pp)), "*Point");
}

static void TestMaps(TestingT *t) {
    Map *m = map_make(a, TYPE_STRING, TYPE_INT, 0);
    Str k1 = BURROW_S("b"), k2 = BURROW_S("a"), k3 = BURROW_S("c");
    Int v1 = 2, v2 = 1, v3 = 3;
    map_set(m, &k1, &v1);
    map_set(m, &k2, &v2);
    map_set(m, &k3, &v3);
    CHECK_STR_EQ(SP("%v", m), "map[a:1 b:2 c:3]");
    CHECK_STR_EQ(SP("%#v", m), "map[string]int{\"a\":1, \"b\":2, \"c\":3}");
    CHECK_STR_EQ(SP("%T", m), "map[string]int");

    Map *n = map_make(a, TYPE_INT, TYPE_STRING, 0);
    for (Int i = 5; i > 0; i--) {
        Str s = i % 2 ? BURROW_S("odd") : BURROW_S("even");
        map_set(n, &i, &s);
    }
    CHECK_STR_EQ(SP("%v", n), "map[1:odd 2:even 3:odd 4:even 5:odd]");
}

/* ---------------------------------------------------------------- methods */

static void TestStringers(TestingT *t) {
    Named one = {1}, two = {2};
    CHECK_STR_EQ(SP("%v", BURROW_ANY(TYPE_OF(Named), &one)), "one");
    CHECK_STR_EQ(SP("%s", BURROW_ANY(TYPE_OF(Named), &two)), "many");
    CHECK_STR_EQ(SP("%q", BURROW_ANY(TYPE_OF(Named), &one)), "\"one\"");
    CHECK_STR_EQ(SP("%x", BURROW_ANY(TYPE_OF(Named), &one)), "6f6e65");
    CHECK_STR_EQ(SP("%d", BURROW_ANY(TYPE_OF(Named), &one)), "{1}");
    CHECK_STR_EQ(SP("%+v", BURROW_ANY(TYPE_OF(Named), &one)), "one");
    CHECK_STR_EQ(SP("%#v", BURROW_ANY(TYPE_OF(Named), &one)), "Named{N:1}");
    CHECK_STR_EQ(SP("%6v|", BURROW_ANY(TYPE_OF(Named), &one)), "   one|");

    /* An exported field is printed with its method and an unexported one is
     * not, which is Go's CanInterface rule. */
    Outer o = {{1}, {1}};
    CHECK_STR_EQ(SP("%v", BURROW_ANY(TYPE_OF(Outer), &o)), "{one {1}}");

    Gos g = {3};
    CHECK_STR_EQ(SP("%v", BURROW_ANY(TYPE_OF(Gos), &g)), "String(Gos)");
    CHECK_STR_EQ(SP("%#v", BURROW_ANY(TYPE_OF(Gos), &g)), "GoString(Gos)");
}

static void TestFormatters(TestingT *t) {
    Fmtr f = {5};
    Any v = BURROW_ANY(TYPE_OF(Fmtr), &f);
    CHECK_STR_EQ(SP("%v", v), "<v=F(5)>");
    CHECK_STR_EQ(SP("%d", v), "<d=F(5)>");
    CHECK_STR_EQ(SP("%#v", v), "<v=F(5)>");
    CHECK_STR_EQ(SP("%T", v), "Fmtr");
}

/* A Formatter that reports its flags through fmt_format_string. */
#define FLAGS_FIELDS(F, T) F(T, Int, V, "")
BURROW_STRUCT_DECL(Flags, FLAGS_FIELDS);

static void flags_format(Flags *f, FmtState s, Rune verb) {
    (void)f;
    Str out = fmt_format_string(a, s, verb);
    s.vt->write(s.data,
                slice_from((void *)(uintptr_t)out.p, out.len, out.len, TYPE_BYTE),
                NULL);
}

#define FLAGS_METHODS(M, T) M(T, Format, flags_format, FMTR_SIG_Format)
BURROW_STRUCT_DEFINE_METHODS(Flags, FLAGS_FIELDS, FLAGS_METHODS);

static void TestFormatStringRebuildsTheDirective(TestingT *t) {
    Flags f = {0};
    Any v = BURROW_ANY(TYPE_OF(Flags), &f);
    CHECK_STR_EQ(SP("%v", v), "%v");
    CHECK_STR_EQ(SP("%-+# 012.34x", v), "% +-#012.34x");
    CHECK_STR_EQ(SP("%#v", v), "%#v");
    CHECK_STR_EQ(SP("%+v", v), "%+v");
    CHECK_STR_EQ(SP("%3.x", v), "%3.0x");
}

static void TestPanicsInMethods(TestingT *t) {
    Panicky p = {0};
    CHECK_STR_EQ(SP("%s", BURROW_ANY(TYPE_OF(Panicky), &p)),
                 "%!s(PANIC=String method: oops)");
    CHECK_STR_EQ(SP("%v", BURROW_ANY(TYPE_OF(Panicky), &p)),
                 "%!v(PANIC=String method: oops)");
}

/* ----------------------------------------------------------------- errors */

BURROW_SENTINEL_ERROR(test_err_eof, "EOF");

static void TestErrorsPrintTheirText(TestingT *t) {
    Error e = errors_new(a, BURROW_S("boom"));
    CHECK_STR_EQ(SP("%v", e), "boom");
    CHECK_STR_EQ(SP("%s", e), "boom");
    CHECK_STR_EQ(SP("%q", e), "\"boom\"");
    CHECK_STR_EQ(SP("%T", e), "*errors.errorString");
    CHECK_STR_EQ(SP("%#v", e), "&errors.errorString{s:\"boom\"}");
    CHECK_STR_EQ(SP("%d", e), "&{%!d(string=boom)}");
    CHECK_STR_EQ(SP("%v", BURROW_NO_ERROR), "<nil>");
}

static void TestErrorfWraps(TestingT *t) {
    Error inner = errors_new(a, BURROW_S("inner"));
    Error e = fmt_errorf_v("outer: %w", inner);
    CHECK_STR_EQ(cs(error_text(e)), "outer: inner");
    CHECK(errors_is(e, inner));
    CHECK(errors_unwrap(e).data == inner.data);

    Error e2 = fmt_errorf_v("a %w b %w", inner, test_err_eof);
    CHECK_STR_EQ(cs(error_text(e2)), "a inner b EOF");
    CHECK(errors_is(e2, inner));
    CHECK(errors_is(e2, test_err_eof));
    CHECK(errors_unwrap(e2).vt == NULL);

    Error e3 = fmt_errorf_v("no wrap %d", 3);
    CHECK_STR_EQ(cs(error_text(e3)), "no wrap 3");
    CHECK(errors_unwrap(e3).vt == NULL);

    Error e4 = fmt_errorf_v("%w", 3);
    CHECK_STR_EQ(cs(error_text(e4)), "%!w(int=3)");

    Error e5 = fmt_errorf_v("%[1]w %[1]w", inner);
    CHECK_STR_EQ(cs(error_text(e5)), "inner inner");
    CHECK(errors_is(e5, inner));

    /* %w outside Errorf is a bad verb. */
    CHECK_STR_EQ(SP("%w", inner), "%!w(*errors.errorString=&{inner})");
}

/* ---------------------------------------------------------------- pointers */

static void TestPointers(TestingT *t) {
    Int x = 1;
    Int *px = &x;
    Str got = fmt_sprintf_v(a, "%v", BURROW_ANY(TYPE_OF(IntPtr), &px));
    CHECK(got.len > 2 && got.p[0] == '0' && got.p[1] == 'x');
    CHECK_STR_EQ(SP("%T", BURROW_ANY(TYPE_OF(IntPtr), &px)), "*int");
    Int *nilp = NULL;
    CHECK_STR_EQ(SP("%v", BURROW_ANY(TYPE_OF(IntPtr), &nilp)), "<nil>");
    CHECK_STR_EQ(SP("%#v", BURROW_ANY(TYPE_OF(IntPtr), &nilp)), "(*int)(nil)");
    CHECK_STR_EQ(SP("%p", BURROW_ANY(TYPE_OF(IntPtr), &nilp)), "0x0");
    CHECK_STR_EQ(SP("%d", "x", BURROW_ANY(TYPE_OF(IntPtr), &nilp)),
                 "%!d(string=x)%!(EXTRA *int=<nil>)");
    void *vp = NULL;
    CHECK_STR_EQ(SP("%v", vp), "<nil>");
    CHECK_STR_EQ(SP("%T", vp), "unsafe.Pointer");
}

/* ---------------------------------------------------------- print, println */

static void TestSprintAndSprintln(TestingT *t) {
    CHECK_STR_EQ(cs(fmt_sprint_v(a, 1, 2)), "1 2");
    CHECK_STR_EQ(cs(fmt_sprint_v(a, "a", 1, 2, "b")), "a1 2b");
    CHECK_STR_EQ(cs(fmt_sprint_v(a, "a", "b")), "ab");
    CHECK_STR_EQ(cs(fmt_sprintln_v(a, "a", 1, "b")), "a 1 b\n");
    CHECK_STR_EQ(cs(fmt_sprintln(a, slice_nil(TYPE_ANY))), "\n");
    CHECK_STR_EQ(cs(fmt_sprint(a, slice_nil(TYPE_ANY))), "");
}

/* A writer that keeps what it is given. */
typedef struct Sink {
    Byte b[64];
    Int n;
} Sink;

static Int sink_write(void *self, Slice p, Error *err) {
    Sink *s = (Sink *)self;
    memcpy(s->b + s->n, p.p, (size_t)p.len);
    s->n += p.len;
    if (err != NULL)
        *err = BURROW_NO_ERROR;
    return p.len;
}

static const IoWriterVT sink_vt = {NULL, sink_write};

static void TestAppendAndFprint(TestingT *t) {
    Slice b = fmt_appendf_v(a, slice_nil(TYPE_BYTE), "x=%d", 7);
    b = fmt_append_v(a, b, " ", (bool)true);
    b = fmt_appendln_v(a, b, " done");
    CHECK_STR_EQ(cs((Str){(const Byte *)b.p, b.len}), "x=7 true done\n");

    Sink sink = {{0}, 0};
    IoWriter w = {&sink_vt, &sink};
    Error err = BURROW_NO_ERROR;
    Int n = fmt_fprintf_v(w, "%s-%d", "a", 1);
    CHECK_INT_EQ(n, 3);
    Any one[] = {BURROW_ANY_OF(2.5)};
    n = fmt_fprintln(w, slice_from(one, 1, 1, TYPE_ANY), &err);
    CHECK_INT_EQ(n, 4);
    CHECK(err.vt == NULL);
    CHECK_STR_EQ(cs((Str){sink.b, sink.n}), "a-12.5\n");
}

static void TestLongOutputGrowsTheBuffer(TestingT *t) {
    Byte big[3000];
    memset(big, 'z', sizeof big);
    Str s = {big, (Int)sizeof big};
    Str got = fmt_sprintf_v(a, "[%s]", s);
    CHECK_INT_EQ(got.len, 3002);
    CHECK(got.p[0] == '[' && got.p[1] == 'z' && got.p[3001] == ']');
    CHECK_INT_EQ(fmt_sprintf_v(a, "%3000d", 1).len, 3000);
}

/* ---------------------------------------------------------- Go's own table */

typedef enum FcKind {
    FC_NONE,
    FC_NIL,
    FC_BOOL,
    FC_INT,
    FC_INT8,
    FC_INT16,
    FC_INT32,
    FC_INT64,
    FC_UINT,
    FC_UINT8,
    FC_UINT16,
    FC_UINT32,
    FC_UINT64,
    FC_UINTPTR,
    FC_FLOAT32,
    FC_FLOAT64,
    FC_COMPLEX64,
    FC_COMPLEX128,
    FC_STRING,
    FC_BYTES,
    FC_BYTES_NIL,
} FcKind;

/* One operand: its type, its bits, and its bytes for a string or a []byte. */
typedef struct FmtOperand {
    FcKind kind;
    uint64_t u;
    uint64_t u2;
    Str s;
} FmtOperand;

typedef struct FmtCase {
    Str format;
    Str out;
    int first;
    int n;
    /* Go's answer assumes a 64 bit int. */
    bool wide;
} FmtCase;

#include "fmt_gen.h"

typedef union FcValue {
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
} FcValue;

static float f32_of(uint64_t bits) {
    uint32_t b = (uint32_t)bits;
    float f;
    memcpy(&f, &b, sizeof f);
    return f;
}

static double f64_of(uint64_t bits) {
    double f;
    memcpy(&f, &bits, sizeof f);
    return f;
}

static Any operand_any(const FmtOperand *op, FcValue *v) {
    switch (op->kind) {
    case FC_BOOL:
        v->b = op->u != 0;
        return BURROW_ANY(TYPE_BOOL, &v->b);
    case FC_INT:
        v->i = (Int)(int64_t)op->u;
        return BURROW_ANY(TYPE_INT, &v->i);
    case FC_INT8:
        v->i8 = (int8_t)op->u;
        return BURROW_ANY(TYPE_OF(int8_t), &v->i8);
    case FC_INT16:
        v->i16 = (int16_t)op->u;
        return BURROW_ANY(TYPE_OF(int16_t), &v->i16);
    case FC_INT32:
        v->i32 = (int32_t)op->u;
        return BURROW_ANY(TYPE_OF(int32_t), &v->i32);
    case FC_INT64:
        v->i64 = (int64_t)op->u;
        return BURROW_ANY(TYPE_OF(int64_t), &v->i64);
    case FC_UINT:
        v->u = (Uint)op->u;
        return BURROW_ANY(TYPE_UINT, &v->u);
    case FC_UINT8:
        v->u8 = (uint8_t)op->u;
        return BURROW_ANY(TYPE_OF(uint8_t), &v->u8);
    case FC_UINT16:
        v->u16 = (uint16_t)op->u;
        return BURROW_ANY(TYPE_OF(uint16_t), &v->u16);
    case FC_UINT32:
        v->u32 = (uint32_t)op->u;
        return BURROW_ANY(TYPE_OF(uint32_t), &v->u32);
    case FC_UINT64:
        v->u64 = op->u;
        return BURROW_ANY(TYPE_OF(uint64_t), &v->u64);
    case FC_UINTPTR:
        v->up = (Uintptr)op->u;
        return BURROW_ANY(TYPE_UINTPTR, &v->up);
    case FC_FLOAT32:
        v->f32 = f32_of(op->u);
        return BURROW_ANY(TYPE_FLOAT32, &v->f32);
    case FC_FLOAT64:
        v->f64 = f64_of(op->u);
        return BURROW_ANY(TYPE_FLOAT64, &v->f64);
    case FC_COMPLEX64:
        v->c64 = (Complex64){f32_of(op->u), f32_of(op->u2)};
        return BURROW_ANY(TYPE_COMPLEX64, &v->c64);
    case FC_COMPLEX128:
        v->c128 = (Complex128){f64_of(op->u), f64_of(op->u2)};
        return BURROW_ANY(TYPE_COMPLEX128, &v->c128);
    case FC_STRING:
        v->s = op->s;
        return BURROW_ANY(TYPE_STRING, &v->s);
    case FC_BYTES:
        v->sl = slice_from((void *)(uintptr_t)op->s.p, op->s.len, op->s.len, TYPE_BYTE);
        return BURROW_ANY(&bytes_slice_type, &v->sl);
    case FC_BYTES_NIL:
        v->sl = slice_nil(TYPE_BYTE);
        return BURROW_ANY(&bytes_slice_type, &v->sl);
    case FC_NONE:
    case FC_NIL:
    default:
        return (Any){NULL, NULL};
    }
}

static void TestGoFmtTests(TestingT *t) {
    enum { MAXOPS = 16 };
    Int skipped = 0;
    for (size_t k = 0; k < sizeof fmt_cases / sizeof fmt_cases[0]; k++) {
        const FmtCase *c = &fmt_cases[k];
        if (c->wide && sizeof(Int) < 8) {
            skipped++;
            continue;
        }
        FcValue vals[MAXOPS];
        Any args[MAXOPS];
        CHECK(c->n <= MAXOPS);
        for (int i = 0; i < c->n && i < MAXOPS; i++)
            args[i] = operand_any(&fmt_operands[c->first + i], &vals[i]);
        Str got = fmt_sprintf(a, c->format, slice_from(args, c->n, c->n, TYPE_ANY));
        if (!str_eq(got, c->out))
            testing_t_errorf_v(t, "go_fmt_tests[%d]: Sprintf(%q) = %q, want %q", (Int)k,
                               c->format, got, c->out);
    }
    (void)skipped;
}

#define TESTS(X)                                                                       \
    X(TestIntegers)                                                                    \
    X(TestFloats)                                                                      \
    X(TestStringsAndBools)                                                             \
    X(TestBadFormats)                                                                  \
    X(TestSlicesAndBytes)                                                              \
    X(TestStructs)                                                                     \
    X(TestMaps)                                                                        \
    X(TestStringers)                                                                   \
    X(TestFormatters)                                                                  \
    X(TestFormatStringRebuildsTheDirective)                                            \
    X(TestPanicsInMethods)                                                             \
    X(TestErrorsPrintTheirText)                                                        \
    X(TestErrorfWraps)                                                                 \
    X(TestPointers)                                                                    \
    X(TestSprintAndSprintln)                                                           \
    X(TestAppendAndFprint)                                                             \
    X(TestLongOutputGrowsTheBuffer)                                                    \
    X(TestGoFmtTests)

static int TestMain(TestingM *m) {
    setup();
    int code = testing_m_run(m);
    teardown();
    return code;
}

TESTING_MAIN_WITH(TestMain, TESTS)

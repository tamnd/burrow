/* Derived from Go's src/html/escape_test.go, entity_test.go and fuzz_test.go.
 * Go source: go1.27.1.
 *
 * TestEntityLength walks the two entity tables as Go's walks the two maps.
 * TestEntitySorted is burrow's own: Go's maps need no order, and the tables
 * here are binary searched, so it checks they are sorted and that every entry of
 * the second one unescapes to its two code points.
 *
 * Copyright 2013 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/html.h"

#include "../src/html/internal.h"
#include "burrow/burrow.h"
#include "burrow/mem/arena.h"
#include "burrow/strings.h"
#include "burrow/utf8.h"

#include "check.h"

#include <string.h>

#define S_(lit) BURROW_S_INIT(lit)
#define NELEM(x) ((Int)(sizeof(x) / sizeof((x)[0])))

typedef struct UnescapeTest {
    /* A short description of the test case. */
    Str desc;
    /* The HTML text. */
    Str html;
    /* The unescaped text. */
    Str unescaped;
} UnescapeTest;

static const UnescapeTest unescape_tests[] = {
    /* Handle no entities. */
    {S_("copy"), S_("A\ttext\nstring"), S_("A\ttext\nstring")},
    /* Handle simple named entities. */
    {S_("simple"), S_("&amp; &gt; &lt;"), S_("& > <")},
    /* Handle hitting the end of the string. */
    {S_("stringEnd"), S_("&amp &amp"), S_("& &")},
    /* Handle entities with two codepoints. */
    {S_("multiCodepoint"), S_("text &gesl; blah"), S_("text ⋛︀ blah")},
    /* Handle decimal numeric entities. */
    {S_("decimalEntity"), S_("Delta = &#916; "), S_("Delta = Δ ")},
    /* Handle hexadecimal numeric entities. */
    {S_("hexadecimalEntity"), S_("Lambda = &#x3bb; = &#X3Bb "), S_("Lambda = λ = λ ")},
    /* Handle numeric early termination. */
    {S_("numericEnds"), S_("&# &#x &#128;43 &copy = &#169f = &#xa9"),
     S_("&# &#x €43 © = ©f = ©")},
    /* Handle numeric ISO-8859-1 entity replacements. */
    {S_("numericReplacements"), S_("Footnote&#x87;"), S_("Footnote‡")},
    /* Handle single ampersand. */
    {S_("copySingleAmpersand"), S_("&"), S_("&")},
    /* Handle ampersand followed by non-entity. */
    {S_("copyAmpersandNonEntity"), S_("text &test"), S_("text &test")},
    /* Handle "&#". */
    {S_("copyAmpersandHash"), S_("text &#"), S_("text &#")},
};

static void TestUnescape(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof unescape_tests / sizeof unescape_tests[0]; i++) {
        const UnescapeTest *tt = &unescape_tests[i];
        Str unescaped = html_unescape_string(a, tt->html);
        if (!str_eq(unescaped, tt->unescaped))
            testing_t_errorf_v(t, "TestUnescape %s: want %q, got %q", tt->desc,
                               tt->unescaped, unescaped);
    }
    arena_free(&ar);
}

static void TestUnescapeEscape(TestingT *t) {
    static const Str ss[] = {
        S_(""),
        S_("abc def"),
        S_("a & b"),
        S_("a&amp;b"),
        S_("a &amp b"),
        S_("&quot;"),
        S_("\""),
        S_("\"<&>\""),
        S_("&quot;&lt;&amp;&gt;&quot;"),
        S_("3&5==1 && 0<1, \"0&lt;1\", a+acute=&aacute;"),
        S_("The special characters are: <, >, &, ' and \""),
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof ss / sizeof ss[0]; i++) {
        Str got = html_unescape_string(a, html_escape_string(a, ss[i]));
        if (!str_eq(got, ss[i]))
            testing_t_errorf_v(t, "got %q want %q", got, ss[i]);
    }
    arena_free(&ar);
}

static void TestEntityLength(TestingT *t) {
    if (burrow__html_entity_len == 0 || burrow__html_entity2_len == 0)
        testing_t_fatal_v(t, "maps not loaded");

    /* We verify that the length of UTF-8 encoding of each value is <= 1 +
     * len(key). The +1 comes from the leading "&". This property implies that
     * the length of unescaped text is <= the length of escaped text. */
    for (Int i = 0; i < burrow__html_entity_len; i++) {
        Str k = burrow__html_entity[i].name;
        Rune v = burrow__html_entity[i].r[0];
        if (1 + k.len < utf8_rune_len(v))
            testing_t_errorf_v(
                t, "escaped entity &%s is shorter than its UTF-8 encoding %c", k, v);
        if (k.len > BURROW__HTML_LONGEST_ENTITY_WITHOUT_SEMICOLON &&
            k.p[k.len - 1] != ';')
            testing_t_errorf_v(
                t,
                "entity name %s is %d characters, but longestEntityWithoutSemicolon=%d",
                k, k.len, (Int)BURROW__HTML_LONGEST_ENTITY_WITHOUT_SEMICOLON);
    }
    for (Int i = 0; i < burrow__html_entity2_len; i++) {
        Str k = burrow__html_entity2[i].name;
        const Rune *v = burrow__html_entity2[i].r;
        if (1 + k.len < utf8_rune_len(v[0]) + utf8_rune_len(v[1]))
            testing_t_errorf_v(
                t, "escaped entity &%s is shorter than its UTF-8 encoding %c%c", k,
                v[0], v[1]);
    }
}

/* The tables have to be sorted for the binary search, and each name may only
 * be in one of them, as a name is one key of one map in Go. */
static void TestEntitySorted(TestingT *t) {
    const burrow__HtmlEntity *tabs[] = {burrow__html_entity, burrow__html_entity2};
    const Int lens[] = {burrow__html_entity_len, burrow__html_entity2_len};
    for (int j = 0; j < 2; j++)
        for (Int i = 1; i < lens[j]; i++)
            if (strings_compare(tabs[j][i - 1].name, tabs[j][i].name) >= 0)
                testing_t_errorf_v(t, "table %d: %q is not before %q", j,
                                   tabs[j][i - 1].name, tabs[j][i].name);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < burrow__html_entity2_len; i++) {
        Str name = burrow__html_entity2[i].name;
        Byte in[64];
        Byte want[2 * UTF8_UTF_MAX];
        in[0] = '&';
        memcpy(in + 1, name.p, (size_t)name.len);
        Int n = utf8_encode_rune((Slice){want, NELEM(want), NELEM(want), TYPE_BYTE},
                                 burrow__html_entity2[i].r[0]);
        n += utf8_encode_rune(
            (Slice){want + n, NELEM(want) - n, NELEM(want) - n, TYPE_BYTE},
            burrow__html_entity2[i].r[1]);
        Str s = str_from_bytes(in, 1 + name.len);
        Str got = html_unescape_string(a, s);
        Str w = str_from_bytes(want, n);
        if (!str_eq(got, w))
            testing_t_errorf_v(t, "UnescapeString(%q) = %q, want %q", s, got, w);
    }
    arena_free(&ar);
}

static Str bench_repeat(Alloc *a, Str s, Int n) {
    return strings_repeat(a, s, n);
}

typedef struct BenchData {
    Arena ar;
    Str escape_data;
    Str escape_none;
    Str unescape_sparse;
    Str unescape_dense;
} BenchData;

static void bench_data(BenchData *d) {
    arena_init(&d->ar, NULL, 0);
    Alloc *a = arena_allocator(&d->ar);
    d->escape_data =
        bench_repeat(a, BURROW_S("AAAAA < BBBBB > CCCCC & DDDDD ' EEEEE \" "), 100);
    d->escape_none =
        bench_repeat(a, BURROW_S("AAAAA x BBBBB x CCCCC x DDDDD x EEEEE x "), 100);
    Str parts[] = {
        bench_repeat(a, BURROW_S("AAAAA x BBBBB x CCCCC x DDDDD x EEEEE x "), 10),
        BURROW_S("&amp;")};
    Str sparse = strings_join(a, (Slice){parts, 2, 2, TYPE_STRING}, BURROW_STR_EMPTY);
    d->unescape_sparse = bench_repeat(a, sparse, 10);
    d->unescape_dense = bench_repeat(a, BURROW_S("&amp;&lt; &amp; &lt;"), 100);
}

/* The arena is reset every so often, so that a long run does not keep every
 * result. */
static void bench_escape(TestingB *b, Str in) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Int n = 0;
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++) {
        n += html_escape_string(a, in).len;
        if (i % 1024 == 1023)
            arena_reset(&ar);
    }
    (void)n;
    arena_free(&ar);
}

static void bench_unescape(TestingB *b, Str in) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Int n = 0;
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++) {
        n += html_unescape_string(a, in).len;
        if (i % 1024 == 1023)
            arena_reset(&ar);
    }
    (void)n;
    arena_free(&ar);
}

static void BenchmarkEscape(TestingB *b) {
    BenchData d;
    bench_data(&d);
    bench_escape(b, d.escape_data);
    arena_free(&d.ar);
}

static void BenchmarkEscapeNone(TestingB *b) {
    BenchData d;
    bench_data(&d);
    bench_escape(b, d.escape_none);
    arena_free(&d.ar);
}

static void BenchmarkUnescape(TestingB *b) {
    BenchData d;
    bench_data(&d);
    Str s = html_escape_string(arena_allocator(&d.ar), d.escape_data);
    bench_unescape(b, s);
    arena_free(&d.ar);
}

static void BenchmarkUnescapeNone(TestingB *b) {
    BenchData d;
    bench_data(&d);
    Str s = html_escape_string(arena_allocator(&d.ar), d.escape_none);
    bench_unescape(b, s);
    arena_free(&d.ar);
}

static void BenchmarkUnescapeSparse(TestingB *b) {
    BenchData d;
    bench_data(&d);
    bench_unescape(b, d.unescape_sparse);
    arena_free(&d.ar);
}

static void BenchmarkUnescapeDense(TestingB *b) {
    BenchData d;
    bench_data(&d);
    bench_unescape(b, d.unescape_dense);
    arena_free(&d.ar);
}

static void fuzz_escape_unescape(void *env, TestingT *t, Slice args) {
    (void)env;
    Str v = testing_fuzz_arg(args, 0, Str);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str e = html_escape_string(a, v);
    Str u = html_unescape_string(a, e);
    if (!str_eq(u, v))
        testing_t_errorf_v(t, "EscapeString(%q) = %q, UnescapeString(%q) = %q, want %q",
                           v, e, e, u, v);

    /* As per the documentation, this isn't always equal to v, so it makes no
     * sense to check for equality. It can still be interesting to find panics
     * in it though. */
    (void)html_escape_string(a, html_unescape_string(a, v));
    arena_free(&ar);
}

static void FuzzEscapeUnescape(TestingF *f) {
    testing_f_fuzz_v(f, BURROW_FN(TestingFuzzFunc, fuzz_escape_unescape, NULL),
                     TYPE_STRING);
}

#define TESTS(X)                                                                       \
    X(TestUnescape)                                                                    \
    X(TestUnescapeEscape)                                                              \
    X(TestEntityLength)                                                                \
    X(TestEntitySorted)                                                                \
    X(BenchmarkEscape)                                                                 \
    X(BenchmarkEscapeNone)                                                             \
    X(BenchmarkUnescape)                                                               \
    X(BenchmarkUnescapeNone)                                                           \
    X(BenchmarkUnescapeSparse)                                                         \
    X(BenchmarkUnescapeDense)                                                          \
    X(FuzzEscapeUnescape)

TESTING_MAIN(TESTS)

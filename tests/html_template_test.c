/* Derived from Go's src/html/template/css_test.go, html_test.go, js_test.go,
 * transition_test.go and url_test.go. Go source: go1.27.1.
 *
 * These are the tests of the escapers and the pieces of the context machine
 * that Go tests on their own, through the internal header. Go's escapers take
 * ...any, so the tests here pass a Slice of one Any. TestJSValEscaper's type
 * with a failing MarshalJSON is JsonErrType, and the message names it as
 * *JsonErrType where Go says *template.jsonErrType, since burrow types have no
 * package qualifier. C has no empty struct, so JsonErrType has one unused
 * field. BenchmarkTemplateSpecialTags needs the whole template and comes with
 * it. The yield and property access cases of TestNextJsCtx came with go1.27.2
 * (GO-2026-6600, CL 840925).
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/html/template.h"

#include "../src/html/template_internal.h"
#include "burrow/burrow.h"
#include "burrow/declare.h"
#include "burrow/encoding/json.h"
#include "burrow/html.h"
#include "burrow/mem/arena.h"
#include "burrow/strconv.h"
#include "burrow/strings.h"
#include "burrow/utf8.h"

#include "check.h"

#define S_(lit) BURROW_S_INIT(lit)
#define NELEM(x) ((Int)(sizeof(x) / sizeof((x)[0])))

typedef Str (*Escaper)(Alloc *a, Slice args);

/* The escaper called with the one argument v, as Go's f(v). */
static Str call1(Escaper f, Alloc *a, Any v) {
    return f(a, slice_from(&v, 1, 1, TYPE_ANY));
}

/* The escaper called with the one string s. */
static Str call_str(Escaper f, Alloc *a, Str s) {
    return call1(f, a, BURROW_ANY(TYPE_STRING, &s));
}

/* The input most of the escaper tests share: every 7 bit byte in order, then a
 * few code points above them. */
static const Str lower7 =
    S_("\x00\x01\x02\x03\x04\x05\x06\x07\x08\t\n\x0b\x0c\r\x0e\x0f\x10\x11\x12"
       "\x13\x14\x15\x16\x17\x18\x19\x1a\x1b\x1c\x1d\x1e\x1f !\"#$%&'()*+,-./0"
       "123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstu"
       "vwxyz{|}~\x7f\xc2\xa0\xc4\x80\xe2\x80\xa8\xe2\x80\xa9\xef\xbb\xbf\xf0\x9d"
       "\x84\x9e");

/* ---------------------------------------------------------------- css_test */

typedef struct EndsWithCSSKeywordTest {
    Str css, kw;
    bool want;
} EndsWithCSSKeywordTest;

static void TestEndsWithCSSKeyword(TestingT *t) {
    static const EndsWithCSSKeywordTest tests[] = {
        {S_(""), S_("url"), false},          {S_("url"), S_("url"), true},
        {S_("URL"), S_("url"), true},        {S_("Url"), S_("url"), true},
        {S_("url"), S_("important"), false}, {S_("important"), S_("important"), true},
        {S_("image-url"), S_("url"), false}, {S_("imageurl"), S_("url"), false},
        {S_("image url"), S_("url"), true},
    };
    for (Int i = 0; i < NELEM(tests); i++) {
        const EndsWithCSSKeywordTest *test = &tests[i];
        bool got = burrow__ht_ends_with_css_keyword(test->css, test->kw);
        if (got != test->want)
            testing_t_errorf_v(t, "want %t but got %t for css=%v, kw=%v", test->want,
                               got, test->css, test->kw);
    }
}

typedef struct IsCSSNmcharTest {
    Rune rune;
    bool want;
} IsCSSNmcharTest;

static void TestIsCSSNmchar(TestingT *t) {
    static const IsCSSNmcharTest tests[] = {
        {0, false},      {'0', true},     {'9', true},     {'A', true},
        {'Z', true},     {'a', true},     {'z', true},     {'_', true},
        {'-', true},     {':', false},    {';', false},    {' ', false},
        {0x7f, false},   {0x80, true},    {0x1234, true},  {0xd800, false},
        {0xdc00, false}, {0xfffe, false}, {0x10000, true}, {0x110000, false},
    };
    for (Int i = 0; i < NELEM(tests); i++) {
        const IsCSSNmcharTest *test = &tests[i];
        bool got = burrow__ht_is_css_nmchar(test->rune);
        if (got != test->want)
            testing_t_errorf_v(t, "%#x: want %t but got %t", (int64_t)test->rune,
                               test->want, got);
    }
}

typedef struct StrPairTest {
    Str in, want;
} StrPairTest;

static void TestDecodeCSS(TestingT *t) {
    static const StrPairTest tests[] = {
        {S_(""), S_("")},
        {S_("foo"), S_("foo")},
        {S_("foo\\"), S_("foo")},
        {S_("foo\\\\"), S_("foo\\")},
        {S_("\\"), S_("")},
        {S_("\\A"), S_("\n")},
        {S_("\\a"), S_("\n")},
        {S_("\\0a"), S_("\n")},
        {S_("\\00000a"), S_("\n")},
        {S_("\\000000a"), S_("\x00"
                             "a")},
        {S_("\\1234 5"), S_("\xe1\x88\xb4"
                            "5")},
        {S_("\\1234\\20 5"), S_("\xe1\x88\xb4 5")},
        {S_("\\1234\\A 5"), S_("\xe1\x88\xb4\n5")},
        {S_("\\1234\t5"), S_("\xe1\x88\xb4"
                             "5")},
        {S_("\\1234\n5"), S_("\xe1\x88\xb4"
                             "5")},
        {S_("\\1234\r\n5"), S_("\xe1\x88\xb4"
                               "5")},
        {S_("\\12345"), S_("\xf0\x92\x8d\x85")},
        {S_("\\\\"), S_("\\")},
        {S_("\\\\ "), S_("\\ ")},
        {S_("\\\""), S_("\"")},
        {S_("\\'"), S_("'")},
        {S_("\\."), S_(".")},
        {S_("\\. ."), S_(". .")},
        {S_("The \\3c i\\3equick\\3c/i\\3e,\\d\\A\\3cspan style=\\27 color:brown\\2"
            "7\\3e brown\\3c/span\\3e  fox jumps\\2028over the \\3c canine class=\\"
            "22lazy\\22 \\3e dog\\3c/canine\\3e"),
         S_("The <i>quick</i>,\r\n<span style='color:brown'>brown</span> fox jumps\xe2"
            "\x80\xa8over the <canine class=\"lazy\">dog</canine>")},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NELEM(tests); i++) {
        const StrPairTest *test = &tests[i];
        Str got1 = burrow__ht_decode_css(a, test->in);
        if (!str_eq(got1, test->want))
            testing_t_errorf_v(t, "%q: want\n\t%q\nbut got\n\t%q", test->in, test->want,
                               got1);
        Str recoded = call_str(burrow__ht_css_escaper, a, got1);
        Str got2 = burrow__ht_decode_css(a, recoded);
        if (!str_eq(got2, test->want))
            testing_t_errorf_v(t, "%q: escape & decode not dual for %q", test->in,
                               recoded);
    }
    arena_free(&ar);
}

static void TestHexDecode(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < 0x200000; i += 101 /* coprime with 16 */) {
        Str s = strconv_format_int(a, (int64_t)i, 16);
        Int got = (Int)burrow__ht_hex_decode(s);
        if (got != i)
            testing_t_errorf_v(t, "%s: want %d but got %d", s, i, got);
        s = strings_to_upper(a, s);
        got = (Int)burrow__ht_hex_decode(s);
        if (got != i)
            testing_t_errorf_v(t, "%s: want %d but got %d", s, i, got);
        arena_reset(&ar);
    }
    arena_free(&ar);
}

static void TestSkipCSSSpace(TestingT *t) {
    static const StrPairTest tests[] = {
        {S_(""), S_("")},          {S_("foo"), S_("foo")},   {S_("\n"), S_("")},
        {S_("\r\n"), S_("")},      {S_("\r"), S_("")},       {S_("\t"), S_("")},
        {S_(" "), S_("")},         {S_("\f"), S_("")},       {S_(" foo"), S_("foo")},
        {S_("  foo"), S_(" foo")}, {S_("\\20"), S_("\\20")},
    };
    for (Int i = 0; i < NELEM(tests); i++) {
        const StrPairTest *test = &tests[i];
        Str got = burrow__ht_skip_css_space(test->in);
        if (!str_eq(got, test->want))
            testing_t_errorf_v(t, "%q: want %q but got %q", test->in, test->want, got);
    }
}

static void TestCSSEscaper(TestingT *t) {
    Str input = lower7;
    Str want = BURROW_S(
        "\\0\x01\x02\x03\x04\x05\x06\x07\x08\\9 \\a\x0b\\c \\d\x0e\x0f\x10\x11\x12"
        "\x13\x14\x15\x16\x17\x18\x19\x1a\x1b\x1c\x1d\x1e\x1f !\\22#$%\\26\\27\\"
        "28\\29*\\2b,-.\\2f 0123456789\\3a\\3b\\3c=\\3e?@ABCDEFGHIJKLMNOPQRSTUV"
        "WXYZ[\\\\]^_`abcdefghijklmnopqrstuvwxyz\\7b|\\7d~\x7f\xc2\xa0\xc4\x80\xe2"
        "\x80\xa8\xe2\x80\xa9\xef\xbb\xbf\xf0\x9d\x84\x9e");

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str got = call_str(burrow__ht_css_escaper, a, input);
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "encode: want\n\t%q\nbut got\n\t%q", want, got);

    got = burrow__ht_decode_css(a, got);
    if (!str_eq(input, got))
        testing_t_errorf_v(t, "decode: want\n\t%q\nbut got\n\t%q", input, got);
    arena_free(&ar);
}

static void TestCSSValueFilter(TestingT *t) {
    static const StrPairTest tests[] = {
        {S_(""), S_("")},
        {S_("foo"), S_("foo")},
        {S_("0"), S_("0")},
        {S_("0px"), S_("0px")},
        {S_("-5px"), S_("-5px")},
        {S_("1.25in"), S_("1.25in")},
        {S_("+.33em"), S_("+.33em")},
        {S_("100%"), S_("100%")},
        {S_("12.5%"), S_("12.5%")},
        {S_(".foo"), S_(".foo")},
        {S_("#bar"), S_("#bar")},
        {S_("corner-radius"), S_("corner-radius")},
        {S_("-moz-corner-radius"), S_("-moz-corner-radius")},
        {S_("#000"), S_("#000")},
        {S_("#48f"), S_("#48f")},
        {S_("#123456"), S_("#123456")},
        {S_("U+00-FF, U+980-9FF"), S_("U+00-FF, U+980-9FF")},
        {S_("color: red"), S_("color: red")},
        {S_("<!--"), S_("ZgotmplZ")},
        {S_("-->"), S_("ZgotmplZ")},
        {S_("<![CDATA["), S_("ZgotmplZ")},
        {S_("]]>"), S_("ZgotmplZ")},
        {S_("</style"), S_("ZgotmplZ")},
        {S_("\""), S_("ZgotmplZ")},
        {S_("'"), S_("ZgotmplZ")},
        {S_("`"), S_("ZgotmplZ")},
        {S_("\x00"), S_("ZgotmplZ")},
        {S_("/* foo */"), S_("ZgotmplZ")},
        {S_("//"), S_("ZgotmplZ")},
        {S_("[href=~"), S_("ZgotmplZ")},
        {S_("expression(alert(1337))"), S_("ZgotmplZ")},
        {S_("-expression(alert(1337))"), S_("ZgotmplZ")},
        {S_("expression"), S_("ZgotmplZ")},
        {S_("Expression"), S_("ZgotmplZ")},
        {S_("EXPRESSION"), S_("ZgotmplZ")},
        {S_("-moz-binding"), S_("ZgotmplZ")},
        {S_("-expr\x00"
            "ession(alert(1337))"),
         S_("ZgotmplZ")},
        {S_("-expr\\0ession(alert(1337))"), S_("ZgotmplZ")},
        {S_("-express\\69on(alert(1337))"), S_("ZgotmplZ")},
        {S_("-express\\69 on(alert(1337))"), S_("ZgotmplZ")},
        {S_("-exp\\72 ession(alert(1337))"), S_("ZgotmplZ")},
        {S_("-exp\\52 ession(alert(1337))"), S_("ZgotmplZ")},
        {S_("-exp\\000052 ession(alert(1337))"), S_("ZgotmplZ")},
        {S_("-expre\\0000073sion"), S_("-expre\x07"
                                       "3sion")},
        {S_("@import url evil.css"), S_("ZgotmplZ")},
        {S_("<"), S_("ZgotmplZ")},
        {S_(">"), S_("ZgotmplZ")},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NELEM(tests); i++) {
        const StrPairTest *test = &tests[i];
        Str got = call_str(burrow__ht_css_value_filter, a, test->in);
        if (!str_eq(got, test->want))
            testing_t_errorf_v(t, "%q: want %q but got %q", test->in, test->want, got);
    }
    arena_free(&ar);
}

/* The benchmarks call an escaper with one argument over and over, or one of
 * the helpers that take a string. */
static void bench_escaper(TestingB *b, Escaper f, Any v) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice args = slice_from(&v, 1, 1, TYPE_ANY);
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++) {
        (void)f(a, args);
        if (i % 1024 == 1023)
            arena_reset(&ar);
    }
    arena_free(&ar);
}

static void bench_str(TestingB *b, Escaper f, Str s) {
    bench_escaper(b, f, BURROW_ANY(TYPE_STRING, &s));
}

static void bench_helper(TestingB *b, Str (*f)(Alloc *a, Str s), Str s) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++) {
        (void)f(a, s);
        if (i % 1024 == 1023)
            arena_reset(&ar);
    }
    arena_free(&ar);
}

/* The string most of Go's benchmarks escape, and the one without specials. */
static const Str quick =
    S_("The <i>quick</i>,\r\n<span style='color:brown'>brown</span> fox jumps\xe2"
       "\x80\xa8over the <canine class=\"lazy\">dog</canine>");
static const Str lazy_dog = S_("The quick, brown fox jumps over the lazy dog.");

static void BenchmarkCSSEscaper(TestingB *b) {
    bench_str(b, burrow__ht_css_escaper, quick);
}

static void BenchmarkCSSEscaperNoSpecials(TestingB *b) {
    bench_str(b, burrow__ht_css_escaper, lazy_dog);
}

static void BenchmarkDecodeCSS(TestingB *b) {
    bench_helper(
        b, burrow__ht_decode_css,
        BURROW_S(
            "The \\3c i\\3equick\\3c/i\\3e,\\d\\A\\3cspan style=\\27 color:brown\\2"
            "7\\3e brown\\3c/span\\3e fox jumps\\2028over the \\3c canine class=\\2"
            "2lazy\\22 \\3edog\\3c/canine\\3e"));
}

static void BenchmarkDecodeCSSNoSpecials(TestingB *b) {
    bench_helper(b, burrow__ht_decode_css, lazy_dog);
}

static void BenchmarkCSSValueFilter(TestingB *b) {
    bench_str(b, burrow__ht_css_value_filter,
              BURROW_S("  e\\78preS\\0Sio/**/n(alert(1337))"));
}

static void BenchmarkCSSValueFilterOk(TestingB *b) {
    bench_str(b, burrow__ht_css_value_filter, BURROW_S("Times New Roman"));
}

/* --------------------------------------------------------------- html_test */

static void TestHTMLNospaceEscaper(TestingT *t) {
    Str input = BURROW_S(
        "\x00\x01\x02\x03\x04\x05\x06\x07\x08\t\n\x0b\x0c\r\x0e\x0f\x10\x11\x12"
        "\x13\x14\x15\x16\x17\x18\x19\x1a\x1b\x1c\x1d\x1e\x1f !\"#$%&'()*+,-./0"
        "123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstu"
        "vwxyz{|}~\x7f\xc2\xa0\xc4\x80\xe2\x80\xa8\xe2\x80\xa9\xef\xbb\xbf\xef\xb7"
        "\xac\xf0\x9d\x84\x9e"
        "erroneous\x96"
        "0");

    Str want = BURROW_S(
        "&#xfffd;\x01\x02\x03\x04\x05\x06\x07\x08&#9;&#10;&#11;&#12;&#13;\x0e\x0f"
        "\x10\x11\x12\x13\x14\x15\x16\x17\x18\x19\x1a\x1b\x1c\x1d\x1e\x1f&#32;!"
        "&#34;#$%&amp;&#39;()*&#43;,-./0123456789:;&lt;&#61;&gt;?@ABCDEFGHIJKLM"
        "NOPQRSTUVWXYZ[\\]^_&#96;abcdefghijklmnopqrstuvwxyz{|}~\x7f\xc2\xa0\xc4"
        "\x80\xe2\x80\xa8\xe2\x80\xa9\xef\xbb\xbf&#xfdec;\xf0\x9d\x84\x9e"
        "err"
        "oneous&#xfffd;0");

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str got = call_str(burrow__ht_html_nospace_escaper, a, input);
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "encode: want\n\t%q\nbut got\n\t%q", want, got);

    /* Go's strings.NewReplacer("\x00", "\ufffd", "\x96", "\ufffd"). */
    got = html_unescape_string(a, got);
    want = strings_replace_all(a, input, BURROW_S("\x00"), BURROW_S("\xef\xbf\xbd"));
    want = strings_replace_all(a, want, BURROW_S("\x96"), BURROW_S("\xef\xbf\xbd"));
    if (!str_eq(want, got))
        testing_t_errorf_v(t, "decode: want\n\t%q\nbut got\n\t%q", want, got);
    arena_free(&ar);
}

static void TestStripTags(TestingT *t) {
    static const StrPairTest tests[] = {
        {S_(""), S_("")},
        {S_("Hello, World!"), S_("Hello, World!")},
        {S_("foo&amp;bar"), S_("foo&amp;bar")},
        {S_("Hello <a href=\"www.example.com/\">World</a>!"), S_("Hello World!")},
        {S_("Foo <textarea>Bar</textarea> Baz"), S_("Foo Bar Baz")},
        {S_("Foo <!-- Bar --> Baz"), S_("Foo  Baz")},
        {S_("<"), S_("<")},
        {S_("foo < bar"), S_("foo < bar")},
        {S_("Foo<script type=\"text/javascript\">alert(1337)</script>Bar"),
         S_("FooBar")},
        {S_("Foo<div title=\"1>2\">Bar"), S_("FooBar")},
        {S_("I <3 Ponies!"), S_("I <3 Ponies!")},
        {S_("<script>foo()</script>"), S_("")},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NELEM(tests); i++) {
        const StrPairTest *test = &tests[i];
        Str got = burrow__ht_strip_tags(a, test->in);
        if (!str_eq(got, test->want))
            testing_t_errorf_v(t, "%q: want %q, got %q", test->in, test->want, got);
    }
    arena_free(&ar);
}

static void BenchmarkHTMLNospaceEscaper(TestingB *b) {
    bench_str(b, burrow__ht_html_nospace_escaper, quick);
}

static void BenchmarkHTMLNospaceEscaperNoSpecials(TestingB *b) {
    bench_str(b, burrow__ht_html_nospace_escaper,
              BURROW_S("The_quick,_brown_fox_jumps_over_the_lazy_dog."));
}

static void BenchmarkStripTags(TestingB *b) {
    bench_helper(b, burrow__ht_strip_tags, quick);
}

static void BenchmarkStripTagsNoSpecials(TestingB *b) {
    bench_helper(b, burrow__ht_strip_tags, lazy_dog);
}

/* ----------------------------------------------------------------- js_test */

typedef struct NextJsCtxTest {
    burrow__HtJsCtx js_ctx;
    Str s;
} NextJsCtxTest;

static void TestNextJsCtx(TestingT *t) {
    static const NextJsCtxTest tests[] = {
        /* Statement terminators precede regexps. */
        {BURROW__HT_JS_CTX_REGEXP, S_(";")},
        /* This is not airtight.
         *     ({ valueOf: function () { return 1 } } / 2)
         * is valid JavaScript but in practice, devs do not do this.
         * A block followed by a statement starting with a RegExp is
         * much more common:
         *     while (x) {...} /foo/.test(x) || panic() */
        {BURROW__HT_JS_CTX_REGEXP, S_("}")},
        /* But member, call, grouping, and array expression terminators
         * precede div ops. */
        {BURROW__HT_JS_CTX_DIV_OP, S_(")")},
        {BURROW__HT_JS_CTX_DIV_OP, S_("]")},
        /* At the start of a primary expression, array, or expression
         * statement, expect a regexp. */
        {BURROW__HT_JS_CTX_REGEXP, S_("(")},
        {BURROW__HT_JS_CTX_REGEXP, S_("[")},
        {BURROW__HT_JS_CTX_REGEXP, S_("{")},
        /* Assignment operators precede regexps as do all exclusively
         * prefix and binary operators. */
        {BURROW__HT_JS_CTX_REGEXP, S_("=")},
        {BURROW__HT_JS_CTX_REGEXP, S_("+=")},
        {BURROW__HT_JS_CTX_REGEXP, S_("*=")},
        {BURROW__HT_JS_CTX_REGEXP, S_("*")},
        {BURROW__HT_JS_CTX_REGEXP, S_("!")},
        /* Whether the + or - is infix or prefix, it cannot precede a
         * div op. */
        {BURROW__HT_JS_CTX_REGEXP, S_("+")},
        {BURROW__HT_JS_CTX_REGEXP, S_("-")},
        /* An incr/decr op precedes a div operator.
         * This is not airtight. In (g = ++/h/i) a regexp follows a
         * pre-increment operator, but in practice devs do not try to
         * increment or decrement regular expressions.
         * (g++/h/i) where ++ is a postfix operator on g is much more
         * common. */
        {BURROW__HT_JS_CTX_DIV_OP, S_("--")},
        {BURROW__HT_JS_CTX_DIV_OP, S_("++")},
        {BURROW__HT_JS_CTX_DIV_OP, S_("x--")},
        /* When we have many dashes or pluses, then they are grouped
         * left to right. */
        {BURROW__HT_JS_CTX_REGEXP, S_("x---")}, /* A postfix -- then a -. */
        /* return followed by a slash returns the regexp literal or the
         * slash starts a regexp literal in an expression statement that
         * is dead code. */
        {BURROW__HT_JS_CTX_REGEXP, S_("return")},
        {BURROW__HT_JS_CTX_REGEXP, S_("return ")},
        {BURROW__HT_JS_CTX_REGEXP, S_("return\t")},
        {BURROW__HT_JS_CTX_REGEXP, S_("return\n")},
        {BURROW__HT_JS_CTX_REGEXP, S_("return\xe2\x80\xa8")},
        {BURROW__HT_JS_CTX_REGEXP, S_("yield")},
        /* A keyword after property access is a property name. */
        {BURROW__HT_JS_CTX_DIV_OP, S_("x.yield")},
        {BURROW__HT_JS_CTX_DIV_OP, S_("x?.yield")},
        {BURROW__HT_JS_CTX_DIV_OP, S_("x.\nyield")},
        {BURROW__HT_JS_CTX_DIV_OP, S_("x.in")},
        /* Identifiers can be divided and cannot validly be preceded by
         * a regular expressions. Semicolon insertion cannot happen
         * between an identifier and a regular expression on a new line
         * because the one token lookahead for semicolon insertion has
         * to conclude that it could be a div binary op and treat it as
         * such. */
        {BURROW__HT_JS_CTX_DIV_OP, S_("x")},
        {BURROW__HT_JS_CTX_DIV_OP, S_("x ")},
        {BURROW__HT_JS_CTX_DIV_OP, S_("x\t")},
        {BURROW__HT_JS_CTX_DIV_OP, S_("x\n")},
        {BURROW__HT_JS_CTX_DIV_OP, S_("x\xe2\x80\xa8")},
        {BURROW__HT_JS_CTX_DIV_OP, S_("preturn")},
        /* Numbers precede div ops. */
        {BURROW__HT_JS_CTX_DIV_OP, S_("0")},
        /* Dots that are part of a number are div preceders. */
        {BURROW__HT_JS_CTX_DIV_OP, S_("0.")},
        /* Some JS interpreters treat NBSP as a normal space, so
         * we must too in order to properly escape things. */
        {BURROW__HT_JS_CTX_REGEXP, S_("=\xc2\xa0")},
    };

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NELEM(tests); i++) {
        const NextJsCtxTest *test = &tests[i];
        burrow__HtJsCtx ctx = burrow__ht_next_js_ctx(test->s, BURROW__HT_JS_CTX_REGEXP);
        if (ctx != test->js_ctx)
            testing_t_errorf_v(t, "%q: want %s got %s", test->s,
                               burrow__ht_js_ctx_string(a, test->js_ctx),
                               burrow__ht_js_ctx_string(a, ctx));
        ctx = burrow__ht_next_js_ctx(test->s, BURROW__HT_JS_CTX_DIV_OP);
        if (ctx != test->js_ctx)
            testing_t_errorf_v(t, "%q: want %s got %s", test->s,
                               burrow__ht_js_ctx_string(a, test->js_ctx),
                               burrow__ht_js_ctx_string(a, ctx));
    }
    arena_free(&ar);

    if (burrow__ht_next_js_ctx(BURROW_S("   "), BURROW__HT_JS_CTX_REGEXP) !=
        BURROW__HT_JS_CTX_REGEXP)
        testing_t_errorf_v(t, "Blank tokens");

    if (burrow__ht_next_js_ctx(BURROW_S("   "), BURROW__HT_JS_CTX_DIV_OP) !=
        BURROW__HT_JS_CTX_DIV_OP)
        testing_t_errorf_v(t, "Blank tokens");
}

#define JSON_ERR_TYPE_FIELDS(F, T) F(T, Int, Unused, "json:\"-\"")
BURROW_STRUCT_DECL(JsonErrType, JSON_ERR_TYPE_FIELDS);

static Slice json_err_type_marshal_json(JsonErrType *e, Alloc *a, Error *err) {
    (void)e;
    (void)a;
    BURROW_OUT(
        err,
        errors_new(error_allocator(),
                   BURROW_S("a */ b <script c </script d <!-- e <sCrIpT f </sCrIpT")));
    return slice_nil(TYPE_BYTE);
}

#define JSON_ERR_TYPE_METHODS(M, T)                                                    \
    M(T, MarshalJSON, json_err_type_marshal_json, JSONV2_SIG_MARSHAL_JSON)
BURROW_STRUCT_DEFINE_METHODS(JsonErrType, JSON_ERR_TYPE_FIELDS, JSON_ERR_TYPE_METHODS);

/* Go's struct{ X, Y int }. */
#define XY_FIELDS(F, T) F(T, Int, X, "") F(T, Int, Y, "")
BURROW_STRUCT(XY, XY_FIELDS);

/* Go's struct{ S string; N int } of the benchmarks. */
#define SN_FIELDS(F, T) F(T, Str, S, "") F(T, Int, N, "")
BURROW_STRUCT(SN, SN_FIELDS);

BURROW_SLICE_TYPE(Anys, Any);
BURROW_SLICE_TYPE(Strs, Str);

typedef struct JSValEscaperTest {
    Any x;
    Str js;
    bool skip_nest;
} JSValEscaperTest;

static void TestJSValEscaper(TestingT *t) {
    /* Go's []any{} is not nil, so it points somewhere. */
    static Any no_anys[1];
    Anys empty = slice_from(no_anys, 0, 0, TYPE_ANY);
    Any anys_elems[] = {
        BURROW_ANY_VAL(TYPE_INT, Int, 42),
        BURROW_ANY_VAL(TYPE_STRING, Str, BURROW_S("foo")),
        BURROW_ANY(NULL, NULL),
    };
    Anys anys = slice_from(anys_elems, 3, 3, TYPE_ANY);
    Str strs_elems[] = {BURROW_S("<!--"), BURROW_S("</script>"), BURROW_S("-->")};
    Strs strs = slice_from(strs_elems, 3, 3, TYPE_STRING);
    JsonErrType json_err = {0};

    const JSValEscaperTest tests[] = {
        {BURROW_ANY_VAL(TYPE_INT, Int, 42), BURROW_S(" 42 "), false},
        {BURROW_ANY_VAL(TYPE_UINT, Uint, 42), BURROW_S(" 42 "), false},
        {BURROW_ANY_VAL(TYPE_INT16, int16_t, 42), BURROW_S(" 42 "), false},
        {BURROW_ANY_VAL(TYPE_UINT16, uint16_t, 42), BURROW_S(" 42 "), false},
        {BURROW_ANY_VAL(TYPE_INT32, int32_t, -42), BURROW_S(" -42 "), false},
        {BURROW_ANY_VAL(TYPE_UINT32, uint32_t, 42), BURROW_S(" 42 "), false},
        {BURROW_ANY_VAL(TYPE_INT16, int16_t, -42), BURROW_S(" -42 "), false},
        {BURROW_ANY_VAL(TYPE_UINT16, uint16_t, 42), BURROW_S(" 42 "), false},
        {BURROW_ANY_VAL(TYPE_INT64, int64_t, -42), BURROW_S(" -42 "), false},
        {BURROW_ANY_VAL(TYPE_UINT64, uint64_t, 42), BURROW_S(" 42 "), false},
        {BURROW_ANY_VAL(TYPE_UINT64, uint64_t, (uint64_t)1 << 53),
         BURROW_S(" 9007199254740992 "), false},
        /* ulp(1 << 53) > 1 so this loses precision in JS
         * but it is still a representable integer literal. */
        {BURROW_ANY_VAL(TYPE_UINT64, uint64_t, ((uint64_t)1 << 53) + 1),
         BURROW_S(" 9007199254740993 "), false},
        {BURROW_ANY_VAL(TYPE_FLOAT32, float, 1.0f), BURROW_S(" 1 "), false},
        {BURROW_ANY_VAL(TYPE_FLOAT32, float, -1.0f), BURROW_S(" -1 "), false},
        {BURROW_ANY_VAL(TYPE_FLOAT32, float, 0.5f), BURROW_S(" 0.5 "), false},
        {BURROW_ANY_VAL(TYPE_FLOAT32, float, -0.5f), BURROW_S(" -0.5 "), false},
        {BURROW_ANY_VAL(TYPE_FLOAT32, float, 1.0f / 256.0f), BURROW_S(" 0.00390625 "),
         false},
        {BURROW_ANY_VAL(TYPE_FLOAT32, float, 0.0f), BURROW_S(" 0 "), false},
        {BURROW_ANY_VAL(TYPE_FLOAT64, double, -0.0), BURROW_S(" -0 "), false},
        {BURROW_ANY_VAL(TYPE_FLOAT64, double, 1.0), BURROW_S(" 1 "), false},
        {BURROW_ANY_VAL(TYPE_FLOAT64, double, -1.0), BURROW_S(" -1 "), false},
        {BURROW_ANY_VAL(TYPE_FLOAT64, double, 0.5), BURROW_S(" 0.5 "), false},
        {BURROW_ANY_VAL(TYPE_FLOAT64, double, -0.5), BURROW_S(" -0.5 "), false},
        {BURROW_ANY_VAL(TYPE_FLOAT64, double, 0.0), BURROW_S(" 0 "), false},
        {BURROW_ANY_VAL(TYPE_FLOAT64, double, -0.0), BURROW_S(" -0 "), false},
        {BURROW_ANY_VAL(TYPE_STRING, Str, BURROW_S("")), BURROW_S("\"\""), false},
        {BURROW_ANY_VAL(TYPE_STRING, Str, BURROW_S("foo")), BURROW_S("\"foo\""), false},
        /* Newlines. */
        {BURROW_ANY_VAL(TYPE_STRING, Str, BURROW_S("\r\n\xe2\x80\xa8\xe2\x80\xa9")),
         BURROW_S("\"\\r\\n\\u2028\\u2029\""), false},
        /* "\v" == "v" on IE 6 so use "\u000b" instead. */
        {BURROW_ANY_VAL(TYPE_STRING, Str, BURROW_S("\t\x0b")),
         BURROW_S("\"\\t\\u000b\""), false},
        {BURROW_ANY_VAL(TYPE_OF(XY), XY, ((XY){1, 2})), BURROW_S("{\"X\":1,\"Y\":2}"),
         false},
        {BURROW_ANY(TYPE_OF(Anys), &empty), BURROW_S("[]"), false},
        {BURROW_ANY(TYPE_OF(Anys), &anys), BURROW_S("[42,\"foo\",null]"), false},
        {BURROW_ANY(TYPE_OF(Strs), &strs),
         BURROW_S("[\"\\u003c!--\",\"\\u003c/script\\u003e\",\"--\\u003e\"]"), false},
        {BURROW_ANY_VAL(TYPE_STRING, Str, BURROW_S("<!--")), BURROW_S("\"\\u003c!--\""),
         false},
        {BURROW_ANY_VAL(TYPE_STRING, Str, BURROW_S("-->")), BURROW_S("\"--\\u003e\""),
         false},
        {BURROW_ANY_VAL(TYPE_STRING, Str, BURROW_S("<![CDATA[")),
         BURROW_S("\"\\u003c![CDATA[\""), false},
        {BURROW_ANY_VAL(TYPE_STRING, Str, BURROW_S("]]>")), BURROW_S("\"]]\\u003e\""),
         false},
        {BURROW_ANY_VAL(TYPE_STRING, Str, BURROW_S("</script")),
         BURROW_S("\"\\u003c/script\""), false},
        {BURROW_ANY_VAL(TYPE_STRING, Str, BURROW_S("\xf0\x9d\x84\x9e")),
         BURROW_S("\"\xf0\x9d\x84\x9e\""), false}, /* or "\uD834\uDD1E" */
        {BURROW_ANY(NULL, NULL), BURROW_S(" null "), false},
        {BURROW_ANY(TYPE_OF(JsonErrType), &json_err),
         BURROW_S(
             " /* json: error calling MarshalJSON for type *JsonErrType: a * / b \\x"
             "3Cscript c \\x3C/script d \\x3C!-- e \\x3Cscript f \\x3C/script */null"
             " "),
         true},
    };

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NELEM(tests); i++) {
        const JSValEscaperTest *test = &tests[i];
        Str js = call1(burrow__ht_js_val_escaper, a, test->x);
        if (!str_eq(js, test->js))
            testing_t_errorf_v(t, "%+v: want\n\t%q\ngot\n\t%q", test->x, test->js, js);
        if (test->skip_nest)
            continue;
        /* Make sure that escaping corner cases are not broken
         * by nesting. */
        Any elems[1] = {test->x};
        Anys nest = slice_from(elems, 1, 1, TYPE_ANY);
        Any nested = BURROW_ANY(TYPE_OF(Anys), &nest);
        StringsBuilder w = STRINGS_BUILDER(a);
        burrow__ht_write(&w, BURROW_S("["));
        burrow__ht_write(&w, strings_trim_space(test->js));
        burrow__ht_write(&w, BURROW_S("]"));
        Str want = strings_builder_string(&w);
        js = call1(burrow__ht_js_val_escaper, a, nested);
        if (!str_eq(js, want))
            testing_t_errorf_v(t, "%+v: want\n\t%q\ngot\n\t%q", nested, want, js);
    }
    arena_free(&ar);
}

static void TestJSStrEscaper(TestingT *t) {
    static const StrPairTest tests[] = {
        {S_(""), S_("")},
        {S_("foo"), S_("foo")},
        {S_("\x00"), S_("\\u0000")},
        {S_("\t"), S_("\\t")},
        {S_("\n"), S_("\\n")},
        {S_("\r"), S_("\\r")},
        {S_("\xe2\x80\xa8"), S_("\\u2028")},
        {S_("\xe2\x80\xa9"), S_("\\u2029")},
        {S_("\\"), S_("\\\\")},
        {S_("\\n"), S_("\\\\n")},
        {S_("foo\r\nbar"), S_("foo\\r\\nbar")},
        /* Preserve attribute boundaries. */
        {S_("\""), S_("\\u0022")},
        {S_("'"), S_("\\u0027")},
        /* Allow embedding in HTML without further escaping. */
        {S_("&amp;"), S_("\\u0026amp;")},
        /* Prevent breaking out of text node and element boundaries. */
        {S_("</script>"), S_("\\u003c\\/script\\u003e")},
        {S_("<![CDATA["), S_("\\u003c![CDATA[")},
        {S_("]]>"), S_("]]\\u003e")},
        /* https://dev.w3.org/html5/markup/aria/syntax.html#escaping-text-span
         *   "The text in style, script, title, and textarea elements
         *   must not have an escaping text span start that is not
         *   followed by an escaping text span end."
         * Furthermore, spoofing an escaping text span end could lead
         * to different interpretation of a </script> sequence otherwise
         * masked by the escaping text span, and spoofing a start could
         * allow regular text content to be interpreted as script
         * allowing script execution via a combination of a JS string
         * injection followed by an HTML text injection. */
        {S_("<!--"), S_("\\u003c!--")},
        {S_("-->"), S_("--\\u003e")},
        /* From https://code.google.com/p/doctype/wiki/ArticleUtf7 */
        {S_("+ADw-script+AD4-alert(1)+ADw-/script+AD4-"),
         S_("\\u002bADw-script\\u002bAD4-alert(1)\\u002bADw-\\/script\\u002bAD4-")},
        /* Invalid UTF-8 sequence */
        {S_("foo\xa0"
            "bar"),
         S_("foo\xa0"
            "bar")},
        /* Invalid unicode scalar value. */
        {S_("foo\xed\xa0\x80"
            "bar"),
         S_("foo\xed\xa0\x80"
            "bar")},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NELEM(tests); i++) {
        const StrPairTest *test = &tests[i];
        Str esc = call_str(burrow__ht_js_str_escaper, a, test->in);
        if (!str_eq(esc, test->want))
            testing_t_errorf_v(t, "%q: want %q got %q", test->in, test->want, esc);
    }
    arena_free(&ar);
}

static void TestJSRegexpEscaper(TestingT *t) {
    static const StrPairTest tests[] = {
        {S_(""), S_("(?:)")},
        {S_("foo"), S_("foo")},
        {S_("\x00"), S_("\\u0000")},
        {S_("\t"), S_("\\t")},
        {S_("\n"), S_("\\n")},
        {S_("\r"), S_("\\r")},
        {S_("\xe2\x80\xa8"), S_("\\u2028")},
        {S_("\xe2\x80\xa9"), S_("\\u2029")},
        {S_("\\"), S_("\\\\")},
        {S_("\\n"), S_("\\\\n")},
        {S_("foo\r\nbar"), S_("foo\\r\\nbar")},
        /* Preserve attribute boundaries. */
        {S_("\""), S_("\\u0022")},
        {S_("'"), S_("\\u0027")},
        /* Allow embedding in HTML without further escaping. */
        {S_("&amp;"), S_("\\u0026amp;")},
        /* Prevent breaking out of text node and element boundaries. */
        {S_("</script>"), S_("\\u003c\\/script\\u003e")},
        {S_("<![CDATA["), S_("\\u003c!\\[CDATA\\[")},
        {S_("]]>"), S_("\\]\\]\\u003e")},
        /* Escaping text spans. */
        {S_("<!--"), S_("\\u003c!\\-\\-")},
        {S_("-->"), S_("\\-\\-\\u003e")},
        {S_("*"), S_("\\*")},
        {S_("+"), S_("\\u002b")},
        {S_("?"), S_("\\?")},
        {S_("[](){}"), S_("\\[\\]\\(\\)\\{\\}")},
        {S_("$foo|x.y"), S_("\\$foo\\|x\\.y")},
        {S_("x^y"), S_("x\\^y")},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NELEM(tests); i++) {
        const StrPairTest *test = &tests[i];
        Str esc = call_str(burrow__ht_js_regexp_escaper, a, test->in);
        if (!str_eq(esc, test->want))
            testing_t_errorf_v(t, "%q: want %q got %q", test->in, test->want, esc);
    }
    arena_free(&ar);
}

typedef struct EscaperTest {
    Str name;
    Escaper escaper;
    Str escaped;
} EscaperTest;

static void TestEscapersOnLower7AndSelectHighCodepoints(TestingT *t) {
    Str input = lower7;

    const EscaperTest tests[] = {
        {
            BURROW_S("jsStrEscaper"),
            burrow__ht_js_str_escaper,
            BURROW_S(
                "\\u0000\\u0001\\u0002\\u0003\\u0004\\u0005\\u0006\\u0007\\u0008\\t\\n"
                "\\"
                "u000b\\f\\r\\u000e\\u000f\\u0010\\u0011\\u0012\\u0013\\u0014\\u0015\\u"
                "0016\\u0017\\u0018\\u0019\\u001a\\u001b\\u001c\\u001d\\u001e\\u001f "
                "!\\"
                "u0022#$%\\u0026\\u0027()*\\u002b,-.\\/0123456789:;\\u003c=\\u003e?@ABC"
                "DEFGHIJKLMNOPQRSTUVWXYZ[\\\\]^_\\u0060abcdefghijklmnopqrstuvwxyz{|}~"
                "\x7f"
                "\xc2\xa0\xc4\x80\\u2028\\u2029\xef\xbb\xbf\xf0\x9d\x84\x9e"),
        },
        {
            BURROW_S("jsRegexpEscaper"),
            burrow__ht_js_regexp_escaper,
            BURROW_S(
                "\\u0000\\u0001\\u0002\\u0003\\u0004\\u0005\\u0006\\u0007\\u0008\\t\\n"
                "\\"
                "u000b\\f\\r\\u000e\\u000f\\u0010\\u0011\\u0012\\u0013\\u0014\\u0015\\u"
                "0016\\u0017\\u0018\\u0019\\u001a\\u001b\\u001c\\u001d\\u001e\\u001f "
                "!\\"
                "u0022#\\$%\\u0026\\u0027\\(\\)\\*\\u002b,\\-\\.\\/0123456789:;\\u003c="
                "\\u003e\\?@ABCDEFGHIJKLMNOPQRSTUVWXYZ\\[\\\\\\]\\^_`abcdefghijklmnopqr"
                "stuvwxyz\\{\\|\\}~"
                "\x7f\xc2\xa0\xc4\x80\\u2028\\u2029\xef\xbb\xbf\xf0\x9d"
                "\x84\x9e"),
        },
    };

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NELEM(tests); i++) {
        const EscaperTest *test = &tests[i];
        Str s = call_str(test->escaper, a, input);
        if (!str_eq(s, test->escaped)) {
            testing_t_errorf_v(t, "%s once: want\n\t%q\ngot\n\t%q", test->name,
                               test->escaped, s);
            continue;
        }

        /* Escape it rune by rune to make sure that any
         * fast-path checking does not break escaping. */
        StringsBuilder buf = STRINGS_BUILDER(a);
        for (Int j = 0; j < input.len;) {
            Int size = 0;
            (void)utf8_decode_rune_in_string(burrow__ht_sub(input, j, input.len),
                                             &size);
            burrow__ht_write(
                &buf, call_str(test->escaper, a, burrow__ht_sub(input, j, j + size)));
            j += size;
        }

        s = strings_builder_string(&buf);
        if (!str_eq(s, test->escaped)) {
            testing_t_errorf_v(t, "%s rune-wise: want\n\t%q\ngot\n\t%q", test->name,
                               test->escaped, s);
            continue;
        }
    }
    arena_free(&ar);
}

typedef struct IsJsMimeTypeTest {
    Str in;
    bool out;
} IsJsMimeTypeTest;

static void TestIsJsMimeType(TestingT *t) {
    static const IsJsMimeTypeTest tests[] = {
        {S_("application/javascript;version=1.8"), true},
        {S_("application/javascript;version=1.8;foo=bar"), true},
        {S_("application/javascript/version=1.8"), false},
        {S_("text/javascript"), true},
        {S_("application/json"), true},
        {S_("application/ld+json"), true},
        {S_("module"), true},
    };
    for (Int i = 0; i < NELEM(tests); i++) {
        const IsJsMimeTypeTest *test = &tests[i];
        if (burrow__ht_is_js_type(test->in) != test->out)
            testing_t_errorf_v(t, "isJSType(%q) = %v, want %v", test->in, !test->out,
                               test->out);
    }
}

static void BenchmarkJSValEscaperWithNum(TestingB *b) {
    bench_escaper(b, burrow__ht_js_val_escaper,
                  BURROW_ANY_VAL(TYPE_FLOAT64, double, 3.141592654));
}

static void BenchmarkJSValEscaperWithStr(TestingB *b) {
    bench_str(b, burrow__ht_js_val_escaper, quick);
}

static void BenchmarkJSValEscaperWithStrNoSpecials(TestingB *b) {
    bench_str(b, burrow__ht_js_val_escaper,
              BURROW_S("The quick, brown fox jumps over the lazy dog"));
}

static void BenchmarkJSValEscaperWithObj(TestingB *b) {
    SN o = {
        BURROW_S(
            "The <i>quick</i>,\r\n<span style='color:brown'>brown</span> fox jumps\xe2"
            "\x80\xa8over the <canine class=\"lazy\">dog</canine>\xe2\x80\xa8"),
        42};
    bench_escaper(b, burrow__ht_js_val_escaper, BURROW_ANY(TYPE_OF(SN), &o));
}

static void BenchmarkJSValEscaperWithObjNoSpecials(TestingB *b) {
    SN o = {BURROW_S("The quick, brown fox jumps over the lazy dog"), 42};
    bench_escaper(b, burrow__ht_js_val_escaper, BURROW_ANY(TYPE_OF(SN), &o));
}

static void BenchmarkJSStrEscaperNoSpecials(TestingB *b) {
    bench_str(b, burrow__ht_js_str_escaper, lazy_dog);
}

static void BenchmarkJSStrEscaper(TestingB *b) {
    bench_str(b, burrow__ht_js_str_escaper, quick);
}

static void BenchmarkJSRegexpEscaperNoSpecials(TestingB *b) {
    bench_str(b, burrow__ht_js_regexp_escaper,
              BURROW_S("The quick, brown fox jumps over the lazy dog"));
}

static void BenchmarkJSRegexpEscaper(TestingB *b) {
    bench_str(b, burrow__ht_js_regexp_escaper, quick);
}

/* --------------------------------------------------------- transition_test */

typedef struct FindEndTagTest {
    Str s, tag;
    Int want;
} FindEndTagTest;

static void TestFindEndTag(TestingT *t) {
    static const FindEndTagTest tests[] = {
        {S_(""), S_("tag"), -1},
        {S_("hello </textarea> hello"), S_("textarea"), 6},
        {S_("hello </TEXTarea> hello"), S_("textarea"), 6},
        {S_("hello </textAREA>"), S_("textarea"), 6},
        {S_("hello </textarea"), S_("textareax"), -1},
        {S_("hello </textarea>"), S_("tag"), -1},
        {S_("hello tag </textarea"), S_("tag"), -1},
        {S_("hello </tag> </other> </textarea> <other>"), S_("textarea"), 22},
        {S_("</textarea> <other>"), S_("textarea"), 0},
        {S_("<div> </div> </TEXTAREA>"), S_("textarea"), 13},
        {S_("<div> </div> </TEXTAREA\t>"), S_("textarea"), 13},
        {S_("<div> </div> </TEXTAREA >"), S_("textarea"), 13},
        {S_("<div> </div> </TEXTAREAfoo"), S_("textarea"), -1},
        {S_("</TEXTAREAfoo </textarea>"), S_("textarea"), 14},
        {S_("<</script >"), S_("script"), 1},
        {S_("</script>"), S_("textarea"), -1},
    };
    for (Int i = 0; i < NELEM(tests); i++) {
        const FindEndTagTest *test = &tests[i];
        Int got = burrow__ht_index_tag_end(test->s, test->tag);
        if (test->want != got)
            testing_t_errorf_v(t, "%q/%q: want\n\t%d\nbut got\n\t%d", test->s,
                               test->tag, test->want, got);
    }
}

/* ---------------------------------------------------------------- url_test */

static void TestURLNormalizer(TestingT *t) {
    static const StrPairTest tests[] = {
        {S_(""), S_("")},
        {
            S_("http://example.com:80/foo/bar?q=foo%20&bar=x+y#frag"),
            S_("http://example.com:80/foo/bar?q=foo%20&bar=x+y#frag"),
        },
        {S_(" "), S_("%20")},
        {S_("%7c"), S_("%7c")},
        {S_("%7C"), S_("%7C")},
        {S_("%2"), S_("%252")},
        {S_("%"), S_("%25")},
        {S_("%z"), S_("%25z")},
        {S_("/foo|bar/%5c\xe1\x88\xb4"), S_("/foo%7cbar/%5c%e1%88%b4")},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NELEM(tests); i++) {
        const StrPairTest *test = &tests[i];
        Str got = call_str(burrow__ht_url_normalizer, a, test->in);
        if (!str_eq(test->want, got))
            testing_t_errorf_v(t, "%q: want\n\t%q\nbut got\n\t%q", test->in, test->want,
                               got);
        if (!str_eq(test->want, call_str(burrow__ht_url_normalizer, a, test->want)))
            testing_t_errorf_v(t, "not idempotent: %q", test->want);
    }
    arena_free(&ar);
}

static void TestURLFilters(TestingT *t) {
    Str input = lower7;

    const EscaperTest tests[] = {
        {
            BURROW_S("urlEscaper"),
            burrow__ht_url_escaper,
            BURROW_S(
                "%00%01%02%03%04%05%06%07%08%09%0a%0b%0c%0d%0e%0f%10%11%12%13%14%15%16%"
                "17%18%19%1a%1b%1c%1d%1e%1f%20%21%22%23%24%25%26%27%28%29%2a%2b%2c-.%2f"
                "0123456789%3a%3b%3c%3d%3e%3f%40ABCDEFGHIJKLMNOPQRSTUVWXYZ%5b%5c%5d%5e_"
                "%60abcdefghijklmnopqrstuvwxyz%7b%7c%7d~%7f%c2%a0%c4%80%e2%80%a8%e2%80%"
                "a9%ef%bb%bf%f0%9d%84%9e"),
        },
        {
            BURROW_S("urlNormalizer"),
            burrow__ht_url_normalizer,
            BURROW_S(
                "%00%01%02%03%04%05%06%07%08%09%0a%0b%0c%0d%0e%0f%10%11%12%13%14%15%16%"
                "17%18%19%1a%1b%1c%1d%1e%1f%20!%22#$%25&%27%28%29*+,-./0123456789:;%3c="
                "%3e?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[%5c]%5e_%60abcdefghijklmnopqrstuvwxyz%"
                "7b%7c%7d~%7f%c2%a0%c4%80%e2%80%a8%e2%80%a9%ef%bb%bf%f0%9d%84%9e"),
        },
    };

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NELEM(tests); i++) {
        const EscaperTest *test = &tests[i];
        Str s = call_str(test->escaper, a, input);
        if (!str_eq(s, test->escaped)) {
            testing_t_errorf_v(t, "%s: want\n\t%q\ngot\n\t%q", test->name,
                               test->escaped, s);
            continue;
        }
    }
    arena_free(&ar);
}

typedef struct SrcsetFilterTest {
    Str name;
    Str input;
    Str want;
} SrcsetFilterTest;

static void TestSrcsetFilter(TestingT *t) {
    static const SrcsetFilterTest tests[] = {
        {
            S_("one ok"),
            S_("http://example.com/img.png"),
            S_("http://example.com/img.png"),
        },
        {
            S_("one ok with metadata"),
            S_(" /img.png 200w"),
            S_(" /img.png 200w"),
        },
        {
            S_("one bad"),
            S_("javascript:alert(1) 200w"),
            S_("#ZgotmplZ"),
        },
        {
            S_("two ok"),
            S_("foo.png, bar.png"),
            S_("foo.png, bar.png"),
        },
        {
            S_("left bad"),
            S_("javascript:alert(1), /foo.png"),
            S_("#ZgotmplZ, /foo.png"),
        },
        {
            S_("right bad"),
            S_("/bogus#, javascript:alert(1)"),
            S_("/bogus#,#ZgotmplZ"),
        },
    };

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NELEM(tests); i++) {
        const SrcsetFilterTest *test = &tests[i];
        Str got = call_str(burrow__ht_srcset_filter_and_escaper, a, test->input);
        if (!str_eq(got, test->want))
            testing_t_errorf_v(t, "%s: srcsetFilterAndEscaper(%q) want %q != %q",
                               test->name, test->input, test->want, got);
    }
    arena_free(&ar);
}

static void BenchmarkURLEscaper(TestingB *b) {
    bench_str(b, burrow__ht_url_escaper,
              BURROW_S("http://example.com:80/foo?q=bar%20&baz=x+y#frag"));
}

static void BenchmarkURLEscaperNoSpecials(TestingB *b) {
    bench_str(b, burrow__ht_url_escaper,
              BURROW_S("TheQuickBrownFoxJumpsOverTheLazyDog."));
}

static void BenchmarkURLNormalizer(TestingB *b) {
    bench_str(b, burrow__ht_url_normalizer,
              BURROW_S("The quick brown fox jumps over the lazy dog.\n"));
}

static void BenchmarkURLNormalizerNoSpecials(TestingB *b) {
    bench_str(b, burrow__ht_url_normalizer,
              BURROW_S("http://example.com:80/foo?q=bar%20&baz=x+y#frag"));
}

static void BenchmarkSrcsetFilter(TestingB *b) {
    bench_str(b, burrow__ht_srcset_filter_and_escaper,
              BURROW_S(" /foo/bar.png 200w, /baz/boo(1).png"));
}

static void BenchmarkSrcsetFilterNoSpecials(TestingB *b) {
    bench_str(b, burrow__ht_srcset_filter_and_escaper,
              BURROW_S("http://example.com:80/foo?q=bar%20&baz=x+y#frag"));
}

#define TESTS(X)                                                                       \
    X(TestEndsWithCSSKeyword)                                                          \
    X(TestIsCSSNmchar)                                                                 \
    X(TestDecodeCSS)                                                                   \
    X(TestHexDecode)                                                                   \
    X(TestSkipCSSSpace)                                                                \
    X(TestCSSEscaper)                                                                  \
    X(TestCSSValueFilter)                                                              \
    X(BenchmarkCSSEscaper)                                                             \
    X(BenchmarkCSSEscaperNoSpecials)                                                   \
    X(BenchmarkDecodeCSS)                                                              \
    X(BenchmarkDecodeCSSNoSpecials)                                                    \
    X(BenchmarkCSSValueFilter)                                                         \
    X(BenchmarkCSSValueFilterOk)                                                       \
    X(TestHTMLNospaceEscaper)                                                          \
    X(TestStripTags)                                                                   \
    X(BenchmarkHTMLNospaceEscaper)                                                     \
    X(BenchmarkHTMLNospaceEscaperNoSpecials)                                           \
    X(BenchmarkStripTags)                                                              \
    X(BenchmarkStripTagsNoSpecials)                                                    \
    X(TestNextJsCtx)                                                                   \
    X(TestJSValEscaper)                                                                \
    X(TestJSStrEscaper)                                                                \
    X(TestJSRegexpEscaper)                                                             \
    X(TestEscapersOnLower7AndSelectHighCodepoints)                                     \
    X(TestIsJsMimeType)                                                                \
    X(BenchmarkJSValEscaperWithNum)                                                    \
    X(BenchmarkJSValEscaperWithStr)                                                    \
    X(BenchmarkJSValEscaperWithStrNoSpecials)                                          \
    X(BenchmarkJSValEscaperWithObj)                                                    \
    X(BenchmarkJSValEscaperWithObjNoSpecials)                                          \
    X(BenchmarkJSStrEscaperNoSpecials)                                                 \
    X(BenchmarkJSStrEscaper)                                                           \
    X(BenchmarkJSRegexpEscaperNoSpecials)                                              \
    X(BenchmarkJSRegexpEscaper)                                                        \
    X(TestFindEndTag)                                                                  \
    X(TestURLNormalizer)                                                               \
    X(TestURLFilters)                                                                  \
    X(TestSrcsetFilter)                                                                \
    X(BenchmarkURLEscaper)                                                             \
    X(BenchmarkURLEscaperNoSpecials)                                                   \
    X(BenchmarkURLNormalizer)                                                          \
    X(BenchmarkURLNormalizerNoSpecials)                                                \
    X(BenchmarkSrcsetFilter)                                                           \
    X(BenchmarkSrcsetFilterNoSpecials)

TESTING_MAIN(TESTS)

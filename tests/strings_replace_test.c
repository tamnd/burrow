/* Derived from Go's src/strings/replace_test.go and search_test.go.
 * Go source: go1.27.1.
 *
 * Go reaches the replacer's insides through export_test.go, and these tests
 * reach them through the burrow__strings_ hooks in src/strings/internal.h,
 * which do the same four jobs. A replacer keeps the strings it was given
 * rather than copies, so the one byte strings here are slices of a static
 * table of all 256 bytes. The two tests at the end are not from Go.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/strings.h"

#include "burrow/burrow.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/fixed.h"
#include "burrow/mem/heap.h"
#include "burrow/mem/track.h"
#include "burrow/panic.h"

#include "../src/strings/internal.h"
#include "check.h"

#include <stdint.h>
#include <string.h>

#define S BURROW_S
#define SI(lit) {(const Byte *)(lit), (Int)(sizeof(lit) - 1)}
#define LEN(a) ((Int)(sizeof(a) / sizeof((a)[0])))

static Byte all_bytes[256];

/* Go's str(b), a one byte string, which may be 0xff and so not UTF-8. */
static Str str1(Byte b) {
    Str s = {&all_bytes[b], 1};
    return s;
}

static void init_all_bytes(void) {
    for (int i = 0; i < 256; i++)
        all_bytes[i] = (Byte)i;
}

static StringsReplacer *replacer(Alloc *a, const Str *oldnew, Int n) {
    return strings_new_replacer(
        a, slice_from((void *)(uintptr_t)oldnew, n, n, TYPE_STRING));
}

static Str cat(Alloc *a, Str x, Str y) {
    Byte *p = (Byte *)mem_alloc(a, (size_t)(x.len + y.len) + 1, 1);
    memcpy(p, x.p, (size_t)x.len);
    memcpy(p + x.len, y.p, (size_t)y.len);
    Str s = {p, x.len + y.len};
    return s;
}

static const Str html_escaper_pairs[] = {
    SI("&"),    SI("&amp;"), SI("<"),      SI("&lt;"), SI(">"),
    SI("&gt;"), SI("\""),    SI("&quot;"), SI("'"),    SI("&apos;"),
};

static const Str html_unescaper_pairs[] = {
    SI("&amp;"), SI("&"),      SI("&lt;"), SI("<"),      SI("&gt;"),
    SI(">"),     SI("&quot;"), SI("\""),   SI("&apos;"), SI("'"),
};

static const Str capital_letters_pairs[] = {SI("a"), SI("A"), SI("b"), SI("B")};

typedef struct TestCase {
    StringsReplacer *r;
    Str in, out;
} TestCase;

typedef struct Cases {
    TestCase c[128];
    Int n;
} Cases;

static void add(Cases *cs, StringsReplacer *r, Str in, Str out) {
    TestCase tc = {r, in, out};
    cs->c[cs->n++] = tc;
}

#define R(a, ...)                                                                      \
    replacer((a), (const Str[]){__VA_ARGS__},                                          \
             (Int)(sizeof((const Str[]){__VA_ARGS__}) / sizeof(Str)))

/* TestReplacer tests the replacer implementations. */
static void TestReplacer(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    init_all_bytes();
    static Cases cases;
    Cases *cs = &cases;
    cs->n = 0;

    StringsReplacer *capital_letters = replacer(a, capital_letters_pairs, 4);
    StringsReplacer *html_escaper = replacer(a, html_escaper_pairs, 10);
    StringsReplacer *html_unescaper = replacer(a, html_unescaper_pairs, 10);

    /* inc maps "\x00"->"\x01", ..., "a"->"b", "b"->"c", ..., "\xff"->"\x00". */
    Str *s = (Str *)mem_alloc(a, 512 * sizeof(Str), _Alignof(Str));
    for (Int i = 0; i < 256; i++) {
        s[2 * i] = str1((Byte)i);
        s[2 * i + 1] = str1((Byte)(i + 1));
    }
    StringsReplacer *inc = replacer(a, s, 512);

    /* Test cases with 1-byte old strings, 1-byte new strings. */
    Int big = (32 << 10) + 123;
    add(cs, capital_letters, S("brad"), S("BrAd"));
    add(cs, capital_letters, strings_repeat(a, S("a"), big),
        strings_repeat(a, S("A"), big));
    add(cs, capital_letters, S(""), S(""));

    add(cs, inc, S("brad"), S("csbe"));
    add(cs, inc, S("\x00\xff"), S("\x01\x00"));
    add(cs, inc, S(""), S(""));

    add(cs, R(a, S("a"), S("1"), S("a"), S("2")), S("brad"), S("br1d"));

    /* repeat maps "a"->"a", "b"->"bb", "c"->"ccc", ... */
    s = (Str *)mem_alloc(a, 512 * sizeof(Str), _Alignof(Str));
    for (Int i = 0; i < 256; i++) {
        Int n = i + 1 - 'a';
        if (n < 1)
            n = 1;
        s[2 * i] = str1((Byte)i);
        s[2 * i + 1] = strings_repeat(a, str1((Byte)i), n);
    }
    StringsReplacer *repeat = replacer(a, s, 512);

    /* Test cases with 1-byte old strings, variable length new strings. */
    add(cs, html_escaper, S("No changes"), S("No changes"));
    add(cs, html_escaper, S("I <3 escaping & stuff"),
        S("I &lt;3 escaping &amp; stuff"));
    add(cs, html_escaper, S("&&&"), S("&amp;&amp;&amp;"));
    add(cs, html_escaper, S(""), S(""));

    add(cs, repeat, S("brad"), S("bbrrrrrrrrrrrrrrrrrradddd"));
    add(cs, repeat, S("abba"), S("abbbba"));
    add(cs, repeat, S(""), S(""));

    add(cs, R(a, S("a"), S("11"), S("a"), S("22")), S("brad"), S("br11d"));

    /* The remaining test cases have variable length old strings. */

    add(cs, html_unescaper, S("&amp;amp;"), S("&amp;"));
    add(cs, html_unescaper, S("&lt;b&gt;HTML&apos;s neat&lt;/b&gt;"),
        S("<b>HTML's neat</b>"));
    add(cs, html_unescaper, S(""), S(""));

    add(cs, R(a, S("a"), S("1"), S("a"), S("2"), S("xxx"), S("xxx")), S("brad"),
        S("br1d"));

    add(cs, R(a, S("a"), S("1"), S("aa"), S("2"), S("aaa"), S("3")), S("aaaa"),
        S("1111"));

    add(cs, R(a, S("aaa"), S("3"), S("aa"), S("2"), S("a"), S("1")), S("aaaa"),
        S("31"));

    /* gen1 has multiple old strings of variable length. There is no overall
     * non-empty common prefix, but some pairwise common prefixes. */
    StringsReplacer *gen1 =
        R(a, S("aaa"), S("3[aaa]"), S("aa"), S("2[aa]"), S("a"), S("1[a]"), S("i"),
          S("i"), S("longerst"), S("most long"), S("longer"), S("medium"), S("long"),
          S("short"), S("xx"), S("xx"), S("x"), S("X"), S("X"), S("Y"), S("Y"), S("Z"));
    add(cs, gen1, S("fooaaabar"), S("foo3[aaa]b1[a]r"));
    add(cs, gen1, S("long, longerst, longer"), S("short, most long, medium"));
    add(cs, gen1, S("xxxxx"), S("xxxxX"));
    add(cs, gen1, S("XiX"), S("YiY"));
    add(cs, gen1, S(""), S(""));

    /* gen2 has multiple old strings with no pairwise common prefix. */
    StringsReplacer *gen2 =
        R(a, S("roses"), S("red"), S("violets"), S("blue"), S("sugar"), S("sweet"));
    add(cs, gen2, S("roses are red, violets are blue..."),
        S("red are red, blue are blue..."));
    add(cs, gen2, S(""), S(""));

    /* gen3 has multiple old strings with an overall common prefix. */
    StringsReplacer *gen3 = R(a, S("abracadabra"), S("poof"), S("abracadabrakazam"),
                              S("splat"), S("abraham"), S("lincoln"), S("abrasion"),
                              S("scrape"), S("abraham"), S("isaac"));
    add(cs, gen3, S("abracadabrakazam abraham"), S("poofkazam lincoln"));
    add(cs, gen3, S("abrasion abracad"), S("scrape abracad"));
    add(cs, gen3, S("abba abram abrasive"), S("abba abram abrasive"));
    add(cs, gen3, S(""), S(""));

    /* foo{1,2,3,4} have multiple old strings with an overall common prefix and
     * 1- or 2- byte extensions from the common prefix. */
    StringsReplacer *foo1 =
        R(a, S("foo1"), S("A"), S("foo2"), S("B"), S("foo3"), S("C"));
    StringsReplacer *foo2 = R(a, S("foo1"), S("A"), S("foo2"), S("B"), S("foo31"),
                              S("C"), S("foo32"), S("D"));
    StringsReplacer *foo3 = R(a, S("foo11"), S("A"), S("foo12"), S("B"), S("foo31"),
                              S("C"), S("foo32"), S("D"));
    StringsReplacer *foo4 = R(a, S("foo12"), S("B"), S("foo32"), S("D"));
    add(cs, foo1, S("fofoofoo12foo32oo"), S("fofooA2C2oo"));
    add(cs, foo1, S(""), S(""));

    add(cs, foo2, S("fofoofoo12foo32oo"), S("fofooA2Doo"));
    add(cs, foo2, S(""), S(""));

    add(cs, foo3, S("fofoofoo12foo32oo"), S("fofooBDoo"));
    add(cs, foo3, S(""), S(""));

    add(cs, foo4, S("fofoofoo12foo32oo"), S("fofooBDoo"));
    add(cs, foo4, S(""), S(""));

    /* genAll maps "\x00\x01\x02...\xfe\xff" to "[all]", amongst other things. */
    Str all_string = {all_bytes, 256};
    StringsReplacer *gen_all =
        R(a, all_string, S("[all]"), S("\xff"), S("[ff]"), str1(0), S("[00]"));
    add(cs, gen_all, all_string, S("[all]"));
    add(cs, gen_all, cat(a, cat(a, S("a\xff"), all_string), str1(0)),
        S("a[ff][all][00]"));
    add(cs, gen_all, S(""), S(""));

    /* Test cases with empty old strings. */

    StringsReplacer *blank_to_x1 = R(a, S(""), S("X"));
    StringsReplacer *blank_to_x2 = R(a, S(""), S("X"), S(""), S(""));
    StringsReplacer *blank_high_priority = R(a, S(""), S("X"), S("o"), S("O"));
    StringsReplacer *blank_low_priority = R(a, S("o"), S("O"), S(""), S("X"));
    StringsReplacer *blank_no_op1 = R(a, S(""), S(""));
    StringsReplacer *blank_no_op2 = R(a, S(""), S(""), S(""), S("A"));
    StringsReplacer *blank_foo =
        R(a, S(""), S("X"), S("foobar"), S("R"), S("foobaz"), S("Z"));
    add(cs, blank_to_x1, S("foo"), S("XfXoXoX"));
    add(cs, blank_to_x1, S(""), S("X"));

    add(cs, blank_to_x2, S("foo"), S("XfXoXoX"));
    add(cs, blank_to_x2, S(""), S("X"));

    add(cs, blank_high_priority, S("oo"), S("XOXOX"));
    add(cs, blank_high_priority, S("ii"), S("XiXiX"));
    add(cs, blank_high_priority, S("oiio"), S("XOXiXiXOX"));
    add(cs, blank_high_priority, S("iooi"), S("XiXOXOXiX"));
    add(cs, blank_high_priority, S(""), S("X"));

    add(cs, blank_low_priority, S("oo"), S("OOX"));
    add(cs, blank_low_priority, S("ii"), S("XiXiX"));
    add(cs, blank_low_priority, S("oiio"), S("OXiXiOX"));
    add(cs, blank_low_priority, S("iooi"), S("XiOOXiX"));
    add(cs, blank_low_priority, S(""), S("X"));

    add(cs, blank_no_op1, S("foo"), S("foo"));
    add(cs, blank_no_op1, S(""), S(""));

    add(cs, blank_no_op2, S("foo"), S("foo"));
    add(cs, blank_no_op2, S(""), S(""));

    add(cs, blank_foo, S("foobarfoobaz"), S("XRXZX"));
    add(cs, blank_foo, S("foobar-foobaz"), S("XRX-XZX"));
    add(cs, blank_foo, S(""), S("X"));

    /* single string replacer */

    StringsReplacer *abc_matcher = R(a, S("abc"), S("[match]"));

    add(cs, abc_matcher, S(""), S(""));
    add(cs, abc_matcher, S("ab"), S("ab"));
    add(cs, abc_matcher, S("abc"), S("[match]"));
    add(cs, abc_matcher, S("abcd"), S("[match]d"));
    add(cs, abc_matcher, S("cabcabcdabca"), S("c[match][match]d[match]a"));

    /* Issue 6659 cases (more single string replacer) */

    StringsReplacer *no_hello = R(a, S("Hello"), S(""));
    add(cs, no_hello, S("Hello"), S(""));
    add(cs, no_hello, S("Hellox"), S("x"));
    add(cs, no_hello, S("xHello"), S("x"));
    add(cs, no_hello, S("xHellox"), S("xx"));

    /* No-arg test cases. */

    StringsReplacer *nop = strings_new_replacer(a, slice_nil(TYPE_STRING));
    add(cs, nop, S("abc"), S("abc"));
    add(cs, nop, S(""), S(""));

    /* Run the test cases. */

    for (Int i = 0; i < cs->n; i++) {
        TestCase tc = cs->c[i];
        Str got = strings_replacer_replace(tc.r, a, tc.in);
        if (!str_eq(got, tc.out))
            testing_t_errorf_v(t, "%d. Replace(%q) = %q, want %q", i, tc.in, got,
                               tc.out);
        StringsBuilder buf = STRINGS_BUILDER(a);
        Error err;
        Int n = strings_replacer_write_string(tc.r, strings_builder_as_io_writer(&buf),
                                              tc.in, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%d. WriteString: %v", i, err);
            continue;
        }
        got = strings_builder_string(&buf);
        if (!str_eq(got, tc.out)) {
            testing_t_errorf_v(t, "%d. WriteString(%q) wrote %q, want %q", i, tc.in,
                               got, tc.out);
            continue;
        }
        if (n != tc.out.len)
            testing_t_errorf_v(
                t,
                "%d. WriteString(%q) wrote correct string but reported %d "
                "bytes; want %d (%q)",
                i, tc.in, n, tc.out.len, tc.out);
    }
    arena_free(&ar);
}

typedef struct AlgorithmCase {
    StringsReplacer *r;
    const char *want;
} AlgorithmCase;

static void algorithm_test_cases(Alloc *a, AlgorithmCase c[6]) {
    c[0].r = replacer(a, capital_letters_pairs, 4);
    c[0].want = "*strings.byteReplacer";
    c[1].r = replacer(a, html_escaper_pairs, 10);
    c[1].want = "*strings.byteStringReplacer";
    c[2].r = R(a, S("12"), S("123"));
    c[2].want = "*strings.singleStringReplacer";
    c[3].r = R(a, S("1"), S("12"));
    c[3].want = "*strings.byteStringReplacer";
    c[4].r = R(a, S(""), S("X"));
    c[4].want = "*strings.genericReplacer";
    c[5].r = R(a, S("a"), S("1"), S("b"), S("12"), S("cde"), S("123"));
    c[5].want = "*strings.genericReplacer";
}

/* TestPickAlgorithm tests that NewReplacer picks the correct algorithm. */
static void TestPickAlgorithm(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    AlgorithmCase c[6];
    algorithm_test_cases(arena_allocator(&ar), c);
    for (Int i = 0; i < LEN(c); i++) {
        const char *got = burrow__strings_replacer_kind(c[i].r);
        if (got == NULL || strcmp(got, c[i].want) != 0)
            testing_t_errorf_v(t, "%d. algorithm = %s, want %s", i,
                               str_from_cstr(got != NULL ? got : "<nil>"),
                               str_from_cstr(c[i].want));
    }
    arena_free(&ar);
}

static const Str unwritable_text = SI("unwritable");
static const Error unwritable = {&burrow_sentinel_error_vt, &unwritable_text};

static Int err_write(void *self, Slice p, Error *err) {
    (void)self;
    (void)p;
    BURROW_OUT(err, unwritable);
    return 0;
}

static const IoWriterVT err_writer_vt = {NULL, err_write};

/* TestWriteStringError tests that WriteString returns an error received from
 * the underlying io.Writer. */
static void TestWriteStringError(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    AlgorithmCase c[6];
    algorithm_test_cases(arena_allocator(&ar), c);
    IoWriter w = {&err_writer_vt, NULL};
    for (Int i = 0; i < LEN(c); i++) {
        Error err = BURROW_NO_ERROR;
        Int n = strings_replacer_write_string(c[i].r, w, S("abc"), &err);
        if (n != 0 || BURROW_OK(err) || !str_eq(error_text(err), S("unwritable")))
            testing_t_errorf_v(t, "%d. WriteStringError = %d, %v, want 0, unwritable",
                               i, n, err);
    }
    arena_free(&ar);
}

/* TestGenericTrieBuilding verifies the structure of the generated trie. There
 * is one node per line, and the key ending with the current line is in the
 * trie if it ends with a "+". */
static void TestGenericTrieBuilding(TestingT *t) {
    static const struct {
        Str in, out;
    } test_cases[] = {
        {SI("abc;abdef;abdefgh;xx;xy;z"), SI("-\n"
                                             "\t\t\ta-\n"
                                             "\t\t\t.b-\n"
                                             "\t\t\t..c+\n"
                                             "\t\t\t..d-\n"
                                             "\t\t\t...ef+\n"
                                             "\t\t\t.....gh+\n"
                                             "\t\t\tx-\n"
                                             "\t\t\t.x+\n"
                                             "\t\t\t.y+\n"
                                             "\t\t\tz+\n"
                                             "\t\t\t")},
        {SI("abracadabra;abracadabrakazam;abraham;abrasion"),
         SI("-\n"
            "\t\t\ta-\n"
            "\t\t\t.bra-\n"
            "\t\t\t....c-\n"
            "\t\t\t.....adabra+\n"
            "\t\t\t...........kazam+\n"
            "\t\t\t....h-\n"
            "\t\t\t.....am+\n"
            "\t\t\t....s-\n"
            "\t\t\t.....ion+\n"
            "\t\t\t")},
        {SI("aaa;aa;a;i;longerst;longer;long;xx;x;X;Y"), SI("-\n"
                                                            "\t\t\tX+\n"
                                                            "\t\t\tY+\n"
                                                            "\t\t\ta+\n"
                                                            "\t\t\t.a+\n"
                                                            "\t\t\t..a+\n"
                                                            "\t\t\ti+\n"
                                                            "\t\t\tl-\n"
                                                            "\t\t\t.ong+\n"
                                                            "\t\t\t....er+\n"
                                                            "\t\t\t......st+\n"
                                                            "\t\t\tx+\n"
                                                            "\t\t\t.x+\n"
                                                            "\t\t\t")},
        {SI("foo;;foo;foo1"), SI("+\n"
                                 "\t\t\tf-\n"
                                 "\t\t\t.oo+\n"
                                 "\t\t\t...1+\n"
                                 "\t\t\t")},
    };

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int k = 0; k < LEN(test_cases); k++) {
        Str in = test_cases[k].in, out = test_cases[k].out;
        Slice keys = strings_split(a, in, S(";"));
        Str *args =
            (Str *)mem_alloc(a, (size_t)keys.len * 2 * sizeof(Str), _Alignof(Str));
        for (Int i = 0; i < keys.len; i++)
            args[i * 2] = BURROW_AT(Str, keys, i);

        Str got = burrow__strings_print_trie(replacer(a, args, keys.len * 2), a);
        /* Remove tabs from tc.out */
        Byte *wantbuf = (Byte *)mem_alloc(a, (size_t)out.len + 1, 1);
        Int n = 0;
        for (Int i = 0; i < out.len; i++) {
            if (out.p[i] != '\t')
                wantbuf[n++] = out.p[i];
        }
        Str want = {wantbuf, n};

        if (!str_eq(got, want))
            testing_t_errorf_v(t, "PrintTrie(%q)\ngot\n%swant\n%s", in, got, want);
    }
    arena_free(&ar);
}

static void TestFinderNext(TestingT *t) {
    static const struct {
        Str pat, text;
        Int index;
    } test_cases[] = {
        {SI(""), SI(""), 0},
        {SI(""), SI("abc"), 0},
        {SI("abc"), SI(""), -1},
        {SI("abc"), SI("abc"), 0},
        {SI("d"), SI("abcdefg"), 3},
        {SI("nan"), SI("banana"), 2},
        {SI("pan"), SI("anpanman"), 2},
        {SI("nnaaman"), SI("anpanmanam"), -1},
        {SI("abcd"), SI("abc"), -1},
        {SI("abcd"), SI("bcd"), -1},
        {SI("bcd"), SI("abcd"), 1},
        {SI("abc"), SI("acca"), -1},
        {SI("aa"), SI("aaa"), 0},
        {SI("baa"), SI("aaaaa"), -1},
        {SI("at that"), SI("which finally halts.  at that point"), 22},
    };

    Arena ar;
    arena_init(&ar, NULL, 0);
    for (Int i = 0; i < LEN(test_cases); i++) {
        Int got = burrow__strings_string_find(arena_allocator(&ar), test_cases[i].pat,
                                              test_cases[i].text);
        Int want = test_cases[i].index;
        if (got != want)
            testing_t_errorf_v(t, "stringFind(%q, %q) got %d, want %d\n",
                               test_cases[i].pat, test_cases[i].text, got, want);
    }
    arena_free(&ar);
}

static void TestFinderCreation(TestingT *t) {
    static const struct {
        Str pattern;
        Int bad[256];
        Int suf[9];
    } test_cases[] = {
        {
            SI("abc"),
            {['a'] = 2, ['b'] = 1, ['c'] = 3},
            {5, 4, 1},
        },
        {
            SI("mississi"),
            {['i'] = 3, ['m'] = 7, ['s'] = 1},
            {15, 14, 13, 7, 11, 10, 7, 1},
        },
        /* From https://www.cs.utexas.edu/~moore/publications/fstrpos.pdf */
        {
            SI("abcxxxabc"),
            {['a'] = 2, ['b'] = 1, ['c'] = 6, ['x'] = 3},
            {14, 13, 12, 11, 10, 9, 11, 10, 1},
        },
        {
            SI("abyxcdeyx"),
            {['a'] = 8,
             ['b'] = 7,
             ['c'] = 4,
             ['d'] = 3,
             ['e'] = 2,
             ['y'] = 1,
             ['x'] = 5},
            {17, 16, 15, 14, 13, 12, 7, 10, 1},
        },
    };

    Arena ar;
    arena_init(&ar, NULL, 0);
    for (Int k = 0; k < LEN(test_cases); k++) {
        Str pattern = test_cases[k].pattern;
        Int bad[256];
        Int *good = NULL;
        if (!burrow__strings_dump_tables(arena_allocator(&ar), pattern, bad, &good)) {
            testing_t_fatalf_v(t, "DumpTables(%q): out of memory", pattern);
            return;
        }

        for (Int i = 0; i < 256; i++) {
            Int got = bad[i];
            Int want = test_cases[k].bad[i];
            if (want == 0)
                want = pattern.len;
            if (got != want)
                testing_t_errorf_v(t, "boyerMoore(%q) bad['%c']: got %d want %d",
                                   pattern, i, got, want);
        }

        Slice got = slice_from(good, pattern.len, pattern.len, TYPE_INT);
        Slice want = slice_from((void *)(uintptr_t)test_cases[k].suf, pattern.len,
                                pattern.len, TYPE_INT);
        if (memcmp(good, test_cases[k].suf, (size_t)pattern.len * sizeof(Int)) != 0)
            testing_t_errorf_v(t, "boyerMoore(%q) got %v want %v", pattern, got, want);
    }
    arena_free(&ar);
}

/* Not from Go. strings_replacer_free has to give back every byte the replacer
 * took, whichever algorithm it picked and whether or not it was ever used. */
static void TestReplacerFree(TestingT *t) {
    Track tr;
    track_init(&tr, heap_allocator());
    Alloc *a = track_allocator(&tr);
    AlgorithmCase c[6];
    algorithm_test_cases(a, c);
    Arena ar;
    arena_init(&ar, NULL, 0);
    for (Int i = 0; i < LEN(c); i++) {
        strings_replacer_replace(c[i].r, arena_allocator(&ar), S("a12cde<"));
        strings_replacer_free(c[i].r);
    }
    arena_free(&ar);
    strings_replacer_free(R(a, S("x"), S("y")));
    strings_replacer_free(NULL);
    if (track_live(&tr) != 0)
        testing_t_errorf_v(t, "%d bytes still live after strings_replacer_free",
                           (Int)track_live(&tr));
    track_free(&tr);
}

/* Not from Go. A replacer that cannot build its tables gives the empty string
 * from replace and burrow_err_out_of_memory from write_string, and keeps doing
 * so. */
static void TestReplacerOutOfMemory(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *good = arena_allocator(&ar);
    static Byte buf[1 << 16];
    Fixed none;
    fixed_init(&none, buf, 0);
    if (replacer(fixed_allocator(&none), capital_letters_pairs, 4) != NULL)
        testing_t_errorf_v(t, "NewReplacer with no memory did not give NULL");

    /* The smallest block the replacer and its list fit in, which leaves
     * nothing for the trie. */
    StringsReplacer *r = NULL;
    Fixed fx;
    for (size_t size = 0; r == NULL && size <= sizeof buf; size += 8) {
        fixed_init(&fx, buf, size);
        r = R(fixed_allocator(&fx), S("abracadabra"), S("poof"), S("abraham"),
              S("lincoln"), S("abrasion"), S("scrape"), S(""), S("X"));
    }
    if (r == NULL) {
        testing_t_fatalf_v(t, "NewReplacer in %d bytes gave NULL", (Int)sizeof buf);
        return;
    }
    for (int round = 0; round < 2; round++) {
        Str got = strings_replacer_replace(r, good, S("abraham"));
        if (got.len != 0)
            testing_t_errorf_v(t, "Replace after a failed build = %q, want \"\"", got);
        StringsBuilder b = STRINGS_BUILDER(good);
        Error err = BURROW_NO_ERROR;
        Int n = strings_replacer_write_string(r, strings_builder_as_io_writer(&b),
                                              S("abraham"), &err);
        if (n != 0 || !errors_is(err, burrow_err_out_of_memory))
            testing_t_errorf_v(t,
                               "WriteString after a failed build = %d, %v, want 0, %v",
                               n, err, burrow_err_out_of_memory);
    }
    if (burrow__strings_replacer_kind(r) != NULL)
        testing_t_errorf_v(t, "a failed build still reports an algorithm");
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestReplacer)                                                                    \
    X(TestPickAlgorithm)                                                               \
    X(TestWriteStringError)                                                            \
    X(TestGenericTrieBuilding)                                                         \
    X(TestFinderNext)                                                                  \
    X(TestFinderCreation)                                                              \
    X(TestReplacerFree)                                                                \
    X(TestReplacerOutOfMemory)

TESTING_MAIN(TESTS)

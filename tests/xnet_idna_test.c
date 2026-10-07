/* Derived from golang.org/x/net's idna tests, idna_test.go, and from
 * golang.org/x/text v0.37.0's internal/export/idna tests, idna_test.go,
 * punycode_test.go, conformancev2_test.go and example_test.go. x/net's package
 * is generated from the x/text one, and those are the versions Go 1.27.1
 * vendors.
 *
 * The Punycode cases, IdnaTestV2.txt and the hashes come from
 * tests/xnet_idna_test_gen.h, which tools/gen-xnet-idna.sh writes from the Go
 * package itself. TestDifferential runs a corpus of names through thirteen
 * profiles and TestSweep runs every rune through three, and both compare a
 * hash of the records with what Go gives. describe and sweep_record below have
 * to stay in step with the generator. Set BURROW_XNET_SHOW to see the first
 * record that differs.
 *
 * Go's TestAllocToUnicode and TestAllocToASCII count allocations, and here
 * that is the allocator's count. Go's TestProfiles compares the printed
 * profiles, which here is a compare of the fields. Its examples are tests that
 * build the lines the examples print. TestProfileString and TestOutOfMemory
 * are burrow's own.
 *
 * Copyright 2012 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/xnet/idna.h"

#include "burrow/burrow.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct IdnaLit {
    const char *s;
    Int len;
} IdnaLit;

typedef struct IdnaPunycodeCase {
    const char *s;
    Int s_len;
    const char *enc;
    Int enc_len;
} IdnaPunycodeCase;

typedef struct IdnaConformance {
    IdnaLit src;
    IdnaLit to_unicode, to_unicode_err;
    IdnaLit to_ascii_n, to_ascii_n_err;
    IdnaLit to_ascii_t, to_ascii_t_err;
    int go_ok;
} IdnaConformance;

#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Woverlength-strings"
#endif
#include "xnet_idna_test_gen.h"

#define LEN(x) ((Int)(sizeof(x) / sizeof((x)[0])))
#define L(s) str_from_bytes((s), (Int)sizeof(s) - 1)

#define CHECK_TEXT(got, want)                                                          \
    do {                                                                               \
        Str g_ = (got), w_ = (want);                                                   \
        if (!str_eq(g_, w_))                                                           \
            testing_t_errorf_v(t, "got %q, want %q", g_, w_);                          \
    } while (0)

static Str lit(IdnaLit l) {
    return str_from_bytes(l.s, l.len);
}

static bool contains(Str s, Str sub) {
    for (Int i = 0; i + sub.len <= s.len; i++)
        if (sub.len == 0 || memcmp(s.p + i, sub.p, (size_t)sub.len) == 0)
            return true;
    return false;
}

/* Go's strings.Trim(s, "[]"). */
static Str trim_brackets(Str s) {
    while (s.len > 0 && (s.p[0] == '[' || s.p[0] == ']'))
        s = str_from_bytes(s.p + 1, s.len - 1);
    while (s.len > 0 && (s.p[s.len - 1] == '[' || s.p[s.len - 1] == ']'))
        s.len--;
    return s;
}

/* A conversion: a profile, and which way. */
typedef struct Conv {
    const char *name;
    IdnaProfile p;
    bool to_ascii;
} Conv;

static Str convert(Alloc *a, const Conv *k, Str s, Error *err) {
    if (k->to_ascii)
        return burrow__idna_profile_to_ascii(a, &k->p, s, err);
    return burrow__idna_profile_to_unicode(a, &k->p, s, err);
}

/* doTest: whether k(input) gives want, when want is not empty, and an error
 * whose code is in errors. Go runs each as a subtest. */
static bool do_test(TestingT *t, Alloc *a, const Conv *k, Str input, Str want,
                    Str errors) {
    bool ok = true;
    errors = trim_brackets(errors);
    Error err;
    Str got = convert(a, k, input, &err);
    if (BURROW_FAILED(err)) {
        Str code = burrow__idna_error_code(err);
        if (!contains(errors, code)) {
            testing_t_errorf_v(t, "%s %+q: error %q not in set of expected errors {%s}",
                               k->name, input, code, errors);
            ok = false;
        }
    } else if (errors.len != 0) {
        testing_t_errorf_v(t, "%s %+q: got %+q, no errors; want error in {%s}", k->name,
                           input, got, errors);
        ok = false;
    }
    if (want.len != 0 && !str_eq(got, want)) {
        testing_t_errorf_v(t, "%s: input=%+q string: got %+q; want %+q", k->name, input,
                           got, want);
        ok = false;
    }
    return ok;
}

static Alloc *test_alloc(Arena *ar) {
    arena_init(ar, heap_allocator(), 0);
    return arena_allocator(ar);
}

/* ------------------------------------------------------------- allocations */

static void TestAllocToUnicode(TestingT *t) {
    Arena ar;
    Alloc *a = test_alloc(&ar);
    for (int i = 0; i < 1000; i++) {
        Error err;
        Str in = L("www.golang.org");
        Str got = burrow__idna_to_unicode(a, in, &err);
        if (got.p != in.p || BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "ToUnicode(%q) = %q, %v; want the input", in, got,
                               err);
            break;
        }
    }
    AllocStats st = mem_stats(a);
    if (st.allocs != 0 || st.blocks != 0)
        testing_t_errorf_v(t, "got %d allocations; want 0", (Int)st.allocs);
    arena_free(&ar);
}

static void TestAllocToASCII(TestingT *t) {
    Arena ar;
    Alloc *a = test_alloc(&ar);
    for (int i = 0; i < 1000; i++) {
        Error err;
        Str in = L("www.golang.org");
        Str got = burrow__idna_to_ascii(a, in, &err);
        if (got.p != in.p || BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "ToASCII(%q) = %q, %v; want the input", in, got, err);
            break;
        }
    }
    AllocStats st = mem_stats(a);
    if (st.allocs != 0 || st.blocks != 0)
        testing_t_errorf_v(t, "got %d allocations; want 0", (Int)st.allocs);
    arena_free(&ar);
}

/* ---------------------------------------------------------------- profiles */

static bool profile_eq(const IdnaProfile *x, const IdnaProfile *y) {
    return x->transitional == y->transitional &&
           x->use_std3_rules == y->use_std3_rules &&
           x->check_hyphens == y->check_hyphens &&
           x->check_joiners == y->check_joiners &&
           x->verify_dns_length == y->verify_dns_length &&
           x->remove_leading_dots == y->remove_leading_dots && x->trie == y->trie &&
           x->from_puny == y->from_puny && x->mapping == y->mapping &&
           x->bidirule == y->bidirule;
}

static void TestProfiles(TestingT *t) {
    const IdnaOption reg[] = {burrow__idna_validate_for_registration()};
    const IdnaOption reg2[] = {
        burrow__idna_validate_for_registration(),
        burrow__idna_verify_dns_length(true),
        burrow__idna_bidi_rule(),
    };
    const IdnaOption lookup[] = {
        burrow__idna_map_for_lookup(),
        burrow__idna_bidi_rule(),
        burrow__idna_transitional(false),
    };
    const IdnaOption display[] = {burrow__idna_map_for_lookup(),
                                  burrow__idna_bidi_rule()};
    const struct {
        const char *name;
        const IdnaProfile *want;
        IdnaProfile got;
    } cases[] = {
        {"Punycode", &burrow__idna_punycode, burrow__idna_new(NULL, 0)},
        {"Registration", &burrow__idna_registration, burrow__idna_new(reg, LEN(reg))},
        {"Registration", &burrow__idna_registration, burrow__idna_new(reg2, LEN(reg2))},
        {"Lookup", &burrow__idna_lookup, burrow__idna_new(lookup, LEN(lookup))},
        {"Display", &burrow__idna_display, burrow__idna_new(display, LEN(display))},
    };
    for (Int i = 0; i < LEN(cases); i++)
        if (!profile_eq(&cases[i].got, cases[i].want))
            testing_t_errorf_v(t, "%s: profiles differ", cases[i].name);
}

static void TestProfileString(TestingT *t) {
    Arena ar;
    Alloc *a = test_alloc(&ar);
    const IdnaOption tr[] = {burrow__idna_transitional(true)};
    const struct {
        IdnaProfile p;
        const char *want;
    } cases[] = {
        {burrow__idna_punycode, "NonTransitional"},
        {burrow__idna_lookup, "NonTransitional:UseSTD3Rules:CheckHyphens:CheckJoiners"},
        {burrow__idna_display,
         "NonTransitional:UseSTD3Rules:CheckHyphens:CheckJoiners"},
        {burrow__idna_registration,
         "NonTransitional:UseSTD3Rules:CheckHyphens:CheckJoiners:VerifyDNSLength"},
        {burrow__idna_new(tr, LEN(tr)), "Transitional"},
    };
    for (Int i = 0; i < LEN(cases); i++) {
        Str got = burrow__idna_profile_string(a, &cases[i].p);
        CHECK_TEXT(got, str_from_cstr(cases[i].want));
    }
    arena_free(&ar);
}

/* -------------------------------------------------------------------- IDNA */

static void TestIDNA(TestingT *t) {
    static const struct {
        const char *ascii, *unicode;
    } cases[] = {
        /* Labels. */
        {"books", "books"},
        {"xn--bcher-kva", "b\xC3\xBC"
                          "cher"},

        /* Domains. */
        {"foo--xn--bar.org", "foo--xn--bar.org"},
        {"golang.org", "golang.org"},
        {"example.xn--p1ai", "example.\xD1\x80\xD1\x84"},
        {"xn--czrw28b.tw", "\xE5\x95\x86\xE6\xA5\xAD.tw"},
        {"www.xn--mller-kva.de", "www.m\xC3\xBCller.de"},
    };
    Arena ar;
    Alloc *a = test_alloc(&ar);
    for (Int i = 0; i < LEN(cases); i++) {
        Str ascii = str_from_cstr(cases[i].ascii);
        Str unicode = str_from_cstr(cases[i].unicode);
        Error err;
        Str got = burrow__idna_to_ascii(a, unicode, &err);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "ToASCII(%q): %v", unicode, err);
        else if (!str_eq(got, ascii))
            testing_t_errorf_v(t, "ToASCII(%q): got %q, want %q", unicode, got, ascii);

        got = burrow__idna_to_unicode(a, ascii, &err);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "ToUnicode(%q): %v", ascii, err);
        else if (!str_eq(got, unicode))
            testing_t_errorf_v(t, "ToUnicode(%q): got %q, want %q", ascii, got,
                               unicode);
    }
    arena_free(&ar);
}

typedef struct SepCase {
    const char *unicode;
    const char *want_ascii;
    bool want_err;
} SepCase;

typedef struct SepTest {
    const char *name;
    const IdnaProfile *profile;
    SepCase sub[3];
} SepTest;

#define EXAMPLE_JP "example\xE3\x80\x82jp"
#define TOKYO_JP "\xE6\x9D\xB1\xE4\xBA\xAC\xEF\xBC\x8Ejp"
#define OSAKA_JP "\xE5\xA4\xA7\xE9\x98\xAA\xEF\xBD\xA1jp"

static const SepTest sep_tests[] = {
    {"Punycode",
     &burrow__idna_punycode,
     {
         {EXAMPLE_JP, "xn--examplejp-ck3h", false},
         {TOKYO_JP, "xn--jp-l92cn98g071o", false},
         {OSAKA_JP, "xn--jp-ku9cz72u463f", false},
     }},
    {"Lookup",
     &burrow__idna_lookup,
     {
         {EXAMPLE_JP, "example.jp", false},
         {TOKYO_JP, "xn--1lqs71d.jp", false},
         {OSAKA_JP, "xn--pssu33l.jp", false},
     }},
    {"Display",
     &burrow__idna_display,
     {
         {EXAMPLE_JP, "example.jp", false},
         {TOKYO_JP, "xn--1lqs71d.jp", false},
         {OSAKA_JP, "xn--pssu33l.jp", false},
     }},
    {"Registration",
     &burrow__idna_registration,
     {
         {EXAMPLE_JP, "", true},
         {TOKYO_JP, "", true},
         {OSAKA_JP, "", true},
     }},
};

static void separators(void *env, TestingT *t) {
    const SepTest *tc = env;
    Arena ar;
    Alloc *a = test_alloc(&ar);
    for (Int i = 0; i < LEN(tc->sub); i++) {
        const SepCase *c = &tc->sub[i];
        Str in = str_from_cstr(c->unicode);
        Error err;
        Str got = burrow__idna_profile_to_ascii(a, tc->profile, in, &err);
        if (c->want_err) {
            if (!BURROW_FAILED(err))
                testing_t_errorf_v(
                    t, "ToASCII(%q): got no error, but an error expected", in);
        } else if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "ToASCII(%q): got err=%v, but no error expected", in,
                               err);
        } else if (!str_eq(got, str_from_cstr(c->want_ascii))) {
            testing_t_errorf_v(t, "ToASCII(%q): got %q, want %q", in, got,
                               str_from_cstr(c->want_ascii));
        }
    }
    arena_free(&ar);
}

static void TestIDNASeparators(TestingT *t) {
    for (Int i = 0; i < LEN(sep_tests); i++)
        testing_t_run(
            t, str_from_cstr(sep_tests[i].name),
            BURROW_FN(TestingTFunc, separators, (void *)(uintptr_t)&sep_tests[i]));
}

/* ---------------------------------------------------------------- punycode */

static void TestPunycode(TestingT *t) {
    Arena ar;
    Alloc *a = test_alloc(&ar);
    for (Int i = 0; i < LEN(idna_punycode_cases); i++) {
        const IdnaPunycodeCase *tc = &idna_punycode_cases[i];
        Str s = str_from_bytes(tc->s, tc->s_len);
        Str enc = str_from_bytes(tc->enc, tc->enc_len);
        Error err;
        Str got = burrow__idna_punycode_decode(a, enc, &err);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "decode(%q): %v", enc, err);
        else if (!str_eq(got, s))
            testing_t_errorf_v(t, "decode(%q): got %q, want %q", enc, got, s);

        got = burrow__idna_punycode_encode(a, L(""), s, &err);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "encode(\"\", %q): %v", s, err);
        else if (!str_eq(got, enc))
            testing_t_errorf_v(t, "encode(\"\", %q): got %q, want %q", s, got, enc);
    }
    arena_free(&ar);
}

/* strings.Repeat("x", n) + tail, from the heap. */
static Str x_then(Alloc *a, Int n, Str tail) {
    Byte *p = mem_alloc_nozero(a, (size_t)(n + tail.len), 1);
    if (p == NULL)
        return str_from_bytes(NULL, 0);
    memset(p, 'x', (size_t)n);
    memcpy(p + n, tail.p, (size_t)tail.len);
    return str_from_bytes(p, n + tail.len);
}

static void TestPunycodeErrors(TestingT *t) {
    static const char *const decode_cases[] = {
        "-",              /* A sole '-' is invalid. */
        "foo\0bar",       /* '\x00' is not in [0-9A-Za-z]. */
        "foo#bar",        /* '#' is not in [0-9A-Za-z]. */
        "foo\302\243bar", /* U+00A3 is not in [0-9A-Za-z]. */
        "9",              /* "9a" decodes to U+00A3; "9" is truncated. */
        "99999a",         /* "99999a" decodes to U+48A3C1, which is > U+10FFFF. */
        "9999999999a",    /* "9999999999a" overflows the int32 calculation. */
    };
    static const Int decode_lens[] = {1, 7, 7, 8, 1, 6, 11};
    Arena ar;
    Alloc *a = test_alloc(&ar);
    for (Int i = 0; i < LEN(decode_cases); i++) {
        Str in = str_from_bytes(decode_cases[i], decode_lens[i]);
        Error err = BURROW_NO_ERROR;
        burrow__idna_punycode_decode(a, in, &err);
        if (!BURROW_FAILED(err))
            testing_t_errorf_v(t, "no error for decode %q", in);
    }

    /* int32 overflow, and the second is issue #28233. */
    Str enc[2] = {x_then(a, 65536, L("\xEF\xBC\x80")),
                  x_then(a, 65666, L("\xEF\xBF\xBF"))};
    for (Int i = 0; i < 2; i++) {
        if (enc[i].len == 0) {
            testing_t_fatalf_v(t, "out of memory");
            return;
        }
        Error err = BURROW_NO_ERROR;
        burrow__idna_punycode_encode(a, L(""), enc[i], &err);
        if (!BURROW_FAILED(err))
            testing_t_errorf_v(t, "no error for encode %q...%q",
                               str_from_bytes(enc[i].p, 100),
                               str_from_bytes(enc[i].p + enc[i].len - 100, 100));
    }
    arena_free(&ar);
}

/* -------------------------------------------------------------- label errors */

static Str encode(Alloc *a, Str s) {
    Error err;
    return burrow__idna_punycode_encode(a, L("xn--"), s, &err);
}

static IdnaProfile profile_of(const IdnaOption *opts, Int n) {
    return burrow__idna_new(opts, n);
}

static void TestLabelErrors(TestingT *t) {
    const IdnaOption o_length[] = {
        burrow__idna_verify_dns_length(true),
        burrow__idna_map_for_lookup(),
        burrow__idna_bidi_rule(),
    };
    const IdnaOption o_std3[] = {
        burrow__idna_map_for_lookup(),
        burrow__idna_strict_domain_name(false),
    };
    const IdnaOption o_hyphens[] = {
        burrow__idna_map_for_lookup(),
        burrow__idna_check_hyphens(false),
    };
    const IdnaOption o_trans[] = {
        burrow__idna_map_for_lookup(),
        burrow__idna_transitional(true),
    };
    const IdnaOption o_nontrans[] = {
        burrow__idna_map_for_lookup(),
        burrow__idna_transitional(false),
    };
    const Conv punyA = {"PunycodeA", burrow__idna_punycode, true};
    const Conv resolve = {"ResolveA", burrow__idna_lookup, true};
    const Conv display = {"ToUnicode", burrow__idna_display, false};
    const Conv lengthU = {"CheckLengthU", profile_of(o_length, LEN(o_length)), false};
    const Conv lengthA = {"CheckLengthA", profile_of(o_length, LEN(o_length)), true};
    const Conv std3 = {"STD3", profile_of(o_std3, LEN(o_std3)), true};
    const Conv hyphens = {"CheckHyphens", profile_of(o_hyphens, LEN(o_hyphens)), true};
    const Conv transitional = {"Transitional", profile_of(o_trans, LEN(o_trans)), true};
    const Conv nontransitional = {"Nontransitional",
                                  profile_of(o_nontrans, LEN(o_nontrans)), true};

    const struct {
        const Conv *kind;
        const char *input;
        const char *want;
        const char *want_err;
    } cases[] = {
        {&lengthU, "", "", "X4_2"}, /* From UTS 46 conformance test. */
        {&lengthA, "", "", "A4"},

        {&lengthU, "xn--", "", "X4_2"},
        {&lengthU, "foo.xn--", "foo.", "X4_2"},
        {&lengthU, "xn--.foo", ".foo", "X4_2"},
        {&lengthU, "foo.xn--.bar", "foo..bar", "X4_2"},

        {&display, "xn--", "", ""},
        {&display, "foo.xn--", "foo.", ""},
        {&display, "xn--.foo", ".foo", ""},
        {&display, "foo.xn--.bar", "foo..bar", ""},

        {&lengthA, "a..b", "a..b", "A4"},
        {&punyA, ".b", ".b", ""},
        /* For backwards compatibility, the Punycode profile does not map runes. */
        {&punyA,
         "\xE3\x80\x82"
         "b",
         "xn--b-83t", ""},
        {&punyA, "..b", "..b", ""},

        {&lengthA, ".b", ".b", "A4"},
        {&lengthA,
         "\xE3\x80\x82"
         "b",
         ".b", "A4"},
        {&lengthA, "..b", "..b", "A4"},
        {&lengthA, "b..", "b..", "A4"},

        /* Sharpened Bidi rules for Unicode 10.0.0. Apply for ALL labels in ANY of
         * the labels is RTL. */
        {&lengthA, "\xEF\xB8\x85\xE3\x80\x82\xE3\x80\x82\xF0\xA6\x80\xBE\xE1\xB3\xA0",
         "..xn--t6f5138v", "A4"},
        {&lengthA,
         "FAX\xE2\xA9\xB7\xF0\x9D\x86\x86\xE3\x80\x82\xF0\x9E\xA5\x82\xF3\xA0\x86\x81"
         "\xE1\xA0\x8C",
         "", "B6"},

        {&resolve, "a..b", "a..b", ""},
        /* Note that leading dots are not stripped. This is to be consistent with
         * the Punycode profile as well as the conformance test. */
        {&resolve, ".b", ".b", ""},
        {&resolve,
         "\xE3\x80\x82"
         "b",
         ".b", ""},
        {&resolve, "..b", "..b", ""},
        {&resolve, "b..", "b..", ""},
        {&resolve, "\xED", "", "P1"},

        /* Raw punycode */
        {&punyA, "", "", ""},
        {&punyA, "*.foo.com", "*.foo.com", ""},
        {&punyA, "Foo.com", "Foo.com", ""},

        /* STD3 rules */
        {&display, "*.foo.com", "*.foo.com", "U1"},
        {&std3, "*.foo.com", "*.foo.com", ""},

        /* Hyphens */
        {&display, "r3---sn-apo3qvuoxuxbt-j5pe.googlevideo.com",
         "r3---sn-apo3qvuoxuxbt-j5pe.googlevideo.com", "V2"},
        {&hyphens, "r3---sn-apo3qvuoxuxbt-j5pe.googlevideo.com",
         "r3---sn-apo3qvuoxuxbt-j5pe.googlevideo.com", ""},
        {&display, "-label-.com", "-label-.com", "V3"},
        {&hyphens, "-label-.com", "-label-.com", ""},

        /* Don't map U+2490 (DIGIT NINE FULL STOP). This is the behavior of
         * Chrome, modern Firefox, Safari, and IE. */
        {&resolve,
         "lab\xE2\x92\x90"
         "be",
         "xn--labbe-zh9b", "V7"},
        {&display,
         "lab\xE2\x92\x90"
         "be",
         "lab\xE2\x92\x90"
         "be",
         "V7"},
        {&transitional,
         "plan\xE2\x92\x90"
         "fa\xC3\x9F.de",
         "xn--planfass-c31e.de", "V7"},
        {&display,
         "Plan\xE2\x92\x90"
         "fa\xC3\x9F.de",
         "plan\xE2\x92\x90"
         "fa\xC3\x9F.de",
         "V7"},

        /* Transitional vs Nontransitional processing */
        {&transitional, "Plan9fa\xC3\x9F.de", "plan9fass.de", ""},
        {&nontransitional, "Plan9fa\xC3\x9F.de", "xn--plan9fa-6va.de", ""},

        /* Chrome 54.0 recognizes the error and treats this input verbatim as a
         * search string. */
        {&transitional,
         "\xE6\x97\xA5\xE6\x9C\xAC\xE2\x92\x88"
         "co.\xC3\x9F\xC3\x9F\xC3\x9F.de",
         "xn--co-wuw5954azlb.ssssss.de", "V7"},
        {&display,
         "\xE6\x97\xA5\xE6\x9C\xAC\xE2\x92\x88"
         "co.\xC3\x9F\xC3\x9F\xC3\x9F.de",
         "\xE6\x97\xA5\xE6\x9C\xAC\xE2\x92\x88"
         "co.\xC3\x9F\xC3\x9F\xC3\x9F.de",
         "V7"},

        {&transitional,
         "a\xE2\x80\x8C"
         "b",
         "ab", ""},
        {&display,
         "a\xE2\x80\x8C"
         "b",
         "a\xE2\x80\x8C"
         "b",
         "C"},

        {&resolve, "gr\xEF\xBB\x8B\xEF\xBA\xAE\xEF\xBA\x91\xEF\xBB\xB2.de",
         "xn--gr-gtd9a1b0g.de", "B"},
        /* Notice how the string gets transformed, even with an error. Chrome will
         * use the original string if it finds an error, so not the transformed
         * one. */
        {&display, "gr\xEF\xBB\x8B\xEF\xBA\xAE\xEF\xBA\x91\xEF\xBB\xB2.de",
         "gr\xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A.de", "B"},

        {&resolve, "\xD9\xB1.\xCF\x83\xDF\x9C", "xn--qib.xn--4xa21s", "B"},
        {&display, "\xD9\xB1.\xCF\x83\xDF\x9C", "\xD9\xB1.\xCF\x83\xDF\x9C", "B"},

        /* normalize input */
        {&resolve, "a\xCC\xA3\xCC\xA2", "xn--jta191l", ""},
        {&display, "a\xCC\xA3\xCC\xA2", "\xE1\xBA\xA1\xCC\xA2", ""},
    };

    Arena ar;
    Alloc *a = test_alloc(&ar);
    for (Int i = 0; i < LEN(cases); i++)
        do_test(t, a, cases[i].kind, str_from_cstr(cases[i].input),
                str_from_cstr(cases[i].want), str_from_cstr(cases[i].want_err));

    /* The cases Go builds with encode. */
    Str zwnj = encode(a, L("a\xE2\x80\x8C"
                           "b"));
    do_test(t, a, &resolve, zwnj, zwnj, L("C"));
    do_test(t, a, &display,
            L("a\xE2\x80\x8C"
              "b"),
            L("a\xE2\x80\x8C"
              "b"),
            L("C"));

    /* Non-normalized strings are not normalized when they originate from
     * punycode. Despite the error, Chrome, Safari and Firefox will attempt to
     * look up the input punycode. */
    Str marks = encode(a, L("a\xCC\xA3\xCC\xA2"));
    StringsBuilder b = STRINGS_BUILDER(a);
    strings_builder_write_string(&b, marks, NULL);
    strings_builder_write_string(&b, L(".com"), NULL);
    Str in = strings_builder_string(&b);
    do_test(t, a, &resolve, in, L("xn--a-tdbc.com"), L("V1"));
    do_test(t, a, &display, in, L("a\xCC\xA3\xCC\xA2.com"), L("V1"));
    arena_free(&ar);
}

static void TestTransitionalDefault(TestingT *t) {
    Arena ar;
    Alloc *a = test_alloc(&ar);
    const Conv lookup = {"Lookup", burrow__idna_lookup, true};
    do_test(t, a, &lookup,
            L("stra\xC3\x9F"
              "e.de"),
            L("xn--strae-oqa.de"), L(""));
    arena_free(&ar);
}

/* ---------------------------------------------------------------- examples */

/* fmt.Println(s, err) */
static void println_result(StringsBuilder *b, Str s, Error err) {
    strings_builder_write_string(b, s, NULL);
    strings_builder_write_string(b, L(" "), NULL);
    strings_builder_write_string(b, BURROW_FAILED(err) ? error_text(err) : L("<nil>"),
                                 NULL);
    strings_builder_write_string(b, L("\n"), NULL);
}

static void TestExampleProfile(TestingT *t) {
    Arena ar;
    Alloc *a = test_alloc(&ar);
    StringsBuilder b = STRINGS_BUILDER(a);
    Error err;
    Str s;

    /* Raw Punycode has no restrictions and does no mappings. */
    s = burrow__idna_to_ascii(a, L(""), &err);
    println_result(&b, s, err);
    s = burrow__idna_to_ascii(a, L("*.G\xC3\x96PHER.com"), &err);
    println_result(&b, s, err);
    s = burrow__idna_profile_to_ascii(a, &burrow__idna_punycode,
                                      L("*.G\xC3\x96PHER.com"), &err);
    println_result(&b, s, err);

    /* Rewrite IDN for lookup. */
    s = burrow__idna_profile_to_ascii(a, &burrow__idna_lookup, L(""), &err);
    println_result(&b, s, err);
    s = burrow__idna_profile_to_ascii(a, &burrow__idna_lookup,
                                      L("www.G\xC3\x96PHER.com"), &err);
    println_result(&b, s, err);

    /* Convert an IDN to ASCII for registration purposes. This reports an error
     * if the input was illformed. */
    s = burrow__idna_profile_to_ascii(a, &burrow__idna_registration,
                                      L("www.G\xC3\x96PHER.com"), &err);
    println_result(&b, s, err);
    s = burrow__idna_profile_to_ascii(a, &burrow__idna_registration,
                                      L("www.g\xC3\xB6pher.com"), &err);
    println_result(&b, s, err);

    Str want = L(" <nil>\n"
                 "*.xn--GPHER-1oa.com <nil>\n"
                 "*.xn--GPHER-1oa.com <nil>\n"
                 " <nil>\n"
                 "www.xn--gpher-jua.com <nil>\n"
                 "www.xn--GPHER-1oa.com idna: disallowed rune U+0047\n"
                 "www.xn--gpher-jua.com <nil>\n");
    CHECK_TEXT(strings_builder_string(&b), want);
    arena_free(&ar);
}

static void TestExampleNew(TestingT *t) {
    Arena ar;
    Alloc *a = test_alloc(&ar);
    StringsBuilder b = STRINGS_BUILDER(a);
    Str fass = L("*.fa\xC3\x9F.com");
    Error err;
    Str s;
    IdnaProfile p;

    /* Raw Punycode has no restrictions and does no mappings. */
    p = burrow__idna_new(NULL, 0);
    s = burrow__idna_profile_to_ascii(a, &p, fass, &err);
    println_result(&b, s, err);

    /* Do mappings. Note that star is not allowed in a DNS lookup. */
    const IdnaOption lookup[] = {
        burrow__idna_map_for_lookup(),
        burrow__idna_transitional(true), /* Map ß -> ss */
    };
    p = burrow__idna_new(lookup, LEN(lookup));
    s = burrow__idna_profile_to_ascii(a, &p, fass, &err);
    println_result(&b, s, err);

    /* Lookup for registration. Also does not allow '*'. */
    const IdnaOption reg[] = {burrow__idna_validate_for_registration()};
    p = burrow__idna_new(reg, LEN(reg));
    s = burrow__idna_profile_to_unicode(a, &p, fass, &err);
    println_result(&b, s, err);

    /* Set up a profile maps for lookup, but allows wild cards. */
    const IdnaOption wild[] = {
        burrow__idna_map_for_lookup(),
        burrow__idna_transitional(true),        /* Map ß -> ss */
        burrow__idna_strict_domain_name(false), /* Set more permissive ASCII rules. */
    };
    p = burrow__idna_new(wild, LEN(wild));
    s = burrow__idna_profile_to_ascii(a, &p, fass, &err);
    println_result(&b, s, err);

    Str want = L("*.xn--fa-hia.com <nil>\n"
                 "*.fass.com idna: disallowed rune U+002A\n"
                 "*.fa\xC3\x9F.com idna: disallowed rune U+002A\n"
                 "*.fass.com <nil>\n");
    CHECK_TEXT(strings_builder_string(&b), want);
    arena_free(&ar);
}

/* ------------------------------------------------------------- conformance */

static IdnaProfile conf_profile(bool transitional) {
    const IdnaOption opts[] = {
        burrow__idna_transitional(true),
        burrow__idna_verify_dns_length(true),
        burrow__idna_bidi_rule(),
        burrow__idna_map_for_lookup(),
    };
    if (transitional)
        return burrow__idna_new(opts, LEN(opts));
    return burrow__idna_new(opts + 1, LEN(opts) - 1);
}

/* Go runs each line as three subtests. Every one of them passes in Go, which
 * the generator records, and a line Go failed would have to fail here too. */
static void TestConformance(TestingT *t) {
    const Conv to_unicode = {"main:ToUnicode", conf_profile(false), false};
    const Conv to_ascii_n = {"main:ToASCII:N", conf_profile(false), true};
    const Conv to_ascii_t = {"main:ToASCII:T", conf_profile(true), true};
    Arena ar;
    Alloc *a = test_alloc(&ar);
    for (Int i = 0; i < LEN(idna_conformance); i++) {
        const IdnaConformance *c = &idna_conformance[i];
        Str src = lit(c->src);
        bool ok =
            do_test(t, a, &to_unicode, src, lit(c->to_unicode), lit(c->to_unicode_err));
        if (ok != ((c->go_ok & 1) != 0))
            testing_t_errorf_v(t, "line %d: ToUnicode passes %t, in Go %t", i, ok, !ok);
        ok =
            do_test(t, a, &to_ascii_n, src, lit(c->to_ascii_n), lit(c->to_ascii_n_err));
        if (ok != ((c->go_ok & 2) != 0))
            testing_t_errorf_v(t, "line %d: ToASCII:N passes %t, in Go %t", i, ok, !ok);
        ok =
            do_test(t, a, &to_ascii_t, src, lit(c->to_ascii_t), lit(c->to_ascii_t_err));
        if (ok != ((c->go_ok & 4) != 0))
            testing_t_errorf_v(t, "line %d: ToASCII:T passes %t, in Go %t", i, ok, !ok);
        mem_reset(a);
    }
    arena_free(&ar);
}

/* ----------------------------------------------------------- out of memory */

typedef struct Budget {
    Alloc *inner;
    long long left;
} Budget;

static void *budget_alloc(void *self, size_t size, size_t align) {
    Budget *b = (Budget *)self;
    if (b->left <= 0)
        return NULL;
    b->left--;
    return mem_alloc(b->inner, size, align);
}

static void *budget_realloc(void *self, void *p, size_t old, size_t nsz, size_t align) {
    Budget *b = (Budget *)self;
    if (b->left <= 0)
        return NULL;
    b->left--;
    return mem_realloc(b->inner, p, old, nsz, align);
}

static void budget_free(void *self, void *p, size_t size, size_t align) {
    Budget *b = (Budget *)self;
    mem_free(b->inner, p, size, align);
}

static const AllocVT budget_vt = {budget_alloc, NULL, budget_realloc,
                                  budget_free,  NULL, NULL};

/* Every conversion either runs out with an empty result, or gives what it
 * gives with all the memory it wants. */
static void TestOutOfMemory(TestingT *t) {
    static const char *const inputs[] = {
        "www.G\xC3\x96PHER.com",
        "Plan\342\222\220fa\303\237.de",
        "xn--a-tdbc.com",
        "gr\xEF\xBB\x8B\xEF\xBA\xAE\xEF\xBA\x91\xEF\xBB\xB2.de",
        "\xE6\x9D\xB1\xE4\xBA\xAC\xEF\xBC\x8Ejp.xn--bcher-kva.example",
    };
    const Conv kinds[] = {
        {"Punycode", burrow__idna_punycode, true},
        {"Lookup", burrow__idna_lookup, true},
        {"Display", burrow__idna_display, false},
        {"Registration", burrow__idna_registration, true},
    };
    Arena ar;
    Alloc *a = test_alloc(&ar);
    for (Int k = 0; k < LEN(kinds); k++) {
        for (Int i = 0; i < LEN(inputs); i++) {
            Str in = str_from_cstr(inputs[i]);
            Error want_err;
            Str want = convert(a, &kinds[k], in, &want_err);
            for (long long n = 0;; n++) {
                Budget bud = {a, n};
                Alloc ba = {&budget_vt, &bud, NULL, NULL};
                Error err;
                Str got = convert(&ba, &kinds[k], in, &err);
                if (errors_is(err, burrow_err_out_of_memory)) {
                    if (got.len != 0)
                        testing_t_errorf_v(t, "%s %q, %d allocations: got %q with OOM",
                                           kinds[k].name, in, (Int)n, got);
                    continue;
                }
                if (!str_eq(got, want) ||
                    !str_eq(error_text(err), error_text(want_err)))
                    testing_t_errorf_v(
                        t, "%s %q, %d allocations: got %q, %v; want %q, %v",
                        kinds[k].name, in, (Int)n, got, err, want, want_err);
                break;
            }
        }
    }
    arena_free(&ar);
}

/* ------------------------------------------------------------- differential */

typedef struct Rec {
    uint64_t h;
    Byte show[4096];
    Int show_len;
} Rec;

static void rec_init(Rec *r) {
    r->h = 14695981039346656037ULL;
    r->show_len = 0;
}

static void rec_write(Rec *r, const void *p, Int n) {
    const Byte *b = p;
    for (Int i = 0; i < n; i++) {
        r->h ^= b[i];
        r->h *= 1099511628211ULL;
    }
    Int room = (Int)sizeof r->show - r->show_len;
    Int k = n < room ? n : room;
    if (k > 0) {
        memcpy(r->show + r->show_len, b, (size_t)k);
        r->show_len += k;
    }
}

static void rec_cstr(Rec *r, const char *s) {
    rec_write(r, s, (Int)strlen(s));
}

static void rec_int(Rec *r, Int n) {
    char tmp[24];
    int i = (int)sizeof tmp;
    uint64_t u = n < 0 ? (uint64_t)0 - (uint64_t)n : (uint64_t)n;
    do {
        tmp[--i] = (char)('0' + u % 10);
        u /= 10;
    } while (u != 0);
    if (n < 0)
        tmp[--i] = '-';
    rec_write(r, tmp + i, (Int)sizeof tmp - i);
}

static void rec_n(Rec *r, const char *tag, Int n) {
    rec_cstr(r, tag);
    rec_int(r, n);
    rec_cstr(r, ";");
}

static void rec_s(Rec *r, const char *tag, Str s) {
    rec_cstr(r, tag);
    rec_int(r, s.len);
    rec_cstr(r, ":");
    rec_write(r, s.p, s.len);
}

static void rec_e(Rec *r, Error err) {
    if (!BURROW_FAILED(err)) {
        rec_cstr(r, "-;");
        return;
    }
    Str code = burrow__idna_error_code(err);
    rec_write(r, code.p, code.len);
    rec_cstr(r, "|");
    Str s = error_text(err);
    rec_write(r, s.p, s.len);
    rec_cstr(r, ";");
}

/* The profiles of diffProfiles in the generator, in its order. */
static Int diff_profiles(IdnaProfile *out) {
    const IdnaOption o7[] = {burrow__idna_map_for_lookup(),
                             burrow__idna_strict_domain_name(false)};
    const IdnaOption o8[] = {burrow__idna_map_for_lookup(),
                             burrow__idna_check_hyphens(false)};
    const IdnaOption o9[] = {burrow__idna_validate_labels(true)};
    const IdnaOption o10[] = {burrow__idna_validate_for_registration(),
                              burrow__idna_transitional(true)};
    const IdnaOption o11[] = {burrow__idna_remove_leading_dots(true),
                              burrow__idna_map_for_lookup(),
                              burrow__idna_verify_dns_length(true)};
    const IdnaOption o12[] = {burrow__idna_bidi_rule()};
    const IdnaOption o13[] = {burrow__idna_map_for_lookup(),
                              burrow__idna_check_joiners(false),
                              burrow__idna_bidi_rule()};
    Int n = 0;
    out[n++] = burrow__idna_punycode;
    out[n++] = burrow__idna_lookup;
    out[n++] = burrow__idna_display;
    out[n++] = burrow__idna_registration;
    out[n++] = conf_profile(false);
    out[n++] = conf_profile(true);
    out[n++] = burrow__idna_new(o7, LEN(o7));
    out[n++] = burrow__idna_new(o8, LEN(o8));
    out[n++] = burrow__idna_new(o9, LEN(o9));
    out[n++] = burrow__idna_new(o10, LEN(o10));
    out[n++] = burrow__idna_new(o11, LEN(o11));
    out[n++] = burrow__idna_new(o12, LEN(o12));
    out[n++] = burrow__idna_new(o13, LEN(o13));
    return n;
}

static void describe(Rec *r, Alloc *a, const IdnaProfile *ps, Int np, Str s) {
    for (Int i = 0; i < np; i++) {
        Error err;
        Str x = burrow__idna_profile_to_ascii(a, &ps[i], s, &err);
        rec_s(r, "A", x);
        rec_e(r, err);
        x = burrow__idna_profile_to_unicode(a, &ps[i], s, &err);
        rec_s(r, "U", x);
        rec_e(r, err);
    }
}

static void show(TestingT *t, const Rec *r) {
    if (getenv("BURROW_XNET_SHOW") != NULL)
        testing_t_logf_v(t, "record starts %q", str_from_bytes(r->show, r->show_len));
}

static void TestDifferential(TestingT *t) {
    IdnaProfile ps[16];
    Int np = diff_profiles(ps);
    int shown = 0;
    Rec *r = mem_alloc(heap_allocator(), sizeof *r, _Alignof(Rec));
    if (r == NULL) {
        testing_t_fatalf_v(t, "out of memory");
        return;
    }
    Arena ar;
    Alloc *a = test_alloc(&ar);
    for (Int i = 0; i < LEN(idna_corpus); i++) {
        Str s = lit(idna_corpus[i]);
        rec_init(r);
        describe(r, a, ps, np, s);
        mem_reset(a);
        if (r->h != idna_corpus_hash[i]) {
            testing_t_errorf_v(t, "corpus %d %+q: differs from Go", i, s);
            if (shown++ == 0)
                show(t, r);
        }
    }
    arena_free(&ar);
    mem_free(heap_allocator(), r, sizeof *r, _Alignof(Rec));
}

/* -------------------------------------------------------------------- sweep */

/* sweepRecord: the trie value of r and the bytes the lookup reads, and what
 * three profiles say about labels with r in them. */
static void sweep_record(Rec *rec, Alloc *a, const IdnaProfile *trans, Rune r) {
    Byte s[4];
    Int n = utf8_encode_rune((Slice){s, 4, 4, TYPE_BYTE}, r);
    Int sz;
    uint16_t v = burrow__idna_lookup_string(str_from_bytes(s, n), &sz);
    rec_n(rec, "T", v);
    rec_n(rec, "", sz);

    Byte buf[8];
    buf[0] = 'a';
    memcpy(buf + 1, s, (size_t)n);
    buf[n + 1] = '.';
    buf[n + 2] = 'b';
    Error err;
    Str x = burrow__idna_profile_to_ascii(a, &burrow__idna_lookup,
                                          str_from_bytes(buf, n + 3), &err);
    rec_s(rec, "L", x);
    rec_e(rec, err);
    x = burrow__idna_profile_to_unicode(a, &burrow__idna_display, str_from_bytes(s, n),
                                        &err);
    rec_s(rec, "D", x);
    rec_e(rec, err);
    buf[0] = 'x';
    x = burrow__idna_profile_to_ascii(a, trans, str_from_bytes(buf, n + 1), &err);
    rec_s(rec, "T", x);
    rec_e(rec, err);
}

static void TestSweep(TestingT *t) {
    /* Over a million runes three times takes minutes, and longer under the
     * sanitisers and emulators, so the short runs leave it to the long ones. */
    if (testing_short()) {
        testing_t_skip_v(t, "every rune takes minutes");
        return;
    }
    IdnaProfile trans = conf_profile(true);
    Rec *rec = mem_alloc(heap_allocator(), sizeof *rec, _Alignof(Rec));
    if (rec == NULL) {
        testing_t_fatalf_v(t, "out of memory");
        return;
    }
    Arena ar;
    Alloc *a = test_alloc(&ar);
    for (Rune blk = 0; blk < 0x110; blk++) {
        rec_init(rec);
        for (Rune r = blk << 12; r < (blk + 1) << 12; r++) {
            if (r >= 0xd800 && r < 0xe000)
                continue;
            sweep_record(rec, a, &trans, r);
            mem_reset(a);
        }
        if (rec->h != idna_sweep_hash[blk])
            testing_t_errorf_v(t, "block %X: differs from Go", blk);
    }
    arena_free(&ar);
    mem_free(heap_allocator(), rec, sizeof *rec, _Alignof(Rec));
}

#define TESTS(X)                                                                       \
    X(TestAllocToUnicode)                                                              \
    X(TestAllocToASCII)                                                                \
    X(TestProfiles)                                                                    \
    X(TestProfileString)                                                               \
    X(TestIDNA)                                                                        \
    X(TestIDNASeparators)                                                              \
    X(TestPunycode)                                                                    \
    X(TestPunycodeErrors)                                                              \
    X(TestLabelErrors)                                                                 \
    X(TestTransitionalDefault)                                                         \
    X(TestExampleProfile)                                                              \
    X(TestExampleNew)                                                                  \
    X(TestConformance)                                                                 \
    X(TestOutOfMemory)                                                                 \
    X(TestDifferential)                                                                \
    X(TestSweep)

TESTING_MAIN(TESTS)

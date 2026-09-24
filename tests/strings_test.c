/* Derived from Go's src/strings/strings_test.go.
 * Go source: go1.27.1.
 *
 * The tables came out of burrow-gen tests and the test bodies were ported by
 * hand. The two tests at the end are not from Go: they cover running out of
 * memory and freeing a seq, which the collector does in Go.
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
#include "burrow/panic.h"

#include "check.h"

#include <stdint.h>
#include <string.h>

#define S BURROW_S
#define SI BURROW_S_INIT

typedef struct LinesTest {
    Str a;
    const Str *b;
    Int b_len;
} LinesTest;

typedef struct IndexTest {
    Str s;
    Str sep;
    Int out;
} IndexTest;

typedef struct SplitTest {
    Str s;
    Str sep;
    Int n;
    const Str *a;
    Int a_len;
} SplitTest;

typedef struct FieldsTest {
    Str s;
    const Str *a;
    Int a_len;
} FieldsTest;

typedef struct StringTest {
    Str in;
    Str out;
} StringTest;

typedef struct IndexRuneTestsCase {
    Str in;
    Rune rune;
    Int want;
} IndexRuneTestsCase;

typedef struct ToValidUTF8TestsCase {
    Str in;
    Str repl;
    Str out;
} ToValidUTF8TestsCase;

typedef struct TrimTestsCase {
    Str f;
    Str in;
    Str arg;
    Str out;
} TrimTestsCase;

typedef struct Predicate {
    bool (*f)(void *env, Rune r);
    Str name;
} Predicate;

static bool is_space(void *env, Rune r) {
    (void)env;
    return unicode_is_space(r);
}

static bool is_digit(void *env, Rune r) {
    (void)env;
    return unicode_is_digit(r);
}

static bool is_upper(void *env, Rune r) {
    (void)env;
    return unicode_is_upper(r);
}

static bool is_valid_rune(void *env, Rune r) {
    (void)env;
    return r != UTF8_RUNE_ERROR;
}

static bool not_space(void *env, Rune r) {
    return !is_space(env, r);
}

static bool not_digit(void *env, Rune r) {
    return !is_digit(env, r);
}

static bool not_valid_rune(void *env, Rune r) {
    return !is_valid_rune(env, r);
}

#define P_IS_SPACE {is_space, SI("IsSpace")}
#define P_IS_DIGIT {is_digit, SI("IsDigit")}
#define P_IS_UPPER {is_upper, SI("IsUpper")}
#define P_IS_VALID_RUNE {is_valid_rune, SI("IsValidRune")}
#define P_NOT_SPACE {not_space, SI("not(IsSpace)")}
#define P_NOT_DIGIT {not_digit, SI("not(IsDigit)")}
#define P_NOT_VALID_RUNE {not_valid_rune, SI("not(IsValidRune)")}

static RuneFunc pred(Predicate p) {
    RuneFunc f = {p.f, NULL};
    return f;
}

typedef struct TrimFuncTestsCase {
    Predicate f;
    Str in;
    Str trimOut;
    Str leftOut;
    Str rightOut;
} TrimFuncTestsCase;

typedef struct IndexFuncTestsCase {
    Str in;
    Predicate f;
    Int first;
    Int last;
} IndexFuncTestsCase;

typedef struct RepeatTestsCase {
    Str in;
    Str out;
    Int count;
} RepeatTestsCase;

typedef struct RunesTestsCase {
    Str in;
    const Rune *out;
    Int out_len;
    bool lossy;
} RunesTestsCase;

typedef struct UnreadRuneErrorTestsCase {
    Str name;
    void (*f)(StringsReader *r);
} UnreadRuneErrorTestsCase;

typedef struct ReplaceTestsCase {
    Str in;
    Str old;
    Str new_;
    Int n;
    Str out;
} ReplaceTestsCase;

typedef struct TitleTestsCase {
    Str in;
    Str out;
} TitleTestsCase;

typedef struct ContainsTestsCase {
    Str str;
    Str substr;
    bool expected;
} ContainsTestsCase;

typedef struct ContainsAnyTestsCase {
    Str str;
    Str substr;
    bool expected;
} ContainsAnyTestsCase;

typedef struct ContainsRuneTestsCase {
    Str str;
    Rune r;
    bool expected;
} ContainsRuneTestsCase;

typedef struct EqualFoldTestsCase {
    Str s;
    Str t;
    bool out;
} EqualFoldTestsCase;

typedef struct CountTestsCase {
    Str s;
    Str sep;
    Int num;
} CountTestsCase;

typedef struct CutTestsCase {
    Str s;
    Str sep;
    Str before;
    Str after;
    bool found;
} CutTestsCase;

typedef struct CutLastTestsCase {
    Str s;
    Str sep;
    Str before;
    Str after;
    bool found;
} CutLastTestsCase;

typedef struct CutPrefixTestsCase {
    Str s;
    Str sep;
    Str after;
    bool found;
} CutPrefixTestsCase;

typedef struct CutSuffixTestsCase {
    Str s;
    Str sep;
    Str before;
    bool found;
} CutSuffixTestsCase;

static const LinesTest linesTests[] = {
    {.a = SI("abc\nabc\n"), .b = (const Str[]){SI("abc\n"), SI("abc\n")}, .b_len = 2},
    {.a = SI("abc\r\nabc"), .b = (const Str[]){SI("abc\r\n"), SI("abc")}, .b_len = 2},
    {.a = SI("abc\r\n"), .b = (const Str[]){SI("abc\r\n")}, .b_len = 1},
    {.a = SI("\nabc"), .b = (const Str[]){SI("\n"), SI("abc")}, .b_len = 2},
    {.a = SI("\nabc\n\n"),
     .b = (const Str[]){SI("\n"), SI("abc\n"), SI("\n")},
     .b_len = 3},
};

static const IndexTest indexTests[] = {
    {SI(""), SI(""), 0},
    {SI(""), SI("a"), -1},
    {SI(""), SI("foo"), -1},
    {SI("fo"), SI("foo"), -1},
    {SI("foo"), SI("foo"), 0},
    {SI("oofofoofooo"), SI("f"), 2},
    {SI("oofofoofooo"), SI("foo"), 4},
    {SI("barfoobarfoo"), SI("foo"), 3},
    {SI("foo"), SI(""), 0},
    {SI("foo"), SI("o"), 1},
    {SI("abcABCabc"), SI("A"), 3},
    {SI("jrzm6jjhorimglljrea4w3rlgosts0w2gia17hno2td4qd1jz"), SI("jz"), 47},
    {SI("ekkuk5oft4eq0ocpacknhwouic1uua46unx12l37nioq9wbpnocqks6"), SI("ks6"), 52},
    {SI("999f2xmimunbuyew5vrkla9cpwhmxan8o98ec"), SI("98ec"), 33},
    {SI("9lpt9r98i04k8bz6c6dsrthb96bhi"), SI("96bhi"), 24},
    {SI("55u558eqfaod2r2gu42xxsu631xf0zobs5840vl"), SI("5840vl"), 33},
    {SI(""), SI("a"), -1},
    {SI("x"), SI("a"), -1},
    {SI("x"), SI("x"), 0},
    {SI("abc"), SI("a"), 0},
    {SI("abc"), SI("b"), 1},
    {SI("abc"), SI("c"), 2},
    {SI("abc"), SI("x"), -1},
    {SI(""), SI("ab"), -1},
    {SI("bc"), SI("ab"), -1},
    {SI("ab"), SI("ab"), 0},
    {SI("xab"), SI("ab"), 1},
    {SI("xa"), SI("ab"), -1},
    {SI(""), SI("abc"), -1},
    {SI("xbc"), SI("abc"), -1},
    {SI("abc"), SI("abc"), 0},
    {SI("xabc"), SI("abc"), 1},
    {SI("xab"), SI("abc"), -1},
    {SI("xabxc"), SI("abc"), -1},
    {SI(""), SI("abcd"), -1},
    {SI("xbcd"), SI("abcd"), -1},
    {SI("abcd"), SI("abcd"), 0},
    {SI("xabcd"), SI("abcd"), 1},
    {SI("xyabc"), SI("abcd"), -1},
    {SI("xbcqq"), SI("abcqq"), -1},
    {SI("abcqq"), SI("abcqq"), 0},
    {SI("xabcqq"), SI("abcqq"), 1},
    {SI("xyabcq"), SI("abcqq"), -1},
    {SI("xabxcqq"), SI("abcqq"), -1},
    {SI("xabcqxq"), SI("abcqq"), -1},
    {SI(""), SI("01234567"), -1},
    {SI("32145678"), SI("01234567"), -1},
    {SI("01234567"), SI("01234567"), 0},
    {SI("x01234567"), SI("01234567"), 1},
    {SI("x0123456x01234567"), SI("01234567"), 9},
    {SI("xx0123456"), SI("01234567"), -1},
    {SI(""), SI("0123456789"), -1},
    {SI("3214567844"), SI("0123456789"), -1},
    {SI("0123456789"), SI("0123456789"), 0},
    {SI("x0123456789"), SI("0123456789"), 1},
    {SI("x012345678x0123456789"), SI("0123456789"), 11},
    {SI("xyz012345678"), SI("0123456789"), -1},
    {SI("x01234567x89"), SI("0123456789"), -1},
    {SI(""), SI("0123456789012345"), -1},
    {SI("3214567889012345"), SI("0123456789012345"), -1},
    {SI("0123456789012345"), SI("0123456789012345"), 0},
    {SI("x0123456789012345"), SI("0123456789012345"), 1},
    {SI("x012345678901234x0123456789012345"), SI("0123456789012345"), 17},
    {SI(""), SI("01234567890123456789"), -1},
    {SI("32145678890123456789"), SI("01234567890123456789"), -1},
    {SI("01234567890123456789"), SI("01234567890123456789"), 0},
    {SI("x01234567890123456789"), SI("01234567890123456789"), 1},
    {SI("x0123456789012345678x01234567890123456789"), SI("01234567890123456789"), 21},
    {SI("xyz0123456789012345678"), SI("01234567890123456789"), -1},
    {SI(""), SI("0123456789012345678901234567890"), -1},
    {SI("321456788901234567890123456789012345678911"),
     SI("0123456789012345678901234567890"), -1},
    {SI("0123456789012345678901234567890"), SI("0123456789012345678901234567890"), 0},
    {SI("x0123456789012345678901234567890"), SI("0123456789012345678901234567890"), 1},
    {SI("x012345678901234567890123456789x0123456789012345678901234567890"),
     SI("0123456789012345678901234567890"), 32},
    {SI("xyz012345678901234567890123456789"), SI("0123456789012345678901234567890"),
     -1},
    {SI(""), SI("01234567890123456789012345678901"), -1},
    {SI("32145678890123456789012345678901234567890211"),
     SI("01234567890123456789012345678901"), -1},
    {SI("01234567890123456789012345678901"), SI("01234567890123456789012345678901"), 0},
    {SI("x01234567890123456789012345678901"), SI("01234567890123456789012345678901"),
     1},
    {SI("x0123456789012345678901234567890x01234567890123456789012345678901"),
     SI("01234567890123456789012345678901"), 33},
    {SI("xyz0123456789012345678901234567890"), SI("01234567890123456789012345678901"),
     -1},
    {SI("xxxxxx012345678901234567890123456789012345678901234567890123456789012"),
     SI("012345678901234567890123456789012345678901234567890123456789012"), 6},
    {SI(""), SI("0123456789012345678901234567890123456789"), -1},
    {SI("xx012345678901234567890123456789012345678901234567890123456789012"),
     SI("0123456789012345678901234567890123456789"), 2},
    {SI("xx012345678901234567890123456789012345678"),
     SI("0123456789012345678901234567890123456789"), -1},
    {SI("xx012345678901234567890123456789012345678901234567890123456789012"),
     SI("0123456789012345678901234567890123456xxx"), -1},
    {SI("xx0123456789012345678901234567890123456789012345678901234567890120123456789012"
        "345678901234567890123456xxx"),
     SI("0123456789012345678901234567890123456xxx"), 65},
    {SI("oxoxoxoxoxoxoxoxoxoxoxoy"), SI("oy"), 22},
    {SI("oxoxoxoxoxoxoxoxoxoxoxox"), SI("oy"), -1},
    {SI("oxoxoxoxoxoxoxoxoxoxox☺"), SI("☺"), 22},
    {SI("xx0123456789012345678901234567890123456789012345678901234567890120123456789012"
        "345678901234567890123456xxx\xed\x9f\xc0"),
     SI("\xed\x9f\xc0"), 105},
};

static const IndexTest lastIndexTests[] = {
    {SI(""), SI(""), 0},
    {SI(""), SI("a"), -1},
    {SI(""), SI("foo"), -1},
    {SI("fo"), SI("foo"), -1},
    {SI("foo"), SI("foo"), 0},
    {SI("foo"), SI("f"), 0},
    {SI("oofofoofooo"), SI("f"), 7},
    {SI("oofofoofooo"), SI("foo"), 7},
    {SI("barfoobarfoo"), SI("foo"), 9},
    {SI("foo"), SI(""), 3},
    {SI("foo"), SI("o"), 2},
    {SI("abcABCabc"), SI("A"), 3},
    {SI("abcABCabc"), SI("a"), 6},
};

static const IndexTest indexAnyTests[] = {
    {SI(""), SI(""), -1},
    {SI(""), SI("a"), -1},
    {SI(""), SI("abc"), -1},
    {SI("a"), SI(""), -1},
    {SI("a"), SI("a"), 0},
    {SI("\x80"),
     SI("\xff"
        "b"),
     0},
    {SI("aaa"), SI("a"), 0},
    {SI("abc"), SI("xyz"), -1},
    {SI("abc"), SI("xcz"), 2},
    {SI("ab☺c"), SI("x☺yz"), 2},
    {SI("a☺b☻c☹d"), SI("cx"), 8},
    {SI("a☺b☻c☹d"), SI("uvw☻xyz"), 5},
    {SI("aRegExp*"), SI(".(|)*+?^$[]"), 7},
    {SI("1....2....3....41....2....3....41....2....3....4"), SI(" "), -1},
    {SI("012abcba210"),
     SI("\xff"
        "b"),
     4},
    {SI("012\x80"
        "bcb\x80"
        "210"),
     SI("\xff"
        "b"),
     3},
    {SI("0123456πabc"),
     SI("\xcf"
        "b\x80"),
     10},
};

static const IndexTest lastIndexAnyTests[] = {
    {SI(""), SI(""), -1},
    {SI(""), SI("a"), -1},
    {SI(""), SI("abc"), -1},
    {SI("a"), SI(""), -1},
    {SI("a"), SI("a"), 0},
    {SI("\x80"),
     SI("\xff"
        "b"),
     0},
    {SI("aaa"), SI("a"), 2},
    {SI("abc"), SI("xyz"), -1},
    {SI("abc"), SI("ab"), 1},
    {SI("ab☺c"), SI("x☺yz"), 2},
    {SI("a☺b☻c☹d"), SI("cx"), 8},
    {SI("a☺b☻c☹d"), SI("uvw☻xyz"), 5},
    {SI("a.RegExp*"), SI(".(|)*+?^$[]"), 8},
    {SI("1....2....3....41....2....3....41....2....3....4"), SI(" "), -1},
    {SI("012abcba210"),
     SI("\xff"
        "b"),
     6},
    {SI("012\x80"
        "bcb\x80"
        "210"),
     SI("\xff"
        "b"),
     7},
    {SI("0123456πabc"),
     SI("\xcf"
        "b\x80"),
     10},
};

static const IndexTest last_index_byte_testCases[] = {
    {SI(""), SI("q"), -1},
    {SI("abcdef"), SI("q"), -1},
    {SI("abcdefabcdef"), SI("a"), 6},
    {SI("abcdefabcdef"), SI("f"), 11},
    {SI("zabcdefabcdef"), SI("z"), 0},
    {SI("a☺b☻c☹d"), SI("b"), 4},
};

static const IndexRuneTestsCase index_rune_tests[] = {
    {SI(""), 'a', -1},
    {SI(""), 0x263a, -1},
    {SI("foo"), 0x2639, -1},
    {SI("foo"), 'o', 1},
    {SI("foo☺bar"), 0x263a, 3},
    {SI("foo☺☻☹bar"), 0x2639, 9},
    {SI("a A x"), 'A', 2},
    {SI("some_text=some_value"), '=', 9},
    {SI("☺a"), 'a', 3},
    {SI("a☻☺b"), 0x263a, 4},
    {SI("�"), 0xfffd, 0},
    {SI("\xff"), 0xfffd, 0},
    {SI("☻x�"), 0xfffd, 4},
    {SI("☻x\xe2\x98"), 0xfffd, 4},
    {SI("☻x\xe2\x98�"), 0xfffd, 4},
    {SI("☻x\xe2\x98x"), 0xfffd, 4},
    {SI("a☺b☻c☹d\xe2\x98�\xff�\xed\xa0\x80"), -1, -1},
    {SI("a☺b☻c☹d\xe2\x98�\xff�\xed\xa0\x80"), 0xd800, -1},
    {SI("a☺b☻c☹d\xe2\x98�\xff�\xed\xa0\x80"), 0x110000, -1},
    {SI("ӆ"), 0x4c6, 0},
    {SI("a"), 0x4c6, -1},
    {SI("  ӆ"), 0x4c6, 2},
    {SI("  a"), 0x4c6, -1},
    {SI("ццццццццццццццццццццццццццццццццццццццццццццццццццццццццццццццццӆ"), 0x4c6,
     128},
    {SI("ꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꚀ"), 0x4680,
     -1},
    {SI("Ꚁ"), 0xa680, 0},
    {SI("a"), 0xa680, -1},
    {SI("  Ꚁ"), 0xa680, 2},
    {SI("  a"), 0xa680, -1},
    {SI("ꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꚀ"), 0xa680,
     192},
    {SI("𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀"
        "𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡌀"),
     0x23300, -1},
    {SI("𡌀"), 0x21300, 0},
    {SI("a"), 0x21300, -1},
    {SI("  𡌀"), 0x21300, 2},
    {SI("  a"), 0x21300, -1},
    {SI("𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀"
        "𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡌀"),
     0x21300, 256},
    {SI("𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀"
        "𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀"),
     0x21300, -1},
    {SI("aaaaaKKKK\xf2\xbc\x84\x84"), 0xbc104, 17},
    {SI("aaaaaKKKK鄄"), 0x9104, 17},
    {SI("aaKKKKKa\xf2\xbc\x84\x84"), 0xbc104, 18},
    {SI("aaKKKKKa鄄"), 0x9104, 18},
};

static const SplitTest splittests[] = {
    {SI(""), SI(""), -1, NULL, 0},
    {SI("abcd"), SI(""), 2, (const Str[]){SI("a"), SI("bcd")}, 2},
    {SI("abcd"), SI(""), 4, (const Str[]){SI("a"), SI("b"), SI("c"), SI("d")}, 4},
    {SI("abcd"), SI(""), -1, (const Str[]){SI("a"), SI("b"), SI("c"), SI("d")}, 4},
    {SI("☺☻☹"), SI(""), -1, (const Str[]){SI("☺"), SI("☻"), SI("☹")}, 3},
    {SI("☺☻☹"), SI(""), 3, (const Str[]){SI("☺"), SI("☻"), SI("☹")}, 3},
    {SI("☺☻☹"), SI(""), 17, (const Str[]){SI("☺"), SI("☻"), SI("☹")}, 3},
    {SI("☺�☹"), SI(""), -1, (const Str[]){SI("☺"), SI("�"), SI("☹")}, 3},
    {SI("abcd"), SI("a"), 0, NULL, 0},
    {SI("abcd"), SI("a"), -1, (const Str[]){SI(""), SI("bcd")}, 2},
    {SI("abcd"), SI("z"), -1, (const Str[]){SI("abcd")}, 1},
    {SI("1,2,3,4"), SI(","), -1, (const Str[]){SI("1"), SI("2"), SI("3"), SI("4")}, 4},
    {SI("1....2....3....4"), SI("..."), -1,
     (const Str[]){SI("1"), SI(".2"), SI(".3"), SI(".4")}, 4},
    {SI("☺☻☹"), SI("☹"), -1, (const Str[]){SI("☺☻"), SI("")}, 2},
    {SI("☺☻☹"), SI("~"), -1, (const Str[]){SI("☺☻☹")}, 1},
    {SI("1 2 3 4"), SI(" "), 3, (const Str[]){SI("1"), SI("2"), SI("3 4")}, 3},
    {SI("1 2"), SI(" "), 3, (const Str[]){SI("1"), SI("2")}, 2},
    {SI(""), SI("T"), INT64_C(2305843009213693951), (const Str[]){SI("")}, 1},
    {SI("\xff-\xff"), SI(""), -1, (const Str[]){SI("\xff"), SI("-"), SI("\xff")}, 3},
    {SI("\xff-\xff"), SI("-"), -1, (const Str[]){SI("\xff"), SI("\xff")}, 2},
};

static const SplitTest splitaftertests[] = {
    {SI("abcd"), SI("a"), -1, (const Str[]){SI("a"), SI("bcd")}, 2},
    {SI("abcd"), SI("z"), -1, (const Str[]){SI("abcd")}, 1},
    {SI("abcd"), SI(""), -1, (const Str[]){SI("a"), SI("b"), SI("c"), SI("d")}, 4},
    {SI("1,2,3,4"), SI(","), -1, (const Str[]){SI("1,"), SI("2,"), SI("3,"), SI("4")},
     4},
    {SI("1....2....3....4"), SI("..."), -1,
     (const Str[]){SI("1..."), SI(".2..."), SI(".3..."), SI(".4")}, 4},
    {SI("☺☻☹"), SI("☹"), -1, (const Str[]){SI("☺☻☹"), SI("")}, 2},
    {SI("☺☻☹"), SI("~"), -1, (const Str[]){SI("☺☻☹")}, 1},
    {SI("☺☻☹"), SI(""), -1, (const Str[]){SI("☺"), SI("☻"), SI("☹")}, 3},
    {SI("1 2 3 4"), SI(" "), 3, (const Str[]){SI("1 "), SI("2 "), SI("3 4")}, 3},
    {SI("1 2 3"), SI(" "), 3, (const Str[]){SI("1 "), SI("2 "), SI("3")}, 3},
    {SI("1 2"), SI(" "), 3, (const Str[]){SI("1 "), SI("2")}, 2},
    {SI("123"), SI(""), 2, (const Str[]){SI("1"), SI("23")}, 2},
    {SI("123"), SI(""), 17, (const Str[]){SI("1"), SI("2"), SI("3")}, 3},
};

static const FieldsTest fieldstests[] = {
    {SI(""), NULL, 0},
    {SI(" "), NULL, 0},
    {SI(" \t "), NULL, 0},
    {SI("\xe2\x80\x80"), NULL, 0},
    {SI("  abc  "), (const Str[]){SI("abc")}, 1},
    {SI("1 2 3 4"), (const Str[]){SI("1"), SI("2"), SI("3"), SI("4")}, 4},
    {SI("1  2  3  4"), (const Str[]){SI("1"), SI("2"), SI("3"), SI("4")}, 4},
    {SI("1\t\t2\t\t3\t4"), (const Str[]){SI("1"), SI("2"), SI("3"), SI("4")}, 4},
    {SI("1\xe2\x80\x80"
        "2\xe2\x80\x81"
        "3\xe2\x80\x82"
        "4"),
     (const Str[]){SI("1"), SI("2"), SI("3"), SI("4")}, 4},
    {SI("\xe2\x80\x80\xe2\x80\x81\xe2\x80\x82"), NULL, 0},
    {SI("\n™\t™\n"), (const Str[]){SI("™"), SI("™")}, 2},
    {SI("\n\xe2\x80\x80"
        "1™2\xe2\x80\x80 \xe2\x80\x81 ™"),
     (const Str[]){SI("1™2"), SI("™")}, 2},
    {SI("\n1� �2\xe2\x80\x80"
        "3�4"),
     (const Str[]){SI("1�"), SI("�2"), SI("3�4")}, 3},
    {SI("1\xff\xe2\x80\x80\xff"
        "2\xff \xff"),
     (const Str[]){SI("1\xff"),
                   SI("\xff"
                      "2\xff"),
                   SI("\xff")},
     3},
    {SI("☺☻☹"), (const Str[]){SI("☺☻☹")}, 1},
};

static const FieldsTest FieldsFuncTests[] = {
    {SI(""), NULL, 0},
    {SI("XX"), NULL, 0},
    {SI("XXhiXXX"), (const Str[]){SI("hi")}, 1},
    {SI("aXXbXXXcX"), (const Str[]){SI("a"), SI("b"), SI("c")}, 3},
};

static const StringTest upperTests[] = {
    {SI(""), SI("")},
    {SI("ONLYUPPER"), SI("ONLYUPPER")},
    {SI("abc"), SI("ABC")},
    {SI("AbC123"), SI("ABC123")},
    {SI("azAZ09_"), SI("AZAZ09_")},
    {SI("longStrinGwitHmixofsmaLLandcAps"), SI("LONGSTRINGWITHMIXOFSMALLANDCAPS")},
    {SI("RENAN BASTOS 93 AOSDAJDJAIDJAIDAJIaidsjjaidijadsjiadjiOOKKO"),
     SI("RENAN BASTOS 93 AOSDAJDJAIDJAIDAJIAIDSJJAIDIJADSJIADJIOOKKO")},
    {SI("longɐstringɐwithɐnonasciiⱯchars"), SI("LONGⱯSTRINGⱯWITHⱯNONASCIIⱯCHARS")},
    {SI("ɐɐɐɐɐ"), SI("ⱯⱯⱯⱯⱯ")},
    {SI("a\xc2\x80\xf4\x8f\xbf\xbf"), SI("A\xc2\x80\xf4\x8f\xbf\xbf")},
};

static const StringTest lowerTests[] = {
    {SI(""), SI("")},
    {SI("abc"), SI("abc")},
    {SI("AbC123"), SI("abc123")},
    {SI("azAZ09_"), SI("azaz09_")},
    {SI("longStrinGwitHmixofsmaLLandcAps"), SI("longstringwithmixofsmallandcaps")},
    {SI("renan bastos 93 AOSDAJDJAIDJAIDAJIaidsjjaidijadsjiadjiOOKKO"),
     SI("renan bastos 93 aosdajdjaidjaidajiaidsjjaidijadsjiadjiookko")},
    {SI("LONGⱯSTRINGⱯWITHⱯNONASCIIⱯCHARS"), SI("longɐstringɐwithɐnonasciiɐchars")},
    {SI("ⱭⱭⱭⱭⱭ"), SI("ɑɑɑɑɑ")},
    {SI("A\xc2\x80\xf4\x8f\xbf\xbf"), SI("a\xc2\x80\xf4\x8f\xbf\xbf")},
};

static const StringTest trimSpaceTests[] = {
    {SI(""), SI("")},
    {SI("abc"), SI("abc")},
    {SI("\t\v\r\f\n\xc2\x85\xc2\xa0\xe2\x80\x80\xe3\x80\x80"
        "abc\t\v\r\f\n\xc2\x85\xc2\xa0\xe2\x80\x80\xe3\x80\x80"),
     SI("abc")},
    {SI(" "), SI("")},
    {SI(" \t\r\n \t\t\r\r\n\n "), SI("")},
    {SI(" \t\r\n x\t\t\r\r\n\n "), SI("x")},
    {SI(" \xe2\x80\x80\t\r\n x\t\t\r\r\ny\n \xe3\x80\x80"), SI("x\t\t\r\r\ny")},
    {SI("1 \t\r\n2"), SI("1 \t\r\n2")},
    {SI(" x\x80"), SI("x\x80")},
    {SI(" x\xc0"), SI("x\xc0")},
    {SI("x \xc0\xc0 "), SI("x \xc0\xc0")},
    {SI("x \xc0"), SI("x \xc0")},
    {SI("x \xc0 "), SI("x \xc0")},
    {SI("x \xc0\xc0 "), SI("x \xc0\xc0")},
    {SI("x ☺\xc0\xc0 "), SI("x ☺\xc0\xc0")},
    {SI("x ☺ "), SI("x ☺")},
};

static const ToValidUTF8TestsCase toValidUTF8Tests[] = {
    {SI(""), SI("�"), SI("")},
    {SI("abc"), SI("�"), SI("abc")},
    {SI("\xef\xb7\x9d"), SI("�"), SI("\xef\xb7\x9d")},
    {SI("a\xff"
        "b"),
     SI("�"), SI("a�b")},
    {SI("a\xff"
        "b�"),
     SI("X"), SI("aXb�")},
    {SI("a☺\xff"
        "b☺\xc0\xaf"
        "c☺\xff"),
     SI(""), SI("a☺b☺c☺")},
    {SI("a☺\xff"
        "b☺\xc0\xaf"
        "c☺\xff"),
     SI("日本語"), SI("a☺日本語b☺日本語c☺日本語")},
    {SI("\xc0\xaf"), SI("�"), SI("�")},
    {SI("\xe0\x80\xaf"), SI("�"), SI("�")},
    {SI("\xed\xa0\x80"), SI("abc"), SI("abc")},
    {SI("\xed\xbf\xbf"), SI("�"), SI("�")},
    {SI("\xf0\x80\x80\xaf"), SI("☺"), SI("☺")},
    {SI("\xf8\x80\x80\x80\xaf"), SI("�"), SI("�")},
    {SI("\xfc\x80\x80\x80\x80\xaf"), SI("�"), SI("�")},
};

static const TrimTestsCase trimTests[] = {
    {SI("Trim"), SI("abba"), SI("a"), SI("bb")},
    {SI("Trim"), SI("abba"), SI("ab"), SI("")},
    {SI("TrimLeft"), SI("abba"), SI("ab"), SI("")},
    {SI("TrimRight"), SI("abba"), SI("ab"), SI("")},
    {SI("TrimLeft"), SI("abba"), SI("a"), SI("bba")},
    {SI("TrimLeft"), SI("abba"), SI("b"), SI("abba")},
    {SI("TrimRight"), SI("abba"), SI("a"), SI("abb")},
    {SI("TrimRight"), SI("abba"), SI("b"), SI("abba")},
    {SI("Trim"), SI("<tag>"), SI("<>"), SI("tag")},
    {SI("Trim"), SI("* listitem"), SI(" *"), SI("listitem")},
    {SI("Trim"), SI("\"quote\""), SI("\""), SI("quote")},
    {SI("Trim"), SI("ⱯⱯɐɐⱯⱯ"), SI("Ɐ"), SI("ɐɐ")},
    {SI("Trim"), SI("\x80test\xff"), SI("\xff"), SI("test")},
    {SI("Trim"), SI(" Ġ "), SI(" "), SI("Ġ")},
    {SI("Trim"), SI(" Ġİ0"), SI("0 "), SI("Ġİ")},
    {SI("Trim"), SI("abba"), SI(""), SI("abba")},
    {SI("Trim"), SI(""), SI("123"), SI("")},
    {SI("Trim"), SI(""), SI(""), SI("")},
    {SI("TrimLeft"), SI("abba"), SI(""), SI("abba")},
    {SI("TrimLeft"), SI(""), SI("123"), SI("")},
    {SI("TrimLeft"), SI(""), SI(""), SI("")},
    {SI("TrimRight"), SI("abba"), SI(""), SI("abba")},
    {SI("TrimRight"), SI(""), SI("123"), SI("")},
    {SI("TrimRight"), SI(""), SI(""), SI("")},
    {SI("TrimRight"), SI("☺\xc0"), SI("☺"), SI("☺\xc0")},
    {SI("TrimPrefix"), SI("aabb"), SI("a"), SI("abb")},
    {SI("TrimPrefix"), SI("aabb"), SI("b"), SI("aabb")},
    {SI("TrimSuffix"), SI("aabb"), SI("a"), SI("aabb")},
    {SI("TrimSuffix"), SI("aabb"), SI("b"), SI("aab")},
};

static const TrimFuncTestsCase trimFuncTests[] = {
    {P_IS_SPACE,
     SI("\t\v\r\f\n\xc2\x85\xc2\xa0\xe2\x80\x80\xe3\x80\x80 hello "
        "\t\v\r\f\n\xc2\x85\xc2\xa0\xe2\x80\x80\xe3\x80\x80"),
     SI("hello"), SI("hello \t\v\r\f\n\xc2\x85\xc2\xa0\xe2\x80\x80\xe3\x80\x80"),
     SI("\t\v\r\f\n\xc2\x85\xc2\xa0\xe2\x80\x80\xe3\x80\x80 hello")},
    {P_IS_DIGIT, SI("๐๒12hello34๐๑"), SI("hello"), SI("hello34๐๑"), SI("๐๒12hello")},
    {P_IS_UPPER, SI("ⱯⱯⱯⱯABCDhelloEFⱯⱯGHⱯⱯ"), SI("hello"), SI("helloEFⱯⱯGHⱯⱯ"),
     SI("ⱯⱯⱯⱯABCDhello")},
    {P_NOT_SPACE, SI("hello\t\v\r\f\n\xc2\x85\xc2\xa0\xe2\x80\x80\xe3\x80\x80hello"),
     SI("\t\v\r\f\n\xc2\x85\xc2\xa0\xe2\x80\x80\xe3\x80\x80"),
     SI("\t\v\r\f\n\xc2\x85\xc2\xa0\xe2\x80\x80\xe3\x80\x80hello"),
     SI("hello\t\v\r\f\n\xc2\x85\xc2\xa0\xe2\x80\x80\xe3\x80\x80")},
    {P_NOT_DIGIT, SI("hello๐๒1234๐๑helo"), SI("๐๒1234๐๑"), SI("๐๒1234๐๑helo"),
     SI("hello๐๒1234๐๑")},
    {P_IS_VALID_RUNE,
     SI("ab\xc0"
        "a\xc0"
        "cd"),
     SI("\xc0"
        "a\xc0"),
     SI("\xc0"
        "a\xc0"
        "cd"),
     SI("ab\xc0"
        "a\xc0")},
    {P_NOT_VALID_RUNE,
     SI("\xc0"
        "a\xc0"),
     SI("a"), SI("a\xc0"),
     SI("\xc0"
        "a")},
    {P_IS_SPACE, SI(""), SI(""), SI(""), SI("")},
    {P_IS_SPACE, SI(" "), SI(""), SI(""), SI("")},
};

static const IndexFuncTestsCase indexFuncTests[] = {
    {SI(""), P_IS_VALID_RUNE, -1, -1},
    {SI("abc"), P_IS_DIGIT, -1, -1},
    {SI("0123"), P_IS_DIGIT, 0, 3},
    {SI("a1b"), P_IS_DIGIT, 1, 1},
    {SI("\t\v\r\f\n\xc2\x85\xc2\xa0\xe2\x80\x80\xe3\x80\x80"), P_IS_SPACE, 0, 12},
    {SI("๐๒12hello34๐๑"), P_IS_DIGIT, 0, 18},
    {SI("ⱯⱯⱯⱯABCDhelloEFⱯⱯGHⱯⱯ"), P_IS_UPPER, 0, 34},
    {SI("12๐๒hello34๐๑"), P_NOT_DIGIT, 8, 12},
    {SI("\x80"
        "1"),
     P_IS_DIGIT, 1, 1},
    {SI("\x80"
        "abc"),
     P_IS_DIGIT, -1, -1},
    {SI("\xc0"
        "a\xc0"),
     P_IS_VALID_RUNE, 1, 1},
    {SI("\xc0"
        "a\xc0"),
     P_NOT_VALID_RUNE, 0, 2},
    {SI("\xc0☺\xc0"), P_NOT_VALID_RUNE, 0, 4},
    {SI("\xc0☺\xc0\xc0"), P_NOT_VALID_RUNE, 0, 5},
    {SI("ab\xc0"
        "a\xc0"
        "cd"),
     P_NOT_VALID_RUNE, 2, 4},
    {SI("a\xe0\x80"
        "cd"),
     P_NOT_VALID_RUNE, 1, 2},
    {SI("\x80\x80\x80\x80"), P_NOT_VALID_RUNE, 0, 3},
};

static const RepeatTestsCase RepeatTests[] = {
    {SI(""), SI(""), 0},
    {SI(""), SI(""), 1},
    {SI(""), SI(""), 2},
    {SI("-"), SI(""), 0},
    {SI("-"), SI("-"), 1},
    {SI("-"), SI("----------"), 10},
    {SI("abc "), SI("abc abc abc "), 3},
    {SI(" "), SI(" "), 1},
    {SI("--"), SI("----"), 2},
    {SI("==="), SI("======"), 2},
    {SI("000"), SI("000000000"), 3},
    {SI("\t\t\t\t"), SI("\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t"), 4},
};

static const RunesTestsCase RunesTests[] = {
    {SI(""), NULL, 0, false},
    {SI(" "), (const Rune[]){' '}, 1, false},
    {SI("ABC"), (const Rune[]){'A', 'B', 'C'}, 3, false},
    {SI("abc"), (const Rune[]){'a', 'b', 'c'}, 3, false},
    {SI("日本語"), (const Rune[]){0x65e5, 0x672c, 0x8a9e}, 3, false},
    {SI("ab\x80"
        "c"),
     (const Rune[]){'a', 'b', 0xfffd, 'c'}, 4, true},
    {SI("ab\xc0"
        "c"),
     (const Rune[]){'a', 'b', 0xfffd, 'c'}, 4, true},
};

static const Str read_byte_testStrings[] = {
    SI(""),
    SI("abcd"),
    SI("☺☻☹"),
    SI("1,2,3,4"),
};

static const Str read_rune_testStrings[] = {
    SI(""),
    SI("abcd"),
    SI("☺☻☹"),
    SI("1,2,3,4"),
};

/* Writes nothing and says it wrote everything, for WriteTo. */
static Int discard_write(void *self, Slice p, Error *err) {
    (void)self;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return p.len;
}

static const IoWriterVT discard_vt = {NULL, discard_write};

static IoWriter discard(void) {
    IoWriter w = {&discard_vt, NULL};
    return w;
}

static void ur_read(StringsReader *r) {
    Byte b[1] = {0};
    strings_reader_read(r, slice_from(b, 1, 1, TYPE_BYTE), NULL);
}

static void ur_read_byte(StringsReader *r) {
    strings_reader_read_byte(r, NULL);
}

static void ur_unread_rune(StringsReader *r) {
    strings_reader_unread_rune(r);
}

static void ur_seek(StringsReader *r) {
    strings_reader_seek(r, 0, BURROW_IO_SEEK_CURRENT, NULL);
}

static void ur_write_to(StringsReader *r) {
    strings_reader_write_to(r, discard(), NULL);
}

static const UnreadRuneErrorTestsCase UnreadRuneErrorTests[] = {
    {SI("Read"), ur_read},
    {SI("ReadByte"), ur_read_byte},
    {SI("UnreadRune"), ur_unread_rune},
    {SI("Seek"), ur_seek},
    {SI("WriteTo"), ur_write_to},
};

static const ReplaceTestsCase ReplaceTests[] = {
    {SI("hello"), SI("l"), SI("L"), 0, SI("hello")},
    {SI("hello"), SI("l"), SI("L"), -1, SI("heLLo")},
    {SI("hello"), SI("x"), SI("X"), -1, SI("hello")},
    {SI(""), SI("x"), SI("X"), -1, SI("")},
    {SI("radar"), SI("r"), SI("<r>"), -1, SI("<r>ada<r>")},
    {SI(""), SI(""), SI("<>"), -1, SI("<>")},
    {SI("banana"), SI("a"), SI("<>"), -1, SI("b<>n<>n<>")},
    {SI("banana"), SI("a"), SI("<>"), 1, SI("b<>nana")},
    {SI("banana"), SI("a"), SI("<>"), 1000, SI("b<>n<>n<>")},
    {SI("banana"), SI("an"), SI("<>"), -1, SI("b<><>a")},
    {SI("banana"), SI("ana"), SI("<>"), -1, SI("b<>na")},
    {SI("banana"), SI(""), SI("<>"), -1, SI("<>b<>a<>n<>a<>n<>a<>")},
    {SI("banana"), SI(""), SI("<>"), 10, SI("<>b<>a<>n<>a<>n<>a<>")},
    {SI("banana"), SI(""), SI("<>"), 6, SI("<>b<>a<>n<>a<>n<>a")},
    {SI("banana"), SI(""), SI("<>"), 5, SI("<>b<>a<>n<>a<>na")},
    {SI("banana"), SI(""), SI("<>"), 1, SI("<>banana")},
    {SI("banana"), SI("a"), SI("a"), -1, SI("banana")},
    {SI("banana"), SI("a"), SI("a"), 1, SI("banana")},
    {SI("☺☻☹"), SI(""), SI("<>"), -1, SI("<>☺<>☻<>☹<>")},
};

static const TitleTestsCase TitleTests[] = {
    {SI(""), SI("")},
    {SI("a"), SI("A")},
    {SI(" aaa aaa aaa "), SI(" Aaa Aaa Aaa ")},
    {SI(" Aaa Aaa Aaa "), SI(" Aaa Aaa Aaa ")},
    {SI("123a456"), SI("123a456")},
    {SI("double-blind"), SI("Double-Blind")},
    {SI("ÿøû"), SI("Ÿøû")},
    {SI("with_underscore"), SI("With_underscore")},
    {SI("unicode \xe2\x80\xa8 line separator"),
     SI("Unicode \xe2\x80\xa8 Line Separator")},
};

static const ContainsTestsCase ContainsTests[] = {
    {SI("abc"), SI("bc"), true},
    {SI("abc"), SI("bcd"), false},
    {SI("abc"), SI(""), true},
    {SI(""), SI("a"), false},
    {SI("xxxxxx"), SI("01"), false},
    {SI("01xxxx"), SI("01"), true},
    {SI("xx01xx"), SI("01"), true},
    {SI("xxxx01"), SI("01"), true},
    {SI("1xxxxx"), SI("01"), false},
    {SI("xxxxx0"), SI("01"), false},
    {SI("xxxxxxx"), SI("012"), false},
    {SI("012xxxx"), SI("012"), true},
    {SI("xx012xx"), SI("012"), true},
    {SI("xxxx012"), SI("012"), true},
    {SI("12xxxxx"), SI("012"), false},
    {SI("xxxxx01"), SI("012"), false},
    {SI("xxxxxxxx"), SI("0123"), false},
    {SI("0123xxxx"), SI("0123"), true},
    {SI("xx0123xx"), SI("0123"), true},
    {SI("xxxx0123"), SI("0123"), true},
    {SI("123xxxxx"), SI("0123"), false},
    {SI("xxxxx012"), SI("0123"), false},
    {SI("xxxxxxxxx"), SI("01234"), false},
    {SI("01234xxxx"), SI("01234"), true},
    {SI("xx01234xx"), SI("01234"), true},
    {SI("xxxx01234"), SI("01234"), true},
    {SI("1234xxxxx"), SI("01234"), false},
    {SI("xxxxx0123"), SI("01234"), false},
    {SI("xxxxxxxxxxxx"), SI("01234567"), false},
    {SI("01234567xxxx"), SI("01234567"), true},
    {SI("xx01234567xx"), SI("01234567"), true},
    {SI("xxxx01234567"), SI("01234567"), true},
    {SI("1234567xxxxx"), SI("01234567"), false},
    {SI("xxxxx0123456"), SI("01234567"), false},
    {SI("xxxxxxxxxxxxx"), SI("012345678"), false},
    {SI("012345678xxxx"), SI("012345678"), true},
    {SI("xx012345678xx"), SI("012345678"), true},
    {SI("xxxx012345678"), SI("012345678"), true},
    {SI("12345678xxxxx"), SI("012345678"), false},
    {SI("xxxxx01234567"), SI("012345678"), false},
    {SI("xxxxxxxxxxxxxxxxxxxx"), SI("0123456789ABCDEF"), false},
    {SI("0123456789ABCDEFxxxx"), SI("0123456789ABCDEF"), true},
    {SI("xx0123456789ABCDEFxx"), SI("0123456789ABCDEF"), true},
    {SI("xxxx0123456789ABCDEF"), SI("0123456789ABCDEF"), true},
    {SI("123456789ABCDEFxxxxx"), SI("0123456789ABCDEF"), false},
    {SI("xxxxx0123456789ABCDE"), SI("0123456789ABCDEF"), false},
    {SI("xxxxxxxxxxxxxxxxxxxxx"), SI("0123456789ABCDEFG"), false},
    {SI("0123456789ABCDEFGxxxx"), SI("0123456789ABCDEFG"), true},
    {SI("xx0123456789ABCDEFGxx"), SI("0123456789ABCDEFG"), true},
    {SI("xxxx0123456789ABCDEFG"), SI("0123456789ABCDEFG"), true},
    {SI("123456789ABCDEFGxxxxx"), SI("0123456789ABCDEFG"), false},
    {SI("xxxxx0123456789ABCDEF"), SI("0123456789ABCDEFG"), false},
    {SI("xx01x"), SI("012"), false},
    {SI("xx0123x"), SI("01234"), false},
    {SI("xx01234567x"), SI("012345678"), false},
    {SI("xx0123456789ABCDEFx"), SI("0123456789ABCDEFG"), false},
};

static const ContainsAnyTestsCase ContainsAnyTests[] = {
    {SI(""), SI(""), false},
    {SI(""), SI("a"), false},
    {SI(""), SI("abc"), false},
    {SI("a"), SI(""), false},
    {SI("a"), SI("a"), true},
    {SI("aaa"), SI("a"), true},
    {SI("abc"), SI("xyz"), false},
    {SI("abc"), SI("xcz"), true},
    {SI("a☺b☻c☹d"), SI("uvw☻xyz"), true},
    {SI("aRegExp*"), SI(".(|)*+?^$[]"), true},
    {SI("1....2....3....41....2....3....41....2....3....4"), SI(" "), false},
};

static const ContainsRuneTestsCase ContainsRuneTests[] = {
    {SI(""), 'a', false},          {SI("a"), 'a', true},
    {SI("aaa"), 'a', true},        {SI("abc"), 'y', false},
    {SI("abc"), 'c', true},        {SI("a☺b☻c☹d"), 'x', false},
    {SI("a☺b☻c☹d"), 0x263b, true}, {SI("aRegExp*"), '*', true},
};

static const EqualFoldTestsCase EqualFoldTests[] = {
    {SI("abc"), SI("abc"), true},
    {SI("ABcd"), SI("ABcd"), true},
    {SI("123abc"), SI("123ABC"), true},
    {SI("αβδ"), SI("ΑΒΔ"), true},
    {SI("abc"), SI("xyz"), false},
    {SI("abc"), SI("XYZ"), false},
    {SI("abcdefghijk"), SI("abcdefghijX"), false},
    {SI("abcdefghijk"), SI("abcdefghijK"), true},
    {SI("abcdefghijK"), SI("abcdefghijK"), true},
    {SI("abcdefghijkz"), SI("abcdefghijKy"), false},
    {SI("abcdefghijKz"), SI("abcdefghijKy"), false},
    {SI("1"), SI("2"), false},
    {SI("utf-8"), SI("US-ASCII"), false},
};

static const CountTestsCase CountTests[] = {
    {SI(""), SI(""), 1},
    {SI(""), SI("notempty"), 0},
    {SI("notempty"), SI(""), 9},
    {SI("smaller"), SI("not smaller"), 0},
    {SI("12345678987654321"), SI("6"), 2},
    {SI("611161116"), SI("6"), 3},
    {SI("notequal"), SI("NotEqual"), 0},
    {SI("equal"), SI("equal"), 1},
    {SI("abc1231231123q"), SI("123"), 3},
    {SI("11111"), SI("11"), 2},
};

static const CutTestsCase cutTests[] = {
    {SI("abc"), SI("b"), SI("a"), SI("c"), true},
    {SI("abc"), SI("a"), SI(""), SI("bc"), true},
    {SI("abc"), SI("c"), SI("ab"), SI(""), true},
    {SI("abc"), SI("abc"), SI(""), SI(""), true},
    {SI("abc"), SI(""), SI(""), SI("abc"), true},
    {SI("abc"), SI("d"), SI("abc"), SI(""), false},
    {SI(""), SI("d"), SI(""), SI(""), false},
    {SI(""), SI(""), SI(""), SI(""), true},
};

static const CutLastTestsCase cut_last_tests[] = {
    {SI("a/b/c"), SI("/"), SI("a/b"), SI("c"), true},
    {SI("a//b//c"), SI("//"), SI("a//b"), SI("c"), true},
    {SI("abc"), SI("/"), SI("abc"), SI(""), false},
    {SI("abc"), SI(""), SI("abc"), SI(""), true},
    {SI(""), SI(""), SI(""), SI(""), true},
    {SI("/abc"), SI("/"), SI(""), SI("abc"), true},
    {SI("abc/"), SI("/"), SI("abc"), SI(""), true},
};

static const CutPrefixTestsCase cutPrefixTests[] = {
    {SI("abc"), SI("a"), SI("bc"), true}, {SI("abc"), SI("abc"), SI(""), true},
    {SI("abc"), SI(""), SI("abc"), true}, {SI("abc"), SI("d"), SI("abc"), false},
    {SI(""), SI("d"), SI(""), false},     {SI(""), SI(""), SI(""), true},
};

static const CutSuffixTestsCase cutSuffixTests[] = {
    {SI("abc"), SI("bc"), SI("a"), true}, {SI("abc"), SI("abc"), SI(""), true},
    {SI("abc"), SI(""), SI("abc"), true}, {SI("abc"), SI("d"), SI("abc"), false},
    {SI(""), SI("d"), SI(""), false},     {SI(""), SI(""), SI(""), true},
};

/* ------------------------------------------------------------ helpers */

#define LEN(a) ((Int)(sizeof(a) / sizeof((a)[0])))

static Slice strs(const Str *p, Int n) {
    return slice_from((void *)(uintptr_t)p, n, n, TYPE_STRING);
}

static bool strs_eq(Slice got, const Str *want, Int n) {
    if (got.len != n)
        return false;
    for (Int i = 0; i < n; i++) {
        if (!str_eq(BURROW_AT(Str, got, i), want[i]))
            return false;
    }
    return true;
}

static bool slices_eq(Slice a, Slice b) {
    return strs_eq(a, b.len > 0 ? (const Str *)b.p : NULL, b.len);
}

typedef struct Collector {
    Alloc *a;
    Slice out;
} Collector;

static bool collect_yield(void *env, const void *v) {
    Collector *c = (Collector *)env;
    c->out = slice_append(c->a, c->out, v, 1);
    return true;
}

/* slices.Collect. */
static Slice collect(Alloc *a, IterSeq seq) {
    Collector c = {a, slice_nil(TYPE_STRING)};
    BURROW_CALLF(seq, BURROW_FN(IterYield, collect_yield, &c));
    return c.out;
}

/* The collect in Go's test, which runs the seq twice and wants the same. */
static Slice collect_twice(TestingT *t, Alloc *a, IterSeq seq) {
    Slice out = collect(a, seq);
    Slice out1 = collect(a, seq);
    if (!slices_eq(out, out1))
        testing_t_fatalf_v(t, "inconsistent seq:\n%q\n%q", out, out1);
    return out;
}

static Str encode(Alloc *a, const Rune *r, Int n) {
    Byte *p = (Byte *)mem_alloc(a, (size_t)(n * UTF8_UTF_MAX + 1), 1);
    Int w = 0;
    for (Int i = 0; i < n; i++)
        w += utf8_encode_rune(slice_from(p + w, UTF8_UTF_MAX, UTF8_UTF_MAX, TYPE_BYTE),
                              r[i]);
    return str_from_bytes(p, w);
}

static Str concat(Alloc *a, Str x, Str y) {
    Str parts[] = {x, y};
    return strings_join(a, strs(parts, 2), BURROW_STR_EMPTY);
}

static Str rune_str(Alloc *a, Rune r) {
    return encode(a, &r, 1);
}

/* ------------------------------------------------------------ tests */

static void TestLines(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < LEN(linesTests); i++) {
        const LinesTest *s = &linesTests[i];
        Slice result = collect(a, strings_lines(a, s->a));
        if (!strs_eq(result, s->b, s->b_len))
            testing_t_errorf_v(t, "slices.Collect(Lines(%q)) = %q; want %q", s->a,
                               result, strs(s->b, s->b_len));
    }
    arena_free(&ar);
}

static void run_index_tests(TestingT *t, Int (*f)(Str, Str), const char *name,
                            const IndexTest *tests, Int n) {
    for (Int i = 0; i < n; i++) {
        const IndexTest *test = &tests[i];
        Int actual = f(test->s, test->sep);
        if (actual != test->out)
            testing_t_errorf_v(t, "%s(%q,%q) = %v; want %v", name, test->s, test->sep,
                               actual, test->out);
    }
}

static void TestIndex(TestingT *t) {
    run_index_tests(t, strings_index, "Index", indexTests, LEN(indexTests));
}

static void TestLastIndex(TestingT *t) {
    run_index_tests(t, strings_last_index, "LastIndex", lastIndexTests,
                    LEN(lastIndexTests));
}

static void TestIndexAny(TestingT *t) {
    run_index_tests(t, strings_index_any, "IndexAny", indexAnyTests,
                    LEN(indexAnyTests));
}

static void TestLastIndexAny(TestingT *t) {
    run_index_tests(t, strings_last_index_any, "LastIndexAny", lastIndexAnyTests,
                    LEN(lastIndexAnyTests));
}

static void TestIndexByte(TestingT *t) {
    for (Int i = 0; i < LEN(indexTests); i++) {
        const IndexTest *tt = &indexTests[i];
        if (tt->sep.len != 1)
            continue;
        Int pos = strings_index_byte(tt->s, tt->sep.p[0]);
        if (pos != tt->out)
            testing_t_errorf_v(t, "IndexByte(%q, %q) = %v; want %v", tt->s,
                               (Rune)tt->sep.p[0], pos, tt->out);
    }
}

static void TestLastIndexByte(TestingT *t) {
    for (Int i = 0; i < LEN(last_index_byte_testCases); i++) {
        const IndexTest *test = &last_index_byte_testCases[i];
        Int actual = strings_last_index_byte(test->s, test->sep.p[0]);
        if (actual != test->out)
            testing_t_errorf_v(t, "LastIndexByte(%q,%c) = %v; want %v", test->s,
                               (Rune)test->sep.p[0], actual, test->out);
    }
}

static uint64_t rng_state = 0x9E3779B97F4A7C15U;

/* rand.Intn, from a xorshift, which is all this needs. */
static Int rand_intn(Int n) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (Int)(rng_state % (uint64_t)n);
}

static Int simple_index(Str s, Str sep) {
    Int n = sep.len;
    for (Int i = n; i <= s.len; i++) {
        if (str_eq(str_from_bytes(s.p + i - n, n), sep))
            return i - n;
    }
    return -1;
}

static void TestIndexRandom(TestingT *t) {
    static const char chars[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    Byte s1[140];
    Byte sepbuf[141];
    for (int times = 0; times < 10; times++) {
        for (Int str_len = 5 + rand_intn(5); str_len < 140; str_len += 10) {
            for (Int i = 0; i < str_len; i++)
                s1[i] = (Byte)chars[rand_intn((Int)sizeof chars - 1)];
            Str s = str_from_bytes(s1, str_len);
            for (int i = 0; i < 50; i++) {
                Int begin = rand_intn(s.len + 1);
                Int end = begin + rand_intn(s.len + 1 - begin);
                Str sep = str_from_bytes(s.p + begin, end - begin);
                if (i % 4 == 0) {
                    Int pos = rand_intn(sep.len + 1);
                    /* sep.p is NULL when sep is empty, and memcpy wants a
                     * real pointer even for no bytes. */
                    if (pos > 0)
                        memcpy(sepbuf, sep.p, (size_t)pos);
                    sepbuf[pos] = 'A';
                    if (sep.len > pos)
                        memcpy(sepbuf + pos + 1, sep.p + pos, (size_t)(sep.len - pos));
                    sep = str_from_bytes(sepbuf, sep.len + 1);
                }
                Int want = simple_index(s, sep);
                Int res = strings_index(s, sep);
                if (res != want)
                    testing_t_errorf_v(t, "Index(%s,%s) = %d; want %d", s, sep, res,
                                       want);
            }
        }
    }
}

/* Not in Go. Index on long input reads eight positions per step, so this
 * checks it against simple_index over a two letter alphabet, where matches and
 * near misses turn up at every offset within a word and across the tail. */
static void TestIndexWords(TestingT *t) {
    Byte s1[300];
    Byte sepbuf[16];
    for (int times = 0; times < 200; times++) {
        Int str_len = 60 + rand_intn(240);
        for (Int i = 0; i < str_len; i++)
            s1[i] = (Byte)("ab"[rand_intn(2)]);
        Str s = str_from_bytes(s1, str_len);
        for (int i = 0; i < 20; i++) {
            Int n = 2 + rand_intn(14);
            for (Int j = 0; j < n; j++)
                sepbuf[j] = (Byte)("ab"[rand_intn(2)]);
            Str sep = str_from_bytes(sepbuf, n);
            Int want = simple_index(s, sep);
            Int res = strings_index(s, sep);
            if (res != want)
                testing_t_errorf_v(t, "Index(%s,%s) = %d; want %d", s, sep, res, want);
        }
    }
}

static void TestIndexRune(TestingT *t) {
    for (Int i = 0; i < LEN(index_rune_tests); i++) {
        const IndexRuneTestsCase *tt = &index_rune_tests[i];
        Int got = strings_index_rune(tt->in, tt->rune);
        if (got != tt->want)
            testing_t_errorf_v(t, "IndexRune(%q, %d) = %v; want %v", tt->in, tt->rune,
                               got, tt->want);
    }

    /* Make sure we trigger the cutover and string(rune) conversion. Go counts
     * allocations here, and IndexRune has no allocator to allocate from. */
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str haystack =
        concat(a, concat(a, S("test"), strings_repeat(a, S("𡋀"), 32)), S("𡌀"));
    Int i = strings_index_rune(haystack, 's');
    if (i != 2)
        testing_t_fatalf_v(t, "'s' at %d; want 2", i);
    i = strings_index_rune(haystack, 0x21300);
    if (i != 132)
        testing_t_fatalf_v(t, "'𡌀' at %d; want 4", i);
    arena_free(&ar);
}

static void TestSplit(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < LEN(splittests); i++) {
        const SplitTest *tt = &splittests[i];
        Slice got = strings_split_n(a, tt->s, tt->sep, tt->n);
        if (!strs_eq(got, tt->a, tt->a_len)) {
            testing_t_errorf_v(t, "Split(%q, %q, %d) = %v; want %v", tt->s, tt->sep,
                               tt->n, got, strs(tt->a, tt->a_len));
            continue;
        }
        if (tt->n < 0) {
            Slice a2 = collect(a, strings_split_seq(a, tt->s, tt->sep));
            if (!strs_eq(a2, tt->a, tt->a_len))
                testing_t_errorf_v(t, "collect(SplitSeq(%q, %q)) = %v; want %v", tt->s,
                                   tt->sep, a2, strs(tt->a, tt->a_len));
        }
        if (tt->n == 0)
            continue;
        Str s = strings_join(a, got, tt->sep);
        if (!str_eq(s, tt->s))
            testing_t_errorf_v(t, "Join(Split(%q, %q, %d), %q) = %q", tt->s, tt->sep,
                               tt->n, tt->sep, s);
        if (tt->n < 0) {
            Slice b = strings_split(a, tt->s, tt->sep);
            if (!slices_eq(got, b))
                testing_t_errorf_v(
                    t, "Split disagrees with SplitN(%q, %q, %d) = %v; want %v", tt->s,
                    tt->sep, tt->n, b, got);
        }
    }
    arena_free(&ar);
}

static void TestSplitAfter(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < LEN(splitaftertests); i++) {
        const SplitTest *tt = &splitaftertests[i];
        Slice got = strings_split_after_n(a, tt->s, tt->sep, tt->n);
        if (!strs_eq(got, tt->a, tt->a_len)) {
            testing_t_errorf_v(t, "Split(%q, %q, %d) = %v; want %v", tt->s, tt->sep,
                               tt->n, got, strs(tt->a, tt->a_len));
            continue;
        }
        if (tt->n < 0) {
            Slice a2 = collect(a, strings_split_after_seq(a, tt->s, tt->sep));
            if (!strs_eq(a2, tt->a, tt->a_len))
                testing_t_errorf_v(t, "collect(SplitAfterSeq(%q, %q)) = %v; want %v",
                                   tt->s, tt->sep, a2, strs(tt->a, tt->a_len));
        }
        Str s = strings_join(a, got, BURROW_STR_EMPTY);
        if (!str_eq(s, tt->s))
            testing_t_errorf_v(t, "Join(Split(%q, %q, %d), %q) = %q", tt->s, tt->sep,
                               tt->n, tt->sep, s);
        if (tt->n < 0) {
            Slice b = strings_split_after(a, tt->s, tt->sep);
            if (!slices_eq(got, b))
                testing_t_errorf_v(
                    t,
                    "SplitAfter disagrees with SplitAfterN(%q, %q, %d) = %v; "
                    "want %v",
                    tt->s, tt->sep, tt->n, b, got);
        }
    }
    arena_free(&ar);
}

static void TestFields(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < LEN(fieldstests); i++) {
        const FieldsTest *tt = &fieldstests[i];
        Slice got = strings_fields(a, tt->s);
        if (!strs_eq(got, tt->a, tt->a_len)) {
            testing_t_errorf_v(t, "Fields(%q) = %v; want %v", tt->s, got,
                               strs(tt->a, tt->a_len));
            continue;
        }
        Slice a2 = collect_twice(t, a, strings_fields_seq(a, tt->s));
        if (!strs_eq(a2, tt->a, tt->a_len))
            testing_t_errorf_v(t, "collect(FieldsSeq(%q)) = %v; want %v", tt->s, a2,
                               strs(tt->a, tt->a_len));
    }
    arena_free(&ar);
}

static bool is_x(void *env, Rune c) {
    (void)env;
    return c == 'X';
}

static void TestFieldsFunc(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < LEN(fieldstests); i++) {
        const FieldsTest *tt = &fieldstests[i];
        Slice got = strings_fields_func(a, tt->s, pred((Predicate)P_IS_SPACE));
        if (!strs_eq(got, tt->a, tt->a_len)) {
            testing_t_errorf_v(t, "FieldsFunc(%q, unicode.IsSpace) = %v; want %v",
                               tt->s, got, strs(tt->a, tt->a_len));
            continue;
        }
    }
    RuneFunc p = BURROW_FN(RuneFunc, is_x, NULL);
    for (Int i = 0; i < LEN(FieldsFuncTests); i++) {
        const FieldsTest *tt = &FieldsFuncTests[i];
        Slice got = strings_fields_func(a, tt->s, p);
        if (!strs_eq(got, tt->a, tt->a_len))
            testing_t_errorf_v(t, "FieldsFunc(%q) = %v, want %v", tt->s, got,
                               strs(tt->a, tt->a_len));
        Slice a2 = collect_twice(t, a, strings_fields_func_seq(a, tt->s, p));
        if (!strs_eq(a2, tt->a, tt->a_len))
            testing_t_errorf_v(t, "collect(FieldsFuncSeq(%q)) = %v; want %v", tt->s, a2,
                               strs(tt->a, tt->a_len));
    }
    arena_free(&ar);
}

static Str ten_runes(Alloc *a, Rune ch) {
    Rune r[10];
    for (int i = 0; i < 10; i++)
        r[i] = ch;
    return encode(a, r, 10);
}

/* User-defined self-inverse mapping function */
static Rune rot13(void *env, Rune r) {
    (void)env;
    Rune step = 13;
    if (r >= 'a' && r <= 'z')
        return ((r - 'a' + step) % 26) + 'a';
    if (r >= 'A' && r <= 'Z')
        return ((r - 'A' + step) % 26) + 'A';
    return r;
}

static Rune max_rune(void *env, Rune r) {
    (void)env;
    (void)r;
    return UNICODE_MAX_RUNE;
}

static Rune min_rune(void *env, Rune r) {
    (void)env;
    (void)r;
    return 'a';
}

static Rune drop_not_latin(void *env, Rune r) {
    (void)env;
    if (unicode_is(unicode_latin, r))
        return r;
    return -1;
}

static Rune identity(void *env, Rune r) {
    (void)env;
    return r;
}

static Rune replace_not_latin(void *env, Rune r) {
    (void)env;
    if (unicode_is(unicode_latin, r))
        return r;
    return UTF8_RUNE_ERROR;
}

static Rune encode_swap(void *env, Rune r) {
    (void)env;
    switch (r) {
    case UTF8_RUNE_SELF:
        return UNICODE_MAX_RUNE;
    case UNICODE_MAX_RUNE:
        return UTF8_RUNE_SELF;
    default:
        return r;
    }
}

static Rune trim_spaces(void *env, Rune r) {
    (void)env;
    if (unicode_is_space(r))
        return -1;
    return r;
}

#define MAPPING(f) BURROW_FN(RuneMapFunc, f, NULL)

static void TestMap(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *al = arena_allocator(&ar);

    /* Run a couple of awful growth/shrinkage tests */
    Str a = ten_runes(al, 'a');
    /* 1. Grow. This triggers two reallocations in Map. */
    Str m = strings_map(al, MAPPING(max_rune), a);
    Str expect = ten_runes(al, UNICODE_MAX_RUNE);
    if (!str_eq(m, expect))
        testing_t_errorf_v(t, "growing: expected %q got %q", expect, m);

    /* 2. Shrink */
    m = strings_map(al, MAPPING(min_rune), ten_runes(al, UNICODE_MAX_RUNE));
    expect = a;
    if (!str_eq(m, expect))
        testing_t_errorf_v(t, "shrinking: expected %q got %q", expect, m);

    /* 3. Rot13 */
    m = strings_map(al, MAPPING(rot13), S("a to zed"));
    expect = S("n gb mrq");
    if (!str_eq(m, expect))
        testing_t_errorf_v(t, "rot13: expected %q got %q", expect, m);

    /* 4. Rot13^2 */
    m = strings_map(al, MAPPING(rot13), strings_map(al, MAPPING(rot13), S("a to zed")));
    expect = S("a to zed");
    if (!str_eq(m, expect))
        testing_t_errorf_v(t, "rot13: expected %q got %q", expect, m);

    /* 5. Drop */
    m = strings_map(al, MAPPING(drop_not_latin), S("Hello, 세계"));
    expect = S("Hello");
    if (!str_eq(m, expect))
        testing_t_errorf_v(t, "drop: expected %q got %q", expect, m);

    /* 6. Identity */
    Str orig = S("Input string that we expect not to be copied.");
    m = strings_map(al, MAPPING(identity), orig);
    if (orig.p != m.p)
        testing_t_error_v(t, "unexpected copy during identity map");

    /* 7. Handle invalid UTF-8 sequence */
    m = strings_map(al, MAPPING(replace_not_latin), S("Hello\255World"));
    expect = S("Hello�World");
    if (!str_eq(m, expect))
        testing_t_errorf_v(t, "replace invalid sequence: expected %q got %q", expect,
                           m);

    /* 8. Check utf8.RuneSelf and utf8.MaxRune encoding */
    Str s = concat(al, rune_str(al, UTF8_RUNE_SELF), rune_str(al, UTF8_MAX_RUNE));
    Str r = concat(al, rune_str(al, UTF8_MAX_RUNE), rune_str(al, UTF8_RUNE_SELF));
    m = strings_map(al, MAPPING(encode_swap), s);
    if (!str_eq(m, r))
        testing_t_errorf_v(t, "encoding not handled correctly: expected %q got %q", r,
                           m);
    m = strings_map(al, MAPPING(encode_swap), r);
    if (!str_eq(m, s))
        testing_t_errorf_v(t, "encoding not handled correctly: expected %q got %q", s,
                           m);

    /* 9. Check mapping occurs in the front, middle and back */
    m = strings_map(al, MAPPING(trim_spaces), S("   abc    123   "));
    expect = S("abc123");
    if (!str_eq(m, expect))
        testing_t_errorf_v(t, "trimSpaces: expected %q got %q", expect, m);
    arena_free(&ar);
}

static void run_string_tests(TestingT *t, Str (*f)(Alloc *, Str), const char *name,
                             const StringTest *tests, Int n) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < n; i++) {
        const StringTest *tc = &tests[i];
        Str actual = f(a, tc->in);
        if (!str_eq(actual, tc->out))
            testing_t_errorf_v(t, "%s(%q) = %q; want %q", name, tc->in, actual,
                               tc->out);
    }
    arena_free(&ar);
}

static void TestToUpper(TestingT *t) {
    run_string_tests(t, strings_to_upper, "ToUpper", upperTests, LEN(upperTests));
}

static void TestToLower(TestingT *t) {
    run_string_tests(t, strings_to_lower, "ToLower", lowerTests, LEN(lowerTests));
}

static void TestToValidUTF8(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < LEN(toValidUTF8Tests); i++) {
        const ToValidUTF8TestsCase *tc = &toValidUTF8Tests[i];
        Str got = strings_to_valid_utf8(a, tc->in, tc->repl);
        if (!str_eq(got, tc->out))
            testing_t_errorf_v(t, "ToValidUTF8(%q, %q) = %q; want %q", tc->in, tc->repl,
                               got, tc->out);
    }
    arena_free(&ar);
}

static void TestSpecialCase(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str lower = S("abcçdefgğhıijklmnoöprsştuüvyz");
    Str upper = S("ABCÇDEFGĞHIİJKLMNOÖPRSŞTUÜVYZ");
    Str u = strings_to_upper_special(a, unicode_turkish_case, upper);
    if (!str_eq(u, upper))
        testing_t_errorf_v(t, "Upper(upper) is %s not %s", u, upper);
    u = strings_to_upper_special(a, unicode_turkish_case, lower);
    if (!str_eq(u, upper))
        testing_t_errorf_v(t, "Upper(lower) is %s not %s", u, upper);
    Str l = strings_to_lower_special(a, unicode_turkish_case, lower);
    if (!str_eq(l, lower))
        testing_t_errorf_v(t, "Lower(lower) is %s not %s", l, lower);
    l = strings_to_lower_special(a, unicode_turkish_case, upper);
    if (!str_eq(l, lower))
        testing_t_errorf_v(t, "Lower(upper) is %s not %s", l, lower);
    arena_free(&ar);
}

static Str trim_space(Alloc *a, Str s) {
    (void)a;
    return strings_trim_space(s);
}

static void TestTrimSpace(TestingT *t) {
    run_string_tests(t, trim_space, "TrimSpace", trimSpaceTests, LEN(trimSpaceTests));
}

static void TestTrim(TestingT *t) {
    for (Int i = 0; i < LEN(trimTests); i++) {
        const TrimTestsCase *tc = &trimTests[i];
        Str name = tc->f;
        Str (*f)(Str, Str) = NULL;
        if (str_eq(name, S("Trim")))
            f = strings_trim;
        else if (str_eq(name, S("TrimLeft")))
            f = strings_trim_left;
        else if (str_eq(name, S("TrimRight")))
            f = strings_trim_right;
        else if (str_eq(name, S("TrimPrefix")))
            f = strings_trim_prefix;
        else if (str_eq(name, S("TrimSuffix")))
            f = strings_trim_suffix;
        if (f == NULL) {
            testing_t_errorf_v(t, "Undefined trim function %s", name);
            continue;
        }
        Str actual = f(tc->in, tc->arg);
        if (!str_eq(actual, tc->out))
            testing_t_errorf_v(t, "%s(%q, %q) = %q; want %q", name, tc->in, tc->arg,
                               actual, tc->out);
    }
}

static void TestTrimFunc(TestingT *t) {
    for (Int i = 0; i < LEN(trimFuncTests); i++) {
        const TrimFuncTestsCase *tc = &trimFuncTests[i];
        struct {
            const char *name;
            Str (*trim)(Str s, RuneFunc f);
            Str out;
        } trimmers[] = {
            {"TrimFunc", strings_trim_func, tc->trimOut},
            {"TrimLeftFunc", strings_trim_left_func, tc->leftOut},
            {"TrimRightFunc", strings_trim_right_func, tc->rightOut},
        };
        for (Int j = 0; j < LEN(trimmers); j++) {
            Str actual = trimmers[j].trim(tc->in, pred(tc->f));
            if (!str_eq(actual, trimmers[j].out))
                testing_t_errorf_v(t, "%s(%q, %q) = %q; want %q", trimmers[j].name,
                                   tc->in, tc->f.name, actual, trimmers[j].out);
        }
    }
}

static void TestIndexFunc(TestingT *t) {
    for (Int i = 0; i < LEN(indexFuncTests); i++) {
        const IndexFuncTestsCase *tc = &indexFuncTests[i];
        Int first = strings_index_func(tc->in, pred(tc->f));
        if (first != tc->first)
            testing_t_errorf_v(t, "IndexFunc(%q, %s) = %d; want %d", tc->in, tc->f.name,
                               first, tc->first);
        Int last = strings_last_index_func(tc->in, pred(tc->f));
        if (last != tc->last)
            testing_t_errorf_v(t, "LastIndexFunc(%q, %s) = %d; want %d", tc->in,
                               tc->f.name, last, tc->last);
    }
}

static void TestCaseConsistency(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    /* Make a string of all the runes. */
    Int num_runes = (Int)UNICODE_MAX_RUNE + 1;
    if (testing_short())
        num_runes = 1000;
    Rune *r = (Rune *)mem_alloc(a, (size_t)num_runes * sizeof(Rune), _Alignof(Rune));
    for (Int i = 0; i < num_runes; i++)
        r[i] = (Rune)i;
    Str s = encode(a, r, num_runes);
    /* convert the cases. */
    Str upper = strings_to_upper(a, s);
    Str lower = strings_to_lower(a, s);

    /* Consistency checks */
    Int n = utf8_rune_count_in_string(upper);
    if (n != num_runes)
        testing_t_error_v(t, "rune count wrong in upper:", n);
    n = utf8_rune_count_in_string(lower);
    if (n != num_runes)
        testing_t_error_v(t, "rune count wrong in lower:", n);
    if (!str_eq(strings_to_upper(a, upper), upper))
        testing_t_error_v(t, "ToUpper(upper) consistency fail");
    if (!str_eq(strings_to_lower(a, lower), lower))
        testing_t_error_v(t, "ToLower(lower) consistency fail");
    /* Go keeps ToUpper(lower) and ToLower(upper) commented out, because the
     * data is not one to one: several upper case I map to i. */
    arena_free(&ar);
}

static void check_repeat(TestingT *t, Alloc *a, Str in, Str out, Int count) {
    Str got = strings_repeat(a, in, count);
    if (!str_eq(got, out))
        testing_t_errorf_v(t, "Repeat(%v, %d) = %v; want %v", in, count, got, out);
}

static void TestRepeat(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < LEN(RepeatTests); i++) {
        const RepeatTestsCase *tt = &RepeatTests[i];
        check_repeat(t, a, tt->in, tt->out, tt->count);
    }

    /* The three rows Go builds at init time. */
    Byte *spaces = (Byte *)mem_alloc(a, 200, 1);
    memset(spaces, ' ', 200);
    Str long_spaces = str_from_bytes(spaces, 200);
    check_repeat(t, a, S(" "), long_spaces, long_spaces.len);

    /* Tests for results over the chunkLimit */
    Str zeros = str_from_bytes(mem_alloc(a, 1 << 16, 1), 1 << 16);
    check_repeat(t, a, str_from_bytes("", 1), zeros, 1 << 16);

    Str long_string = concat(a, concat(a, S("a"), zeros), S("z"));
    check_repeat(t, a, long_string, concat(a, long_string, long_string), 2);
    arena_free(&ar);
}

typedef struct RepeatCall {
    Alloc *a;
    Str s;
    Int count;
} RepeatCall;

static void call_repeat(void *env) {
    RepeatCall *c = (RepeatCall *)env;
    strings_repeat(c->a, c->s, c->count);
}

/* The panic text from repeat, or the empty string if it did not panic. */
static Str repeat_panic(Alloc *a, Str s, Int count) {
    RepeatCall c = {a, s, count};
    Str text = BURROW_STR_EMPTY;
    BURROW_TRY {
        call_repeat(&c);
    }
    BURROW_CATCH(r) {
        if (r.t != TYPE_STRING)
            panic(r);
        text = *(const Str *)r.data;
    }
    BURROW_TRY_END;
    return text;
}

static void TestRepeatCatchesOverflow(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str b255 = str_from_bytes(mem_alloc(a, 255, 1), 255);
    struct {
        Str s;
        Int count;
        const char *err_str;
    } tests[] = {
        {S("--"), -2147483647, "negative"},
        {S(""), BURROW_INT_MAX, ""},
        {S("-"), 10, ""},
        {S("gopher"), 0, ""},
        {S("-"), -1, "negative"},
        {S("--"), -102, "negative"},
        {b255, (Int)(UINTPTR_MAX / 255 + 1), "overflow"},
    };
    for (Int i = 0; i < LEN(tests); i++) {
        Str err = repeat_panic(a, tests[i].s, tests[i].count);
        Str want = str_from_cstr(tests[i].err_str);
        if (want.len == 0) {
            if (err.len != 0)
                testing_t_errorf_v(t, "#%d panicked %v", i, err);
            continue;
        }
        if (err.len == 0 || !strings_contains(err, want))
            testing_t_errorf_v(t, "#%d got %q want %q", i, err, want);
    }

    /* Go's 64-bit case, {"-", maxInt}, is the runtime refusing to make the
     * buffer, which panics with "out of range". Here the allocator refuses
     * and Repeat gives the empty string, as every function here does when it
     * runs out of memory. A fixed allocator refuses without asking malloc. */
    unsigned char buf[64];
    Fixed fx;
    fixed_init(&fx, buf, sizeof buf);
    Str got = strings_repeat(fixed_allocator(&fx), S("-"), BURROW_INT_MAX);
    if (got.len != 0)
        testing_t_errorf_v(t, "Repeat(\"-\", maxInt) = %d bytes, want 0", got.len);
    arena_free(&ar);
}

static void TestRunes(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *al = arena_allocator(&ar);
    for (Int i = 0; i < LEN(RunesTests); i++) {
        const RunesTestsCase *tt = &RunesTests[i];
        Rune a[16];
        Int n = 0;
        Int j;
        Rune r;
        for (StrIter it = str_runes(tt->in); str_next_rune(&it, &j, &r);)
            a[n++] = r;
        bool eq = n == tt->out_len;
        for (Int k = 0; eq && k < n; k++)
            eq = a[k] == tt->out[k];
        if (!eq) {
            testing_t_errorf_v(t, "[]rune(%q) = %v; want %v", tt->in,
                               slice_from(a, n, n, TYPE_RUNE),
                               slice_from((void *)(uintptr_t)tt->out, tt->out_len,
                                          tt->out_len, TYPE_RUNE));
            continue;
        }
        if (!tt->lossy) {
            /* can only test reassembly if we didn't lose information */
            Str s = encode(al, a, n);
            if (!str_eq(s, tt->in))
                testing_t_errorf_v(t, "string([]rune(%q)) = %x; want %x", tt->in, s,
                                   tt->in);
        }
    }
    arena_free(&ar);
}

static void TestReadByte(TestingT *t) {
    for (Int i = 0; i < LEN(read_byte_testStrings); i++) {
        Str s = read_byte_testStrings[i];
        StringsReader reader;
        strings_reader_reset(&reader, s);
        if (BURROW_OK(strings_reader_unread_byte(&reader)))
            testing_t_errorf_v(t, "Unreading %q at beginning: expected error", s);
        Byte res[64];
        Int n = 0;
        for (;;) {
            Error e;
            Byte b = strings_reader_read_byte(&reader, &e);
            if (errors_is(e, io_eof))
                break;
            if (BURROW_FAILED(e)) {
                testing_t_errorf_v(t, "Reading %q: %s", s, error_text(e));
                break;
            }
            res[n++] = b;
            /* unread and read again */
            e = strings_reader_unread_byte(&reader);
            if (BURROW_FAILED(e)) {
                testing_t_errorf_v(t, "Unreading %q: %s", s, error_text(e));
                break;
            }
            Byte b1 = strings_reader_read_byte(&reader, &e);
            if (BURROW_FAILED(e)) {
                testing_t_errorf_v(t, "Reading %q after unreading: %s", s,
                                   error_text(e));
                break;
            }
            if (b1 != b) {
                testing_t_errorf_v(t,
                                   "Reading %q after unreading: want byte %q, got %q",
                                   s, (Rune)b, (Rune)b1);
                break;
            }
        }
        if (!str_eq(str_from_bytes(res, n), s))
            testing_t_errorf_v(t, "Reader(%q).ReadByte() produced %q", s,
                               str_from_bytes(res, n));
    }
}

static void TestReadRune(TestingT *t) {
    for (Int i = 0; i < LEN(read_rune_testStrings); i++) {
        Str s = read_rune_testStrings[i];
        StringsReader reader;
        strings_reader_reset(&reader, s);
        if (BURROW_OK(strings_reader_unread_rune(&reader)))
            testing_t_errorf_v(t, "Unreading %q at beginning: expected error", s);
        Byte res[64];
        Int n = 0;
        for (;;) {
            Error e;
            Int z;
            Rune r = strings_reader_read_rune(&reader, &z, &e);
            if (errors_is(e, io_eof))
                break;
            if (BURROW_FAILED(e)) {
                testing_t_errorf_v(t, "Reading %q: %s", s, error_text(e));
                break;
            }
            n += utf8_encode_rune(
                slice_from(res + n, UTF8_UTF_MAX, UTF8_UTF_MAX, TYPE_BYTE), r);
            /* unread and read again */
            e = strings_reader_unread_rune(&reader);
            if (BURROW_FAILED(e)) {
                testing_t_errorf_v(t, "Unreading %q: %s", s, error_text(e));
                break;
            }
            Int z1;
            Rune r1 = strings_reader_read_rune(&reader, &z1, &e);
            if (BURROW_FAILED(e)) {
                testing_t_errorf_v(t, "Reading %q after unreading: %s", s,
                                   error_text(e));
                break;
            }
            if (r1 != r) {
                testing_t_errorf_v(
                    t, "Reading %q after unreading: want rune %q, got %q", s, r, r1);
                break;
            }
            if (z1 != z) {
                testing_t_errorf_v(
                    t, "Reading %q after unreading: want size %d, got %d", s, z, z1);
                break;
            }
        }
        if (!str_eq(str_from_bytes(res, n), s))
            testing_t_errorf_v(t, "Reader(%q).ReadRune() produced %q", s,
                               str_from_bytes(res, n));
    }
}

static void TestUnreadRuneError(TestingT *t) {
    for (Int i = 0; i < LEN(UnreadRuneErrorTests); i++) {
        const UnreadRuneErrorTestsCase *tt = &UnreadRuneErrorTests[i];
        StringsReader reader;
        strings_reader_reset(&reader, S("0123456789"));
        Error err;
        strings_reader_read_rune(&reader, NULL, &err);
        if (BURROW_FAILED(err)) {
            /* should not happen */
            testing_t_fatal_v(t, error_text(err));
        }
        tt->f(&reader);
        err = strings_reader_unread_rune(&reader);
        if (BURROW_OK(err))
            testing_t_errorf_v(t, "Unreading after %s: expected error", tt->name);
    }
}

static void TestReplace(TestingT *t) {
    for (Int i = 0; i < LEN(ReplaceTests); i++) {
        const ReplaceTestsCase *tt = &ReplaceTests[i];
        /* Go wants at most one allocation, and a fixed allocator counts. */
        unsigned char buf[256];
        Fixed fx;
        fixed_init(&fx, buf, sizeof buf);
        Alloc *a = fixed_allocator(&fx);
        Str s = strings_replace(a, tt->in, tt->old, tt->new_, tt->n);
        if (fx.allocs > 1)
            testing_t_errorf_v(t, "Replace(%q, %q, %q, %d) allocates %d objects",
                               tt->in, tt->old, tt->new_, tt->n, fx.allocs);
        if (!str_eq(s, tt->out))
            testing_t_errorf_v(t, "Replace(%q, %q, %q, %d) = %q, want %q", tt->in,
                               tt->old, tt->new_, tt->n, s, tt->out);
        if (tt->n == -1) {
            s = strings_replace_all(a, tt->in, tt->old, tt->new_);
            if (!str_eq(s, tt->out))
                testing_t_errorf_v(t, "ReplaceAll(%q, %q, %q) = %q, want %q", tt->in,
                                   tt->old, tt->new_, s, tt->out);
        }
    }
}

static void TestTitle(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < LEN(TitleTests); i++) {
        const TitleTestsCase *tt = &TitleTests[i];
        Str s = strings_title(a, tt->in);
        if (!str_eq(s, tt->out))
            testing_t_errorf_v(t, "Title(%q) = %q, want %q", tt->in, s, tt->out);
    }
    arena_free(&ar);
}

static void TestContains(TestingT *t) {
    for (Int i = 0; i < LEN(ContainsTests); i++) {
        const ContainsTestsCase *ct = &ContainsTests[i];
        if (strings_contains(ct->str, ct->substr) != ct->expected)
            testing_t_errorf_v(t, "Contains(%s, %s) = %v, want %v", ct->str, ct->substr,
                               !ct->expected, ct->expected);
    }
}

static void TestContainsAny(TestingT *t) {
    for (Int i = 0; i < LEN(ContainsAnyTests); i++) {
        const ContainsAnyTestsCase *ct = &ContainsAnyTests[i];
        if (strings_contains_any(ct->str, ct->substr) != ct->expected)
            testing_t_errorf_v(t, "ContainsAny(%s, %s) = %v, want %v", ct->str,
                               ct->substr, !ct->expected, ct->expected);
    }
}

static void TestContainsRune(TestingT *t) {
    for (Int i = 0; i < LEN(ContainsRuneTests); i++) {
        const ContainsRuneTestsCase *ct = &ContainsRuneTests[i];
        if (strings_contains_rune(ct->str, ct->r) != ct->expected)
            testing_t_errorf_v(t, "ContainsRune(%q, %q) = %v, want %v", ct->str, ct->r,
                               !ct->expected, ct->expected);
    }
}

static bool rune_is(void *env, Rune r) {
    return *(const Rune *)env == r;
}

static void TestContainsFunc(TestingT *t) {
    for (Int i = 0; i < LEN(ContainsRuneTests); i++) {
        const ContainsRuneTestsCase *ct = &ContainsRuneTests[i];
        Rune want = ct->r;
        if (strings_contains_func(ct->str, BURROW_FN(RuneFunc, rune_is, &want)) !=
            ct->expected)
            testing_t_errorf_v(t, "ContainsFunc(%q, func(%q)) = %v, want %v", ct->str,
                               ct->r, !ct->expected, ct->expected);
    }
}

static void TestEqualFold(TestingT *t) {
    for (Int i = 0; i < LEN(EqualFoldTests); i++) {
        const EqualFoldTestsCase *tt = &EqualFoldTests[i];
        bool out = strings_equal_fold(tt->s, tt->t);
        if (out != tt->out)
            testing_t_errorf_v(t, "EqualFold(%#q, %#q) = %v, want %v", tt->s, tt->t,
                               out, tt->out);
        out = strings_equal_fold(tt->t, tt->s);
        if (out != tt->out)
            testing_t_errorf_v(t, "EqualFold(%#q, %#q) = %v, want %v", tt->t, tt->s,
                               out, tt->out);
    }
}

static void TestCount(TestingT *t) {
    for (Int i = 0; i < LEN(CountTests); i++) {
        const CountTestsCase *tt = &CountTests[i];
        Int num = strings_count(tt->s, tt->sep);
        if (num != tt->num)
            testing_t_errorf_v(t, "Count(%q, %q) = %d, want %d", tt->s, tt->sep, num,
                               tt->num);
    }
}

static void TestCut(TestingT *t) {
    for (Int i = 0; i < LEN(cutTests); i++) {
        const CutTestsCase *tt = &cutTests[i];
        Str after;
        bool found;
        Str before = strings_cut(tt->s, tt->sep, &after, &found);
        if (!str_eq(before, tt->before) || !str_eq(after, tt->after) ||
            found != tt->found)
            testing_t_errorf_v(t, "Cut(%q, %q) = %q, %q, %v, want %q, %q, %v", tt->s,
                               tt->sep, before, after, found, tt->before, tt->after,
                               tt->found);
    }
}

static void TestCutLast(TestingT *t) {
    for (Int i = 0; i < LEN(cut_last_tests); i++) {
        const CutLastTestsCase *tt = &cut_last_tests[i];
        Str after;
        bool found;
        Str before = strings_cut_last(tt->s, tt->sep, &after, &found);
        if (!str_eq(before, tt->before) || !str_eq(after, tt->after) ||
            found != tt->found)
            testing_t_errorf_v(t, "CutLast(%q, %q) = %q, %q, %v; want %q, %q, %v",
                               tt->s, tt->sep, before, after, found, tt->before,
                               tt->after, tt->found);
    }
}

static void TestCutPrefix(TestingT *t) {
    for (Int i = 0; i < LEN(cutPrefixTests); i++) {
        const CutPrefixTestsCase *tt = &cutPrefixTests[i];
        bool found;
        Str after = strings_cut_prefix(tt->s, tt->sep, &found);
        if (!str_eq(after, tt->after) || found != tt->found)
            testing_t_errorf_v(t, "CutPrefix(%q, %q) = %q, %v, want %q, %v", tt->s,
                               tt->sep, after, found, tt->after, tt->found);
    }
}

static void TestCutSuffix(TestingT *t) {
    for (Int i = 0; i < LEN(cutSuffixTests); i++) {
        const CutSuffixTestsCase *tt = &cutSuffixTests[i];
        bool found;
        Str before = strings_cut_suffix(tt->s, tt->sep, &found);
        if (!str_eq(before, tt->before) || found != tt->found)
            testing_t_errorf_v(t, "CutSuffix(%q, %q) = %q, %v, want %q, %v", tt->s,
                               tt->sep, before, found, tt->before, tt->found);
    }
}

/* ------------------------------------------------------------ not from Go */

/* Go's collector owns the memory, so Go has no test for running out of it.
 * Every function here that allocates hands back its zero value when a says
 * no, and a fixed allocator with nothing in it always says no. */
static void TestOutOfMemory(TestingT *t) {
    unsigned char buf[1];
    Fixed fx;
    fixed_init(&fx, buf, 0);
    Alloc *a = fixed_allocator(&fx);
    CHECK(strings_split(a, S("a,b,c"), S(",")).len == 0);
    CHECK(strings_fields(a, S("a b c")).len == 0);
    CHECK(strings_repeat(a, S("ab"), 3).len == 0);
    CHECK(strings_replace_all(a, S("aaa"), S("a"), S("b")).len == 0);
    CHECK(strings_to_upper(a, S("abc")).len == 0);
    CHECK(strings_clone(a, S("abc")).len == 0);
    CHECK(BURROW_FUNC_IS_NIL(strings_lines(a, S("a\nb"))));
    CHECK(BURROW_FUNC_IS_NIL(strings_split_seq(a, S("a,b"), S(","))));
    CHECK(BURROW_FUNC_IS_NIL(strings_fields_seq(a, S("a b"))));
    CHECK(strings_new_reader(a, S("abc")) == NULL);
    /* Nothing that needs no memory is refused. */
    CHECK(str_eq(strings_repeat(a, S("ab"), 1), S("ab")));
    CHECK(str_eq(strings_to_upper(a, S("ABC")), S("ABC")));
}

/* With the heap, a seq's state is freed with strings_seq_free. Under ASan this
 * is a leak check as well. */
static void TestSeqFree(TestingT *t) {
    Alloc *a = heap_allocator();
    Arena ar;
    arena_init(&ar, NULL, 0);
    IterSeq seq = strings_fields_seq(a, S(" a  b c "));
    Slice got = collect(arena_allocator(&ar), seq);
    static const Str want[] = {SI("a"), SI("b"), SI("c")};
    if (!strs_eq(got, want, 3))
        testing_t_errorf_v(t, "FieldsSeq = %q, want %q", got, strs(want, 3));
    strings_seq_free(a, seq);
    strings_seq_free(a, strings_lines(a, S("x\ny")));
    IterSeq nil_seq = {NULL, NULL};
    strings_seq_free(a, nil_seq);
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestLines)                                                                       \
    X(TestIndex)                                                                       \
    X(TestLastIndex)                                                                   \
    X(TestIndexAny)                                                                    \
    X(TestLastIndexAny)                                                                \
    X(TestIndexByte)                                                                   \
    X(TestLastIndexByte)                                                               \
    X(TestIndexRandom)                                                                 \
    X(TestIndexWords)                                                                  \
    X(TestIndexRune)                                                                   \
    X(TestSplit)                                                                       \
    X(TestSplitAfter)                                                                  \
    X(TestFields)                                                                      \
    X(TestFieldsFunc)                                                                  \
    X(TestMap)                                                                         \
    X(TestToUpper)                                                                     \
    X(TestToLower)                                                                     \
    X(TestToValidUTF8)                                                                 \
    X(TestSpecialCase)                                                                 \
    X(TestTrimSpace)                                                                   \
    X(TestTrim)                                                                        \
    X(TestTrimFunc)                                                                    \
    X(TestIndexFunc)                                                                   \
    X(TestCaseConsistency)                                                             \
    X(TestRepeat)                                                                      \
    X(TestRepeatCatchesOverflow)                                                       \
    X(TestRunes)                                                                       \
    X(TestReadByte)                                                                    \
    X(TestReadRune)                                                                    \
    X(TestUnreadRuneError)                                                             \
    X(TestReplace)                                                                     \
    X(TestTitle)                                                                       \
    X(TestContains)                                                                    \
    X(TestContainsAny)                                                                 \
    X(TestContainsRune)                                                                \
    X(TestContainsFunc)                                                                \
    X(TestEqualFold)                                                                   \
    X(TestCount)                                                                       \
    X(TestCut)                                                                         \
    X(TestCutLast)                                                                     \
    X(TestCutPrefix)                                                                   \
    X(TestCutSuffix)                                                                   \
    X(TestOutOfMemory)                                                                 \
    X(TestSeqFree)

TESTING_MAIN(TESTS)

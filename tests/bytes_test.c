/* Derived from Go's src/bytes/bytes_test.go.
 * Go source: go1.27.1.
 *
 * The tables came out of burrow-gen tests and the test bodies were ported by
 * hand. Go's tests that count allocations check the results here and leave
 * the counting out: nothing in this package allocates without being handed an
 * allocator, so the functions that take none cannot allocate at all. The two
 * tests at the end are not from Go: they cover running out of memory and
 * freeing a seq, which the collector does in Go.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/bytes.h"

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
#define LEN(a) ((Int)(sizeof(a) / sizeof((a)[0])))

#define DOTS "1....2....3....4"

typedef struct LinesTest {
    Str a;
    const Str *b;
    Int b_len;
} LinesTest;

typedef struct BinOpTest {
    Str a;
    Str b;
    Int i;
} BinOpTest;

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

typedef struct RepeatTest {
    Str in;
    Str out;
    Int count;
} RepeatTest;

typedef struct RunesTest {
    Str in;
    const Rune *out;
    Int out_len;
    bool lossy;
} RunesTest;

/* In this and TrimNilTest, a Str of {0} stands for Go's nil slice. */
typedef struct TrimTest {
    Str f;
    Str in;
    Str arg;
    Str out;
} TrimTest;

typedef struct TrimNilTest {
    Str f;
    Str in;
    Str arg;
    Str out;
} TrimNilTest;

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

typedef struct TrimFuncTest {
    Predicate f;
    Str in;
    Str trimOut;
    Str leftOut;
    Str rightOut;
} TrimFuncTest;

typedef struct IndexFuncTest {
    Str in;
    Predicate f;
    Int first;
    Int last;
} IndexFuncTest;

typedef struct ReplaceTest {
    Str in;
    Str old;
    Str new_;
    Int n;
    Str out;
} ReplaceTest;

typedef struct TitleTest {
    Str in;
    Str out;
} TitleTest;

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

typedef struct EqualFoldTestsCase {
    Str s;
    Str t;
    bool out;
} EqualFoldTestsCase;

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

typedef struct ContainsTestsCase {
    Str b;
    Str subslice;
    bool want;
} ContainsTestsCase;

typedef struct ContainsAnyTestsCase {
    Str b;
    Str substr;
    bool expected;
} ContainsAnyTestsCase;

typedef struct ContainsRuneTestsCase {
    Str b;
    Rune r;
    bool expected;
} ContainsRuneTestsCase;

static const LinesTest linesTests[] = {
    {.a = SI("abc\nabc\n"), .b = (const Str[]){SI("abc\n"), SI("abc\n")}, .b_len = 2},
    {.a = SI("abc\r\nabc"), .b = (const Str[]){SI("abc\r\n"), SI("abc")}, .b_len = 2},
    {.a = SI("abc\r\n"), .b = (const Str[]){SI("abc\r\n")}, .b_len = 1},
    {.a = SI("\nabc"), .b = (const Str[]){SI("\n"), SI("abc")}, .b_len = 2},
    {.a = SI("\nabc\n\n"),
     .b = (const Str[]){SI("\n"), SI("abc\n"), SI("\n")},
     .b_len = 3},
};

static const BinOpTest indexTests[] = {
    {SI(""), SI(""), 0},
    {SI(""), SI("a"), -1},
    {SI(""), SI("foo"), -1},
    {SI("fo"), SI("foo"), -1},
    {SI("foo"), SI("baz"), -1},
    {SI("foo"), SI("foo"), 0},
    {SI("oofofoofooo"), SI("f"), 2},
    {SI("oofofoofooo"), SI("foo"), 4},
    {SI("barfoobarfoo"), SI("foo"), 3},
    {SI("foo"), SI(""), 0},
    {SI("foo"), SI("o"), 1},
    {SI("abcABCabc"), SI("A"), 3},
    {SI(""), SI("a"), -1},
    {SI("x"), SI("a"), -1},
    {SI("x"), SI("x"), 0},
    {SI("abc"), SI("a"), 0},
    {SI("abc"), SI("b"), 1},
    {SI("abc"), SI("c"), 2},
    {SI("abc"), SI("x"), -1},
    {SI("barfoobarfooyyyzzzyyyzzzyyyzzzyyyxxxzzzyyy"), SI("x"), 33},
    {SI("fofofofooofoboo"), SI("oo"), 7},
    {SI("fofofofofofoboo"), SI("ob"), 11},
    {SI("fofofofofofoboo"), SI("boo"), 12},
    {SI("fofofofofofoboo"), SI("oboo"), 11},
    {SI("fofofofofoooboo"), SI("fooo"), 8},
    {SI("fofofofofofoboo"), SI("foboo"), 10},
    {SI("fofofofofofoboo"), SI("fofob"), 8},
    {SI("fofofofofofofoffofoobarfoo"), SI("foffof"), 12},
    {SI("fofofofofoofofoffofoobarfoo"), SI("foffof"), 13},
    {SI("fofofofofofofoffofoobarfoo"), SI("foffofo"), 12},
    {SI("fofofofofoofofoffofoobarfoo"), SI("foffofo"), 13},
    {SI("fofofofofoofofoffofoobarfoo"), SI("foffofoo"), 13},
    {SI("fofofofofofofoffofoobarfoo"), SI("foffofoo"), 12},
    {SI("fofofofofoofofoffofoobarfoo"), SI("foffofoob"), 13},
    {SI("fofofofofofofoffofoobarfoo"), SI("foffofoob"), 12},
    {SI("fofofofofoofofoffofoobarfoo"), SI("foffofooba"), 13},
    {SI("fofofofofofofoffofoobarfoo"), SI("foffofooba"), 12},
    {SI("fofofofofoofofoffofoobarfoo"), SI("foffofoobar"), 13},
    {SI("fofofofofofofoffofoobarfoo"), SI("foffofoobar"), 12},
    {SI("fofofofofoofofoffofoobarfoo"), SI("foffofoobarf"), 13},
    {SI("fofofofofofofoffofoobarfoo"), SI("foffofoobarf"), 12},
    {SI("fofofofofoofofoffofoobarfoo"), SI("foffofoobarfo"), 13},
    {SI("fofofofofofofoffofoobarfoo"), SI("foffofoobarfo"), 12},
    {SI("fofofofofoofofoffofoobarfoo"), SI("foffofoobarfoo"), 13},
    {SI("fofofofofofofoffofoobarfoo"), SI("foffofoobarfoo"), 12},
    {SI("fofofofofoofofoffofoobarfoo"), SI("ofoffofoobarfoo"), 12},
    {SI("fofofofofofofoffofoobarfoo"), SI("ofoffofoobarfoo"), 11},
    {SI("fofofofofoofofoffofoobarfoo"), SI("fofoffofoobarfoo"), 11},
    {SI("fofofofofofofoffofoobarfoo"), SI("fofoffofoobarfoo"), 10},
    {SI("fofofofofoofofoffofoobarfoo"), SI("foobars"), -1},
    {SI("foofyfoobarfoobar"), SI("y"), 4},
    {SI("oooooooooooooooooooooo"), SI("r"), -1},
    {SI("oxoxoxoxoxoxoxoxoxoxoxoy"), SI("oy"), 22},
    {SI("oxoxoxoxoxoxoxoxoxoxoxox"), SI("oy"), -1},
    {SI("000000000000000000000000000000000000000000000000000000000000000000000001"),
     SI("0000000000000000000000000000000000000000000000000000000000000000001"), 5},
    {SI("oxoxoxoxoxoxoxoxoxoxox☺"), SI("☺"), 22},
    {SI("xx0123456789012345678901234567890123456789012345678901234567890120123456789012"
        "345678901234567890123456xxx\xed\x9f\xc0"),
     SI("\xed\x9f\xc0"), 105},
};

static const BinOpTest lastIndexTests[] = {
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

static const BinOpTest indexAnyTests[] = {
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

static const BinOpTest lastIndexAnyTests[] = {
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

static const BinOpTest last_index_byte_testCases[] = {
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
    {SI("𠀳𠀗𠀾𠁄𠀧𠁆𠁂𠀫𠀖𠀪𠀲𠀴𠁀𠀨𠀿"), 0x2003f, 56},
    {SI("ӆ"), 0x4c6, 0},
    {SI("a"), 0x4c6, -1},
    {SI("  ӆ"), 0x4c6, 2},
    {SI("  a"), 0x4c6, -1},
    {SI("ццццццццццццццццццццццццццццццццццццццццццццццццццццццццццццццццӆ"), 0x4c6,
     128},
    {SI("цццццццццццццццццццццццццццццццццццццццццццццццццццццццццццццццц"), 0x4c6, -1},
    {SI("Ꚁ"), 0xa680, 0},
    {SI("a"), 0xa680, -1},
    {SI("  Ꚁ"), 0xa680, 2},
    {SI("  a"), 0xa680, -1},
    {SI("ꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꚀ"), 0xa680,
     192},
    {SI("ꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꙀꚀ"), 0x4680,
     -1},
    {SI("𡌀"), 0x21300, 0},
    {SI("a"), 0x21300, -1},
    {SI("  𡌀"), 0x21300, 2},
    {SI("  a"), 0x21300, -1},
    {SI("𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀"
        "𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡌀"),
     0x21300, 256},
    {SI("𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀"
        "𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡋀𡌀"),
     0x23300, -1},
    {SI("�"), 0xfffd, 0},
    {SI("\xff"), 0xfffd, 0},
    {SI("☻x�"), 0xfffd, 4},
    {SI("☻x\xe2\x98"), 0xfffd, 4},
    {SI("☻x\xe2\x98�"), 0xfffd, 4},
    {SI("☻x\xe2\x98x"), 0xfffd, 4},
    {SI("a☺b☻c☹d\xe2\x98�\xff�\xed\xa0\x80"), -1, -1},
    {SI("a☺b☻c☹d\xe2\x98�\xff�\xed\xa0\x80"), 0xd800, -1},
    {SI("a☺b☻c☹d\xe2\x98�\xff�\xed\xa0\x80"), 0x110000, -1},
    {SI("aaaaaKKKK\xf2\xbc\x84\x84"), 0xbc104, 17},
    {SI("aaaaaKKKK鄄"), 0x9104, 17},
    {SI("aaKKKKKa\xf2\xbc\x84\x84"), 0xbc104, 18},
    {SI("aaKKKKKa鄄"), 0x9104, 18},
};

static const Int windows[] = {
    1, 2, 3, 4, 15, 16, 17, 31, 32, 33, 63, 64, 65, 128,
};

static const SplitTest splittests[] = {
    {SI(""), SI(""), -1, NULL, 0},
    {SI("abcd"), SI("a"), 0, NULL, 0},
    {SI("abcd"), SI(""), 2, (const Str[]){SI("a"), SI("bcd")}, 2},
    {SI("abcd"), SI("a"), -1, (const Str[]){SI(""), SI("bcd")}, 2},
    {SI("abcd"), SI("z"), -1, (const Str[]){SI("abcd")}, 1},
    {SI("abcd"), SI(""), -1, (const Str[]){SI("a"), SI("b"), SI("c"), SI("d")}, 4},
    {SI("1,2,3,4"), SI(","), -1, (const Str[]){SI("1"), SI("2"), SI("3"), SI("4")}, 4},
    {SI("1....2....3....4"), SI("..."), -1,
     (const Str[]){SI("1"), SI(".2"), SI(".3"), SI(".4")}, 4},
    {SI("☺☻☹"), SI("☹"), -1, (const Str[]){SI("☺☻"), SI("")}, 2},
    {SI("☺☻☹"), SI("~"), -1, (const Str[]){SI("☺☻☹")}, 1},
    {SI("☺☻☹"), SI(""), -1, (const Str[]){SI("☺"), SI("☻"), SI("☹")}, 3},
    {SI("1 2 3 4"), SI(" "), 3, (const Str[]){SI("1"), SI("2"), SI("3 4")}, 3},
    {SI("1 2"), SI(" "), 3, (const Str[]){SI("1"), SI("2")}, 2},
    {SI("123"), SI(""), 2, (const Str[]){SI("1"), SI("23")}, 2},
    {SI("123"), SI(""), 17, (const Str[]){SI("1"), SI("2"), SI("3")}, 3},
    {SI("bT"), SI("T"), INT64_C(2305843009213693951), (const Str[]){SI("b"), SI("")},
     2},
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
    {SI("☺☻☹"), (const Str[]){SI("☺☻☹")}, 1},
};

static const FieldsTest fields_func_fieldsFuncTests[] = {
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
    {SI("LONGⱯSTRINGⱯWITHⱯNONASCIIⱯCHARS"), SI("longɐstringɐwithɐnonasciiɐchars")},
    {SI("ⱭⱭⱭⱭⱭ"), SI("ɑɑɑɑɑ")},
    {SI("A\xc2\x80\xf4\x8f\xbf\xbf"), SI("a\xc2\x80\xf4\x8f\xbf\xbf")},
};

static const StringTest trimSpaceTests[] = {
    {SI(""), {0}},
    {SI("  a"), SI("a")},
    {SI("b  "), SI("b")},
    {SI("abc"), SI("abc")},
    {SI("\t\v\r\f\n\xc2\x85\xc2\xa0\xe2\x80\x80\xe3\x80\x80"
        "abc\t\v\r\f\n\xc2\x85\xc2\xa0\xe2\x80\x80\xe3\x80\x80"),
     SI("abc")},
    {SI(" "), {0}},
    {SI("\xe3\x80\x80 "), {0}},
    {SI(" \xe3\x80\x80"), {0}},
    {SI(" \t\r\n \t\t\r\r\n\n "), {0}},
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

static const RepeatTest RepeatTests[] = {
    {SI(""), SI(""), 0},
    {SI(""), SI(""), 1},
    {SI(""), SI(""), 2},
    {SI("-"), SI(""), 0},
    {SI("-"), SI("-"), 1},
    {SI("-"), SI("----------"), 10},
    {SI("abc "), SI("abc abc abc "), 3},
};

static const RunesTest RunesTests[] = {
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

static const TrimTest trimTests[] = {
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

static const TrimNilTest trimNilTests[] = {
    {SI("Trim"), {0}, SI(""), {0}},
    {SI("Trim"), SI(""), SI(""), {0}},
    {SI("Trim"), SI("a"), SI("a"), {0}},
    {SI("Trim"), SI("aa"), SI("a"), {0}},
    {SI("Trim"), SI("a"), SI("ab"), {0}},
    {SI("Trim"), SI("ab"), SI("ab"), {0}},
    {SI("Trim"), SI("☺"), SI("☺"), {0}},
    {SI("TrimLeft"), {0}, SI(""), {0}},
    {SI("TrimLeft"), SI(""), SI(""), {0}},
    {SI("TrimLeft"), SI("a"), SI("a"), {0}},
    {SI("TrimLeft"), SI("aa"), SI("a"), {0}},
    {SI("TrimLeft"), SI("a"), SI("ab"), {0}},
    {SI("TrimLeft"), SI("ab"), SI("ab"), {0}},
    {SI("TrimLeft"), SI("☺"), SI("☺"), {0}},
    {SI("TrimRight"), {0}, SI(""), {0}},
    {SI("TrimRight"), SI(""), SI(""), SI("")},
    {SI("TrimRight"), SI("a"), SI("a"), SI("")},
    {SI("TrimRight"), SI("aa"), SI("a"), SI("")},
    {SI("TrimRight"), SI("a"), SI("ab"), SI("")},
    {SI("TrimRight"), SI("ab"), SI("ab"), SI("")},
    {SI("TrimRight"), SI("☺"), SI("☺"), SI("")},
    {SI("TrimPrefix"), {0}, SI(""), {0}},
    {SI("TrimPrefix"), SI(""), SI(""), SI("")},
    {SI("TrimPrefix"), SI("a"), SI("a"), SI("")},
    {SI("TrimPrefix"), SI("☺"), SI("☺"), SI("")},
    {SI("TrimSuffix"), {0}, SI(""), {0}},
    {SI("TrimSuffix"), SI(""), SI(""), SI("")},
    {SI("TrimSuffix"), SI("a"), SI("a"), SI("")},
    {SI("TrimSuffix"), SI("☺"), SI("☺"), SI("")},
};

static const TrimFuncTest trimFuncTests[] = {
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
    {P_IS_SPACE, SI(""), {0}, {0}, SI("")},
    {P_IS_SPACE, SI(" "), {0}, {0}, SI("")},
};

static const IndexFuncTest indexFuncTests[] = {
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
};

static const ReplaceTest ReplaceTests[] = {
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

static const TitleTest TitleTests[] = {
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

static const TitleTest ToTitleTests[] = {
    {SI(""), SI("")},
    {SI("a"), SI("A")},
    {SI(" aaa aaa aaa "), SI(" AAA AAA AAA ")},
    {SI(" Aaa Aaa Aaa "), SI(" AAA AAA AAA ")},
    {SI("123a456"), SI("123A456")},
    {SI("double-blind"), SI("DOUBLE-BLIND")},
    {SI("ÿøû"), SI("ŸØÛ")},
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

static const ContainsTestsCase containsTests[] = {
    {SI("hello"), SI("hel"), true},
    {SI("日本語"), SI("日本"), true},
    {SI("hello"), SI("Hello, world"), false},
    {SI("東京"), SI("京東"), false},
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
    {SI(DOTS DOTS DOTS), SI(" "), false},
};

static const ContainsRuneTestsCase ContainsRuneTests[] = {
    {SI(""), 'a', false},          {SI("a"), 'a', true},
    {SI("aaa"), 'a', true},        {SI("abc"), 'y', false},
    {SI("abc"), 'c', true},        {SI("a☺b☻c☹d"), 'x', false},
    {SI("a☺b☻c☹d"), 0x263b, true}, {SI("aRegExp*"), '*', true},
};

/* Go's Equal tests share compare_test.go's table. */
static const struct {
    const char *a, *b;
    Int i;
} compareTests[] = {
    {"", "", 0},
    {"a", "", 1},
    {"", "a", -1},
    {"abc", "abc", 0},
    {"abd", "abc", 1},
    {"abc", "abd", -1},
    {"ab", "abc", -1},
    {"abc", "ab", 1},
    {"x", "ab", 1},
    {"ab", "x", -1},
    {"x", "a", 1},
    {"b", "x", -1},
    /* test runtime·memeq's chunked implementation */
    {"abcdefgh", "abcdefgh", 0},
    {"abcdefghi", "abcdefghi", 0},
    {"abcdefghi", "abcdefghj", -1},
    {"abcdefghj", "abcdefghi", 1},
    /* nil tests */
    {NULL, NULL, 0},
    {"", NULL, 0},
    {NULL, "", 0},
    {"a", NULL, 1},
    {NULL, "a", -1},
};

/* ------------------------------------------------------------ helpers */

/* []byte(s) without the copy, for the functions that only read. A Str of {0}
 * is a nil slice. */
static Slice b(Str s) {
    if (s.p == NULL)
        return slice_nil(TYPE_BYTE);
    return slice_from((void *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE);
}

static Slice cb(const char *text) {
    return text == NULL ? slice_nil(TYPE_BYTE) : b(str_from_cstr(text));
}

/* []byte(s) with the copy, for the tests that write to it or check that
 * nothing did. */
static Slice bcopy_of(Alloc *a, Str s) {
    Slice out = slice_make(a, TYPE_BYTE, s.len, s.len);
    if (s.len > 0)
        memcpy(out.p, s.p, (size_t)s.len);
    return out;
}

/* string(s), for printing with %q and comparing. */
static Str q(Slice s) {
    return str_from_bytes(s.p, s.len);
}

static Byte *bp(Slice s) {
    return (Byte *)s.p;
}

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

/* sliceOfString: a [][]byte as a []string, pointing into the same bytes. */
static Slice sos(Alloc *a, Slice s) {
    Slice out = slice_make(a, TYPE_STRING, s.len, s.len);
    for (Int i = 0; i < s.len; i++)
        BURROW_AT(Str, out, i) = q(BURROW_AT(Slice, s, i));
    return out;
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
    Collector c = {a, slice_nil(TYPE_BYTES)};
    BURROW_CALLF(seq, BURROW_FN(IterYield, collect_yield, &c));
    return c.out;
}

/* The collect in Go's test, which runs the seq twice and wants the same. */
static Slice collect_twice(TestingT *t, Alloc *a, IterSeq seq) {
    Slice out = collect(a, seq);
    Slice out1 = collect(a, seq);
    if (!slices_eq(sos(a, out), sos(a, out1)))
        testing_t_fatalf_v(t, "inconsistent seq:\n%q\n%q", sos(a, out), sos(a, out1));
    return out;
}

/* Go's
 *
 *     var x []byte
 *     for _, v := range a {
 *         x = append(v, 'z')
 *     }
 *
 * which writes over the next piece if a piece has room past its end. */
static Slice append_z(Alloc *al, Slice a) {
    Slice x = slice_nil(TYPE_BYTE);
    static const Byte z = 'z';
    for (Int i = 0; i < a.len; i++)
        x = slice_append(al, BURROW_AT(Slice, a, i), &z, 1);
    return x;
}

static Str concat(Alloc *a, Str x, Str y) {
    Byte *p = (Byte *)mem_alloc(a, (size_t)(x.len + y.len + 1), 1);
    if (x.len > 0)
        memcpy(p, x.p, (size_t)x.len);
    if (y.len > 0)
        memcpy(p + x.len, y.p, (size_t)y.len);
    return str_from_bytes(p, x.len + y.len);
}

static Str encode(Alloc *a, const Rune *r, Int n) {
    Byte *p = (Byte *)mem_alloc(a, (size_t)(n * UTF8_UTF_MAX + 1), 1);
    Int w = 0;
    for (Int i = 0; i < n; i++)
        w += utf8_encode_rune(slice_from(p + w, UTF8_UTF_MAX, UTF8_UTF_MAX, TYPE_BYTE),
                              r[i]);
    return str_from_bytes(p, w);
}

static bool recovered(void (*f)(void *), void *env) {
    volatile bool got = false;
    BURROW_TRY {
        f(env);
    }
    BURROW_CATCH(r) {
        (void)r;
        got = true;
    }
    BURROW_TRY_END;
    return got;
}

/* ------------------------------------------------------------ tests */

static void TestLines(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < LEN(linesTests); i++) {
        const LinesTest *s = &linesTests[i];
        Slice result = sos(a, collect(a, bytes_lines(a, b(s->a))));
        if (!strs_eq(result, s->b, s->b_len))
            testing_t_errorf_v(t, "slices.Collect(Lines(%q)) = %q; want %q", s->a,
                               result, strs(s->b, s->b_len));
    }
    arena_free(&ar);
}

static void TestEqual(TestingT *t) {
    for (Int i = 0; i < LEN(compareTests); i++) {
        Slice a = cb(compareTests[i].a);
        Slice bb = cb(compareTests[i].b);
        bool eql = bytes_equal(a, bb);
        if (eql != (compareTests[i].i == 0))
            testing_t_errorf_v(t, "Equal(%q, %q) = %v", q(a), q(bb), eql);
    }
}

static void TestEqualExhaustive(TestingT *t) {
    Int size = 128;
    if (testing_short())
        size = 32;
    Byte a[128], bb[128], b_init[128];
    for (Int i = 0; i < size; i++) {
        a[i] = (Byte)(17 * i);
        b_init[i] = (Byte)(23 * i + 100);
    }
    for (Int len = 0; len <= size; len++) {
        for (Int x = 0; x <= size - len; x++) {
            for (Int y = 0; y <= size - len; y++) {
                memcpy(bb, b_init, (size_t)size);
                memcpy(bb + y, a + x, (size_t)len);
                Slice sa = slice_from(a + x, len, len, TYPE_BYTE);
                Slice sb = slice_from(bb + y, len, len, TYPE_BYTE);
                if (!bytes_equal(sa, sb) || !bytes_equal(sb, sa))
                    testing_t_errorf_v(t, "Equal(%d, %d, %d) = false", len, x, y);
            }
        }
    }
}

/* make sure Equal returns false for minimally different strings. The data
 * is all zeros except for a single one in one location. */
static void TestNotEqual(TestingT *t) {
    Int size = 128;
    if (testing_short())
        size = 32;
    Byte a[128] = {0}, bb[128] = {0};
    for (Int len = 0; len <= size; len++) {
        for (Int x = 0; x <= size - len; x++) {
            for (Int y = 0; y <= size - len; y++) {
                for (Int diffpos = x; diffpos < x + len; diffpos++) {
                    a[diffpos] = 1;
                    Slice sa = slice_from(a + x, len, len, TYPE_BYTE);
                    Slice sb = slice_from(bb + y, len, len, TYPE_BYTE);
                    if (bytes_equal(sa, sb) || bytes_equal(sb, sa))
                        testing_t_errorf_v(t, "NotEqual(%d, %d, %d, %d) = true", len, x,
                                           y, diffpos);
                    a[diffpos] = 0;
                }
            }
        }
    }
}

static void run_index_tests(TestingT *t, Int (*f)(Slice, Slice), const char *name,
                            const BinOpTest *tests, Int n) {
    for (Int i = 0; i < n; i++) {
        const BinOpTest *test = &tests[i];
        Int actual = f(b(test->a), b(test->b));
        if (actual != test->i)
            testing_t_errorf_v(t, "%s(%q,%q) = %v; want %v", name, test->a, test->b,
                               actual, test->i);
    }
    static const struct {
        const char *a, *b;
        Int i;
    } allocTests[] = {
        /* case for function Index. */
        {"000000000000000000000000000000000000000000000000000000000000000000000001",
         "0000000000000000000000000000000000000000000000000000000000000000001", 5},
        /* case for function LastIndex. */
        {"000000000000000000000000000000000000000000000000000000000000000010000",
         "00000000000000000000000000000000000000000000000000000000000001", 3},
    };
    Int i = bytes_index(cb(allocTests[1].a), cb(allocTests[1].b));
    if (i != allocTests[1].i)
        testing_t_errorf_v(t, "Index([]byte(%q), []byte(%q)) = %v; want %v",
                           allocTests[1].a, allocTests[1].b, i, allocTests[1].i);
    i = bytes_last_index(cb(allocTests[0].a), cb(allocTests[0].b));
    if (i != allocTests[0].i)
        testing_t_errorf_v(t, "LastIndex([]byte(%q), []byte(%q)) = %v; want %v",
                           allocTests[0].a, allocTests[0].b, i, allocTests[0].i);
}

static void run_index_any_tests(TestingT *t, Int (*f)(Slice, Str), const char *name,
                                const BinOpTest *tests, Int n) {
    for (Int i = 0; i < n; i++) {
        const BinOpTest *test = &tests[i];
        Int actual = f(b(test->a), test->b);
        if (actual != test->i)
            testing_t_errorf_v(t, "%s(%q,%q) = %v; want %v", name, test->a, test->b,
                               actual, test->i);
    }
}

static void TestIndex(TestingT *t) {
    run_index_tests(t, bytes_index, "Index", indexTests, LEN(indexTests));
}

static void TestLastIndex(TestingT *t) {
    run_index_tests(t, bytes_last_index, "LastIndex", lastIndexTests,
                    LEN(lastIndexTests));
}

static void TestIndexAny(TestingT *t) {
    run_index_any_tests(t, bytes_index_any, "IndexAny", indexAnyTests,
                        LEN(indexAnyTests));
}

static void TestLastIndexAny(TestingT *t) {
    run_index_any_tests(t, bytes_last_index_any, "LastIndexAny", lastIndexAnyTests,
                        LEN(lastIndexAnyTests));
}

static void TestIndexByte(TestingT *t) {
    for (Int i = 0; i < LEN(indexTests); i++) {
        const BinOpTest *tt = &indexTests[i];
        if (tt->b.len != 1)
            continue;
        Byte c = tt->b.p[0];
        Int pos = bytes_index_byte(b(tt->a), c);
        if (pos != tt->i)
            testing_t_errorf_v(t, "IndexByte(%q, '%c') = %v", tt->a, (Rune)c, pos);
    }
}

static void TestLastIndexByte(TestingT *t) {
    for (Int i = 0; i < LEN(last_index_byte_testCases); i++) {
        const BinOpTest *test = &last_index_byte_testCases[i];
        Int actual = bytes_last_index_byte(b(test->a), test->b.p[0]);
        if (actual != test->i)
            testing_t_errorf_v(t, "LastIndexByte(%q,%c) = %v; want %v", test->a,
                               (Rune)test->b.p[0], actual, test->i);
    }
}

static void index_byte_window(TestingT *t, Byte *b1, Int n) {
    Slice s = slice_from(b1, n, n, TYPE_BYTE);
    for (Int j = 0; j < n; j++) {
        b1[j] = 'x';
        Int pos = bytes_index_byte(s, 'x');
        if (pos != j)
            testing_t_errorf_v(t, "IndexByte(%q, 'x') = %v", q(s), pos);
        b1[j] = 0;
        pos = bytes_index_byte(s, 'x');
        if (pos != -1)
            testing_t_errorf_v(t, "IndexByte(%q, 'x') = %v", q(s), pos);
    }
}

/* test a larger buffer with different sizes and alignments */
static void TestIndexByteBig(TestingT *t) {
    Int n = 1024;
    if (testing_short())
        n = 128;
    static Byte bb[1024];
    memset(bb, 0, sizeof bb);
    for (Int i = 0; i < n; i++) {
        /* different start alignments */
        index_byte_window(t, bb + i, n - i);
        /* different end alignments */
        index_byte_window(t, bb, i);
        /* different start and end alignments */
        index_byte_window(t, bb + i / 2, n - (i + 1) / 2 - i / 2);
    }
}

/* test a small index across all page offsets */
static void TestIndexByteSmall(TestingT *t) {
    static Byte bb[5015];
    Int n = LEN(bb);
    memset(bb, 0, sizeof bb);
    /* Make sure we find the correct byte even when straddling a page. */
    for (Int i = 0; i <= n - 15; i++) {
        for (Int j = 0; j < 15; j++)
            bb[i + j] = (Byte)(100 + j);
        Slice s = slice_from(bb + i, 15, 15, TYPE_BYTE);
        for (Int j = 0; j < 15; j++) {
            Int p = bytes_index_byte(s, (Byte)(100 + j));
            if (p != j)
                testing_t_errorf_v(t, "IndexByte(%q, %d) = %d", q(s), 100 + j, p);
        }
        memset(bb + i, 0, 15);
    }
    /* Make sure matches outside the slice never trigger. */
    for (Int i = 0; i <= n - 15; i++) {
        memset(bb + i, 1, 15);
        Slice s = slice_from(bb + i, 15, 15, TYPE_BYTE);
        for (Int j = 0; j < 15; j++) {
            Int p = bytes_index_byte(s, 0);
            if (p != -1)
                testing_t_errorf_v(t, "IndexByte(%q, %d) = %d", q(s), 0, p);
        }
        memset(bb + i, 0, 15);
    }
}

static void TestIndexRune(TestingT *t) {
    for (Int i = 0; i < LEN(index_rune_tests); i++) {
        const IndexRuneTestsCase *tt = &index_rune_tests[i];
        Int got = bytes_index_rune(b(tt->in), tt->rune);
        if (got != tt->want)
            testing_t_errorf_v(t, "IndexRune(%q, %d) = %v; want %v", tt->in, tt->rune,
                               got, tt->want);
    }

    Slice haystack = b(S("test世界"));
    Int i = bytes_index_rune(haystack, 's');
    if (i != 2)
        testing_t_fatalf_v(t, "'s' at %d; want 2", i);
    i = bytes_index_rune(haystack, 0x4e16);
    if (i != 4)
        testing_t_fatalf_v(t, "'世' at %d; want 4", i);
}

/* test count of a single byte across page offsets */
static void TestCountByte(TestingT *t) {
    static Byte bb[5015];
    Int n = LEN(bb);
    memset(bb, 0, sizeof bb);
    Int max_wnd = windows[LEN(windows) - 1];
    static const Byte hundred = 100;
    Slice sep = slice_from((void *)(uintptr_t)&hundred, 1, 1, TYPE_BYTE);
    for (int pass = 0; pass < 2; pass++) {
        Int from = pass == 0 ? 0 : 4096 - (max_wnd + 1);
        Int to = pass == 0 ? 2 * max_wnd + 1 : n;
        for (Int i = from; i < to; i++) {
            for (Int w = 0; w < LEN(windows); w++) {
                Int window = windows[w];
                if (window > n - i)
                    window = n - i;
                Slice s = slice_from(bb + i, window, window, TYPE_BYTE);
                for (Int j = 0; j < window; j++) {
                    bb[i + j] = 100;
                    Int p = bytes_count(s, sep);
                    if (p != j + 1)
                        testing_t_errorf_v(t, "TestCountByte.Count(%q, 100) = %d", q(s),
                                           p);
                }
                memset(bb + i, 0, (size_t)window);
            }
        }
    }
}

/* Make sure we don't count bytes outside our window */
static void TestCountByteNoMatch(TestingT *t) {
    static Byte bb[5015];
    Int n = LEN(bb);
    memset(bb, 0, sizeof bb);
    static const Byte zero = 0;
    Slice sep = slice_from((void *)(uintptr_t)&zero, 1, 1, TYPE_BYTE);
    for (Int i = 0; i <= n; i++) {
        for (Int w = 0; w < LEN(windows); w++) {
            Int window = windows[w];
            if (window > n - i)
                window = n - i;
            memset(bb + i, 100, (size_t)window);
            Slice s = slice_from(bb + i, window, window, TYPE_BYTE);
            Int p = bytes_count(s, sep);
            if (p != 0)
                testing_t_errorf_v(t, "TestCountByteNoMatch(%q, 0) = %d", q(s), p);
            memset(bb + i, 0, (size_t)window);
        }
    }
}

static void check_last_z(TestingT *t, Alloc *a, Slice x, const Str *want, Int n) {
    Str w = concat(a, want[n - 1], S("z"));
    if (!str_eq(q(x), w))
        testing_t_errorf_v(t, "last appended result was %s; want %s", q(x), w);
}

static void TestSplit(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < LEN(splittests); i++) {
        const SplitTest *tt = &splittests[i];
        Slice got = bytes_split_n(a, bcopy_of(a, tt->s), b(tt->sep), tt->n);

        /* Appending to the results should not change future results. */
        Slice x = append_z(a, got);

        Slice result = sos(a, got);
        if (!strs_eq(result, tt->a, tt->a_len)) {
            testing_t_errorf_v(t, "Split(%q, %q, %d) = %v; want %v", tt->s, tt->sep,
                               tt->n, result, strs(tt->a, tt->a_len));
            continue;
        }
        if (tt->n < 0) {
            Slice b2 = sos(a, collect(a, bytes_split_seq(a, b(tt->s), b(tt->sep))));
            if (!strs_eq(b2, tt->a, tt->a_len))
                testing_t_errorf_v(t, "collect(SplitSeq(%q, %q)) = %v; want %v", tt->s,
                                   tt->sep, b2, strs(tt->a, tt->a_len));
        }
        if (tt->n == 0 || got.len == 0)
            continue;
        check_last_z(t, a, x, tt->a, tt->a_len);

        Slice s = bytes_join(a, got, b(tt->sep));
        if (!str_eq(q(s), tt->s))
            testing_t_errorf_v(t, "Join(Split(%q, %q, %d), %q) = %q", tt->s, tt->sep,
                               tt->n, tt->sep, q(s));
        if (tt->n < 0) {
            Slice b2 = sos(a, bytes_split(a, b(tt->s), b(tt->sep)));
            if (!slices_eq(result, b2))
                testing_t_errorf_v(
                    t, "Split disagrees with SplitN(%q, %q, %d) = %v; want %v", tt->s,
                    tt->sep, tt->n, b2, result);
        }
        Slice in = BURROW_AT(Slice, got, 0);
        if (in.len > 0 && in.p == s.p)
            testing_t_errorf_v(t, "Join(%v, %q) didn't copy", result, tt->sep);
    }
    arena_free(&ar);
}

static void TestSplitAfter(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < LEN(splitaftertests); i++) {
        const SplitTest *tt = &splitaftertests[i];
        Slice got = bytes_split_after_n(a, bcopy_of(a, tt->s), b(tt->sep), tt->n);

        /* Appending to the results should not change future results. */
        Slice x = append_z(a, got);

        Slice result = sos(a, got);
        if (!strs_eq(result, tt->a, tt->a_len)) {
            testing_t_errorf_v(t, "Split(%q, %q, %d) = %v; want %v", tt->s, tt->sep,
                               tt->n, result, strs(tt->a, tt->a_len));
            continue;
        }
        if (tt->n < 0) {
            Slice b2 =
                sos(a, collect(a, bytes_split_after_seq(a, b(tt->s), b(tt->sep))));
            if (!strs_eq(b2, tt->a, tt->a_len))
                testing_t_errorf_v(t, "collect(SplitAfterSeq(%q, %q)) = %v; want %v",
                                   tt->s, tt->sep, b2, strs(tt->a, tt->a_len));
        }
        check_last_z(t, a, x, tt->a, tt->a_len);

        Slice s = bytes_join(a, got, slice_nil(TYPE_BYTE));
        if (!str_eq(q(s), tt->s))
            testing_t_errorf_v(t, "Join(Split(%q, %q, %d), %q) = %q", tt->s, tt->sep,
                               tt->n, tt->sep, q(s));
        if (tt->n < 0) {
            Slice b2 = sos(a, bytes_split_after(a, b(tt->s), b(tt->sep)));
            if (!slices_eq(result, b2))
                testing_t_errorf_v(
                    t,
                    "SplitAfter disagrees with SplitAfterN(%q, %q, %d) = %v; want %v",
                    tt->s, tt->sep, tt->n, b2, result);
        }
    }
    arena_free(&ar);
}

static void check_fields(TestingT *t, Alloc *a, const FieldsTest *tt, const char *name,
                         Slice bb, Slice got, IterSeq seq) {
    /* Appending to the results should not change future results. */
    Slice x = append_z(a, got);

    Slice result = sos(a, got);
    if (!strs_eq(result, tt->a, tt->a_len)) {
        testing_t_errorf_v(t, "%s(%q) = %v; want %v", name, tt->s, result,
                           strs(tt->a, tt->a_len));
        return;
    }
    Slice result2 = sos(a, collect_twice(t, a, seq));
    if (!strs_eq(result2, tt->a, tt->a_len))
        testing_t_errorf_v(t, "collect(%sSeq(%q)) = %v; want %v", name, tt->s, result2,
                           strs(tt->a, tt->a_len));
    if (!str_eq(q(bb), tt->s))
        testing_t_errorf_v(t, "slice changed to %s; want %s", q(bb), tt->s);
    if (tt->a_len > 0)
        check_last_z(t, a, x, tt->a, tt->a_len);
}

static void TestFields(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < LEN(fieldstests); i++) {
        const FieldsTest *tt = &fieldstests[i];
        Slice bb = bcopy_of(a, tt->s);
        Slice got = bytes_fields(a, bb);
        check_fields(t, a, tt, "Fields", bb, got, bytes_fields_seq(a, b(tt->s)));
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
        Slice got = sos(a, bytes_fields_func(a, b(tt->s), pred((Predicate)P_IS_SPACE)));
        if (!strs_eq(got, tt->a, tt->a_len)) {
            testing_t_errorf_v(t, "FieldsFunc(%q, unicode.IsSpace) = %v; want %v",
                               tt->s, got, strs(tt->a, tt->a_len));
            continue;
        }
    }
    RuneFunc p = BURROW_FN(RuneFunc, is_x, NULL);
    for (Int i = 0; i < LEN(fields_func_fieldsFuncTests); i++) {
        const FieldsTest *tt = &fields_func_fieldsFuncTests[i];
        Slice bb = bcopy_of(a, tt->s);
        Slice got = bytes_fields_func(a, bb, p);
        check_fields(t, a, tt, "FieldsFunc", bb, got,
                     bytes_fields_func_seq(a, b(tt->s), p));
    }
    arena_free(&ar);
}

/* Test case for any function which accepts and returns a byte slice. For ease
 * of creation, we write the input byte slice as a string. */
static void run_string_tests(TestingT *t, Slice (*f)(Alloc *, Slice), const char *name,
                             const StringTest *tests, Int n) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < n; i++) {
        const StringTest *tc = &tests[i];
        Slice actual = f(a, b(tc->in));
        if (actual.p == NULL && tc->out.p != NULL)
            testing_t_errorf_v(t, "%s(%q) = nil; want %q", name, tc->in, tc->out);
        if (actual.p != NULL && tc->out.p == NULL)
            testing_t_errorf_v(t, "%s(%q) = %q; want nil", name, tc->in, q(actual));
        if (!bytes_equal(actual, b(tc->out)))
            testing_t_errorf_v(t, "%s(%q) = %q; want %q", name, tc->in, q(actual),
                               tc->out);
    }
    arena_free(&ar);
}

static Str ten_runes(Alloc *a, Rune r) {
    Rune runes[10];
    for (int i = 0; i < 10; i++)
        runes[i] = r;
    return encode(a, runes, 10);
}

/* User-defined self-inverse mapping function */
static Rune rot13(void *env, Rune r) {
    (void)env;
    const Rune step = 13;
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

static Rune invalid_rune(void *env, Rune r) {
    (void)env;
    (void)r;
    return UTF8_MAX_RUNE + 1;
}

#define MAPPING(f) BURROW_FN(RuneMapFunc, f, NULL)

static void TestMap(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *al = arena_allocator(&ar);

    /* Run a couple of awful growth/shrinkage tests */
    Str a = ten_runes(al, 'a');

    /* 1. Grow. This triggers two reallocations in Map. */
    Slice m = bytes_map(al, MAPPING(max_rune), b(a));
    Str expect = ten_runes(al, UNICODE_MAX_RUNE);
    if (!str_eq(q(m), expect))
        testing_t_errorf_v(t, "growing: expected %q got %q", expect, q(m));

    /* 2. Shrink */
    m = bytes_map(al, MAPPING(min_rune), b(ten_runes(al, UNICODE_MAX_RUNE)));
    expect = a;
    if (!str_eq(q(m), expect))
        testing_t_errorf_v(t, "shrinking: expected %q got %q", expect, q(m));

    /* 3. Rot13 */
    m = bytes_map(al, MAPPING(rot13), b(S("a to zed")));
    expect = S("n gb mrq");
    if (!str_eq(q(m), expect))
        testing_t_errorf_v(t, "rot13: expected %q got %q", expect, q(m));

    /* 4. Rot13^2 */
    m = bytes_map(al, MAPPING(rot13), bytes_map(al, MAPPING(rot13), b(S("a to zed"))));
    expect = S("a to zed");
    if (!str_eq(q(m), expect))
        testing_t_errorf_v(t, "rot13: expected %q got %q", expect, q(m));

    /* 5. Drop */
    m = bytes_map(al, MAPPING(drop_not_latin), b(S("Hello, 세계")));
    expect = S("Hello");
    if (!str_eq(q(m), expect))
        testing_t_errorf_v(t, "drop: expected %q got %q", expect, q(m));

    /* 6. Invalid rune */
    m = bytes_map(al, MAPPING(invalid_rune), b(S("x")));
    expect = S("\xef\xbf\xbd");
    if (!str_eq(q(m), expect))
        testing_t_errorf_v(t, "invalidRune: expected %q got %q", expect, q(m));
    arena_free(&ar);
}

static void TestToUpper(TestingT *t) {
    run_string_tests(t, bytes_to_upper, "ToUpper", upperTests, LEN(upperTests));
}

static void TestToLower(TestingT *t) {
    run_string_tests(t, bytes_to_lower, "ToLower", lowerTests, LEN(lowerTests));
}

static void TestToValidUTF8(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < LEN(toValidUTF8Tests); i++) {
        const ToValidUTF8TestsCase *tc = &toValidUTF8Tests[i];
        Slice got = bytes_to_valid_utf8(a, b(tc->in), b(tc->repl));
        if (!bytes_equal(got, b(tc->out)))
            testing_t_errorf_v(t, "ToValidUTF8(%q, %q) = %q; want %q", tc->in, tc->repl,
                               q(got), tc->out);
    }
    arena_free(&ar);
}

static Slice trim_space(Alloc *a, Slice s) {
    (void)a;
    return bytes_trim_space(s);
}

static void TestTrimSpace(TestingT *t) {
    run_string_tests(t, trim_space, "TrimSpace", trimSpaceTests, LEN(trimSpaceTests));
}

static void TestRepeat(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < LEN(RepeatTests); i++) {
        const RepeatTest *tt = &RepeatTests[i];
        Slice got = bytes_repeat(a, b(tt->in), tt->count);
        if (!bytes_equal(got, b(tt->out)))
            testing_t_errorf_v(t, "Repeat(%q, %d) = %q; want %q", tt->in, tt->count,
                               q(got), tt->out);
    }

    /* The two long cases in Go's table, built here. */
    Int n = 1 << 16;
    Slice zeros = slice_make(a, TYPE_BYTE, n, n);
    Slice got = bytes_repeat(a, b(S("\x00")), n);
    if (!bytes_equal(got, zeros))
        testing_t_errorf_v(t, "Repeat(%q, %d) = %d bytes, not all zero", S("\x00"), n,
                           got.len);
    Slice long_string = slice_make(a, TYPE_BYTE, n + 2, n + 2);
    bp(long_string)[0] = 'a';
    bp(long_string)[n + 1] = 'z';
    Slice want = slice_make(a, TYPE_BYTE, 2 * (n + 2), 2 * (n + 2));
    memcpy(bp(want), long_string.p, (size_t)(n + 2));
    memcpy(bp(want) + n + 2, long_string.p, (size_t)(n + 2));
    got = bytes_repeat(a, long_string, 2);
    if (!bytes_equal(got, want))
        testing_t_errorf_v(t, "Repeat(longString, 2) = %d bytes, want %d", got.len,
                           want.len);
    arena_free(&ar);
}

typedef struct RepeatCall {
    Alloc *a;
    Slice s;
    Int count;
} RepeatCall;

static void call_repeat(void *env) {
    RepeatCall *c = (RepeatCall *)env;
    bytes_repeat(c->a, c->s, c->count);
}

/* The panic text from repeat, or the empty string if it did not panic. */
static Str repeat_panic(Alloc *a, Slice s, Int count) {
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
    struct {
        Slice s;
        Int count;
        const char *err_str;
    } tests[] = {
        {cb("--"), -2147483647, "negative"},
        {cb(""), BURROW_INT_MAX, ""},
        {cb("-"), 10, ""},
        {cb("gopher"), 0, ""},
        {cb("-"), -1, "negative"},
        {cb("--"), -102, "negative"},
        {slice_make(a, TYPE_BYTE, 255, 255), (Int)(UINTPTR_MAX / 255 + 1), "overflow"},
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
     * and Repeat gives an empty slice, as every function here does when it
     * runs out of memory. A fixed allocator refuses without asking malloc. */
    unsigned char buf[64];
    Fixed fx;
    fixed_init(&fx, buf, sizeof buf);
    Slice got = bytes_repeat(fixed_allocator(&fx), cb("-"), BURROW_INT_MAX);
    if (got.len != 0)
        testing_t_errorf_v(t, "Repeat(\"-\", maxInt) = %d bytes, want 0", got.len);
    arena_free(&ar);
}

static void TestRunes(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *al = arena_allocator(&ar);
    for (Int i = 0; i < LEN(RunesTests); i++) {
        const RunesTest *tt = &RunesTests[i];
        Slice a = bytes_runes(al, b(tt->in));
        bool eq = a.len == tt->out_len;
        for (Int k = 0; eq && k < a.len; k++)
            eq = BURROW_AT(Rune, a, k) == tt->out[k];
        if (!eq) {
            testing_t_errorf_v(t, "Runes(%q) = %v; want %v", tt->in, a,
                               slice_from((void *)(uintptr_t)tt->out, tt->out_len,
                                          tt->out_len, TYPE_RUNE));
            continue;
        }
        if (!tt->lossy) {
            /* can only test reassembly if we didn't lose information */
            Str s = encode(al, a.len > 0 ? (const Rune *)a.p : NULL, a.len);
            if (!str_eq(s, tt->in))
                testing_t_errorf_v(t, "string(Runes(%q)) = %x; want %x", tt->in, s,
                                   tt->in);
        }
    }
    arena_free(&ar);
}

/* The trim function a test case names, as either the kind taking a cutset
 * string or the kind taking a prefix or suffix. */
static bool to_fn(TestingT *t, Str name, Slice (**f)(Slice, Str),
                  Slice (**fb)(Slice, Slice)) {
    *f = NULL;
    *fb = NULL;
    if (str_eq(name, S("Trim")))
        *f = bytes_trim;
    else if (str_eq(name, S("TrimLeft")))
        *f = bytes_trim_left;
    else if (str_eq(name, S("TrimRight")))
        *f = bytes_trim_right;
    else if (str_eq(name, S("TrimPrefix")))
        *fb = bytes_trim_prefix;
    else if (str_eq(name, S("TrimSuffix")))
        *fb = bytes_trim_suffix;
    else
        testing_t_errorf_v(t, "Undefined trim function %s", name);
    return *f != NULL || *fb != NULL;
}

static Str report(Alloc *a, Slice s) {
    if (s.p == NULL)
        return S("nil");
    return fmt_sprintf_v(a, "%q", q(s));
}

static void TestTrim(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice (*f)(Slice, Str);
    Slice (*fb)(Slice, Slice);
    for (Int i = 0; i < LEN(trimTests); i++) {
        const TrimTest *tc = &trimTests[i];
        Str name = tc->f;
        if (!to_fn(t, name, &f, &fb))
            continue;
        Slice actual = f != NULL ? f(b(tc->in), tc->arg) : fb(b(tc->in), b(tc->arg));
        if (!str_eq(q(actual), tc->out))
            testing_t_errorf_v(t, "%s(%q, %q) = %q; want %q", name, tc->in, tc->arg,
                               q(actual), tc->out);
    }

    for (Int i = 0; i < LEN(trimNilTests); i++) {
        const TrimNilTest *tc = &trimNilTests[i];
        Str name = tc->f;
        if (!to_fn(t, name, &f, &fb))
            continue;
        Slice actual = f != NULL ? f(b(tc->in), tc->arg) : fb(b(tc->in), b(tc->arg));
        if (actual.len != 0) {
            testing_t_errorf_v(t, "%s(%s, %q) returned non-empty value", name,
                               report(a, b(tc->in)), tc->arg);
        } else {
            bool actual_nil = actual.p == NULL;
            bool out_nil = tc->out.p == NULL;
            if (actual_nil != out_nil)
                testing_t_errorf_v(t, "%s(%s, %q) got nil %t; want nil %t", name,
                                   report(a, b(tc->in)), tc->arg, actual_nil, out_nil);
        }
    }
    arena_free(&ar);
}

static void TestTrimFunc(TestingT *t) {
    for (Int i = 0; i < LEN(trimFuncTests); i++) {
        const TrimFuncTest *tc = &trimFuncTests[i];
        struct {
            const char *name;
            Slice (*trim)(Slice s, RuneFunc f);
            Slice out;
        } trimmers[] = {
            {"TrimFunc", bytes_trim_func, b(tc->trimOut)},
            {"TrimLeftFunc", bytes_trim_left_func, b(tc->leftOut)},
            {"TrimRightFunc", bytes_trim_right_func, b(tc->rightOut)},
        };
        for (Int j = 0; j < LEN(trimmers); j++) {
            Slice actual = trimmers[j].trim(b(tc->in), pred(tc->f));
            if (actual.p == NULL && trimmers[j].out.p != NULL)
                testing_t_errorf_v(t, "%s(%q, %q) = nil; want %q", trimmers[j].name,
                                   tc->in, tc->f.name, q(trimmers[j].out));
            if (actual.p != NULL && trimmers[j].out.p == NULL)
                testing_t_errorf_v(t, "%s(%q, %q) = %q; want nil", trimmers[j].name,
                                   tc->in, tc->f.name, q(actual));
            if (!bytes_equal(actual, trimmers[j].out))
                testing_t_errorf_v(t, "%s(%q, %q) = %q; want %q", trimmers[j].name,
                                   tc->in, tc->f.name, q(actual), q(trimmers[j].out));
        }
    }
}

static void TestIndexFunc(TestingT *t) {
    for (Int i = 0; i < LEN(indexFuncTests); i++) {
        const IndexFuncTest *tc = &indexFuncTests[i];
        Int first = bytes_index_func(b(tc->in), pred(tc->f));
        if (first != tc->first)
            testing_t_errorf_v(t, "IndexFunc(%q, %s) = %d; want %d", tc->in, tc->f.name,
                               first, tc->first);
        Int last = bytes_last_index_func(b(tc->in), pred(tc->f));
        if (last != tc->last)
            testing_t_errorf_v(t, "LastIndexFunc(%q, %s) = %d; want %d", tc->in,
                               tc->f.name, last, tc->last);
    }
}

static void TestReplace(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *al = arena_allocator(&ar);
    for (Int i = 0; i < LEN(ReplaceTests); i++) {
        const ReplaceTest *tt = &ReplaceTests[i];
        /* Go wants at most one allocation, and a fixed allocator counts. */
        unsigned char buf[256];
        Fixed fx;
        fixed_init(&fx, buf, sizeof buf);
        Alloc *a = fixed_allocator(&fx);
        bytes_replace(a, b(tt->in), b(tt->old), b(tt->new_), tt->n);
        if (fx.allocs > 1)
            testing_t_errorf_v(t, "Replace(%q, %q, %q, %d) allocates %d objects",
                               tt->in, tt->old, tt->new_, tt->n, fx.allocs);

        /* Go's in = append(in, "<spare>"...)[:len(tt.in)], which gives the
         * input room to grow into, so a Replace that forgot to copy would
         * hand it back. */
        Slice in = slice_make(al, TYPE_BYTE, tt->in.len, tt->in.len + 7);
        if (tt->in.len > 0)
            memcpy(in.p, tt->in.p, (size_t)tt->in.len);
        memcpy(bp(in) + tt->in.len, "<spare>", 7);
        Slice out = bytes_replace(al, in, b(tt->old), b(tt->new_), tt->n);
        if (!str_eq(q(out), tt->out))
            testing_t_errorf_v(t, "Replace(%q, %q, %q, %d) = %q, want %q", tt->in,
                               tt->old, tt->new_, tt->n, q(out), tt->out);
        if (in.cap == out.cap && in.p == out.p)
            testing_t_errorf_v(t, "Replace(%q, %q, %q, %d) didn't copy", tt->in,
                               tt->old, tt->new_, tt->n);
        if (tt->n == -1) {
            out = bytes_replace_all(al, in, b(tt->old), b(tt->new_));
            if (!str_eq(q(out), tt->out))
                testing_t_errorf_v(t, "ReplaceAll(%q, %q, %q) = %q, want %q", tt->in,
                                   tt->old, tt->new_, q(out), tt->out);
        }
    }
    arena_free(&ar);
}

static void TestTitle(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < LEN(TitleTests); i++) {
        const TitleTest *tt = &TitleTests[i];
        Str s = q(bytes_title(a, b(tt->in)));
        if (!str_eq(s, tt->out))
            testing_t_errorf_v(t, "Title(%q) = %q, want %q", tt->in, s, tt->out);
    }
    arena_free(&ar);
}

static void TestToTitle(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < LEN(ToTitleTests); i++) {
        const TitleTest *tt = &ToTitleTests[i];
        Str s = q(bytes_to_title(a, b(tt->in)));
        if (!str_eq(s, tt->out))
            testing_t_errorf_v(t, "ToTitle(%q) = %q, want %q", tt->in, s, tt->out);
    }
    arena_free(&ar);
}

static void TestEqualFold(TestingT *t) {
    for (Int i = 0; i < LEN(EqualFoldTests); i++) {
        const EqualFoldTestsCase *tt = &EqualFoldTests[i];
        bool out = bytes_equal_fold(b(tt->s), b(tt->t));
        if (out != tt->out)
            testing_t_errorf_v(t, "EqualFold(%#q, %#q) = %v, want %v", tt->s, tt->t,
                               out, tt->out);
        out = bytes_equal_fold(b(tt->t), b(tt->s));
        if (out != tt->out)
            testing_t_errorf_v(t, "EqualFold(%#q, %#q) = %v, want %v", tt->t, tt->s,
                               out, tt->out);
    }
}

static void TestCut(TestingT *t) {
    for (Int i = 0; i < LEN(cutTests); i++) {
        const CutTestsCase *tt = &cutTests[i];
        Slice after;
        bool found;
        Slice before = bytes_cut(b(tt->s), b(tt->sep), &after, &found);
        if (!str_eq(q(before), tt->before) || !str_eq(q(after), tt->after) ||
            found != tt->found)
            testing_t_errorf_v(t, "Cut(%q, %q) = %q, %q, %v, want %q, %q, %v", tt->s,
                               tt->sep, q(before), q(after), found, tt->before,
                               tt->after, tt->found);
    }
}

static void TestCutLast(TestingT *t) {
    for (Int i = 0; i < LEN(cut_last_tests); i++) {
        const CutLastTestsCase *tt = &cut_last_tests[i];
        Slice after;
        bool found;
        Slice before = bytes_cut_last(b(tt->s), b(tt->sep), &after, &found);
        if (!str_eq(q(before), tt->before) || !str_eq(q(after), tt->after) ||
            found != tt->found)
            testing_t_errorf_v(t, "CutLast(%q, %q) = %q, %q, %v; want %q, %q, %v",
                               tt->s, tt->sep, q(before), q(after), found, tt->before,
                               tt->after, tt->found);
    }
}

static void TestCutPrefix(TestingT *t) {
    for (Int i = 0; i < LEN(cutPrefixTests); i++) {
        const CutPrefixTestsCase *tt = &cutPrefixTests[i];
        bool found;
        Slice after = bytes_cut_prefix(b(tt->s), b(tt->sep), &found);
        if (!str_eq(q(after), tt->after) || found != tt->found)
            testing_t_errorf_v(t, "CutPrefix(%q, %q) = %q, %v, want %q, %v", tt->s,
                               tt->sep, q(after), found, tt->after, tt->found);
    }
}

static void TestCutSuffix(TestingT *t) {
    for (Int i = 0; i < LEN(cutSuffixTests); i++) {
        const CutSuffixTestsCase *tt = &cutSuffixTests[i];
        bool found;
        Slice before = bytes_cut_suffix(b(tt->s), b(tt->sep), &found);
        if (!str_eq(q(before), tt->before) || found != tt->found)
            testing_t_errorf_v(t, "CutSuffix(%q, %q) = %q, %v, want %q, %v", tt->s,
                               tt->sep, q(before), found, tt->before, tt->found);
    }
}

static void grow_negative(void *env) {
    (void)env;
    BytesBuffer bb = {0};
    bytes_buffer_grow(&bb, -1);
}

static void truncate_negative(void *env) {
    (void)env;
    BytesBuffer bb = {0};
    bytes_buffer_truncate(&bb, -1);
}

static void truncate_out_of_range(void *env) {
    BytesBuffer *bb = (BytesBuffer *)env;
    Byte ten[10] = {0};
    Error err;
    bytes_buffer_write(bb, slice_from(ten, 10, 10, TYPE_BYTE), &err);
    bytes_buffer_truncate(bb, 20);
}

static void TestBufferGrowNegative(TestingT *t) {
    if (!recovered(grow_negative, NULL))
        testing_t_fatal_v(t, "Grow(-1) should have panicked");
}

static void TestBufferTruncateNegative(TestingT *t) {
    if (!recovered(truncate_negative, NULL))
        testing_t_fatal_v(t, "Truncate(-1) should have panicked");
}

static void TestBufferTruncateOutOfRange(TestingT *t) {
    BytesBuffer *bb = bytes_new_buffer(heap_allocator(), slice_nil(TYPE_BYTE));
    bool panicked = recovered(truncate_out_of_range, bb);
    bytes_buffer_free(bb);
    if (!panicked)
        testing_t_fatal_v(t, "Truncate(20) should have panicked");
}

static void TestContains(TestingT *t) {
    for (Int i = 0; i < LEN(containsTests); i++) {
        const ContainsTestsCase *tt = &containsTests[i];
        bool got = bytes_contains(b(tt->b), b(tt->subslice));
        if (got != tt->want)
            testing_t_errorf_v(t, "Contains(%q, %q) = %v, want %v", tt->b, tt->subslice,
                               got, tt->want);
    }
}

static void TestContainsAny(TestingT *t) {
    for (Int i = 0; i < LEN(ContainsAnyTests); i++) {
        const ContainsAnyTestsCase *ct = &ContainsAnyTests[i];
        if (bytes_contains_any(b(ct->b), ct->substr) != ct->expected)
            testing_t_errorf_v(t, "ContainsAny(%s, %s) = %v, want %v", ct->b,
                               ct->substr, !ct->expected, ct->expected);
    }
}

static void TestContainsRune(TestingT *t) {
    for (Int i = 0; i < LEN(ContainsRuneTests); i++) {
        const ContainsRuneTestsCase *ct = &ContainsRuneTests[i];
        if (bytes_contains_rune(b(ct->b), ct->r) != ct->expected)
            testing_t_errorf_v(t, "ContainsRune(%q, %q) = %v, want %v", ct->b, ct->r,
                               !ct->expected, ct->expected);
    }
}

static bool rune_is(void *env, Rune r) {
    return *(const Rune *)env == r;
}

static void TestContainsFunc(TestingT *t) {
    for (Int i = 0; i < LEN(ContainsRuneTests); i++) {
        const ContainsRuneTestsCase *ct = &ContainsRuneTests[i];
        RuneFunc f = BURROW_FN(RuneFunc, rune_is, (void *)(uintptr_t)&ct->r);
        if (bytes_contains_func(b(ct->b), f) != ct->expected)
            testing_t_errorf_v(t, "ContainsFunc(%q, func(%q)) = %v, want %v", ct->b,
                               ct->r, !ct->expected, ct->expected);
    }
}

static void TestClone(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    static const Byte empty[1];
    Slice many = bcopy_of(a, S("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
    Slice cloneTests[] = {
        slice_nil(TYPE_BYTE),
        slice_from((void *)(uintptr_t)empty, 0, 0, TYPE_BYTE),
        bytes_clone(a, slice_from((void *)(uintptr_t)empty, 0, 0, TYPE_BYTE)),
        slice_from(many.p, 0, 42, TYPE_BYTE),
        slice_from(many.p, 0, 0, TYPE_BYTE),
        bcopy_of(a, S("short")),
        many,
    };
    for (Int i = 0; i < LEN(cloneTests); i++) {
        Slice input = cloneTests[i];
        Slice clone = bytes_clone(a, input);
        if (!bytes_equal(clone, input))
            testing_t_errorf_v(t, "Clone(%q) = %q; want %q", q(input), q(clone),
                               q(input));
        if (input.p == NULL && clone.p != NULL)
            testing_t_errorf_v(
                t, "Clone(%q) return value should be equal to nil slice.", q(input));
        if (input.p != NULL && clone.p == NULL)
            testing_t_errorf_v(
                t, "Clone(%q) return value should not be equal to nil slice.",
                q(input));
        if (input.cap != 0 && input.p == clone.p)
            testing_t_errorf_v(
                t, "Clone(%q) return value should not reference inputs backing memory.",
                q(input));
    }
    arena_free(&ar);
}

/* Not in Go: running out of memory gives an empty result, never a crash. */
static void TestOutOfMemory(TestingT *t) {
    unsigned char buf[1];
    Fixed fx;
    fixed_init(&fx, buf, 0);
    Alloc *a = fixed_allocator(&fx);
    CHECK(bytes_split(a, cb("a,b,c"), cb(",")).len == 0);
    CHECK(bytes_fields(a, cb("a b c")).len == 0);
    CHECK(bytes_repeat(a, cb("ab"), 3).len == 0);
    CHECK(bytes_replace_all(a, cb("aaa"), cb("a"), cb("b")).len == 0);
    CHECK(bytes_to_upper(a, cb("abc")).len == 0);
    CHECK(bytes_clone(a, cb("abc")).len == 0);
    CHECK(bytes_join(a, slice_nil(TYPE_BYTES), cb(",")).len == 0);
    CHECK(bytes_runes(a, cb("abc")).len == 0);
    CHECK(BURROW_FUNC_IS_NIL(bytes_lines(a, cb("a\nb"))));
    CHECK(BURROW_FUNC_IS_NIL(bytes_split_seq(a, cb("a,b"), cb(","))));
    CHECK(BURROW_FUNC_IS_NIL(bytes_fields_seq(a, cb("a b"))));
    CHECK(bytes_new_reader(a, cb("abc")) == NULL);
    CHECK(bytes_new_buffer(a, slice_nil(TYPE_BYTE)) == NULL);
}

/* Not in Go: with the heap, a seq's state is freed with bytes_seq_free. Under
 * ASan this is a leak check as well. */
static void TestSeqFree(TestingT *t) {
    Alloc *a = heap_allocator();
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *al = arena_allocator(&ar);
    IterSeq seq = bytes_fields_seq(a, cb(" a  b c "));
    Slice got = sos(al, collect(al, seq));
    static const Str want[] = {SI("a"), SI("b"), SI("c")};
    if (!strs_eq(got, want, 3))
        testing_t_errorf_v(t, "FieldsSeq = %q, want %q", got, strs(want, 3));
    bytes_seq_free(a, seq);
    bytes_seq_free(a, bytes_lines(a, cb("x\ny")));
    IterSeq nil_seq = {NULL, NULL};
    bytes_seq_free(a, nil_seq);
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestLines)                                                                       \
    X(TestEqual)                                                                       \
    X(TestEqualExhaustive)                                                             \
    X(TestNotEqual)                                                                    \
    X(TestIndex)                                                                       \
    X(TestLastIndex)                                                                   \
    X(TestIndexAny)                                                                    \
    X(TestLastIndexAny)                                                                \
    X(TestIndexByte)                                                                   \
    X(TestLastIndexByte)                                                               \
    X(TestIndexByteBig)                                                                \
    X(TestIndexByteSmall)                                                              \
    X(TestIndexRune)                                                                   \
    X(TestCountByte)                                                                   \
    X(TestCountByteNoMatch)                                                            \
    X(TestSplit)                                                                       \
    X(TestSplitAfter)                                                                  \
    X(TestFields)                                                                      \
    X(TestFieldsFunc)                                                                  \
    X(TestMap)                                                                         \
    X(TestToUpper)                                                                     \
    X(TestToLower)                                                                     \
    X(TestToValidUTF8)                                                                 \
    X(TestTrimSpace)                                                                   \
    X(TestRepeat)                                                                      \
    X(TestRepeatCatchesOverflow)                                                       \
    X(TestRunes)                                                                       \
    X(TestTrim)                                                                        \
    X(TestTrimFunc)                                                                    \
    X(TestIndexFunc)                                                                   \
    X(TestReplace)                                                                     \
    X(TestTitle)                                                                       \
    X(TestToTitle)                                                                     \
    X(TestEqualFold)                                                                   \
    X(TestCut)                                                                         \
    X(TestCutLast)                                                                     \
    X(TestCutPrefix)                                                                   \
    X(TestCutSuffix)                                                                   \
    X(TestBufferGrowNegative)                                                          \
    X(TestBufferTruncateNegative)                                                      \
    X(TestBufferTruncateOutOfRange)                                                    \
    X(TestContains)                                                                    \
    X(TestContainsAny)                                                                 \
    X(TestContainsRune)                                                                \
    X(TestContainsFunc)                                                                \
    X(TestClone)                                                                       \
    X(TestOutOfMemory)                                                                 \
    X(TestSeqFree)

TESTING_MAIN(TESTS)

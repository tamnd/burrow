/* Derived from Go's src/internal/strconv/atoi_test.go, itoa_test.go and
 * atob_test.go, and the NumError tests in src/strconv/number_test.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "fatal.h"
#include "harness.h"

#include "burrow/error.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/mem/track.h"
#include "burrow/strconv.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static Arena ar;
static Alloc *a;

#define S BURROW_S
#define SI BURROW_S_INIT

/* The reason a case expects, before it is wrapped in a NumError. */
typedef enum Want { E_NONE, E_SYNTAX, E_RANGE } Want;

typedef struct Uint64Case {
    Str in;
    Int base;
    uint64_t out;
    Want err;
} Uint64Case;

typedef struct Int64Case {
    Str in;
    Int base;
    int64_t out;
    Want err;
} Int64Case;

typedef struct FormatCase {
    int64_t in;
    Int base;
    Str out;
} FormatCase;

typedef struct UformatCase {
    uint64_t in;
    Int base;
    Str out;
} UformatCase;

typedef struct BoolCase {
    Str in;
    bool out;
    Want err;
} BoolCase;

/* The tables are Go's, printed as C by a Go program that walks the slices in
 * the Go test files, so values like 1<<64 - 1 are the numbers Go computed. */

static const Uint64Case parse_uint64_tests[] = {
    {SI(""), 10, UINT64_C(0), E_SYNTAX},
    {SI("0"), 10, UINT64_C(0), E_NONE},
    {SI("1"), 10, UINT64_C(1), E_NONE},
    {SI("12345"), 10, UINT64_C(12345), E_NONE},
    {SI("012345"), 10, UINT64_C(12345), E_NONE},
    {SI("12345x"), 10, UINT64_C(0), E_SYNTAX},
    {SI("98765432100"), 10, UINT64_C(98765432100), E_NONE},
    {SI("18446744073709551615"), 10, UINT64_C(18446744073709551615), E_NONE},
    {SI("18446744073709551616"), 10, UINT64_C(18446744073709551615), E_RANGE},
    {SI("18446744073709551620"), 10, UINT64_C(18446744073709551615), E_RANGE},
    {SI("1_2_3_4_5"), 10, UINT64_C(0), E_SYNTAX},
    {SI("_12345"), 10, UINT64_C(0), E_SYNTAX},
    {SI("1__2345"), 10, UINT64_C(0), E_SYNTAX},
    {SI("12345_"), 10, UINT64_C(0), E_SYNTAX},
    {SI("-0"), 10, UINT64_C(0), E_SYNTAX},
    {SI("-1"), 10, UINT64_C(0), E_SYNTAX},
    {SI("+1"), 10, UINT64_C(0), E_SYNTAX},
};

static const Uint64Case parse_uint64_base_tests[] = {
    {SI(""), 0, UINT64_C(0), E_SYNTAX},
    {SI("0"), 0, UINT64_C(0), E_NONE},
    {SI("0x"), 0, UINT64_C(0), E_SYNTAX},
    {SI("0X"), 0, UINT64_C(0), E_SYNTAX},
    {SI("1"), 0, UINT64_C(1), E_NONE},
    {SI("12345"), 0, UINT64_C(12345), E_NONE},
    {SI("012345"), 0, UINT64_C(5349), E_NONE},
    {SI("0x12345"), 0, UINT64_C(74565), E_NONE},
    {SI("0X12345"), 0, UINT64_C(74565), E_NONE},
    {SI("12345x"), 0, UINT64_C(0), E_SYNTAX},
    {SI("0xabcdefg123"), 0, UINT64_C(0), E_SYNTAX},
    {SI("123456789abc"), 0, UINT64_C(0), E_SYNTAX},
    {SI("98765432100"), 0, UINT64_C(98765432100), E_NONE},
    {SI("18446744073709551615"), 0, UINT64_C(18446744073709551615), E_NONE},
    {SI("18446744073709551616"), 0, UINT64_C(18446744073709551615), E_RANGE},
    {SI("18446744073709551620"), 0, UINT64_C(18446744073709551615), E_RANGE},
    {SI("0xFFFFFFFFFFFFFFFF"), 0, UINT64_C(18446744073709551615), E_NONE},
    {SI("0x10000000000000000"), 0, UINT64_C(18446744073709551615), E_RANGE},
    {SI("01777777777777777777777"), 0, UINT64_C(18446744073709551615), E_NONE},
    {SI("01777777777777777777778"), 0, UINT64_C(0), E_SYNTAX},
    {SI("02000000000000000000000"), 0, UINT64_C(18446744073709551615), E_RANGE},
    {SI("0200000000000000000000"), 0, UINT64_C(2305843009213693952), E_NONE},
    {SI("0b"), 0, UINT64_C(0), E_SYNTAX},
    {SI("0B"), 0, UINT64_C(0), E_SYNTAX},
    {SI("0b101"), 0, UINT64_C(5), E_NONE},
    {SI("0B101"), 0, UINT64_C(5), E_NONE},
    {SI("0o"), 0, UINT64_C(0), E_SYNTAX},
    {SI("0O"), 0, UINT64_C(0), E_SYNTAX},
    {SI("0o377"), 0, UINT64_C(255), E_NONE},
    {SI("0O377"), 0, UINT64_C(255), E_NONE},
    {SI("1_2_3_4_5"), 0, UINT64_C(12345), E_NONE},
    {SI("_12345"), 0, UINT64_C(0), E_SYNTAX},
    {SI("1__2345"), 0, UINT64_C(0), E_SYNTAX},
    {SI("12345_"), 0, UINT64_C(0), E_SYNTAX},
    {SI("1_2_3_4_5"), 10, UINT64_C(0), E_SYNTAX},
    {SI("_12345"), 10, UINT64_C(0), E_SYNTAX},
    {SI("1__2345"), 10, UINT64_C(0), E_SYNTAX},
    {SI("12345_"), 10, UINT64_C(0), E_SYNTAX},
    {SI("0x_1_2_3_4_5"), 0, UINT64_C(74565), E_NONE},
    {SI("_0x12345"), 0, UINT64_C(0), E_SYNTAX},
    {SI("0x__12345"), 0, UINT64_C(0), E_SYNTAX},
    {SI("0x1__2345"), 0, UINT64_C(0), E_SYNTAX},
    {SI("0x1234__5"), 0, UINT64_C(0), E_SYNTAX},
    {SI("0x12345_"), 0, UINT64_C(0), E_SYNTAX},
    {SI("1_2_3_4_5"), 16, UINT64_C(0), E_SYNTAX},
    {SI("_12345"), 16, UINT64_C(0), E_SYNTAX},
    {SI("1__2345"), 16, UINT64_C(0), E_SYNTAX},
    {SI("1234__5"), 16, UINT64_C(0), E_SYNTAX},
    {SI("12345_"), 16, UINT64_C(0), E_SYNTAX},
    {SI("0_1_2_3_4_5"), 0, UINT64_C(5349), E_NONE},
    {SI("_012345"), 0, UINT64_C(0), E_SYNTAX},
    {SI("0__12345"), 0, UINT64_C(0), E_SYNTAX},
    {SI("01234__5"), 0, UINT64_C(0), E_SYNTAX},
    {SI("012345_"), 0, UINT64_C(0), E_SYNTAX},
    {SI("0o_1_2_3_4_5"), 0, UINT64_C(5349), E_NONE},
    {SI("_0o12345"), 0, UINT64_C(0), E_SYNTAX},
    {SI("0o__12345"), 0, UINT64_C(0), E_SYNTAX},
    {SI("0o1234__5"), 0, UINT64_C(0), E_SYNTAX},
    {SI("0o12345_"), 0, UINT64_C(0), E_SYNTAX},
    {SI("0_1_2_3_4_5"), 8, UINT64_C(0), E_SYNTAX},
    {SI("_012345"), 8, UINT64_C(0), E_SYNTAX},
    {SI("0__12345"), 8, UINT64_C(0), E_SYNTAX},
    {SI("01234__5"), 8, UINT64_C(0), E_SYNTAX},
    {SI("012345_"), 8, UINT64_C(0), E_SYNTAX},
    {SI("0b_1_0_1"), 0, UINT64_C(5), E_NONE},
    {SI("_0b101"), 0, UINT64_C(0), E_SYNTAX},
    {SI("0b__101"), 0, UINT64_C(0), E_SYNTAX},
    {SI("0b1__01"), 0, UINT64_C(0), E_SYNTAX},
    {SI("0b10__1"), 0, UINT64_C(0), E_SYNTAX},
    {SI("0b101_"), 0, UINT64_C(0), E_SYNTAX},
    {SI("1_0_1"), 2, UINT64_C(0), E_SYNTAX},
    {SI("_101"), 2, UINT64_C(0), E_SYNTAX},
    {SI("1_01"), 2, UINT64_C(0), E_SYNTAX},
    {SI("10_1"), 2, UINT64_C(0), E_SYNTAX},
    {SI("101_"), 2, UINT64_C(0), E_SYNTAX},
};

static const Int64Case parse_int64_tests[] = {
    {SI(""), 10, INT64_C(0), E_SYNTAX},
    {SI("0"), 10, INT64_C(0), E_NONE},
    {SI("-0"), 10, INT64_C(0), E_NONE},
    {SI("+0"), 10, INT64_C(0), E_NONE},
    {SI("1"), 10, INT64_C(1), E_NONE},
    {SI("-1"), 10, INT64_C(-1), E_NONE},
    {SI("+1"), 10, INT64_C(1), E_NONE},
    {SI("12345"), 10, INT64_C(12345), E_NONE},
    {SI("-12345"), 10, INT64_C(-12345), E_NONE},
    {SI("012345"), 10, INT64_C(12345), E_NONE},
    {SI("-012345"), 10, INT64_C(-12345), E_NONE},
    {SI("98765432100"), 10, INT64_C(98765432100), E_NONE},
    {SI("-98765432100"), 10, INT64_C(-98765432100), E_NONE},
    {SI("9223372036854775807"), 10, INT64_C(9223372036854775807), E_NONE},
    {SI("-9223372036854775807"), 10, INT64_C(-9223372036854775807), E_NONE},
    {SI("9223372036854775808"), 10, INT64_C(9223372036854775807), E_RANGE},
    {SI("-9223372036854775808"), 10, INT64_MIN, E_NONE},
    {SI("9223372036854775809"), 10, INT64_C(9223372036854775807), E_RANGE},
    {SI("-9223372036854775809"), 10, INT64_MIN, E_RANGE},
    {SI("-1_2_3_4_5"), 10, INT64_C(0), E_SYNTAX},
    {SI("-_12345"), 10, INT64_C(0), E_SYNTAX},
    {SI("_12345"), 10, INT64_C(0), E_SYNTAX},
    {SI("1__2345"), 10, INT64_C(0), E_SYNTAX},
    {SI("12345_"), 10, INT64_C(0), E_SYNTAX},
    {SI("123%45"), 10, INT64_C(0), E_SYNTAX},
};

static const Int64Case parse_int64_base_tests[] = {
    {SI(""), 0, INT64_C(0), E_SYNTAX},
    {SI("0"), 0, INT64_C(0), E_NONE},
    {SI("-0"), 0, INT64_C(0), E_NONE},
    {SI("1"), 0, INT64_C(1), E_NONE},
    {SI("-1"), 0, INT64_C(-1), E_NONE},
    {SI("12345"), 0, INT64_C(12345), E_NONE},
    {SI("-12345"), 0, INT64_C(-12345), E_NONE},
    {SI("012345"), 0, INT64_C(5349), E_NONE},
    {SI("-012345"), 0, INT64_C(-5349), E_NONE},
    {SI("0x12345"), 0, INT64_C(74565), E_NONE},
    {SI("-0X12345"), 0, INT64_C(-74565), E_NONE},
    {SI("12345x"), 0, INT64_C(0), E_SYNTAX},
    {SI("-12345x"), 0, INT64_C(0), E_SYNTAX},
    {SI("98765432100"), 0, INT64_C(98765432100), E_NONE},
    {SI("-98765432100"), 0, INT64_C(-98765432100), E_NONE},
    {SI("9223372036854775807"), 0, INT64_C(9223372036854775807), E_NONE},
    {SI("-9223372036854775807"), 0, INT64_C(-9223372036854775807), E_NONE},
    {SI("9223372036854775808"), 0, INT64_C(9223372036854775807), E_RANGE},
    {SI("-9223372036854775808"), 0, INT64_MIN, E_NONE},
    {SI("9223372036854775809"), 0, INT64_C(9223372036854775807), E_RANGE},
    {SI("-9223372036854775809"), 0, INT64_MIN, E_RANGE},
    {SI("g"), 17, INT64_C(16), E_NONE},
    {SI("10"), 25, INT64_C(25), E_NONE},
    {SI("holycow"), 35, INT64_C(32544027072), E_NONE},
    {SI("holycow"), 36, INT64_C(38493362624), E_NONE},
    {SI("0"), 2, INT64_C(0), E_NONE},
    {SI("-1"), 2, INT64_C(-1), E_NONE},
    {SI("1010"), 2, INT64_C(10), E_NONE},
    {SI("1000000000000000"), 2, INT64_C(32768), E_NONE},
    {SI("111111111111111111111111111111111111111111111111111111111111111"), 2,
     INT64_C(9223372036854775807), E_NONE},
    {SI("1000000000000000000000000000000000000000000000000000000000000000"), 2,
     INT64_C(9223372036854775807), E_RANGE},
    {SI("-1000000000000000000000000000000000000000000000000000000000000000"), 2,
     INT64_MIN, E_NONE},
    {SI("-1000000000000000000000000000000000000000000000000000000000000001"), 2,
     INT64_MIN, E_RANGE},
    {SI("-10"), 8, INT64_C(-8), E_NONE},
    {SI("57635436545"), 8, INT64_C(6416645477), E_NONE},
    {SI("100000000"), 8, INT64_C(16777216), E_NONE},
    {SI("10"), 16, INT64_C(16), E_NONE},
    {SI("-123456789abcdef"), 16, INT64_C(-81985529216486895), E_NONE},
    {SI("7fffffffffffffff"), 16, INT64_C(9223372036854775807), E_NONE},
    {SI("-0x_1_2_3_4_5"), 0, INT64_C(-74565), E_NONE},
    {SI("0x_1_2_3_4_5"), 0, INT64_C(74565), E_NONE},
    {SI("-_0x12345"), 0, INT64_C(0), E_SYNTAX},
    {SI("_-0x12345"), 0, INT64_C(0), E_SYNTAX},
    {SI("_0x12345"), 0, INT64_C(0), E_SYNTAX},
    {SI("0x__12345"), 0, INT64_C(0), E_SYNTAX},
    {SI("0x1__2345"), 0, INT64_C(0), E_SYNTAX},
    {SI("0x1234__5"), 0, INT64_C(0), E_SYNTAX},
    {SI("0x12345_"), 0, INT64_C(0), E_SYNTAX},
    {SI("-0_1_2_3_4_5"), 0, INT64_C(-5349), E_NONE},
    {SI("0_1_2_3_4_5"), 0, INT64_C(5349), E_NONE},
    {SI("-_012345"), 0, INT64_C(0), E_SYNTAX},
    {SI("_-012345"), 0, INT64_C(0), E_SYNTAX},
    {SI("_012345"), 0, INT64_C(0), E_SYNTAX},
    {SI("0__12345"), 0, INT64_C(0), E_SYNTAX},
    {SI("01234__5"), 0, INT64_C(0), E_SYNTAX},
    {SI("012345_"), 0, INT64_C(0), E_SYNTAX},
    {SI("+0xf"), 0, INT64_C(15), E_NONE},
    {SI("-0xf"), 0, INT64_C(-15), E_NONE},
    {SI("0x+f"), 0, INT64_C(0), E_SYNTAX},
    {SI("0x-f"), 0, INT64_C(0), E_SYNTAX},
};

static const Uint64Case parse_uint32_tests[] = {
    {SI(""), 10, UINT64_C(0), E_SYNTAX},
    {SI("0"), 10, UINT64_C(0), E_NONE},
    {SI("1"), 10, UINT64_C(1), E_NONE},
    {SI("12345"), 10, UINT64_C(12345), E_NONE},
    {SI("012345"), 10, UINT64_C(12345), E_NONE},
    {SI("12345x"), 10, UINT64_C(0), E_SYNTAX},
    {SI("987654321"), 10, UINT64_C(987654321), E_NONE},
    {SI("4294967295"), 10, UINT64_C(4294967295), E_NONE},
    {SI("4294967296"), 10, UINT64_C(4294967295), E_RANGE},
    {SI("1_2_3_4_5"), 10, UINT64_C(0), E_SYNTAX},
    {SI("_12345"), 10, UINT64_C(0), E_SYNTAX},
    {SI("_12345"), 10, UINT64_C(0), E_SYNTAX},
    {SI("1__2345"), 10, UINT64_C(0), E_SYNTAX},
    {SI("12345_"), 10, UINT64_C(0), E_SYNTAX},
};

static const Int64Case parse_int32_tests[] = {
    {SI(""), 10, INT64_C(0), E_SYNTAX},
    {SI("0"), 10, INT64_C(0), E_NONE},
    {SI("-0"), 10, INT64_C(0), E_NONE},
    {SI("1"), 10, INT64_C(1), E_NONE},
    {SI("-1"), 10, INT64_C(-1), E_NONE},
    {SI("12345"), 10, INT64_C(12345), E_NONE},
    {SI("-12345"), 10, INT64_C(-12345), E_NONE},
    {SI("012345"), 10, INT64_C(12345), E_NONE},
    {SI("-012345"), 10, INT64_C(-12345), E_NONE},
    {SI("12345x"), 10, INT64_C(0), E_SYNTAX},
    {SI("-12345x"), 10, INT64_C(0), E_SYNTAX},
    {SI("987654321"), 10, INT64_C(987654321), E_NONE},
    {SI("-987654321"), 10, INT64_C(-987654321), E_NONE},
    {SI("2147483647"), 10, INT64_C(2147483647), E_NONE},
    {SI("-2147483647"), 10, INT64_C(-2147483647), E_NONE},
    {SI("2147483648"), 10, INT64_C(2147483647), E_RANGE},
    {SI("-2147483648"), 10, INT64_C(-2147483648), E_NONE},
    {SI("2147483649"), 10, INT64_C(2147483647), E_RANGE},
    {SI("-2147483649"), 10, INT64_C(-2147483648), E_RANGE},
    {SI("-1_2_3_4_5"), 10, INT64_C(0), E_SYNTAX},
    {SI("-_12345"), 10, INT64_C(0), E_SYNTAX},
    {SI("_12345"), 10, INT64_C(0), E_SYNTAX},
    {SI("1__2345"), 10, INT64_C(0), E_SYNTAX},
    {SI("12345_"), 10, INT64_C(0), E_SYNTAX},
    {SI("123%45"), 10, INT64_C(0), E_SYNTAX},
};

static const FormatCase itob64_tests[] = {
    {INT64_C(0), 10, SI("0")},
    {INT64_C(1), 10, SI("1")},
    {INT64_C(-1), 10, SI("-1")},
    {INT64_C(12345678), 10, SI("12345678")},
    {INT64_C(-987654321), 10, SI("-987654321")},
    {INT64_C(2147483647), 10, SI("2147483647")},
    {INT64_C(-2147483647), 10, SI("-2147483647")},
    {INT64_C(2147483648), 10, SI("2147483648")},
    {INT64_C(-2147483648), 10, SI("-2147483648")},
    {INT64_C(2147483649), 10, SI("2147483649")},
    {INT64_C(-2147483649), 10, SI("-2147483649")},
    {INT64_C(4294967295), 10, SI("4294967295")},
    {INT64_C(-4294967295), 10, SI("-4294967295")},
    {INT64_C(4294967296), 10, SI("4294967296")},
    {INT64_C(-4294967296), 10, SI("-4294967296")},
    {INT64_C(4294967297), 10, SI("4294967297")},
    {INT64_C(-4294967297), 10, SI("-4294967297")},
    {INT64_C(1125899906842624), 10, SI("1125899906842624")},
    {INT64_C(9223372036854775807), 10, SI("9223372036854775807")},
    {INT64_C(-9223372036854775807), 10, SI("-9223372036854775807")},
    {INT64_MIN, 10, SI("-9223372036854775808")},
    {INT64_C(0), 2, SI("0")},
    {INT64_C(10), 2, SI("1010")},
    {INT64_C(-1), 2, SI("-1")},
    {INT64_C(32768), 2, SI("1000000000000000")},
    {INT64_C(-8), 8, SI("-10")},
    {INT64_C(6416645477), 8, SI("57635436545")},
    {INT64_C(16777216), 8, SI("100000000")},
    {INT64_C(16), 16, SI("10")},
    {INT64_C(-81985529216486895), 16, SI("-123456789abcdef")},
    {INT64_C(9223372036854775807), 16, SI("7fffffffffffffff")},
    {INT64_C(9223372036854775807), 2,
     SI("111111111111111111111111111111111111111111111111111111111111111")},
    {INT64_MIN, 2,
     SI("-1000000000000000000000000000000000000000000000000000000000000000")},
    {INT64_C(16), 17, SI("g")},
    {INT64_C(25), 25, SI("10")},
    {INT64_C(32544027072), 35, SI("holycow")},
    {INT64_C(38493362624), 36, SI("holycow")},
};

static const UformatCase uitob64_tests[] = {
    {UINT64_C(9223372036854775807), 10, SI("9223372036854775807")},
    {UINT64_C(9223372036854775808), 10, SI("9223372036854775808")},
    {UINT64_C(9223372036854775809), 10, SI("9223372036854775809")},
    {UINT64_C(18446744073709551614), 10, SI("18446744073709551614")},
    {UINT64_C(18446744073709551615), 10, SI("18446744073709551615")},
    {UINT64_C(18446744073709551615), 2,
     SI("1111111111111111111111111111111111111111111111111111111111111111")},
};

static const UformatCase varlen_uints[] = {
    {UINT64_C(1), 10, SI("1")},
    {UINT64_C(12), 10, SI("12")},
    {UINT64_C(123), 10, SI("123")},
    {UINT64_C(1234), 10, SI("1234")},
    {UINT64_C(12345), 10, SI("12345")},
    {UINT64_C(123456), 10, SI("123456")},
    {UINT64_C(1234567), 10, SI("1234567")},
    {UINT64_C(12345678), 10, SI("12345678")},
    {UINT64_C(123456789), 10, SI("123456789")},
    {UINT64_C(1234567890), 10, SI("1234567890")},
    {UINT64_C(12345678901), 10, SI("12345678901")},
    {UINT64_C(123456789012), 10, SI("123456789012")},
    {UINT64_C(1234567890123), 10, SI("1234567890123")},
    {UINT64_C(12345678901234), 10, SI("12345678901234")},
    {UINT64_C(123456789012345), 10, SI("123456789012345")},
    {UINT64_C(1234567890123456), 10, SI("1234567890123456")},
    {UINT64_C(12345678901234567), 10, SI("12345678901234567")},
    {UINT64_C(123456789012345678), 10, SI("123456789012345678")},
    {UINT64_C(1234567890123456789), 10, SI("1234567890123456789")},
    {UINT64_C(12345678901234567890), 10, SI("12345678901234567890")},
};

static const BoolCase atob_tests[] = {
    {SI(""), false, E_SYNTAX},    {SI("asdf"), false, E_SYNTAX},
    {SI("0"), false, E_NONE},     {SI("f"), false, E_NONE},
    {SI("F"), false, E_NONE},     {SI("FALSE"), false, E_NONE},
    {SI("false"), false, E_NONE}, {SI("False"), false, E_NONE},
    {SI("1"), true, E_NONE},      {SI("t"), true, E_NONE},
    {SI("T"), true, E_NONE},      {SI("TRUE"), true, E_NONE},
    {SI("true"), true, E_NONE},   {SI("True"), true, E_NONE},
};

#define COUNT(t) ((Int)(sizeof(t) / sizeof((t)[0])))

/* What Go's reflect.DeepEqual against &NumError{func, in, reason} checks:
 * nothing at all for a success, and otherwise a NumError with the right
 * function, a copy of the input and the right reason inside, whose text is
 * Go's text. */
static bool num_error_is(Error err, const char *func, Str in, Want want) {
    if (want == E_NONE)
        return BURROW_OK(err);

    Error reason = want == E_SYNTAX ? strconv_err_syntax : strconv_err_range;
    const StrconvNumError *ne = errors_as(err, TYPE_STRCONV_NUM_ERROR);
    if (ne == NULL || !errors_is(err, reason))
        return false;
    if (!str_eq(ne->func, str_from_cstr(func)) || !str_eq(ne->num, in))
        return false;
    if (in.len > 0 && ne->num.p == in.p)
        return false;

    StrconvNumError expect = {str_from_cstr(func), in, reason};
    return str_eq(error_text(err), strconv_num_error_error(a, &expect));
}

TEST(parse_uint64) {
    for (Int i = 0; i < COUNT(parse_uint64_tests); i++) {
        const Uint64Case *tt = &parse_uint64_tests[i];
        Error err = BURROW_NO_ERROR;
        uint64_t out = strconv_parse_uint(tt->in, 10, 64, &err);
        CHECK(out == tt->out);
        CHECK(num_error_is(err, "ParseUint", tt->in, tt->err));
    }
}

TEST(parse_uint64_base) {
    for (Int i = 0; i < COUNT(parse_uint64_base_tests); i++) {
        const Uint64Case *tt = &parse_uint64_base_tests[i];
        Error err = BURROW_NO_ERROR;
        uint64_t out = strconv_parse_uint(tt->in, tt->base, 64, &err);
        CHECK(out == tt->out);
        CHECK(num_error_is(err, "ParseUint", tt->in, tt->err));
    }
}

TEST(parse_int64) {
    for (Int i = 0; i < COUNT(parse_int64_tests); i++) {
        const Int64Case *tt = &parse_int64_tests[i];
        Error err = BURROW_NO_ERROR;
        int64_t out = strconv_parse_int(tt->in, 10, 64, &err);
        CHECK(out == tt->out);
        CHECK(num_error_is(err, "ParseInt", tt->in, tt->err));
    }
}

TEST(parse_int64_base) {
    for (Int i = 0; i < COUNT(parse_int64_base_tests); i++) {
        const Int64Case *tt = &parse_int64_base_tests[i];
        Error err = BURROW_NO_ERROR;
        int64_t out = strconv_parse_int(tt->in, tt->base, 64, &err);
        CHECK(out == tt->out);
        CHECK(num_error_is(err, "ParseInt", tt->in, tt->err));
    }
}

TEST(parse_uint32) {
    for (Int i = 0; i < COUNT(parse_uint32_tests); i++) {
        const Uint64Case *tt = &parse_uint32_tests[i];
        Error err = BURROW_NO_ERROR;
        uint64_t out = strconv_parse_uint(tt->in, 10, 32, &err);
        CHECK(out == tt->out);
        CHECK(num_error_is(err, "ParseUint", tt->in, tt->err));
    }
}

TEST(parse_int32) {
    for (Int i = 0; i < COUNT(parse_int32_tests); i++) {
        const Int64Case *tt = &parse_int32_tests[i];
        Error err = BURROW_NO_ERROR;
        int64_t out = strconv_parse_int(tt->in, 10, 32, &err);
        CHECK(out == tt->out);
        CHECK(num_error_is(err, "ParseInt", tt->in, tt->err));
    }
}

/* Bit size 0 is Int, so which table applies depends on the platform, the same
 * as Go's switch on IntSize. */
TEST(parse_int_size) {
    const Uint64Case *ut =
        STRCONV_INT_SIZE == 32 ? parse_uint32_tests : parse_uint64_tests;
    Int un =
        STRCONV_INT_SIZE == 32 ? COUNT(parse_uint32_tests) : COUNT(parse_uint64_tests);
    for (Int i = 0; i < un; i++) {
        Error err = BURROW_NO_ERROR;
        uint64_t out = strconv_parse_uint(ut[i].in, 10, 0, &err);
        CHECK(out == ut[i].out);
        CHECK(num_error_is(err, "ParseUint", ut[i].in, ut[i].err));
    }

    const Int64Case *it =
        STRCONV_INT_SIZE == 32 ? parse_int32_tests : parse_int64_tests;
    Int in =
        STRCONV_INT_SIZE == 32 ? COUNT(parse_int32_tests) : COUNT(parse_int64_tests);
    for (Int i = 0; i < in; i++) {
        Error err = BURROW_NO_ERROR;
        int64_t out = strconv_parse_int(it[i].in, 10, 0, &err);
        CHECK(out == it[i].out);
        CHECK(num_error_is(err, "ParseInt", it[i].in, it[i].err));

        err = BURROW_NO_ERROR;
        Int n = strconv_atoi(it[i].in, &err);
        CHECK((int64_t)n == it[i].out);
        CHECK(num_error_is(err, "Atoi", it[i].in, it[i].err));
    }
}

TEST(atoi_without_an_error) {
    CHECK(strconv_atoi(S("-42"), NULL) == -42);
    CHECK(strconv_atoi(S("x"), NULL) == 0);
    CHECK(strconv_parse_int(S("99999999999999999999"), 10, 64, NULL) == INT64_MAX);
    CHECK(strconv_parse_uint(S("0x"), 0, 64, NULL) == 0);
}

typedef struct ArgCase {
    Int arg;
    const char *err;
} ArgCase;

static const ArgCase bit_size_tests[] = {
    {-1, "invalid bit size -1"},
    {0, NULL},
    {64, NULL},
    {65, "invalid bit size 65"},
};

static const ArgCase base_tests[] = {
    {-1, "invalid base -1"}, {0, NULL}, {1, "invalid base 1"}, {2, NULL}, {36, NULL},
    {37, "invalid base 37"},
};

/* Go's number_test compares these by their text, since the reason inside is a
 * fresh errors.New and has no identity to compare. */
static bool arg_error_is(Error err, const char *func, const char *reason) {
    if (reason == NULL)
        return BURROW_OK(err);

    char want[96];
    snprintf(want, sizeof want, "strconv.%s: parsing \"0\": %s", func, reason);
    const StrconvNumError *ne = errors_as(err, TYPE_STRCONV_NUM_ERROR);
    return ne != NULL && str_eq(error_text(err), str_from_cstr(want)) &&
           str_eq(error_text(ne->err), str_from_cstr(reason));
}

TEST(parse_bit_size) {
    for (Int i = 0; i < COUNT(bit_size_tests); i++) {
        const ArgCase *tt = &bit_size_tests[i];
        Error err = BURROW_NO_ERROR;
        (void)strconv_parse_int(S("0"), 0, tt->arg, &err);
        CHECK(arg_error_is(err, "ParseInt", tt->err));
        err = BURROW_NO_ERROR;
        (void)strconv_parse_uint(S("0"), 0, tt->arg, &err);
        CHECK(arg_error_is(err, "ParseUint", tt->err));
    }
}

TEST(parse_base) {
    for (Int i = 0; i < COUNT(base_tests); i++) {
        const ArgCase *tt = &base_tests[i];
        Error err = BURROW_NO_ERROR;
        (void)strconv_parse_int(S("0"), tt->arg, 0, &err);
        CHECK(arg_error_is(err, "ParseInt", tt->err));
        err = BURROW_NO_ERROR;
        (void)strconv_parse_uint(S("0"), tt->arg, 0, &err);
        CHECK(arg_error_is(err, "ParseUint", tt->err));
    }
}

static bool appended(Slice got, Str want) {
    return got.len == want.len && memcmp(got.p, want.p, (size_t)want.len) == 0;
}

static Slice abc(void) {
    return slice_from_str(a, S("abc"));
}

static Str with_abc(Str s) {
    Byte *p = mem_alloc(a, (size_t)s.len + 3, 1);
    memcpy(p, "abc", 3);
    memcpy(p + 3, s.p, (size_t)s.len);
    return str_from_bytes(p, s.len + 3);
}

TEST(itoa) {
    for (Int i = 0; i < COUNT(itob64_tests); i++) {
        const FormatCase *tt = &itob64_tests[i];
        CHECK(str_eq(strconv_format_int(a, tt->in, tt->base), tt->out));
        CHECK(appended(strconv_append_int(a, abc(), tt->in, tt->base),
                       with_abc(tt->out)));

        if (tt->in >= 0) {
            CHECK(str_eq(strconv_format_uint(a, (uint64_t)tt->in, tt->base), tt->out));
            CHECK(
                appended(strconv_append_uint(a, (Slice){0}, (uint64_t)tt->in, tt->base),
                         tt->out));
        }
        if (tt->base == 10 && (int64_t)(Int)tt->in == tt->in)
            CHECK(str_eq(strconv_itoa(a, (Int)tt->in), tt->out));
    }
}

TEST(uitoa) {
    for (Int i = 0; i < COUNT(uitob64_tests); i++) {
        const UformatCase *tt = &uitob64_tests[i];
        CHECK(str_eq(strconv_format_uint(a, tt->in, tt->base), tt->out));
        CHECK(appended(strconv_append_uint(a, abc(), tt->in, tt->base),
                       with_abc(tt->out)));
    }
}

TEST(format_uint_varlen) {
    for (Int i = 0; i < COUNT(varlen_uints); i++)
        CHECK(str_eq(strconv_format_uint(a, varlen_uints[i].in, 10),
                     varlen_uints[i].out));
}

/* Every base and a spread of values, against a plain digit by digit loop. The
 * fast paths for 10 and the powers of two are the ones worth checking. */
TEST(format_every_base) {
    static const char dig[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    uint64_t v = 1;
    for (Int n = 0; n < 2000; n++) {
        v = v * 6364136223846793005ULL + 1442695040888963407ULL;
        uint64_t x = v >> (n % 64);
        for (Int base = 2; base <= 36; base++) {
            char buf[70];
            Int k = (Int)sizeof buf;
            uint64_t y = x;
            do {
                buf[--k] = dig[y % (uint64_t)base];
                y /= (uint64_t)base;
            } while (y != 0);
            Str want = str_from_bytes(buf + k, (Int)sizeof buf - k);
            CHECK(str_eq(strconv_format_uint(a, x, base), want));

            Error err = BURROW_NO_ERROR;
            CHECK(strconv_parse_uint(want, base, 64, &err) == x);
            CHECK(BURROW_OK(err));
        }
    }
}

TEST(format_illegal_base) {
    CHECK_PANIC((void)strconv_format_uint(a, 12345678, 1),
                "strconv: illegal AppendInt/FormatInt base");
    CHECK_PANIC((void)strconv_append_int(a, (Slice){0}, -1, 37),
                "strconv: illegal AppendInt/FormatInt base");
}

TEST(parse_bool) {
    for (Int i = 0; i < COUNT(atob_tests); i++) {
        const BoolCase *tt = &atob_tests[i];
        Error err = BURROW_NO_ERROR;
        CHECK(strconv_parse_bool(tt->in, &err) == tt->out);
        CHECK(num_error_is(err, "ParseBool", tt->in, tt->err));
    }
}

TEST(format_bool) {
    CHECK(str_eq(strconv_format_bool(true), S("true")));
    CHECK(str_eq(strconv_format_bool(false), S("false")));
}

TEST(append_bool) {
    CHECK(appended(strconv_append_bool(a, slice_from_str(a, S("foo ")), true),
                   S("foo true")));
    CHECK(appended(strconv_append_bool(a, slice_from_str(a, S("foo ")), false),
                   S("foo false")));
    Slice got = strconv_append_bool(a, (Slice){0}, true);
    CHECK(got.elem == TYPE_BYTE && appended(got, S("true")));
}

BURROW_SENTINEL_ERROR(failed, "failed");

typedef struct NumErrorCase {
    Str num, want;
} NumErrorCase;

static const NumErrorCase num_error_tests[] = {
    {SI("0"), SI("strconv.ParseFloat: parsing \"0\": failed")},
    {SI("`"), SI("strconv.ParseFloat: parsing \"`\": failed")},
    {SI("1\x00.2"), SI("strconv.ParseFloat: parsing \"1\\x00.2\": failed")},
};

TEST(num_error) {
    for (Int i = 0; i < COUNT(num_error_tests); i++) {
        const NumErrorCase *tt = &num_error_tests[i];
        StrconvNumError e = {S("ParseFloat"), tt->num, failed};
        CHECK(str_eq(strconv_num_error_error(a, &e), tt->want));

        Error err = strconv_num_error_as_error(a, &e);
        CHECK(str_eq(error_text(err), tt->want));
        CHECK(errors_is(err, failed));
    }
}

TEST(num_error_unwrap) {
    StrconvNumError e = {S("ParseInt"), S("x"), strconv_err_syntax};
    CHECK(errors_is(strconv_num_error_as_error(a, &e), strconv_err_syntax));
    CHECK(errors_is(strconv_num_error_unwrap(&e), strconv_err_syntax));

    Error err = BURROW_NO_ERROR;
    (void)strconv_parse_int(S("x"), 10, 64, &err);
    CHECK(errors_is(errors_unwrap(err), strconv_err_syntax));
}

/* An error from a Parse function lives in the error arena. error_retain has to
 * give back one that is still a NumError after the arena lets go, including
 * the reason inside it when that was made in the arena too. */
TEST(num_error_survives_retain) {
    ArenaMark m = error_mark();
    Error err = BURROW_NO_ERROR;
    (void)strconv_parse_int(S("12"), 99, 0, &err);
    Error kept = error_retain(a, err);
    error_release(m);

    const StrconvNumError *ne = errors_as(kept, TYPE_STRCONV_NUM_ERROR);
    CHECK(ne != NULL);
    if (ne != NULL) {
        CHECK(str_eq(ne->func, S("ParseInt")));
        CHECK(str_eq(ne->num, S("12")));
        CHECK(str_eq(error_text(ne->err), S("invalid base 99")));
    }
    CHECK(str_eq(error_text(kept),
                 S("strconv.ParseInt: parsing \"12\": invalid base 99")));
}

TEST(results_own_exactly_their_length) {
    Track tr;
    track_init(&tr, heap_allocator());
    Alloc *t = track_allocator(&tr);

    Str s = strconv_format_int(t, INT64_MIN, 2);
    CHECK(s.len == 65);
    mem_free(t, (void *)(Uintptr)s.p, (size_t)s.len, 1);

    s = strconv_itoa(t, 7);
    CHECK(str_eq(s, S("7")));
    mem_free(t, (void *)(Uintptr)s.p, (size_t)s.len, 1);

    StrconvNumError e = {S("Atoi"), S("\xff"), strconv_err_syntax};
    s = strconv_num_error_error(t, &e);
    CHECK(str_eq(s, S("strconv.Atoi: parsing \"\\xff\": invalid syntax")));
    mem_free(t, (void *)(Uintptr)s.p, (size_t)s.len, 1);

    CHECK(track_check(&tr) == 0);
    track_free(&tr);
}

int main(void) {
    arena_init(&ar, NULL, 0);
    a = arena_allocator(&ar);

    RUN(parse_uint64);
    RUN(parse_uint64_base);
    RUN(parse_int64);
    RUN(parse_int64_base);
    RUN(parse_uint32);
    RUN(parse_int32);
    RUN(parse_int_size);
    RUN(atoi_without_an_error);
    RUN(parse_bit_size);
    RUN(parse_base);
    RUN(itoa);
    RUN(uitoa);
    RUN(format_uint_varlen);
    RUN(format_every_base);
    RUN(format_illegal_base);
    RUN(parse_bool);
    RUN(format_bool);
    RUN(append_bool);
    RUN(num_error);
    RUN(num_error_unwrap);
    RUN(num_error_survives_retain);
    RUN(results_own_exactly_their_length);

    arena_free(&ar);
    return harness_report("strconv number");
}

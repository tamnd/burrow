/* Derived from Go's src/go/constant/value_test.go.
 * Go source: go1.27.1.
 *
 * Copyright 2013 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/go/constant.h"
#include "burrow/mem/arena.h"

#include <math.h>
#include <string.h>

#define S_(lit) BURROW_S_INIT(lit)
#define NELEM(x) ((Int)(sizeof(x) / sizeof((x)[0])))

static const Str int_tests[] = {
    S_("0_123 = 0123"),
    S_("0123_456 = 0123456"),
    S_("1_234 = 1234"),
    S_("1_234_567 = 1234567"),
    S_("0X_0 = 0"),
    S_("0X_1234 = 0x1234"),
    S_("0X_CAFE_f00d = 0xcafef00d"),
    S_("0o0 = 0"),
    S_("0o1234 = 01234"),
    S_("0o01234567 = 01234567"),
    S_("0O0 = 0"),
    S_("0O1234 = 01234"),
    S_("0O01234567 = 01234567"),
    S_("0o_0 = 0"),
    S_("0o_1234 = 01234"),
    S_("0o0123_4567 = 01234567"),
    S_("0O_0 = 0"),
    S_("0O_1234 = 01234"),
    S_("0O0123_4567 = 01234567"),
    S_("0b0 = 0"),
    S_("0b1011 = 0xb"),
    S_("0b00101101 = 0x2d"),
    S_("0B0 = 0"),
    S_("0B1011 = 0xb"),
    S_("0B00101101 = 0x2d"),
    S_("0b_0 = 0"),
    S_("0b10_11 = 0xb"),
    S_("0b_0010_1101 = 0x2d"),
};

static const Str float_tests[] = {
    S_("1_2_3. = 123."),
    S_("0_123. = 123."),
    S_("0_0e0 = 0."),
    S_("1_2_3e0 = 123."),
    S_("0_123e0 = 123."),
    S_("0e-0_0 = 0."),
    S_("1_2_3E+0 = 123."),
    S_("0123E1_2_3 = 123e123"),
    S_("0.e+1 = 0."),
    S_("123.E-1_0 = 123e-10"),
    S_("01_23.e123 = 123e123"),
    S_(".0e-1 = .0"),
    S_(".123E+10 = .123e10"),
    S_(".0123E123 = .0123e123"),
    S_("1_2_3.123 = 123.123"),
    S_("0123.01_23 = 123.0123"),
    S_("1e-1000000000 = 0"),
    S_("1e+1000000000 = ?"),
    S_("6e5518446744 = ?"),
    S_("-6e5518446744 = ?"),
    S_("0x0.p+0 = 0."),
    S_("0Xdeadcafe.p-10 = 0xdeadcafe/1024"),
    S_("0x1234.P84 = 0x1234000000000000000000000"),
    S_("0x.1p-0 = 1/16"),
    S_("0X.deadcafep4 = 0xdeadcafe/0x10000000"),
    S_("0x.1234P+12 = 0x1234/0x10"),
    S_("0x0p0 = 0."),
    S_("0Xdeadcafep+1 = 0x1bd5b95fc"),
    S_("0x1234P-10 = 0x1234/1024"),
    S_("0x0.0p0 = 0."),
    S_("0Xdead.cafep+1 = 0x1bd5b95fc/0x10000"),
    S_("0x12.34P-10 = 0x1234/0x40000"),
    S_("0Xdead_cafep+1 = 0xdeadcafep+1"),
    S_("0x_1234P-10 = 0x1234p-10"),
    S_("0X_dead_cafe.p-10 = 0xdeadcafe.p-10"),
    S_("0x12_34.P1_2_3 = 0x1234.p123"),
};

static const Str imag_tests[] = {
    S_("1_234i = 1234i"),
    S_("1_234_567i = 1234567i"),
    S_("0.i = 0i"),
    S_("123.i = 123i"),
    S_("0123.i = 123i"),
    S_("0.e+1i = 0i"),
    S_("123.E-1_0i = 123e-10i"),
    S_("01_23.e123i = 123e123i"),
    S_("1e-1000000000i = 0i"),
    S_("1e+1000000000i = ?"),
    S_("6e5518446744i = ?"),
    S_("-6e5518446744i = ?"),
};

static const Str op_tests[] = {
    S_("+ 0 = 0"),
    S_("+ ? = ?"),
    S_("- 1 = -1"),
    S_("- ? = ?"),
    S_("^ 0 = -1"),
    S_("^ ? = ?"),
    S_("! true = false"),
    S_("! false = true"),
    S_("! ? = ?"),
    S_("\"\" + \"\" = \"\""),
    S_("\"foo\" + \"\" = \"foo\""),
    S_("\"\" + \"bar\" = \"bar\""),
    S_("\"foo\" + \"bar\" = \"foobar\""),
    S_("0 + 0 = 0"),
    S_("0 + 0.1 = 0.1"),
    S_("0 + 0.1i = 0.1i"),
    S_("0.1 + 0.9 = 1"),
    S_("1e100 + 1e100 = 2e100"),
    S_("? + 0 = ?"),
    S_("0 + ? = ?"),
    S_("0 - 0 = 0"),
    S_("0 - 0.1 = -0.1"),
    S_("0 - 0.1i = -0.1i"),
    S_("1e100 - 1e100 = 0"),
    S_("? - 0 = ?"),
    S_("0 - ? = ?"),
    S_("0 * 0 = 0"),
    S_("1 * 0.1 = 0.1"),
    S_("1 * 0.1i = 0.1i"),
    S_("1i * 1i = -1"),
    S_("? * 0 = ?"),
    S_("0 * ? = ?"),
    S_("0 * 1e+1000000000 = ?"),
    S_("0 / 0 = \"division_by_zero\""),
    S_("10 / 2 = 5"),
    S_("5 / 3 = 5/3"),
    S_("5i / 3i = 5/3"),
    S_("? / 0 = ?"),
    S_("0 / ? = ?"),
    S_("0 * 1e+1000000000i = ?"),
    S_("0 % 0 = \"runtime_error:_integer_divide_by_zero\""),
    S_("10 % 3 = 1"),
    S_("? % 0 = ?"),
    S_("0 % ? = ?"),
    S_("0 & 0 = 0"),
    S_("12345 & 0 = 0"),
    S_("0xff & 0xf = 0xf"),
    S_("? & 0 = ?"),
    S_("0 & ? = ?"),
    S_("0 | 0 = 0"),
    S_("12345 | 0 = 12345"),
    S_("0xb | 0xa0 = 0xab"),
    S_("? | 0 = ?"),
    S_("0 | ? = ?"),
    S_("0 ^ 0 = 0"),
    S_("1 ^ -1 = -2"),
    S_("? ^ 0 = ?"),
    S_("0 ^ ? = ?"),
    S_("0 &^ 0 = 0"),
    S_("0xf &^ 1 = 0xe"),
    S_("1 &^ 0xf = 0"),
    S_("0 << 0 = 0"),
    S_("1 << 10 = 1024"),
    S_("0 >> 0 = 0"),
    S_("1024 >> 10 == 1"),
    S_("? << 0 == ?"),
    S_("? >> 10 == ?"),
    S_("false == false = true"),
    S_("false == true = false"),
    S_("true == false = false"),
    S_("true == true = true"),
    S_("false != false = false"),
    S_("false != true = true"),
    S_("true != false = true"),
    S_("true != true = false"),
    S_("\"foo\" == \"bar\" = false"),
    S_("\"foo\" != \"bar\" = true"),
    S_("\"foo\" < \"bar\" = false"),
    S_("\"foo\" <= \"bar\" = false"),
    S_("\"foo\" > \"bar\" = true"),
    S_("\"foo\" >= \"bar\" = true"),
    S_("0 == 0 = true"),
    S_("0 != 0 = false"),
    S_("0 < 10 = true"),
    S_("10 <= 10 = true"),
    S_("0 > 10 = false"),
    S_("10 >= 10 = true"),
    S_("1/123456789 == 1/123456789 == true"),
    S_("1/123456789 != 1/123456789 == false"),
    S_("1/123456789 < 1/123456788 == true"),
    S_("1/123456788 <= 1/123456789 == false"),
    S_("0.11 > 0.11 = false"),
    S_("0.11 >= 0.11 = true"),
    S_("? == 0 = false"),
    S_("? != 0 = false"),
    S_("? < 10 = false"),
    S_("? <= 10 = false"),
    S_("? > 10 = false"),
    S_("? >= 10 = false"),
    S_("0 == ? = false"),
    S_("0 != ? = false"),
    S_("0 < ? = false"),
    S_("10 <= ? = false"),
    S_("0 > ? = false"),
    S_("10 >= ? = false"),
};

static const Str frac_tests[] = {
    S_("0"),
    S_("1"),
    S_("-1"),
    S_("1.2"),
    S_("-0.991"),
    S_("2.718281828"),
    S_("3.14159265358979323e-10"),
    S_("1e100"),
    S_("1e1000"),
};

static const Str bytes_tests[] = {
    S_("0"),
    S_("1"),
    S_("123456789"),
    S_("123456789012345678901234567890123456789012345678901234567890"),
};

static const struct {
    Str input, short_, exact;
} string_tests[] = {
    {S_(""), S_("unknown"), S_("unknown")},
    {S_("0x"), S_("unknown"), S_("unknown")},
    {S_("'"), S_("unknown"), S_("unknown")},
    {S_("1f0"), S_("unknown"), S_("unknown")},
    {S_("unknown"), S_("unknown"), S_("unknown")},
    {S_("true"), S_("true"), S_("true")},
    {S_("false"), S_("false"), S_("false")},
    {S_("\"\""), S_("\"\""), S_("\"\"")},
    {S_("\"foo\""), S_("\"foo\""), S_("\"foo\"")},
    {S_("\"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\""),
     S_("\"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\""),
     S_("\"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\"")},
    {S_("\"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\""),
     S_("\"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx..."),
     S_("\"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\"")},
    {S_("\"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\""),
     S_("\"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx..."),
     S_("\"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\"")},
    {S_("\"\xd8\xa8\xd9\x85\xd9\x88\xd8\xac\xd8\xa8 "
        "\xd8\xa7\xd9\x84\xd8\xb4\xd8\xb1\xd9\x88\xd8\xb7 "
        "\xd8\xa7\xd9\x84\xd8\xaa\xd8\xa7\xd9\x84\xd9\x8a\xd8\xa9 "
        "\xd9\x86\xd8\xb3\xd8\xa8 \xd8\xa7\xd9\x84\xd9\x85\xd8\xb5\xd9\x86\xd9\x81 "
        "\xe2\x80\x94 \xd9\x8a\xd8\xac\xd8\xa8 \xd8\xb9\xd9\x84\xd9\x8a\xd9\x83 "
        "\xd8\xa3\xd9\x86 \xd8\xaa\xd9\x86\xd8\xb3\xd8\xa8 "
        "\xd8\xa7\xd9\x84\xd8\xb9\xd9\x85\xd9\x84 "
        "\xd8\xa8\xd8\xa7\xd9\x84\xd8\xb7\xd8\xb1\xd9\x8a\xd9\x82\xd8\xa9 "
        "\xd8\xa7\xd9\x84\xd8\xaa\xd9\x8a "
        "\xd8\xaa\xd8\xad\xd8\xaf\xd8\xaf\xd9\x87\xd8\xa7 "
        "\xd8\xa7\xd9\x84\xd9\x85\xd8\xa4\xd9\x84\xd9\x81 \xd8\xa3\xd9\x88 "
        "\xd8\xa7\xd9\x84\xd9\x85\xd8\xb1\xd8\xae\xd8\xb5 "
        "(\xd9\x88\xd9\x84\xd9\x83\xd9\x86 \xd9\x84\xd9\x8a\xd8\xb3 "
        "\xd8\xa8\xd8\xa3\xd9\x8a \xd8\xad\xd8\xa7\xd9\x84 \xd9\x85\xd9\x86 "
        "\xd8\xa7\xd9\x84\xd8\xa3\xd8\xad\xd9\x88\xd8\xa7\xd9\x84 \xd8\xa3\xd9\x86 "
        "\xd8\xaa\xd9\x88\xd8\xad\xd9\x8a "
        "\xd9\x88\xd8\xaa\xd9\x82\xd8\xaa\xd8\xb1\xd8\xad "
        "\xd8\xa8\xd8\xaa\xd8\xad\xd9\x88\xd9\x84 \xd8\xa3\xd9\x88 "
        "\xd8\xa7\xd8\xb3\xd8\xaa\xd8\xae\xd8\xaf\xd8\xa7\xd9\x85\xd9\x83 "
        "\xd9\x84\xd9\x84\xd8\xb9\xd9\x85\xd9\x84).  "
        "\xd8\xa7\xd9\x84\xd9\x85\xd8\xb4\xd8\xa7\xd8\xb1\xd9\x83\xd8\xa9 "
        "\xd8\xb9\xd9\x84\xd9\x89 \xd9\x82\xd8\xaf\xd9\x85 "
        "\xd8\xa7\xd9\x84\xd9\x85\xd8\xb3\xd8\xa7\xd9\x88\xd8\xa7\xd8\xa9 \xe2\x80\x94 "
        "\xd8\xa5\xd8\xb0\xd8\xa7 \xd9\x83\xd9\x86\xd8\xaa "
        "\xd9\x8a\xd8\xb9\xd8\xaf\xd9\x84 \xd8\x8c "
        "\xd9\x88\xd8\xa7\xd9\x84\xd8\xaa\xd8\xba\xd9\x8a\xd9\x8a\xd8\xb1 \xd8\x8c "
        "\xd8\xa3\xd9\x88 "
        "\xd8\xa7\xd9\x84\xd8\xa7\xd8\xb3\xd8\xaa\xd9\x81\xd8\xa7\xd8\xaf\xd8\xa9 "
        "\xd9\x85\xd9\x86 \xd9\x87\xd8\xb0\xd8\xa7 "
        "\xd8\xa7\xd9\x84\xd8\xb9\xd9\x85\xd9\x84 \xd8\x8c \xd9\x82\xd8\xaf "
        "\xd9\x8a\xd9\x86\xd8\xaa\xd8\xac \xd8\xb9\xd9\x86 "
        "\xd8\xaa\xd9\x88\xd8\xb2\xd9\x8a\xd8\xb9 "
        "\xd8\xa7\xd9\x84\xd8\xb9\xd9\x85\xd9\x84 \xd8\xa5\xd9\x84\xd8\xa7 "
        "\xd9\x81\xd9\x8a \xd8\xb8\xd9\x84 \xd8\xaa\xd8\xb4\xd8\xa7\xd8\xa8\xd9\x87 "
        "\xd8\xa7\xd9\x88 \xd8\xaa\xd8\xb7\xd8\xa7\xd8\xa8\xd9\x82 \xd9\x81\xd9\x89 "
        "\xd9\x88\xd8\xa7\xd8\xad\xd8\xaf \xd9\x84\xd9\x87\xd8\xb0\xd8\xa7 "
        "\xd8\xa7\xd9\x84\xd8\xaa\xd8\xb1\xd8\xae\xd9\x8a\xd8\xb5.\""),
     S_("\"\xd8\xa8\xd9\x85\xd9\x88\xd8\xac\xd8\xa8 "
        "\xd8\xa7\xd9\x84\xd8\xb4\xd8\xb1\xd9\x88\xd8\xb7 "
        "\xd8\xa7\xd9\x84\xd8\xaa\xd8\xa7\xd9\x84\xd9\x8a\xd8\xa9 "
        "\xd9\x86\xd8\xb3\xd8\xa8 \xd8\xa7\xd9\x84\xd9\x85\xd8\xb5\xd9\x86\xd9\x81 "
        "\xe2\x80\x94 \xd9\x8a\xd8\xac\xd8\xa8 \xd8\xb9\xd9\x84\xd9\x8a\xd9\x83 "
        "\xd8\xa3\xd9\x86 \xd8\xaa\xd9\x86\xd8\xb3\xd8\xa8 "
        "\xd8\xa7\xd9\x84\xd8\xb9\xd9\x85\xd9\x84 "
        "\xd8\xa8\xd8\xa7\xd9\x84\xd8\xb7\xd8\xb1\xd9\x8a\xd9\x82\xd8\xa9 "
        "\xd8\xa7\xd9\x84..."),
     S_("\"\xd8\xa8\xd9\x85\xd9\x88\xd8\xac\xd8\xa8 "
        "\xd8\xa7\xd9\x84\xd8\xb4\xd8\xb1\xd9\x88\xd8\xb7 "
        "\xd8\xa7\xd9\x84\xd8\xaa\xd8\xa7\xd9\x84\xd9\x8a\xd8\xa9 "
        "\xd9\x86\xd8\xb3\xd8\xa8 \xd8\xa7\xd9\x84\xd9\x85\xd8\xb5\xd9\x86\xd9\x81 "
        "\xe2\x80\x94 \xd9\x8a\xd8\xac\xd8\xa8 \xd8\xb9\xd9\x84\xd9\x8a\xd9\x83 "
        "\xd8\xa3\xd9\x86 \xd8\xaa\xd9\x86\xd8\xb3\xd8\xa8 "
        "\xd8\xa7\xd9\x84\xd8\xb9\xd9\x85\xd9\x84 "
        "\xd8\xa8\xd8\xa7\xd9\x84\xd8\xb7\xd8\xb1\xd9\x8a\xd9\x82\xd8\xa9 "
        "\xd8\xa7\xd9\x84\xd8\xaa\xd9\x8a "
        "\xd8\xaa\xd8\xad\xd8\xaf\xd8\xaf\xd9\x87\xd8\xa7 "
        "\xd8\xa7\xd9\x84\xd9\x85\xd8\xa4\xd9\x84\xd9\x81 \xd8\xa3\xd9\x88 "
        "\xd8\xa7\xd9\x84\xd9\x85\xd8\xb1\xd8\xae\xd8\xb5 "
        "(\xd9\x88\xd9\x84\xd9\x83\xd9\x86 \xd9\x84\xd9\x8a\xd8\xb3 "
        "\xd8\xa8\xd8\xa3\xd9\x8a \xd8\xad\xd8\xa7\xd9\x84 \xd9\x85\xd9\x86 "
        "\xd8\xa7\xd9\x84\xd8\xa3\xd8\xad\xd9\x88\xd8\xa7\xd9\x84 \xd8\xa3\xd9\x86 "
        "\xd8\xaa\xd9\x88\xd8\xad\xd9\x8a "
        "\xd9\x88\xd8\xaa\xd9\x82\xd8\xaa\xd8\xb1\xd8\xad "
        "\xd8\xa8\xd8\xaa\xd8\xad\xd9\x88\xd9\x84 \xd8\xa3\xd9\x88 "
        "\xd8\xa7\xd8\xb3\xd8\xaa\xd8\xae\xd8\xaf\xd8\xa7\xd9\x85\xd9\x83 "
        "\xd9\x84\xd9\x84\xd8\xb9\xd9\x85\xd9\x84).  "
        "\xd8\xa7\xd9\x84\xd9\x85\xd8\xb4\xd8\xa7\xd8\xb1\xd9\x83\xd8\xa9 "
        "\xd8\xb9\xd9\x84\xd9\x89 \xd9\x82\xd8\xaf\xd9\x85 "
        "\xd8\xa7\xd9\x84\xd9\x85\xd8\xb3\xd8\xa7\xd9\x88\xd8\xa7\xd8\xa9 \xe2\x80\x94 "
        "\xd8\xa5\xd8\xb0\xd8\xa7 \xd9\x83\xd9\x86\xd8\xaa "
        "\xd9\x8a\xd8\xb9\xd8\xaf\xd9\x84 \xd8\x8c "
        "\xd9\x88\xd8\xa7\xd9\x84\xd8\xaa\xd8\xba\xd9\x8a\xd9\x8a\xd8\xb1 \xd8\x8c "
        "\xd8\xa3\xd9\x88 "
        "\xd8\xa7\xd9\x84\xd8\xa7\xd8\xb3\xd8\xaa\xd9\x81\xd8\xa7\xd8\xaf\xd8\xa9 "
        "\xd9\x85\xd9\x86 \xd9\x87\xd8\xb0\xd8\xa7 "
        "\xd8\xa7\xd9\x84\xd8\xb9\xd9\x85\xd9\x84 \xd8\x8c \xd9\x82\xd8\xaf "
        "\xd9\x8a\xd9\x86\xd8\xaa\xd8\xac \xd8\xb9\xd9\x86 "
        "\xd8\xaa\xd9\x88\xd8\xb2\xd9\x8a\xd8\xb9 "
        "\xd8\xa7\xd9\x84\xd8\xb9\xd9\x85\xd9\x84 \xd8\xa5\xd9\x84\xd8\xa7 "
        "\xd9\x81\xd9\x8a \xd8\xb8\xd9\x84 \xd8\xaa\xd8\xb4\xd8\xa7\xd8\xa8\xd9\x87 "
        "\xd8\xa7\xd9\x88 \xd8\xaa\xd8\xb7\xd8\xa7\xd8\xa8\xd9\x82 \xd9\x81\xd9\x89 "
        "\xd9\x88\xd8\xa7\xd8\xad\xd8\xaf \xd9\x84\xd9\x87\xd8\xb0\xd8\xa7 "
        "\xd8\xa7\xd9\x84\xd8\xaa\xd8\xb1\xd8\xae\xd9\x8a\xd8\xb5.\"")},
    {S_("0"), S_("0"), S_("0")},
    {S_("-1"), S_("-1"), S_("-1")},
    {S_("12345"), S_("12345"), S_("12345")},
    {S_("-12345678901234567890"), S_("-12345678901234567890"),
     S_("-12345678901234567890")},
    {S_("12345678901234567890"), S_("12345678901234567890"),
     S_("12345678901234567890")},
    {S_("0."), S_("0"), S_("0")},
    {S_("-0.0"), S_("0"), S_("0")},
    {S_("10.0"), S_("10"), S_("10")},
    {S_("2.1"), S_("2.1"), S_("21/10")},
    {S_("-2.1"), S_("-2.1"), S_("-21/10")},
    {S_("1e9999"), S_("1e+9999"),
     S_("0x."
        "f8d4a9da224650a8cb2959e10d985ad92adbd44c62917e608b1f24c0e1b76b6f61edffeb15c135"
        "a4b601637315f7662f325f82325422b244286a07663c9415d2p+33216")},
    {S_("1e-9999"), S_("1e-9999"),
     S_("0x."
        "83b01ba6d8c0425eec1b21e96f7742d63c2653ed0a024cf8a2f9686df578d7b07d7a83d84df6a2"
        "ec70a921d1f6cd5574893a7eda4d28ee719e13a5dce2700759p-33215")},
    {S_("2.71828182845904523536028747135266249775724709369995957496696763"),
     S_("2.71828"),
     S_("271828182845904523536028747135266249775724709369995957496696763/"
        "100000000000000000000000000000000000000000000000000000000000000")},
    {S_("0e9999999999"), S_("0"), S_("0")},
    {S_("-6e-1886451601"), S_("0"), S_("0")},
    {S_("0i"), S_("(0 + 0i)"), S_("(0 + 0i)")},
    {S_("-0i"), S_("(0 + 0i)"), S_("(0 + 0i)")},
    {S_("10i"), S_("(0 + 10i)"), S_("(0 + 10i)")},
    {S_("-10i"), S_("(0 + -10i)"), S_("(0 + -10i)")},
    {S_("1e9999i"), S_("(0 + 1e+9999i)"),
     S_("(0 + "
        "0x."
        "f8d4a9da224650a8cb2959e10d985ad92adbd44c62917e608b1f24c0e1b76b6f61edffeb15c135"
        "a4b601637315f7662f325f82325422b244286a07663c9415d2p+33216i)")},
};

static const struct {
    int64_t val;
    Int want;
} bit_len_tests[] = {
    {INT64_C(0), 0},
    {INT64_C(1), 1},
    {INT64_C(-16), 5},
    {INT64_C(2305843009213693952), 62},
    {INT64_C(4611686018427387904), 63},
    {INT64_C(-4611686018427387904), 63},
    {INT64_MIN, 64},
};

/* ------------------------------------------------------------ support */

static Str vstr(Alloc *a, ConstantValue x) {
    return constant_value_string(x, a);
}

static ConstantValue val(Alloc *a, Str lit) {
    if (lit.len == 0 || str_eq(lit, BURROW_S("?")))
        return constant_make_unknown();
    if (str_eq(lit, BURROW_S("true")))
        return constant_make_bool(true);
    if (str_eq(lit, BURROW_S("false")))
        return constant_make_bool(false);

    Str bs;
    bool found;
    Str as = strings_cut(lit, BURROW_S("/"), &bs, &found);
    if (found) {
        /* assume fraction */
        ConstantValue x = constant_make_from_literal(a, as, TOKEN_INT, 0);
        ConstantValue y = constant_make_from_literal(a, bs, TOKEN_INT, 0);
        return constant_binary_op(a, x, TOKEN_QUO, y);
    }

    Token tok = TOKEN_INT;
    Byte first = lit.p[0], last = lit.p[lit.len - 1];
    if (first == '"' || first == '`') {
        tok = TOKEN_STRING;
        lit = strings_replace_all(a, lit, BURROW_S("_"), BURROW_S(" "));
    } else if (first == '\'') {
        tok = TOKEN_CHAR;
    } else if (last == 'i') {
        tok = TOKEN_IMAG;
    } else if (!strings_has_prefix(lit, BURROW_S("0x")) &&
               strings_contains_any(lit, BURROW_S("./Ee"))) {
        tok = TOKEN_FLOAT;
    }
    return constant_make_from_literal(a, lit, tok, 0);
}

static const struct {
    Str name;
    Token tok;
} optab[] = {
    {S_("!"), TOKEN_NOT},  {S_("+"), TOKEN_ADD},  {S_("-"), TOKEN_SUB},
    {S_("*"), TOKEN_MUL},  {S_("/"), TOKEN_QUO},  {S_("%"), TOKEN_REM},
    {S_("<<"), TOKEN_SHL}, {S_(">>"), TOKEN_SHR}, {S_("&"), TOKEN_AND},
    {S_("|"), TOKEN_OR},   {S_("^"), TOKEN_XOR},  {S_("&^"), TOKEN_AND_NOT},
    {S_("=="), TOKEN_EQL}, {S_("!="), TOKEN_NEQ}, {S_("<"), TOKEN_LSS},
    {S_("<="), TOKEN_LEQ}, {S_(">"), TOKEN_GTR},  {S_(">="), TOKEN_GEQ},
};

static bool lookup_op(Str name, Token *tok) {
    for (Int i = 0; i < NELEM(optab); i++) {
        if (str_eq(optab[i].name, name)) {
            *tok = optab[i].tok;
            return true;
        }
    }
    return false;
}

static ConstantValue run_op(Alloc *a, const ConstantValue *x, Token op,
                            ConstantValue y) {
    if (x == NULL)
        return constant_unary_op(a, op, y, 0);
    switch (op) {
    case TOKEN_EQL:
    case TOKEN_NEQ:
    case TOKEN_LSS:
    case TOKEN_LEQ:
    case TOKEN_GTR:
    case TOKEN_GEQ:
        return constant_make_bool(constant_compare(*x, op, y));
    case TOKEN_SHL:
    case TOKEN_SHR: {
        int64_t s = constant_int64_val(y, NULL);
        return constant_shift(a, *x, op, (Uint)s);
    }
    default:
        return constant_binary_op(a, *x, op, y);
    }
}

/* Go's doOp with its panicHandler: a panic comes back as a String holding its
 * message. The result goes out through z, which lives in the caller, so that
 * it survives the jump back into the catch. */
static void do_op(Alloc *a, const ConstantValue *x, Token op, ConstantValue y,
                  ConstantValue *z) {
    BURROW_TRY {
        *z = run_op(a, x, op, y);
    }
    BURROW_CATCH(r) {
        *z = constant_make_string(a, panic_text(r));
    }
    BURROW_TRY_END;
}

static bool is_unknown(ConstantValue x) {
    return constant_value_kind(x) == CONSTANT_UNKNOWN;
}

static bool eql(ConstantValue x, ConstantValue y) {
    bool ux = is_unknown(x), uy = is_unknown(y);
    if (ux || uy)
        return ux == uy;
    return constant_compare(x, TOKEN_EQL, y);
}

/* ------------------------------------------------------------ literals */

static void test_numbers(TestingT *t, Alloc *a, Token kind, const Str *tests, Int n) {
    for (Int i = 0; i < n; i++) {
        Str test = tests[i];
        Str rhs;
        bool ok;
        Str lhs = strings_cut(test, BURROW_S(" = "), &rhs, &ok);
        if (!ok) {
            testing_t_errorf_v(t, "invalid test case: %s", test);
            continue;
        }

        ConstantValue x = constant_make_from_literal(a, lhs, kind, 0);
        ConstantValue y;
        if (str_eq(rhs, BURROW_S("?"))) {
            y = constant_make_unknown();
        } else {
            Str ds;
            bool frac;
            Str ns = strings_cut(rhs, BURROW_S("/"), &ds, &frac);
            if (frac && kind == TOKEN_FLOAT) {
                ConstantValue nv = constant_make_from_literal(a, ns, TOKEN_INT, 0);
                ConstantValue dv = constant_make_from_literal(a, ds, TOKEN_INT, 0);
                y = constant_binary_op(a, nv, TOKEN_QUO, dv);
            } else {
                y = constant_make_from_literal(a, rhs, kind, 0);
            }
            if (is_unknown(y)) {
                testing_t_fatalf_v(t, "invalid test case: %s %d", test,
                                   constant_value_kind(y));
            }
        }

        ConstantKind xk = constant_value_kind(x);
        ConstantKind yk = constant_value_kind(y);
        if (xk != yk) {
            testing_t_errorf_v(t, "%s: got kind %d != %d", test, xk, yk);
            continue;
        }
        if (yk == CONSTANT_UNKNOWN)
            continue;
        if (!constant_compare(x, TOKEN_EQL, y))
            testing_t_errorf_v(t, "%s: %s != %s", test, vstr(a, x), vstr(a, y));
    }
}

static void TestNumbers(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    test_numbers(t, a, TOKEN_INT, int_tests, NELEM(int_tests));
    test_numbers(t, a, TOKEN_FLOAT, float_tests, NELEM(float_tests));
    test_numbers(t, a, TOKEN_IMAG, imag_tests, NELEM(imag_tests));
    arena_free(&ar);
}

/* ---------------------------------------------------------- operations */

static void TestOps(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int k = 0; k < NELEM(op_tests); k++) {
        Str test = op_tests[k];
        Slice f = strings_split(a, test, BURROW_S(" "));
        Int i = 0; /* operator index */

        ConstantValue x = {0}, x0 = {0};
        bool binary = false;
        switch (f.len) {
        case 4:
            /* unary operation */
            break;
        case 5:
            /* binary operation */
            x = val(a, BURROW_AT(Str, f, 0));
            x0 = val(a, BURROW_AT(Str, f, 0));
            binary = true;
            i = 1;
            break;
        default:
            testing_t_errorf_v(t, "invalid test case: %s", test);
            continue;
        }

        Token op = TOKEN_ILLEGAL;
        if (!lookup_op(BURROW_AT(Str, f, i), &op))
            testing_t_fatalf_v(t, "missing optab entry for %s", BURROW_AT(Str, f, i));

        ConstantValue y = val(a, BURROW_AT(Str, f, i + 1));
        ConstantValue y0 = val(a, BURROW_AT(Str, f, i + 1));

        ConstantValue got = {0};
        do_op(a, binary ? &x : NULL, op, y, &got);
        ConstantValue want = val(a, BURROW_AT(Str, f, i + 3));
        if (!eql(got, want)) {
            testing_t_errorf_v(t, "%s: got %s; want %s", test, vstr(a, got),
                               vstr(a, want));
            continue;
        }
        if (binary && !eql(x, x0)) {
            testing_t_errorf_v(t, "%s: x changed to %s", test, vstr(a, x));
            continue;
        }
        if (!eql(y, y0)) {
            testing_t_errorf_v(t, "%s: y changed to %s", test, vstr(a, y));
            continue;
        }
    }
    arena_free(&ar);
}

/* --------------------------------------------------------------- strings */

static void TestString(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NELEM(string_tests); i++) {
        ConstantValue x = val(a, string_tests[i].input);
        Str got = constant_value_string(x, a);
        if (!str_eq(got, string_tests[i].short_))
            testing_t_errorf_v(t, "%s: got %q; want %q as short string",
                               string_tests[i].input, got, string_tests[i].short_);
        got = constant_value_exact_string(x, a);
        if (!str_eq(got, string_tests[i].exact))
            testing_t_errorf_v(t, "%s: got %q; want %q as exact string",
                               string_tests[i].input, got, string_tests[i].exact);
    }
    arena_free(&ar);
}

static void TestStringLen(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    struct {
        ConstantValue x;
        int64_t want;
    } tests[] = {
        {constant_make_unknown(), 0},
        {val(a, BURROW_S("\"\"")), 0},
        {val(a, BURROW_S("\"foo\"")), 3},
        {val(a, BURROW_S("\"\xe4\xb8\x96\xe7\x95\x8c\"")), 6},
        {constant_binary_op(a, val(a, BURROW_S("\"foo\"")), TOKEN_ADD,
                            val(a, BURROW_S("\"bar\""))),
         6},
        {constant_binary_op(a, val(a, BURROW_S("\"\xe4\xb8\x96\xe7\x95\x8c\"")),
                            TOKEN_ADD, val(a, BURROW_S("\"!\""))),
         7},
        {constant_binary_op(a, val(a, BURROW_S("\"a\"")), TOKEN_ADD,
                            constant_binary_op(a, val(a, BURROW_S("\"b\"")), TOKEN_ADD,
                                               val(a, BURROW_S("\"c\"")))),
         3},
    };
    for (Int i = 0; i < NELEM(tests); i++) {
        int64_t got = constant_string_len(tests[i].x);
        if (got != tests[i].want)
            testing_t_errorf_v(t, "StringLen(%s): got %d; want %d", vstr(a, tests[i].x),
                               got, tests[i].want);
    }
    arena_free(&ar);
}

/* ------------------------------------------------------------- the rest */

static void TestFractions(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NELEM(frac_tests); i++) {
        ConstantValue x = val(a, frac_tests[i]);
        /* Go doesn't check the numerator and denominator themselves, since
         * floatVal rounding makes them unlikely to be exactly right. It works
         * the fraction out again and compares the rounded result. */
        ConstantValue q =
            constant_binary_op(a, constant_num(a, x), TOKEN_QUO, constant_denom(a, x));
        Str got = vstr(a, q);
        Str want = vstr(a, x);
        if (!str_eq(got, want))
            testing_t_errorf_v(t, "%s: got quotient %s, want %s", want, got, want);
    }
    arena_free(&ar);
}

static void TestBytes(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NELEM(bytes_tests); i++) {
        Str test = bytes_tests[i];
        ConstantValue x = val(a, test);
        Slice bytes = constant_bytes(a, x);

        /* special case 0 */
        if (constant_sign(x) == 0 && bytes.len != 0)
            testing_t_errorf_v(t, "%s: got %d bytes; want empty byte slice", test,
                               bytes.len);
        Int n = bytes.len;
        if (n > 0 && BURROW_AT(Byte, bytes, n - 1) == 0)
            testing_t_errorf_v(t, "%s: got a leading 0 byte", test);

        ConstantValue got = constant_make_from_bytes(a, bytes);
        if (!eql(got, x))
            testing_t_errorf_v(t, "%s: got %s; want %s", test, vstr(a, got),
                               vstr(a, x));
    }
    arena_free(&ar);
}

static void TestUnknown(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    ConstantValue u = constant_make_unknown();
    ConstantValue values[] = {
        u,
        constant_make_bool(false), /* the + below is never looked at */
        constant_make_string(a, BURROW_S("")),
        constant_make_int64(1),
        constant_make_from_literal(a, BURROW_S("''"), TOKEN_CHAR, 0),
        constant_make_from_literal(
            a, BURROW_S("-1234567890123456789012345678901234567890"), TOKEN_INT, 0),
        constant_make_float64(a, 1.2),
        constant_make_imag(a, constant_make_float64(a, 1.2)),
    };
    for (Int k = 0; k < NELEM(values); k++) {
        ConstantValue x = values[k], y = u;
        for (int i = 0; i < 2; i++) {
            if (i == 1) {
                ConstantValue tmp = x;
                x = y;
                y = tmp;
            }
            ConstantValue got = constant_binary_op(a, x, TOKEN_ADD, y);
            if (!is_unknown(got))
                testing_t_errorf_v(t, "%s + %s: got %s; want %s", vstr(a, x),
                                   vstr(a, y), vstr(a, got), vstr(a, u));
            if (constant_compare(x, TOKEN_EQL, y))
                testing_t_errorf_v(t, "%s == %s: got true; want false", vstr(a, x),
                                   vstr(a, y));
            if (constant_compare(x, TOKEN_NEQ, y))
                testing_t_errorf_v(t, "%s != %s: got true; want false", vstr(a, x),
                                   vstr(a, y));
        }
    }
    arena_free(&ar);
}

static uint64_t float64_bits(double f) {
    uint64_t u;
    memcpy(&u, &f, sizeof u);
    return u;
}

static void TestMakeFloat64(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    volatile double zero = 0;
    double args[] = {
        -MATH_MAX_FLOAT32, -10, -0.5, -zero, zero, 1, 10, 123456789.87654321e-23, 1e10,
        MATH_MAX_FLOAT64,
    };
    for (Int i = 0; i < NELEM(args); i++) {
        double arg = args[i];
        ConstantValue v = constant_make_float64(a, arg);
        if (constant_value_kind(v) != CONSTANT_FLOAT)
            testing_t_errorf_v(t, "%v: got kind = %d; want %d", arg,
                               constant_value_kind(v), CONSTANT_FLOAT);

        /* -0.0 is mapped to 0.0 */
        bool exact = false;
        double got = constant_float64_val(v, &exact);
        if (!exact || float64_bits(got) != float64_bits(arg + 0))
            testing_t_errorf_v(t, "%v: got %v (exact = %v)", arg, got, exact);
    }

    /* infinity */
    for (int sign = 0; sign < 2; sign++) {
        double arg = math_inf(sign);
        ConstantValue v = constant_make_float64(a, arg);
        if (!is_unknown(v))
            testing_t_errorf_v(t, "%v: got kind = %d; want %d", arg,
                               constant_value_kind(v), CONSTANT_UNKNOWN);
    }
    arena_free(&ar);
}

/* Go's TestMake compares what Val gives back with ==, which for the big
 * values is the pointer Make was given. Make copies here, so each case says
 * what it wants by value instead, and a float zero by its precision too. */
typedef struct MakeCase {
    TestingT *t;
    Alloc *a;
    Int n;
    ConstantValue v;
    Any got;
} MakeCase;

static bool make_case(MakeCase *c, Any arg, ConstantKind kind, const Type *want) {
    c->n++;
    c->v = constant_make(c->a, arg);
    c->got = constant_val(c->a, c->v);
    if (constant_value_kind(c->v) == kind && c->got.t == want)
        return true;
    testing_t_errorf_v(c->t, "%d: got %s (kind = %d); want kind = %d", c->n,
                       vstr(c->a, c->v), constant_value_kind(c->v), kind);
    return false;
}

static void make_failed(MakeCase *c) {
    testing_t_errorf_v(c->t, "%d: got %s", c->n, vstr(c->a, c->v));
}

static void TestMake(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    MakeCase c = {t, a, 0, {0}, {NULL, NULL}};

    bool b = false;
    if (make_case(&c, BURROW_ANY(TYPE_BOOL, &b), CONSTANT_BOOL, TYPE_BOOL) &&
        *(const bool *)c.got.data != false)
        make_failed(&c);

    Str hello = BURROW_S("hello");
    if (make_case(&c, BURROW_ANY(TYPE_STRING, &hello), CONSTANT_STRING, TYPE_STRING) &&
        !str_eq(*(const Str *)c.got.data, hello))
        make_failed(&c);

    int64_t one = 1;
    if (make_case(&c, BURROW_ANY(TYPE_INT64, &one), CONSTANT_INT, TYPE_INT64) &&
        *(const int64_t *)c.got.data != 1)
        make_failed(&c);

    if (make_case(&c, BURROW_ANY(TYPE_BIG_INT, big_new_int(a, 10)), CONSTANT_INT,
                  TYPE_INT64) &&
        *(const int64_t *)c.got.data != 10)
        make_failed(&c);

    BigInt *lsh62 = big_int_lsh(big_new_int(a, 0), big_new_int(a, 1), 62);
    if (make_case(&c, BURROW_ANY(TYPE_BIG_INT, lsh62), CONSTANT_INT, TYPE_INT64) &&
        *(const int64_t *)c.got.data != INT64_C(1) << 62)
        make_failed(&c);

    BigInt *lsh63 = big_int_lsh(big_new_int(a, 0), big_new_int(a, 1), 63);
    if (make_case(&c, BURROW_ANY(TYPE_BIG_INT, lsh63), CONSTANT_INT, TYPE_BIG_INT) &&
        big_int_cmp(c.got.data, lsh63) != 0)
        make_failed(&c);

    /* a float zero is Go's floatVal0, which has 512 bits */
    if (make_case(&c, BURROW_ANY(TYPE_BIG_FLOAT, big_new_float(a, 0)), CONSTANT_FLOAT,
                  TYPE_BIG_FLOAT) &&
        (big_float_sign(c.got.data) != 0 || big_float_prec(c.got.data) != 512))
        make_failed(&c);

    BigFloat *two = big_new_float(a, 2.0);
    if (make_case(&c, BURROW_ANY(TYPE_BIG_FLOAT, two), CONSTANT_FLOAT,
                  TYPE_BIG_FLOAT) &&
        (big_float_cmp(c.got.data, two) != 0 ||
         big_float_prec(c.got.data) != big_float_prec(two)))
        make_failed(&c);

    BigRat *third = big_new_rat(a, 1, 3);
    if (make_case(&c, BURROW_ANY(TYPE_BIG_RAT, third), CONSTANT_FLOAT, TYPE_BIG_RAT) &&
        big_rat_cmp(c.got.data, third) != 0)
        make_failed(&c);

    arena_free(&ar);
}

static void TestBitLen(TestingT *t) {
    for (Int i = 0; i < NELEM(bit_len_tests); i++) {
        Int got = constant_bit_len(constant_make_int64(bit_len_tests[i].val));
        if (got != bit_len_tests[i].want)
            testing_t_errorf_v(t, "%v: got %v, want %v", bit_len_tests[i].val, got,
                               bit_len_tests[i].want);
    }
}

/* --------------------------------------------------- burrow's own tests */

/* What Go 1.26 gives for the cases below, from a program run against
 * go/constant there: the value, or the message of the panic. */
static const struct {
    Str name;
    bool panics;
    Str want;
} edge_tests[] = {
    {S_("BoolVal(1)"), true, S_("1 not a Bool")},
    {S_("StringVal(true)"), true, S_("true not a String")},
    {S_("Int64Val(1.5)"), true, S_("1.5 not an Int")},
    {S_("Uint64Val(\"a\")"), true, S_("\"a\" not an Int")},
    {S_("Float64Val(1i)"), true, S_("(0 + 1i) not a Float")},
    {S_("BitLen(1.5)"), true, S_("1.5 not an Int")},
    {S_("Sign(true)"), true, S_("true not numeric")},
    {S_("Bytes(1.5)"), true, S_("1.5 not an Int")},
    {S_("Num(1i)"), true, S_("(0 + 1i) not Int or Float")},
    {S_("Real(\"a\")"), true, S_("\"a\" not numeric")},
    {S_("MakeImag(true)"), true, S_("true not Int or Float")},
    {S_("!1"), true, S_("invalid unary operation !1")},
    {S_("-true"), true, S_("invalid unary operation -true")},
    {S_("^1.5"), true, S_("invalid unary operation ^1.5")},
    {S_("*1"), true, S_("invalid unary operation *1")},
    {S_("true + true"), true, S_("invalid binary operation true + true")},
    {S_("\"a\" - \"a\""), true, S_("invalid binary operation \"a\" - \"a\"")},
    {S_("1.5 % 1.5"), true, S_("invalid binary operation 1.5 % 1.5")},
    {S_("1 && 1"), true, S_("invalid binary operation 1 && 1")},
    {S_("1 / 0"), true, S_("division by zero")},
    {S_("1 /= 0"), true, S_("runtime error: integer divide by zero")},
    {S_("1.5 / 0"), true, S_("division by zero")},
    {S_("MinInt64 /= -1"), false, S_("-9223372036854775808")},
    {S_("MinInt64 % -1"), false, S_("0")},
    {S_("\"a\" << 1"), true, S_("invalid shift \"a\" << 1")},
    {S_("1.5 << 1"), true, S_("invalid shift 1.5 << 1")},
    {S_("Shift(1, +, 1)"), true, S_("invalid shift 1 + 1")},
    {S_("-5 >> 70"), false, S_("-1")},
    {S_("true < true"), true, S_("invalid comparison true < true")},
    {S_("Compare(1, +, 1)"), true, S_("invalid comparison 1 + 1")},
    {S_("1i < 1i"), true, S_("invalid comparison (0 + 1i) < (0 + 1i)")},
    {S_("true == \"a\""), true,
     S_("interface conversion: constant.Value is *constant.stringVal, not "
        "constant.boolVal")},
    {S_("\"a\" == true"), true,
     S_("interface conversion: constant.Value is constant.boolVal, not "
        "*constant.stringVal")},
    {S_("true == 1"), false, S_("true")},
    {S_("1 == true"), false, S_("true")},
    {S_("\"a\" + 1"), false, S_("\"aa\"")},
    {S_("MakeFromLiteral(1, IDENT)"), true, S_("IDENT is not a valid token")},
    {S_("MakeFromLiteral(1, INT, 1)"), true,
     S_("MakeFromLiteral called with non-zero last argument")},
    {S_("Kind(-1)"), false, S_("Kind(-1)")},
    {S_("Kind(6)"), false, S_("Kind(6)")},
    {S_("Kind(5)"), false, S_("Complex")},
    {S_("Val(unknown)"), false, S_("<nil>")},
    {S_("Val(1i)"), false, S_("<nil>")},
    {S_("Make(uint8)"), false, S_("Unknown")},
    {S_("Make(+Inf)"), false, S_("Unknown")},
    {S_("1e100 / 3"), false, S_("3.33333e+99")},
    {S_("Float32Val(1e100)"), false, S_("+Inf false")},
    {S_("Uint64Val(-1)"), false, S_("18446744073709551615 false")},
    {S_("Int64Val(1<<63)"), false, S_("-9223372036854775808 false")},
    {S_("ToInt(2.0000000000000000000001)"), false, S_("unknown")},
    {S_("ToInt(2.5)"), false, S_("Unknown")},
    {S_("ToComplex(1)"), false, S_("(1 + 0i)")},
    {S_("ToFloat(\"a\")"), false, S_("Unknown")},
    {S_("UnaryOp(^, 5, 8)"), false, S_("250")},
    {S_("-MinInt64"), false, S_("9223372036854775808")},
};

static Str kind_of(Alloc *a, ConstantValue x) {
    return constant_kind_string(constant_value_kind(x), a);
}

static Str edge(Alloc *a, Int i) {
    ConstantValue b = constant_make_bool(true);
    ConstantValue s = constant_make_string(a, BURROW_S("a"));
    ConstantValue n = constant_make_int64(1);
    ConstantValue f = constant_make_float64(a, 1.5);
    ConstantValue c = constant_make_imag(a, n);
    ConstantValue zero = constant_make_int64(0);
    ConstantValue min = constant_make_int64(INT64_MIN);
    ConstantValue neg = constant_make_int64(-1);
    ConstantValue big =
        constant_make_from_literal(a, BURROW_S("1e100"), TOKEN_FLOAT, 0);
    bool exact = false;
    switch (i) {
    case 0:
        return fmt_sprintf_v(a, "%v", constant_bool_val(n));
    case 1:
        return constant_string_val(b);
    case 2:
        return fmt_sprintf_v(a, "%v", constant_int64_val(f, NULL));
    case 3:
        return fmt_sprintf_v(a, "%v", constant_uint64_val(s, NULL));
    case 4:
        return fmt_sprintf_v(a, "%v", constant_float64_val(c, NULL));
    case 5:
        return fmt_sprintf_v(a, "%v", constant_bit_len(f));
    case 6:
        return fmt_sprintf_v(a, "%v", constant_sign(b));
    case 7:
        return fmt_sprintf_v(a, "%v", constant_bytes(a, f).len);
    case 8:
        return vstr(a, constant_num(a, c));
    case 9:
        return vstr(a, constant_real(s));
    case 10:
        return vstr(a, constant_make_imag(a, b));
    case 11:
        return vstr(a, constant_unary_op(a, TOKEN_NOT, n, 0));
    case 12:
        return vstr(a, constant_unary_op(a, TOKEN_SUB, b, 0));
    case 13:
        return vstr(a, constant_unary_op(a, TOKEN_XOR, f, 0));
    case 14:
        return vstr(a, constant_unary_op(a, TOKEN_MUL, n, 0));
    case 15:
        return vstr(a, constant_binary_op(a, b, TOKEN_ADD, b));
    case 16:
        return vstr(a, constant_binary_op(a, s, TOKEN_SUB, s));
    case 17:
        return vstr(a, constant_binary_op(a, f, TOKEN_REM, f));
    case 18:
        return vstr(a, constant_binary_op(a, n, TOKEN_LAND, n));
    case 19:
        return vstr(a, constant_binary_op(a, n, TOKEN_QUO, zero));
    case 20:
        return vstr(a, constant_binary_op(a, n, TOKEN_QUO_ASSIGN, zero));
    case 21:
        return vstr(a, constant_binary_op(a, f, TOKEN_QUO, zero));
    case 22:
        return vstr(a, constant_binary_op(a, min, TOKEN_QUO_ASSIGN, neg));
    case 23:
        return vstr(a, constant_binary_op(a, min, TOKEN_REM, neg));
    case 24:
        return vstr(a, constant_shift(a, s, TOKEN_SHL, 1));
    case 25:
        return vstr(a, constant_shift(a, f, TOKEN_SHL, 1));
    case 26:
        return vstr(a, constant_shift(a, n, TOKEN_ADD, 1));
    case 27:
        return vstr(a, constant_shift(a, constant_make_int64(-5), TOKEN_SHR, 70));
    case 28:
        return fmt_sprintf_v(a, "%v", constant_compare(b, TOKEN_LSS, b));
    case 29:
        return fmt_sprintf_v(a, "%v", constant_compare(n, TOKEN_ADD, n));
    case 30:
        return fmt_sprintf_v(a, "%v", constant_compare(c, TOKEN_LSS, c));
    case 31:
        return fmt_sprintf_v(a, "%v", constant_compare(b, TOKEN_EQL, s));
    case 32:
        return fmt_sprintf_v(a, "%v", constant_compare(s, TOKEN_EQL, b));
    case 33:
        return fmt_sprintf_v(a, "%v", constant_compare(b, TOKEN_EQL, n));
    case 34:
        return fmt_sprintf_v(a, "%v", constant_compare(n, TOKEN_EQL, b));
    case 35:
        return vstr(a, constant_binary_op(a, s, TOKEN_ADD, n));
    case 36:
        return vstr(a, constant_make_from_literal(a, BURROW_S("1"), TOKEN_IDENT, 0));
    case 37:
        return vstr(a, constant_make_from_literal(a, BURROW_S("1"), TOKEN_INT, 1));
    case 38:
        return constant_kind_string(-1, a);
    case 39:
        return constant_kind_string(6, a);
    case 40:
        return constant_kind_string(5, a);
    case 41:
        return constant_val(a, constant_make_unknown()).t == NULL ? BURROW_S("<nil>")
                                                                  : BURROW_S("non-nil");
    case 42:
        return constant_val(a, c).t == NULL ? BURROW_S("<nil>") : BURROW_S("non-nil");
    case 43:
        return kind_of(a, constant_make(a, BURROW_ANY_VAL(TYPE_BYTE, Byte, 1)));
    case 44: {
        BigFloat *inf = big_new_float(a, 0);
        big_float_set_inf(inf, false);
        return kind_of(a, constant_make(a, BURROW_ANY(TYPE_BIG_FLOAT, inf)));
    }
    case 45:
        return vstr(a, constant_binary_op(a, big, TOKEN_QUO, constant_make_int64(3)));
    case 46: {
        float v = constant_float32_val(big, &exact);
        return fmt_sprintf_v(a, "%v %v", v, exact);
    }
    case 47: {
        uint64_t v = constant_uint64_val(neg, &exact);
        return fmt_sprintf_v(a, "%v %v", v, exact);
    }
    case 48: {
        int64_t v =
            constant_int64_val(constant_make_uint64(a, UINT64_C(1) << 63), &exact);
        return fmt_sprintf_v(a, "%v %v", v, exact);
    }
    case 49:
        return constant_value_exact_string(
            constant_to_int(
                a, constant_make_from_literal(a, BURROW_S("2.0000000000000000000001"),
                                              TOKEN_FLOAT, 0)),
            a);
    case 50:
        return kind_of(a, constant_to_int(a, constant_make_float64(a, 2.5)));
    case 51:
        return vstr(a, constant_to_complex(a, n));
    case 52:
        return kind_of(a, constant_to_float(a, s));
    case 53:
        return vstr(a, constant_unary_op(a, TOKEN_XOR, constant_make_int64(5), 8));
    case 54:
        return vstr(a, constant_unary_op(a, TOKEN_SUB, min, 0));
    default:
        return BURROW_S("no such case");
    }
}

/* edge, with a panic caught and its message put in *out. */
static void run_edge(Alloc *a, Int i, Str *out, bool *panicked) {
    BURROW_TRY {
        *out = edge(a, i);
    }
    BURROW_CATCH(r) {
        *panicked = true;
        *out = str_clone(a, panic_text(r));
    }
    BURROW_TRY_END;
}

static void TestEdges(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NELEM(edge_tests); i++) {
        Str got = BURROW_STR_EMPTY;
        bool panicked = false;
        run_edge(a, i, &got, &panicked);
        if (panicked != edge_tests[i].panics || !str_eq(got, edge_tests[i].want))
            testing_t_errorf_v(t, "%s: got %q (panicked = %v); want %q (panicked = %v)",
                               edge_tests[i].name, got, panicked, edge_tests[i].want,
                               edge_tests[i].panics);
    }
    arena_free(&ar);
}

/* A string made by + holds its parts until something needs the bytes, and
 * a long chain of them is joined without recursing down it. */
static void TestStringChain(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    ConstantValue x = constant_make_string(a, BURROW_S(""));
    ConstantValue one = constant_make_string(a, BURROW_S("ab"));
    for (int i = 0; i < 100000; i++)
        x = constant_binary_op(a, x, TOKEN_ADD, one);
    x = constant_binary_op(a, constant_make_string(a, BURROW_S("<")), TOKEN_ADD, x);
    if (constant_string_len(x) != 200001)
        testing_t_errorf_v(t, "StringLen = %d; want 200001", constant_string_len(x));
    Str s = constant_string_val(x);
    Str rest = strings_repeat(a, BURROW_S("ab"), 100000);
    if (s.len != 200001 || s.p[0] != '<' || memcmp(s.p + 1, rest.p, 200000) != 0)
        testing_t_errorf_v(t, "StringVal: got %d bytes, not <abab...", s.len);
    /* the second time it is already joined */
    Str again = constant_string_val(x);
    if (again.p != s.p)
        testing_t_errorf_v(t, "StringVal joined the string twice");
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestNumbers)                                                                     \
    X(TestOps)                                                                         \
    X(TestString)                                                                      \
    X(TestStringLen)                                                                   \
    X(TestFractions)                                                                   \
    X(TestBytes)                                                                       \
    X(TestUnknown)                                                                     \
    X(TestMakeFloat64)                                                                 \
    X(TestMake)                                                                        \
    X(TestBitLen)                                                                      \
    X(TestEdges)                                                                       \
    X(TestStringChain)

TESTING_MAIN(TESTS)

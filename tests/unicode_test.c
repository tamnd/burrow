/* Derived from Go's src/unicode/letter_test.go, digit_test.go, graphic_test.go
 * and script_test.go.
 * Go source: go1.27.1.
 *
 * Go's tests all live in one package, and here they share one file so they
 * can share the tables. Two of them change shape. TestSpecialCaseNoMapping
 * goes through strings.ToLowerSpecial in Go, which is strings.Map over
 * SpecialCase.ToLower, and strings is not ported yet, so it maps the runes
 * itself. TestCalibrate runs only when Go is given -calibrate, and there are no
 * test flags here, so it runs when BURROW_UNICODE_CALIBRATE is set instead.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/unicode.h"

#include <stdio.h>
#include <stdlib.h>

#define LEN(x) ((Int)(sizeof(x) / sizeof((x)[0])))

typedef struct CaseT {
    Int cas;
    Rune in, out;
} CaseT;

typedef struct T {
    Rune rune;
    Str script;
} T;

static const Rune upperTest[] = {
    0x41,   0xc0,   0xd8,   0x100,   0x139,   0x14a,   0x178,  0x181,
    0x376,  0x3cf,  0x13bd, 0x1f2a,  0x2102,  0x2c00,  0x2c10, 0x2c20,
    0xa650, 0xa722, 0xff3a, 0x10400, 0x1d400, 0x1d7ca,
};

static const Rune notupperTest[] = {
    0x40, 0x5b, 0x61, 0x185, 0x1b0, 0x377, 0x387, 0x2150, 0xab7d, 0xffff, 0x10000,
};

static const Rune letterTest[] = {
    0x41,   0x61,   0xaa,    0xba,    0xc8,    0xdb,    0xf9,    0x2ec,
    0x535,  0x620,  0x6e6,   0x93d,   0xa15,   0xb99,   0xdc0,   0xedd,
    0x1000, 0x1200, 0x1312,  0x1401,  0x2c00,  0xa800,  0xf900,  0xfa30,
    0xffda, 0xffdc, 0x10000, 0x10300, 0x10400, 0x20000, 0x2f800, 0x2fa1d,
};

static const Rune notletterTest[] = {
    0x20, 0x35, 0x375, 0x619, 0x700, 0x1885, 0xfffe, 0x1ffff, 0x10ffff,
};

static const Rune spaceTest[] = {
    0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x20, 0x85, 0xA0, 0x2000, 0x3000,
};

static const Rune testDigit[] = {
    0x0030, 0x0039, 0x0661, 0x06F1, 0x07C9,  0x0966,  0x09EF, 0x0A66, 0x0AEF, 0x0B66,
    0x0B6F, 0x0BE6, 0x0BEF, 0x0C66, 0x0CEF,  0x0D66,  0x0D6F, 0x0E50, 0x0E59, 0x0ED0,
    0x0ED9, 0x0F20, 0x0F29, 0x1040, 0x1049,  0x1090,  0x1091, 0x1099, 0x17E0, 0x17E9,
    0x1810, 0x1819, 0x1946, 0x194F, 0x19D0,  0x19D9,  0x1B50, 0x1B59, 0x1BB0, 0x1BB9,
    0x1C40, 0x1C49, 0x1C50, 0x1C59, 0xA620,  0xA629,  0xA8D0, 0xA8D9, 0xA900, 0xA909,
    0xAA50, 0xAA59, 0xFF10, 0xFF19, 0x104A1, 0x1D7CE,
};

static const Rune testLetter[] = {
    0x0041, 0x0061, 0x00AA,  0x00BA,  0x00C8,  0x00DB,  0x00F9,  0x02EC,
    0x0535, 0x06E6, 0x093D,  0x0A15,  0x0B99,  0x0DC0,  0x0EDD,  0x1000,
    0x1200, 0x1312, 0x1401,  0x1885,  0x2C00,  0xA800,  0xF900,  0xFA30,
    0xFFDA, 0xFFDC, 0x10000, 0x10300, 0x10400, 0x20000, 0x2F800, 0x2FA1D,
};

static const CaseT caseTest[] = {
    /* errors */
    {-1, 0x000A, 0xFFFD},
    {UNICODE_UPPER_CASE, -1, -1},
    {UNICODE_UPPER_CASE, 1 << 30, 1 << 30},

    /* ASCII (special-cased so test carefully) */
    {UNICODE_UPPER_CASE, 0x000A, 0x000A},
    {UNICODE_UPPER_CASE, 'a', 'A'},
    {UNICODE_UPPER_CASE, 'A', 'A'},
    {UNICODE_UPPER_CASE, '7', '7'},
    {UNICODE_LOWER_CASE, 0x000A, 0x000A},
    {UNICODE_LOWER_CASE, 'a', 'a'},
    {UNICODE_LOWER_CASE, 'A', 'a'},
    {UNICODE_LOWER_CASE, '7', '7'},
    {UNICODE_TITLE_CASE, 0x000A, 0x000A},
    {UNICODE_TITLE_CASE, 'a', 'A'},
    {UNICODE_TITLE_CASE, 'A', 'A'},
    {UNICODE_TITLE_CASE, '7', '7'},

    /* Latin-1: easy to read the tests! */
    {UNICODE_UPPER_CASE, 0x80, 0x80},
    {UNICODE_UPPER_CASE, 0x00C5, 0x00C5},
    {UNICODE_UPPER_CASE, 0x00E5, 0x00C5},
    {UNICODE_LOWER_CASE, 0x80, 0x80},
    {UNICODE_LOWER_CASE, 0x00C5, 0x00E5},
    {UNICODE_LOWER_CASE, 0x00E5, 0x00E5},
    {UNICODE_TITLE_CASE, 0x80, 0x80},
    {UNICODE_TITLE_CASE, 0x00C5, 0x00C5},
    {UNICODE_TITLE_CASE, 0x00E5, 0x00C5},

    /* 0131;LATIN SMALL LETTER DOTLESS I;Ll;0;L;;;;;N;;;0049;;0049 */
    {UNICODE_UPPER_CASE, 0x0131, 'I'},
    {UNICODE_LOWER_CASE, 0x0131, 0x0131},
    {UNICODE_TITLE_CASE, 0x0131, 'I'},

    /* 0133;LATIN SMALL LIGATURE IJ;Ll;0;L;<compat> 0069 006A;;;;N;LATIN SMALL LETTER I J;;0132;;0132 */
    {UNICODE_UPPER_CASE, 0x0133, 0x0132},
    {UNICODE_LOWER_CASE, 0x0133, 0x0133},
    {UNICODE_TITLE_CASE, 0x0133, 0x0132},

    /* 212A;KELVIN SIGN;Lu;0;L;004B;;;;N;DEGREES KELVIN;;;006B; */
    {UNICODE_UPPER_CASE, 0x212A, 0x212A},
    {UNICODE_LOWER_CASE, 0x212A, 'k'},
    {UNICODE_TITLE_CASE, 0x212A, 0x212A},

    /* From an UpperLower sequence */
    /* A640;CYRILLIC CAPITAL LETTER ZEMLYA;Lu;0;L;;;;;N;;;;A641; */
    {UNICODE_UPPER_CASE, 0xA640, 0xA640},
    {UNICODE_LOWER_CASE, 0xA640, 0xA641},
    {UNICODE_TITLE_CASE, 0xA640, 0xA640},
    /* A641;CYRILLIC SMALL LETTER ZEMLYA;Ll;0;L;;;;;N;;;A640;;A640 */
    {UNICODE_UPPER_CASE, 0xA641, 0xA640},
    {UNICODE_LOWER_CASE, 0xA641, 0xA641},
    {UNICODE_TITLE_CASE, 0xA641, 0xA640},
    /* A64E;CYRILLIC CAPITAL LETTER NEUTRAL YER;Lu;0;L;;;;;N;;;;A64F; */
    {UNICODE_UPPER_CASE, 0xA64E, 0xA64E},
    {UNICODE_LOWER_CASE, 0xA64E, 0xA64F},
    {UNICODE_TITLE_CASE, 0xA64E, 0xA64E},
    /* A65F;CYRILLIC SMALL LETTER YN;Ll;0;L;;;;;N;;;A65E;;A65E */
    {UNICODE_UPPER_CASE, 0xA65F, 0xA65E},
    {UNICODE_LOWER_CASE, 0xA65F, 0xA65F},
    {UNICODE_TITLE_CASE, 0xA65F, 0xA65E},

    /* From another UpperLower sequence */
    /* 0139;LATIN CAPITAL LETTER L WITH ACUTE;Lu;0;L;004C 0301;;;;N;LATIN CAPITAL LETTER L ACUTE;;;013A; */
    {UNICODE_UPPER_CASE, 0x0139, 0x0139},
    {UNICODE_LOWER_CASE, 0x0139, 0x013A},
    {UNICODE_TITLE_CASE, 0x0139, 0x0139},
    /* 013F;LATIN CAPITAL LETTER L WITH MIDDLE DOT;Lu;0;L;<compat> 004C 00B7;;;;N;;;;0140; */
    {UNICODE_UPPER_CASE, 0x013f, 0x013f},
    {UNICODE_LOWER_CASE, 0x013f, 0x0140},
    {UNICODE_TITLE_CASE, 0x013f, 0x013f},
    /* 0148;LATIN SMALL LETTER N WITH CARON;Ll;0;L;006E 030C;;;;N;LATIN SMALL LETTER N HACEK;;0147;;0147 */
    {UNICODE_UPPER_CASE, 0x0148, 0x0147},
    {UNICODE_LOWER_CASE, 0x0148, 0x0148},
    {UNICODE_TITLE_CASE, 0x0148, 0x0147},

    /* Lowercase lower than uppercase. */
    /* AB78;CHEROKEE SMALL LETTER GE;Ll;0;L;;;;;N;;;13A8;;13A8 */
    {UNICODE_UPPER_CASE, 0xab78, 0x13a8},
    {UNICODE_LOWER_CASE, 0xab78, 0xab78},
    {UNICODE_TITLE_CASE, 0xab78, 0x13a8},
    {UNICODE_UPPER_CASE, 0x13a8, 0x13a8},
    {UNICODE_LOWER_CASE, 0x13a8, 0xab78},
    {UNICODE_TITLE_CASE, 0x13a8, 0x13a8},

    /* Last block in the 5.1.0 table */
    /* 10400;DESERET CAPITAL LETTER LONG I;Lu;0;L;;;;;N;;;;10428; */
    {UNICODE_UPPER_CASE, 0x10400, 0x10400},
    {UNICODE_LOWER_CASE, 0x10400, 0x10428},
    {UNICODE_TITLE_CASE, 0x10400, 0x10400},
    /* 10427;DESERET CAPITAL LETTER EW;Lu;0;L;;;;;N;;;;1044F; */
    {UNICODE_UPPER_CASE, 0x10427, 0x10427},
    {UNICODE_LOWER_CASE, 0x10427, 0x1044F},
    {UNICODE_TITLE_CASE, 0x10427, 0x10427},
    /* 10428;DESERET SMALL LETTER LONG I;Ll;0;L;;;;;N;;;10400;;10400 */
    {UNICODE_UPPER_CASE, 0x10428, 0x10400},
    {UNICODE_LOWER_CASE, 0x10428, 0x10428},
    {UNICODE_TITLE_CASE, 0x10428, 0x10400},
    /* 1044F;DESERET SMALL LETTER EW;Ll;0;L;;;;;N;;;10427;;10427 */
    {UNICODE_UPPER_CASE, 0x1044F, 0x10427},
    {UNICODE_LOWER_CASE, 0x1044F, 0x1044F},
    {UNICODE_TITLE_CASE, 0x1044F, 0x10427},

    /* First one not in the 5.1.0 table */
    /* 10450;SHAVIAN LETTER PEEP;Lo;0;L;;;;;N;;;;; */
    {UNICODE_UPPER_CASE, 0x10450, 0x10450},
    {UNICODE_LOWER_CASE, 0x10450, 0x10450},
    {UNICODE_TITLE_CASE, 0x10450, 0x10450},

    /* Non-letters with case. */
    {UNICODE_LOWER_CASE, 0x2161, 0x2171},
    {UNICODE_UPPER_CASE, 0x0345, 0x0399},
};

static const T inCategoryTest[] = {
    {0x0081, BURROW_S_INIT("Cc")},
    {0x200B, BURROW_S_INIT("Cf")},
    {0xf0000, BURROW_S_INIT("Co")},
    {0xdb80, BURROW_S_INIT("Cs")},
    {0x0236, BURROW_S_INIT("Ll")},
    {0x1d9d, BURROW_S_INIT("Lm")},
    {0x07cf, BURROW_S_INIT("Lo")},
    {0x1f8a, BURROW_S_INIT("Lt")},
    {0x03ff, BURROW_S_INIT("Lu")},
    {0x0bc1, BURROW_S_INIT("Mc")},
    {0x20df, BURROW_S_INIT("Me")},
    {0x07f0, BURROW_S_INIT("Mn")},
    {0x1bb2, BURROW_S_INIT("Nd")},
    {0x10147, BURROW_S_INIT("Nl")},
    {0x2478, BURROW_S_INIT("No")},
    {0xfe33, BURROW_S_INIT("Pc")},
    {0x2011, BURROW_S_INIT("Pd")},
    {0x301e, BURROW_S_INIT("Pe")},
    {0x2e03, BURROW_S_INIT("Pf")},
    {0x2e02, BURROW_S_INIT("Pi")},
    {0x0022, BURROW_S_INIT("Po")},
    {0x2770, BURROW_S_INIT("Ps")},
    {0x00a4, BURROW_S_INIT("Sc")},
    {0xa711, BURROW_S_INIT("Sk")},
    {0x25f9, BURROW_S_INIT("Sm")},
    {0x2108, BURROW_S_INIT("So")},
    {0x2028, BURROW_S_INIT("Zl")},
    {0x2029, BURROW_S_INIT("Zp")},
    {0x202f, BURROW_S_INIT("Zs")},
    /* Unifieds. */
    {0x04aa, BURROW_S_INIT("L")},
    {0x0009, BURROW_S_INIT("C")},
    {0x1712, BURROW_S_INIT("M")},
    {0x0031, BURROW_S_INIT("N")},
    {0x00bb, BURROW_S_INIT("P")},
    {0x00a2, BURROW_S_INIT("S")},
    {0x00a0, BURROW_S_INIT("Z")},
    {0x0065, BURROW_S_INIT("LC")},
    /* Unassigned */
    {0x0378, BURROW_S_INIT("Cn")},
    {0x0378, BURROW_S_INIT("C")},
};

static const T inPropTest[] = {
    {0x0046, BURROW_S_INIT("ASCII_Hex_Digit")},
    {0x200F, BURROW_S_INIT("Bidi_Control")},
    {0x2212, BURROW_S_INIT("Dash")},
    {0xE0001, BURROW_S_INIT("Deprecated")},
    {0x00B7, BURROW_S_INIT("Diacritic")},
    {0x30FE, BURROW_S_INIT("Extender")},
    {0xFF46, BURROW_S_INIT("Hex_Digit")},
    {0x2E17, BURROW_S_INIT("Hyphen")},
    {0x2FFB, BURROW_S_INIT("IDS_Binary_Operator")},
    {0x2FF3, BURROW_S_INIT("IDS_Trinary_Operator")},
    {0xFA6A, BURROW_S_INIT("Ideographic")},
    {0x200D, BURROW_S_INIT("Join_Control")},
    {0x0EC4, BURROW_S_INIT("Logical_Order_Exception")},
    {0x2FFFF, BURROW_S_INIT("Noncharacter_Code_Point")},
    {0x065E, BURROW_S_INIT("Other_Alphabetic")},
    {0x2065, BURROW_S_INIT("Other_Default_Ignorable_Code_Point")},
    {0x0BD7, BURROW_S_INIT("Other_Grapheme_Extend")},
    {0x0387, BURROW_S_INIT("Other_ID_Continue")},
    {0x212E, BURROW_S_INIT("Other_ID_Start")},
    {0x2094, BURROW_S_INIT("Other_Lowercase")},
    {0x2040, BURROW_S_INIT("Other_Math")},
    {0x216F, BURROW_S_INIT("Other_Uppercase")},
    {0x0027, BURROW_S_INIT("Pattern_Syntax")},
    {0x0020, BURROW_S_INIT("Pattern_White_Space")},
    {0x06DD, BURROW_S_INIT("Prepended_Concatenation_Mark")},
    {0x300D, BURROW_S_INIT("Quotation_Mark")},
    {0x2EF3, BURROW_S_INIT("Radical")},
    {0x1f1ff, BURROW_S_INIT("Regional_Indicator")},
    {0x061F, BURROW_S_INIT("STerm")}, /* Deprecated alias of Sentence_Terminal */
    {0x061F, BURROW_S_INIT("Sentence_Terminal")},
    {0x2071, BURROW_S_INIT("Soft_Dotted")},
    {0x003A, BURROW_S_INIT("Terminal_Punctuation")},
    {0x9FC3, BURROW_S_INIT("Unified_Ideograph")},
    {0xFE0F, BURROW_S_INIT("Variation_Selector")},
    {0x0020, BURROW_S_INIT("White_Space")},
    {0x221e, BURROW_S_INIT("ID_Compat_Math_Start")},
    {0x06e3, BURROW_S_INIT("Modifier_Combining_Mark")},
    {0x2080, BURROW_S_INIT("ID_Compat_Math_Continue")},
    {0x2ffe, BURROW_S_INIT("IDS_Unary_Operator")},
};

static void TestIsLetter(TestingT *t) {
    for (Int i = 0; i < LEN(upperTest); i++) {
        if (!unicode_is_letter(upperTest[i]))
            testing_t_errorf_v(t, "IsLetter(U+%04X) = false, want true", upperTest[i]);
    }
    for (Int i = 0; i < LEN(letterTest); i++) {
        if (!unicode_is_letter(letterTest[i]))
            testing_t_errorf_v(t, "IsLetter(U+%04X) = false, want true", letterTest[i]);
    }
    for (Int i = 0; i < LEN(notletterTest); i++) {
        if (unicode_is_letter(notletterTest[i]))
            testing_t_errorf_v(t, "IsLetter(U+%04X) = true, want false",
                               notletterTest[i]);
    }
}

static void TestIsUpper(TestingT *t) {
    for (Int i = 0; i < LEN(upperTest); i++) {
        if (!unicode_is_upper(upperTest[i]))
            testing_t_errorf_v(t, "IsUpper(U+%04X) = false, want true", upperTest[i]);
    }
    for (Int i = 0; i < LEN(notupperTest); i++) {
        if (unicode_is_upper(notupperTest[i]))
            testing_t_errorf_v(t, "IsUpper(U+%04X) = true, want false",
                               notupperTest[i]);
    }
    for (Int i = 0; i < LEN(notletterTest); i++) {
        if (unicode_is_upper(notletterTest[i]))
            testing_t_errorf_v(t, "IsUpper(U+%04X) = true, want false",
                               notletterTest[i]);
    }
}

static Str caseString(Int c) {
    switch (c) {
    case UNICODE_UPPER_CASE:
        return BURROW_S("UpperCase");
    case UNICODE_LOWER_CASE:
        return BURROW_S("LowerCase");
    case UNICODE_TITLE_CASE:
        return BURROW_S("TitleCase");
    default:
        break;
    }
    return BURROW_S("ErrorCase");
}

static void TestTo(TestingT *t) {
    for (Int i = 0; i < LEN(caseTest); i++) {
        CaseT c = caseTest[i];
        Rune r = unicode_to(c.cas, c.in);
        if (c.out != r)
            testing_t_errorf_v(t, "To(U+%04X, %s) = U+%04X want U+%04X", c.in,
                               caseString(c.cas), r, c.out);
    }
}

static void TestToUpperCase(TestingT *t) {
    for (Int i = 0; i < LEN(caseTest); i++) {
        CaseT c = caseTest[i];
        if (c.cas != UNICODE_UPPER_CASE)
            continue;
        Rune r = unicode_to_upper(c.in);
        if (c.out != r)
            testing_t_errorf_v(t, "ToUpper(U+%04X) = U+%04X want U+%04X", c.in, r,
                               c.out);
    }
}

static void TestToLowerCase(TestingT *t) {
    for (Int i = 0; i < LEN(caseTest); i++) {
        CaseT c = caseTest[i];
        if (c.cas != UNICODE_LOWER_CASE)
            continue;
        Rune r = unicode_to_lower(c.in);
        if (c.out != r)
            testing_t_errorf_v(t, "ToLower(U+%04X) = U+%04X want U+%04X", c.in, r,
                               c.out);
    }
}

static void TestToTitleCase(TestingT *t) {
    for (Int i = 0; i < LEN(caseTest); i++) {
        CaseT c = caseTest[i];
        if (c.cas != UNICODE_TITLE_CASE)
            continue;
        Rune r = unicode_to_title(c.in);
        if (c.out != r)
            testing_t_errorf_v(t, "ToTitle(U+%04X) = U+%04X want U+%04X", c.in, r,
                               c.out);
    }
}

static void TestIsSpace(TestingT *t) {
    for (Int i = 0; i < LEN(spaceTest); i++) {
        if (!unicode_is_space(spaceTest[i]))
            testing_t_errorf_v(t, "IsSpace(U+%04X) = false; want true", spaceTest[i]);
    }
    for (Int i = 0; i < LEN(letterTest); i++) {
        if (unicode_is_space(letterTest[i]))
            testing_t_errorf_v(t, "IsSpace(U+%04X) = true; want false", letterTest[i]);
    }
}

/* Check that the optimizations for IsLetter etc. agree with the tables. We
 * only need to check the Latin-1 range. */
static void TestLetterOptimizations(TestingT *t) {
    for (Rune i = 0; i <= UNICODE_MAX_LATIN1; i++) {
        if (unicode_is(unicode_letter, i) != unicode_is_letter(i))
            testing_t_errorf_v(t, "IsLetter(U+%04X) disagrees with Is(Letter)", i);
        if (unicode_is(unicode_upper, i) != unicode_is_upper(i))
            testing_t_errorf_v(t, "IsUpper(U+%04X) disagrees with Is(Upper)", i);
        if (unicode_is(unicode_lower, i) != unicode_is_lower(i))
            testing_t_errorf_v(t, "IsLower(U+%04X) disagrees with Is(Lower)", i);
        if (unicode_is(unicode_title, i) != unicode_is_title(i))
            testing_t_errorf_v(t, "IsTitle(U+%04X) disagrees with Is(Title)", i);
        if (unicode_is(unicode_white_space, i) != unicode_is_space(i))
            testing_t_errorf_v(t, "IsSpace(U+%04X) disagrees with Is(White_Space)", i);
        if (unicode_to(UNICODE_UPPER_CASE, i) != unicode_to_upper(i))
            testing_t_errorf_v(t, "ToUpper(U+%04X) disagrees with To(Upper)", i);
        if (unicode_to(UNICODE_LOWER_CASE, i) != unicode_to_lower(i))
            testing_t_errorf_v(t, "ToLower(U+%04X) disagrees with To(Lower)", i);
        if (unicode_to(UNICODE_TITLE_CASE, i) != unicode_to_title(i))
            testing_t_errorf_v(t, "ToTitle(U+%04X) disagrees with To(Title)", i);
    }
}

static void TestTurkishCase(TestingT *t) {
    /* abcçdefgğhıijklmnoöprsştuüvyz and ABCÇDEFGĞHIİJKLMNOÖPRSŞTUÜVYZ. */
    static const Rune lower[] = {'a',    'b',    'c',    0x00E7, 'd', 'e', 'f',    'g',
                                 0x011F, 'h',    0x0131, 'i',    'j', 'k', 'l',    'm',
                                 'n',    'o',    0x00F6, 'p',    'r', 's', 0x015F, 't',
                                 'u',    0x00FC, 'v',    'y',    'z'};
    static const Rune upper[] = {'A',    'B',    'C',    0x00C7, 'D', 'E', 'F',    'G',
                                 0x011E, 'H',    'I',    0x0130, 'J', 'K', 'L',    'M',
                                 'N',    'O',    0x00D6, 'P',    'R', 'S', 0x015E, 'T',
                                 'U',    0x00DC, 'V',    'Y',    'Z'};
    UnicodeSpecialCase tc = unicode_turkish_case;
    for (Int i = 0; i < LEN(lower); i++) {
        Rune l = lower[i], u = upper[i];
        if (unicode_special_case_to_lower(tc, l) != l)
            testing_t_errorf_v(t, "lower(U+%04X) is U+%04X not U+%04X", l,
                               unicode_special_case_to_lower(tc, l), l);
        if (unicode_special_case_to_upper(tc, u) != u)
            testing_t_errorf_v(t, "upper(U+%04X) is U+%04X not U+%04X", u,
                               unicode_special_case_to_upper(tc, u), u);
        if (unicode_special_case_to_upper(tc, l) != u)
            testing_t_errorf_v(t, "upper(U+%04X) is U+%04X not U+%04X", l,
                               unicode_special_case_to_upper(tc, l), u);
        if (unicode_special_case_to_lower(tc, u) != l)
            testing_t_errorf_v(t, "lower(U+%04X) is U+%04X not U+%04X", u,
                               unicode_special_case_to_lower(tc, l), l);
        if (unicode_special_case_to_title(tc, u) != u)
            testing_t_errorf_v(t, "title(U+%04X) is U+%04X not U+%04X", u,
                               unicode_special_case_to_title(tc, u), u);
        if (unicode_special_case_to_title(tc, l) != u)
            testing_t_errorf_v(t, "title(U+%04X) is U+%04X not U+%04X", l,
                               unicode_special_case_to_title(tc, l), u);
    }
}

/* SimpleFold(x) returns the next equivalent rune > x or wraps around to
 * smaller values. Each cycle ends with 0. */
static const Rune simpleFoldTests[][5] = {
    /* Easy cases. */
    {'A', 'a'},
    {0x03B4, 0x0394}, /* δΔ */

    /* ASCII special cases. */
    {'K', 'k', 0x212A}, /* the last is the Kelvin sign */
    {'S', 's', 0x017F}, /* the last is the long s */

    /* Non-ASCII special cases. */
    {0x03C1, 0x03F1, 0x03A1},
    {0x0345, 0x0399, 0x03B9, 0x1FBE},

    /* Extra special cases: has lower/upper but no case fold. */
    {0x0130},
    {0x0131},

    /* Upper comes before lower (Cherokee). */
    {0x13B0, 0xAB80},
};

static void TestSimpleFold(TestingT *t) {
    for (Int i = 0; i < LEN(simpleFoldTests); i++) {
        const Rune *cycle = simpleFoldTests[i];
        Int n = 0;
        while (n < 5 && cycle[n] != 0)
            n++;
        Rune r = cycle[n - 1];
        for (Int j = 0; j < n; j++) {
            Rune out = cycle[j];
            Rune got = unicode_simple_fold(r);
            if (got != out)
                testing_t_errorf_v(t, "SimpleFold(%#U) = %#U, want %#U", r, got, out);
            r = out;
        }
    }

    Rune r = unicode_simple_fold(-42);
    if (r != -42)
        testing_t_errorf_v(t, "SimpleFold(-42) = %v, want -42", r);
}

/* The calibration Go runs with -calibrate: find where binary search of a range
 * list starts to beat a linear scan, which is where linear_max in letter.c
 * came from. */
typedef struct FakeTable {
    UnicodeRange16 r16[64];
    Int n;
} FakeTable;

static void fakeTable(FakeTable *tab, Int n) {
    tab->n = n;
    for (Int i = 0; i < n; i++)
        tab->r16[i] =
            (UnicodeRange16){(uint16_t)(i * 5 + 10), (uint16_t)(i * 5 + 12), 1};
}

static bool linear(const FakeTable *tab, uint16_t r) {
    for (Int i = 0; i < tab->n; i++) {
        const UnicodeRange16 *range_ = &tab->r16[i];
        if (r < range_->lo)
            return false;
        if (r <= range_->hi)
            return (uint16_t)(r - range_->lo) % range_->stride == 0;
    }
    return false;
}

static bool binary(const FakeTable *tab, uint16_t r) {
    /* binary search over ranges */
    Int lo = 0, hi = tab->n;
    while (lo < hi) {
        Int m = (Int)((Uint)(lo + hi) >> 1);
        const UnicodeRange16 *range_ = &tab->r16[m];
        if (range_->lo <= r && r <= range_->hi)
            return (uint16_t)(r - range_->lo) % range_->stride == 0;
        if (r < range_->lo)
            hi = m;
        else
            lo = m + 1;
    }
    return false;
}

static volatile bool calibrateSink;

static void blinear(void *env, TestingB *b) {
    const FakeTable *tab = env;
    Int max = tab->n * 5 + 20;
    for (Int i = 0; i < testing_b_n(b); i++) {
        for (Int j = 0; j <= max; j++)
            calibrateSink = linear(tab, (uint16_t)j);
    }
}

static void bbinary(void *env, TestingB *b) {
    const FakeTable *tab = env;
    Int max = tab->n * 5 + 20;
    for (Int i = 0; i < testing_b_n(b); i++) {
        for (Int j = 0; j <= max; j++)
            calibrateSink = binary(tab, (uint16_t)j);
    }
}

/* Whether binary search wins by more than 10% on a table of n ranges. The 10%
 * bias gives linear search an edge when they're close, because on
 * predominantly ASCII inputs linear search is even better than our benchmarks
 * measure. */
static bool binaryWins(Int n) {
    FakeTable tab;
    fakeTable(&tab, n);
    TestingBenchmarkResult bmlinear =
        testing_benchmark(BURROW_FN(TestingBFunc, blinear, &tab));
    TestingBenchmarkResult bmbinary =
        testing_benchmark(BURROW_FN(TestingBFunc, bbinary, &tab));
    int64_t l = testing_benchmark_result_ns_per_op(bmlinear);
    int64_t bn = testing_benchmark_result_ns_per_op(bmbinary);
    printf("n=%lld: linear=%lld binary=%lld\n", (long long)n, (long long)l,
           (long long)bn);
    testing_benchmark_result_free(&bmlinear);
    testing_benchmark_result_free(&bmbinary);
    return l * 100 > bn * 110;
}

static void TestCalibrate(TestingT *t) {
    (void)t;
    if (getenv("BURROW_UNICODE_CALIBRATE") == NULL)
        return;

    /* sort.Search(64, binaryWins) */
    Int lo = 0, hi = 64;
    while (lo < hi) {
        Int m = (Int)((Uint)(lo + hi) >> 1);
        if (!binaryWins(m))
            lo = m + 1;
        else
            hi = m;
    }
    printf("calibration: linear cutoff = %lld\n", (long long)lo);
}

static void checkLatinOffset(TestingT *t, UnicodeTableMap m) {
    for (Int k = 0; k < m.len; k++) {
        const UnicodeRangeTable *tab = m.p[k].table;
        Int i = 0;
        while (i < tab->r16_len && tab->r16[i].hi <= UNICODE_MAX_LATIN1)
            i++;
        if (tab->latin_offset != i)
            testing_t_errorf_v(t, "%s: LatinOffset=%d, want %d", m.p[k].name,
                               tab->latin_offset, i);
    }
}

static void TestLatinOffset(TestingT *t) {
    checkLatinOffset(t, unicode_categories);
    checkLatinOffset(t, unicode_fold_category);
    checkLatinOffset(t, unicode_fold_script);
    checkLatinOffset(t, unicode_properties);
    checkLatinOffset(t, unicode_scripts);
}

static void TestSpecialCaseNoMapping(TestingT *t) {
    /* Issue 25636
     * no change for rune 'A', zero delta, under upper/lower/title case change. */
    static const UnicodeCaseRange noChangeForCapitalA[] = {{'A', 'A', {0, 0, 0}}};
    UnicodeSpecialCase special = {noChangeForCapitalA, 1};
    char got[4] = {0};
    const char *in = "ABC";
    for (int i = 0; i < 3; i++)
        got[i] = (char)unicode_special_case_to_lower(special, (Rune)in[i]);
    Str want = BURROW_S("Abc");
    Str g = {(const Byte *)got, 3};
    if (!str_eq(g, want))
        testing_t_errorf_v(t, "got %q; want %q", g, want);
}

static void TestNegativeRune(TestingT *t) {
    /* Issue 43254
     * These tests cover negative rune handling by testing values which, when
     * cast to uint8 or uint16, look like a particular valid rune. This package
     * has Latin-1-specific optimizations, so we test all of Latin-1 and
     * representative non-Latin-1 values in the character categories covered by
     * IsGraphic, etc. */
    static const uint32_t nonLatin1[] = {
        /* Lu: LATIN CAPITAL LETTER A WITH MACRON */
        0x0100,
        /* Ll: LATIN SMALL LETTER A WITH MACRON */
        0x0101,
        /* Lt: LATIN CAPITAL LETTER D WITH SMALL LETTER Z WITH CARON */
        0x01C5,
        /* M: COMBINING GRAVE ACCENT */
        0x0300,
        /* Nd: ARABIC-INDIC DIGIT ZERO */
        0x0660,
        /* P: GREEK QUESTION MARK */
        0x037E,
        /* S: MODIFIER LETTER LEFT ARROWHEAD */
        0x02C2,
        /* Z: OGHAM SPACE MARK */
        0x1680,
    };
    for (Int i = 0; i < UNICODE_MAX_LATIN1 + LEN(nonLatin1); i++) {
        uint32_t base = (uint32_t)i;
        if (i >= UNICODE_MAX_LATIN1)
            base = nonLatin1[i - UNICODE_MAX_LATIN1];

        /* Note r is negative, but uint8(r) == uint8(base) and
         * uint16(r) == uint16(base). */
        Rune r = (Rune)(base - (UINT32_C(1) << 31));
        if (unicode_is(unicode_letter, r))
            testing_t_errorf_v(t, "Is(Letter, 0x%x - 1<<31) = true, want false", base);
        if (unicode_is_control(r))
            testing_t_errorf_v(t, "IsControl(0x%x - 1<<31) = true, want false", base);
        if (unicode_is_digit(r))
            testing_t_errorf_v(t, "IsDigit(0x%x - 1<<31) = true, want false", base);
        if (unicode_is_graphic(r))
            testing_t_errorf_v(t, "IsGraphic(0x%x - 1<<31) = true, want false", base);
        if (unicode_is_letter(r))
            testing_t_errorf_v(t, "IsLetter(0x%x - 1<<31) = true, want false", base);
        if (unicode_is_lower(r))
            testing_t_errorf_v(t, "IsLower(0x%x - 1<<31) = true, want false", base);
        if (unicode_is_mark(r))
            testing_t_errorf_v(t, "IsMark(0x%x - 1<<31) = true, want false", base);
        if (unicode_is_number(r))
            testing_t_errorf_v(t, "IsNumber(0x%x - 1<<31) = true, want false", base);
        if (unicode_is_print(r))
            testing_t_errorf_v(t, "IsPrint(0x%x - 1<<31) = true, want false", base);
        if (unicode_is_punct(r))
            testing_t_errorf_v(t, "IsPunct(0x%x - 1<<31) = true, want false", base);
        if (unicode_is_space(r))
            testing_t_errorf_v(t, "IsSpace(0x%x - 1<<31) = true, want false", base);
        if (unicode_is_symbol(r))
            testing_t_errorf_v(t, "IsSymbol(0x%x - 1<<31) = true, want false", base);
        if (unicode_is_title(r))
            testing_t_errorf_v(t, "IsTitle(0x%x - 1<<31) = true, want false", base);
        if (unicode_is_upper(r))
            testing_t_errorf_v(t, "IsUpper(0x%x - 1<<31) = true, want false", base);
    }
}

/* digit_test.go */

static void TestDigit(TestingT *t) {
    for (Int i = 0; i < LEN(testDigit); i++) {
        if (!unicode_is_digit(testDigit[i]))
            testing_t_errorf_v(t, "IsDigit(U+%04X) = false, want true", testDigit[i]);
    }
    for (Int i = 0; i < LEN(testLetter); i++) {
        if (unicode_is_digit(testLetter[i]))
            testing_t_errorf_v(t, "IsDigit(U+%04X) = true, want false", testLetter[i]);
    }
}

/* Test that the special case in IsDigit agrees with the table */
static void TestDigitOptimization(TestingT *t) {
    for (Rune i = 0; i <= UNICODE_MAX_LATIN1; i++) {
        if (unicode_is(unicode_digit, i) != unicode_is_digit(i))
            testing_t_errorf_v(t, "IsDigit(U+%04X) disagrees with Is(Digit)", i);
    }
}

/* graphic_test.go: independently check that the special "Is" functions work
 * in the Latin-1 range through the property table. */

static void TestIsControlLatin1(TestingT *t) {
    for (Rune i = 0; i <= UNICODE_MAX_LATIN1; i++) {
        bool got = unicode_is_control(i);
        bool want = false;
        if (0x00 <= i && i <= 0x1F)
            want = true;
        else if (0x7F <= i && i <= 0x9F)
            want = true;
        if (got != want)
            testing_t_errorf_v(t, "%U incorrect: got %t; want %t", i, got, want);
    }
}

#define LATIN1_TEST(name, got_expr, want_expr)                                         \
    static void name(TestingT *t) {                                                    \
        for (Rune i = 0; i <= UNICODE_MAX_LATIN1; i++) {                               \
            bool got = got_expr;                                                       \
            bool want = want_expr;                                                     \
            if (got != want)                                                           \
                testing_t_errorf_v(t, "%U incorrect: got %t; want %t", i, got, want);  \
        }                                                                              \
    }

LATIN1_TEST(TestIsLetterLatin1, unicode_is_letter(i), unicode_is(unicode_letter, i))
LATIN1_TEST(TestIsUpperLatin1, unicode_is_upper(i), unicode_is(unicode_upper, i))
LATIN1_TEST(TestIsLowerLatin1, unicode_is_lower(i), unicode_is(unicode_lower, i))
LATIN1_TEST(TestNumberLatin1, unicode_is_number(i), unicode_is(unicode_number, i))
LATIN1_TEST(TestIsPrintLatin1, unicode_is_print(i),
            i == ' ' || unicode_in(i, unicode_print_ranges))
LATIN1_TEST(TestIsGraphicLatin1, unicode_is_graphic(i),
            unicode_in(i, unicode_graphic_ranges))
LATIN1_TEST(TestIsPunctLatin1, unicode_is_punct(i), unicode_is(unicode_punct, i))
LATIN1_TEST(TestIsSpaceLatin1, unicode_is_space(i), unicode_is(unicode_white_space, i))
LATIN1_TEST(TestIsSymbolLatin1, unicode_is_symbol(i), unicode_is(unicode_symbol, i))

/* script_test.go */

static void checkTableMap(TestingT *t, UnicodeTableMap m, const T *tests, Int n,
                          const char *what) {
    bool notTested[512] = {false};
    CHECK(m.len <= LEN(notTested));
    for (Int i = 0; i < m.len; i++)
        notTested[i] = true;
    for (Int i = 0; i < n; i++) {
        T test = tests[i];
        const UnicodeRangeTable *tab = unicode_table_map_get(m, test.script);
        if (tab == NULL)
            testing_t_fatalf_v(t, "%s not a known %s", test.script, what);
        if (!unicode_is(tab, test.rune))
            testing_t_errorf_v(t, "IsCategory(%U, %s) = false, want true", test.rune,
                               test.script);
        for (Int k = 0; k < m.len; k++) {
            if (m.p[k].table == tab && str_eq(m.p[k].name, test.script))
                notTested[k] = false;
        }
    }
    for (Int k = 0; k < m.len; k++) {
        if (notTested[k])
            testing_t_errorf_v(t, "%s not tested: %s", what, m.p[k].name);
    }
}

static void TestCategories(TestingT *t) {
    checkTableMap(t, unicode_categories, inCategoryTest, LEN(inCategoryTest),
                  "category");
}

static void TestProperties(TestingT *t) {
    checkTableMap(t, unicode_properties, inPropTest, LEN(inPropTest), "property");
}

/* Not from Go: the lookups that stand in for its maps, and In and IsOneOf,
 * which Go only tests through IsGraphic and IsPrint. */
static void TestMaps(TestingT *t) {
    if (unicode_table_map_get(unicode_scripts, BURROW_S("Greek")) != unicode_greek)
        testing_t_error_v(t, "Scripts[\"Greek\"] is not Greek");
    if (unicode_table_map_get(unicode_scripts, BURROW_S("Klingon")) != NULL)
        testing_t_error_v(t, "Scripts[\"Klingon\"] is not nil");
    if (unicode_table_map_get(unicode_categories, BURROW_S("")) != NULL)
        testing_t_error_v(t, "Categories[\"\"] is not nil");
    for (Int k = 1; k < unicode_scripts.len; k++) {
        if (str_cmp(unicode_scripts.p[k - 1].name, unicode_scripts.p[k].name) >= 0)
            testing_t_errorf_v(t, "Scripts out of order at %s",
                               unicode_scripts.p[k].name);
    }
    Str target = BURROW_S("");
    if (!unicode_alias_map_get(unicode_category_aliases, BURROW_S("Letter"), &target) ||
        !str_eq(target, BURROW_S("L")))
        testing_t_errorf_v(t, "CategoryAliases[\"Letter\"] = %q, want \"L\"", target);
    if (unicode_alias_map_get(unicode_category_aliases, BURROW_S("L"), NULL))
        testing_t_error_v(t, "CategoryAliases[\"L\"] exists");
    for (Int k = 0; k < unicode_category_aliases.len; k++) {
        Str to = unicode_category_aliases.p[k].target;
        if (unicode_table_map_get(unicode_categories, to) == NULL)
            testing_t_errorf_v(t, "alias %s names %s, which is not a category",
                               unicode_category_aliases.p[k].name, to);
    }
    if (!unicode_in_v(0x03B1, unicode_latin, unicode_greek))
        testing_t_error_v(t, "In(alpha, Latin, Greek) = false");
    if (unicode_in_v(0x0416, unicode_latin, unicode_greek))
        testing_t_error_v(t, "In(Zhe, Latin, Greek) = true");
    if (!unicode_is_one_of(UNICODE_RANGES(unicode_cyrillic), 0x0416))
        testing_t_error_v(t, "IsOneOf(Cyrillic, Zhe) = false");
    if (unicode_in(0x0416, slice_nil(TYPE_UNSAFE_POINTER)))
        testing_t_error_v(t, "In(Zhe) with no tables = true");
    if (unicode_to(UNICODE_MAX_CASE, 'a') != UNICODE_REPLACEMENT_CHAR)
        testing_t_error_v(t, "To(MaxCase, 'a') is not the replacement character");
    if (unicode_special_case_to_upper(unicode_azeri_case, 'i') != 0x0130)
        testing_t_error_v(t, "AzeriCase.ToUpper('i') is not U+0130");
}

static volatile Rune sink;

static void BenchmarkToUpper(TestingB *b) {
    for (Int i = 0; i < testing_b_n(b); i++)
        sink = unicode_to_upper(0x03B4); /* δ */
}

static void BenchmarkToLower(TestingB *b) {
    for (Int i = 0; i < testing_b_n(b); i++)
        sink = unicode_to_lower(0x0394); /* Δ */
}

static void benchSimpleFold(void *env, TestingB *b) {
    Rune r = *(const Rune *)env;
    for (Int i = 0; i < testing_b_n(b); i++)
        sink = unicode_simple_fold(r);
}

static void BenchmarkSimpleFold(TestingB *b) {
    static Rune upper = 0x0394, lower = 0x03B4, fold = 0x212A, nofold = 0x7FD2;
    testing_b_run(b, BURROW_S("Upper"),
                  BURROW_FN(TestingBFunc, benchSimpleFold, &upper));
    testing_b_run(b, BURROW_S("Lower"),
                  BURROW_FN(TestingBFunc, benchSimpleFold, &lower));
    testing_b_run(b, BURROW_S("Fold"), BURROW_FN(TestingBFunc, benchSimpleFold, &fold));
    testing_b_run(b, BURROW_S("NoFold"),
                  BURROW_FN(TestingBFunc, benchSimpleFold, &nofold));
}

#define TESTS(X)                                                                       \
    X(TestIsLetter)                                                                    \
    X(TestIsUpper)                                                                     \
    X(TestTo)                                                                          \
    X(TestToUpperCase)                                                                 \
    X(TestToLowerCase)                                                                 \
    X(TestToTitleCase)                                                                 \
    X(TestIsSpace)                                                                     \
    X(TestLetterOptimizations)                                                         \
    X(TestTurkishCase)                                                                 \
    X(TestSimpleFold)                                                                  \
    X(TestCalibrate)                                                                   \
    X(TestLatinOffset)                                                                 \
    X(TestSpecialCaseNoMapping)                                                        \
    X(TestNegativeRune)                                                                \
    X(TestDigit)                                                                       \
    X(TestDigitOptimization)                                                           \
    X(TestIsControlLatin1)                                                             \
    X(TestIsLetterLatin1)                                                              \
    X(TestIsUpperLatin1)                                                               \
    X(TestIsLowerLatin1)                                                               \
    X(TestNumberLatin1)                                                                \
    X(TestIsPrintLatin1)                                                               \
    X(TestIsGraphicLatin1)                                                             \
    X(TestIsPunctLatin1)                                                               \
    X(TestIsSpaceLatin1)                                                               \
    X(TestIsSymbolLatin1)                                                              \
    X(TestCategories)                                                                  \
    X(TestProperties)                                                                  \
    X(TestMaps)                                                                        \
    X(BenchmarkToUpper)                                                                \
    X(BenchmarkToLower)                                                                \
    X(BenchmarkSimpleFold)

TESTING_MAIN(TESTS)

/* Derived from Go's src/unicode/utf8/utf8_test.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"

/* The tables are Go's, byte for byte, and they are the whole value of this
 * file. A UTF-8 decoder that handles the ordinary cases is an afternoon's work
 * and every one of the interesting bugs is in the cases below: the byte before
 * the surrogate range and the byte after it, the overlong encodings, the
 * truncated sequence at the end of a buffer, the sequence that is one byte too
 * long. Go has been handed all of those by real input for fifteen years. */

typedef struct Utf8Map {
    Rune r;
    const char *str;
    Int len;
} Utf8Map;

#define M(rune, lit) {(Rune)(rune), lit, (Int)(sizeof(lit) - 1)}

static const Utf8Map utf8map[] = {
    M(0x0000, "\x00"),
    M(0x0001, "\x01"),
    M(0x007e, "\x7e"),
    M(0x007f, "\x7f"),
    M(0x0080, "\xc2\x80"),
    M(0x0081, "\xc2\x81"),
    M(0x00bf, "\xc2\xbf"),
    M(0x00c0, "\xc3\x80"),
    M(0x00c1, "\xc3\x81"),
    M(0x00c8, "\xc3\x88"),
    M(0x00d0, "\xc3\x90"),
    M(0x00e0, "\xc3\xa0"),
    M(0x00f0, "\xc3\xb0"),
    M(0x00f8, "\xc3\xb8"),
    M(0x00ff, "\xc3\xbf"),
    M(0x0100, "\xc4\x80"),
    M(0x07ff, "\xdf\xbf"),
    M(0x0400, "\xd0\x80"),
    M(0x0800, "\xe0\xa0\x80"),
    M(0x0801, "\xe0\xa0\x81"),
    M(0x1000, "\xe1\x80\x80"),
    M(0xd000, "\xed\x80\x80"),
    M(0xd7ff, "\xed\x9f\xbf"), /* last code point before the surrogate half */
    M(0xe000, "\xee\x80\x80"), /* first code point after it */
    M(0xfffe, "\xef\xbf\xbe"),
    M(0xffff, "\xef\xbf\xbf"),
    M(0x10000, "\xf0\x90\x80\x80"),
    M(0x10001, "\xf0\x90\x80\x81"),
    M(0x40000, "\xf1\x80\x80\x80"),
    M(0x10fffe, "\xf4\x8f\xbf\xbe"),
    M(0x10ffff, "\xf4\x8f\xbf\xbf"),
    M(0xFFFD, "\xef\xbf\xbd"),
};

/* Both halves of the surrogate range, which are what UTF-16 uses to spell a
 * code point above 0xFFFF and which are not valid UTF-8. Each decodes to the
 * error rune and a width of one. */
static const Utf8Map surrogate_map[] = {
    M(0xd800, "\xed\xa0\x80"),
    M(0xdfff, "\xed\xbf\xbf"),
};

static const Utf8Map test_strings[] = {
    M(0, ""),
    M(0, "abcd"),
    M(0, "\xe2\x98\xba\xe2\x98\xbb\xe2\x98\xb9"), /* the three smileys */
    M(0, "\xe6\x97\xa5"
         "a"
         "\xe6\x9c\xac"
         "b"
         "\xe8\xaa\x9e"
         "\xc3\xa7"),
    M(0, "\x80\x80\x80\x80"),
};

#define COUNT(a) ((Int)(sizeof(a) / sizeof((a)[0])))

/* A Str over a literal, keeping the length rather than asking strlen, because
 * half of the table has a NUL in it. */
static Str str_of(const Utf8Map *m) {
    Str s = {(const Byte *)m->str, m->len};
    return s;
}

static Slice bytes_of(Byte *buf, Str s) {
    if (s.len > 0)
        memcpy(buf, s.p, (size_t)s.len);
    return slice_from(buf, s.len, s.len, TYPE_BYTE);
}

/* ------------------------------------------------------------------ decoding */

static void TestDecodeWalksTheWholeTable(TestingT *t) {
    Int i;

    for (i = 0; i < COUNT(utf8map); i++) {
        Byte buf[8];
        Str s = str_of(&utf8map[i]);
        Slice b = bytes_of(buf, s);
        Int size = -1;

        CHECK_INT_EQ(utf8_decode_rune_in_string(s, &size), utf8map[i].r);
        CHECK_INT_EQ(size, s.len);

        size = -1;
        CHECK_INT_EQ(utf8_decode_rune(b, &size), utf8map[i].r);
        CHECK_INT_EQ(size, s.len);

        /* Trailing bytes must not change the answer. Go tests this by decoding
         * the slice out to its capacity, which is the same question. */
        {
            Slice longer = slice_from(buf, s.len + 1, s.len + 1, TYPE_BYTE);
            buf[s.len] = 0;
            size = -1;
            CHECK_INT_EQ(utf8_decode_rune(longer, &size), utf8map[i].r);
            CHECK_INT_EQ(size, s.len);
        }
    }
}

static void TestDecodeOfATruncatedSequence(TestingT *t) {
    Int i;

    for (i = 0; i < COUNT(utf8map); i++) {
        Byte buf[8];
        Str full = str_of(&utf8map[i]);
        Str s = {full.p, full.len - 1};
        Slice b = bytes_of(buf, s);
        Int size = -1;

        /* One byte short. The answer is the error rune, and the width is one
         * for a multi byte sequence and zero for an empty input, which is the
         * pair a caller uses to tell "keep reading" from "give up". */
        Int want = full.len > 1 ? 1 : 0;

        CHECK_INT_EQ(utf8_decode_rune_in_string(s, &size), UTF8_RUNE_ERROR);
        CHECK_INT_EQ(size, want);

        size = -1;
        CHECK_INT_EQ(utf8_decode_rune(b, &size), UTF8_RUNE_ERROR);
        CHECK_INT_EQ(size, want);
    }
}

static void TestDecodeOfABrokenSequence(TestingT *t) {
    Int i;

    for (i = 0; i < COUNT(utf8map); i++) {
        Byte buf[8];
        Str full = str_of(&utf8map[i]);
        Slice b = bytes_of(buf, full);
        Str s;
        Int size = -1;

        /* Go's mutation: wreck the last byte of a multi byte sequence, or turn
         * a one byte sequence into a stray continuation byte. Either way the
         * result is invalid and the width is one. */
        if (full.len == 1)
            buf[0] = 0x80;
        else
            buf[full.len - 1] = 0x7F;

        s.p = buf;
        s.len = full.len;

        CHECK_INT_EQ(utf8_decode_rune_in_string(s, &size), UTF8_RUNE_ERROR);
        CHECK_INT_EQ(size, 1);

        size = -1;
        CHECK_INT_EQ(utf8_decode_rune(b, &size), UTF8_RUNE_ERROR);
        CHECK_INT_EQ(size, 1);
    }
}

static void TestDecodeRejectsSurrogateHalves(TestingT *t) {
    Int i;

    for (i = 0; i < COUNT(surrogate_map); i++) {
        Byte buf[8];
        Str s = str_of(&surrogate_map[i]);
        Slice b = bytes_of(buf, s);
        Int size = -1;

        CHECK_INT_EQ(utf8_decode_rune_in_string(s, &size), UTF8_RUNE_ERROR);
        CHECK_INT_EQ(size, 1);

        size = -1;
        CHECK_INT_EQ(utf8_decode_rune(b, &size), UTF8_RUNE_ERROR);
        CHECK_INT_EQ(size, 1);
    }
}

/* Every way a four byte window can be wrong, one row per branch of the accept
 * table. This is the table that makes the difference between a decoder that
 * works and a decoder that lets an overlong encoding of a slash through. */
static const Utf8Map invalid_sequences[] = {
    M(0, "\xed\xa0\x80\x80"), /* surrogate min */
    M(0, "\xed\xbf\xbf\x80"), /* surrogate max */

    M(0, "\x91\x80\x80\x80"), /* a continuation byte on its own */

    M(0, "\xC2\x7F\x80\x80"), M(0, "\xC2\xC0\x80\x80"),
    M(0, "\xDF\x7F\x80\x80"), M(0, "\xDF\xC0\x80\x80"),

    M(0, "\xE0\x9F\xBF\x80"), /* overlong three byte */
    M(0, "\xE0\xA0\x7F\x80"), M(0, "\xE0\xBF\xC0\x80"),
    M(0, "\xE0\xC0\x80\x80"),

    M(0, "\xE1\x7F\xBF\x80"), M(0, "\xE1\x80\x7F\x80"),
    M(0, "\xE1\xBF\xC0\x80"), M(0, "\xE1\xC0\x80\x80"),

    M(0, "\xED\x7F\xBF\x80"), M(0, "\xED\x80\x7F\x80"),
    M(0, "\xED\x9F\xC0\x80"), M(0, "\xED\xA0\x80\x80"),

    M(0, "\xF0\x8F\xBF\xBF"), /* overlong four byte */
    M(0, "\xF0\x90\x7F\xBF"), M(0, "\xF0\x90\x80\x7F"),
    M(0, "\xF0\xBF\xBF\xC0"), M(0, "\xF0\xBF\xC0\x80"),
    M(0, "\xF0\xC0\x80\x80"),

    M(0, "\xF1\x7F\xBF\xBF"), M(0, "\xF1\x80\x7F\xBF"),
    M(0, "\xF1\x80\x80\x7F"), M(0, "\xF1\xBF\xBF\xC0"),
    M(0, "\xF1\xBF\xC0\x80"), M(0, "\xF1\xC0\x80\x80"),

    M(0, "\xF4\x7F\xBF\xBF"), M(0, "\xF4\x80\x7F\xBF"),
    M(0, "\xF4\x80\x80\x7F"), M(0, "\xF4\x8F\xBF\xC0"),
    M(0, "\xF4\x8F\xC0\x80"), M(0, "\xF4\x90\x80\x80"), /* above the maximum rune */
};

static void TestDecodeRejectsEveryInvalidSequence(TestingT *t) {
    Int i;

    for (i = 0; i < COUNT(invalid_sequences); i++) {
        Byte buf[8];
        Str s = str_of(&invalid_sequences[i]);
        Slice b = bytes_of(buf, s);
        Rune r1 = utf8_decode_rune(b, NULL);
        Rune r2 = utf8_decode_rune_in_string(s, NULL);
        Int index = -1;
        Rune r3 = 0;
        StrIter it = str_runes(s);

        CHECK_INT_EQ(r1, UTF8_RUNE_ERROR);
        CHECK_INT_EQ(r2, UTF8_RUNE_ERROR);

        /* And the rune loop has to agree with both, since it is the thing most
         * callers actually use and it has its own ASCII shortcut. */
        CHECK(str_next_rune(&it, &index, &r3));
        CHECK_INT_EQ(index, 0);
        CHECK_INT_EQ(r3, UTF8_RUNE_ERROR);
    }
}

static void TestFullRuneKnowsWhenToWait(TestingT *t) {
    Int i;

    for (i = 0; i < COUNT(utf8map); i++) {
        Byte buf[8];
        Str s = str_of(&utf8map[i]);
        Slice b = bytes_of(buf, s);
        Str short_s = {s.p, s.len - 1};
        Slice short_b = slice_sub(b, 0, b.len - 1);

        CHECK(utf8_full_rune_in_string(s));
        CHECK(utf8_full_rune(b));
        CHECK(!utf8_full_rune_in_string(short_s));
        CHECK(!utf8_full_rune(short_b));
    }

    /* Two bytes that can never start anything. More input will not help, so
     * they count as full and decode as one byte errors. */
    {
        Byte bad[2] = {0xC0, 0xC1};
        Int j;
        for (j = 0; j < 2; j++) {
            Str s = {&bad[j], 1};
            Slice b = slice_from(&bad[j], 1, 1, TYPE_BYTE);
            CHECK(utf8_full_rune_in_string(s));
            CHECK(utf8_full_rune(b));
        }
    }
}

/* ------------------------------------------------------------------ encoding */

static void TestEncodeRoundTripsTheWholeTable(TestingT *t) {
    Int i;

    for (i = 0; i < COUNT(utf8map); i++) {
        Byte buf[10] = {0};
        Slice out = slice_from(buf, (Int)sizeof buf, (Int)sizeof buf, TYPE_BYTE);
        Str s = str_of(&utf8map[i]);
        Int n = utf8_encode_rune(out, utf8map[i].r);
        Str got = {buf, n};

        CHECK_INT_EQ(n, s.len);
        CHECK(str_eq(got, s));
        CHECK_INT_EQ(utf8_rune_len(utf8map[i].r), s.len);
    }
}

static void TestEncodeOfARuneThatCannotBeEncoded(TestingT *t) {
    Byte buf[UTF8_UTF_MAX] = {0};
    Byte want_buf[UTF8_UTF_MAX] = {0};
    Slice out = slice_from(buf, UTF8_UTF_MAX, UTF8_UTF_MAX, TYPE_BYTE);
    Slice want_out = slice_from(want_buf, UTF8_UTF_MAX, UTF8_UTF_MAX, TYPE_BYTE);
    Int want_n = utf8_encode_rune(want_out, UTF8_RUNE_ERROR);
    Str want = {want_buf, want_n};

    /* Negative, past the end of Unicode, and both surrogate halves. All four
     * come out as the replacement character rather than as an error, because
     * this function has nowhere to report one and Go made the same call. */
    Rune bad[4] = {-1, UTF8_MAX_RUNE + 1, 0xD800, 0xDFFF};
    Int i;

    for (i = 0; i < 4; i++) {
        Int n = utf8_encode_rune(out, bad[i]);
        Str got = {buf, n};
        CHECK_INT_EQ(n, 3);
        CHECK(str_eq(got, want));
        CHECK_INT_EQ(utf8_rune_len(bad[i]), -1);
    }
}

static void TestAppendRuneBuildsASlice(TestingT *t) {
    Arena ar;
    Alloc *a;
    Int i;

    arena_init(&ar, NULL, 0);
    a = arena_allocator(&ar);

    for (i = 0; i < COUNT(utf8map); i++) {
        Str want = str_of(&utf8map[i]);
        Slice got;
        Str got_str;

        /* Appending to nothing at all, which is Go's AppendRune(nil, r). */
        got = utf8_append_rune(a, slice_nil(TYPE_BYTE), utf8map[i].r);
        got_str.p = (const Byte *)got.p;
        got_str.len = got.len;
        CHECK(str_eq(got_str, want));

        /* And appending to something, where the four bytes have to land after
         * what was already there. */
        got = slice_make(a, TYPE_BYTE, 0, 0);
        got = slice_append(a, got, "init", 4);
        got = utf8_append_rune(a, got, utf8map[i].r);
        CHECK_INT_EQ(got.len, 4 + want.len);
        got_str.p = (const Byte *)got.p + 4;
        got_str.len = got.len - 4;
        CHECK(str_eq(got_str, want));
    }

    arena_free(&ar);
}

/* ------------------------------------------------------------------ counting */

static void TestRuneCountMatchesADecodeLoop(TestingT *t) {
    static const struct {
        const char *in;
        Int len;
        Int want;
    } cases[] = {
        {"abcd", 4, 4},
        {"\xe2\x98\xba\xe2\x98\xbb\xe2\x98\xb9", 9, 3},
        {"1,2,3,4", 7, 7},
        {"\xe2\x00", 2, 2}, /* a starter with the wrong thing after it */
        {"\xe2\x80", 2, 2},
        {"a\xe2\x80", 3, 3},
    };
    Int i;

    for (i = 0; i < (Int)(sizeof cases / sizeof cases[0]); i++) {
        Byte buf[16];
        Str s = {(const Byte *)cases[i].in, cases[i].len};
        Slice b = bytes_of(buf, s);
        Int loop = 0;
        StrIter it = str_runes(s);

        CHECK_INT_EQ(utf8_rune_count_in_string(s), cases[i].want);
        CHECK_INT_EQ(utf8_rune_count(b), cases[i].want);

        /* The counter and the loop have to agree, since each is written
         * separately and the whole point is that a program can use either. */
        while (str_next_rune(&it, NULL, NULL))
            loop++;
        CHECK_INT_EQ(loop, cases[i].want);
    }
}

static void TestRuneLenCoversEveryWidth(TestingT *t) {
    CHECK_INT_EQ(utf8_rune_len(0), 1);
    CHECK_INT_EQ(utf8_rune_len('e'), 1);
    CHECK_INT_EQ(utf8_rune_len(0x00E9), 2); /* e acute */
    CHECK_INT_EQ(utf8_rune_len(0x263A), 3); /* a smiley */
    CHECK_INT_EQ(utf8_rune_len(UTF8_RUNE_ERROR), 3);
    CHECK_INT_EQ(utf8_rune_len(UTF8_MAX_RUNE), 4);
    CHECK_INT_EQ(utf8_rune_len(0xD800), -1);
    CHECK_INT_EQ(utf8_rune_len(0xDFFF), -1);
    CHECK_INT_EQ(utf8_rune_len(UTF8_MAX_RUNE + 1), -1);
    CHECK_INT_EQ(utf8_rune_len(-1), -1);
}

static void TestRuneStartFindsABoundary(TestingT *t) {
    Byte b;

    /* Continuation bytes are 0x80 to 0xBF and nothing else is, which is the
     * property that lets you seek into the middle of a buffer and find your
     * footing. */
    for (b = 0; b < 0x80; b++)
        CHECK(utf8_rune_start(b));
    for (b = 0x80; b < 0xC0; b++)
        CHECK(!utf8_rune_start(b));
    for (b = 0xC0; b < 0xFF; b++)
        CHECK(utf8_rune_start(b));
    CHECK(utf8_rune_start(0xFF));
}

/* ------------------------------------------------------------------ validity */

static void TestValidSaysYesToUtf8AndNoToEverythingElse(TestingT *t) {
    static const struct {
        const char *in;
        Int len;
        bool want;
    } cases[] = {
        {"", 0, true},
        {"a", 1, true},
        {"abc", 3, true},
        {"\xd0\x96", 2, true}, /* Ж */
        {"\xd0\x96\xd0\x96", 4, true},
        {"\xe2\x98\xba\xe2\x98\xbb\xe2\x98\xb9", 9, true},
        {"aa\xe2", 3, false}, /* truncated at the end */
        {"\x42\xfa", 2, false},
        {"\x42\xfa\x43", 3, false},
        {"a\xef\xbf\xbd"
         "b",
         5, true},                          /* a real U+FFFD is valid */
        {"\xF4\x8F\xBF\xBF", 4, true},      /* U+10FFFF */
        {"\xF4\x90\x80\x80", 4, false},     /* one past it */
        {"\xF7\xBF\xBF\xBF", 4, false},     /* 0x1FFFFF, out of range */
        {"\xFB\xBF\xBF\xBF\xBF", 5, false}, /* 0x3FFFFFF, out of range */
        {"\xc0\x80", 2, false},             /* U+0000 written in two bytes */
        {"\xed\xa0\x80", 3, false},         /* a high surrogate */
        {"\xed\xbf\xbf", 3, false},         /* a low one */
    };
    Int i;

    for (i = 0; i < (Int)(sizeof cases / sizeof cases[0]); i++) {
        Byte buf[16];
        Str s = {(const Byte *)cases[i].in, cases[i].len};
        Slice b = bytes_of(buf, s);

        CHECK(utf8_valid_string(s) == cases[i].want);
        CHECK(utf8_valid(b) == cases[i].want);
    }
}

/* Go builds a hundred more cases at init time by padding each shape with
 * leading and trailing ASCII, which is what shakes out an off by one in a fast
 * path that consumes ASCII in blocks. There is no such fast path here yet and
 * this is the test that will catch it when there is. */
static void TestValidAtEveryAlignment(TestingT *t) {
    Byte buf[256];
    Int i;

    for (i = 0; i < 100; i++) {
        Int j;
        Str s;

        for (j = 0; j < i; j++)
            buf[j] = 'a';

        s.p = buf;
        s.len = i;
        CHECK(utf8_valid_string(s));

        /* Ж on the end, so the last rune straddles whatever boundary the ASCII
         * run ended on. */
        buf[i] = 0xD0;
        buf[i + 1] = 0x96;
        s.len = i + 2;
        CHECK(utf8_valid_string(s));

        /* And the same with ASCII after it. */
        for (j = 0; j < i; j++)
            buf[i + 2 + j] = 'b';
        s.len = i + 2 + i;
        CHECK(utf8_valid_string(s));

        /* A starter with nothing behind it, at the same alignment. */
        buf[i] = 0xE2;
        s.len = i + 1;
        CHECK(!utf8_valid_string(s));

        for (j = 0; j < i; j++)
            buf[i + 1 + j] = 'b';
        s.len = i + 1 + i;
        CHECK(!utf8_valid_string(s));
    }
}

static void TestValidRuneKnowsTheRange(TestingT *t) {
    CHECK(utf8_valid_rune(0));
    CHECK(utf8_valid_rune('e'));
    CHECK(utf8_valid_rune(0x00E9));
    CHECK(utf8_valid_rune(0x263A));
    CHECK(utf8_valid_rune(UTF8_RUNE_ERROR));
    CHECK(utf8_valid_rune(UTF8_MAX_RUNE));
    CHECK(utf8_valid_rune(0xD7FF));
    CHECK(!utf8_valid_rune(0xD800));
    CHECK(!utf8_valid_rune(0xDFFF));
    CHECK(utf8_valid_rune(0xE000));
    CHECK(!utf8_valid_rune(UTF8_MAX_RUNE + 1));
    CHECK(!utf8_valid_rune(-1));
}

/* ----------------------------------------------------------------- sequencing
 *
 * Go's TestSequencing, which is the test that ties the three ways of walking a
 * string together. Decode forwards with the rune loop, record what it found,
 * check the one shot decoder agrees at every offset, then walk backwards with
 * the last rune decoder and check it visits the same offsets in reverse. Any
 * disagreement between the three is a bug in one of them, and this is the only
 * test here that would catch a decoder that is self consistently wrong. */

typedef struct Visit {
    Int index;
    Rune r;
} Visit;

static void check_sequence(TestingT *t, Str s) {
    Visit seen[512];
    Byte buf[512];
    Slice b;
    Int j = 0, si;
    Int index;
    Rune r;
    StrIter it = str_runes(s);

    if (s.len > (Int)sizeof buf)
        return;
    b = bytes_of(buf, s);

    while (str_next_rune(&it, &index, &r)) {
        Int size1 = -1, size2 = -1;
        Str rest = {s.p + index, s.len - index};
        Slice rest_b = slice_sub(b, index, b.len);

        CHECK_INT_EQ(utf8_decode_rune(rest_b, &size1), r);
        CHECK_INT_EQ(utf8_decode_rune_in_string(rest, &size2), r);
        CHECK_INT_EQ(size1, size2);

        seen[j].index = index;
        seen[j].r = r;
        j++;
    }

    j--;
    for (si = s.len; si > 0;) {
        Int size1 = -1, size2 = -1;
        Str head = {s.p, si};
        Slice head_b = slice_sub(b, 0, si);

        CHECK_INT_EQ(utf8_decode_last_rune(head_b, &size1), seen[j].r);
        CHECK_INT_EQ(utf8_decode_last_rune_in_string(head, &size2), seen[j].r);
        CHECK_INT_EQ(size1, size2);

        si -= size1;
        CHECK_INT_EQ(si, seen[j].index);
        j--;
    }

    CHECK_INT_EQ(si, 0);
}

static void TestForwardsAndBackwardsVisitTheSameRunes(TestingT *t) {
    Int i, k;

    for (i = 0; i < COUNT(test_strings); i++) {
        for (k = 0; k < COUNT(utf8map); k++) {
            Byte buf[256];
            Str ts = str_of(&test_strings[i]);
            Str m = str_of(&utf8map[k]);
            Str joined;
            Int n = 0;

            /* ts + m */
            memcpy(buf, ts.p, (size_t)ts.len);
            memcpy(buf + ts.len, m.p, (size_t)m.len);
            n = ts.len + m.len;
            joined.p = buf;
            joined.len = n;
            check_sequence(t, joined);

            /* m + ts */
            memcpy(buf, m.p, (size_t)m.len);
            memcpy(buf + m.len, ts.p, (size_t)ts.len);
            check_sequence(t, joined);

            /* ts + m + ts */
            memcpy(buf, ts.p, (size_t)ts.len);
            memcpy(buf + ts.len, m.p, (size_t)m.len);
            memcpy(buf + ts.len + m.len, ts.p, (size_t)ts.len);
            joined.len = n + ts.len;
            check_sequence(t, joined);
        }
    }
}

static void TestDecodeLastRuneOfNothing(TestingT *t) {
    Int size = -1;
    Str empty = BURROW_STR_EMPTY;

    CHECK_INT_EQ(utf8_decode_last_rune_in_string(empty, &size), UTF8_RUNE_ERROR);
    CHECK_INT_EQ(size, 0);

    size = -1;
    CHECK_INT_EQ(utf8_decode_rune_in_string(empty, &size), UTF8_RUNE_ERROR);
    CHECK_INT_EQ(size, 0);

    /* A run of continuation bytes longer than any rune, which is the case the
     * backwards search has to give up on rather than scan to the start of the
     * buffer. Getting this wrong makes the backwards walk quadratic. */
    {
        Byte junk[16];
        Str s = {junk, 16};
        Int i;
        for (i = 0; i < 16; i++)
            junk[i] = 0x80;
        size = -1;
        CHECK_INT_EQ(utf8_decode_last_rune_in_string(s, &size), UTF8_RUNE_ERROR);
        CHECK_INT_EQ(size, 1);
    }
}

/* --------------------------------------------------------------- the rune loop */

static void TestTheRuneLoopIsGosRangeOverAString(TestingT *t) {
    Str s = BURROW_S("a\xe2\x98\xba"
                     "b");
    Int index;
    Rune r;
    StrIter it = str_runes(s);
    Int n = 0;

    /* The index is the byte offset the rune started at, not a count of runes,
     * which is what Go's loop gives you and what you need in order to slice. */
    CHECK(str_next_rune(&it, &index, &r));
    CHECK_INT_EQ(index, 0);
    CHECK_INT_EQ(r, 'a');

    CHECK(str_next_rune(&it, &index, &r));
    CHECK_INT_EQ(index, 1);
    CHECK_INT_EQ(r, 0x263A);

    CHECK(str_next_rune(&it, &index, &r));
    CHECK_INT_EQ(index, 4);
    CHECK_INT_EQ(r, 'b');

    CHECK(!str_next_rune(&it, &index, &r));
    CHECK(!str_next_rune(&it, &index, &r));

    /* Both out parameters are optional, the same as everywhere else. */
    it = str_runes(s);
    while (str_next_rune(&it, NULL, NULL))
        n++;
    CHECK_INT_EQ(n, 3);

    /* The zero value iterates zero times rather than reading through a NULL
     * pointer, which is what the zero value rule buys. */
    {
        StrIter zero = BURROW_ZERO(StrIter);
        CHECK(!str_next_rune(&zero, NULL, NULL));
    }
}

static void TestTheRuneLoopTerminatesOnRubbish(TestingT *t) {
    Byte junk[64];
    Str s = {junk, 64};
    Int i, n = 0;
    StrIter it;

    for (i = 0; i < 64; i++)
        junk[i] = (Byte)(0x80 + (i % 0x40));

    /* Every byte here is a continuation byte, so every step is a one byte
     * error rune and the loop runs exactly as many times as there are bytes.
     * A decoder that returned a width of zero for invalid input would hang
     * here, and that is the reason the width is one. */
    for (it = str_runes(s); str_next_rune(&it, NULL, NULL);)
        n++;
    CHECK_INT_EQ(n, 64);
}

#define TESTS(X)                                                                       \
    X(TestDecodeWalksTheWholeTable)                                                    \
    X(TestDecodeOfATruncatedSequence)                                                  \
    X(TestDecodeOfABrokenSequence)                                                     \
    X(TestDecodeRejectsSurrogateHalves)                                                \
    X(TestDecodeRejectsEveryInvalidSequence)                                           \
    X(TestFullRuneKnowsWhenToWait)                                                     \
    X(TestEncodeRoundTripsTheWholeTable)                                               \
    X(TestEncodeOfARuneThatCannotBeEncoded)                                            \
    X(TestAppendRuneBuildsASlice)                                                      \
    X(TestRuneCountMatchesADecodeLoop)                                                 \
    X(TestRuneLenCoversEveryWidth)                                                     \
    X(TestRuneStartFindsABoundary)                                                     \
    X(TestValidSaysYesToUtf8AndNoToEverythingElse)                                     \
    X(TestValidAtEveryAlignment)                                                       \
    X(TestValidRuneKnowsTheRange)                                                      \
    X(TestForwardsAndBackwardsVisitTheSameRunes)                                       \
    X(TestDecodeLastRuneOfNothing)                                                     \
    X(TestTheRuneLoopIsGosRangeOverAString)                                            \
    X(TestTheRuneLoopTerminatesOnRubbish)

TESTING_MAIN(TESTS)

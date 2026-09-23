/* Derived from Go's src/strconv/quote_test.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "harness.h"

#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/mem/track.h"
#include "burrow/strconv.h"

#include <string.h>

static Arena ar;
static Alloc *a;

#define S BURROW_S
#define SI BURROW_S_INIT

/* Go checks IsPrint and IsGraphic against the unicode package for every rune.
 * burrow has no unicode package yet, so the answer Go gives is folded into an
 * FNV-1a hash, one byte per rune, together with how many runes said yes. The
 * numbers are what Go 1.27.1's unicode.IsPrint and unicode.IsGraphic give, and
 * tools/gen-isprint.sh has to be rerun whenever they change. */
#define PRINT_COUNT 159613
#define PRINT_HASH 0xd8c3265c7df90aULL
#define GRAPHIC_COUNT 159629
#define GRAPHIC_HASH 0x27a72dae2a8fb0a4ULL

TEST(is_print) {
    uint64_t h = 0xcbf29ce484222325ULL;
    Int n = 0;

    for (Rune r = 0; r <= BURROW_RUNE_MAX; r++) {
        bool p = strconv_is_print(r);
        n += p;
        h = (h ^ (uint64_t)p) * 0x100000001b3ULL;
    }
    CHECK_INT_EQ(n, PRINT_COUNT);
    CHECK(h == PRINT_HASH);
}

TEST(is_graphic) {
    uint64_t h = 0xcbf29ce484222325ULL;
    Int n = 0;

    for (Rune r = 0; r <= BURROW_RUNE_MAX; r++) {
        bool g = strconv_is_graphic(r);
        n += g;
        h = (h ^ (uint64_t)g) * 0x100000001b3ULL;
    }
    CHECK_INT_EQ(n, GRAPHIC_COUNT);
    CHECK(h == GRAPHIC_HASH);
}

TEST(is_print_outside_the_range) {
    CHECK(!strconv_is_print(-1));
    CHECK(!strconv_is_print(0x110000));
    CHECK(!strconv_is_graphic(-1));
    CHECK(!strconv_is_graphic(0x7fffffff));
    /* Go looks a negative rune up by its low 16 bits, and 0x3000 is graphic. */
    CHECK(strconv_is_graphic(-53248));
}

typedef struct QuoteTest {
    Str in, out, ascii, graphic;
} QuoteTest;

static const QuoteTest quotetests[] = {
    {SI("\a\b\f\r\n\t\v"), SI("\"\\a\\b\\f\\r\\n\\t\\v\""),
     SI("\"\\a\\b\\f\\r\\n\\t\\v\""), SI("\"\\a\\b\\f\\r\\n\\t\\v\"")},
    {SI("\\"), SI("\"\\\\\""), SI("\"\\\\\""), SI("\"\\\\\"")},
    {SI("abc\xff"
        "def"),
     SI("\"abc\\xffdef\""), SI("\"abc\\xffdef\""), SI("\"abc\\xffdef\"")},
    {SI("☺"), SI("\"☺\""), SI("\"\\u263a\""), SI("\"☺\"")},
    {SI("\xf4\x8f\xbf\xbf"), SI("\"\\U0010ffff\""), SI("\"\\U0010ffff\""),
     SI("\"\\U0010ffff\"")},
    {SI("\x04"), SI("\"\\x04\""), SI("\"\\x04\""), SI("\"\\x04\"")},
    /* Some runes that are graphic and not printable. The last column is the
     * only one that keeps them. */
    {SI("!\u00a0!\u2000!\u3000!"), SI("\"!\\u00a0!\\u2000!\\u3000!\""),
     SI("\"!\\u00a0!\\u2000!\\u3000!\""), SI("\"!\u00a0!\u2000!\u3000!\"")},
    {SI("\x7f"), SI("\"\\x7f\""), SI("\"\\x7f\""), SI("\"\\x7f\"")},
};

#define NQUOTE ((int)(sizeof quotetests / sizeof quotetests[0]))

/* "abc" + want, which is what every Append test compares against. */
static bool appended(Slice got, Str want) {
    return got.len == 3 + want.len && memcmp(got.p, "abc", 3) == 0 &&
           memcmp((const Byte *)got.p + 3, want.p, (size_t)want.len) == 0;
}

static Slice abc(void) {
    return slice_from_str(a, S("abc"));
}

TEST(quote) {
    for (int i = 0; i < NQUOTE; i++) {
        const QuoteTest *tt = &quotetests[i];
        CHECK(str_eq(strconv_quote(a, tt->in), tt->out));
        CHECK(appended(strconv_append_quote(a, abc(), tt->in), tt->out));
    }
}

TEST(quote_to_ascii) {
    for (int i = 0; i < NQUOTE; i++) {
        const QuoteTest *tt = &quotetests[i];
        CHECK(str_eq(strconv_quote_to_ascii(a, tt->in), tt->ascii));
        CHECK(appended(strconv_append_quote_to_ascii(a, abc(), tt->in), tt->ascii));
    }
}

TEST(quote_to_graphic) {
    for (int i = 0; i < NQUOTE; i++) {
        const QuoteTest *tt = &quotetests[i];
        CHECK(str_eq(strconv_quote_to_graphic(a, tt->in), tt->graphic));
        CHECK(appended(strconv_append_quote_to_graphic(a, abc(), tt->in), tt->graphic));
    }
}

typedef struct QuoteRuneTest {
    Rune in;
    Str out, ascii, graphic;
} QuoteRuneTest;

static const QuoteRuneTest quoterunetests[] = {
    {'a', SI("'a'"), SI("'a'"), SI("'a'")},
    {'\a', SI("'\\a'"), SI("'\\a'"), SI("'\\a'")},
    {'\\', SI("'\\\\'"), SI("'\\\\'"), SI("'\\\\'")},
    {0xFF, SI("'ÿ'"), SI("'\\u00ff'"), SI("'ÿ'")},
    {0x263a, SI("'☺'"), SI("'\\u263a'"), SI("'☺'")},
    {0xdead, SI("'\xef\xbf\xbd'"), SI("'\\ufffd'"), SI("'\xef\xbf\xbd'")},
    {0xfffd, SI("'\xef\xbf\xbd'"), SI("'\\ufffd'"), SI("'\xef\xbf\xbd'")},
    {0x0010ffff, SI("'\\U0010ffff'"), SI("'\\U0010ffff'"), SI("'\\U0010ffff'")},
    {0x0010ffff + 1, SI("'\xef\xbf\xbd'"), SI("'\\ufffd'"), SI("'\xef\xbf\xbd'")},
    {0x04, SI("'\\x04'"), SI("'\\x04'"), SI("'\\x04'")},
    /* Where graphic and printable part ways. */
    {0x00a0, SI("'\\u00a0'"), SI("'\\u00a0'"), SI("'\u00a0'")},
    {0x2000, SI("'\\u2000'"), SI("'\\u2000'"), SI("'\u2000'")},
    {0x3000, SI("'\\u3000'"), SI("'\\u3000'"), SI("'\u3000'")},
};

#define NQUOTERUNE ((int)(sizeof quoterunetests / sizeof quoterunetests[0]))

TEST(quote_rune) {
    for (int i = 0; i < NQUOTERUNE; i++) {
        const QuoteRuneTest *tt = &quoterunetests[i];
        CHECK(str_eq(strconv_quote_rune(a, tt->in), tt->out));
        CHECK(appended(strconv_append_quote_rune(a, abc(), tt->in), tt->out));
    }
}

TEST(quote_rune_to_ascii) {
    for (int i = 0; i < NQUOTERUNE; i++) {
        const QuoteRuneTest *tt = &quoterunetests[i];
        CHECK(str_eq(strconv_quote_rune_to_ascii(a, tt->in), tt->ascii));
        CHECK(
            appended(strconv_append_quote_rune_to_ascii(a, abc(), tt->in), tt->ascii));
    }
}

TEST(quote_rune_to_graphic) {
    for (int i = 0; i < NQUOTERUNE; i++) {
        const QuoteRuneTest *tt = &quoterunetests[i];
        CHECK(str_eq(strconv_quote_rune_to_graphic(a, tt->in), tt->graphic));
        CHECK(appended(strconv_append_quote_rune_to_graphic(a, abc(), tt->in),
                       tt->graphic));
    }
}

TEST(append_to_nothing_and_in_place) {
    /* A zero Slice is Go's nil []byte. */
    Slice got = strconv_append_quote(a, (Slice){0}, S("x"));
    CHECK(got.elem == TYPE_BYTE);
    CHECK(got.len == 3 && memcmp(got.p, "\"x\"", 3) == 0);

    /* Room enough, so the bytes go into the array that is already there. */
    Slice buf = slice_make(a, TYPE_BYTE, 0, 16);
    Slice out = strconv_append_quote_rune(a, buf, 'q');
    CHECK(out.p == buf.p);
    CHECK(out.len == 3 && memcmp(out.p, "'q'", 3) == 0);
}

TEST(results_own_exactly_their_length) {
    /* The counting run and the writing run agree, so each result can be freed
     * with its own length. The tracking allocator checks the size on every free
     * and reports anything left over. */
    Track tr;
    track_init(&tr, heap_allocator());
    Alloc *t = track_allocator(&tr);

    Str q = strconv_quote(t, S("tab\there ☺ \xff"));
    CHECK(str_eq(q, S("\"tab\\there ☺ \\xff\"")));
    mem_free(t, (void *)(Uintptr)q.p, (size_t)q.len, 1);

    q = strconv_quote_rune_to_ascii(t, 0x10ffff);
    CHECK(str_eq(q, S("'\\U0010ffff'")));
    mem_free(t, (void *)(Uintptr)q.p, (size_t)q.len, 1);

    q = strconv_unquote(t, S("\"a\\tb\\u263a\""), NULL);
    CHECK(str_eq(q, S("a\tb☺")));
    mem_free(t, (void *)(Uintptr)q.p, (size_t)q.len, 1);

    q = strconv_unquote(t, S("`a\rb\r`"), NULL);
    CHECK(str_eq(q, S("ab")));
    mem_free(t, (void *)(Uintptr)q.p, (size_t)q.len, 1);

    CHECK(track_check(&tr) == 0);
    track_free(&tr);
}

TEST(results_longer_than_the_stack_buffer) {
    /* 200 copies of a tab, a smiley and a stray byte quote to 1802 bytes, far
     * past what fits on the stack, so the writing is done a second time into
     * the result. Unquoting it is long too, and has to give back the input. */
    Track tr;
    track_init(&tr, heap_allocator());
    Alloc *t = track_allocator(&tr);

    Byte in[200 * 5];
    for (int i = 0; i < 200; i++)
        memcpy(in + i * 5, "\t\xe2\x98\xba\xff", 5);
    Str s = str_from_bytes(in, (Int)sizeof in);

    Str q = strconv_quote(t, s);
    CHECK(q.len == 2 + 200 * 9);
    CHECK(q.p[0] == '"' && q.p[q.len - 1] == '"');
    CHECK(memcmp(q.p + 1, "\\t\xe2\x98\xba\\xff\\t", 11) == 0);

    Slice buf = slice_make(t, TYPE_BYTE, 1, 4);
    ((Byte *)buf.p)[0] = '>';
    Slice ap = strconv_append_quote(t, buf, s);
    CHECK(ap.len == 1 + q.len && ((Byte *)ap.p)[0] == '>');
    CHECK(memcmp((Byte *)ap.p + 1, q.p, (size_t)q.len) == 0);

    Error err;
    Str u = strconv_unquote(t, q, &err);
    CHECK(BURROW_OK(err));
    CHECK(str_eq(u, s));

    mem_free(t, (void *)(Uintptr)u.p, (size_t)u.len, 1);
    mem_free(t, ap.p, (size_t)ap.cap, 1);
    mem_free(t, buf.p, (size_t)buf.cap, 1);
    mem_free(t, (void *)(Uintptr)q.p, (size_t)q.len, 1);
    CHECK(track_check(&tr) == 0);
    track_free(&tr);
}

typedef struct CanBackquoteTest {
    Str in;
    bool out;
} CanBackquoteTest;

static const CanBackquoteTest canbackquotetests[] = {
    {SI("`"), false},
    {SI("\x00"), false},
    {SI("\x01"), false},
    {SI("\x02"), false},
    {SI("\x03"), false},
    {SI("\x04"), false},
    {SI("\x05"), false},
    {SI("\x06"), false},
    {SI("\x07"), false},
    {SI("\x08"), false},
    {SI("\x09"), true}, /* \t */
    {SI("\x0a"), false},
    {SI("\x0b"), false},
    {SI("\x0c"), false},
    {SI("\x0d"), false},
    {SI("\x0e"), false},
    {SI("\x0f"), false},
    {SI("\x10"), false},
    {SI("\x11"), false},
    {SI("\x12"), false},
    {SI("\x13"), false},
    {SI("\x14"), false},
    {SI("\x15"), false},
    {SI("\x16"), false},
    {SI("\x17"), false},
    {SI("\x18"), false},
    {SI("\x19"), false},
    {SI("\x1a"), false},
    {SI("\x1b"), false},
    {SI("\x1c"), false},
    {SI("\x1d"), false},
    {SI("\x1e"), false},
    {SI("\x1f"), false},
    {SI("\x7f"), false},
    {SI("' !\"#$%&'()*+,-./:;<=>?@[\\]^_{|}~"), true},
    {SI("0123456789"), true},
    {SI("ABCDEFGHIJKLMNOPQRSTUVWXYZ"), true},
    {SI("abcdefghijklmnopqrstuvwxyz"), true},
    {SI("☺"), true},
    {SI("\x80"), false},
    {SI("a\xe0\xa0z"), false},
    {SI("\xef\xbb\xbf"
        "abc"),
     false},
    {SI("a\xef\xbb\xbfz"), false},
};

TEST(can_backquote) {
    for (size_t i = 0; i < sizeof canbackquotetests / sizeof canbackquotetests[0]; i++)
        CHECK(strconv_can_backquote(canbackquotetests[i].in) ==
              canbackquotetests[i].out);
}

typedef struct UnquoteTest {
    Str in, out;
} UnquoteTest;

static const UnquoteTest unquotetests[] = {
    {SI("\"\""), SI("")},
    {SI("\"a\""), SI("a")},
    {SI("\"abc\""), SI("abc")},
    {SI("\"☺\""), SI("☺")},
    {SI("\"hello world\""), SI("hello world")},
    {SI("\"\\xFF\""), SI("\xFF")},
    {SI("\"\\377\""), SI("\377")},
    {SI("\"\\u1234\""), SI("\u1234")},
    {SI("\"\\U00010111\""), SI("\U00010111")},
    {SI("\"\\U0001011111\""), SI("\U00010111"
                                 "11")},
    {SI("\"\\a\\b\\f\\n\\r\\t\\v\\\\\\\"\""), SI("\a\b\f\n\r\t\v\\\"")},
    {SI("\"'\""), SI("'")},

    {SI("'a'"), SI("a")},
    {SI("'☹'"), SI("☹")},
    {SI("'\\a'"), SI("\a")},
    {SI("'\\x10'"), SI("\x10")},
    {SI("'\\377'"), SI("\377")},
    {SI("'\\u1234'"), SI("\u1234")},
    {SI("'\\U00010111'"), SI("\U00010111")},
    {SI("'\\t'"), SI("\t")},
    {SI("' '"), SI(" ")},
    {SI("'\\''"), SI("'")},
    {SI("'\"'"), SI("\"")},

    {SI("``"), SI("")},
    {SI("`a`"), SI("a")},
    {SI("`abc`"), SI("abc")},
    {SI("`☺`"), SI("☺")},
    {SI("`hello world`"), SI("hello world")},
    {SI("`\\xFF`"), SI("\\xFF")},
    {SI("`\\377`"), SI("\\377")},
    {SI("`\\`"), SI("\\")},
    {SI("`\n`"), SI("\n")},
    {SI("`\t`"), SI("\t")},
    {SI("` `"), SI(" ")},
    {SI("`a\rb`"), SI("ab")},
};

static const Str misquoted[] = {
    SI(""),          SI("\""),      SI("\"a"),         SI("\"'"),
    SI("b\""),       SI("\"\\\""),  SI("\"\\9\""),     SI("\"\\19\""),
    SI("\"\\129\""), SI("'\\'"),    SI("'\\9'"),       SI("'\\19'"),
    SI("'\\129'"),   SI("'ab'"),    SI("\"\\x1!\""),   SI("\"\\U12345678\""),
    SI("\"\\z\""),   SI("`"),       SI("`xxx"),        SI("``x\r"),
    SI("`\""),       SI("\"\\'\""), SI("'\\\"'"),      SI("\"\n\""),
    SI("\"\\n\n\""), SI("'\n'"),    SI("\"\\udead\""), SI("\"\\ud83d\\ude4f\""),
};

static bool same_error(Error got, Error want) {
    return got.vt == want.vt && got.data == want.data;
}

/* Go's testUnquote, which also checks QuotedPrefix with some junk on the end. */
static void check_unquote(Str in, Str want, Error want_err) {
    Error err;
    Str got = strconv_unquote(a, in, &err);
    CHECK(str_eq(got, want));
    CHECK(same_error(err, want_err));

    if (BURROW_OK(err))
        want = in;

    /* The characters that mean something inside a literal, minus whichever one
     * opens this one, since that would close it. */
    static const char specials[] = "\n\r\\\"`'";
    Byte buf[256];
    Int n = in.len;
    if ((size_t)n + sizeof specials > sizeof buf) {
        CHECK(n < 0); /* the test buffer is too small */
        return;
    }
    memcpy(buf, in.p, (size_t)n);
    for (const char *c = specials; *c != '\0'; c++)
        if (in.len == 0 || (Byte)*c != in.p[0])
            buf[n++] = (Byte)*c;
    Str with_suffix = str_from_bytes(buf, n);

    got = strconv_quoted_prefix(with_suffix, &err);
    if (BURROW_OK(err) && BURROW_FAILED(want_err)) {
        /* The input was junk after a good literal. Reparse just the literal. */
        (void)strconv_unquote(a, got, &want_err);
        want = got;
    }
    CHECK(str_eq(got, want));
    CHECK(same_error(err, want_err));
}

TEST(unquote) {
    for (size_t i = 0; i < sizeof unquotetests / sizeof unquotetests[0]; i++)
        check_unquote(unquotetests[i].in, unquotetests[i].out, BURROW_NO_ERROR);
    for (int i = 0; i < NQUOTE; i++)
        check_unquote(quotetests[i].out, quotetests[i].in, BURROW_NO_ERROR);
    for (size_t i = 0; i < sizeof misquoted / sizeof misquoted[0]; i++)
        check_unquote(misquoted[i], S(""), strconv_err_syntax);
}

/* Issue 23685: invalid UTF-8 must not take the fast path. */
TEST(unquote_invalid_utf8) {
    check_unquote(S("\"foo\""), S("foo"), BURROW_NO_ERROR);
    check_unquote(S("\"foo"), S(""), strconv_err_syntax);
    check_unquote(S("\"\xc0\""), S("\xef\xbf\xbd"), BURROW_NO_ERROR);
    check_unquote(S("\"a\xc0\""), S("a\xef\xbf\xbd"), BURROW_NO_ERROR);
    check_unquote(S("\"\\t\xc0\""), S("\t\xef\xbf\xbd"), BURROW_NO_ERROR);
}

TEST(unquote_borrows_when_it_can) {
    Str in = S("\"plain\"");
    Str got = strconv_unquote(a, in, NULL);
    CHECK(got.p == in.p + 1);

    in = S("`raw`");
    got = strconv_unquote(a, in, NULL);
    CHECK(got.p == in.p + 1);

    in = S("\"esc\\n\"");
    got = strconv_unquote(a, in, NULL);
    CHECK(str_eq(got, S("esc\n")));
    CHECK(got.p < in.p || got.p >= in.p + in.len);
}

TEST(unquote_char) {
    bool mb = true;
    Str tail = S("junk");
    Error err;

    Rune r = strconv_unquote_char(S("\\u263a rest"), '"', &mb, &tail, &err);
    CHECK(r == 0x263a && mb && str_eq(tail, S(" rest")) && BURROW_OK(err));

    r = strconv_unquote_char(S("\\xffz"), '"', &mb, &tail, &err);
    CHECK(r == 0xff && !mb && str_eq(tail, S("z")) && BURROW_OK(err));

    r = strconv_unquote_char(S("☺!"), 0, &mb, &tail, &err);
    CHECK(r == 0x263a && mb && str_eq(tail, S("!")) && BURROW_OK(err));

    /* Quote zero allows both quotes bare and neither escaped. */
    r = strconv_unquote_char(S("'"), 0, &mb, &tail, &err);
    CHECK(r == '\'' && BURROW_OK(err));
    r = strconv_unquote_char(S("\\'"), 0, &mb, &tail, &err);
    CHECK(r == 0 && !mb && tail.len == 0 && same_error(err, strconv_err_syntax));
    r = strconv_unquote_char(S("\\'"), '\'', NULL, NULL, &err);
    CHECK(r == '\'' && BURROW_OK(err));
    r = strconv_unquote_char(S("'"), '\'', NULL, NULL, &err);
    CHECK(same_error(err, strconv_err_syntax));

    /* Every out pointer may be NULL. */
    CHECK(strconv_unquote_char(S("x"), 0, NULL, NULL, NULL) == 'x');
}

TEST(errors_read_like_go) {
    CHECK(str_eq(error_text(strconv_err_syntax), S("invalid syntax")));
    CHECK(str_eq(error_text(strconv_err_range), S("value out of range")));
}

int main(void) {
    arena_init(&ar, NULL, 0);
    a = arena_allocator(&ar);

    RUN(is_print);
    RUN(is_graphic);
    RUN(is_print_outside_the_range);
    RUN(quote);
    RUN(quote_to_ascii);
    RUN(quote_to_graphic);
    RUN(quote_rune);
    RUN(quote_rune_to_ascii);
    RUN(quote_rune_to_graphic);
    RUN(append_to_nothing_and_in_place);
    RUN(results_own_exactly_their_length);
    RUN(results_longer_than_the_stack_buffer);
    RUN(can_backquote);
    RUN(unquote);
    RUN(unquote_invalid_utf8);
    RUN(unquote_borrows_when_it_can);
    RUN(unquote_char);
    RUN(errors_read_like_go);

    arena_free(&ar);
    return harness_report("strconv quote");
}

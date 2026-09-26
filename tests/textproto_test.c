/* Derived from Go's src/net/textproto/reader_test.go, header_test.go and
 * writer_test.go. Go source: go1.27.1.
 *
 * TestIssue46363 is a race on the lazily built table of common headers, and
 * the table here is a constant, so there is nothing for it to race on. It
 * becomes TestCommonHeaderFromGoroutines, which reads headers from several
 * goroutines at once for tsan to look at. TestReadMIMEHeaderAllocations counts
 * bytes with runtime.MemStats and here counts them with a Track allocator
 * over the test's arena, which frees what the headers hold.
 * TestConn and TestPipeline are new, since Go tests those through net/smtp.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/net/textproto.h"

#include "burrow/bufio.h"
#include "burrow/bytes.h"
#include "burrow/error.h"
#include "burrow/func.h"
#include "burrow/io.h"
#include "burrow/map.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/track.h"
#include "burrow/panic.h"
#include "burrow/proc.h"
#include "burrow/strings.h"
#include "burrow/sync.h"
#include "burrow/sync/atomic.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define S BURROW_S

#define ARENA_BEGIN                                                                    \
    Arena ar;                                                                          \
    arena_init(&ar, NULL, 0);                                                          \
    Alloc *a = arena_allocator(&ar)
#define ARENA_END arena_free(&ar)

static TextprotoReader *reader(Alloc *a, Str s) {
    StringsReader *sr = strings_new_reader(a, s);
    return textproto_new_reader(a,
                                bufio_new_reader(a, strings_reader_as_io_reader(sr)));
}

static bool same_error(Error a, Error b) {
    return a.vt == b.vt && a.data == b.data;
}

static bool str_is(Str s, const char *want) {
    return str_eq(s, str_from_cstr(want));
}

/* The header as text, keys sorted, for comparing with what Go's %v prints of
 * the map. */
static Str header_text(Alloc *a, TextprotoMIMEHeader h) {
    Int n = map_len(h);
    Str *keys =
        (Str *)mem_alloc(a, sizeof(Str) * (size_t)(n > 0 ? n : 1), _Alignof(Str));
    Int k = 0;
    const void *key;
    void *val;
    for (MapIter it = map_iter(h); map_next(&it, &key, &val);)
        keys[k++] = *(const Str *)key;
    for (Int i = 1; i < k; i++)
        for (Int j = i; j > 0 && str_cmp(keys[j - 1], keys[j]) > 0; j--) {
            Str x = keys[j];
            keys[j] = keys[j - 1];
            keys[j - 1] = x;
        }
    StringsBuilder b = STRINGS_BUILDER(a);
    strings_builder_write_string(&b, S("map["), NULL);
    for (Int i = 0; i < k; i++) {
        if (i > 0)
            strings_builder_write_byte(&b, ' ');
        strings_builder_write_string(&b, keys[i], NULL);
        strings_builder_write_string(&b, S(":["), NULL);
        Slice vs = textproto_mime_header_values(h, keys[i]);
        for (Int j = 0; j < vs.len; j++) {
            if (j > 0)
                strings_builder_write_byte(&b, ' ');
            strings_builder_write_string(&b, ((const Str *)vs.p)[j], NULL);
        }
        strings_builder_write_byte(&b, ']');
    }
    strings_builder_write_byte(&b, ']');
    return strings_builder_string(&b);
}

static void TestReadLine(TestingT *t) {
    ARENA_BEGIN;
    TextprotoReader *r = reader(a, S("line1\nline2\n"));
    Error err;
    Str s = textproto_reader_read_line(r, a, &err);
    CHECK(str_is(s, "line1") && BURROW_OK(err));
    s = textproto_reader_read_line(r, a, &err);
    CHECK(str_is(s, "line2") && BURROW_OK(err));
    s = textproto_reader_read_line(r, a, &err);
    CHECK(s.len == 0 && same_error(err, io_eof));
    ARENA_END;
}

static void TestReadLineLongLine(TestingT *t) {
    ARENA_BEGIN;
    Str line = strings_repeat(a, S("12345"), 10000);
    StringsBuilder b = STRINGS_BUILDER(a);
    strings_builder_write_string(&b, line, NULL);
    strings_builder_write_string(&b, S("\r\n"), NULL);
    TextprotoReader *r = reader(a, strings_builder_string(&b));
    Error err;
    Str s = textproto_reader_read_line(r, a, &err);
    CHECK(BURROW_OK(err));
    CHECK(str_eq(s, line));

    /* ReadLineBytes gives the same line. */
    r = reader(a, strings_builder_string(&b));
    Slice bs = textproto_reader_read_line_bytes(r, a, &err);
    CHECK(BURROW_OK(err));
    CHECK(str_eq(str_from_bytes(bs.p, bs.len), line));
    ARENA_END;
}

static void TestReadContinuedLine(TestingT *t) {
    ARENA_BEGIN;
    TextprotoReader *r = reader(a, S("line1\nline\n 2\nline3\n"));
    Error err;
    Str s = textproto_reader_read_continued_line(r, a, &err);
    CHECK(str_is(s, "line1") && BURROW_OK(err));
    s = textproto_reader_read_continued_line(r, a, &err);
    CHECK(str_is(s, "line 2") && BURROW_OK(err));
    s = textproto_reader_read_continued_line(r, a, &err);
    CHECK(str_is(s, "line3") && BURROW_OK(err));
    s = textproto_reader_read_continued_line(r, a, &err);
    CHECK(s.len == 0 && same_error(err, io_eof));

    r = reader(a, S("a\n  b\n\tc\nd\n"));
    Slice bs = textproto_reader_read_continued_line_bytes(r, a, &err);
    CHECK(BURROW_OK(err) && str_is(str_from_bytes(bs.p, bs.len), "a b c"));
    ARENA_END;
}

static void TestReadCodeLine(TestingT *t) {
    ARENA_BEGIN;
    TextprotoReader *r = reader(a, S("123 hi\n234 bye\n345 no way\n"));
    Error err;
    Str msg;
    Int code = textproto_reader_read_code_line(r, a, 0, &msg, &err);
    CHECK(code == 123 && str_is(msg, "hi") && BURROW_OK(err));
    code = textproto_reader_read_code_line(r, a, 23, &msg, &err);
    CHECK(code == 234 && str_is(msg, "bye") && BURROW_OK(err));
    code = textproto_reader_read_code_line(r, a, 346, &msg, &err);
    CHECK(code == 345 && str_is(msg, "no way") && !BURROW_OK(err));
    const TextprotoError *e =
        (const TextprotoError *)errors_as(err, TYPE_TEXTPROTO_ERROR);
    CHECK(e != NULL && e->code == code && str_eq(e->msg, msg));
    CHECK(str_is(error_text(err), "345 \"no way\""));
    code = textproto_reader_read_code_line(r, a, 1, &msg, &err);
    CHECK(code == 0 && msg.len == 0 && same_error(err, io_eof));
    ARENA_END;
}

/* The three kinds of bad line, which Go tests only through net/smtp. */
static void TestReadCodeLineErrors(TestingT *t) {
    ARENA_BEGIN;
    static const struct {
        const char *in;
        Int code;
        const char *err;
    } tests[] = {
        {"12\n", 0, "short response: \"12\""},
        {"abc def\n", 0, "invalid response code: \"abc def\""},
        {"099 low\n", 99, "invalid response code: \"099 low\""},
        {"-12 x\n", -12, "invalid response code: \"-12 x\""},
        {"250-more\n", 250, "unexpected multi-line response: \"more\""},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        TextprotoReader *r = reader(a, str_from_cstr(tests[i].in));
        Error err;
        Str msg;
        Int code = textproto_reader_read_code_line(r, a, 0, &msg, &err);
        if (code != tests[i].code || !str_is(error_text(err), tests[i].err))
            testing_t_errorf_v(t, "ReadCodeLine(%q) = %d, %q; want %d, %q", tests[i].in,
                               code, error_text(err), tests[i].code, tests[i].err);
        CHECK(errors_as(err, TYPE_TEXTPROTO_PROTOCOL_ERROR) != NULL);
    }
    ARENA_END;
}

static void check_lines(TestingT *t, Slice got, const char *const *want, Int n) {
    const Str *g = (const Str *)got.p;
    if (got.len != n) {
        testing_t_errorf_v(t, "got %d lines, want %d", got.len, n);
        return;
    }
    for (Int i = 0; i < n; i++)
        if (!str_is(g[i], want[i]))
            testing_t_errorf_v(t, "line %d = %q, want %q", i, g[i], want[i]);
}

static void TestReadDotLines(TestingT *t) {
    ARENA_BEGIN;
    TextprotoReader *r =
        reader(a, S("dotlines\r\n.foo\r\n..bar\n...baz\nquux\r\n\r\n.\r\nanother\n"));
    Error err;
    Slice s = textproto_reader_read_dot_lines(r, a, &err);
    static const char *const want[] = {"dotlines", "foo", ".bar", "..baz", "quux", ""};
    check_lines(t, s, want, 6);
    CHECK(BURROW_OK(err));

    s = textproto_reader_read_dot_lines(r, a, &err);
    static const char *const want2[] = {"another"};
    check_lines(t, s, want2, 1);
    CHECK(same_error(err, io_err_unexpected_eof));
    ARENA_END;
}

static void TestReadDotBytes(TestingT *t) {
    ARENA_BEGIN;
    TextprotoReader *r = reader(
        a, S("dotlines\r\n.foo\r\n..bar\n...baz\nquux\r\n\r\n.\r\nanot.her\r\n"));
    Error err;
    Slice b = textproto_reader_read_dot_bytes(r, a, &err);
    CHECK(str_is(str_from_bytes(b.p, b.len), "dotlines\nfoo\n.bar\n..baz\nquux\n\n"));
    CHECK(BURROW_OK(err));

    b = textproto_reader_read_dot_bytes(r, a, &err);
    CHECK(str_is(str_from_bytes(b.p, b.len), "anot.her\n"));
    CHECK(same_error(err, io_err_unexpected_eof));
    ARENA_END;
}

/* A dot reader left part way through is drained by the next read, and a lone
 * "\r" or ".\r" inside a line comes through as data. */
static void TestDotReaderClose(TestingT *t) {
    ARENA_BEGIN;
    TextprotoReader *r = reader(a, S("a\rb\r\n.\rc\r\nrest\r\n.\r\nnext\r\n"));
    IoReader d = textproto_reader_dot_reader(r);
    Byte buf[3];
    Error err;
    Int n = d.vt->read(d.data, slice_from(buf, 3, 3, TYPE_BYTE), &err);
    CHECK(n == 3 && memcmp(buf, "a\rb", 3) == 0 && BURROW_OK(err));
    Str line = textproto_reader_read_line(r, a, &err);
    CHECK(str_is(line, "next") && BURROW_OK(err));

    r = reader(a, S("a\rb\r\n.\rc\r\n.\r\n"));
    Slice all = textproto_reader_read_dot_bytes(r, a, &err);
    CHECK(str_is(str_from_bytes(all.p, all.len), "a\rb\n\rc\n") && BURROW_OK(err));
    ARENA_END;
}

static void TestReadMIMEHeader(TestingT *t) {
    ARENA_BEGIN;
    TextprotoReader *r = reader(a, S("my-key: Value 1  \r\nLong-key: Even \n Longer "
                                     "Value\r\nmy-Key: Value 2\r\n\n"));
    Error err;
    TextprotoMIMEHeader m = textproto_reader_read_mime_header(r, a, &err);
    Str got = header_text(a, m);
    if (!str_is(got, "map[Long-Key:[Even Longer Value] My-Key:[Value 1 Value 2]]") ||
        !BURROW_OK(err))
        testing_t_errorf_v(t, "ReadMIMEHeader: %s, %q", got, error_text(err));
    ARENA_END;
}

static void TestReadMIMEHeaderSingle(TestingT *t) {
    ARENA_BEGIN;
    TextprotoReader *r = reader(a, S("Foo: bar\n\n"));
    Error err;
    TextprotoMIMEHeader m = textproto_reader_read_mime_header(r, a, &err);
    Str got = header_text(a, m);
    if (!str_is(got, "map[Foo:[bar]]") || !BURROW_OK(err))
        testing_t_errorf_v(t, "ReadMIMEHeader: %s, %q", got, error_text(err));
    ARENA_END;
}

static void TestReaderUpcomingHeaderKeys(TestingT *t) {
    ARENA_BEGIN;
    static const struct {
        const char *input;
        Int want;
    } tests[] = {
        {"", 0},
        {"A: v", 1},
        {"A: v\r\nB: v\r\n", 2},
        {"A: v\nB: v\n", 2},
        {"A: v\r\n  continued\r\n  still continued\r\nB: v\r\n\r\n", 2},
        {"A: v\r\n\r\nB: v\r\nC: v\r\n", 1},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        TextprotoReader *r = reader(a, str_from_cstr(tests[i].input));
        Int got = burrow__textproto_upcoming_header_keys(r);
        if (got != tests[i].want)
            testing_t_errorf_v(t, "upcomingHeaderKeys(%q): %d; want %d", tests[i].input,
                               got, tests[i].want);
    }
    Str many = strings_repeat(a, S("\n"), 1000);
    StringsBuilder b = STRINGS_BUILDER(a);
    strings_builder_write_string(&b, S("A: v"), NULL);
    strings_builder_write_string(&b, many, NULL);
    CHECK(burrow__textproto_upcoming_header_keys(
              reader(a, strings_builder_string(&b))) == 1);
    ARENA_END;
}

static void TestReadMIMEHeaderNoKey(TestingT *t) {
    ARENA_BEGIN;
    TextprotoReader *r = reader(a, S(": bar\ntest-1: 1\n\n"));
    Error err;
    TextprotoMIMEHeader m = textproto_reader_read_mime_header(r, a, &err);
    CHECK(m != NULL && map_len(m) == 0);
    CHECK(!BURROW_OK(err));
    ARENA_END;
}

static void TestLargeReadMIMEHeader(TestingT *t) {
    ARENA_BEGIN;
    Str sdata = strings_repeat(a, S("x"), 16 * 1024);
    StringsBuilder b = STRINGS_BUILDER(a);
    strings_builder_write_string(&b, S("Cookie: "), NULL);
    strings_builder_write_string(&b, sdata, NULL);
    strings_builder_write_string(&b, S("\r\n\n"), NULL);
    TextprotoReader *r = reader(a, strings_builder_string(&b));
    Error err;
    TextprotoMIMEHeader m = textproto_reader_read_mime_header(r, a, &err);
    CHECK(BURROW_OK(err));
    Str cookie = textproto_mime_header_get(m, S("Cookie"));
    if (!str_eq(cookie, sdata))
        testing_t_errorf_v(t, "ReadMIMEHeader: %d bytes, want %d bytes", cookie.len,
                           sdata.len);
    ARENA_END;
}

static void TestReadMIMEHeaderNonCompliant(TestingT *t) {
    ARENA_BEGIN;
    TextprotoReader *r = reader(a, S("Foo: bar\r\n"
                                     "Content-Language: en\r\n"
                                     "SID : 0\r\n"
                                     "Audio Mode : None\r\n"
                                     "Privilege : 127\r\n\r\n"));
    Error err;
    TextprotoMIMEHeader m = textproto_reader_read_mime_header(r, a, &err);
    Str got = header_text(a, m);
    if (!str_is(got, "map[Audio Mode :[None] Content-Language:[en] Foo:[bar] "
                     "Privilege :[127] SID :[0]]") ||
        !BURROW_OK(err))
        testing_t_errorf_v(t, "ReadMIMEHeader =\n%s, %q", got, error_text(err));
    ARENA_END;
}

static void TestReadMIMEHeaderMalformed(TestingT *t) {
    ARENA_BEGIN;
    static const char *const inputs[] = {
        "No colon first line\r\nFoo: foo\r\n\r\n",
        " No colon first line with leading space\r\nFoo: foo\r\n\r\n",
        "\tNo colon first line with leading tab\r\nFoo: foo\r\n\r\n",
        " First: line with leading space\r\nFoo: foo\r\n\r\n",
        "\tFirst: line with leading tab\r\nFoo: foo\r\n\r\n",
        "Foo: foo\r\nNo colon second line\r\n\r\n",
        "Foo-\n\tBar: foo\r\n\r\n",
        "Foo-\r\n\tBar: foo\r\n\r\n",
        "Foo\r\n\t: foo\r\n\r\n",
        "Foo-\n\tBar",
        "Foo \tBar: foo\r\n\r\n",
        ": empty key\r\n\r\n",
    };
    for (size_t i = 0; i < sizeof inputs / sizeof inputs[0]; i++) {
        TextprotoReader *r = reader(a, str_from_cstr(inputs[i]));
        Error err;
        (void)textproto_reader_read_mime_header(r, a, &err);
        if (BURROW_OK(err) || same_error(err, io_eof))
            testing_t_errorf_v(t, "ReadMIMEHeader(%q) = %q; want an error", inputs[i],
                               error_text(err));
    }
    ARENA_END;
}

/* The messages the errors have, which Go's tests leave to net/http. */
static void TestReadMIMEHeaderErrorText(TestingT *t) {
    ARENA_BEGIN;
    static const struct {
        const char *in;
        const char *err;
    } tests[] = {
        {"No colon\r\n\r\n", "malformed MIME header: missing colon: \"No colon\""},
        {" lead: x\r\n\r\n", "malformed MIME header initial line: \" lead: x\""},
        {"Foo \tBar: foo\r\n\r\n", "malformed MIME header line: \"Foo \\tBar: foo\""},
        {"Foo: a\x01z\r\n\r\n", "malformed MIME header line: \"Foo: a\\x01z\""},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        TextprotoReader *r = reader(a, str_from_cstr(tests[i].in));
        Error err;
        (void)textproto_reader_read_mime_header(r, a, &err);
        if (!str_is(error_text(err), tests[i].err))
            testing_t_errorf_v(t, "ReadMIMEHeader(%q): %q; want %q", tests[i].in,
                               error_text(err), tests[i].err);
        Error same = textproto_protocol_error_as_error(error_text(err), a);
        CHECK(errors_is(err, same));
    }
    ARENA_END;
}

static void TestReadMIMEHeaderBytes(TestingT *t) {
    ARENA_BEGIN;
    for (int i = 0; i <= 0xff; i++) {
        /* string(rune(i)) is UTF-8, two bytes from 0x80 up. */
        Byte in[32];
        Int n = 0;
        memcpy(in, "Foo", 3);
        n = 3;
        if (i < 0x80) {
            in[n++] = (Byte)i;
        } else {
            in[n++] = (Byte)(0xc0 | (i >> 6));
            in[n++] = (Byte)(0x80 | (i & 0x3f));
        }
        memcpy(in + n, "Bar: foo\r\n\r\n", 12);
        n += 12;
        bool want_err = true;
        if ((i >= '0' && i <= '9') || (i >= 'a' && i <= 'z') ||
            (i >= 'A' && i <= 'Z') ||
            (i != 0 && strchr("!#$%&'*+-.^_`|~", i) != NULL) || i == ':' || i == ' ')
            want_err = false;
        TextprotoReader *r = reader(a, str_from_bytes(in, n));
        Error err;
        (void)textproto_reader_read_mime_header(r, a, &err);
        if (!BURROW_OK(err) != want_err)
            testing_t_errorf_v(t, "ReadMIMEHeader(key byte %d): %q; want error=%t", i,
                               error_text(err), want_err);
    }
    for (int i = 0; i <= 0xff; i++) {
        Byte in[32];
        Int n = 0;
        memcpy(in, "Foo: foo", 8);
        n = 8;
        if (i < 0x80) {
            in[n++] = (Byte)i;
        } else {
            in[n++] = (Byte)(0xc0 | (i >> 6));
            in[n++] = (Byte)(0x80 | (i & 0x3f));
        }
        memcpy(in + n, "bar\r\n\r\n", 7);
        n += 7;
        bool want_err = true;
        if ((i >= 0x21 && i <= 0x7e) || i == ' ' || i == '\t' ||
            (i >= 0x80 && i <= 0xff))
            want_err = false;
        TextprotoReader *r = reader(a, str_from_bytes(in, n));
        Error err;
        (void)textproto_reader_read_mime_header(r, a, &err);
        if (!BURROW_OK(err) != want_err)
            testing_t_errorf_v(t, "ReadMIMEHeader(value byte %d): %q; want error=%t", i,
                               error_text(err), want_err);
    }
    ARENA_END;
}

static void TestReadMIMEHeaderTrimContinued(TestingT *t) {
    ARENA_BEGIN;
    TextprotoReader *r = reader(a, S("a:\n"
                                     " 0 \r\n"
                                     "b:1 \t\r\n"
                                     "c: 2\r\n"
                                     " 3\t\n"
                                     "  \t 4  \r\n\n"));
    Error err;
    TextprotoMIMEHeader m = textproto_reader_read_mime_header(r, a, &err);
    CHECK(BURROW_OK(err));
    Str got = header_text(a, m);
    if (!str_is(got, "map[A:[0] B:[1] C:[2 3 4]]"))
        testing_t_errorf_v(t, "ReadMIMEHeader mismatch.\n got: %s", got);
    ARENA_END;
}

/* Issue 58975: a header followed by a lot of blank lines should not cost
 * memory in proportion to them. The count covers the header, its strings and
 * the map, and not the readers, which Go does not count either. */
static void TestReadMIMEHeaderAllocations(TestingT *t) {
    ARENA_BEGIN;
    Str blank = strings_repeat(a, S("\n"), 4096);
    StringsBuilder b = STRINGS_BUILDER(a);
    strings_builder_write_string(&b, S("A: b\r\n\r\n"), NULL);
    strings_builder_write_string(&b, blank, NULL);
    Str in = strings_builder_string(&b);
    Track tr;
    track_init(&tr, a);
    Alloc *ta = track_allocator(&tr);
    enum { COUNT = 200 };
    for (int i = 0; i < COUNT; i++) {
        TextprotoReader *r = reader(a, in);
        Error err;
        TextprotoMIMEHeader m = textproto_reader_read_mime_header(r, ta, &err);
        if (!BURROW_OK(err))
            testing_t_fatalf_v(t, "ReadMIMEHeader: %q", error_text(err));
        (void)m;
    }
    if (tr.bytes_total / COUNT > 32768)
        testing_t_errorf_v(t, "ReadMIMEHeader allocated %d bytes, want < 32768",
                           (Int)(tr.bytes_total / COUNT));
    track_free(&tr);
    ARENA_END;
}

static const struct {
    const char *in;
    Int in_code;
    Int want_code;
    const char *want_msg;
} read_response_tests[] = {
    {"230-Anonymous access granted, restrictions apply\n"
     "Read the file README.txt,\n"
     "230  please",
     23, 230,
     "Anonymous access granted, restrictions apply\nRead the file README.txt,\n "
     "please"},
    {"230 Anonymous access granted, restrictions apply\n", 23, 230,
     "Anonymous access granted, restrictions apply"},
    {"400-A\n400-B\n400 C", 4, 400, "A\nB\nC"},
    {"400-A\r\n400-B\r\n400 C\r\n", 4, 400, "A\nB\nC"},
};

/* See https://www.ietf.org/rfc/rfc959.txt page 36. */
static void TestRFC959Lines(TestingT *t) {
    ARENA_BEGIN;
    for (size_t i = 0; i < sizeof read_response_tests / sizeof read_response_tests[0];
         i++) {
        StringsBuilder b = STRINGS_BUILDER(a);
        strings_builder_write_string(&b, str_from_cstr(read_response_tests[i].in),
                                     NULL);
        strings_builder_write_string(&b, S("\nFOLLOWING DATA"), NULL);
        TextprotoReader *r = reader(a, strings_builder_string(&b));
        Error err;
        Str msg;
        Int code = textproto_reader_read_response(r, a, read_response_tests[i].in_code,
                                                  &msg, &err);
        if (!BURROW_OK(err)) {
            testing_t_errorf_v(t, "#%d: ReadResponse: %q", (Int)i, error_text(err));
            continue;
        }
        if (code != read_response_tests[i].want_code)
            testing_t_errorf_v(t, "#%d: code=%d, want %d", (Int)i, code,
                               read_response_tests[i].want_code);
        if (!str_is(msg, read_response_tests[i].want_msg))
            testing_t_errorf_v(t, "#%d: msg=%q, want %q", (Int)i, msg,
                               read_response_tests[i].want_msg);
    }
    ARENA_END;
}

/* Issue 10230: a multi-line error is read to its end. */
static void TestReadMultiLineError(TestingT *t) {
    ARENA_BEGIN;
    TextprotoReader *r = reader(
        a,
        S("550-5.1.1 The email account that you tried to reach does not exist. Please "
          "try\n"
          "550-5.1.1 double-checking the recipient's email address for typos or\n"
          "550-5.1.1 unnecessary spaces. Learn more at\n"
          "Unexpected but legal text!\n"
          "550 5.1.1 https://support.google.com/mail/answer/6596 h20si25154304pfd.166 "
          "- "
          "gsmtp\n"));
    const char *want_msg =
        "5.1.1 The email account that you tried to reach does not exist. Please try\n"
        "5.1.1 double-checking the recipient's email address for typos or\n"
        "5.1.1 unnecessary spaces. Learn more at\n"
        "Unexpected but legal text!\n"
        "5.1.1 https://support.google.com/mail/answer/6596 h20si25154304pfd.166 - "
        "gsmtp";
    const char *want_error =
        "550 \"5.1.1 The email account that you tried to reach does not exist. Please "
        "try\\n5.1.1 double-checking the recipient's email address for typos "
        "or\\n5.1.1 "
        "unnecessary spaces. Learn more at\\nUnexpected but legal text!\\n5.1.1 "
        "https://support.google.com/mail/answer/6596 h20si25154304pfd.166 - gsmtp\"";
    Error err;
    Str msg;
    Int code = textproto_reader_read_response(r, a, 250, &msg, &err);
    CHECK(!BURROW_OK(err));
    CHECK(code == 550);
    if (!str_is(msg, want_msg))
        testing_t_errorf_v(t, "ReadResponse: msg=%q, want %q", msg, want_msg);
    if (!BURROW_OK(err) && !str_is(error_text(err), want_error))
        testing_t_errorf_v(t, "ReadResponse: error=%q, want %q", error_text(err),
                           want_error);
    const TextprotoError *e =
        (const TextprotoError *)errors_as(err, TYPE_TEXTPROTO_ERROR);
    CHECK(e != NULL && e->code == 550 && str_eq(e->msg, msg));
    Str text = textproto_error_error(e, a);
    CHECK(str_is(text, want_error));
    ARENA_END;
}

static void TestCommonHeaders(TestingT *t) {
    ARENA_BEGIN;
    for (Int i = 0;; i++) {
        Str h = burrow__textproto_common_header(i);
        if (h.len == 0)
            break;
        if (!str_eq(h, textproto_canonical_mime_header_key(a, h)))
            testing_t_errorf_v(t, "Non-canonical header %q in commonHeader", h);
        if (i > 0 && str_cmp(burrow__textproto_common_header(i - 1), h) >= 0)
            testing_t_errorf_v(t, "commonHeader not sorted at %q", h);
    }
    /* A common header costs no allocation, as in Go. */
    Track tr;
    track_init(&tr, a);
    Alloc *ta = track_allocator(&tr);
    for (int i = 0; i < 200; i++) {
        Str x = textproto_canonical_mime_header_key(ta, S("content-Length"));
        if (!str_is(x, "Content-Length"))
            testing_t_fatalf_v(t, "canonicalMIMEHeaderKey = %q; want %q", x,
                               "Content-Length");
    }
    CHECK(tr.allocs == 0);
    track_free(&tr);
    ARENA_END;
}

/* In place of TestIssue46363. The table is a constant, so this only shows
 * that header reads on several goroutines at once are fine. */
#define HEADER_WORKERS 8

static SyncWaitGroup header_wg;
static SyncAtomicUint32 header_bad;

static void header_worker(void *env) {
    (void)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (int i = 0; i < 50; i++) {
        TextprotoReader *r = reader(a, S("A: 1\r\ncontent-type: 2\r\nC: 3\r\n\r\n"));
        Error err;
        TextprotoMIMEHeader m = textproto_reader_read_mime_header(r, a, &err);
        if (!BURROW_OK(err) ||
            !str_is(textproto_mime_header_get(m, S("Content-Type")), "2"))
            sync_atomic_uint32_add(&header_bad, 1);
        Str k = textproto_canonical_mime_header_key(a, S("a"));
        if (!str_is(k, "A"))
            sync_atomic_uint32_add(&header_bad, 1);
    }
    arena_free(&ar);
}

static void header_main(void *env) {
    (void)env;
    for (int i = 0; i < HEADER_WORKERS; i++)
        sync_wait_group_go(&header_wg, BURROW_FN(Func, header_worker, NULL));
    sync_wait_group_wait(&header_wg);
}

static void TestCommonHeaderFromGoroutines(TestingT *t) {
    header_main(NULL);
    CHECK(sync_atomic_uint32_load(&header_bad) == 0);
}

/* ------------------------------------------------------------ header_test.go */

static void TestCanonicalMIMEHeaderKey(TestingT *t) {
    ARENA_BEGIN;
    static const struct {
        const char *in, *out;
    } tests[] = {
        {"a-b-c", "A-B-C"},
        {"a-1-c", "A-1-C"},
        {"User-Agent", "User-Agent"},
        {"uSER-aGENT", "User-Agent"},
        {"user-agent", "User-Agent"},
        {"USER-AGENT", "User-Agent"},

        /* Other valid tchar bytes in tokens. */
        {"foo-bar_baz", "Foo-Bar_baz"},
        {"foo-bar$baz", "Foo-Bar$baz"},
        {"foo-bar~baz", "Foo-Bar~baz"},
        {"foo-bar*baz", "Foo-Bar*baz"},

        /* Non-ASCII or anything with spaces or non-token chars is unchanged. */
        {"\xc3\xbcser-agenT", "\xc3\xbcser-agenT"},
        {"a B", "a B"},

        /* This caused a panic due to mishandling of a space. */
        {"C Ontent-Transfer-Encoding", "C Ontent-Transfer-Encoding"},
        {"foo bar", "foo bar"},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Str s = textproto_canonical_mime_header_key(a, str_from_cstr(tests[i].in));
        if (!str_is(s, tests[i].out))
            testing_t_errorf_v(t, "CanonicalMIMEHeaderKey(%q) = %q, want %q",
                               tests[i].in, s, tests[i].out);
    }
    /* A long key goes through the allocator rather than the stack buffer. */
    Str long_key = strings_repeat(a, S("ab-"), 100);
    Str want = strings_repeat(a, S("Ab-"), 100);
    CHECK(str_eq(textproto_canonical_mime_header_key(a, long_key), want));
    ARENA_END;
}

static void TestMIMEHeaderMultipleValues(TestingT *t) {
    ARENA_BEGIN;
    TextprotoMIMEHeader h = textproto_mime_header_make(a);
    CHECK(textproto_mime_header_add(h, S("Set-Cookie"), S("cookie 1")));
    CHECK(textproto_mime_header_add(h, S("Set-Cookie"), S("cookie 2")));
    Slice values = textproto_mime_header_values(h, S("set-cookie"));
    CHECK_INT_EQ(values.len, 2);
    ARENA_END;
}

/* Add, Set, Get, Values and Del, which Go leaves to net/http's tests. */
static void TestMIMEHeaderMethods(TestingT *t) {
    ARENA_BEGIN;
    TextprotoMIMEHeader h = textproto_mime_header_make(a);
    CHECK(textproto_mime_header_add(h, S("x-custom"), S("1")));
    CHECK(textproto_mime_header_add(h, S("X-CUSTOM"), S("2")));
    CHECK(str_is(textproto_mime_header_get(h, S("X-Custom")), "1"));
    CHECK(textproto_mime_header_values(h, S("x-custom")).len == 2);
    CHECK(textproto_mime_header_set(h, S("x-custom"), S("3")));
    CHECK(str_is(textproto_mime_header_get(h, S("x-custom")), "3"));
    CHECK(textproto_mime_header_values(h, S("x-custom")).len == 1);
    CHECK(textproto_mime_header_set(h, S("content-type"), S("text/plain")));
    CHECK(str_is(header_text(a, h), "map[Content-Type:[text/plain] X-Custom:[3]]"));
    textproto_mime_header_del(h, S("CONTENT-TYPE"));
    CHECK(str_is(textproto_mime_header_get(h, S("Content-Type")), ""));
    CHECK(map_len(h) == 1);
    /* A key that is not a token is kept as it is. */
    CHECK(textproto_mime_header_set(h, S("a b"), S("c")));
    CHECK(str_is(textproto_mime_header_get(h, S("a b")), "c"));
    /* A nil header reads as empty. */
    CHECK(textproto_mime_header_get(NULL, S("x")).len == 0);
    CHECK(textproto_mime_header_values(NULL, S("x")).len == 0);
    textproto_mime_header_del(NULL, S("x"));
    ARENA_END;
}

/* ------------------------------------------------------------ writer_test.go */

static TextprotoWriter *writer(Alloc *a, BytesBuffer *buf) {
    return textproto_new_writer(a, bufio_new_writer(a, bytes_buffer_as_io_writer(buf)));
}

static bool buf_is(BytesBuffer *b, const char *want) {
    Slice s = bytes_buffer_bytes(b);
    return str_is(str_from_bytes(s.p, s.len), want);
}

static void TestPrintfLine(TestingT *t) {
    ARENA_BEGIN;
    BytesBuffer buf = BYTES_BUFFER(a);
    TextprotoWriter *w = writer(a, &buf);
    Error err = textproto_writer_printf_line_v(w, "foo %d", 123);
    CHECK(buf_is(&buf, "foo 123\r\n") && BURROW_OK(err));
    ARENA_END;
}

static void TestDotWriter(TestingT *t) {
    ARENA_BEGIN;
    BytesBuffer buf = BYTES_BUFFER(a);
    TextprotoWriter *w = writer(a, &buf);
    IoWriteCloser d = textproto_writer_dot_writer(w);
    Error err;
    const char *in = "abc\n.def\n..ghi\n.jkl\n.";
    Int n = d.vt->writer.write(
        d.data, slice_from((Byte *)(uintptr_t)in, 21, 21, TYPE_BYTE), &err);
    CHECK(n == 21 && BURROW_OK(err));
    CHECK(BURROW_OK(d.vt->closer.close(d.data)));
    CHECK(buf_is(&buf, "abc\r\n..def\r\n...ghi\r\n..jkl\r\n..\r\n.\r\n"));
    ARENA_END;
}

static void TestDotWriterCloseEmptyWrite(TestingT *t) {
    ARENA_BEGIN;
    BytesBuffer buf = BYTES_BUFFER(a);
    TextprotoWriter *w = writer(a, &buf);
    IoWriteCloser d = textproto_writer_dot_writer(w);
    Error err;
    Int n = d.vt->writer.write(d.data, slice_from(NULL, 0, 0, TYPE_BYTE), &err);
    CHECK(n == 0 && BURROW_OK(err));
    (void)d.vt->closer.close(d.data);
    CHECK(buf_is(&buf, "\r\n.\r\n"));
    ARENA_END;
}

static void TestDotWriterCloseNoWrite(TestingT *t) {
    ARENA_BEGIN;
    BytesBuffer buf = BYTES_BUFFER(a);
    TextprotoWriter *w = writer(a, &buf);
    IoWriteCloser d = textproto_writer_dot_writer(w);
    (void)d.vt->closer.close(d.data);
    CHECK(buf_is(&buf, "\r\n.\r\n"));
    ARENA_END;
}

/* A dot writer left open is closed by the next line, and a "\r\n" already in
 * the data is not doubled. */
static void TestDotWriterClosedByPrintfLine(TestingT *t) {
    ARENA_BEGIN;
    BytesBuffer buf = BYTES_BUFFER(a);
    TextprotoWriter *w = writer(a, &buf);
    IoWriteCloser d = textproto_writer_dot_writer(w);
    Error err;
    (void)d.vt->writer.write(
        d.data, slice_from((Byte *)(uintptr_t)"a\r\nb\r", 5, 5, TYPE_BYTE), &err);
    CHECK(BURROW_OK(textproto_writer_printf_line_v(w, "QUIT")));
    CHECK(buf_is(&buf, "a\r\nb\r\n.\r\nQUIT\r\n"));
    CHECK(BURROW_OK(d.vt->closer.close(d.data)));
    CHECK(buf_is(&buf, "a\r\nb\r\n.\r\nQUIT\r\n"));
    ARENA_END;
}

/* ----------------------------------------------------------- Conn, Pipeline */

/* A connection whose reads come from one buffer and whose writes go to
 * another. */
typedef struct FakeConn {
    BytesBuffer in;
    BytesBuffer out;
    bool closed;
} FakeConn;

static Int fake_read(void *self, Slice p, Error *err) {
    return bytes_buffer_as_io_reader(&((FakeConn *)self)->in)
        .vt->read(&((FakeConn *)self)->in, p, err);
}

static Int fake_write(void *self, Slice p, Error *err) {
    return bytes_buffer_as_io_writer(&((FakeConn *)self)->out)
        .vt->write(&((FakeConn *)self)->out, p, err);
}

static Error fake_close(void *self) {
    ((FakeConn *)self)->closed = true;
    return BURROW_NO_ERROR;
}

static const IoReadWriteCloserVT fake_vt = {
    {NULL, fake_read},
    {NULL, fake_write},
    {NULL, fake_close},
};

static void TestConn(TestingT *t) {
    ARENA_BEGIN;
    FakeConn fc = {BYTES_BUFFER(a), BYTES_BUFFER(a), false};
    bytes_buffer_write_string(&fc.in, S("220 ready\r\n331 password please\r\n"), NULL);
    TextprotoConn *c = textproto_new_conn(a, (IoReadWriteCloser){&fake_vt, &fc});
    CHECK(c != NULL);
    Error err;
    Str msg;
    Int code = textproto_reader_read_code_line(&c->reader, a, 220, &msg, &err);
    CHECK(code == 220 && str_is(msg, "ready") && BURROW_OK(err));

    Uint id = textproto_conn_cmd_v(c, &err, "USER %s", "gopher");
    CHECK(id == 0 && BURROW_OK(err));
    textproto_conn_start_response(c, id);
    code = textproto_conn_read_code_line(c, a, 331, &msg, &err);
    textproto_conn_end_response(c, id);
    CHECK(code == 331 && str_is(msg, "password please") && BURROW_OK(err));
    CHECK(buf_is(&fc.out, "USER gopher\r\n"));

    id = textproto_conn_cmd_v(c, &err, "PASS %s", "x");
    CHECK(id == 1 && BURROW_OK(err));
    CHECK(textproto_conn_next(c) == 2);
    textproto_conn_start_request(c, 2);
    CHECK(BURROW_OK(textproto_conn_printf_line_v(c, "QUIT")));
    textproto_conn_end_request(c, 2);
    CHECK(buf_is(&fc.out, "USER gopher\r\nPASS x\r\nQUIT\r\n"));
    textproto_conn_start_response(c, 1);
    textproto_conn_end_response(c, 1);
    textproto_conn_start_response(c, 2);
    Str line = textproto_conn_read_line(c, a, &err);
    CHECK(line.len == 0 && same_error(err, io_eof));
    textproto_conn_end_response(c, 2);
    CHECK(BURROW_OK(textproto_conn_close(c)) && fc.closed);
    textproto_conn_free(c);
    ARENA_END;
}

#define PIPE_WORKERS 16

static TextprotoPipeline pipe_p;
static SyncWaitGroup pipe_wg;
static SyncMutex pipe_mu;
static Uint pipe_log[PIPE_WORKERS];
static Int pipe_n;

static void pipe_worker(void *env) {
    (void)env;
    Uint id = textproto_pipeline_next(&pipe_p);
    textproto_pipeline_start_request(&pipe_p, id);
    sync_mutex_lock(&pipe_mu);
    pipe_log[pipe_n++] = id;
    sync_mutex_unlock(&pipe_mu);
    textproto_pipeline_end_request(&pipe_p, id);
    textproto_pipeline_start_response(&pipe_p, id);
    textproto_pipeline_end_response(&pipe_p, id);
}

static void pipe_main(void *env) {
    (void)env;
    for (int i = 0; i < PIPE_WORKERS; i++)
        sync_wait_group_go(&pipe_wg, BURROW_FN(Func, pipe_worker, NULL));
    sync_wait_group_wait(&pipe_wg);
}

static void end_out_of_turn(void *env) {
    TextprotoPipeline *p = (TextprotoPipeline *)env;
    textproto_pipeline_end_request(p, 5);
}

static void TestPipeline(TestingT *t) {
    pipe_main(NULL);
    CHECK(pipe_n == PIPE_WORKERS);
    for (Int i = 0; i < pipe_n; i++)
        if (pipe_log[i] != (Uint)i)
            testing_t_errorf_v(t, "turn %d went to id %d", i, (Int)pipe_log[i]);

    TextprotoPipeline p = {0};
    volatile bool got = false;
    BURROW_TRY {
        end_out_of_turn(&p);
    }
    BURROW_CATCH(r) {
        got = str_is(panic_text(r), "out of sync");
    }
    BURROW_TRY_END;
    CHECK(got);
}

static void TestTrim(TestingT *t) {
    CHECK(str_is(textproto_trim_string(S(" \t\r\nx y\n ")), "x y"));
    CHECK(textproto_trim_string(S(" \r\n")).len == 0);
    CHECK(textproto_trim_string(BURROW_STR_EMPTY).len == 0);
    Byte b[] = " ab\t";
    Slice s = textproto_trim_bytes(slice_from(b, 4, 4, TYPE_BYTE));
    CHECK(s.len == 2 && memcmp(s.p, "ab", 2) == 0);
    CHECK(textproto_trim_bytes(slice_from(NULL, 0, 0, TYPE_BYTE)).len == 0);
}

#define TESTS(X)                                                                       \
    X(TestReadLine)                                                                    \
    X(TestReadLineLongLine)                                                            \
    X(TestReadContinuedLine)                                                           \
    X(TestReadCodeLine)                                                                \
    X(TestReadCodeLineErrors)                                                          \
    X(TestReadDotLines)                                                                \
    X(TestReadDotBytes)                                                                \
    X(TestDotReaderClose)                                                              \
    X(TestReadMIMEHeader)                                                              \
    X(TestReadMIMEHeaderSingle)                                                        \
    X(TestReaderUpcomingHeaderKeys)                                                    \
    X(TestReadMIMEHeaderNoKey)                                                         \
    X(TestLargeReadMIMEHeader)                                                         \
    X(TestReadMIMEHeaderNonCompliant)                                                  \
    X(TestReadMIMEHeaderMalformed)                                                     \
    X(TestReadMIMEHeaderErrorText)                                                     \
    X(TestReadMIMEHeaderBytes)                                                         \
    X(TestReadMIMEHeaderTrimContinued)                                                 \
    X(TestReadMIMEHeaderAllocations)                                                   \
    X(TestRFC959Lines)                                                                 \
    X(TestReadMultiLineError)                                                          \
    X(TestCommonHeaders)                                                               \
    X(TestCommonHeaderFromGoroutines)                                                  \
    X(TestCanonicalMIMEHeaderKey)                                                      \
    X(TestMIMEHeaderMultipleValues)                                                    \
    X(TestMIMEHeaderMethods)                                                           \
    X(TestPrintfLine)                                                                  \
    X(TestDotWriter)                                                                   \
    X(TestDotWriterCloseEmptyWrite)                                                    \
    X(TestDotWriterCloseNoWrite)                                                       \
    X(TestDotWriterClosedByPrintfLine)                                                 \
    X(TestConn)                                                                        \
    X(TestPipeline)                                                                    \
    X(TestTrim)
TESTING_MAIN(TESTS)

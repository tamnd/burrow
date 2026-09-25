/* Derived from Go's src/bufio/scan_test.go.
 * Go source: go1.27.1.
 *
 * Copyright 2013 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/bufio.h"

#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/mem/arena.h"
#include "burrow/strings.h"
#include "burrow/unicode.h"
#include "burrow/utf8.h"

#include "check.h"

#include <stdint.h>
#include <string.h>

#define S BURROW_S
#define LEN(a) ((Int)(sizeof(a) / sizeof((a)[0])))

enum { SMALL_MAX_TOKEN_SIZE = 256 }; /* Much smaller for more efficient testing. */

static Arena ar;
static Alloc *a;

static void setup(void) {
    arena_init(&ar, NULL, 0);
    a = arena_allocator(&ar);
}

static void teardown(void) {
    arena_free(&ar);
}

static const Type test_type = {
    {(const Byte *)"testReader", 10},
    {(const Byte *)"bufio_test", 10},
    KIND_STRUCT,
    0,
    1,
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x73636e74U,
    NULL,
};

static bool same_error(Error x, Error y) {
    return x.vt == y.vt && x.data == y.data;
}

static Str text_of(Slice b) {
    return str_from_bytes((const Byte *)b.p, b.len);
}

/* The two strings one after the other, from the arena. */
static Str cat(Str x, Str y) {
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)(x.len + y.len), 1);
    memcpy(p, x.p, (size_t)x.len);
    memcpy(p + x.len, y.p, (size_t)y.len);
    return str_from_bytes(p, x.len + y.len);
}

static Str str_c(const char *s) {
    return str_from_cstr(s);
}

static StringsReader *new_strings_reader(Str s) {
    StringsReader *r =
        (StringsReader *)mem_alloc(a, sizeof *r, _Alignof(StringsReader));
    strings_reader_reset(r, s);
    return r;
}

static IoReader strings_io(Str s) {
    return strings_reader_as_io_reader(new_strings_reader(s));
}

/* The export_test.go hook: a fresh buffer of n bytes and a limit of n. */
static void max_token_size(BufioScanner *s, Int n) {
    if (n < UTF8_UTF_MAX || n > 1000000000)
        panic_str(S("bad max token size"));
    Byte *buf = (Byte *)mem_alloc(a, (size_t)n, 1);
    bufio_scanner_buffer(s, slice_from(buf, 0, n, TYPE_BYTE), n);
}

/* Test white space table matches the Unicode definition. */
static void TestSpace(TestingT *t) {
    for (Rune r = 0; r <= UTF8_MAX_RUNE; r++) {
        if (burrow__bufio_is_space(r) != unicode_is_space(r))
            testing_t_fatalf_v(t, "white space property disagrees: %#U should be %t", r,
                               unicode_is_space(r));
    }
}

static const char *scanTests[] = {
    "",
    "a",
    "\xc2\xbc",     /* ¼ */
    "\xe2\x98\xb9", /* ☹ */
    "\x81",         /* UTF-8 error */
    "\xef\xbf\xbd", /* correctly encoded RuneError */
    "abcdefgh",
    "abc def\n\t\tgh    ",
    ("abc\xc2\xbc\xe2\x98\xb9\x81\xef\xbf\xbd\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\x82"
     "\141bc"),
};

static void TestScanByte(TestingT *t) {
    for (Int n = 0; n < LEN(scanTests); n++) {
        Str test = str_c(scanTests[n]);
        BufioScanner *s = bufio_new_scanner(a, strings_io(test));
        bufio_scanner_split(s, BUFIO_SCAN_BYTES);
        Int i;
        for (i = 0; bufio_scanner_scan(s); i++) {
            Slice b = bufio_scanner_bytes(s);
            if (b.len != 1 || ((const Byte *)b.p)[0] != test.p[i])
                testing_t_errorf_v(t, "#%d: %d: expected %q got %q", n, i, test,
                                   text_of(b));
        }
        if (i != test.len)
            testing_t_errorf_v(t, "#%d: termination expected at %d; got %d", n,
                               test.len, i);
        Error err = bufio_scanner_err(s);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "#%d: %v", n, err);
    }
}

/* Test that the rune splitter returns same sequence of runes (not bytes) as for
 * range string. */
static void TestScanRune(TestingT *t) {
    for (Int n = 0; n < LEN(scanTests); n++) {
        Str test = str_c(scanTests[n]);
        BufioScanner *s = bufio_new_scanner(a, strings_io(test));
        bufio_scanner_split(s, BUFIO_SCAN_RUNES);
        Int rune_count = 0;
        /* Use a string range loop to validate the sequence of runes. */
        for (Int i = 0, w = 0; i < test.len; i += w) {
            Rune expect =
                utf8_decode_rune(slice_from((void *)(uintptr_t)(test.p + i),
                                            test.len - i, test.len - i, TYPE_BYTE),
                                 &w);
            if (!bufio_scanner_scan(s))
                break;
            rune_count++;
            Rune got = utf8_decode_rune(bufio_scanner_bytes(s), NULL);
            if (got != expect)
                testing_t_errorf_v(t, "#%d: %d: expected %q got %q", n, i, expect, got);
        }
        if (bufio_scanner_scan(s))
            testing_t_errorf_v(t, "#%d: scan ran too long, got %q", n,
                               bufio_scanner_text(s));
        Int test_rune_count = utf8_rune_count_in_string(test);
        if (rune_count != test_rune_count)
            testing_t_errorf_v(t, "#%d: termination expected at %d; got %d", n,
                               test_rune_count, rune_count);
        Error err = bufio_scanner_err(s);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "#%d: %v", n, err);
    }
}

static const char *wordScanTests[] = {
    "",    " ",       "\n",        "a",
    " a ", "abc def", " abc def ", " abc\tdef\nghi\rjkl\fmno\vpqr\xc2\x85stu\xc2\xa0\n",
};

/* Test that the word splitter returns the same data as strings.Fields. */
static void TestScanWords(TestingT *t) {
    for (Int n = 0; n < LEN(wordScanTests); n++) {
        Str test = str_c(wordScanTests[n]);
        BufioScanner *s = bufio_new_scanner(a, strings_io(test));
        bufio_scanner_split(s, BUFIO_SCAN_WORDS);
        Slice words = strings_fields(a, test);
        const Str *w = (const Str *)words.p;
        Int word_count;
        for (word_count = 0; word_count < words.len; word_count++) {
            if (!bufio_scanner_scan(s))
                break;
            Str got = bufio_scanner_text(s);
            if (!str_eq(got, w[word_count]))
                testing_t_errorf_v(t, "#%d: %d: expected %q got %q", n, word_count,
                                   w[word_count], got);
        }
        if (bufio_scanner_scan(s))
            testing_t_errorf_v(t, "#%d: scan ran too long, got %q", n,
                               bufio_scanner_text(s));
        if (word_count != words.len)
            testing_t_errorf_v(t, "#%d: termination expected at %d; got %d", n,
                               words.len, word_count);
        Error err = bufio_scanner_err(s);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "#%d: %v", n, err);
    }
}

/* slowReader is a reader that returns only a few bytes at a time, to test the
 * incremental reads in Scanner.Scan. */
typedef struct SlowReader {
    Int max;
    IoReader buf;
} SlowReader;

static Int slow_read(void *self, Slice p, Error *err) {
    SlowReader *sr = (SlowReader *)self;
    if (p.len > sr->max) {
        p.len = sr->max;
        p.cap = sr->max;
    }
    return BURROW_CALL(sr->buf, read, p, err);
}

static const IoReaderVT slow_vt = {&test_type, slow_read};

static IoReader slow_reader(Int max, IoReader buf) {
    SlowReader *sr = (SlowReader *)mem_alloc(a, sizeof *sr, _Alignof(SlowReader));
    sr->max = max;
    sr->buf = buf;
    IoReader r = {&slow_vt, sr};
    return r;
}

/* genLine writes to buf a predictable but non-trivial line of text of length
 * n, including the terminal newline and an occasional carriage return. If
 * addNewline is false, the \r and \n are not emitted. */
static void gen_line(BytesBuffer *buf, Int line_num, Int n, bool add_newline) {
    bytes_buffer_reset(buf);
    bool do_cr = line_num % 5 == 0;
    if (do_cr)
        n--;
    for (Int i = 0; i < n - 1; i++) { /* Stop early for \n. */
        Byte c = (Byte)('a' + (Byte)(line_num + i));
        if (c == '\n' || c == '\r') /* Don't confuse us. */
            c = 'N';
        (void)bytes_buffer_write_byte(buf, c);
    }
    if (add_newline) {
        if (do_cr)
            (void)bytes_buffer_write_byte(buf, '\r');
        (void)bytes_buffer_write_byte(buf, '\n');
    }
}

/* Test the line splitter, including some carriage returns but no long lines. */
static void TestScanLongLines(TestingT *t) {
    /* Build a buffer of lots of line lengths up to but not exceeding
     * smallMaxTokenSize. */
    BytesBuffer tmp = BYTES_BUFFER(a);
    BytesBuffer *buf = bytes_new_buffer(a, slice_nil(TYPE_BYTE));
    Int line_num = 0;
    Int j = 0;
    for (Int i = 0; i < 2 * SMALL_MAX_TOKEN_SIZE; i++) {
        gen_line(&tmp, line_num, j, true);
        if (j < SMALL_MAX_TOKEN_SIZE)
            j++;
        else
            j--;
        (void)bytes_buffer_write(buf, bytes_buffer_bytes(&tmp), NULL);
        line_num++;
    }
    BufioScanner *s =
        bufio_new_scanner(a, slow_reader(1, bytes_buffer_as_io_reader(buf)));
    bufio_scanner_split(s, BUFIO_SCAN_LINES);
    max_token_size(s, SMALL_MAX_TOKEN_SIZE);
    j = 0;
    for (line_num = 0; bufio_scanner_scan(s); line_num++) {
        gen_line(&tmp, line_num, j, false);
        if (j < SMALL_MAX_TOKEN_SIZE)
            j++;
        else
            j--;
        Str line = text_of(bytes_buffer_bytes(&tmp));
        if (!str_eq(bufio_scanner_text(s), line))
            testing_t_errorf_v(t, "%d: bad line: %d %d\n%.100q\n%.100q\n", line_num,
                               bufio_scanner_bytes(s).len, line.len,
                               bufio_scanner_text(s), line);
    }
    Error err = bufio_scanner_err(s);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
}

/* Test that the line splitter errors out on a long line. */
static void TestScanLineTooLong(TestingT *t) {
    /* Build a buffer of lots of line lengths up to but not exceeding
     * smallMaxTokenSize. */
    BytesBuffer tmp = BYTES_BUFFER(a);
    BytesBuffer *buf = bytes_new_buffer(a, slice_nil(TYPE_BYTE));
    Int line_num = 0;
    Int j = 0;
    for (Int i = 0; i < 2 * SMALL_MAX_TOKEN_SIZE; i++) {
        gen_line(&tmp, line_num, j, true);
        j++;
        (void)bytes_buffer_write(buf, bytes_buffer_bytes(&tmp), NULL);
        line_num++;
    }
    BufioScanner *s =
        bufio_new_scanner(a, slow_reader(3, bytes_buffer_as_io_reader(buf)));
    bufio_scanner_split(s, BUFIO_SCAN_LINES);
    max_token_size(s, SMALL_MAX_TOKEN_SIZE);
    j = 0;
    for (line_num = 0; bufio_scanner_scan(s); line_num++) {
        gen_line(&tmp, line_num, j, false);
        if (j < SMALL_MAX_TOKEN_SIZE)
            j++;
        else
            j--;
        Str line = text_of(bytes_buffer_bytes(&tmp));
        if (!str_eq(bufio_scanner_text(s), line))
            testing_t_errorf_v(t, "%d: bad line: %d %d\n%.100q\n%.100q\n", line_num,
                               bufio_scanner_bytes(s).len, line.len,
                               bufio_scanner_text(s), line);
    }
    Error err = bufio_scanner_err(s);
    if (!same_error(err, bufio_err_too_long))
        testing_t_fatalf_v(t, "expected ErrTooLong; got %s", err);
}

/* Test that the line splitter handles a final line without a newline. */
static void test_no_newline(TestingT *t, const char *text, const char *const *lines,
                            Int nlines) {
    BufioScanner *s = bufio_new_scanner(a, slow_reader(7, strings_io(str_c(text))));
    bufio_scanner_split(s, BUFIO_SCAN_LINES);
    for (Int line_num = 0; bufio_scanner_scan(s); line_num++) {
        if (line_num >= nlines) {
            testing_t_fatalf_v(t, "%d: too many lines", line_num);
            return;
        }
        Str line = str_c(lines[line_num]);
        if (!str_eq(bufio_scanner_text(s), line))
            testing_t_errorf_v(t, "%d: bad line: %d %d\n%.100q\n%.100q\n", line_num,
                               bufio_scanner_bytes(s).len, line.len,
                               bufio_scanner_text(s), line);
    }
    Error err = bufio_scanner_err(s);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
}

/* Test that the line splitter handles a final line without a newline. */
static void TestScanLineNoNewline(TestingT *t) {
    static const char *const lines[] = {"abcdefghijklmn", "opqrstuvwxyz"};
    test_no_newline(t, "abcdefghijklmn\nopqrstuvwxyz", lines, LEN(lines));
}

/* Test that the line splitter handles a final line with a carriage return but
 * no newline. */
static void TestScanLineReturnButNoNewline(TestingT *t) {
    static const char *const lines[] = {"abcdefghijklmn", "opqrstuvwxyz"};
    test_no_newline(t, "abcdefghijklmn\nopqrstuvwxyz\r", lines, LEN(lines));
}

/* Test that the line splitter handles a final empty line. */
static void TestScanLineEmptyFinalLine(TestingT *t) {
    static const char *const lines[] = {"abcdefghijklmn", "opqrstuvwxyz", ""};
    test_no_newline(t, "abcdefghijklmn\nopqrstuvwxyz\n\n", lines, LEN(lines));
}

/* Test that the line splitter handles a final empty line with a carriage return
 * but no newline. */
static void TestScanLineEmptyFinalLineWithCR(TestingT *t) {
    static const char *const lines[] = {"abcdefghijklmn", "opqrstuvwxyz", ""};
    test_no_newline(t, "abcdefghijklmn\nopqrstuvwxyz\n\r", lines, LEN(lines));
}

static const Str test_error__text = {(const Byte *)"testError", 9};
static const Error test_error = {&burrow_sentinel_error_vt, &test_error__text};

enum { OK_COUNT = 7 };

/* Create a split function that delivers a little data, then a predictable
 * error. */
static Int error_split(void *env, Slice data, bool at_eof, Slice *token, Error *err) {
    Int *num_splits = (Int *)env;
    if (at_eof)
        panic_str(S("didn't get enough data"));
    if (*num_splits >= OK_COUNT) {
        *err = test_error;
        return 0;
    }
    (*num_splits)++;
    *token = slice_from(data.p, 1, 1, TYPE_BYTE);
    return 1;
}

/* Test the correct error is returned when the split function errors out. */
static void TestSplitError(TestingT *t) {
    Int num_splits = 0;
    /* Read the data. */
    Str text = S("abcdefghijklmnopqrstuvwxyz");
    BufioScanner *s = bufio_new_scanner(a, slow_reader(1, strings_io(text)));
    bufio_scanner_split(s, BURROW_FN(BufioSplitFunc, error_split, &num_splits));
    Int i;
    for (i = 0; bufio_scanner_scan(s); i++) {
        Slice b = bufio_scanner_bytes(s);
        if (b.len != 1 || text.p[i] != ((const Byte *)b.p)[0])
            testing_t_errorf_v(t, "#%d: expected %q got %q", i, (Rune)text.p[i],
                               (Rune)((const Byte *)b.p)[0]);
    }
    /* Check correct termination location and error. */
    if (i != OK_COUNT)
        testing_t_errorf_v(t, "unexpected termination; expected %d tokens got %d",
                           (Int)OK_COUNT, i);
    Error err = bufio_scanner_err(s);
    if (!same_error(err, test_error))
        testing_t_fatalf_v(t, "expected %q got %v", test_error, err);
}

typedef struct ErrAtEOF {
    TestingT *t;
    BufioScanner *s;
} ErrAtEOF;

/* This splitter will fail on last entry, after s.err==EOF. */
static Int err_at_eof_split(void *env, Slice data, bool at_eof, Slice *token,
                            Error *err) {
    ErrAtEOF *e = (ErrAtEOF *)env;
    TestingT *t = e->t;
    Int advance = bufio_scan_words(data, at_eof, token, err);
    if (token->len > 1) {
        if (!same_error(e->s->err, io_eof))
            testing_t_fatal_v(t, "not testing EOF");
        *err = test_error;
    }
    return advance;
}

/* Test that an EOF is overridden by a user-generated scan error. */
static void TestErrAtEOF(TestingT *t) {
    BufioScanner *s = bufio_new_scanner(a, strings_io(S("1 2 33")));
    ErrAtEOF env = {t, s};
    bufio_scanner_split(s, BURROW_FN(BufioSplitFunc, err_at_eof_split, &env));
    while (bufio_scanner_scan(s)) {
    }
    if (!same_error(bufio_scanner_err(s), test_error))
        testing_t_fatal_v(t, "wrong error:", bufio_scanner_err(s));
}

/* Test for issue 5268. */
static Int always_error_read(void *self, Slice p, Error *err) {
    (void)self;
    (void)p;
    *err = io_err_unexpected_eof;
    return 0;
}

static const IoReaderVT always_error_vt = {&test_type, always_error_read};

static void TestNonEOFWithEmptyRead(TestingT *t) {
    IoReader r = {&always_error_vt, NULL};
    BufioScanner *scanner = bufio_new_scanner(a, r);
    while (bufio_scanner_scan(scanner))
        testing_t_fatal_v(t, "read should fail");
    Error err = bufio_scanner_err(scanner);
    if (!same_error(err, io_err_unexpected_eof))
        testing_t_errorf_v(t, "unexpected error: %v", err);
}

/* Test that Scan finishes if we have endless empty reads. */
static Int endless_zeros_read(void *self, Slice p, Error *err) {
    (void)self;
    (void)p;
    (void)err;
    return 0;
}

static const IoReaderVT endless_zeros_vt = {&test_type, endless_zeros_read};

static void TestBadReader(TestingT *t) {
    IoReader r = {&endless_zeros_vt, NULL};
    BufioScanner *scanner = bufio_new_scanner(a, r);
    while (bufio_scanner_scan(scanner))
        testing_t_fatal_v(t, "read should fail");
    Error err = bufio_scanner_err(scanner);
    if (!same_error(err, io_err_no_progress))
        testing_t_errorf_v(t, "unexpected error: %v", err);
}

static void TestScanWordsExcessiveWhiteSpace(TestingT *t) {
    Str word = S("ipsum");
    Str s = cat(strings_repeat(a, S(" "), 4 * SMALL_MAX_TOKEN_SIZE), word);
    BufioScanner *scanner = bufio_new_scanner(a, strings_io(s));
    max_token_size(scanner, SMALL_MAX_TOKEN_SIZE);
    bufio_scanner_split(scanner, BUFIO_SCAN_WORDS);
    if (!bufio_scanner_scan(scanner))
        testing_t_fatalf_v(t, "scan failed: %v", bufio_scanner_err(scanner));
    Str token = bufio_scanner_text(scanner);
    if (!str_eq(token, word))
        testing_t_fatalf_v(t, "unexpected token: %v", token);
}

/* Test that empty tokens, including at end of line or end of file, are found by
 * the scanner. Issue 8672: Could miss final empty token. */
static Int comma_split(void *env, Slice data, bool at_eof, Slice *token, Error *err) {
    (void)env;
    (void)at_eof;
    const Byte *p = (const Byte *)data.p;
    for (Int i = 0; i < data.len; i++) {
        if (p[i] == ',') {
            *token = slice_from(data.p, i, i, TYPE_BYTE);
            return i + 1;
        }
    }
    *token = data;
    *err = bufio_err_final_token;
    return 0;
}

static void test_empty_tokens(TestingT *t, const char *text, const char *const *values,
                              Int nvalues) {
    BufioScanner *s = bufio_new_scanner(a, strings_io(str_c(text)));
    bufio_scanner_split(s, BURROW_FN(BufioSplitFunc, comma_split, NULL));
    Int i;
    for (i = 0; bufio_scanner_scan(s); i++) {
        if (i >= nvalues)
            testing_t_fatalf_v(t, "got %d fields, expected %d", i + 1, nvalues);
        if (!str_eq(bufio_scanner_text(s), str_c(values[i])))
            testing_t_errorf_v(t, "%d: expected %q got %q", i, values[i],
                               bufio_scanner_text(s));
    }
    if (i != nvalues)
        testing_t_fatalf_v(t, "got %d fields, expected %d", i, nvalues);
    Error err = bufio_scanner_err(s);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
}

static void TestEmptyTokens(TestingT *t) {
    static const char *const values[] = {"1", "2", "3", ""};
    test_empty_tokens(t, "1,2,3,", values, LEN(values));
}

static void TestWithNoEmptyTokens(TestingT *t) {
    static const char *const values[] = {"1", "2", "3"};
    test_empty_tokens(t, "1,2,3", values, LEN(values));
}

static Int loop_at_eof_split(void *env, Slice data, bool at_eof, Slice *token,
                             Error *err) {
    (void)env;
    (void)at_eof;
    (void)err;
    if (data.len > 0) {
        *token = slice_from(data.p, 1, 1, TYPE_BYTE);
        return 1;
    }
    *token = data;
    return 0;
}

static void TestDontLoopForever(TestingT *t) {
    BufioScanner *s = bufio_new_scanner(a, strings_io(S("abc")));
    bufio_scanner_split(s, BURROW_FN(BufioSplitFunc, loop_at_eof_split, NULL));
    /* Expect a panic */
    volatile bool panicked = false;
    volatile bool looping = false;
    BURROW_TRY {
        for (int count = 0; bufio_scanner_scan(s); count++) {
            if (count > 1000) {
                looping = true;
                break;
            }
        }
    }
    BURROW_CATCH(r) {
        (void)r;
        panicked = true;
    }
    BURROW_TRY_END;
    if (looping)
        testing_t_fatal_v(t, "looping");
    if (!panicked)
        testing_t_fatal_v(t, "should have panicked");
}

static void TestBlankLines(TestingT *t) {
    BufioScanner *s =
        bufio_new_scanner(a, strings_io(strings_repeat(a, S("\n"), 1000)));
    for (int count = 0; bufio_scanner_scan(s); count++) {
        if (count > 2000)
            testing_t_fatal_v(t, "looping");
    }
    if (BURROW_FAILED(bufio_scanner_err(s)))
        testing_t_fatal_v(t, "after scan:", bufio_scanner_err(s));
}

static Int countdown_split(void *env, Slice data, bool at_eof, Slice *token,
                           Error *err) {
    (void)at_eof;
    (void)err;
    Int *c = (Int *)env;
    if (*c > 0) {
        (*c)--;
        *token = slice_from(data.p, 1, 1, TYPE_BYTE);
        return 1;
    }
    return 0;
}

/* Check that the looping-at-EOF check doesn't trigger for merely empty tokens. */
static void TestEmptyLinesOK(TestingT *t) {
    Int c = 10000;
    BufioScanner *s =
        bufio_new_scanner(a, strings_io(strings_repeat(a, S("\n"), 10000)));
    bufio_scanner_split(s, BURROW_FN(BufioSplitFunc, countdown_split, &c));
    while (bufio_scanner_scan(s)) {
    }
    if (BURROW_FAILED(bufio_scanner_err(s)))
        testing_t_fatal_v(t, "after scan:", bufio_scanner_err(s));
    if (c != 0)
        testing_t_fatalf_v(t, "stopped with %d left to process", c);
}

/* Make sure we can read a huge token if a big enough buffer is provided. */
static void TestHugeBuffer(TestingT *t) {
    Str text = strings_repeat(a, S("x"), 2 * BUFIO_MAX_SCAN_TOKEN_SIZE);
    BufioScanner *s = bufio_new_scanner(a, strings_io(cat(text, S("\n"))));
    Byte *small = (Byte *)mem_alloc(a, 100, 1);
    bufio_scanner_buffer(s, slice_from(small, 100, 100, TYPE_BYTE),
                         3 * BUFIO_MAX_SCAN_TOKEN_SIZE);
    while (bufio_scanner_scan(s)) {
        Str token = bufio_scanner_text(s);
        if (!str_eq(token, text))
            testing_t_errorf_v(t, "scan got incorrect token of length %d", token.len);
    }
    if (BURROW_FAILED(bufio_scanner_err(s)))
        testing_t_fatal_v(t, "after scan:", bufio_scanner_err(s));
}

/* negativeEOFReader returns an invalid -1 at the end, as though it were
 * wrapping the read system call. */
static Int negative_eof_read(void *self, Slice p, Error *err) {
    Int *r = (Int *)self;
    if (*r > 0) {
        Int c = *r;
        if (c > p.len)
            c = p.len;
        Byte *q = (Byte *)p.p;
        for (Int i = 0; i < c; i++)
            q[i] = 'a';
        q[c - 1] = '\n';
        *r -= c;
        return c;
    }
    *err = io_eof;
    return -1;
}

static const IoReaderVT negative_eof_vt = {&test_type, negative_eof_read};

/* Test that the scanner doesn't panic and returns ErrBadReadCount on a reader
 * that returns a negative count of bytes read (issue 38053). */
static void TestNegativeEOFReader(TestingT *t) {
    Int left = 10;
    IoReader r = {&negative_eof_vt, &left};
    BufioScanner *scanner = bufio_new_scanner(a, r);
    int c = 0;
    while (bufio_scanner_scan(scanner)) {
        c++;
        if (c > 1) {
            testing_t_error_v(t, "read too many lines");
            break;
        }
    }
    Error got = bufio_scanner_err(scanner);
    if (!same_error(got, bufio_err_bad_read_count))
        testing_t_errorf_v(t, "scanner.Err: got %v, want %v", got,
                           bufio_err_bad_read_count);
}

/* largeReader returns an invalid count that is larger than the number of bytes
 * requested. */
static Int large_read(void *self, Slice p, Error *err) {
    (void)self;
    (void)err;
    return p.len + 1;
}

static const IoReaderVT large_vt = {&test_type, large_read};

/* Test that the scanner doesn't panic and returns ErrBadReadCount on a reader
 * that returns an impossibly large count of bytes read (issue 38053). */
static void TestLargeReader(TestingT *t) {
    IoReader r = {&large_vt, NULL};
    BufioScanner *scanner = bufio_new_scanner(a, r);
    while (bufio_scanner_scan(scanner)) {
    }
    Error got = bufio_scanner_err(scanner);
    if (!same_error(got, bufio_err_bad_read_count))
        testing_t_errorf_v(t, "scanner.Err: got %v, want %v", got,
                           bufio_err_bad_read_count);
}

/* Not in Go: Buffer and Split may not be called once scanning has begun, and a
 * scanner whose buffer came from the heap gives it back when freed. */
static void call_split(void *env) {
    bufio_scanner_split((BufioScanner *)env, BUFIO_SCAN_WORDS);
}

static void call_buffer(void *env) {
    bufio_scanner_buffer((BufioScanner *)env, slice_nil(TYPE_BYTE), 10);
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

static void TestScannerSettingsAfterScan(TestingT *t) {
    BufioScanner *s = bufio_new_scanner(heap_allocator(), strings_io(S("a\nb\n")));
    CHECK(bufio_scanner_scan(s));
    CHECK(str_eq(bufio_scanner_text(s), S("a")));
    if (!recovered(call_split, s))
        testing_t_error_v(t, "Split after Scan should have panicked");
    if (!recovered(call_buffer, s))
        testing_t_error_v(t, "Buffer after Scan should have panicked");
    CHECK(bufio_scanner_scan(s));
    CHECK(str_eq(bufio_scanner_text(s), S("b")));
    CHECK(!bufio_scanner_scan(s));
    CHECK(BURROW_OK(bufio_scanner_err(s)));
    bufio_scanner_free(s);
}

#define TESTS(X)                                                                       \
    X(TestSpace)                                                                       \
    X(TestScanByte)                                                                    \
    X(TestScanRune)                                                                    \
    X(TestScanWords)                                                                   \
    X(TestScanLongLines)                                                               \
    X(TestScanLineTooLong)                                                             \
    X(TestScanLineNoNewline)                                                           \
    X(TestScanLineReturnButNoNewline)                                                  \
    X(TestScanLineEmptyFinalLine)                                                      \
    X(TestScanLineEmptyFinalLineWithCR)                                                \
    X(TestSplitError)                                                                  \
    X(TestErrAtEOF)                                                                    \
    X(TestNonEOFWithEmptyRead)                                                         \
    X(TestBadReader)                                                                   \
    X(TestScanWordsExcessiveWhiteSpace)                                                \
    X(TestEmptyTokens)                                                                 \
    X(TestWithNoEmptyTokens)                                                           \
    X(TestDontLoopForever)                                                             \
    X(TestBlankLines)                                                                  \
    X(TestEmptyLinesOK)                                                                \
    X(TestHugeBuffer)                                                                  \
    X(TestNegativeEOFReader)                                                           \
    X(TestLargeReader)                                                                 \
    X(TestScannerSettingsAfterScan)

static int TestMain(TestingM *m) {
    setup();
    int code = testing_m_run(m);
    teardown();
    return code;
}

TESTING_MAIN_WITH(TestMain, TESTS)

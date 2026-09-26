/* Derived from Go's src/text/scanner/text_scanner_test.go and example_test.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/fixed.h"
#include "burrow/text/scanner.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Go's StringReader, which hands out its data one segment per Read. */
typedef struct SegReader {
    const Str *data;
    Int len;
    Int step;
} SegReader;

static Int seg_read(void *self, Slice p, Error *err) {
    SegReader *r = self;
    if (r->step < r->len) {
        Str s = r->data[r->step];
        Int n = s.len < p.len ? s.len : p.len;
        if (n > 0)
            memcpy(p.p, s.p, (size_t)n);
        r->step++;
        return n;
    }
    *err = io_eof;
    return 0;
}

static const IoReaderVT seg_reader_vt = {NULL, seg_read};

static IoReader seg_reader(SegReader *r) {
    IoReader rd = {&seg_reader_vt, r};
    return rd;
}

/* A reader over one Str, which most tests want. */
static IoReader str_reader(StringsReader *r, Str s) {
    strings_reader_reset(r, s);
    return strings_reader_as_io_reader(r);
}

static Str tok_name(Alloc *a, Rune tok) {
    return text_scanner_token_string(a, tok);
}

static void read_rune_segments(TestingT *t, Alloc *a, const Str *segments, Int n) {
    BytesBuffer want = BYTES_BUFFER(a);
    BytesBuffer got = BYTES_BUFFER(a);
    Error err = BURROW_NO_ERROR;
    for (Int i = 0; i < n; i++)
        bytes_buffer_write_string(&want, segments[i], &err);
    SegReader r = {segments, n, 0};
    TextScanner s = {0};
    text_scanner_init(&s, seg_reader(&r));
    for (;;) {
        Rune ch = text_scanner_next(&s);
        if (ch == TEXT_SCANNER_EOF)
            break;
        bytes_buffer_write_rune(&got, ch, &err);
    }
    Slice g = bytes_buffer_bytes(&got), w = bytes_buffer_bytes(&want);
    if (!str_eq(str_from_bytes(g.p, g.len), str_from_bytes(w.p, w.len)))
        testing_t_errorf_v(t, "segments=%d got=%s want=%s", n,
                           str_from_bytes(g.p, g.len), str_from_bytes(w.p, w.len));
    text_scanner_free(&s);
}

static void TestNext(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    static const Str s1[] = {BURROW_S_INIT("")};
    static const Str s2[] = {BURROW_S_INIT("\346\227\245"),
                             BURROW_S_INIT("\346\234\254\350\252\236")};
    static const Str s3[] = {BURROW_S_INIT("\346\227\245"),
                             BURROW_S_INIT("\346\234\254"),
                             BURROW_S_INIT("\350\252\236")};
    static const Str s4[] = {BURROW_S_INIT("\346\227\245"), BURROW_S_INIT(" "),
                             BURROW_S_INIT("\346\234\254"),
                             BURROW_S_INIT("\350\252\236")};
    static const Str s5[] = {BURROW_S_INIT("\346"), BURROW_S_INIT("\227\245\346"),
                             BURROW_S_INIT("\234\254\350\252\236")};
    static const Str s6[] = {BURROW_S_INIT("Hello"), BURROW_S_INIT(", "),
                             BURROW_S_INIT("World"), BURROW_S_INIT("!")};
    static const Str s7[] = {BURROW_S_INIT("Hello"), BURROW_S_INIT(", "),
                             BURROW_S_INIT(""), BURROW_S_INIT("World"),
                             BURROW_S_INIT("!")};
    read_rune_segments(t, a, NULL, 0);
    read_rune_segments(t, a, s1, 1);
    read_rune_segments(t, a, s2, 2);
    read_rune_segments(t, a, s3, 3);
    read_rune_segments(t, a, s4, 4);
    read_rune_segments(t, a, s5, 3);
    read_rune_segments(t, a, s6, 4);
    read_rune_segments(t, a, s7, 5);
    arena_free(&ar);
}

typedef struct Token {
    Rune tok;
    Str text;
} Token;

/* Printed from Go's tokenList by a Go program, so the bytes are the same. */
static const Token token_list[] = {
    {TEXT_SCANNER_COMMENT, BURROW_S_INIT("// line comments")},
    {TEXT_SCANNER_COMMENT, BURROW_S_INIT("//")},
    {TEXT_SCANNER_COMMENT, BURROW_S_INIT("////")},
    {TEXT_SCANNER_COMMENT, BURROW_S_INIT("// comment")},
    {TEXT_SCANNER_COMMENT, BURROW_S_INIT("// /* comment */")},
    {TEXT_SCANNER_COMMENT, BURROW_S_INIT("// // comment //")},
    {TEXT_SCANNER_COMMENT,
     BURROW_S_INIT("//"
                   "fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
                   "fffffffffffffffffffffffffffffffff")},
    {TEXT_SCANNER_COMMENT, BURROW_S_INIT("// general comments")},
    {TEXT_SCANNER_COMMENT, BURROW_S_INIT("/**/")},
    {TEXT_SCANNER_COMMENT, BURROW_S_INIT("/***/")},
    {TEXT_SCANNER_COMMENT, BURROW_S_INIT("/* comment */")},
    {TEXT_SCANNER_COMMENT, BURROW_S_INIT("/* // comment */")},
    {TEXT_SCANNER_COMMENT, BURROW_S_INIT("/* /* comment */")},
    {TEXT_SCANNER_COMMENT, BURROW_S_INIT("/*\n comment\n*/")},
    {TEXT_SCANNER_COMMENT,
     BURROW_S_INIT("/*"
                   "fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
                   "fffffffffffffffffffffffffffffffff*/")},
    {TEXT_SCANNER_COMMENT, BURROW_S_INIT("// identifiers")},
    {TEXT_SCANNER_IDENT, BURROW_S_INIT("a")},
    {TEXT_SCANNER_IDENT, BURROW_S_INIT("a0")},
    {TEXT_SCANNER_IDENT, BURROW_S_INIT("foobar")},
    {TEXT_SCANNER_IDENT, BURROW_S_INIT("abc123")},
    {TEXT_SCANNER_IDENT, BURROW_S_INIT("LGTM")},
    {TEXT_SCANNER_IDENT, BURROW_S_INIT("_")},
    {TEXT_SCANNER_IDENT, BURROW_S_INIT("_abc123")},
    {TEXT_SCANNER_IDENT, BURROW_S_INIT("abc123_")},
    {TEXT_SCANNER_IDENT, BURROW_S_INIT("_abc_123_")},
    {TEXT_SCANNER_IDENT, BURROW_S_INIT("_\303\244\303\266\303\274")},
    {TEXT_SCANNER_IDENT, BURROW_S_INIT("_\346\234\254")},
    {TEXT_SCANNER_IDENT, BURROW_S_INIT("\303\244\303\266\303\274")},
    {TEXT_SCANNER_IDENT, BURROW_S_INIT("\346\234\254")},
    {TEXT_SCANNER_IDENT, BURROW_S_INIT("a\333\260\333\261\333\270")},
    {TEXT_SCANNER_IDENT, BURROW_S_INIT("foo\340\245\254\340\245\252")},
    {TEXT_SCANNER_IDENT,
     BURROW_S_INIT("bar\357\274\231\357\274\230\357\274\227\357\274\226")},
    {TEXT_SCANNER_IDENT,
     BURROW_S_INIT("ffffffffffffffffffffffffffffffffffffffffffffffffffff"
                   "ffffffffffffffffffffffffffffffffffffffffffffffff")},
    {TEXT_SCANNER_COMMENT, BURROW_S_INIT("// decimal ints")},
    {TEXT_SCANNER_INT, BURROW_S_INIT("0")},
    {TEXT_SCANNER_INT, BURROW_S_INIT("1")},
    {TEXT_SCANNER_INT, BURROW_S_INIT("9")},
    {TEXT_SCANNER_INT, BURROW_S_INIT("42")},
    {TEXT_SCANNER_INT, BURROW_S_INIT("1234567890")},
    {TEXT_SCANNER_COMMENT, BURROW_S_INIT("// octal ints")},
    {TEXT_SCANNER_INT, BURROW_S_INIT("00")},
    {TEXT_SCANNER_INT, BURROW_S_INIT("01")},
    {TEXT_SCANNER_INT, BURROW_S_INIT("07")},
    {TEXT_SCANNER_INT, BURROW_S_INIT("042")},
    {TEXT_SCANNER_INT, BURROW_S_INIT("01234567")},
    {TEXT_SCANNER_COMMENT, BURROW_S_INIT("// hexadecimal ints")},
    {TEXT_SCANNER_INT, BURROW_S_INIT("0x0")},
    {TEXT_SCANNER_INT, BURROW_S_INIT("0x1")},
    {TEXT_SCANNER_INT, BURROW_S_INIT("0xf")},
    {TEXT_SCANNER_INT, BURROW_S_INIT("0x42")},
    {TEXT_SCANNER_INT, BURROW_S_INIT("0x123456789abcDEF")},
    {TEXT_SCANNER_INT,
     BURROW_S_INIT("0xffffffffffffffffffffffffffffffffffffffffffffffffffff"
                   "ffffffffffffffffffffffffffffffffffffffffffffffff")},
    {TEXT_SCANNER_INT, BURROW_S_INIT("0X0")},
    {TEXT_SCANNER_INT, BURROW_S_INIT("0X1")},
    {TEXT_SCANNER_INT, BURROW_S_INIT("0XF")},
    {TEXT_SCANNER_INT, BURROW_S_INIT("0X42")},
    {TEXT_SCANNER_INT, BURROW_S_INIT("0X123456789abcDEF")},
    {TEXT_SCANNER_INT,
     BURROW_S_INIT("0Xffffffffffffffffffffffffffffffffffffffffffffffffffff"
                   "ffffffffffffffffffffffffffffffffffffffffffffffff")},
    {TEXT_SCANNER_COMMENT, BURROW_S_INIT("// floats")},
    {TEXT_SCANNER_FLOAT, BURROW_S_INIT("0.")},
    {TEXT_SCANNER_FLOAT, BURROW_S_INIT("1.")},
    {TEXT_SCANNER_FLOAT, BURROW_S_INIT("42.")},
    {TEXT_SCANNER_FLOAT, BURROW_S_INIT("01234567890.")},
    {TEXT_SCANNER_FLOAT, BURROW_S_INIT(".0")},
    {TEXT_SCANNER_FLOAT, BURROW_S_INIT(".1")},
    {TEXT_SCANNER_FLOAT, BURROW_S_INIT(".42")},
    {TEXT_SCANNER_FLOAT, BURROW_S_INIT(".0123456789")},
    {TEXT_SCANNER_FLOAT, BURROW_S_INIT("0.0")},
    {TEXT_SCANNER_FLOAT, BURROW_S_INIT("1.0")},
    {TEXT_SCANNER_FLOAT, BURROW_S_INIT("42.0")},
    {TEXT_SCANNER_FLOAT, BURROW_S_INIT("01234567890.0")},
    {TEXT_SCANNER_FLOAT, BURROW_S_INIT("0e0")},
    {TEXT_SCANNER_FLOAT, BURROW_S_INIT("1e0")},
    {TEXT_SCANNER_FLOAT, BURROW_S_INIT("42e0")},
    {TEXT_SCANNER_FLOAT, BURROW_S_INIT("01234567890e0")},
    {TEXT_SCANNER_FLOAT, BURROW_S_INIT("0E0")},
    {TEXT_SCANNER_FLOAT, BURROW_S_INIT("1E0")},
    {TEXT_SCANNER_FLOAT, BURROW_S_INIT("42E0")},
    {TEXT_SCANNER_FLOAT, BURROW_S_INIT("01234567890E0")},
    {TEXT_SCANNER_FLOAT, BURROW_S_INIT("0e+10")},
    {TEXT_SCANNER_FLOAT, BURROW_S_INIT("1e-10")},
    {TEXT_SCANNER_FLOAT, BURROW_S_INIT("42e+10")},
    {TEXT_SCANNER_FLOAT, BURROW_S_INIT("01234567890e-10")},
    {TEXT_SCANNER_FLOAT, BURROW_S_INIT("0E+10")},
    {TEXT_SCANNER_FLOAT, BURROW_S_INIT("1E-10")},
    {TEXT_SCANNER_FLOAT, BURROW_S_INIT("42E+10")},
    {TEXT_SCANNER_FLOAT, BURROW_S_INIT("01234567890E-10")},
    {TEXT_SCANNER_COMMENT, BURROW_S_INIT("// chars")},
    {TEXT_SCANNER_CHAR, BURROW_S_INIT("' '")},
    {TEXT_SCANNER_CHAR, BURROW_S_INIT("'a'")},
    {TEXT_SCANNER_CHAR, BURROW_S_INIT("'\346\234\254'")},
    {TEXT_SCANNER_CHAR, BURROW_S_INIT("'\\a'")},
    {TEXT_SCANNER_CHAR, BURROW_S_INIT("'\\b'")},
    {TEXT_SCANNER_CHAR, BURROW_S_INIT("'\\f'")},
    {TEXT_SCANNER_CHAR, BURROW_S_INIT("'\\n'")},
    {TEXT_SCANNER_CHAR, BURROW_S_INIT("'\\r'")},
    {TEXT_SCANNER_CHAR, BURROW_S_INIT("'\\t'")},
    {TEXT_SCANNER_CHAR, BURROW_S_INIT("'\\v'")},
    {TEXT_SCANNER_CHAR, BURROW_S_INIT("'\\''")},
    {TEXT_SCANNER_CHAR, BURROW_S_INIT("'\\000'")},
    {TEXT_SCANNER_CHAR, BURROW_S_INIT("'\\777'")},
    {TEXT_SCANNER_CHAR, BURROW_S_INIT("'\\x00'")},
    {TEXT_SCANNER_CHAR, BURROW_S_INIT("'\\xff'")},
    {TEXT_SCANNER_CHAR, BURROW_S_INIT("'\\u0000'")},
    {TEXT_SCANNER_CHAR, BURROW_S_INIT("'\\ufA16'")},
    {TEXT_SCANNER_CHAR, BURROW_S_INIT("'\\U00000000'")},
    {TEXT_SCANNER_CHAR, BURROW_S_INIT("'\\U0000ffAB'")},
    {TEXT_SCANNER_COMMENT, BURROW_S_INIT("// strings")},
    {TEXT_SCANNER_STRING, BURROW_S_INIT("\" \"")},
    {TEXT_SCANNER_STRING, BURROW_S_INIT("\"a\"")},
    {TEXT_SCANNER_STRING, BURROW_S_INIT("\"\346\234\254\"")},
    {TEXT_SCANNER_STRING, BURROW_S_INIT("\"\\a\"")},
    {TEXT_SCANNER_STRING, BURROW_S_INIT("\"\\b\"")},
    {TEXT_SCANNER_STRING, BURROW_S_INIT("\"\\f\"")},
    {TEXT_SCANNER_STRING, BURROW_S_INIT("\"\\n\"")},
    {TEXT_SCANNER_STRING, BURROW_S_INIT("\"\\r\"")},
    {TEXT_SCANNER_STRING, BURROW_S_INIT("\"\\t\"")},
    {TEXT_SCANNER_STRING, BURROW_S_INIT("\"\\v\"")},
    {TEXT_SCANNER_STRING, BURROW_S_INIT("\"\\\"\"")},
    {TEXT_SCANNER_STRING, BURROW_S_INIT("\"\\000\"")},
    {TEXT_SCANNER_STRING, BURROW_S_INIT("\"\\777\"")},
    {TEXT_SCANNER_STRING, BURROW_S_INIT("\"\\x00\"")},
    {TEXT_SCANNER_STRING, BURROW_S_INIT("\"\\xff\"")},
    {TEXT_SCANNER_STRING, BURROW_S_INIT("\"\\u0000\"")},
    {TEXT_SCANNER_STRING, BURROW_S_INIT("\"\\ufA16\"")},
    {TEXT_SCANNER_STRING, BURROW_S_INIT("\"\\U00000000\"")},
    {TEXT_SCANNER_STRING, BURROW_S_INIT("\"\\U0000ffAB\"")},
    {TEXT_SCANNER_STRING,
     BURROW_S_INIT("\"fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
                   "fffffffffffffffffffffffffffffffffff\"")},
    {TEXT_SCANNER_COMMENT, BURROW_S_INIT("// raw strings")},
    {TEXT_SCANNER_RAW_STRING, BURROW_S_INIT("``")},
    {TEXT_SCANNER_RAW_STRING, BURROW_S_INIT("`\\`")},
    {TEXT_SCANNER_RAW_STRING, BURROW_S_INIT("`\n\n/* foobar */\n\n`")},
    {TEXT_SCANNER_RAW_STRING,
     BURROW_S_INIT("`ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
                   "ffffffffffffffffffffffffffffffffff`")},
    {TEXT_SCANNER_COMMENT, BURROW_S_INIT("// individual characters")},
    {1, BURROW_S_INIT("\001")},
    {31, BURROW_S_INIT("\037")},
    {43, BURROW_S_INIT("+")},
    {47, BURROW_S_INIT("/")},
    {46, BURROW_S_INIT(".")},
    {126, BURROW_S_INIT("~")},
    {40, BURROW_S_INIT("(")},
};

enum { n_tokens = (int)(sizeof token_list / sizeof token_list[0]) };

/* makeSource: every token's text through pattern, which has one %s. */
static IoReader make_source(BytesBuffer *buf, const char *pattern) {
    IoWriter w = bytes_buffer_as_io_writer(buf);
    for (int i = 0; i < n_tokens; i++)
        fmt_fprintf_v(w, pattern, token_list[i].text);
    return bytes_buffer_as_io_reader(buf);
}

static void check_tok(TestingT *t, Alloc *a, TextScanner *s, Int line, Rune got,
                      Rune want, Str text) {
    if (got != want) {
        testing_t_fatalf_v(t, "tok = %s, want %s for %q", tok_name(a, got),
                           tok_name(a, want), text);
        return;
    }
    if (s->position.line != line)
        testing_t_errorf_v(t, "line = %d, want %d for %q", s->position.line, line,
                           text);
    Str stext = text_scanner_token_text(s);
    if (!str_eq(stext, text)) {
        testing_t_errorf_v(t, "text = %q, want %q", stext, text);
    } else {
        /* Check the idempotency of TokenText. */
        stext = text_scanner_token_text(s);
        if (!str_eq(stext, text))
            testing_t_errorf_v(t, "text = %q, want %q (idempotency check)", stext,
                               text);
    }
}

static void check_tok_err(TestingT *t, Alloc *a, TextScanner *s, Int line, Rune want,
                          Str text) {
    Int prev = s->error_count;
    check_tok(t, a, s, line, text_scanner_scan(s), want, text);
    if (s->error_count != prev + 1)
        testing_t_fatalf_v(t, "want error for %q", text);
}

static Int count_newlines(Str s) {
    Int n = 0;
    for (Int i = 0; i < s.len; i++)
        if (s.p[i] == '\n')
            n++;
    return n;
}

static void test_scan(TestingT *t, Uint mode) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer buf = BYTES_BUFFER(a);
    TextScanner s = {0};
    text_scanner_init(&s, make_source(&buf, " \t%s\n"));
    s.mode = mode;
    Rune tok = text_scanner_scan(&s);
    Int line = 1;
    for (int i = 0; i < n_tokens; i++) {
        const Token *k = &token_list[i];
        if ((mode & TEXT_SCANNER_SKIP_COMMENTS) == 0 ||
            k->tok != TEXT_SCANNER_COMMENT) {
            check_tok(t, a, &s, line, tok, k->tok, k->text);
            tok = text_scanner_scan(&s);
        }
        line += count_newlines(k->text) + 1; /* each token is on a new line */
    }
    check_tok(t, a, &s, line, tok, TEXT_SCANNER_EOF, BURROW_S(""));
    text_scanner_free(&s);
    arena_free(&ar);
}

static void TestScan(TestingT *t) {
    test_scan(t, TEXT_SCANNER_GO_TOKENS);
    test_scan(t, TEXT_SCANNER_GO_TOKENS & ~(Uint)TEXT_SCANNER_SKIP_COMMENTS);
}

typedef struct ErrEnv {
    TestingT *t;
    Alloc *a;
    Str want; /* the message expected, for TestInvalidExponent */
    Str pos;  /* the position expected, for test_error */
    Str msg;  /* the first message seen */
    bool called;
    Str text; /* the token text at the first error */
} ErrEnv;

static void want_msg(void *env, TextScanner *s, Str msg) {
    ErrEnv *e = env;
    if (!str_eq(msg, e->want))
        testing_t_errorf_v(e->t, "%s: got error %q; want %q",
                           text_scanner_token_text(s), msg, e->want);
}

static void TestInvalidExponent(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    StringsReader r;
    TextScanner s = {0};
    text_scanner_init(&s, str_reader(&r, BURROW_S("1.5e 1.5E 1e+ 1e- 1.5z")));
    ErrEnv env = {t, a, BURROW_S("exponent has no digits"), {0}, {0}, false, {0}};
    s.error = BURROW_FN(TextScannerErrorFunc, want_msg, &env);
    check_tok_err(t, a, &s, 1, TEXT_SCANNER_FLOAT, BURROW_S("1.5e"));
    check_tok_err(t, a, &s, 1, TEXT_SCANNER_FLOAT, BURROW_S("1.5E"));
    check_tok_err(t, a, &s, 1, TEXT_SCANNER_FLOAT, BURROW_S("1e+"));
    check_tok_err(t, a, &s, 1, TEXT_SCANNER_FLOAT, BURROW_S("1e-"));
    check_tok(t, a, &s, 1, text_scanner_scan(&s), TEXT_SCANNER_FLOAT, BURROW_S("1.5"));
    check_tok(t, a, &s, 1, text_scanner_scan(&s), TEXT_SCANNER_IDENT, BURROW_S("z"));
    check_tok(t, a, &s, 1, text_scanner_scan(&s), TEXT_SCANNER_EOF, BURROW_S(""));
    if (s.error_count != 4)
        testing_t_errorf_v(t, "%d errors, want 4", s.error_count);
    text_scanner_free(&s);
    arena_free(&ar);
}

static void TestPosition(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer buf = BYTES_BUFFER(a);
    TextScanner s = {0};
    text_scanner_init(&s, make_source(&buf, "\t\t\t\t%s\n"));
    s.mode = TEXT_SCANNER_GO_TOKENS & ~(Uint)TEXT_SCANNER_SKIP_COMMENTS;
    text_scanner_scan(&s);
    TextScannerPosition pos = {{0}, 4, 1, 5};
    for (int i = 0; i < n_tokens; i++) {
        const Token *k = &token_list[i];
        if (s.position.offset != pos.offset)
            testing_t_errorf_v(t, "offset = %d, want %d for %q", s.position.offset,
                               pos.offset, k->text);
        if (s.position.line != pos.line)
            testing_t_errorf_v(t, "line = %d, want %d for %q", s.position.line,
                               pos.line, k->text);
        if (s.position.column != pos.column)
            testing_t_errorf_v(t, "column = %d, want %d for %q", s.position.column,
                               pos.column, k->text);
        pos.offset += 4 + k->text.len + 1;       /* 4 tabs + token bytes + newline */
        pos.line += count_newlines(k->text) + 1; /* each token is on a new line */
        text_scanner_scan(&s);
    }
    /* Make sure there were no token-internal errors reported by the scanner. */
    if (s.error_count != 0)
        testing_t_errorf_v(t, "%d errors", s.error_count);
    text_scanner_free(&s);
    arena_free(&ar);
}

static void TestScanZeroMode(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer buf = BYTES_BUFFER(a);
    IoReader src = make_source(&buf, "%s\n");
    Slice b = bytes_buffer_bytes(&buf);
    Str str = str_from_bytes(b.p, b.len);
    TextScanner s = {0};
    text_scanner_init(&s, src);
    s.mode = 0;       /* don't recognize any token classes */
    s.whitespace = 0; /* don't skip any whitespace */
    Rune tok = text_scanner_scan(&s);
    Int i = 0;
    for (Int off = 0; off < str.len;) {
        Int size = 0;
        Rune ch = utf8_decode_rune_in_string(str_from_bytes(str.p + off, str.len - off),
                                             &size);
        if (tok != ch) {
            testing_t_fatalf_v(t, "%d. tok = %s, want %s", i, tok_name(a, tok),
                               tok_name(a, ch));
            break;
        }
        tok = text_scanner_scan(&s);
        off += size;
        i++;
    }
    if (tok != TEXT_SCANNER_EOF)
        testing_t_fatalf_v(t, "tok = %s, want EOF", tok_name(a, tok));
    if (s.error_count != 0)
        testing_t_errorf_v(t, "%d errors", s.error_count);
    text_scanner_free(&s);
    arena_free(&ar);
}

static void test_scan_selected_mode(TestingT *t, Uint mode, Rune klass) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer buf = BYTES_BUFFER(a);
    TextScanner s = {0};
    text_scanner_init(&s, make_source(&buf, "%s\n"));
    s.mode = mode;
    Rune tok = text_scanner_scan(&s);
    while (tok != TEXT_SCANNER_EOF) {
        if (tok < 0 && tok != klass) {
            testing_t_fatalf_v(t, "tok = %s, want %s", tok_name(a, tok),
                               tok_name(a, klass));
            break;
        }
        tok = text_scanner_scan(&s);
    }
    if (s.error_count != 0)
        testing_t_errorf_v(t, "%d errors", s.error_count);
    text_scanner_free(&s);
    arena_free(&ar);
}

static void TestScanSelectedMask(TestingT *t) {
    test_scan_selected_mode(t, 0, 0);
    test_scan_selected_mode(t, TEXT_SCANNER_SCAN_IDENTS, TEXT_SCANNER_IDENT);
    /* Don't test ScanInts and ScanNumbers since some parts of the floats in
     * the source look like (invalid) octal ints and ScanNumbers may return
     * either Int or Float. */
    test_scan_selected_mode(t, TEXT_SCANNER_SCAN_CHARS, TEXT_SCANNER_CHAR);
    test_scan_selected_mode(t, TEXT_SCANNER_SCAN_STRINGS, TEXT_SCANNER_STRING);
    test_scan_selected_mode(t, TEXT_SCANNER_SKIP_COMMENTS, 0);
    test_scan_selected_mode(t, TEXT_SCANNER_SCAN_COMMENTS, TEXT_SCANNER_COMMENT);
}

/* ident = ( 'a' | 'b' ) { digit } . digit = '0' .. '3' . with a maximum
 * length of 4 */
static bool custom_ident(void *env, Rune ch, Int i) {
    (void)env;
    return (i == 0 && (ch == 'a' || ch == 'b')) ||
           (0 < i && i < 4 && '0' <= ch && ch <= '3');
}

static void TestScanCustomIdent(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    StringsReader r;
    TextScanner s = {0};
    text_scanner_init(&s, str_reader(&r, BURROW_S("faab12345 a12b123 a12 3b")));
    s.is_ident_rune = BURROW_FN(TextScannerIdentFunc, custom_ident, NULL);
    check_tok(t, a, &s, 1, text_scanner_scan(&s), 'f', BURROW_S("f"));
    check_tok(t, a, &s, 1, text_scanner_scan(&s), TEXT_SCANNER_IDENT, BURROW_S("a"));
    check_tok(t, a, &s, 1, text_scanner_scan(&s), TEXT_SCANNER_IDENT, BURROW_S("a"));
    check_tok(t, a, &s, 1, text_scanner_scan(&s), TEXT_SCANNER_IDENT, BURROW_S("b123"));
    check_tok(t, a, &s, 1, text_scanner_scan(&s), TEXT_SCANNER_INT, BURROW_S("45"));
    check_tok(t, a, &s, 1, text_scanner_scan(&s), TEXT_SCANNER_IDENT, BURROW_S("a12"));
    check_tok(t, a, &s, 1, text_scanner_scan(&s), TEXT_SCANNER_IDENT, BURROW_S("b123"));
    check_tok(t, a, &s, 1, text_scanner_scan(&s), TEXT_SCANNER_IDENT, BURROW_S("a12"));
    check_tok(t, a, &s, 1, text_scanner_scan(&s), TEXT_SCANNER_INT, BURROW_S("3"));
    check_tok(t, a, &s, 1, text_scanner_scan(&s), TEXT_SCANNER_IDENT, BURROW_S("b"));
    check_tok(t, a, &s, 1, text_scanner_scan(&s), TEXT_SCANNER_EOF, BURROW_S(""));
    text_scanner_free(&s);
    arena_free(&ar);
}

#define BOM_S "\357\273\277"

static void TestScanNext(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    StringsReader r;
    TextScanner s = {0};
    text_scanner_init(&s,
                      str_reader(&r, BURROW_S(BOM_S "if a == bcd /* com" BOM_S
                                                    "ment */ {\n\ta += c\n}" BOM_S
                                                    "// line comment ending in eof")));
    check_tok(t, a, &s, 1, text_scanner_scan(&s), TEXT_SCANNER_IDENT,
              BURROW_S("if")); /* the first BOM is ignored */
    check_tok(t, a, &s, 1, text_scanner_scan(&s), TEXT_SCANNER_IDENT, BURROW_S("a"));
    check_tok(t, a, &s, 1, text_scanner_scan(&s), '=', BURROW_S("="));
    check_tok(t, a, &s, 0, text_scanner_next(&s), '=', BURROW_S(""));
    check_tok(t, a, &s, 0, text_scanner_next(&s), ' ', BURROW_S(""));
    check_tok(t, a, &s, 0, text_scanner_next(&s), 'b', BURROW_S(""));
    check_tok(t, a, &s, 1, text_scanner_scan(&s), TEXT_SCANNER_IDENT, BURROW_S("cd"));
    check_tok(t, a, &s, 1, text_scanner_scan(&s), '{', BURROW_S("{"));
    check_tok(t, a, &s, 2, text_scanner_scan(&s), TEXT_SCANNER_IDENT, BURROW_S("a"));
    check_tok(t, a, &s, 2, text_scanner_scan(&s), '+', BURROW_S("+"));
    check_tok(t, a, &s, 0, text_scanner_next(&s), '=', BURROW_S(""));
    check_tok(t, a, &s, 2, text_scanner_scan(&s), TEXT_SCANNER_IDENT, BURROW_S("c"));
    check_tok(t, a, &s, 3, text_scanner_scan(&s), '}', BURROW_S("}"));
    check_tok(t, a, &s, 3, text_scanner_scan(&s), 0xFEFF, BURROW_S(BOM_S));
    check_tok(t, a, &s, 3, text_scanner_scan(&s), -1, BURROW_S(""));
    if (s.error_count != 0)
        testing_t_errorf_v(t, "%d errors", s.error_count);
    text_scanner_free(&s);
    arena_free(&ar);
}

static void TestScanWhitespace(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Byte src[33];
    uint64_t ws = 0;
    /* Start at 1, the NUL character is not allowed. */
    int n = 0;
    for (int ch = 1; ch < ' '; ch++) {
        src[n++] = (Byte)ch;
        ws |= (uint64_t)1 << ch;
    }
    const Rune orig = 'x';
    src[n++] = (Byte)orig;

    StringsReader r;
    TextScanner s = {0};
    text_scanner_init(&s, str_reader(&r, str_from_bytes(src, n)));
    s.mode = 0;
    s.whitespace = ws;
    Rune tok = text_scanner_scan(&s);
    if (tok != orig)
        testing_t_errorf_v(t, "tok = %s, want %s", tok_name(a, tok), tok_name(a, orig));
    text_scanner_free(&s);
    arena_free(&ar);
}

static void first_error(void *env, TextScanner *s, Str m) {
    ErrEnv *e = env;
    if (!e->called) {
        /* Only look at the first error. */
        Str p = text_scanner_position_string(text_scanner_pos(s), e->a);
        if (!str_eq(p, e->pos))
            testing_t_errorf_v(e->t, "pos = %q, want %q", p, e->pos);
        if (!str_eq(m, e->want))
            testing_t_errorf_v(e->t, "msg = %q, want %q", m, e->want);
        e->called = true;
    }
}

static void test_error(TestingT *t, Alloc *a, Str src, const char *pos, const char *msg,
                       Rune tok) {
    StringsReader r;
    TextScanner s = {0};
    text_scanner_init(&s, str_reader(&r, src));
    ErrEnv env = {t, a, str_from_cstr(msg), str_from_cstr(pos), {0}, false, {0}};
    s.error = BURROW_FN(TextScannerErrorFunc, first_error, &env);
    Rune tk = text_scanner_scan(&s);
    if (tk != tok)
        testing_t_errorf_v(t, "tok = %s, want %s for %q", tok_name(a, tk),
                           tok_name(a, tok), src);
    if (!env.called)
        testing_t_errorf_v(t, "error handler not called for %q", src);
    if (s.error_count == 0)
        testing_t_errorf_v(t, "count = %d, want > 0 for %q", s.error_count, src);
    text_scanner_free(&s);
}

#define TE(src, pos, msg, tok)                                                         \
    test_error(t, a, str_from_bytes((src), sizeof(src) - 1), pos, msg, tok)

static void TestError(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TE("\000", "<input>:1:1", "invalid character NUL", 0);
    TE("\200", "<input>:1:1", "invalid UTF-8 encoding", UTF8_RUNE_ERROR);
    TE("\377", "<input>:1:1", "invalid UTF-8 encoding", UTF8_RUNE_ERROR);

    TE("a\000", "<input>:1:2", "invalid character NUL", TEXT_SCANNER_IDENT);
    TE("ab\200", "<input>:1:3", "invalid UTF-8 encoding", TEXT_SCANNER_IDENT);
    TE("abc\377", "<input>:1:4", "invalid UTF-8 encoding", TEXT_SCANNER_IDENT);

    TE("\"a\000", "<input>:1:3", "invalid character NUL", TEXT_SCANNER_STRING);
    TE("\"ab\200", "<input>:1:4", "invalid UTF-8 encoding", TEXT_SCANNER_STRING);
    TE("\"abc\377", "<input>:1:5", "invalid UTF-8 encoding", TEXT_SCANNER_STRING);

    TE("`a\000", "<input>:1:3", "invalid character NUL", TEXT_SCANNER_RAW_STRING);
    TE("`ab\200", "<input>:1:4", "invalid UTF-8 encoding", TEXT_SCANNER_RAW_STRING);
    TE("`abc\377", "<input>:1:5", "invalid UTF-8 encoding", TEXT_SCANNER_RAW_STRING);

    TE("'\\\"'", "<input>:1:3", "invalid char escape", TEXT_SCANNER_CHAR);
    TE("\"\\'\"", "<input>:1:3", "invalid char escape", TEXT_SCANNER_STRING);

    TE("01238", "<input>:1:6", "invalid digit '8' in octal literal", TEXT_SCANNER_INT);
    TE("01238123", "<input>:1:9", "invalid digit '8' in octal literal",
       TEXT_SCANNER_INT);
    TE("0x", "<input>:1:3", "hexadecimal literal has no digits", TEXT_SCANNER_INT);
    TE("0xg", "<input>:1:3", "hexadecimal literal has no digits", TEXT_SCANNER_INT);
    TE("'aa'", "<input>:1:4", "invalid char literal", TEXT_SCANNER_CHAR);
    TE("1.5e", "<input>:1:5", "exponent has no digits", TEXT_SCANNER_FLOAT);
    TE("1.5E", "<input>:1:5", "exponent has no digits", TEXT_SCANNER_FLOAT);
    TE("1.5e+", "<input>:1:6", "exponent has no digits", TEXT_SCANNER_FLOAT);
    TE("1.5e-", "<input>:1:6", "exponent has no digits", TEXT_SCANNER_FLOAT);

    TE("'", "<input>:1:2", "literal not terminated", TEXT_SCANNER_CHAR);
    TE("'\n", "<input>:1:2", "literal not terminated", TEXT_SCANNER_CHAR);
    TE("\"abc", "<input>:1:5", "literal not terminated", TEXT_SCANNER_STRING);
    TE("\"abc\n", "<input>:1:5", "literal not terminated", TEXT_SCANNER_STRING);
    TE("`abc\n", "<input>:2:1", "literal not terminated", TEXT_SCANNER_RAW_STRING);
    TE("/*/", "<input>:1:4", "comment not terminated", TEXT_SCANNER_EOF);
    arena_free(&ar);
}

/* An errReader returns (0, err) where err is not io.EOF. */
static Int err_read(void *self, Slice p, Error *err) {
    (void)self;
    (void)p;
    *err = io_err_no_progress; /* some error that is not io.EOF */
    return 0;
}

static const IoReaderVT err_reader_vt = {NULL, err_read};

static void TestIOError(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TextScanner s = {0};
    IoReader r = {&err_reader_vt, NULL};
    text_scanner_init(&s, r);
    ErrEnv env = {t, a, error_text(io_err_no_progress), {0}, {0}, false, {0}};
    s.error = BURROW_FN(TextScannerErrorFunc, want_msg, &env);
    Rune tok = text_scanner_scan(&s);
    if (tok != TEXT_SCANNER_EOF)
        testing_t_errorf_v(t, "tok = %s, want EOF", tok_name(a, tok));
    if (s.error_count != 1)
        testing_t_errorf_v(t, "error handler called %d times, want 1", s.error_count);
    text_scanner_free(&s);
    arena_free(&ar);
}

static void check_pos(TestingT *t, TextScannerPosition got, TextScannerPosition want) {
    if (got.offset != want.offset || got.line != want.line || got.column != want.column)
        testing_t_errorf_v(t, "got offset, line, column = %d, %d, %d; want %d, %d, %d",
                           got.offset, got.line, got.column, want.offset, want.line,
                           want.column);
}

static TextScannerPosition at(Int offset, Int line, Int column) {
    TextScannerPosition p = {{0}, offset, line, column};
    return p;
}

static void check_next_pos(TestingT *t, Alloc *a, TextScanner *s, Int offset, Int line,
                           Int column, Rune ch0) {
    Rune ch = text_scanner_next(s);
    if (ch != ch0)
        testing_t_errorf_v(t, "ch = %s, want %s", tok_name(a, ch), tok_name(a, ch0));
    check_pos(t, text_scanner_pos(s), at(offset, line, column));
}

static void check_scan_pos(TestingT *t, Alloc *a, TextScanner *s, Int offset, Int line,
                           Int column, Rune ch0) {
    TextScannerPosition want = at(offset, line, column);
    check_pos(t, text_scanner_pos(s), want);
    Rune ch = text_scanner_scan(s);
    if (ch != ch0)
        testing_t_errorf_v(t, "ch = %s, want %s", tok_name(a, ch), tok_name(a, ch0));
    check_pos(t, s->position, want);
}

static void TestPos(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    StringsReader r;
    TextScanner s = {0};

    /* Corner case: empty source. */
    text_scanner_init(&s, str_reader(&r, BURROW_S("")));
    check_pos(t, text_scanner_pos(&s), at(0, 1, 1));
    text_scanner_peek(&s); /* peek doesn't affect the position */
    check_pos(t, text_scanner_pos(&s), at(0, 1, 1));

    /* Corner case: source with only a newline. */
    text_scanner_init(&s, str_reader(&r, BURROW_S("\n")));
    check_pos(t, text_scanner_pos(&s), at(0, 1, 1));
    check_next_pos(t, a, &s, 1, 2, 1, '\n');
    /* After EOF the position doesn't change. */
    for (int i = 10; i > 0; i--)
        check_scan_pos(t, a, &s, 1, 2, 1, TEXT_SCANNER_EOF);
    if (s.error_count != 0)
        testing_t_errorf_v(t, "%d errors", s.error_count);

    /* Corner case: source with only a single character. */
    text_scanner_init(&s, str_reader(&r, BURROW_S("\346\234\254")));
    check_pos(t, text_scanner_pos(&s), at(0, 1, 1));
    check_next_pos(t, a, &s, 3, 1, 2, 0x672c);
    for (int i = 10; i > 0; i--)
        check_scan_pos(t, a, &s, 3, 1, 2, TEXT_SCANNER_EOF);
    if (s.error_count != 0)
        testing_t_errorf_v(t, "%d errors", s.error_count);

    /* Positions after calling Next. */
    text_scanner_init(
        &s, str_reader(&r, BURROW_S("  foo\340\245\254\340\245\252  \n\n\346\234\254"
                                    "\350\252\236\n")));
    check_next_pos(t, a, &s, 1, 1, 2, ' ');
    text_scanner_peek(&s); /* peek doesn't affect the position */
    check_next_pos(t, a, &s, 2, 1, 3, ' ');
    check_next_pos(t, a, &s, 3, 1, 4, 'f');
    check_next_pos(t, a, &s, 4, 1, 5, 'o');
    check_next_pos(t, a, &s, 5, 1, 6, 'o');
    check_next_pos(t, a, &s, 8, 1, 7, 0x096c);
    check_next_pos(t, a, &s, 11, 1, 8, 0x096a);
    check_next_pos(t, a, &s, 12, 1, 9, ' ');
    check_next_pos(t, a, &s, 13, 1, 10, ' ');
    check_next_pos(t, a, &s, 14, 2, 1, '\n');
    check_next_pos(t, a, &s, 15, 3, 1, '\n');
    check_next_pos(t, a, &s, 18, 3, 2, 0x672c);
    check_next_pos(t, a, &s, 21, 3, 3, 0x8a9e);
    check_next_pos(t, a, &s, 22, 4, 1, '\n');
    for (int i = 10; i > 0; i--)
        check_scan_pos(t, a, &s, 22, 4, 1, TEXT_SCANNER_EOF);
    if (s.error_count != 0)
        testing_t_errorf_v(t, "%d errors", s.error_count);

    /* Positions after calling Scan. */
    text_scanner_init(&s,
                      str_reader(&r, BURROW_S("abc\n\346\234\254\350\252\236\n\nx")));
    s.mode = 0;
    s.whitespace = 0;
    check_scan_pos(t, a, &s, 0, 1, 1, 'a');
    text_scanner_peek(&s); /* peek doesn't affect the position */
    check_scan_pos(t, a, &s, 1, 1, 2, 'b');
    check_scan_pos(t, a, &s, 2, 1, 3, 'c');
    check_scan_pos(t, a, &s, 3, 1, 4, '\n');
    check_scan_pos(t, a, &s, 4, 2, 1, 0x672c);
    check_scan_pos(t, a, &s, 7, 2, 2, 0x8a9e);
    check_scan_pos(t, a, &s, 10, 2, 3, '\n');
    check_scan_pos(t, a, &s, 11, 3, 1, '\n');
    check_scan_pos(t, a, &s, 12, 4, 1, 'x');
    for (int i = 10; i > 0; i--)
        check_scan_pos(t, a, &s, 13, 4, 2, TEXT_SCANNER_EOF);
    if (s.error_count != 0)
        testing_t_errorf_v(t, "%d errors", s.error_count);
    text_scanner_free(&s);
    arena_free(&ar);
}

/* countReader counts its Reads and never has anything. */
static Int count_read(void *self, Slice p, Error *err) {
    (void)p;
    (*(int *)self)++;
    *err = io_eof;
    return 0;
}

static const IoReaderVT count_reader_vt = {NULL, count_read};

static void TestNextEOFHandling(TestingT *t) {
    int n = 0;
    IoReader r = {&count_reader_vt, &n};
    TextScanner s = {0};
    text_scanner_init(&s, r); /* corner case: empty source */
    if (text_scanner_next(&s) != TEXT_SCANNER_EOF)
        testing_t_errorf_v(t, "1) EOF not reported");
    if (text_scanner_peek(&s) != TEXT_SCANNER_EOF)
        testing_t_errorf_v(t, "2) EOF not reported");
    if (n != 1)
        testing_t_errorf_v(t, "scanner called Read %d times, not once", n);
    text_scanner_free(&s);
}

static void TestScanEOFHandling(TestingT *t) {
    int n = 0;
    IoReader r = {&count_reader_vt, &n};
    TextScanner s = {0};
    text_scanner_init(&s, r); /* corner case: empty source */
    if (text_scanner_scan(&s) != TEXT_SCANNER_EOF)
        testing_t_errorf_v(t, "1) EOF not reported");
    if (text_scanner_peek(&s) != TEXT_SCANNER_EOF)
        testing_t_errorf_v(t, "2) EOF not reported");
    if (n != 1)
        testing_t_errorf_v(t, "scanner called Read %d times, not once", n);
    text_scanner_free(&s);
}

static void text_is_quote(void *env, TextScanner *s, Str msg) {
    (void)msg;
    TestingT *t = env;
    Str got = text_scanner_token_text(s); /* this call shouldn't panic */
    if (!str_eq(got, BURROW_S("\"")))
        testing_t_errorf_v(t, "got %q; want %q", got, BURROW_S("\""));
}

static void TestIssue29723(TestingT *t) {
    StringsReader r;
    TextScanner s = {0};
    text_scanner_init(&s, str_reader(&r, BURROW_S("x \"")));
    s.error = BURROW_FN(TextScannerErrorFunc, text_is_quote, t);
    for (Rune tok = text_scanner_scan(&s); tok != TEXT_SCANNER_EOF;
         tok = text_scanner_scan(&s)) {
    }
    text_scanner_free(&s);
}

typedef struct NumberCase {
    Rune tok;
    const char *src, *tokens, *err;
} NumberCase;

/* Go's TestNumbers table, turned into C by a script. */
static const NumberCase number_cases[] = {
    {TEXT_SCANNER_INT, "0b0", "0b0", ""},
    {TEXT_SCANNER_INT, "0b1010", "0b1010", ""},
    {TEXT_SCANNER_INT, "0B1110", "0B1110", ""},
    {TEXT_SCANNER_INT, "0b", "0b", "binary literal has no digits"},
    {TEXT_SCANNER_INT, "0b0190", "0b0190", "invalid digit '9' in binary literal"},
    {TEXT_SCANNER_INT, "0b01a0", "0b01 a0", ""},
    {TEXT_SCANNER_FLOAT, "0b.", "0b.", "invalid radix point in binary literal"},
    {TEXT_SCANNER_FLOAT, "0b.1", "0b.1", "invalid radix point in binary literal"},
    {TEXT_SCANNER_FLOAT, "0b1.0", "0b1.0", "invalid radix point in binary literal"},
    {TEXT_SCANNER_FLOAT, "0b1e10", "0b1e10", "'e' exponent requires decimal mantissa"},
    {TEXT_SCANNER_FLOAT, "0b1P-1", "0b1P-1",
     "'P' exponent requires hexadecimal mantissa"},
    {TEXT_SCANNER_INT, "0o0", "0o0", ""},
    {TEXT_SCANNER_INT, "0o1234", "0o1234", ""},
    {TEXT_SCANNER_INT, "0O1234", "0O1234", ""},
    {TEXT_SCANNER_INT, "0o", "0o", "octal literal has no digits"},
    {TEXT_SCANNER_INT, "0o8123", "0o8123", "invalid digit '8' in octal literal"},
    {TEXT_SCANNER_INT, "0o1293", "0o1293", "invalid digit '9' in octal literal"},
    {TEXT_SCANNER_INT, "0o12a3", "0o12 a3", ""},
    {TEXT_SCANNER_FLOAT, "0o.", "0o.", "invalid radix point in octal literal"},
    {TEXT_SCANNER_FLOAT, "0o.2", "0o.2", "invalid radix point in octal literal"},
    {TEXT_SCANNER_FLOAT, "0o1.2", "0o1.2", "invalid radix point in octal literal"},
    {TEXT_SCANNER_FLOAT, "0o1E+2", "0o1E+2", "'E' exponent requires decimal mantissa"},
    {TEXT_SCANNER_FLOAT, "0o1p10", "0o1p10",
     "'p' exponent requires hexadecimal mantissa"},
    {TEXT_SCANNER_INT, "0", "0", ""},
    {TEXT_SCANNER_INT, "0123", "0123", ""},
    {TEXT_SCANNER_INT, "08123", "08123", "invalid digit '8' in octal literal"},
    {TEXT_SCANNER_INT, "01293", "01293", "invalid digit '9' in octal literal"},
    {TEXT_SCANNER_INT, "0F.", "0 F .", ""},
    {TEXT_SCANNER_INT, "0123F.", "0123 F .", ""},
    {TEXT_SCANNER_INT, "0123456x", "0123456 x", ""},
    {TEXT_SCANNER_INT, "1", "1", ""},
    {TEXT_SCANNER_INT, "1234", "1234", ""},
    {TEXT_SCANNER_INT, "1f", "1 f", ""},
    {TEXT_SCANNER_FLOAT, "0.", "0.", ""},
    {TEXT_SCANNER_FLOAT, "123.", "123.", ""},
    {TEXT_SCANNER_FLOAT, "0123.", "0123.", ""},
    {TEXT_SCANNER_FLOAT, ".0", ".0", ""},
    {TEXT_SCANNER_FLOAT, ".123", ".123", ""},
    {TEXT_SCANNER_FLOAT, ".0123", ".0123", ""},
    {TEXT_SCANNER_FLOAT, "0.0", "0.0", ""},
    {TEXT_SCANNER_FLOAT, "123.123", "123.123", ""},
    {TEXT_SCANNER_FLOAT, "0123.0123", "0123.0123", ""},
    {TEXT_SCANNER_FLOAT, "0e0", "0e0", ""},
    {TEXT_SCANNER_FLOAT, "123e+0", "123e+0", ""},
    {TEXT_SCANNER_FLOAT, "0123E-1", "0123E-1", ""},
    {TEXT_SCANNER_FLOAT, "0.e+1", "0.e+1", ""},
    {TEXT_SCANNER_FLOAT, "123.E-10", "123.E-10", ""},
    {TEXT_SCANNER_FLOAT, "0123.e123", "0123.e123", ""},
    {TEXT_SCANNER_FLOAT, ".0e-1", ".0e-1", ""},
    {TEXT_SCANNER_FLOAT, ".123E+10", ".123E+10", ""},
    {TEXT_SCANNER_FLOAT, ".0123E123", ".0123E123", ""},
    {TEXT_SCANNER_FLOAT, "0.0e1", "0.0e1", ""},
    {TEXT_SCANNER_FLOAT, "123.123E-10", "123.123E-10", ""},
    {TEXT_SCANNER_FLOAT, "0123.0123e+456", "0123.0123e+456", ""},
    {TEXT_SCANNER_FLOAT, "0e", "0e", "exponent has no digits"},
    {TEXT_SCANNER_FLOAT, "0E+", "0E+", "exponent has no digits"},
    {TEXT_SCANNER_FLOAT, "1e+f", "1e+ f", "exponent has no digits"},
    {TEXT_SCANNER_FLOAT, "0p0", "0p0", "'p' exponent requires hexadecimal mantissa"},
    {TEXT_SCANNER_FLOAT, "1.0P-1", "1.0P-1",
     "'P' exponent requires hexadecimal mantissa"},
    {TEXT_SCANNER_INT, "0x0", "0x0", ""},
    {TEXT_SCANNER_INT, "0x1234", "0x1234", ""},
    {TEXT_SCANNER_INT, "0xcafef00d", "0xcafef00d", ""},
    {TEXT_SCANNER_INT, "0XCAFEF00D", "0XCAFEF00D", ""},
    {TEXT_SCANNER_INT, "0x", "0x", "hexadecimal literal has no digits"},
    {TEXT_SCANNER_INT, "0x1g", "0x1 g", ""},
    {TEXT_SCANNER_FLOAT, "0x0p0", "0x0p0", ""},
    {TEXT_SCANNER_FLOAT, "0x12efp-123", "0x12efp-123", ""},
    {TEXT_SCANNER_FLOAT, "0xABCD.p+0", "0xABCD.p+0", ""},
    {TEXT_SCANNER_FLOAT, "0x.0189P-0", "0x.0189P-0", ""},
    {TEXT_SCANNER_FLOAT, "0x1.ffffp+1023", "0x1.ffffp+1023", ""},
    {TEXT_SCANNER_FLOAT, "0x.", "0x.", "hexadecimal literal has no digits"},
    {TEXT_SCANNER_FLOAT, "0x0.", "0x0.",
     "hexadecimal mantissa requires a 'p' exponent"},
    {TEXT_SCANNER_FLOAT, "0x.0", "0x.0",
     "hexadecimal mantissa requires a 'p' exponent"},
    {TEXT_SCANNER_FLOAT, "0x1.1", "0x1.1",
     "hexadecimal mantissa requires a 'p' exponent"},
    {TEXT_SCANNER_FLOAT, "0x1.1e0", "0x1.1e0",
     "hexadecimal mantissa requires a 'p' exponent"},
    {TEXT_SCANNER_FLOAT, "0x1.2gp1a", "0x1.2 gp1a",
     "hexadecimal mantissa requires a 'p' exponent"},
    {TEXT_SCANNER_FLOAT, "0x0p", "0x0p", "exponent has no digits"},
    {TEXT_SCANNER_FLOAT, "0xeP-", "0xeP-", "exponent has no digits"},
    {TEXT_SCANNER_FLOAT, "0x1234PAB", "0x1234P AB", "exponent has no digits"},
    {TEXT_SCANNER_FLOAT, "0x1.2p1a", "0x1.2p1 a", ""},
    {TEXT_SCANNER_INT, "0b_1000_0001", "0b_1000_0001", ""},
    {TEXT_SCANNER_INT, "0o_600", "0o_600", ""},
    {TEXT_SCANNER_INT, "0_466", "0_466", ""},
    {TEXT_SCANNER_INT, "1_000", "1_000", ""},
    {TEXT_SCANNER_FLOAT, "1_000.000_1", "1_000.000_1", ""},
    {TEXT_SCANNER_INT, "0x_f00d", "0x_f00d", ""},
    {TEXT_SCANNER_FLOAT, "0x_f00d.0p1_2", "0x_f00d.0p1_2", ""},
    {TEXT_SCANNER_INT, "0b__1000", "0b__1000", "'_' must separate successive digits"},
};

/* Keeps the first message. msg only lives for the call, so it is copied. */
typedef struct FirstErr {
    Alloc *a;
    Str err;
} FirstErr;

static void keep_first(void *env, TextScanner *s, Str msg) {
    (void)s;
    FirstErr *e = env;
    if (e->err.len == 0)
        e->err = strings_clone(e->a, msg);
}

static void TestNumbers(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t c = 0; c < sizeof number_cases / sizeof number_cases[0]; c++) {
        const NumberCase *test = &number_cases[c];
        Str src = str_from_cstr(test->src);
        StringsReader r;
        TextScanner s = {0};
        text_scanner_init(&s, str_reader(&r, src));
        FirstErr fe = {a, {0}};
        s.error = BURROW_FN(TextScannerErrorFunc, keep_first, &fe);

        Str rest = str_from_cstr(test->tokens);
        for (Int i = 0;; i++) {
            Int sp = strings_index_byte(rest, ' ');
            Str want = sp < 0 ? rest : str_from_bytes(rest.p, sp);
            fe.err = (Str){0};
            Rune tok = text_scanner_scan(&s);
            Str lit = str_from_bytes(text_scanner_token_text(&s).p,
                                     text_scanner_token_text(&s).len);
            /* The literal points into the scanner, so copy it before err is
             * built from anything else. */
            lit = strings_clone(a, lit);
            if (i == 0) {
                if (tok != test->tok)
                    testing_t_errorf_v(t, "%q: got token %s; want %s", src,
                                       tok_name(a, tok), tok_name(a, test->tok));
                if (!str_eq(fe.err, str_from_cstr(test->err)))
                    testing_t_errorf_v(t, "%q: got error %q; want %q", src, fe.err,
                                       str_from_cstr(test->err));
            }
            if (!str_eq(lit, want))
                testing_t_errorf_v(t, "%q: got literal %q (%s); want %s", src, lit,
                                   tok_name(a, tok), want);
            if (sp < 0)
                break;
            rest = str_from_bytes(rest.p + sp + 1, rest.len - sp - 1);
        }

        /* Make sure we read all. */
        Rune tok = text_scanner_scan(&s);
        if (tok != TEXT_SCANNER_EOF)
            testing_t_errorf_v(t, "%q: got %s; want EOF", src, tok_name(a, tok));
        text_scanner_free(&s);
    }
    arena_free(&ar);
}

static Str extract_ints(Alloc *a, Str text, Uint mode) {
    BytesBuffer res = BYTES_BUFFER(a);
    Error err = BURROW_NO_ERROR;
    StringsReader r;
    TextScanner s = {0};
    text_scanner_init(&s, str_reader(&r, text));
    s.mode = mode;
    for (;;) {
        Rune tok = text_scanner_scan(&s);
        if (tok == TEXT_SCANNER_INT || tok == TEXT_SCANNER_FLOAT) {
            if (bytes_buffer_len(&res) > 0)
                bytes_buffer_write_byte(&res, ' ');
            bytes_buffer_write_string(&res, text_scanner_token_text(&s), &err);
        } else if (tok == TEXT_SCANNER_EOF) {
            break;
        }
    }
    text_scanner_free(&s);
    Slice b = bytes_buffer_bytes(&res);
    return str_from_bytes(b.p, b.len);
}

static void TestIssue30320(TestingT *t) {
    static const struct {
        const char *in, *want;
        Uint mode;
    } tests[] = {
        {"foo01.bar31.xx-0-1-1-0", "01 31 0 1 1 0", TEXT_SCANNER_SCAN_INTS},
        {"foo0/12/0/5.67", "0 12 0 5 67", TEXT_SCANNER_SCAN_INTS},
        {"xxx1e0yyy", "1 0", TEXT_SCANNER_SCAN_INTS},
        {"1_2", "1_2", TEXT_SCANNER_SCAN_INTS},
        {"xxx1.0yyy2e3ee", "1 0 2 3", TEXT_SCANNER_SCAN_INTS},
        {"xxx1.0yyy2e3ee", "1.0 2e3", TEXT_SCANNER_SCAN_FLOATS},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Str got = extract_ints(a, str_from_cstr(tests[i].in), tests[i].mode);
        if (!str_eq(got, str_from_cstr(tests[i].want)))
            testing_t_errorf_v(t, "%q: got %q; want %q", str_from_cstr(tests[i].in),
                               got, str_from_cstr(tests[i].want));
    }
    arena_free(&ar);
}

static bool not_newline(void *env, Rune ch, Int i) {
    (void)env;
    (void)i;
    return ch != '\n';
}

static void TestIssue50909(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    StringsReader r;
    TextScanner s = {0};
    text_scanner_init(&s, str_reader(&r, BURROW_S("hello \n\nworld\n!\n")));
    s.is_ident_rune = BURROW_FN(TextScannerIdentFunc, not_newline, NULL);

    BytesBuffer res = BYTES_BUFFER(a);
    Error err = BURROW_NO_ERROR;
    int n = 0;
    while (text_scanner_scan(&s) != TEXT_SCANNER_EOF && n < 10) {
        bytes_buffer_write_string(&res, text_scanner_token_text(&s), &err);
        n++;
    }
    Slice b = bytes_buffer_bytes(&res);
    Str got = str_from_bytes(b.p, b.len);
    if (!str_eq(got, BURROW_S("hello world!")) || n != 3)
        testing_t_errorf_v(t, "got %q (n = %d); want %q (n = %d)", got, n,
                           BURROW_S("hello world!"), 3);
    text_scanner_free(&s);
    arena_free(&ar);
}

/* TokenString and Position.String, which Go only tests by using them. */
static void TestStrings(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    static const struct {
        Rune tok;
        const char *want;
    } toks[] = {
        {TEXT_SCANNER_EOF, "EOF"},
        {TEXT_SCANNER_IDENT, "Ident"},
        {TEXT_SCANNER_INT, "Int"},
        {TEXT_SCANNER_FLOAT, "Float"},
        {TEXT_SCANNER_CHAR, "Char"},
        {TEXT_SCANNER_STRING, "String"},
        {TEXT_SCANNER_RAW_STRING, "RawString"},
        {TEXT_SCANNER_COMMENT, "Comment"},
        {'a', "\"a\""},
        {'"', "\"\\\"\""},
        {0, "\"\\x00\""},
        {0x672c, "\"\346\234\254\""},
        {-9, "\"\357\277\275\""},
        {0x110000, "\"\357\277\275\""},
    };
    for (size_t i = 0; i < sizeof toks / sizeof toks[0]; i++) {
        Str got = text_scanner_token_string(a, toks[i].tok);
        if (!str_eq(got, str_from_cstr(toks[i].want)))
            testing_t_errorf_v(t, "TokenString(%d) = %q, want %q", toks[i].tok, got,
                               str_from_cstr(toks[i].want));
    }

    TextScannerPosition p = {{0}, 0, 0, 0};
    CHECK(str_eq(text_scanner_position_string(p, a), BURROW_S("<input>")));
    p.filename = BURROW_S("f.go");
    CHECK(str_eq(text_scanner_position_string(p, a), BURROW_S("f.go")));
    p.line = 3;
    p.column = 7;
    CHECK(str_eq(text_scanner_position_string(p, a), BURROW_S("f.go:3:7")));

    /* The same two through the scanner, as Go promotes them. */
    StringsReader r;
    TextScanner s = {0};
    text_scanner_init(&s, str_reader(&r, BURROW_S("  x")));
    s.position.filename = BURROW_S("f.go");
    CHECK(!text_scanner_is_valid(&s));
    CHECK(str_eq(text_scanner_string(&s, a), BURROW_S("f.go")));
    text_scanner_scan(&s);
    CHECK(text_scanner_is_valid(&s));
    CHECK(str_eq(text_scanner_string(&s, a), BURROW_S("f.go:1:3")));
    text_scanner_next(&s);
    CHECK(!text_scanner_is_valid(&s));
    text_scanner_free(&s);
    arena_free(&ar);
}

/* A token longer than the scanner's buffer is put together from tok_buf and
 * the buffer, and the text has to come out whole. */
static void TestLongToken(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Int n = 5000;
    Byte *src = mem_alloc(a, (size_t)n + 2, 1);
    src[0] = '"';
    for (Int i = 1; i <= n; i++)
        src[i] = (Byte)('a' + i % 26);
    src[n + 1] = '"';
    Str want = str_from_bytes(src, n + 2);
    StringsReader r;
    TextScanner s = {0};
    s.a = a;
    text_scanner_init(&s, str_reader(&r, want));
    Rune tok = text_scanner_scan(&s);
    CHECK_INT_EQ(tok, TEXT_SCANNER_STRING);
    Str got = text_scanner_token_text(&s);
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "long string came back as %d bytes, want %d", got.len,
                           want.len);
    CHECK(str_eq(text_scanner_token_text(&s), want));
    CHECK_INT_EQ(text_scanner_scan(&s), TEXT_SCANNER_EOF);
    CHECK_INT_EQ(s.error_count, 0);
    text_scanner_free(&s);
    arena_free(&ar);
}

/* With no room to keep a long token's head, the scanner says so as an error
 * and goes on. */
static void TestOutOfMemory(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Int n = 3000;
    Byte *src = mem_alloc(a, (size_t)n, 1);
    for (Int i = 0; i < n; i++)
        src[i] = 'x';
    Byte mem[16];
    Fixed fx;
    fixed_init(&fx, mem, sizeof mem);
    StringsReader r;
    TextScanner s = {0};
    s.a = fixed_allocator(&fx);
    text_scanner_init(&s, str_reader(&r, str_from_bytes(src, n)));
    FirstErr fe = {a, {0}};
    s.error = BURROW_FN(TextScannerErrorFunc, keep_first, &fe);
    CHECK_INT_EQ(text_scanner_scan(&s), TEXT_SCANNER_IDENT);
    if (!str_eq(fe.err, error_text(burrow_err_out_of_memory)))
        testing_t_errorf_v(t, "error = %q, want %q", fe.err,
                           error_text(burrow_err_out_of_memory));
    CHECK(s.error_count > 0);
    CHECK_INT_EQ(text_scanner_scan(&s), TEXT_SCANNER_EOF);
    text_scanner_free(&s);
    arena_free(&ar);
}

/* A scanner with no error function reports to stderr and keeps count. */
static void TestDefaultError(TestingT *t) {
    StringsReader r;
    TextScanner s = {0};
    text_scanner_init(&s, str_reader(&r, BURROW_S("\"x")));
    s.position.filename = BURROW_S("default-error-test");
    CHECK_INT_EQ(text_scanner_scan(&s), TEXT_SCANNER_STRING);
    CHECK_INT_EQ(s.error_count, 1);
    text_scanner_free(&s);
}

/* The examples from example_test.go, printing to a buffer. */
static Str print_tokens(Alloc *a, TextScanner *s, BytesBuffer *out) {
    IoWriter w = bytes_buffer_as_io_writer(out);
    for (Rune tok = text_scanner_scan(s); tok != TEXT_SCANNER_EOF;
         tok = text_scanner_scan(s))
        fmt_fprintf_v(w, "%s: %s\n", text_scanner_position_string(s->position, a),
                      text_scanner_token_text(s));
    Slice b = bytes_buffer_bytes(out);
    return str_from_bytes(b.p, b.len);
}

static void check_output(TestingT *t, Str got, const char *want) {
    if (!str_eq(got, str_from_cstr(want)))
        testing_t_errorf_v(t, "got:\n%s\nwant:\n%s", got, str_from_cstr(want));
}

static void TestExample(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    StringsReader r;
    TextScanner s = {0};
    text_scanner_init(
        &s, str_reader(&r, BURROW_S("\n// This is scanned code.\nif a > 10 {\n\t"
                                    "someParsable = text\n}")));
    s.position.filename = BURROW_S("example");
    BytesBuffer out = BYTES_BUFFER(a);
    check_output(t, print_tokens(a, &s, &out),
                 "example:3:1: if\n"
                 "example:3:4: a\n"
                 "example:3:6: >\n"
                 "example:3:8: 10\n"
                 "example:3:11: {\n"
                 "example:4:2: someParsable\n"
                 "example:4:15: =\n"
                 "example:4:17: text\n"
                 "example:5:1: }\n");
    text_scanner_free(&s);
    arena_free(&ar);
}

/* Treat a leading '%' as part of an identifier. */
static bool percent_ident(void *env, Rune ch, Int i) {
    (void)env;
    return (ch == '%' && i == 0) || unicode_is_letter(ch) ||
           (unicode_is_digit(ch) && i > 0);
}

static void TestExampleIsIdentRune(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str src = BURROW_S("%var1 var2%");
    StringsReader r;
    TextScanner s = {0};
    BytesBuffer out = BYTES_BUFFER(a);

    text_scanner_init(&s, str_reader(&r, src));
    s.position.filename = BURROW_S("default");
    print_tokens(a, &s, &out);
    bytes_buffer_write_byte(&out, '\n');

    text_scanner_init(&s, str_reader(&r, src));
    s.position.filename = BURROW_S("percent");
    s.is_ident_rune = BURROW_FN(TextScannerIdentFunc, percent_ident, NULL);
    check_output(t, print_tokens(a, &s, &out),
                 "default:1:1: %\n"
                 "default:1:2: var1\n"
                 "default:1:7: var2\n"
                 "default:1:11: %\n"
                 "\n"
                 "percent:1:1: %var1\n"
                 "percent:1:7: var2\n"
                 "percent:1:11: %\n");
    text_scanner_free(&s);
    arena_free(&ar);
}

static void TestExampleMode(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    StringsReader r;
    TextScanner s = {0};
    text_scanner_init(
        &s, str_reader(&r, BURROW_S("\n    // Comment begins at column 5.\n\nThis line "
                                    "should not be included in the output.\n\n/*\nThis "
                                    "multiline comment\nshould be extracted in\nits "
                                    "entirety.\n*/\n")));
    s.position.filename = BURROW_S("comments");
    s.mode ^= TEXT_SCANNER_SKIP_COMMENTS; /* don't skip comments */
    BytesBuffer out = BYTES_BUFFER(a);
    IoWriter w = bytes_buffer_as_io_writer(&out);
    for (Rune tok = text_scanner_scan(&s); tok != TEXT_SCANNER_EOF;
         tok = text_scanner_scan(&s)) {
        Str txt = text_scanner_token_text(&s);
        if (strings_has_prefix(txt, BURROW_S("//")) ||
            strings_has_prefix(txt, BURROW_S("/*")))
            fmt_fprintf_v(w, "%s: %s\n", text_scanner_position_string(s.position, a),
                          txt);
    }
    Slice b = bytes_buffer_bytes(&out);
    check_output(t, str_from_bytes(b.p, b.len),
                 "comments:2:5: // Comment begins at column 5.\n"
                 "comments:6:1: /*\n"
                 "This multiline comment\n"
                 "should be extracted in\n"
                 "its entirety.\n"
                 "*/\n");
    text_scanner_free(&s);
    arena_free(&ar);
}

static void TestExampleWhitespace(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    /* Tab separated values. */
    StringsReader r;
    TextScanner s = {0};
    text_scanner_init(
        &s, str_reader(&r, BURROW_S("aa\tab\tac\tad\nba\tbb\tbc\tbd\nca\tcb\tcc\tcd\n"
                                    "da\tdb\tdc\tdd")));
    s.whitespace ^=
        (uint64_t)1 << '\t' | (uint64_t)1 << '\n'; /* don't skip tabs and newlines */
    Str tsv[4][4] = {{{0}}};
    int col = 0, row = 0;
    for (Rune tok = text_scanner_scan(&s); tok != TEXT_SCANNER_EOF;
         tok = text_scanner_scan(&s)) {
        switch (tok) {
        case '\n':
            row++;
            col = 0;
            break;
        case '\t':
            col++;
            break;
        default:
            tsv[row][col] = strings_clone(a, text_scanner_token_text(&s));
            break;
        }
    }
    BytesBuffer out = BYTES_BUFFER(a);
    IoWriter w = bytes_buffer_as_io_writer(&out);
    fmt_fprintf_v(w, "%s %s %s %s|%s %s %s %s|%s %s %s %s|%s %s %s %s", tsv[0][0],
                  tsv[0][1], tsv[0][2], tsv[0][3], tsv[1][0], tsv[1][1], tsv[1][2],
                  tsv[1][3], tsv[2][0], tsv[2][1], tsv[2][2], tsv[2][3], tsv[3][0],
                  tsv[3][1], tsv[3][2], tsv[3][3]);
    Slice b = bytes_buffer_bytes(&out);
    check_output(t, str_from_bytes(b.p, b.len),
                 "aa ab ac ad|ba bb bc bd|ca cb cc cd|da db dc dd");
    text_scanner_free(&s);
    arena_free(&ar);
}

/* Scanning Go's own token list, repeated, through a reader. */
typedef struct BenchEnv {
    Str src;
    Uint mode;
} BenchEnv;

static void bench_scan(void *env, TestingB *b) {
    BenchEnv *e = env;
    testing_b_set_bytes(b, e->src.len);
    testing_b_report_allocs(b);
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++) {
        StringsReader r;
        TextScanner s = {0};
        text_scanner_init(&s, str_reader(&r, e->src));
        s.mode = e->mode;
        while (text_scanner_scan(&s) != TEXT_SCANNER_EOF) {
        }
        text_scanner_free(&s);
    }
}

static void BenchmarkScan(TestingB *b) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer buf = BYTES_BUFFER(a);
    for (int i = 0; i < 20; i++)
        make_source(&buf, "\t%s\n");
    Slice p = bytes_buffer_bytes(&buf);
    BenchEnv tokens = {str_from_bytes(p.p, p.len), TEXT_SCANNER_GO_TOKENS};
    BenchEnv chars = {tokens.src, 0};
    testing_b_run(b, BURROW_S("GoTokens"),
                  BURROW_FN(TestingBFunc, bench_scan, &tokens));
    testing_b_run(b, BURROW_S("Chars"), BURROW_FN(TestingBFunc, bench_scan, &chars));
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestNext)                                                                        \
    X(TestScan)                                                                        \
    X(TestInvalidExponent)                                                             \
    X(TestPosition)                                                                    \
    X(TestScanZeroMode)                                                                \
    X(TestScanSelectedMask)                                                            \
    X(TestScanCustomIdent)                                                             \
    X(TestScanNext)                                                                    \
    X(TestScanWhitespace)                                                              \
    X(TestError)                                                                       \
    X(TestIOError)                                                                     \
    X(TestPos)                                                                         \
    X(TestNextEOFHandling)                                                             \
    X(TestScanEOFHandling)                                                             \
    X(TestIssue29723)                                                                  \
    X(TestNumbers)                                                                     \
    X(TestIssue30320)                                                                  \
    X(TestIssue50909)                                                                  \
    X(TestStrings)                                                                     \
    X(TestLongToken)                                                                   \
    X(TestOutOfMemory)                                                                 \
    X(TestDefaultError)                                                                \
    X(TestExample)                                                                     \
    X(TestExampleIsIdentRune)                                                          \
    X(TestExampleMode)                                                                 \
    X(TestExampleWhitespace)                                                           \
    X(BenchmarkScan)

TESTING_MAIN(TESTS)

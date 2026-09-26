/* Derived from Go's src/text/tabwriter/tabwriter_test.go and example_test.go.
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
#include "burrow/text/tabwriter.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Go's buffer: a fixed capacity writer that panics when it runs out. */
typedef struct Buffer {
    Byte a[1000];
    Int n;
} Buffer;

static Int buffer_write(void *self, Slice p, Error *err) {
    (void)err;
    Buffer *b = self;
    if (b->n + p.len > (Int)sizeof b->a)
        panic_str(BURROW_S("buffer.Write: buffer too small"));
    if (p.len > 0)
        memcpy(b->a + b->n, p.p, (size_t)p.len);
    b->n += p.len;
    return p.len;
}

static const IoWriterVT buffer_vt = {NULL, buffer_write};

static IoWriter buffer_writer(Buffer *b) {
    IoWriter w = {&buffer_vt, b};
    return w;
}

static Str buffer_string(Buffer *b) {
    return (Str){b->a, b->n};
}

static void want_str(TestingT *t, Str got, Str want) {
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "got %q, want %q", got, want);
}

static Slice bytes_of(Str s) {
    return slice_from((void *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE);
}

static void write_str(TestingT *t, Str testname, TabwriterWriter *w, Str src) {
    Error err = BURROW_NO_ERROR;
    Int written = io_write_string(tabwriter_writer_as_io_writer(w), src, &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "--- test: %s\n--- src:\n%q\n--- write error: %v\n",
                           testname, src, err);
    if (written != src.len)
        testing_t_errorf_v(
            t, "--- test: %s\n--- src:\n%q\n--- written = %d, len(src) = %d\n",
            testname, src, written, src.len);
}

static void verify(TestingT *t, Str testname, TabwriterWriter *w, Buffer *b, Str src,
                   Str expected) {
    Error err = tabwriter_writer_flush(w);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "--- test: %s\n--- src:\n%q\n--- flush error: %v\n",
                           testname, src, err);

    Str res = buffer_string(b);
    if (!str_eq(res, expected))
        testing_t_errorf_v(
            t, "--- test: %s\n--- src:\n%q\n--- found:\n%q\n--- expected:\n%q\n",
            testname, src, res, expected);
}

typedef struct Case {
    const char *testname;
    Int minwidth, tabwidth, padding;
    Byte padchar;
    Uint flags;
    Str src, expected;
} Case;

static void check(TestingT *t, Alloc *a, const Case *e) {
    Buffer b = {{0}, 0};
    TabwriterWriter w;
    memset(&w, 0, sizeof w);
    tabwriter_writer_init(&w, buffer_writer(&b), e->minwidth, e->tabwidth, e->padding,
                          e->padchar, e->flags);
    Str src = e->src;
    Str name = str_from_cstr(e->testname);

    /* write all at once */
    Str title = fmt_sprintf_v(a, "%s (written all at once)", name);
    b.n = 0;
    write_str(t, title, &w, src);
    verify(t, title, &w, &b, src, e->expected);

    /* write byte-by-byte */
    title = fmt_sprintf_v(a, "%s (written byte-by-byte)", name);
    b.n = 0;
    for (Int i = 0; i < src.len; i++)
        write_str(t, title, &w, str_from_bytes(src.p + i, 1));
    verify(t, title, &w, &b, src, e->expected);

    /* write using Fibonacci slice sizes */
    title = fmt_sprintf_v(a, "%s (written in fibonacci slices)", name);
    b.n = 0;
    for (Int i = 0, d = 0; i < src.len;) {
        write_str(t, title, &w, str_from_bytes(src.p + i, d));
        i = i + d;
        d = d + 1;
        if (i + d > src.len)
            d = src.len - i;
    }
    verify(t, title, &w, &b, src, e->expected);

    tabwriter_writer_free(&w);
}

static const Case cases[] = {
    {"1a", 8, 0, 1, '.', 0, BURROW_S_INIT(""), BURROW_S_INIT("")},
    {"1a debug", 8, 0, 1, '.', TABWRITER_DEBUG, BURROW_S_INIT(""), BURROW_S_INIT("")},
    {"1b esc stripped", 8, 0, 1, '.', TABWRITER_STRIP_ESCAPE, BURROW_S_INIT("\377\377"),
     BURROW_S_INIT("")},
    {"1b esc", 8, 0, 1, '.', 0, BURROW_S_INIT("\377\377"), BURROW_S_INIT("\377\377")},
    {"1c esc stripped", 8, 0, 1, '.', TABWRITER_STRIP_ESCAPE,
     BURROW_S_INIT("\377\t\377"), BURROW_S_INIT("\t")},
    {"1c esc", 8, 0, 1, '.', 0, BURROW_S_INIT("\377\t\377"),
     BURROW_S_INIT("\377\t\377")},
    {"1d esc stripped", 8, 0, 1, '.', TABWRITER_STRIP_ESCAPE,
     BURROW_S_INIT("\377\"foo\t\n"
                   "\tbar\"\377"),
     BURROW_S_INIT("\"foo\t\n"
                   "\tbar\"")},
    {"1d esc", 8, 0, 1, '.', 0,
     BURROW_S_INIT("\377\"foo\t\n"
                   "\tbar\"\377"),
     BURROW_S_INIT("\377\"foo\t\n"
                   "\tbar\"\377")},
    {"1e esc stripped", 8, 0, 1, '.', TABWRITER_STRIP_ESCAPE,
     BURROW_S_INIT("abc\377\tdef"), BURROW_S_INIT("abc\tdef")},
    {"1e esc", 8, 0, 1, '.', 0, BURROW_S_INIT("abc\377\tdef"),
     BURROW_S_INIT("abc\377\tdef")},
    {"2", 8, 0, 1, '.', 0,
     BURROW_S_INIT("\n"
                   "\n"
                   "\n"),
     BURROW_S_INIT("\n"
                   "\n"
                   "\n")},
    {"3", 8, 0, 1, '.', 0,
     BURROW_S_INIT("a\n"
                   "b\n"
                   "c"),
     BURROW_S_INIT("a\n"
                   "b\n"
                   "c")},
    {"4a", 8, 0, 1, '.', 0, BURROW_S_INIT("\t"), BURROW_S_INIT("")},
    {"4b", 8, 0, 1, '.', TABWRITER_ALIGN_RIGHT, BURROW_S_INIT("\t"), BURROW_S_INIT("")},
    {"5", 8, 0, 1, '.', 0, BURROW_S_INIT("*\t*"), BURROW_S_INIT("*.......*")},
    {"5b", 8, 0, 1, '.', 0, BURROW_S_INIT("*\t*\n"), BURROW_S_INIT("*.......*\n")},
    {"5c", 8, 0, 1, '.', 0, BURROW_S_INIT("*\t*\t"), BURROW_S_INIT("*.......*")},
    {"5c debug", 8, 0, 1, '.', TABWRITER_DEBUG, BURROW_S_INIT("*\t*\t"),
     BURROW_S_INIT("*.......|*")},
    {"5d", 8, 0, 1, '.', TABWRITER_ALIGN_RIGHT, BURROW_S_INIT("*\t*\t"),
     BURROW_S_INIT(".......**")},
    {"6", 8, 0, 1, '.', 0, BURROW_S_INIT("\t\n"), BURROW_S_INIT("........\n")},
    {"7a", 8, 0, 1, '.', 0, BURROW_S_INIT("a) foo"), BURROW_S_INIT("a) foo")},
    {"7b", 8, 0, 1, ' ', 0, BURROW_S_INIT("b) foo\tbar"), BURROW_S_INIT("b) foo  bar")},
    {"7c", 8, 0, 1, '.', 0, BURROW_S_INIT("c) foo\tbar\t"),
     BURROW_S_INIT("c) foo..bar")},
    {"7d", 8, 0, 1, '.', 0, BURROW_S_INIT("d) foo\tbar\n"),
     BURROW_S_INIT("d) foo..bar\n")},
    {"7e", 8, 0, 1, '.', 0, BURROW_S_INIT("e) foo\tbar\t\n"),
     BURROW_S_INIT("e) foo..bar.....\n")},
    {"7f", 8, 0, 1, '.', TABWRITER_FILTER_HTML,
     BURROW_S_INIT("f) f&lt;o\t<b>bar</b>\t\n"),
     BURROW_S_INIT("f) f&lt;o..<b>bar</b>.....\n")},
    {"7g", 8, 0, 1, '.', TABWRITER_FILTER_HTML,
     BURROW_S_INIT("g) f&lt;o\t<b>bar</b>\t non-terminated entity &amp"),
     BURROW_S_INIT("g) f&lt;o..<b>bar</b>..... non-terminated entity &amp")},
    {"7g debug", 8, 0, 1, '.', TABWRITER_FILTER_HTML | TABWRITER_DEBUG,
     BURROW_S_INIT("g) f&lt;o\t<b>bar</b>\t non-terminated entity &amp"),
     BURROW_S_INIT("g) f&lt;o..|<b>bar</b>.....| non-terminated entity &amp")},
    {"8", 8, 0, 1, '*', 0, BURROW_S_INIT("Hello, world!\n"),
     BURROW_S_INIT("Hello, world!\n")},
    {"9a", 1, 0, 0, '.', 0,
     BURROW_S_INIT("1\t2\t3\t4\n"
                   "11\t222\t3333\t44444\n"),
     BURROW_S_INIT("1.2..3...4\n"
                   "11222333344444\n")},
    {"9b", 1, 0, 0, '.', TABWRITER_FILTER_HTML,
     BURROW_S_INIT("1\t2<!---\f--->\t3\t4\n"
                   "11\t222\t3333\t44444\n"),
     BURROW_S_INIT("1.2<!---\f--->..3...4\n"
                   "11222333344444\n")},
    {"9c", 1, 0, 0, '.', 0, BURROW_S_INIT("1\t2\t3\t4\f11\t222\t3333\t44444\n"),
     BURROW_S_INIT("1234\n"
                   "11222333344444\n")},
    {"9c debug", 1, 0, 0, '.', TABWRITER_DEBUG,
     BURROW_S_INIT("1\t2\t3\t4\f11\t222\t3333\t44444\n"),
     BURROW_S_INIT("1|2|3|4\n"
                   "---\n"
                   "11|222|3333|44444\n")},
    {"10a", 5, 0, 0, '.', 0, BURROW_S_INIT("1\t2\t3\t4\n"),
     BURROW_S_INIT("1....2....3....4\n")},
    {"10b", 5, 0, 0, '.', 0, BURROW_S_INIT("1\t2\t3\t4\t\n"),
     BURROW_S_INIT("1....2....3....4....\n")},
    {"11", 8, 0, 1, '.', 0,
     BURROW_S_INIT("\346\234\254\tb\tc\n"
                   "aa\t\346\234\254\346\234\254\346\234\254\tcccc\tddddd\n"
                   "aaa\tbbbb\n"),
     BURROW_S_INIT("\346\234\254.......b.......c\n"
                   "aa......\346\234\254\346\234\254\346\234\254.....cccc....ddddd\n"
                   "aaa.....bbbb\n")},
    {"12a", 8, 0, 1, ' ', TABWRITER_ALIGN_RIGHT,
     BURROW_S_INIT("a\t\303\250\tc\t\n"
                   "aa\t\303\250\303\250\303\250\tcccc\tddddd\t\n"
                   "aaa\t\303\250\303\250\303\250\303\250\t\n"),
     BURROW_S_INIT("       a       \303\250       c\n"
                   "      aa     \303\250\303\250\303\250    cccc   ddddd\n"
                   "     aaa    \303\250\303\250\303\250\303\250\n")},
    {"12b", 2, 0, 0, ' ', 0,
     BURROW_S_INIT("a\tb\tc\n"
                   "aa\tbbb\tcccc\n"
                   "aaa\tbbbb\n"),
     BURROW_S_INIT("a  b  c\n"
                   "aa bbbcccc\n"
                   "aaabbbb\n")},
    {"12c", 8, 0, 1, '_', 0,
     BURROW_S_INIT("a\tb\tc\n"
                   "aa\tbbb\tcccc\n"
                   "aaa\tbbbb\n"),
     BURROW_S_INIT("a_______b_______c\n"
                   "aa______bbb_____cccc\n"
                   "aaa_____bbbb\n")},
    {"13a", 4, 0, 1, '-', 0,
     BURROW_S_INIT("4444\t\346\227\245\346\234\254\350\252\236\t22\t1\t333\n"
                   "999999999\t22\n"
                   "7\t22\n"
                   "\t\t\t88888888\n"
                   "\n"
                   "666666\t666666\t666666\t4444\n"
                   "1\t1\t999999999\t0000000000\n"),
     BURROW_S_INIT("4444------\346\227\245\346\234\254\350\252\236-22--1---333\n"
                   "999999999-22\n"
                   "7---------22\n"
                   "------------------88888888\n"
                   "\n"
                   "666666-666666-666666----4444\n"
                   "1------1------999999999-0000000000\n")},
    {"13b", 4, 0, 3, '.', 0,
     BURROW_S_INIT("4444\t333\t22\t1\t333\n"
                   "999999999\t22\n"
                   "7\t22\n"
                   "\t\t\t88888888\n"
                   "\n"
                   "666666\t666666\t666666\t4444\n"
                   "1\t1\t999999999\t0000000000\n"),
     BURROW_S_INIT("4444........333...22...1...333\n"
                   "999999999...22\n"
                   "7...........22\n"
                   "....................88888888\n"
                   "\n"
                   "666666...666666...666666......4444\n"
                   "1........1........999999999...0000000000\n")},
    {"13c", 8, 8, 1, '\t', TABWRITER_FILTER_HTML,
     BURROW_S_INIT(
         "4444\t333\t22\t1\t333\n"
         "999999999\t22\n"
         "7\t22\n"
         "\t\t\t88888888\n"
         "\n"
         "666666\t666666\t666666\t4444\n"
         "1\t1\t<font color=red "
         "attr=\346\227\245\346\234\254\350\252\236>999999999</font>\t0000000000\n"),
     BURROW_S_INIT(
         "4444\t\t333\t22\t1\t333\n"
         "999999999\t22\n"
         "7\t\t22\n"
         "\t\t\t\t88888888\n"
         "\n"
         "666666\t666666\t666666\t\t4444\n"
         "1\t1\t<font color=red "
         "attr=\346\227\245\346\234\254\350\252\236>999999999</font>\t0000000000\n")},
    {"14", 1, 0, 2, ' ', TABWRITER_ALIGN_RIGHT,
     BURROW_S_INIT(".0\t.3\t2.4\t-5.1\t\n"
                   "23.0\t12345678.9\t2.4\t-989.4\t\n"
                   "5.1\t12.0\t2.4\t-7.0\t\n"
                   ".0\t0.0\t332.0\t8908.0\t\n"
                   ".0\t-.3\t456.4\t22.1\t\n"
                   ".0\t1.2\t44.4\t-13.3\t\t"),
     BURROW_S_INIT("    .0          .3    2.4    -5.1\n"
                   "  23.0  12345678.9    2.4  -989.4\n"
                   "   5.1        12.0    2.4    -7.0\n"
                   "    .0         0.0  332.0  8908.0\n"
                   "    .0         -.3  456.4    22.1\n"
                   "    .0         1.2   44.4   -13.3")},
    {"14 debug", 1, 0, 2, ' ', TABWRITER_ALIGN_RIGHT | TABWRITER_DEBUG,
     BURROW_S_INIT(".0\t.3\t2.4\t-5.1\t\n"
                   "23.0\t12345678.9\t2.4\t-989.4\t\n"
                   "5.1\t12.0\t2.4\t-7.0\t\n"
                   ".0\t0.0\t332.0\t8908.0\t\n"
                   ".0\t-.3\t456.4\t22.1\t\n"
                   ".0\t1.2\t44.4\t-13.3\t\t"),
     BURROW_S_INIT("    .0|          .3|    2.4|    -5.1|\n"
                   "  23.0|  12345678.9|    2.4|  -989.4|\n"
                   "   5.1|        12.0|    2.4|    -7.0|\n"
                   "    .0|         0.0|  332.0|  8908.0|\n"
                   "    .0|         -.3|  456.4|    22.1|\n"
                   "    .0|         1.2|   44.4|   -13.3|")},
    {"15a", 4, 0, 0, '.', 0, BURROW_S_INIT("a\t\tb"), BURROW_S_INIT("a.......b")},
    {"15b", 4, 0, 0, '.', TABWRITER_DISCARD_EMPTY_COLUMNS, BURROW_S_INIT("a\t\tb"),
     BURROW_S_INIT("a.......b")},
    {"15c", 4, 0, 0, '.', TABWRITER_DISCARD_EMPTY_COLUMNS, BURROW_S_INIT("a\v\vb"),
     BURROW_S_INIT("a...b")},
    {"15d", 4, 0, 0, '.', TABWRITER_ALIGN_RIGHT | TABWRITER_DISCARD_EMPTY_COLUMNS,
     BURROW_S_INIT("a\v\vb"), BURROW_S_INIT("...ab")},
    {"16a", 100, 100, 0, '\t', 0,
     BURROW_S_INIT("a\tb\t\td\n"
                   "a\tb\t\td\te\n"
                   "a\n"
                   "a\tb\tc\td\n"
                   "a\tb\tc\td\te\n"),
     BURROW_S_INIT("a\tb\t\td\n"
                   "a\tb\t\td\te\n"
                   "a\n"
                   "a\tb\tc\td\n"
                   "a\tb\tc\td\te\n")},
    {"16b", 100, 100, 0, '\t', TABWRITER_DISCARD_EMPTY_COLUMNS,
     BURROW_S_INIT("a\vb\v\vd\n"
                   "a\vb\v\vd\ve\n"
                   "a\n"
                   "a\vb\vc\vd\n"
                   "a\vb\vc\vd\ve\n"),
     BURROW_S_INIT("a\tb\td\n"
                   "a\tb\td\te\n"
                   "a\n"
                   "a\tb\tc\td\n"
                   "a\tb\tc\td\te\n")},
    {"16b debug", 100, 100, 0, '\t', TABWRITER_DISCARD_EMPTY_COLUMNS | TABWRITER_DEBUG,
     BURROW_S_INIT("a\vb\v\vd\n"
                   "a\vb\v\vd\ve\n"
                   "a\n"
                   "a\vb\vc\vd\n"
                   "a\vb\vc\vd\ve\n"),
     BURROW_S_INIT("a\t|b\t||d\n"
                   "a\t|b\t||d\t|e\n"
                   "a\n"
                   "a\t|b\t|c\t|d\n"
                   "a\t|b\t|c\t|d\t|e\n")},
    {"16c", 100, 100, 0, '\t', TABWRITER_DISCARD_EMPTY_COLUMNS,
     BURROW_S_INIT("a\tb\t\td\n"
                   "a\tb\t\td\te\n"
                   "a\n"
                   "a\tb\tc\td\n"
                   "a\tb\tc\td\te\n"),
     BURROW_S_INIT("a\tb\t\td\n"
                   "a\tb\t\td\te\n"
                   "a\n"
                   "a\tb\tc\td\n"
                   "a\tb\tc\td\te\n")},
    {"16c debug", 100, 100, 0, '\t', TABWRITER_DISCARD_EMPTY_COLUMNS | TABWRITER_DEBUG,
     BURROW_S_INIT("a\tb\t\td\n"
                   "a\tb\t\td\te\n"
                   "a\n"
                   "a\tb\tc\td\n"
                   "a\tb\tc\td\te\n"),
     BURROW_S_INIT("a\t|b\t|\t|d\n"
                   "a\t|b\t|\t|d\t|e\n"
                   "a\n"
                   "a\t|b\t|c\t|d\n"
                   "a\t|b\t|c\t|d\t|e\n")},
};

static void Test(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++)
        check(t, a, &cases[i]);
    arena_free(&ar);
}

/* ------------------------------------------------------------------ panics */

static Int panic_write(void *self, Slice p, Error *err) {
    (void)self;
    (void)p;
    (void)err;
    panic_str(BURROW_S("cannot write"));
}

static const IoWriterVT panic_vt = {NULL, panic_write};

static Byte panic_buf[256];

/* The text of the panic f raises, or a NULL Str when it does not panic. */
static Str recovered(void (*f)(TabwriterWriter *), TabwriterWriter *w) {
    volatile Int n = -1;
    BURROW_TRY {
        f(w);
    }
    BURROW_CATCH(r) {
        Str m = panic_text(r);
        n = m.len < (Int)sizeof panic_buf ? m.len : (Int)sizeof panic_buf;
        memcpy(panic_buf, m.p, (size_t)n);
    }
    BURROW_TRY_END;
    if (n < 0)
        return (Str){NULL, 0};
    return (Str){panic_buf, n};
}

static void want_panic_string(TestingT *t, Str got, Str want) {
    if (got.p == NULL)
        testing_t_errorf_v(t, "failed to panic");
    else if (!str_eq(got, want))
        testing_t_errorf_v(t, "wrong panic message: got %q, want %q", got, want);
}

static void flush_after_a(TabwriterWriter *w) {
    io_write_string(tabwriter_writer_as_io_writer(w), BURROW_S("a"), NULL);
    tabwriter_writer_flush(w);
}

static void TestPanicDuringFlush(TestingT *t) {
    TabwriterWriter *w = tabwriter_new_writer(
        heap_allocator(), (IoWriter){&panic_vt, NULL}, 0, 0, 5, ' ', 0);
    want_panic_string(t, recovered(flush_after_a, w),
                      BURROW_S("tabwriter: panic during Flush (cannot write)"));
    tabwriter_writer_free(w);
}

static void write_two_lines(TabwriterWriter *w) {
    /* the second \n triggers a call to w.Write and thus a panic */
    io_write_string(tabwriter_writer_as_io_writer(w), BURROW_S("a\n\n"), NULL);
}

static void TestPanicDuringWrite(TestingT *t) {
    TabwriterWriter *w = tabwriter_new_writer(
        heap_allocator(), (IoWriter){&panic_vt, NULL}, 0, 0, 5, ' ', 0);
    want_panic_string(t, recovered(write_two_lines, w),
                      BURROW_S("tabwriter: panic during Write (cannot write)"));
    tabwriter_writer_free(w);
}

static void init_negative(TabwriterWriter *w) {
    tabwriter_writer_init(w, io_discard, 0, -1, 0, ' ', 0);
}

static void TestNegativeWidthPanics(TestingT *t) {
    TabwriterWriter w;
    memset(&w, 0, sizeof w);
    want_panic_string(t, recovered(init_negative, &w),
                      BURROW_S("negative minwidth, tabwidth, or padding"));
    tabwriter_writer_free(&w);
}

/* ------------------------------------------------------------------ errors */

/* A writer that takes limit bytes and then fails, either with an error or,
 * when short is set, by reporting less than it was given. */
typedef struct Failing {
    Int limit;
    Int n;
    bool short_write;
} Failing;

BURROW_SENTINEL_ERROR(err_disk_full, "disk full");

static Int failing_write(void *self, Slice p, Error *err) {
    Failing *f = self;
    if (f->n + p.len <= f->limit) {
        f->n += p.len;
        return p.len;
    }
    Int m = f->limit - f->n;
    f->n = f->limit;
    if (!f->short_write)
        *err = err_disk_full;
    return m;
}

static const IoWriterVT failing_vt = {NULL, failing_write};

/* A write error inside Write comes back from Write, with n saying how much of
 * the input was taken in before it happened, as in Go. */
static void TestWriteError(TestingT *t) {
    Failing f = {2, 0, false};
    TabwriterWriter *w = tabwriter_new_writer(
        heap_allocator(), (IoWriter){&failing_vt, &f}, 0, 8, 1, ' ', 0);
    Error err = BURROW_NO_ERROR;
    Int n = tabwriter_writer_write(w, bytes_of(BURROW_S("abc\nxyz\n")), &err);
    CHECK(errors_is(err, err_disk_full));
    CHECK_INT_EQ(n, 4);
    tabwriter_writer_free(w);
}

/* A short write with no error is io.ErrShortWrite, and Flush leaves the
 * writer empty, so the next Flush writes nothing and succeeds. */
static void TestFlushShortWrite(TestingT *t) {
    Failing f = {3, 0, true};
    TabwriterWriter *w = tabwriter_new_writer(
        heap_allocator(), (IoWriter){&failing_vt, &f}, 0, 8, 1, ' ', 0);
    Error err = BURROW_NO_ERROR;
    tabwriter_writer_write(w, bytes_of(BURROW_S("a\tb\tc\t")), &err);
    CHECK(BURROW_OK(err));
    err = tabwriter_writer_flush(w);
    CHECK(errors_is(err, io_err_short_write));
    f.limit = 100;
    Int before = f.n;
    err = tabwriter_writer_flush(w);
    CHECK(BURROW_OK(err));
    CHECK_INT_EQ(f.n, before);
    tabwriter_writer_free(w);
}

/* Running out of memory is an error from Write, not a crash. */
static void TestOutOfMemory(TestingT *t) {
    static unsigned char mem[2048];
    Fixed fx;
    fixed_init(&fx, mem, sizeof mem);
    Buffer b = {{0}, 0};
    TabwriterWriter *w =
        tabwriter_new_writer(fixed_allocator(&fx), buffer_writer(&b), 0, 8, 1, ' ', 0);
    CHECK(w != NULL);
    Error err = BURROW_NO_ERROR;
    Str cell = BURROW_S("0123456789abcdef\t");
    bool failed = false;
    for (int i = 0; i < 1000 && !failed; i++) {
        tabwriter_writer_write(w, bytes_of(cell), &err);
        failed = BURROW_FAILED(err);
    }
    CHECK(failed);
    CHECK(errors_is(err, burrow_err_out_of_memory));

    fixed_init(&fx, mem, 64);
    CHECK(tabwriter_new_writer(fixed_allocator(&fx), buffer_writer(&b), 0, 8, 1, ' ',
                               0) == NULL);
}

/* ------------------------------------------------------------------ examples */

static void TestExampleWriterInit(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer out = BYTES_BUFFER(a);
    IoWriter o = bytes_buffer_as_io_writer(&out);
    TabwriterWriter *w = tabwriter_new_writer(a, o, 0, 8, 0, '\t', 0);
    IoWriter tw = tabwriter_writer_as_io_writer(w);

    /* Format in tab-separated columns with a tab stop of 8. */
    fmt_fprintln_v(tw, "a\tb\tc\td\t.");
    fmt_fprintln_v(tw, "123\t12345\t1234567\t123456789\t.");
    fmt_fprintln(tw, (Slice){NULL, 0, 0, NULL}, NULL);
    tabwriter_writer_flush(w);

    /* Format right-aligned in space-separated columns of minimal width 5 and
     * at least one blank of padding (so wider column entries do not touch
     * each other). */
    tabwriter_writer_init(w, o, 5, 0, 1, ' ', TABWRITER_ALIGN_RIGHT);
    fmt_fprintln_v(tw, "a\tb\tc\td\t.");
    fmt_fprintln_v(tw, "123\t12345\t1234567\t123456789\t.");
    fmt_fprintln(tw, (Slice){NULL, 0, 0, NULL}, NULL);
    tabwriter_writer_flush(w);

    want_str(t, bytes_buffer_string(&out, a),
             BURROW_S("a\tb\tc\td\t\t.\n"
                      "123\t12345\t1234567\t123456789\t.\n"
                      "\n"
                      "    a     b       c         d.\n"
                      "  123 12345 1234567 123456789.\n"
                      "\n"));
    bytes_buffer_free(&out);
    arena_free(&ar);
}

static void TestExampleElastic(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer out = BYTES_BUFFER(a);
    /* Observe how the b's and the d's, despite appearing in the second cell of
     * each line, belong to different columns. */
    TabwriterWriter *w =
        tabwriter_new_writer(a, bytes_buffer_as_io_writer(&out), 0, 0, 1, '.',
                             TABWRITER_ALIGN_RIGHT | TABWRITER_DEBUG);
    IoWriter tw = tabwriter_writer_as_io_writer(w);
    fmt_fprintln_v(tw, "a\tb\tc");
    fmt_fprintln_v(tw, "aa\tbb\tcc");
    fmt_fprintln_v(tw, "aaa\t"); /* trailing tab */
    fmt_fprintln_v(tw, "aaaa\tdddd\teeee");
    tabwriter_writer_flush(w);

    want_str(t, bytes_buffer_string(&out, a),
             BURROW_S("....a|..b|c\n"
                      "...aa|.bb|cc\n"
                      "..aaa|\n"
                      ".aaaa|.dddd|eeee\n"));
    bytes_buffer_free(&out);
    arena_free(&ar);
}

static void TestExampleTrailingTab(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer out = BYTES_BUFFER(a);
    /* Observe that the third line has no trailing tab, so its final cell is
     * not part of an aligned column. */
    const Int padding = 3;
    TabwriterWriter *w =
        tabwriter_new_writer(a, bytes_buffer_as_io_writer(&out), 0, 0, padding, '-',
                             TABWRITER_ALIGN_RIGHT | TABWRITER_DEBUG);
    IoWriter tw = tabwriter_writer_as_io_writer(w);
    fmt_fprintln_v(tw, "a\tb\taligned\t");
    fmt_fprintln_v(tw, "aa\tbb\taligned\t");
    fmt_fprintln_v(tw, "aaa\tbbb\tunaligned"); /* no trailing tab */
    fmt_fprintln_v(tw, "aaaa\tbbbb\taligned\t");
    tabwriter_writer_flush(w);

    want_str(t, bytes_buffer_string(&out, a),
             BURROW_S("------a|------b|---aligned|\n"
                      "-----aa|-----bb|---aligned|\n"
                      "----aaa|----bbb|unaligned\n"
                      "---aaaa|---bbbb|---aligned|\n"));
    bytes_buffer_free(&out);
    arena_free(&ar);
}

/* ------------------------------------------------------------------ benchmarks */

static Slice repeat_cells(Alloc *a, Int n) {
    Slice line = slice_make(a, TYPE_BYTE, 2 * n, 2 * n + 1);
    for (Int i = 0; i < n; i++) {
        ((Byte *)line.p)[2 * i] = 'a';
        ((Byte *)line.p)[2 * i + 1] = '\t';
    }
    return line;
}

typedef struct TableBench {
    Int w, h;
    bool reuse;
} TableBench;

static void bench_table(void *env, TestingB *b) {
    TableBench *tb = env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    /* Build a line with w cells. */
    Slice line = repeat_cells(a, tb->w);
    line = slice_append(a, line, &(Byte){'\n'}, 1);
    testing_b_report_allocs(b);
    TabwriterWriter *w = NULL;
    if (tb->reuse)
        w = tabwriter_new_writer(heap_allocator(), io_discard, 4, 4, 1, ' ', 0);
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++) {
        if (!tb->reuse)
            w = tabwriter_new_writer(heap_allocator(), io_discard, 4, 4, 1, ' ', 0);
        /* Write the line h times. */
        for (Int j = 0; j < tb->h; j++)
            tabwriter_writer_write(w, line, NULL);
        tabwriter_writer_flush(w);
        if (!tb->reuse)
            tabwriter_writer_free(w);
    }
    if (tb->reuse)
        tabwriter_writer_free(w);
    arena_free(&ar);
}

static void bench_table_size(void *env, TestingB *b) {
    TableBench *tb = env;
    TableBench n = *tb, r = *tb;
    n.reuse = false;
    r.reuse = true;
    testing_b_run(b, BURROW_S("new"), BURROW_FN(TestingBFunc, bench_table, &n));
    testing_b_run(b, BURROW_S("reuse"), BURROW_FN(TestingBFunc, bench_table, &r));
}

static void BenchmarkTable(TestingB *b) {
    static const Int ws[] = {1, 10, 100};
    static const Int hs[] = {10, 1000, 100000};
    char name[32];
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            TableBench tb = {ws[i], hs[j], false};
            int k = snprintf(name, sizeof name, "%dx%d", (int)ws[i], (int)hs[j]);
            testing_b_run(b, str_from_bytes(name, k),
                          BURROW_FN(TestingBFunc, bench_table_size, &tb));
        }
    }
}

static void bench_pyramid(void *env, TestingB *b) {
    Int x = *(Int *)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    /* Build a line with x cells. */
    Slice line = repeat_cells(a, x);
    Slice nl = bytes_of(BURROW_S("\n"));
    testing_b_report_allocs(b);
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++) {
        TabwriterWriter *w =
            tabwriter_new_writer(heap_allocator(), io_discard, 4, 4, 1, ' ', 0);
        /* Write increasing prefixes of that line. */
        for (Int j = 0; j < x; j++) {
            tabwriter_writer_write(w, slice_sub(line, 0, j * 2), NULL);
            tabwriter_writer_write(w, nl, NULL);
        }
        tabwriter_writer_flush(w);
        tabwriter_writer_free(w);
    }
    arena_free(&ar);
}

static void BenchmarkPyramid(TestingB *b) {
    static Int xs[] = {10, 100, 1000};
    char name[32];
    for (int i = 0; i < 3; i++) {
        int k = snprintf(name, sizeof name, "%d", (int)xs[i]);
        testing_b_run(b, str_from_bytes(name, k),
                      BURROW_FN(TestingBFunc, bench_pyramid, &xs[i]));
    }
}

static void bench_ragged(void *env, TestingB *b) {
    Int h = *(Int *)env;
    static const Int ws[8] = {6, 2, 9, 5, 5, 7, 3, 8};
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice lines[8];
    /* Build a line with w cells. */
    for (int i = 0; i < 8; i++)
        lines[i] = repeat_cells(a, ws[i]);
    Slice nl = bytes_of(BURROW_S("\n"));
    testing_b_report_allocs(b);
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++) {
        TabwriterWriter *w =
            tabwriter_new_writer(heap_allocator(), io_discard, 4, 4, 1, ' ', 0);
        /* Write the lines in turn h times. */
        for (Int j = 0; j < h; j++) {
            tabwriter_writer_write(w, lines[j % 8], NULL);
            tabwriter_writer_write(w, nl, NULL);
        }
        tabwriter_writer_flush(w);
        tabwriter_writer_free(w);
    }
    arena_free(&ar);
}

static void BenchmarkRagged(TestingB *b) {
    static Int hs[] = {10, 100, 1000};
    char name[32];
    for (int i = 0; i < 3; i++) {
        int k = snprintf(name, sizeof name, "%d", (int)hs[i]);
        testing_b_run(b, str_from_bytes(name, k),
                      BURROW_FN(TestingBFunc, bench_ragged, &hs[i]));
    }
}

static const Str code_snippet = BURROW_S_INIT("\n"
                                              "some command\n"
                                              "\n"
                                              "foo\t# aligned\n"
                                              "barbaz\t# comments\n"
                                              "\n"
                                              "but\n"
                                              "mostly\n"
                                              "single\n"
                                              "cell\n"
                                              "lines\n");

static void BenchmarkCode(TestingB *b) {
    testing_b_report_allocs(b);
    for (Int i = 0; i < testing_b_n(b); i++) {
        TabwriterWriter *w =
            tabwriter_new_writer(heap_allocator(), io_discard, 4, 4, 1, ' ', 0);
        /* The code is small, so it's reasonable for the tabwriter user to write
         * it all at once, or buffer the writes. */
        tabwriter_writer_write(w, bytes_of(code_snippet), NULL);
        tabwriter_writer_flush(w);
        tabwriter_writer_free(w);
    }
}

#define TESTS(X)                                                                       \
    X(Test)                                                                            \
    X(TestPanicDuringFlush)                                                            \
    X(TestPanicDuringWrite)                                                            \
    X(TestNegativeWidthPanics)                                                         \
    X(TestWriteError)                                                                  \
    X(TestFlushShortWrite)                                                             \
    X(TestOutOfMemory)                                                                 \
    X(TestExampleWriterInit)                                                           \
    X(TestExampleElastic)                                                              \
    X(TestExampleTrailingTab)                                                          \
    X(BenchmarkTable)                                                                  \
    X(BenchmarkPyramid)                                                                \
    X(BenchmarkRagged)                                                                 \
    X(BenchmarkCode)

TESTING_MAIN(TESTS)

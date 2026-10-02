#include <stdint.h>

#include "burrow/burrow.h"

/* The code under test: counts the newlines it reads, and stops at the first
 * error that is not the end of the input. */
static Int count_lines(IoReader r, Error *err) {
    Byte buf[8];
    Int lines = 0;
    for (;;) {
        Error e = BURROW_NO_ERROR;
        Int n = BURROW_CALL(r, read, slice_from(buf, 8, 8, TYPE_BYTE), &e);
        for (Int i = 0; i < n; i++)
            lines += buf[i] == '\n';
        if (errors_is(e, io_eof))
            break;
        if (BURROW_FAILED(e)) {
            *err = e;
            return lines;
        }
    }
    *err = BURROW_NO_ERROR;
    return lines;
}

static void TestCountLines(TestingT *t) {
    Alloc *a = heap_allocator();
    Str text = BURROW_S("one\ntwo\nthree\n");
    const char *names[] = {"plain", "one byte", "half", "data with EOF"};
    for (int i = 0; i < 4; i++) {
        StringsReader sr;
        strings_reader_reset(&sr, text);
        IoReader r = strings_reader_as_io_reader(&sr);
        IoReader w = r;
        if (i == 1)
            w = iotest_one_byte_reader(a, r);
        if (i == 2)
            w = iotest_half_reader(a, r);
        if (i == 3)
            w = iotest_data_err_reader(a, r);
        Error err;
        Int got = count_lines(w, &err);
        fmt_printf_v("%-14s %d lines\n", str_from_cstr(names[i]), got);
        if (got != 3 || BURROW_FAILED(err))
            testing_t_errorf_v(t, "%s: got %d lines, %v", str_from_cstr(names[i]), got,
                               err);
        if (i > 0)
            iotest_reader_free(a, w);
    }
}

// doc: timeout
static void TestCountLinesTimeout(TestingT *t) {
    StringsReader sr;
    strings_reader_reset(&sr, BURROW_S("one\ntwo\nthree\nfour\nfive\n"));
    IoReader r =
        iotest_timeout_reader(heap_allocator(), strings_reader_as_io_reader(&sr));
    Error err;
    Int got = count_lines(r, &err);
    fmt_printf_v("before the timeout: %d lines, %v\n", got, err);
    if (!errors_is(err, iotest_err_timeout))
        testing_t_errorf_v(t, "got %v, want the timeout", err);
    iotest_reader_free(heap_allocator(), r);
}
// doc: end

// doc: testreader
static void TestReaderChecks(TestingT *t) {
    Str text = BURROW_S("Now is the time");
    StringsReader sr;
    strings_reader_reset(&sr, text);
    Slice want = slice_from((void *)(uintptr_t)text.p, text.len, text.len, TYPE_BYTE);
    Error err = iotest_test_reader(strings_reader_as_io_reader(&sr), want);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);

    /* Now one that is wrong about what it holds. */
    strings_reader_reset(&sr, BURROW_S("Now is the tune"));
    err = iotest_test_reader(strings_reader_as_io_reader(&sr), want);
    fmt_println_v(err);
}
// doc: end

#define TESTS(X) X(TestCountLines) X(TestCountLinesTimeout) X(TestReaderChecks)
TESTING_MAIN(TESTS)

/* Output:
plain          3 lines
one byte       3 lines
half           3 lines
data with EOF  3 lines
before the timeout: 2 lines, timeout
ReadAll(small amounts) = "Now is the tune"
	want "Now is the time"
PASS
*/

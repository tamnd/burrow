#include <stdint.h>
#include <stdio.h>

#include "burrow/burrow.h"
#include "burrow/mem/arena.h"

// doc: split
/* Splits on commas. At the end of the input the rest is the last field, even
 * when it is empty, and bufio_err_final_token says to stop after it. */
static Int split_commas(void *env, Slice data, bool at_eof, Slice *token, Error *err) {
    (void)env;
    const Byte *p = (const Byte *)data.p;
    for (Int i = 0; i < data.len; i++) {
        if (p[i] == ',') {
            *token = slice_from(data.p, i, i, TYPE_BYTE);
            return i + 1;
        }
    }
    if (!at_eof)
        return 0;
    *token = data;
    *err = bufio_err_final_token;
    return 0;
}
// doc: end

static void run(void *env) {
    (void)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    StringsReader src;

    // doc: lines
    strings_reader_reset(&src, BURROW_S("first line\nsecond line\r\nthird"));
    BufioScanner *lines = bufio_new_scanner(a, strings_reader_as_io_reader(&src));
    while (bufio_scanner_scan(lines)) {
        Str line = bufio_scanner_text(lines);
        printf("line: " BURROW_STR_FMT "\n", BURROW_STR_ARG(line));
    }
    Error err = bufio_scanner_err(lines);
    bufio_scanner_free(lines);
    // doc: end
    printf("scan error: %s\n", BURROW_FAILED(err) ? "yes" : "none");

    // doc: words
    strings_reader_reset(&src, BURROW_S("  the quick\tbrown\n fox  "));
    BufioScanner *words = bufio_new_scanner(a, strings_reader_as_io_reader(&src));
    bufio_scanner_split(words, BUFIO_SCAN_WORDS);
    Int count = 0;
    while (bufio_scanner_scan(words))
        count++;
    bufio_scanner_free(words);
    // doc: end
    printf("words: %lld\n", (long long)count);

    // doc: fields
    strings_reader_reset(&src, BURROW_S("1,2,3,"));
    BufioScanner *fields = bufio_new_scanner(a, strings_reader_as_io_reader(&src));
    bufio_scanner_split(fields, BURROW_FN(BufioSplitFunc, split_commas, NULL));
    while (bufio_scanner_scan(fields)) {
        Str f = bufio_scanner_text(fields);
        printf("field: \"" BURROW_STR_FMT "\"\n", BURROW_STR_ARG(f));
    }
    bufio_scanner_free(fields);
    // doc: end

    // doc: reader
    strings_reader_reset(&src, BURROW_S("key=value\nnext"));
    BufioReader *r = bufio_new_reader(a, strings_reader_as_io_reader(&src));
    Slice peek = bufio_reader_peek(r, 3, &err);
    printf("peek: %.*s\n", (int)peek.len, (const char *)peek.p);
    Str first = bufio_reader_read_string(r, a, '\n', &err);
    Str rest = bufio_reader_read_string(r, a, '\n', &err);
    // doc: end
    printf("first: " BURROW_STR_FMT, BURROW_STR_ARG(first));
    printf("rest: " BURROW_STR_FMT ", " BURROW_STR_FMT "\n", BURROW_STR_ARG(rest),
           BURROW_STR_ARG(error_text(err)));
    bufio_reader_free(r);

    // doc: writer
    BytesBuffer out = BYTES_BUFFER(a);
    BufioWriter *w = bufio_new_writer(a, bytes_buffer_as_io_writer(&out));
    bufio_writer_write_string(w, BURROW_S("hello, "), NULL);
    bufio_writer_write_rune(w, 0x4e16, NULL);
    bufio_writer_write_byte(w, (Byte)'!');
    Int before = bytes_buffer_len(&out);
    err = bufio_writer_flush(w);
    // doc: end
    printf("before flush: %lld, after: %lld\n", (long long)before,
           (long long)bytes_buffer_len(&out));
    printf("out: " BURROW_STR_FMT "\n", BURROW_STR_ARG(bytes_buffer_string(&out, a)));
    bufio_writer_free(w);

    arena_free(&ar);
}

int main(void) {
    runtime_main(BURROW_FN(Func, run, NULL));
    return 0;
}

/* Output:
line: first line
line: second line
line: third
scan error: none
words: 4
field: "1"
field: "2"
field: "3"
field: ""
peek: key
first: key=value
rest: next, EOF
before flush: 0, after: 11
out: hello, 世!
*/

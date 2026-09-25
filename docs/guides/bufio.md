# Buffered input and output

`burrow/bufio.h` is Go's `bufio`. It puts a buffer in front of any `IoReader` or `IoWriter`, so that reading a byte or a line at a time does not turn into a system call per byte, and it has `BufioScanner`, the easy way to walk through input a line or a word at a time.

## Scanning lines

Most programs that read text want it line by line. A scanner does that by default:

<!-- example: ../examples/bufio/bufio.c#lines -->
```c
strings_reader_reset(&src, BURROW_S("first line\nsecond line\r\nthird"));
BufioScanner *lines = bufio_new_scanner(a, strings_reader_as_io_reader(&src));
while (bufio_scanner_scan(lines)) {
    Str line = bufio_scanner_text(lines);
    printf("line: " BURROW_STR_FMT "\n", BURROW_STR_ARG(line));
}
Error err = bufio_scanner_err(lines);
bufio_scanner_free(lines);
```

That prints the three lines without their endings. A `\r` before the `\n` is dropped too, and the last line counts even though no newline ends it. When the loop stops, `bufio_scanner_err` tells a clean end of input, where it is nil, from a real error.

One thing differs from Go. `Scanner.Text` in Go copies the token into a new string. `bufio_scanner_text` does not: the `Str` points into the scanner's buffer and is only good until the next call to `bufio_scanner_scan`. Copy it with `str_clone` or similar if you need to keep it.

A line longer than 64 KiB stops the scan with `bufio_err_too_long`. Give the scanner a bigger limit with `bufio_scanner_buffer` before the first scan. The buffer you pass stays yours, and the scanner only allocates a larger one from its allocator if a token does not fit.

## Other ways to split

`bufio_scanner_split` changes what a token is. The four splitters from Go are there as `BUFIO_SCAN_LINES`, `BUFIO_SCAN_WORDS`, `BUFIO_SCAN_RUNES` and `BUFIO_SCAN_BYTES`:

<!-- example: ../examples/bufio/bufio.c#words -->
```c
strings_reader_reset(&src, BURROW_S("  the quick\tbrown\n fox  "));
BufioScanner *words = bufio_new_scanner(a, strings_reader_as_io_reader(&src));
bufio_scanner_split(words, BUFIO_SCAN_WORDS);
Int count = 0;
while (bufio_scanner_scan(words))
    count++;
bufio_scanner_free(words);
```

`count` is 4. Words are split on Unicode white space, so tabs, newlines and runs of spaces all count as one gap.

A split function of your own gets the unread input and whether the reader has ended. It returns how many bytes to move past and sets `*token` to the next token, or leaves it alone to ask for more input. Here is one that splits on commas and keeps an empty last field:

<!-- example: ../examples/bufio/bufio.c#split -->
```c
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
```

It is a function value like any other, so it goes in with `BURROW_FN`, and `env` carries whatever state it needs:

<!-- example: ../examples/bufio/bufio.c#fields -->
```c
strings_reader_reset(&src, BURROW_S("1,2,3,"));
BufioScanner *fields = bufio_new_scanner(a, strings_reader_as_io_reader(&src));
bufio_scanner_split(fields, BURROW_FN(BufioSplitFunc, split_commas, NULL));
while (bufio_scanner_scan(fields)) {
    Str f = bufio_scanner_text(fields);
    printf("field: \"" BURROW_STR_FMT "\"\n", BURROW_STR_ARG(f));
}
bufio_scanner_free(fields);
```

That gives four fields, the last one empty. A token that is set but empty is a real token, which is how Go tells it apart from no token at all. The plain functions `bufio_scan_lines` and friends are there for splitters that want to build on the standard ones.

## Reading

`BufioReader` is for when you need more control than a scanner gives. It can peek ahead without consuming, read up to a delimiter, read runes, and put back the last byte or rune:

<!-- example: ../examples/bufio/bufio.c#reader -->
```c
strings_reader_reset(&src, BURROW_S("key=value\nnext"));
BufioReader *r = bufio_new_reader(a, strings_reader_as_io_reader(&src));
Slice peek = bufio_reader_peek(r, 3, &err);
printf("peek: %.*s\n", (int)peek.len, (const char *)peek.p);
Str first = bufio_reader_read_string(r, a, '\n', &err);
Str rest = bufio_reader_read_string(r, a, '\n', &err);
```

The peek sees `key` and leaves it to be read. `first` is `key=value\n`, delimiter included, and `rest` is `next` with `err` set to `io_eof`, because the input ended before another newline. As in Go, data and an error can come back together.

`bufio_reader_peek`, `bufio_reader_read_slice` and `bufio_reader_read_line` return views into the reader's buffer, which the next read may overwrite, so use the bytes before reading again. `bufio_reader_read_bytes` and `bufio_reader_read_string` copy into memory from the allocator you pass, which is yours to keep.

## Writing

`BufioWriter` gathers small writes and hands them to the writer underneath in large pieces. Nothing reaches it until the buffer fills or you flush:

<!-- example: ../examples/bufio/bufio.c#writer -->
```c
BytesBuffer out = BYTES_BUFFER(a);
BufioWriter *w = bufio_new_writer(a, bytes_buffer_as_io_writer(&out));
bufio_writer_write_string(w, BURROW_S("hello, "), NULL);
bufio_writer_write_rune(w, 0x4e16, NULL);
bufio_writer_write_byte(w, (Byte)'!');
Int before = bytes_buffer_len(&out);
err = bufio_writer_flush(w);
```

`before` is 0 and the buffer holds all 11 bytes after the flush. Once a write fails, every later write and flush returns the same error, so checking the final flush is enough to know whether it all got through. Freeing a writer does not flush it.

## Memory

`bufio_new_reader`, `bufio_new_writer` and `bufio_new_scanner` take the allocator the struct and its buffer come from, and the matching `_free` functions give both back. As in Go, wrapping a `BufioReader` or `BufioWriter` that already has a big enough buffer hands back the same one. It counts how many times that happened, and only the last free releases it.

Go code often declares a zero `bufio.Reader` and calls `Reset` on it. That works here too. A zeroed `BufioReader` or `BufioWriter` gets a 4096 byte buffer on its first reset, from its `a` field if you set one and from the heap if not, and its free function releases just the buffer, since the struct is yours.

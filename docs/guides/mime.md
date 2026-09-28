# MIME

Go's `mime` packages are coming over one at a time. `burrow/mime/quotedprintable.h` is the first. `mime` itself, with media types and the encoded words of mail headers, and `mime/multipart` will follow.

## quotedprintable

`burrow/mime/quotedprintable.h` is Go's `mime/quotedprintable`, the quoted-printable encoding of RFC 2045. Mail uses it for bodies that are mostly ASCII text. A printable byte stays as it is, any other byte becomes `=` and two hex digits, and a line longer than 76 characters gets a soft line break, which is an `=` at the end of the line that the reader takes out again.

The writer encodes what is written to it into another writer. Its output is the same as Go's byte for byte:

<!-- example: ../examples/mime/quotedprintable.c#encode -->
```c
BytesBuffer out = BYTES_BUFFER(a);
QuotedprintableWriter *w =
    quotedprintable_new_writer(a, bytes_buffer_as_io_writer(&out));
io_write_string(
    quotedprintable_writer_as_io_writer(w),
    BURROW_S("Caf\xc3\xa9 au lait, 2 = 1 + 1 \n"
             "This line is long enough that the encoder has to break it in "
             "two, since a line may be 76 characters at most."),
    &err);
err = quotedprintable_writer_close(w);
show("encoded", bytes_buffer_bytes(&out));
```

That prints:

```
encoded: "Caf=C3=A9 au lait, 2 =3D 1 + 1=20\r\nThis line is long enough that the encoder has to break it in two, since a l=\r\nine may be 76 characters at most."
```

The é is two bytes of UTF-8, so it becomes two escapes. The `=` sign is escaped because it starts an escape itself. The space at the end of the first line is escaped too, because a reader drops spaces and tabs at the end of a line. The line break comes out as CRLF, which is what mail wants. A lone LF or a lone CR counts as a line break as well. Setting `w->binary` to true before the first write makes every CR and LF data instead, and they get escaped like any other byte. `quotedprintable_writer_close` writes out the last line. It does not close the writer underneath.

The reader goes the other way:

<!-- example: ../examples/mime/quotedprintable.c#decode -->
```c
StringsReader src;
strings_reader_reset(&src,
                     BURROW_S("Caf=C3=A9 au lait, soft=\r\n break, tab=09.\r\n"));
QuotedprintableReader *r =
    quotedprintable_new_reader(a, strings_reader_as_io_reader(&src));
Slice text = io_read_all(a, quotedprintable_reader_as_io_reader(r), &err);
show("decoded", text);
quotedprintable_reader_free(r);
```

That prints `decoded: "Café au lait, soft break, tab\t.\r\n"`. The soft line break is gone and the hard one at the end is kept as it was. The reader is lenient in the places where Go's is: an `=` that is not followed by two hex digits or a line break is passed through as a plain `=`, and bytes of 0x80 and over are passed through too, since plenty of mail has them. A control character other than tab, CR and LF is an error, and it comes back after the bytes that decoded before it:

<!-- example: ../examples/mime/quotedprintable.c#errors -->
```c
strings_reader_reset(&src, BURROW_S("fine=\r\nbad\x01"));
r = quotedprintable_new_reader(a, strings_reader_as_io_reader(&src));
text = io_read_all(a, quotedprintable_reader_as_io_reader(r), &err);
show("before the error", text);
fmt_printf_v("error: %v\n", err);
quotedprintable_reader_free(r);
```

That prints `before the error: "finebad"` and then `error: quotedprintable: invalid unescaped byte 0x01 in body`. An input that ends in the middle of an escape gives `io_err_unexpected_eof`.

The reader reads through a `bufio` reader, and reuses the one it is given when it is already a `bufio` reader with a buffer of 4096 bytes or more. It hands out the decoded bytes straight out of that buffer, so it does not copy a line before decoding it.

## How close it is to Go

The tests check the reader against what Go's reader makes of Go's own test table and of more edge cases, both read all at once and read one byte at a time. They also check it against a digest of what Go makes of every string of up to six bytes from `0A \r\n=\t`, 137,257 inputs in all. The writer is checked against Go on Go's test table and on 40 random inputs, as text and as binary, written whole and three bytes at a time. `tools/gen-quotedprintable-tests.sh` makes the expected results from the Go toolchain it finds.

# MIME

Go's `mime` packages are coming over one at a time. `burrow/mime.h` and `burrow/mime/quotedprintable.h` are done, and `mime/multipart` will follow.

## mime

`burrow/mime.h` is Go's `mime` package: media types, the table that maps file extensions to them, and the encoded words of mail headers.

### Media types

A media type is the value of a Content-Type or Content-Disposition header: a type, then parameters after semicolons. `mime_parse_media_type` reads one:

<!-- example: ../examples/mime/mime.c#parse -->
```c
Map *params;
Str t = mime_parse_media_type(
    a, BURROW_S("Text/HTML; Charset=\"UTF-8\"; title*=utf-8''caf%C3%A9"), &params,
    &err);
Str charset = BURROW_S("charset"), title = BURROW_S("title");
fmt_printf_v("type %s, charset %s, title %s\n", t,
             *(const Str *)map_get(params, &charset),
             *(const Str *)map_get(params, &title));

t = mime_parse_media_type(a, BURROW_S("text/plain; oops"), &params, &err);
fmt_printf_v("type %s, error: %v\n", t, err);
```

That prints:

```
type text/html, charset UTF-8, title café
type text/plain, error: mime: invalid media parameter
```

The type and the parameter names come back in lower case, and the values come back as they were. The `title*=utf-8''caf%C3%A9` parameter is written the RFC 2231 way, and it comes back as plain `title`, decoded. Values split over `title*0`, `title*1` and so on are put back together the same way. When the type is fine and a parameter is not, you get the type and `mime_err_invalid_media_parameter`, and `*params` is NULL, which is what Go does too. Pass NULL for `params` when you only want the type.

`mime_format_media_type` goes the other way. It takes a map from `Str` to `Str`:

<!-- example: ../examples/mime/mime.c#format -->
```c
Map *m = map_make(a, TYPE_STRING, TYPE_STRING, 2);
Str k1 = BURROW_S("filename"), v1 = BURROW_S("r\xc3\xa9sum\xc3\xa9.pdf");
Str k2 = BURROW_S("Size"), v2 = BURROW_S("1024");
map_set(m, &k1, &v1);
map_set(m, &k2, &v2);
fmt_printf_v("%s\n", mime_format_media_type(a, BURROW_S("attachment"), m));
```

That prints `attachment; size=1024; filename*=utf-8''r%C3%A9sum%C3%A9.pdf`. The parameters are sorted by name as given, so `Size` sorts before `filename`, and then the names are lower-cased. A value that is not ASCII is written the RFC 2231 way. A type or a name that is not a valid token gives the empty string.

### Extensions

`mime_type_by_extension` gives the media type for a file extension, and `mime_extensions_by_type` goes the other way:

<!-- example: ../examples/mime/mime.c#extensions -->
```c
fmt_printf_v("%s\n", mime_type_by_extension(BURROW_S(".HTML")));

mime_add_extension_type(BURROW_S(".bw"), BURROW_S("application/x-burrow"));
mime_add_extension_type(BURROW_S(".burrow"), BURROW_S("application/x-burrow"));
mime_add_extension_type(BURROW_S(".note"), BURROW_S("text/x-note"));
fmt_printf_v("%s\n", mime_type_by_extension(BURROW_S(".note")));
Slice exts = mime_extensions_by_type(a, BURROW_S("application/x-burrow"), &err);
for (Int i = 0; i < exts.len; i++)
    fmt_printf_v("%s\n", ((const Str *)exts.p)[i]);

err = mime_add_extension_type(BURROW_S("bw"), BURROW_S("application/x-burrow"));
fmt_printf_v("error: %v\n", err);
```

That prints:

```
text/html; charset=utf-8
text/x-note; charset=utf-8
.burrow
.bw
error: mime: extension "bw" missing leading dot
```

The table starts as Go's built in one, and on first use it adds what the system knows. On Linux and the BSDs that is the shared MIME database's `globs2` file when there is one, and the `mime.types` files of Apache and friends when there is not. On macOS it is the `mime.types` files. On Windows it is the `Content Type` values under `HKEY_CLASSES_ROOT`, read through `advapi32.dll`, which is loaded when the table is first used rather than linked. So what you get for less common extensions depends on the machine, the same as in Go.

A text type without a charset gets `charset=utf-8`. An exact match on the extension wins, and after that the case does not matter. The media types that come back are borrowed from the table and live as long as the program, so there is nothing to free, and the lookup takes a read lock and allocates nothing for an ASCII extension up to 64 bytes. `mime_add_extension_type` takes the write lock and keeps copies of what it is given. The slice from `mime_extensions_by_type` comes from the allocator you pass, and the strings in it belong to the table.

### Encoded words

A mail header can only hold ASCII, so other text goes in as encoded words like `=?utf-8?q?Caf=C3=A9?=`. `mime_word_encoder_encode` makes them, with either of the two encodings:

<!-- example: ../examples/mime/mime.c#encode -->
```c
Str subject = BURROW_S("Caf\xc3\xa9 au lait");
fmt_printf_v("%s\n", mime_word_encoder_encode(MIME_Q_ENCODING, a, BURROW_S("utf-8"),
                                              subject));
fmt_printf_v("%s\n", mime_word_encoder_encode(MIME_B_ENCODING, a, BURROW_S("utf-8"),
                                              subject));
```

That prints:

```
=?utf-8?q?Caf=C3=A9_au_lait?=
=?utf-8?b?Q2Fmw6kgYXUgbGFpdA==?=
```

Text that is plain ASCII with no control characters comes back as it is, without a copy. With UTF-8, long text is split into words of at most 75 characters, never in the middle of a character.

`MimeWordDecoder` reads them. A zeroed one is ready to use:

<!-- example: ../examples/mime/mime.c#decode -->
```c
MimeWordDecoder dec = {0};
Str h = mime_word_decoder_decode_header(
    &dec, a, BURROW_S("Re: =?utf-8?q?Caf=C3=A9?= =?iso-8859-1?b?YXUgbGFpdA==?="),
    &err);
fmt_printf_v("%s\n", h);
```

That prints `Re: Caféau lait`. The space between two encoded words is not part of the text, so it goes, as RFC 2047 says. A word that does not decode is left in the header as it was. `mime_word_decoder_decode` decodes a single word and is strict about it.

UTF-8, ISO-8859-1 and US-ASCII are built in. For anything else, set `charset_reader`, which gets the charset name in lower case and a reader of the raw bytes, and returns a reader of UTF-8:

<!-- example: ../examples/mime/mime.c#charset -->
```c
Converter cv = {.a = a};
MimeWordDecoder custom = {BURROW_FN(MimeCharsetReader, cp1252, &cv)};
h = mime_word_decoder_decode_header(
    &custom, a, BURROW_S("Price: =?Windows-1252?q?=80_5?="), &err);
fmt_printf_v("%s\n", h);
h = mime_word_decoder_decode_header(&custom, a, BURROW_S("=?koi8-r?q?x?="), &err);
fmt_printf_v("error: %v\n", err);
```

That prints `Price: € 5` and then `error: no reader for "koi8-r"`. The decoder reads what `charset_reader` returns to the end before the call returns, so whatever the reader allocated can be let go of after that. Without a `charset_reader`, an unknown charset is the error `mime: unhandled charset "koi8-r"`.

Every string that comes back is one allocation from the allocator you pass, and so is every key and value in a params map. When an allocation fails you get the empty string, and nothing is left allocated.

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

The `mime` tests check the parser, the formatter, the encoder and the decoder against what Go makes of Go's own test tables, errors included. Past that they check digests of what Go makes of 20,000 random media types, 20,000 random headers and 5,000 random strings to encode, each built from pieces that hit the hard cases: RFC 2231 continuations, quoting, bad escapes, odd charsets and words split across white space. `tools/gen-mime-tests.sh` makes the expected results.

The extension table is tested with a script of 16,017 steps from `tools/gen-mime-type-tests.sh`. Each step empties the table or puts the built in one back, loads a `globs2` or `mime.types` file, adds a type, or asks for one, and the test replays them and checks every answer against what Go gave at the same point. The script has Go's own tests in it, every built in extension and type in both cases, and 200 files of each format built at random from pieces such as comments, CRLF line ends, glob patterns Go skips, bad types and non-ASCII extensions. Another test adds and looks up types from eight threads at once. The Windows registry reader is not covered by the script, since Go's test for it needs a real registry too.

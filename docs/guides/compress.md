# Compression

`burrow/compress/flate.h` is Go's `compress/flate`, the DEFLATE format of RFC 1951. It is the compression inside gzip, zlib, zip and PNG, without any of their headers or checksums around it. It reads and writes DEFLATE, and the writer's output is the same as Go's byte for byte at every level. `burrow/compress/zlib.h` and `burrow/compress/gzip.h` put the zlib and gzip headers and checksums on top of it. `burrow/compress/lzw.h` is the older LZW format of GIF and PDF, and `burrow/compress/bzip2.h` reads bzip2 files. Neither has anything to do with DEFLATE.

## Reading

`flate_new_reader` takes any `IoReader` and gives back an `IoReadCloser` that reads the decompressed bytes. Everything in `burrow/io.h` works on it, so `io_read_all` gets the whole thing and `io_copy` streams it somewhere else:

<!-- example: ../examples/compress/flate.c#read -->
```c
BytesReader in;
bytes_reader_reset(&in, slice_from(hello, sizeof hello, sizeof hello, TYPE_BYTE));
IoReadCloser rc = flate_new_reader(a, bytes_reader_as_io_reader(&in));
Slice text = io_read_all(a, io_read_closer_as_io_reader(rc), &err);
```

`hello` holds 13 bytes that Go's `flate.NewWriter` wrote for `"hello, hello, hello, hello\n"`, and `text` comes back as that line.

The reader takes the compressed input a byte at a time when the `IoReader` it was given has a `ReadByte` method, which `BufioReader`, `BytesBuffer`, `BytesReader` and `StringsReader` all do. It then stops reading at the exact end of the compressed stream, so whatever comes after it in the input is still there for the next reader. That is what gzip and zip need, since their trailers follow the compressed data directly. Any other reader gets a `BufioReader` put in front of it, the same as in Go, and that one reads ahead.

Closing the reader does not close the input. It only reports the error that stopped reading, if there was one. `flate_reader_free` gives the reader and its 32 KiB window back to the allocator it came from.

## Dictionaries and reuse

A stream compressed with a preset dictionary needs the same dictionary to read it back, and `flate_new_reader_dict` takes one. A reader can also be pointed at a new input without allocating again, through the `FlateResetter` that `flate_reader_as_resetter` gives, which is Go's `r.(flate.Resetter)`:

<!-- example: ../examples/compress/flate.c#reset -->
```c
Slice dict = slice_from((char[]){"hello, "}, 7, 7, TYPE_BYTE);
bytes_reader_reset(
    &in, slice_from(with_dict, sizeof with_dict, sizeof with_dict, TYPE_BYTE));
flate_resetter_reset(flate_reader_as_resetter(rc), bytes_reader_as_io_reader(&in),
                     dict);
text = io_read_all(a, io_read_closer_as_io_reader(rc), &err);
```

`with_dict` is six bytes, and they read back as `"hello, hello\n"` because the first `hello, ` comes from the dictionary.

## Errors

Input that is not valid DEFLATE gives a `FlateCorruptInputError`, which is how many bytes had been read when the problem showed up. The message is the same as Go's, and `errors_as` finds the offset:

<!-- example: ../examples/compress/flate.c#corrupt -->
```c
static Byte bad[] = {0x07};
bytes_reader_reset(&in, slice_from(bad, 1, 1, TYPE_BYTE));
flate_resetter_reset(flate_reader_as_resetter(rc), bytes_reader_as_io_reader(&in),
                     (Slice){0});
io_read_all(a, io_read_closer_as_io_reader(rc), &err);
const FlateCorruptInputError *off = errors_as(err, TYPE_FLATE_CORRUPT_INPUT_ERROR);
if (off != NULL)
    printf("%.*s\n", (int)error_text(err).len, (const char *)error_text(err).p);
```

That prints `flate: corrupt input before offset 1`. Input that ends before the final block does gives `io_err_unexpected_eof`, after the bytes that could be decoded have been handed out. The reader keeps returning the same error once it has one, until it is reset.

## Writing

`flate_new_writer` compresses into any `IoWriter` at a level from -2 to 9. Level 1 (`FLATE_BEST_SPEED`) is the fastest, 9 (`FLATE_BEST_COMPRESSION`) packs the smallest, 0 stores the input as it is, -1 is the default of 6, and -2 only Huffman codes the input with no matching. Write as much as you like, then close the writer to end the stream:

<!-- example: ../examples/compress/flate.c#write -->
```c
BytesBuffer out = BYTES_BUFFER(a);
FlateWriter *fw = flate_new_writer(a, bytes_buffer_as_io_writer(&out),
                                   FLATE_BEST_COMPRESSION, &err);
if (fw == NULL)
    return 1;
Slice line =
    slice_from((char[]){"hello, hello, hello, hello\n"}, 27, 27, TYPE_BYTE);
flate_writer_write(fw, line, &err);
err = flate_writer_close(fw);
flate_writer_free(fw);
printf("%d bytes in, %d out\n", (int)line.len, (int)bytes_buffer_len(&out));
```

That prints `27 bytes in, 13 out`, and the 13 bytes are the same `hello` the reading example started from, because the writer makes exactly what Go's does. Closing does not close the `IoWriter` underneath. `flate_writer_flush` writes out everything so far followed by an empty stored block, so the other end can decode all of it before the stream ends, which is what network protocols want. It costs a few bytes each time.

A writer holds between about 400 KiB and 1 MiB depending on the level, so reuse one with `flate_writer_reset` rather than making a new one per stream. `flate_new_writer_dict` takes a preset dictionary, which helps a lot on short inputs that look like the dictionary, and the stream can then only be read with `flate_new_reader_dict` and the same bytes. Once a write to the underlying writer fails, every later call gives that same error until the writer is reset.

## zlib

`burrow/compress/zlib.h` is Go's `compress/zlib`, the format of RFC 1950. It is DEFLATE with a two byte header in front and an Adler-32 checksum of the uncompressed data at the end, and it is what PNG uses inside its image data and what HTTP calls `deflate`. The writer works like the flate one:

<!-- example: ../examples/compress/zlib.c#write -->
```c
BytesBuffer out = BYTES_BUFFER(a);
ZlibWriter *zw = zlib_new_writer(a, bytes_buffer_as_io_writer(&out));
if (zw == NULL)
    return 1;
zlib_writer_write(zw, slice_from((char[]){"hello, world\n"}, 13, 13, TYPE_BYTE),
                  &err);
err = zlib_writer_close(zw);
zlib_writer_free(zw);
printf("%d bytes\n", (int)bytes_buffer_len(&out));
```

That prints `26 bytes`, the same 26 that Go's `ExampleNewWriter` shows. A line this short does not compress, so most of it is a stored block. `zlib_new_writer` uses the default level, `zlib_new_writer_level` takes one from -2 to 9 as flate does, and a level outside that gives `zlib: invalid compression level: 10`. The compressor is only allocated on the first write, flush or close, and `zlib_writer_reset` keeps it for the next stream.

Reading checks the header when the reader is made, so a bad one comes back from `zlib_new_reader` straight away as `zlib_err_header`:

<!-- example: ../examples/compress/zlib.c#read -->
```c
BytesReader in;
bytes_reader_reset(&in, bytes_buffer_bytes(&out));
IoReadCloser rc = zlib_new_reader(a, bytes_reader_as_io_reader(&in), &err);
if (BURROW_FAILED(err))
    return 1;
Slice text = io_read_all(a, io_read_closer_as_io_reader(rc), &err);
zlib_reader_free(rc);
```

That prints `hello, world`. The checksum is checked when the compressed data runs out, and a mismatch gives `zlib_err_checksum` instead of `io_eof`. As with flate, the reader never reads past the checksum when its input has `ReadByte`.

A stream written with a preset dictionary says so in its header, along with the dictionary's checksum. Reading one without the dictionary, or with the wrong one, fails when the reader is made:

<!-- example: ../examples/compress/zlib.c#dict -->
```c
Slice dict = slice_from((char[]){"hello, "}, 7, 7, TYPE_BYTE);
bytes_buffer_reset(&out);
zw = zlib_new_writer_level_dict(a, bytes_buffer_as_io_writer(&out),
                                ZLIB_BEST_COMPRESSION, dict, &err);
if (zw == NULL)
    return 1;
zlib_writer_write(zw, slice_from((char[]){"hello, hello\n"}, 13, 13, TYPE_BYTE),
                  &err);
err = zlib_writer_close(zw);
zlib_writer_free(zw);

bytes_reader_reset(&in, bytes_buffer_bytes(&out));
rc = zlib_new_reader(a, bytes_reader_as_io_reader(&in), &err);
if (errors_is(err, zlib_err_dictionary))
    printf("%.*s\n", (int)error_text(err).len, (const char *)error_text(err).p);

bytes_reader_reset(&in, bytes_buffer_bytes(&out));
rc = zlib_new_reader_dict(a, bytes_reader_as_io_reader(&in), dict, &err);
if (BURROW_FAILED(err))
    return 1;
text = io_read_all(a, io_read_closer_as_io_reader(rc), &err);
zlib_reader_free(rc);
```

That prints `zlib: invalid dictionary` and then `hello, hello`. `zlib_reader_as_resetter` gives a `ZlibResetter`, which points a reader at a new stream and dictionary without allocating again.

The writer makes the same bytes as Go's at every level, with and without a dictionary, with one exception. At levels 7 to 9 with a dictionary, when the first block does not compress and ends up stored, Go 1.27 writes the dictionary into that stored block as if it were data, so the stream does not read back as what was written, even with Go's own reader. burrow makes the same choices Go does and leaves the dictionary out of that block, so its stream is the dictionary's length shorter than Go's and reads back correctly. The same goes for `flate_new_writer_dict`.

## gzip

`burrow/compress/gzip.h` is Go's `compress/gzip`, the format of RFC 1952 that `.gz` files and HTTP's `Content-Encoding: gzip` use. It is DEFLATE with a header that can name the file and carry a comment, a modification time and some extra bytes, and a CRC-32 and length at the end. The header is a plain struct on the writer, filled in before the first write:

<!-- example: ../examples/compress/gzip.c#write -->
```c
BytesBuffer buf = BYTES_BUFFER(a);
GzipWriter *zw = gzip_new_writer(a, bytes_buffer_as_io_writer(&buf));
if (zw == NULL)
    return 1;
zw->header.name = BURROW_S("a-new-hope.txt");
zw->header.comment = BURROW_S("an epic space opera by George Lucas");
zw->header.mod_time = time_date(1977, TIME_MAY, 25, 0, 0, 0, 0, time_utc_loc);
gzip_writer_write(zw, text("A long time ago in a galaxy far, far away..."), &err);
err = gzip_writer_close(zw);
gzip_writer_free(zw);
printf("%d bytes\n", (int)bytes_buffer_len(&buf));
```

That prints `120 bytes`, which is what Go's `NewWriter` makes of the same header and text. The strings are UTF-8 in C and Latin-1 in the file, so a name or comment with a character past U+00FF, or with a NUL in it, fails the first write with `gzip.Write: non-Latin-1 header string`. A `mod_time` of the zero `Time` leaves the time out. `gzip_new_writer_level` takes the same levels as flate.

The reader reads the header when it is made, and gives it back in `zr->header`:

<!-- example: ../examples/compress/gzip.c#read -->
```c
GzipReader *zr = gzip_new_reader(a, bytes_buffer_as_io_reader(&buf), &err);
if (zr == NULL)
    return 1;
Str when = time_string(time_utc(zr->header.mod_time), a);
printf("Name: %.*s\nComment: %.*s\nModTime: %.*s\n\n", (int)zr->header.name.len,
       (const char *)zr->header.name.p, (int)zr->header.comment.len,
       (const char *)zr->header.comment.p, (int)when.len, (const char *)when.p);
Slice data = io_read_all(a, gzip_reader_as_io_reader(zr), &err);
gzip_reader_free(zr);
```

That prints the name, the comment and `ModTime: 1977-05-25 00:00:00 +0000 UTC`, then the text. The strings in the header belong to the reader and go away with `gzip_reader_free` or the next reset. The checksum and length are checked when the data runs out, and a mismatch gives `gzip_err_checksum` instead of `io_eof`, so the data is only known to be good once a read has given `io_eof`.

A gzip file can hold several members one after the other, and by default they read as one stream. To see each member's header, turn that off with `gzip_reader_multistream`, read to the end of the member, and reset onto the same input for the next one. The reset gives `io_eof` when nothing is left:

<!-- example: ../examples/compress/gzip.c#multistream -->
```c
bytes_buffer_reset(&buf);
zw = gzip_new_writer(a, bytes_buffer_as_io_writer(&buf));
if (zw == NULL)
    return 1;
zw->header.name = BURROW_S("file-1.txt");
gzip_writer_write(zw, text("Hello Gophers - 1\n"), &err);
err = gzip_writer_close(zw);
gzip_writer_reset(zw, bytes_buffer_as_io_writer(&buf));
zw->header.name = BURROW_S("file-2.txt");
gzip_writer_write(zw, text("Hello Gophers - 2\n"), &err);
err = gzip_writer_close(zw);
gzip_writer_free(zw);

zr = gzip_new_reader(a, bytes_buffer_as_io_reader(&buf), &err);
if (zr == NULL)
    return 1;
do {
    gzip_reader_multistream(zr, false);
    data = io_read_all(a, gzip_reader_as_io_reader(zr), &err);
    printf("%.*s: %.*s", (int)zr->header.name.len, (const char *)zr->header.name.p,
           (int)data.len, (const char *)data.p);
    err = gzip_reader_reset(zr, bytes_buffer_as_io_reader(&buf));
} while (BURROW_OK(err));
gzip_reader_free(zr);
```

That prints `file-1.txt: Hello Gophers - 1` and `file-2.txt: Hello Gophers - 2`. This only works when the input has `ReadByte`, as a `BytesBuffer` does, because otherwise the reader puts a `bufio.Reader` in front of it that reads ahead into the next member.

## lzw

`burrow/compress/lzw.h` is Go's `compress/lzw`. LZW has no header, so both sides have to agree on two things up front: the bit order, `LZW_LSB` for GIF and `LZW_MSB` for PDF, and the width of a literal, which is 8 for bytes and can go down to 2 when every byte is known to be small. The writer's output is the same as Go's byte for byte:

<!-- example: ../examples/compress/lzw.c#write -->
```c
BytesBuffer out = BYTES_BUFFER(a);
LzwWriter *zw = lzw_new_writer(a, bytes_buffer_as_io_writer(&out), LZW_LSB, 8);
if (zw == NULL)
    return 1;
lzw_writer_write(
    zw, slice_from((char[]){"TOBEORNOTTOBEORTOBEORNOT"}, 24, 24, TYPE_BYTE), &err);
err = lzw_writer_close(zw);
lzw_writer_free(zw);
printf("%d bytes\n", (int)bytes_buffer_len(&out));
```

That prints `21 bytes`. Reading works like the other readers here, and takes bytes one at a time from an input with `ReadByte` so it stops at the end code:

<!-- example: ../examples/compress/lzw.c#read -->
```c
BytesReader in;
bytes_reader_reset(&in, bytes_buffer_bytes(&out));
LzwReader *zr = lzw_new_reader(a, bytes_reader_as_io_reader(&in), LZW_LSB, 8);
if (zr == NULL)
    return 1;
Slice text = io_read_all(a, lzw_reader_as_io_reader(zr), &err);
```

A stream that stops early gives `io_err_unexpected_eof` after the bytes that did decode, and a code the table does not have yet gives `lzw: invalid code`. An order or width out of range is not reported by `lzw_new_reader` or `lzw_new_writer`, which only return NULL when the allocator refuses. It comes back from the first read, write or close instead, the way Go does it:

<!-- example: ../examples/compress/lzw.c#errors -->
```c
bytes_reader_reset(&in, slice_from(tobe, 11, 11, TYPE_BYTE));
lzw_reader_reset(zr, bytes_reader_as_io_reader(&in), LZW_LSB, 8);
text = io_read_all(a, lzw_reader_as_io_reader(zr), &err);
if (errors_is(err, io_err_unexpected_eof))
    printf("%.*s after %d bytes\n", (int)error_text(err).len,
           (const char *)error_text(err).p, (int)text.len);

bytes_reader_reset(&in, bytes_buffer_bytes(&out));
lzw_reader_reset(zr, bytes_reader_as_io_reader(&in), LZW_LSB, 9);
text = io_read_all(a, lzw_reader_as_io_reader(zr), &err);
printf("%.*s\n", (int)error_text(err).len, (const char *)error_text(err).p);
lzw_reader_free(zr);
```

That prints `unexpected EOF after 8 bytes` and then `lzw: litWidth 9 out of range`. `lzw_reader_reset` and `lzw_writer_reset` start a new stream without allocating again.

TIFF writes LZW with the code width changing one code early. This package does not read or write that variant, and neither does Go's.

## bzip2

`burrow/compress/bzip2.h` is Go's `compress/bzip2`. Like Go's, it only reads. `bzip2_new_reader` gives back a plain `IoReader`, and `bzip2_reader_free` releases it:

<!-- example: ../examples/compress/bzip2.c#read -->
```c
BytesReader in;
bytes_reader_reset(&in, slice_from(hello, sizeof hello, sizeof hello, TYPE_BYTE));
IoReader zr = bzip2_new_reader(a, bytes_reader_as_io_reader(&in));
if (zr.vt == NULL)
    return 1;
Slice text = io_read_all(a, zr, &err);
bzip2_reader_free(zr);
```

`hello` holds the 52 bytes that `bzip2 -9` wrote for `"hello world\n"`. The reader allocates its block buffer on the first read rather than in `bzip2_new_reader`, because only the header says how big it has to be: 400 KB for each step of the level, so 3.6 MB for `-9`. If the allocator refuses that, the read gives `burrow_err_out_of_memory`. Several bzip2 files one after another, which is what `pbzip2` writes and what `cat a.bz2 b.bz2` makes, read back as one stream.

As with the other readers, an input with `ReadByte` is read a byte at a time and left positioned right after the last file, and any other input gets a `BufioReader` in front of it. A stream that stops early gives `io_err_unexpected_eof`, and anything else wrong gives a `Bzip2StructuralError`, whose message starts with `bzip2 data invalid: `. `errors_as` gives the text after that. Checksums are checked once a block has been read out, so the bytes of a damaged block come out before the error:

<!-- example: ../examples/compress/bzip2.c#errors -->
```c
hello[sizeof hello - 3] ^= 0x01;
bytes_reader_reset(&in, slice_from(hello, sizeof hello, sizeof hello, TYPE_BYTE));
zr = bzip2_new_reader(a, bytes_reader_as_io_reader(&in));
if (zr.vt == NULL)
    return 1;
text = io_read_all(a, zr, &err);
bzip2_reader_free(zr);
const Bzip2StructuralError *se = errors_as(err, TYPE_BZIP2_STRUCTURAL_ERROR);
if (se != NULL)
    printf("%d bytes, then %.*s\n", (int)text.len, (int)se->len,
           (const char *)se->p);
```

That flips one bit of the checksum over the whole file and prints `12 bytes, then file checksum mismatch`.

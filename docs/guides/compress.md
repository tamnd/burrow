# Compression

`burrow/compress/flate.h` is Go's `compress/flate`, the DEFLATE format of RFC 1951. It is the compression inside gzip, zlib, zip and PNG, without any of their headers or checksums around it. It reads and writes DEFLATE, and the writer's output is the same as Go's byte for byte at every level. `burrow/compress/zlib.h` puts the zlib header and checksum on top of it, and `compress/gzip` will follow. `burrow/compress/lzw.h` is the older LZW format of GIF and PDF, which has nothing to do with DEFLATE.

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

# Compression

`burrow/compress/flate.h` is Go's `compress/flate`, the DEFLATE format of RFC 1951. It is the compression inside gzip, zlib, zip and PNG, without any of their headers or checksums around it. It reads and writes DEFLATE, and the writer's output is the same as Go's byte for byte at every level. `compress/gzip` and `compress/zlib` will sit on top of both.

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

# Readers and writers

`burrow/io.h` is Go's `io`. It has the small interfaces everything else reads and writes through, `IoReader` and `IoWriter` first, and the helpers that work on any of them: copying, reading everything, limiting, splitting into sections, joining several into one, and a pipe that connects a writer in one goroutine to a reader in another.

An interface value here is two pointers, a table of functions and the data they work on. A type gets into one by a conversion function named after it, like `strings_reader_as_io_reader` or `bytes_buffer_as_io_writer`. The guide on [interfaces](interfaces.md) covers how that works and how to write your own.

## Copying

`io_copy` moves everything from a reader to a writer and tells you how many bytes it moved:

<!-- example: ../examples/io/io.c#copy -->
```c
StringsReader src;
strings_reader_reset(&src, BURROW_S("hello, world"));
BytesBuffer dst = BYTES_BUFFER(a);
Error err = BURROW_NO_ERROR;
int64_t n = io_copy(a, bytes_buffer_as_io_writer(&dst),
                    strings_reader_as_io_reader(&src), &err);
```

Reaching the end of the reader is not an error, so `err` stays clear and `n` is 12. The allocator is for the buffer the copy goes through, which is freed before `io_copy` returns. If you already have a buffer, `io_copy_buffer` uses yours and allocates nothing.

Like Go, the copy looks for a shortcut first. If the reader has a `WriteTo` method it is asked to write itself out, and if the writer has a `ReadFrom` method it is asked to read the whole source. `BytesBuffer`, `BytesReader` and `StringsReader` have these, so the copy above never touches a buffer of its own. A type declares its methods with `BURROW_METHODS_DEFINE`, using the `IO_SIG_WRITE_TO`, `IO_SIG_READ_FROM` and `IO_SIG_WRITE_STRING` signatures from `io.h`, and the copy finds them by name the way Go's type assertion does.

`io_copy_n` stops after a given number of bytes, and reports `io_eof` if the reader ran out first.

## Reading everything

`io_read_all` reads until the end and returns what it got in one slice from the allocator. Put an `IoLimitedReader` in front to cap how much that can be:

<!-- example: ../examples/io/io.c#readall -->
```c
strings_reader_reset(&src, BURROW_S("the quick brown fox"));
IoLimitedReader lim = io_limit_reader(strings_reader_as_io_reader(&src), 9);
Slice head = io_read_all(a, io_limited_reader_as_io_reader(&lim), &err);
```

`head` is `the quick`. Go's `LimitReader` returns a pointer to a new value. Here `io_limit_reader` returns the struct itself, so it can live on the stack, and `io_limited_reader_as_io_reader` makes the interface from its address. `IoTeeReader`, `IoNopCloser`, `IoSectionReader` and `IoOffsetWriter` work the same way.

`io_read_full` fills a buffer exactly. When the input ends partway through it says so with `io_err_unexpected_eof`, which is how you tell a short record from a clean end:

<!-- example: ../examples/io/io.c#errors -->
```c
strings_reader_reset(&src, BURROW_S("short"));
Byte buf[8];
Int got_n = io_read_full(strings_reader_as_io_reader(&src),
                         slice_from(buf, 8, 8, TYPE_BYTE), &err);
```

That reads 5 bytes and fails with `unexpected EOF`. `io_read_at_least` is the same with a minimum instead of the whole buffer.

## Sections and offsets

An `IoReaderAt` reads at a position without moving anything, so many readers can share one underlying source. `IoSectionReader` turns a window of one into an ordinary reader that can also seek:

<!-- example: ../examples/io/io.c#section -->
```c
strings_reader_reset(&src, BURROW_S("0123456789"));
IoSectionReader sec =
    io_new_section_reader(strings_reader_as_io_reader_at(&src), 3, 4);
Slice mid = io_read_all(a, io_section_reader_as_io_reader(&sec), &err);
```

`mid` is `3456`, and `io_section_reader_size` says 4. `io_section_reader_outer` gives back the source, offset and length it was made with. `IoOffsetWriter` is the writing side of the same idea: it turns an `IoWriterAt` into a writer that starts at an offset and can seek.

## Tee, multi and discard

`IoTeeReader` writes everything read through it to a writer as well, which is handy for keeping a copy of what a parser consumed:

<!-- example: ../examples/io/io.c#tee -->
```c
strings_reader_reset(&src, BURROW_S("seen twice"));
BytesBuffer copy = BYTES_BUFFER(a);
IoTeeReader tee = io_tee_reader(strings_reader_as_io_reader(&src),
                                bytes_buffer_as_io_writer(&copy));
Slice read = io_read_all(a, io_tee_reader_as_io_reader(&tee), &err);
```

`io_multi_reader` reads several readers one after another as if they were one, and `io_multi_writer` sends every write to all of its writers:

<!-- example: ../examples/io/io.c#multi -->
```c
StringsReader r1, r2;
strings_reader_reset(&r1, BURROW_S("one, "));
strings_reader_reset(&r2, BURROW_S("two"));
IoReader parts[2] = {strings_reader_as_io_reader(&r1),
                     strings_reader_as_io_reader(&r2)};
IoReader both = io_multi_reader(a, parts, 2);

BytesBuffer w1 = BYTES_BUFFER(a), w2 = BYTES_BUFFER(a);
IoWriter sinks[2] = {bytes_buffer_as_io_writer(&w1),
                     bytes_buffer_as_io_writer(&w2)};
IoWriter fan = io_multi_writer(a, sinks, 2);
io_copy(a, fan, both, &err);
```

Both buffers end up holding `one, two`. These two copy the list you give them, so the array can go away afterwards, and that copy comes from the allocator. With an arena there is nothing more to do. With the heap, `io_multi_reader_free` and `io_multi_writer_free` give it back. Nesting one inside another is flattened the way Go flattens it, so a chain of them costs one call per read and not one per level.

`io_discard` is a writer that accepts everything and keeps nothing.

## Pipes

`io_pipe` connects a writer to a reader with no buffer in between. Each write waits until readers have taken all of it, so the two ends belong in different goroutines:

<!-- example: ../examples/io/io.c#producer -->
```c
static void produce(void *env) {
    IoPipeWriter *w = (IoPipeWriter *)env;
    io_write_string(io_pipe_writer_as_io_writer(w), BURROW_S("sent through a pipe"),
                    NULL);
    io_pipe_writer_close(w);
}
```

<!-- example: ../examples/io/io.c#pipe -->
```c
IoPipeReader *pr;
IoPipeWriter *pw;
io_pipe(a, &pr, &pw);
SyncWaitGroup wg = {0};
sync_wait_group_go(&wg, BURROW_FN(Func, produce, pw));
Slice got = io_read_all(a, io_pipe_reader_as_io_reader(pr), &err);
sync_wait_group_wait(&wg);
io_pipe_free(pr);
```

Closing the writer is what lets the reader see the end. `io_pipe_writer_close_with_error` makes the reader see an error of your choosing instead, and closing the reader makes further writes fail with `io_err_closed_pipe`. Free the pipe once neither end is in use, which here is after the wait.

## Differences from Go

`io_copy_buffer` with a nil buffer is `io_err_short_buffer`, where Go would allocate one. Use `io_copy` when you want the allocation. The constructors that Go makes return pointers return values here, as described above, and the ones that have to allocate take an allocator and have a free function to match. `io/ioutil` comes with `os`, since nearly all of it is files.

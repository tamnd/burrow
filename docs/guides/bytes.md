# Bytes

`burrow/bytes.h` is Go's `bytes` package, all of it. A Go `[]byte` is a `Slice` of `Byte` here, and most of the package is the [strings](strings.md) functions again with a `Slice` where those take a `Str`. The tests are Go's own, ported line by line, so the edge cases behave the way they do in Go, including which results are nil and which are empty.

`BURROW_B` makes a `Slice` from a string literal for an argument. It points into the literal, so nothing may write through it.

## Searching and trimming

<!-- example: ../examples/bytes/package.c#search -->
```c
Slice line = bytes_trim_space(BURROW_B("  key = value  "));
Int eq = bytes_index_byte(line, '=');
Slice key = bytes_trim_space(slice_sub(line, 0, eq));
Slice value = bytes_trim_space(slice_sub(line, eq + 1, line.len));
```

The search functions take plain values and allocate nothing. The trim and cut functions return slices of the slice you gave them. Where Go returns nil, as `bytes.Trim` does when nothing is left, so does this, and a nil slice has length 0, so code that only looks at the length never has to care.

## Splitting

`bytes_split` returns a `Slice` of `Slice`, from the allocator, with every piece pointing into the input. Each piece has its capacity cut to its length, as Go does it, so appending to one piece copies it rather than writing over the next:

<!-- example: ../examples/bytes/package.c#split -->
```c
Slice fields = bytes_split(a, BURROW_B("a,b,c"), BURROW_B(","));
Slice first = BURROW_AT(Slice, fields, 0);
first = slice_append(a, first, "!", 1);
Slice second = BURROW_AT(Slice, fields, 1);
```

`second` is still `b`. The sequence versions, `bytes_lines`, `bytes_split_seq`, `bytes_fields_seq` and the rest, work the way the strings ones do, and yield a `const Slice *`.

## Building new bytes

The functions that make a result take an allocator first, and unlike their strings versions they always make a new slice, even when nothing changed, because the caller may write to it:

<!-- example: ../examples/bytes/package.c#change -->
```c
Slice in = BURROW_B("Hello, Gophers");
Slice upper = bytes_to_upper(a, in);
Slice swapped = bytes_replace_all(a, upper, BURROW_B("GOPHERS"), BURROW_B("WORLD"));
bool same = bytes_equal_fold(swapped, BURROW_B("hello, world"));
```

A failed allocation gives the nil slice.

## Buffer

`BytesBuffer` is Go's `bytes.Buffer`, something to write into and read back out of. It is a value, set up with `BYTES_BUFFER` and an allocator, and it is an `IoWriter` and an `IoReader` for anything that wants one:

<!-- example: ../examples/bytes/package.c#buffer -->
```c
BytesBuffer b = BYTES_BUFFER(a);
bytes_buffer_write_string(&b, BURROW_S("id,name\n"), NULL);
for (int i = 1; i <= 3; i++)
    fmt_fprintf_v(bytes_buffer_as_io_writer(&b), "%d,user%d\n", i, i);
Str header = bytes_buffer_read_string(&b, a, '\n', NULL);
Int left = bytes_buffer_len(&b);
```

Reading uses up what was written, and the last read ends with `io_eof` the way every reader does:

<!-- example: ../examples/bytes/package.c#drain -->
```c
Error err = BURROW_NO_ERROR;
for (;;) {
    Slice row = bytes_buffer_read_bytes(&b, a, '\n', &err);
    if (BURROW_FAILED(err))
        break;
    printf("row: %.*s", (int)row.len, (const char *)row.p);
}
bytes_buffer_free(&b);
```

`bytes_buffer_free` gives back the memory the buffer allocated. It never frees a slice you handed to `bytes_new_buffer`, which may have come from anywhere.

Go panics with `ErrTooLarge` when a buffer cannot grow. Here the write that needed the room returns `burrow_err_out_of_memory` instead and the buffer is left as it was. As in Go, `bytes_buffer_bytes`, `bytes_buffer_next` and `bytes_buffer_peek` hand out a slice of the buffer's own memory that is good until the next write, read, reset or truncate.

## Reader

`BytesReader` is Go's `bytes.Reader`, a slice you can read, seek and read at an offset:

<!-- example: ../examples/bytes/package.c#reader -->
```c
BytesReader *r = bytes_new_reader(a, BURROW_B("0123456789"));
bytes_reader_seek(r, -3, BURROW_IO_SEEK_END, NULL);
Byte tail[8];
Int n = bytes_reader_read(r, slice_from(tail, 8, 8, TYPE_BYTE), NULL);
```

It never copies the slice, so the slice has to outlive the reader.

## What is not here

Nothing from Go's `bytes` package is missing.

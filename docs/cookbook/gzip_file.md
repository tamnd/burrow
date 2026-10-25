# Compress a file with gzip

Compress one file into another and read it back. The gzip writer wraps the output file, `io_copy` moves the bytes, and `gzip_writer_close` has to come before the file is closed, since it writes the end of the stream. Reading back is the same in reverse with a gzip reader.

<!-- example: ../examples/cookbook/gzip_file.c#main -->
```c
#define BURROW_SHORT 1
#include "burrow/burrow.h"
#include "burrow/compress/gzip.h"

/* Writes src to dst compressed with gzip. */
static Error compress_file(Alloc *a, Str dst, Str src) {
    Error err = NO_ERROR;
    OsFile *in = os_open(a, src, &err);
    if (FAILED(err))
        return err;
    OsFile *out = os_create(a, dst, &err);
    if (FAILED(err)) {
        os_file_close(in);
        return err;
    }
    GzipWriter *zw = gzip_new_writer(a, os_file_as_io_writer(out));
    io_copy(a, gzip_writer_as_io_writer(zw), os_file_as_io_reader(in), &err);
    Error cerr = gzip_writer_close(zw); /* writes what is left, and the trailer */
    gzip_writer_free(zw);
    os_file_close(in);
    Error ferr = os_file_close(out);
    if (FAILED(err))
        return err;
    return FAILED(cerr) ? cerr : ferr;
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    Str text =
        strings_repeat(a, S("all work and no play makes jack a dull boy\n"), 100);
    Error err = os_write_file(S("jack.txt"), slice_from_str(a, text), 0644);
    if (OK(err))
        err = compress_file(a, S("jack.txt.gz"), S("jack.txt"));
    if (FAILED(err)) {
        fmt_println_v(err);
        return 1;
    }
    OsFileInfo fi = os_stat(a, S("jack.txt.gz"), &err);
    fmt_println_v(text.len, "bytes became", fi.vt->size(fi.data));

    /* And back again. */
    OsFile *f = os_open(a, S("jack.txt.gz"), &err);
    GzipReader *zr = gzip_new_reader(a, os_file_as_io_reader(f), &err);
    if (zr == NULL) {
        fmt_println_v(err);
        return 1;
    }
    Slice back = io_read_all(a, gzip_reader_as_io_reader(zr), &err);
    gzip_reader_free(zr);
    os_file_close(f);
    bool same = str_eq(str_from_bytes(back.p, back.len), text);
    fmt_println_v("read back the same:", same);
    arena_free(&ar);
}
```

It prints:

```text
4300 bytes became 86
read back the same: true
```

The program is [docs/examples/cookbook/gzip_file.c](../examples/cookbook/gzip_file.c). CI builds and runs it and checks that it prints this.

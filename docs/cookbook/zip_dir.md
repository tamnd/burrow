# Zip up a directory

`filepath_walk_dir` visits every file under a directory, and the zip writer adds each one under its path from the top. Names in a zip file always use forward slashes, so `filepath_to_slash` fixes them up on Windows. The archive isn't finished until `zip_writer_close` writes its directory at the end. Reading it back with `zip_open_reader` lists what went in.

<!-- example: ../examples/cookbook/zip_dir.c#main -->
```c
#define BURROW_SHORT 1
#include "burrow/burrow.h"
#include "burrow/path/filepath.h"

typedef struct Zipping {
    Alloc *a;
    Str root;
    ZipWriter *zw;
} Zipping;

/* Adds one file under its path from the root. Names in a zip file always use
 * forward slashes, whatever the system uses. */
static Error add(void *env, Str path, FsDirEntry d, Error err) {
    Zipping *z = env;
    if (FAILED(err) || d.vt->is_dir(d.data))
        return err;
    Str rel = filepath_rel(z->a, z->root, path, &err);
    if (FAILED(err))
        return err;
    IoWriter w = zip_writer_create(z->zw, filepath_to_slash(z->a, rel), &err);
    if (FAILED(err))
        return err;
    OsFile *f = os_open(z->a, path, &err);
    if (FAILED(err))
        return err;
    io_copy(z->a, w, os_file_as_io_reader(f), &err);
    os_file_close(f);
    return err;
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    os_mkdir_all(S("site/css"), 0755);
    os_write_file(S("site/index.html"),
                  BURROW_B("<h1>Hello</h1>\n<p>Hello, hello, hello, hello.</p>\n"),
                  0644);
    os_write_file(S("site/css/main.css"), BURROW_B("body { margin: 0; }\n"), 0644);

    /* The archive is written straight to the file. Closing the zip writer
     * writes the directory at the end, and only then is the archive whole. */
    Error err = NO_ERROR;
    OsFile *out = os_create(a, S("site.zip"), &err);
    if (FAILED(err)) {
        fmt_println_v(err);
        return 1;
    }
    Zipping z = {a, S("site"), zip_new_writer(a, os_file_as_io_writer(out))};
    err = filepath_walk_dir(a, S("site"), FN(FsWalkDirFunc, add, &z));
    if (!FAILED(err))
        err = zip_writer_close(z.zw);
    zip_writer_free(z.zw);
    os_file_close(out);
    if (FAILED(err)) {
        fmt_println_v(err);
        return 1;
    }

    /* Open it again and list what is inside. Files this small come out a
     * few bytes bigger, since Deflate has some overhead of its own. */
    ZipReadCloser *rc = zip_open_reader(a, S("site.zip"), &err);
    if (FAILED(err)) {
        fmt_println_v(err);
        return 1;
    }
    for (Int i = 0; i < rc->reader.file.len; i++) {
        ZipFileHeader *h = &AT(ZipFile *, rc->reader.file, i)->file_header;
        uint64_t size = h->uncompressed_size64, packed = h->compressed_size64;
        fmt_printf_v("%-14s %3d bytes, %3d packed\n", h->name, size, packed);
    }
    zip_read_closer_close(rc);
    os_remove_all(S("site"));
    os_remove(S("site.zip"));
    arena_free(&ar);
}
```

It prints:

```text
css/main.css    20 bytes,  27 packed
index.html      50 bytes,  57 packed
```

The program is [docs/examples/cookbook/zip_dir.c](../examples/cookbook/zip_dir.c). CI builds and runs it and checks that it prints this.

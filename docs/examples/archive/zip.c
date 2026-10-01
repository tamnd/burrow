#include <stdio.h>

#include "burrow/burrow.h"
#include "burrow/mem/arena.h"

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;

    // doc: write
    BytesBuffer buf = BYTES_BUFFER(a);
    ZipWriter *zw = zip_new_writer(a, bytes_buffer_as_io_writer(&buf));
    if (zw == NULL)
        return 1;
    static const char *const files[][2] = {
        {"readme.txt", "This archive contains some text files."},
        {"gopher.txt", "Gopher names:\nGeorge\nGeoffrey\nGonzo"},
        {"todo.txt", "Get animal handling licence.\nWrite more examples."},
    };
    for (int i = 0; i < 3; i++) {
        IoWriter f = zip_writer_create(zw, str_from_cstr(files[i][0]), &err);
        if (BURROW_FAILED(err))
            return 1;
        io_write_string(f, str_from_cstr(files[i][1]), &err);
        if (BURROW_FAILED(err))
            return 1;
    }
    err = zip_writer_close(zw);
    zip_writer_free(zw);
    // doc: end
    if (BURROW_FAILED(err))
        return 1;

    // doc: read
    BytesReader br;
    bytes_reader_reset(&br, bytes_buffer_bytes(&buf));
    ZipReader *zr = zip_new_reader(a, bytes_reader_as_io_reader_at(&br),
                                   bytes_buffer_len(&buf), &err);
    if (zr == NULL)
        return 1;
    for (Int i = 0; i < zr->file.len; i++) {
        ZipFile *f = ((ZipFile **)zr->file.p)[i];
        printf("Contents of " BURROW_STR_FMT " (%d bytes, method %d):\n",
               BURROW_STR_ARG(f->file_header.name),
               (int)f->file_header.uncompressed_size64, f->file_header.method);
        IoReadCloser rc = zip_file_open(f, a, &err);
        if (BURROW_FAILED(err))
            return 1;
        Slice data = io_read_all(a, (IoReader){&rc.vt->reader, rc.data}, &err);
        rc.vt->closer.close(rc.data);
        if (BURROW_FAILED(err))
            return 1;
        printf("%.*s\n", (int)data.len, (const char *)data.p);
    }
    // doc: end

    // doc: fs
    Fs fsys = zip_reader_as_fs(zr);
    Slice todo = fs_read_file(a, fsys, BURROW_S("todo.txt"), &err);
    printf("%d bytes in todo.txt\n", (int)todo.len);
    fs_read_file(a, fsys, BURROW_S("missing.txt"), &err);
    printf(BURROW_STR_FMT "\n", BURROW_STR_ARG(error_text(err)));
    zip_reader_free(zr);
    // doc: end

    arena_free(&ar);
    return 0;
}

/* Output:
Contents of readme.txt (38 bytes, method 8):
This archive contains some text files.
Contents of gopher.txt (35 bytes, method 8):
Gopher names:
George
Geoffrey
Gonzo
Contents of todo.txt (49 bytes, method 8):
Get animal handling licence.
Write more examples.
49 bytes in todo.txt
open missing.txt: file does not exist
*/

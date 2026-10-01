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
    TarWriter *tw = tar_new_writer(a, bytes_buffer_as_io_writer(&buf));
    if (tw == NULL)
        return 1;
    static const char *const files[][2] = {
        {"readme.txt", "This archive contains some text files."},
        {"gopher.txt", "Gopher names:\nGeorge\nGeoffrey\nGonzo"},
        {"todo.txt", "Get animal handling license."},
    };
    for (int i = 0; i < 3; i++) {
        Str body = str_from_cstr(files[i][1]);
        TarHeader hdr = {
            .name = str_from_cstr(files[i][0]),
            .mode = 0600,
            .size = body.len,
        };
        err = tar_writer_write_header(tw, &hdr);
        if (BURROW_FAILED(err))
            return 1;
        tar_writer_write(tw, slice_from_str(a, body), &err);
        if (BURROW_FAILED(err))
            return 1;
    }
    err = tar_writer_close(tw);
    tar_writer_free(tw);
    // doc: end
    if (BURROW_FAILED(err))
        return 1;
    printf("%d bytes\n", (int)bytes_buffer_len(&buf));

    // doc: read
    TarReader *tr = tar_new_reader(a, bytes_buffer_as_io_reader(&buf));
    if (tr == NULL)
        return 1;
    for (;;) {
        TarHeader *hdr = tar_reader_next(tr, &err);
        if (errors_is(err, io_eof))
            break;
        if (BURROW_FAILED(err))
            return 1;
        printf("Contents of " BURROW_STR_FMT ":\n", BURROW_STR_ARG(hdr->name));
        Slice data = io_read_all(a, tar_reader_as_io_reader(tr), &err);
        printf("%.*s\n", (int)data.len, (const char *)data.p);
    }
    tar_reader_free(tr);
    // doc: end

    // doc: fileinfo
    TarHeader dir = {
        .typeflag = TAR_TYPE_DIR, .name = BURROW_S("src/cmd/"), .mode = 0755};
    FsFileInfo fi = tar_header_file_info(&dir);
    printf(BURROW_STR_FMT "\n", BURROW_STR_ARG(fs_format_file_info(a, fi)));
    TarHeader *back = tar_file_info_header(a, fi, (Str){NULL, 0}, &err);
    printf(BURROW_STR_FMT " %c\n", BURROW_STR_ARG(back->name), back->typeflag);
    // doc: end

    // doc: format
    TarHeader big = {.name = BURROW_S("big"), .uid = 1 << 21, .size = 0};
    tw = tar_new_writer(a, io_discard);
    err = tar_writer_write_header(tw, &big);
    printf("PAX ok: %s\n", BURROW_OK(err) ? "yes" : "no");
    big.format = TAR_FORMAT_USTAR;
    err = tar_writer_write_header(tw, &big);
    printf(BURROW_STR_FMT "\n", BURROW_STR_ARG(error_text(err)));
    tar_writer_free(tw);
    // doc: end

    arena_free(&ar);
    return 0;
}

/* Output:
4096 bytes
Contents of readme.txt:
This archive contains some text files.
Contents of gopher.txt:
Gopher names:
George
Geoffrey
Gonzo
Contents of todo.txt:
Get animal handling license.
drwxr-xr-x 0 0001-01-01 00:00:00 cmd/
cmd/ 5
PAX ok: yes
archive/tar: cannot encode header: Format specifies USTAR; and USTAR cannot encode Uid=2097152
*/

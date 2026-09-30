#include <stdio.h>
#include <string.h>

#include "burrow/burrow.h"
#include "burrow/compress/gzip.h"
#include "burrow/mem/arena.h"

static Slice text(const char *s) {
    return slice_from((void *)(uintptr_t)s, (Int)strlen(s), (Int)strlen(s), TYPE_BYTE);
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err;

    // doc: write
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
    // doc: end
    if (BURROW_FAILED(err))
        return 1;

    // doc: read
    GzipReader *zr = gzip_new_reader(a, bytes_buffer_as_io_reader(&buf), &err);
    if (zr == NULL)
        return 1;
    Str when = time_string(time_utc(zr->header.mod_time), a);
    printf("Name: %.*s\nComment: %.*s\nModTime: %.*s\n\n", (int)zr->header.name.len,
           (const char *)zr->header.name.p, (int)zr->header.comment.len,
           (const char *)zr->header.comment.p, (int)when.len, (const char *)when.p);
    Slice data = io_read_all(a, gzip_reader_as_io_reader(zr), &err);
    gzip_reader_free(zr);
    // doc: end
    if (BURROW_FAILED(err))
        return 1;
    printf("%.*s\n", (int)data.len, (const char *)data.p);

    // doc: multistream
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
    // doc: end
    if (!errors_is(err, io_eof))
        return 1;

    arena_free(&ar);
    return 0;
}

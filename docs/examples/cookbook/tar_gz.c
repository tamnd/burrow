// doc: main
#define BURROW_SHORT 1
#include "burrow/burrow.h"
#include "burrow/compress/gzip.h"

/* Writes the files to dst as a .tar.gz: the tar writer turns them into one
 * stream and the gzip writer under it compresses that. Both have to be closed,
 * the tar writer first, as each one writes its own ending. */
static Error pack(Alloc *a, IoWriter dst, const char *const (*files)[2], Int n) {
    GzipWriter *zw = gzip_new_writer(a, dst);
    TarWriter *tw = tar_new_writer(a, gzip_writer_as_io_writer(zw));
    Error err = NO_ERROR;
    for (Int i = 0; i < n && OK(err); i++) {
        Str body = str_from_cstr(files[i][1]);
        TarHeader hdr = {
            .name = str_from_cstr(files[i][0]), .mode = 0644, .size = body.len};
        err = tar_writer_write_header(tw, &hdr);
        if (OK(err))
            tar_writer_write(tw, slice_from_str(a, body), &err);
    }
    Error terr = tar_writer_close(tw);
    tar_writer_free(tw);
    Error zerr = gzip_writer_close(zw);
    gzip_writer_free(zw);
    if (FAILED(err))
        return err;
    return FAILED(terr) ? terr : zerr;
}

/* Lists what is in a .tar.gz, the way tar -tvzf does, more or less. */
static Error list(Alloc *a, IoReader src) {
    Error err = NO_ERROR;
    GzipReader *zr = gzip_new_reader(a, src, &err);
    if (zr == NULL)
        return err;
    TarReader *tr = tar_new_reader(a, gzip_reader_as_io_reader(zr));
    for (;;) {
        TarHeader *hdr = tar_reader_next(tr, &err);
        if (errors_is(err, io_eof)) {
            err = NO_ERROR;
            break;
        }
        if (FAILED(err))
            break;
        /* The reader gives the body of each file in turn. Reading it is
         * optional: next skips whatever is left. */
        Slice body = io_read_all(a, tar_reader_as_io_reader(tr), &err);
        if (FAILED(err))
            break;
        Str rest;
        bool found;
        Str first =
            strings_cut(str_from_bytes(body.p, body.len), S("\n"), &rest, &found);
        fmt_printf_v("%o %4d %-16s %q\n", hdr->mode, hdr->size, hdr->name, first);
    }
    tar_reader_free(tr);
    gzip_reader_free(zr);
    return err;
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    static const char *const files[][2] = {
        {"notes/todo.txt", "buy milk\nfix the bike\n"},
        {"notes/ideas.txt", "a cookbook for burrow\n"},
        {"README", "Notes, kept in plain text.\n"},
    };
    BytesBuffer buf = BYTES_BUFFER(a);
    Error err = pack(a, bytes_buffer_as_io_writer(&buf), files, 3);
    if (OK(err)) {
        /* tar pads each file to 512 bytes and ends with 1024 bytes of zeros,
         * which gzip squeezes almost to nothing. */
        fmt_println_v("packed into", bytes_buffer_len(&buf), "bytes");
        err = list(a, bytes_buffer_as_io_reader(&buf));
    }
    if (FAILED(err)) {
        fmt_println_v(err);
        return 1;
    }
    arena_free(&ar);
}
// doc: end

/* Output:
packed into 219 bytes
644   22 notes/todo.txt   "buy milk"
644   22 notes/ideas.txt  "a cookbook for burrow"
644   27 README           "Notes, kept in plain text."
*/

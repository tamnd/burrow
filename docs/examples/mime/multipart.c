#include "burrow/mime/multipart.h"
#include "burrow/burrow.h"
#include "burrow/mem/arena.h"

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err;

    // doc: write
    BytesBuffer body = BYTES_BUFFER(a);
    MultipartWriter *w = multipart_new_writer(a, bytes_buffer_as_io_writer(&body));
    err = multipart_writer_set_boundary(w, BURROW_S("xyz"));
    err = multipart_writer_write_field(w, BURROW_S("name"), BURROW_S("gopher"));
    IoWriter f = multipart_writer_create_form_file(w, BURROW_S("notes"),
                                                   BURROW_S("todo.txt"), &err);
    io_write_string(f, BURROW_S("feed the gopher\n"), &err);
    err = multipart_writer_close(w);
    fmt_printf_v("Content-Type: %s\n", multipart_writer_form_data_content_type(w, a));
    Slice out = bytes_buffer_bytes(&body);
    Str s = {(const Byte *)out.p, out.len};
    fmt_printf_v("%q\n", s);
    multipart_writer_free(w);
    // doc: end

    // doc: read
    BytesReader src;
    bytes_reader_reset(&src, out);
    MultipartReader *r =
        multipart_new_reader(a, bytes_reader_as_io_reader(&src), BURROW_S("xyz"));
    for (;;) {
        MultipartPart *p = multipart_reader_next_part(r, &err);
        if (p == NULL)
            break;
        Slice data = io_read_all(a, multipart_part_as_io_reader(p), &err);
        s = (Str){(const Byte *)data.p, data.len};
        fmt_printf_v("part %q file %q: %q\n", multipart_part_form_name(p),
                     multipart_part_file_name(p), s);
    }
    fmt_printf_v("end: %v\n", err);
    multipart_reader_free(r);
    // doc: end

    // doc: form
    bytes_reader_reset(&src, out);
    r = multipart_new_reader(a, bytes_reader_as_io_reader(&src), BURROW_S("xyz"));
    MultipartForm *form = multipart_reader_read_form(r, 1 << 20, &err);
    Str key = BURROW_S("name");
    Slice names = *(Slice *)map_get(form->value, &key);
    fmt_printf_v("name = %q\n", ((Str *)names.p)[0]);
    key = BURROW_S("notes");
    Slice files = *(Slice *)map_get(form->file, &key);
    MultipartFileHeader *fh = ((MultipartFileHeader **)files.p)[0];
    fmt_printf_v("notes: %q, %d bytes, %s\n", fh->filename, fh->size,
                 textproto_mime_header_get(fh->header, BURROW_S("Content-Type")));
    MultipartFile *file = multipart_file_header_open(fh, a, &err);
    Slice text = io_read_all(a, multipart_file_as_io_reader(file), &err);
    s = (Str){(const Byte *)text.p, text.len};
    fmt_printf_v("contents: %q\n", s);
    multipart_file_close(file);
    err = multipart_form_remove_all(form);
    multipart_form_free(form);
    multipart_reader_free(r);
    // doc: end

    arena_free(&ar);
    return 0;
}

/* Output:
Content-Type: multipart/form-data; boundary=xyz
"--xyz\r\nContent-Disposition: form-data; name=\"name\"\r\n\r\ngopher\r\n--xyz\r\nContent-Disposition: form-data; name=\"notes\"; filename=\"todo.txt\"\r\nContent-Type: application/octet-stream\r\n\r\nfeed the gopher\n\r\n--xyz--\r\n"
part "name" file "": "gopher"
part "notes" file "todo.txt": "feed the gopher\n"
end: EOF
name = "gopher"
notes: "todo.txt", 16 bytes, application/octet-stream
contents: "feed the gopher\n"
*/

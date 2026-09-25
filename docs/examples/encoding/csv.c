#include <stdio.h>

#include "burrow/burrow.h"
#include "burrow/encoding/csv.h"
#include "burrow/mem/arena.h"

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    // doc: read
    StringsReader sr;
    strings_reader_reset(&sr, BURROW_S("name,city\n"
                                       "\"Pike, Rob\",Sydney\n"
                                       "Ken,\"New\nJersey\"\n"));
    CsvReader *r = csv_new_reader(a, strings_reader_as_io_reader(&sr));
    for (;;) {
        Error err = BURROW_NO_ERROR;
        Slice rec = csv_reader_read(r, a, &err);
        if (errors_is(err, io_eof))
            break;
        Int col;
        Int line = csv_reader_field_pos(r, 1, &col);
        fmt_printf_v("%q at %d:%d\n", rec, line, col);
    }
    csv_reader_free(r);
    // doc: end

    // doc: bad
    strings_reader_reset(&sr, BURROW_S("a,b\nc,d,e\n"));
    r = csv_new_reader(a, strings_reader_as_io_reader(&sr));
    Error err = BURROW_NO_ERROR;
    Slice all = csv_reader_read_all(r, a, &err);
    const CsvParseError *pe = errors_as(err, TYPE_CSV_PARSE_ERROR);
    // doc: end
    printf("all: %d records\n", (int)all.len);
    printf("line %d column %d, field count: %d\n", (int)pe->line, (int)pe->column,
           errors_is(err, csv_err_field_count));
    printf("err: " BURROW_STR_FMT "\n", BURROW_STR_ARG(error_text(err)));
    csv_reader_free(r);

    // doc: write
    BytesBuffer out = BYTES_BUFFER(a);
    CsvWriter *w = csv_new_writer(a, bytes_buffer_as_io_writer(&out));
    Str row1[] = {BURROW_S("id"), BURROW_S("quote")};
    Str row2[] = {BURROW_S("1"), BURROW_S("say \"hi\", then go")};
    csv_writer_write(w, slice_from(row1, 2, 2, TYPE_STRING));
    csv_writer_write(w, slice_from(row2, 2, 2, TYPE_STRING));
    csv_writer_flush(w);
    err = csv_writer_error(w);
    csv_writer_free(w);
    // doc: end
    Str text = bytes_buffer_string(&out, a);
    printf(BURROW_STR_FMT, BURROW_STR_ARG(text));
    printf("err: %d\n", BURROW_FAILED(err));

    arena_free(&ar);
    return 0;
}

/* Output:
["name" "city"] at 1:6
["Pike, Rob" "Sydney"] at 2:13
["Ken" "New\nJersey"] at 3:5
all: 0 records
line 2 column 1, field count: 1
err: record on line 2: wrong number of fields
id,quote
1,"say ""hi"", then go"
err: 0
*/

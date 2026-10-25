// doc: main
#define BURROW_SHORT 1
#include "burrow/burrow.h"
#include "burrow/encoding/csv.h"

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    /* The file to read. A real program would be handed one. */
    Error err = os_write_file(S("sales.csv"),
                              BURROW_B("city,amount\n"
                                       "Sydney,12.50\n"
                                       "Oslo,3\n"
                                       "Sydney,7.25\n"
                                       "Lima,10\n"
                                       "Oslo,4.75\n"),
                              0644);
    OsFile *f = os_open(a, S("sales.csv"), &err);
    if (FAILED(err)) {
        fmt_println_v(err);
        return 1;
    }

    CsvReader *r = csv_new_reader(a, os_file_as_io_reader(f));
    Map *totals = map_make(a, TYPE_STRING, TYPE_FLOAT64, 0);
    csv_reader_read(r, a, &err); /* the header line */
    for (;;) {
        Slice rec = csv_reader_read(r, a, &err);
        if (errors_is(err, io_eof))
            break;
        if (FAILED(err)) {
            fmt_println_v(err);
            return 1;
        }
        Str city = AT(Str, rec, 0);
        double amount = strconv_parse_float(AT(Str, rec, 1), 64, &err);
        if (FAILED(err)) {
            fmt_println_v(err);
            return 1;
        }
        double *sum = MAP_GET(Str, double, totals, city);
        MAP_SET(Str, double, totals, city, sum != NULL ? *sum + amount : amount);
    }
    fmt_println_v(totals);

    csv_reader_free(r);
    os_file_close(f);
    arena_free(&ar);
}
// doc: end

/* Output:
map[Lima:10 Oslo:7.75 Sydney:19.75]
*/

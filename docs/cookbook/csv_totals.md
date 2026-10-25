# Add up a column of a CSV file

Read a CSV file a record at a time and total one column by another. The header line is read and dropped, each amount goes through `strconv_parse_float`, and the sums go in a map from city to total. `fmt_println_v` prints a map with its keys sorted, the way Go's fmt does, so the output is the same every run.

<!-- example: ../examples/cookbook/csv_totals.c#main -->
```c
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
```

It prints:

```text
map[Lima:10 Oslo:7.75 Sydney:19.75]
```

The program is [docs/examples/cookbook/csv_totals.c](../examples/cookbook/csv_totals.c). CI builds and runs it and checks that it prints this.

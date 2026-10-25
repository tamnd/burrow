# Write a CSV file

A `CsvWriter` takes each record as a Slice of Str and quotes only the fields that need it: one with the separator, a quote or a line break in it. Output is buffered, so call `csv_writer_flush` at the end and then `csv_writer_error` to find out whether every write got through. Setting `comma` to a tab gives tab separated output.

<!-- example: ../examples/cookbook/csv_write.c#main -->
```c
#define BURROW_SHORT 1
#include "burrow/burrow.h"

typedef struct Book {
    const char *title;
    const char *author;
    Int year;
    double price;
} Book;

/* Writes the books as CSV, a header first. Each record is a Slice of Str, so
 * the numbers are turned into text before they go in. */
static Error write_books(Alloc *a, IoWriter dst, Rune comma, const Book *books, Int n) {
    CsvWriter *w = csv_new_writer(a, dst);
    w->comma = comma;
    static Str header[] = {BURROW_S_INIT("title"), BURROW_S_INIT("author"),
                           BURROW_S_INIT("year"), BURROW_S_INIT("price")};
    csv_writer_write(w, slice_from((void *)(uintptr_t)header, 4, 4, TYPE_STRING));
    for (Int i = 0; i < n; i++) {
        Str row[] = {
            str_from_cstr(books[i].title),
            str_from_cstr(books[i].author),
            strconv_itoa(a, books[i].year),
            strconv_format_float(a, books[i].price, 'f', 2, 64),
        };
        csv_writer_write(w, slice_from(row, 4, 4, TYPE_STRING));
    }
    /* Nothing reaches dst until the flush, and a write that failed only
     * shows up in csv_writer_error. */
    csv_writer_flush(w);
    Error err = csv_writer_error(w);
    csv_writer_free(w);
    return err;
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    /* A field with the separator, a quote or a line break in it is put in
     * quotes, and a quote inside is doubled. The rest go as they are. */
    static const Book books[] = {
        {"The C Programming Language", "Kernighan, Ritchie", 1978, 45.5},
        {"The \"Dragon\" Book", "Aho", 1986, 99},
        {"Go", "Donovan\nKernighan", 2015, 39.99},
    };
    IoWriter out = os_file_as_io_writer(os_stdout);
    Error err = write_books(a, out, ',', books, 3);
    if (OK(err)) {
        fmt_fprintf_v(out, "\n"); /* to out too, so it comes out in order */
        /* With tabs between the fields, a comma no longer needs quotes. */
        err = write_books(a, out, '\t', books, 1);
    }
    if (FAILED(err)) {
        fmt_println_v(err);
        return 1;
    }
    arena_free(&ar);
}
```

It prints:

```text
title,author,year,price
The C Programming Language,"Kernighan, Ritchie",1978,45.50
"The ""Dragon"" Book",Aho,1986,99.00
Go,"Donovan
Kernighan",2015,39.99

title	author	year	price
The C Programming Language	Kernighan, Ritchie	1978	45.50
```

The program is [docs/examples/cookbook/csv_write.c](../examples/cookbook/csv_write.c). CI builds and runs it and checks that it prints this.

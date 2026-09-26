#include <stdio.h>

#include "burrow/burrow.h"

/* Standard output as an IoWriter, until os.Stdout arrives. */
static Int stdout_write(void *self, Slice p, Error *err) {
    (void)self;
    (void)err;
    return (Int)fwrite(p.p, 1, (size_t)p.len, stdout);
}

static const IoWriterVT stdout_vt = {NULL, stdout_write};

static void table(Alloc *a, IoWriter out) {
    // doc: table
    TabwriterWriter *w = tabwriter_new_writer(a, out, 0, 8, 2, ' ', 0);
    IoWriter tw = tabwriter_writer_as_io_writer(w);
    fmt_fprintln_v(tw, "NAME\tSIZE\tKIND");
    fmt_fprintf_v(tw, "%s\t%d\t%s\n", "burrow.h", 2048, "header");
    fmt_fprintf_v(tw, "%s\t%d\t%s\n", "libburrow.a", 1843200, "archive");
    fmt_fprintf_v(tw, "%s\t%d\t%s\n", "README.md", 512, "text");
    Error err = tabwriter_writer_flush(w);
    tabwriter_writer_free(w);
    // doc: end
    (void)err;
}

static void right(Alloc *a, IoWriter out) {
    // doc: right
    TabwriterWriter *w = tabwriter_new_writer(a, out, 5, 0, 1, ' ',
                                              TABWRITER_ALIGN_RIGHT | TABWRITER_DEBUG);
    IoWriter tw = tabwriter_writer_as_io_writer(w);
    fmt_fprintln_v(tw, "a\tb\tc\t");
    fmt_fprintln_v(tw, "123\t12345\t1234567\t");
    tabwriter_writer_flush(w);
    tabwriter_writer_free(w);
    // doc: end
}

static void elastic(Alloc *a, IoWriter out) {
    // doc: elastic
    TabwriterWriter *w = tabwriter_new_writer(a, out, 0, 0, 1, '.', TABWRITER_DEBUG);
    IoWriter tw = tabwriter_writer_as_io_writer(w);
    fmt_fprintln_v(tw, "a\tb\tc");
    fmt_fprintln_v(tw, "aa\tbb\tcc");
    fmt_fprintln_v(tw, "aaa\t"); /* no cell in column two, so the b column ends */
    fmt_fprintln_v(tw, "aaaa\tdddd\teeee");
    tabwriter_writer_flush(w);
    tabwriter_writer_free(w);
    // doc: end
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    IoWriter out = {&stdout_vt, NULL};
    table(a, out);
    right(a, out);
    elastic(a, out);
    arena_free(&ar);
    return 0;
}

/* Output:
NAME         SIZE     KIND
burrow.h     2048     header
libburrow.a  1843200  archive
README.md    512      text
    a|     b|       c|
  123| 12345| 1234567|
a....|b..|c
aa...|bb.|cc
aaa..|
aaaa.|dddd.|eeee
*/

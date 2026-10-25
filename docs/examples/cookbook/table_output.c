// doc: main
#define BURROW_SHORT 1
#include "burrow/burrow.h"

typedef struct Row {
    const char *name;
    Int size;
    const char *kind;
} Row;

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    static const Row rows[] = {
        {"burrow.h", 2048, "header"},
        {"libburrow.a", 1843200, "archive"},
        {"README.md", 512, "text"},
    };

    /* Cells end at a tab. The writer holds the lines until the flush, then
     * pads every column to its widest cell plus two spaces. */
    TabwriterWriter *w =
        tabwriter_new_writer(a, os_file_as_io_writer(os_stdout), 0, 8, 2, ' ', 0);
    IoWriter tw = tabwriter_writer_as_io_writer(w);
    fmt_fprintln_v(tw, "NAME\tSIZE\tKIND");
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++)
        fmt_fprintf_v(tw, "%s\t%d\t%s\n", str_from_cstr(rows[i].name), rows[i].size,
                      str_from_cstr(rows[i].kind));
    Error err = tabwriter_writer_flush(w);
    tabwriter_writer_free(w);
    if (FAILED(err)) {
        fmt_println_v(err);
        return 1;
    }
    arena_free(&ar);
}
// doc: end

/* Output:
NAME         SIZE     KIND
burrow.h     2048     header
libburrow.a  1843200  archive
README.md    512      text
*/

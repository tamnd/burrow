/* The README's blocks that use os. Each program starts in an empty directory,
 * so the files it makes are its own. */
#define BURROW_SHORT 1
#include <stdio.h>

#include "burrow/burrow.h"

static Error write_and_remove(Str path, Slice data) {
    // doc: errors
    Error err = os_write_file(path, data, 0644);
    if (FAILED(err))
        return err;

    err = os_remove(S("missing.txt"));
    if (errors_is(err, os_err_not_exist))
        puts("missing.txt was not there");
    // doc: end

    // doc: errors-as
    const OsPathError *pe = errors_as(err, TYPE_OS_PATH_ERROR);
    // doc: end
    if (pe != NULL)
        printf(STR_FMT " failed on " STR_FMT "\n", STR_ARG(pe->op), STR_ARG(pe->path));
    return NO_ERROR;
}

static void close_file(void *f) {
    Error err = os_file_close(f);
    if (FAILED(err))
        printf("close: " STR_FMT "\n", STR_ARG(error_text(err)));
}

static Error read_header(OsFile *f) {
    Byte buf[5];
    Error err = NO_ERROR;
    Int n = os_file_read(f, slice_from(buf, 5, 5, TYPE_BYTE), &err);
    printf("header %.*s\n", (int)n, (const char *)buf);
    return err;
}

static Error header(Alloc *a, Str path) {
    Error err = NO_ERROR;
    // doc: defer
    BURROW_SCOPE {
        OsFile *f = os_open(a, path, &err);
        if (BURROW_FAILED(err))
            return err;
        BURROW_DEFER(close_file, f);

        err = read_header(f);
    }
    BURROW_SCOPE_END;
    // doc: end
    return err;
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    static Byte text[] = "burrow, a file of bytes\n";
    Int n = (Int)sizeof text - 1;
    Error err = write_and_remove(S("notes.txt"), slice_from(text, n, n, TYPE_BYTE));
    if (FAILED(err))
        printf("write: " STR_FMT "\n", STR_ARG(error_text(err)));
    err = header(a, S("notes.txt"));
    if (FAILED(err))
        printf("header: " STR_FMT "\n", STR_ARG(error_text(err)));
    arena_free(&ar);
    return 0;
}

/* Output:
missing.txt was not there
remove failed on missing.txt
header burro
*/

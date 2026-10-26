/* The README's blocks that use strings and os. The README uses the short
 * spellings, so this does too. */
#define BURROW_SHORT 1
#include <stdio.h>

#include "burrow/burrow.h"

static void alloc(Alloc *a) {
    Str name = S("gopher"), path = S("usr/local/bin");

    // doc: alloc
    Str upper = strings_to_upper(a, name);
    Slice parts = strings_split(a, path, S("/"));
    // doc: end
    printf(STR_FMT " %lld\n", STR_ARG(upper), (long long)parts.len);
}

static Error copy_config(Alloc *a, Str from, Str to) {
    Error err = NO_ERROR;

    // doc: errors
    Slice data = os_read_file(a, from, &err);
    if (errors_is(err, os_err_not_exist))
        return NO_ERROR; /* nothing to copy */
    if (FAILED(err))
        return err;

    err = os_write_file(to, data, 0644);
    if (FAILED(err))
        return err;
    // doc: end
    return NO_ERROR;
}

/* os_file_close in the shape a deferred call takes. */
static void close_file(void *f) {
    (void)os_file_close(f);
}

static Error read_header(OsFile *f) {
    Byte b[4];
    Error err = NO_ERROR;
    Int n = os_file_read(f, (Slice){b, 4, 4, TYPE_BYTE}, &err);
    printf("header %.*s\n", (int)n, (const char *)b);
    return err;
}

static Error check_header(Alloc *a, Str path) {
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

static void run(void *env) {
    (void)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    alloc(a);

    Str from = S("readme-files-in.txt"), to = S("readme-files-out.txt");
    Error err = copy_config(a, from, to);
    printf("missing source, failed: %s\n", FAILED(err) ? "yes" : "no");

    err = os_write_file(from, BURROW_B("BRWdata"), 0644);
    if (!FAILED(err))
        err = copy_config(a, from, to);
    printf("copied, failed: %s\n", FAILED(err) ? "yes" : "no");

    err = check_header(a, to);
    printf("header, failed: %s\n", FAILED(err) ? "yes" : "no");

    err = check_header(a, S("readme-files-missing.txt"));
    // doc: errors-as
    const OsPathError *pe = errors_as(err, TYPE_OS_PATH_ERROR);
    // doc: end
    if (pe != NULL)
        printf("op " STR_FMT ", path " STR_FMT "\n", STR_ARG(pe->op),
               STR_ARG(pe->path));

    (void)os_remove(from);
    (void)os_remove(to);
    arena_free(&ar);
}

int main(void) {
    runtime_main(BURROW_FN(Func, run, NULL));
    return 0;
}

/* Output:
GOPHER 3
missing source, failed: no
copied, failed: no
header BRWd
header, failed: no
op open, path readme-files-missing.txt
*/

#include <stdio.h>

#include "burrow/burrow.h"
#include "burrow/mem/arena.h"
#include "burrow/os.h"

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    // doc: env
    Error err = os_setenv(BURROW_S("GREETING"), BURROW_S("hello"));
    bool found = false;
    Str g = os_lookup_env(a, BURROW_S("GREETING"), &found); /* "hello", true */
    Str msg = os_expand_env(a, BURROW_S("$GREETING, ${USER_NAME}!"));
    /* "hello, !" since USER_NAME is not set */
    err = os_unsetenv(BURROW_S("GREETING"));
    // doc: end
    if (BURROW_FAILED(err) || !found)
        return 1;
    printf(BURROW_STR_FMT "\n" BURROW_STR_FMT "\n", BURROW_STR_ARG(g),
           BURROW_STR_ARG(msg));

    // doc: wd
    Str wd = os_getwd(a, &err);
    Str tmp = os_temp_dir(a);
    err = os_chdir(tmp); /* relative names now start from tmp */
    Error back = os_chdir(wd);
    // doc: end
    if (BURROW_FAILED(err) || BURROW_FAILED(back))
        return 1;

    // doc: pipe
    OsFile *w = NULL;
    OsFile *r = os_pipe(a, &w, &err);
    if (BURROW_FAILED(err))
        return 1;
    os_file_write_string(w, BURROW_S("through the pipe"), &err);
    Error cerr = os_file_close(w); /* the reader sees EOF after the data */
    Byte buf[32];
    Int n = os_file_read(r, slice_from(buf, 32, 32, TYPE_BYTE), &err);
    // doc: end
    if (BURROW_FAILED(cerr))
        return 1;
    printf("%.*s\n", (int)n, (const char *)buf);
    (void)os_file_close(r);
    os_file_free(r);
    os_file_free(w);

    arena_free(&ar);
    return 0;
}

/* Output:
hello
hello, !
through the pipe
*/

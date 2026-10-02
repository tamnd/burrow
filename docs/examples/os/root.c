#include <stdio.h>

#include "burrow/burrow.h"
#include "burrow/fmt.h"
#include "burrow/io/fs.h"
#include "burrow/mem/arena.h"
#include "burrow/os.h"

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    Str dir = os_mkdir_temp(a, BURROW_S(""), BURROW_S("example-*"), &err);
    if (BURROW_FAILED(err))
        return 1;

    // doc: openroot
    OsRoot *root = os_open_root(a, dir, &err);
    err = os_root_mkdir_all(root, BURROW_S("logs/old"), 0755);
    err = os_root_write_file(root, BURROW_S("logs/today.txt"), BURROW_B("started\n"), 0644);
    Slice data = os_root_read_file(root, a, BURROW_S("logs/today.txt"), &err);
    /* "started\n" */
    // doc: end
    if (BURROW_FAILED(err))
        return 1;
    printf("%.*s", (int)data.len, (const char *)data.p);

    // doc: escape
    Error out = BURROW_NO_ERROR, link_out = BURROW_NO_ERROR;
    OsFile *f = os_root_open(root, a, BURROW_S("../secret.txt"), &out);
    /* NULL, openat ../secret.txt: path escapes from parent */
    os_root_readlink(root, a, BURROW_S("logs/../../x"), &link_out);
    /* readlinkat logs/../../x: path escapes from parent */
    // doc: end
    if (f != NULL)
        return 1;
    printf(BURROW_STR_FMT "\n" BURROW_STR_FMT "\n", BURROW_STR_ARG(error_text(out)),
           BURROW_STR_ARG(error_text(link_out)));

    // doc: rootfs
    Fs fsys = os_root_fs(root);
    Slice entries = fs_read_dir(a, fsys, BURROW_S("logs"), &err);
    for (Int i = 0; i < entries.len; i++) {
        FsDirEntry *e = (FsDirEntry *)slice_at(entries, i);
        Str entry = fs_format_dir_entry(a, *e); /* "d old/", then "- today.txt" */
        printf(BURROW_STR_FMT "\n", BURROW_STR_ARG(entry));
    }
    // doc: end
    if (BURROW_FAILED(err))
        return 1;

    // doc: close
    err = os_root_close(root); /* later calls fail with "file already closed" */
    os_root_free(root);
    // doc: end

    err = os_remove_all(dir);
    arena_free(&ar);
    return BURROW_OK(err) ? 0 : 1;
}

/* Output:
started
openat ../secret.txt: path escapes from parent
readlinkat logs/../../x: path escapes from parent
d old/
- today.txt
*/

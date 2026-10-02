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

    // doc: temp
    Str dir = os_mkdir_temp(a, BURROW_S(""), BURROW_S("example-*"), &err);
    if (BURROW_FAILED(err))
        return 1;
    /* dir is something like /tmp/example-2851094712 */
    // doc: end

    // doc: mkdirall
    Str deep = fmt_sprintf_v(a, "%s%ca%cb%cc", dir, (Int)OS_PATH_SEPARATOR,
                             (Int)OS_PATH_SEPARATOR, (Int)OS_PATH_SEPARATOR);
    err = os_mkdir_all(deep, 0755);         /* makes a, a/b and a/b/c */
    Error again = os_mkdir_all(deep, 0755); /* nil, it is already there */
    // doc: end
    if (BURROW_FAILED(err) || BURROW_FAILED(again))
        return 1;
    Str notes = fmt_sprintf_v(a, "%s%cnotes.txt", dir, (Int)OS_PATH_SEPARATOR);
    if (BURROW_FAILED(os_write_file(notes, BURROW_B("hello\n"), 0644)))
        return 1;

    // doc: readdir
    Slice entries = os_read_dir(a, dir, &err);
    for (Int i = 0; i < entries.len; i++) {
        FsDirEntry *e = (FsDirEntry *)slice_at(entries, i);
        Str entry = fs_format_dir_entry(a, *e); /* "d a/", then "- notes.txt" */
        printf(BURROW_STR_FMT "\n", BURROW_STR_ARG(entry));
    }
    // doc: end
    if (BURROW_FAILED(err))
        return 1;

    // doc: dirfs
    Fs fsys = os_dir_fs(a, dir);
    Slice data = fs_read_file(a, fsys, BURROW_S("notes.txt"), &err); /* "hello\n" */
    Error out = BURROW_NO_ERROR;
    fs_read_file(a, fsys, BURROW_S("../notes.txt"), &out);
    /* readfile ../notes.txt: invalid argument */
    // doc: end
    printf("%.*s" BURROW_STR_FMT "\n", (int)data.len, (const char *)data.p,
           BURROW_STR_ARG(error_text(out)));

    // doc: removeall
    err = os_remove_all(dir); /* dir and everything under it */
    // doc: end

    arena_free(&ar);
    return BURROW_OK(err) ? 0 : 1;
}

/* Output:
d a/
- notes.txt
hello
readfile ../notes.txt: invalid argument
*/

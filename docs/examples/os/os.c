#include <stdio.h>
#include <string.h>

#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/fmt.h"
#include "burrow/mem/arena.h"
#include "burrow/os.h"
#include "burrow/pal.h"

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    /* A directory of our own to work in. burrow has no MkdirTemp yet. */
    char tmp[512];
    if (pal_temp_dir(tmp, (int64_t)sizeof tmp, NULL) < 0)
        return 1;
    Str dir = fmt_sprintf_v(a, "%s%cburrow-os-example-%d", str_from_cstr(tmp),
                            (Int)OS_PATH_SEPARATOR, (Int)pal_getpid());
    if (BURROW_FAILED(os_mkdir(dir, 0700)))
        return 1;
    Str name = fmt_sprintf_v(a, "%s%cnotes.txt", dir, (Int)OS_PATH_SEPARATOR);

    // doc: whole
    Error err = os_write_file(name, BURROW_B("one\ntwo\n"), 0644);
    if (BURROW_FAILED(err))
        return 1;
    Slice data = os_read_file(a, name, &err); /* "one\ntwo\n", from a */
    // doc: end
    printf("%d %.*s", BURROW_OK(err), (int)data.len, (const char *)data.p);

    // doc: file
    OsFile *f = os_open_file(a, name, OS_O_RDWR | OS_O_APPEND, 0, &err);
    if (f == NULL)
        return 1;
    os_file_write_string(f, BURROW_S("three\n"), &err);
    os_file_seek(f, 4, OS_SEEK_SET, &err);
    Byte buf[64];
    Int n =
        os_file_read(f, slice_from(buf, 64, 64, TYPE_BYTE), &err); /* "two\nthree\n" */
    Error cerr = os_file_close(f);
    os_file_free(f);
    // doc: end
    printf("%.*s%d\n", (int)n, (const char *)buf, BURROW_OK(cerr));

    // doc: stat
    OsFileInfo fi = os_stat(a, name, &err);
    if (BURROW_FAILED(err))
        return 1;
    Str base = fi.vt->name(fi.data);      /* "notes.txt" */
    int64_t size = fi.vt->size(fi.data);  /* 14 */
    bool is_dir = fi.vt->is_dir(fi.data); /* false */
    // doc: end
    printf("%.*s %lld %d\n", (int)base.len, (const char *)base.p, (long long)size,
           is_dir);

    // doc: errors
    Error rerr = os_remove(name);  /* gone */
    Error again = os_remove(name); /* remove .../notes.txt: no such file or directory */
    bool missing = os_is_not_exist(again);          /* true */
    bool same = errors_is(again, os_err_not_exist); /* true */
    // doc: end
    printf("%d %d %d\n", BURROW_OK(rerr), missing, same);

    os_remove(dir);
    arena_free(&ar);
    return missing && same ? 0 : 1;
}

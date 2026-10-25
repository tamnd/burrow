// doc: main
#define BURROW_SHORT 1
#include "burrow/burrow.h"

/* Replaces the file at path with data so that anyone reading it sees either
 * the old contents or the new, never half of each. The new contents go to a
 * temporary file in the same directory, and a rename puts it in place. The
 * same directory matters: a rename can't move a file to another disk. */
static Error write_file_atomic(Alloc *a, Str path, Slice data) {
    Error err = NO_ERROR;
    OsFile *f = os_create_temp(a, filepath_dir(a, path), S(".tmp-*"), &err);
    if (FAILED(err))
        return err;
    Str tmp = os_file_name(f);
    os_file_write(f, data, &err);
    /* Sync makes sure the bytes are on the disk before the rename, so a
     * crash can't leave a file that has the new name and no contents. */
    if (OK(err))
        err = os_file_sync(f);
    Error cerr = os_file_close(f);
    if (OK(err))
        err = cerr;
    if (OK(err))
        err = os_rename(tmp, path);
    if (FAILED(err))
        os_remove(tmp); /* don't leave it lying around */
    return err;
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    Error err = os_mkdir_all(S("state"), 0755);
    if (OK(err))
        err = write_file_atomic(a, S("state/count.txt"), BURROW_B("1\n"));
    if (OK(err))
        err = write_file_atomic(a, S("state/count.txt"), BURROW_B("2\n"));
    if (FAILED(err)) {
        fmt_println_v(err);
        return 1;
    }
    Slice got = os_read_file(a, S("state/count.txt"), &err);
    fmt_printf_v("count.txt holds %q\n", str_from_bytes(got.p, got.len));

    /* Only the one file is left: each temporary file became count.txt. */
    Slice entries = os_read_dir(a, S("state"), &err);
    fmt_println_v("files in state:", entries.len);

    /* A directory that doesn't exist fails before anything is written. */
    err = write_file_atomic(a, S("nowhere/count.txt"), BURROW_B("3\n"));
    bool missing = errors_is(err, fs_err_not_exist);
    fmt_printf_v("into a missing directory: not exist is %t\n", missing);
    os_remove_all(S("state"));
    arena_free(&ar);
}
// doc: end

/* Output:
count.txt holds "2\n"
files in state: 1
into a missing directory: not exist is true
*/

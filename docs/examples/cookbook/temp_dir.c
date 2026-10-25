// doc: main
#define BURROW_SHORT 1
#include "burrow/burrow.h"
#include "burrow/path/filepath.h"

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    /* A new directory with a name nobody else has, under the system's
     * temporary directory. The * in the pattern is where the random part
     * goes. */
    Error err = NO_ERROR;
    Str dir = os_mkdir_temp(a, S(""), S("cookbook-*"), &err);
    if (FAILED(err)) {
        fmt_println_v(err);
        return 1;
    }
    bool prefixed = strings_has_prefix(filepath_base(dir), S("cookbook-"));
    fmt_println_v("made a cookbook- directory:", prefixed);

    os_write_file(filepath_join_v(a, 2, dir, S("b.txt")), BURROW_B("bee\n"), 0644);
    os_write_file(filepath_join_v(a, 2, dir, S("a.txt")), BURROW_B("ay\n"), 0644);
    os_mkdir(filepath_join_v(a, 2, dir, S("sub")), 0755);

    /* os_read_dir gives the entries sorted by name. */
    Slice entries = os_read_dir(a, dir, &err);
    for (Int i = 0; i < entries.len; i++) {
        FsDirEntry e = AT(FsDirEntry, entries, i);
        bool is_dir = e.vt->is_dir(e.data);
        fmt_println_v(e.vt->name(e.data), is_dir);
    }

    /* Take it all away again, and check that it went. */
    err = os_remove_all(dir);
    os_stat(a, dir, &err);
    bool gone = errors_is(err, os_err_not_exist);
    fmt_println_v("gone:", gone);
    arena_free(&ar);
}
// doc: end

/* Output:
made a cookbook- directory: true
a.txt false
b.txt false
sub true
gone: true
*/

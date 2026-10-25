// doc: main
#define BURROW_SHORT 1
#include "burrow/burrow.h"

BURROW_SENTINEL_ERROR(err_no_user, "no such user");

static Error find_user(Str name) {
    if (str_eq(name, S("ada")))
        return NO_ERROR;
    return err_no_user;
}

/* Each layer says what it was doing and keeps the error under it with %w, so
 * the message reads from the outside in and errors_is can still see the
 * cause. */
static Error load_profile(Str name) {
    Error err = find_user(name);
    if (FAILED(err))
        return fmt_errorf_v("load profile %q: %w", name, err);
    return NO_ERROR;
}

static Error read_settings(Alloc *a, Str file) {
    Error err = NO_ERROR;
    os_read_file(a, file, &err);
    if (FAILED(err))
        return fmt_errorf_v("read settings: %w", err);
    return NO_ERROR;
}

static void explain(Error err) {
    fmt_println_v("error:", err);
    bool no_user = errors_is(err, err_no_user);
    bool not_exist = errors_is(err, fs_err_not_exist);
    fmt_printf_v("  no such user: %t, file missing: %t\n", no_user, not_exist);

    /* errors_as finds the first error of a type in the chain, here the one
     * os made, which has the operation and the path in it. */
    const OsPathError *pe = errors_as(err, TYPE_OS_PATH_ERROR);
    if (pe != NULL)
        fmt_printf_v("  the path was %q\n", pe->path);
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    Error err = load_profile(S("ada"));
    fmt_println_v("ada:", err);
    explain(load_profile(S("bob")));
    explain(read_settings(a, S("settings.toml")));
    arena_free(&ar);
}
// doc: end

/* Output:
ada: <nil>
error: load profile "bob": no such user
  no such user: true, file missing: false
error: read settings: open settings.toml: no such file or directory
  no such user: false, file missing: true
  the path was "settings.toml"
*/

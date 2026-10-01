#include <stdio.h>

#include "burrow/burrow.h"
#include "burrow/io/fs.h"
#include "burrow/mem/arena.h"
#include "burrow/syscall.h"

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    // doc: errno
    Error err = fs_path_error_new(a, BURROW_S("mkdir"), BURROW_S("/tmp/cache"),
                                  syscall_errno_as_error(SYSCALL_EEXIST, a));
    Str text = error_text(err);                 /* mkdir /tmp/cache: file exists */
    bool exists = errors_is(err, fs_err_exist); /* true */
    const SyscallErrno *e = (const SyscallErrno *)errors_as(err, TYPE_SYSCALL_ERRNO);
    bool same = e != NULL && *e == SYSCALL_EEXIST; /* true */
    // doc: end
    printf("%.*s\n%d %d\n", (int)text.len, (const char *)text.p, exists, same);

    // doc: pal
    PalErrno perr = PAL_OK;
    int64_t fd = pal_open("no/such/dir/file", PAL_O_RDONLY, 0, &perr);
    SyscallErrno code =
        syscall_errno_from_pal(perr); /* ENOENT, or ERROR_PATH_NOT_FOUND on Windows */
    bool missing = syscall_errno_is(code, fs_err_not_exist); /* true */
    // doc: end
    printf("%d %d\n", fd == PAL_INVALID_HANDLE, missing);

    arena_free(&ar);
    return exists && same && missing ? 0 : 1;
}

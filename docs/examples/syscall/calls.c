#include <stdio.h>

#include "burrow/burrow.h"
#include "burrow/syscall.h"

int main(void) {
#if !defined(BURROW_OS_WINDOWS) && !defined(BURROW_OS_WASI) && !defined(BURROW_OS_COSMO)
    // doc: getpid
    Int pid = syscall_getpid();
    // doc: end
    // doc: chdir
    Error err = syscall_chdir(BURROW_S("/no/such/dir"));
    const SyscallErrno *e = errors_as(err, TYPE_SYSCALL_ERRNO);
    if (e != NULL && *e == SYSCALL_ENOENT)
        printf("no such directory\n");
    // doc: end
    // doc: dup
    Int fd = syscall_dup(1, &err);
    if (BURROW_OK(err))
        err = syscall_close(fd);
    // doc: end
    if (pid <= 0 || BURROW_FAILED(err))
        return 1;
#endif
    return 0;
}

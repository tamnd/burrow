#include <stdio.h>

#include "burrow/burrow.h"
#include "burrow/syscall.h"

int main(void) {
#if !defined(BURROW_OS_WINDOWS) && !defined(BURROW_OS_WASI)
    Error err = BURROW_NO_ERROR;

    // doc: forkexec
    Str argv[2] = {BURROW_S("/bin/echo"), BURROW_S("hello from a child")};
    Uintptr files[3] = {0, 1, 2};
    SyscallSysProcAttr sys = {.setpgid = true};
    SyscallProcAttr attr = {
        .env = slice_nil(TYPE_STRING),
        .files = {files, 3, 3, TYPE_UINTPTR},
        .sys = &sys,
    };
    Int pid = syscall_fork_exec(argv[0], (Slice){argv, 2, 2, TYPE_STRING}, &attr, &err);
    if (BURROW_OK(err)) {
        SyscallWaitStatus ws;
        (void)syscall_wait4(pid, &ws, 0, NULL, &err);
        printf("exit status %d\n", (int)syscall_wait_status_exit_status(ws));
    }
    // doc: end
    if (BURROW_FAILED(err))
        return 1;
#endif
    return 0;
}

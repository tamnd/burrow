#include <stdio.h>

#include "burrow/burrow.h"
#include "burrow/syscall.h"

int main(void) {
    // doc: consts
    Int family = SYSCALL_AF_INET6;   /* 10 on Linux, 30 on macOS, 23 on Windows */
    Int kind = SYSCALL_SOCK_STREAM;  /* 1, but 2 on Linux on MIPS */
    Int proto = SYSCALL_IPPROTO_TCP; /* 6 */
    // doc: end
#if !defined(BURROW_OS_WINDOWS)
    // doc: unix
    Int size = SYSCALL_SIZEOF_SOCKADDR_INET6; /* 28 */
    Int call = SYSCALL_SYS_GETPID; /* 39 on Linux on amd64, 172 on arm64, 20 on macOS */
    // doc: end
    printf("%d %d\n", (int)size, (int)call);
#endif
    printf("%d %d %d\n", (int)family, (int)kind, (int)proto);
    return proto == 6 ? 0 : 1;
}

#include <stdio.h>

#include "burrow/burrow.h"
#include "burrow/mem/arena.h"
#include "burrow/syscall.h"

int main(void) {
#if !defined(BURROW_OS_WINDOWS) && !defined(BURROW_OS_WASI) && !defined(BURROW_OS_COSMO)
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;

    // doc: udp
    Int fd = syscall_socket(SYSCALL_AF_INET, SYSCALL_SOCK_DGRAM, 0, &err);
    SyscallSockaddrInet4 lo = {.addr = {127, 0, 0, 1}};
    if (BURROW_OK(err))
        err = syscall_bind(fd, syscall_sockaddr_inet4_as_sockaddr(&lo));
    SyscallSockaddr sa = syscall_getsockname(a, fd, &err);
    if (BURROW_OK(err) && sa.vt->self_type == TYPE_SYSCALL_SOCKADDR_INET4) {
        Byte buf[16] = "hi";
        err = syscall_sendto(fd, (Slice){buf, 2, 2, TYPE_BYTE}, 0, sa);
        SyscallSockaddr from;
        Int n =
            syscall_recvfrom(a, fd, (Slice){buf, 16, 16, TYPE_BYTE}, 0, &from, &err);
        if (BURROW_OK(err))
            printf("sent and received %d bytes\n", (int)n);
    }
    (void)syscall_close(fd);
    // doc: end
    if (BURROW_FAILED(err))
        return 1;

    // doc: rights
    SyscallSocketpairRet s =
        syscall_socketpair(SYSCALL_AF_UNIX, SYSCALL_SOCK_STREAM, 0, &err);
    Int pass = 0; /* standard input */
    Byte one[1] = {'x'};
    Slice oob = syscall_unix_rights(a, (Slice){&pass, 1, 1, TYPE_INT});
    if (BURROW_OK(err))
        err = syscall_sendmsg(s.fd[0], (Slice){one, 1, 1, TYPE_BYTE}, oob,
                              (SyscallSockaddr){NULL, NULL}, 0);
    Byte cbuf[64];
    Int oobn = 0, flags = 0;
    SyscallSockaddr none;
    (void)syscall_recvmsg(a, s.fd[1], (Slice){one, 1, 1, TYPE_BYTE},
                          (Slice){cbuf, 64, 64, TYPE_BYTE}, 0, &oobn, &flags, &none,
                          &err);
    Slice msgs = syscall_parse_socket_control_message(
        a, (Slice){cbuf, oobn, oobn, TYPE_BYTE}, &err);
    if (BURROW_OK(err) && msgs.len == 1) {
        Slice fds = syscall_parse_unix_rights(a, msgs.p, &err);
        if (BURROW_OK(err) && fds.len == 1) {
            printf("got a descriptor\n");
            (void)syscall_close(((Int *)fds.p)[0]);
        }
    }
    (void)syscall_close(s.fd[0]);
    (void)syscall_close(s.fd[1]);
    // doc: end
    if (BURROW_FAILED(err))
        return 1;
    arena_free(&ar);
#endif
    return 0;
}

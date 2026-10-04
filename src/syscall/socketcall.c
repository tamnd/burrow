/* The socket calls on Linux for 32-bit x86 and s390x, which have no system
 * call of their own for each and go through socketcall(2) instead, with the
 * arguments in memory. Everywhere else they are generated in zsyscall.c.
 *
 * Derived from Go's src/syscall/syscall_linux_386.go and
 * syscall_linux_s390x.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/syscall.h"

#if (defined(BURROW_OS_LINUX) || defined(BURROW_OS_COSMO) ||                           \
     defined(BURROW_OS_WASI)) &&                                                       \
    (defined(BURROW_ARCH_386) || defined(BURROW_ARCH_S390X))

#include "internal.h"

/* The call numbers, from linux/net.h. */
enum {
    SOCKETCALL_SOCKET = 1,
    SOCKETCALL_BIND = 2,
    SOCKETCALL_CONNECT = 3,
    SOCKETCALL_LISTEN = 4,
    SOCKETCALL_GETSOCKNAME = 6,
    SOCKETCALL_GETPEERNAME = 7,
    SOCKETCALL_SOCKETPAIR = 8,
    SOCKETCALL_SENDTO = 11,
    SOCKETCALL_RECVFROM = 12,
    SOCKETCALL_SHUTDOWN = 13,
    SOCKETCALL_SETSOCKOPT = 14,
    SOCKETCALL_GETSOCKOPT = 15,
    SOCKETCALL_SENDMSG = 16,
    SOCKETCALL_RECVMSG = 17,
    SOCKETCALL_ACCEPT4 = 18,
};

/* Go's socketcall: call with its six arguments, -1 and the Errno in *err if
 * it fails. */
static Int socketcall(Int call, Uintptr a0, Uintptr a1, Uintptr a2, Uintptr a3,
                      Uintptr a4, Uintptr a5, Error *err) {
    Uintptr args[6] = {a0, a1, a2, a3, a4, a5};
    SyscallErrno e = 0;
    Uintptr r = syscall_syscall(SYSCALL_SYS_SOCKETCALL, (Uintptr)call, (Uintptr)args, 0,
                                NULL, &e);
    if (e != 0) {
        BURROW_OUT(err, burrow__syscall_errno_err(e));
        return -1;
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return (Int)r;
}

/* The same for a call whose only result is whether it failed. */
static Error socketcall_err(Int call, Uintptr a0, Uintptr a1, Uintptr a2, Uintptr a3,
                            Uintptr a4, Uintptr a5) {
    Error err = BURROW_NO_ERROR;
    (void)socketcall(call, a0, a1, a2, a3, a4, a5, &err);
    return err;
}

/* The address of p's bytes, or 0 when it has none, as Go passes it. */
static Uintptr socketcall_base(Slice p) {
    return p.len > 0 ? (Uintptr)p.p : 0;
}

Int burrow__syscall_accept4(Int s, SyscallRawSockaddrAny *rsa, uint32_t *addrlen,
                            Int flags, Error *err) {
    return socketcall(SOCKETCALL_ACCEPT4, (Uintptr)s, (Uintptr)rsa, (Uintptr)addrlen,
                      (Uintptr)flags, 0, 0, err);
}

Error burrow__syscall_getsockname(Int fd, SyscallRawSockaddrAny *rsa,
                                  uint32_t *addrlen) {
    return socketcall_err(SOCKETCALL_GETSOCKNAME, (Uintptr)fd, (Uintptr)rsa,
                          (Uintptr)addrlen, 0, 0, 0);
}

Error burrow__syscall_getpeername(Int fd, SyscallRawSockaddrAny *rsa,
                                  uint32_t *addrlen) {
    return socketcall_err(SOCKETCALL_GETPEERNAME, (Uintptr)fd, (Uintptr)rsa,
                          (Uintptr)addrlen, 0, 0, 0);
}

Error burrow__syscall_socketpair(Int domain, Int typ, Int proto, int32_t *fd) {
    return socketcall_err(SOCKETCALL_SOCKETPAIR, (Uintptr)domain, (Uintptr)typ,
                          (Uintptr)proto, (Uintptr)fd, 0, 0);
}

Error burrow__syscall_bind(Int s, void *addr, uint32_t addrlen) {
    return socketcall_err(SOCKETCALL_BIND, (Uintptr)s, (Uintptr)addr, (Uintptr)addrlen,
                          0, 0, 0);
}

Error burrow__syscall_connect(Int s, void *addr, uint32_t addrlen) {
    return socketcall_err(SOCKETCALL_CONNECT, (Uintptr)s, (Uintptr)addr,
                          (Uintptr)addrlen, 0, 0, 0);
}

Int burrow__syscall_socket(Int domain, Int typ, Int proto, Error *err) {
    return socketcall(SOCKETCALL_SOCKET, (Uintptr)domain, (Uintptr)typ, (Uintptr)proto,
                      0, 0, 0, err);
}

Error burrow__syscall_getsockopt(Int s, Int level, Int name, void *val,
                                 uint32_t *vallen) {
    return socketcall_err(SOCKETCALL_GETSOCKOPT, (Uintptr)s, (Uintptr)level,
                          (Uintptr)name, (Uintptr)val, (Uintptr)vallen, 0);
}

Error burrow__syscall_setsockopt(Int s, Int level, Int name, void *val,
                                 Uintptr vallen) {
    return socketcall_err(SOCKETCALL_SETSOCKOPT, (Uintptr)s, (Uintptr)level,
                          (Uintptr)name, (Uintptr)val, vallen, 0);
}

Int burrow__syscall_recvfrom(Int fd, Slice p, Int flags, SyscallRawSockaddrAny *from,
                             uint32_t *fromlen, Error *err) {
    return socketcall(SOCKETCALL_RECVFROM, (Uintptr)fd, socketcall_base(p),
                      (Uintptr)p.len, (Uintptr)flags, (Uintptr)from, (Uintptr)fromlen,
                      err);
}

Error burrow__syscall_sendto(Int s, Slice buf, Int flags, void *to, uint32_t addrlen) {
    return socketcall_err(SOCKETCALL_SENDTO, (Uintptr)s, socketcall_base(buf),
                          (Uintptr)buf.len, (Uintptr)flags, (Uintptr)to,
                          (Uintptr)addrlen);
}

Int burrow__syscall_recvmsg(Int s, SyscallMsghdr *msg, Int flags, Error *err) {
    return socketcall(SOCKETCALL_RECVMSG, (Uintptr)s, (Uintptr)msg, (Uintptr)flags, 0,
                      0, 0, err);
}

Int burrow__syscall_sendmsg(Int s, SyscallMsghdr *msg, Int flags, Error *err) {
    return socketcall(SOCKETCALL_SENDMSG, (Uintptr)s, (Uintptr)msg, (Uintptr)flags, 0,
                      0, 0, err);
}

Error syscall_listen(Int s, Int n) {
    return socketcall_err(SOCKETCALL_LISTEN, (Uintptr)s, (Uintptr)n, 0, 0, 0, 0);
}

Error syscall_shutdown(Int s, Int how) {
    return socketcall_err(SOCKETCALL_SHUTDOWN, (Uintptr)s, (Uintptr)how, 0, 0, 0, 0);
}

#endif

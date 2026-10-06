/* The socket calls Go writes by hand: the Sockaddr types, Socketpair, Bind,
 * Accept, Recvmsg and Sendmsg, the control messages, and on Linux the
 * credentials, netlink and the socket filters.
 *
 * TestUnixRightsRoundtrip, TestSetsockoptString, TestSCMCredentials and
 * TestParseNetlinkMessage are ported from Go's src/syscall/syscall_unix_test.go,
 * creds_test.go and syscall_linux_test.go. TestPassFD there runs a child
 * process to pass the descriptor to, and TestPassRights here does the same
 * thing within one process. The others are burrow's own.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/mem/arena.h"
#include "burrow/os.h"
#include "burrow/syscall.h"

#include <stdio.h>
#include <string.h>

#if !defined(BURROW_OS_WINDOWS)
#include <fcntl.h>
#include <unistd.h>
#endif

#define ARENA_BEGIN                                                                    \
    Arena ar;                                                                          \
    arena_init(&ar, NULL, 0);                                                          \
    Alloc *a = arena_allocator(&ar)
#define ARENA_END arena_free(&ar)

#if !defined(BURROW_OS_WINDOWS)

static Str cstr(const char *s) {
    return str_from_bytes((const Byte *)s, (Int)strlen(s));
}

/* The Errno err holds, or 0 if it holds none. */
static SyscallErrno errno_of(Error err) {
    const SyscallErrno *e = errors_as(err, TYPE_SYSCALL_ERRNO);
    return e == NULL ? 0 : *e;
}

/* Cosmopolitan has no way to make a raw system call, so everything that goes
 * through one gives ENOSYS there, and the socket calls all do. wasip1 has no
 * socket, socketpair or setsockopt at all, and Go only runs these tests on
 * unix, which wasip1 is not. */
static void skip_without_raw_calls(TestingT *t) {
#if defined(BURROW_OS_COSMO)
    testing_t_skip_v(t, "Cosmopolitan has no raw system calls");
#elif defined(BURROW_OS_WASI)
    testing_t_skip_v(t, "wasip1 has no sockets to make");
#else
    (void)t;
#endif
}

static Slice bytes_of(void *p, Int n) {
    return (Slice){p, n, n, TYPE_BYTE};
}

/* The bytes of s, without its NUL, for the calls that only read them. */
static Slice text(const char *s) {
    return bytes_of((void *)(uintptr_t)s, (Int)strlen(s));
}

static SyscallSocketpairRet pair(TestingT *t, Int typ) {
    Error err = BURROW_NO_ERROR;
    SyscallSocketpairRet fds = syscall_socketpair(SYSCALL_AF_UNIX, typ, 0, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Socketpair: %v", err);
    return fds;
}

static void close_pair(SyscallSocketpairRet fds) {
    (void)syscall_close(fds.fd[0]);
    (void)syscall_close(fds.fd[1]);
}

static void TestUnixRightsRoundtrip(TestingT *t) {
    ARENA_BEGIN;
    static const Int c0[] = {42}, c1[] = {1, 2}, c2[] = {3, 4, 5}, c4a[] = {1, 2},
                     c4b[] = {3, 4, 5}, c4d[] = {7};
    static const struct {
        Int n;
        const Int *fds[4];
        Int lens[4];
    } cases[] = {
        {1, {c0}, {1}},
        {1, {c1}, {2}},
        {1, {c2}, {3}},
        {1, {NULL}, {0}},
        {4, {c4a, c4b, NULL, c4d}, {2, 3, 0, 1}},
    };
    for (size_t c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        Byte buf[512];
        Int blen = 0, n = 0;
        for (Int k = 0; k < cases[c].n; k++) {
            /* Last assignment to n wins */
            n = blen + syscall_cmsg_len(4 * cases[c].lens[k]);
            Slice fds = {(void *)(uintptr_t)cases[c].fds[k], cases[c].lens[k],
                         cases[c].lens[k], TYPE_INT};
            Slice r = syscall_unix_rights(a, fds);
            memcpy(buf + blen, r.p, (size_t)r.len);
            blen += r.len;
        }
        /* Truncate b */
        Error err = BURROW_NO_ERROR;
        Slice scms = syscall_parse_socket_control_message(a, bytes_of(buf, n), &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "ParseSocketControlMessage: %v", err);
        if (scms.len != cases[c].n)
            testing_t_fatalf_v(t, "expected %d SocketControlMessage; got %d",
                               cases[c].n, scms.len);
        for (Int i = 0; i < scms.len; i++) {
            SyscallSocketControlMessage *scm =
                &((SyscallSocketControlMessage *)scms.p)[i];
            Slice got = syscall_parse_unix_rights(a, scm, &err);
            if (BURROW_FAILED(err))
                testing_t_fatalf_v(t, "ParseUnixRights: %v", err);
            if (got.len != cases[c].lens[i])
                testing_t_fatalf_v(t, "expected %d fds, got %d", cases[c].lens[i],
                                   got.len);
            for (Int j = 0; j < got.len; j++)
                if (((Int *)got.p)[j] != cases[c].fds[i][j])
                    testing_t_fatalf_v(t, "expected fd %d, got %d", cases[c].fds[i][j],
                                       ((Int *)got.p)[j]);
        }
    }

    /* A header that says it is longer than what is there. */
    Byte bad[64];
    memset(bad, 0, sizeof bad);
    SyscallCmsghdr h;
    memset(&h, 0, sizeof h);
    syscall_cmsghdr_set_len(&h, 1000);
    memcpy(bad, &h, sizeof h);
    Error err = BURROW_NO_ERROR;
    Slice scms = syscall_parse_socket_control_message(a, bytes_of(bad, 64), &err);
    CHECK(errno_of(err) == SYSCALL_EINVAL && scms.p == NULL);
    ARENA_END;
}

static void TestSetsockoptString(TestingT *t) {
    /* should not panic on empty string, see issue #31277 */
    Error err = syscall_setsockopt_string(-1, 0, 0, BURROW_S(""));
    if (!BURROW_FAILED(err))
        testing_t_fatalf_v(t, "SetsockoptString: did not fail");
}

/* The other end of a pipe goes over a socket in an SCM_RIGHTS message, and what
 * is written to the pipe comes out of the descriptor that arrives. */
static void TestPassRights(TestingT *t) {
    skip_without_raw_calls(t);
    ARENA_BEGIN;
    SyscallSocketpairRet s = pair(t, SYSCALL_SOCK_STREAM);
    int p[2];
    if (pipe(p) != 0)
        testing_t_fatalf_v(t, "pipe failed");

    Int fd = p[0];
    Slice oob = syscall_unix_rights(a, (Slice){&fd, 1, 1, TYPE_INT});
    Byte one = 'x';
    Error err = syscall_sendmsg(s.fd[0], bytes_of(&one, 1), oob,
                                (SyscallSockaddr){NULL, NULL}, 0);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Sendmsg: %v", err);

    Byte buf[1];
    Byte oob2[64];
    Int oobn = 0, flags = 0;
    SyscallSockaddr from;
    Int n = syscall_recvmsg(a, s.fd[1], bytes_of(buf, 1), bytes_of(oob2, 64), 0, &oobn,
                            &flags, &from, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Recvmsg: %v", err);
    if (n != 1 || buf[0] != 'x')
        testing_t_errorf_v(t, "Recvmsg read %d bytes", n);
    if (flags != 0)
        testing_t_errorf_v(t, "Recvmsg flags = %#x, want 0", flags);

    Slice scms = syscall_parse_socket_control_message(a, bytes_of(oob2, oobn), &err);
    if (BURROW_FAILED(err) || scms.len != 1)
        testing_t_fatalf_v(t, "ParseSocketControlMessage: %d messages, %v", scms.len,
                           err);
    Slice fds =
        syscall_parse_unix_rights(a, (SyscallSocketControlMessage *)scms.p, &err);
    if (BURROW_FAILED(err) || fds.len != 1)
        testing_t_fatalf_v(t, "ParseUnixRights: %d fds, %v", fds.len, err);
    Int got = ((Int *)fds.p)[0];
    CHECK(got != p[0]);

    CHECK(write(p[1], "hello", 5) == 5);
    char hello[8] = {0};
    CHECK(read((int)got, hello, sizeof hello) == 5);
    CHECK(memcmp(hello, "hello", 5) == 0);

    (void)close((int)got);
    (void)close(p[0]);
    (void)close(p[1]);
    close_pair(s);
    ARENA_END;
}

/* A listening Unix socket, a client, Accept, Getsockname and Getpeername. */
static void TestUnixListen(TestingT *t) {
    skip_without_raw_calls(t);
    ARENA_BEGIN;
    Error err = BURROW_NO_ERROR;
    Str dir = os_mkdir_temp(a, BURROW_S(""), BURROW_S("burrow-sock-*"), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "MkdirTemp: %v", err);
    char path[256];
    snprintf(path, sizeof path, "%.*s/s", (int)dir.len, (const char *)dir.p);

    Int l = syscall_socket(SYSCALL_AF_UNIX, SYSCALL_SOCK_STREAM, 0, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Socket: %v", err);
    SyscallSockaddrUnix su;
    memset(&su, 0, sizeof su);
    su.name = cstr(path);
    err = syscall_bind(l, syscall_sockaddr_unix_as_sockaddr(&su));
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Bind(%q): %v", su.name, err);
    err = syscall_listen(l, 1);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Listen: %v", err);

    SyscallSockaddr sa = syscall_getsockname(a, l, &err);
    if (BURROW_FAILED(err) || sa.vt == NULL ||
        sa.vt->self_type != TYPE_SYSCALL_SOCKADDR_UNIX)
        testing_t_fatalf_v(t, "Getsockname: %v", err);
    Str name = ((SyscallSockaddrUnix *)sa.data)->name;
    if (!str_eq(name, su.name))
        testing_t_errorf_v(t, "Getsockname = %q, want %q", name, su.name);

    Int c = syscall_socket(SYSCALL_AF_UNIX, SYSCALL_SOCK_STREAM, 0, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Socket: %v", err);
    err = syscall_connect(c, syscall_sockaddr_unix_as_sockaddr(&su));
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Connect: %v", err);

    SyscallSockaddr peer;
    Int s = syscall_accept(a, l, &peer, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Accept: %v", err);
    if (peer.vt == NULL || peer.vt->self_type != TYPE_SYSCALL_SOCKADDR_UNIX)
        testing_t_errorf_v(t, "Accept gave back a Sockaddr that is not a Unix one");

    sa = syscall_getpeername(a, c, &err);
    if (BURROW_FAILED(err) || sa.vt == NULL ||
        sa.vt->self_type != TYPE_SYSCALL_SOCKADDR_UNIX)
        testing_t_fatalf_v(t, "Getpeername: %v", err);
    name = ((SyscallSockaddrUnix *)sa.data)->name;
    if (!str_eq(name, su.name))
        testing_t_errorf_v(t, "Getpeername = %q, want %q", name, su.name);

    Int n = syscall_write(c, text("ping"), &err);
    CHECK(BURROW_OK(err) && n == 4);
    Byte buf[8];
    n = syscall_read(s, bytes_of(buf, 8), &err);
    CHECK(BURROW_OK(err) && n == 4 && memcmp(buf, "ping", 4) == 0);

    /* The name has to fit in sun_path with room for the NUL. */
    char longname[300];
    memset(longname, 'a', sizeof longname - 1);
    longname[sizeof longname - 1] = 0;
    SyscallSockaddrUnix bad;
    memset(&bad, 0, sizeof bad);
    bad.name = cstr(longname);
    CHECK(errno_of(syscall_bind(c, syscall_sockaddr_unix_as_sockaddr(&bad))) ==
          SYSCALL_EINVAL);

    (void)syscall_close(s);
    (void)syscall_close(c);
    (void)syscall_close(l);
    (void)os_remove_all(dir);
    ARENA_END;
}

/* UDP on the loopback address: Bind to port 0, Getsockname for the port the
 * system chose, Sendto and Recvfrom. */
static void TestUDPLoopback(TestingT *t) {
    skip_without_raw_calls(t);
    ARENA_BEGIN;
    Error err = BURROW_NO_ERROR;
    Int r = syscall_socket(SYSCALL_AF_INET, SYSCALL_SOCK_DGRAM, 0, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Socket: %v", err);
    Int w = syscall_socket(SYSCALL_AF_INET, SYSCALL_SOCK_DGRAM, 0, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Socket: %v", err);

    SyscallSockaddrInet4 lo;
    memset(&lo, 0, sizeof lo);
    lo.addr[0] = 127;
    lo.addr[3] = 1;
    err = syscall_bind(r, syscall_sockaddr_inet4_as_sockaddr(&lo));
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Bind: %v", err);
    err = syscall_bind(w, syscall_sockaddr_inet4_as_sockaddr(&lo));
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Bind: %v", err);

    SyscallSockaddr sa = syscall_getsockname(a, r, &err);
    if (BURROW_FAILED(err) || sa.vt == NULL ||
        sa.vt->self_type != TYPE_SYSCALL_SOCKADDR_INET4)
        testing_t_fatalf_v(t, "Getsockname: %v", err);
    SyscallSockaddrInet4 to = *(SyscallSockaddrInet4 *)sa.data;
    CHECK(to.port != 0);
    CHECK(to.addr[0] == 127 && to.addr[3] == 1);
    sa = syscall_getsockname(a, w, &err);
    CHECK(BURROW_OK(err) && sa.vt != NULL);
    Int wport = ((SyscallSockaddrInet4 *)sa.data)->port;

    err =
        syscall_sendto(w, text("datagram"), 0, syscall_sockaddr_inet4_as_sockaddr(&to));
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Sendto: %v", err);

    Byte buf[16];
    SyscallSockaddr from;
    Int n = syscall_recvfrom(a, r, bytes_of(buf, 16), 0, &from, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Recvfrom: %v", err);
    CHECK(n == 8 && memcmp(buf, "datagram", 8) == 0);
    if (from.vt == NULL || from.vt->self_type != TYPE_SYSCALL_SOCKADDR_INET4)
        testing_t_fatalf_v(t, "Recvfrom gave back a Sockaddr that is not an Inet4 one");
    SyscallSockaddrInet4 *f = (SyscallSockaddrInet4 *)from.data;
    if (f->port != wport || f->addr[0] != 127 || f->addr[3] != 1)
        testing_t_errorf_v(t, "Recvfrom from port %d, want %d", f->port, wport);

    /* The port has to fit in 16 bits. */
    SyscallSockaddrInet4 bad = lo;
    bad.port = 70000;
    CHECK(errno_of(syscall_connect(w, syscall_sockaddr_inet4_as_sockaddr(&bad))) ==
          SYSCALL_EINVAL);

    (void)syscall_close(r);
    (void)syscall_close(w);
    ARENA_END;
}

static void TestSockopts(TestingT *t) {
    skip_without_raw_calls(t);
    Error err = BURROW_NO_ERROR;
    Int fd = syscall_socket(SYSCALL_AF_INET, SYSCALL_SOCK_STREAM, 0, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Socket: %v", err);
    Int typ = syscall_getsockopt_int(fd, SYSCALL_SOL_SOCKET, SYSCALL_SO_TYPE, &err);
    CHECK(BURROW_OK(err) && typ == SYSCALL_SOCK_STREAM);

    err = syscall_setsockopt_int(fd, SYSCALL_SOL_SOCKET, SYSCALL_SO_REUSEADDR, 1);
    CHECK(BURROW_OK(err));
    CHECK(syscall_getsockopt_int(fd, SYSCALL_SOL_SOCKET, SYSCALL_SO_REUSEADDR, &err) !=
          0);

    SyscallLinger l;
    memset(&l, 0, sizeof l);
    l.onoff = 1;
    l.linger = 2;
    CHECK(BURROW_OK(
        syscall_setsockopt_linger(fd, SYSCALL_SOL_SOCKET, SYSCALL_SO_LINGER, &l)));

    SyscallTimeval tv = syscall_nsec_to_timeval(1500000000);
    CHECK(BURROW_OK(
        syscall_setsockopt_timeval(fd, SYSCALL_SOL_SOCKET, SYSCALL_SO_RCVTIMEO, &tv)));

    /* A closed descriptor is EBADF, and the value read is 0. */
    (void)syscall_close(fd);
    typ = syscall_getsockopt_int(fd, SYSCALL_SOL_SOCKET, SYSCALL_SO_TYPE, &err);
    CHECK(errno_of(err) == SYSCALL_EBADF && typ == 0);
}

static void TestCmsgSizes(TestingT *t) {
    (void)t;
    Int hdr = syscall_cmsg_len(0);
    CHECK(hdr >= SYSCALL_SIZEOF_CMSGHDR);
    CHECK(syscall_cmsg_len(4) == hdr + 4);
    CHECK(syscall_cmsg_space(0) == hdr);
    CHECK(syscall_cmsg_space(1) > hdr && syscall_cmsg_space(1) % 4 == 0);
    CHECK(syscall_cmsg_space(4) >= syscall_cmsg_len(4));
}

#define UNIX_TESTS(X)                                                                  \
    X(TestUnixRightsRoundtrip)                                                         \
    X(TestSetsockoptString)                                                            \
    X(TestPassRights)                                                                  \
    X(TestUnixListen)                                                                  \
    X(TestUDPLoopback)                                                                 \
    X(TestSockopts)                                                                    \
    X(TestCmsgSizes)
#else
#define UNIX_TESTS(X)
#endif

#if defined(BURROW_OS_LINUX)

static void TestSCMCredentials(TestingT *t) {
    ARENA_BEGIN;
    static const struct {
        Int socket_type;
        Int data_len;
    } socket_type_tests[] = {
        {SYSCALL_SOCK_STREAM, 1},
        {SYSCALL_SOCK_DGRAM, 0},
    };
    for (size_t i = 0; i < sizeof socket_type_tests / sizeof socket_type_tests[0];
         i++) {
        SyscallSocketpairRet fds = pair(t, socket_type_tests[i].socket_type);
        Error err = syscall_setsockopt_int(fds.fd[0], SYSCALL_SOL_SOCKET,
                                           SYSCALL_SO_PASSCRED, 1);
        if (BURROW_FAILED(err)) {
            close_pair(fds);
            testing_t_fatalf_v(t, "SetsockoptInt: %v", err);
        }
        Int srv = fds.fd[0], cli = fds.fd[1];
        SyscallSockaddr none = {NULL, NULL};

        SyscallUcred ucred;
        memset(&ucred, 0, sizeof ucred);
        if (syscall_getuid() != 0) {
            ucred.pid = (int32_t)syscall_getpid();
            ucred.uid = 0;
            ucred.gid = 0;
            Slice oob = syscall_unix_credentials(a, &ucred);
            (void)syscall_sendmsg_n(cli, slice_nil(TYPE_BYTE), oob, none, 0, &err);
            SyscallErrno e = errno_of(err);
            if (e != SYSCALL_EPERM && e != SYSCALL_EINVAL)
                testing_t_fatalf_v(
                    t, "WriteMsgUnix failed with %v, want EPERM or EINVAL", err);
        }

        ucred.pid = (int32_t)syscall_getpid();
        ucred.uid = (uint32_t)syscall_getuid();
        ucred.gid = (uint32_t)syscall_getgid();
        Slice oob = syscall_unix_credentials(a, &ucred);

        /* On SOCK_STREAM, this is internally going to send a dummy byte */
        Int n = syscall_sendmsg_n(cli, slice_nil(TYPE_BYTE), oob, none, 0, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "WriteMsgUnix: %v", err);
        if (n != 0)
            testing_t_fatalf_v(t, "WriteMsgUnix n = %d, want 0", n);

        Byte oob2[256] = {0};
        Int oobn2 = 0, flags = 0;
        SyscallSockaddr from;
        n = syscall_recvmsg(a, srv, slice_nil(TYPE_BYTE), bytes_of(oob2, 10 * oob.len),
                            SYSCALL_MSG_CMSG_CLOEXEC, &oobn2, &flags, &from, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "ReadMsgUnix: %v", err);
        if (flags != SYSCALL_MSG_CMSG_CLOEXEC)
            testing_t_fatalf_v(t,
                               "ReadMsgUnix flags = %#x, want %#x (MSG_CMSG_CLOEXEC)",
                               flags, (Int)SYSCALL_MSG_CMSG_CLOEXEC);
        if (n != socket_type_tests[i].data_len)
            testing_t_fatalf_v(t, "ReadMsgUnix n = %d, want %d", n,
                               socket_type_tests[i].data_len);
        if (oobn2 != oob.len)
            /* without SO_PASSCRED set on the socket, ReadMsgUnix will
             * return zero oob bytes */
            testing_t_fatalf_v(t, "ReadMsgUnix oobn = %d, want %d", oobn2, oob.len);
        if (memcmp(oob.p, oob2, (size_t)oob.len) != 0)
            testing_t_fatalf_v(t, "ReadMsgUnix oob bytes don't match");

        Slice scm =
            syscall_parse_socket_control_message(a, bytes_of(oob2, oobn2), &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "ParseSocketControlMessage: %v", err);
        SyscallUcred got =
            syscall_parse_unix_credentials((SyscallSocketControlMessage *)scm.p, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "ParseUnixCredentials: %v", err);
        if (got.pid != ucred.pid || got.uid != ucred.uid || got.gid != ucred.gid)
            testing_t_fatalf_v(t, "ParseUnixCredentials = {%d %d %d}, want {%d %d %d}",
                               got.pid, got.uid, got.gid, ucred.pid, ucred.uid,
                               ucred.gid);

        /* The same three from the other end, as SO_PEERCRED. */
        got = syscall_getsockopt_ucred(srv, SYSCALL_SOL_SOCKET, SYSCALL_SO_PEERCRED,
                                       &err);
        CHECK(BURROW_OK(err) && got.pid == ucred.pid && got.uid == ucred.uid);
        close_pair(fds);
    }
    ARENA_END;
}

static void TestParseNetlinkMessage(TestingT *t) {
    ARENA_BEGIN;
    static const Byte b0[] = {
        103, 0,   0,  0,  0,   3,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,   2,   0,  0,  2,   11,  0,   1,   0,   0,   0,   0,   5,   8,   0,
        3,   0,   8,  0,  6,   0,   0,   0,   0,   1,   63,  0,   10,  0,   69,
        16,  0,   59, 39, 82,  64,  0,   64,  6,   21,  89,  127, 0,   0,   1,
        127, 0,   0,  1,  230, 228, 31,  144, 32,  186, 155, 211, 185, 151, 209,
        179, 128, 24, 1,  86,  53,  119, 0,   0,   1,   1,   8,   10,  0,   17,
        234, 12,  0,  17, 189, 126, 107, 106, 108, 107, 106, 13,  10,
    };
    static const Byte b1[] = {
        106, 0,   0,   0,   0,   3,   0,   0,   0,   0,  0,   0,  0,   0,  0,   0,
        2,   0,   0,   2,   11,  0,   1,   0,   0,   0,  0,   3,  8,   0,  3,   0,
        8,   0,   6,   0,   0,   0,   0,   1,   66,  0,  10,  0,  69,  0,  0,   62,
        230, 255, 64,  0,   64,  6,   85,  184, 127, 0,  0,   1,  127, 0,  0,   1,
        237, 206, 31,  144, 73,  197, 128, 65,  250, 60, 192, 97, 128, 24, 1,   86,
        253, 21,  0,   0,   1,   1,   8,   10,  0,   51, 106, 89, 0,   51, 102, 198,
        108, 104, 106, 108, 107, 104, 108, 107, 104, 10,
    };
    static const Byte b2[] = {
        102, 0,   0,  0,   0,   3,   0,   0,   0,  0,   0,   0,   0,   0,  0,
        0,   2,   0,  0,   2,   11,  0,   1,   0,  0,   0,   0,   1,   8,  0,
        3,   0,   8,  0,   6,   0,   0,   0,   0,  1,   62,  0,   10,  0,  69,
        0,   0,   58, 231, 2,   64,  0,   64,  6,  85,  185, 127, 0,   0,  1,
        127, 0,   0,  1,   237, 206, 31,  144, 73, 197, 128, 86,  250, 60, 192,
        97,  128, 24, 1,   86,  104, 64,  0,   0,  1,   1,   8,   10,  0,  52,
        198, 200, 0,  51,  135, 232, 101, 115, 97, 103, 103, 10,
    };
    static const struct {
        const Byte *p;
        Int n;
    } cases[] = {{b0, sizeof b0}, {b1, sizeof b1}, {b2, sizeof b2}};
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        /* Copied, since the parser takes a Slice it may point into. */
        Byte b[128];
        memcpy(b, cases[i].p, (size_t)cases[i].n);
        Error err = BURROW_NO_ERROR;
        Slice m = syscall_parse_netlink_message(a, bytes_of(b, cases[i].n), &err);
        if (errno_of(err) != SYSCALL_EINVAL)
            testing_t_errorf_v(t, "#%d: got %v; want EINVAL", (Int)i, err);
        if (m.p != NULL)
            testing_t_errorf_v(t, "#%d: got %d messages; want nil", (Int)i, m.len);
    }
    ARENA_END;
}

/* The interfaces from NetlinkRIB, which always has lo. */
static void TestNetlinkRIB(TestingT *t) {
    ARENA_BEGIN;
    Error err = BURROW_NO_ERROR;
    Slice tab = syscall_netlink_rib(a, SYSCALL_RTM_GETLINK, SYSCALL_AF_UNSPEC, &err);
    SyscallErrno e = errno_of(err);
    if (e == SYSCALL_EAFNOSUPPORT || e == SYSCALL_EPROTONOSUPPORT ||
        e == SYSCALL_EPERM || e == SYSCALL_EACCES)
        testing_t_skipf_v(t, "no netlink here: %v", err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "NetlinkRIB: %v", err);
    Slice msgs = syscall_parse_netlink_message(a, tab, &err);
    if (BURROW_FAILED(err) || msgs.len == 0)
        testing_t_fatalf_v(t, "ParseNetlinkMessage: %d messages, %v", msgs.len, err);

    bool lo = false;
    Int links = 0;
    for (Int i = 0; i < msgs.len; i++) {
        SyscallNetlinkMessage *m = &((SyscallNetlinkMessage *)msgs.p)[i];
        if (m->header.type == SYSCALL_NLMSG_DONE)
            break;
        if (m->header.type != SYSCALL_RTM_NEWLINK)
            continue;
        links++;
        Slice attrs = syscall_parse_netlink_route_attr(a, m, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "ParseNetlinkRouteAttr: %v", err);
        for (Int j = 0; j < attrs.len; j++) {
            SyscallNetlinkRouteAttr *ra = &((SyscallNetlinkRouteAttr *)attrs.p)[j];
            if (ra->attr.type == SYSCALL_IFLA_IFNAME && ra->value.len >= 3 &&
                memcmp(ra->value.p, "lo", 3) == 0)
                lo = true;
        }
    }
    if (links == 0)
        testing_t_errorf_v(t, "NetlinkRIB gave back no RTM_NEWLINK messages");
    if (!lo)
        testing_t_errorf_v(t, "NetlinkRIB has no interface called lo");

    /* A message whose attributes run past its end. */
    SyscallNetlinkMessage bad;
    memset(&bad, 0, sizeof bad);
    bad.header.type = SYSCALL_RTM_NEWLINK;
    Byte short_data[4] = {0};
    bad.data = bytes_of(short_data, 4);
    Slice attrs = syscall_parse_netlink_route_attr(a, &bad, &err);
    CHECK(errno_of(err) == SYSCALL_EINVAL && attrs.p == NULL);
    ARENA_END;
}

/* Accept4 with SOCK_CLOEXEC, and a socket filter that takes everything
 * attached and taken off again. */
static void TestAccept4Filter(TestingT *t) {
    ARENA_BEGIN;
    Error err = BURROW_NO_ERROR;
    Int l = syscall_socket(SYSCALL_AF_INET, SYSCALL_SOCK_STREAM, 0, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Socket: %v", err);
    SyscallSockaddrInet4 lo;
    memset(&lo, 0, sizeof lo);
    lo.addr[0] = 127;
    lo.addr[3] = 1;
    CHECK(BURROW_OK(syscall_bind(l, syscall_sockaddr_inet4_as_sockaddr(&lo))));
    CHECK(BURROW_OK(syscall_listen(l, 1)));
    SyscallSockaddr sa = syscall_getsockname(a, l, &err);
    if (BURROW_FAILED(err) || sa.vt == NULL)
        testing_t_fatalf_v(t, "Getsockname: %v", err);

    Int c = syscall_socket(SYSCALL_AF_INET, SYSCALL_SOCK_STREAM, 0, &err);
    CHECK(BURROW_OK(err));
    err = syscall_connect(c, sa);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Connect: %v", err);
    SyscallSockaddr peer;
    Int s = syscall_accept4(a, l, SYSCALL_SOCK_CLOEXEC, &peer, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Accept4: %v", err);
    CHECK(peer.vt != NULL && peer.vt->self_type == TYPE_SYSCALL_SOCKADDR_INET4);
    CHECK((fcntl((int)s, F_GETFD) & FD_CLOEXEC) != 0);

    SyscallSockFilter accept_all[1];
    accept_all[0] = syscall_lsf_stmt(SYSCALL_BPF_RET | SYSCALL_BPF_K, 0xFFFF);
    CHECK(accept_all[0].code == (SYSCALL_BPF_RET | SYSCALL_BPF_K) &&
          accept_all[0].k == 0xFFFF && accept_all[0].jt == 0 && accept_all[0].jf == 0);
    SyscallSockFilter j = syscall_lsf_jump(SYSCALL_BPF_JMP | SYSCALL_BPF_JEQ, 1, 2, 3);
    CHECK(j.code == (SYSCALL_BPF_JMP | SYSCALL_BPF_JEQ) && j.k == 1 && j.jt == 2 &&
          j.jf == 3);
    Slice prog = {accept_all, 1, 1, NULL};
    err = syscall_attach_lsf(s, prog);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "AttachLsf: %v", err);
    /* The big endian CI job runs under qemu's user mode, where detaching
     * failed. A kernel that took the filter can take it off again, so
     * ENOPROTOOPT, what an emulator says for an option it does not know, is
     * only logged. */
    err = syscall_detach_lsf(s);
    if (errno_of(err) == SYSCALL_ENOPROTOOPT)
        testing_t_logf_v(t, "DetachLsf: %v", err);
    else if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "DetachLsf: %v", err);

    (void)syscall_close(s);
    (void)syscall_close(c);
    (void)syscall_close(l);
    ARENA_END;
}

#define LINUX_TESTS(X)                                                                 \
    X(TestSCMCredentials)                                                              \
    X(TestParseNetlinkMessage)                                                         \
    X(TestNetlinkRIB)                                                                  \
    X(TestAccept4Filter)
#else
#define LINUX_TESTS(X)
#endif

#if defined(BURROW_OS_WINDOWS)
static void TestNothing(TestingT *t) {
    testing_t_skip_v(t, "no sockets here");
}
#define OTHER_TESTS(X) X(TestNothing)
#else
#define OTHER_TESTS(X)
#endif

#define TESTS(X)                                                                       \
    UNIX_TESTS(X)                                                                      \
    LINUX_TESTS(X)                                                                     \
    OTHER_TESTS(X)

TESTING_MAIN(TESTS)

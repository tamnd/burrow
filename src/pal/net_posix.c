/* Sockets on everything but Windows, through the BSD socket calls.
 *
 * What this file adds to those calls is what Go's syscall package adds on the
 * way to them: a socket that is non blocking and close on exec from the start,
 * with the fork lock held over the gap on the systems that cannot do it in one
 * call, and addresses converted between the platform's sockaddr and ours in
 * both directions. It also makes sure a write to a dead connection is an
 * error and not a signal, which Go gets from its runtime and a C program has
 * to ask for.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#if !defined(_WIN32)
#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE 1
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#endif
#endif

#include "burrow/platform.h"

#if !defined(BURROW_OS_WINDOWS)

#include "burrow/pal.h"

#include "internal.h"

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if !defined(BURROW_OS_WASI)
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#if defined(BURROW_OS_LINUX)
#include <sys/utsname.h>
#elif defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS) ||                           \
    defined(BURROW_OS_FREEBSD) || defined(BURROW_OS_OPENBSD)
#include <sys/sysctl.h>
#endif

#if defined(BURROW_OS_WASI)

/* wasip1 sockets are the ones the host opened and handed in, and Go's net
 * reaches them through FileListener and FileConn, not through these. */

static bool pnet_nosys(PalErrno *err) {
    BURROW_OUT(err, PAL_ENOSYS);
    return false;
}

int64_t pal_socket(int32_t family, int32_t type, int32_t protocol, PalErrno *err) {
    (void)family;
    (void)type;
    (void)protocol;
    pnet_nosys(err);
    return -1;
}

bool pal_socket_close(int64_t fd, PalErrno *err) {
    (void)fd;
    return pnet_nosys(err);
}

int64_t pal_socket_dup(int64_t fd, PalErrno *err) {
    (void)fd;
    pnet_nosys(err);
    return -1;
}

bool pal_bind(int64_t fd, const PalSockAddr *addr, PalErrno *err) {
    (void)fd;
    (void)addr;
    return pnet_nosys(err);
}

bool pal_listen(int64_t fd, int32_t backlog, PalErrno *err) {
    (void)fd;
    (void)backlog;
    return pnet_nosys(err);
}

int64_t pal_accept(int64_t fd, PalSockAddr *peer, PalErrno *err) {
    (void)fd;
    (void)peer;
    pnet_nosys(err);
    return -1;
}

bool pal_getsockname(int64_t fd, PalSockAddr *out, PalErrno *err) {
    (void)fd;
    (void)out;
    return pnet_nosys(err);
}

bool pal_getpeername(int64_t fd, PalSockAddr *out, PalErrno *err) {
    (void)fd;
    (void)out;
    return pnet_nosys(err);
}

bool pal_connect(int64_t fd, const PalSockAddr *addr, PalErrno *err) {
    (void)fd;
    (void)addr;
    return pnet_nosys(err);
}

int64_t pal_sendto(int64_t fd, const void *buf, int64_t n, const PalSockAddr *addr,
                   PalErrno *err) {
    (void)fd;
    (void)buf;
    (void)n;
    (void)addr;
    pnet_nosys(err);
    return -1;
}

int64_t pal_recvfrom(int64_t fd, void *buf, int64_t n, PalSockAddr *from,
                     PalErrno *err) {
    (void)fd;
    (void)buf;
    (void)n;
    (void)from;
    pnet_nosys(err);
    return -1;
}

int64_t pal_recvmsg(int64_t fd, void *buf, int64_t n, void *oob, int64_t oobcap,
                    int64_t *oobn, int32_t *flags, PalSockAddr *from, PalErrno *err) {
    (void)fd;
    (void)buf;
    (void)n;
    (void)oob;
    (void)oobcap;
    (void)oobn;
    (void)flags;
    (void)from;
    pnet_nosys(err);
    return -1;
}

int64_t pal_writev(int64_t fd, const PalIovec *v, int32_t count, PalErrno *err) {
    (void)fd;
    (void)v;
    (void)count;
    pnet_nosys(err);
    return -1;
}

int64_t pal_sendmsg(int64_t fd, const void *buf, int64_t n, const void *oob,
                    int64_t oobn, const PalSockAddr *to, PalErrno *err) {
    (void)fd;
    (void)buf;
    (void)n;
    (void)oob;
    (void)oobn;
    (void)to;
    pnet_nosys(err);
    return -1;
}

bool pal_getsockopt(int64_t fd, int32_t opt, int64_t *value, PalErrno *err) {
    (void)fd;
    (void)opt;
    (void)value;
    return pnet_nosys(err);
}

bool pal_setsockopt(int64_t fd, int32_t opt, int64_t value, PalErrno *err) {
    (void)fd;
    (void)opt;
    (void)value;
    return pnet_nosys(err);
}

bool pal_setsockopt_mreq(int64_t fd, int32_t opt, const PalMreq *m, PalErrno *err) {
    (void)fd;
    (void)opt;
    (void)m;
    return pnet_nosys(err);
}

bool pal_shutdown(int64_t fd, int32_t how, PalErrno *err) {
    (void)fd;
    (void)how;
    return pnet_nosys(err);
}

int32_t pal_listen_backlog_max(void) {
    return 0;
}

#else

/* The systems with socket flags and accept4, where a socket is made non
 * blocking and close on exec in the call that makes it. Go uses the same
 * calls on the same systems. */
#if defined(BURROW_OS_LINUX) || defined(BURROW_OS_FREEBSD) ||                          \
    defined(BURROW_OS_NETBSD) || defined(BURROW_OS_OPENBSD) ||                         \
    defined(BURROW_OS_DRAGONFLY)
#define PNET_FLAGS 1
#endif

/* The systems whose sockaddrs start with a length byte, which Go fills in. */
#if defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS) || BURROW_BSD
#define PNET_SA_LEN 1
#endif

#if defined(MSG_NOSIGNAL)
#define PNET_SEND_FLAGS MSG_NOSIGNAL
#else
#define PNET_SEND_FLAGS 0
#endif

static bool pnet_fail(PalErrno *err) {
    BURROW_OUT(err, burrow__pal_errno(errno));
    return false;
}

static int64_t pnet_fail_n(PalErrno *err) {
    BURROW_OUT(err, burrow__pal_errno(errno));
    return -1;
}

static bool pnet_fd_ok(int64_t fd, PalErrno *err) {
    if (fd < 0 || fd > INT32_MAX) {
        BURROW_OUT(err, PAL_EBADF);
        return false;
    }
    return true;
}

/* A count for send and recv, which take a size_t and answer an ssize_t, so
 * that the answer cannot be negative for a count that was not. */
static size_t pnet_count(int64_t n) {
    if (n <= 0)
        return 0;
    if ((uint64_t)n > (uint64_t)SSIZE_MAX)
        return (size_t)SSIZE_MAX;
    return (size_t)n;
}

/* Ours into the platform's. False for a family or a type there is no such
 * thing as, which socket() would refuse anyway. */
static bool pnet_family(int32_t family, int *out) {
    switch (family) {
    case PAL_AF_UNSPEC:
        *out = AF_UNSPEC;
        return true;
    case PAL_AF_INET:
        *out = AF_INET;
        return true;
    case PAL_AF_INET6:
        *out = AF_INET6;
        return true;
    case PAL_AF_UNIX:
        *out = AF_UNIX;
        return true;
    default:
        return false;
    }
}

static bool pnet_type(int32_t type, int *out) {
    switch (type) {
    case PAL_SOCK_STREAM:
        *out = SOCK_STREAM;
        return true;
    case PAL_SOCK_DGRAM:
        *out = SOCK_DGRAM;
        return true;
    case PAL_SOCK_RAW:
        *out = SOCK_RAW;
        return true;
#if defined(SOCK_SEQPACKET)
    case PAL_SOCK_SEQPACKET:
        *out = SOCK_SEQPACKET;
        return true;
#endif
    default:
        return false;
    }
}

/* SO_TYPE's answer as a PAL_SOCK_ type, 0 for one with no name here. */
static int64_t pnet_type_from(int ty) {
    if (ty == SOCK_STREAM)
        return PAL_SOCK_STREAM;
    if (ty == SOCK_DGRAM)
        return PAL_SOCK_DGRAM;
    if (ty == SOCK_RAW)
        return PAL_SOCK_RAW;
#if defined(SOCK_SEQPACKET)
    if (ty == SOCK_SEQPACKET)
        return PAL_SOCK_SEQPACKET;
#endif
    return 0;
}

/* An address of ours as the platform's, in ss, with its length. The port and
 * the address go into network byte order here and nowhere else. */
static bool pnet_to_native(const PalSockAddr *a, struct sockaddr_storage *ss,
                           socklen_t *len, PalErrno *err) {
    memset(ss, 0, sizeof *ss);
    if (a == NULL) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }
    switch (a->family) {
    case PAL_AF_INET: {
        struct sockaddr_in *in = (struct sockaddr_in *)ss;
        in->sin_family = (sa_family_t)AF_INET;
        in->sin_port = htons(a->port);
        memcpy(&in->sin_addr, a->addr, 4);
#if defined(PNET_SA_LEN)
        in->sin_len = (uint8_t)sizeof *in;
#endif
        *len = (socklen_t)sizeof *in;
        return true;
    }
    case PAL_AF_INET6: {
        struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)ss;
        in6->sin6_family = (sa_family_t)AF_INET6;
        in6->sin6_port = htons(a->port);
        in6->sin6_scope_id = a->scope_id;
        memcpy(&in6->sin6_addr, a->addr, 16);
#if defined(PNET_SA_LEN)
        in6->sin6_len = (uint8_t)sizeof *in6;
#endif
        *len = (socklen_t)sizeof *in6;
        return true;
    }
    case PAL_AF_UNIX: {
        struct sockaddr_un *un = (struct sockaddr_un *)ss;
        size_t n = a->path_len;
        size_t room = sizeof un->sun_path;
        if (room > sizeof a->path)
            room = sizeof a->path;
        if (n > room) {
            BURROW_OUT(err, PAL_EINVAL);
            return false;
        }
        un->sun_family = (sa_family_t)AF_UNIX;
        memcpy(un->sun_path, a->path, n);
        *len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + n);
#if defined(PNET_SA_LEN)
        un->sun_len = (uint8_t)*len;
#endif
        return true;
    }
    default:
        BURROW_OUT(err, PAL_EAFNOSUPPORT);
        return false;
    }
}

/* The platform's address, len bytes of it, as ours. A family this layer has no
 * shape for comes out as PAL_AF_UNSPEC and the answer is false, which is
 * Go's EAFNOSUPPORT from anyToSockaddr. A length of zero is no address at
 * all, and is true. */
static bool pnet_from_native(const struct sockaddr_storage *ss, socklen_t len,
                             PalSockAddr *out) {
    memset(out, 0, sizeof *out);
    if (len < (socklen_t)(offsetof(struct sockaddr, sa_family) + sizeof ss->ss_family))
        return true;
    /* An if and not a switch, because Cosmopolitan's constants are the
     * host's and are not known until the program runs. */
    int fam = ss->ss_family;
    if (fam == AF_UNSPEC)
        return true;
    if (fam == AF_INET) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)ss;
        out->family = PAL_AF_INET;
        out->port = ntohs(in->sin_port);
        memcpy(out->addr, &in->sin_addr, 4);
        return true;
    }
    if (fam == AF_INET6) {
        const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)ss;
        out->family = PAL_AF_INET6;
        out->port = ntohs(in6->sin6_port);
        out->scope_id = in6->sin6_scope_id;
        memcpy(out->addr, &in6->sin6_addr, 16);
        return true;
    }
    if (fam == AF_UNIX) {
        const struct sockaddr_un *un = (const struct sockaddr_un *)ss;
        size_t off = offsetof(struct sockaddr_un, sun_path);
        size_t n = (size_t)len > off ? (size_t)len - off : 0;
        if (n > sizeof un->sun_path)
            n = sizeof un->sun_path;
        if (n > sizeof out->path)
            n = sizeof out->path;
        out->family = PAL_AF_UNIX;
        out->path_len = (uint16_t)n;
        memcpy(out->path, un->sun_path, n);
        return true;
    }
    return false;
}

#if !defined(PNET_FLAGS)

/* The second half of making a socket where it takes two calls: close on exec
 * and non blocking, and on macOS no SIGPIPE. The caller holds the fork lock. */
static bool pnet_setup(int fd) {
    int fdf = fcntl(fd, F_GETFD);
    if (fdf < 0 || fcntl(fd, F_SETFD, fdf | FD_CLOEXEC) != 0)
        return false;
    int fl = fcntl(fd, F_GETFL);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | (int)O_NONBLOCK) != 0)
        return false;
    return true;
}

#endif

/* The socket's own setting where there is one, as well as the send flag, so
 * that a system with only one of the two is covered. macOS has SO_NOSIGPIPE
 * in every release and MSG_NOSIGNAL only in newer headers. */
static bool pnet_nosigpipe(int fd) {
#if defined(SO_NOSIGPIPE)
    int on = 1;
    return setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, (socklen_t)sizeof on) == 0;
#else
    (void)fd;
    return true;
#endif
}

int64_t pal_socket(int32_t family, int32_t type, int32_t protocol, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    int fam = 0;
    int ty = 0;
    if (!pnet_family(family, &fam) || family == PAL_AF_UNSPEC) {
        BURROW_OUT(err, PAL_EAFNOSUPPORT);
        return -1;
    }
    if (!pnet_type(type, &ty) || protocol < 0) {
        BURROW_OUT(err, PAL_EINVAL);
        return -1;
    }
#if defined(PNET_FLAGS)
    int fd = socket(fam, ty | SOCK_NONBLOCK | SOCK_CLOEXEC, (int)protocol);
    if (fd < 0)
        return pnet_fail_n(err);
#else
    burrow__pal_fork_rlock();
    int fd = socket(fam, ty, (int)protocol);
    if (fd < 0) {
        PalErrno e = burrow__pal_errno(errno);
        burrow__pal_fork_runlock();
        BURROW_OUT(err, e);
        return -1;
    }
    if (!pnet_setup(fd)) {
        PalErrno e = burrow__pal_errno(errno);
        close(fd);
        burrow__pal_fork_runlock();
        BURROW_OUT(err, e);
        return -1;
    }
    burrow__pal_fork_runlock();
#endif
    if (!pnet_nosigpipe(fd)) {
        PalErrno e = burrow__pal_errno(errno);
        close(fd);
        BURROW_OUT(err, e);
        return -1;
    }
    return fd;
}

bool pal_socket_close(int64_t fd, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!pnet_fd_ok(fd, err))
        return false;
    /* As pal_close does: the descriptor is gone whatever close says, so an
     * EINTR is not a reason to try again. */
    if (close((int)fd) != 0 && errno != EINTR)
        return pnet_fail(err);
    return true;
}

int64_t pal_socket_dup(int64_t fd, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!pnet_fd_ok(fd, err))
        return -1;
    int nfd = fcntl((int)fd, F_DUPFD_CLOEXEC, 0);
    if (nfd < 0)
        return pnet_fail_n(err);
    return nfd;
}

bool pal_bind(int64_t fd, const PalSockAddr *addr, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    struct sockaddr_storage ss;
    socklen_t len = 0;
    if (!pnet_fd_ok(fd, err) || !pnet_to_native(addr, &ss, &len, err))
        return false;
    if (bind((int)fd, (const struct sockaddr *)&ss, len) != 0)
        return pnet_fail(err);
    return true;
}

bool pal_listen(int64_t fd, int32_t backlog, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!pnet_fd_ok(fd, err))
        return false;
    if (listen((int)fd, (int)backlog) != 0)
        return pnet_fail(err);
    return true;
}

int64_t pal_accept(int64_t fd, PalSockAddr *peer, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!pnet_fd_ok(fd, err))
        return -1;
    struct sockaddr_storage ss;
    socklen_t len = (socklen_t)sizeof ss;
    memset(&ss, 0, sizeof ss);
#if defined(PNET_FLAGS)
    int nfd =
        accept4((int)fd, (struct sockaddr *)&ss, &len, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (nfd < 0)
        return pnet_fail_n(err);
#else
    burrow__pal_fork_rlock();
    int nfd = accept((int)fd, (struct sockaddr *)&ss, &len);
    if (nfd < 0) {
        PalErrno e = burrow__pal_errno(errno);
        burrow__pal_fork_runlock();
        BURROW_OUT(err, e);
        return -1;
    }
    if (!pnet_setup(nfd)) {
        PalErrno e = burrow__pal_errno(errno);
        close(nfd);
        burrow__pal_fork_runlock();
        BURROW_OUT(err, e);
        return -1;
    }
    burrow__pal_fork_runlock();
#endif
    if (!pnet_nosigpipe(nfd)) {
        PalErrno e = burrow__pal_errno(errno);
        close(nfd);
        BURROW_OUT(err, e);
        return -1;
    }
    PalSockAddr tmp;
    if (!pnet_from_native(&ss, len, peer != NULL ? peer : &tmp)) {
        /* Go's Accept closes a connection from an address it cannot
         * represent and says EAFNOSUPPORT. */
        close(nfd);
        BURROW_OUT(err, PAL_EAFNOSUPPORT);
        return -1;
    }
    return nfd;
}

static bool pnet_name(int64_t fd, PalSockAddr *out, bool peer, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!pnet_fd_ok(fd, err))
        return false;
    if (out == NULL) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }
    struct sockaddr_storage ss;
    socklen_t len = (socklen_t)sizeof ss;
    memset(&ss, 0, sizeof ss);
    int r = peer ? getpeername((int)fd, (struct sockaddr *)&ss, &len)
                 : getsockname((int)fd, (struct sockaddr *)&ss, &len);
    if (r != 0)
        return pnet_fail(err);
    if (!pnet_from_native(&ss, len, out)) {
        BURROW_OUT(err, PAL_EAFNOSUPPORT);
        return false;
    }
    return true;
}

bool pal_getsockname(int64_t fd, PalSockAddr *out, PalErrno *err) {
    return pnet_name(fd, out, false, err);
}

bool pal_getpeername(int64_t fd, PalSockAddr *out, PalErrno *err) {
    return pnet_name(fd, out, true, err);
}

bool pal_connect(int64_t fd, const PalSockAddr *addr, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    struct sockaddr_storage ss;
    socklen_t len = 0;
    if (!pnet_fd_ok(fd, err) || !pnet_to_native(addr, &ss, &len, err))
        return false;
    if (connect((int)fd, (const struct sockaddr *)&ss, len) != 0)
        return pnet_fail(err);
    return true;
}

int64_t pal_sendto(int64_t fd, const void *buf, int64_t n, const PalSockAddr *addr,
                   PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!pnet_fd_ok(fd, err))
        return -1;
    if (buf == NULL && n > 0) {
        BURROW_OUT(err, PAL_EFAULT);
        return -1;
    }
    ssize_t r;
    if (addr == NULL) {
        r = send((int)fd, buf, pnet_count(n), PNET_SEND_FLAGS);
    } else {
        struct sockaddr_storage ss;
        socklen_t len = 0;
        if (!pnet_to_native(addr, &ss, &len, err))
            return -1;
        r = sendto((int)fd, buf, pnet_count(n), PNET_SEND_FLAGS,
                   (const struct sockaddr *)&ss, len);
    }
    if (r < 0)
        return pnet_fail_n(err);
    return (int64_t)r;
}

int64_t pal_recvfrom(int64_t fd, void *buf, int64_t n, PalSockAddr *from,
                     PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!pnet_fd_ok(fd, err))
        return -1;
    if (buf == NULL && n > 0) {
        BURROW_OUT(err, PAL_EFAULT);
        return -1;
    }
    if (from == NULL) {
        ssize_t r = recv((int)fd, buf, pnet_count(n), 0);
        if (r < 0)
            return pnet_fail_n(err);
        return (int64_t)r;
    }
    struct sockaddr_storage ss;
    socklen_t len = (socklen_t)sizeof ss;
    memset(&ss, 0, sizeof ss);
    ssize_t r = recvfrom((int)fd, buf, pnet_count(n), 0, (struct sockaddr *)&ss, &len);
    if (r < 0)
        return pnet_fail_n(err);
    if (!pnet_from_native(&ss, len, from))
        from->family = PAL_AF_UNSPEC;
    return (int64_t)r;
}

/* Where recvmsg can make the descriptors it hands over close-on-exec itself,
 * it does, and elsewhere they are made so after, as Go's
 * setReadMsgCloseOnExec does, which leaves a window a fork can slip into. */
#if defined(MSG_CMSG_CLOEXEC)
#define PNET_RECVMSG_FLAGS MSG_CMSG_CLOEXEC
static void pnet_rights_cloexec(struct msghdr *msg) {
    (void)msg;
}
#else
#define PNET_RECVMSG_FLAGS 0
static void pnet_rights_cloexec(struct msghdr *msg) {
    if (msg->msg_controllen == 0)
        return;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(msg); c != NULL; c = CMSG_NXTHDR(msg, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS)
            continue;
        const unsigned char *d = CMSG_DATA(c);
        size_t len = (size_t)c->cmsg_len - (size_t)(d - (const unsigned char *)c);
        for (size_t i = 0; i + sizeof(int) <= len; i += sizeof(int)) {
            int rfd;
            memcpy(&rfd, d + i, sizeof rfd);
            (void)fcntl(rfd, F_SETFD, FD_CLOEXEC);
        }
    }
}
#endif

/* Whether a message with control data and no data needs a byte to carry it.
 * Go's syscall package on Linux and AIX asks whether fd is something other
 * than a datagram socket, and gives up on the message when it cannot ask, and
 * on the BSDs and Solaris it always sends the byte. */
static bool pnet_needs_byte(int fd, PalErrno *err, bool *ok) {
#if !defined(BURROW_OS_LINUX) && !defined(BURROW_OS_ANDROID) && !defined(BURROW_OS_AIX)
    (void)fd;
    (void)err;
    *ok = true;
    return true;
#else
    int type = 0;
    socklen_t len = (socklen_t)sizeof type;
    *ok = getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &len) == 0;
    if (!*ok) {
        (void)pnet_fail(err);
        return false;
    }
    return type != SOCK_DGRAM;
#endif
}

int64_t pal_recvmsg(int64_t fd, void *buf, int64_t n, void *oob, int64_t oobcap,
                    int64_t *oobn, int32_t *flags, PalSockAddr *from, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (oobn != NULL)
        *oobn = 0;
    if (flags != NULL)
        *flags = 0;
    if (!pnet_fd_ok(fd, err))
        return -1;
    if ((buf == NULL && n > 0) || (oob == NULL && oobcap > 0)) {
        BURROW_OUT(err, PAL_EFAULT);
        return -1;
    }
    struct sockaddr_storage ss;
    memset(&ss, 0, sizeof ss);
    unsigned char dummy = 0;
    struct iovec iov;
    iov.iov_base = buf;
    iov.iov_len = pnet_count(n);
    struct msghdr msg;
    memset(&msg, 0, sizeof msg);
    msg.msg_name = &ss;
    msg.msg_namelen = (socklen_t)sizeof ss;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    if (oobcap > 0) {
        if (n <= 0) {
            bool ok = false;
            if (pnet_needs_byte((int)fd, err, &ok)) {
                iov.iov_base = &dummy;
                iov.iov_len = 1;
            } else if (!ok) {
                return -1;
            }
        }
        msg.msg_control = oob;
        msg.msg_controllen = (socklen_t)(oobcap > INT32_MAX ? INT32_MAX : oobcap);
    }
    ssize_t r = recvmsg((int)fd, &msg, PNET_RECVMSG_FLAGS);
    if (r < 0)
        return pnet_fail_n(err);
    pnet_rights_cloexec(&msg);
    if (oobn != NULL)
        *oobn = (int64_t)msg.msg_controllen;
    if (flags != NULL)
        *flags = (int32_t)msg.msg_flags;
    if (from != NULL && !pnet_from_native(&ss, msg.msg_namelen, from))
        from->family = PAL_AF_UNSPEC;
    return (int64_t)r;
}

int64_t pal_sendmsg(int64_t fd, const void *buf, int64_t n, const void *oob,
                    int64_t oobn, const PalSockAddr *to, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!pnet_fd_ok(fd, err))
        return -1;
    if ((buf == NULL && n > 0) || (oob == NULL && oobn > 0)) {
        BURROW_OUT(err, PAL_EFAULT);
        return -1;
    }
    struct sockaddr_storage ss;
    socklen_t sslen = 0;
    if (to != NULL && !pnet_to_native(to, &ss, &sslen, err))
        return -1;
    unsigned char dummy = 0;
    struct iovec iov;
    iov.iov_base = (void *)(uintptr_t)buf;
    iov.iov_len = pnet_count(n);
    struct msghdr msg;
    memset(&msg, 0, sizeof msg);
    if (to != NULL) {
        msg.msg_name = &ss;
        msg.msg_namelen = sslen;
    }
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    if (oobn > 0) {
        if (n <= 0) {
            bool ok = false;
            if (pnet_needs_byte((int)fd, err, &ok)) {
                iov.iov_base = &dummy;
                iov.iov_len = 1;
            } else if (!ok) {
                return -1;
            }
        }
        msg.msg_control = (void *)(uintptr_t)oob;
        msg.msg_controllen = (socklen_t)(oobn > INT32_MAX ? INT32_MAX : oobn);
    }
    ssize_t r = sendmsg((int)fd, &msg, PNET_SEND_FLAGS);
    if (r < 0)
        return pnet_fail_n(err);
    if (oobn > 0 && n <= 0)
        return 0;
    return (int64_t)r;
}

_Static_assert(sizeof(PalIovec) == sizeof(struct iovec) &&
                   offsetof(PalIovec, base) == offsetof(struct iovec, iov_base) &&
                   offsetof(PalIovec, len) == offsetof(struct iovec, iov_len),
               "PalIovec is laid out as struct iovec");

int64_t pal_writev(int64_t fd, const PalIovec *v, int32_t count, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!pnet_fd_ok(fd, err))
        return -1;
    if ((v == NULL && count > 0) || count < 0) {
        BURROW_OUT(err, PAL_EFAULT);
        return -1;
    }
    struct msghdr msg;
    memset(&msg, 0, sizeof msg);
    msg.msg_iov = (struct iovec *)(uintptr_t)v;
    /* size_t on glibc and int on the BSDs and musl, and count fits both. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
    msg.msg_iovlen = count;
#pragma GCC diagnostic pop
    ssize_t r = sendmsg((int)fd, &msg, PNET_SEND_FLAGS);
    if (r < 0)
        return pnet_fail_n(err);
    return (int64_t)r;
}

/* An option of ours as the level and name the platform knows it by, and
 * whether it is on or off rather than a number, since macOS and the BSDs read
 * those back as the flag's bit and not as 1. False for one this platform does
 * not have. */
static bool pnet_opt(int32_t opt, int *level, int *name, bool *flag) {
    *flag = opt == PAL_SO_REUSEADDR || opt == PAL_SO_REUSEPORT ||
            opt == PAL_SO_KEEPALIVE || opt == PAL_SO_BROADCAST ||
            opt == PAL_TCP_NODELAY || opt == PAL_IPV6_V6ONLY ||
            opt == PAL_IP_MULTICAST_LOOP || opt == PAL_IPV6_MULTICAST_LOOP;
    switch (opt) {
    case PAL_SO_REUSEADDR:
        *level = SOL_SOCKET;
        *name = SO_REUSEADDR;
        return true;
    case PAL_SO_REUSEPORT:
#if defined(SO_REUSEPORT)
        *level = SOL_SOCKET;
        *name = SO_REUSEPORT;
        return true;
#else
        return false;
#endif
    case PAL_SO_KEEPALIVE:
        *level = SOL_SOCKET;
        *name = SO_KEEPALIVE;
        return true;
    case PAL_SO_BROADCAST:
        *level = SOL_SOCKET;
        *name = SO_BROADCAST;
        return true;
    case PAL_SO_LINGER:
        *level = SOL_SOCKET;
        *name = SO_LINGER;
        return true;
    case PAL_SO_RCVBUF:
        *level = SOL_SOCKET;
        *name = SO_RCVBUF;
        return true;
    case PAL_SO_SNDBUF:
        *level = SOL_SOCKET;
        *name = SO_SNDBUF;
        return true;
    case PAL_SO_ERROR:
        *level = SOL_SOCKET;
        *name = SO_ERROR;
        return true;
    case PAL_SO_TYPE:
        *level = SOL_SOCKET;
        *name = SO_TYPE;
        return true;
    case PAL_TCP_NODELAY:
        *level = IPPROTO_TCP;
        *name = TCP_NODELAY;
        return true;
    case PAL_TCP_KEEPIDLE:
        *level = IPPROTO_TCP;
#if defined(TCP_KEEPIDLE)
        *name = TCP_KEEPIDLE;
        return true;
#elif defined(TCP_KEEPALIVE)
        /* macOS's name for it, which Go uses there. */
        *name = TCP_KEEPALIVE;
        return true;
#else
        return false;
#endif
    case PAL_TCP_KEEPINTVL:
#if defined(TCP_KEEPINTVL)
        *level = IPPROTO_TCP;
        *name = TCP_KEEPINTVL;
        return true;
#else
        return false;
#endif
    case PAL_TCP_KEEPCNT:
#if defined(TCP_KEEPCNT)
        *level = IPPROTO_TCP;
        *name = TCP_KEEPCNT;
        return true;
#else
        return false;
#endif
    case PAL_IP_TTL:
        *level = IPPROTO_IP;
        *name = IP_TTL;
        return true;
    case PAL_IPV6_V6ONLY:
        *level = IPPROTO_IPV6;
        *name = IPV6_V6ONLY;
        return true;
    case PAL_IPV6_HOPLIMIT:
        *level = IPPROTO_IPV6;
        *name = IPV6_UNICAST_HOPS;
        return true;
    case PAL_IP_MULTICAST_LOOP:
        *level = IPPROTO_IP;
        *name = IP_MULTICAST_LOOP;
        return true;
    case PAL_IPV6_MULTICAST_IF:
        *level = IPPROTO_IPV6;
        *name = IPV6_MULTICAST_IF;
        return true;
    case PAL_IPV6_MULTICAST_LOOP:
        *level = IPPROTO_IPV6;
        *name = IPV6_MULTICAST_LOOP;
        return true;
    default:
        return false;
    }
}

/* Whether IP_MULTICAST_LOOP is a u_char here rather than an int, which is
 * what Go's sockoptip4_bsdvar.go passes it as. */
#if defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS) ||                             \
    defined(BURROW_OS_FREEBSD) || defined(BURROW_OS_NETBSD) ||                         \
    defined(BURROW_OS_OPENBSD) || defined(BURROW_OS_DRAGONFLY) ||                      \
    defined(BURROW_OS_SOLARIS) || defined(BURROW_OS_AIX)
#define PNET_LOOP_BYTE 1
#else
#define PNET_LOOP_BYTE 0
#endif

bool pal_getsockopt(int64_t fd, int32_t opt, int64_t *value, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!pnet_fd_ok(fd, err))
        return false;
    if (value == NULL) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }
    int level = 0;
    int name = 0;
    bool flag = false;
    if (!pnet_opt(opt, &level, &name, &flag)) {
        BURROW_OUT(err, PAL_ENOTSUP);
        return false;
    }
    if (opt == PAL_SO_LINGER) {
        struct linger l;
        socklen_t len = (socklen_t)sizeof l;
        memset(&l, 0, sizeof l);
        if (getsockopt((int)fd, level, name, &l, &len) != 0)
            return pnet_fail(err);
        *value = l.l_onoff ? (int64_t)l.l_linger : -1;
        return true;
    }
    if (PNET_LOOP_BYTE && opt == PAL_IP_MULTICAST_LOOP) {
        unsigned char c = 0;
        socklen_t len = (socklen_t)sizeof c;
        if (getsockopt((int)fd, level, name, &c, &len) != 0)
            return pnet_fail(err);
        *value = c != 0;
        return true;
    }
    int v = 0;
    socklen_t len = (socklen_t)sizeof v;
    if (getsockopt((int)fd, level, name, &v, &len) != 0)
        return pnet_fail(err);
    if (opt == PAL_SO_ERROR)
        *value = v == 0 ? (int64_t)PAL_OK : (int64_t)burrow__pal_errno(v);
    else if (opt == PAL_SO_TYPE)
        *value = pnet_type_from(v);
    else if (flag)
        *value = v != 0;
    else
        *value = v;
    return true;
}

bool pal_setsockopt(int64_t fd, int32_t opt, int64_t value, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!pnet_fd_ok(fd, err))
        return false;
    int level = 0;
    int name = 0;
    bool flag = false;
    if (!pnet_opt(opt, &level, &name, &flag)) {
        BURROW_OUT(err, PAL_ENOTSUP);
        return false;
    }
    (void)flag;
    if (opt == PAL_SO_ERROR || opt == PAL_SO_TYPE || value > INT32_MAX || value < -1 ||
        (value < 0 && opt != PAL_SO_LINGER)) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }
    int r;
    if (opt == PAL_SO_LINGER) {
        struct linger l;
        memset(&l, 0, sizeof l);
        l.l_onoff = value >= 0;
        l.l_linger = value >= 0 ? (int)value : 0;
        r = setsockopt((int)fd, level, name, &l, (socklen_t)sizeof l);
    } else if (PNET_LOOP_BYTE && opt == PAL_IP_MULTICAST_LOOP) {
        unsigned char c = value != 0;
        r = setsockopt((int)fd, level, name, &c, (socklen_t)sizeof c);
    } else {
        int v = (int)value;
        r = setsockopt((int)fd, level, name, &v, (socklen_t)sizeof v);
    }
    if (r != 0)
        return pnet_fail(err);
    return true;
}

bool pal_setsockopt_mreq(int64_t fd, int32_t opt, const PalMreq *m, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!pnet_fd_ok(fd, err))
        return false;
    if (m == NULL || m->index < 0) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }
    int r;
    switch (opt) {
    case PAL_MREQ_IPV4_IF:
    case PAL_MREQ_IPV4_JOIN: {
#if defined(BURROW_OS_LINUX)
        struct ip_mreqn q;
        memset(&q, 0, sizeof q);
        q.imr_ifindex = (int)m->index;
        if (opt == PAL_MREQ_IPV4_JOIN)
            memcpy(&q.imr_multiaddr, m->group, 4);
        r = setsockopt((int)fd, IPPROTO_IP,
                       opt == PAL_MREQ_IPV4_IF ? IP_MULTICAST_IF : IP_ADD_MEMBERSHIP,
                       &q, (socklen_t)sizeof q);
#else
        if (opt == PAL_MREQ_IPV4_IF) {
            struct in_addr a;
            memcpy(&a, m->ifaddr, 4);
            r = setsockopt((int)fd, IPPROTO_IP, IP_MULTICAST_IF, &a,
                           (socklen_t)sizeof a);
        } else {
            struct ip_mreq q;
            memset(&q, 0, sizeof q);
            memcpy(&q.imr_multiaddr, m->group, 4);
            memcpy(&q.imr_interface, m->ifaddr, 4);
            r = setsockopt((int)fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &q,
                           (socklen_t)sizeof q);
        }
#endif
        break;
    }
    case PAL_MREQ_IPV6_JOIN: {
        struct ipv6_mreq q;
        memset(&q, 0, sizeof q);
        memcpy(&q.ipv6mr_multiaddr, m->group, 16);
        q.ipv6mr_interface = (unsigned int)m->index;
        r = setsockopt((int)fd, IPPROTO_IPV6, IPV6_JOIN_GROUP, &q, (socklen_t)sizeof q);
        break;
    }
    default:
        BURROW_OUT(err, PAL_ENOTSUP);
        return false;
    }
    if (r != 0)
        return pnet_fail(err);
    return true;
}

bool pal_shutdown(int64_t fd, int32_t how, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!pnet_fd_ok(fd, err))
        return false;
    int h;
    switch (how) {
    case PAL_SHUT_RD:
        h = SHUT_RD;
        break;
    case PAL_SHUT_WR:
        h = SHUT_WR;
        break;
    case PAL_SHUT_RDWR:
        h = SHUT_RDWR;
        break;
    default:
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }
    if (shutdown((int)fd, h) != 0)
        return pnet_fail(err);
    return true;
}

#if defined(BURROW_OS_LINUX)

/* unix.KernelVersionGE(4, 1), from the release uname gives, such as
 * "6.8.0-45-generic". A release that does not parse counts as new enough. */
static bool pnet_kernel_4_1(void) {
    struct utsname u;
    if (uname(&u) != 0)
        return true;
    long major = 0;
    long minor = 0;
    const char *p = u.release;
    while (*p >= '0' && *p <= '9')
        major = major * 10 + (*p++ - '0');
    if (*p == '.')
        p++;
    while (*p >= '0' && *p <= '9')
        minor = minor * 10 + (*p++ - '0');
    return major > 4 || (major == 4 && minor >= 1);
}

/* Go's maxListenerBacklog on Linux: the first field of
 * /proc/sys/net/core/somaxconn, read with Go's dtoi, which gives up on a
 * number of 0xFFFFFF or more. Above 65535 it is capped at what the kernel's
 * backlog field holds, 16 bits before 4.1 and 32 bits since. */
int32_t pal_listen_backlog_max(void) {
    int fd;
    do {
        fd = open("/proc/sys/net/core/somaxconn", O_RDONLY | O_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0)
        return 0;
    char buf[64];
    ssize_t n;
    do {
        n = read(fd, buf, sizeof buf);
    } while (n < 0 && errno == EINTR);
    (void)close(fd);
    if (n <= 0)
        return 0;
    ssize_t i = 0;
    while (i < n && (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\r'))
        i++;
    long v = 0;
    ssize_t start = i;
    while (i < n && buf[i] >= '0' && buf[i] <= '9') {
        v = v * 10 + (buf[i] - '0');
        if (v >= 0xFFFFFF)
            return 0;
        i++;
    }
    if (i == start || v == 0)
        return 0;
    if (v > 65535 && !pnet_kernel_4_1())
        v = 65535;
    return (int32_t)v;
}

#elif defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS) ||                           \
    defined(BURROW_OS_FREEBSD) || defined(BURROW_OS_OPENBSD)

/* Go's maxListenerBacklog on the BSDs: the sysctl each of them keeps the
 * limit in, capped at 65535. NetBSD has none, and Go asks nothing there. */
int32_t pal_listen_backlog_max(void) {
#if defined(BURROW_OS_FREEBSD)
    const char *name = "kern.ipc.soacceptqueue";
#elif defined(BURROW_OS_OPENBSD)
    const char *name = "kern.somaxconn";
#else
    const char *name = "kern.ipc.somaxconn";
#endif
    uint32_t v = 0;
    size_t len = sizeof v;
    if (sysctlbyname(name, &v, &len, NULL, 0) != 0 || len != sizeof v)
        return 0;
    if (v > 65535)
        v = 65535;
    return (int32_t)v;
}

#else

int32_t pal_listen_backlog_max(void) {
    return 0;
}

#endif

#endif /* BURROW_OS_WASI */

#endif /* !BURROW_OS_WINDOWS */

/* Sockets on Windows, through Winsock.
 *
 * The calls have the BSD names and nearly the BSD shapes, and the differences
 * are the ones this file is for. A socket is a SOCKET, which is a handle and
 * not a small int, and it is closed with closesocket. Winsock has to be
 * started before the first call, which Go does in syscall's init and this
 * does the first time it is needed. A non blocking connect says
 * WSAEWOULDBLOCK where every other system says EINPROGRESS. The lengths are
 * ints, the option values are char pointers, and the Unix domain sockaddr is
 * in a header that older SDKs do not have.
 *
 * Sockets are made overlapped as well as non blocking, so that the completion
 * port can take them when net arrives, and are not inherited by children,
 * which is Windows' close on exec.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/platform.h"

#if defined(BURROW_OS_WINDOWS)

#include "burrow/pal.h"

#include "internal.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* winsock2.h before windows.h, for the reason errno_windows.c gives, and
 * ws2tcpip.h after it for IPv6. */
#include <winsock2.h>

#include <ws2tcpip.h>

#include <windows.h>

#if !defined(WSA_FLAG_NO_HANDLE_INHERIT)
#define WSA_FLAG_NO_HANDLE_INHERIT 0x80
#endif

#if !defined(AF_UNIX)
#define AF_UNIX 1
#endif

/* The TCP keepalive options, which Windows 10 1709 added and older headers do
 * not name. Go's own numbers for them. */
#define WNET_TCP_KEEPIDLE 3
#define WNET_TCP_KEEPCNT 16
#define WNET_TCP_KEEPINTVL 17

/* afunix.h's sockaddr_un, which is not in every SDK burrow builds with. */
typedef struct WnetSockaddrUn {
    u_short sun_family;
    char sun_path[108];
} WnetSockaddrUn;

/* Starts Winsock once, as Go's syscall does in its init. wsa_start_rc is what
 * WSAStartup said, which every call after the first looks at. */
static INIT_ONCE wsa_once = INIT_ONCE_STATIC_INIT;
static int wsa_start_rc = 0;

static BOOL CALLBACK wnet_start(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    (void)once;
    (void)param;
    (void)ctx;
    WSADATA d;
    wsa_start_rc = WSAStartup(MAKEWORD(2, 2), &d);
    return TRUE;
}

static bool wnet_ready(PalErrno *err) {
    InitOnceExecuteOnce(&wsa_once, wnet_start, NULL, NULL);
    if (wsa_start_rc != 0) {
        BURROW_OUT(err, burrow__pal_errno_wsa(wsa_start_rc));
        return false;
    }
    return true;
}

static bool wnet_fail(PalErrno *err) {
    BURROW_OUT(err, burrow__pal_errno_wsa(WSAGetLastError()));
    return false;
}

static int64_t wnet_fail_n(PalErrno *err) {
    BURROW_OUT(err, burrow__pal_errno_wsa(WSAGetLastError()));
    return -1;
}

static bool wnet_fd_ok(int64_t fd, PalErrno *err) {
    if (fd < 0) {
        BURROW_OUT(err, PAL_EBADF);
        return false;
    }
    return wnet_ready(err);
}

static int wnet_count(int64_t n) {
    if (n <= 0)
        return 0;
    if (n > INT_MAX)
        return INT_MAX;
    return (int)n;
}

static bool wnet_family(int32_t family, int *out) {
    switch (family) {
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

static bool wnet_type(int32_t type, int *out) {
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
    case PAL_SOCK_SEQPACKET:
        *out = SOCK_SEQPACKET;
        return true;
    default:
        return false;
    }
}

static bool wnet_to_native(const PalSockAddr *a, struct sockaddr_storage *ss, int *len,
                           PalErrno *err) {
    memset(ss, 0, sizeof *ss);
    if (a == NULL) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }
    switch (a->family) {
    case PAL_AF_INET: {
        struct sockaddr_in *in = (struct sockaddr_in *)ss;
        in->sin_family = AF_INET;
        in->sin_port = htons(a->port);
        memcpy(&in->sin_addr, a->addr, 4);
        *len = (int)sizeof *in;
        return true;
    }
    case PAL_AF_INET6: {
        struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)ss;
        in6->sin6_family = AF_INET6;
        in6->sin6_port = htons(a->port);
        in6->sin6_scope_id = a->scope_id;
        memcpy(&in6->sin6_addr, a->addr, 16);
        *len = (int)sizeof *in6;
        return true;
    }
    case PAL_AF_UNIX: {
        WnetSockaddrUn *un = (WnetSockaddrUn *)ss;
        size_t n = a->path_len;
        if (n > sizeof un->sun_path || n > sizeof a->path) {
            BURROW_OUT(err, PAL_EINVAL);
            return false;
        }
        un->sun_family = AF_UNIX;
        memcpy(un->sun_path, a->path, n);
        *len = (int)(offsetof(WnetSockaddrUn, sun_path) + n);
        return true;
    }
    default:
        BURROW_OUT(err, PAL_EAFNOSUPPORT);
        return false;
    }
}

static bool wnet_from_native(const struct sockaddr_storage *ss, int len,
                             PalSockAddr *out) {
    memset(out, 0, sizeof *out);
    if (len < (int)sizeof ss->ss_family)
        return true;
    switch (ss->ss_family) {
    case AF_UNSPEC:
        return true;
    case AF_INET: {
        const struct sockaddr_in *in = (const struct sockaddr_in *)ss;
        out->family = PAL_AF_INET;
        out->port = ntohs(in->sin_port);
        memcpy(out->addr, &in->sin_addr, 4);
        return true;
    }
    case AF_INET6: {
        const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)ss;
        out->family = PAL_AF_INET6;
        out->port = ntohs(in6->sin6_port);
        out->scope_id = (uint32_t)in6->sin6_scope_id;
        memcpy(out->addr, &in6->sin6_addr, 16);
        return true;
    }
    case AF_UNIX: {
        const WnetSockaddrUn *un = (const WnetSockaddrUn *)ss;
        size_t off = offsetof(WnetSockaddrUn, sun_path);
        size_t n = (size_t)len > off ? (size_t)len - off : 0;
        if (n > sizeof un->sun_path)
            n = sizeof un->sun_path;
        out->family = PAL_AF_UNIX;
        out->path_len = (uint16_t)n;
        memcpy(out->path, un->sun_path, n);
        return true;
    }
    default:
        return false;
    }
}

/* Non blocking, and not inherited. The new socket is closed on failure. */
static bool wnet_setup(SOCKET s, PalErrno *err) {
    u_long on = 1;
    if (ioctlsocket(s, (long)FIONBIO, &on) != 0) {
        PalErrno e = burrow__pal_errno_wsa(WSAGetLastError());
        closesocket(s);
        BURROW_OUT(err, e);
        return false;
    }
    if (!SetHandleInformation((HANDLE)s, HANDLE_FLAG_INHERIT, 0)) {
        PalErrno e = burrow__pal_errno_win(GetLastError());
        closesocket(s);
        BURROW_OUT(err, e);
        return false;
    }
    return true;
}

int64_t pal_socket(int32_t family, int32_t type, int32_t protocol, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!wnet_ready(err))
        return -1;
    int fam = 0;
    int ty = 0;
    if (!wnet_family(family, &fam)) {
        BURROW_OUT(err, PAL_EAFNOSUPPORT);
        return -1;
    }
    if (!wnet_type(type, &ty) || protocol < 0) {
        BURROW_OUT(err, PAL_EINVAL);
        return -1;
    }
    SOCKET s = WSASocketW(fam, ty, (int)protocol, NULL, 0,
                          WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
    if (s == INVALID_SOCKET)
        return wnet_fail_n(err);
    if (!wnet_setup(s, err))
        return -1;
    return (int64_t)s;
}

bool pal_socket_close(int64_t fd, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!wnet_fd_ok(fd, err))
        return false;
    if (closesocket((SOCKET)fd) != 0)
        return wnet_fail(err);
    return true;
}

bool pal_bind(int64_t fd, const PalSockAddr *addr, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    struct sockaddr_storage ss;
    int len = 0;
    if (!wnet_fd_ok(fd, err) || !wnet_to_native(addr, &ss, &len, err))
        return false;
    if (bind((SOCKET)fd, (const struct sockaddr *)&ss, len) != 0)
        return wnet_fail(err);
    return true;
}

bool pal_listen(int64_t fd, int32_t backlog, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!wnet_fd_ok(fd, err))
        return false;
    if (listen((SOCKET)fd, (int)backlog) != 0)
        return wnet_fail(err);
    return true;
}

int64_t pal_accept(int64_t fd, PalSockAddr *peer, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!wnet_fd_ok(fd, err))
        return -1;
    struct sockaddr_storage ss;
    int len = (int)sizeof ss;
    memset(&ss, 0, sizeof ss);
    SOCKET s = accept((SOCKET)fd, (struct sockaddr *)&ss, &len);
    if (s == INVALID_SOCKET)
        return wnet_fail_n(err);
    if (!wnet_setup(s, err))
        return -1;
    PalSockAddr tmp;
    if (!wnet_from_native(&ss, len, peer != NULL ? peer : &tmp)) {
        closesocket(s);
        BURROW_OUT(err, PAL_EAFNOSUPPORT);
        return -1;
    }
    return (int64_t)s;
}

static bool wnet_name(int64_t fd, PalSockAddr *out, bool peer, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!wnet_fd_ok(fd, err))
        return false;
    if (out == NULL) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }
    struct sockaddr_storage ss;
    int len = (int)sizeof ss;
    memset(&ss, 0, sizeof ss);
    int r = peer ? getpeername((SOCKET)fd, (struct sockaddr *)&ss, &len)
                 : getsockname((SOCKET)fd, (struct sockaddr *)&ss, &len);
    if (r != 0)
        return wnet_fail(err);
    if (!wnet_from_native(&ss, len, out)) {
        BURROW_OUT(err, PAL_EAFNOSUPPORT);
        return false;
    }
    return true;
}

bool pal_getsockname(int64_t fd, PalSockAddr *out, PalErrno *err) {
    return wnet_name(fd, out, false, err);
}

bool pal_getpeername(int64_t fd, PalSockAddr *out, PalErrno *err) {
    return wnet_name(fd, out, true, err);
}

bool pal_connect(int64_t fd, const PalSockAddr *addr, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    struct sockaddr_storage ss;
    int len = 0;
    if (!wnet_fd_ok(fd, err) || !wnet_to_native(addr, &ss, &len, err))
        return false;
    if (connect((SOCKET)fd, (const struct sockaddr *)&ss, len) != 0) {
        int e = WSAGetLastError();
        if (e == WSAEWOULDBLOCK) {
            /* What the rest of the world calls EINPROGRESS. */
            burrow__pal_errno_note(e, PAL_EINPROGRESS);
            BURROW_OUT(err, PAL_EINPROGRESS);
            return false;
        }
        BURROW_OUT(err, burrow__pal_errno_wsa(e));
        return false;
    }
    return true;
}

int64_t pal_sendto(int64_t fd, const void *buf, int64_t n, const PalSockAddr *addr,
                   PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!wnet_fd_ok(fd, err))
        return -1;
    if (buf == NULL && n > 0) {
        BURROW_OUT(err, PAL_EFAULT);
        return -1;
    }
    int r;
    if (addr == NULL) {
        r = send((SOCKET)fd, (const char *)buf, wnet_count(n), 0);
    } else {
        struct sockaddr_storage ss;
        int len = 0;
        if (!wnet_to_native(addr, &ss, &len, err))
            return -1;
        r = sendto((SOCKET)fd, (const char *)buf, wnet_count(n), 0,
                   (const struct sockaddr *)&ss, len);
    }
    if (r == SOCKET_ERROR)
        return wnet_fail_n(err);
    return (int64_t)r;
}

int64_t pal_recvfrom(int64_t fd, void *buf, int64_t n, PalSockAddr *from,
                     PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!wnet_fd_ok(fd, err))
        return -1;
    if (buf == NULL && n > 0) {
        BURROW_OUT(err, PAL_EFAULT);
        return -1;
    }
    if (from == NULL) {
        int r = recv((SOCKET)fd, (char *)buf, wnet_count(n), 0);
        if (r == SOCKET_ERROR)
            return wnet_fail_n(err);
        return (int64_t)r;
    }
    struct sockaddr_storage ss;
    int len = (int)sizeof ss;
    memset(&ss, 0, sizeof ss);
    int r = recvfrom((SOCKET)fd, (char *)buf, wnet_count(n), 0, (struct sockaddr *)&ss,
                     &len);
    if (r == SOCKET_ERROR)
        return wnet_fail_n(err);
    if (!wnet_from_native(&ss, len, from))
        from->family = PAL_AF_UNSPEC;
    return (int64_t)r;
}

/* The level and name of an option, and whether it is on or off rather than a
 * number. False for one Windows does not have. */
static bool wnet_opt(int32_t opt, int *level, int *name, bool *flag) {
    *flag = false;
    switch (opt) {
    case PAL_SO_REUSEADDR:
        *level = SOL_SOCKET;
        *name = SO_REUSEADDR;
        *flag = true;
        return true;
    case PAL_SO_KEEPALIVE:
        *level = SOL_SOCKET;
        *name = SO_KEEPALIVE;
        *flag = true;
        return true;
    case PAL_SO_BROADCAST:
        *level = SOL_SOCKET;
        *name = SO_BROADCAST;
        *flag = true;
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
    case PAL_TCP_NODELAY:
        *level = IPPROTO_TCP;
        *name = TCP_NODELAY;
        *flag = true;
        return true;
    case PAL_TCP_KEEPIDLE:
        *level = IPPROTO_TCP;
        *name = WNET_TCP_KEEPIDLE;
        return true;
    case PAL_TCP_KEEPINTVL:
        *level = IPPROTO_TCP;
        *name = WNET_TCP_KEEPINTVL;
        return true;
    case PAL_TCP_KEEPCNT:
        *level = IPPROTO_TCP;
        *name = WNET_TCP_KEEPCNT;
        return true;
    case PAL_IP_TTL:
        *level = IPPROTO_IP;
        *name = IP_TTL;
        return true;
    case PAL_IPV6_V6ONLY:
        *level = IPPROTO_IPV6;
        *name = IPV6_V6ONLY;
        *flag = true;
        return true;
    case PAL_IPV6_HOPLIMIT:
        *level = IPPROTO_IPV6;
        *name = IPV6_UNICAST_HOPS;
        return true;
    default:
        /* PAL_SO_REUSEPORT among them: Windows has no such option. */
        return false;
    }
}

bool pal_getsockopt(int64_t fd, int32_t opt, int64_t *value, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!wnet_fd_ok(fd, err))
        return false;
    if (value == NULL) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }
    int level = 0;
    int name = 0;
    bool flag = false;
    if (!wnet_opt(opt, &level, &name, &flag)) {
        BURROW_OUT(err, PAL_ENOTSUP);
        return false;
    }
    if (opt == PAL_SO_LINGER) {
        struct linger l;
        int len = (int)sizeof l;
        memset(&l, 0, sizeof l);
        if (getsockopt((SOCKET)fd, level, name, (char *)&l, &len) != 0)
            return wnet_fail(err);
        *value = l.l_onoff ? (int64_t)l.l_linger : -1;
        return true;
    }
    /* Some options are a DWORD and some a BOOL, both four bytes, and a few
     * are written as one byte by older systems, so start from zero. */
    int v = 0;
    int len = (int)sizeof v;
    if (getsockopt((SOCKET)fd, level, name, (char *)&v, &len) != 0)
        return wnet_fail(err);
    if (opt == PAL_SO_ERROR)
        *value = v == 0 ? (int64_t)PAL_OK : (int64_t)burrow__pal_errno_wsa(v);
    else if (flag)
        *value = v != 0;
    else
        *value = v;
    return true;
}

bool pal_setsockopt(int64_t fd, int32_t opt, int64_t value, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!wnet_fd_ok(fd, err))
        return false;
    int level = 0;
    int name = 0;
    bool flag = false;
    if (!wnet_opt(opt, &level, &name, &flag)) {
        BURROW_OUT(err, PAL_ENOTSUP);
        return false;
    }
    (void)flag;
    if (opt == PAL_SO_ERROR || value > INT32_MAX || value < -1 ||
        (value < 0 && opt != PAL_SO_LINGER) ||
        (opt == PAL_SO_LINGER && value > 65535)) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }
    int r;
    if (opt == PAL_SO_LINGER) {
        struct linger l;
        memset(&l, 0, sizeof l);
        l.l_onoff = (u_short)(value >= 0);
        l.l_linger = (u_short)(value >= 0 ? value : 0);
        r = setsockopt((SOCKET)fd, level, name, (const char *)&l, (int)sizeof l);
    } else {
        int v = (int)value;
        r = setsockopt((SOCKET)fd, level, name, (const char *)&v, (int)sizeof v);
    }
    if (r != 0)
        return wnet_fail(err);
    return true;
}

bool pal_shutdown(int64_t fd, int32_t how, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (!wnet_fd_ok(fd, err))
        return false;
    int h;
    switch (how) {
    case PAL_SHUT_RD:
        h = SD_RECEIVE;
        break;
    case PAL_SHUT_WR:
        h = SD_SEND;
        break;
    case PAL_SHUT_RDWR:
        h = SD_BOTH;
        break;
    default:
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }
    if (shutdown((SOCKET)fd, h) != 0)
        return wnet_fail(err);
    return true;
}

/* Go's maxListenerBacklog on Windows is syscall.SOMAXCONN, which is the
 * caller's fallback. */
int32_t pal_listen_backlog_max(void) {
    return 0;
}

#endif /* BURROW_OS_WINDOWS */

/* The socket calls of syscall on every Unix: the Sockaddr types and the
 * system's form of each, Socket, Bind, Accept and the rest that take or give
 * one, and the Getsockopt and Setsockopt functions.
 *
 * Derived from Go's src/syscall/syscall_unix.go, syscall_linux.go,
 * syscall_bsd.go, syscall_darwin.go and syscall_freebsd.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/syscall.h"

#if !defined(BURROW_OS_WINDOWS)

#include "burrow/mem.h"

#include "internal.h"

#include <string.h>

#if defined(BURROW_OS_LINUX) || defined(BURROW_OS_COSMO) || defined(BURROW_OS_WASI)
#define SOCK_LINUX 1
#endif

#if defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS) || defined(BURROW_OS_FREEBSD)
#define SOCK_BSD 1
#endif

bool syscall_socket_disable_ipv6 = false;

static Error sock_errno(SyscallErrno e) {
    return burrow__syscall_errno_err(e);
}

/* ------------------------------------------------------------ descriptors */

static const Type sock_inet4_desc = {
    {(const Byte *)"SockaddrInet4", 13},
    {(const Byte *)"syscall", 7},
    KIND_STRUCT,
    (uint32_t)sizeof(SyscallSockaddrInet4),
    (uint16_t)_Alignof(SyscallSockaddrInet4),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x73616934U, /* "sai4" */
    NULL,
};

static const Type sock_inet6_desc = {
    {(const Byte *)"SockaddrInet6", 13},
    {(const Byte *)"syscall", 7},
    KIND_STRUCT,
    (uint32_t)sizeof(SyscallSockaddrInet6),
    (uint16_t)_Alignof(SyscallSockaddrInet6),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x73616936U, /* "sai6" */
    NULL,
};

static const Type sock_unix_desc = {
    {(const Byte *)"SockaddrUnix", 12},
    {(const Byte *)"syscall", 7},
    KIND_STRUCT,
    (uint32_t)sizeof(SyscallSockaddrUnix),
    (uint16_t)_Alignof(SyscallSockaddrUnix),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x7361756eU, /* "saun" */
    NULL,
};

const Type *const TYPE_SYSCALL_SOCKADDR_INET4 = &sock_inet4_desc;
const Type *const TYPE_SYSCALL_SOCKADDR_INET6 = &sock_inet6_desc;
const Type *const TYPE_SYSCALL_SOCKADDR_UNIX = &sock_unix_desc;

#if defined(SOCK_BSD)
static const Type sock_datalink_desc = {
    {(const Byte *)"SockaddrDatalink", 16},
    {(const Byte *)"syscall", 7},
    KIND_STRUCT,
    (uint32_t)sizeof(SyscallSockaddrDatalink),
    (uint16_t)_Alignof(SyscallSockaddrDatalink),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x7361646cU, /* "sadl" */
    NULL,
};

const Type *const TYPE_SYSCALL_SOCKADDR_DATALINK = &sock_datalink_desc;
#endif

#if defined(SOCK_LINUX)
static const Type sock_linklayer_desc = {
    {(const Byte *)"SockaddrLinklayer", 17},
    {(const Byte *)"syscall", 7},
    KIND_STRUCT,
    (uint32_t)sizeof(SyscallSockaddrLinklayer),
    (uint16_t)_Alignof(SyscallSockaddrLinklayer),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x73616c6cU, /* "sall" */
    NULL,
};

static const Type sock_netlink_desc = {
    {(const Byte *)"SockaddrNetlink", 15},
    {(const Byte *)"syscall", 7},
    KIND_STRUCT,
    (uint32_t)sizeof(SyscallSockaddrNetlink),
    (uint16_t)_Alignof(SyscallSockaddrNetlink),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x73616e6cU, /* "sanl" */
    NULL,
};

const Type *const TYPE_SYSCALL_SOCKADDR_LINKLAYER = &sock_linklayer_desc;
const Type *const TYPE_SYSCALL_SOCKADDR_NETLINK = &sock_netlink_desc;
#endif

/* --------------------------------------------------------- the sockaddrs */

/* The port, which the system keeps in network order, as two bytes. */
static void sock_put_port(void *raw_port, Int port) {
    uint8_t *p = (uint8_t *)raw_port;
    p[0] = (uint8_t)(port >> 8);
    p[1] = (uint8_t)port;
}

static Int sock_get_port(const void *raw_port) {
    const uint8_t *p = (const uint8_t *)raw_port;
    return ((Int)p[0] << 8) + (Int)p[1];
}

static void *sock_inet4_sockaddr(void *self, uint32_t *len, Error *err) {
    SyscallSockaddrInet4 *sa = (SyscallSockaddrInet4 *)self;
    if (sa->port < 0 || sa->port > 0xFFFF) {
        BURROW_OUT(err, sock_errno(SYSCALL_EINVAL));
        return NULL;
    }
#if defined(SOCK_BSD)
    sa->raw.len = SYSCALL_SIZEOF_SOCKADDR_INET4;
#endif
    sa->raw.family = SYSCALL_AF_INET;
    sock_put_port(&sa->raw.port, sa->port);
    memcpy(sa->raw.addr, sa->addr, sizeof sa->raw.addr);
    *len = SYSCALL_SIZEOF_SOCKADDR_INET4;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return &sa->raw;
}

static void *sock_inet6_sockaddr(void *self, uint32_t *len, Error *err) {
    SyscallSockaddrInet6 *sa = (SyscallSockaddrInet6 *)self;
    if (sa->port < 0 || sa->port > 0xFFFF) {
        BURROW_OUT(err, sock_errno(SYSCALL_EINVAL));
        return NULL;
    }
#if defined(SOCK_BSD)
    sa->raw.len = SYSCALL_SIZEOF_SOCKADDR_INET6;
#endif
    sa->raw.family = SYSCALL_AF_INET6;
    sock_put_port(&sa->raw.port, sa->port);
    sa->raw.scope_id = sa->zone_id;
    memcpy(sa->raw.addr, sa->addr, sizeof sa->raw.addr);
    *len = SYSCALL_SIZEOF_SOCKADDR_INET6;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return &sa->raw;
}

/* On Linux a name that starts with @ or a NUL is abstract: it goes with a NUL
 * first and none after, and may fill the whole path. Any other name needs room
 * for the NUL after it, and the empty name is an unnamed socket. The BSDs have
 * no abstract names and no unnamed ones. */
static void *sock_unix_sockaddr(void *self, uint32_t *len, Error *err) {
    SyscallSockaddrUnix *sa = (SyscallSockaddrUnix *)self;
    Int n = sa->name.len;
    Int max = (Int)sizeof sa->raw.path;
#if defined(SOCK_LINUX)
    bool abstract = n > 0 && (sa->name.p[0] == '@' || sa->name.p[0] == 0);
    if (n > max || (n == max && !abstract)) {
        BURROW_OUT(err, sock_errno(SYSCALL_EINVAL));
        return NULL;
    }
    sa->raw.family = SYSCALL_AF_UNIX;
    if (n > 0)
        memcpy(sa->raw.path, sa->name.p, (size_t)n);
    uint32_t sl = (uint32_t)(2 + n);
    if (abstract) {
        sa->raw.path[0] = 0;
    } else if (n > 0) {
        sa->raw.path[n] = 0;
        sl++;
    }
    *len = sl;
#else
    if (n >= max || n == 0) {
        BURROW_OUT(err, sock_errno(SYSCALL_EINVAL));
        return NULL;
    }
    sa->raw.len = (uint8_t)(3 + n);
    sa->raw.family = SYSCALL_AF_UNIX;
    memcpy(sa->raw.path, sa->name.p, (size_t)n);
    sa->raw.path[n] = 0;
    *len = sa->raw.len;
#endif
    BURROW_OUT(err, BURROW_NO_ERROR);
    return &sa->raw;
}

static const SyscallSockaddrVT sock_inet4_vt = {&sock_inet4_desc, sock_inet4_sockaddr};
static const SyscallSockaddrVT sock_inet6_vt = {&sock_inet6_desc, sock_inet6_sockaddr};
static const SyscallSockaddrVT sock_unix_vt = {&sock_unix_desc, sock_unix_sockaddr};

SyscallSockaddr syscall_sockaddr_inet4_as_sockaddr(SyscallSockaddrInet4 *sa) {
    return (SyscallSockaddr){&sock_inet4_vt, sa};
}

SyscallSockaddr syscall_sockaddr_inet6_as_sockaddr(SyscallSockaddrInet6 *sa) {
    return (SyscallSockaddr){&sock_inet6_vt, sa};
}

SyscallSockaddr syscall_sockaddr_unix_as_sockaddr(SyscallSockaddrUnix *sa) {
    return (SyscallSockaddr){&sock_unix_vt, sa};
}

#if defined(SOCK_BSD)
static void *sock_datalink_sockaddr(void *self, uint32_t *len, Error *err) {
    SyscallSockaddrDatalink *sa = (SyscallSockaddrDatalink *)self;
    if (sa->index == 0) {
        BURROW_OUT(err, sock_errno(SYSCALL_EINVAL));
        return NULL;
    }
    sa->raw.len = sa->len;
    sa->raw.family = SYSCALL_AF_LINK;
    sa->raw.index = sa->index;
    sa->raw.type = sa->type;
    sa->raw.nlen = sa->nlen;
    sa->raw.alen = sa->alen;
    sa->raw.slen = sa->slen;
    memcpy(sa->raw.data, sa->data, sizeof sa->raw.data);
    *len = SYSCALL_SIZEOF_SOCKADDR_DATALINK;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return &sa->raw;
}

static const SyscallSockaddrVT sock_datalink_vt = {&sock_datalink_desc,
                                                   sock_datalink_sockaddr};

SyscallSockaddr syscall_sockaddr_datalink_as_sockaddr(SyscallSockaddrDatalink *sa) {
    return (SyscallSockaddr){&sock_datalink_vt, sa};
}
#endif

#if defined(SOCK_LINUX)
static void *sock_linklayer_sockaddr(void *self, uint32_t *len, Error *err) {
    SyscallSockaddrLinklayer *sa = (SyscallSockaddrLinklayer *)self;
    if (sa->ifindex < 0 || sa->ifindex > 0x7fffffff) {
        BURROW_OUT(err, sock_errno(SYSCALL_EINVAL));
        return NULL;
    }
    sa->raw.family = SYSCALL_AF_PACKET;
    sa->raw.protocol = sa->protocol;
    sa->raw.ifindex = (int32_t)sa->ifindex;
    sa->raw.hatype = sa->hatype;
    sa->raw.pkttype = sa->pkttype;
    sa->raw.halen = sa->halen;
    memcpy(sa->raw.addr, sa->addr, sizeof sa->raw.addr);
    *len = SYSCALL_SIZEOF_SOCKADDR_LINKLAYER;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return &sa->raw;
}

static void *sock_netlink_sockaddr(void *self, uint32_t *len, Error *err) {
    SyscallSockaddrNetlink *sa = (SyscallSockaddrNetlink *)self;
    sa->raw.family = SYSCALL_AF_NETLINK;
    sa->raw.pad = sa->pad;
    sa->raw.pid = sa->pid;
    sa->raw.groups = sa->groups;
    *len = SYSCALL_SIZEOF_SOCKADDR_NETLINK;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return &sa->raw;
}

static const SyscallSockaddrVT sock_linklayer_vt = {&sock_linklayer_desc,
                                                    sock_linklayer_sockaddr};
static const SyscallSockaddrVT sock_netlink_vt = {&sock_netlink_desc,
                                                  sock_netlink_sockaddr};

SyscallSockaddr syscall_sockaddr_linklayer_as_sockaddr(SyscallSockaddrLinklayer *sa) {
    return (SyscallSockaddr){&sock_linklayer_vt, sa};
}

SyscallSockaddr syscall_sockaddr_netlink_as_sockaddr(SyscallSockaddrNetlink *sa) {
    return (SyscallSockaddr){&sock_netlink_vt, sa};
}
#endif

/* The size of the one allocation behind a Sockaddr this file made. */
static size_t sock_alloc_size(SyscallSockaddr sa) {
    size_t n = sa.vt->self_type->size;
    if (sa.vt == &sock_unix_vt)
        n += (size_t)((SyscallSockaddrUnix *)sa.data)->name.len;
    return n;
}

void syscall_sockaddr_free(Alloc *a, SyscallSockaddr sa) {
    if (sa.vt == NULL || sa.data == NULL)
        return;
    mem_free(a, sa.data, sock_alloc_size(sa), sa.vt->self_type->align);
}

static Error sock_unix_from(Alloc *a, const Byte *path, Int n, SyscallSockaddr *out) {
    SyscallSockaddrUnix *sa = (SyscallSockaddrUnix *)mem_alloc(
        a, sizeof(SyscallSockaddrUnix) + (size_t)n, _Alignof(SyscallSockaddrUnix));
    if (sa == NULL)
        return burrow_err_out_of_memory;
    Byte *name = (Byte *)(sa + 1);
    if (n > 0)
        memcpy(name, path, (size_t)n);
    sa->name = (Str){name, n};
    *out = (SyscallSockaddr){&sock_unix_vt, sa};
    return BURROW_NO_ERROR;
}

/* Go's anyToSockaddr: the Sockaddr for the address the system wrote in rsa,
 * made from a. */
Error burrow__syscall_any_to_sockaddr(Alloc *a, SyscallRawSockaddrAny *rsa,
                                      SyscallSockaddr *out) {
    *out = (SyscallSockaddr){NULL, NULL};
    switch (rsa->addr.family) {
#if defined(SOCK_LINUX)
    case SYSCALL_AF_NETLINK: {
        const SyscallRawSockaddrNetlink *pp =
            (const SyscallRawSockaddrNetlink *)(void *)rsa;
        SyscallSockaddrNetlink *sa = (SyscallSockaddrNetlink *)mem_alloc(
            a, sizeof(SyscallSockaddrNetlink), _Alignof(SyscallSockaddrNetlink));
        if (sa == NULL)
            return burrow_err_out_of_memory;
        sa->family = pp->family;
        sa->pad = pp->pad;
        sa->pid = pp->pid;
        sa->groups = pp->groups;
        *out = (SyscallSockaddr){&sock_netlink_vt, sa};
        return BURROW_NO_ERROR;
    }
    case SYSCALL_AF_PACKET: {
        const SyscallRawSockaddrLinklayer *pp =
            (const SyscallRawSockaddrLinklayer *)(void *)rsa;
        SyscallSockaddrLinklayer *sa = (SyscallSockaddrLinklayer *)mem_alloc(
            a, sizeof(SyscallSockaddrLinklayer), _Alignof(SyscallSockaddrLinklayer));
        if (sa == NULL)
            return burrow_err_out_of_memory;
        sa->protocol = pp->protocol;
        sa->ifindex = pp->ifindex;
        sa->hatype = pp->hatype;
        sa->pkttype = pp->pkttype;
        sa->halen = pp->halen;
        memcpy(sa->addr, pp->addr, sizeof sa->addr);
        *out = (SyscallSockaddr){&sock_linklayer_vt, sa};
        return BURROW_NO_ERROR;
    }
    case SYSCALL_AF_UNIX: {
        /* An abstract name comes with a NUL first, which Go shows as @, and
         * ends at the first NUL after, which is not what Linux says but what
         * everyone does. */
        SyscallRawSockaddrUnix *pp = (SyscallRawSockaddrUnix *)(void *)rsa;
        if (pp->path[0] == 0)
            pp->path[0] = '@';
        Int n = 0;
        while (n < (Int)sizeof pp->path && pp->path[n] != 0)
            n++;
        return sock_unix_from(a, (const Byte *)pp->path, n, out);
    }
#else
    case SYSCALL_AF_LINK: {
        const SyscallRawSockaddrDatalink *pp =
            (const SyscallRawSockaddrDatalink *)(void *)rsa;
        SyscallSockaddrDatalink *sa = (SyscallSockaddrDatalink *)mem_alloc(
            a, sizeof(SyscallSockaddrDatalink), _Alignof(SyscallSockaddrDatalink));
        if (sa == NULL)
            return burrow_err_out_of_memory;
        sa->len = pp->len;
        sa->family = pp->family;
        sa->index = pp->index;
        sa->type = pp->type;
        sa->nlen = pp->nlen;
        sa->alen = pp->alen;
        sa->slen = pp->slen;
        memcpy(sa->data, pp->data, sizeof sa->data);
        *out = (SyscallSockaddr){&sock_datalink_vt, sa};
        return BURROW_NO_ERROR;
    }
    case SYSCALL_AF_UNIX: {
        /* Some BSDs count the NUL after the path in its length and some do
         * not, so the path ends at the length or the first NUL in it. */
        const SyscallRawSockaddrUnix *pp = (const SyscallRawSockaddrUnix *)(void *)rsa;
        if (pp->len < 2 || pp->len > SYSCALL_SIZEOF_SOCKADDR_UNIX)
            return sock_errno(SYSCALL_EINVAL);
        Int n = (Int)pp->len - 2;
        for (Int i = 0; i < n; i++) {
            if (pp->path[i] == 0) {
                n = i;
                break;
            }
        }
        return sock_unix_from(a, (const Byte *)pp->path, n, out);
    }
#endif
    case SYSCALL_AF_INET: {
        const SyscallRawSockaddrInet4 *pp =
            (const SyscallRawSockaddrInet4 *)(void *)rsa;
        SyscallSockaddrInet4 *sa = (SyscallSockaddrInet4 *)mem_alloc(
            a, sizeof(SyscallSockaddrInet4), _Alignof(SyscallSockaddrInet4));
        if (sa == NULL)
            return burrow_err_out_of_memory;
        sa->port = sock_get_port(&pp->port);
        memcpy(sa->addr, pp->addr, sizeof sa->addr);
        *out = (SyscallSockaddr){&sock_inet4_vt, sa};
        return BURROW_NO_ERROR;
    }
    case SYSCALL_AF_INET6: {
        const SyscallRawSockaddrInet6 *pp =
            (const SyscallRawSockaddrInet6 *)(void *)rsa;
        SyscallSockaddrInet6 *sa = (SyscallSockaddrInet6 *)mem_alloc(
            a, sizeof(SyscallSockaddrInet6), _Alignof(SyscallSockaddrInet6));
        if (sa == NULL)
            return burrow_err_out_of_memory;
        sa->port = sock_get_port(&pp->port);
        sa->zone_id = pp->scope_id;
        memcpy(sa->addr, pp->addr, sizeof sa->addr);
        *out = (SyscallSockaddr){&sock_inet6_vt, sa};
        return BURROW_NO_ERROR;
    }
    default:
        return sock_errno(SYSCALL_EAFNOSUPPORT);
    }
}

/* sa's system form, or NULL and 0 for the zero Sockaddr. */
static void *sock_raw(SyscallSockaddr sa, uint32_t *len, Error *err) {
    *len = 0;
    if (sa.vt == NULL) {
        BURROW_OUT(err, BURROW_NO_ERROR);
        return NULL;
    }
    return sa.vt->sockaddr(sa.data, len, err);
}

/* ---------------------------------------------------------------- sockets */

Int syscall_socket(Int domain, Int typ, Int proto, Error *err) {
    if (domain == SYSCALL_AF_INET6 && syscall_socket_disable_ipv6) {
        BURROW_OUT(err, sock_errno(SYSCALL_EAFNOSUPPORT));
        return -1;
    }
    return burrow__syscall_socket(domain, typ, proto, err);
}

SyscallSocketpairRet syscall_socketpair(Int domain, Int typ, Int proto, Error *err) {
    int32_t fdx[2] = {0, 0};
    SyscallSocketpairRet r = {{0, 0}};
    Error e = burrow__syscall_socketpair(domain, typ, proto, fdx);
    if (BURROW_OK(e)) {
        r.fd[0] = fdx[0];
        r.fd[1] = fdx[1];
    }
    BURROW_OUT(err, e);
    return r;
}

Error syscall_bind(Int fd, SyscallSockaddr sa) {
    Error err = BURROW_NO_ERROR;
    uint32_t n = 0;
    void *ptr = sa.vt->sockaddr(sa.data, &n, &err);
    if (BURROW_FAILED(err))
        return err;
    return burrow__syscall_bind(fd, ptr, n);
}

Error syscall_connect(Int fd, SyscallSockaddr sa) {
    Error err = BURROW_NO_ERROR;
    uint32_t n = 0;
    void *ptr = sa.vt->sockaddr(sa.data, &n, &err);
    if (BURROW_FAILED(err))
        return err;
    return burrow__syscall_connect(fd, ptr, n);
}

/* What Accept and Accept4 share once the call is made. */
static Int sock_accepted(Alloc *a, Int nfd, SyscallRawSockaddrAny *rsa, uint32_t len,
                         SyscallSockaddr *sa, Error *err) {
    if (len > SYSCALL_SIZEOF_SOCKADDR_ANY)
        panic_str(BURROW_S("RawSockaddrAny too small"));
#if defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS)
    /* An xnu bug: an accepted socket with no address, where there should have
     * been ECONNABORTED. */
    if (len == 0) {
        (void)syscall_close(nfd);
        BURROW_OUT(sa, ((SyscallSockaddr){NULL, NULL}));
        BURROW_OUT(err, sock_errno(SYSCALL_ECONNABORTED));
        return 0;
    }
#endif
    SyscallSockaddr out;
    Error e = burrow__syscall_any_to_sockaddr(a, rsa, &out);
    if (BURROW_FAILED(e)) {
        (void)syscall_close(nfd);
        nfd = 0;
    }
    BURROW_OUT(sa, out);
    BURROW_OUT(err, e);
    return nfd;
}

#if defined(SOCK_LINUX) || defined(BURROW_OS_FREEBSD)
Int syscall_accept4(Alloc *a, Int fd, Int flags, SyscallSockaddr *sa, Error *err) {
    SyscallRawSockaddrAny rsa;
    memset(&rsa, 0, sizeof rsa);
    uint32_t len = SYSCALL_SIZEOF_SOCKADDR_ANY;
    Error e = BURROW_NO_ERROR;
    Int nfd = burrow__syscall_accept4(fd, &rsa, &len, flags, &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(sa, ((SyscallSockaddr){NULL, NULL}));
        BURROW_OUT(err, e);
        return nfd;
    }
    return sock_accepted(a, nfd, &rsa, len, sa, err);
}
#endif

Int syscall_accept(Alloc *a, Int fd, SyscallSockaddr *sa, Error *err) {
#if defined(SOCK_LINUX)
    return syscall_accept4(a, fd, 0, sa, err);
#else
    SyscallRawSockaddrAny rsa;
    memset(&rsa, 0, sizeof rsa);
    uint32_t len = SYSCALL_SIZEOF_SOCKADDR_ANY;
    Error e = BURROW_NO_ERROR;
    Int nfd = burrow__syscall_accept(fd, &rsa, &len, &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(sa, ((SyscallSockaddr){NULL, NULL}));
        BURROW_OUT(err, e);
        return nfd;
    }
    return sock_accepted(a, nfd, &rsa, len, sa, err);
#endif
}

SyscallSockaddr syscall_getsockname(Alloc *a, Int fd, Error *err) {
    SyscallRawSockaddrAny rsa;
    memset(&rsa, 0, sizeof rsa);
    uint32_t len = SYSCALL_SIZEOF_SOCKADDR_ANY;
    SyscallSockaddr sa = {NULL, NULL};
    Error e = burrow__syscall_getsockname(fd, &rsa, &len);
    if (BURROW_OK(e))
        e = burrow__syscall_any_to_sockaddr(a, &rsa, &sa);
    BURROW_OUT(err, e);
    return sa;
}

SyscallSockaddr syscall_getpeername(Alloc *a, Int fd, Error *err) {
    SyscallRawSockaddrAny rsa;
    memset(&rsa, 0, sizeof rsa);
    uint32_t len = SYSCALL_SIZEOF_SOCKADDR_ANY;
    SyscallSockaddr sa = {NULL, NULL};
    Error e = burrow__syscall_getpeername(fd, &rsa, &len);
    if (BURROW_OK(e))
        e = burrow__syscall_any_to_sockaddr(a, &rsa, &sa);
    BURROW_OUT(err, e);
    return sa;
}

Int syscall_recvfrom(Alloc *a, Int fd, Slice p, Int flags, SyscallSockaddr *from,
                     Error *err) {
    SyscallRawSockaddrAny rsa;
    memset(&rsa, 0, sizeof rsa);
    uint32_t len = SYSCALL_SIZEOF_SOCKADDR_ANY;
    SyscallSockaddr sa = {NULL, NULL};
    Error e = BURROW_NO_ERROR;
    Int n = burrow__syscall_recvfrom(fd, p, flags, &rsa, &len, &e);
    if (BURROW_OK(e) && rsa.addr.family != SYSCALL_AF_UNSPEC)
        e = burrow__syscall_any_to_sockaddr(a, &rsa, &sa);
    BURROW_OUT(from, sa);
    BURROW_OUT(err, e);
    return n;
}

/* Go's recvmsgRaw and sendmsgN, one message of p and oob. With oob and no p,
 * one byte goes or comes so that oob does, except of a datagram socket on
 * Linux, which takes a message with no data. */
static Error sock_msg(Int fd, Slice p, Slice oob, SyscallMsghdr *msg, SyscallIovec *iov,
                      uint8_t *dummy) {
    memset(iov, 0, sizeof *iov);
    if (p.len > 0) {
        iov->base = (uint8_t *)p.p;
        syscall_iovec_set_len(iov, p.len);
    }
    if (oob.len > 0) {
        if (p.len == 0) {
#if defined(SOCK_LINUX)
            Error e = BURROW_NO_ERROR;
            Int typ =
                syscall_getsockopt_int(fd, SYSCALL_SOL_SOCKET, SYSCALL_SO_TYPE, &e);
            if (BURROW_FAILED(e))
                return e;
            if (typ != SYSCALL_SOCK_DGRAM) {
                iov->base = dummy;
                syscall_iovec_set_len(iov, 1);
            }
#else
            (void)fd;
            iov->base = dummy;
            syscall_iovec_set_len(iov, 1);
#endif
        }
        msg->control = (uint8_t *)oob.p;
        syscall_msghdr_set_controllen(msg, oob.len);
    }
    msg->iov = iov;
    msg->iovlen = 1;
    return BURROW_NO_ERROR;
}

Int syscall_recvmsg(Alloc *a, Int fd, Slice p, Slice oob, Int flags, Int *oobn,
                    Int *recvflags, SyscallSockaddr *from, Error *err) {
    SyscallRawSockaddrAny rsa;
    memset(&rsa, 0, sizeof rsa);
    SyscallMsghdr msg;
    memset(&msg, 0, sizeof msg);
    msg.name = (uint8_t *)&rsa;
    msg.namelen = SYSCALL_SIZEOF_SOCKADDR_ANY;
    SyscallIovec iov;
    uint8_t dummy = 0;
    Int n = 0;
    SyscallSockaddr sa = {NULL, NULL};
    Error e = sock_msg(fd, p, oob, &msg, &iov, &dummy);
    if (BURROW_OK(e))
        n = burrow__syscall_recvmsg(fd, &msg, flags, &e);
    BURROW_OUT(oobn, BURROW_OK(e) ? (Int)msg.controllen : 0);
    BURROW_OUT(recvflags, BURROW_OK(e) ? (Int)msg.flags : 0);
    /* The sender is only there when the socket is not connected. */
    if (BURROW_OK(e) && rsa.addr.family != SYSCALL_AF_UNSPEC)
        e = burrow__syscall_any_to_sockaddr(a, &rsa, &sa);
    BURROW_OUT(from, sa);
    BURROW_OUT(err, e);
    return n;
}

Error syscall_sendto(Int fd, Slice p, Int flags, SyscallSockaddr to) {
    Error err = BURROW_NO_ERROR;
    uint32_t n = 0;
    void *ptr = sock_raw(to, &n, &err);
    if (BURROW_FAILED(err))
        return err;
    return burrow__syscall_sendto(fd, p, flags, ptr, n);
}

Int syscall_sendmsg_n(Int fd, Slice p, Slice oob, SyscallSockaddr to, Int flags,
                      Error *err) {
    Error e = BURROW_NO_ERROR;
    uint32_t salen = 0;
    void *ptr = sock_raw(to, &salen, &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return 0;
    }
    SyscallMsghdr msg;
    memset(&msg, 0, sizeof msg);
    msg.name = (uint8_t *)ptr;
    msg.namelen = salen;
    SyscallIovec iov;
    uint8_t dummy = 0;
    e = sock_msg(fd, p, oob, &msg, &iov, &dummy);
    Int n = 0;
    if (BURROW_OK(e))
        n = burrow__syscall_sendmsg(fd, &msg, flags, &e);
    if (BURROW_FAILED(e) || (oob.len > 0 && p.len == 0))
        n = 0;
    BURROW_OUT(err, e);
    return n;
}

Error syscall_sendmsg(Int fd, Slice p, Slice oob, SyscallSockaddr to, Int flags) {
    Error err = BURROW_NO_ERROR;
    (void)syscall_sendmsg_n(fd, p, oob, to, flags, &err);
    return err;
}

/* ----------------------------------------------------------- getsockopt */

/* getsockopt(2) into the n bytes at v. */
static Error sock_getsockopt(Int fd, Int level, Int opt, void *v, uint32_t n) {
    uint32_t vallen = n;
    return burrow__syscall_getsockopt(fd, level, opt, v, &vallen);
}

Int syscall_getsockopt_int(Int fd, Int level, Int opt, Error *err) {
    int32_t n = 0;
    BURROW_OUT(err, sock_getsockopt(fd, level, opt, &n, 4));
    return n;
}

SyscallGetsockoptInet4AddrRet syscall_getsockopt_inet4_addr(Int fd, Int level, Int opt,
                                                            Error *err) {
    SyscallGetsockoptInet4AddrRet value;
    memset(&value, 0, sizeof value);
    BURROW_OUT(err, sock_getsockopt(fd, level, opt, &value, (uint32_t)sizeof value));
    return value;
}

SyscallIPMreq syscall_getsockopt_ip_mreq(Int fd, Int level, Int opt, Error *err) {
    SyscallIPMreq value;
    memset(&value, 0, sizeof value);
    BURROW_OUT(err, sock_getsockopt(fd, level, opt, &value, (uint32_t)sizeof value));
    return value;
}

SyscallIPv6Mreq syscall_getsockopt_ipv6_mreq(Int fd, Int level, Int opt, Error *err) {
    SyscallIPv6Mreq value;
    memset(&value, 0, sizeof value);
    BURROW_OUT(err, sock_getsockopt(fd, level, opt, &value, (uint32_t)sizeof value));
    return value;
}

SyscallIPv6MTUInfo syscall_getsockopt_ipv6_mtu_info(Int fd, Int level, Int opt,
                                                    Error *err) {
    SyscallIPv6MTUInfo value;
    memset(&value, 0, sizeof value);
    BURROW_OUT(err, sock_getsockopt(fd, level, opt, &value, (uint32_t)sizeof value));
    return value;
}

SyscallICMPv6Filter syscall_getsockopt_icmpv6_filter(Int fd, Int level, Int opt,
                                                     Error *err) {
    SyscallICMPv6Filter value;
    memset(&value, 0, sizeof value);
    BURROW_OUT(err, sock_getsockopt(fd, level, opt, &value, (uint32_t)sizeof value));
    return value;
}

#if defined(SOCK_BSD)
uint8_t syscall_getsockopt_byte(Int fd, Int level, Int opt, Error *err) {
    uint8_t n = 0;
    BURROW_OUT(err, sock_getsockopt(fd, level, opt, &n, 1));
    return n;
}
#endif

#if defined(SOCK_LINUX) || defined(BURROW_OS_FREEBSD)
SyscallIPMreqn syscall_getsockopt_ip_mreqn(Int fd, Int level, Int opt, Error *err) {
    SyscallIPMreqn value;
    memset(&value, 0, sizeof value);
    BURROW_OUT(err, sock_getsockopt(fd, level, opt, &value, (uint32_t)sizeof value));
    return value;
}

#endif

#if defined(SOCK_LINUX)
SyscallUcred syscall_getsockopt_ucred(Int fd, Int level, Int opt, Error *err) {
    SyscallUcred value;
    memset(&value, 0, sizeof value);
    BURROW_OUT(err, sock_getsockopt(fd, level, opt, &value, (uint32_t)sizeof value));
    return value;
}

#endif

/* ----------------------------------------------------------- setsockopt */

Error syscall_setsockopt_byte(Int fd, Int level, Int opt, uint8_t value) {
    return burrow__syscall_setsockopt(fd, level, opt, &value, 1);
}

Error syscall_setsockopt_int(Int fd, Int level, Int opt, Int value) {
    int32_t n = (int32_t)value;
    return burrow__syscall_setsockopt(fd, level, opt, &n, 4);
}

Error syscall_setsockopt_inet4_addr(Int fd, Int level, Int opt,
                                    const uint8_t value[4]) {
    uint8_t v[4];
    memcpy(v, value, 4);
    return burrow__syscall_setsockopt(fd, level, opt, v, 4);
}

Error syscall_setsockopt_ip_mreq(Int fd, Int level, Int opt, SyscallIPMreq *mreq) {
    return burrow__syscall_setsockopt(fd, level, opt, mreq, sizeof *mreq);
}

Error syscall_setsockopt_ipv6_mreq(Int fd, Int level, Int opt, SyscallIPv6Mreq *mreq) {
    return burrow__syscall_setsockopt(fd, level, opt, mreq, sizeof *mreq);
}

Error syscall_setsockopt_icmpv6_filter(Int fd, Int level, Int opt,
                                       SyscallICMPv6Filter *filter) {
    return burrow__syscall_setsockopt(fd, level, opt, filter, sizeof *filter);
}

Error syscall_setsockopt_linger(Int fd, Int level, Int opt, SyscallLinger *l) {
    return burrow__syscall_setsockopt(fd, level, opt, l, sizeof *l);
}

Error syscall_setsockopt_string(Int fd, Int level, Int opt, Str s) {
    return burrow__syscall_setsockopt(fd, level, opt, (void *)(uintptr_t)s.p,
                                      (Uintptr)s.len);
}

Error syscall_setsockopt_timeval(Int fd, Int level, Int opt, SyscallTimeval *tv) {
    return burrow__syscall_setsockopt(fd, level, opt, tv, sizeof *tv);
}

#if defined(SOCK_LINUX) || defined(BURROW_OS_FREEBSD)
Error syscall_setsockopt_ip_mreqn(Int fd, Int level, Int opt, SyscallIPMreqn *mreq) {
    return burrow__syscall_setsockopt(fd, level, opt, mreq, sizeof *mreq);
}
#endif

#if defined(SOCK_LINUX)
Error syscall_bind_to_device(Int fd, Str device) {
    return syscall_setsockopt_string(fd, SYSCALL_SOL_SOCKET, SYSCALL_SO_BINDTODEVICE,
                                     device);
}
#endif

#endif

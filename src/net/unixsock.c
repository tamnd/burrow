/* UnixAddr, UnixConn and UnixListener.
 *
 * Derived from Go's src/net/unixsock.go and unixsock_posix.go, and the
 * SockaddrUnix parts of syscall_linux.go, syscall_bsd.go, syscall_windows.go
 * and syscall_solaris.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "internal.h"

#include "burrow/declare.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/net.h"
#include "burrow/sync/atomic.h"
#include "burrow/type.h"

#include "../os/internal.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define NX_LIT(s) str_from_bytes((const Byte *)(s), (Int)sizeof(s) - 1)
#define NX_COUNT(arr) (uint16_t)(sizeof(arr) / sizeof((arr)[0]))
#define NX_SIG_STRING(IN, OUT) OUT(Str)

/* The most a name can hold, which is the size of PalSockAddr's path. */
#define NX_NAME_MAX 108

_Static_assert(sizeof(((PalSockAddr *)0)->path) == NX_NAME_MAX,
               "NX_NAME_MAX is not the size of PalSockAddr's path");

#if defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS) ||                             \
    defined(BURROW_OS_FREEBSD) || defined(BURROW_OS_NETBSD) ||                         \
    defined(BURROW_OS_OPENBSD) || defined(BURROW_OS_DRAGONFLY)
#define NX_BSD 1
/* sizeof sun_path on the BSDs. */
#define NX_BSD_PATH 104
#elif defined(BURROW_OS_LINUX) || defined(BURROW_OS_ANDROID) ||                        \
    defined(BURROW_OS_WINDOWS) || defined(BURROW_OS_COSMO)
#define NX_ABSTRACT 1
#endif

static const NetAddr nx_nil_addr = {NULL, NULL};

static bool nx_str_eq(Str a, Str b) {
    return a.len == b.len && (a.len == 0 || memcmp(a.p, b.p, (size_t)a.len) == 0);
}

/* sotypeToNet. */
static Str nx_sotype_net(int32_t sotype) {
    if (sotype == PAL_SOCK_DGRAM)
        return NX_LIT("unixgram");
    if (sotype == PAL_SOCK_SEQPACKET)
        return NX_LIT("unixpacket");
    return NX_LIT("unix");
}

/* The network as a literal, which outlives the caller's Str, and the type of
 * socket it is. */
static bool nx_network(Str network, Str *lit, int32_t *sotype) {
    static const int32_t types[] = {PAL_SOCK_STREAM, PAL_SOCK_DGRAM,
                                    PAL_SOCK_SEQPACKET};
    for (size_t i = 0; i < sizeof types / sizeof types[0]; i++) {
        Str n = nx_sotype_net(types[i]);
        if (nx_str_eq(network, n)) {
            *lit = n;
            *sotype = types[i];
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------- sockaddrs */

/* SockaddrUnix.sockaddr: the name as the system takes it. A name the system
 * cannot hold is burrow__net_err_sockaddr_einval, which the call that wanted
 * it reports as its EINVAL. */
static Error nx_sockaddr(Str name, PalSockAddr *out) {
    *out = (PalSockAddr){0};
    out->family = PAL_AF_UNIX;
    Int n = name.len;
#if defined(NX_BSD)
    if (n == 0 || n >= NX_BSD_PATH)
        return burrow__net_err_sockaddr_einval;
    memcpy(out->path, name.p, (size_t)n);
    out->path_len = (uint16_t)(n + 1);
#elif defined(NX_ABSTRACT)
    /* A name that starts with "@" or a NUL is abstract, and goes to the
     * system with a NUL first and none after. Anything else has one after,
     * except the empty name, which binds to a fresh abstract one. */
    if (n > NX_NAME_MAX)
        return burrow__net_err_sockaddr_einval;
    bool abstract = n > 0 && (name.p[0] == '@' || name.p[0] == 0);
    if (n == NX_NAME_MAX && !abstract)
        return burrow__net_err_sockaddr_einval;
    if (n > 0)
        memcpy(out->path, name.p, (size_t)n);
    if (abstract)
        out->path[0] = 0;
    out->path_len = (uint16_t)(abstract || n == 0 ? n : n + 1);
#else
    if (n >= NX_NAME_MAX)
        return burrow__net_err_sockaddr_einval;
    if (n > 0)
        memcpy(out->path, name.p, (size_t)n);
    Int sl = n > 0 ? n + 1 : 0;
#if defined(BURROW_OS_SOLARIS)
    if (out->path[0] == '@' || (out->path[0] == 0 && sl > 1)) {
        out->path[0] = 0;
        sl--;
    }
#endif
    out->path_len = (uint16_t)sl;
#endif
    return BURROW_NO_ERROR;
}

static Error nx_to_sockaddr(const void *addr, int32_t family, PalSockAddr *out) {
    (void)family;
    return nx_sockaddr(((const NetUnixAddr *)addr)->name, out);
}

/* anyToSockaddr's name for an AF_UNIX address, in buf, and its length. */
static Int nx_name(const PalSockAddr *sa, Byte buf[NX_NAME_MAX]) {
    memcpy(buf, sa->path, NX_NAME_MAX);
#if defined(NX_BSD)
    /* As long as the system said, up to a NUL if there is one in it. */
    Int max = sa->path_len < NX_NAME_MAX ? (Int)sa->path_len : NX_NAME_MAX;
#else
#if defined(NX_ABSTRACT)
    /* An abstract name starts with a NUL, which Go shows as "@", and so does
     * the name of a socket that has none. */
    if (buf[0] == 0)
        buf[0] = '@';
#endif
    Int max = NX_NAME_MAX;
#endif
    Int n = 0;
    while (n < max && buf[n] != 0)
        n++;
    return n;
}

/* ---------------------------------------------------------------- UnixAddr */

Str net_unix_addr_network(const NetUnixAddr *a) {
    if (a == NULL)
        return BURROW_STR_EMPTY;
    return a->net;
}

Str net_unix_addr_string(const NetUnixAddr *a, Alloc *al) {
    Str s = a == NULL ? NX_LIT("<nil>") : a->name;
    if (s.len == 0)
        return BURROW_STR_EMPTY;
    Byte *p = (Byte *)mem_alloc_nozero(al, (size_t)s.len, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    memcpy(p, s.p, (size_t)s.len);
    return str_from_bytes(p, s.len);
}

static Str nx_addr_m_network(NetUnixAddr *self) {
    return net_unix_addr_network(self);
}

static Str nx_addr_m_string(NetUnixAddr *self) {
    return net_unix_addr_string(self, error_allocator());
}

#define NX_ADDR_METHODS(M, T)                                                          \
    M(T, Network, nx_addr_m_network, NX_SIG_STRING)                                    \
    M(T, String, nx_addr_m_string, NX_SIG_STRING)

BURROW_METHODS_DEFINE(NetUnixAddr, NX_ADDR_METHODS);

static const Field nx_addr_fields[] = {
    {BURROW_S_INIT("Name"),
     {NULL, 0},
     TYPE_STRING,
     (uint32_t)offsetof(NetUnixAddr, name)},
    {BURROW_S_INIT("Net"),
     {NULL, 0},
     TYPE_STRING,
     (uint32_t)offsetof(NetUnixAddr, net)},
};

static const Type nx_addr_desc = {
    BURROW_S_INIT("UnixAddr"),
    BURROW_S_INIT("net"),
    KIND_STRUCT,
    (uint32_t)sizeof(NetUnixAddr),
    (uint16_t)_Alignof(NetUnixAddr),
    NX_COUNT(nx_addr_fields),
    NX_COUNT(burrow__methods_NetUnixAddr),
    nx_addr_fields,
    burrow__methods_NetUnixAddr,
    NULL,
    NULL,
    0,
    0x6e787561U, /* "nxua" */
    NULL,
};

const Type *const TYPE_NET_UNIX_ADDR = &nx_addr_desc;

static Str nx_addr_network(void *self) {
    return net_unix_addr_network((const NetUnixAddr *)self);
}

static Str nx_addr_string(void *self, Alloc *a) {
    return net_unix_addr_string((const NetUnixAddr *)self, a);
}

static const NetAddrVT nx_addr_vt = {&nx_addr_desc, nx_addr_network, nx_addr_string};

NetAddr net_unix_addr_as_addr(const NetUnixAddr *a) {
    NetAddr addr = {NULL, NULL};
    if (a != NULL) {
        addr.vt = &nx_addr_vt;
        addr.data = (void *)(uintptr_t)a;
    }
    return addr;
}

/* A NetUnixAddr this package made, with the bytes of its name and network
 * after it, and the size to give back. */
typedef struct NxAddrBox {
    NetUnixAddr a;
    size_t size;
} NxAddrBox;

static NetUnixAddr *nx_addr_new(Alloc *a, Str name, Str net) {
    size_t size = sizeof(NxAddrBox) + (size_t)name.len + (size_t)net.len;
    NxAddrBox *b = (NxAddrBox *)mem_alloc_nozero(a, size, _Alignof(NxAddrBox));
    if (b == NULL)
        return NULL;
    b->size = size;
    Byte *p = (Byte *)(b + 1);
    if (name.len > 0)
        memcpy(p, name.p, (size_t)name.len);
    b->a.name = str_from_bytes(p, name.len);
    if (net.len > 0)
        memcpy(p + name.len, net.p, (size_t)net.len);
    b->a.net = str_from_bytes(p + name.len, net.len);
    return &b->a;
}

NetUnixAddr *net_resolve_unix_addr(Alloc *a, Str network, Str address, Error *err) {
    Str net = BURROW_STR_EMPTY;
    int32_t sotype = 0;
    if (!nx_network(network, &net, &sotype)) {
        BURROW_OUT(err, net_unknown_network_error(error_allocator(), network));
        return NULL;
    }
    NetUnixAddr *addr = nx_addr_new(a, address, net);
    BURROW_OUT(err, addr == NULL ? burrow_err_out_of_memory : BURROW_NO_ERROR);
    return addr;
}

void net_unix_addr_free(Alloc *a, NetUnixAddr *addr) {
    if (addr == NULL)
        return;
    NxAddrBox *b = (NxAddrBox *)(void *)addr;
    mem_free(a, b, b->size, _Alignof(NxAddrBox));
}

/* A UnixAddr that owns its name, for each end of a connection. */
typedef struct NxAddr {
    NetUnixAddr a;
    Byte name[NX_NAME_MAX];
} NxAddr;

/* sockaddrToUnix and the other two: the address in sa, for a socket of
 * sotype, and false when sa is not a Unix address, which Go's nil is. */
static bool nx_from_sockaddr(NxAddr *out, const PalSockAddr *sa, int32_t sotype) {
    if (sa->family != PAL_AF_UNIX)
        return false;
    Int n = nx_name(sa, out->name);
    out->a.name = str_from_bytes(out->name, n);
    out->a.net = nx_sotype_net(sotype);
    return true;
}

/* -------------------------------------------------------------- the socket */

/* UnixAddr.String, for a Control function. */
static Str nx_text(const void *addr, int32_t family, Alloc *a) {
    (void)family;
    return str_clone(a, ((const NetUnixAddr *)addr)->name);
}

/* unixSocket. A dial treats an empty name as no address at all. */
static Error nx_socket(burrow__NetFD *fd, const burrow__NetSysOpts *o, Str net,
                       int32_t sotype, const NetUnixAddr *laddr,
                       const NetUnixAddr *raddr, bool dial) {
    if (dial) {
        if (laddr != NULL && laddr->name.len == 0)
            laddr = NULL;
        if (raddr != NULL && raddr->name.len == 0)
            raddr = NULL;
        if (raddr == NULL && (sotype != PAL_SOCK_DGRAM || laddr == NULL))
            return burrow__net_err_missing_address;
    }
    return burrow__netfd_socket(fd, o != NULL ? &o->ctl : NULL, net, PAL_AF_UNIX,
                                sotype, 0, false, laddr, raddr, false, nx_to_sockaddr,
                                nx_text);
}

static bool nx_is_oom(Error e) {
    return e.vt == burrow_err_out_of_memory.vt &&
           e.data == burrow_err_out_of_memory.data;
}

static bool nx_is_eof(Error e) {
    return e.vt == io_eof.vt && e.data == io_eof.data;
}

/* ------------------------------------------------------------------- conn */

struct NetUnixConn {
    burrow__NetConnCore c;
    NxAddr laddr;
    NxAddr raddr;
};

/* newUnixConn, with the addresses the socket ended up with. */
static void nx_new_conn(NetUnixConn *c) {
    if (nx_from_sockaddr(&c->laddr, &c->c.fd.laddr, c->c.fd.sotype))
        c->c.laddr = net_unix_addr_as_addr(&c->laddr.a);
    if (nx_from_sockaddr(&c->raddr, &c->c.fd.raddr, c->c.fd.sotype))
        c->c.raddr = net_unix_addr_as_addr(&c->raddr.a);
    c->c.raw = (burrow__NetRawConn){&c->c.fd, c->c.laddr, c->c.raddr, false};
}

/* A new conn for a dial or for ListenUnixgram, or the error as it is. */
static NetUnixConn *nx_sys_conn(Alloc *a, const burrow__NetSysOpts *o, bool dial,
                                Str net, int32_t sotype, const NetUnixAddr *laddr,
                                const NetUnixAddr *raddr, Error *err) {
    NetUnixConn *c =
        (NetUnixConn *)mem_alloc(a, sizeof(NetUnixConn), _Alignof(NetUnixConn));
    if (c == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    c->c.alloc = a;
    Error e = nx_socket(&c->c.fd, o, net, sotype, laddr, raddr, dial);
    if (BURROW_FAILED(e)) {
        mem_free(a, c, sizeof(NetUnixConn), _Alignof(NetUnixConn));
        BURROW_OUT(err, e);
        return NULL;
    }
    nx_new_conn(c);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return c;
}

/* The same with the error an OpError with the op "dial" or "listen". */
static NetUnixConn *nx_conn(Alloc *a, const burrow__NetSysOpts *o, bool dial,
                            Str network, Str net, int32_t sotype,
                            const NetUnixAddr *laddr, const NetUnixAddr *raddr,
                            Error *err) {
    Error e = BURROW_NO_ERROR;
    NetUnixConn *c = nx_sys_conn(a, o, dial, net, sotype, laddr, raddr, &e);
    if (c == NULL && !nx_is_oom(e))
        e = burrow__net_op_error(dial ? NX_LIT("dial") : NX_LIT("listen"), network,
                                 dial ? net_unix_addr_as_addr(laddr) : nx_nil_addr,
                                 net_unix_addr_as_addr(dial ? raddr : laddr), e);
    BURROW_OUT(err, e);
    return c;
}

NetUnixConn *burrow__net_sys_dial_unix(Alloc *a, const burrow__NetSysOpts *o,
                                       Str network, const NetUnixAddr *laddr,
                                       const NetUnixAddr *raddr, Error *err) {
    Str net = BURROW_STR_EMPTY;
    int32_t sotype = 0;
    if (!nx_network(network, &net, &sotype)) {
        BURROW_OUT(err, net_unknown_network_error(error_allocator(), network));
        return NULL;
    }
    return nx_sys_conn(a, o, true, net, sotype, laddr, raddr, err);
}

NetUnixConn *burrow__net_sys_listen_unixgram(Alloc *a, const burrow__NetSysOpts *o,
                                             Str network, const NetUnixAddr *laddr,
                                             Error *err) {
    Str net = BURROW_STR_EMPTY;
    int32_t sotype = 0;
    if (!nx_network(network, &net, &sotype)) {
        BURROW_OUT(err, net_unknown_network_error(error_allocator(), network));
        return NULL;
    }
    return nx_sys_conn(a, o, false, net, sotype, laddr, NULL, err);
}

NetUnixConn *burrow__net_dial_unix(Alloc *a, const burrow__NetSysOpts *o, Str network,
                                   const NetUnixAddr *laddr, const NetUnixAddr *raddr,
                                   Error *err) {
    Str net = BURROW_STR_EMPTY;
    int32_t sotype = 0;
    if (!nx_network(network, &net, &sotype)) {
        Error u = net_unknown_network_error(error_allocator(), network);
        BURROW_OUT(err, burrow__net_op_error(NX_LIT("dial"), network,
                                             net_unix_addr_as_addr(laddr),
                                             net_unix_addr_as_addr(raddr), u));
        return NULL;
    }
    return nx_conn(a, o, true, network, net, sotype, laddr, raddr, err);
}

NetUnixConn *net_dial_unix(Alloc *a, Str network, const NetUnixAddr *laddr,
                           const NetUnixAddr *raddr, Error *err) {
    return burrow__net_dial_unix(a, NULL, network, laddr, raddr, err);
}

SyscallRawConn net_unix_conn_syscall_conn(NetUnixConn *c, Error *err) {
    if (c == NULL) {
        BURROW_OUT(err, burrow__net_einval());
        return (SyscallRawConn){NULL, NULL};
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return burrow__net_raw_conn(&c->c.raw);
}

NetUnixConn *net_listen_unixgram(Alloc *a, Str network, const NetUnixAddr *laddr,
                                 Error *err) {
    Str net = BURROW_STR_EMPTY;
    int32_t sotype = 0;
    if (!nx_network(network, &net, &sotype) || sotype != PAL_SOCK_DGRAM) {
        Error u = net_unknown_network_error(error_allocator(), network);
        BURROW_OUT(err, burrow__net_op_error(NX_LIT("listen"), network, nx_nil_addr,
                                             net_unix_addr_as_addr(laddr), u));
        return NULL;
    }
    if (laddr == NULL) {
        BURROW_OUT(err,
                   burrow__net_op_error(NX_LIT("listen"), network, nx_nil_addr,
                                        nx_nil_addr, burrow__net_err_missing_address));
        return NULL;
    }
    return nx_conn(a, NULL, false, network, net, sotype, laddr, NULL, err);
}

Int net_unix_conn_read(NetUnixConn *c, Slice p, Error *err) {
    if (c == NULL) {
        BURROW_OUT(err, burrow__net_einval());
        return 0;
    }
    return burrow__conn_read(&c->c, p, err);
}

Int net_unix_conn_write(NetUnixConn *c, Slice p, Error *err) {
    if (c == NULL) {
        BURROW_OUT(err, burrow__net_einval());
        return 0;
    }
    return burrow__conn_write(&c->c, p, err);
}

/* readFrom, with the sender in got when it has a name, and the error wrapped
 * the way ReadFromUnix does. */
static Int nx_read_from(NetUnixConn *c, Slice p, NxAddr *got, bool *named, Error *err) {
    PalSockAddr from = {0};
    Error e = BURROW_NO_ERROR;
    Int n = burrow__netfd_read_from(&c->c.fd, p, &from, &e);
    *named = nx_from_sockaddr(got, &from, c->c.fd.sotype) && got->a.name.len > 0;
    if (BURROW_FAILED(e) && !nx_is_eof(e))
        e = burrow__net_op_error(NX_LIT("read"), c->c.fd.net, c->c.laddr, c->c.raddr,
                                 e);
    *err = e;
    return n;
}

Int net_unix_conn_read_from_unix(NetUnixConn *c, Slice p, Alloc *a, NetUnixAddr **addr,
                                 Error *err) {
    if (addr != NULL)
        *addr = NULL;
    if (c == NULL) {
        BURROW_OUT(err, burrow__net_einval());
        return 0;
    }
    NxAddr got = {0};
    bool named = false;
    Error e = BURROW_NO_ERROR;
    Int n = nx_read_from(c, p, &got, &named, &e);
    if (named && addr != NULL) {
        *addr = nx_addr_new(a, got.a.name, got.a.net);
        if (*addr == NULL && BURROW_OK(e))
            e = burrow_err_out_of_memory;
    }
    BURROW_OUT(err, e);
    return n;
}

Int net_unix_conn_read_from(NetUnixConn *c, Slice p, Alloc *a, NetAddr *addr,
                            Error *err) {
    NetUnixAddr *u = NULL;
    Int n = net_unix_conn_read_from_unix(c, p, a, addr != NULL ? &u : NULL, err);
    if (addr != NULL)
        *addr = net_unix_addr_as_addr(u);
    return n;
}

Int net_unix_conn_write_to_unix(NetUnixConn *c, Slice p, const NetUnixAddr *addr,
                                Error *err) {
    if (c == NULL) {
        BURROW_OUT(err, burrow__net_einval());
        return 0;
    }
    Error e = BURROW_NO_ERROR;
    Int n = 0;
    if (c->c.fd.is_connected) {
        e = net_err_write_to_connected;
    } else if (addr == NULL) {
        e = burrow__net_err_missing_address;
    } else if (!nx_str_eq(addr->net, nx_sotype_net(c->c.fd.sotype))) {
        e = burrow__os_errno(PAL_EAFNOSUPPORT);
    } else {
        PalSockAddr to = {0};
        e = nx_sockaddr(addr->name, &to);
        if (BURROW_FAILED(e))
            e = burrow__netfd_write_to_error(e);
        else
            n = burrow__netfd_write_to(&c->c.fd, p, &to, &e);
    }
    if (BURROW_FAILED(e))
        e = burrow__net_op_error(NX_LIT("write"), c->c.fd.net, c->c.laddr,
                                 net_unix_addr_as_addr(addr), e);
    BURROW_OUT(err, e);
    return n;
}

Int net_unix_conn_write_to(NetUnixConn *c, Slice p, NetAddr addr, Error *err) {
    if (c == NULL) {
        BURROW_OUT(err, burrow__net_einval());
        return 0;
    }
    if (addr.vt != &nx_addr_vt) {
        BURROW_OUT(err, burrow__net_op_error(NX_LIT("write"), c->c.fd.net, c->c.laddr,
                                             addr, burrow__net_einval()));
        return 0;
    }
    return net_unix_conn_write_to_unix(c, p, (const NetUnixAddr *)addr.data, err);
}

Int net_unix_conn_read_msg_unix(NetUnixConn *c, Slice p, Slice oob, Alloc *a, Int *oobn,
                                Int *flags, NetUnixAddr **addr, Error *err) {
    if (oobn != NULL)
        *oobn = 0;
    if (flags != NULL)
        *flags = 0;
    if (addr != NULL)
        *addr = NULL;
    if (c == NULL) {
        BURROW_OUT(err, burrow__net_einval());
        return 0;
    }
    PalSockAddr from = {0};
    Int on = 0;
    Int fl = 0;
    Error e = BURROW_NO_ERROR;
    Int n = burrow__netfd_read_msg(&c->c.fd, p, oob, &on, &fl, &from, &e);
    NxAddr got = {0};
    if (addr != NULL && nx_from_sockaddr(&got, &from, c->c.fd.sotype) &&
        got.a.name.len > 0) {
        *addr = nx_addr_new(a, got.a.name, got.a.net);
        if (*addr == NULL && BURROW_OK(e))
            e = burrow_err_out_of_memory;
    }
    if (BURROW_FAILED(e) && !nx_is_eof(e) &&
        !(e.vt == burrow_err_out_of_memory.vt &&
          e.data == burrow_err_out_of_memory.data))
        e = burrow__net_op_error(NX_LIT("read"), c->c.fd.net, c->c.laddr, c->c.raddr,
                                 e);
    if (oobn != NULL)
        *oobn = on;
    if (flags != NULL)
        *flags = fl;
    BURROW_OUT(err, e);
    return n;
}

Int net_unix_conn_write_msg_unix(NetUnixConn *c, Slice p, Slice oob,
                                 const NetUnixAddr *addr, Int *oobn, Error *err) {
    if (oobn != NULL)
        *oobn = 0;
    if (c == NULL) {
        BURROW_OUT(err, burrow__net_einval());
        return 0;
    }
    Error e = BURROW_NO_ERROR;
    Int n = 0;
    Int on = 0;
    if (c->c.fd.sotype == PAL_SOCK_DGRAM && c->c.fd.is_connected) {
        e = net_err_write_to_connected;
    } else if (addr == NULL) {
        n = burrow__netfd_write_msg(&c->c.fd, p, oob, NULL, &on, &e);
    } else if (!nx_str_eq(addr->net, nx_sotype_net(c->c.fd.sotype))) {
        e = burrow__os_errno(PAL_EAFNOSUPPORT);
    } else {
        PalSockAddr to = {0};
        e = nx_sockaddr(addr->name, &to);
        if (BURROW_FAILED(e))
            e = burrow__netfd_write_msg_error(e);
        else
            n = burrow__netfd_write_msg(&c->c.fd, p, oob, &to, &on, &e);
    }
    if (BURROW_FAILED(e))
        e = burrow__net_op_error(NX_LIT("write"), c->c.fd.net, c->c.laddr,
                                 net_unix_addr_as_addr(addr), e);
    if (oobn != NULL)
        *oobn = on;
    BURROW_OUT(err, e);
    return n;
}

Error net_unix_conn_close(NetUnixConn *c) {
    if (c == NULL)
        return burrow__net_einval();
    return burrow__conn_close(&c->c);
}

/* CloseRead and CloseWrite fail the way Close does. */
static Error nx_close_op(NetUnixConn *c, Error e) {
    if (BURROW_OK(e))
        return e;
    return burrow__net_op_error(NX_LIT("close"), c->c.fd.net, c->c.laddr, c->c.raddr,
                                e);
}

Error net_unix_conn_close_read(NetUnixConn *c) {
    if (c == NULL)
        return burrow__net_einval();
    return nx_close_op(c, burrow__netfd_shutdown(&c->c.fd, PAL_SHUT_RD));
}

Error net_unix_conn_close_write(NetUnixConn *c) {
    if (c == NULL)
        return burrow__net_einval();
    return nx_close_op(c, burrow__netfd_shutdown(&c->c.fd, PAL_SHUT_WR));
}

NetAddr net_unix_conn_local_addr(NetUnixConn *c) {
    if (c == NULL)
        return nx_nil_addr;
    return c->c.laddr;
}

NetAddr net_unix_conn_remote_addr(NetUnixConn *c) {
    if (c == NULL)
        return nx_nil_addr;
    return c->c.raddr;
}

static Error nx_deadline(NetUnixConn *c, Time t, uint32_t mode) {
    if (c == NULL)
        return burrow__net_einval();
    return burrow__conn_set_deadline(&c->c, t, mode);
}

Error net_unix_conn_set_deadline(NetUnixConn *c, Time t) {
    return nx_deadline(c, t, BURROW_POLL_READ | BURROW_POLL_WRITE);
}

Error net_unix_conn_set_read_deadline(NetUnixConn *c, Time t) {
    return nx_deadline(c, t, BURROW_POLL_READ);
}

Error net_unix_conn_set_write_deadline(NetUnixConn *c, Time t) {
    return nx_deadline(c, t, BURROW_POLL_WRITE);
}

Error net_unix_conn_set_read_buffer(NetUnixConn *c, Int bytes) {
    if (c == NULL)
        return burrow__net_einval();
    return burrow__conn_set_buffer(&c->c, PAL_SO_RCVBUF, bytes);
}

Error net_unix_conn_set_write_buffer(NetUnixConn *c, Int bytes) {
    if (c == NULL)
        return burrow__net_einval();
    return burrow__conn_set_buffer(&c->c, PAL_SO_SNDBUF, bytes);
}

/* The NetConn methods. */
static Int nx_m_read(void *self, Slice p, Error *err) {
    return net_unix_conn_read((NetUnixConn *)self, p, err);
}

static Int nx_m_write(void *self, Slice p, Error *err) {
    return net_unix_conn_write((NetUnixConn *)self, p, err);
}

static Error nx_m_close(void *self) {
    return net_unix_conn_close((NetUnixConn *)self);
}

static NetAddr nx_m_local_addr(void *self) {
    return net_unix_conn_local_addr((NetUnixConn *)self);
}

static NetAddr nx_m_remote_addr(void *self) {
    return net_unix_conn_remote_addr((NetUnixConn *)self);
}

static Error nx_m_set_deadline(void *self, Time t) {
    return net_unix_conn_set_deadline((NetUnixConn *)self, t);
}

static Error nx_m_set_read_deadline(void *self, Time t) {
    return net_unix_conn_set_read_deadline((NetUnixConn *)self, t);
}

static Error nx_m_set_write_deadline(void *self, Time t) {
    return net_unix_conn_set_write_deadline((NetUnixConn *)self, t);
}

static const Type nx_conn_desc = {
    BURROW_S_INIT("UnixConn"),
    BURROW_S_INIT("net"),
    KIND_STRUCT,
    (uint32_t)sizeof(NetUnixConn),
    (uint16_t)_Alignof(NetUnixConn),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x6e787563U, /* "nxuc" */
    NULL,
};

static const NetConnVT nx_conn_vt = {
    {&nx_conn_desc, nx_m_read},
    {&nx_conn_desc, nx_m_write},
    {&nx_conn_desc, nx_m_close},
    nx_m_local_addr,
    nx_m_remote_addr,
    nx_m_set_deadline,
    nx_m_set_read_deadline,
    nx_m_set_write_deadline,
};

const IoWriterVT *const burrow__nx_conn_writer = &nx_conn_vt.writer;

NetConn net_unix_conn_as_conn(NetUnixConn *c) {
    NetConn conn = {NULL, NULL};
    if (c != NULL) {
        conn.vt = &nx_conn_vt;
        conn.data = c;
    }
    return conn;
}

NetUnixConn *net_conn_as_unix_conn(NetConn c) {
    if (c.vt != &nx_conn_vt)
        return NULL;
    return (NetUnixConn *)c.data;
}

static Int nx_m_read_from(void *self, Slice p, Alloc *a, NetAddr *addr, Error *err) {
    return net_unix_conn_read_from((NetUnixConn *)self, p, a, addr, err);
}

static Int nx_m_write_to(void *self, Slice p, NetAddr addr, Error *err) {
    return net_unix_conn_write_to((NetUnixConn *)self, p, addr, err);
}

static const NetPacketConnVT nx_packet_conn_vt = {
    {&nx_conn_desc, nx_m_close},
    nx_m_read_from,
    nx_m_write_to,
    nx_m_local_addr,
    nx_m_set_deadline,
    nx_m_set_read_deadline,
    nx_m_set_write_deadline,
};

NetPacketConn net_unix_conn_as_packet_conn(NetUnixConn *c) {
    NetPacketConn conn = {NULL, NULL};
    if (c != NULL) {
        conn.vt = &nx_packet_conn_vt;
        conn.data = c;
    }
    return conn;
}

NetUnixConn *net_packet_conn_as_unix_conn(NetPacketConn c) {
    if (c.vt != &nx_packet_conn_vt)
        return NULL;
    return (NetUnixConn *)c.data;
}

void net_unix_conn_free(NetUnixConn *c) {
    if (c == NULL)
        return;
    (void)burrow__netfd_close(&c->c.fd);
    mem_free(c->c.alloc, c, sizeof(NetUnixConn), _Alignof(NetUnixConn));
}

/* ------------------------------------------------------------ the listener */

struct NetUnixListener {
    burrow__NetFD fd;
    Alloc *alloc;
    NxAddr laddr;
    burrow__NetRawConn raw;
    bool has_laddr;
    bool unlink;
    SyncAtomicBool unlinked;
};

static NetAddr nx_listener_laddr(const NetUnixListener *l) {
    return l->has_laddr ? net_unix_addr_as_addr(&l->laddr.a) : nx_nil_addr;
}

NetUnixListener *burrow__net_sys_listen_unix(Alloc *a, const burrow__NetSysOpts *o,
                                             Str network, const NetUnixAddr *laddr,
                                             Error *err) {
    Str net = BURROW_STR_EMPTY;
    int32_t sotype = 0;
    if (!nx_network(network, &net, &sotype)) {
        BURROW_OUT(err, net_unknown_network_error(error_allocator(), network));
        return NULL;
    }
    NetUnixListener *l = (NetUnixListener *)mem_alloc(a, sizeof(NetUnixListener),
                                                      _Alignof(NetUnixListener));
    if (l == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    l->alloc = a;
    Error e = nx_socket(&l->fd, o, net, sotype, laddr, NULL, false);
    if (BURROW_FAILED(e)) {
        mem_free(a, l, sizeof(NetUnixListener), _Alignof(NetUnixListener));
        BURROW_OUT(err, e);
        return NULL;
    }
    l->has_laddr = nx_from_sockaddr(&l->laddr, &l->fd.laddr, sotype);
    l->raw = (burrow__NetRawConn){&l->fd, nx_listener_laddr(l), nx_nil_addr, true};
    l->unlink = true;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return l;
}

NetUnixListener *net_listen_unix(Alloc *a, Str network, const NetUnixAddr *laddr,
                                 Error *err) {
    Str net = BURROW_STR_EMPTY;
    int32_t sotype = 0;
    NetAddr want = net_unix_addr_as_addr(laddr);
    if (!nx_network(network, &net, &sotype) || sotype == PAL_SOCK_DGRAM) {
        Error u = net_unknown_network_error(error_allocator(), network);
        BURROW_OUT(err, burrow__net_op_error(NX_LIT("listen"), network,
                                             (NetAddr){NULL, NULL}, want, u));
        return NULL;
    }
    if (laddr == NULL) {
        BURROW_OUT(err, burrow__net_op_error(NX_LIT("listen"), network,
                                             (NetAddr){NULL, NULL}, want,
                                             burrow__net_err_missing_address));
        return NULL;
    }
    Error e = BURROW_NO_ERROR;
    NetUnixListener *l = burrow__net_sys_listen_unix(a, NULL, network, laddr, &e);
    if (l == NULL && !nx_is_oom(e))
        e = burrow__net_op_error(NX_LIT("listen"), network, (NetAddr){NULL, NULL}, want,
                                 e);
    BURROW_OUT(err, e);
    return l;
}

SyscallRawConn net_unix_listener_syscall_conn(NetUnixListener *l, Error *err) {
    if (l == NULL) {
        BURROW_OUT(err, burrow__net_einval());
        return (SyscallRawConn){NULL, NULL};
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return burrow__net_raw_conn(&l->raw);
}

NetUnixConn *net_unix_listener_accept_unix(NetUnixListener *l, Error *err) {
    if (l == NULL) {
        BURROW_OUT(err, burrow__net_einval());
        return NULL;
    }
    NetUnixConn *c =
        (NetUnixConn *)mem_alloc(l->alloc, sizeof(NetUnixConn), _Alignof(NetUnixConn));
    if (c == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    c->c.alloc = l->alloc;
    Error e = burrow__netfd_accept(&l->fd, &c->c.fd);
    if (BURROW_FAILED(e)) {
        mem_free(l->alloc, c, sizeof(NetUnixConn), _Alignof(NetUnixConn));
        BURROW_OUT(err, burrow__net_op_error(NX_LIT("accept"), l->fd.net, nx_nil_addr,
                                             nx_listener_laddr(l), e));
        return NULL;
    }
    nx_new_conn(c);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return c;
}

/* UnixListener.close: the file goes first, once, unless it is abstract or
 * the caller said to keep it, and then the socket. */
static Error nx_listener_close(NetUnixListener *l) {
    if (!sync_atomic_bool_swap(&l->unlinked, true) && l->unlink && l->has_laddr) {
        Str path = l->laddr.a.name;
        if (path.len > 0 && path.p[0] != '@' &&
            memchr(path.p, 0, (size_t)path.len) == NULL) {
            char buf[NX_NAME_MAX + 1];
            memcpy(buf, path.p, (size_t)path.len);
            buf[path.len] = 0;
            PalErrno pe = PAL_OK;
            (void)pal_unlink(buf, &pe);
        }
    }
    return burrow__netfd_close(&l->fd);
}

Error net_unix_listener_close(NetUnixListener *l) {
    if (l == NULL)
        return burrow__net_einval();
    Error e = nx_listener_close(l);
    if (BURROW_FAILED(e))
        e = burrow__net_op_error(NX_LIT("close"), l->fd.net, nx_nil_addr,
                                 nx_listener_laddr(l), e);
    return e;
}

NetAddr net_unix_listener_addr(NetUnixListener *l) {
    if (l == NULL)
        return nx_nil_addr;
    return nx_listener_laddr(l);
}

Error net_unix_listener_set_deadline(NetUnixListener *l, Time t) {
    if (l == NULL)
        return burrow__net_einval();
    return burrow__pfd_set_deadline(&l->fd.pfd, t,
                                    BURROW_POLL_READ | BURROW_POLL_WRITE);
}

void net_unix_listener_set_unlink_on_close(NetUnixListener *l, bool unlink) {
    if (l != NULL)
        l->unlink = unlink;
}

static Error nx_l_close(void *self) {
    return net_unix_listener_close((NetUnixListener *)self);
}

NetConn net_unix_listener_accept(NetUnixListener *l, Error *err) {
    return net_unix_conn_as_conn(net_unix_listener_accept_unix(l, err));
}

static NetConn nx_l_accept(void *self, Error *err) {
    return net_unix_listener_accept((NetUnixListener *)self, err);
}

static NetAddr nx_l_addr(void *self) {
    return net_unix_listener_addr((NetUnixListener *)self);
}

static const Type nx_listener_desc = {
    BURROW_S_INIT("UnixListener"),
    BURROW_S_INIT("net"),
    KIND_STRUCT,
    (uint32_t)sizeof(NetUnixListener),
    (uint16_t)_Alignof(NetUnixListener),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x6e78756cU, /* "nxul" */
    NULL,
};

static const NetListenerVT nx_listener_vt = {
    {&nx_listener_desc, nx_l_close},
    nx_l_accept,
    nx_l_addr,
};

NetListener net_unix_listener_as_listener(NetUnixListener *l) {
    NetListener nl = {NULL, NULL};
    if (l != NULL) {
        nl.vt = &nx_listener_vt;
        nl.data = l;
    }
    return nl;
}

NetUnixListener *net_listener_as_unix_listener(NetListener l) {
    if (l.vt != &nx_listener_vt)
        return NULL;
    return (NetUnixListener *)l.data;
}

void net_unix_listener_free(NetUnixListener *l) {
    if (l == NULL)
        return;
    (void)nx_listener_close(l);
    mem_free(l->alloc, l, sizeof(NetUnixListener), _Alignof(NetUnixListener));
}

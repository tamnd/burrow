/* TCPAddr, TCPConn and TCPListener, and UnknownNetworkError.
 *
 * Derived from Go's src/net/tcpsock.go, tcpsock_posix.go, tcpsock_unix.go,
 * tcpsockopt_posix.go, tcpsockopt_unix.go, tcpsockopt_openbsd.go,
 * sockopt_posix.go, ipsock.go, ipsock_posix.go, net.go's conn, dial.go's
 * keep-alive defaults and the UnknownNetworkError and zone parts of net.go
 * and interface.go.
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
#include "burrow/os.h"
#include "burrow/platform.h"
#include "burrow/syscall.h"
#include "burrow/type.h"

#include "../os/internal.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define NT_LIT(s) str_from_bytes((const Byte *)(s), (Int)sizeof(s) - 1)
#define NT_COUNT(arr) (uint16_t)(sizeof(arr) / sizeof((arr)[0]))

/* defaultTCPKeepAliveIdle, defaultTCPKeepAliveInterval and
 * defaultTCPKeepAliveCount, from dial.go. */
#define NT_KEEPALIVE_IDLE (15 * TIME_SECOND)
#define NT_KEEPALIVE_INTERVAL (15 * TIME_SECOND)
#define NT_KEEPALIVE_COUNT 9

static Byte *nt_put(Byte *p, const void *s, Int n) {
    if (n > 0)
        memcpy(p, s, (size_t)n);
    return p + n;
}

static bool nt_str_eq(Str a, Str b) {
    return a.len == b.len && (a.len == 0 || memcmp(a.p, b.p, (size_t)a.len) == 0);
}

/* The decimal text of v, into buf, which has room for any int64. */
static Int nt_itoa(Byte buf[24], int64_t v) {
    Byte tmp[24];
    Int n = 0;
    uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    do {
        tmp[n++] = (Byte)('0' + (u % 10U));
        u /= 10U;
    } while (u != 0);
    Int out = 0;
    if (v < 0)
        buf[out++] = '-';
    while (n > 0)
        buf[out++] = tmp[--n];
    return out;
}

/* ----------------------------------------------------- UnknownNetworkError */

/* The name first, so that errors_as gives a Str *, and the text after it,
 * built once since the message slot cannot allocate. */
typedef struct NtUnknownBox {
    NetUnknownNetworkError name;
    Str message;
} NtUnknownBox;

static Str nt_unknown_message(const void *self) {
    return ((const NtUnknownBox *)self)->message;
}

static Str nt_unknown_m_error(NetUnknownNetworkError *self) {
    return ((const NtUnknownBox *)(const void *)self)->message;
}

static bool nt_unknown_m_false(NetUnknownNetworkError *self) {
    (void)self;
    return false;
}

#define NT_SIG_STRING(IN, OUT) OUT(Str)
#define NT_SIG_BOOL(IN, OUT) OUT(bool)

#define NT_UNKNOWN_METHODS(M, T)                                                       \
    M(T, Error, nt_unknown_m_error, NT_SIG_STRING)                                     \
    M(T, Temporary, nt_unknown_m_false, NT_SIG_BOOL)                                   \
    M(T, Timeout, nt_unknown_m_false, NT_SIG_BOOL)

BURROW_METHODS_DEFINE(NetUnknownNetworkError, NT_UNKNOWN_METHODS);

static const Type nt_unknown_desc = {
    BURROW_S_INIT("UnknownNetworkError"),
    BURROW_S_INIT("net"),
    KIND_STRING,
    (uint32_t)sizeof(Str),
    (uint16_t)_Alignof(Str),
    0,
    NT_COUNT(burrow__methods_NetUnknownNetworkError),
    NULL,
    burrow__methods_NetUnknownNetworkError,
    NULL,
    NULL,
    0,
    0x6e756e6bU, /* "nunk" */
    NULL,
};

const Type *const TYPE_NET_UNKNOWN_NETWORK_ERROR = &nt_unknown_desc;

static bool nt_unknown_is(const void *self, Error target);
static Error nt_unknown_clone(const void *self, Alloc *a);

static const ErrorVT nt_unknown_vt = {
    &nt_unknown_desc, nt_unknown_message, NULL, NULL, nt_unknown_is, NULL,
    nt_unknown_clone,
};

/* Go compares two of them with ==, which for a string is the text. */
static bool nt_unknown_is(const void *self, Error target) {
    if (target.vt != &nt_unknown_vt || target.data == NULL)
        return false;
    return nt_str_eq(((const NtUnknownBox *)self)->name,
                     ((const NtUnknownBox *)target.data)->name);
}

static const char nt_msg_unknown[] = "unknown network ";

Error net_unknown_network_error(Alloc *a, Str network) {
    Int plen = (Int)sizeof nt_msg_unknown - 1;
    size_t size =
        sizeof(NtUnknownBox) + (size_t)network.len + (size_t)plen + (size_t)network.len;
    NtUnknownBox *b = (NtUnknownBox *)mem_alloc_nozero(a, size, _Alignof(NtUnknownBox));
    if (b == NULL)
        return burrow_err_out_of_memory;
    Byte *p = (Byte *)(b + 1);
    b->name = str_from_bytes(p, network.len);
    p = nt_put(p, network.p, network.len);
    b->message = str_from_bytes(p, plen + network.len);
    p = nt_put(p, nt_msg_unknown, plen);
    nt_put(p, network.p, network.len);
    return (Error){&nt_unknown_vt, b};
}

static Error nt_unknown_clone(const void *self, Alloc *a) {
    return net_unknown_network_error(a, ((const NtUnknownBox *)self)->name);
}

/* errMissingAddress. */
BURROW_SENTINEL_ERROR(nt_err_missing_address, "missing address");

/* ----------------------------------------------------------------- TCPAddr */

Str net_tcp_addr_network(const NetTCPAddr *a) {
    (void)a;
    return NT_LIT("tcp");
}

static const char nt_hex[] = "0123456789abcdef";

Str net_tcp_addr_string(const NetTCPAddr *a, Alloc *al) {
    if (a == NULL) {
        Byte *p = (Byte *)mem_alloc_nozero(al, 5, 1);
        if (p == NULL)
            return BURROW_STR_EMPTY;
        nt_put(p, "<nil>", 5);
        return str_from_bytes(p, 5);
    }

    /* ipEmptyString: nothing for an empty ip, and IP.String otherwise, whose
     * text for a length other than 4 and 16 is "?" and the bytes in hex. */
    Byte ipbuf[64];
    Slice ipt = slice_from(ipbuf, 0, (Int)sizeof ipbuf, TYPE_BYTE);
    Int iplen = 0;
    bool hex = false;
    if (a->ip.len == 4 || a->ip.len == 16) {
        ipt = net_ip_append_text(a->ip, al, ipt, NULL);
        iplen = ipt.len;
    } else if (a->ip.len > 0) {
        hex = true;
        iplen = 1 + 2 * a->ip.len;
    }
    Byte portbuf[24];
    Int portlen = nt_itoa(portbuf, (int64_t)a->port);

    /* JoinHostPort brackets a host with a colon in it. */
    bool colon =
        (!hex && iplen > 0 && memchr(ipt.p, ':', (size_t)iplen) != NULL) ||
        (a->zone.len > 0 && memchr(a->zone.p, ':', (size_t)a->zone.len) != NULL);
    Int n = iplen + 1 + portlen;
    if (a->zone.len > 0)
        n += 1 + a->zone.len;
    if (colon)
        n += 2;
    Byte *out = (Byte *)mem_alloc_nozero(al, (size_t)n, 1);
    if (out == NULL)
        return BURROW_STR_EMPTY;
    Byte *p = out;
    if (colon)
        *p++ = '[';
    if (hex) {
        *p++ = '?';
        const Byte *b = (const Byte *)a->ip.p;
        for (Int i = 0; i < a->ip.len; i++) {
            *p++ = (Byte)nt_hex[b[i] >> 4];
            *p++ = (Byte)nt_hex[b[i] & 0x0f];
        }
    } else {
        p = nt_put(p, ipt.p, iplen);
    }
    if (a->zone.len > 0) {
        *p++ = '%';
        p = nt_put(p, a->zone.p, a->zone.len);
    }
    if (colon)
        *p++ = ']';
    *p++ = ':';
    nt_put(p, portbuf, portlen);
    return str_from_bytes(out, n);
}

static Str nt_addr_m_network(NetTCPAddr *self) {
    return net_tcp_addr_network(self);
}

static Str nt_addr_m_string(NetTCPAddr *self) {
    return net_tcp_addr_string(self, error_allocator());
}

#define NT_ADDR_METHODS(M, T)                                                          \
    M(T, Network, nt_addr_m_network, NT_SIG_STRING)                                    \
    M(T, String, nt_addr_m_string, NT_SIG_STRING)

BURROW_METHODS_DEFINE(NetTCPAddr, NT_ADDR_METHODS);

static const Field nt_addr_fields[] = {
    {BURROW_S_INIT("IP"),
     {NULL, 0},
     &burrow_type_NetIP,
     (uint32_t)offsetof(NetTCPAddr, ip)},
    {BURROW_S_INIT("Port"), {NULL, 0}, TYPE_INT, (uint32_t)offsetof(NetTCPAddr, port)},
    {BURROW_S_INIT("Zone"),
     {NULL, 0},
     TYPE_STRING,
     (uint32_t)offsetof(NetTCPAddr, zone)},
};

static const Type nt_addr_desc = {
    BURROW_S_INIT("TCPAddr"),
    BURROW_S_INIT("net"),
    KIND_STRUCT,
    (uint32_t)sizeof(NetTCPAddr),
    (uint16_t)_Alignof(NetTCPAddr),
    NT_COUNT(nt_addr_fields),
    NT_COUNT(burrow__methods_NetTCPAddr),
    nt_addr_fields,
    burrow__methods_NetTCPAddr,
    NULL,
    NULL,
    0,
    0x6e746361U, /* "ntca" */
    NULL,
};

const Type *const TYPE_NET_TCP_ADDR = &nt_addr_desc;

static Str nt_addr_network(void *self) {
    return net_tcp_addr_network((const NetTCPAddr *)self);
}

static Str nt_addr_string(void *self, Alloc *a) {
    return net_tcp_addr_string((const NetTCPAddr *)self, a);
}

static const NetAddrVT nt_addr_vt = {&nt_addr_desc, nt_addr_network, nt_addr_string};

NetAddr net_tcp_addr_as_addr(const NetTCPAddr *a) {
    NetAddr addr = {NULL, NULL};
    if (a != NULL) {
        addr.vt = &nt_addr_vt;
        addr.data = (void *)(uintptr_t)a;
    }
    return addr;
}

/* A TCPAddr that owns its bytes, which is what a connection keeps for each
 * end: the ip points into ip16 and the zone into zone. */
typedef struct NtAddr {
    NetTCPAddr a;
    Byte ip16[16];
    Byte zone[12];
} NtAddr;

/* sockaddrToTCP, with false for an address that is not IPv4 or IPv6, which
 * Go makes a nil Addr. The zone is the interface's index in decimal, which is
 * what Go's zone cache gives for an index it has no name for, and every index
 * until net.Interfaces is here to give the names. */
static bool nt_from_sockaddr(NtAddr *out, const PalSockAddr *sa) {
    *out = (NtAddr){0};
    if (sa->family == PAL_AF_INET) {
        memcpy(out->ip16, sa->addr, 4);
        out->a.ip = slice_from(out->ip16, 4, 4, TYPE_BYTE);
    } else if (sa->family == PAL_AF_INET6) {
        memcpy(out->ip16, sa->addr, 16);
        out->a.ip = slice_from(out->ip16, 16, 16, TYPE_BYTE);
        if (sa->scope_id != 0) {
            Byte buf[24];
            Int n = nt_itoa(buf, (int64_t)sa->scope_id);
            memcpy(out->zone, buf, (size_t)n);
            out->a.zone = str_from_bytes(out->zone, n);
        }
    } else {
        return false;
    }
    out->a.port = (Int)sa->port;
    return true;
}

/* ip as 16 bytes, an IPv4 address as IPv4-mapped, which is IP.To16. False
 * for a length that is neither. */
static bool nt_to16(NetIP ip, Byte out[16]) {
    if (ip.len == 16) {
        memcpy(out, ip.p, 16);
        return true;
    }
    if (ip.len == 4) {
        memset(out, 0, 10);
        out[10] = 0xff;
        out[11] = 0xff;
        memcpy(out + 12, ip.p, 4);
        return true;
    }
    return false;
}

static bool nt_is_v4mapped(const Byte b[16]) {
    static const Byte prefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    return memcmp(b, prefix, 12) == 0;
}

/* dtoi, which reads the digits at the start of s and stops at the first
 * thing that is not one, or at 0xFFFFFF. */
static int64_t nt_dtoi(Str s) {
    int64_t n = 0;
    for (Int i = 0; i < s.len; i++) {
        Byte c = s.p[i];
        if (c < '0' || c > '9')
            break;
        n = n * 10 + (c - '0');
        if (n >= 0xFFFFFF)
            return 0xFFFFFF;
    }
    return n;
}

/* zoneCache.index: a zone's interface index. Go looks the name up first and
 * reads it as a number when there is no such interface. The lookup arrives
 * with net.Interfaces, and until then every zone is read as a number. */
static uint32_t nt_zone_index(Str zone) {
    if (zone.len == 0)
        return 0;
    return (uint32_t)nt_dtoi(zone);
}

static Error nt_addr_error(const char *msg, NetIP ip) {
    NetAddrError e = {str_from_cstr(msg), net_ip_string(ip, error_allocator())};
    return net_addr_error_as_error(&e, error_allocator());
}

/* ipToSockaddr. */
static Error nt_ip_sockaddr(int32_t family, NetIP ip, Int port, Str zone,
                            PalSockAddr *out) {
    *out = (PalSockAddr){0};
    if (family == PAL_AF_INET) {
        Byte b[16] = {0};
        if (ip.len != 0) {
            if (!nt_to16(ip, b) || !nt_is_v4mapped(b))
                return nt_addr_error("non-IPv4 address", ip);
        }
        out->family = PAL_AF_INET;
        memcpy(out->addr, b + 12, 4);
    } else if (family == PAL_AF_INET6) {
        Byte b[16] = {0};
        if (ip.len != 0 && !nt_to16(ip, b))
            return nt_addr_error("non-IPv6 address", ip);
        /* IPv4zero means every address, and so does IPv6zero. */
        static const Byte v4zero[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
        if (memcmp(b, v4zero, 16) == 0)
            memset(b, 0, 16);
        out->family = PAL_AF_INET6;
        memcpy(out->addr, b, 16);
        out->scope_id = nt_zone_index(zone);
    } else {
        return nt_addr_error("invalid address family", ip);
    }
    if (port < 0 || port > 0xFFFF)
        return burrow__net_err_sockaddr_einval;
    out->port = (uint16_t)port;
    return BURROW_NO_ERROR;
}

static Error nt_tcp_sockaddr(const void *addr, int32_t family, PalSockAddr *out) {
    const NetTCPAddr *a = (const NetTCPAddr *)addr;
    return nt_ip_sockaddr(family, a->ip, a->port, a->zone, out);
}

/* TCPAddr.family. */
static int32_t nt_family(const NetTCPAddr *a) {
    if (a == NULL || a->ip.len <= 4)
        return PAL_AF_INET;
    Byte b[16];
    if (nt_to16(a->ip, b) && nt_is_v4mapped(b))
        return PAL_AF_INET;
    return PAL_AF_INET6;
}

/* TCPAddr.isWildcard. */
static bool nt_is_wildcard(const NetTCPAddr *a) {
    return a == NULL || a->ip.p == NULL || net_ip_is_unspecified(a->ip);
}

/* --------------------------------------------------------------- the dial */

/* The networks TCP knows, as literals the netFD can keep. */
static bool nt_network(Str network, Str *lit) {
    static const char *const names[] = {"tcp", "tcp4", "tcp6"};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        Str n = str_from_cstr(names[i]);
        if (nt_str_eq(network, n)) {
            *lit = n;
            return true;
        }
    }
    return false;
}

/* favoriteAddrFamily, for TCP. */
static int32_t nt_favorite_family(Str net, const NetTCPAddr *laddr,
                                  const NetTCPAddr *raddr, bool listen,
                                  bool *ipv6only) {
    *ipv6only = false;
    Byte last = net.p[net.len - 1];
    if (last == '4')
        return PAL_AF_INET;
    if (last == '6') {
        *ipv6only = true;
        return PAL_AF_INET6;
    }
    if (listen && nt_is_wildcard(laddr)) {
        if (burrow__net_supports_ipv4map() || !burrow__net_supports_ipv4())
            return PAL_AF_INET6;
        if (laddr == NULL)
            return PAL_AF_INET;
        return nt_family(laddr);
    }
    if ((laddr == NULL || nt_family(laddr) == PAL_AF_INET) &&
        (raddr == NULL || nt_family(raddr) == PAL_AF_INET))
        return PAL_AF_INET;
    return PAL_AF_INET6;
}

/* internetSocket, for a TCP stream. */
static Error nt_internet_socket(burrow__NetFD *fd, Str net, const NetTCPAddr *laddr,
                                const NetTCPAddr *raddr, bool listen) {
#if defined(BURROW_OS_FREEBSD) || defined(BURROW_OS_OPENBSD) ||                        \
    defined(BURROW_OS_WINDOWS)
    /* These systems will not connect to the unspecified address, which means
     * this machine everywhere else, so Go dials loopback instead. */
    static const Byte loop4[4] = {127, 0, 0, 1};
    static const Byte loop6[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    NetTCPAddr local;
    if (!listen && raddr != NULL && nt_is_wildcard(raddr)) {
        bool six = net.p[net.len - 1] == '6';
        local.ip = six ? slice_from((void *)(uintptr_t)loop6, 16, 16, TYPE_BYTE)
                       : slice_from((void *)(uintptr_t)loop4, 4, 4, TYPE_BYTE);
        local.port = raddr->port;
        local.zone = raddr->zone;
        raddr = &local;
    }
#endif
    bool ipv6only = false;
    int32_t family = nt_favorite_family(net, laddr, raddr, listen, &ipv6only);
    return burrow__netfd_socket(fd, net, family, PAL_SOCK_STREAM, 0, ipv6only, laddr,
                                raddr, nt_tcp_sockaddr, (Time){0});
}

/* selfConnect: a connection whose two ends are the same address and port,
 * which a dial to a port on this machine that nothing listens on can make
 * when the system picks that very port for the local end. A connection with
 * either address missing counts too, as in Go. */
static bool nt_self_connect(const burrow__NetFD *fd) {
    const PalSockAddr *l = &fd->laddr;
    const PalSockAddr *r = &fd->raddr;
    if (l->family == PAL_AF_UNSPEC || r->family == PAL_AF_UNSPEC)
        return true;
    if (l->port != r->port)
        return false;
    Byte lb[16];
    Byte rb[16];
    NetIP lip = slice_from((void *)(uintptr_t)l->addr,
                           l->family == PAL_AF_INET ? 4 : 16, 16, TYPE_BYTE);
    NetIP rip = slice_from((void *)(uintptr_t)r->addr,
                           r->family == PAL_AF_INET ? 4 : 16, 16, TYPE_BYTE);
    return nt_to16(lip, lb) && nt_to16(rip, rb) && memcmp(lb, rb, 16) == 0;
}

/* spuriousENOTAVAIL: an EADDRNOTAVAIL from a dial that let the system pick
 * the local port, which can happen when the ports run short, and is worth
 * one more try. */
static bool nt_spurious_enotavail(Error err) {
    if (err.vt != NULL && err.vt->self_type == TYPE_NET_OP_ERROR)
        err = ((const NetOpError *)err.data)->err;
    if (err.vt != NULL && err.vt->self_type == TYPE_OS_SYSCALL_ERROR)
        err = ((const OsSyscallError *)err.data)->err;
    return err.vt != NULL && err.vt->self_type == TYPE_SYSCALL_ERRNO &&
           *(const SyscallErrno *)err.data == SYSCALL_EADDRNOTAVAIL;
}

/* ------------------------------------------------------------- the conns */

struct NetTCPConn {
    burrow__NetFD fd;
    Alloc *alloc;
    NtAddr laddr;
    NtAddr raddr;
    bool has_laddr;
    bool has_raddr;
};

struct NetTCPListener {
    burrow__NetFD fd;
    Alloc *alloc;
    NtAddr laddr;
    bool has_laddr;
};

static NetAddr nt_conn_laddr(const NetTCPConn *c) {
    return c->has_laddr ? net_tcp_addr_as_addr(&c->laddr.a) : (NetAddr){NULL, NULL};
}

static NetAddr nt_conn_raddr(const NetTCPConn *c) {
    return c->has_raddr ? net_tcp_addr_as_addr(&c->raddr.a) : (NetAddr){NULL, NULL};
}

static NetAddr nt_listener_laddr(const NetTCPListener *l) {
    return l->has_laddr ? net_tcp_addr_as_addr(&l->laddr.a) : (NetAddr){NULL, NULL};
}

static Error nt_op_error(Str op, Str net, NetAddr source, NetAddr addr, Error err) {
    NetOpError oe = {op, net, source, addr, err};
    return net_op_error_as_error(&oe, error_allocator());
}

/* Go's !c.ok(), which answers EINVAL. */
static Error nt_einval(void) {
    return burrow__os_errno_value(SYSCALL_EINVAL);
}

/* roundDurationUp to whole seconds. */
static int64_t nt_seconds_up(Duration d) {
    return (int64_t)((d + TIME_SECOND - 1) / TIME_SECOND);
}

/* setKeepAliveIdle, setKeepAliveInterval and setKeepAliveCount, as one:
 * below zero leaves it alone and zero is the default. OpenBSD has none of
 * the three and Go answers ENOPROTOOPT there without asking. */
static Error nt_keepalive_opt(NetTCPConn *c, int32_t opt, int64_t v, int64_t dflt) {
    if (v < 0)
        return BURROW_NO_ERROR;
#if defined(BURROW_OS_OPENBSD)
    (void)c;
    (void)opt;
    (void)dflt;
    return burrow__os_errno_value(SYSCALL_ENOPROTOOPT);
#else
    if (v == 0)
        v = dflt;
    return burrow__netfd_setsockopt(&c->fd, opt, v);
#endif
}

static Error nt_set_op(NetTCPConn *c, Error err) {
    if (BURROW_OK(err))
        return err;
    return nt_op_error(NT_LIT("set"), c->fd.net, nt_conn_laddr(c), nt_conn_raddr(c),
                       err);
}

Error net_tcp_conn_set_keep_alive_config(NetTCPConn *c, NetKeepAliveConfig config) {
    if (c == NULL)
        return nt_einval();
    Error e = burrow__netfd_setsockopt(&c->fd, PAL_SO_KEEPALIVE, config.enable ? 1 : 0);
    if (BURROW_FAILED(e))
        return nt_set_op(c, e);
    e = nt_keepalive_opt(c, PAL_TCP_KEEPIDLE,
                         config.idle < 0 ? -1 : nt_seconds_up(config.idle),
                         nt_seconds_up(NT_KEEPALIVE_IDLE));
    if (BURROW_FAILED(e))
        return nt_set_op(c, e);
    e = nt_keepalive_opt(c, PAL_TCP_KEEPINTVL,
                         config.interval < 0 ? -1 : nt_seconds_up(config.interval),
                         nt_seconds_up(NT_KEEPALIVE_INTERVAL));
    if (BURROW_FAILED(e))
        return nt_set_op(c, e);
    e = nt_keepalive_opt(c, PAL_TCP_KEEPCNT, (int64_t)config.count, NT_KEEPALIVE_COUNT);
    return nt_set_op(c, e);
}

/* newTCPConn, with the zero Dialer or ListenConfig, whose KeepAlive of zero
 * turns keep-alives on with the defaults. What these fail with is dropped,
 * as in Go. */
static void nt_new_conn(NetTCPConn *c) {
    c->has_laddr = nt_from_sockaddr(&c->laddr, &c->fd.laddr);
    c->has_raddr = nt_from_sockaddr(&c->raddr, &c->fd.raddr);
    (void)burrow__netfd_setsockopt(&c->fd, PAL_TCP_NODELAY, 1);
    NetKeepAliveConfig cfg = {true, 0, 0, 0};
    (void)net_tcp_conn_set_keep_alive_config(c, cfg);
}

NetTCPConn *net_dial_tcp(Alloc *a, Str network, const NetTCPAddr *laddr,
                         const NetTCPAddr *raddr, Error *err) {
    Str net = BURROW_STR_EMPTY;
    NetAddr src = net_tcp_addr_as_addr(laddr);
    NetAddr dst = net_tcp_addr_as_addr(raddr);
    if (!nt_network(network, &net)) {
        Error u = net_unknown_network_error(error_allocator(), network);
        BURROW_OUT(err, nt_op_error(NT_LIT("dial"), network, src, dst, u));
        return NULL;
    }
    if (raddr == NULL) {
        BURROW_OUT(err, nt_op_error(NT_LIT("dial"), network, src, dst,
                                    nt_err_missing_address));
        return NULL;
    }
    NetTCPConn *c =
        (NetTCPConn *)mem_alloc(a, sizeof(NetTCPConn), _Alignof(NetTCPConn));
    if (c == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    c->alloc = a;
    Error e = BURROW_NO_ERROR;
    for (int i = 0;; i++) {
        e = nt_internet_socket(&c->fd, net, laddr, raddr, false);
        if (i >= 2 || (laddr != NULL && laddr->port != 0))
            break;
        if (BURROW_OK(e) ? !nt_self_connect(&c->fd) : !nt_spurious_enotavail(e))
            break;
        if (BURROW_OK(e))
            (void)burrow__netfd_close(&c->fd);
    }
    if (BURROW_FAILED(e)) {
        mem_free(a, c, sizeof(NetTCPConn), _Alignof(NetTCPConn));
        BURROW_OUT(err, nt_op_error(NT_LIT("dial"), network, src, dst, e));
        return NULL;
    }
    nt_new_conn(c);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return c;
}

Int net_tcp_conn_read(NetTCPConn *c, Slice p, Error *err) {
    if (c == NULL) {
        BURROW_OUT(err, nt_einval());
        return 0;
    }
    Error e = BURROW_NO_ERROR;
    Int n = burrow__netfd_read(&c->fd, p, &e);
    if (BURROW_FAILED(e) && !(e.vt == io_eof.vt && e.data == io_eof.data))
        e = nt_op_error(NT_LIT("read"), c->fd.net, nt_conn_laddr(c), nt_conn_raddr(c),
                        e);
    BURROW_OUT(err, e);
    return n;
}

Int net_tcp_conn_write(NetTCPConn *c, Slice p, Error *err) {
    if (c == NULL) {
        BURROW_OUT(err, nt_einval());
        return 0;
    }
    Error e = BURROW_NO_ERROR;
    Int n = burrow__netfd_write(&c->fd, p, &e);
    if (BURROW_FAILED(e))
        e = nt_op_error(NT_LIT("write"), c->fd.net, nt_conn_laddr(c), nt_conn_raddr(c),
                        e);
    BURROW_OUT(err, e);
    return n;
}

static Error nt_close_op(NetTCPConn *c, Error e) {
    if (BURROW_OK(e))
        return e;
    return nt_op_error(NT_LIT("close"), c->fd.net, nt_conn_laddr(c), nt_conn_raddr(c),
                       e);
}

Error net_tcp_conn_close(NetTCPConn *c) {
    if (c == NULL)
        return nt_einval();
    return nt_close_op(c, burrow__netfd_close(&c->fd));
}

Error net_tcp_conn_close_read(NetTCPConn *c) {
    if (c == NULL)
        return nt_einval();
    return nt_close_op(c, burrow__netfd_shutdown(&c->fd, PAL_SHUT_RD));
}

Error net_tcp_conn_close_write(NetTCPConn *c) {
    if (c == NULL)
        return nt_einval();
    return nt_close_op(c, burrow__netfd_shutdown(&c->fd, PAL_SHUT_WR));
}

NetAddr net_tcp_conn_local_addr(NetTCPConn *c) {
    if (c == NULL)
        return (NetAddr){NULL, NULL};
    return nt_conn_laddr(c);
}

NetAddr net_tcp_conn_remote_addr(NetTCPConn *c) {
    if (c == NULL)
        return (NetAddr){NULL, NULL};
    return nt_conn_raddr(c);
}

/* conn's setters other than TCP's own put the local address where the
 * remote one would be and no source, as Go's do. */
static Error nt_conn_set_op(NetTCPConn *c, Error e) {
    if (BURROW_OK(e))
        return e;
    return nt_op_error(NT_LIT("set"), c->fd.net, (NetAddr){NULL, NULL},
                       nt_conn_laddr(c), e);
}

static Error nt_conn_deadline(NetTCPConn *c, Time t, uint32_t mode) {
    if (c == NULL)
        return nt_einval();
    return nt_conn_set_op(c, burrow__pfd_set_deadline(&c->fd.pfd, t, mode));
}

Error net_tcp_conn_set_deadline(NetTCPConn *c, Time t) {
    return nt_conn_deadline(c, t, BURROW_POLL_READ | BURROW_POLL_WRITE);
}

Error net_tcp_conn_set_read_deadline(NetTCPConn *c, Time t) {
    return nt_conn_deadline(c, t, BURROW_POLL_READ);
}

Error net_tcp_conn_set_write_deadline(NetTCPConn *c, Time t) {
    return nt_conn_deadline(c, t, BURROW_POLL_WRITE);
}

Error net_tcp_conn_set_read_buffer(NetTCPConn *c, Int bytes) {
    if (c == NULL)
        return nt_einval();
    return nt_conn_set_op(c, burrow__netfd_setsockopt(&c->fd, PAL_SO_RCVBUF, bytes));
}

Error net_tcp_conn_set_write_buffer(NetTCPConn *c, Int bytes) {
    if (c == NULL)
        return nt_einval();
    return nt_conn_set_op(c, burrow__netfd_setsockopt(&c->fd, PAL_SO_SNDBUF, bytes));
}

Error net_tcp_conn_set_linger(NetTCPConn *c, Int sec) {
    if (c == NULL)
        return nt_einval();
    return nt_set_op(
        c, burrow__netfd_setsockopt(&c->fd, PAL_SO_LINGER, sec < 0 ? -1 : sec));
}

Error net_tcp_conn_set_no_delay(NetTCPConn *c, bool no_delay) {
    if (c == NULL)
        return nt_einval();
    return nt_set_op(
        c, burrow__netfd_setsockopt(&c->fd, PAL_TCP_NODELAY, no_delay ? 1 : 0));
}

Error net_tcp_conn_set_keep_alive(NetTCPConn *c, bool keepalive) {
    if (c == NULL)
        return nt_einval();
    return nt_set_op(
        c, burrow__netfd_setsockopt(&c->fd, PAL_SO_KEEPALIVE, keepalive ? 1 : 0));
}

Error net_tcp_conn_set_keep_alive_period(NetTCPConn *c, Duration d) {
    if (c == NULL)
        return nt_einval();
    return nt_set_op(c, nt_keepalive_opt(c, PAL_TCP_KEEPIDLE,
                                         d < 0 ? -1 : nt_seconds_up(d),
                                         nt_seconds_up(NT_KEEPALIVE_IDLE)));
}

/* The NetConn methods. */
static Int nt_m_read(void *self, Slice p, Error *err) {
    return net_tcp_conn_read((NetTCPConn *)self, p, err);
}

static Int nt_m_write(void *self, Slice p, Error *err) {
    return net_tcp_conn_write((NetTCPConn *)self, p, err);
}

static Error nt_m_close(void *self) {
    return net_tcp_conn_close((NetTCPConn *)self);
}

static NetAddr nt_m_local_addr(void *self) {
    return net_tcp_conn_local_addr((NetTCPConn *)self);
}

static NetAddr nt_m_remote_addr(void *self) {
    return net_tcp_conn_remote_addr((NetTCPConn *)self);
}

static Error nt_m_set_deadline(void *self, Time t) {
    return net_tcp_conn_set_deadline((NetTCPConn *)self, t);
}

static Error nt_m_set_read_deadline(void *self, Time t) {
    return net_tcp_conn_set_read_deadline((NetTCPConn *)self, t);
}

static Error nt_m_set_write_deadline(void *self, Time t) {
    return net_tcp_conn_set_write_deadline((NetTCPConn *)self, t);
}

static const Type nt_conn_desc = {
    BURROW_S_INIT("TCPConn"),
    BURROW_S_INIT("net"),
    KIND_STRUCT,
    (uint32_t)sizeof(NetTCPConn),
    (uint16_t)_Alignof(NetTCPConn),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x6e746363U, /* "ntcc" */
    NULL,
};

static const NetConnVT nt_conn_vt = {
    {&nt_conn_desc, nt_m_read},
    {&nt_conn_desc, nt_m_write},
    {&nt_conn_desc, nt_m_close},
    nt_m_local_addr,
    nt_m_remote_addr,
    nt_m_set_deadline,
    nt_m_set_read_deadline,
    nt_m_set_write_deadline,
};

NetConn net_tcp_conn_as_conn(NetTCPConn *c) {
    NetConn conn = {NULL, NULL};
    if (c != NULL) {
        conn.vt = &nt_conn_vt;
        conn.data = c;
    }
    return conn;
}

NetTCPConn *net_conn_as_tcp_conn(NetConn c) {
    if (c.vt != &nt_conn_vt)
        return NULL;
    return (NetTCPConn *)c.data;
}

void net_tcp_conn_free(NetTCPConn *c) {
    if (c == NULL)
        return;
    (void)burrow__netfd_close(&c->fd);
    mem_free(c->alloc, c, sizeof(NetTCPConn), _Alignof(NetTCPConn));
}

/* ------------------------------------------------------------ the listener */

NetTCPListener *net_listen_tcp(Alloc *a, Str network, const NetTCPAddr *laddr,
                               Error *err) {
    Str net = BURROW_STR_EMPTY;
    if (!nt_network(network, &net)) {
        Error u = net_unknown_network_error(error_allocator(), network);
        BURROW_OUT(err, nt_op_error(NT_LIT("listen"), network, (NetAddr){NULL, NULL},
                                    net_tcp_addr_as_addr(laddr), u));
        return NULL;
    }
    /* A NULL laddr is the zero TCPAddr, and is what the error shows. */
    NetTCPAddr zero = {0};
    if (laddr == NULL)
        laddr = &zero;
    NetAddr dst = net_tcp_addr_as_addr(laddr);
    NetTCPListener *l = (NetTCPListener *)mem_alloc(a, sizeof(NetTCPListener),
                                                    _Alignof(NetTCPListener));
    if (l == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    l->alloc = a;
    Error e = nt_internet_socket(&l->fd, net, laddr, NULL, true);
    if (BURROW_FAILED(e)) {
        mem_free(a, l, sizeof(NetTCPListener), _Alignof(NetTCPListener));
        BURROW_OUT(
            err, nt_op_error(NT_LIT("listen"), network, (NetAddr){NULL, NULL}, dst, e));
        return NULL;
    }
    l->has_laddr = nt_from_sockaddr(&l->laddr, &l->fd.laddr);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return l;
}

NetTCPConn *net_tcp_listener_accept_tcp(NetTCPListener *l, Error *err) {
    if (l == NULL) {
        BURROW_OUT(err, nt_einval());
        return NULL;
    }
    NetTCPConn *c =
        (NetTCPConn *)mem_alloc(l->alloc, sizeof(NetTCPConn), _Alignof(NetTCPConn));
    if (c == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    c->alloc = l->alloc;
    Error e = burrow__netfd_accept(&l->fd, &c->fd);
    if (BURROW_FAILED(e)) {
        mem_free(l->alloc, c, sizeof(NetTCPConn), _Alignof(NetTCPConn));
        BURROW_OUT(err, nt_op_error(NT_LIT("accept"), l->fd.net, (NetAddr){NULL, NULL},
                                    nt_listener_laddr(l), e));
        return NULL;
    }
    nt_new_conn(c);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return c;
}

Error net_tcp_listener_close(NetTCPListener *l) {
    if (l == NULL)
        return nt_einval();
    Error e = burrow__netfd_close(&l->fd);
    if (BURROW_FAILED(e))
        e = nt_op_error(NT_LIT("close"), l->fd.net, (NetAddr){NULL, NULL},
                        nt_listener_laddr(l), e);
    return e;
}

NetAddr net_tcp_listener_addr(NetTCPListener *l) {
    if (l == NULL)
        return (NetAddr){NULL, NULL};
    return nt_listener_laddr(l);
}

Error net_tcp_listener_set_deadline(NetTCPListener *l, Time t) {
    if (l == NULL)
        return nt_einval();
    return burrow__pfd_set_deadline(&l->fd.pfd, t,
                                    BURROW_POLL_READ | BURROW_POLL_WRITE);
}

static Error nt_l_close(void *self) {
    return net_tcp_listener_close((NetTCPListener *)self);
}

static NetConn nt_l_accept(void *self, Error *err) {
    return net_tcp_conn_as_conn(
        net_tcp_listener_accept_tcp((NetTCPListener *)self, err));
}

static NetAddr nt_l_addr(void *self) {
    return net_tcp_listener_addr((NetTCPListener *)self);
}

static const Type nt_listener_desc = {
    BURROW_S_INIT("TCPListener"),
    BURROW_S_INIT("net"),
    KIND_STRUCT,
    (uint32_t)sizeof(NetTCPListener),
    (uint16_t)_Alignof(NetTCPListener),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x6e74636cU, /* "ntcl" */
    NULL,
};

static const NetListenerVT nt_listener_vt = {
    {&nt_listener_desc, nt_l_close},
    nt_l_accept,
    nt_l_addr,
};

NetListener net_tcp_listener_as_listener(NetTCPListener *l) {
    NetListener nl = {NULL, NULL};
    if (l != NULL) {
        nl.vt = &nt_listener_vt;
        nl.data = l;
    }
    return nl;
}

void net_tcp_listener_free(NetTCPListener *l) {
    if (l == NULL)
        return;
    (void)burrow__netfd_close(&l->fd);
    mem_free(l->alloc, l, sizeof(NetTCPListener), _Alignof(NetTCPListener));
}

/* TCPAddr, TCPConn and TCPListener.
 *
 * Derived from Go's src/net/tcpsock.go, tcpsock_posix.go, tcpsock_unix.go,
 * tcpsockopt_posix.go, tcpsockopt_unix.go, tcpsockopt_openbsd.go,
 * sockopt_posix.go and dial.go's keep-alive defaults.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "internal.h"

#include "burrow/declare.h"
#include "burrow/error.h"
#include "burrow/mem.h"
#include "burrow/net.h"
#include "burrow/net/netip.h"
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

static bool nt_str_eq(Str a, Str b) {
    return a.len == b.len && (a.len == 0 || memcmp(a.p, b.p, (size_t)a.len) == 0);
}

#define NT_SIG_STRING(IN, OUT) OUT(Str)

/* ----------------------------------------------------------------- TCPAddr */

Str net_tcp_addr_network(const NetTCPAddr *a) {
    (void)a;
    return NT_LIT("tcp");
}

/* A NetTCPAddr is laid out as a burrow__NetInetAddr, which is what lets the
 * code TCP shares with UDP take one. */
_Static_assert(sizeof(NetTCPAddr) == sizeof(burrow__NetInetAddr) &&
                   offsetof(NetTCPAddr, ip) == offsetof(burrow__NetInetAddr, ip) &&
                   offsetof(NetTCPAddr, port) == offsetof(burrow__NetInetAddr, port) &&
                   offsetof(NetTCPAddr, zone) == offsetof(burrow__NetInetAddr, zone),
               "NetTCPAddr and burrow__NetInetAddr differ");

static const burrow__NetInetAddr *nt_inet(const NetTCPAddr *a) {
    return (const burrow__NetInetAddr *)(const void *)a;
}

Str net_tcp_addr_string(const NetTCPAddr *a, Alloc *al) {
    return burrow__net_inet_addr_string(nt_inet(a), al);
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
 * end. */
NetipAddrPort net_tcp_addr_addr_port(const NetTCPAddr *a) {
    return burrow__net_inet_addr_port(nt_inet(a));
}

NetTCPAddr *net_tcp_addr_from_addr_port(Alloc *a, NetipAddrPort addr) {
    return (NetTCPAddr *)(void *)burrow__net_inet_from_addr_port(a, addr);
}

void net_tcp_addr_free(Alloc *a, NetTCPAddr *addr) {
    burrow__net_inet_addr_free(a, (burrow__NetInetAddr *)(void *)addr);
}

typedef struct NtAddr {
    NetTCPAddr a;
    burrow__NetInetBytes b;
} NtAddr;

/* sockaddrToTCP, with false for an address that is not IPv4 or IPv6, which
 * Go makes a nil Addr. */
static bool nt_from_sockaddr(NtAddr *out, const PalSockAddr *sa) {
    return burrow__net_inet_from_sockaddr(sa, &out->a.ip, &out->a.port, &out->a.zone,
                                          &out->b);
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

/* internetSocket, for a TCP stream. */
static Error nt_internet_socket(burrow__NetFD *fd, const burrow__NetSysOpts *o, Str net,
                                const NetTCPAddr *laddr, const NetTCPAddr *raddr,
                                bool listen) {
    return burrow__net_internet_socket(fd, o != NULL ? &o->ctl : NULL, net,
                                       nt_inet(laddr), nt_inet(raddr), PAL_SOCK_STREAM,
                                       0, listen);
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
    return burrow__net_ip_to16(lip, lb) && burrow__net_ip_to16(rip, rb) &&
           memcmp(lb, rb, 16) == 0;
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
    burrow__NetConnCore c;
    NtAddr laddr;
    NtAddr raddr;
};

struct NetTCPListener {
    burrow__NetFD fd;
    Alloc *alloc;
    NtAddr laddr;
    /* The ListenConfig's keep-alive, which every accepted connection gets. */
    Duration keep_alive;
    NetKeepAliveConfig keep_alive_config;
    burrow__NetRawConn raw;
    bool has_laddr;
};

static NetAddr nt_listener_laddr(const NetTCPListener *l) {
    return l->has_laddr ? net_tcp_addr_as_addr(&l->laddr.a) : (NetAddr){NULL, NULL};
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
    return burrow__netfd_setsockopt(&c->c.fd, opt, v);
#endif
}

static Error nt_set_op(NetTCPConn *c, Error err) {
    if (BURROW_OK(err))
        return err;
    return burrow__net_op_error(NT_LIT("set"), c->c.fd.net, c->c.laddr, c->c.raddr,
                                err);
}

Error net_tcp_conn_set_keep_alive_config(NetTCPConn *c, NetKeepAliveConfig config) {
    if (c == NULL)
        return burrow__net_einval();
    Error e =
        burrow__netfd_setsockopt(&c->c.fd, PAL_SO_KEEPALIVE, config.enable ? 1 : 0);
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

/* newTCPConn: Nagle off, and keep-alives on unless the Dialer or the
 * ListenConfig says otherwise. A KeepAliveConfig that is not enabled gives
 * way to KeepAlive, whose zero is the defaults and which turns keep-alives
 * off when it is below zero. What these fail with is dropped, as in Go. */
static void nt_new_conn(NetTCPConn *c, Duration keep_alive, NetKeepAliveConfig cfg) {
    if (nt_from_sockaddr(&c->laddr, &c->c.fd.laddr))
        c->c.laddr = net_tcp_addr_as_addr(&c->laddr.a);
    if (nt_from_sockaddr(&c->raddr, &c->c.fd.raddr))
        c->c.raddr = net_tcp_addr_as_addr(&c->raddr.a);
    c->c.raw = (burrow__NetRawConn){&c->c.fd, c->c.laddr, c->c.raddr, false};
    (void)burrow__netfd_setsockopt(&c->c.fd, PAL_TCP_NODELAY, 1);
    if (!cfg.enable && keep_alive >= 0)
        cfg = (NetKeepAliveConfig){true, keep_alive, 0, 0};
    if (cfg.enable)
        (void)net_tcp_conn_set_keep_alive_config(c, cfg);
}

static void nt_lock(const burrow__NetSysOpts *o) {
    if (o != NULL && o->alloc_mu != NULL)
        sync_mutex_lock(o->alloc_mu);
}

static void nt_unlock(const burrow__NetSysOpts *o) {
    if (o != NULL && o->alloc_mu != NULL)
        sync_mutex_unlock(o->alloc_mu);
}

NetTCPConn *burrow__net_sys_dial_tcp(Alloc *a, const burrow__NetSysOpts *o, Str network,
                                     const NetTCPAddr *laddr, const NetTCPAddr *raddr,
                                     Error *err) {
    Str net = BURROW_STR_EMPTY;
    if (!nt_network(network, &net)) {
        BURROW_OUT(err, net_unknown_network_error(error_allocator(), network));
        return NULL;
    }
    nt_lock(o);
    NetTCPConn *c =
        (NetTCPConn *)mem_alloc(a, sizeof(NetTCPConn), _Alignof(NetTCPConn));
    nt_unlock(o);
    if (c == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    c->c.alloc = a;
    Error e = BURROW_NO_ERROR;
    for (int i = 0;; i++) {
        e = nt_internet_socket(&c->c.fd, o, net, laddr, raddr, false);
        if (i >= 2 || (laddr != NULL && laddr->port != 0))
            break;
        if (BURROW_OK(e) ? !nt_self_connect(&c->c.fd) : !nt_spurious_enotavail(e))
            break;
        if (BURROW_OK(e))
            (void)burrow__netfd_close(&c->c.fd);
    }
    if (BURROW_FAILED(e)) {
        nt_lock(o);
        mem_free(a, c, sizeof(NetTCPConn), _Alignof(NetTCPConn));
        nt_unlock(o);
        BURROW_OUT(err, e);
        return NULL;
    }
    if (o != NULL)
        nt_new_conn(c, o->keep_alive, o->keep_alive_config);
    else
        nt_new_conn(c, 0, (NetKeepAliveConfig){false, 0, 0, 0});
    BURROW_OUT(err, BURROW_NO_ERROR);
    return c;
}

static bool nt_is_oom(Error e) {
    return e.vt == burrow_err_out_of_memory.vt &&
           e.data == burrow_err_out_of_memory.data;
}

NetTCPConn *burrow__net_dial_tcp(Alloc *a, const burrow__NetSysOpts *o, Str network,
                                 const NetTCPAddr *laddr, const NetTCPAddr *raddr,
                                 Error *err) {
    NetAddr src = net_tcp_addr_as_addr(laddr);
    NetAddr dst = net_tcp_addr_as_addr(raddr);
    Str net = BURROW_STR_EMPTY;
    if (!nt_network(network, &net)) {
        Error u = net_unknown_network_error(error_allocator(), network);
        BURROW_OUT(err, burrow__net_op_error(NT_LIT("dial"), network, src, dst, u));
        return NULL;
    }
    if (raddr == NULL) {
        BURROW_OUT(err, burrow__net_op_error(NT_LIT("dial"), network, src, dst,
                                             burrow__net_err_missing_address));
        return NULL;
    }
    Error e = BURROW_NO_ERROR;
    NetTCPConn *c = burrow__net_sys_dial_tcp(a, o, network, laddr, raddr, &e);
    if (c == NULL && !nt_is_oom(e))
        e = burrow__net_op_error(NT_LIT("dial"), network, src, dst, e);
    BURROW_OUT(err, e);
    return c;
}

NetTCPConn *net_dial_tcp(Alloc *a, Str network, const NetTCPAddr *laddr,
                         const NetTCPAddr *raddr, Error *err) {
    return burrow__net_dial_tcp(a, NULL, network, laddr, raddr, err);
}

SyscallRawConn net_tcp_conn_syscall_conn(NetTCPConn *c, Error *err) {
    if (c == NULL) {
        BURROW_OUT(err, burrow__net_einval());
        return (SyscallRawConn){NULL, NULL};
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return burrow__net_raw_conn(&c->c.raw);
}

SyscallRawConn net_tcp_listener_syscall_conn(NetTCPListener *l, Error *err) {
    if (l == NULL) {
        BURROW_OUT(err, burrow__net_einval());
        return (SyscallRawConn){NULL, NULL};
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return burrow__net_raw_conn(&l->raw);
}

/* CloseRead and CloseWrite fail the way Close does. */
static Error nt_close_op(NetTCPConn *c, Error e) {
    if (BURROW_OK(e))
        return e;
    return burrow__net_op_error(NT_LIT("close"), c->c.fd.net, c->c.laddr, c->c.raddr,
                                e);
}

Error net_tcp_conn_close_read(NetTCPConn *c) {
    if (c == NULL)
        return burrow__net_einval();
    return nt_close_op(c, burrow__netfd_shutdown(&c->c.fd, PAL_SHUT_RD));
}

Error net_tcp_conn_close_write(NetTCPConn *c) {
    if (c == NULL)
        return burrow__net_einval();
    return nt_close_op(c, burrow__netfd_shutdown(&c->c.fd, PAL_SHUT_WR));
}

NetAddr net_tcp_conn_local_addr(NetTCPConn *c) {
    if (c == NULL)
        return (NetAddr){NULL, NULL};
    return c->c.laddr;
}

NetAddr net_tcp_conn_remote_addr(NetTCPConn *c) {
    if (c == NULL)
        return (NetAddr){NULL, NULL};
    return c->c.raddr;
}

static Error nt_conn_deadline(NetTCPConn *c, Time t, uint32_t mode) {
    if (c == NULL)
        return burrow__net_einval();
    return burrow__conn_set_deadline(&c->c, t, mode);
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
        return burrow__net_einval();
    return burrow__conn_set_buffer(&c->c, PAL_SO_RCVBUF, bytes);
}

Error net_tcp_conn_set_write_buffer(NetTCPConn *c, Int bytes) {
    if (c == NULL)
        return burrow__net_einval();
    return burrow__conn_set_buffer(&c->c, PAL_SO_SNDBUF, bytes);
}

Error net_tcp_conn_set_linger(NetTCPConn *c, Int sec) {
    if (c == NULL)
        return burrow__net_einval();
    return nt_set_op(
        c, burrow__netfd_setsockopt(&c->c.fd, PAL_SO_LINGER, sec < 0 ? -1 : sec));
}

Error net_tcp_conn_set_no_delay(NetTCPConn *c, bool no_delay) {
    if (c == NULL)
        return burrow__net_einval();
    return nt_set_op(
        c, burrow__netfd_setsockopt(&c->c.fd, PAL_TCP_NODELAY, no_delay ? 1 : 0));
}

Error net_tcp_conn_set_keep_alive(NetTCPConn *c, bool keepalive) {
    if (c == NULL)
        return burrow__net_einval();
    return nt_set_op(
        c, burrow__netfd_setsockopt(&c->c.fd, PAL_SO_KEEPALIVE, keepalive ? 1 : 0));
}

Error net_tcp_conn_set_keep_alive_period(NetTCPConn *c, Duration d) {
    if (c == NULL)
        return burrow__net_einval();
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
    (void)burrow__netfd_close(&c->c.fd);
    mem_free(c->c.alloc, c, sizeof(NetTCPConn), _Alignof(NetTCPConn));
}

/* ------------------------------------------------------------ the listener */

NetTCPListener *burrow__net_sys_listen_tcp(Alloc *a, const burrow__NetSysOpts *o,
                                           Str network, const NetTCPAddr *laddr,
                                           Error *err) {
    Str net = BURROW_STR_EMPTY;
    if (!nt_network(network, &net)) {
        BURROW_OUT(err, net_unknown_network_error(error_allocator(), network));
        return NULL;
    }
    NetTCPListener *l = (NetTCPListener *)mem_alloc(a, sizeof(NetTCPListener),
                                                    _Alignof(NetTCPListener));
    if (l == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    l->alloc = a;
    Error e = nt_internet_socket(&l->fd, o, net, laddr, NULL, true);
    if (BURROW_FAILED(e)) {
        mem_free(a, l, sizeof(NetTCPListener), _Alignof(NetTCPListener));
        BURROW_OUT(err, e);
        return NULL;
    }
    if (o != NULL) {
        l->keep_alive = o->keep_alive;
        l->keep_alive_config = o->keep_alive_config;
    }
    l->has_laddr = nt_from_sockaddr(&l->laddr, &l->fd.laddr);
    l->raw = (burrow__NetRawConn){&l->fd, nt_listener_laddr(l), {NULL, NULL}, true};
    BURROW_OUT(err, BURROW_NO_ERROR);
    return l;
}

NetTCPListener *net_listen_tcp(Alloc *a, Str network, const NetTCPAddr *laddr,
                               Error *err) {
    Str net = BURROW_STR_EMPTY;
    if (!nt_network(network, &net)) {
        Error u = net_unknown_network_error(error_allocator(), network);
        BURROW_OUT(err, burrow__net_op_error(NT_LIT("listen"), network,
                                             (NetAddr){NULL, NULL},
                                             net_tcp_addr_as_addr(laddr), u));
        return NULL;
    }
    /* A NULL laddr is the zero TCPAddr, and is what the error shows. */
    NetTCPAddr zero = {0};
    if (laddr == NULL)
        laddr = &zero;
    Error e = BURROW_NO_ERROR;
    NetTCPListener *l = burrow__net_sys_listen_tcp(a, NULL, network, laddr, &e);
    if (l == NULL && !nt_is_oom(e))
        e = burrow__net_op_error(NT_LIT("listen"), network, (NetAddr){NULL, NULL},
                                 net_tcp_addr_as_addr(laddr), e);
    BURROW_OUT(err, e);
    return l;
}

NetTCPConn *net_tcp_listener_accept_tcp(NetTCPListener *l, Error *err) {
    if (l == NULL) {
        BURROW_OUT(err, burrow__net_einval());
        return NULL;
    }
    NetTCPConn *c =
        (NetTCPConn *)mem_alloc(l->alloc, sizeof(NetTCPConn), _Alignof(NetTCPConn));
    if (c == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    c->c.alloc = l->alloc;
    Error e = burrow__netfd_accept(&l->fd, &c->c.fd);
    if (BURROW_FAILED(e)) {
        mem_free(l->alloc, c, sizeof(NetTCPConn), _Alignof(NetTCPConn));
        BURROW_OUT(err, burrow__net_op_error(NT_LIT("accept"), l->fd.net,
                                             (NetAddr){NULL, NULL},
                                             nt_listener_laddr(l), e));
        return NULL;
    }
    nt_new_conn(c, l->keep_alive, l->keep_alive_config);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return c;
}

Error net_tcp_listener_close(NetTCPListener *l) {
    if (l == NULL)
        return burrow__net_einval();
    Error e = burrow__netfd_close(&l->fd);
    if (BURROW_FAILED(e))
        e = burrow__net_op_error(NT_LIT("close"), l->fd.net, (NetAddr){NULL, NULL},
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
        return burrow__net_einval();
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

NetTCPListener *net_listener_as_tcp_listener(NetListener l) {
    if (l.vt != &nt_listener_vt)
        return NULL;
    return (NetTCPListener *)l.data;
}

void net_tcp_listener_free(NetTCPListener *l) {
    if (l == NULL)
        return;
    (void)burrow__netfd_close(&l->fd);
    mem_free(l->alloc, l, sizeof(NetTCPListener), _Alignof(NetTCPListener));
}

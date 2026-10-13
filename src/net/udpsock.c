/* UDPAddr and UDPConn.
 *
 * Derived from Go's src/net/udpsock.go and udpsock_posix.go.
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
#include "burrow/mem/arena.h"
#include "burrow/net.h"
#include "burrow/net/netip.h"
#include "burrow/os.h"
#include "burrow/platform.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define NU_LIT(s) str_from_bytes((const Byte *)(s), (Int)sizeof(s) - 1)
#define NU_COUNT(arr) (uint16_t)(sizeof(arr) / sizeof((arr)[0]))
#define NU_SIG_STRING(IN, OUT) OUT(Str)

BURROW_SENTINEL_ERROR(net_err_write_to_connected,
                      "use of WriteTo with pre-connected connection");

static const NetAddr nu_nil_addr = {NULL, NULL};

/* ----------------------------------------------------------------- UDPAddr */

_Static_assert(sizeof(NetUDPAddr) == sizeof(burrow__NetInetAddr) &&
                   offsetof(NetUDPAddr, ip) == offsetof(burrow__NetInetAddr, ip) &&
                   offsetof(NetUDPAddr, port) == offsetof(burrow__NetInetAddr, port) &&
                   offsetof(NetUDPAddr, zone) == offsetof(burrow__NetInetAddr, zone),
               "NetUDPAddr and burrow__NetInetAddr differ");

static const burrow__NetInetAddr *nu_inet(const NetUDPAddr *a) {
    return (const burrow__NetInetAddr *)(const void *)a;
}

Str net_udp_addr_network(const NetUDPAddr *a) {
    (void)a;
    return NU_LIT("udp");
}

Str net_udp_addr_string(const NetUDPAddr *a, Alloc *al) {
    return burrow__net_inet_addr_string(nu_inet(a), al);
}

static Str nu_addr_m_network(NetUDPAddr *self) {
    return net_udp_addr_network(self);
}

static Str nu_addr_m_string(NetUDPAddr *self) {
    return net_udp_addr_string(self, error_allocator());
}

#define NU_ADDR_METHODS(M, T)                                                          \
    M(T, Network, nu_addr_m_network, NU_SIG_STRING)                                    \
    M(T, String, nu_addr_m_string, NU_SIG_STRING)

BURROW_METHODS_DEFINE(NetUDPAddr, NU_ADDR_METHODS);

static const Field nu_addr_fields[] = {
    {BURROW_S_INIT("IP"),
     {NULL, 0},
     &burrow_type_NetIP,
     (uint32_t)offsetof(NetUDPAddr, ip)},
    {BURROW_S_INIT("Port"), {NULL, 0}, TYPE_INT, (uint32_t)offsetof(NetUDPAddr, port)},
    {BURROW_S_INIT("Zone"),
     {NULL, 0},
     TYPE_STRING,
     (uint32_t)offsetof(NetUDPAddr, zone)},
};

static const Type nu_addr_desc = {
    BURROW_S_INIT("UDPAddr"),
    BURROW_S_INIT("net"),
    KIND_STRUCT,
    (uint32_t)sizeof(NetUDPAddr),
    (uint16_t)_Alignof(NetUDPAddr),
    NU_COUNT(nu_addr_fields),
    NU_COUNT(burrow__methods_NetUDPAddr),
    nu_addr_fields,
    burrow__methods_NetUDPAddr,
    NULL,
    NULL,
    0,
    0x6e756461U, /* "nuda" */
    NULL,
};

const Type *const TYPE_NET_UDP_ADDR = &nu_addr_desc;

static Str nu_addr_network(void *self) {
    return net_udp_addr_network((const NetUDPAddr *)self);
}

static Str nu_addr_string(void *self, Alloc *a) {
    return net_udp_addr_string((const NetUDPAddr *)self, a);
}

static const NetAddrVT nu_addr_vt = {&nu_addr_desc, nu_addr_network, nu_addr_string};

NetAddr net_udp_addr_as_addr(const NetUDPAddr *a) {
    NetAddr addr = {NULL, NULL};
    if (a != NULL) {
        addr.vt = &nu_addr_vt;
        addr.data = (void *)(uintptr_t)a;
    }
    return addr;
}

NetipAddrPort net_udp_addr_addr_port(const NetUDPAddr *a) {
    return burrow__net_inet_addr_port(nu_inet(a));
}

/* A NetUDPAddr in a for ip, port and zone, copying their bytes. */
static NetUDPAddr *nu_addr_new(Alloc *a, const Byte *ip, Int iplen, Int port,
                               Str zone) {
    return (NetUDPAddr *)(void *)burrow__net_inet_addr_new(a, ip, iplen, port, zone);
}

NetUDPAddr *net_udp_addr_from_addr_port(Alloc *a, NetipAddrPort addr) {
    return (NetUDPAddr *)(void *)burrow__net_inet_from_addr_port(a, addr);
}

void net_udp_addr_free(Alloc *a, NetUDPAddr *addr) {
    burrow__net_inet_addr_free(a, (burrow__NetInetAddr *)(void *)addr);
}

/* addrPortUDPAddr: a NetipAddrPort as an Addr, whose String is the
 * AddrPort's. The errors that hold one copy it into the error arena, so that
 * it lives as long as they do. */
static Str nu_ap_network(void *self) {
    (void)self;
    return NU_LIT("udp");
}

static Str nu_ap_string(void *self, Alloc *a) {
    return netip_addr_port_string(*(const NetipAddrPort *)self, a);
}

static const Type nu_ap_desc = {
    BURROW_S_INIT("addrPortUDPAddr"),
    BURROW_S_INIT("net"),
    KIND_STRUCT,
    (uint32_t)sizeof(NetipAddrPort),
    (uint16_t)_Alignof(NetipAddrPort),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x6e756170U, /* "nuap" */
    NULL,
};

static const NetAddrVT nu_ap_vt = {&nu_ap_desc, nu_ap_network, nu_ap_string};

static NetAddr nu_ap_addr(NetipAddrPort ap) {
    NetipAddrPort *p = (NetipAddrPort *)mem_alloc_nozero(
        error_allocator(), sizeof(NetipAddrPort), _Alignof(NetipAddrPort));
    if (p == NULL)
        return nu_nil_addr;
    *p = ap;
    return (NetAddr){&nu_ap_vt, p};
}

/* ------------------------------------------------------------------- conn */

/* A UDPAddr that owns its bytes, for each end of a connection. */
typedef struct NuAddr {
    NetUDPAddr a;
    burrow__NetInetBytes b;
} NuAddr;

struct NetUDPConn {
    burrow__NetConnCore c;
    NuAddr laddr;
    NuAddr raddr;
};

static bool nu_network(Str network, Str *lit) {
    static const char *const names[] = {"udp", "udp4", "udp6"};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        Str n = str_from_cstr(names[i]);
        if (network.len == n.len && memcmp(network.p, n.p, (size_t)n.len) == 0) {
            *lit = n;
            return true;
        }
    }
    return false;
}

/* newUDPConn, with the addresses the socket ended up with. */
static void nu_new_conn(NetUDPConn *c) {
    NuAddr *l = &c->laddr;
    NuAddr *r = &c->raddr;
    if (burrow__net_inet_from_sockaddr(&c->c.fd.laddr, &l->a.ip, &l->a.port, &l->a.zone,
                                       &l->b))
        c->c.laddr = net_udp_addr_as_addr(&l->a);
    if (burrow__net_inet_from_sockaddr(&c->c.fd.raddr, &r->a.ip, &r->a.port, &r->a.zone,
                                       &r->b))
        c->c.raddr = net_udp_addr_as_addr(&r->a);
    c->c.raw = (burrow__NetRawConn){&c->c.fd, c->c.laddr, c->c.raddr, false};
}

static bool nu_is_oom(Error e) {
    return e.vt == burrow_err_out_of_memory.vt &&
           e.data == burrow_err_out_of_memory.data;
}

static NetUDPConn *nu_socket(Alloc *a, const burrow__NetSysOpts *o, Str net,
                             const NetUDPAddr *laddr, const NetUDPAddr *raddr,
                             bool listen, Error *err) {
    NetUDPConn *c =
        (NetUDPConn *)mem_alloc(a, sizeof(NetUDPConn), _Alignof(NetUDPConn));
    if (c == NULL) {
        *err = burrow_err_out_of_memory;
        return NULL;
    }
    c->c.alloc = a;
    *err = burrow__net_internet_socket(&c->c.fd, o != NULL ? &o->ctl : NULL, net,
                                       nu_inet(laddr), nu_inet(raddr), PAL_SOCK_DGRAM,
                                       0, listen);
    if (BURROW_FAILED(*err)) {
        mem_free(a, c, sizeof(NetUDPConn), _Alignof(NetUDPConn));
        return NULL;
    }
    nu_new_conn(c);
    return c;
}

NetUDPConn *burrow__net_sys_dial_udp(Alloc *a, const burrow__NetSysOpts *o, Str network,
                                     const NetUDPAddr *laddr, const NetUDPAddr *raddr,
                                     Error *err) {
    Str net = BURROW_STR_EMPTY;
    if (!nu_network(network, &net)) {
        BURROW_OUT(err, net_unknown_network_error(error_allocator(), network));
        return NULL;
    }
    Error e = BURROW_NO_ERROR;
    NetUDPConn *c = nu_socket(a, o, net, laddr, raddr, false, &e);
    BURROW_OUT(err, e);
    return c;
}

NetUDPConn *burrow__net_sys_listen_udp(Alloc *a, const burrow__NetSysOpts *o,
                                       Str network, const NetUDPAddr *laddr,
                                       Error *err) {
    Str net = BURROW_STR_EMPTY;
    if (!nu_network(network, &net)) {
        BURROW_OUT(err, net_unknown_network_error(error_allocator(), network));
        return NULL;
    }
    Error e = BURROW_NO_ERROR;
    NetUDPConn *c = nu_socket(a, o, net, laddr, NULL, true, &e);
    BURROW_OUT(err, e);
    return c;
}

NetUDPConn *burrow__net_dial_udp(Alloc *a, const burrow__NetSysOpts *o, Str network,
                                 const NetUDPAddr *laddr, const NetUDPAddr *raddr,
                                 Error *err) {
    Str net = BURROW_STR_EMPTY;
    NetAddr src = net_udp_addr_as_addr(laddr);
    NetAddr dst = net_udp_addr_as_addr(raddr);
    if (!nu_network(network, &net)) {
        Error u = net_unknown_network_error(error_allocator(), network);
        BURROW_OUT(err, burrow__net_op_error(NU_LIT("dial"), network, src, dst, u));
        return NULL;
    }
    if (raddr == NULL) {
        BURROW_OUT(err, burrow__net_op_error(NU_LIT("dial"), network, src, nu_nil_addr,
                                             burrow__net_err_missing_address));
        return NULL;
    }
    Error e = BURROW_NO_ERROR;
    NetUDPConn *c = nu_socket(a, o, net, laddr, raddr, false, &e);
    if (c == NULL && !nu_is_oom(e))
        e = burrow__net_op_error(NU_LIT("dial"), network, src, dst, e);
    BURROW_OUT(err, e);
    return c;
}

NetUDPConn *net_dial_udp(Alloc *a, Str network, const NetUDPAddr *laddr,
                         const NetUDPAddr *raddr, Error *err) {
    return burrow__net_dial_udp(a, NULL, network, laddr, raddr, err);
}

SyscallRawConn net_udp_conn_syscall_conn(NetUDPConn *c, Error *err) {
    if (c == NULL) {
        BURROW_OUT(err, burrow__net_einval());
        return (SyscallRawConn){NULL, NULL};
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return burrow__net_raw_conn(&c->c.raw);
}

NetUDPConn *net_listen_udp(Alloc *a, Str network, const NetUDPAddr *laddr, Error *err) {
    Str net = BURROW_STR_EMPTY;
    if (!nu_network(network, &net)) {
        Error u = net_unknown_network_error(error_allocator(), network);
        BURROW_OUT(err, burrow__net_op_error(NU_LIT("listen"), network, nu_nil_addr,
                                             net_udp_addr_as_addr(laddr), u));
        return NULL;
    }
    /* A NULL laddr is the zero UDPAddr, and is what the error shows. */
    NetUDPAddr zero = {0};
    if (laddr == NULL)
        laddr = &zero;
    Error e = BURROW_NO_ERROR;
    NetUDPConn *c = nu_socket(a, NULL, net, laddr, NULL, true, &e);
    if (c == NULL && !nu_is_oom(e))
        e = burrow__net_op_error(NU_LIT("listen"), network, nu_nil_addr,
                                 net_udp_addr_as_addr(laddr), e);
    BURROW_OUT(err, e);
    return c;
}

/* ------------------------------------------------------------- multicast */

/* Linux names an IPv4 interface by its index, in an ip_mreqn, and the other
 * systems by one of its addresses, which Go has to go and find. */
#if defined(BURROW_OS_LINUX)
#define NU_MREQN 1
#else
#define NU_MREQN 0
#endif

/* interfaceToIPv4Addr for an interface that is there: its first IPv4
 * address, or errNoSuchInterface when it has none. */
static Error nu_interface_ipv4(const NetInterface *ifi, Byte out[4]) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Error e = BURROW_NO_ERROR;
    Slice ifat = net_interface_addrs_of(ifi, arena_allocator(&ar), &e);
    if (BURROW_OK(e)) {
        e = burrow__net_err_no_such_interface;
        const NetAddr *as = (const NetAddr *)ifat.p;
        for (Int i = 0; i < ifat.len; i++) {
            NetIP ip = slice_nil(TYPE_BYTE);
            if (as[i].vt != NULL && as[i].vt->self_type == TYPE_NET_IP_ADDR)
                ip = ((const NetIPAddr *)as[i].data)->ip;
            else if (as[i].vt != NULL && as[i].vt->self_type == TYPE_NET_IP_NET)
                ip = ((const NetIPNet *)as[i].data)->ip;
            NetIP v4 = net_ip_to4(ip);
            if (v4.p != NULL) {
                memcpy(out, v4.p, 4);
                e = BURROW_NO_ERROR;
                break;
            }
        }
    }
    /* The error is the interface's, made outside the arena. */
    arena_free(&ar);
    return e;
}

static int32_t nu_index(const NetInterface *ifi) {
    if (ifi == NULL)
        return 0;
    return ifi->index < 0 || ifi->index > INT32_MAX ? -1 : (int32_t)ifi->index;
}

/* setIPv4MulticastInterface. */
static Error nu_set_ipv4_multicast_if(NetUDPConn *c, const NetInterface *ifi) {
    PalMreq m;
    memset(&m, 0, sizeof m);
    m.index = nu_index(ifi);
    if (!NU_MREQN) {
        Error e = nu_interface_ipv4(ifi, m.ifaddr);
        if (BURROW_FAILED(e)) {
#if defined(BURROW_OS_WINDOWS)
            /* Go's Windows code wraps whatever it got, not only an Errno. */
            if (e.vt != burrow_err_out_of_memory.vt ||
                e.data != burrow_err_out_of_memory.data)
                e = os_new_syscall_error(error_allocator(), NU_LIT("setsockopt"), e);
#endif
            return e;
        }
    }
    return burrow__netfd_setsockopt_mreq(&c->c.fd, PAL_MREQ_IPV4_IF, &m);
}

/* joinIPv4Group. Outside Linux an interface has to have an IPv4 address to
 * name it by, or it is errNoSuchMulticastInterface, and no interface is the
 * system's choice. */
static Error nu_join_ipv4_group(NetUDPConn *c, const NetInterface *ifi, NetIP ip4) {
    PalMreq m;
    memset(&m, 0, sizeof m);
    memcpy(m.group, ip4.p, 4);
    m.index = nu_index(ifi);
    if (!NU_MREQN && ifi != NULL) {
        Error e = nu_interface_ipv4(ifi, m.ifaddr);
        if (e.vt == burrow__net_err_no_such_interface.vt &&
            e.data == burrow__net_err_no_such_interface.data)
            return burrow__net_err_no_such_multicast_interface;
        if (BURROW_FAILED(e))
            return e;
        static const Byte zero[4] = {0, 0, 0, 0};
        if (memcmp(m.ifaddr, zero, 4) == 0)
            return burrow__net_err_no_such_multicast_interface;
    }
    return burrow__netfd_setsockopt_mreq(&c->c.fd, PAL_MREQ_IPV4_JOIN, &m);
}

/* listenIPv4MulticastUDP and listenIPv6MulticastUDP. */
static Error nu_listen_multicast(NetUDPConn *c, const NetInterface *ifi, NetIP ip) {
    NetIP ip4 = net_ip_to4(ip);
    Error e = BURROW_NO_ERROR;
    if (ip4.p != NULL) {
        if (ifi != NULL)
            e = nu_set_ipv4_multicast_if(c, ifi);
        if (BURROW_OK(e))
            e = burrow__netfd_setsockopt(&c->c.fd, PAL_IP_MULTICAST_LOOP, 0);
        if (BURROW_OK(e))
            e = nu_join_ipv4_group(c, ifi, ip4);
        return e;
    }
    if (ifi != NULL)
        e = burrow__netfd_setsockopt(&c->c.fd, PAL_IPV6_MULTICAST_IF, nu_index(ifi));
    if (BURROW_OK(e))
        e = burrow__netfd_setsockopt(&c->c.fd, PAL_IPV6_MULTICAST_LOOP, 0);
    if (BURROW_OK(e)) {
        PalMreq m;
        memset(&m, 0, sizeof m);
        memcpy(m.group, ip.p, (size_t)(ip.len < 16 ? ip.len : 16));
        m.index = nu_index(ifi);
        e = burrow__netfd_setsockopt_mreq(&c->c.fd, PAL_MREQ_IPV6_JOIN, &m);
    }
    return e;
}

NetUDPConn *net_listen_multicast_udp(Alloc *a, Str network, const NetInterface *ifi,
                                     const NetUDPAddr *gaddr, Error *err) {
    Str net = BURROW_STR_EMPTY;
    NetAddr ga = net_udp_addr_as_addr(gaddr);
    if (!nu_network(network, &net)) {
        Error u = net_unknown_network_error(error_allocator(), network);
        BURROW_OUT(err,
                   burrow__net_op_error(NU_LIT("listen"), network, nu_nil_addr, ga, u));
        return NULL;
    }
    if (gaddr == NULL || gaddr->ip.p == NULL) {
        BURROW_OUT(err, burrow__net_op_error(NU_LIT("listen"), network, nu_nil_addr, ga,
                                             burrow__net_err_missing_address));
        return NULL;
    }
    Error e = BURROW_NO_ERROR;
    NetUDPConn *c = nu_socket(a, NULL, net, gaddr, NULL, true, &e);
    if (c != NULL) {
        e = nu_listen_multicast(c, ifi, gaddr->ip);
        if (BURROW_FAILED(e)) {
            net_udp_conn_free(c);
            c = NULL;
        }
    }
    if (c == NULL && !nu_is_oom(e))
        e = burrow__net_op_error(NU_LIT("listen"), network, nu_nil_addr, ga, e);
    BURROW_OUT(err, e);
    return c;
}

Int net_udp_conn_read(NetUDPConn *c, Slice p, Error *err) {
    if (c == NULL) {
        BURROW_OUT(err, burrow__net_einval());
        return 0;
    }
    return burrow__conn_read(&c->c, p, err);
}

Int net_udp_conn_write(NetUDPConn *c, Slice p, Error *err) {
    if (c == NULL) {
        BURROW_OUT(err, burrow__net_einval());
        return 0;
    }
    return burrow__conn_write(&c->c, p, err);
}

/* readFrom, with the sender's sockaddr in from. */
static Int nu_read_from(NetUDPConn *c, Slice p, PalSockAddr *from, Error *err) {
    Error e = BURROW_NO_ERROR;
    Int n = burrow__netfd_read_from(&c->c.fd, p, from, &e);
    if (BURROW_FAILED(e))
        e = burrow__net_op_error(NU_LIT("read"), c->c.fd.net, c->c.laddr, c->c.raddr,
                                 e);
    *err = e;
    return n;
}

Int net_udp_conn_read_from_udp(NetUDPConn *c, Slice p, Alloc *a, NetUDPAddr **addr,
                               Error *err) {
    if (addr != NULL)
        *addr = NULL;
    if (c == NULL) {
        BURROW_OUT(err, burrow__net_einval());
        return 0;
    }
    PalSockAddr from = {0};
    Error e = BURROW_NO_ERROR;
    Int n = nu_read_from(c, p, &from, &e);
    if (BURROW_OK(e) && addr != NULL) {
        NuAddr got = {0};
        if (burrow__net_inet_from_sockaddr(&from, &got.a.ip, &got.a.port, &got.a.zone,
                                           &got.b)) {
            *addr = nu_addr_new(a, got.b.ip, got.a.ip.len, got.a.port, got.a.zone);
            if (*addr == NULL)
                e = burrow_err_out_of_memory;
        }
    }
    BURROW_OUT(err, e);
    return n;
}

Int net_udp_conn_read_from(NetUDPConn *c, Slice p, Alloc *a, NetAddr *addr,
                           Error *err) {
    NetUDPAddr *u = NULL;
    Int n = net_udp_conn_read_from_udp(c, p, a, addr != NULL ? &u : NULL, err);
    if (addr != NULL)
        *addr = net_udp_addr_as_addr(u);
    return n;
}

Int net_udp_conn_read_from_udp_addr_port(NetUDPConn *c, Slice p, NetipAddrPort *addr,
                                         Error *err) {
    if (addr != NULL)
        *addr = (NetipAddrPort){0};
    if (c == NULL) {
        BURROW_OUT(err, burrow__net_einval());
        return 0;
    }
    PalSockAddr from = {0};
    Error e = BURROW_NO_ERROR;
    Int n = nu_read_from(c, p, &from, &e);
    if (BURROW_OK(e) && addr != NULL) {
        NuAddr got = {0};
        if (burrow__net_inet_from_sockaddr(&from, &got.a.ip, &got.a.port, &got.a.zone,
                                           &got.b)) {
            NetipAddr ip =
                got.a.ip.len == 4
                    ? netip_addr_from4(got.b.ip)
                    : netip_addr_with_zone(netip_addr_from16(got.b.ip), got.a.zone);
            *addr = netip_addr_port_from(ip, (uint16_t)got.a.port);
        }
    }
    BURROW_OUT(err, e);
    return n;
}

/* writeTo: the checks Go makes before the sockaddr, and the send. */
static Int nu_write_to(NetUDPConn *c, Slice p, NetIP ip, Int port, Str zone,
                       Error *err) {
    PalSockAddr to = {0};
    Error e = burrow__net_ip_sockaddr(c->c.fd.family, ip, port, zone, &to);
    if (BURROW_FAILED(e)) {
        e = burrow__netfd_write_to_error(e);
        *err = e;
        return 0;
    }
    return burrow__netfd_write_to(&c->c.fd, p, &to, err);
}

Int net_udp_conn_write_to_udp(NetUDPConn *c, Slice p, const NetUDPAddr *addr,
                              Error *err) {
    if (c == NULL) {
        BURROW_OUT(err, burrow__net_einval());
        return 0;
    }
    Error e = BURROW_NO_ERROR;
    Int n = 0;
    if (c->c.fd.is_connected)
        e = net_err_write_to_connected;
    else if (addr == NULL)
        e = burrow__net_err_missing_address;
    else
        n = nu_write_to(c, p, addr->ip, addr->port, addr->zone, &e);
    if (BURROW_FAILED(e))
        e = burrow__net_op_error(NU_LIT("write"), c->c.fd.net, c->c.laddr,
                                 net_udp_addr_as_addr(addr), e);
    BURROW_OUT(err, e);
    return n;
}

Int net_udp_conn_write_to_udp_addr_port(NetUDPConn *c, Slice p, NetipAddrPort addr,
                                        Error *err) {
    if (c == NULL) {
        BURROW_OUT(err, burrow__net_einval());
        return 0;
    }
    Error e = BURROW_NO_ERROR;
    Int n = 0;
    NetipAddr ip = netip_addr_port_addr(addr);
    if (c->c.fd.is_connected) {
        e = net_err_write_to_connected;
    } else if (!netip_addr_port_is_valid(addr)) {
        e = burrow__net_err_missing_address;
    } else if (c->c.fd.family == PAL_AF_INET && !netip_addr_is4(ip) &&
               !netip_addr_is4_in6(ip)) {
        /* addrPortToSockaddrInet4. */
        NetAddrError ae = {NU_LIT("non-IPv4 address"),
                           netip_addr_string(ip, error_allocator())};
        e = net_addr_error_as_error(&ae, error_allocator());
    } else {
        /* addrPortToSockaddrInet4 and Inet6, which As4 and As16 make out of
         * the address, whatever the family. */
        NetipAddrAs16Ret b = netip_addr_as16(ip);
        NetIP nip = slice_from(b.a, 16, 16, TYPE_BYTE);
        n = nu_write_to(c, p, nip, (Int)netip_addr_port_port(addr), netip_addr_zone(ip),
                        &e);
    }
    if (BURROW_FAILED(e))
        e = burrow__net_op_error(NU_LIT("write"), c->c.fd.net, c->c.laddr,
                                 nu_ap_addr(addr), e);
    BURROW_OUT(err, e);
    return n;
}

Int net_udp_conn_write_to(NetUDPConn *c, Slice p, NetAddr addr, Error *err) {
    if (c == NULL) {
        BURROW_OUT(err, burrow__net_einval());
        return 0;
    }
    if (addr.vt != &nu_addr_vt) {
        BURROW_OUT(err, burrow__net_op_error(NU_LIT("write"), c->c.fd.net, c->c.laddr,
                                             addr, burrow__net_einval()));
        return 0;
    }
    return net_udp_conn_write_to_udp(c, p, (const NetUDPAddr *)addr.data, err);
}

/* ---------------------------------------------------------------- messages */

/* readMsg, with the sender's sockaddr in from. */
static Int nu_read_msg(NetUDPConn *c, Slice p, Slice oob, Int *oobn, Int *flags,
                       PalSockAddr *from, Error *err) {
    Int on = 0;
    Int fl = 0;
    Error e = BURROW_NO_ERROR;
    Int n = burrow__netfd_read_msg(&c->c.fd, p, oob, &on, &fl, from, &e);
    if (BURROW_FAILED(e))
        e = burrow__net_op_error(NU_LIT("read"), c->c.fd.net, c->c.laddr, c->c.raddr,
                                 e);
    if (oobn != NULL)
        *oobn = on;
    if (flags != NULL)
        *flags = fl;
    *err = e;
    return n;
}

Int net_udp_conn_read_msg_udp(NetUDPConn *c, Slice p, Slice oob, Alloc *a, Int *oobn,
                              Int *flags, NetUDPAddr **addr, Error *err) {
    if (addr != NULL)
        *addr = NULL;
    NetipAddrPort ap = {0};
    Error e = BURROW_NO_ERROR;
    Int n = net_udp_conn_read_msg_udp_addr_port(c, p, oob, oobn, flags, &ap, &e);
    if (addr != NULL && netip_addr_port_is_valid(ap)) {
        /* UDPAddrFromAddrPort. */
        NetipAddr ip = netip_addr_port_addr(ap);
        NetipAddrAs16Ret b = netip_addr_as16(ip);
        bool v4 = netip_addr_is4(ip);
        *addr = nu_addr_new(a, v4 ? b.a + 12 : b.a, v4 ? 4 : 16,
                            (Int)netip_addr_port_port(ap), netip_addr_zone(ip));
        if (*addr == NULL && BURROW_OK(e))
            e = burrow_err_out_of_memory;
    }
    BURROW_OUT(err, e);
    return n;
}

Int net_udp_conn_read_msg_udp_addr_port(NetUDPConn *c, Slice p, Slice oob, Int *oobn,
                                        Int *flags, NetipAddrPort *addr, Error *err) {
    if (oobn != NULL)
        *oobn = 0;
    if (flags != NULL)
        *flags = 0;
    if (addr != NULL)
        *addr = (NetipAddrPort){0};
    if (c == NULL) {
        BURROW_OUT(err, burrow__net_einval());
        return 0;
    }
    PalSockAddr from = {0};
    Error e = BURROW_NO_ERROR;
    Int n = nu_read_msg(c, p, oob, oobn, flags, &from, &e);
    if (addr != NULL) {
        NuAddr got = {0};
        if (burrow__net_inet_from_sockaddr(&from, &got.a.ip, &got.a.port, &got.a.zone,
                                           &got.b)) {
            NetipAddr ip =
                got.a.ip.len == 4
                    ? netip_addr_from4(got.b.ip)
                    : netip_addr_with_zone(netip_addr_from16(got.b.ip), got.a.zone);
            *addr = netip_addr_port_from(ip, (uint16_t)got.a.port);
        }
    }
    BURROW_OUT(err, e);
    return n;
}

/* writeMsg past its checks: to the address, when there is one. */
static Int nu_write_msg(NetUDPConn *c, Slice p, Slice oob, bool has_addr, NetIP ip,
                        Int port, Str zone, Int *oobn, Error *err) {
    PalSockAddr to = {0};
    if (has_addr) {
        Error e = burrow__net_ip_sockaddr(c->c.fd.family, ip, port, zone, &to);
        if (BURROW_FAILED(e)) {
            *err = burrow__netfd_write_msg_error(e);
            return 0;
        }
    }
    return burrow__netfd_write_msg(&c->c.fd, p, oob, has_addr ? &to : NULL, oobn, err);
}

Int net_udp_conn_write_msg_udp(NetUDPConn *c, Slice p, Slice oob,
                               const NetUDPAddr *addr, Int *oobn, Error *err) {
    if (oobn != NULL)
        *oobn = 0;
    if (c == NULL) {
        BURROW_OUT(err, burrow__net_einval());
        return 0;
    }
    Error e = BURROW_NO_ERROR;
    Int n = 0;
    Int on = 0;
    if (c->c.fd.is_connected && addr != NULL)
        e = net_err_write_to_connected;
    else if (!c->c.fd.is_connected && addr == NULL)
        e = burrow__net_err_missing_address;
    else if (addr == NULL)
        n = nu_write_msg(c, p, oob, false, slice_nil(TYPE_BYTE), 0, BURROW_STR_EMPTY,
                         &on, &e);
    else
        n = nu_write_msg(c, p, oob, true, addr->ip, addr->port, addr->zone, &on, &e);
    if (BURROW_FAILED(e))
        e = burrow__net_op_error(NU_LIT("write"), c->c.fd.net, c->c.laddr,
                                 net_udp_addr_as_addr(addr), e);
    if (oobn != NULL)
        *oobn = on;
    BURROW_OUT(err, e);
    return n;
}

Int net_udp_conn_write_msg_udp_addr_port(NetUDPConn *c, Slice p, Slice oob,
                                         NetipAddrPort addr, Int *oobn, Error *err) {
    if (oobn != NULL)
        *oobn = 0;
    if (c == NULL) {
        BURROW_OUT(err, burrow__net_einval());
        return 0;
    }
    Error e = BURROW_NO_ERROR;
    Int n = 0;
    Int on = 0;
    NetipAddr ip = netip_addr_port_addr(addr);
    bool valid = netip_addr_port_is_valid(addr);
    if (c->c.fd.is_connected && valid) {
        e = net_err_write_to_connected;
    } else if (!c->c.fd.is_connected && !valid) {
        e = burrow__net_err_missing_address;
    } else if (!valid) {
        n = nu_write_msg(c, p, oob, false, slice_nil(TYPE_BYTE), 0, BURROW_STR_EMPTY,
                         &on, &e);
    } else if (c->c.fd.family == PAL_AF_INET && !netip_addr_is4(ip) &&
               !netip_addr_is4_in6(ip)) {
        /* addrPortToSockaddrInet4. */
        NetAddrError ae = {NU_LIT("non-IPv4 address"),
                           netip_addr_string(ip, error_allocator())};
        e = net_addr_error_as_error(&ae, error_allocator());
    } else {
        NetipAddrAs16Ret b = netip_addr_as16(ip);
        NetIP nip = slice_from(b.a, 16, 16, TYPE_BYTE);
        n = nu_write_msg(c, p, oob, true, nip, (Int)netip_addr_port_port(addr),
                         netip_addr_zone(ip), &on, &e);
    }
    if (BURROW_FAILED(e))
        e = burrow__net_op_error(NU_LIT("write"), c->c.fd.net, c->c.laddr,
                                 nu_ap_addr(addr), e);
    if (oobn != NULL)
        *oobn = on;
    BURROW_OUT(err, e);
    return n;
}

Error net_udp_conn_close(NetUDPConn *c) {
    if (c == NULL)
        return burrow__net_einval();
    return burrow__conn_close(&c->c);
}

NetAddr net_udp_conn_local_addr(NetUDPConn *c) {
    if (c == NULL)
        return nu_nil_addr;
    return c->c.laddr;
}

NetAddr net_udp_conn_remote_addr(NetUDPConn *c) {
    if (c == NULL)
        return nu_nil_addr;
    return c->c.raddr;
}

static Error nu_deadline(NetUDPConn *c, Time t, uint32_t mode) {
    if (c == NULL)
        return burrow__net_einval();
    return burrow__conn_set_deadline(&c->c, t, mode);
}

Error net_udp_conn_set_deadline(NetUDPConn *c, Time t) {
    return nu_deadline(c, t, BURROW_POLL_READ | BURROW_POLL_WRITE);
}

Error net_udp_conn_set_read_deadline(NetUDPConn *c, Time t) {
    return nu_deadline(c, t, BURROW_POLL_READ);
}

Error net_udp_conn_set_write_deadline(NetUDPConn *c, Time t) {
    return nu_deadline(c, t, BURROW_POLL_WRITE);
}

Error net_udp_conn_set_read_buffer(NetUDPConn *c, Int bytes) {
    if (c == NULL)
        return burrow__net_einval();
    return burrow__conn_set_buffer(&c->c, PAL_SO_RCVBUF, bytes);
}

Error net_udp_conn_set_write_buffer(NetUDPConn *c, Int bytes) {
    if (c == NULL)
        return burrow__net_einval();
    return burrow__conn_set_buffer(&c->c, PAL_SO_SNDBUF, bytes);
}

/* The NetConn methods. */
static Int nu_m_read(void *self, Slice p, Error *err) {
    return net_udp_conn_read((NetUDPConn *)self, p, err);
}

static Int nu_m_write(void *self, Slice p, Error *err) {
    return net_udp_conn_write((NetUDPConn *)self, p, err);
}

static Error nu_m_close(void *self) {
    return net_udp_conn_close((NetUDPConn *)self);
}

static NetAddr nu_m_local_addr(void *self) {
    return net_udp_conn_local_addr((NetUDPConn *)self);
}

static NetAddr nu_m_remote_addr(void *self) {
    return net_udp_conn_remote_addr((NetUDPConn *)self);
}

static Error nu_m_set_deadline(void *self, Time t) {
    return net_udp_conn_set_deadline((NetUDPConn *)self, t);
}

static Error nu_m_set_read_deadline(void *self, Time t) {
    return net_udp_conn_set_read_deadline((NetUDPConn *)self, t);
}

static Error nu_m_set_write_deadline(void *self, Time t) {
    return net_udp_conn_set_write_deadline((NetUDPConn *)self, t);
}

static const Type nu_conn_desc = {
    BURROW_S_INIT("UDPConn"),
    BURROW_S_INIT("net"),
    KIND_STRUCT,
    (uint32_t)sizeof(NetUDPConn),
    (uint16_t)_Alignof(NetUDPConn),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x6e756463U, /* "nudc" */
    NULL,
};

static const NetConnVT nu_conn_vt = {
    {&nu_conn_desc, nu_m_read},
    {&nu_conn_desc, nu_m_write},
    {&nu_conn_desc, nu_m_close},
    nu_m_local_addr,
    nu_m_remote_addr,
    nu_m_set_deadline,
    nu_m_set_read_deadline,
    nu_m_set_write_deadline,
};

NetConn net_udp_conn_as_conn(NetUDPConn *c) {
    NetConn conn = {NULL, NULL};
    if (c != NULL) {
        conn.vt = &nu_conn_vt;
        conn.data = c;
    }
    return conn;
}

NetUDPConn *net_conn_as_udp_conn(NetConn c) {
    if (c.vt != &nu_conn_vt)
        return NULL;
    return (NetUDPConn *)c.data;
}

static Int nu_m_read_from(void *self, Slice p, Alloc *a, NetAddr *addr, Error *err) {
    return net_udp_conn_read_from((NetUDPConn *)self, p, a, addr, err);
}

static Int nu_m_write_to(void *self, Slice p, NetAddr addr, Error *err) {
    return net_udp_conn_write_to((NetUDPConn *)self, p, addr, err);
}

static const NetPacketConnVT nu_packet_conn_vt = {
    {&nu_conn_desc, nu_m_close},
    nu_m_read_from,
    nu_m_write_to,
    nu_m_local_addr,
    nu_m_set_deadline,
    nu_m_set_read_deadline,
    nu_m_set_write_deadline,
};

NetPacketConn net_udp_conn_as_packet_conn(NetUDPConn *c) {
    NetPacketConn conn = {NULL, NULL};
    if (c != NULL) {
        conn.vt = &nu_packet_conn_vt;
        conn.data = c;
    }
    return conn;
}

NetUDPConn *net_packet_conn_as_udp_conn(NetPacketConn c) {
    if (c.vt != &nu_packet_conn_vt)
        return NULL;
    return (NetUDPConn *)c.data;
}

void net_udp_conn_free(NetUDPConn *c) {
    if (c == NULL)
        return;
    (void)burrow__netfd_close(&c->c.fd);
    mem_free(c->c.alloc, c, sizeof(NetUDPConn), _Alignof(NetUDPConn));
}

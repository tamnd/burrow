/* IPAddr, the address of an end of an IP connection and what the resolver
 * hands back for each address it finds, and IPConn, the raw socket that
 * goes with it.
 *
 * Derived from Go's src/net/iprawsock.go and iprawsock_posix.go.
 * Go source: go1.27.1.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "internal.h"

#include "burrow/declare.h"
#include "burrow/error.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/net.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define IR_LIT(s) str_from_bytes((const Byte *)(s), (Int)sizeof(s) - 1)
#define IR_COUNT(arr) (uint16_t)(sizeof(arr) / sizeof((arr)[0]))
#define IR_SIG_STRING(IN, OUT) OUT(Str)

static const NetAddr ir_nil_addr = {NULL, NULL};

Str net_ip_addr_network(const NetIPAddr *a) {
    (void)a;
    return IR_LIT("ip");
}

Str net_ip_addr_string(const NetIPAddr *a, Alloc *al) {
    if (a == NULL)
        return str_clone(al, IR_LIT("<nil>"));
    Arena ar;
    arena_init(&ar, NULL, 0);
    Str ip =
        a->ip.len == 0 ? BURROW_STR_EMPTY : net_ip_string(a->ip, arena_allocator(&ar));
    Str s = a->zone.len > 0 ? burrow__net_cat(al, 3, ip, IR_LIT("%"), a->zone)
                            : str_clone(al, ip);
    arena_free(&ar);
    return s;
}

static Str ir_addr_m_network(NetIPAddr *self) {
    return net_ip_addr_network(self);
}

static Str ir_addr_m_string(NetIPAddr *self) {
    return net_ip_addr_string(self, error_allocator());
}

#define IR_ADDR_METHODS(M, T)                                                          \
    M(T, Network, ir_addr_m_network, IR_SIG_STRING)                                    \
    M(T, String, ir_addr_m_string, IR_SIG_STRING)

BURROW_METHODS_DEFINE(NetIPAddr, IR_ADDR_METHODS);

static const Field ir_addr_fields[] = {
    {BURROW_S_INIT("IP"),
     {NULL, 0},
     &burrow_type_NetIP,
     (uint32_t)offsetof(NetIPAddr, ip)},
    {BURROW_S_INIT("Zone"),
     {NULL, 0},
     TYPE_STRING,
     (uint32_t)offsetof(NetIPAddr, zone)},
};

static const Type ir_addr_desc = {
    BURROW_S_INIT("IPAddr"),
    BURROW_S_INIT("net"),
    KIND_STRUCT,
    (uint32_t)sizeof(NetIPAddr),
    (uint16_t)_Alignof(NetIPAddr),
    IR_COUNT(ir_addr_fields),
    IR_COUNT(burrow__methods_NetIPAddr),
    ir_addr_fields,
    burrow__methods_NetIPAddr,
    NULL,
    NULL,
    0,
    0x6e697261U, /* "nira" */
    NULL,
};

const Type *const TYPE_NET_IP_ADDR = &ir_addr_desc;

static Str ir_addr_network(void *self) {
    return net_ip_addr_network((const NetIPAddr *)self);
}

static Str ir_addr_string(void *self, Alloc *a) {
    return net_ip_addr_string((const NetIPAddr *)self, a);
}

static const NetAddrVT ir_addr_vt = {&ir_addr_desc, ir_addr_network, ir_addr_string};

NetAddr net_ip_addr_as_addr(const NetIPAddr *a) {
    NetAddr addr = {NULL, NULL};
    if (a != NULL) {
        addr.vt = &ir_addr_vt;
        addr.data = (void *)(uintptr_t)a;
    }
    return addr;
}

/* A NetIPAddr this package made, with the bytes its ip and zone point at
 * after it, and the size to give back. */
typedef struct IrAddrBox {
    NetIPAddr a;
    size_t size;
} IrAddrBox;

NetIPAddr *burrow__net_ip_addr_new(Alloc *a, NetIP ip, Str zone) {
    size_t size = sizeof(IrAddrBox) + (size_t)ip.len + (size_t)zone.len;
    IrAddrBox *b = (IrAddrBox *)mem_alloc_nozero(a, size, _Alignof(IrAddrBox));
    if (b == NULL)
        return NULL;
    b->size = size;
    Byte *p = (Byte *)(b + 1);
    if (ip.len > 0)
        memcpy(p, ip.p, (size_t)ip.len);
    b->a.ip = ip.len > 0 ? slice_from(p, ip.len, ip.len, TYPE_BYTE) : (NetIP){0};
    if (zone.len > 0)
        memcpy(p + ip.len, zone.p, (size_t)zone.len);
    b->a.zone = str_from_bytes(p + ip.len, zone.len);
    return &b->a;
}

void net_ip_addr_free(Alloc *a, NetIPAddr *addr) {
    if (addr == NULL)
        return;
    IrAddrBox *b = (IrAddrBox *)(void *)addr;
    mem_free(a, b, b->size, _Alignof(IrAddrBox));
}

/* ------------------------------------------------------------------- conn */

/* An IPAddr that owns its bytes, for each end of a connection. */
typedef struct IrAddr {
    NetIPAddr a;
    burrow__NetInetBytes b;
} IrAddr;

struct NetIPConn {
    burrow__NetConnCore c;
    IrAddr laddr;
    IrAddr raddr;
};

static bool ir_is_oom(Error e) {
    return e.vt == burrow_err_out_of_memory.vt &&
           e.data == burrow_err_out_of_memory.data;
}

/* sockaddrToIP, into a's bytes. False for a sockaddr that is not IP. */
static bool ir_from_sockaddr(const PalSockAddr *sa, IrAddr *a) {
    Int port = 0;
    return burrow__net_inet_from_sockaddr(sa, &a->a.ip, &port, &a->a.zone, &a->b);
}

/* newIPConn, with the addresses the socket ended up with. */
static void ir_new_conn(NetIPConn *c) {
    if (ir_from_sockaddr(&c->c.fd.laddr, &c->laddr))
        c->c.laddr = net_ip_addr_as_addr(&c->laddr.a);
    if (ir_from_sockaddr(&c->c.fd.raddr, &c->raddr))
        c->c.raddr = net_ip_addr_as_addr(&c->raddr.a);
    c->c.raw = (burrow__NetRawConn){&c->c.fd, c->c.laddr, c->c.raddr, false};
}

/* The IPAddr as the TCP and UDP one, with no port, for internetSocket. */
static const burrow__NetInetAddr *ir_inet(const NetIPAddr *a,
                                          burrow__NetInetAddr *out) {
    if (a == NULL)
        return NULL;
    out->ip = a->ip;
    out->port = 0;
    out->zone = a->zone;
    return out;
}

/* dialIP and listenIP: parseNetwork, then a raw socket for the protocol. */
static NetIPConn *ir_socket(Alloc *a, const burrow__NetSysOpts *o, Str network,
                            const NetIPAddr *laddr, const NetIPAddr *raddr, bool listen,
                            Error *err) {
    Str net = BURROW_STR_EMPTY;
    Int proto = 0;
    Error e = burrow__net_parse_network(network, true, &net, &proto);
    if (BURROW_FAILED(e)) {
        *err = e;
        return NULL;
    }
    if (!str_eq(net, IR_LIT("ip")) && !str_eq(net, IR_LIT("ip4")) &&
        !str_eq(net, IR_LIT("ip6"))) {
        *err = net_unknown_network_error(error_allocator(), network);
        return NULL;
    }
    NetIPConn *c = (NetIPConn *)mem_alloc(a, sizeof(NetIPConn), _Alignof(NetIPConn));
    if (c == NULL) {
        *err = burrow_err_out_of_memory;
        return NULL;
    }
    c->c.alloc = a;
    burrow__NetInetAddr l;
    burrow__NetInetAddr r;
    *err = burrow__net_internet_socket(&c->c.fd, o != NULL ? &o->ctl : NULL, net,
                                       ir_inet(laddr, &l), ir_inet(raddr, &r),
                                       PAL_SOCK_RAW, (int32_t)proto, listen);
    if (BURROW_FAILED(*err)) {
        mem_free(a, c, sizeof(NetIPConn), _Alignof(NetIPConn));
        return NULL;
    }
    ir_new_conn(c);
    return c;
}

NetIPConn *burrow__net_sys_dial_ip(Alloc *a, const burrow__NetSysOpts *o, Str network,
                                   const NetIPAddr *laddr, const NetIPAddr *raddr,
                                   Error *err) {
    Error e = BURROW_NO_ERROR;
    NetIPConn *c = ir_socket(a, o, network, laddr, raddr, false, &e);
    BURROW_OUT(err, e);
    return c;
}

NetIPConn *burrow__net_sys_listen_ip(Alloc *a, const burrow__NetSysOpts *o, Str network,
                                     const NetIPAddr *laddr, Error *err) {
    Error e = BURROW_NO_ERROR;
    NetIPConn *c = ir_socket(a, o, network, laddr, NULL, true, &e);
    BURROW_OUT(err, e);
    return c;
}

NetIPConn *burrow__net_dial_ip(Alloc *a, const burrow__NetSysOpts *o, Str network,
                               const NetIPAddr *laddr, const NetIPAddr *raddr,
                               Error *err) {
    NetAddr src = net_ip_addr_as_addr(laddr);
    if (raddr == NULL) {
        BURROW_OUT(err, burrow__net_op_error(IR_LIT("dial"), network, src, ir_nil_addr,
                                             burrow__net_err_missing_address));
        return NULL;
    }
    Error e = BURROW_NO_ERROR;
    NetIPConn *c = ir_socket(a, o, network, laddr, raddr, false, &e);
    if (c == NULL && !ir_is_oom(e))
        e = burrow__net_op_error(IR_LIT("dial"), network, src,
                                 net_ip_addr_as_addr(raddr), e);
    BURROW_OUT(err, e);
    return c;
}

NetIPConn *net_dial_ip(Alloc *a, Str network, const NetIPAddr *laddr,
                       const NetIPAddr *raddr, Error *err) {
    return burrow__net_dial_ip(a, NULL, network, laddr, raddr, err);
}

NetIPConn *net_listen_ip(Alloc *a, Str network, const NetIPAddr *laddr, Error *err) {
    /* A NULL laddr is the zero IPAddr, and is what the error shows. */
    NetIPAddr zero = {0};
    if (laddr == NULL)
        laddr = &zero;
    Error e = BURROW_NO_ERROR;
    NetIPConn *c = ir_socket(a, NULL, network, laddr, NULL, true, &e);
    if (c == NULL && !ir_is_oom(e))
        e = burrow__net_op_error(IR_LIT("listen"), network, ir_nil_addr,
                                 net_ip_addr_as_addr(laddr), e);
    BURROW_OUT(err, e);
    return c;
}

OsFile *net_ip_conn_file(NetIPConn *c, Alloc *a, Error *err) {
    if (c == NULL) {
        BURROW_OUT(err, burrow__net_einval());
        return NULL;
    }
    return burrow__conn_file(&c->c, a, err);
}

NetIPConn *burrow__net_ip_conn_from_file(Alloc *a, const burrow__NetFileSock *fs,
                                         Error *err) {
    NetIPConn *c = (NetIPConn *)mem_alloc(a, sizeof(NetIPConn), _Alignof(NetIPConn));
    if (c == NULL) {
        (void)pal_socket_close(fs->s, NULL);
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    c->c.alloc = a;
    Error e = burrow__netfd_from_file(&c->c.fd, fs);
    if (BURROW_FAILED(e)) {
        mem_free(a, c, sizeof(NetIPConn), _Alignof(NetIPConn));
        BURROW_OUT(err, e);
        return NULL;
    }
    ir_new_conn(c);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return c;
}

SyscallRawConn net_ip_conn_syscall_conn(NetIPConn *c, Error *err) {
    if (c == NULL) {
        BURROW_OUT(err, burrow__net_einval());
        return (SyscallRawConn){NULL, NULL};
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return burrow__net_raw_conn(&c->c.raw);
}

Int net_ip_conn_read(NetIPConn *c, Slice p, Error *err) {
    if (c == NULL) {
        BURROW_OUT(err, burrow__net_einval());
        return 0;
    }
    return burrow__conn_read(&c->c, p, err);
}

Int net_ip_conn_write(NetIPConn *c, Slice p, Error *err) {
    if (c == NULL) {
        BURROW_OUT(err, burrow__net_einval());
        return 0;
    }
    return burrow__conn_write(&c->c, p, err);
}

/* stripIPv4Header: the IPv4 header that a raw socket hands back in front of
 * the payload, moved out of the way. Anything that does not look like one
 * stays. */
static Int ir_strip_ipv4_header(Int n, Slice p) {
    if (p.len < 20)
        return n;
    Byte *b = (Byte *)p.p;
    Int l = (Int)(b[0] & 0x0f) << 2;
    if (l < 20 || l > p.len)
        return n;
    if (b[0] >> 4 != 4)
        return n;
    memmove(b, b + l, (size_t)(p.len - l));
    return n - l;
}

Int net_ip_conn_read_from_ip(NetIPConn *c, Slice p, Alloc *a, NetIPAddr **addr,
                             Error *err) {
    if (addr != NULL)
        *addr = NULL;
    if (c == NULL) {
        BURROW_OUT(err, burrow__net_einval());
        return 0;
    }
    PalSockAddr from = {0};
    Error e = BURROW_NO_ERROR;
    Int n = burrow__netfd_read_from(&c->c.fd, p, &from, &e);
    IrAddr got;
    memset(&got, 0, sizeof got);
    if (ir_from_sockaddr(&from, &got)) {
        if (from.family == PAL_AF_INET)
            n = ir_strip_ipv4_header(n, p);
        if (addr != NULL && BURROW_OK(e)) {
            *addr = burrow__net_ip_addr_new(a, got.a.ip, got.a.zone);
            if (*addr == NULL)
                e = burrow_err_out_of_memory;
        }
    }
    if (BURROW_FAILED(e) && !ir_is_oom(e))
        e = burrow__net_op_error(IR_LIT("read"), c->c.fd.net, c->c.laddr, c->c.raddr,
                                 e);
    BURROW_OUT(err, e);
    return n;
}

Int net_ip_conn_read_from(NetIPConn *c, Slice p, Alloc *a, NetAddr *addr, Error *err) {
    NetIPAddr *ip = NULL;
    Int n = net_ip_conn_read_from_ip(c, p, a, addr != NULL ? &ip : NULL, err);
    if (addr != NULL)
        *addr = net_ip_addr_as_addr(ip);
    return n;
}

Int net_ip_conn_write_to_ip(NetIPConn *c, Slice p, const NetIPAddr *addr, Error *err) {
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
    } else {
        PalSockAddr to = {0};
        e = burrow__net_ip_sockaddr(c->c.fd.family, addr->ip, 0, addr->zone, &to);
        if (BURROW_FAILED(e))
            e = burrow__netfd_write_to_error(e);
        else
            n = burrow__netfd_write_to(&c->c.fd, p, &to, &e);
    }
    if (BURROW_FAILED(e))
        e = burrow__net_op_error(IR_LIT("write"), c->c.fd.net, c->c.laddr,
                                 net_ip_addr_as_addr(addr), e);
    BURROW_OUT(err, e);
    return n;
}

Int net_ip_conn_write_to(NetIPConn *c, Slice p, NetAddr addr, Error *err) {
    if (c == NULL) {
        BURROW_OUT(err, burrow__net_einval());
        return 0;
    }
    if (addr.vt != &ir_addr_vt) {
        BURROW_OUT(err, burrow__net_op_error(IR_LIT("write"), c->c.fd.net, c->c.laddr,
                                             addr, burrow__net_einval()));
        return 0;
    }
    return net_ip_conn_write_to_ip(c, p, (const NetIPAddr *)addr.data, err);
}

Int net_ip_conn_read_msg_ip(NetIPConn *c, Slice p, Slice oob, Alloc *a, Int *oobn,
                            Int *flags, NetIPAddr **addr, Error *err) {
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
    IrAddr got;
    memset(&got, 0, sizeof got);
    if (addr != NULL && ir_from_sockaddr(&from, &got)) {
        *addr = burrow__net_ip_addr_new(a, got.a.ip, got.a.zone);
        if (*addr == NULL && BURROW_OK(e))
            e = burrow_err_out_of_memory;
    }
    if (BURROW_FAILED(e) && !ir_is_oom(e))
        e = burrow__net_op_error(IR_LIT("read"), c->c.fd.net, c->c.laddr, c->c.raddr,
                                 e);
    if (oobn != NULL)
        *oobn = on;
    if (flags != NULL)
        *flags = fl;
    BURROW_OUT(err, e);
    return n;
}

Int net_ip_conn_write_msg_ip(NetIPConn *c, Slice p, Slice oob, const NetIPAddr *addr,
                             Int *oobn, Error *err) {
    if (oobn != NULL)
        *oobn = 0;
    if (c == NULL) {
        BURROW_OUT(err, burrow__net_einval());
        return 0;
    }
    Error e = BURROW_NO_ERROR;
    Int n = 0;
    Int on = 0;
    if (c->c.fd.is_connected) {
        e = net_err_write_to_connected;
    } else if (addr == NULL) {
        e = burrow__net_err_missing_address;
    } else {
        PalSockAddr to = {0};
        e = burrow__net_ip_sockaddr(c->c.fd.family, addr->ip, 0, addr->zone, &to);
        if (BURROW_FAILED(e))
            e = burrow__netfd_write_msg_error(e);
        else
            n = burrow__netfd_write_msg(&c->c.fd, p, oob, &to, &on, &e);
    }
    if (BURROW_FAILED(e))
        e = burrow__net_op_error(IR_LIT("write"), c->c.fd.net, c->c.laddr,
                                 net_ip_addr_as_addr(addr), e);
    if (oobn != NULL)
        *oobn = on;
    BURROW_OUT(err, e);
    return n;
}

Error net_ip_conn_close(NetIPConn *c) {
    if (c == NULL)
        return burrow__net_einval();
    return burrow__conn_close(&c->c);
}

NetAddr net_ip_conn_local_addr(NetIPConn *c) {
    if (c == NULL)
        return ir_nil_addr;
    return c->c.laddr;
}

NetAddr net_ip_conn_remote_addr(NetIPConn *c) {
    if (c == NULL)
        return ir_nil_addr;
    return c->c.raddr;
}

static Error ir_deadline(NetIPConn *c, Time t, uint32_t mode) {
    if (c == NULL)
        return burrow__net_einval();
    return burrow__conn_set_deadline(&c->c, t, mode);
}

Error net_ip_conn_set_deadline(NetIPConn *c, Time t) {
    return ir_deadline(c, t, BURROW_POLL_READ | BURROW_POLL_WRITE);
}

Error net_ip_conn_set_read_deadline(NetIPConn *c, Time t) {
    return ir_deadline(c, t, BURROW_POLL_READ);
}

Error net_ip_conn_set_write_deadline(NetIPConn *c, Time t) {
    return ir_deadline(c, t, BURROW_POLL_WRITE);
}

Error net_ip_conn_set_read_buffer(NetIPConn *c, Int bytes) {
    if (c == NULL)
        return burrow__net_einval();
    return burrow__conn_set_buffer(&c->c, PAL_SO_RCVBUF, bytes);
}

Error net_ip_conn_set_write_buffer(NetIPConn *c, Int bytes) {
    if (c == NULL)
        return burrow__net_einval();
    return burrow__conn_set_buffer(&c->c, PAL_SO_SNDBUF, bytes);
}

/* The NetConn methods. */
static Int ir_m_read(void *self, Slice p, Error *err) {
    return net_ip_conn_read((NetIPConn *)self, p, err);
}

static Int ir_m_write(void *self, Slice p, Error *err) {
    return net_ip_conn_write((NetIPConn *)self, p, err);
}

static Error ir_m_close(void *self) {
    return net_ip_conn_close((NetIPConn *)self);
}

static NetAddr ir_m_local_addr(void *self) {
    return net_ip_conn_local_addr((NetIPConn *)self);
}

static NetAddr ir_m_remote_addr(void *self) {
    return net_ip_conn_remote_addr((NetIPConn *)self);
}

static Error ir_m_set_deadline(void *self, Time t) {
    return net_ip_conn_set_deadline((NetIPConn *)self, t);
}

static Error ir_m_set_read_deadline(void *self, Time t) {
    return net_ip_conn_set_read_deadline((NetIPConn *)self, t);
}

static Error ir_m_set_write_deadline(void *self, Time t) {
    return net_ip_conn_set_write_deadline((NetIPConn *)self, t);
}

static const Type ir_conn_desc = {
    BURROW_S_INIT("IPConn"),
    BURROW_S_INIT("net"),
    KIND_STRUCT,
    (uint32_t)sizeof(NetIPConn),
    (uint16_t)_Alignof(NetIPConn),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x6e697063U, /* "nipc" */
    NULL,
};

static const NetConnVT ir_conn_vt = {
    {&ir_conn_desc, ir_m_read},
    {&ir_conn_desc, ir_m_write},
    {&ir_conn_desc, ir_m_close},
    ir_m_local_addr,
    ir_m_remote_addr,
    ir_m_set_deadline,
    ir_m_set_read_deadline,
    ir_m_set_write_deadline,
};

const IoWriterVT *const burrow__ir_conn_writer = &ir_conn_vt.writer;

NetConn net_ip_conn_as_conn(NetIPConn *c) {
    NetConn conn = {NULL, NULL};
    if (c != NULL) {
        conn.vt = &ir_conn_vt;
        conn.data = c;
    }
    return conn;
}

NetIPConn *net_conn_as_ip_conn(NetConn c) {
    if (c.vt != &ir_conn_vt)
        return NULL;
    return (NetIPConn *)c.data;
}

static Int ir_m_read_from(void *self, Slice p, Alloc *a, NetAddr *addr, Error *err) {
    return net_ip_conn_read_from((NetIPConn *)self, p, a, addr, err);
}

static Int ir_m_write_to(void *self, Slice p, NetAddr addr, Error *err) {
    return net_ip_conn_write_to((NetIPConn *)self, p, addr, err);
}

static const NetPacketConnVT ir_packet_conn_vt = {
    {&ir_conn_desc, ir_m_close},
    ir_m_read_from,
    ir_m_write_to,
    ir_m_local_addr,
    ir_m_set_deadline,
    ir_m_set_read_deadline,
    ir_m_set_write_deadline,
};

NetPacketConn net_ip_conn_as_packet_conn(NetIPConn *c) {
    NetPacketConn conn = {NULL, NULL};
    if (c != NULL) {
        conn.vt = &ir_packet_conn_vt;
        conn.data = c;
    }
    return conn;
}

NetIPConn *net_packet_conn_as_ip_conn(NetPacketConn c) {
    if (c.vt != &ir_packet_conn_vt)
        return NULL;
    return (NetIPConn *)c.data;
}

void net_ip_conn_free(NetIPConn *c) {
    if (c == NULL)
        return;
    (void)burrow__netfd_close(&c->c.fd);
    mem_free(c->c.alloc, c, sizeof(NetIPConn), _Alignof(NetIPConn));
}

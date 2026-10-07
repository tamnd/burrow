/* The routing messages of macOS and FreeBSD: RouteRIB, which reads what the
 * kernel knows about routes and interfaces, ParseRoutingMessage, which splits
 * that or what a routing socket says into messages, and ParseRoutingSockaddr,
 * which reads the addresses after each message's header. Go has deprecated
 * all of it in favour of golang.org/x/net/route.
 *
 * Every read of a header or an address goes through memcpy, so b can be
 * anywhere. Go reads the header through a pointer into b even when b is too
 * short to hold it, and slices past the end of b when a length in it is
 * wrong, and panics. Here those are EINVAL.
 *
 * Derived from Go's src/syscall/route_bsd.go, route_darwin.go,
 * route_freebsd.go, route_freebsd_32bit.go and route_freebsd_64bit.go.
 * Go source: go1.27.1.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/syscall.h"

#if defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS) || defined(BURROW_OS_FREEBSD)

#include "burrow/atomic.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"

#include "internal.h"

#include <stddef.h>
#include <string.h>

#if defined(BURROW_OS_FREEBSD) && (defined(BURROW_ARCH_386) || defined(BURROW_ARCH_ARM))
#define ROUTE_FREEBSD32 1
#endif

static Error route_errno(SyscallErrno e) {
    return burrow__syscall_errno_err(e);
}

/* ------------------------------------------------------------ descriptors */

static const Type route_message_desc = {
    {(const Byte *)"RouteMessage", 12},
    {(const Byte *)"syscall", 7},
    KIND_STRUCT,
    (uint32_t)sizeof(SyscallRouteMessage),
    (uint16_t)_Alignof(SyscallRouteMessage),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x72746d67U, /* "rtmg" */
    NULL,
};

static const Type route_if_message_desc = {
    {(const Byte *)"InterfaceMessage", 16},
    {(const Byte *)"syscall", 7},
    KIND_STRUCT,
    (uint32_t)sizeof(SyscallInterfaceMessage),
    (uint16_t)_Alignof(SyscallInterfaceMessage),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x72746966U, /* "rtif" */
    NULL,
};

static const Type route_ifa_message_desc = {
    {(const Byte *)"InterfaceAddrMessage", 20},
    {(const Byte *)"syscall", 7},
    KIND_STRUCT,
    (uint32_t)sizeof(SyscallInterfaceAddrMessage),
    (uint16_t)_Alignof(SyscallInterfaceAddrMessage),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x72746661U, /* "rtfa" */
    NULL,
};

static const Type route_ifma_message_desc = {
    {(const Byte *)"InterfaceMulticastAddrMessage", 29},
    {(const Byte *)"syscall", 7},
    KIND_STRUCT,
    (uint32_t)sizeof(SyscallInterfaceMulticastAddrMessage),
    (uint16_t)_Alignof(SyscallInterfaceMulticastAddrMessage),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x72746d61U, /* "rtma" */
    NULL,
};

#if defined(BURROW_OS_FREEBSD)
static const Type route_announce_message_desc = {
    {(const Byte *)"InterfaceAnnounceMessage", 24},
    {(const Byte *)"syscall", 7},
    KIND_STRUCT,
    (uint32_t)sizeof(SyscallInterfaceAnnounceMessage),
    (uint16_t)_Alignof(SyscallInterfaceAnnounceMessage),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x7274616eU, /* "rtan" */
    NULL,
};
#endif

/* The element types of the two Slices this file makes, which are interfaces. */
static const Type route_routing_message_desc = {
    {(const Byte *)"RoutingMessage", 14},
    {(const Byte *)"syscall", 7},
    KIND_INTERFACE,
    (uint32_t)sizeof(SyscallRoutingMessage),
    (uint16_t)_Alignof(SyscallRoutingMessage),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x7274726dU, /* "rtrm" */
    NULL,
};

static const Type route_sockaddr_desc = {
    {(const Byte *)"Sockaddr", 8},
    {(const Byte *)"syscall", 7},
    KIND_INTERFACE,
    (uint32_t)sizeof(SyscallSockaddr),
    (uint16_t)_Alignof(SyscallSockaddr),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x72747361U, /* "rtsa" */
    NULL,
};

const Type *const TYPE_SYSCALL_ROUTE_MESSAGE = &route_message_desc;
const Type *const TYPE_SYSCALL_INTERFACE_MESSAGE = &route_if_message_desc;
const Type *const TYPE_SYSCALL_INTERFACE_ADDR_MESSAGE = &route_ifa_message_desc;
const Type *const TYPE_SYSCALL_INTERFACE_MULTICAST_ADDR_MESSAGE =
    &route_ifma_message_desc;
#if defined(BURROW_OS_FREEBSD)
const Type *const TYPE_SYSCALL_INTERFACE_ANNOUNCE_MESSAGE =
    &route_announce_message_desc;
#endif

/* ------------------------------------------------------------- alignment */

#if defined(ROUTE_FREEBSD32)
/* Go's freebsdConfArch is "amd64" when this 32-bit program runs on a 64-bit
 * kernel, whose routing messages are laid out for 64 bits. Go reads it from
 * the "machine" line of kern.conftxt when the package starts, and this does
 * the first time it is needed: 0 for not yet, 1 for amd64, 2 for anything
 * else. */
static uint32_t route_conf_arch;

static bool route_conf_amd64(void) {
    uint32_t v = burrow__atomic_load_acquire_u32(&route_conf_arch);
    if (v != 0)
        return v == 1;
    /* An arena, since the Str does not say how much was allocated for it. */
    Arena ar;
    arena_init(&ar, NULL, 0);
    Str conf = syscall_sysctl(arena_allocator(&ar), BURROW_S("kern.conftxt"), NULL);
    Str arch = {NULL, 0};
    for (Int i = 0, j = 0; j < conf.len; j++) {
        if (conf.p[j] != '\n')
            continue;
        Str s = {conf.p + i, j - i};
        i = j + 1;
        if (s.len > 7 && memcmp(s.p, "machine", 7) == 0) {
            s.p += 7;
            s.len -= 7;
            /* Go takes off one space or tab, not all of them. */
            if (s.len > 0 && (s.p[0] == ' ' || s.p[0] == '\t')) {
                s.p++;
                s.len--;
            }
            arch = s;
            break;
        }
    }
    v = arch.len == 5 && memcmp(arch.p, "amd64", 5) == 0 ? 1 : 2;
    arena_free(&ar);
    burrow__atomic_store_release_u32(&route_conf_arch, v);
    return v == 1;
}
#endif

/* Go's rsaAlignOf: salen rounded up to what the kernel aligns addresses to,
 * which for 0 is the alignment itself. 64-bit macOS aligns to 4 bytes, and
 * FreeBSD to the pointer size of the kernel. */
static Int route_align(Int salen) {
#if defined(BURROW_OS_FREEBSD)
    Int salign = (Int)sizeof(void *);
#if defined(ROUTE_FREEBSD32)
    if (route_conf_amd64())
        salign = 8;
#endif
#else
    Int salign = sizeof(void *) == 8 ? 4 : (Int)sizeof(void *);
#endif
    if (salen == 0)
        return salign;
    return (salen + salign - 1) & ~(salign - 1);
}

/* ------------------------------------------------------------- addresses */

/* Go's parseLinkLayerAddr: b as a datalink address in the kernel's short form,
 * a type, three lengths, and the name, address and selector after them. *n is
 * how many bytes it took, aligned. */
static Error route_link_layer(Alloc *a, const Byte *b, Int blen,
                              SyscallSockaddrDatalink **out, Int *n) {
    Int l = 4 + (Int)b[1] + (Int)b[2] + (Int)b[3];
    if (blen < l)
        return route_errno(SYSCALL_EINVAL);
    SyscallSockaddrDatalink *sa = (SyscallSockaddrDatalink *)mem_alloc(
        a, sizeof(SyscallSockaddrDatalink), _Alignof(SyscallSockaddrDatalink));
    if (sa == NULL)
        return burrow_err_out_of_memory;
    sa->type = b[0];
    sa->nlen = b[1];
    sa->alen = b[2];
    sa->slen = b[3];
    for (Int i = 0; i < (Int)sizeof sa->data && i < l - 4; i++)
        sa->data[i] = (int8_t)b[4 + i];
    *out = sa;
    *n = route_align(l);
    return BURROW_NO_ERROR;
}

/* Go's parseSockaddrLink: b as a whole datalink address. */
static Error route_sockaddr_link(Alloc *a, const Byte *b, Int blen,
                                 SyscallSockaddr *out) {
    if (blen < 8)
        return route_errno(SYSCALL_EINVAL);
    SyscallSockaddrDatalink *sa = NULL;
    Int n = 0;
    Error e = route_link_layer(a, b + 4, blen - 4, &sa, &n);
    if (BURROW_FAILED(e))
        return e;
    SyscallRawSockaddrDatalink raw;
    memcpy(&raw, b, 4);
    sa->len = raw.len;
    sa->family = raw.family;
    sa->index = raw.index;
    *out = syscall_sockaddr_datalink_as_sockaddr(sa);
    return BURROW_NO_ERROR;
}

/* Go's parseSockaddrInet: b as an IPv4 or IPv6 address. */
static Error route_sockaddr_inet(Alloc *a, const Byte *b, Int blen, uint8_t family,
                                 SyscallSockaddr *out) {
    Int need = family == SYSCALL_AF_INET    ? SYSCALL_SIZEOF_SOCKADDR_INET4
               : family == SYSCALL_AF_INET6 ? SYSCALL_SIZEOF_SOCKADDR_INET6
                                            : -1;
    if (need < 0 || blen < need)
        return route_errno(SYSCALL_EINVAL);
    SyscallRawSockaddrAny rsa;
    memset(&rsa, 0, sizeof rsa);
    memcpy(&rsa, b, (size_t)(blen < (Int)sizeof rsa ? blen : (Int)sizeof rsa));
    return burrow__syscall_any_to_sockaddr(a, &rsa, out);
}

/* Where the address starts in sockaddr_in and sockaddr_in6, Go's
 * offsetofInet4 and offsetofInet6. */
#define ROUTE_OFF_INET4 ((Int)offsetof(SyscallRawSockaddrInet4, addr))
#define ROUTE_OFF_INET6 ((Int)offsetof(SyscallRawSockaddrInet6, addr))

/* Copies what of src fits into dst, as Go's copy does. */
static void route_copy(uint8_t *dst, Int dlen, const Byte *src, Int slen) {
    if (slen > 0)
        memcpy(dst, src, (size_t)(slen < dlen ? slen : dlen));
}

static Error route_new_inet4(Alloc *a, SyscallSockaddrInet4 **out) {
    *out = (SyscallSockaddrInet4 *)mem_alloc(a, sizeof(SyscallSockaddrInet4),
                                             _Alignof(SyscallSockaddrInet4));
    return *out == NULL ? burrow_err_out_of_memory : BURROW_NO_ERROR;
}

static Error route_new_inet6(Alloc *a, SyscallSockaddrInet6 **out) {
    *out = (SyscallSockaddrInet6 *)mem_alloc(a, sizeof(SyscallSockaddrInet6),
                                             _Alignof(SyscallSockaddrInet6));
    return *out == NULL ? burrow_err_out_of_memory : BURROW_NO_ERROR;
}

/* Go's parseNetworkLayerAddr: b as an address in the kernel's short form, a
 * length in bytes and as much of the address as is not zero, which is how
 * netmasks come. family is the family of the last full address before it.
 * The cases are in Go's order, which matters. */
static Error route_network_layer(Alloc *a, const Byte *b, Int blen, uint8_t family,
                                 SyscallSockaddr *out) {
    Int l = route_align((Int)b[0]);
    if (blen < l)
        return route_errno(SYSCALL_EINVAL);
    Error e;
    if (b[0] == SYSCALL_SIZEOF_SOCKADDR_INET6 || family == SYSCALL_AF_INET6) {
        SyscallSockaddrInet6 *sa = NULL;
        if (BURROW_FAILED(e = route_new_inet6(a, &sa)))
            return e;
        if (b[0] == SYSCALL_SIZEOF_SOCKADDR_INET6)
            route_copy(sa->addr, 16, b + ROUTE_OFF_INET6, blen - ROUTE_OFF_INET6);
        else if (l - 1 < ROUTE_OFF_INET6)
            route_copy(sa->addr, 16, b + 1, l - 1);
        else
            route_copy(sa->addr, 16, b + l - ROUTE_OFF_INET6, ROUTE_OFF_INET6);
        *out = syscall_sockaddr_inet6_as_sockaddr(sa);
        return BURROW_NO_ERROR;
    }
    /* An IPv4 address, or an old one, AF_UNSPEC or one Go does not know,
     * which it takes as IPv4 too. */
    SyscallSockaddrInet4 *sa = NULL;
    if (BURROW_FAILED(e = route_new_inet4(a, &sa)))
        return e;
    if (b[0] == SYSCALL_SIZEOF_SOCKADDR_INET4)
        route_copy(sa->addr, 4, b + ROUTE_OFF_INET4, blen - ROUTE_OFF_INET4);
    else if (l - 1 < ROUTE_OFF_INET4)
        route_copy(sa->addr, 4, b + 1, l - 1);
    else
        route_copy(sa->addr, 4, b + l - ROUTE_OFF_INET4, ROUTE_OFF_INET4);
    *out = syscall_sockaddr_inet4_as_sockaddr(sa);
    return BURROW_NO_ERROR;
}

void syscall_routing_sockaddr_free(Alloc *a, Slice sas) {
    if (sas.p == NULL)
        return;
    SyscallSockaddr *s = (SyscallSockaddr *)sas.p;
    for (Int i = 0; i < sas.len; i++)
        syscall_sockaddr_free(a, s[i]);
    mem_free(a, sas.p, (size_t)sas.cap * sizeof(SyscallSockaddr),
             _Alignof(SyscallSockaddr));
}

static Slice route_nil_sockaddrs(void) {
    return (Slice){NULL, 0, 0, &route_sockaddr_desc};
}

/* A Slice of RTAX_MAX zero Sockaddrs from a. */
static Slice route_new_sockaddrs(Alloc *a, Error *err) {
    void *p = mem_alloc(a, (size_t)SYSCALL_RTAX_MAX * sizeof(SyscallSockaddr),
                        _Alignof(SyscallSockaddr));
    if (p == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return route_nil_sockaddrs();
    }
    return (Slice){p, SYSCALL_RTAX_MAX, SYSCALL_RTAX_MAX, &route_sockaddr_desc};
}

/* The addresses after a route, interface address or multicast address
 * message's header, one for each bit in addrs. Go's sockaddr methods for the
 * three are this loop with one difference: the multicast one reads an address
 * of a family it does not know as a link layer address and does not keep the
 * family for the next one. */
static Slice route_addrs(Alloc *a, Slice data, int32_t addrs, bool multicast,
                         Error *err) {
    Slice out = route_new_sockaddrs(a, err);
    if (out.p == NULL)
        return out;
    SyscallSockaddr *sas = (SyscallSockaddr *)out.p;
    const Byte *b = (const Byte *)data.p;
    Int blen = data.len;
    Int min = route_align(0);
    uint8_t family = SYSCALL_AF_UNSPEC;
    Error e = BURROW_NO_ERROR;
    for (Int i = 0; i < SYSCALL_RTAX_MAX && blen >= min; i++) {
        if ((addrs & (int32_t)(1U << i)) == 0)
            continue;
        uint8_t rlen = b[0], rfamily = b[1];
        Int n;
        if (rfamily == SYSCALL_AF_LINK) {
            e = route_sockaddr_link(a, b, blen, &sas[i]);
            n = route_align((Int)rlen);
        } else if (rfamily == SYSCALL_AF_INET || rfamily == SYSCALL_AF_INET6) {
            e = route_sockaddr_inet(a, b, blen, rfamily, &sas[i]);
            n = route_align((Int)rlen);
            if (!multicast)
                family = rfamily;
        } else if (multicast) {
            SyscallSockaddrDatalink *sa = NULL;
            n = 0;
            e = route_link_layer(a, b, blen, &sa, &n);
            if (BURROW_OK(e))
                sas[i] = syscall_sockaddr_datalink_as_sockaddr(sa);
        } else {
            e = route_network_layer(a, b, blen, family, &sas[i]);
            n = route_align((Int)b[0]);
        }
        if (BURROW_OK(e) && n > blen)
            e = route_errno(SYSCALL_EINVAL);
        if (BURROW_FAILED(e))
            break;
        b += n;
        blen -= n;
    }
    if (BURROW_FAILED(e)) {
        syscall_routing_sockaddr_free(a, out);
        BURROW_OUT(err, e);
        return route_nil_sockaddrs();
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return out;
}

/* -------------------------------------------------------------- messages */

static Slice route_message_sockaddr(void *self, Alloc *a, Error *err) {
    SyscallRouteMessage *m = (SyscallRouteMessage *)self;
    return route_addrs(a, m->data, m->header.addrs, false, err);
}

static Slice route_if_message_sockaddr(void *self, Alloc *a, Error *err) {
    SyscallInterfaceMessage *m = (SyscallInterfaceMessage *)self;
    if ((m->header.addrs & SYSCALL_RTA_IFP) == 0) {
        BURROW_OUT(err, BURROW_NO_ERROR);
        return route_nil_sockaddrs();
    }
    SyscallSockaddr sa = {NULL, NULL};
    Error e = route_sockaddr_link(a, (const Byte *)m->data.p, m->data.len, &sa);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return route_nil_sockaddrs();
    }
    Slice out = route_new_sockaddrs(a, err);
    if (out.p == NULL) {
        syscall_sockaddr_free(a, sa);
        return out;
    }
    ((SyscallSockaddr *)out.p)[SYSCALL_RTAX_IFP] = sa;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return out;
}

static Slice route_ifa_message_sockaddr(void *self, Alloc *a, Error *err) {
    SyscallInterfaceAddrMessage *m = (SyscallInterfaceAddrMessage *)self;
    return route_addrs(a, m->data, m->header.addrs, false, err);
}

static Slice route_ifma_message_sockaddr(void *self, Alloc *a, Error *err) {
    SyscallInterfaceMulticastAddrMessage *m =
        (SyscallInterfaceMulticastAddrMessage *)self;
    return route_addrs(a, m->data, m->header.addrs, true, err);
}

static const SyscallRoutingMessageVT route_message_vt = {&route_message_desc,
                                                         route_message_sockaddr};
static const SyscallRoutingMessageVT route_if_message_vt = {&route_if_message_desc,
                                                            route_if_message_sockaddr};
static const SyscallRoutingMessageVT route_ifa_message_vt = {
    &route_ifa_message_desc, route_ifa_message_sockaddr};
static const SyscallRoutingMessageVT route_ifma_message_vt = {
    &route_ifma_message_desc, route_ifma_message_sockaddr};

#if defined(BURROW_OS_FREEBSD)
static Slice route_announce_message_sockaddr(void *self, Alloc *a, Error *err) {
    (void)self;
    (void)a;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return route_nil_sockaddrs();
}

static const SyscallRoutingMessageVT route_announce_message_vt = {
    &route_announce_message_desc, route_announce_message_sockaddr};
#endif

Slice syscall_parse_routing_sockaddr(Alloc *a, SyscallRoutingMessage msg, Error *err) {
    if (msg.vt == NULL || msg.data == NULL) {
        BURROW_OUT(err, BURROW_NO_ERROR);
        return route_nil_sockaddrs();
    }
    return msg.vt->sockaddr(msg.data, a, err);
}

/* Room for any one message, so that all of them can sit in one array. */
typedef union RouteAnyMessage {
    SyscallRouteMessage route;
    SyscallInterfaceMessage iface;
    SyscallInterfaceAddrMessage ifa;
    SyscallInterfaceMulticastAddrMessage ifma;
#if defined(BURROW_OS_FREEBSD)
    SyscallInterfaceAnnounceMessage announce;
#endif
} RouteAnyMessage;

/* The header of the message at b into h, as much of it as b has. Go reads
 * the whole header whatever b's length, and the rest of it is zero here. */
static void route_header(void *h, size_t size, const Byte *b, Int blen) {
    memset(h, 0, size);
    memcpy(h, b, blen < (Int)size ? (size_t)blen : size);
}

/* Data is b[off:msglen], or EINVAL where Go's slice would panic. */
static bool route_data(Slice *data, Byte *b, Int off, Int msglen) {
    if (off < 0 || off > msglen)
        return false;
    *data = (Slice){b + off, msglen - off, msglen - off, TYPE_BYTE};
    return true;
}

#if defined(ROUTE_FREEBSD32)
/* FreeBSD 10 and later's struct if_data, which is not what Go's IfData is on
 * 32-bit systems: hwassist grew to 64 bits. Go's ifMsghdr and ifData. */
typedef struct RouteIfData10 {
    uint8_t type, physical, addrlen, hdrlen, link_state, vhid, baudrate_pf, datalen;
    uint32_t mtu, metric, baudrate, ipackets, ierrors, opackets, oerrors, collisions;
    uint32_t ibytes, obytes, imcasts, omcasts, iqdrops, noproto;
    uint64_t hwassist;
#if defined(BURROW_ARCH_386)
    int32_t epoch;
#else
    int64_t epoch;
#endif
    SyscallTimeval lastchange;
} RouteIfData10;

typedef struct RouteIfMsghdr10 {
    uint16_t msglen;
    uint8_t version;
    uint8_t type;
    int32_t addrs;
    int32_t flags;
    uint16_t index;
    uint8_t pad[2];
    RouteIfData10 data;
} RouteIfMsghdr10;
#endif

/* Go's toRoutingMessage: the message at b, whose length is msglen, as one
 * of the message types in u, or a zero RoutingMessage for a type Go has none
 * for. false when its length is too short for its header. */
static bool route_to_message(Byte *b, Int msglen, uint8_t typ, RouteAnyMessage *u,
                             SyscallRoutingMessage *out) {
    *out = (SyscallRoutingMessage){NULL, NULL};
    switch (typ) {
    case SYSCALL_RTM_ADD:
    case SYSCALL_RTM_DELETE:
    case SYSCALL_RTM_CHANGE:
    case SYSCALL_RTM_GET:
    case SYSCALL_RTM_LOSING:
    case SYSCALL_RTM_REDIRECT:
    case SYSCALL_RTM_MISS:
    case SYSCALL_RTM_LOCK:
    case SYSCALL_RTM_RESOLVE: {
        SyscallRouteMessage *m = &u->route;
        route_header(&m->header, sizeof m->header, b, msglen);
#if defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS)
        Int off = SYSCALL_SIZEOF_RT_MSGHDR;
#else
        Int off = (Int)offsetof(SyscallRtMsghdr, rmx) + SYSCALL_SIZEOF_RT_METRICS;
#if defined(ROUTE_FREEBSD32)
        /* A 64-bit kernel's rt_metrics is twice the size. */
        if (route_conf_amd64())
            off += SYSCALL_SIZEOF_RT_METRICS;
#endif
        off = route_align(off);
#endif
        if (!route_data(&m->data, b, off, msglen))
            return false;
        *out = (SyscallRoutingMessage){&route_message_vt, m};
        return true;
    }
    case SYSCALL_RTM_IFINFO: {
        SyscallInterfaceMessage *m = &u->iface;
        route_header(&m->header, sizeof m->header, b, msglen);
#if defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS)
        Int off = SYSCALL_SIZEOF_IF_MSGHDR;
#else
#if defined(ROUTE_FREEBSD32)
        RouteIfMsghdr10 h10;
        route_header(&h10, sizeof h10, b, msglen);
        m->header.data.hwassist = (uint32_t)h10.data.hwassist;
        m->header.data.epoch = h10.data.epoch;
        m->header.data.lastchange = h10.data.lastchange;
#endif
        Int off = (Int)offsetof(SyscallIfMsghdr, data) + (Int)m->header.data.datalen;
#endif
        if (!route_data(&m->data, b, off, msglen))
            return false;
        *out = (SyscallRoutingMessage){&route_if_message_vt, m};
        return true;
    }
    case SYSCALL_RTM_NEWADDR:
    case SYSCALL_RTM_DELADDR: {
        SyscallInterfaceAddrMessage *m = &u->ifa;
        route_header(&m->header, sizeof m->header, b, msglen);
        if (!route_data(&m->data, b, SYSCALL_SIZEOF_IFA_MSGHDR, msglen))
            return false;
        *out = (SyscallRoutingMessage){&route_ifa_message_vt, m};
        return true;
    }
#if defined(BURROW_OS_FREEBSD)
    case SYSCALL_RTM_IFANNOUNCE: {
        SyscallInterfaceAnnounceMessage *m = &u->announce;
        route_header(&m->header, sizeof m->header, b, msglen);
        *out = (SyscallRoutingMessage){&route_announce_message_vt, m};
        return true;
    }
    case SYSCALL_RTM_NEWMADDR:
    case SYSCALL_RTM_DELMADDR: {
        SyscallInterfaceMulticastAddrMessage *m = &u->ifma;
        route_header(&m->header, sizeof m->header, b, msglen);
        if (!route_data(&m->data, b, SYSCALL_SIZEOF_IFMA_MSGHDR, msglen))
            return false;
        *out = (SyscallRoutingMessage){&route_ifma_message_vt, m};
        return true;
    }
#else
    case SYSCALL_RTM_NEWMADDR2:
    case SYSCALL_RTM_DELMADDR: {
        SyscallInterfaceMulticastAddrMessage *m = &u->ifma;
        route_header(&m->header, sizeof m->header, b, msglen);
        if (!route_data(&m->data, b, SYSCALL_SIZEOF_IFMA_MSGHDR2, msglen))
            return false;
        *out = (SyscallRoutingMessage){&route_ifma_message_vt, m};
        return true;
    }
#endif
    default:
        break;
    }
    return true;
}

/* The bytes behind a Slice of n messages: the n interfaces, then the n
 * messages they point at. */
static size_t route_messages_off(Int n) {
    size_t a = _Alignof(RouteAnyMessage);
    return ((size_t)n * sizeof(SyscallRoutingMessage) + a - 1) / a * a;
}

static size_t route_messages_size(Int n) {
    return route_messages_off(n) + (size_t)n * sizeof(RouteAnyMessage);
}

#define ROUTE_MESSAGES_ALIGN                                                           \
    (_Alignof(RouteAnyMessage) > _Alignof(SyscallRoutingMessage)                       \
         ? _Alignof(RouteAnyMessage)                                                   \
         : _Alignof(SyscallRoutingMessage))

void syscall_routing_message_free(Alloc *a, Slice msgs) {
    if (msgs.p != NULL)
        mem_free(a, msgs.p, route_messages_size(msgs.cap), ROUTE_MESSAGES_ALIGN);
}

/* Goes over the messages in b and, when ms is not NULL, fills in ms and us.
 * The count of messages there are, or -1 for EINVAL. */
static Int route_walk(Slice b, SyscallRoutingMessage *ms, RouteAnyMessage *us) {
    Byte *p = (Byte *)b.p;
    Int left = b.len, nmsgs = 0, nskips = 0, n = 0;
    while (left >= 4) {
        uint16_t msglen;
        memcpy(&msglen, p, 2);
        uint8_t version = p[2], typ = p[3];
        if (msglen == 0 || (Int)msglen > left)
            return -1;
        nmsgs++;
        if (version == SYSCALL_RTM_VERSION) {
            RouteAnyMessage tmp;
            SyscallRoutingMessage m;
            if (!route_to_message(p, (Int)msglen, typ, ms != NULL ? &us[n] : &tmp, &m))
                return -1;
            if (m.vt == NULL) {
                nskips++;
            } else {
                if (ms != NULL)
                    ms[n] = m;
                n++;
            }
        }
        p += msglen;
        left -= msglen;
    }
    /* A message of another version is in neither count. */
    if (nmsgs != n + nskips)
        return -1;
    return n;
}

Slice syscall_parse_routing_message(Alloc *a, Slice b, Error *err) {
    Slice nil = {NULL, 0, 0, &route_routing_message_desc};
    Int n = route_walk(b, NULL, NULL);
    if (n < 0) {
        BURROW_OUT(err, route_errno(SYSCALL_EINVAL));
        return nil;
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (n == 0)
        return nil;
    Byte *mem = (Byte *)mem_alloc(a, route_messages_size(n), ROUTE_MESSAGES_ALIGN);
    if (mem == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return nil;
    }
    SyscallRoutingMessage *ms = (SyscallRoutingMessage *)(void *)mem;
    RouteAnyMessage *us = (RouteAnyMessage *)(void *)(mem + route_messages_off(n));
    (void)route_walk(b, ms, us);
    return (Slice){ms, n, n, &route_routing_message_desc};
}

/* ------------------------------------------------------------------- RIB */

Slice syscall_route_rib(Alloc *a, Int facility, Int param, Error *err) {
    Slice nil = {NULL, 0, 0, TYPE_BYTE};
    int32_t mib[6] = {SYSCALL_CTL_NET,   SYSCALL_AF_ROUTE, 0, 0,
                      (int32_t)facility, (int32_t)param};
    Slice ms = {mib, 6, 6, TYPE_INT32};
    Uintptr n = 0;
    Error e = burrow__syscall_sysctl(ms, NULL, &n, NULL, 0);
    if (BURROW_FAILED(e) || n == 0) {
        BURROW_OUT(err, e);
        return nil;
    }
    Uintptr size = n;
    Byte *tab = (Byte *)mem_alloc(a, (size_t)size, 1);
    if (tab == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return nil;
    }
    e = burrow__syscall_sysctl(ms, tab, &n, NULL, 0);
    if (BURROW_FAILED(e)) {
        mem_free(a, tab, (size_t)size, 1);
        BURROW_OUT(err, e);
        return nil;
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return (Slice){tab, (Int)n, (Int)size, TYPE_BYTE};
}

#endif

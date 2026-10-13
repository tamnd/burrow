/* What TCP and UDP share: an IP, a port and a zone, their text, their
 * sockaddr, and the choice of family a socket for them is made with.
 *
 * Derived from Go's src/net/ipsock.go, ipsock_posix.go, the address parts of
 * tcpsock.go, tcpsock_posix.go, udpsock.go and udpsock_posix.go, and the zone
 * cache in interface.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "internal.h"

#include "burrow/error.h"
#include "burrow/mem.h"
#include "burrow/net.h"
#include "burrow/net/netip.h"
#include "burrow/platform.h"

#include <stdint.h>
#include <string.h>

static Byte *ip_put(Byte *p, const void *s, Int n) {
    if (n > 0)
        memcpy(p, s, (size_t)n);
    return p + n;
}

/* The decimal text of v, into buf, which has room for any int64. */
static Int ip_itoa(Byte buf[24], int64_t v) {
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

/* ------------------------------------------------------------------ text */

static const char ip_hex[] = "0123456789abcdef";

Str burrow__net_inet_addr_string(const burrow__NetInetAddr *a, Alloc *al) {
    if (a == NULL) {
        Byte *p = (Byte *)mem_alloc_nozero(al, 5, 1);
        if (p == NULL)
            return BURROW_STR_EMPTY;
        ip_put(p, "<nil>", 5);
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
    Int portlen = ip_itoa(portbuf, (int64_t)a->port);

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
            *p++ = (Byte)ip_hex[b[i] >> 4];
            *p++ = (Byte)ip_hex[b[i] & 0x0f];
        }
    } else {
        p = ip_put(p, ipt.p, iplen);
    }
    if (a->zone.len > 0) {
        *p++ = '%';
        p = ip_put(p, a->zone.p, a->zone.len);
    }
    if (colon)
        *p++ = ']';
    *p++ = ':';
    ip_put(p, portbuf, portlen);
    return str_from_bytes(out, n);
}

/* ------------------------------------------------------------ sockaddrs */

/* An address burrow__net_inet_addr_new made, with the bytes its ip and zone
 * point at after it, and the size to give back. */
typedef struct IpAddrBox {
    burrow__NetInetAddr a;
    size_t size;
} IpAddrBox;

burrow__NetInetAddr *burrow__net_inet_addr_new(Alloc *a, const Byte *ip, Int iplen,
                                               Int port, Str zone) {
    size_t size = sizeof(IpAddrBox) + (size_t)iplen + (size_t)zone.len;
    IpAddrBox *b = (IpAddrBox *)mem_alloc_nozero(a, size, _Alignof(IpAddrBox));
    if (b == NULL)
        return NULL;
    b->size = size;
    Byte *p = (Byte *)(b + 1);
    if (iplen > 0)
        memcpy(p, ip, (size_t)iplen);
    b->a.ip = iplen > 0 ? slice_from(p, iplen, iplen, TYPE_BYTE) : (NetIP){0};
    if (zone.len > 0)
        memcpy(p + iplen, zone.p, (size_t)zone.len);
    b->a.zone = str_from_bytes(p + iplen, zone.len);
    b->a.port = port;
    return &b->a;
}

void burrow__net_inet_addr_free(Alloc *a, burrow__NetInetAddr *addr) {
    if (addr == NULL)
        return;
    IpAddrBox *b = (IpAddrBox *)(void *)addr;
    mem_free(a, b, b->size, _Alignof(IpAddrBox));
}

NetipAddrPort burrow__net_inet_addr_port(const burrow__NetInetAddr *a) {
    if (a == NULL)
        return (NetipAddrPort){0};
    bool ok = false;
    NetipAddr na = netip_addr_from_slice(a->ip, &ok);
    na = netip_addr_with_zone(na, a->zone);
    return netip_addr_port_from(na, (uint16_t)a->port);
}

burrow__NetInetAddr *burrow__net_inet_from_addr_port(Alloc *a, NetipAddrPort ap) {
    NetipAddr ip = netip_addr_port_addr(ap);
    Byte bytes[16] = {0};
    Int iplen = 0;
    if (netip_addr_is4(ip)) {
        NetipAddrAs4Ret b4 = netip_addr_as4(ip);
        memcpy(bytes, b4.a, 4);
        iplen = 4;
    } else if (netip_addr_is_valid(ip)) {
        NetipAddrAs16Ret b16 = netip_addr_as16(ip);
        memcpy(bytes, b16.a, 16);
        iplen = 16;
    }
    return burrow__net_inet_addr_new(a, bytes, iplen, (Int)netip_addr_port_port(ap),
                                     netip_addr_zone(ip));
}

bool burrow__net_inet_from_sockaddr(const PalSockAddr *sa, NetIP *ip, Int *port,
                                    Str *zone, burrow__NetInetBytes *b) {
    *b = (burrow__NetInetBytes){0};
    *ip = (NetIP){0};
    *zone = BURROW_STR_EMPTY;
    *port = 0;
    if (sa->family == PAL_AF_INET) {
        memcpy(b->ip, sa->addr, 4);
        *ip = slice_from(b->ip, 4, 4, TYPE_BYTE);
    } else if (sa->family == PAL_AF_INET6) {
        memcpy(b->ip, sa->addr, 16);
        *ip = slice_from(b->ip, 16, 16, TYPE_BYTE);
        if (sa->scope_id != 0) {
            Byte buf[24];
            Int n = ip_itoa(buf, (int64_t)sa->scope_id);
            memcpy(b->zone, buf, (size_t)n);
            *zone = str_from_bytes(b->zone, n);
        }
    } else {
        return false;
    }
    *port = (Int)sa->port;
    return true;
}

bool burrow__net_ip_to16(NetIP ip, Byte out[16]) {
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

static bool ip_is_v4mapped(const Byte b[16]) {
    static const Byte prefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    return memcmp(b, prefix, 12) == 0;
}

/* dtoi, which reads the digits at the start of s and stops at the first
 * thing that is not one, or at 0xFFFFFF. */
static int64_t ip_dtoi(Str s) {
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
static uint32_t ip_zone_index(Str zone) {
    if (zone.len == 0)
        return 0;
    return (uint32_t)ip_dtoi(zone);
}

static Error ip_addr_error(const char *msg, NetIP ip) {
    NetAddrError e = {str_from_cstr(msg), net_ip_string(ip, error_allocator())};
    return net_addr_error_as_error(&e, error_allocator());
}

Error burrow__net_ip_sockaddr(int32_t family, NetIP ip, Int port, Str zone,
                              PalSockAddr *out) {
    *out = (PalSockAddr){0};
    if (family == PAL_AF_INET) {
        Byte b[16] = {0};
        if (ip.len != 0) {
            if (!burrow__net_ip_to16(ip, b) || !ip_is_v4mapped(b))
                return ip_addr_error("non-IPv4 address", ip);
        }
        out->family = PAL_AF_INET;
        memcpy(out->addr, b + 12, 4);
    } else if (family == PAL_AF_INET6) {
        Byte b[16] = {0};
        if (ip.len != 0 && !burrow__net_ip_to16(ip, b))
            return ip_addr_error("non-IPv6 address", ip);
        /* IPv4zero means every address, and so does IPv6zero. */
        static const Byte v4zero[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
        if (memcmp(b, v4zero, 16) == 0)
            memset(b, 0, 16);
        out->family = PAL_AF_INET6;
        memcpy(out->addr, b, 16);
        out->scope_id = ip_zone_index(zone);
    } else {
        return ip_addr_error("invalid address family", ip);
    }
    if (port < 0 || port > 0xFFFF)
        return burrow__net_err_sockaddr_einval;
    out->port = (uint16_t)port;
    return BURROW_NO_ERROR;
}

static Error ip_inet_sockaddr(const void *addr, int32_t family, PalSockAddr *out) {
    const burrow__NetInetAddr *a = (const burrow__NetInetAddr *)addr;
    return burrow__net_ip_sockaddr(family, a->ip, a->port, a->zone, out);
}

/* A multicast group's address as a listener binds it: the unspecified
 * address of the socket's family, with the group's port and zone. */
static Error ip_group_sockaddr(const void *addr, int32_t family, PalSockAddr *out) {
    const burrow__NetInetAddr *a = (const burrow__NetInetAddr *)addr;
    return burrow__net_ip_sockaddr(family, (NetIP){0}, a->port, a->zone, out);
}

/* TCPAddr.String and UDPAddr.String, which are the same text. */
static Str ip_inet_text(const void *addr, int32_t family, Alloc *a) {
    (void)family;
    return burrow__net_inet_addr_string((const burrow__NetInetAddr *)addr, a);
}

/* The text of the address ip_group_sockaddr binds: IPv4zero or
 * IPv6unspecified, with the group's port and zone. */
static Str ip_group_text(const void *addr, int32_t family, Alloc *a) {
    static const Byte v4zero[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    static const Byte v6zero[16] = {0};
    burrow__NetInetAddr g = *(const burrow__NetInetAddr *)addr;
    g.ip = slice_from((void *)(uintptr_t)(family == PAL_AF_INET ? v4zero : v6zero), 16,
                      16, TYPE_BYTE);
    return burrow__net_inet_addr_string(&g, a);
}

/* TCPAddr.family and UDPAddr.family. */
static int32_t ip_family(const burrow__NetInetAddr *a) {
    if (a == NULL || a->ip.len <= 4)
        return PAL_AF_INET;
    Byte b[16];
    if (burrow__net_ip_to16(a->ip, b) && ip_is_v4mapped(b))
        return PAL_AF_INET;
    return PAL_AF_INET6;
}

/* isWildcard. */
static bool ip_is_wildcard(const burrow__NetInetAddr *a) {
    return a == NULL || a->ip.p == NULL || net_ip_is_unspecified(a->ip);
}

/* ---------------------------------------------------------- the socket */

/* favoriteAddrFamily, for TCP and UDP. */
static int32_t ip_favorite_family(Str net, const burrow__NetInetAddr *laddr,
                                  const burrow__NetInetAddr *raddr, bool listen,
                                  bool *ipv6only) {
    *ipv6only = false;
    Byte last = net.p[net.len - 1];
    if (last == '4')
        return PAL_AF_INET;
    if (last == '6') {
        *ipv6only = true;
        return PAL_AF_INET6;
    }
    if (listen && ip_is_wildcard(laddr)) {
        if (burrow__net_supports_ipv4map() || !burrow__net_supports_ipv4())
            return PAL_AF_INET6;
        if (laddr == NULL)
            return PAL_AF_INET;
        return ip_family(laddr);
    }
    if ((laddr == NULL || ip_family(laddr) == PAL_AF_INET) &&
        (raddr == NULL || ip_family(raddr) == PAL_AF_INET))
        return PAL_AF_INET;
    return PAL_AF_INET6;
}

Error burrow__net_internet_socket(burrow__NetFD *fd, const burrow__NetSockCtl *ctl,
                                  Str net, const burrow__NetInetAddr *laddr,
                                  const burrow__NetInetAddr *raddr, int32_t sotype,
                                  int32_t proto, bool listen) {
#if defined(BURROW_OS_AIX) || defined(BURROW_OS_FREEBSD) ||                            \
    defined(BURROW_OS_OPENBSD) || defined(BURROW_OS_WINDOWS)
    /* These systems will not connect to the unspecified address, which means
     * this machine everywhere else, so Go dials loopback instead. */
    static const Byte loop4[4] = {127, 0, 0, 1};
    static const Byte loop6[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    burrow__NetInetAddr local;
    if (!listen && raddr != NULL && ip_is_wildcard(raddr)) {
        bool six = net.p[net.len - 1] == '6';
        local.ip = six ? slice_from((void *)(uintptr_t)loop6, 16, 16, TYPE_BYTE)
                       : slice_from((void *)(uintptr_t)loop4, 4, 4, TYPE_BYTE);
        local.port = raddr->port;
        local.zone = raddr->zone;
        raddr = &local;
    }
#endif
    bool ipv6only = false;
    int32_t family = ip_favorite_family(net, laddr, raddr, listen, &ipv6only);
    /* listenDatagram binds a multicast group as the unspecified address. */
    bool group = sotype == PAL_SOCK_DGRAM && laddr != NULL && raddr == NULL &&
                 laddr->ip.p != NULL && net_ip_is_multicast(laddr->ip);
    return burrow__netfd_socket(fd, ctl, net, family, sotype, proto, ipv6only, laddr,
                                raddr, group,
                                group ? ip_group_sockaddr : ip_inet_sockaddr,
                                group ? ip_group_text : ip_inet_text);
}

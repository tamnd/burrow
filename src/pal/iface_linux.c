/* The network interfaces on Linux, from a netlink dump.
 *
 * This is Go's interface_linux.go with syscall.NetlinkRIB under it: a
 * NETLINK_ROUTE socket, one RTM_GETLINK or RTM_GETADDR dump request, and every
 * answer read until NLMSG_DONE before any of it is looked at. Go checks each
 * answer's sequence number and port id against its own as it goes, and so does
 * this, with the same EINVAL when they do not match.
 *
 * The kernel's structures are written out here rather than taken from
 * linux/rtnetlink.h, which is in the kernel headers package and not in libc,
 * and which a musl system often does not have. They are the kernel's ABI and
 * do not change.
 *
 * The multicast groups are not here. Go reads them from /proc/net/igmp and
 * /proc/net/igmp6, which are files, and net reads them the same way.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#if !defined(_WIN32)
#define _DEFAULT_SOURCE 1
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif
#endif

#include "burrow/platform.h"

#if defined(BURROW_OS_LINUX)

#include "burrow/pal.h"

#include "internal.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

/* linux/netlink.h and linux/rtnetlink.h. */
enum {
    NL_ROUTE = 0,
    NL_F_REQUEST = 0x1,
    NL_F_DUMP = 0x300,
    NL_MSG_ERROR = 2,
    NL_MSG_DONE = 3,
    NL_RTM_NEWLINK = 16,
    NL_RTM_GETLINK = 18,
    NL_RTM_NEWADDR = 20,
    NL_RTM_GETADDR = 22,
    NL_IFLA_ADDRESS = 1,
    NL_IFLA_IFNAME = 3,
    NL_IFLA_MTU = 4,
    NL_IFA_ADDRESS = 1,
    NL_IFA_LOCAL = 2
};

/* linux/if.h. */
enum {
    NL_IFF_UP = 0x1,
    NL_IFF_BROADCAST = 0x2,
    NL_IFF_LOOPBACK = 0x8,
    NL_IFF_POINTOPOINT = 0x10,
    NL_IFF_RUNNING = 0x40,
    NL_IFF_MULTICAST = 0x1000
};

/* linux/if_arp.h, the tunnels whose IFLA_ADDRESS is an IP address. */
enum {
    NL_ARPHRD_TUNNEL = 768,
    NL_ARPHRD_TUNNEL6 = 769,
    NL_ARPHRD_SIT = 776,
    NL_ARPHRD_IPGRE = 778,
    NL_ARPHRD_IP6GRE = 823
};

typedef struct NlMsgHdr {
    uint32_t len;
    uint16_t type;
    uint16_t flags;
    uint32_t seq;
    uint32_t pid;
} NlMsgHdr;

typedef struct NlSockAddr {
    sa_family_t family;
    unsigned short pad;
    uint32_t pid;
    uint32_t groups;
} NlSockAddr;

typedef struct NlIfInfoMsg {
    uint8_t family;
    uint8_t pad;
    uint16_t type;
    int32_t index;
    uint32_t flags;
    uint32_t change;
} NlIfInfoMsg;

typedef struct NlIfAddrMsg {
    uint8_t family;
    uint8_t prefixlen;
    uint8_t flags;
    uint8_t scope;
    uint32_t index;
} NlIfAddrMsg;

typedef struct NlRtAttr {
    uint16_t len;
    uint16_t type;
} NlRtAttr;

#define NL_HDRLEN ((int64_t)sizeof(NlMsgHdr))
#define NL_ALIGN(n) (((n) + 3) & ~(int64_t)3)

/* What Go's pageBufPool hands NetlinkRIB is a page. This is more, which costs
 * nothing and means a kernel that fills bigger answers is not cut short. */
#define NL_RECV_BUF (32 * 1024)

static void nl_fail(PalIfError *err, const char *call, PalErrno e) {
    if (err != NULL) {
        err->call = call;
        err->err = e;
    }
}

/* Whether b holds whole messages, which is ParseNetlinkMessage's check. */
static bool nl_well_formed(const uint8_t *b, int64_t n) {
    while (n >= NL_HDRLEN) {
        NlMsgHdr h;
        memcpy(&h, b, sizeof h);
        int64_t l = NL_ALIGN((int64_t)h.len);
        if ((int64_t)h.len < NL_HDRLEN || l > n)
            return false;
        b += l;
        n -= l;
    }
    return true;
}

/* Memory for the dump, straight from the system, since the PAL sits under the
 * allocators. Each block keeps its size in the 16 bytes before it. */
static uint8_t *nl_map(int64_t size) {
    void *m = mmap(NULL, (size_t)size + 16, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED)
        return NULL;
    memcpy(m, &size, sizeof size);
    return (uint8_t *)m + 16;
}

static void nl_unmap(uint8_t *p) {
    if (p == NULL)
        return;
    int64_t size;
    memcpy(&size, p - 16, sizeof size);
    munmap(p - 16, (size_t)size + 16);
}

/* NetlinkRIB: the whole dump for proto, in a buffer from nl_map that the
 * caller gives to nl_unmap, or NULL with the reason in *e. */
static uint8_t *nl_rib(uint16_t proto, int64_t *len, PalErrno *e) {
    int s = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NL_ROUTE);
    if (s < 0) {
        *e = burrow__pal_errno(errno);
        return NULL;
    }
    uint8_t *tab = NULL;
    uint8_t *rb = nl_map(NL_RECV_BUF);
    int64_t n = 0;
    int64_t cap = 0;
    NlSockAddr sa;
    NlSockAddr lsa;
    socklen_t lsalen = sizeof lsa;
    struct {
        NlMsgHdr h;
        uint8_t family;
    } req;
    memset(&sa, 0, sizeof sa);
    memset(&lsa, 0, sizeof lsa);
    memset(&req, 0, sizeof req);
    sa.family = AF_NETLINK;
    if (rb == NULL) {
        *e = PAL_ENOMEM;
        goto fail;
    }
    if (bind(s, (struct sockaddr *)(void *)&sa, sizeof sa) != 0)
        goto fail_errno;
    req.h.len = (uint32_t)NL_HDRLEN + 1;
    req.h.type = proto;
    req.h.flags = NL_F_DUMP | NL_F_REQUEST;
    req.h.seq = 1;
    req.family = AF_UNSPEC;
    for (;;) {
        if (sendto(s, &req, (size_t)NL_HDRLEN + 1, 0, (struct sockaddr *)(void *)&sa,
                   sizeof sa) >= 0)
            break;
        if (errno != EINTR)
            goto fail_errno;
    }
    if (getsockname(s, (struct sockaddr *)(void *)&lsa, &lsalen) != 0)
        goto fail_errno;
    if (lsa.family != AF_NETLINK) {
        *e = PAL_EINVAL;
        goto fail;
    }
    for (;;) {
        ssize_t nr = recvfrom(s, rb, NL_RECV_BUF, 0, NULL, NULL);
        if (nr < 0) {
            if (errno == EINTR)
                continue;
            goto fail_errno;
        }
        if ((int64_t)nr < NL_HDRLEN || !nl_well_formed(rb, (int64_t)nr)) {
            *e = PAL_EINVAL;
            goto fail;
        }
        if (n + (int64_t)nr > cap) {
            int64_t ncap = cap == 0 ? 2 * NL_RECV_BUF : 2 * cap;
            while (ncap < n + (int64_t)nr)
                ncap *= 2;
            uint8_t *nt = nl_map(ncap);
            if (nt == NULL) {
                *e = PAL_ENOMEM;
                goto fail;
            }
            if (n > 0)
                memcpy(nt, tab, (size_t)n);
            nl_unmap(tab);
            tab = nt;
            cap = ncap;
        }
        memcpy(tab + n, rb, (size_t)nr);
        n += (int64_t)nr;
        bool done = false;
        const uint8_t *b = rb;
        int64_t left = (int64_t)nr;
        while (left >= NL_HDRLEN) {
            NlMsgHdr h;
            memcpy(&h, b, sizeof h);
            if (h.seq != 1 || h.pid != lsa.pid || h.type == NL_MSG_ERROR) {
                *e = PAL_EINVAL;
                goto fail;
            }
            if (h.type == NL_MSG_DONE) {
                done = true;
                break;
            }
            int64_t l = NL_ALIGN((int64_t)h.len);
            b += l;
            left -= l;
        }
        if (done)
            break;
    }
    nl_unmap(rb);
    close(s);
    *len = n;
    return tab;
fail_errno:
    *e = burrow__pal_errno(errno);
fail:
    nl_unmap(rb);
    nl_unmap(tab);
    close(s);
    return NULL;
}

/* The next message in the dump, which nl_rib has already checked is whole. */
static bool nl_next(const uint8_t **b, int64_t *n, NlMsgHdr *h, const uint8_t **data,
                    int64_t *dlen) {
    if (*n < NL_HDRLEN)
        return false;
    memcpy(h, *b, sizeof *h);
    *data = *b + NL_HDRLEN;
    *dlen = (int64_t)h->len - NL_HDRLEN;
    int64_t l = NL_ALIGN((int64_t)h->len);
    *b += l;
    *n -= l;
    return true;
}

/* ParseNetlinkRouteAttr's walk over the attributes after the fixed header: the
 * next one into a and its value into v and vlen, false at the end, and *bad
 * set when the attribute does not fit, which Go calls EINVAL. */
static bool nl_attr(const uint8_t **b, int64_t *n, NlRtAttr *a, const uint8_t **v,
                    int64_t *vlen, bool *bad) {
    if (*n < (int64_t)sizeof *a)
        return false;
    memcpy(a, *b, sizeof *a);
    if ((int64_t)a->len < (int64_t)sizeof *a || (int64_t)a->len > *n) {
        *bad = true;
        return false;
    }
    *v = *b + sizeof *a;
    *vlen = (int64_t)a->len - (int64_t)sizeof *a;
    int64_t l = NL_ALIGN((int64_t)a->len);
    if (l > *n)
        l = *n;
    *b += l;
    *n -= l;
    return true;
}

static uint32_t nl_flags(uint32_t raw) {
    uint32_t f = 0;
    if ((raw & NL_IFF_UP) != 0)
        f |= PAL_IFF_UP;
    if ((raw & NL_IFF_RUNNING) != 0)
        f |= PAL_IFF_RUNNING;
    if ((raw & NL_IFF_BROADCAST) != 0)
        f |= PAL_IFF_BROADCAST;
    if ((raw & NL_IFF_LOOPBACK) != 0)
        f |= PAL_IFF_LOOPBACK;
    if ((raw & NL_IFF_POINTOPOINT) != 0)
        f |= PAL_IFF_POINTTOPOINT;
    if ((raw & NL_IFF_MULTICAST) != 0)
        f |= PAL_IFF_MULTICAST;
    return f;
}

/* newLink. False when the attributes do not parse. */
static bool nl_link(const NlIfInfoMsg *ifim, const uint8_t *b, int64_t n,
                    PalInterface *ifi) {
    memset(ifi, 0, sizeof *ifi);
    ifi->index = ifim->index;
    ifi->flags = nl_flags(ifim->flags);
    NlRtAttr a;
    const uint8_t *v = NULL;
    int64_t vlen = 0;
    bool bad = false;
    while (nl_attr(&b, &n, &a, &v, &vlen, &bad)) {
        switch (a.type) {
        case NL_IFLA_ADDRESS: {
            /* An IP tunnel's address is one of its ends, which Go never hands
             * back as a hardware address. */
            if (vlen == 4 &&
                (ifim->type == NL_ARPHRD_TUNNEL || ifim->type == NL_ARPHRD_IPGRE ||
                 ifim->type == NL_ARPHRD_SIT))
                break;
            if (vlen == 16 &&
                (ifim->type == NL_ARPHRD_TUNNEL6 || ifim->type == NL_ARPHRD_IP6GRE))
                break;
            bool nonzero = false;
            for (int64_t i = 0; i < vlen; i++)
                nonzero = nonzero || v[i] != 0;
            if (nonzero) {
                int64_t m = vlen < (int64_t)sizeof ifi->hwaddr
                                ? vlen
                                : (int64_t)sizeof ifi->hwaddr;
                memcpy(ifi->hwaddr, v, (size_t)m);
                ifi->hwaddr_len = (int32_t)m;
            }
            break;
        }
        case NL_IFLA_IFNAME: {
            int64_t m = vlen > 0 ? vlen - 1 : 0;
            if (m > PAL_NAME_MAX)
                m = PAL_NAME_MAX;
            memcpy(ifi->name, v, (size_t)m);
            ifi->name[m] = 0;
            break;
        }
        case NL_IFLA_MTU:
            if (vlen >= 4) {
                uint32_t mtu;
                memcpy(&mtu, v, sizeof mtu);
                ifi->mtu = (int32_t)mtu;
            }
            break;
        default:
            break;
        }
    }
    return !bad;
}

int64_t pal_if_enumerate(int32_t index, PalInterface *out, int64_t cap,
                         PalIfError *err) {
    PalErrno e = PAL_OK;
    int64_t len = 0;
    uint8_t *tab = nl_rib(NL_RTM_GETLINK, &len, &e);
    if (tab == NULL) {
        nl_fail(err, "netlinkrib", e);
        return -1;
    }
    int64_t count = 0;
    const uint8_t *b = tab;
    NlMsgHdr h;
    const uint8_t *data = NULL;
    int64_t dlen = 0;
    while (nl_next(&b, &len, &h, &data, &dlen)) {
        if (h.type == NL_MSG_DONE)
            break;
        if (h.type != NL_RTM_NEWLINK)
            continue;
        NlIfInfoMsg ifim;
        if (dlen < (int64_t)sizeof ifim) {
            nl_unmap(tab);
            nl_fail(err, "parsenetlinkrouteattr", PAL_EINVAL);
            return -1;
        }
        memcpy(&ifim, data, sizeof ifim);
        if (index != 0 && index != ifim.index)
            continue;
        PalInterface ifi;
        if (!nl_link(&ifim, data + sizeof ifim, dlen - (int64_t)sizeof ifim, &ifi)) {
            nl_unmap(tab);
            nl_fail(err, "parsenetlinkrouteattr", PAL_EINVAL);
            return -1;
        }
        if (count < cap)
            out[count] = ifi;
        count++;
        if (index == ifim.index)
            break;
    }
    nl_unmap(tab);
    return count;
}

static void nl_mask(uint8_t *mask, int bits) {
    for (int i = 0; i < bits / 8; i++)
        mask[i] = 0xff;
    if (bits % 8 != 0)
        mask[bits / 8] = (uint8_t)(0xff << (8 - bits % 8));
}

/* newAddr. False when there is nothing Go would hand back. */
static bool nl_addr(const NlIfAddrMsg *ifam, const uint8_t *b, int64_t n,
                    PalIfAddr *out) {
    /* A point to point link has the far end as IFA_ADDRESS and its own as
     * IFA_LOCAL, and the one wanted is the local one. */
    bool ptp = false;
    {
        const uint8_t *p = b;
        int64_t left = n;
        NlRtAttr a;
        const uint8_t *v = NULL;
        int64_t vlen = 0;
        bool bad = false;
        while (nl_attr(&p, &left, &a, &v, &vlen, &bad)) {
            if (a.type == NL_IFA_LOCAL) {
                ptp = true;
                break;
            }
        }
    }
    NlRtAttr a;
    const uint8_t *v = NULL;
    int64_t vlen = 0;
    bool bad = false;
    while (nl_attr(&b, &n, &a, &v, &vlen, &bad)) {
        if (ptp && a.type == NL_IFA_ADDRESS)
            continue;
        int bits;
        if (ifam->family == AF_INET)
            bits = 32;
        else if (ifam->family == AF_INET6)
            bits = 128;
        else
            continue;
        memset(out, 0, sizeof *out);
        out->index = (int32_t)ifam->index;
        out->kind = PAL_IFA_NET;
        out->family = bits == 32 ? PAL_AF_INET : PAL_AF_INET6;
        int64_t m = bits / 8;
        if (vlen < m)
            m = vlen;
        memcpy(out->addr, v, (size_t)m);
        /* CIDRMask is nil past the end of the address. */
        if ((int)ifam->prefixlen <= bits) {
            out->mask_len = bits / 8;
            nl_mask(out->mask, (int)ifam->prefixlen);
        }
        return true;
    }
    return false;
}

/* The attributes of an RTM_NEWADDR, checked the way ParseNetlinkRouteAttr
 * checks them. */
static bool nl_attrs_ok(const uint8_t *b, int64_t n) {
    NlRtAttr a;
    const uint8_t *v = NULL;
    int64_t vlen = 0;
    bool bad = false;
    while (nl_attr(&b, &n, &a, &v, &vlen, &bad)) {
    }
    return !bad;
}

int64_t pal_if_addrs(int32_t index, PalIfAddr *out, int64_t cap, PalIfError *err) {
    PalErrno e = PAL_OK;
    int64_t len = 0;
    uint8_t *tab = nl_rib(NL_RTM_GETADDR, &len, &e);
    if (tab == NULL) {
        nl_fail(err, "netlinkrib", e);
        return -1;
    }
    int64_t count = 0;
    const uint8_t *b = tab;
    NlMsgHdr h;
    const uint8_t *data = NULL;
    int64_t dlen = 0;
    while (nl_next(&b, &len, &h, &data, &dlen)) {
        if (h.type == NL_MSG_DONE)
            break;
        if (h.type != NL_RTM_NEWADDR)
            continue;
        NlIfAddrMsg ifam;
        if (dlen < (int64_t)sizeof ifam) {
            nl_unmap(tab);
            nl_fail(err, "parsenetlinkrouteattr", PAL_EINVAL);
            return -1;
        }
        memcpy(&ifam, data, sizeof ifam);
        if (index != 0 && (uint32_t)index != ifam.index)
            continue;
        const uint8_t *ab = data + sizeof ifam;
        int64_t an = dlen - (int64_t)sizeof ifam;
        if (!nl_attrs_ok(ab, an)) {
            nl_unmap(tab);
            nl_fail(err, "parsenetlinkrouteattr", PAL_EINVAL);
            return -1;
        }
        PalIfAddr ifa;
        if (!nl_addr(&ifam, ab, an, &ifa))
            continue;
        if (count < cap)
            out[count] = ifa;
        count++;
    }
    nl_unmap(tab);
    return count;
}

int64_t pal_if_multicast_addrs(int32_t index, PalIfAddr *out, int64_t cap,
                               PalIfError *err) {
    (void)index;
    (void)out;
    (void)cap;
    (void)err;
    return 0;
}

#endif /* BURROW_OS_LINUX */

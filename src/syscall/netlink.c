/* Netlink and the Linux socket filter: NetlinkRIB, which asks the kernel for
 * its links, addresses or routes, the parsers for what it says, and the Lsf
 * functions Go keeps for compatibility.
 *
 * Derived from Go's src/syscall/netlink_linux.go and lsf_linux.go.
 * Go source: go1.27.1.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/platform.h"

#if defined(BURROW_OS_LINUX) || defined(BURROW_OS_COSMO) || defined(BURROW_OS_WASI)

#include "burrow/syscall.h"

#include "burrow/mem.h"
#include "burrow/slice.h"

#include "internal.h"

#include <string.h>

static const Type netlink_message_desc = {
    {(const Byte *)"NetlinkMessage", 14},
    {(const Byte *)"syscall", 7},
    KIND_STRUCT,
    (uint32_t)sizeof(SyscallNetlinkMessage),
    (uint16_t)_Alignof(SyscallNetlinkMessage),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x6e6c6d67U, /* "nlmg" */
    NULL,
};

static const Type netlink_route_attr_desc = {
    {(const Byte *)"NetlinkRouteAttr", 16},
    {(const Byte *)"syscall", 7},
    KIND_STRUCT,
    (uint32_t)sizeof(SyscallNetlinkRouteAttr),
    (uint16_t)_Alignof(SyscallNetlinkRouteAttr),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x6e6c7261U, /* "nlra" */
    NULL,
};

const Type *const TYPE_SYSCALL_NETLINK_MESSAGE = &netlink_message_desc;
const Type *const TYPE_SYSCALL_NETLINK_ROUTE_ATTR = &netlink_route_attr_desc;

static Int nlm_align(Int n) {
    return (n + SYSCALL_NLMSG_ALIGNTO - 1) & ~(Int)(SYSCALL_NLMSG_ALIGNTO - 1);
}

static Int rta_align(Int n) {
    return (n + SYSCALL_RTA_ALIGNTO - 1) & ~(Int)(SYSCALL_RTA_ALIGNTO - 1);
}

static Error netlink_einval(void) {
    return burrow__syscall_errno_err(SYSCALL_EINVAL);
}

/* ---------------------------------------------------------------- parsing */

/* The header at the start of the n bytes at b, and how far the next message
 * is. */
static Error netlink_header(const Byte *b, Int n, SyscallNlMsghdr *h, Int *next) {
    memcpy(h, b, sizeof *h);
    Int l = nlm_align((Int)h->len);
    if ((Int)h->len < SYSCALL_NLMSG_HDRLEN || l > n)
        return netlink_einval();
    *next = l;
    return BURROW_NO_ERROR;
}

Slice syscall_parse_netlink_message(Alloc *a, Slice b, Error *err) {
    const Byte *p = (const Byte *)b.p;
    Slice nil = slice_nil(TYPE_SYSCALL_NETLINK_MESSAGE);
    Int count = 0;
    for (Int i = 0; b.len - i >= SYSCALL_NLMSG_HDRLEN; count++) {
        SyscallNlMsghdr h;
        Int next = 0;
        Error e = netlink_header(p + i, b.len - i, &h, &next);
        if (BURROW_FAILED(e)) {
            BURROW_OUT(err, e);
            return nil;
        }
        i += next;
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (count == 0)
        return nil;
    SyscallNetlinkMessage *msgs = (SyscallNetlinkMessage *)mem_alloc_array(
        a, (size_t)count, sizeof *msgs, _Alignof(SyscallNetlinkMessage));
    if (msgs == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return nil;
    }
    Int i = 0;
    for (Int k = 0; k < count; k++) {
        Int next = 0;
        (void)netlink_header(p + i, b.len - i, &msgs[k].header, &next);
        Int dlen = (Int)msgs[k].header.len - SYSCALL_NLMSG_HDRLEN;
        msgs[k].data = (Slice){(void *)(uintptr_t)(p + i + SYSCALL_NLMSG_HDRLEN), dlen,
                               b.len - i - SYSCALL_NLMSG_HDRLEN, TYPE_BYTE};
        i += next;
    }
    return (Slice){msgs, count, count, TYPE_SYSCALL_NETLINK_MESSAGE};
}

/* The attribute at the start of the n bytes at b, and how far the next one
 * is. */
static Error netlink_attr(const Byte *b, Int n, SyscallRtAttr *attr, Int *next) {
    memcpy(attr, b, sizeof *attr);
    if ((Int)attr->len < SYSCALL_SIZEOF_RT_ATTR || (Int)attr->len > n)
        return netlink_einval();
    *next = rta_align((Int)attr->len);
    return BURROW_NO_ERROR;
}

/* Go slices past the fixed header of the message without checking it is
 * there, and panics when it is not. Here that is EINVAL. */
Slice syscall_parse_netlink_route_attr(Alloc *a, SyscallNetlinkMessage *m, Error *err) {
    Slice nil = slice_nil(TYPE_SYSCALL_NETLINK_ROUTE_ATTR);
    Int skip;
    switch (m->header.type) {
    case SYSCALL_RTM_NEWLINK:
    case SYSCALL_RTM_DELLINK:
        skip = SYSCALL_SIZEOF_IF_INFOMSG;
        break;
    case SYSCALL_RTM_NEWADDR:
    case SYSCALL_RTM_DELADDR:
        skip = SYSCALL_SIZEOF_IF_ADDRMSG;
        break;
    case SYSCALL_RTM_NEWROUTE:
    case SYSCALL_RTM_DELROUTE:
        skip = SYSCALL_SIZEOF_RT_MSG;
        break;
    default:
        BURROW_OUT(err, netlink_einval());
        return nil;
    }
    if (m->data.len < skip) {
        BURROW_OUT(err, netlink_einval());
        return nil;
    }
    const Byte *p = (const Byte *)m->data.p + skip;
    Int n = m->data.len - skip;
    Int count = 0;
    /* The last attribute's padding may run past the end, which only ends the
     * loop, as in Go. */
    for (Int i = 0; n - i >= SYSCALL_SIZEOF_RT_ATTR; count++) {
        SyscallRtAttr attr;
        Int next = 0;
        Error e = netlink_attr(p + i, n - i, &attr, &next);
        if (BURROW_FAILED(e)) {
            BURROW_OUT(err, e);
            return nil;
        }
        i += next;
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (count == 0)
        return nil;
    SyscallNetlinkRouteAttr *attrs = (SyscallNetlinkRouteAttr *)mem_alloc_array(
        a, (size_t)count, sizeof *attrs, _Alignof(SyscallNetlinkRouteAttr));
    if (attrs == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return nil;
    }
    Int i = 0;
    for (Int k = 0; k < count; k++) {
        Int next = 0;
        (void)netlink_attr(p + i, n - i, &attrs[k].attr, &next);
        Int vlen = (Int)attrs[k].attr.len - SYSCALL_SIZEOF_RT_ATTR;
        attrs[k].value = (Slice){(void *)(uintptr_t)(p + i + SYSCALL_SIZEOF_RT_ATTR),
                                 vlen, n - i - SYSCALL_SIZEOF_RT_ATTR, TYPE_BYTE};
        i += next;
    }
    return (Slice){attrs, count, count, TYPE_SYSCALL_NETLINK_ROUTE_ATTR};
}

/* ------------------------------------------------------------ NetlinkRIB */

/* Appends the n bytes at p to *tab, which grows from a. */
static bool netlink_append(Alloc *a, Slice *tab, const Byte *p, Int n) {
    if (tab->len + n > tab->cap) {
        Int cap = tab->cap == 0 ? n : tab->cap;
        while (cap < tab->len + n)
            cap *= 2;
        Byte *b = (Byte *)mem_realloc(a, tab->p, (size_t)tab->cap, (size_t)cap, 1);
        if (b == NULL)
            return false;
        tab->p = b;
        tab->cap = cap;
    }
    memcpy((Byte *)tab->p + tab->len, p, (size_t)n);
    tab->len += n;
    return true;
}

/* Reads the replies to the request into *tab until the one that says the dump
 * is done. Each has to be for sequence 1 and the socket's own port. */
static Error netlink_read(Alloc *a, Int s, uint32_t pid, Slice rb, Slice *tab) {
    for (;;) {
        Error err = BURROW_NO_ERROR;
        Int nr = syscall_recvfrom(a, s, rb, 0, NULL, &err);
        if (BURROW_FAILED(err))
            return err;
        if (nr < SYSCALL_NLMSG_HDRLEN)
            return netlink_einval();
        if (!netlink_append(a, tab, (const Byte *)rb.p, nr))
            return burrow_err_out_of_memory;
        Slice msgs = syscall_parse_netlink_message(a, slice_sub(rb, 0, nr), &err);
        if (BURROW_FAILED(err))
            return err;
        const SyscallNetlinkMessage *m = (const SyscallNetlinkMessage *)msgs.p;
        Error res = BURROW_NO_ERROR;
        bool done = false;
        for (Int i = 0; i < msgs.len && !done; i++) {
            if (m[i].header.seq != 1 || m[i].header.pid != pid) {
                res = netlink_einval();
                done = true;
            } else if (m[i].header.type == SYSCALL_NLMSG_DONE) {
                done = true;
            } else if (m[i].header.type == SYSCALL_NLMSG_ERROR) {
                res = netlink_einval();
                done = true;
            }
        }
        if (msgs.p != NULL)
            mem_free(a, msgs.p, (size_t)msgs.cap * sizeof *m,
                     _Alignof(SyscallNetlinkMessage));
        if (done)
            return res;
    }
}

Slice syscall_netlink_rib(Alloc *a, Int proto, Int family, Error *err) {
    Slice tab = slice_nil(TYPE_BYTE);
    Error e = BURROW_NO_ERROR;
    Int s = syscall_socket(SYSCALL_AF_NETLINK, SYSCALL_SOCK_RAW | SYSCALL_SOCK_CLOEXEC,
                           SYSCALL_NETLINK_ROUTE, &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return tab;
    }
    SyscallSockaddrNetlink sa;
    memset(&sa, 0, sizeof sa);
    sa.family = SYSCALL_AF_NETLINK;
    SyscallSockaddr ssa = syscall_sockaddr_netlink_as_sockaddr(&sa);
    e = syscall_bind(s, ssa);

    /* Go's newNetlinkRouteRequest: the header and one rtgenmsg, written out
     * byte by byte in the machine's order. */
    Byte wb[SYSCALL_NLMSG_HDRLEN + SYSCALL_SIZEOF_RT_GENMSG];
    if (BURROW_OK(e)) {
        SyscallNlMsghdr h;
        memset(&h, 0, sizeof h);
        h.len = (uint32_t)sizeof wb;
        h.type = (uint16_t)proto;
        h.flags = SYSCALL_NLM_F_DUMP | SYSCALL_NLM_F_REQUEST;
        h.seq = 1;
        memset(wb, 0, sizeof wb);
        memcpy(wb, &h, sizeof h);
        wb[SYSCALL_NLMSG_HDRLEN] = (Byte)family;
        e = syscall_sendto(s, (Slice){wb, (Int)sizeof wb, (Int)sizeof wb, TYPE_BYTE}, 0,
                           ssa);
    }
    SyscallSockaddr lsa = {NULL, NULL};
    if (BURROW_OK(e))
        lsa = syscall_getsockname(a, s, &e);
    if (BURROW_OK(e) && lsa.vt->self_type != TYPE_SYSCALL_SOCKADDR_NETLINK)
        e = netlink_einval();
    if (BURROW_OK(e)) {
        uint32_t pid = ((const SyscallSockaddrNetlink *)lsa.data)->pid;
        Int page = syscall_getpagesize();
        Byte *rb = (Byte *)mem_alloc(a, (size_t)page, 1);
        if (rb == NULL) {
            e = burrow_err_out_of_memory;
        } else {
            e = netlink_read(a, s, pid, (Slice){rb, page, page, TYPE_BYTE}, &tab);
            mem_free(a, rb, (size_t)page, 1);
        }
    }
    syscall_sockaddr_free(a, lsa);
    (void)syscall_close(s);
    if (BURROW_FAILED(e) && tab.p != NULL) {
        mem_free(a, tab.p, (size_t)tab.cap, 1);
        tab = slice_nil(TYPE_BYTE);
    }
    BURROW_OUT(err, e);
    return tab;
}

/* ------------------------------------------------------------------- LSF */

SyscallSockFilter syscall_lsf_stmt(Int code, Int k) {
    SyscallSockFilter f;
    memset(&f, 0, sizeof f);
    f.code = (uint16_t)code;
    f.k = (uint32_t)k;
    return f;
}

SyscallSockFilter syscall_lsf_jump(Int code, Int k, Int jt, Int jf) {
    SyscallSockFilter f;
    f.code = (uint16_t)code;
    f.jt = (uint8_t)jt;
    f.jf = (uint8_t)jf;
    f.k = (uint32_t)k;
    return f;
}

Int syscall_lsf_socket(Int ifindex, Int proto, Error *err) {
    Error e = BURROW_NO_ERROR;
    Int s = syscall_socket(SYSCALL_AF_PACKET, SYSCALL_SOCK_RAW, proto, &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return 0;
    }
    SyscallSockaddrLinklayer lsall;
    memset(&lsall, 0, sizeof lsall);
    /* The protocol goes in network order, as the system wants it. */
    uint8_t *p = (uint8_t *)&lsall.protocol;
    p[0] = (uint8_t)(proto >> 8);
    p[1] = (uint8_t)proto;
    lsall.ifindex = ifindex;
    e = syscall_bind(s, syscall_sockaddr_linklayer_as_sockaddr(&lsall));
    if (BURROW_FAILED(e)) {
        (void)syscall_close(s);
        BURROW_OUT(err, e);
        return 0;
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return s;
}

/* Go's iflags, the start of a struct ifreq: the name and the flags. Go hands
 * the kernel only these 18 bytes and the kernel copies a whole ifreq both
 * ways, so the room for the rest of one is here too. */
typedef struct NetlinkIflags {
    uint8_t name[SYSCALL_IFNAMSIZ];
    uint16_t flags;
    uint8_t rest[32];
} NetlinkIflags;

Error syscall_set_lsf_promisc(Str name, bool m) {
    Error e = BURROW_NO_ERROR;
    Int s = syscall_socket(SYSCALL_AF_INET, SYSCALL_SOCK_DGRAM | SYSCALL_SOCK_CLOEXEC,
                           0, &e);
    if (BURROW_FAILED(e))
        return e;
    NetlinkIflags ifl;
    memset(&ifl, 0, sizeof ifl);
    if (name.len > 0)
        memcpy(ifl.name, name.p,
               (size_t)(name.len < SYSCALL_IFNAMSIZ ? name.len : SYSCALL_IFNAMSIZ));
    SyscallErrno ep = 0;
    (void)syscall_syscall(SYSCALL_SYS_IOCTL, (Uintptr)s, SYSCALL_SIOCGIFFLAGS,
                          (Uintptr)&ifl, NULL, &ep);
    if (ep == 0) {
        if (m)
            ifl.flags |= (uint16_t)SYSCALL_IFF_PROMISC;
        else
            ifl.flags &= (uint16_t)~(uint16_t)SYSCALL_IFF_PROMISC;
        (void)syscall_syscall(SYSCALL_SYS_IOCTL, (Uintptr)s, SYSCALL_SIOCSIFFLAGS,
                              (Uintptr)&ifl, NULL, &ep);
    }
    (void)syscall_close(s);
    return burrow__syscall_errno_err(ep);
}

/* Go takes the address of the program's first instruction, and panics on an
 * empty one. Here an empty program goes as a NULL filter, which the kernel
 * refuses with EINVAL. */
Error syscall_attach_lsf(Int fd, Slice i) {
    SyscallSockFprog p;
    memset(&p, 0, sizeof p);
    p.len = (uint16_t)i.len;
    p.filter = (SyscallSockFilter *)i.p;
    return burrow__syscall_setsockopt(fd, SYSCALL_SOL_SOCKET, SYSCALL_SO_ATTACH_FILTER,
                                      &p, sizeof p);
}

Error syscall_detach_lsf(Int fd) {
    Int dummy = 0;
    return burrow__syscall_setsockopt(fd, SYSCALL_SOL_SOCKET, SYSCALL_SO_DETACH_FILTER,
                                      &dummy, sizeof dummy);
}

#endif

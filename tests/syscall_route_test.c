/* The routing messages and the packet filter of macOS and FreeBSD: RouteRIB,
 * ParseRoutingMessage, ParseRoutingSockaddr and the Bpf calls.
 *
 * Go has no tests for these. These are burrow's own: they read what the
 * kernel says about lo0 and check it against the C library, parse messages
 * built by hand, including broken ones, and use /dev/bpf where the test is
 * allowed to open it.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/mem/arena.h"
#include "burrow/syscall.h"

#if defined(BURROW_OS_DARWIN) || defined(BURROW_OS_FREEBSD)

#include <stdio.h>
#include <string.h>

#include <net/if.h>

#define ARENA_BEGIN                                                                    \
    Arena ar;                                                                          \
    arena_init(&ar, NULL, 0);                                                          \
    Alloc *a = arena_allocator(&ar)
#define ARENA_END arena_free(&ar)

/* The Errno err holds, or 0 if it holds none. */
static SyscallErrno errno_of(Error err) {
    const SyscallErrno *e = errors_as(err, TYPE_SYSCALL_ERRNO);
    return e == NULL ? 0 : *e;
}

static const Type *type_of(SyscallSockaddr sa) {
    return sa.vt == NULL ? NULL : sa.vt->self_type;
}

/* --------------------------------------------------------------- RouteRIB */

static void TestRouteRIBInterfaces(TestingT *t) {
    ARENA_BEGIN;
    Error err = BURROW_NO_ERROR;
    Slice tab = syscall_route_rib(a, SYSCALL_NET_RT_IFLIST, 0, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "RouteRIB: %v", err);
    Slice msgs = syscall_parse_routing_message(a, tab, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "ParseRoutingMessage: %v", err);
    CHECK(msgs.len > 0);

    unsigned lo = if_nametoindex("lo0");
    CHECK(lo != 0);
    bool saw_link = false, saw_addr = false;
    SyscallRoutingMessage *ms = (SyscallRoutingMessage *)msgs.p;
    for (Int i = 0; i < msgs.len; i++) {
        Slice sas = syscall_parse_routing_sockaddr(a, ms[i], &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "ParseRoutingSockaddr of message %v: %v", i, err);
        const Type *mt = ms[i].vt->self_type;
        if (mt == TYPE_SYSCALL_INTERFACE_MESSAGE) {
            SyscallInterfaceMessage *m = (SyscallInterfaceMessage *)ms[i].data;
            CHECK(m->header.type == SYSCALL_RTM_IFINFO);
            if (sas.len == 0)
                continue;
            SyscallSockaddr ifp = ((SyscallSockaddr *)sas.p)[SYSCALL_RTAX_IFP];
            CHECK(type_of(ifp) == TYPE_SYSCALL_SOCKADDR_DATALINK);
            SyscallSockaddrDatalink *dl = (SyscallSockaddrDatalink *)ifp.data;
            if (dl->nlen == 3 && memcmp(dl->data, "lo0", 3) == 0) {
                saw_link = true;
                CHECK(dl->index == lo);
                CHECK(m->header.index == lo);
                CHECK(dl->family == SYSCALL_AF_LINK);
            }
        } else if (mt == TYPE_SYSCALL_INTERFACE_ADDR_MESSAGE) {
            SyscallInterfaceAddrMessage *m = (SyscallInterfaceAddrMessage *)ms[i].data;
            CHECK(sas.len == SYSCALL_RTAX_MAX);
            SyscallSockaddr ifa = ((SyscallSockaddr *)sas.p)[SYSCALL_RTAX_IFA];
            if (m->header.index != lo || type_of(ifa) != TYPE_SYSCALL_SOCKADDR_INET4)
                continue;
            SyscallSockaddrInet4 *in = (SyscallSockaddrInet4 *)ifa.data;
            const uint8_t want[4] = {127, 0, 0, 1};
            if (memcmp(in->addr, want, 4) == 0) {
                saw_addr = true;
                SyscallSockaddr mask = ((SyscallSockaddr *)sas.p)[SYSCALL_RTAX_NETMASK];
                CHECK(type_of(mask) == TYPE_SYSCALL_SOCKADDR_INET4);
                if (type_of(mask) == TYPE_SYSCALL_SOCKADDR_INET4)
                    CHECK(((SyscallSockaddrInet4 *)mask.data)->addr[0] == 255);
            }
        }
        syscall_routing_sockaddr_free(a, sas);
    }
    if (!saw_link)
        testing_t_errorf_v(t, "no InterfaceMessage for lo0 in %v messages", msgs.len);
    if (!saw_addr)
        testing_t_errorf_v(t, "no InterfaceAddrMessage with 127.0.0.1 on lo0");
    syscall_routing_message_free(a, msgs);
    ARENA_END;
}

static void TestRouteRIBDump(TestingT *t) {
    ARENA_BEGIN;
    Error err = BURROW_NO_ERROR;
    Slice tab = syscall_route_rib(a, SYSCALL_NET_RT_DUMP, 0, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "RouteRIB: %v", err);
    Slice msgs = syscall_parse_routing_message(a, tab, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "ParseRoutingMessage: %v", err);
    Int routes = 0;
    SyscallRoutingMessage *ms = (SyscallRoutingMessage *)msgs.p;
    for (Int i = 0; i < msgs.len; i++) {
        if (ms[i].vt->self_type != TYPE_SYSCALL_ROUTE_MESSAGE)
            continue;
        routes++;
        SyscallRouteMessage *m = (SyscallRouteMessage *)ms[i].data;
        CHECK(m->header.version == SYSCALL_RTM_VERSION);
        Slice sas = syscall_parse_routing_sockaddr(a, ms[i], &err);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "ParseRoutingSockaddr of route %v: %v", i, err);
        CHECK(sas.len == SYSCALL_RTAX_MAX);
        syscall_routing_sockaddr_free(a, sas);
    }
    /* Even a machine with no network has a route to lo0. */
    CHECK(routes > 0);
    testing_t_logf_v(t, "%v messages, %v routes", msgs.len, routes);
    syscall_routing_message_free(a, msgs);
    ARENA_END;
}

/* ------------------------------------------------------- built by hand */

/* A message whose header is h, size bytes of it, with data after it. */
static Int build(uint64_t *buf, const void *h, size_t size, const Byte *data,
                 size_t dlen) {
    Byte *b = (Byte *)buf;
    memcpy(b, h, size);
    memcpy(b + size, data, dlen);
    uint16_t msglen = (uint16_t)(size + dlen);
    memcpy(b, &msglen, 2);
    return (Int)msglen;
}

static void TestParseRoutingMessageAddrs(TestingT *t) {
    ARENA_BEGIN;
    Error err = BURROW_NO_ERROR;
    SyscallIfaMsghdr h;
    memset(&h, 0, sizeof h);
    h.version = SYSCALL_RTM_VERSION;
    h.type = SYSCALL_RTM_NEWADDR;
    h.addrs = SYSCALL_RTA_NETMASK | SYSCALL_RTA_IFA;
    h.index = 7;
    /* The netmask in the kernel's short form, then a whole sockaddr_in. */
    const Byte data[24] = {8, 0, 0,  0, 255, 0, 0, 0, 16, SYSCALL_AF_INET,
                           0, 0, 10, 1, 2,   3, 0, 0, 0,  0,
                           0, 0, 0,  0};
    uint64_t buf[64];
    Int n = build(buf, &h, SYSCALL_SIZEOF_IFA_MSGHDR, data, sizeof data);
    Slice msgs = syscall_parse_routing_message(a, (Slice){buf, n, n, TYPE_BYTE}, &err);
    CHECK(BURROW_OK(err));
    CHECK(msgs.len == 1);
    if (msgs.len != 1)
        testing_t_fatalf_v(t, "got %v messages, want 1", msgs.len);
    SyscallRoutingMessage m = ((SyscallRoutingMessage *)msgs.p)[0];
    CHECK(m.vt->self_type == TYPE_SYSCALL_INTERFACE_ADDR_MESSAGE);
    SyscallInterfaceAddrMessage *im = (SyscallInterfaceAddrMessage *)m.data;
    CHECK(im->header.index == 7);
    CHECK(im->data.len == 24);

    Slice sas = syscall_parse_routing_sockaddr(a, m, &err);
    CHECK(BURROW_OK(err));
    CHECK(sas.len == SYSCALL_RTAX_MAX);
    SyscallSockaddr *s = (SyscallSockaddr *)sas.p;
    for (Int i = 0; i < sas.len; i++) {
        if (i == SYSCALL_RTAX_NETMASK || i == SYSCALL_RTAX_IFA)
            continue;
        CHECK(s[i].vt == NULL);
    }
    CHECK(type_of(s[SYSCALL_RTAX_NETMASK]) == TYPE_SYSCALL_SOCKADDR_INET4);
    CHECK(type_of(s[SYSCALL_RTAX_IFA]) == TYPE_SYSCALL_SOCKADDR_INET4);
    if (type_of(s[SYSCALL_RTAX_NETMASK]) == TYPE_SYSCALL_SOCKADDR_INET4) {
        const uint8_t want[4] = {255, 0, 0, 0};
        CHECK(memcmp(((SyscallSockaddrInet4 *)s[SYSCALL_RTAX_NETMASK].data)->addr, want,
                     4) == 0);
    }
    if (type_of(s[SYSCALL_RTAX_IFA]) == TYPE_SYSCALL_SOCKADDR_INET4) {
        const uint8_t want[4] = {10, 1, 2, 3};
        CHECK(memcmp(((SyscallSockaddrInet4 *)s[SYSCALL_RTAX_IFA].data)->addr, want,
                     4) == 0);
    }
    syscall_routing_sockaddr_free(a, sas);

    /* A link address whose lengths say it is longer than what is there. */
    h.addrs = SYSCALL_RTA_IFP;
    const Byte bad[8] = {20, SYSCALL_AF_LINK, 1, 0, 0, 3, 6, 0};
    n = build(buf, &h, SYSCALL_SIZEOF_IFA_MSGHDR, bad, sizeof bad);
    syscall_routing_message_free(a, msgs);
    msgs = syscall_parse_routing_message(a, (Slice){buf, n, n, TYPE_BYTE}, &err);
    CHECK(BURROW_OK(err) && msgs.len == 1);
    if (msgs.len == 1) {
        sas = syscall_parse_routing_sockaddr(a, ((SyscallRoutingMessage *)msgs.p)[0],
                                             &err);
        CHECK(errno_of(err) == SYSCALL_EINVAL);
        CHECK(sas.p == NULL && sas.len == 0);
    }
    syscall_routing_message_free(a, msgs);
    ARENA_END;
}

static void TestParseRoutingMessageBad(TestingT *t) {
    ARENA_BEGIN;
    Error err = BURROW_NO_ERROR;
    uint64_t buf[8];
    Byte *b = (Byte *)buf;
    uint16_t l;

    /* Too short for even the length, version and type: nothing, and fine. */
    memset(buf, 0, sizeof buf);
    Slice msgs = syscall_parse_routing_message(a, (Slice){b, 3, 3, TYPE_BYTE}, &err);
    CHECK(BURROW_OK(err) && msgs.len == 0);

    /* A type there is no message for is skipped. */
    l = 4;
    memcpy(b, &l, 2);
    b[2] = SYSCALL_RTM_VERSION;
    b[3] = 0xfe;
    msgs = syscall_parse_routing_message(a, (Slice){b, 4, 4, TYPE_BYTE}, &err);
    CHECK(BURROW_OK(err) && msgs.len == 0);

    /* Another version is EINVAL. */
    b[2] = SYSCALL_RTM_VERSION + 1;
    b[3] = SYSCALL_RTM_ADD;
    msgs = syscall_parse_routing_message(a, (Slice){b, 4, 4, TYPE_BYTE}, &err);
    CHECK(errno_of(err) == SYSCALL_EINVAL && msgs.len == 0);

    /* A length of 0, which Go would loop on forever. */
    l = 0;
    memcpy(b, &l, 2);
    b[2] = SYSCALL_RTM_VERSION;
    msgs = syscall_parse_routing_message(a, (Slice){b, 4, 4, TYPE_BYTE}, &err);
    CHECK(errno_of(err) == SYSCALL_EINVAL);

    /* A length past the end, and one shorter than the header, where Go's
     * slices would panic. */
    l = 60;
    memcpy(b, &l, 2);
    msgs = syscall_parse_routing_message(a, (Slice){b, 8, 8, TYPE_BYTE}, &err);
    CHECK(errno_of(err) == SYSCALL_EINVAL);
    l = 8;
    memcpy(b, &l, 2);
    msgs = syscall_parse_routing_message(a, (Slice){b, 8, 8, TYPE_BYTE}, &err);
    CHECK(errno_of(err) == SYSCALL_EINVAL);

    /* A message with no address in it has no Sockaddrs. */
    SyscallRoutingMessage none = {NULL, NULL};
    Slice sas = syscall_parse_routing_sockaddr(a, none, &err);
    CHECK(BURROW_OK(err) && sas.p == NULL);
    ARENA_END;
}

/* -------------------------------------------------------------------- BPF */

static void TestBpfInsn(TestingT *t) {
    (void)t;
    SyscallBpfInsn i = syscall_bpf_stmt(SYSCALL_BPF_RET | SYSCALL_BPF_K, 0x10000);
    CHECK(i.code == (SYSCALL_BPF_RET | SYSCALL_BPF_K) && i.k == 0x10000);
    CHECK(i.jt == 0 && i.jf == 0);
    i = syscall_bpf_jump(0x15, 0x800, 1, 2);
    CHECK(i.code == 0x15 && i.k == 0x800 && i.jt == 1 && i.jf == 2);
    /* Go truncates to the field's width, and so does this. */
    i = syscall_bpf_jump(0x10015, -1, 0x101, 0x102);
    CHECK(i.code == 0x15 && i.k == 0xffffffff && i.jt == 1 && i.jf == 2);
}

/* A /dev/bpf descriptor, or -1 when this test may not have one. */
static Int open_bpf(void) {
    Error err = BURROW_NO_ERROR;
    Int fd = syscall_open(BURROW_S("/dev/bpf"), SYSCALL_O_RDWR, 0, &err);
    if (BURROW_OK(err))
        return fd;
    for (int i = 0; i < 16; i++) {
        char name[32];
        snprintf(name, sizeof name, "/dev/bpf%d", i);
        err = BURROW_NO_ERROR;
        fd = syscall_open(str_from_bytes((const Byte *)name, (Int)strlen(name)),
                          SYSCALL_O_RDWR, 0, &err);
        if (BURROW_OK(err))
            return fd;
        if (errno_of(err) != SYSCALL_EBUSY)
            break;
    }
    return -1;
}

static void TestBpf(TestingT *t) {
    /* A descriptor that is not open, which needs no root. */
    Error err = BURROW_NO_ERROR;
    CHECK(errno_of(syscall_check_bpf_version(-1)) == SYSCALL_EBADF);
    CHECK(syscall_bpf_buflen(-1, &err) == 0 && errno_of(err) == SYSCALL_EBADF);
    SyscallTimeval tv = syscall_bpf_timeout(-1, &err);
    CHECK(errno_of(err) == SYSCALL_EBADF && tv.sec == 0);

    Int fd = open_bpf();
    if (fd < 0) {
        testing_t_skip_v(t, "cannot open /dev/bpf, which needs root");
        return;
    }
    CHECK(BURROW_OK(syscall_check_bpf_version(fd)));
    Int n = syscall_set_bpf_buflen(fd, 4096, &err);
    CHECK(BURROW_OK(err) && n == 4096);
    n = syscall_bpf_buflen(fd, &err);
    CHECK(BURROW_OK(err) && n == 4096);

    CHECK(BURROW_OK(syscall_set_bpf_interface(fd, BURROW_S("lo0"))));
    Str name = syscall_bpf_interface(fd, BURROW_S("anything"), &err);
    CHECK(BURROW_OK(err) && str_eq(name, BURROW_S("anything")));
    n = syscall_bpf_datalink(fd, &err);
    CHECK(BURROW_OK(err) && n == SYSCALL_DLT_NULL);

    CHECK(BURROW_OK(syscall_set_bpf_immediate(fd, 1)));
    tv = (SyscallTimeval){.sec = 1};
    CHECK(BURROW_OK(syscall_set_bpf_timeout(fd, &tv)));
    tv = syscall_bpf_timeout(fd, &err);
    CHECK(BURROW_OK(err) && tv.sec == 1 && tv.usec == 0);

    SyscallBpfInsn prog[1] = {
        syscall_bpf_stmt(SYSCALL_BPF_RET | SYSCALL_BPF_K, 0xffff)};
    CHECK(BURROW_OK(syscall_set_bpf(fd, (Slice){prog, 1, 1, TYPE_BYTE})));
    SyscallBpfStat st = syscall_bpf_stats(fd, &err);
    CHECK(BURROW_OK(err));
    testing_t_logf_v(t, "recv %v drop %v", (Int)st.recv, (Int)st.drop);
    CHECK(BURROW_OK(syscall_flush_bpf(fd)));

    CHECK(BURROW_OK(syscall_set_bpf_headercmpl(fd, 1)));
    n = syscall_bpf_headercmpl(fd, &err);
    CHECK(BURROW_OK(err) && n == 1);
    (void)syscall_close(fd);
}

#define TESTS(X)                                                                       \
    X(TestRouteRIBInterfaces)                                                          \
    X(TestRouteRIBDump)                                                                \
    X(TestParseRoutingMessageAddrs)                                                    \
    X(TestParseRoutingMessageBad)                                                      \
    X(TestBpfInsn)                                                                     \
    X(TestBpf)

#else

static void TestNothing(TestingT *t) {
    testing_t_skip_v(t, "these are the macOS and FreeBSD functions");
}

#define TESTS(X) X(TestNothing)

#endif

TESTING_MAIN(TESTS)

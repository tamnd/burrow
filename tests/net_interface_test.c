/* Interfaces, from Go's interface_test.go and interface_linux_test.go.
 *
 * What these look at is the machine they run on, so they check what Go's do:
 * that an interface found in the list is the one found again by its index and
 * by its name, and that the addresses look like addresses of the right kind.
 * TestParseProcNet reads Go's copies of /proc/net/igmp and /proc/net/igmp6,
 * which are below, and runs everywhere.
 *
 * Go's interface_unix_test.go is not here. Each of its tests adds a tunnel or
 * a dummy interface to the machine with the ip command, as root, and that
 * changes the network of whatever machine runs the suite.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/burrow.h"
#include "burrow/error.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/net.h"
#include "burrow/os.h"
#include "burrow/path.h"
#include "burrow/testing.h"

#include "../src/net/internal.h"
#include "check.h"

#include <stdint.h>
#include <string.h>

#define S(lit) BURROW_S(lit)

static Arena ar;
static Alloc *a;

static const NetInterface *at(Slice ift, Int i) {
    return &((const NetInterface *)ift.p)[i];
}

static bool same_bytes(Slice x, Slice y) {
    return x.len == y.len && (x.len == 0 || memcmp(x.p, y.p, (size_t)x.len) == 0);
}

/* reflect.DeepEqual for two Interfaces. */
static bool same_interface(const NetInterface *x, const NetInterface *y) {
    return x->index == y->index && x->mtu == y->mtu && str_eq(x->name, y->name) &&
           same_bytes(x->hardware_addr, y->hardware_addr) && x->flags == y->flags &&
           (x->hardware_addr.p == NULL) == (y->hardware_addr.p == NULL);
}

static void TestInterfaces(TestingT *t) {
    Error err = BURROW_NO_ERROR;
    Slice ift = net_interfaces(a, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
        return;
    }
    for (Int i = 0; i < ift.len; i++) {
        const NetInterface *ifi = at(ift, i);
        NetInterface *ifxi = net_interface_by_index(a, ifi->index, &err);
        if (BURROW_FAILED(err)) {
            testing_t_fatal_v(t, err);
            return;
        }
        if (!same_interface(ifxi, ifi))
            testing_t_errorf_v(t, "by index %d: got %s, want %s", ifi->index,
                               ifxi->name, ifi->name);
        if (ifi->name.len != 0) {
            NetInterface *ifxn = net_interface_by_name(a, ifi->name, &err);
            if (BURROW_FAILED(err)) {
                testing_t_fatal_v(t, err);
                return;
            }
            if (!same_interface(ifxn, ifi))
                testing_t_errorf_v(t, "by name %s: got index %d, want %d", ifi->name,
                                   ifxn->index, ifi->index);
        }
        testing_t_logf_v(t, "%s: flags=%s index=%d mtu=%d hwaddr=%s", ifi->name,
                         net_flags_string(ifi->flags, a), ifi->index, ifi->mtu,
                         net_hardware_addr_string(ifi->hardware_addr, a));
    }
}

typedef struct IfStats {
    Int loop;  /* active loopback interfaces */
    Int other; /* every other active interface */
} IfStats;

typedef struct RouteStats {
    Int ipv4;
    Int ipv6;
} RouteStats;

static IfStats interface_stats(Slice ift) {
    IfStats s = {0, 0};
    for (Int i = 0; i < ift.len; i++) {
        NetFlags f = at(ift, i)->flags;
        if ((f & NET_FLAG_UP) != 0) {
            if ((f & NET_FLAG_LOOPBACK) != 0)
                s.loop++;
            else
                s.other++;
        }
    }
    return s;
}

static bool is_v4(NetIP ip) {
    return net_ip_to4(ip).len != 0;
}

/* validateInterfaceUnicastAddrs. False, with the test failed, on the first
 * address that is wrong. */
static bool validate_unicast(TestingT *t, Slice ifat, RouteStats *stats) {
    for (Int i = 0; i < ifat.len; i++) {
        NetAddr x = ((const NetAddr *)ifat.p)[i];
        Str text = x.vt == NULL ? S("<nil>") : x.vt->string(x.data, a);
        if (x.vt != NULL && x.vt->self_type == TYPE_NET_IP_NET) {
            const NetIPNet *n = (const NetIPNet *)x.data;
            if (n->ip.p == NULL || net_ip_is_multicast(n->ip) || n->mask.p == NULL) {
                testing_t_errorf_v(t, "unexpected value: %s", text);
                return false;
            }
            if (n->ip.len != 16) {
                testing_t_errorf_v(t,
                                   "should be internal representation either IPv6 or "
                                   "IPv4-mapped IPv6 address: %s",
                                   text);
                return false;
            }
            Int bits = 0;
            Int prefix = net_ip_mask_size(n->mask, &bits);
            if (is_v4(n->ip)) {
                if (0 >= prefix || prefix > 32 || bits != 32 ||
                    (net_ip_is_loopback(n->ip) && prefix < 8)) {
                    testing_t_errorf_v(t, "unexpected prefix length: %d/%d for %s",
                                       prefix, bits, text);
                    return false;
                }
                stats->ipv4++;
            } else {
                if (0 >= prefix || prefix > 128 || bits != 128 ||
                    (net_ip_is_loopback(n->ip) && prefix != 128)) {
                    testing_t_errorf_v(t, "unexpected prefix length: %d/%d for %s",
                                       prefix, bits, text);
                    return false;
                }
                stats->ipv6++;
            }
        } else if (x.vt != NULL && x.vt->self_type == TYPE_NET_IP_ADDR) {
            const NetIPAddr *p = (const NetIPAddr *)x.data;
            if (p->ip.p == NULL || net_ip_is_multicast(p->ip)) {
                testing_t_errorf_v(t, "unexpected value: %s", text);
                return false;
            }
            if (p->ip.len != 16) {
                testing_t_errorf_v(t,
                                   "should be internal representation either IPv6 or "
                                   "IPv4-mapped IPv6 address: %s",
                                   text);
                return false;
            }
            if (is_v4(p->ip))
                stats->ipv4++;
            else
                stats->ipv6++;
        } else {
            testing_t_errorf_v(t, "unexpected type: %s", text);
            return false;
        }
    }
    return true;
}

/* validateInterfaceMulticastAddrs. */
static bool validate_multicast(TestingT *t, Slice ifat, RouteStats *stats) {
    for (Int i = 0; i < ifat.len; i++) {
        NetAddr x = ((const NetAddr *)ifat.p)[i];
        Str text = x.vt == NULL ? S("<nil>") : x.vt->string(x.data, a);
        if (x.vt == NULL || x.vt->self_type != TYPE_NET_IP_ADDR) {
            testing_t_errorf_v(t, "unexpected type: %s", text);
            return false;
        }
        const NetIPAddr *p = (const NetIPAddr *)x.data;
        if (p->ip.p == NULL || net_ip_is_unspecified(p->ip) ||
            !net_ip_is_multicast(p->ip)) {
            testing_t_errorf_v(t, "unexpected value: %s", text);
            return false;
        }
        if (p->ip.len != 16) {
            testing_t_errorf_v(t,
                               "should be internal representation either IPv6 or "
                               "IPv4-mapped IPv6 address: %s",
                               text);
            return false;
        }
        if (is_v4(p->ip))
            stats->ipv4++;
        else
            stats->ipv6++;
    }
    return true;
}

static void check_unicast_stats(TestingT *t, IfStats s, RouteStats u) {
    if (burrow__net_supports_ipv4() && s.loop + s.other > 0 && u.ipv4 == 0)
        testing_t_errorf_v(t, "num IPv4 unicast routes = 0; want >0; loop %d other %d",
                           s.loop, s.other);
    /* There is ::1/128 wherever there is a loopback interface. */
    if (burrow__net_supports_ipv6() && s.loop > 0 && u.ipv6 == 0)
        testing_t_errorf_v(t, "num IPv6 unicast routes = 0; want >0; loop %d other %d",
                           s.loop, s.other);
}

static void check_multicast_stats(TestingT *t, IfStats s, RouteStats u, RouteStats m) {
#if defined(BURROW_OS_AIX) || defined(BURROW_OS_DRAGONFLY) ||                          \
    defined(BURROW_OS_NETBSD) || defined(BURROW_OS_OPENBSD) ||                         \
    defined(BURROW_OS_SOLARIS)
    (void)t;
    (void)s;
    (void)u;
    (void)m;
#else
    /* IPv4 multicast is optional, so only IPv6 is checked, and only where
     * there is a loopback interface and some other IPv6 address besides ::1. */
    if (burrow__net_supports_ipv6() && s.loop > 0 && u.ipv6 > 1 && m.ipv6 == 0)
        testing_t_errorf_v(t, "num IPv6 multicast route clones = 0; want >0; ipv6 %d",
                           u.ipv6);
#endif
}

static void TestInterfaceAddrs(TestingT *t) {
    Error err = BURROW_NO_ERROR;
    Slice ift = net_interfaces(a, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
        return;
    }
    IfStats s = interface_stats(ift);
    Slice ifat = net_interface_addrs(a, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
        return;
    }
    RouteStats u = {0, 0};
    if (!validate_unicast(t, ifat, &u))
        return;
    check_unicast_stats(t, s, u);
}

static void TestInterfaceUnicastAddrs(TestingT *t) {
    Error err = BURROW_NO_ERROR;
    Slice ift = net_interfaces(a, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
        return;
    }
    IfStats s = interface_stats(ift);
    RouteStats u = {0, 0};
    for (Int i = 0; i < ift.len; i++) {
        Slice ifat = net_interface_addrs_of(at(ift, i), a, &err);
        if (BURROW_FAILED(err)) {
            testing_t_fatal_v(t, at(ift, i)->name, err);
            return;
        }
        if (!validate_unicast(t, ifat, &u))
            return;
    }
    check_unicast_stats(t, s, u);
}

static void TestInterfaceMulticastAddrs(TestingT *t) {
    Error err = BURROW_NO_ERROR;
    Slice ift = net_interfaces(a, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
        return;
    }
    IfStats s = interface_stats(ift);
    Slice ifat = net_interface_addrs(a, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
        return;
    }
    RouteStats u = {0, 0};
    if (!validate_unicast(t, ifat, &u))
        return;
    RouteStats m = {0, 0};
    for (Int i = 0; i < ift.len; i++) {
        Slice ifmat = net_interface_multicast_addrs(at(ift, i), a, &err);
        if (BURROW_FAILED(err)) {
            testing_t_fatal_v(t, at(ift, i)->name, err);
            return;
        }
        if (!validate_multicast(t, ifmat, &m))
            return;
    }
    check_multicast_stats(t, s, u, m);
}

/* The errors, which Go gives as an OpError with no addresses. */
static void TestInterfaceErrors(TestingT *t) {
    Error err = BURROW_NO_ERROR;
    CHECK(net_interface_by_index(a, 0, &err) == NULL);
    CHECK(str_eq(error_text(err), S("route ip+net: invalid network interface index")));
    err = BURROW_NO_ERROR;
    CHECK(net_interface_by_index(a, -1, &err) == NULL);
    CHECK(str_eq(error_text(err), S("route ip+net: invalid network interface index")));
    err = BURROW_NO_ERROR;
    CHECK(net_interface_by_name(a, BURROW_STR_EMPTY, &err) == NULL);
    CHECK(str_eq(error_text(err), S("route ip+net: invalid network interface name")));
    err = BURROW_NO_ERROR;
    CHECK(net_interface_by_name(a, S("no-such-interface-here"), &err) == NULL);
    CHECK(str_eq(error_text(err), S("route ip+net: no such network interface")));
    err = BURROW_NO_ERROR;
    (void)net_interface_addrs_of(NULL, a, &err);
    CHECK(str_eq(error_text(err), S("route ip+net: invalid network interface")));
    err = BURROW_NO_ERROR;
    (void)net_interface_multicast_addrs(NULL, a, &err);
    CHECK(str_eq(error_text(err), S("route ip+net: invalid network interface")));
}

static void TestFlagsString(TestingT *t) {
    CHECK(str_eq(net_flags_string(0, a), S("0")));
    CHECK(
        str_eq(net_flags_string(NET_FLAG_UP | NET_FLAG_LOOPBACK | NET_FLAG_RUNNING, a),
               S("up|loopback|running")));
    CHECK(str_eq(net_flags_string(NET_FLAG_BROADCAST | NET_FLAG_POINT_TO_POINT |
                                      NET_FLAG_MULTICAST,
                                  a),
                 S("broadcast|pointtopoint|multicast")));
}

/* The zone cache answers with the names and indexes in the list, the first
 * name for an index and the last index for a name, and with the number when
 * there is no such interface. */
static void TestZoneCacheLookups(TestingT *t) {
    Error err = BURROW_NO_ERROR;
    Slice ift = net_interfaces(a, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
        return;
    }
    burrow__net_zone_cache_update(true);
    Byte buf[24];
    for (Int i = 0; i < ift.len; i++) {
        const NetInterface *ifi = at(ift, i);
        if (ifi->name.len == 0)
            continue;
        Int first = -1;
        Int last = -1;
        for (Int j = 0; j < ift.len; j++) {
            if (first < 0 && at(ift, j)->index == ifi->index &&
                at(ift, j)->name.len > 0)
                first = j;
            if (str_eq(at(ift, j)->name, ifi->name))
                last = j;
        }
        Str name = burrow__net_zone_name(ifi->index, buf);
        if (!str_eq(name, at(ift, first)->name))
            testing_t_errorf_v(t, "zone name of %d = %s; want %s", ifi->index, name,
                               at(ift, first)->name);
        Int index = burrow__net_zone_index(ifi->name);
        if (index != at(ift, last)->index)
            testing_t_errorf_v(t, "zone index of %s = %d; want %d", ifi->name, index,
                               at(ift, last)->index);
    }
    CHECK(burrow__net_zone_name(0, buf).len == 0);
    CHECK(burrow__net_zone_index(BURROW_STR_EMPTY) == 0);
    CHECK(str_eq(burrow__net_zone_name(9999999, buf), S("9999999")));
    CHECK_INT_EQ(burrow__net_zone_index(S("no-such-zone")), 0);
    CHECK_INT_EQ(burrow__net_zone_index(S("12345678")), 0xFFFFFF);
}

/* ---------------------------------------------------------- /proc/net/igmp */

typedef struct TestFile {
    const char *name;
    const char *data;
    Int len;
} TestFile;

/* Go's src/net/testdata/igmp and igmp6. */
static const TestFile test_files[] = {
    {"igmp",
     "Idx\tDevice    : Count Querier\tGroup    Users Timer\tReporter\n"
     "1\tlo        :     1      V3\n"
     "\t\t\t\t010000E0     1 0:00000000\t\t0\n"
     "2\teth0      :     2      V2\n"
     "\t\t\t\tFB0000E0     1 0:00000000\t\t1\n"
     "\t\t\t\t010000E0     1 0:00000000\t\t0\n"
     "3\teth1      :     1      V3\n"
     "\t\t\t\t010000E0     1 0:00000000\t\t0\n"
     "4\teth2      :     1      V3\n"
     "\t\t\t\t010000E0     1 0:00000000\t\t0\n"
     "5\teth0.100  :     2      V3\n"
     "\t\t\t\tFB0000E0     1 0:00000000\t\t0\n"
     "\t\t\t\t010000E0     1 0:00000000\t\t0\n"
     "6\teth0.101  :     2      V3\n"
     "\t\t\t\tFB0000E0     1 0:00000000\t\t0\n"
     "\t\t\t\t010000E0     1 0:00000000\t\t0\n"
     "7\teth0.102  :     2      V3\n"
     "\t\t\t\tFB0000E0     1 0:00000000\t\t0\n"
     "\t\t\t\t010000E0     1 0:00000000\t\t0\n"
     "8\teth0.103  :     2      V3\n"
     "\t\t\t\tFB0000E0     1 0:00000000\t\t0\n"
     "\t\t\t\t010000E0     1 0:00000000\t\t0\n"
     "9\tdevice1tap2:     1      V3\n"
     "\t\t\t\t010000E0     1 0:00000000\t\t0\n",
     775},
    {"igmp6",
     "1    lo              ff020000000000000000000000000001     1 0000000C 0\n"
     "2    eth0            ff0200000000000000000001ffac891e     1 00000006 0\n"
     "2    eth0            ff020000000000000000000000000001     1 0000000C 0\n"
     "3    eth1            ff0200000000000000000001ffac8928     2 00000006 0\n"
     "3    eth1            ff020000000000000000000000000001     1 0000000C 0\n"
     "4    eth2            ff0200000000000000000001ffac8932     2 00000006 0\n"
     "4    eth2            ff020000000000000000000000000001     1 0000000C 0\n"
     "5    eth0.100        ff0200000000000000000001ffac891e     1 00000004 0\n"
     "5    eth0.100        ff020000000000000000000000000001     1 0000000C 0\n"
     "6    pan0            ff020000000000000000000000000001     1 0000000C 0\n"
     "7    eth0.101        ff0200000000000000000001ffac891e     1 00000004 0\n"
     "7    eth0.101        ff020000000000000000000000000001     1 0000000C 0\n"
     "8    eth0.102        ff0200000000000000000001ffac891e     1 00000004 0\n"
     "8    eth0.102        ff020000000000000000000000000001     1 0000000C 0\n"
     "9    eth0.103        ff0200000000000000000001ffac891e     1 00000004 0\n"
     "9    eth0.103        ff020000000000000000000000000001     1 0000000C 0\n"
     "10   device1tap2     ff0200000000000000000001ff4cc3a3     1 00000004 0\n"
     "10   device1tap2     ff020000000000000000000000000001     1 0000000C 0\n",
     1278},
};

static const char *const igmp_interfaces[] = {"lo",       "eth0",     "eth1",
                                              "eth2",     "eth0.100", "eth0.101",
                                              "eth0.102", "eth0.103", "device1tap2"};

static const char *const igmp6_interfaces[] = {
    "lo",       "eth0",     "eth1",     "eth2",        "eth0.100",
    "eth0.101", "eth0.102", "eth0.103", "device1tap2", "pan0"};

static void TestParseProcNet(TestingT *t) {
    Error err = BURROW_NO_ERROR;
    Str dir = os_mkdir_temp(a, BURROW_STR_EMPTY, S("net-igmp"), &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "MkdirTemp: %v", err);
        return;
    }
    Str path[2];
    for (size_t i = 0; i < sizeof test_files / sizeof test_files[0]; i++) {
        const TestFile *f = &test_files[i];
        path[i] = path_join_v(a, 2, dir, str_from_cstr(f->name));
        Slice data = slice_from((void *)(uintptr_t)f->data, f->len, f->len, TYPE_BYTE);
        err = os_write_file(path[i], data, 0644);
        if (BURROW_FAILED(err)) {
            (void)os_remove_all(dir);
            testing_t_fatalf_v(t, "WriteFile: %v", err);
            return;
        }
    }
    Slice ifmat4 = slice_nil(TYPE_NET_ADDR);
    for (size_t i = 0; i < sizeof igmp_interfaces / sizeof igmp_interfaces[0]; i++) {
        NetInterface ifi;
        memset(&ifi, 0, sizeof ifi);
        ifi.name = str_from_cstr(igmp_interfaces[i]);
        CHECK(burrow__net_parse_proc_net_igmp(a, path[0], &ifi, &ifmat4));
    }
    CHECK_INT_EQ(ifmat4.len, 14);
    Slice ifmat6 = slice_nil(TYPE_NET_ADDR);
    for (size_t i = 0; i < sizeof igmp6_interfaces / sizeof igmp6_interfaces[0]; i++) {
        NetInterface ifi;
        memset(&ifi, 0, sizeof ifi);
        ifi.name = str_from_cstr(igmp6_interfaces[i]);
        CHECK(burrow__net_parse_proc_net_igmp6(a, path[1], &ifi, &ifmat6));
    }
    CHECK_INT_EQ(ifmat6.len, 18);
    (void)os_remove_all(dir);
}

#define TESTS(X)                                                                       \
    X(TestInterfaces)                                                                  \
    X(TestInterfaceAddrs)                                                              \
    X(TestInterfaceUnicastAddrs)                                                       \
    X(TestInterfaceMulticastAddrs)                                                     \
    X(TestInterfaceErrors)                                                             \
    X(TestFlagsString)                                                                 \
    X(TestZoneCacheLookups)                                                            \
    X(TestParseProcNet)

static int net_interface_main(TestingM *m) {
    arena_init(&ar, NULL, 0);
    a = arena_allocator(&ar);
    int r = testing_m_run(m);
    arena_free(&ar);
    return r;
}

TESTING_MAIN_WITH(net_interface_main, TESTS)

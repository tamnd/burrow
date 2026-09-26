/* net/netip's tests, from Go's netip_test.go and netip_pkg_test.go. The JSON
 * round trips become text marshalling round trips, since that is all
 * encoding/json does with these types, and parseIPSlow and the uint128 tests
 * are left out, as they test Go's internals.
 *
 * Copyright 2020 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/net/netip.h"

#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/panic.h"
#include "burrow/strings.h"
#include "burrow/testing.h"

#include "check.h"

#include <stdint.h>
#include <string.h>

static NetipAddr ip(const char *s) {
    return netip_must_parse_addr(str_from_cstr(s));
}

static NetipAddrPort ipp(const char *s) {
    return netip_must_parse_addr_port(str_from_cstr(s));
}

static NetipPrefix pfx(const char *s) {
    return netip_must_parse_prefix(str_from_cstr(s));
}

static const NetipAddr zero_addr;
static const NetipAddrPort zero_addr_port;
static const NetipPrefix zero_prefix;

/* A C string copy of s, for CHECK_STR_EQ. */
static const char *cz(Alloc *a, Str s) {
    char *p = mem_alloc_nozero(a, (size_t)s.len + 1, 1);
    if (s.len > 0)
        memcpy(p, s.p, (size_t)s.len);
    p[s.len] = 0;
    return p;
}

static const char *bz(Alloc *a, Slice b) {
    return cz(a, str_from_bytes(b.p, b.len));
}

/* Go's MkAddr(Mk128(hi, lo), z), with zone NULL for Z4, "" for Z6noz and
 * anything else for that zone. */
static NetipAddr mk(uint64_t hi, uint64_t lo, const char *zone) {
    NetipAddr r = {hi, lo, {NULL}};
    if (zone == NULL) {
        r.z.value = &burrow__netip_z4;
    } else if (*zone == 0) {
        r.z.value = &burrow__netip_z6noz;
    } else {
        Str z = str_from_cstr(zone);
        r.z = unique_make(TYPE_STRING, &z);
    }
    return r;
}

/* A slice of four bytes of junk, to append after, as Go's tests do. */
static Slice four(Alloc *a) {
    return slice_make(a, TYPE_BYTE, 4, 32);
}

static Slice tail4(Slice b) {
    return slice_sub(b, 4, b.len);
}

/* Go's testAppendToMarshal. */
static void check_append_to_addr(TestingT *t, Alloc *a, NetipAddr x) {
    Error err;
    Slice m = netip_addr_marshal_text(x, a, &err);
    Slice b = netip_addr_append_to(x, a, slice_nil(TYPE_BYTE));
    CHECK(BURROW_OK(err));
    CHECK_STR_EQ(bz(a, b), bz(a, m));
}

static void check_append_to_addr_port(TestingT *t, Alloc *a, NetipAddrPort x) {
    Error err;
    Slice m = netip_addr_port_marshal_text(x, a, &err);
    Slice b = netip_addr_port_append_to(x, a, slice_nil(TYPE_BYTE));
    CHECK(BURROW_OK(err));
    CHECK_STR_EQ(bz(a, b), bz(a, m));
}

static void check_append_to_prefix(TestingT *t, Alloc *a, NetipPrefix x) {
    Error err;
    Slice m = netip_prefix_marshal_text(x, a, &err);
    Slice b = netip_prefix_append_to(x, a, slice_nil(TYPE_BYTE));
    CHECK(BURROW_OK(err));
    CHECK_STR_EQ(bz(a, b), bz(a, m));
}

/* s as a byte slice. The unmarshal functions keep nothing from their input,
 * so one buffer does for all of them. */
static Byte text_buf[256];

static Slice text(const char *s) {
    Int n = (Int)strlen(s);
    memcpy(text_buf, s, (size_t)n);
    return slice_from(text_buf, n, n, TYPE_BYTE);
}

static void TestParseAddr(TestingT *t) {
    static const struct {
        const char *in;
        uint64_t hi, lo;
        const char *zone; /* NULL for IPv4, "" for IPv6 without a zone */
        const char *str;
        const char *want_err;
    } valid[] = {
        {"0.0.0.0", 0, 0xffff00000000, NULL, NULL, NULL},
        {"192.168.140.255", 0, 0xffffc0a88cff, NULL, NULL, NULL},
        {"010.000.015.001", 0, 0, NULL, NULL,
         "ParseAddr(\"010.000.015.001\"): IPv4 field has octet with leading zero"},
        {"000001.00000002.00000003.000000004", 0, 0, NULL, NULL,
         "ParseAddr(\"000001.00000002.00000003.000000004\"): IPv4 field has octet "
         "with leading zero"},
        {"::ffff:1.2.03.4", 0, 0, NULL, NULL,
         "ParseAddr(\"::ffff:1.2.03.4\"): IPv4 field has octet with leading zero"},
        {"::ffff:1.2.3.z", 0, 0, NULL, NULL,
         "ParseAddr(\"::ffff:1.2.3.z\"): unexpected character (at \"z\")"},
        {"::", 0, 0, "", NULL, NULL},
        {"::1", 0, 1, "", NULL, NULL},
        {"fd7a:115c:a1e0:ab12:4843:cd96:626b:430b", 0xfd7a115ca1e0ab12,
         0x4843cd96626b430b, "", NULL, NULL},
        {"fd7a:115c::626b:430b", 0xfd7a115c00000000, 0x00000000626b430b, "", NULL,
         NULL},
        {"fd7a:115c:a1e0:ab12:4843:cd96::", 0xfd7a115ca1e0ab12, 0x4843cd9600000000, "",
         NULL, NULL},
        {"fd7a:115c:a1e0:ab12:4843:cd96:626b::", 0xfd7a115ca1e0ab12, 0x4843cd96626b0000,
         "", "fd7a:115c:a1e0:ab12:4843:cd96:626b:0", NULL},
        {"fd7a:115c:a1e0::4843:cd96:626b:430b", 0xfd7a115ca1e00000, 0x4843cd96626b430b,
         "", "fd7a:115c:a1e0:0:4843:cd96:626b:430b", NULL},
        {"::ffff:192.168.140.255", 0, 0x0000ffffc0a88cff, "", "::ffff:192.168.140.255",
         NULL},
        {"fd7a:115c:a1e0:ab12:4843:cd96:626b:430b%eth0", 0xfd7a115ca1e0ab12,
         0x4843cd96626b430b, "eth0", NULL, NULL},
        {"1:2::ffff:192.168.140.255%eth1", 0x0001000200000000, 0x0000ffffc0a88cff,
         "eth1", "1:2::ffff:c0a8:8cff%eth1", NULL},
        {"::ffff:192.168.140.255%eth1", 0, 0x0000ffffc0a88cff, "eth1",
         "::ffff:192.168.140.255%eth1", NULL},
        {"FD9E:1A04:F01D::1", 0xfd9e1a04f01d0000, 0x1, "", "fd9e:1a04:f01d::1", NULL},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof valid / sizeof valid[0]; i++) {
        Error err;
        Str in = str_from_cstr(valid[i].in);
        NetipAddr got = netip_parse_addr(in, &err);
        if (valid[i].want_err != NULL) {
            if (BURROW_OK(err))
                testing_t_errorf_v(t, "wanted error %q; got none", valid[i].want_err);
            else
                CHECK_STR_EQ(cz(a, error_text(err)), valid[i].want_err);
            continue;
        }
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "ParseAddr(%q): %s", in, error_text(err));
            continue;
        }
        NetipAddr want = mk(valid[i].hi, valid[i].lo, valid[i].zone);
        if (!netip_addr_eq(got, want))
            testing_t_errorf_v(t, "ParseAddr(%q) = %v, want %v", in,
                               BURROW_ANY(TYPE_NETIP_ADDR, &got),
                               BURROW_ANY(TYPE_NETIP_ADDR, &want));
        CHECK(netip_addr_eq(netip_parse_addr(in, NULL), got));

        Str s = netip_addr_string(got, a);
        NetipAddr got3 = netip_parse_addr(s, &err);
        CHECK(BURROW_OK(err));
        if (!netip_addr_eq(got, got3))
            testing_t_errorf_v(t, "ParseAddr(%q) != ParseAddr(%q)", in, s);

        const char *wants = valid[i].str != NULL ? valid[i].str : valid[i].in;
        CHECK_STR_EQ(cz(a, s), wants);

        check_append_to_addr(t, a, got);

        NetipAddr tgot = zero_addr;
        CHECK(BURROW_OK(netip_addr_unmarshal_text(&tgot, text(valid[i].in))));
        CHECK(netip_addr_eq(tgot, got));
        Slice back = netip_addr_marshal_text(tgot, a, &err);
        CHECK(BURROW_OK(err));
        CHECK_STR_EQ(bz(a, back), wants);
    }

    static const char *const invalid[] = {
        "",
        "bad",
        "1234",
        "1.2.3.4%eth0",
        ".1.2.3",
        "1.2.3.",
        "1..2.3",
        "1.2.3.4.5",
        "0300.0250.0214.0377",
        "0xc0.0xa8.0x8c.0xff",
        "192.168.12345",
        "127.0.1",
        "192.1234567",
        "127.1",
        "192.168.300.1",
        "192.168.0.1.5.6",
        "1:2:3:4:5:6:7",
        "1:2:3:4:5:6:7:8:9",
        "1:2:3:4::5:6:7:8",
        "fe801::1",
        "fe80:tail:scal:e::",
        "fe80::1%",
        "ffff:ffff:ffff:ffff:ffff:ffff:ffff:192.168.140.255",
        "ffff::ffff:ffff:ffff:ffff:ffff:ffff:192.168.140.255",
        "::ffff:192.168.140.bad",
        "fe80::1::1",
        "fe80:1?:1",
        "fe80:",
        "0:0:0:0:0:ffff:0:00000",
        "0:0:0:0:00000:ffff:127.1.2.3",
    };
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
        Error err;
        NetipAddr got = netip_parse_addr(str_from_cstr(invalid[i]), &err);
        if (BURROW_OK(err))
            testing_t_errorf_v(t, "ParseAddr(%q) = %v, want error", invalid[i],
                               BURROW_ANY(TYPE_NETIP_ADDR, &got));
        if (!netip_addr_eq(got, zero_addr))
            testing_t_errorf_v(t, "ParseAddr(%q) is not the zero Addr", invalid[i]);
        if (invalid[i][0] == 0)
            continue;
        NetipAddr tgot = zero_addr;
        if (BURROW_OK(netip_addr_unmarshal_text(&tgot, text(invalid[i]))))
            testing_t_errorf_v(t, "UnmarshalText(%q) = %v, want error", invalid[i],
                               BURROW_ANY(TYPE_NETIP_ADDR, &tgot));
    }
    arena_free(&ar);
}

static void TestAddrFromSlice(TestingT *t) {
    Byte v4[] = {10, 0, 0, 1};
    Byte v6[16] = {0xfe, 0x80, [15] = 0x01};
    Byte bad[] = {0, 1, 2};
    struct {
        Slice ip;
        NetipAddr want;
        bool ok;
    } tests[] = {
        {slice_from(v4, 4, 4, TYPE_BYTE), ip("10.0.0.1"), true},
        {slice_from(v6, 16, 16, TYPE_BYTE), ip("fe80::01"), true},
        {slice_from(bad, 3, 3, TYPE_BYTE), zero_addr, false},
        {slice_nil(TYPE_BYTE), zero_addr, false},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        bool ok = !tests[i].ok;
        NetipAddr got = netip_addr_from_slice(tests[i].ip, &ok);
        if (ok != tests[i].ok || !netip_addr_eq(got, tests[i].want))
            testing_t_errorf_v(t, "%d. AddrFromSlice = %v, %v, want %v, %v", i,
                               BURROW_ANY(TYPE_NETIP_ADDR, &got), ok,
                               BURROW_ANY(TYPE_NETIP_ADDR, &tests[i].want),
                               tests[i].ok);
    }
}

static void TestIPv4Constructors(TestingT *t) {
    const Byte b[4] = {1, 2, 3, 4};
    CHECK(netip_addr_eq(netip_addr_from4(b), ip("1.2.3.4")));
}

static void TestAddrAppendText(TestingT *t) {
    struct {
        NetipAddr ip;
        const char *want;
    } tests[] = {
        {zero_addr, ""},
        {ip("1.2.3.4"), "1.2.3.4"},
        {ip("fd7a:115c:a1e0:ab12:4843:cd96:626b:430b"),
         "fd7a:115c:a1e0:ab12:4843:cd96:626b:430b"},
        {ip("::ffff:192.168.140.255"), "::ffff:192.168.140.255"},
        {ip("::ffff:192.168.140.255%en0"), "::ffff:192.168.140.255%en0"},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Error err;
        Slice b = netip_addr_append_text(tests[i].ip, a, four(a), &err);
        CHECK(BURROW_OK(err));
        CHECK_STR_EQ(bz(a, tail4(b)), tests[i].want);
    }
    arena_free(&ar);
}

static void TestAddrMarshalUnmarshalBinary(TestingT *t) {
    static const struct {
        const char *ip;
        Int want_size;
    } tests[] = {
        {"", 0},
        {"1.2.3.4", 4},
        {"fd7a:115c:a1e0:ab12:4843:cd96:626b:430b", 16},
        {"::ffff:c000:0280", 16},
        {"::ffff:c000:0280%eth0", 20},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        NetipAddr x = tests[i].ip[0] ? ip(tests[i].ip) : zero_addr;
        Error err;
        Slice b = netip_addr_marshal_binary(x, a, &err);
        CHECK(BURROW_OK(err));
        CHECK_INT_EQ(b.len, tests[i].want_size);
        NetipAddr x2 = ip("::9");
        CHECK(BURROW_OK(netip_addr_unmarshal_binary(&x2, b)));
        CHECK(netip_addr_eq(x, x2));

        Slice ba = tail4(netip_addr_append_binary(x, a, four(a), &err));
        CHECK(BURROW_OK(err));
        CHECK_INT_EQ(ba.len, tests[i].want_size);
        NetipAddr x3 = zero_addr;
        CHECK(BURROW_OK(netip_addr_unmarshal_binary(&x3, ba)));
        CHECK(netip_addr_eq(x, x3));
    }
    Byte ones[5] = {1, 1, 1, 1, 1};
    for (Int n = 3; n <= 5; n += 2) {
        NetipAddr x2 = zero_addr;
        if (BURROW_OK(
                netip_addr_unmarshal_binary(&x2, slice_from(ones, n, n, TYPE_BYTE))))
            testing_t_errorf_v(t, "unmarshaled from unexpected IP length %d", n);
    }
    arena_free(&ar);
}

static void TestAddrPortMarshalTextString(TestingT *t) {
    struct {
        NetipAddrPort in;
        const char *want;
    } tests[] = {
        {ipp("1.2.3.4:80"), "1.2.3.4:80"},
        {ipp("[::]:80"), "[::]:80"},
        {ipp("[1::CAFE]:80"), "[1::cafe]:80"},
        {ipp("[1::CAFE%en0]:80"), "[1::cafe%en0]:80"},
        {ipp("[::FFFF:192.168.140.255]:80"), "[::ffff:192.168.140.255]:80"},
        {ipp("[::FFFF:192.168.140.255%en0]:80"), "[::ffff:192.168.140.255%en0]:80"},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Error err;
        CHECK_STR_EQ(cz(a, netip_addr_port_string(tests[i].in, a)), tests[i].want);
        Slice mt = netip_addr_port_marshal_text(tests[i].in, a, &err);
        CHECK(BURROW_OK(err));
        CHECK_STR_EQ(bz(a, mt), tests[i].want);
        Slice ap = netip_addr_port_append_text(tests[i].in, a, four(a), &err);
        CHECK(BURROW_OK(err));
        CHECK_STR_EQ(bz(a, tail4(ap)), tests[i].want);
    }
    arena_free(&ar);
}

static void TestAddrPortMarshalUnmarshalBinary(TestingT *t) {
    static const struct {
        const char *ipport;
        Int want_size;
    } tests[] = {
        {"1.2.3.4:51820", 4 + 2},
        {"[fd7a:115c:a1e0:ab12:4843:cd96:626b:430b]:80", 16 + 2},
        {"[::ffff:c000:0280]:65535", 16 + 2},
        {"[::ffff:c000:0280%eth0]:1", 20 + 2},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        NetipAddrPort x = ipp(tests[i].ipport);
        Error err;
        Slice b = netip_addr_port_marshal_binary(x, a, &err);
        CHECK(BURROW_OK(err));
        CHECK_INT_EQ(b.len, tests[i].want_size);
        NetipAddrPort x2 = zero_addr_port;
        CHECK(BURROW_OK(netip_addr_port_unmarshal_binary(&x2, b)));
        CHECK(netip_addr_port_eq(x, x2));

        Slice ba = tail4(netip_addr_port_append_binary(x, a, four(a), &err));
        CHECK(BURROW_OK(err));
        CHECK_INT_EQ(ba.len, tests[i].want_size);
        NetipAddrPort x3 = zero_addr_port;
        CHECK(BURROW_OK(netip_addr_port_unmarshal_binary(&x3, ba)));
        CHECK(netip_addr_port_eq(x, x3));
    }
    Byte ones[7] = {1, 1, 1, 1, 1, 1, 1};
    for (Int n = 3; n <= 7; n += 4) {
        NetipAddrPort x2 = zero_addr_port;
        if (BURROW_OK(netip_addr_port_unmarshal_binary(
                &x2, slice_from(ones, n, n, TYPE_BYTE))))
            testing_t_errorf_v(t, "unmarshaled from unexpected length %d", n);
    }
    arena_free(&ar);
}

static void TestPrefixMarshalTextString(TestingT *t) {
    struct {
        NetipPrefix in;
        const char *want;
    } tests[] = {
        {pfx("1.2.3.4/24"), "1.2.3.4/24"},
        {pfx("fd7a:115c:a1e0:ab12:4843:cd96:626b:430b/118"),
         "fd7a:115c:a1e0:ab12:4843:cd96:626b:430b/118"},
        {pfx("::ffff:c000:0280/96"), "::ffff:192.0.2.128/96"},
        {pfx("::ffff:192.168.140.255/8"), "::ffff:192.168.140.255/8"},
        /* The zone is dropped. */
        {netip_prefix_from(
             netip_addr_with_zone(ip("::ffff:c000:0280"), BURROW_S("eth0")), 37),
         "::ffff:192.0.2.128/37"},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Error err;
        CHECK_STR_EQ(cz(a, netip_prefix_string(tests[i].in, a)), tests[i].want);
        Slice mt = netip_prefix_marshal_text(tests[i].in, a, &err);
        CHECK(BURROW_OK(err));
        CHECK_STR_EQ(bz(a, mt), tests[i].want);
        Slice ap = netip_prefix_append_text(tests[i].in, a, four(a), &err);
        CHECK(BURROW_OK(err));
        CHECK_STR_EQ(bz(a, tail4(ap)), tests[i].want);
    }
    arena_free(&ar);
}

static void TestPrefixMarshalUnmarshalBinary(TestingT *t) {
    struct {
        NetipPrefix prefix;
        Int want_size;
    } tests[6] = {
        {pfx("1.2.3.4/24"), 4 + 1},
        {pfx("fd7a:115c:a1e0:ab12:4843:cd96:626b:430b/118"), 16 + 1},
        {pfx("::ffff:c000:0280/96"), 16 + 1},
        {netip_prefix_from(
             netip_addr_with_zone(ip("::ffff:c000:0280"), BURROW_S("eth0")), 37),
         16 + 1},
    };
    tests[4].prefix = netip_prefix_from(netip_prefix_addr(tests[0].prefix), 33);
    tests[4].want_size = tests[0].want_size;
    tests[5].prefix = netip_prefix_from(netip_prefix_addr(tests[1].prefix), 129);
    tests[5].want_size = tests[1].want_size;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        NetipPrefix p = tests[i].prefix;
        Error err;
        Slice b = netip_prefix_marshal_binary(p, a, &err);
        CHECK(BURROW_OK(err));
        CHECK_INT_EQ(b.len, tests[i].want_size);
        NetipPrefix p2 = zero_prefix;
        CHECK(BURROW_OK(netip_prefix_unmarshal_binary(&p2, b)));
        if (!netip_prefix_eq(p, p2))
            testing_t_errorf_v(t, "%d. got %v; want %v", i,
                               BURROW_ANY(TYPE_NETIP_PREFIX, &p2),
                               BURROW_ANY(TYPE_NETIP_PREFIX, &p));

        Slice ba = tail4(netip_prefix_append_binary(p, a, four(a), &err));
        CHECK(BURROW_OK(err));
        CHECK_INT_EQ(ba.len, tests[i].want_size);
        NetipPrefix p3 = zero_prefix;
        CHECK(BURROW_OK(netip_prefix_unmarshal_binary(&p3, ba)));
        CHECK(netip_prefix_eq(p, p3));
    }
    Byte ones[6] = {1, 1, 1, 1, 1, 1};
    for (Int n = 3; n <= 6; n += 3) {
        NetipPrefix p2 = zero_prefix;
        if (BURROW_OK(
                netip_prefix_unmarshal_binary(&p2, slice_from(ones, n, n, TYPE_BYTE))))
            testing_t_errorf_v(t, "unmarshaled from unexpected length %d", n);
    }
    arena_free(&ar);
}

static void TestAddrMarshalUnmarshal(TestingT *t) {
    NetipAddr x = ip("1.2.3.4");
    CHECK(BURROW_OK(netip_addr_unmarshal_text(&x, text(""))));
    CHECK(netip_addr_eq(x, zero_addr));
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err;
    Slice b = netip_addr_marshal_text(x, a, &err);
    CHECK(BURROW_OK(err));
    CHECK_INT_EQ(b.len, 0);
    arena_free(&ar);
}

static void TestAddrFrom16(TestingT *t) {
    const Byte v6raw[16] = {[15] = 1};
    const Byte v4raw[16] = {
        [10] = 0xff, [11] = 0xff, [12] = 1, [13] = 2, [14] = 3, [15] = 4};
    CHECK(netip_addr_eq(netip_addr_from16(v6raw), mk(0, 1, "")));
    CHECK(netip_addr_eq(netip_addr_from16(v4raw), mk(0, 0xffff01020304, "")));
}

static NetipAddr from16(NetipAddr x) {
    return netip_addr_from16(netip_addr_as16(x).a);
}

static void TestIPProperties(TestingT *t) {
    NetipAddr unicast4 = ip("192.0.2.1");
    NetipAddr multicast4 = ip("224.0.0.1");
    NetipAddr llu4 = ip("169.254.0.1");
    struct {
        const char *name;
        NetipAddr ip;
        bool global_unicast, interface_local_multicast, link_local_multicast,
            link_local_unicast, loopback, multicast, private_, unspecified;
    } tests[] = {
        {"nil", zero_addr, 0, 0, 0, 0, 0, 0, 0, 0},
        {"unicast v4Addr", unicast4, 1, 0, 0, 0, 0, 0, 0, 0},
        {"unicast v6 mapped v4Addr", from16(unicast4), 1, 0, 0, 0, 0, 0, 0, 0},
        {"unicast v6Addr", ip("2001:db8::1"), 1, 0, 0, 0, 0, 0, 0, 0},
        {"unicast v6AddrZone", ip("2001:db8::1%eth0"), 1, 0, 0, 0, 0, 0, 0, 0},
        {"unicast v6Addr unassigned", ip("4000::1"), 1, 0, 0, 0, 0, 0, 0, 0},
        {"multicast v4Addr", multicast4, 0, 0, 1, 0, 0, 1, 0, 0},
        {"multicast v6 mapped v4Addr", from16(multicast4), 0, 0, 1, 0, 0, 1, 0, 0},
        {"multicast v6Addr", ip("ff02::1"), 0, 0, 1, 0, 0, 1, 0, 0},
        {"multicast v6AddrZone", ip("ff02::1%eth0"), 0, 0, 1, 0, 0, 1, 0, 0},
        {"link-local unicast v4Addr", llu4, 0, 0, 0, 1, 0, 0, 0, 0},
        {"link-local unicast v6 mapped v4Addr", from16(llu4), 0, 0, 0, 1, 0, 0, 0, 0},
        {"link-local unicast v6Addr", ip("fe80::1"), 0, 0, 0, 1, 0, 0, 0, 0},
        {"link-local unicast v6Addr upper bound",
         ip("febf:ffff:ffff:ffff:ffff:ffff:ffff:ffff"), 0, 0, 0, 1, 0, 0, 0, 0},
        {"link-local unicast v6AddrZone", ip("fe80::1%eth0"), 0, 0, 0, 1, 0, 0, 0, 0},
        {"loopback v4Addr", ip("127.0.0.1"), 0, 0, 0, 0, 1, 0, 0, 0},
        {"loopback v6Addr", netip_ipv6_loopback(), 0, 0, 0, 0, 1, 0, 0, 0},
        {"loopback v6 mapped v4Addr", from16(netip_ipv6_loopback()), 0, 0, 0, 0, 1, 0,
         0, 0},
        {"interface-local multicast v6Addr", ip("ff01::1"), 0, 1, 0, 0, 0, 1, 0, 0},
        {"interface-local multicast v6AddrZone", ip("ff01::1%eth0"), 0, 1, 0, 0, 0, 1,
         0, 0},
        {"private v4Addr 10/8", ip("10.0.0.1"), 1, 0, 0, 0, 0, 0, 1, 0},
        {"private v4Addr 172.16/12", ip("172.16.0.1"), 1, 0, 0, 0, 0, 0, 1, 0},
        {"private v4Addr 192.168/16", ip("192.168.1.1"), 1, 0, 0, 0, 0, 0, 1, 0},
        {"private v6Addr", ip("fd00::1"), 1, 0, 0, 0, 0, 0, 1, 0},
        {"private v6 mapped v4Addr 10/8", ip("::ffff:10.0.0.1"), 1, 0, 0, 0, 0, 0, 1,
         0},
        {"private v6 mapped v4Addr 172.16/12", ip("::ffff:172.16.0.1"), 1, 0, 0, 0, 0,
         0, 1, 0},
        {"private v6 mapped v4Addr 192.168/16", ip("::ffff:192.168.1.1"), 1, 0, 0, 0, 0,
         0, 1, 0},
        {"unspecified v4Addr", netip_ipv4_unspecified(), 0, 0, 0, 0, 0, 0, 0, 1},
        {"unspecified v6Addr", netip_ipv6_unspecified(), 0, 0, 0, 0, 0, 0, 0, 1},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        NetipAddr x = tests[i].ip;
        const char *n = tests[i].name;
        if (netip_addr_is_global_unicast(x) != tests[i].global_unicast)
            testing_t_errorf_v(t, "%s: IsGlobalUnicast = %v", n,
                               !tests[i].global_unicast);
        if (netip_addr_is_interface_local_multicast(x) !=
            tests[i].interface_local_multicast)
            testing_t_errorf_v(t, "%s: IsInterfaceLocalMulticast = %v", n,
                               !tests[i].interface_local_multicast);
        if (netip_addr_is_link_local_unicast(x) != tests[i].link_local_unicast)
            testing_t_errorf_v(t, "%s: IsLinkLocalUnicast = %v", n,
                               !tests[i].link_local_unicast);
        if (netip_addr_is_link_local_multicast(x) != tests[i].link_local_multicast)
            testing_t_errorf_v(t, "%s: IsLinkLocalMulticast = %v", n,
                               !tests[i].link_local_multicast);
        if (netip_addr_is_loopback(x) != tests[i].loopback)
            testing_t_errorf_v(t, "%s: IsLoopback = %v", n, !tests[i].loopback);
        if (netip_addr_is_multicast(x) != tests[i].multicast)
            testing_t_errorf_v(t, "%s: IsMulticast = %v", n, !tests[i].multicast);
        if (netip_addr_is_private(x) != tests[i].private_)
            testing_t_errorf_v(t, "%s: IsPrivate = %v", n, !tests[i].private_);
        if (netip_addr_is_unspecified(x) != tests[i].unspecified)
            testing_t_errorf_v(t, "%s: IsUnspecified = %v", n, !tests[i].unspecified);
    }
}

static void TestAddrWellKnown(TestingT *t) {
    struct {
        NetipAddr ip;
        const char *want;
    } tests[] = {
        {netip_ipv4_unspecified(), "0.0.0.0"},
        {netip_ipv6_link_local_all_nodes(), "ff02::1"},
        {netip_ipv6_link_local_all_routers(), "ff02::2"},
        {netip_ipv6_loopback(), "::1"},
        {netip_ipv6_unspecified(), "::"},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++)
        CHECK_STR_EQ(cz(a, netip_addr_string(tests[i].ip, a)), tests[i].want);
    arena_free(&ar);
}

static void TestAddrLessCompare(TestingT *t) {
    struct {
        NetipAddr a, b;
        bool want;
    } tests[] = {
        {zero_addr, zero_addr, false},
        {zero_addr, ip("1.2.3.4"), true},
        {ip("1.2.3.4"), zero_addr, false},

        {ip("1.2.3.4"), ip("0102:0304::0"), true},
        {ip("0102:0304::0"), ip("1.2.3.4"), false},
        {ip("1.2.3.4"), ip("1.2.3.4"), false},

        {ip("::1"), ip("::2"), true},
        {ip("::1"), ip("::1%foo"), true},
        {ip("::1%foo"), ip("::2"), true},
        {ip("::2"), ip("::3"), true},

        {ip("::"), ip("0.0.0.0"), false},
        {ip("0.0.0.0"), ip("::"), true},

        {ip("::1%a"), ip("::1%b"), true},
        {ip("::1%a"), ip("::1%a"), false},
        {ip("::1%b"), ip("::1%a"), false},

        {ip("::ffff:11.1.1.12"), ip("11.1.1.12"), false},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        NetipAddr x = tests[i].a, y = tests[i].b;
        bool got = netip_addr_less(x, y);
        if (got != tests[i].want)
            testing_t_errorf_v(t, "Less(%v, %v) = %v; want %v",
                               BURROW_ANY(TYPE_NETIP_ADDR, &x),
                               BURROW_ANY(TYPE_NETIP_ADDR, &y), got, tests[i].want);
        Int cmp = netip_addr_compare(x, y);
        if (got && cmp != -1)
            testing_t_errorf_v(t, "%d. Less is true, but Compare = %v", i, cmp);
        if (cmp < -1 || cmp > 1)
            testing_t_errorf_v(t, "bogus Compare return value %v", cmp);
        if (cmp == 0 && !netip_addr_eq(x, y))
            testing_t_errorf_v(t, "%d. Compare = 0; but not equal", i);
        if (cmp == 1 && !netip_addr_less(y, x))
            testing_t_errorf_v(t, "%d. Compare = 1; but b.Less(a) isn't true", i);
        if (got == tests[i].want && got && netip_addr_less(y, x))
            testing_t_errorf_v(t, "%d. Less was true both ways", i);
    }
}

/* Sorts n values of size sz with an insertion sort, for the sort checks,
 * then prints them the way fmt's %s prints a slice. */
typedef int (*CmpFn)(const void *, const void *);

static Str sort_and_print(Alloc *a, void *vals, Int n, size_t sz, CmpFn cmp,
                          const Type *typ) {
    Byte *base = vals, tmp[64];
    for (Int i = 1; i < n; i++)
        for (Int j = i;
             j > 0 && cmp(base + (size_t)j * sz, base + (size_t)(j - 1) * sz) < 0;
             j--) {
            memcpy(tmp, base + (size_t)j * sz, sz);
            memcpy(base + (size_t)j * sz, base + (size_t)(j - 1) * sz, sz);
            memcpy(base + (size_t)(j - 1) * sz, tmp, sz);
        }
    StringsBuilder sb = STRINGS_BUILDER(a);
    strings_builder_write_byte(&sb, '[');
    for (Int i = 0; i < n; i++) {
        if (i > 0)
            strings_builder_write_byte(&sb, ' ');
        Str s = fmt_sprintf_v(a, "%s", BURROW_ANY(typ, base + (size_t)i * sz));
        strings_builder_write_string(&sb, s, NULL);
    }
    strings_builder_write_byte(&sb, ']');
    return strings_builder_string(&sb);
}

static int cmp_addr(const void *x, const void *y) {
    return (int)netip_addr_compare(*(const NetipAddr *)x, *(const NetipAddr *)y);
}

static int cmp_addr_port(const void *x, const void *y) {
    return (int)netip_addr_port_compare(*(const NetipAddrPort *)x,
                                        *(const NetipAddrPort *)y);
}

static int cmp_prefix(const void *x, const void *y) {
    return (int)netip_prefix_compare(*(const NetipPrefix *)x, *(const NetipPrefix *)y);
}

static void TestAddrSort(TestingT *t) {
    NetipAddr values[] = {ip("::1"),     ip("::2"),     zero_addr,
                          ip("1.2.3.4"), ip("8.8.8.8"), ip("::1%foo")};
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str got = sort_and_print(a, values, 6, sizeof values[0], cmp_addr, TYPE_NETIP_ADDR);
    CHECK_STR_EQ(cz(a, got), "[invalid IP 1.2.3.4 8.8.8.8 ::1 ::1%foo ::2]");
    arena_free(&ar);
}

static void TestAddrPortCompare(TestingT *t) {
    struct {
        NetipAddrPort a, b;
        int want;
    } tests[] = {
        {zero_addr_port, zero_addr_port, 0},
        {zero_addr_port, ipp("1.2.3.4:80"), -1},

        {ipp("1.2.3.4:80"), ipp("1.2.3.4:80"), 0},
        {ipp("[::1]:80"), ipp("[::1]:80"), 0},

        {ipp("1.2.3.4:80"), ipp("2.3.4.5:22"), -1},
        {ipp("[::1]:80"), ipp("[::2]:22"), -1},

        {ipp("1.2.3.4:80"), ipp("1.2.3.4:443"), -1},
        {ipp("[::1]:80"), ipp("[::1]:443"), -1},

        {ipp("1.2.3.4:80"), ipp("[0102:0304::0]:80"), -1},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Int got = netip_addr_port_compare(tests[i].a, tests[i].b);
        CHECK_INT_EQ(got, tests[i].want);
        CHECK_INT_EQ(netip_addr_port_compare(tests[i].b, tests[i].a), -tests[i].want);
    }

    NetipAddrPort values[] = {ipp("[::1]:80"),     ipp("[::2]:80"),
                              zero_addr_port,      ipp("1.2.3.4:443"),
                              ipp("8.8.8.8:8080"), ipp("[::1%foo]:1024")};
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str got = sort_and_print(a, values, 6, sizeof values[0], cmp_addr_port,
                             TYPE_NETIP_ADDR_PORT);
    CHECK_STR_EQ(cz(a, got), "[invalid AddrPort 1.2.3.4:443 8.8.8.8:8080 [::1]:80 "
                             "[::1%foo]:1024 [::2]:80]");
    arena_free(&ar);
}

static void TestPrefixCompare(TestingT *t) {
    struct {
        NetipPrefix a, b;
        int want;
    } tests[] = {
        {zero_prefix, zero_prefix, 0},
        {zero_prefix, pfx("1.2.3.0/24"), -1},

        {pfx("1.2.3.0/24"), pfx("1.2.3.0/24"), 0},
        {pfx("fe80::/64"), pfx("fe80::/64"), 0},

        {pfx("1.2.3.0/24"), pfx("1.2.4.0/24"), -1},
        {pfx("fe80::/64"), pfx("fe90::/64"), -1},

        {pfx("1.2.0.0/16"), pfx("1.2.0.0/24"), -1},
        {pfx("fe80::/48"), pfx("fe80::/64"), -1},

        {pfx("1.2.3.0/24"), pfx("fe80::/8"), -1},

        {pfx("1.2.3.0/24"), pfx("1.2.3.4/24"), -1},
        {pfx("1.2.3.0/24"), pfx("1.2.3.0/28"), -1},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        CHECK_INT_EQ(netip_prefix_compare(tests[i].a, tests[i].b), tests[i].want);
        CHECK_INT_EQ(netip_prefix_compare(tests[i].b, tests[i].a), -tests[i].want);
    }

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    NetipPrefix values[] = {
        pfx("1.2.3.0/24"), pfx("fe90::/64"),  pfx("fe80::/64"),
        pfx("1.2.0.0/16"), zero_prefix,       pfx("fe80::/48"),
        pfx("1.2.0.0/24"), pfx("1.2.3.4/24"), pfx("1.2.3.0/28"),
    };
    Str got = sort_and_print(a, values, sizeof values / sizeof values[0],
                             sizeof values[0], cmp_prefix, TYPE_NETIP_PREFIX);
    CHECK_STR_EQ(cz(a, got),
                 "[invalid Prefix 1.2.0.0/16 1.2.0.0/24 1.2.3.0/24 1.2.3.4/24 "
                 "1.2.3.0/28 fe80::/48 fe80::/64 fe90::/64]");

    /* IANA's lists, to check the order matches the one they use. */
    static const char *const iana[] = {
        "0.0.0.0/8",       "127.0.0.0/8",     "10.0.0.0/8",     "203.0.113.0/24",
        "169.254.0.0/16",  "192.0.0.0/24",    "240.0.0.0/4",    "192.0.2.0/24",
        "192.0.0.170/32",  "198.18.0.0/15",   "192.0.0.8/32",   "0.0.0.0/32",
        "192.0.0.9/32",    "198.51.100.0/24", "192.168.0.0/16", "192.0.0.10/32",
        "192.175.48.0/24", "192.52.193.0/24", "100.64.0.0/10",  "255.255.255.255/32",
        "192.31.196.0/24", "172.16.0.0/12",   "192.0.0.0/29",   "192.88.99.0/24",
        "fec0::/10",       "6000::/3",        "fe00::/9",       "8000::/3",
        "0000::/8",        "0400::/6",        "f800::/6",       "e000::/4",
        "ff00::/8",        "a000::/3",        "fc00::/7",       "1000::/4",
        "0800::/5",        "4000::/3",        "0100::/8",       "c000::/3",
        "fe80::/10",       "0200::/7",        "f000::/5",       "2000::/3",
    };
    enum { NIANA = sizeof iana / sizeof iana[0] };
    NetipPrefix vs[NIANA];
    for (Int i = 0; i < NIANA; i++)
        vs[i] = pfx(iana[i]);
    got = sort_and_print(a, vs, NIANA, sizeof vs[0], cmp_prefix, TYPE_NETIP_PREFIX);
    CHECK_STR_EQ(
        cz(a, got),
        "[0.0.0.0/8 0.0.0.0/32 10.0.0.0/8 100.64.0.0/10 127.0.0.0/8 169.254.0.0/16 "
        "172.16.0.0/12 192.0.0.0/24 192.0.0.0/29 192.0.0.8/32 192.0.0.9/32 "
        "192.0.0.10/32 "
        "192.0.0.170/32 192.0.2.0/24 192.31.196.0/24 192.52.193.0/24 192.88.99.0/24 "
        "192.168.0.0/16 192.175.48.0/24 198.18.0.0/15 198.51.100.0/24 203.0.113.0/24 "
        "240.0.0.0/4 255.255.255.255/32 ::/8 100::/8 200::/7 400::/6 800::/5 1000::/4 "
        "2000::/3 4000::/3 6000::/3 8000::/3 a000::/3 c000::/3 e000::/4 f000::/5 "
        "f800::/6 "
        "fc00::/7 fe00::/9 fe80::/10 fec0::/10 ff00::/8]");
    arena_free(&ar);
}

static void TestIPStringExpanded(TestingT *t) {
    struct {
        NetipAddr ip;
        const char *s;
    } tests[] = {
        {zero_addr, "invalid IP"},
        {ip("192.0.2.1"), "192.0.2.1"},
        {ip("::ffff:192.0.2.1"), "0000:0000:0000:0000:0000:ffff:c000:0201"},
        {ip("2001:db8::1"), "2001:0db8:0000:0000:0000:0000:0000:0001"},
        {ip("2001:db8::1%eth0"), "2001:0db8:0000:0000:0000:0000:0000:0001%eth0"},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++)
        CHECK_STR_EQ(cz(a, netip_addr_string_expanded(tests[i].ip, a)), tests[i].s);
    arena_free(&ar);
}

typedef struct MaskCase {
    NetipAddr ip;
    Int bits;
    NetipPrefix p;
    bool ok;
} MaskCase;

static void check_masking(TestingT *t, Alloc *a, const MaskCase *c, Int n) {
    for (Int i = 0; i < n; i++) {
        Str orig = netip_addr_string(c[i].ip, a);
        Error err;
        NetipPrefix p = netip_addr_prefix(c[i].ip, c[i].bits, &err);
        if (c[i].ok && BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%s/%d: failed to produce prefix: %s", orig,
                               c[i].bits, error_text(err));
            continue;
        }
        if (!c[i].ok && BURROW_OK(err)) {
            testing_t_errorf_v(t, "%s/%d: expected an error, but none occurred", orig,
                               c[i].bits);
            continue;
        }
        if (BURROW_FAILED(err))
            continue;
        NetipPrefix want = c[i].p;
        if (!netip_prefix_eq(p, want))
            testing_t_errorf_v(t, "prefix = %v, want %v",
                               BURROW_ANY(TYPE_NETIP_PREFIX, &p),
                               BURROW_ANY(TYPE_NETIP_PREFIX, &want));
        CHECK(str_eq(netip_addr_string(c[i].ip, a), orig));
    }
}

static void check_masking6(TestingT *t, Alloc *a, const char *zone) {
    NetipAddr ips[] = {ip("2001:db8::1"), ip("2001:db8::1"),
                       ip("fe80::dead:beef:dead:beef"), ip("aaaa::"), ip("::")};
    for (size_t i = 0; i < sizeof ips / sizeof ips[0]; i++)
        ips[i] = netip_addr_with_zone(ips[i], str_from_cstr(zone));
    MaskCase c[] = {
        {ips[0], 255, zero_prefix, false},
        {ips[1], 32, pfx("2001:db8::/32"), true},
        {ips[2], 96, pfx("fe80::dead:beef:0:0/96"), true},
        {ips[3], 4, pfx("a000::/4"), true},
        {ips[4], 63, pfx("::/63"), true},
    };
    check_masking(t, a, c, 5);
}

static void TestPrefixMasking(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    MaskCase nil[] = {
        {zero_addr, 255, zero_prefix, true},
        {zero_addr, 16, zero_prefix, true},
    };
    check_masking(t, a, nil, 2);
    MaskCase v4[] = {
        {ip("192.0.2.0"), 255, zero_prefix, false},
        {ip("192.0.2.0"), 16, pfx("192.0.0.0/16"), true},
        {ip("255.255.255.255"), 20, pfx("255.255.240.0/20"), true},
        /* A byte with ones and zeros on both sides of the mask limit. */
        {ip("100.98.156.66"), 10, pfx("100.64.0.0/10"), true},
    };
    check_masking(t, a, v4, 4);
    check_masking6(t, a, "");
    check_masking6(t, a, "eth0");
    arena_free(&ar);
}

static void TestPrefixMarshalUnmarshal(TestingT *t) {
    static const char *const tests[] = {
        "", "1.2.3.4/32", "0.0.0.0/0", "::/0", "::1/128", "2001:db8::/32",
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        NetipPrefix p = zero_prefix;
        Error err = netip_prefix_unmarshal_text(&p, text(tests[i]));
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "failed to unmarshal %q: %s", tests[i],
                               error_text(err));
            continue;
        }
        Slice b = netip_prefix_marshal_text(p, a, &err);
        CHECK(BURROW_OK(err));
        CHECK_STR_EQ(bz(a, b), tests[i]);
    }
    arena_free(&ar);
}

static void TestPrefixUnmarshalTextNonZero(TestingT *t) {
    NetipPrefix p = pfx("fe80::/64");
    CHECK(BURROW_FAILED(netip_prefix_unmarshal_text(&p, text("xxx"))));
}

static void TestIs4AndIs6(TestingT *t) {
    struct {
        NetipAddr ip;
        bool is4, is6;
    } tests[] = {
        {zero_addr, false, false},
        {ip("1.2.3.4"), true, false},
        {ip("127.0.0.2"), true, false},
        {ip("::1"), false, true},
        {ip("::ffff:192.0.2.128"), false, true},
        {ip("::fffe:c000:0280"), false, true},
        {ip("::1%eth0"), false, true},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        if (netip_addr_is4(tests[i].ip) != tests[i].is4)
            testing_t_errorf_v(t, "%d. Is4 = %v", i, !tests[i].is4);
        if (netip_addr_is6(tests[i].ip) != tests[i].is6)
            testing_t_errorf_v(t, "%d. Is6 = %v", i, !tests[i].is6);
    }
}

static void TestIs4In6(TestingT *t) {
    struct {
        NetipAddr ip;
        bool want;
        NetipAddr unmap;
    } tests[] = {
        {zero_addr, false, zero_addr},
        {ip("::ffff:c000:0280"), true, ip("192.0.2.128")},
        {ip("::ffff:192.0.2.128"), true, ip("192.0.2.128")},
        {ip("::ffff:192.0.2.128%eth0"), true, ip("192.0.2.128")},
        {ip("::fffe:c000:0280"), false, ip("::fffe:c000:0280")},
        {ip("::ffff:127.1.2.3"), true, ip("127.1.2.3")},
        {ip("::ffff:7f01:0203"), true, ip("127.1.2.3")},
        {ip("0:0:0:0:0000:ffff:127.1.2.3"), true, ip("127.1.2.3")},
        {ip("0:0:0:0::ffff:127.1.2.3"), true, ip("127.1.2.3")},
        {ip("::1"), false, ip("::1")},
        {ip("1.2.3.4"), false, ip("1.2.3.4")},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        if (netip_addr_is4_in6(tests[i].ip) != tests[i].want)
            testing_t_errorf_v(t, "%d. Is4In6 = %v", i, !tests[i].want);
        NetipAddr u = netip_addr_unmap(tests[i].ip);
        if (!netip_addr_eq(u, tests[i].unmap))
            testing_t_errorf_v(t, "%d. Unmap = %v; want %v", i,
                               BURROW_ANY(TYPE_NETIP_ADDR, &u),
                               BURROW_ANY(TYPE_NETIP_ADDR, &tests[i].unmap));
    }
}

static void TestPrefixMasked(TestingT *t) {
    struct {
        NetipPrefix prefix, masked;
    } tests[] = {
        {pfx("192.168.0.255/24"), pfx("192.168.0.0/24")},
        {pfx("2100::/3"), pfx("2000::/3")},
        {netip_prefix_from(ip("2000::"), 129), zero_prefix},
        {netip_prefix_from(ip("1.2.3.4"), 33), zero_prefix},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        NetipPrefix got = netip_prefix_masked(tests[i].prefix);
        if (!netip_prefix_eq(got, tests[i].masked))
            testing_t_errorf_v(t, "Masked=%v, want %v",
                               BURROW_ANY(TYPE_NETIP_PREFIX, &got),
                               BURROW_ANY(TYPE_NETIP_PREFIX, &tests[i].masked));
    }
}

static void TestPrefix(TestingT *t) {
    static const struct {
        const char *prefix;
        const char *ip;
        Int bits;
        const char *contains[6];
        const char *not_contains[3]; /* "" is the zero Addr */
    } tests[] = {
        {"192.168.0.0/24",
         "192.168.0.0",
         24,
         {"192.168.0.1", "192.168.0.55"},
         {"192.168.1.1", "1.1.1.1"}},
        {"192.168.1.1/32", "192.168.1.1", 32, {"192.168.1.1"}, {"192.168.1.2"}},
        {"100.64.0.0/10",
         "100.64.0.0",
         10,
         {"100.64.0.0", "100.64.0.1", "100.81.251.94", "100.100.100.100",
          "100.127.255.254", "100.127.255.255"},
         {"100.63.255.255", "100.128.0.0"}},
        {"2001:db8::/96",
         "2001:db8::",
         96,
         {"2001:db8::aaaa:bbbb", "2001:db8::1"},
         {"2001:db8::1:aaaa:bbbb", "2001:db9::"}},
        {"0.0.0.0/0", "0.0.0.0", 0, {"192.168.0.1", "1.1.1.1"}, {"2001:db8::1", ""}},
        {"::/0", "::", 0, {"::1", "2001:db8::1"}, {"192.0.2.1"}},
        {"2000::/3", "2000::", 3, {"2001:db8::1"}, {"fe80::1"}},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Error err;
        NetipPrefix p = netip_parse_prefix(str_from_cstr(tests[i].prefix), &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%s: %s", tests[i].prefix, error_text(err));
            continue;
        }
        CHECK(netip_addr_eq(netip_prefix_addr(p), ip(tests[i].ip)));
        CHECK_INT_EQ(netip_prefix_bits(p), tests[i].bits);
        for (size_t j = 0; j < 6 && tests[i].contains[j] != NULL; j++)
            if (!netip_prefix_contains(p, ip(tests[i].contains[j])))
                testing_t_errorf_v(t, "%s does not contain %s", tests[i].prefix,
                                   tests[i].contains[j]);
        for (size_t j = 0; j < 3 && tests[i].not_contains[j] != NULL; j++) {
            const char *s = tests[i].not_contains[j];
            if (netip_prefix_contains(p, *s ? ip(s) : zero_addr))
                testing_t_errorf_v(t, "%s contains %q", tests[i].prefix, s);
        }
        CHECK_STR_EQ(cz(a, netip_prefix_string(p, a)), tests[i].prefix);
        check_append_to_prefix(t, a, p);
    }
    arena_free(&ar);
}

static void TestPrefixFromInvalidBits(TestingT *t) {
    NetipAddr v4 = ip("1.2.3.4"), v6 = ip("66::66");
    struct {
        NetipAddr ip;
        Int in, want;
    } tests[] = {
        {v4, 0, 0},     {v6, 0, 0},     {v4, 1, 1},    {v4, 33, -1},  {v6, 33, 33},
        {v6, 127, 127}, {v6, 128, 128}, {v4, 254, -1}, {v4, 255, -1}, {v4, -1, -1},
        {v6, -1, -1},   {v4, -5, -1},   {v6, -5, -1},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        NetipPrefix p = netip_prefix_from(tests[i].ip, tests[i].in);
        if (netip_prefix_bits(p) != tests[i].want)
            testing_t_errorf_v(t, "for (%v, %v), Bits out = %v; want %v",
                               BURROW_ANY(TYPE_NETIP_ADDR, &tests[i].ip), tests[i].in,
                               netip_prefix_bits(p), tests[i].want);
    }
}

static uint64_t heap_allocs(void) {
    uint64_t n = 0, bytes = 0;
    burrow__heap_counts(&n, &bytes);
    return n;
}

static void TestParsePrefixAllocs(TestingT *t) {
    static const char *const tests[] = {"192.168.1.0/24", "aaaa:bbbb:cccc::/24"};
    burrow__heap_count(true);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        uint64_t before = heap_allocs();
        for (int j = 0; j < 5; j++)
            (void)netip_parse_prefix(str_from_cstr(tests[i]), NULL);
        uint64_t n = heap_allocs() - before;
        if (n != 0)
            testing_t_errorf_v(t, "%s: allocs=%v, want 0", tests[i], n);
    }
    burrow__heap_count(false);
}

static void TestParsePrefixError(TestingT *t) {
    static const struct {
        const char *prefix;
        const char *errstr;
    } tests[] = {
        {"192.168.0.0", "no '/'"},
        {"1.257.1.1/24", "value >255"},
        {"1.1.1.0/q", "bad bits"},
        {"1.1.1.0/-1", "bad bits"},
        {"1.1.1.0/33", "out of range"},
        {"2001::/129", "out of range"},
        {"1.1.1.0%a/24", "unexpected character"},
        {"2001:db8::%a/32", "zones cannot be present"},
        {"1.1.1.0/+32", "bad bits"},
        {"1.1.1.0/-32", "bad bits"},
        {"1.1.1.0/032", "bad bits"},
        {"1.1.1.0/0032", "bad bits"},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Error err;
        (void)netip_parse_prefix(str_from_cstr(tests[i].prefix), &err);
        if (BURROW_OK(err)) {
            testing_t_errorf_v(t, "%s: no error", tests[i].prefix);
            continue;
        }
        if (!strings_contains(error_text(err), str_from_cstr(tests[i].errstr)))
            testing_t_errorf_v(t, "error is missing substring %q: %s", tests[i].errstr,
                               error_text(err));
    }
}

static void TestPrefixIsSingleIP(TestingT *t) {
    struct {
        NetipPrefix p;
        bool want;
    } tests[] = {
        {pfx("127.0.0.1/32"), true}, {pfx("127.0.0.1/31"), false},
        {pfx("127.0.0.1/0"), false}, {pfx("::1/128"), true},
        {pfx("::1/127"), false},     {pfx("::1/0"), false},
        {zero_prefix, false},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++)
        if (netip_prefix_is_single_ip(tests[i].p) != tests[i].want)
            testing_t_errorf_v(t, "IsSingleIP(%v) = %v want %v",
                               BURROW_ANY(TYPE_NETIP_PREFIX, &tests[i].p),
                               !tests[i].want, tests[i].want);
}

static void TestAs4(TestingT *t) {
    struct {
        NetipAddr ip;
        Byte want[4];
        bool want_panic;
    } tests[] = {
        {ip("1.2.3.4"), {1, 2, 3, 4}, false},
        {from16(ip("1.2.3.4")), {1, 2, 3, 4}, false},
        {ip("0.0.0.0"), {0, 0, 0, 0}, false},
        {zero_addr, {0}, true},
        {ip("::1"), {0}, true},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        volatile bool panicked = false;
        volatile bool wrong = false;
        BURROW_TRY {
            NetipAddrAs4Ret got = netip_addr_as4(tests[i].ip);
            wrong = memcmp(got.a, tests[i].want, 4) != 0;
        }
        BURROW_CATCH(r) {
            (void)r;
            panicked = true;
        }
        BURROW_TRY_END;
        if (panicked != tests[i].want_panic) {
            testing_t_errorf_v(t, "%d. panic = %v; want %v", i, (bool)panicked,
                               tests[i].want_panic);
            continue;
        }
        if (wrong)
            testing_t_errorf_v(t, "%d. As4 is wrong", i);
    }
}

static void TestPrefixOverlaps(TestingT *t) {
    struct {
        NetipPrefix a, b;
        bool want;
    } tests[] = {
        {zero_prefix, pfx("1.2.0.0/16"), false},
        {pfx("1.2.0.0/16"), zero_prefix, false},
        {pfx("::0/3"), pfx("0.0.0.0/3"), false},

        {pfx("1.2.0.0/16"), pfx("1.2.0.0/16"), true},

        {pfx("1.2.0.0/16"), pfx("1.2.3.0/24"), true},
        {pfx("1.2.3.0/24"), pfx("1.2.0.0/16"), true},

        {pfx("1.2.0.0/16"), pfx("1.2.3.0/32"), true},
        {pfx("1.2.3.0/32"), pfx("1.2.0.0/16"), true},

        {pfx("1.2.3.0/32"), pfx("0.0.0.0/0"), true},
        {pfx("0.0.0.0/0"), pfx("1.2.3.0/32"), true},

        {pfx("1.2.3.0/32"), pfx("5.5.5.5/0"), true},

        {pfx("5::1/128"), pfx("5::0/8"), true},
        {pfx("5::0/8"), pfx("5::1/128"), true},

        {pfx("1::1/128"), pfx("2::2/128"), false},
        {pfx("0100::0/8"), pfx("::1/128"), false},

        {netip_prefix_from(from16(ip("1.2.0.0")), 16), pfx("1.2.3.0/24"), false},

        {netip_prefix_from(ip("1.2.3.4"), 33), pfx("1.2.3.0/24"), false},
        {netip_prefix_from(ip("2000::"), 129), pfx("2000::/64"), false},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        if (netip_prefix_overlaps(tests[i].a, tests[i].b) != tests[i].want)
            testing_t_errorf_v(t, "%d. (%v).Overlaps(%v) = %v", i,
                               BURROW_ANY(TYPE_NETIP_PREFIX, &tests[i].a),
                               BURROW_ANY(TYPE_NETIP_PREFIX, &tests[i].b),
                               !tests[i].want);
        if (netip_prefix_overlaps(tests[i].b, tests[i].a) != tests[i].want)
            testing_t_errorf_v(t, "%d. (%v).Overlaps(%v) = %v", i,
                               BURROW_ANY(TYPE_NETIP_PREFIX, &tests[i].b),
                               BURROW_ANY(TYPE_NETIP_PREFIX, &tests[i].a),
                               !tests[i].want);
    }
}

/* Go's TestNoAllocs. Everything but the String and marshal calls is done
 * without the heap. */
static volatile bool sink_bool;
static volatile uint64_t sink_u64;

static void no_allocs_work(void) {
    const Byte four_bytes[4] = {1, 2, 3, 4};
    const Byte sixteen[16] = {0};
    NetipAddr x = netip_addr_from4(four_bytes);
    x = netip_addr_from16(sixteen);
    x = netip_parse_addr(BURROW_S("1.2.3.4"), NULL);
    x = netip_parse_addr(BURROW_S("::1"), NULL);
    x = netip_must_parse_addr(BURROW_S("1.2.3.4"));
    x = netip_ipv6_link_local_all_nodes();
    x = netip_ipv6_link_local_all_routers();
    x = netip_ipv6_loopback();
    x = netip_ipv6_unspecified();
    sink_bool = netip_addr_bit_len(ip("1.2.3.4")) == 8;
    sink_bool = netip_addr_zone(ip("fe80::1%zone")).len == 0;
    sink_bool = netip_addr_compare(ip("1.2.3.4"), ip("2.3.4.5")) == 0;
    sink_bool = netip_addr_less(ip("1.2.3.4"), ip("2.3.4.5"));
    sink_bool = netip_addr_is4_in6(ip("fe80::1"));
    x = netip_addr_unmap(ip("ffff::2.3.4.5"));
    x = netip_addr_with_zone(ip("fe80::1"), BURROW_S(""));
    x = netip_addr_with_zone(ip("fe80::1"), BURROW_S("zone"));
    sink_bool = netip_addr_is_global_unicast(ip("2001:db8::1"));
    sink_bool = netip_addr_is_private(ip("fd00::1"));
    NetipPrefix p = netip_addr_prefix(ip("1.2.3.4"), 20, NULL);
    p = netip_addr_prefix(ip("fe80::1"), 64, NULL);
    sink_u64 = netip_addr_as16(ip("1.2.3.4")).a[15];
    sink_u64 = netip_addr_as4(ip("1.2.3.4")).a[3];
    x = netip_addr_next(ip("1.2.3.4"));
    x = netip_addr_prev(x);
    NetipAddrPort ap = netip_addr_port_from(x, 22);
    ap = netip_parse_addr_port(BURROW_S("[::1]:1234"), NULL);
    ap = netip_must_parse_addr_port(BURROW_S("[::1]:1234"));
    p = netip_prefix_from(netip_addr_port_addr(ap), 32);
    p = netip_parse_prefix(BURROW_S("1.2.3.4/20"), NULL);
    p = netip_parse_prefix(BURROW_S("fe80::1/64"), NULL);
    sink_bool = netip_prefix_contains(pfx("1.2.3.0/24"), ip("1.2.3.4"));
    sink_bool = netip_prefix_overlaps(pfx("1.2.3.0/24"), pfx("1.2.0.0/16"));
    sink_bool = netip_prefix_is_single_ip(pfx("1.2.3.4/32"));
    p = netip_prefix_masked(pfx("1.2.3.4/16"));
    sink_u64 = p.ip.lo;
}

static void TestNoAllocs(TestingT *t) {
    no_allocs_work(); /* makes the "zone" handle, once */
    burrow__heap_count(true);
    uint64_t before = heap_allocs();
    for (int i = 0; i < 100; i++)
        no_allocs_work();
    uint64_t n = heap_allocs() - before;
    burrow__heap_count(false);
    if (n != 0)
        testing_t_errorf_v(t, "allocs = %v; want 0", n);
}

static void TestAddrStringAllocs(TestingT *t) {
    struct {
        const char *name;
        NetipAddr ip;
    } tests[] = {
        {"zero", zero_addr},
        {"ipv4", ip("192.168.1.1")},
        {"ipv6", ip("2001:db8::1")},
        {"ipv6+zone", ip("2001:db8::1%eth0")},
        {"ipv4-in-ipv6", ip("::ffff:192.168.1.1")},
        {"ipv4-in-ipv6+zone", ip("::ffff:192.168.1.1%eth0")},
    };
    Alloc *a = heap_allocator();
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        burrow__heap_count(true);
        uint64_t before = heap_allocs();
        Str s = netip_addr_string(tests[i].ip, a);
        uint64_t n = heap_allocs() - before;
        burrow__heap_count(false);
        mem_free(a, (void *)(uintptr_t)s.p, (size_t)s.len, 1);
        if (n != 1)
            testing_t_errorf_v(t, "%s: allocs=%v, want 1", tests[i].name, n);
    }
}

static void TestPrefixString(TestingT *t) {
    NetipPrefix tests[] = {
        zero_prefix,
        netip_prefix_from(zero_addr, 8),
        netip_prefix_from(ip("1.2.3.4"), 88),
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++)
        CHECK_STR_EQ(cz(a, netip_prefix_string(tests[i], a)), "invalid Prefix");
    arena_free(&ar);
}

static void TestAddrPortString(TestingT *t) {
    struct {
        NetipAddrPort p;
        const char *want;
    } tests[] = {
        {ipp("127.0.0.1:80"), "127.0.0.1:80"},
        {ipp("[0000::0]:8080"), "[::]:8080"},
        {ipp("[FFFF::1]:8080"), "[ffff::1]:8080"},
        {zero_addr_port, "invalid AddrPort"},
        {netip_addr_port_from(zero_addr, 80), "invalid AddrPort"},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++)
        CHECK_STR_EQ(cz(a, netip_addr_port_string(tests[i].p, a)), tests[i].want);
    arena_free(&ar);
}

static void TestAsSlice(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice s = netip_addr_as_slice(zero_addr, a);
    CHECK(s.p == NULL && s.len == 0);
    s = netip_addr_as_slice(ip("1.2.3.4"), a);
    const Byte w4[] = {1, 2, 3, 4};
    CHECK(s.len == 4 && memcmp(s.p, w4, 4) == 0);
    s = netip_addr_as_slice(ip("ffff::1"), a);
    const Byte w6[16] = {0xff, 0xff, [15] = 1};
    CHECK(s.len == 16 && memcmp(s.p, w6, 16) == 0);
    arena_free(&ar);
}

/* From netip_pkg_test.go. */

static void TestPrefixValid(TestingT *t) {
    NetipAddr v4 = ip("1.2.3.4"), v6 = ip("::1");
    struct {
        NetipPrefix p;
        bool want;
    } tests[] = {
        {netip_prefix_from(v4, -2), false},
        {netip_prefix_from(v4, -1), false},
        {netip_prefix_from(v4, 0), true},
        {netip_prefix_from(v4, 32), true},
        {netip_prefix_from(v4, 33), false},

        {netip_prefix_from(v6, -2), false},
        {netip_prefix_from(v6, -1), false},
        {netip_prefix_from(v6, 0), true},
        {netip_prefix_from(v6, 32), true},
        {netip_prefix_from(v6, 128), true},
        {netip_prefix_from(v6, 129), false},

        {netip_prefix_from(zero_addr, -2), false},
        {netip_prefix_from(zero_addr, -1), false},
        {netip_prefix_from(zero_addr, 0), false},
        {netip_prefix_from(zero_addr, 32), false},
        {netip_prefix_from(zero_addr, 128), false},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        bool got = netip_prefix_is_valid(tests[i].p);
        if (got != tests[i].want)
            testing_t_errorf_v(t, "(%v).IsValid() = %v want %v",
                               BURROW_ANY(TYPE_NETIP_PREFIX, &tests[i].p), got,
                               tests[i].want);
        NetipPrefix invalid = netip_prefix_from(netip_prefix_addr(tests[i].p), -1);
        if (!got && !netip_prefix_eq(tests[i].p, invalid))
            testing_t_errorf_v(t, "%d. invalid prefixes differ", i);
    }
}

static void TestIPNextPrev(TestingT *t) {
    struct {
        NetipAddr ip, next, prev;
    } tests[] = {
        {ip("10.0.0.1"), ip("10.0.0.2"), ip("10.0.0.0")},
        {ip("10.0.0.255"), ip("10.0.1.0"), ip("10.0.0.254")},
        {ip("127.0.0.1"), ip("127.0.0.2"), ip("127.0.0.0")},
        {ip("254.255.255.255"), ip("255.0.0.0"), ip("254.255.255.254")},
        {ip("255.255.255.255"), zero_addr, ip("255.255.255.254")},
        {ip("0.0.0.0"), ip("0.0.0.1"), zero_addr},
        {ip("::"), ip("::1"), zero_addr},
        {ip("::%x"), ip("::1%x"), zero_addr},
        {ip("::1"), ip("::2"), ip("::")},
        {ip("ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff"), zero_addr,
         ip("ffff:ffff:ffff:ffff:ffff:ffff:ffff:fffe")},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        NetipAddr x = tests[i].ip;
        NetipAddr n = netip_addr_next(x), p = netip_addr_prev(x);
        if (!netip_addr_eq(n, tests[i].next))
            testing_t_errorf_v(t, "IP(%v).Next = %v; want %v",
                               BURROW_ANY(TYPE_NETIP_ADDR, &x),
                               BURROW_ANY(TYPE_NETIP_ADDR, &n),
                               BURROW_ANY(TYPE_NETIP_ADDR, &tests[i].next));
        if (!netip_addr_eq(p, tests[i].prev))
            testing_t_errorf_v(t, "IP(%v).Prev = %v; want %v",
                               BURROW_ANY(TYPE_NETIP_ADDR, &x),
                               BURROW_ANY(TYPE_NETIP_ADDR, &p),
                               BURROW_ANY(TYPE_NETIP_ADDR, &tests[i].prev));
        if (netip_addr_is_valid(n) && !netip_addr_eq(netip_addr_prev(n), x))
            testing_t_errorf_v(t, "%d. Next.Prev is not the address", i);
        if (netip_addr_is_valid(p) && !netip_addr_eq(netip_addr_next(p), x))
            testing_t_errorf_v(t, "%d. Prev.Next is not the address", i);
    }
    CHECK(!netip_addr_is_valid(netip_addr_prev(ip("0.0.0.0"))));
    CHECK(!netip_addr_is_valid(netip_addr_prev(ip("::"))));
    Byte all_ff[16];
    memset(all_ff, 0xff, sizeof all_ff);
    CHECK(!netip_addr_is_valid(netip_addr_next(ip("255.255.255.255"))));
    CHECK(!netip_addr_is_valid(netip_addr_next(netip_addr_from16(all_ff))));
}

static void TestIPBitLen(TestingT *t) {
    CHECK_INT_EQ(netip_addr_bit_len(zero_addr), 0);
    CHECK_INT_EQ(netip_addr_bit_len(ip("0.0.0.0")), 32);
    CHECK_INT_EQ(netip_addr_bit_len(ip("10.0.0.1")), 32);
    CHECK_INT_EQ(netip_addr_bit_len(ip("::")), 128);
    CHECK_INT_EQ(netip_addr_bit_len(ip("fed0::1")), 128);
    CHECK_INT_EQ(netip_addr_bit_len(ip("::ffff:10.0.0.1")), 128);
}

static void TestPrefixContains(TestingT *t) {
    NetipPrefix z4 = {netip_addr_with_zone(ip("1.2.3.4"), BURROW_S("a")), 33};
    NetipPrefix z6 = {netip_addr_with_zone(ip("::1"), BURROW_S("a")), 129};
    struct {
        NetipPrefix p;
        NetipAddr ip;
        bool want;
    } tests[] = {
        {pfx("9.8.7.6/0"), ip("9.8.7.6"), true},
        {pfx("9.8.7.6/16"), ip("9.8.7.6"), true},
        {pfx("9.8.7.6/16"), ip("9.8.6.4"), true},
        {pfx("9.8.7.6/16"), ip("9.9.7.6"), false},
        {pfx("9.8.7.6/32"), ip("9.8.7.6"), true},
        {pfx("9.8.7.6/32"), ip("9.8.7.7"), false},
        {pfx("9.8.7.6/32"), ip("9.8.7.7"), false},
        {pfx("::1/0"), ip("::1"), true},
        {pfx("::1/0"), ip("::2"), true},
        {pfx("::1/127"), ip("::1"), true},
        {pfx("::1/127"), ip("::2"), false},
        {pfx("::1/128"), ip("::1"), true},
        {pfx("::1/127"), ip("::2"), false},
        {z4, ip("1.2.3.4"), true},
        {z6, ip("::1"), true},
        {pfx("::1/0"), zero_addr, false},
        {pfx("1.2.3.4/0"), zero_addr, false},
        {netip_prefix_from(ip("::1"), 129), ip("::1"), false},
        {netip_prefix_from(ip("1.2.3.4"), 33), ip("1.2.3.4"), false},
        {netip_prefix_from(zero_addr, 0), ip("1.2.3.4"), false},
        {netip_prefix_from(zero_addr, 32), ip("1.2.3.4"), false},
        {netip_prefix_from(zero_addr, 128), ip("::1"), false},
        {pfx("::1/0"), ip("1.2.3.4"), false},
        {pfx("1.2.3.4/0"), ip("::1"), false},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++)
        if (netip_prefix_contains(tests[i].p, tests[i].ip) != tests[i].want)
            testing_t_errorf_v(t, "%d. (%v).Contains(%v) = %v", i,
                               BURROW_ANY(TYPE_NETIP_PREFIX, &tests[i].p),
                               BURROW_ANY(TYPE_NETIP_ADDR, &tests[i].ip),
                               !tests[i].want);
}

static void TestParseIPError(TestingT *t) {
    static const struct {
        const char *ip;
        const char *errstr;
    } tests[] = {
        {"localhost", "unable to parse IP"},
        {"500.0.0.1", "field has value >255"},
        {"::gggg%eth0", "must have at least one digit"},
        {"fe80::1cc0:3e8c:119f:c2e1%", "zone must be a non-empty string"},
        {"%eth0", "missing IPv6 address"},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Error err;
        (void)netip_parse_addr(str_from_cstr(tests[i].ip), &err);
        if (BURROW_OK(err)) {
            testing_t_errorf_v(t, "%s: no error", tests[i].ip);
            continue;
        }
        if (!strings_contains(error_text(err), str_from_cstr(tests[i].errstr)))
            testing_t_errorf_v(t, "error is missing substring %q: %s", tests[i].errstr,
                               error_text(err));
    }
}

static void TestParseAddrPort(TestingT *t) {
    struct {
        const char *in;
        NetipAddrPort want;
        bool want_err;
    } tests[] = {
        {"1.2.3.4:1234", netip_addr_port_from(ip("1.2.3.4"), 1234), false},
        {"1.1.1.1:123456", zero_addr_port, true},
        {"1.1.1.1:-123", zero_addr_port, true},
        {"[::1]:1234", netip_addr_port_from(ip("::1"), 1234), false},
        {"[1.2.3.4]:1234", zero_addr_port, true},
        {"fe80::1:1234", zero_addr_port, true},
        {":0", zero_addr_port, true},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Error err;
        NetipAddrPort got = netip_parse_addr_port(str_from_cstr(tests[i].in), &err);
        if (BURROW_FAILED(err)) {
            if (!tests[i].want_err)
                testing_t_errorf_v(t, "%s: %s", tests[i].in, error_text(err));
        } else {
            if (tests[i].want_err)
                testing_t_errorf_v(t, "%s: no error", tests[i].in);
            CHECK(netip_addr_port_eq(got, tests[i].want));
            CHECK_STR_EQ(cz(a, netip_addr_port_string(got, a)), tests[i].in);
            check_append_to_addr_port(t, a, got);
        }

        NetipAddrPort tgot = zero_addr_port;
        err = netip_addr_port_unmarshal_text(&tgot, text(tests[i].in));
        if (BURROW_FAILED(err)) {
            if (!tests[i].want_err)
                testing_t_errorf_v(t, "%s: UnmarshalText: %s", tests[i].in,
                                   error_text(err));
            continue;
        }
        CHECK(netip_addr_port_eq(tgot, tests[i].want));
        Slice b = netip_addr_port_marshal_text(tgot, a, &err);
        CHECK(BURROW_OK(err));
        CHECK_STR_EQ(bz(a, b), tests[i].in);
    }
    arena_free(&ar);
}

static void TestAddrPortMarshalUnmarshal(TestingT *t) {
    NetipAddrPort p = ipp("1.2.3.4:5");
    CHECK(BURROW_OK(netip_addr_port_unmarshal_text(&p, text(""))));
    CHECK(netip_addr_port_eq(p, zero_addr_port));
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err;
    Slice b = netip_addr_port_marshal_text(p, a, &err);
    CHECK(BURROW_OK(err));
    CHECK_INT_EQ(b.len, 0);
    check_append_to_addr_port(t, a, p);
    arena_free(&ar);
}

static void TestIPv6Accessor(TestingT *t) {
    Byte b[16];
    for (int i = 0; i < 16; i++)
        b[i] = (Byte)(i + 1);
    NetipAddrAs16Ret got = netip_addr_as16(netip_addr_from16(b));
    CHECK(memcmp(got.a, b, 16) == 0);
}

/* Not in Go: the descriptors, as fmt and the other reflective code see them. */
static void TestDescriptors(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    NetipAddr x = ip("fe80::1%eth0");
    NetipAddrPort p = ipp("[::1]:80");
    NetipPrefix f = pfx("10.1.2.3/8");
    NetipAddr z = zero_addr;
    Str s = fmt_sprintf_v(a, "%v %s %v %q", BURROW_ANY(TYPE_NETIP_ADDR, &x),
                          BURROW_ANY(TYPE_NETIP_ADDR_PORT, &p),
                          BURROW_ANY(TYPE_NETIP_PREFIX, &f),
                          BURROW_ANY(TYPE_NETIP_ADDR, &z));
    CHECK_STR_EQ(cz(a, s), "fe80::1%eth0 [::1]:80 10.1.2.3/8 \"invalid IP\"");
    arena_free(&ar);
}

/* Benchmarks. */

static const char *const parse_bench_inputs[][2] = {
    {"v4", "192.168.1.1"},
    {"v6", "fd7a:115c:a1e0:ab12:4843:cd96:626b:430b"},
    {"v6_ellipsis", "fd7a:115c::626b:430b"},
    {"v6_v4", "::ffff:192.168.140.255"},
    {"v6_zone", "1:2::ffff:192.168.140.255%eth1"},
};
enum { NPARSE_BENCH = sizeof parse_bench_inputs / sizeof parse_bench_inputs[0] };

static volatile uint64_t sink;

static void bench_parse_addr(void *env, TestingB *b) {
    Str s = str_from_cstr(env);
    for (Int i = 0; i < testing_b_n(b); i++)
        sink = netip_parse_addr(s, NULL).lo;
}

static void BenchmarkParseAddr(TestingB *b) {
    for (Int i = 0; i < NPARSE_BENCH; i++)
        testing_b_run(b, str_from_cstr(parse_bench_inputs[i][0]),
                      BURROW_FN(TestingBFunc, bench_parse_addr,
                                (void *)(uintptr_t)parse_bench_inputs[i][1]));
}

static void bench_addr_string(void *env, TestingB *b) {
    NetipAddr x = ip(env);
    Alloc *a = heap_allocator();
    for (Int i = 0; i < testing_b_n(b); i++) {
        Str s = netip_addr_string(x, a);
        sink = (uint64_t)s.len;
        mem_free(a, (void *)(uintptr_t)s.p, (size_t)s.len, 1);
    }
}

static void BenchmarkAddrString(TestingB *b) {
    for (Int i = 0; i < NPARSE_BENCH; i++)
        testing_b_run(b, str_from_cstr(parse_bench_inputs[i][0]),
                      BURROW_FN(TestingBFunc, bench_addr_string,
                                (void *)(uintptr_t)parse_bench_inputs[i][1]));
}

static void bench_addr_port_string(void *env, TestingB *b) {
    NetipAddrPort x = netip_addr_port_from(ip(env), 60000);
    Alloc *a = heap_allocator();
    for (Int i = 0; i < testing_b_n(b); i++) {
        Str s = netip_addr_port_string(x, a);
        sink = (uint64_t)s.len;
        mem_free(a, (void *)(uintptr_t)s.p, (size_t)s.len, 1);
    }
}

static void BenchmarkAddrPortString(TestingB *b) {
    for (Int i = 0; i < NPARSE_BENCH; i++)
        testing_b_run(b, str_from_cstr(parse_bench_inputs[i][0]),
                      BURROW_FN(TestingBFunc, bench_addr_port_string,
                                (void *)(uintptr_t)parse_bench_inputs[i][1]));
}

static void bench_parse_addr_port(void *env, TestingB *b) {
    Str s = str_from_cstr(env);
    for (Int i = 0; i < testing_b_n(b); i++)
        sink = netip_parse_addr_port(s, NULL).port;
}

static void BenchmarkParseAddrPort(TestingB *b) {
    static const char *const in[NPARSE_BENCH] = {
        "192.168.1.1:1234",
        "[fd7a:115c:a1e0:ab12:4843:cd96:626b:430b]:1234",
        "[fd7a:115c::626b:430b]:1234",
        "[::ffff:192.168.140.255]:1234",
        "[1:2::ffff:192.168.140.255%eth1]:1234",
    };
    for (Int i = 0; i < NPARSE_BENCH; i++)
        testing_b_run(
            b, str_from_cstr(parse_bench_inputs[i][0]),
            BURROW_FN(TestingBFunc, bench_parse_addr_port, (void *)(uintptr_t)in[i]));
}

typedef struct MaskBench {
    NetipAddr ip;
    Int bits;
} MaskBench;

static void bench_prefix_masking(void *env, TestingB *b) {
    MaskBench *m = env;
    for (Int i = 0; i < testing_b_n(b); i++)
        sink = netip_addr_prefix(m->ip, m->bits, NULL).ip.lo;
}

static void BenchmarkPrefixMasking(TestingB *b) {
    static const struct {
        const char *name, *ip;
        Int bits;
    } tests[] = {
        {"IPv4 /32", "192.0.2.0", 32},
        {"IPv4 /17", "192.0.2.0", 17},
        {"IPv4 /0", "192.0.2.0", 0},
        {"IPv6 /128", "2001:db8::1", 128},
        {"IPv6 /65", "2001:db8::1", 65},
        {"IPv6 /0", "2001:db8::1", 0},
        {"IPv6 zone /128", "2001:db8::1%eth0", 128},
        {"IPv6 zone /65", "2001:db8::1%eth0", 65},
        {"IPv6 zone /0", "2001:db8::1%eth0", 0},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        MaskBench m = {ip(tests[i].ip), tests[i].bits};
        testing_b_run(b, str_from_cstr(tests[i].name),
                      BURROW_FN(TestingBFunc, bench_prefix_masking, &m));
    }
}

static void BenchmarkPrefixString(TestingB *b) {
    NetipPrefix p = pfx("66.55.44.33/22");
    Alloc *a = heap_allocator();
    for (Int i = 0; i < testing_b_n(b); i++) {
        Str s = netip_prefix_string(p, a);
        sink = (uint64_t)s.len;
        mem_free(a, (void *)(uintptr_t)s.p, (size_t)s.len, 1);
    }
}

static void BenchmarkIPv4Contains(TestingB *b) {
    const Byte net[4] = {192, 168, 1, 0}, host[4] = {192, 168, 1, 1};
    NetipPrefix p = netip_prefix_from(netip_addr_from4(net), 24);
    NetipAddr x = netip_addr_from4(host);
    for (Int i = 0; i < testing_b_n(b); i++)
        sink = netip_prefix_contains(p, x);
}

static void BenchmarkIPv6Contains(TestingB *b) {
    NetipPrefix p = pfx("::1/128");
    NetipAddr x = ip("::1");
    for (Int i = 0; i < testing_b_n(b); i++)
        sink = netip_prefix_contains(p, x);
}

static void BenchmarkAs16(TestingB *b) {
    NetipAddr x = ip("1::10");
    for (Int i = 0; i < testing_b_n(b); i++)
        sink = netip_addr_as16(x).a[15];
}

#define TESTS(X)                                                                       \
    X(TestParseAddr)                                                                   \
    X(TestAddrFromSlice)                                                               \
    X(TestIPv4Constructors)                                                            \
    X(TestAddrAppendText)                                                              \
    X(TestAddrMarshalUnmarshalBinary)                                                  \
    X(TestAddrPortMarshalTextString)                                                   \
    X(TestAddrPortMarshalUnmarshalBinary)                                              \
    X(TestPrefixMarshalTextString)                                                     \
    X(TestPrefixMarshalUnmarshalBinary)                                                \
    X(TestAddrMarshalUnmarshal)                                                        \
    X(TestAddrFrom16)                                                                  \
    X(TestIPProperties)                                                                \
    X(TestAddrWellKnown)                                                               \
    X(TestAddrLessCompare)                                                             \
    X(TestAddrSort)                                                                    \
    X(TestAddrPortCompare)                                                             \
    X(TestPrefixCompare)                                                               \
    X(TestIPStringExpanded)                                                            \
    X(TestPrefixMasking)                                                               \
    X(TestPrefixMarshalUnmarshal)                                                      \
    X(TestPrefixUnmarshalTextNonZero)                                                  \
    X(TestIs4AndIs6)                                                                   \
    X(TestIs4In6)                                                                      \
    X(TestPrefixMasked)                                                                \
    X(TestPrefix)                                                                      \
    X(TestPrefixFromInvalidBits)                                                       \
    X(TestParsePrefixAllocs)                                                           \
    X(TestParsePrefixError)                                                            \
    X(TestPrefixIsSingleIP)                                                            \
    X(TestAs4)                                                                         \
    X(TestPrefixOverlaps)                                                              \
    X(TestNoAllocs)                                                                    \
    X(TestAddrStringAllocs)                                                            \
    X(TestPrefixString)                                                                \
    X(TestAddrPortString)                                                              \
    X(TestAsSlice)                                                                     \
    X(TestPrefixValid)                                                                 \
    X(TestIPNextPrev)                                                                  \
    X(TestIPBitLen)                                                                    \
    X(TestPrefixContains)                                                              \
    X(TestParseIPError)                                                                \
    X(TestParseAddrPort)                                                               \
    X(TestAddrPortMarshalUnmarshal)                                                    \
    X(TestIPv6Accessor)                                                                \
    X(TestDescriptors)                                                                 \
    X(BenchmarkParseAddr)                                                              \
    X(BenchmarkAddrString)                                                             \
    X(BenchmarkAddrPortString)                                                         \
    X(BenchmarkParseAddrPort)                                                          \
    X(BenchmarkPrefixMasking)                                                          \
    X(BenchmarkPrefixString)                                                           \
    X(BenchmarkIPv4Contains)                                                           \
    X(BenchmarkIPv6Contains)                                                           \
    X(BenchmarkAs16)

TESTING_MAIN(TESTS)

/* net's address tests, from Go's ip_test.go. TestLookupWithIP waits for the
 * resolver, and TestIPAppendTextNoAllocs counts heap allocations with
 * burrow__heap_count where Go uses testing.AllocsPerRun.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/net.h"

#include "burrow/error.h"
#include "burrow/func.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/testing.h"

#include "check.h"

#include <stdint.h>
#include <string.h>

/* The functions take an allocator for what they make, and each test hands
 * them an arena it frees at the end. */
static const char *cz(Alloc *a, Str s) {
    char *p = mem_alloc_nozero(a, (size_t)s.len + 1, 1);
    if (s.len > 0)
        memcpy(p, s.p, (size_t)s.len);
    p[s.len] = 0;
    return p;
}

static NetIP bytes_of(Alloc *a, const Byte *p, Int n) {
    if (n == 0)
        return slice_nil(TYPE_BYTE);
    Slice s = slice_make(a, TYPE_BYTE, n, n);
    memcpy(s.p, p, (size_t)n);
    return s;
}

#define IP_OF(a, ...)                                                                  \
    bytes_of((a), (const Byte[]){__VA_ARGS__}, (Int)sizeof((const Byte[]){__VA_ARGS__}))

static NetIP v4(Alloc *a, Byte x, Byte y, Byte z, Byte w) {
    return net_ipv4(a, x, y, z, w);
}

static NetIP pip(Alloc *a, const char *s) {
    return net_parse_ip(a, str_from_cstr(s));
}

static bool bytes_eq(Slice x, Slice y) {
    return x.len == y.len && (x.len == 0 || memcmp(x.p, y.p, (size_t)x.len) == 0);
}

/* reflect.DeepEqual on two IPs: nil and empty differ, as do lengths. */
static bool deep_eq(Slice x, Slice y) {
    return (x.p == NULL) == (y.p == NULL) && bytes_eq(x, y);
}

static const char *ips(Alloc *a, NetIP ip) {
    return cz(a, net_ip_string(ip, a));
}

/* --------------------------------------------------------------- ParseIP */

typedef struct ParseIPTest {
    const char *in;
    bool ok;
    Byte out[16];
} ParseIPTest;

#define V4IN6(a, b, c, d) {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, a, b, c, d}

static const ParseIPTest parse_ip_tests[] = {
    {"127.0.1.2", true, V4IN6(127, 0, 1, 2)},
    {"127.0.0.1", true, V4IN6(127, 0, 0, 1)},
    {"::ffff:127.1.2.3", true, V4IN6(127, 1, 2, 3)},
    {"::ffff:7f01:0203", true, V4IN6(127, 1, 2, 3)},
    {"0:0:0:0:0000:ffff:127.1.2.3", true, V4IN6(127, 1, 2, 3)},
    {"0:0:0:0::ffff:127.1.2.3", true, V4IN6(127, 1, 2, 3)},

    {"2001:4860:0:2001::68",
     true,
     {0x20, 0x01, 0x48, 0x60, 0, 0, 0x20, 0x01, 0, 0, 0, 0, 0, 0, 0x00, 0x68}},
    {"2001:4860:0000:2001:0000:0000:0000:0068",
     true,
     {0x20, 0x01, 0x48, 0x60, 0, 0, 0x20, 0x01, 0, 0, 0, 0, 0, 0, 0x00, 0x68}},

    {"-0.0.0.0", false, {0}},
    {"0.-1.0.0", false, {0}},
    {"0.0.-2.0", false, {0}},
    {"0.0.0.-3", false, {0}},
    {"127.0.0.256", false, {0}},
    {"abc", false, {0}},
    {"123:", false, {0}},
    {"fe80::1%lo0", false, {0}},
    {"fe80::1%911", false, {0}},
    {"", false, {0}},
    /* 6 zeroes in one group */
    {"0:0:0:0:000000:ffff:127.1.2.3", false, {0}},
    /* 5 zeroes in one group edge case */
    {"0:0:0:0:00000:ffff:127.1.2.3", false, {0}},
    {"a1:a2:a3:a4::b1:b2:b3:b4", false, {0}}, /* Issue 6628 */
    {"127.001.002.003", false, {0}},
    {"::ffff:127.001.002.003", false, {0}},
    {"123.000.000.000", false, {0}},
    {"1.2..4", false, {0}},
    {"0123.0.0.1", false, {0}},
};

#define NPARSE ((Int)(sizeof parse_ip_tests / sizeof parse_ip_tests[0]))

static NetIP parse_want(const ParseIPTest *tt) {
    if (!tt->ok)
        return slice_nil(TYPE_BYTE);
    return (Slice){(void *)(uintptr_t)tt->out, 16, 16, TYPE_BYTE};
}

static void TestParseIP(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NPARSE; i++) {
        const ParseIPTest *tt = &parse_ip_tests[i];
        NetIP want = parse_want(tt);
        NetIP out = pip(a, tt->in);
        if (!deep_eq(out, want))
            testing_t_errorf_v(t, "ParseIP(%q) = %s, want %s", tt->in, ips(a, out),
                               ips(a, want));
        if (tt->in[0] == 0)
            continue; /* Tested in TestMarshalEmptyIP below. */
        NetIP got = slice_nil(TYPE_BYTE);
        Error err = net_ip_unmarshal_text(&got, a,
                                          (Slice){(void *)(uintptr_t)tt->in,
                                                  (Int)strlen(tt->in),
                                                  (Int)strlen(tt->in), TYPE_BYTE});
        if (!deep_eq(got, want) || (!tt->ok) != BURROW_FAILED(err))
            testing_t_errorf_v(t, "IP.UnmarshalText(%q) = %s, %v, want %s", tt->in,
                               ips(a, got), err, ips(a, want));
        if (!tt->ok) {
            const NetParseError *pe = errors_as(err, TYPE_NET_PARSE_ERROR);
            if (pe == NULL)
                testing_t_errorf_v(t, "IP.UnmarshalText(%q): %v is not a ParseError",
                                   tt->in, err);
            else if (strcmp(cz(a, pe->type), "IP address") != 0 ||
                     strcmp(cz(a, pe->text), tt->in) != 0)
                testing_t_errorf_v(t, "IP.UnmarshalText(%q): ParseError{%q, %q}",
                                   tt->in, cz(a, pe->type), cz(a, pe->text));
        }
    }
    arena_free(&ar);
}

static void bench_parse_ip(void *env, TestingB *b) {
    (void)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int n = 0; n < testing_b_n(b); n++) {
        for (Int i = 0; i < NPARSE; i++)
            pip(a, parse_ip_tests[i].in);
        arena_reset(&ar);
    }
    arena_free(&ar);
}

static void BenchmarkParseIP(TestingB *b) {
    bench_parse_ip(NULL, b);
}

static void bench_parse_one(TestingB *b, const char *s) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int n = 0; n < testing_b_n(b); n++) {
        pip(a, s);
        if ((n & 1023) == 1023)
            arena_reset(&ar);
    }
    arena_free(&ar);
}

static void BenchmarkParseIPValidIPv4(TestingB *b) {
    bench_parse_one(b, "192.0.2.1");
}

static void BenchmarkParseIPValidIPv6(TestingB *b) {
    bench_parse_one(b, "2001:DB8::1");
}

/* Issue 6339 */
static void TestMarshalEmptyIP(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice ins[2] = {slice_nil(TYPE_BYTE), slice_make(a, TYPE_BYTE, 0, 0)};
    for (int i = 0; i < 2; i++) {
        NetIP out = IP_OF(a, 1, 2, 3, 4);
        Error err = net_ip_unmarshal_text(&out, a, ins[i]);
        if (BURROW_FAILED(err) || out.p != NULL)
            testing_t_errorf_v(t, "UnmarshalText(%d) = %s, %v; want nil, nil", i,
                               ips(a, out), err);
    }
    NetIP ip = slice_nil(TYPE_BYTE);
    Error err = BURROW_NO_ERROR;
    Slice got = net_ip_marshal_text(ip, a, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);
    if (got.p == NULL || got.len != 0)
        testing_t_errorf_v(t, "got %d bytes, want []byte(\"\")", (int)got.len);

    Slice buf = slice_make(a, TYPE_BYTE, 4, 4);
    got = net_ip_append_text(ip, a, buf, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);
    if (!bytes_eq(got, IP_OF(a, 0, 0, 0, 0)))
        testing_t_errorf_v(t, "got %d bytes, want []byte(\"\\x00\\x00\\x00\\x00\")",
                           (int)got.len);
    arena_free(&ar);
}

/* -------------------------------------------------------------- IP.String */

typedef struct IPStringTest {
    Int n;
    Byte in[16];
    const char *str;
    const char *byt;     /* NULL for nil */
    const char *err_hex; /* the AddrError's Addr, or NULL for no error */
} IPStringTest;

static const IPStringTest ip_string_tests[] = {
    /* IPv4 address */
    {4, {192, 0, 2, 1}, "192.0.2.1", "192.0.2.1", NULL},
    {4, {0, 0, 0, 0}, "0.0.0.0", "0.0.0.0", NULL},

    /* IPv4-mapped IPv6 address */
    {16, V4IN6(192, 0, 2, 1), "192.0.2.1", "192.0.2.1", NULL},
    {16, V4IN6(0, 0, 0, 0), "0.0.0.0", "0.0.0.0", NULL},

    /* IPv6 address */
    {16,
     {0x20, 0x1, 0xd, 0xb8, 0, 0, 0, 0, 0, 0, 0x1, 0x23, 0, 0x12, 0, 0x1},
     "2001:db8::123:12:1",
     "2001:db8::123:12:1",
     NULL},
    {16,
     {0x20, 0x1, 0xd, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x1},
     "2001:db8::1",
     "2001:db8::1",
     NULL},
    {16,
     {0x20, 0x1, 0xd, 0xb8, 0, 0, 0, 0x1, 0, 0, 0, 0x1, 0, 0, 0, 0x1},
     "2001:db8:0:1:0:1:0:1",
     "2001:db8:0:1:0:1:0:1",
     NULL},
    {16,
     {0x20, 0x1, 0xd, 0xb8, 0, 0x1, 0, 0, 0, 0x1, 0, 0, 0, 0x1, 0, 0},
     "2001:db8:1:0:1:0:1:0",
     "2001:db8:1:0:1:0:1:0",
     NULL},
    {16,
     {0x20, 0x1, 0, 0, 0, 0, 0, 0, 0, 0x1, 0, 0, 0, 0, 0, 0x1},
     "2001::1:0:0:1",
     "2001::1:0:0:1",
     NULL},
    {16,
     {0x20, 0x1, 0xd, 0xb8, 0, 0, 0, 0, 0, 0x1, 0, 0, 0, 0, 0, 0},
     "2001:db8:0:0:1::",
     "2001:db8:0:0:1::",
     NULL},
    {16,
     {0x20, 0x1, 0xd, 0xb8, 0, 0, 0, 0, 0, 0x1, 0, 0, 0, 0, 0, 0x1},
     "2001:db8::1:0:0:1",
     "2001:db8::1:0:0:1",
     NULL},
    {16,
     {0x20, 0x1, 0xd, 0xb8, 0, 0, 0, 0, 0, 0xa, 0, 0xb, 0, 0xc, 0, 0xd},
     "2001:db8::a:b:c:d",
     "2001:db8::a:b:c:d",
     NULL},
    {16, {0}, "::", "::", NULL},

    /* IP wildcard equivalent address in Dial/Listen API */
    {0, {0}, "<nil>", NULL, NULL},

    /* Opaque byte sequence */
    {8,
     {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef},
     "?0123456789abcdef",
     NULL,
     "0123456789abcdef"},
};

#define NSTRING ((Int)(sizeof ip_string_tests / sizeof ip_string_tests[0]))

static NetIP string_in(const IPStringTest *tt) {
    if (tt->n == 0)
        return slice_nil(TYPE_BYTE);
    return (Slice){(void *)(uintptr_t)tt->in, tt->n, tt->n, TYPE_BYTE};
}

static bool str_bytes_eq(Slice got, const char *want) {
    Int n = want == NULL ? 0 : (Int)strlen(want);
    return got.len == n && (n == 0 || memcmp(got.p, want, (size_t)n) == 0);
}

/* reflect.DeepEqual(err, &AddrError{Err: "invalid IP address", Addr: hex}) */
static bool want_err(Error err, const char *hex, Alloc *a) {
    if (hex == NULL)
        return BURROW_OK(err);
    const NetAddrError *e = errors_as(err, TYPE_NET_ADDR_ERROR);
    if (e == NULL)
        return false;
    return strcmp(cz(a, e->err), "invalid IP address") == 0 &&
           strcmp(cz(a, e->addr), hex) == 0;
}

static void TestIPString(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NSTRING; i++) {
        const IPStringTest *tt = &ip_string_tests[i];
        NetIP in = string_in(tt);
        const char *out = ips(a, in);
        if (strcmp(out, tt->str) != 0)
            testing_t_errorf_v(t, "IP.String(%s) = %q, want %q", tt->str, out, tt->str);
        Error err = BURROW_NO_ERROR;
        Slice m = net_ip_marshal_text(in, a, &err);
        if (!str_bytes_eq(m, tt->byt) || !want_err(err, tt->err_hex, a))
            testing_t_errorf_v(t, "IP.MarshalText(%s) = %q, %v, want %q", tt->str,
                               cz(a, str_from_bytes(m.p, m.len)), err,
                               tt->byt ? tt->byt : "");
        Slice buf = slice_make(a, TYPE_BYTE, 4, 32);
        Slice ap = net_ip_append_text(in, a, buf, &err);
        if (!str_bytes_eq(slice_sub(ap, 4, ap.len), tt->byt) ||
            !want_err(err, tt->err_hex, a))
            testing_t_errorf_v(t, "IP.AppendText(%s) = %v, want %q", tt->str, err,
                               tt->byt ? tt->byt : "");
    }
    arena_free(&ar);
}

static uint64_t heap_allocs(void) {
    uint64_t n = 0, bytes = 0;
    burrow__heap_counts(&n, &bytes);
    return n;
}

static void TestIPAppendTextNoAllocs(TestingT *t) {
    /* except the invalid IP */
    Byte buf_data[64];
    for (Int i = 0; i < NSTRING - 1; i++) {
        NetIP in = string_in(&ip_string_tests[i]);
        Slice buf = {buf_data, 0, 64, TYPE_BYTE};
        burrow__heap_count(true);
        uint64_t before = heap_allocs();
        for (int k = 0; k < 1000; k++)
            (void)net_ip_append_text(in, heap_allocator(), buf, NULL);
        uint64_t n = heap_allocs() - before;
        burrow__heap_count(false);
        if (n != 0)
            testing_t_errorf_v(t, "IP(%q) AppendText allocs: %d times, want 0",
                               ip_string_tests[i].str, (int)(n / 1000));
    }
}

static void bench_marshal(void *env, TestingB *b) {
    const IPStringTest *tt = env;
    NetIP ip = string_in(tt);
    Alloc *a = heap_allocator();
    testing_b_report_allocs(b);
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++) {
        Slice s = net_ip_marshal_text(ip, a, NULL);
        mem_free(a, s.p, (size_t)s.cap, 1);
    }
}

static const IPStringTest marshal_bench[] = {
    {4, {192, 0, 2, 1}, "IPv4", NULL, NULL},
    {16,
     {0x20, 0x1, 0xd, 0xb8, 0, 0, 0, 0, 0, 0xa, 0, 0xb, 0, 0xc, 0, 0xd},
     "IPv6",
     NULL,
     NULL},
    /* fd7a:115c:a1e0:ab12:4843:cd96:626b:430b */
    {16,
     {253, 122, 17, 92, 161, 224, 171, 18, 72, 67, 205, 150, 98, 107, 67, 11},
     "IPv6_long",
     NULL,
     NULL},
};

static void BenchmarkIPMarshalText(TestingB *b) {
    for (int i = 0; i < 3; i++)
        testing_b_run(b, str_from_cstr(marshal_bench[i].str),
                      BURROW_FN(TestingBFunc, bench_marshal,
                                (void *)(uintptr_t)&marshal_bench[i]));
}

static volatile Int sink;

static void bench_ip_string(void *env, TestingB *b) {
    Int size = (Int)(intptr_t)env;
    Alloc *a = heap_allocator();
    testing_b_report_allocs(b);
    testing_b_reset_timer(b);
    for (Int n = 0; n < testing_b_n(b); n++) {
        for (Int i = 0; i < NSTRING; i++) {
            NetIP in = string_in(&ip_string_tests[i]);
            if (in.len != 0 && in.len == size) {
                Str s = net_ip_string(in, a);
                sink = s.len;
                mem_free(a, (void *)(uintptr_t)s.p, (size_t)s.len, 1);
            }
        }
    }
}

static void BenchmarkIPString(TestingB *b) {
    testing_b_run(b, BURROW_S("IPv4"),
                  BURROW_FN(TestingBFunc, bench_ip_string, (void *)(intptr_t)4));
    testing_b_run(b, BURROW_S("IPv6"),
                  BURROW_FN(TestingBFunc, bench_ip_string, (void *)(intptr_t)16));
}

/* ---------------------------------------------------------------- IP.Mask */

static void TestIPMask(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    struct {
        NetIP in;
        NetIPMask mask;
        NetIP out;
    } tests[] = {
        {v4(a, 192, 168, 1, 127), net_ipv4_mask(a, 255, 255, 255, 128),
         v4(a, 192, 168, 1, 0)},
        {v4(a, 192, 168, 1, 127), pip(a, "255.255.255.192"), v4(a, 192, 168, 1, 64)},
        {v4(a, 192, 168, 1, 127), pip(a, "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffe0"),
         v4(a, 192, 168, 1, 96)},
        {v4(a, 192, 168, 1, 127), net_ipv4_mask(a, 255, 0, 255, 0),
         v4(a, 192, 0, 1, 0)},
        {pip(a, "2001:db8::1"), pip(a, "ffff:ff80::"), pip(a, "2001:d80::")},
        {pip(a, "2001:db8::1"), pip(a, "f0f0:0f0f::"), pip(a, "2000:d08::")},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        NetIP out = net_ip_mask(tests[i].in, a, tests[i].mask);
        if (out.len == 0 || !net_ip_equal(tests[i].out, out))
            testing_t_errorf_v(t, "IP(%s).Mask(%s) = %s, want %s", ips(a, tests[i].in),
                               cz(a, net_ip_mask_string(tests[i].mask, a)), ips(a, out),
                               ips(a, tests[i].out));
    }
    arena_free(&ar);
}

static void TestIPMaskString(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    struct {
        NetIPMask in;
        const char *out;
    } tests[] = {
        {net_ipv4_mask(a, 255, 255, 255, 240), "fffffff0"},
        {net_ipv4_mask(a, 255, 0, 128, 0), "ff008000"},
        {pip(a, "ffff:ff80::"), "ffffff80000000000000000000000000"},
        {pip(a, "ef00:ff80::cafe:0"), "ef00ff800000000000000000cafe0000"},
        {slice_nil(TYPE_BYTE), "<nil>"},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        const char *out = cz(a, net_ip_mask_string(tests[i].in, a));
        if (strcmp(out, tests[i].out) != 0)
            testing_t_errorf_v(t, "IPMask.String() = %q, want %q", out, tests[i].out);
    }
    arena_free(&ar);
}

static void BenchmarkIPMaskString(TestingB *b) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    NetIPMask masks[5] = {net_ipv4_mask(a, 255, 255, 255, 240),
                          net_ipv4_mask(a, 255, 0, 128, 0), pip(a, "ffff:ff80::"),
                          pip(a, "ef00:ff80::cafe:0"), slice_nil(TYPE_BYTE)};
    Alloc *h = heap_allocator();
    for (Int n = 0; n < testing_b_n(b); n++) {
        for (int i = 0; i < 5; i++) {
            Str s = net_ip_mask_string(masks[i], h);
            sink = s.len;
            mem_free(h, (void *)(uintptr_t)s.p, (size_t)s.len, 1);
        }
    }
    arena_free(&ar);
}

/* -------------------------------------------------------------- ParseCIDR */

static void TestParseCIDR(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    struct {
        const char *in;
        NetIP ip;
        NetIP net_ip;
        NetIPMask net_mask;
        bool err;
    } tests[] = {
        {"135.104.0.0/32", v4(a, 135, 104, 0, 0), v4(a, 135, 104, 0, 0),
         net_ipv4_mask(a, 255, 255, 255, 255), false},
        {"0.0.0.0/24", v4(a, 0, 0, 0, 0), v4(a, 0, 0, 0, 0),
         net_ipv4_mask(a, 255, 255, 255, 0), false},
        {"135.104.0.0/24", v4(a, 135, 104, 0, 0), v4(a, 135, 104, 0, 0),
         net_ipv4_mask(a, 255, 255, 255, 0), false},
        {"135.104.0.1/32", v4(a, 135, 104, 0, 1), v4(a, 135, 104, 0, 1),
         net_ipv4_mask(a, 255, 255, 255, 255), false},
        {"135.104.0.1/24", v4(a, 135, 104, 0, 1), v4(a, 135, 104, 0, 0),
         net_ipv4_mask(a, 255, 255, 255, 0), false},
        {"::1/128", pip(a, "::1"), pip(a, "::1"),
         pip(a, "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff"), false},
        {"abcd:2345::/127", pip(a, "abcd:2345::"), pip(a, "abcd:2345::"),
         pip(a, "ffff:ffff:ffff:ffff:ffff:ffff:ffff:fffe"), false},
        {"abcd:2345::/65", pip(a, "abcd:2345::"), pip(a, "abcd:2345::"),
         pip(a, "ffff:ffff:ffff:ffff:8000::"), false},
        {"abcd:2345::/64", pip(a, "abcd:2345::"), pip(a, "abcd:2345::"),
         pip(a, "ffff:ffff:ffff:ffff::"), false},
        {"abcd:2345::/63", pip(a, "abcd:2345::"), pip(a, "abcd:2345::"),
         pip(a, "ffff:ffff:ffff:fffe::"), false},
        {"abcd:2345::/33", pip(a, "abcd:2345::"), pip(a, "abcd:2345::"),
         pip(a, "ffff:ffff:8000::"), false},
        {"abcd:2345::/32", pip(a, "abcd:2345::"), pip(a, "abcd:2345::"),
         pip(a, "ffff:ffff::"), false},
        {"abcd:2344::/31", pip(a, "abcd:2344::"), pip(a, "abcd:2344::"),
         pip(a, "ffff:fffe::"), false},
        {"abcd:2300::/24", pip(a, "abcd:2300::"), pip(a, "abcd:2300::"),
         pip(a, "ffff:ff00::"), false},
        {"abcd:2345::/24", pip(a, "abcd:2345::"), pip(a, "abcd:2300::"),
         pip(a, "ffff:ff00::"), false},
        {"2001:DB8::/48", pip(a, "2001:DB8::"), pip(a, "2001:DB8::"),
         pip(a, "ffff:ffff:ffff::"), false},
        {"2001:DB8::1/48", pip(a, "2001:DB8::1"), pip(a, "2001:DB8::"),
         pip(a, "ffff:ffff:ffff::"), false},
        {"192.168.1.1/255.255.255.0", {0}, {0}, {0}, true},
        {"192.168.1.1/35", {0}, {0}, {0}, true},
        {"2001:db8::1/-1", {0}, {0}, {0}, true},
        {"2001:db8::1/-0", {0}, {0}, {0}, true},
        {"-0.0.0.0/32", {0}, {0}, {0}, true},
        {"0.-1.0.0/32", {0}, {0}, {0}, true},
        {"0.0.-2.0/32", {0}, {0}, {0}, true},
        {"0.0.0.-3/32", {0}, {0}, {0}, true},
        {"0.0.0.0/-0", {0}, {0}, {0}, true},
        {"127.000.000.001/32", {0}, {0}, {0}, true},
        {"", {0}, {0}, {0}, true},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Error err = BURROW_NO_ERROR;
        struct {
            NetIP ip;
            NetIPNet *net;
        } r;
        r.ip = net_parse_cidr(a, str_from_cstr(tests[i].in), &r.net, &err);
        if (tests[i].err) {
            const NetParseError *pe = errors_as(err, TYPE_NET_PARSE_ERROR);
            if (pe == NULL || strcmp(cz(a, pe->type), "CIDR address") != 0 ||
                strcmp(cz(a, pe->text), tests[i].in) != 0)
                testing_t_errorf_v(t,
                                   "ParseCIDR(%q) = %v, want a CIDR address ParseError",
                                   tests[i].in, err);
            if (r.ip.p != NULL || r.net != NULL)
                testing_t_errorf_v(t, "ParseCIDR(%q) gave a result with its error",
                                   tests[i].in);
            continue;
        }
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "ParseCIDR(%q): %v", tests[i].in, err);
            continue;
        }
        if (!net_ip_equal(tests[i].ip, r.ip) ||
            !net_ip_equal(tests[i].net_ip, r.net->ip) ||
            !deep_eq(r.net->mask, tests[i].net_mask))
            testing_t_errorf_v(t, "ParseCIDR(%q) = %s, {%s, %s}; want %s, {%s, %s}",
                               tests[i].in, ips(a, r.ip), ips(a, r.net->ip),
                               cz(a, net_ip_mask_string(r.net->mask, a)),
                               ips(a, tests[i].ip), ips(a, tests[i].net_ip),
                               cz(a, net_ip_mask_string(tests[i].net_mask, a)));
    }
    arena_free(&ar);
}

/* ------------------------------------------------------------------ IPNet */

static void TestIPNetContains(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    struct {
        NetIP ip;
        NetIPNet net;
        bool ok;
    } tests[] = {
        {v4(a, 172, 16, 1, 1), {v4(a, 172, 16, 0, 0), net_cidr_mask(a, 12, 32)}, true},
        {v4(a, 172, 24, 0, 1), {v4(a, 172, 16, 0, 0), net_cidr_mask(a, 13, 32)}, false},
        {v4(a, 192, 168, 0, 3),
         {v4(a, 192, 168, 0, 0), net_ipv4_mask(a, 0, 0, 255, 252)},
         true},
        {v4(a, 192, 168, 0, 4),
         {v4(a, 192, 168, 0, 0), net_ipv4_mask(a, 0, 255, 0, 252)},
         false},
        {pip(a, "2001:db8:1:2::1"),
         {pip(a, "2001:db8:1::"), net_cidr_mask(a, 47, 128)},
         true},
        {pip(a, "2001:db8:1:2::1"),
         {pip(a, "2001:db8:2::"), net_cidr_mask(a, 47, 128)},
         false},
        {pip(a, "2001:db8:1:2::1"),
         {pip(a, "2001:db8:1::"), pip(a, "ffff:0:ffff::")},
         true},
        {pip(a, "2001:db8:1:2::1"),
         {pip(a, "2001:db8:1::"), pip(a, "0:0:0:ffff::")},
         false},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        bool ok = net_ip_net_contains(&tests[i].net, tests[i].ip);
        if (ok != tests[i].ok)
            testing_t_errorf_v(t, "IPNet(%s).Contains(%s) = %v, want %v",
                               cz(a, net_ip_net_string(&tests[i].net, a)),
                               ips(a, tests[i].ip), ok, tests[i].ok);
    }
    arena_free(&ar);
}

static void TestIPNetString(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    NetIPNet nets[4] = {
        {v4(a, 192, 168, 1, 0), net_cidr_mask(a, 26, 32)},
        {v4(a, 192, 168, 1, 0), net_ipv4_mask(a, 255, 0, 255, 0)},
        {pip(a, "2001:db8::"), net_cidr_mask(a, 55, 128)},
        {pip(a, "2001:db8::"), pip(a, "8000:f123:0:cafe::")},
    };
    struct {
        const NetIPNet *in;
        const char *out;
    } tests[] = {
        {&nets[0], "192.168.1.0/26"},
        {&nets[1], "192.168.1.0/ff00ff00"},
        {&nets[2], "2001:db8::/55"},
        {&nets[3], "2001:db8::/8000f1230000cafe0000000000000000"},
        {NULL, "<nil>"},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        const char *out = cz(a, net_ip_net_string(tests[i].in, a));
        if (strcmp(out, tests[i].out) != 0)
            testing_t_errorf_v(t, "IPNet.String() = %q, want %q", out, tests[i].out);
    }
    arena_free(&ar);
}

static void TestCIDRMask(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    struct {
        Int ones;
        Int bits;
        NetIPMask out;
    } tests[] = {
        {0, 32, net_ipv4_mask(a, 0, 0, 0, 0)},
        {12, 32, net_ipv4_mask(a, 255, 240, 0, 0)},
        {24, 32, net_ipv4_mask(a, 255, 255, 255, 0)},
        {32, 32, net_ipv4_mask(a, 255, 255, 255, 255)},
        {0, 128, IP_OF(a, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)},
        {4, 128, IP_OF(a, 0xf0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)},
        {48, 128,
         IP_OF(a, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)},
        {128, 128,
         IP_OF(a, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
               0xff, 0xff, 0xff, 0xff, 0xff)},
        {33, 32, slice_nil(TYPE_BYTE)},
        {32, 33, slice_nil(TYPE_BYTE)},
        {-1, 128, slice_nil(TYPE_BYTE)},
        {128, -1, slice_nil(TYPE_BYTE)},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        NetIPMask out = net_cidr_mask(a, tests[i].ones, tests[i].bits);
        if (!deep_eq(out, tests[i].out))
            testing_t_errorf_v(t, "CIDRMask(%d, %d) = %s, want %s", (int)tests[i].ones,
                               (int)tests[i].bits, cz(a, net_ip_mask_string(out, a)),
                               cz(a, net_ip_mask_string(tests[i].out, a)));
    }
    arena_free(&ar);
}

bool burrow__net_number_and_mask(const NetIPNet *n, NetIP *ip, NetIPMask *m);

static void TestNetworkNumberAndMask(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    NetIP v4addr = IP_OF(a, 192, 168, 0, 1);
    NetIP v4mappedv6addr =
        IP_OF(a, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 192, 168, 0, 1);
    NetIP v6addr =
        IP_OF(a, 0x20, 0x1, 0xd, 0xb8, 0, 0, 0, 0, 0, 0, 0x1, 0x23, 0, 0x12, 0, 0x1);
    NetIPMask v4mask = IP_OF(a, 255, 255, 255, 0);
    NetIPMask v4mappedv6mask = IP_OF(a, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                     0xff, 0xff, 0xff, 0xff, 255, 255, 255, 0);
    NetIPMask v6mask = IP_OF(a, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0, 0, 0,
                             0, 0, 0, 0, 0);
    NetIP badaddr = IP_OF(a, 192, 168, 0);
    NetIPMask badmask = IP_OF(a, 255, 255, 0);
    NetIPMask v4maskzero = IP_OF(a, 0, 0, 0, 0);
    NetIP nil = slice_nil(TYPE_BYTE);
    struct {
        NetIPNet in;
        NetIPNet out;
    } tests[] = {
        {{v4addr, v4mask}, {v4addr, v4mask}},
        {{v4addr, v4mappedv6mask}, {v4addr, v4mask}},
        {{v4mappedv6addr, v4mappedv6mask}, {v4addr, v4mask}},
        {{v4mappedv6addr, v6mask}, {v4addr, v4maskzero}},
        {{v4addr, v6mask}, {v4addr, v4maskzero}},
        {{v6addr, v6mask}, {v6addr, v6mask}},
        {{v6addr, v4mappedv6mask}, {v6addr, v4mappedv6mask}},
        {{v6addr, v4mask}, {nil, nil}},
        {{v4addr, badmask}, {nil, nil}},
        {{v4mappedv6addr, badmask}, {nil, nil}},
        {{v6addr, badmask}, {nil, nil}},
        {{badaddr, v4mask}, {nil, nil}},
        {{badaddr, v4mappedv6mask}, {nil, nil}},
        {{badaddr, v6mask}, {nil, nil}},
        {{badaddr, badmask}, {nil, nil}},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        NetIP ip;
        NetIPMask m;
        burrow__net_number_and_mask(&tests[i].in, &ip, &m);
        if (!deep_eq(ip, tests[i].out.ip) || !deep_eq(m, tests[i].out.mask))
            testing_t_errorf_v(t, "networkNumberAndMask(%d) = {%s, %s}, want {%s, %s}",
                               (int)i, ips(a, ip), cz(a, net_ip_mask_string(m, a)),
                               ips(a, tests[i].out.ip),
                               cz(a, net_ip_mask_string(tests[i].out.mask, a)));
    }
    arena_free(&ar);
}

/* -------------------------------------------------------------- host:port */

static bool str_is(Str s, const char *want) {
    Int n = (Int)strlen(want);
    return s.len == n && (n == 0 || memcmp(s.p, want, (size_t)n) == 0);
}

static void TestSplitHostPort(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    static const struct {
        const char *host_port, *host, *port;
    } ok[] = {
        /* Host name */
        {"localhost:http", "localhost", "http"},
        {"localhost:80", "localhost", "80"},

        /* Go-specific host name with zone identifier */
        {"localhost%lo0:http", "localhost%lo0", "http"},
        {"localhost%lo0:80", "localhost%lo0", "80"},
        {"[localhost%lo0]:http", "localhost%lo0", "http"}, /* Go 1 behavior */
        {"[localhost%lo0]:80", "localhost%lo0", "80"},     /* Go 1 behavior */

        /* IP literal */
        {"127.0.0.1:http", "127.0.0.1", "http"},
        {"127.0.0.1:80", "127.0.0.1", "80"},
        {"[::1]:http", "::1", "http"},
        {"[::1]:80", "::1", "80"},

        /* IP literal with zone identifier */
        {"[::1%lo0]:http", "::1%lo0", "http"},
        {"[::1%lo0]:80", "::1%lo0", "80"},

        /* Go-specific wildcard for host name */
        {":http", "", "http"}, /* Go 1 behavior */
        {":80", "", "80"},     /* Go 1 behavior */

        /* Go-specific wildcard for service name or transport port number */
        {"golang.org:", "golang.org", ""}, /* Go 1 behavior */
        {"127.0.0.1:", "127.0.0.1", ""},   /* Go 1 behavior */
        {"[::1]:", "::1", ""},             /* Go 1 behavior */

        /* Opaque service name */
        {"golang.org:https%foo", "golang.org", "https%foo"}, /* Go 1 behavior */
    };
    for (size_t i = 0; i < sizeof ok / sizeof ok[0]; i++) {
        Error err = BURROW_NO_ERROR;
        struct {
            Str host, port;
        } r;
        r.host = net_split_host_port(str_from_cstr(ok[i].host_port), &r.port, &err);
        if (!str_is(r.host, ok[i].host) || !str_is(r.port, ok[i].port) ||
            BURROW_FAILED(err))
            testing_t_errorf_v(t, "SplitHostPort(%q) = %q, %q, %v; want %q, %q, nil",
                               ok[i].host_port, cz(a, r.host), cz(a, r.port), err,
                               ok[i].host, ok[i].port);
    }

    static const struct {
        const char *host_port, *err;
    } bad[] = {
        {"golang.org", "missing port in address"},
        {"127.0.0.1", "missing port in address"},
        {"[::1]", "missing port in address"},
        {"[fe80::1%lo0]", "missing port in address"},
        {"[localhost%lo0]", "missing port in address"},
        {"localhost%lo0", "missing port in address"},

        {"::1", "too many colons in address"},
        {"fe80::1%lo0", "too many colons in address"},
        {"fe80::1%lo0:80", "too many colons in address"},

        /* Test cases that didn't fail in Go 1 */

        {"[foo:bar]", "missing port in address"},
        {"[foo:bar]baz", "missing port in address"},
        {"[foo]bar:baz", "missing port in address"},

        {"[foo]:[bar]:baz", "too many colons in address"},

        {"[foo]:[bar]baz", "unexpected '[' in address"},
        {"foo[bar]:baz", "unexpected '[' in address"},

        {"foo]bar:baz", "unexpected ']' in address"},
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        Error err = BURROW_NO_ERROR;
        struct {
            Str host, port;
        } r;
        r.host = net_split_host_port(str_from_cstr(bad[i].host_port), &r.port, &err);
        if (BURROW_OK(err)) {
            testing_t_errorf_v(t, "SplitHostPort(%q) should have failed",
                               bad[i].host_port);
            continue;
        }
        const NetAddrError *e = errors_as(err, TYPE_NET_ADDR_ERROR);
        if (e == NULL) {
            testing_t_errorf_v(t, "SplitHostPort(%q): %v is not an AddrError",
                               bad[i].host_port, err);
            continue;
        }
        if (!str_is(e->err, bad[i].err))
            testing_t_errorf_v(t, "SplitHostPort(%q) = _, _, %q; want %q",
                               bad[i].host_port, cz(a, e->err), bad[i].err);
        if (r.host.len != 0 || r.port.len != 0)
            testing_t_errorf_v(
                t, "SplitHostPort(%q) = %q, %q, err; want %q, %q, err on failure",
                bad[i].host_port, cz(a, r.host), cz(a, r.port), "", "");
    }
    arena_free(&ar);
}

static void TestJoinHostPort(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    static const struct {
        const char *host, *port, *host_port;
    } tests[] = {
        /* Host name */
        {"localhost", "http", "localhost:http"},
        {"localhost", "80", "localhost:80"},

        /* Go-specific host name with zone identifier */
        {"localhost%lo0", "http", "localhost%lo0:http"},
        {"localhost%lo0", "80", "localhost%lo0:80"},

        /* IP literal */
        {"127.0.0.1", "http", "127.0.0.1:http"},
        {"127.0.0.1", "80", "127.0.0.1:80"},
        {"::1", "http", "[::1]:http"},
        {"::1", "80", "[::1]:80"},

        /* IP literal with zone identifier */
        {"::1%lo0", "http", "[::1%lo0]:http"},
        {"::1%lo0", "80", "[::1%lo0]:80"},

        /* Go-specific wildcard for host name */
        {"", "http", ":http"}, /* Go 1 behavior */
        {"", "80", ":80"},     /* Go 1 behavior */

        /* Go-specific wildcard for service name or transport port number */
        {"golang.org", "", "golang.org:"}, /* Go 1 behavior */
        {"127.0.0.1", "", "127.0.0.1:"},   /* Go 1 behavior */
        {"::1", "", "[::1]:"},             /* Go 1 behavior */

        /* Opaque service name */
        {"golang.org", "https%foo", "golang.org:https%foo"}, /* Go 1 behavior */
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Str hp = net_join_host_port(a, str_from_cstr(tests[i].host),
                                    str_from_cstr(tests[i].port));
        if (!str_is(hp, tests[i].host_port))
            testing_t_errorf_v(t, "JoinHostPort(%q, %q) = %q; want %q", tests[i].host,
                               tests[i].port, cz(a, hp), tests[i].host_port);
    }
    arena_free(&ar);
}

/* ---------------------------------------------------------- families, scope */

static void TestIPAddrFamily(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    struct {
        NetIP in;
        bool af4, af6;
    } tests[] = {
        {net_ipv4_bcast, true, false},
        {net_ipv4_allsys, true, false},
        {net_ipv4_allrouter, true, false},
        {net_ipv4_zero, true, false},
        {v4(a, 224, 0, 0, 1), true, false},
        {v4(a, 127, 0, 0, 1), true, false},
        {v4(a, 240, 0, 0, 1), true, false},
        {net_ipv6_unspecified, false, true},
        {net_ipv6_loopback, false, true},
        {net_ipv6_interfacelocalallnodes, false, true},
        {net_ipv6_linklocalallnodes, false, true},
        {net_ipv6_linklocalallrouters, false, true},
        {pip(a, "ff05::a:b:c:d"), false, true},
        {pip(a, "fe80::1:2:3:4"), false, true},
        {pip(a, "2001:db8::123:12:1"), false, true},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        bool af = net_ip_to4(tests[i].in).p != NULL;
        if (af != tests[i].af4)
            testing_t_errorf_v(t, "verifying IPv4 address family for %q = %v, want %v",
                               ips(a, tests[i].in), af, tests[i].af4);
        af = tests[i].in.len == NET_IPV6_LEN && net_ip_to4(tests[i].in).p == NULL;
        if (af != tests[i].af6)
            testing_t_errorf_v(t, "verifying IPv6 address family for %q = %v, want %v",
                               ips(a, tests[i].in), af, tests[i].af6);
    }
    arena_free(&ar);
}

typedef bool (*Scope)(NetIP);

static void TestIPAddrScope(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    NetIP nil = slice_nil(TYPE_BYTE);
#define S(f) f, #f
    struct {
        Scope scope;
        const char *name;
        NetIP in;
        bool ok;
    } tests[] = {
        {S(net_ip_is_unspecified), net_ipv4_zero, true},
        {S(net_ip_is_unspecified), v4(a, 127, 0, 0, 1), false},
        {S(net_ip_is_unspecified), net_ipv6_unspecified, true},
        {S(net_ip_is_unspecified), net_ipv6_interfacelocalallnodes, false},
        {S(net_ip_is_unspecified), nil, false},
        {S(net_ip_is_loopback), v4(a, 127, 0, 0, 1), true},
        {S(net_ip_is_loopback), v4(a, 127, 255, 255, 254), true},
        {S(net_ip_is_loopback), v4(a, 128, 1, 2, 3), false},
        {S(net_ip_is_loopback), net_ipv6_loopback, true},
        {S(net_ip_is_loopback), net_ipv6_linklocalallrouters, false},
        {S(net_ip_is_loopback), nil, false},
        {S(net_ip_is_multicast), v4(a, 224, 0, 0, 0), true},
        {S(net_ip_is_multicast), v4(a, 239, 0, 0, 0), true},
        {S(net_ip_is_multicast), v4(a, 240, 0, 0, 0), false},
        {S(net_ip_is_multicast), net_ipv6_linklocalallnodes, true},
        {S(net_ip_is_multicast),
         IP_OF(a, 0xff, 0x05, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0), true},
        {S(net_ip_is_multicast),
         IP_OF(a, 0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0), false},
        {S(net_ip_is_multicast), nil, false},
        {S(net_ip_is_interface_local_multicast), v4(a, 224, 0, 0, 0), false},
        {S(net_ip_is_interface_local_multicast), v4(a, 0xff, 0x01, 0, 0), false},
        {S(net_ip_is_interface_local_multicast), net_ipv6_interfacelocalallnodes, true},
        {S(net_ip_is_interface_local_multicast), nil, false},
        {S(net_ip_is_link_local_multicast), v4(a, 224, 0, 0, 0), true},
        {S(net_ip_is_link_local_multicast), v4(a, 239, 0, 0, 0), false},
        {S(net_ip_is_link_local_multicast), v4(a, 0xff, 0x02, 0, 0), false},
        {S(net_ip_is_link_local_multicast), net_ipv6_linklocalallrouters, true},
        {S(net_ip_is_link_local_multicast),
         IP_OF(a, 0xff, 0x05, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0), false},
        {S(net_ip_is_link_local_multicast), nil, false},
        {S(net_ip_is_link_local_unicast), v4(a, 169, 254, 0, 0), true},
        {S(net_ip_is_link_local_unicast), v4(a, 169, 255, 0, 0), false},
        {S(net_ip_is_link_local_unicast), v4(a, 0xfe, 0x80, 0, 0), false},
        {S(net_ip_is_link_local_unicast),
         IP_OF(a, 0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0), true},
        {S(net_ip_is_link_local_unicast),
         IP_OF(a, 0xfe, 0xc0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0), false},
        {S(net_ip_is_link_local_unicast), nil, false},
        {S(net_ip_is_global_unicast), v4(a, 240, 0, 0, 0), true},
        {S(net_ip_is_global_unicast), v4(a, 232, 0, 0, 0), false},
        {S(net_ip_is_global_unicast), v4(a, 169, 254, 0, 0), false},
        {S(net_ip_is_global_unicast), net_ipv4_bcast, false},
        {S(net_ip_is_global_unicast),
         IP_OF(a, 0x20, 0x1, 0xd, 0xb8, 0, 0, 0, 0, 0, 0, 0x1, 0x23, 0, 0x12, 0, 0x1),
         true},
        {S(net_ip_is_global_unicast),
         IP_OF(a, 0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0), false},
        {S(net_ip_is_global_unicast),
         IP_OF(a, 0xff, 0x05, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0), false},
        {S(net_ip_is_global_unicast), nil, false},
        {S(net_ip_is_private), nil, false},
        {S(net_ip_is_private), v4(a, 1, 1, 1, 1), false},
        {S(net_ip_is_private), v4(a, 9, 255, 255, 255), false},
        {S(net_ip_is_private), v4(a, 10, 0, 0, 0), true},
        {S(net_ip_is_private), v4(a, 10, 255, 255, 255), true},
        {S(net_ip_is_private), v4(a, 11, 0, 0, 0), false},
        {S(net_ip_is_private), v4(a, 172, 15, 255, 255), false},
        {S(net_ip_is_private), v4(a, 172, 16, 0, 0), true},
        {S(net_ip_is_private), v4(a, 172, 16, 255, 255), true},
        {S(net_ip_is_private), v4(a, 172, 23, 18, 255), true},
        {S(net_ip_is_private), v4(a, 172, 31, 255, 255), true},
        {S(net_ip_is_private), v4(a, 172, 31, 0, 0), true},
        {S(net_ip_is_private), v4(a, 172, 32, 0, 0), false},
        {S(net_ip_is_private), v4(a, 192, 167, 255, 255), false},
        {S(net_ip_is_private), v4(a, 192, 168, 0, 0), true},
        {S(net_ip_is_private), v4(a, 192, 168, 255, 255), true},
        {S(net_ip_is_private), v4(a, 192, 169, 0, 0), false},
        {S(net_ip_is_private),
         IP_OF(a, 0xfb, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
               0xff, 0xff, 0xff, 0xff, 0xff),
         false},
        {S(net_ip_is_private),
         IP_OF(a, 0xfc, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0), true},
        {S(net_ip_is_private),
         IP_OF(a, 0xfc, 0xff, 0x12, 0, 0, 0, 0, 0x44, 0, 0, 0, 0, 0, 0, 0, 0), true},
        {S(net_ip_is_private),
         IP_OF(a, 0xfd, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
               0xff, 0xff, 0xff, 0xff, 0xff),
         true},
        {S(net_ip_is_private),
         IP_OF(a, 0xfe, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0), false},
    };
#undef S
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        bool ok = tests[i].scope(tests[i].in);
        if (ok != tests[i].ok)
            testing_t_errorf_v(t, "%s(%q) = %v, want %v", tests[i].name,
                               ips(a, tests[i].in), ok, tests[i].ok);
        NetIP ip = net_ip_to4(tests[i].in);
        if (ip.p == NULL)
            continue;
        ok = tests[i].scope(ip);
        if (ok != tests[i].ok)
            testing_t_errorf_v(t, "%s(%q) = %v, want %v", tests[i].name, ips(a, ip), ok,
                               tests[i].ok);
    }
    arena_free(&ar);
}

/* math/rand's Read in Go. A fixed generator is enough to make the addresses
 * differ. */
static uint64_t bench_rand_state = 1;

static Byte bench_rand_byte(void) {
    bench_rand_state =
        bench_rand_state * 6364136223846793005ULL + 1442695040888963407ULL;
    return (Byte)(bench_rand_state >> 56);
}

static void bench_ip_equal(void *env, TestingB *b) {
    Int size = (Int)(intptr_t)env;
    enum { NIPS = 1000 };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    NetIP *ipv = mem_alloc(a, sizeof(NetIP) * NIPS, _Alignof(NetIP));
    for (Int i = 0; i < NIPS; i++) {
        ipv[i] = slice_make(a, TYPE_BYTE, size, size);
        for (Int k = 0; k < size; k++)
            ((Byte *)ipv[i].p)[k] = bench_rand_byte();
    }
    Int n = testing_b_n(b);
    /* Half of the N are equal. */
    for (Int i = 0; i < n / 2; i++)
        sink = net_ip_equal(ipv[i % NIPS], ipv[i % NIPS]);
    /* The other half are not equal. */
    for (Int i = 0; i < n / 2; i++)
        sink = net_ip_equal(ipv[i % NIPS], ipv[(i + 1) % NIPS]);
    arena_free(&ar);
}

static void BenchmarkIPEqual(TestingB *b) {
    testing_b_run(b, BURROW_S("IPv4"),
                  BURROW_FN(TestingBFunc, bench_ip_equal, (void *)(intptr_t)4));
    testing_b_run(b, BURROW_S("IPv6"),
                  BURROW_FN(TestingBFunc, bench_ip_equal, (void *)(intptr_t)16));
}

/* ------------------------------------------------------------- the rest */

/* Not in Go's file: the errors' texts and methods, and the descriptors. */
static void TestErrors(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    NetParseError pe = {BURROW_S("IP address"), BURROW_S("abc")};
    CHECK_STR_EQ(cz(a, net_parse_error_error(&pe, a)), "invalid IP address: abc");
    Error e = net_parse_error_as_error(&pe, a);
    CHECK_STR_EQ(cz(a, error_text(e)), "invalid IP address: abc");
    CHECK(!net_parse_error_timeout(&pe) && !net_parse_error_temporary(&pe));

    NetAddrError ae = {BURROW_S("missing port in address"), BURROW_S("golang.org")};
    CHECK_STR_EQ(cz(a, net_addr_error_error(&ae, a)),
                 "address golang.org: missing port in address");
    ae.addr = BURROW_S("");
    CHECK_STR_EQ(cz(a, net_addr_error_error(&ae, a)), "missing port in address");
    CHECK_STR_EQ(cz(a, net_addr_error_error(NULL, a)), "<nil>");
    CHECK(!net_addr_error_timeout(&ae) && !net_addr_error_temporary(&ae));

    Error err = BURROW_NO_ERROR;
    net_split_host_port(BURROW_S("::1"), NULL, &err);
    CHECK_STR_EQ(cz(a, error_text(err)), "address ::1: too many colons in address");
    Error clone = error_retain(a, err);
    CHECK_STR_EQ(cz(a, error_text(clone)), "address ::1: too many colons in address");

    CHECK(type_method_by_name(TYPE_NET_PARSE_ERROR, BURROW_S("Timeout")) != NULL);
    CHECK(type_method_by_name(TYPE_NET_ADDR_ERROR, BURROW_S("Temporary")) != NULL);
    CHECK(type_method_by_name(TYPE_NET_IP, BURROW_S("String")) != NULL);
    CHECK(type_method_by_name(TYPE_NET_IP, BURROW_S("UnmarshalText")) != NULL);
    CHECK(type_method_by_name(TYPE_NET_IP_MASK, BURROW_S("String")) != NULL);
    CHECK(type_method_by_name(TYPE_NET_IP_NET, BURROW_S("Network")) != NULL);
    CHECK_STR_EQ(cz(a, net_ip_net_network(NULL)), "ip+net");

    NetIP ip = pip(a, "10.1.2.3");
    CHECK_STR_EQ(cz(a, net_ip_string(net_ip_default_mask(ip), a)), "255.0.0.0");
    CHECK_STR_EQ(cz(a, net_ip_string(net_ip_default_mask(pip(a, "172.16.0.1")), a)),
                 "255.255.0.0");
    CHECK_STR_EQ(cz(a, net_ip_string(net_ip_default_mask(pip(a, "192.0.2.1")), a)),
                 "255.255.255.0");
    CHECK(net_ip_default_mask(pip(a, "::1")).p == NULL);
    CHECK(net_ip_to16(IP_OF(a, 10, 1, 2, 3), a).len == 16);
    CHECK(net_ip_equal(net_ip_to16(IP_OF(a, 10, 1, 2, 3), a), ip));
    CHECK(net_ip_to16(IP_OF(a, 1, 2, 3), a).p == NULL);
    Int bits = 0;
    CHECK_INT_EQ(net_ip_mask_size(net_cidr_mask(a, 20, 32), &bits), 20);
    CHECK_INT_EQ(bits, 32);
    CHECK_INT_EQ(net_ip_mask_size(net_ipv4_mask(a, 255, 0, 255, 0), &bits), 0);
    CHECK_INT_EQ(bits, 0);
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestParseIP)                                                                     \
    X(TestMarshalEmptyIP)                                                              \
    X(TestIPString)                                                                    \
    X(TestIPAppendTextNoAllocs)                                                        \
    X(TestIPMask)                                                                      \
    X(TestIPMaskString)                                                                \
    X(TestParseCIDR)                                                                   \
    X(TestIPNetContains)                                                               \
    X(TestIPNetString)                                                                 \
    X(TestCIDRMask)                                                                    \
    X(TestNetworkNumberAndMask)                                                        \
    X(TestSplitHostPort)                                                               \
    X(TestJoinHostPort)                                                                \
    X(TestIPAddrFamily)                                                                \
    X(TestIPAddrScope)                                                                 \
    X(TestErrors)                                                                      \
    X(BenchmarkParseIP)                                                                \
    X(BenchmarkParseIPValidIPv4)                                                       \
    X(BenchmarkParseIPValidIPv6)                                                       \
    X(BenchmarkIPMarshalText)                                                          \
    X(BenchmarkIPString)                                                               \
    X(BenchmarkIPMaskString)                                                           \
    X(BenchmarkIPEqual)

TESTING_MAIN(TESTS)

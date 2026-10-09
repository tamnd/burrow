/* HardwareAddr and ParseMAC, from Go's mac_test.go.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/burrow.h"
#include "burrow/error.h"
#include "burrow/mem/arena.h"
#include "burrow/net.h"
#include "burrow/strings.h"
#include "burrow/testing.h"

#include "check.h"

#include <string.h>

static Arena ar;
static Alloc *a;

typedef struct ParseMACTest {
    const char *in;
    Byte out[20];
    Int n; /* 0 for nil */
    const char *err;
} ParseMACTest;

static const ParseMACTest parse_mac_tests[] = {
    /* See RFC 7042, Section 2.1.1. */
    {"00:00:5e:00:53:01", {0x00, 0x00, 0x5e, 0x00, 0x53, 0x01}, 6, ""},
    {"00-00-5e-00-53-01", {0x00, 0x00, 0x5e, 0x00, 0x53, 0x01}, 6, ""},
    {"0000.5e00.5301", {0x00, 0x00, 0x5e, 0x00, 0x53, 0x01}, 6, ""},
    {"00005e005301", {0x00, 0x00, 0x5e, 0x00, 0x53, 0x01}, 6, ""},

    /* See RFC 7042, Section 2.2.2. */
    {"02:00:5e:10:00:00:00:01",
     {0x02, 0x00, 0x5e, 0x10, 0x00, 0x00, 0x00, 0x01},
     8,
     ""},
    {"02-00-5e-10-00-00-00-01",
     {0x02, 0x00, 0x5e, 0x10, 0x00, 0x00, 0x00, 0x01},
     8,
     ""},
    {"0200.5e10.0000.0001", {0x02, 0x00, 0x5e, 0x10, 0x00, 0x00, 0x00, 0x01}, 8, ""},
    {"02005e1000000001", {0x02, 0x00, 0x5e, 0x10, 0x00, 0x00, 0x00, 0x01}, 8, ""},

    /* See RFC 4391, Section 9.1.1. */
    {"00:00:00:00:fe:80:00:00:00:00:00:00:02:00:5e:10:00:00:00:01",
     {0x00, 0x00, 0x00, 0x00, 0xfe, 0x80, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x02, 0x00, 0x5e, 0x10, 0x00, 0x00, 0x00, 0x01},
     20,
     ""},
    {"00-00-00-00-fe-80-00-00-00-00-00-00-02-00-5e-10-00-00-00-01",
     {0x00, 0x00, 0x00, 0x00, 0xfe, 0x80, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x02, 0x00, 0x5e, 0x10, 0x00, 0x00, 0x00, 0x01},
     20,
     ""},
    {"0000.0000.fe80.0000.0000.0000.0200.5e10.0000.0001",
     {0x00, 0x00, 0x00, 0x00, 0xfe, 0x80, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x02, 0x00, 0x5e, 0x10, 0x00, 0x00, 0x00, 0x01},
     20,
     ""},
    {"00000000fe8000000000000002005e1000000001",
     {0x00, 0x00, 0x00, 0x00, 0xfe, 0x80, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x02, 0x00, 0x5e, 0x10, 0x00, 0x00, 0x00, 0x01},
     20,
     ""},

    {"ab:cd:ef:AB:CD:EF", {0xab, 0xcd, 0xef, 0xab, 0xcd, 0xef}, 6, ""},
    {"ab:cd:ef:AB:CD:EF:ab:cd",
     {0xab, 0xcd, 0xef, 0xab, 0xcd, 0xef, 0xab, 0xcd},
     8,
     ""},
    {"ab:cd:ef:AB:CD:EF:ab:cd:ef:AB:CD:EF:ab:cd:ef:AB:CD:EF:ab:cd",
     {0xab, 0xcd, 0xef, 0xab, 0xcd, 0xef, 0xab, 0xcd, 0xef, 0xab,
      0xcd, 0xef, 0xab, 0xcd, 0xef, 0xab, 0xcd, 0xef, 0xab, 0xcd},
     20,
     ""},

    {"01.02.03.04.05.06", {0}, 0, "invalid MAC address"},
    {"01:02:03:04:05:06:", {0}, 0, "invalid MAC address"},
    {"x1:02:03:04:05:06", {0}, 0, "invalid MAC address"},
    {"01002:03:04:05:06", {0}, 0, "invalid MAC address"},
    {"01:02003:04:05:06", {0}, 0, "invalid MAC address"},
    {"01:02:03004:05:06", {0}, 0, "invalid MAC address"},
    {"01:02:03:04005:06", {0}, 0, "invalid MAC address"},
    {"01:02:03:04:05006", {0}, 0, "invalid MAC address"},
    {"01-02:03:04:05:06", {0}, 0, "invalid MAC address"},
    {"01:02-03-04-05-06", {0}, 0, "invalid MAC address"},
    {"0123:4567:89AF", {0}, 0, "invalid MAC address"},
    {"0123-4567-89AF", {0}, 0, "invalid MAC address"},
    {"0123456789AF0", {0}, 0, "invalid MAC address"},
};

/* reflect.DeepEqual for a HardwareAddr and the bytes a case wants, with nil
 * apart from empty. */
static bool same_mac(NetHardwareAddr hw, const Byte *want, Int n) {
    if (n == 0)
        return hw.p == NULL;
    return hw.len == n && memcmp(hw.p, want, (size_t)n) == 0;
}

static bool match(Error err, const char *s) {
    if (s[0] == 0)
        return !BURROW_FAILED(err);
    return BURROW_FAILED(err) && strings_contains(error_text(err), str_from_cstr(s));
}

static void TestParseMAC(TestingT *t) {
    for (size_t i = 0; i < sizeof parse_mac_tests / sizeof parse_mac_tests[0]; i++) {
        const ParseMACTest *tt = &parse_mac_tests[i];
        Error err = BURROW_NO_ERROR;
        NetHardwareAddr out = net_parse_mac(a, str_from_cstr(tt->in), &err);
        if (!same_mac(out, tt->out, tt->n) || !match(err, tt->err))
            testing_t_errorf_v(t, "ParseMAC(%q) = %s, %v, want %s", tt->in,
                               net_hardware_addr_string(out, a), err, tt->err);
        if (tt->err[0] == 0) {
            /* Serialization works too, and round-trips. */
            Str s = net_hardware_addr_string(out, a);
            Error err2 = BURROW_NO_ERROR;
            NetHardwareAddr out2 = net_parse_mac(a, s, &err2);
            if (BURROW_FAILED(err2)) {
                testing_t_errorf_v(t, "%d. ParseMAC(%q) = %v", (Int)i, s, err2);
                continue;
            }
            if (!same_mac(out2, (const Byte *)out.p, out.len))
                testing_t_errorf_v(t, "%d. ParseMAC(%q) = %s, want %s", (Int)i, s,
                                   net_hardware_addr_string(out2, a),
                                   net_hardware_addr_string(out, a));
        }
    }
}

#define TESTS(X) X(TestParseMAC)

static int net_mac_main(TestingM *m) {
    arena_init(&ar, NULL, 0);
    a = arena_allocator(&ar);
    int r = testing_m_run(m);
    arena_free(&ar);
    return r;
}

TESTING_MAIN_WITH(net_mac_main, TESTS)

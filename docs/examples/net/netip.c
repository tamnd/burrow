#include <stdio.h>

#include "burrow/burrow.h"

static void parse(void) {
    // doc: parse
    Error err;
    NetipAddr ip = netip_parse_addr(BURROW_S("192.168.1.20"), &err);
    NetipPrefix lan = netip_parse_prefix(BURROW_S("192.168.0.0/16"), &err);
    NetipAddrPort ap = netip_parse_addr_port(BURROW_S("[fe80::1%eth0]:8080"), &err);
    printf("%d %d\n", netip_prefix_contains(lan, ip),
           netip_addr_is_private(ip)); /* 1 1 */
    fmt_printf_v("%v %v %v\n", BURROW_ANY(TYPE_NETIP_ADDR, &ip),
                 BURROW_ANY(TYPE_NETIP_PREFIX, &lan),
                 BURROW_ANY(TYPE_NETIP_ADDR_PORT, &ap));
    netip_parse_addr(BURROW_S("300.1.2.3"), &err);
    fmt_printf_v("%v\n", err);
    // doc: end
}

static void text(Alloc *a) {
    // doc: text
    NetipAddr ip = netip_must_parse_addr(BURROW_S("2001:db8::1"));
    Str s = netip_addr_string(ip, a);
    Str long_form = netip_addr_string_expanded(ip, a);
    printf("%.*s\n%.*s\n", (int)s.len, s.p, (int)long_form.len, long_form.p);

    Byte buf[64];
    Slice b = slice_from(buf, 0, sizeof buf, TYPE_BYTE);
    NetipPrefix p = netip_must_parse_prefix(BURROW_S("10.1.2.3/8"));
    b = netip_prefix_append_to(p, a, b);
    b = slice_append(a, b, " is in ", 7);
    b = netip_prefix_append_to(netip_prefix_masked(p), a, b);
    printf("%.*s\n", (int)b.len, (const char *)b.p);
    // doc: end
}

static void walk(void) {
    // doc: walk
    NetipPrefix p = netip_must_parse_prefix(BURROW_S("10.0.0.252/30"));
    for (NetipAddr ip = netip_prefix_addr(p); netip_prefix_contains(p, ip);
         ip = netip_addr_next(ip))
        fmt_printf_v("%v\n", BURROW_ANY(TYPE_NETIP_ADDR, &ip));
    // doc: end
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    parse();
    text(arena_allocator(&ar));
    walk();
    arena_free(&ar);
    return 0;
}

/* Output:
1 1
192.168.1.20 192.168.0.0/16 [fe80::1%eth0]:8080
ParseAddr("300.1.2.3"): IPv4 field has value >255
2001:db8::1
2001:0db8:0000:0000:0000:0000:0000:0001
10.1.2.3/8 is in 10.0.0.0/8
10.0.0.252
10.0.0.253
10.0.0.254
10.0.0.255
*/

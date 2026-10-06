#include <stdio.h>

#include "burrow/burrow.h"

static void print(Str s) {
    printf("%.*s\n", (int)s.len, s.p);
}

static void ip(Alloc *a) {
    // doc: ip
    NetIP ip = net_parse_ip(a, BURROW_S("192.0.2.77"));
    printf("%d %d\n", (int)ip.len, (int)net_ip_to4(ip).len); /* 16 4 */

    Error err;
    NetIPNet *lan;
    NetIP host = net_parse_cidr(a, BURROW_S("10.1.2.3/20"), &lan, &err);
    print(net_ip_string(host, a));
    print(net_ip_net_string(lan, a));
    printf("%d %d\n", net_ip_net_contains(lan, net_ipv4(a, 10, 1, 15, 1)),
           net_ip_net_contains(lan, ip));

    NetIP v6 = net_parse_ip(a, BURROW_S("2001:db8:0:0:1::1"));
    print(net_ip_string(net_ip_mask(v6, a, net_cidr_mask(a, 64, 128)), a));

    net_parse_cidr(a, BURROW_S("10.1.2.3/33"), NULL, &err);
    fmt_printf_v("%v\n", err);
    // doc: end
}

static void hostport(Alloc *a) {
    // doc: hostport
    Error err;
    Str port;
    Str host = net_split_host_port(BURROW_S("[fe80::1%eth0]:8080"), &port, &err);
    printf("%.*s %.*s\n", (int)host.len, host.p, (int)port.len, port.p);
    print(net_join_host_port(a, host, BURROW_S("443")));
    print(net_join_host_port(a, BURROW_S("example.com"), BURROW_S("https")));

    net_split_host_port(BURROW_S("example.com"), &port, &err);
    fmt_printf_v("%v\n", err);
    // doc: end
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    ip(arena_allocator(&ar));
    hostport(arena_allocator(&ar));
    arena_free(&ar);
    return 0;
}

/* Output:
*/

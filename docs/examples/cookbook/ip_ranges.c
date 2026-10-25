// doc: main
#define BURROW_SHORT 1
#include "burrow/burrow.h"

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    /* A prefix written with host bits set is still a prefix. Masked clears
     * them, which is what you want before you print it or compare two. */
    const char *nets[] = {"10.0.0.0/8", "192.168.1.77/24", "2001:db8::/32"};
    NetipPrefix prefixes[3];
    for (int i = 0; i < 3; i++) {
        Error err = NO_ERROR;
        NetipPrefix p = netip_parse_prefix(str_from_cstr(nets[i]), &err);
        if (FAILED(err)) {
            fmt_println_v(err);
            return 1;
        }
        prefixes[i] = netip_prefix_masked(p);
        Str s = netip_prefix_string(prefixes[i], a);
        fmt_printf_v("network %s\n", s);
    }

    const char *addrs[] = {"10.1.2.3", "192.168.1.200", "2001:db8::1", "127.0.0.1",
                           "300.1.1.1"};
    for (int i = 0; i < 5; i++) {
        Error err = NO_ERROR;
        NetipAddr ip = netip_parse_addr(str_from_cstr(addrs[i]), &err);
        if (FAILED(err)) {
            fmt_println_v(err);
            continue;
        }
        Str where = S("no network");
        for (int j = 0; j < 3; j++) {
            if (netip_prefix_contains(prefixes[j], ip)) {
                where = netip_prefix_string(prefixes[j], a);
                break;
            }
        }
        Str s = netip_addr_string(ip, a);
        bool private = netip_addr_is_private(ip);
        bool loopback = netip_addr_is_loopback(ip);
        fmt_printf_v("%-14s in %-15s private %-5t loopback %t\n", s, where, private,
                     loopback);
    }
    arena_free(&ar);
}
// doc: end

/* Output:
network 10.0.0.0/8
network 192.168.1.0/24
network 2001:db8::/32
10.1.2.3       in 10.0.0.0/8      private true  loopback false
192.168.1.200  in 192.168.1.0/24  private true  loopback false
2001:db8::1    in 2001:db8::/32   private false loopback false
127.0.0.1      in no network      private false loopback true
ParseAddr("300.1.1.1"): IPv4 field has value >255
*/

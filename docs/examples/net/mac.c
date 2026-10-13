#include <stdio.h>

#include "burrow/burrow.h"

static void print(Str s) {
    printf("%.*s\n", (int)s.len, s.p);
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    // doc: mac
    Error err;
    NetHardwareAddr hw = net_parse_mac(a, BURROW_S("00-00-5E-00-53-01"), &err);
    print(net_hardware_addr_string(hw, a));

    /* Cisco's dotted form, and an EUI-64. */
    hw = net_parse_mac(a, BURROW_S("0200.5e10.0000.0001"), &err);
    printf("%d ", (int)hw.len);
    print(net_hardware_addr_string(hw, a));

    net_parse_mac(a, BURROW_S("01:02:03:04:05"), &err);
    fmt_printf_v("%v\n", err);
    // doc: end
    arena_free(&ar);
    return 0;
}

/* Output:
00:00:5e:00:53:01
8 02:00:5e:10:00:00:00:01
address 01:02:03:04:05: invalid MAC address
*/

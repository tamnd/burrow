#include <stdio.h>

#include "burrow/burrow.h"

static void run(void *env) {
    (void)env;
    // doc: multicast
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err;

    /* The loopback interface, which every machine has. */
    const NetInterface *lo = NULL;
    Slice ift = net_interfaces(a, &err);
    for (Int i = 0; i < ift.len && lo == NULL; i++) {
        const NetInterface *ifi = &((const NetInterface *)ift.p)[i];
        if ((ifi->flags & NET_FLAG_LOOPBACK) != 0 && (ifi->flags & NET_FLAG_UP) != 0)
            lo = ifi;
    }

    /* 224.0.0.254 is a group set aside for experiments. */
    NetUDPAddr group = {net_ipv4(a, 224, 0, 0, 254), 12345, BURROW_STR_EMPTY};
    NetUDPConn *c =
        net_listen_multicast_udp(heap_allocator(), BURROW_S("udp4"), lo, &group, &err);
    if (c == NULL) {
        fmt_printf_v("%v\n", err);
        arena_free(&ar);
        return;
    }
    NetAddr la = net_udp_conn_local_addr(c);
    Str s = la.vt->string(la.data, a);
    printf("listening on %.*s\n", (int)s.len, (const char *)s.p);

    /* The group is in the interface's list of joined groups now. */
    if (lo != NULL) {
        Slice groups = net_interface_multicast_addrs(lo, a, &err);
        for (Int i = 0; i < groups.len; i++) {
            NetAddr g = ((const NetAddr *)groups.p)[i];
            Str gs = g.vt->string(g.data, a);
            printf("    %.*s\n", (int)gs.len, (const char *)gs.p);
        }
    }
    net_udp_conn_free(c);
    arena_free(&ar);
    // doc: end
}

int main(void) {
    runtime_main(BURROW_FN(Func, run, NULL));
    return 0;
}

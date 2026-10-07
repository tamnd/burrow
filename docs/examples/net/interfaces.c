#include <stdio.h>

#include "burrow/burrow.h"

static void run(void *env) {
    (void)env;
    // doc: interfaces
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err;
    Slice ift = net_interfaces(a, &err);
    if (BURROW_FAILED(err)) {
        fmt_printf_v("%v\n", err);
        arena_free(&ar);
        return;
    }
    for (Int i = 0; i < ift.len; i++) {
        const NetInterface *ifi = &((const NetInterface *)ift.p)[i];
        Str flags = net_flags_string(ifi->flags, a);
        Str hw = net_hardware_addr_string(ifi->hardware_addr, a);
        printf("%d %.*s mtu %d <%.*s> %.*s\n", (int)ifi->index, (int)ifi->name.len,
               (const char *)ifi->name.p, (int)ifi->mtu, (int)flags.len,
               (const char *)flags.p, (int)hw.len, (const char *)hw.p);

        /* Unicast addresses come back as NetIPNet, with the prefix. */
        Slice addrs = net_interface_addrs_of(ifi, a, &err);
        for (Int j = 0; j < addrs.len; j++) {
            NetAddr x = ((const NetAddr *)addrs.p)[j];
            Str s = x.vt->string(x.data, a);
            printf("    %.*s\n", (int)s.len, (const char *)s.p);
        }
    }
    arena_free(&ar);
    // doc: end
}

int main(void) {
    runtime_main(BURROW_FN(Func, run, NULL));
    return 0;
}

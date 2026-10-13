#include <stdio.h>

#include "burrow/burrow.h"

static bool has(Slice addrs, const char *want) {
    for (Int i = 0; i < addrs.len; i++)
        if (str_eq(((const Str *)addrs.p)[i], str_from_cstr(want)))
            return true;
    return false;
}

static void run(void *env) {
    (void)env;
    // doc: lookup
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err;

    /* A service by name. The system is asked first where there is one, and
     * Go's own table answers when it does not know. */
    Int port = net_lookup_port(BURROW_S("tcp"), BURROW_S("https"), &err);
    printf("https is port %d\n", (int)port);

    /* A host, through whichever resolver the system's configuration and
     * GODEBUG=netdns= pick. */
    Slice addrs = net_lookup_host(a, BURROW_S("localhost"), &err);
    printf("localhost has 127.0.0.1: %s\n", has(addrs, "127.0.0.1") ? "yes" : "no");

    /* The same through Go's resolver, which reads the hosts file itself. */
    NetResolver r = {0};
    r.prefer_go = true;
    addrs = net_resolver_lookup_host(&r, a, context_background(), BURROW_S("localhost"),
                                     &err);
    printf("and so says Go's: %s\n", has(addrs, "127.0.0.1") ? "yes" : "no");
    arena_free(&ar);
    // doc: end
}

int main(void) {
    runtime_main(BURROW_FN(Func, run, NULL));
    return 0;
}

/* Output:
https is port 443
localhost has 127.0.0.1: yes
and so says Go's: yes
*/

#include <stdio.h>

#include "burrow/burrow.h"

/* The server: greet one connection and hang up. */
static void serve(void *env) {
    NetListener *l = env;
    NetConn c = l->vt->accept(l->data, NULL);
    if (c.vt == NULL)
        return;
    io_write_string(net_conn_as_io_writer(c), BURROW_S("hello from the server"), NULL);
    net_conn_free(c);
}

static void print_str(Str s) {
    printf("%.*s\n", (int)s.len, (const char *)s.p);
}

static void run(void *env) {
    (void)env;
    // doc: dial
    /* Connections go in the heap, since the server's goroutine makes one
     * too. */
    Alloc *heap = heap_allocator();
    Error err;
    NetListener l = net_listen(heap, BURROW_S("tcp"), BURROW_S("127.0.0.1:0"), &err);
    if (l.vt == NULL)
        return;
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, serve, &l));

    /* The text this goroutine makes goes in an arena. The listener's
     * address as text is something Dial takes. */
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    NetAddr la = l.vt->addr(l.data);
    Str address = la.vt->string(la.data, a);
    NetDialer d = {0};
    d.timeout = 5 * TIME_SECOND;
    NetConn c = net_dialer_dial(&d, heap, BURROW_S("tcp"), address, &err);
    if (c.vt != NULL) {
        Slice got = io_read_all(a, net_conn_as_io_reader(c), &err);
        printf("%.*s\n", (int)got.len, (const char *)got.p);
        net_conn_free(c);
    }
    /* Closing the listener stops an accept that is still waiting. */
    (void)l.vt->closer.close(l.data);
    sync_wait_group_wait(&wg);
    net_listener_free(l);

    /* Ports by name, and zones, the way ResolveTCPAddr reads them. */
    NetTCPAddr *ta =
        net_resolve_tcp_addr(a, BURROW_S("tcp"), BURROW_S("[::1%lo]:http"), &err);
    if (ta != NULL) {
        printf("port %d zone %.*s\n", (int)ta->port, (int)ta->zone.len,
               (const char *)ta->zone.p);
        print_str(net_tcp_addr_string(ta, a));
    }

    /* A dial that cannot start says why, as an OpError. */
    c = net_dial(heap, BURROW_S("tcp6"), BURROW_S("127.0.0.1:80"), &err);
    if (c.vt == NULL)
        print_str(error_text(err));
    arena_free(&ar);
    // doc: end
}

int main(void) {
    runtime_main(BURROW_FN(Func, run, NULL));
    return 0;
}

/* Output:
hello from the server
port 80 zone lo
[::1%lo]:80
dial tcp6: address 127.0.0.1: no suitable address found
*/

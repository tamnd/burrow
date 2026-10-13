#include <stdio.h>

#include "burrow/burrow.h"

/* A client for the listener that comes back from the file. */
static void dial(void *env) {
    NetAddr *addr = env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Error err;
    NetConn c = net_dial(heap_allocator(), addr->vt->network(addr->data),
                         addr->vt->string(addr->data, arena_allocator(&ar)), &err);
    if (c.vt != NULL)
        net_conn_free(c);
    arena_free(&ar);
}

static void run(void *env) {
    (void)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err;
    NetListener l =
        net_listen(heap_allocator(), BURROW_S("tcp"), BURROW_S("127.0.0.1:0"), &err);
    if (l.vt == NULL) {
        arena_free(&ar);
        return;
    }
    // doc: file
    /* A copy of the listening socket as a file, which outlives the listener. */
    NetAddr was = l.vt->addr(l.data);
    Str before = was.vt->string(was.data, a);
    OsFile *f =
        net_tcp_listener_file(net_listener_as_tcp_listener(l), heap_allocator(), &err);
    net_listener_free(l);
    if (f == NULL) {
        fmt_printf_v("%v\n", err);
        arena_free(&ar);
        return;
    }

    /* And a listener again, from the file, on the same address. */
    NetListener l2 = net_file_listener(heap_allocator(), f, &err);
    (void)os_file_close(f);
    os_file_free(f);
    if (l2.vt == NULL) {
        fmt_printf_v("%v\n", err);
        arena_free(&ar);
        return;
    }
    NetAddr now = l2.vt->addr(l2.data);
    Str after = now.vt->string(now.data, a);
    printf("same address: %s\n", str_eq(before, after) ? "yes" : "no");

    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, dial, &now));
    NetConn c = l2.vt->accept(l2.data, &err);
    if (c.vt != NULL) {
        NetAddr ra = c.vt->remote_addr(c.data);
        Str network = ra.vt->network(ra.data);
        printf("accepted a %.*s connection\n", (int)network.len,
               (const char *)network.p);
        net_conn_free(c);
    }
    sync_wait_group_wait(&wg);
    net_listener_free(l2);
    // doc: end
    arena_free(&ar);
}

int main(void) {
    runtime_main(BURROW_FN(Func, run, NULL));
    return 0;
}

/* Output:
same address: yes
accepted a tcp connection
*/

#include <stdio.h>

#include "burrow/burrow.h"

/* The server: accept one connection and send back what comes in until the
 * client says it is done writing. */
static void serve(void *env) {
    NetTCPListener *l = env;
    NetTCPConn *c = net_tcp_listener_accept_tcp(l, NULL);
    if (c == NULL)
        return;
    NetConn conn = net_tcp_conn_as_conn(c);
    io_copy(heap_allocator(), net_conn_as_io_writer(conn), net_conn_as_io_reader(conn),
            NULL);
    net_tcp_conn_free(c);
}

static void run(void *env) {
    (void)env;
    // doc: tcp
    Byte loopback[4] = {127, 0, 0, 1};
    NetTCPAddr laddr = {slice_from(loopback, 4, 4, TYPE_BYTE), 0, BURROW_STR_EMPTY};
    Error err;
    NetTCPListener *l = net_listen_tcp(heap_allocator(), BURROW_S("tcp"), &laddr, &err);
    if (l == NULL)
        return;
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, serve, l));

    /* Port 0 asked the system for a free port, and the address says which. */
    const NetTCPAddr *bound = net_tcp_listener_addr(l).data;
    NetTCPAddr raddr = {laddr.ip, bound->port, BURROW_STR_EMPTY};
    NetTCPConn *c = net_dial_tcp(heap_allocator(), BURROW_S("tcp"), NULL, &raddr, &err);
    if (c == NULL)
        return;
    NetConn conn = net_tcp_conn_as_conn(c);
    io_write_string(net_conn_as_io_writer(conn), BURROW_S("hello"), &err);
    net_tcp_conn_close_write(c);

    Byte buf[16];
    Int n = io_read_full(net_conn_as_io_reader(conn), slice_from(buf, 5, 5, TYPE_BYTE),
                         &err);
    printf("%.*s\n", (int)n, (const char *)buf);
    const NetTCPAddr *remote = net_tcp_conn_remote_addr(c).data;
    printf("same port: %d\n", remote->port == bound->port);

    sync_wait_group_wait(&wg);
    net_tcp_conn_free(c);
    net_tcp_listener_free(l);

    /* Errors read the way Go's do. */
    raddr.port = 80;
    net_dial_tcp(heap_allocator(), BURROW_S("tcp5"), NULL, &raddr, &err);
    Str msg = error_text(err);
    printf("%.*s\n", (int)msg.len, (const char *)msg.p);
    // doc: end
}

int main(void) {
    runtime_main(BURROW_FN(Func, run, NULL));
    return 0;
}

/* Output:
hello
same port: 1
dial tcp5 127.0.0.1:80: unknown network tcp5
*/

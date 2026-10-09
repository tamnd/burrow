#include <stdio.h>

#include "burrow/burrow.h"

/* The server: read everything the client sends and print it. */
static void serve(void *env) {
    NetTCPListener *l = env;
    NetTCPConn *c = net_tcp_listener_accept_tcp(l, NULL);
    if (c == NULL)
        return;
    Error err;
    Slice all = io_read_all(heap_allocator(),
                            net_conn_as_io_reader(net_tcp_conn_as_conn(c)), &err);
    printf("server got %d bytes: %.*s", (int)all.len, (int)all.len,
           (const char *)all.p);
    mem_free(heap_allocator(), all.p, (size_t)all.cap, 1);
    net_tcp_conn_free(c);
}

static void run(void *env) {
    (void)env;
    Byte loopback[4] = {127, 0, 0, 1};
    NetTCPAddr laddr = {slice_from(loopback, 4, 4, TYPE_BYTE), 0, BURROW_STR_EMPTY};
    Error err;
    NetTCPListener *l = net_listen_tcp(heap_allocator(), BURROW_S("tcp"), &laddr, &err);
    if (l == NULL)
        return;
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, serve, l));
    const NetTCPAddr *bound = net_tcp_listener_addr(l).data;
    NetTCPAddr raddr = {laddr.ip, bound->port, BURROW_STR_EMPTY};
    NetTCPConn *c = net_dial_tcp(heap_allocator(), BURROW_S("tcp"), NULL, &raddr, &err);
    if (c == NULL)
        return;
    // doc: buffers
    char one[] = "first line\n";
    char two[] = "second line\n";
    char three[] = "third line\n";
    Slice parts[3] = {
        slice_from(one, (Int)sizeof one - 1, (Int)sizeof one - 1, TYPE_BYTE),
        slice_from(two, (Int)sizeof two - 1, (Int)sizeof two - 1, TYPE_BYTE),
        slice_from(three, (Int)sizeof three - 1, (Int)sizeof three - 1, TYPE_BYTE),
    };
    NetBuffers v = slice_from(parts, 3, 3, TYPE_BYTES);

    /* One writev for all three, since the writer is a connection. */
    NetConn conn = net_tcp_conn_as_conn(c);
    int64_t n = net_buffers_write_to(&v, net_conn_as_io_writer(conn), &err);
    printf("wrote %d bytes, %d buffers left\n", (int)n, (int)v.len);
    // doc: end
    net_tcp_conn_free(c);
    sync_wait_group_wait(&wg);
    net_tcp_listener_free(l);
}

int main(void) {
    runtime_main(BURROW_FN(Func, run, NULL));
    return 0;
}

/* Output:
wrote 34 bytes, 0 buffers left
server got 34 bytes: first line
second line
third line
*/

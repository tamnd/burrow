#include <stdint.h>
#include <stdio.h>

#include "burrow/burrow.h"

static void run(void *env) {
    (void)env;
    // doc: udp
    Byte loopback[4] = {127, 0, 0, 1};
    NetUDPAddr laddr = {slice_from(loopback, 4, 4, TYPE_BYTE), 0, BURROW_STR_EMPTY};
    Error err;
    NetUDPConn *server =
        net_listen_udp(heap_allocator(), BURROW_S("udp"), &laddr, &err);
    NetUDPConn *client =
        net_listen_udp(heap_allocator(), BURROW_S("udp"), &laddr, &err);
    if (server == NULL || client == NULL)
        return;

    /* Neither socket is connected, so each datagram says where it goes. */
    NetipAddrPort to = net_udp_addr_addr_port(net_udp_conn_local_addr(server).data);
    char ping[] = "ping";
    net_udp_conn_write_to_udp_addr_port(client, slice_from(ping, 4, 4, TYPE_BYTE), to,
                                        &err);

    Byte buf[64];
    NetipAddrPort from;
    Int n = net_udp_conn_read_from_udp_addr_port(
        server, slice_from(buf, 0, (Int)sizeof buf, TYPE_BYTE), &from, &err);
    Str ip = netip_addr_string(netip_addr_port_addr(from), heap_allocator());
    printf("%.*s from %.*s\n", (int)n, (const char *)buf, (int)ip.len,
           (const char *)ip.p);
    const NetUDPAddr *client_addr = net_udp_conn_local_addr(client).data;
    printf("same port: %d\n", netip_addr_port_port(from) == client_addr->port);
    mem_free(heap_allocator(), (void *)(uintptr_t)ip.p, (size_t)ip.len, 1);

    net_udp_conn_free(client);
    net_udp_conn_free(server);

    /* Errors read the way Go's do. */
    laddr.port = 53;
    if (net_dial_udp(heap_allocator(), BURROW_S("udp5"), NULL, &laddr, &err) == NULL) {
        Str msg = error_text(err);
        printf("%.*s\n", (int)msg.len, (const char *)msg.p);
    }
    // doc: end
}

int main(void) {
    runtime_main(BURROW_FN(Func, run, NULL));
    return 0;
}

/* Output:
ping from 127.0.0.1
same port: 1
dial udp5 127.0.0.1:53: unknown network udp5
*/

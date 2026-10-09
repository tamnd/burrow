#include <stdio.h>
#include <string.h>

#include "burrow/burrow.h"

static void run(void *env) {
    (void)env;
    // doc: listen-packet
    Alloc *heap = heap_allocator();
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err;
    NetPacketConn pc =
        net_listen_packet(heap, BURROW_S("udp"), BURROW_S("127.0.0.1:0"), &err);
    if (pc.vt == NULL) {
        arena_free(&ar);
        return;
    }
    NetAddr la = pc.vt->local_addr(pc.data);
    NetConn c = net_dial(heap, BURROW_S("udp"), la.vt->string(la.data, a), &err);
    if (c.vt != NULL) {
        /* One datagram there, and the answer back to whoever sent it. */
        char buf[64];
        char ping[] = "ping";
        (void)c.vt->writer.write(c.data, slice_from(ping, 4, 4, TYPE_BYTE), &err);
        NetAddr from = {NULL, NULL};
        Int n = pc.vt->read_from(pc.data,
                                 slice_from(buf, sizeof buf, sizeof buf, TYPE_BYTE), a,
                                 &from, &err);
        NetAddr cl = c.vt->local_addr(c.data);
        bool same = from.vt != NULL &&
                    str_eq(from.vt->string(from.data, a), cl.vt->string(cl.data, a));
        printf("got %.*s, from the dialer: %s\n", (int)n, buf, same ? "true" : "false");
        char pong[] = "pong";
        (void)pc.vt->write_to(pc.data, slice_from(pong, 4, 4, TYPE_BYTE), from, &err);
        n = c.vt->reader.read(c.data,
                              slice_from(buf, sizeof buf, sizeof buf, TYPE_BYTE), &err);
        printf("got %.*s back\n", (int)n, buf);
        net_conn_free(c);
    }
    net_packet_conn_free(pc);

    /* Only the datagram networks make a packet connection. */
    pc = net_listen_packet(heap, BURROW_S("tcp"), BURROW_S("127.0.0.1:0"), &err);
    if (pc.vt == NULL) {
        Str s = error_text(err);
        printf("%.*s\n", (int)s.len, (const char *)s.p);
    }
    arena_free(&ar);
    // doc: end
}

int main(void) {
    runtime_main(BURROW_FN(Func, run, NULL));
    return 0;
}

/* Output:
got ping, from the dialer: true
got pong back
listen tcp 127.0.0.1:0: address 127.0.0.1:0: unexpected address type
*/

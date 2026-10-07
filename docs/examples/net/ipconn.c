#include <stdio.h>
#include <string.h>

#include "burrow/burrow.h"

/* The Internet checksum of an ICMP message, with its checksum field zero. */
static void icmp_checksum(Byte *b, size_t n) {
    uint32_t sum = 0;
    for (size_t i = 0; i + 1 < n; i += 2)
        sum += (uint32_t)b[i] << 8 | b[i + 1];
    while (sum >> 16 != 0)
        sum = (sum & 0xffff) + (sum >> 16);
    sum = ~sum & 0xffff;
    b[2] = (Byte)(sum >> 8);
    b[3] = (Byte)sum;
}

static void run(void *env) {
    (void)env;
    // doc: ipconn
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err;
    /* Raw sockets need root, or CAP_NET_RAW on Linux. */
    NetIPConn *c = net_listen_ip(a, BURROW_S("ip4:icmp"), NULL, &err);
    if (c == NULL) {
        Str s = error_text(err);
        printf("%.*s\n", (int)s.len, (const char *)s.p);
        arena_free(&ar);
        return;
    }
    (void)net_ip_conn_set_deadline(c, time_add(time_now(), 5 * TIME_SECOND));

    /* An echo request: type 8, code 0, the checksum, an identifier and a
     * sequence number, then the data. */
    Byte req[12] = {8, 0, 0, 0, 0x62, 0x78, 0, 1, 'p', 'i', 'n', 'g'};
    icmp_checksum(req, sizeof req);
    NetIPAddr *to =
        net_resolve_ip_addr(a, BURROW_S("ip4"), BURROW_S("127.0.0.1"), &err);
    (void)net_ip_conn_write_to_ip(c, slice_from(req, sizeof req, sizeof req, TYPE_BYTE),
                                  to, &err);

    /* The socket sees every ICMP packet to this machine, so wait for the
     * reply, which is type 0. read_from_ip takes the IPv4 header off. */
    for (;;) {
        Byte got[128];
        NetIPAddr *from = NULL;
        Int n = net_ip_conn_read_from_ip(
            c, slice_from(got, sizeof got, sizeof got, TYPE_BYTE), a, &from, &err);
        if (BURROW_FAILED(err)) {
            Str s = error_text(err);
            printf("%.*s\n", (int)s.len, (const char *)s.p);
            break;
        }
        if (n >= 8 && got[0] == 0 && memcmp(got + 4, req + 4, 4) == 0) {
            Str s = net_ip_addr_string(from, a);
            printf("echo reply from %.*s, %d bytes\n", (int)s.len, (const char *)s.p,
                   (int)n);
            break;
        }
    }
    net_ip_conn_free(c);
    arena_free(&ar);
    // doc: end
}

int main(void) {
    runtime_main(BURROW_FN(Func, run, NULL));
    return 0;
}

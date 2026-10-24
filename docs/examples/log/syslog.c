#include <stdio.h>

#include "burrow/burrow.h"

#define P(s) (int)(s).len, (const char *)(s).p

/* Prints a message the way the server got it, less the timestamp, host name,
 * tag and pid, which change from run to run. */
static void show(Str m) {
    Int gt = strings_index_byte(m, '>');
    Int colon = strings_index(m, BURROW_S("]: "));
    if (gt < 0 || colon < 0)
        return;
    Str pri = {m.p, gt + 1};
    Str text = {m.p + colon + 3, m.len - colon - 3};
    printf("%.*s %.*s", P(pri), P(text));
}

int main(void) {
    Alloc *a = heap_allocator();
    Arena ar;
    arena_init(&ar, a, 0);
    Error err = BURROW_NO_ERROR;

    /* A stand-in for the daemon, on a port the system picks. */
    NetPacketConn server =
        net_listen_packet(a, BURROW_S("udp"), BURROW_S("127.0.0.1:0"), &err);
    if (BURROW_FAILED(err))
        log_fatal_v(err);
    NetAddr la = server.vt->local_addr(server.data);
    Str addr = la.vt->string(la.data, arena_allocator(&ar));

    {
        // doc: dial
        SyslogWriter *w = syslog_dial(a, BURROW_S("udp"), addr,
                                      SYSLOG_LOG_WARNING | SYSLOG_LOG_DAEMON,
                                      BURROW_S("demotag"), &err);
        if (BURROW_FAILED(err))
            log_fatal_v(err);
        IoWriter out = syslog_writer_as_io_writer(w);
        fmt_fprintf_v(out, "This is a daemon warning with demotag.");
        err = syslog_writer_emerg(
            w, BURROW_S("And this is a daemon emergency with demotag."));
        if (BURROW_FAILED(err))
            log_fatal_v(err);
        syslog_writer_free(w);
        // doc: end
    }

    Byte buf[1024];
    for (int i = 0; i < 2; i++) {
        NetAddr from = {NULL, NULL};
        Int n =
            server.vt->read_from(server.data, slice_from(buf, 0, sizeof buf, TYPE_BYTE),
                                 arena_allocator(&ar), &from, &err);
        if (BURROW_FAILED(err))
            log_fatal_v(err);
        Str m = {buf, n};
        show(m);
    }
    net_packet_conn_free(server);
    arena_free(&ar);
    return 0;
}

/* Output:
<28> This is a daemon warning with demotag.
<24> And this is a daemon emergency with demotag.
*/

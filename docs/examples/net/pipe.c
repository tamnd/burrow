#include <stdio.h>

#include "burrow/burrow.h"

/* The other end: send back whatever comes in until the client hangs up. */
static void echo(void *env) {
    NetConn c = *(NetConn *)env;
    io_copy(heap_allocator(), net_conn_as_io_writer(c), net_conn_as_io_reader(c), NULL);
    c.vt->closer.close(c.data);
}

static void run(void *env) {
    (void)env;
    // doc: pipe
    NetConn client, server;
    net_pipe(heap_allocator(), &client, &server);
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, echo, &server));

    Error err;
    Byte buf[5];
    Slice b = slice_from(buf, 5, 5, TYPE_BYTE);
    io_write_string(net_conn_as_io_writer(client), BURROW_S("hello"), &err);
    io_read_full(net_conn_as_io_reader(client), b, &err);
    printf("%.*s\n", 5, (const char *)buf);

    /* Nothing more is coming, so this read gives up at the deadline. */
    client.vt->set_read_deadline(client.data,
                                 time_add(time_now(), 10 * TIME_MILLISECOND));
    io_read_full(net_conn_as_io_reader(client), b, &err);
    Str msg = error_text(err);
    printf("%.*s, timeout %d\n", (int)msg.len, (const char *)msg.p,
           net_error_timeout(err));

    client.vt->closer.close(client.data);
    sync_wait_group_wait(&wg);
    net_pipe_free(client);
    // doc: end
}

int main(void) {
    runtime_main(BURROW_FN(Func, run, NULL));
    return 0;
}

/* Output:
hello
read pipe: i/o timeout, timeout 1
*/

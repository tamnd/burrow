#include <stdint.h>
#include <stdio.h>

#include "burrow/burrow.h"

static void run(void *env) {
    (void)env;
    // doc: unix
    Error err;
    Str dir = os_mkdir_temp(heap_allocator(), BURROW_STR_EMPTY, BURROW_S("ex"), &err);
    if (BURROW_FAILED(err))
        return;
    Str path = filepath_join_v(heap_allocator(), 2, dir, BURROW_S("echo.sock"));
    NetUnixAddr addr = {path, BURROW_S("unix")};
    NetUnixListener *l =
        net_listen_unix(heap_allocator(), BURROW_S("unix"), &addr, &err);
    if (l == NULL)
        return;

    NetUnixConn *client =
        net_dial_unix(heap_allocator(), BURROW_S("unix"), NULL, &addr, &err);
    NetUnixConn *server = net_unix_listener_accept_unix(l, &err);
    if (client == NULL || server == NULL)
        return;

    char hello[] = "hello";
    (void)net_unix_conn_write(client, slice_from(hello, 5, 5, TYPE_BYTE), &err);
    Byte buf[64];
    Int n = net_unix_conn_read(server, slice_from(buf, 0, (Int)sizeof buf, TYPE_BYTE),
                               &err);
    printf("%.*s\n", (int)n, (const char *)buf);

    /* The server's end is named for the path the listener is on. */
    const NetUnixAddr *local = net_unix_conn_local_addr(server).data;
    printf("same path: %d\n", str_eq(local->name, path));

    net_unix_conn_free(client);
    net_unix_conn_free(server);

    /* Closing a listener removes the file it made, as Go's does. */
    net_unix_listener_free(l);
    (void)os_lstat(heap_allocator(), path, &err);
    printf("gone: %d\n", os_is_not_exist(err));
    (void)os_remove_all(dir);
    mem_free(heap_allocator(), (void *)(uintptr_t)path.p, (size_t)path.len, 1);
    mem_free(heap_allocator(), (void *)(uintptr_t)dir.p, (size_t)dir.len, 1);

    /* Errors read the way Go's do. */
    NetUnixAddr there = {BURROW_S("/run/app.sock"), BURROW_S("unix")};
    if (net_listen_unix(heap_allocator(), BURROW_S("unixgram"), &there, &err) == NULL) {
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
hello
same path: 1
gone: 1
listen unixgram /run/app.sock: unknown network unixgram
*/

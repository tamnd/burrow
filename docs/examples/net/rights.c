#include <stdint.h>
#include <stdio.h>

#include "burrow/burrow.h"

static void run(void *env) {
    (void)env;
    // doc: rights
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err;
    Str dir = os_mkdir_temp(a, BURROW_STR_EMPTY, BURROW_S("ex"), &err);
    if (BURROW_FAILED(err))
        return;
    Str note = filepath_join_v(a, 2, dir, BURROW_S("note.txt"));
    char text[] = "read through a passed descriptor";
    (void)os_write_file(note, slice_from(text, 32, 32, TYPE_BYTE), 0600);

    NetUnixAddr addr = {filepath_join_v(a, 2, dir, BURROW_S("pass.sock")),
                        BURROW_S("unix")};
    NetUnixListener *l = net_listen_unix(a, BURROW_S("unix"), &addr, &err);
    NetUnixConn *client = net_dial_unix(a, BURROW_S("unix"), NULL, &addr, &err);
    NetUnixConn *server = net_unix_listener_accept_unix(l, &err);
    if (client == NULL || server == NULL)
        return;

    /* One end opens the file and sends the descriptor along with a byte. */
    Int fd = syscall_open(note, SYSCALL_O_RDONLY, 0, &err);
    Slice rights = syscall_unix_rights(a, (Slice){&fd, 1, 1, TYPE_INT});
    char one[] = "f";
    Int oobn = 0;
    (void)net_unix_conn_write_msg_unix(client, slice_from(one, 1, 1, TYPE_BYTE), rights,
                                       NULL, &oobn, &err);
    printf("sent all of it: %d\n", oobn == rights.len);
    (void)syscall_close(fd);

    /* The other end gets a descriptor of its own for the same open file. */
    Byte b[1];
    Byte oob[64];
    (void)net_unix_conn_read_msg_unix(server, slice_from(b, 1, 1, TYPE_BYTE),
                                      slice_from(oob, 64, 64, TYPE_BYTE), a, &oobn,
                                      NULL, NULL, &err);
    Slice msgs = syscall_parse_socket_control_message(
        a, slice_from(oob, oobn, oobn, TYPE_BYTE), &err);
    Slice fds =
        syscall_parse_unix_rights(a, &((SyscallSocketControlMessage *)msgs.p)[0], &err);
    if (BURROW_FAILED(err) || fds.len != 1)
        return;
    Int got = ((const Int *)fds.p)[0];
    Byte data[64];
    Int n = syscall_read(got, slice_from(data, 64, 64, TYPE_BYTE), &err);
    printf("%.*s\n", (int)n, (const char *)data);
    (void)syscall_close(got);

    net_unix_conn_free(client);
    net_unix_conn_free(server);
    net_unix_listener_free(l);
    (void)os_remove_all(dir);
    arena_free(&ar);
    // doc: end
}

int main(void) {
    runtime_main(BURROW_FN(Func, run, NULL));
    return 0;
}

/* Output:
sent all of it: 1
read through a passed descriptor
*/

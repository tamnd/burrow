#include <stdio.h>

#include "burrow/burrow.h"

#define P(s) (int)(s).len, (const char *)(s).p

static void reply(NetConn c, const char *lines) {
    io_write_string(net_conn_as_io_writer(c), str_from_cstr(lines), NULL);
}

/* A pretend mail server on the other end of the pipe, which prints what the
 * client says to it and answers yes to everything. */
static void server(void *env) {
    NetConn c = *(NetConn *)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TextprotoReader *r =
        textproto_new_reader(a, bufio_new_reader(a, net_conn_as_io_reader(c)));
    reply(c, "220 mail.example.com ready\r\n");
    bool data = false;
    for (;;) {
        Error err;
        Str line = textproto_reader_read_line(r, a, &err);
        if (BURROW_FAILED(err))
            break;
        printf("C: %.*s\n", P(line));
        if (data) {
            if (str_eq(line, BURROW_S("."))) {
                data = false;
                reply(c, "250 queued\r\n");
            }
        } else if (strings_has_prefix(line, BURROW_S("EHLO"))) {
            reply(c, "250-mail.example.com\r\n250 8BITMIME\r\n");
        } else if (strings_has_prefix(line, BURROW_S("DATA"))) {
            data = true;
            reply(c, "354 go ahead\r\n");
        } else if (strings_has_prefix(line, BURROW_S("QUIT"))) {
            reply(c, "221 bye\r\n");
            break;
        } else {
            reply(c, "250 OK\r\n");
        }
    }
    c.vt->closer.close(c.data);
    arena_free(&ar);
}

static void run(void *env) {
    (void)env;
    NetConn conn, other;
    net_pipe(heap_allocator(), &conn, &other);
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, server, &other));

    // doc: client
    Error err;
    SmtpClient *c =
        smtp_new_client(heap_allocator(), conn, BURROW_S("mail.example.com"), &err);
    if (c == NULL) {
        fmt_printf_v("%v\n", err);
        return;
    }

    /* Set the sender and recipient first. */
    err = smtp_client_mail(c, BURROW_S("sender@example.org"));
    if (BURROW_OK(err))
        err = smtp_client_rcpt(c, BURROW_S("recipient@example.net"));

    /* Send the email body. */
    IoWriteCloser wc = {0};
    if (BURROW_OK(err))
        wc = smtp_client_data(c, &err);
    if (wc.vt != NULL) {
        fmt_fprintf_v(io_write_closer_as_io_writer(wc), "This is the email body");
        err = wc.vt->closer.close(wc.data);
    }

    /* Send the QUIT command and close the connection. */
    if (BURROW_OK(err))
        err = smtp_client_quit(c);
    sync_wait_group_wait(&wg);
    fmt_printf_v("sent: %v\n", err);
    smtp_client_free(c);
    // doc: end
    net_pipe_free(conn);
}

int main(void) {
    runtime_main(BURROW_FN(Func, run, NULL));
    return 0;
}

/* Output:
PENDING
*/

# Write a TCP server and client

`net_listen` on port 0 gets any free port, and the listener's address says which. The server accepts in a loop and gives each connection its own goroutine. The client dials that address, writes a line and reads the reply. Hanging up ends the handler, and closing the listener makes the waiting accept fail, which ends the loop.

<!-- example: ../examples/cookbook/tcp_echo.c#main -->
```c
#define BURROW_SHORT 1
#include "burrow/burrow.h"

static SyncWaitGroup wg;

/* One goroutine per connection: read a line, send it back in capitals, and
 * stop when the other side hangs up. */
static void handle(void *env) {
    NetConn *c = env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BufioScanner *sc = bufio_new_scanner(a, net_conn_as_io_reader(*c));
    IoWriter w = net_conn_as_io_writer(*c);
    while (bufio_scanner_scan(sc)) {
        Str line = strings_to_upper(a, bufio_scanner_text(sc));
        fmt_fprintf_v(w, "%s\n", line);
    }
    net_conn_free(*c);
    mem_free(heap_allocator(), c, sizeof *c, _Alignof(NetConn));
    arena_free(&ar);
}

/* Accept until the listener is closed, which makes accept fail. */
static void serve(void *env) {
    NetListener *l = env;
    for (;;) {
        Error err = NO_ERROR;
        NetConn c = l->vt->accept(l->data, &err);
        if (FAILED(err))
            return;
        NetConn *p = mem_alloc(heap_allocator(), sizeof *p, _Alignof(NetConn));
        *p = c;
        sync_wait_group_go(&wg, FN(Func, handle, p));
    }
}

static void run(void *env) {
    (void)env;
    Alloc *heap = heap_allocator();
    Error err = NO_ERROR;
    /* Port 0 asks for any free port, and the listener says which it got. */
    NetListener l = net_listen(heap, S("tcp"), S("127.0.0.1:0"), &err);
    if (FAILED(err)) {
        fmt_println_v(err);
        return;
    }
    sync_wait_group_go(&wg, FN(Func, serve, &l));

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    NetAddr la = l.vt->addr(l.data);
    Str address = la.vt->string(la.data, a);
    NetConn c = net_dial(heap, S("tcp"), address, &err);
    if (FAILED(err)) {
        fmt_println_v(err);
        return;
    }
    IoWriter w = net_conn_as_io_writer(c);
    BufioScanner *sc = bufio_new_scanner(a, net_conn_as_io_reader(c));
    const char *lines[] = {"hello", "is anyone there?", "bye"};
    for (int i = 0; i < 3; i++) {
        Str line = str_from_cstr(lines[i]);
        fmt_fprintf_v(w, "%s\n", line);
        if (!bufio_scanner_scan(sc))
            break;
        Str reply = bufio_scanner_text(sc);
        fmt_printf_v("sent %q, got %q\n", line, reply);
    }

    /* Hanging up ends the handler, and closing the listener ends serve. */
    net_conn_free(c);
    (void)l.vt->closer.close(l.data);
    sync_wait_group_wait(&wg);
    net_listener_free(l);
    arena_free(&ar);
}

int main(void) {
    runtime_main(FN(Func, run, NULL));
}
```

It prints:

```text
sent "hello", got "HELLO"
sent "is anyone there?", got "IS ANYONE THERE?"
sent "bye", got "BYE"
```

The program is [docs/examples/cookbook/tcp_echo.c](../examples/cookbook/tcp_echo.c). CI builds and runs it and checks that it prints this.

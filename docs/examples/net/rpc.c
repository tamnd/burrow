#include "burrow/burrow.h"

// doc: service
#define ARGS_FIELDS(F, T) F(T, Int, A, "") F(T, Int, B, "")
BURROW_STRUCT(Args, ARGS_FIELDS);
BURROW_PTR_TYPE(IntPtr, Int);

#define ARITH_FIELDS(F, T) F(T, Int, calls, "")
BURROW_STRUCT_DECL(Arith, ARITH_FIELDS);

static Error arith_divide(Arith *t, Args args, IntPtr reply) {
    (void)t;
    if (args.B == 0)
        return errors_new(error_allocator(), BURROW_S("divide by zero"));
    *reply = args.A / args.B;
    return BURROW_NO_ERROR;
}

static Error arith_multiply(Arith *t, Args args, IntPtr reply) {
    (void)t;
    *reply = args.A * args.B;
    return BURROW_NO_ERROR;
}

#define ARITH_SIG(IN, OUT) IN(0, Args) IN(1, IntPtr) OUT(Error)
#define ARITH_METHODS(M, T)                                                            \
    M(T, Divide, arith_divide, ARITH_SIG)                                              \
    M(T, Multiply, arith_multiply, ARITH_SIG)
BURROW_STRUCT_DEFINE_METHODS(Arith, ARITH_FIELDS, ARITH_METHODS);
// doc: end

static Arith arith;

static void serve(void *env) {
    rpc_serve_conn(net_conn_as_io_read_write_closer((NetConn *)env));
}

static void serve_json(void *env) {
    jsonrpc_serve_conn(net_conn_as_io_read_write_closer((NetConn *)env));
}

static void run(void *env) {
    (void)env;
    Alloc *heap = heap_allocator();
    // doc: call
    rpc_register(BURROW_ANY(TYPE_OF(Arith), &arith));

    /* A pipe stands in for the network: the server on one end, the client on
     * the other. */
    NetConn client, server;
    net_pipe(heap, &client, &server);
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, serve, &server));

    RpcClient *c = rpc_new_client(heap, net_conn_as_io_read_write_closer(&client));
    Args args = {7, 8};
    Int product = 0;
    Error err = rpc_client_call(c, heap, BURROW_S("Arith.Multiply"),
                                BURROW_ANY(TYPE_OF(Args), &args),
                                BURROW_ANY(TYPE_INT, &product));
    fmt_printf_v("%d*%d=%d\n", args.A, args.B, product);

    args.B = 0;
    Int quotient = 0;
    err = rpc_client_call(c, heap, BURROW_S("Arith.Divide"),
                          BURROW_ANY(TYPE_OF(Args), &args),
                          BURROW_ANY(TYPE_INT, &quotient));
    fmt_printf_v("Divide: %v\n", err);
    err = rpc_client_call(c, heap, BURROW_S("Arith.Add"),
                          BURROW_ANY(TYPE_OF(Args), &args),
                          BURROW_ANY(TYPE_INT, &quotient));
    fmt_printf_v("Add: %v\n", err);

    rpc_client_free(c);
    sync_wait_group_wait(&wg);
    net_pipe_free(client);
    // doc: end

    // doc: json
    /* The same service over JSON-RPC, with the request written by hand. */
    net_pipe(heap, &client, &server);
    sync_wait_group_go(&wg, BURROW_FN(Func, serve_json, &server));
    io_write_string(net_conn_as_io_writer(client),
                    BURROW_S("{\"method\": \"Arith.Multiply\", "
                             "\"params\": [{\"A\": 6, \"B\": 7}], \"id\": 1}\n"),
                    &err);
    BufioReader *r = bufio_new_reader(heap, net_conn_as_io_reader(client));
    Arena ar;
    arena_init(&ar, NULL, 0);
    Str line = bufio_reader_read_string(r, arena_allocator(&ar), '\n', &err);
    fmt_printf_v("%s", line);
    arena_free(&ar);
    bufio_reader_free(r);

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
7*8=56
Divide: divide by zero
Add: rpc: can't find method Arith.Add
{"id":1,"result":42,"error":null}
*/

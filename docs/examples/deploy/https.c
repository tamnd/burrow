#include "burrow/burrow.h"

static void hello(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    fmt_fprintf_v(http_response_writer_as_io_writer(w), "hello over %s and %s\n",
                  r->proto, tls_version_name(heap_allocator(), r->tls->version));
}

typedef struct Serving {
    HttpServer *srv;
    NetListener l;
    Error err;
} Serving;

static void serve(void *env) {
    Serving *s = env;
    s->err = http_server_serve_tls(s->srv, s->l, BURROW_STR_EMPTY, BURROW_STR_EMPTY);
}

static void run(void *env) {
    Alloc *a = env;
    // A key and a certificate for 127.0.0.1, made when the program starts.
    Error err = BURROW_NO_ERROR;
    Ed25519PrivateKey priv;
    Ed25519PublicKey pub = ed25519_generate_key(a, (IoReader){0}, &priv, &err);
    if (BURROW_FAILED(err)) {
        fmt_printf_v("key: %v\n", err);
        return;
    }
    Ed25519Signer signer;
    X509Certificate tmpl = {0};
    tmpl.serial_number = big_new_int(a, 1);
    tmpl.subject.common_name = BURROW_S("localhost");
    NetIP loopback_ip = net_ipv4(a, 127, 0, 0, 1);
    tmpl.ip_addresses = slice_append(a, slice_nil(TYPE_NET_IP), &loopback_ip, 1);
    tmpl.not_before = time_add(time_now(), -TIME_HOUR);
    tmpl.not_after = time_add(time_now(), 24 * TIME_HOUR);
    Slice der = x509_create_certificate(
        a, (IoReader){0}, &tmpl, &tmpl, BURROW_ANY(TYPE_ED25519_PUBLIC_KEY, &pub),
        ed25519_private_key_signer(priv, &signer), &err);
    if (BURROW_FAILED(err)) {
        fmt_printf_v("certificate: %v\n", err);
        return;
    }
    TlsCertificate cert = {0};
    cert.certificate = slice_append(a, slice_nil(TYPE_BYTES), &der, 1);
    cert.private_key = BURROW_ANY(TYPE_ED25519_PRIVATE_KEY, &priv);

    // doc: https
    // The server, on a port the system picks.
    Byte loopback[4] = {127, 0, 0, 1};
    NetTCPAddr laddr = {slice_from(loopback, 4, 4, TYPE_BYTE), 0, BURROW_STR_EMPTY};
    NetTCPListener *l = net_listen_tcp(heap_allocator(), BURROW_S("tcp"), &laddr, &err);
    if (l == NULL) {
        fmt_printf_v("listen: %v\n", err);
        return;
    }
    Int port = ((const NetTCPAddr *)net_tcp_listener_addr(l).data)->port;
    TlsConfig server_tls = {0};
    server_tls.certificates = slice_from(&cert, 1, 1, TYPE_TLS_CERTIFICATE);
    HttpHandlerFunc handler = BURROW_FN(HttpHandlerFunc, hello, NULL);
    HttpServer srv = {.handler = http_handler_func_as_handler(&handler),
                      .tls_config = &server_tls,
                      .read_header_timeout = 5 * TIME_SECOND};
    Serving s = {&srv, net_tcp_listener_as_listener(l), BURROW_NO_ERROR};
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, serve, &s));
    // doc: end

    // A client that trusts that certificate and nothing else.
    X509CertPool *roots = x509_new_cert_pool(heap_allocator());
    x509_cert_pool_add_cert(roots, x509_parse_certificate(a, der, &err));
    TlsConfig client_tls = {0};
    client_tls.root_cas = roots;
    HttpTransport tr = {0};
    tr.tls_client_config = &client_tls;
    HttpClient client = {0};
    client.transport = http_transport_as_round_tripper(&tr);

    Str url = fmt_sprintf_v(a, "https://127.0.0.1:%d/", port);
    HttpResponse *res = http_client_get(&client, url, &err);
    if (res == NULL) {
        fmt_printf_v("get: %v\n", err);
    } else {
        Slice body = io_read_all(a, io_read_closer_as_io_reader(res->body), &err);
        fmt_printf_v("%s\n%s", res->status, str_from_bytes(body.p, body.len));
        http_response_free(res);
    }

    http_transport_close_idle_connections(&tr);
    (void)http_server_close(&srv);
    sync_wait_group_wait(&wg);
    http_transport_free(&tr);
    http_server_free(&srv);
    net_tcp_listener_free(l);
    tls_config_release(&client_tls);
    tls_config_release(&server_tls);
    x509_cert_pool_free(roots);
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    runtime_main(BURROW_FN(Func, run, arena_allocator(&ar)));
    arena_free(&ar);
    return 0;
}

/* Output:
200 OK
hello over HTTP/1.1 and TLS 1.3
*/

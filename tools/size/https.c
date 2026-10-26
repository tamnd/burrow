/* The same server over TLS, which brings in crypto/tls, crypto/x509 and
 * HTTP/2, as https.go does it. */
#define BURROW_SHORT 1
#include "burrow/burrow.h"

static void hello(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)r;
    fmt_fprintf_v(http_response_writer_as_io_writer(w), "hello\n");
}

static void run(void *env) {
    (void)env;
    http_handle_func(S("GET /"), FN(HttpHandlerFunc, hello, NULL));
    Error err = http_listen_and_serve_tls(S(":8443"), S("cert.pem"), S("key.pem"),
                                          (HttpHandler){0});
    fmt_println_v(err);
}

int main(void) {
    runtime_main(FN(Func, run, NULL));
}

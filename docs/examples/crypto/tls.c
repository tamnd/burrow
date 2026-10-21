#include <stdio.h>

#include "burrow/burrow.h"

static void print(Str s) {
    printf("%.*s\n", (int)s.len, s.p);
}

static void suites(Alloc *a) {
    // doc: suites
    Slice list = tls_cipher_suites(a);
    TlsCipherSuitePtr *p = list.p;
    printf("%d secure suites, first %s\n", (int)list.len,
           p[0]->supported_versions.len == 1 ? "TLS 1.3 only" : "older too");
    for (Int i = 0; i < 3; i++)
        print(p[i]->name);
    print(tls_cipher_suite_name(a, TLS_TLS_ECDHE_RSA_WITH_RC4_128_SHA));
    print(tls_cipher_suite_name(a, 0xabc));
    // doc: end
}

static void names(Alloc *a) {
    // doc: names
    print(tls_version_name(a, TLS_VERSION_TLS13));
    print(tls_curve_id_string(TLS_X25519MLKEM768, a));
    print(tls_signature_scheme_string(TLS_PSS_WITH_SHA256, a));

    Error err = tls_alert_error_as_error(20, a);
    print(error_text(err));
    print(tls_alert_error_error(7, a));
    // doc: end
}

/* The key pair from Go's ExampleX509KeyPair. The certificate signs itself
 * and was valid from October 2017 to October 2018. */
#define CERT_PEM                                                                       \
    "-----BEGIN CERTIFICATE-----\n"                                                    \
    "MIIBhTCCASugAwIBAgIQIRi6zePL6mKjOipn+dNuaTAKBggqhkjOPQQDAjASMRAw\n"               \
    "DgYDVQQKEwdBY21lIENvMB4XDTE3MTAyMDE5NDMwNloXDTE4MTAyMDE5NDMwNlow\n"               \
    "EjEQMA4GA1UEChMHQWNtZSBDbzBZMBMGByqGSM49AgEGCCqGSM49AwEHA0IABD0d\n"               \
    "7VNhbWvZLWPuj/RtHFjvtJBEwOkhbN/BnnE8rnZR8+sbwnc/KhCk3FhnpHZnQz7B\n"               \
    "5aETbbIgmuvewdjvSBSjYzBhMA4GA1UdDwEB/wQEAwICpDATBgNVHSUEDDAKBggr\n"               \
    "BgEFBQcDATAPBgNVHRMBAf8EBTADAQH/MCkGA1UdEQQiMCCCDmxvY2FsaG9zdDo1\n"               \
    "NDUzgg4xMjcuMC4wLjE6NTQ1MzAKBggqhkjOPQQDAgNIADBFAiEA2zpJEPQyz6/l\n"               \
    "Wf86aX6PepsntZv2GYlA5UpabfT2EZICICpJ5h/iI+i341gBmLiAFQOyTDT+/wQc\n"               \
    "6MF9+Yw1Yy0t\n"                                                                   \
    "-----END CERTIFICATE-----\n"
#define KEY_PEM                                                                        \
    "-----BEGIN EC PRIVATE KEY-----\n"                                                 \
    "MHcCAQEEIIrYSSNQFaA2Hwf1duRSxKtLYX5CB04fSeQ6tF1aY/PuoAoGCCqGSM49\n"               \
    "AwEHoUQDQgAEPR3tU2Fta9ktY+6P9G0cWO+0kETA6SFs38GecTyudlHz6xvCdz8q\n"               \
    "EKTcWGekdmdDPsHloRNtsiCa697B2O9IFA==\n"                                           \
    "-----END EC PRIVATE KEY-----\n"

/* The server side: the handshake runs on the first read, then whatever comes
 * in goes back out until the client sends close_notify. */
static void echo(void *env) {
    NetConn c = tls_conn_as_net_conn((TlsConn *)env);
    io_copy(heap_allocator(), net_conn_as_io_writer(c), net_conn_as_io_reader(c), NULL);
}

typedef struct Verifier {
    X509CertPool *roots;
    Alloc *a;
} Verifier;

/* Checks the server's certificate against roots, at a time when it was still
 * valid, in place of the check that insecure_skip_verify turns off. */
static Error verify(void *env, const TlsConnectionState *cs) {
    Verifier *v = env;
    X509Certificate **certs = cs->peer_certificates.p;
    X509VerifyOptions opts = {0};
    opts.roots = v->roots;
    X509CertPool *intermediates = x509_new_cert_pool(v->a);
    for (Int i = 1; i < cs->peer_certificates.len; i++)
        x509_cert_pool_add_cert(intermediates, certs[i]);
    opts.intermediates = intermediates;
    opts.current_time = time_date(2018, TIME_JUNE, 1, 0, 0, 0, 0, time_utc_loc);
    Error err;
    x509_certificate_verify(certs[0], v->a, opts, &err);
    return err;
}

static void handshake(void *env) {
    Alloc *a = env;
    // doc: handshake
    Error err;
    TlsCertificate cert = tls_x509_key_pair(a, slice_from_str(a, BURROW_S(CERT_PEM)),
                                            slice_from_str(a, BURROW_S(KEY_PEM)), &err);
    if (BURROW_FAILED(err))
        return;
    TlsConfig server_config = {0};
    server_config.certificates = slice_from(&cert, 1, 1, TYPE_TLS_CERTIFICATE);

    Verifier v = {x509_new_cert_pool(a), a};
    x509_cert_pool_add_cert(v.roots, cert.leaf);
    TlsConfig client_config = {0};
    client_config.insecure_skip_verify = true;
    client_config.verify_connection = BURROW_FN(TlsVerifyConnectionFunc, verify, &v);

    NetConn c, s;
    net_pipe(heap_allocator(), &c, &s);
    TlsConn *server = tls_server(heap_allocator(), s, &server_config);
    TlsConn *client = tls_client(heap_allocator(), c, &client_config);
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, echo, server));

    NetConn conn = tls_conn_as_net_conn(client);
    Byte buf[5];
    Slice b = slice_from(buf, 5, 5, TYPE_BYTE);
    io_write_string(net_conn_as_io_writer(conn), BURROW_S("hello"), &err);
    io_read_full(net_conn_as_io_reader(conn), b, &err);
    if (BURROW_FAILED(err))
        print(error_text(err));
    else
        printf("%.*s\n", 5, (const char *)buf);

    TlsConnectionState state = tls_conn_connection_state(client);
    X509Certificate **peer = state.peer_certificates.p;
    print(tls_version_name(a, state.version));
    print(tls_curve_id_string(state.curve_id, a));
    print(((Str *)peer[0]->subject.organization.p)[0]);

    tls_conn_close(client);
    sync_wait_group_wait(&wg);
    tls_conn_close(server);
    tls_conn_free(client);
    tls_conn_free(server);
    net_pipe_free(c);
    tls_config_release(&client_config);
    tls_config_release(&server_config);
    // doc: end
}

static void run(void *env) {
    (void)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    suites(arena_allocator(&ar));
    names(arena_allocator(&ar));
    handshake(arena_allocator(&ar));
    arena_free(&ar);
}

int main(void) {
    runtime_main(BURROW_FN(Func, run, NULL));
    return 0;
}

/* Output:
13 secure suites, first TLS 1.3 only
TLS_AES_128_GCM_SHA256
TLS_AES_256_GCM_SHA384
TLS_CHACHA20_POLY1305_SHA256
TLS_ECDHE_RSA_WITH_RC4_128_SHA
0x0ABC
TLS 1.3
X25519MLKEM768
PSSWithSHA256
tls: bad record MAC
tls: alert(7)
hello
TLS 1.3
X25519MLKEM768
Acme Co
*/

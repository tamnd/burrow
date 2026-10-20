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

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    suites(arena_allocator(&ar));
    names(arena_allocator(&ar));
    arena_free(&ar);
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
*/

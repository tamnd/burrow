/* Derived from Go's src/crypto/tls/bogo_shim_test.go,
 * bogo_shim_unix_test.go, bogo_shim_notunix_test.go, and the TestMain and the
 * BoGo flags of handshake_test.go.
 * Go source: go1.27.1.
 *
 * The config the runner gets is Go's bogo_config.json, copied into
 * tests/tlstest.
 *
 * Not in Go: Go's shim is the crypto/tls test binary, and here it is this
 * program, which has the shim and TestBogoSuite and nothing else. The
 * testing package's flags are not on flag_command_line here, so main takes
 * the BoGo flags out of the command line before the testing package sees it,
 * and TestMain parses them. With -bogo-mode, which is how the runner starts
 * the shim, every argument is the shim's. TestBogoSuite needs the go command
 * on $PATH, as TestACVP does.
 *
 * Copyright 2024 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"
#include "fetchmodule.h"

#include "../src/crypto/cryptobyte.h"
#include "../src/crypto/tls_internal.h"
#include "burrow/burrow.h"
#include "burrow/crypto/tls.h"
#include "burrow/crypto/x509.h"
#include "burrow/encoding/base64.h"
#include "burrow/encoding/binary.h"
#include "burrow/encoding/pem.h"
#include "burrow/flag.h"
#include "burrow/html/template.h"
#include "burrow/log.h"
#include "burrow/net.h"
#include "burrow/slices.h"
#include "burrow/strconv.h"
#include "burrow/strings.h"
#include "burrow/syscall.h"
#include "tls_handshake_test.h"

#include <string.h>

/* boringsslModVer is the version of BoringSSL that we test against.
 * The pseudo-version can be found by executing:
 *
 *     go mod download -json boringssl.googlesource.com/boringssl.git@latest */
static const Str boringssl_mod_ver =
    BURROW_S_INIT("v0.0.0-20260209204302-2a7ca5404e13");

/* Not in Go: the tests run from the top of the tree, so the path starts with
 * the directory the file is in. */
static const Str bogo_config_file = BURROW_S_INIT("tests/tlstest/bogo_config.json");

/* The flags of handshake_test.go. */
static bool bogo_mode;
static Str bogo_filter;
static Str bogo_local_dir;
static Str bogo_report;

/* stringSlice */
static Str string_slice_string(void *self, Alloc *a) {
    return strings_join(a, *(Slice *)self, BURROW_S(","));
}

static Error string_slice_set(void *self, Str s) {
    Slice *saf = self;
    *saf = BURROW_APPEND(Str, heap_allocator(), *saf, s);
    return BURROW_NO_ERROR;
}

static const FlagValueVT string_slice_vt = {NULL, string_slice_string, string_slice_set,
                                            NULL, NULL};

static void flag_string_slice(Slice *f, const char *name, const char *usage) {
    *f = slice_nil(TYPE_STRING);
    flag_var((FlagValue){&string_slice_vt, f}, str_from_cstr(name),
             str_from_cstr(usage));
}

/* intSlice */
static Str int_slice_string(void *self, Alloc *a) {
    Str s = fmt_sprint_v(a, *(Slice *)self);
    return strings_join(a, strings_split(a, s, BURROW_S(" ")), BURROW_S(","));
}

static Error int_slice_set(void *self, Str s) {
    Slice *sf = self;
    Error err = BURROW_NO_ERROR;
    int64_t i = strconv_parse_int(s, 10, 64, &err);
    if (BURROW_FAILED(err))
        return err;
    *sf = BURROW_APPEND(int64_t, heap_allocator(), *sf, i);
    return BURROW_NO_ERROR;
}

static const FlagValueVT int_slice_vt = {NULL, int_slice_string, int_slice_set, NULL,
                                         NULL};

static void flag_int_slice(Slice *f, const char *name, const char *usage) {
    *f = slice_nil(TYPE_INT64);
    flag_var((FlagValue){&int_slice_vt, f}, str_from_cstr(name), str_from_cstr(usage));
}

/* base64Flag. Not in Go: set says whether the flag was given, which is what
 * Go's nil check asks, since an empty value decodes to an empty slice that is
 * not nil. */
typedef struct Base64Flag {
    Slice b;
    bool set;
} Base64Flag;

static Str base64_flag_string(void *self, Alloc *a) {
    const Base64Flag *f = self;
    return base64_encoding_encode_to_string(base64_std_encoding, a, f->b);
}

static Error base64_flag_set(void *self, Str s) {
    Base64Flag *f = self;
    if (f->set)
        return fmt_errorf_v("multiple base64 values not supported");
    Error err = BURROW_NO_ERROR;
    Slice b =
        base64_encoding_decode_string(base64_std_encoding, heap_allocator(), s, &err);
    if (BURROW_FAILED(err))
        return err;
    f->b = b;
    f->set = true;
    return BURROW_NO_ERROR;
}

static const FlagValueVT base64_flag_vt = {NULL, base64_flag_string, base64_flag_set,
                                           NULL, NULL};

static void flag_base64(Base64Flag *f, const char *name, const char *usage) {
    *f = (Base64Flag){slice_nil(TYPE_BYTE), false};
    flag_var((FlagValue){&base64_flag_vt, f}, str_from_cstr(name),
             str_from_cstr(usage));
}

static Str port;
static bool server;

static bool is_handshaker_supported;

static Str keyfile;
static Str certfile;
static Base64Flag ocsp_response;
static Slice signing_prefs;

static Str trust_cert;

static Int min_version;
static Int max_version;
static Int expect_version;

static bool no_tls1;
static bool no_tls11;
static bool no_tls12;
static bool no_tls13;

static bool require_any_client_certificate;

static bool shim_writes_first;

static Int resume_count;

static Slice curves;
static Str expected_curve;

static Slice verify_prefs;
static Str expected_sig_alg;
static Slice expected_peer_sig_alg;

static uint64_t shim_id;

static Base64Flag ech_config_list;
static bool expect_ech_accepted;
static bool expect_hrr;
static bool expect_no_hrr;
static Str expected_ech_retry_configs;
static bool expect_no_ech_retry_configs;
static bool on_initial_expect_ech_accepted;
static Base64Flag on_resume_ech_config_list;
static bool on_resume_expect_ech_accepted;
static Str expected_server_name;
static Slice ech_server_config;
static Slice ech_server_key;
static Slice ech_server_retry_config;

static bool expect_session_miss;

static Str advertise_alpn;
static Str expect_alpn;
static bool reject_alpn;
static bool decline_alpn;
static Str expect_advertised_alpn;
static Str select_alpn;

static Str host_name;

static bool verify_peer;

static bool wait_for_debugger;

/* Not in Go: the flags Go names _, which are defined and never read. */
static bool unused_bool;
static Str unused_string;

/* Not in Go: Go defines the flags in its var blocks, and here this function
 * does, from TestMain. */
static void define_flags(void) {
    flag_bool_var(&bogo_mode, BURROW_S("bogo-mode"), false,
                  BURROW_S("Enabled bogo shim mode, ignore everything else"));
    flag_string_var(&bogo_filter, BURROW_S("bogo-filter"), BURROW_S(""),
                    BURROW_S("BoGo test filter"));
    flag_string_var(&bogo_local_dir, BURROW_S("bogo-local-dir"), BURROW_S(""),
                    BURROW_S("If not-present, checkout BoGo into this dir, or "
                             "otherwise use it as a pre-existing checkout"));
    flag_string_var(&bogo_report, BURROW_S("bogo-html-report"), BURROW_S(""),
                    BURROW_S("File path to render an HTML report with BoGo results"));

    flag_string_var(&port, BURROW_S("port"), BURROW_S(""), BURROW_S(""));
    flag_bool_var(&server, BURROW_S("server"), false, BURROW_S(""));

    flag_bool_var(&is_handshaker_supported, BURROW_S("is-handshaker-supported"), false,
                  BURROW_S(""));

    flag_string_var(&keyfile, BURROW_S("key-file"), BURROW_S(""), BURROW_S(""));
    flag_string_var(&certfile, BURROW_S("cert-file"), BURROW_S(""), BURROW_S(""));
    flag_base64(&ocsp_response, "ocsp-response", "");
    flag_int_slice(&signing_prefs, "signing-prefs", "");

    flag_string_var(&trust_cert, BURROW_S("trust-cert"), BURROW_S(""), BURROW_S(""));

    flag_int_var(&min_version, BURROW_S("min-version"), TLS_VERSION_SSL30,
                 BURROW_S(""));
    flag_int_var(&max_version, BURROW_S("max-version"), TLS_VERSION_TLS13,
                 BURROW_S(""));
    flag_int_var(&expect_version, BURROW_S("expect-version"), 0, BURROW_S(""));

    flag_bool_var(&no_tls1, BURROW_S("no-tls1"), false, BURROW_S(""));
    flag_bool_var(&no_tls11, BURROW_S("no-tls11"), false, BURROW_S(""));
    flag_bool_var(&no_tls12, BURROW_S("no-tls12"), false, BURROW_S(""));
    flag_bool_var(&no_tls13, BURROW_S("no-tls13"), false, BURROW_S(""));

    flag_bool_var(&require_any_client_certificate,
                  BURROW_S("require-any-client-certificate"), false, BURROW_S(""));

    flag_bool_var(&shim_writes_first, BURROW_S("shim-writes-first"), false,
                  BURROW_S(""));

    flag_int_var(&resume_count, BURROW_S("resume-count"), 0, BURROW_S(""));

    flag_int_slice(&curves, "curves", "");
    flag_string_var(&expected_curve, BURROW_S("expect-curve-id"), BURROW_S(""),
                    BURROW_S(""));

    flag_int_slice(&verify_prefs, "verify-prefs", "");
    flag_string_var(&expected_sig_alg, BURROW_S("expect-peer-signature-algorithm"),
                    BURROW_S(""), BURROW_S(""));
    flag_int_slice(&expected_peer_sig_alg, "expect-peer-verify-pref", "");

    flag_uint64_var(&shim_id, BURROW_S("shim-id"), 0, BURROW_S(""));
    flag_bool_var(&unused_bool, BURROW_S("ipv6"), false, BURROW_S(""));

    flag_base64(&ech_config_list, "ech-config-list", "");
    flag_bool_var(&expect_ech_accepted, BURROW_S("expect-ech-accept"), false,
                  BURROW_S(""));
    flag_bool_var(&expect_hrr, BURROW_S("expect-hrr"), false, BURROW_S(""));
    flag_bool_var(&expect_no_hrr, BURROW_S("expect-no-hrr"), false, BURROW_S(""));
    flag_string_var(&expected_ech_retry_configs, BURROW_S("expect-ech-retry-configs"),
                    BURROW_S(""), BURROW_S(""));
    flag_bool_var(&expect_no_ech_retry_configs, BURROW_S("expect-no-ech-retry-configs"),
                  false, BURROW_S(""));
    flag_bool_var(&on_initial_expect_ech_accepted,
                  BURROW_S("on-initial-expect-ech-accept"), false, BURROW_S(""));
    flag_bool_var(&unused_bool, BURROW_S("expect-no-ech-name-override"), false,
                  BURROW_S(""));
    flag_string_var(&unused_string, BURROW_S("expect-ech-name-override"), BURROW_S(""),
                    BURROW_S(""));
    flag_bool_var(&unused_bool, BURROW_S("reverify-on-resume"), false, BURROW_S(""));
    flag_base64(&on_resume_ech_config_list, "on-resume-ech-config-list", "");
    flag_bool_var(&unused_bool, BURROW_S("on-resume-expect-reject-early-data"), false,
                  BURROW_S(""));
    flag_bool_var(&on_resume_expect_ech_accepted,
                  BURROW_S("on-resume-expect-ech-accept"), false, BURROW_S(""));
    flag_bool_var(&unused_bool, BURROW_S("on-resume-expect-no-ech-name-override"),
                  false, BURROW_S(""));
    flag_string_var(&expected_server_name, BURROW_S("expect-server-name"), BURROW_S(""),
                    BURROW_S(""));
    flag_string_slice(&ech_server_config, "ech-server-config", "");
    flag_string_slice(&ech_server_key, "ech-server-key", "");
    flag_string_slice(&ech_server_retry_config, "ech-is-retry-config", "");

    flag_bool_var(&expect_session_miss, BURROW_S("expect-session-miss"), false,
                  BURROW_S(""));

    flag_bool_var(&unused_bool, BURROW_S("enable-early-data"), false, BURROW_S(""));
    flag_bool_var(&unused_bool, BURROW_S("on-resume-expect-accept-early-data"), false,
                  BURROW_S(""));
    flag_bool_var(&unused_bool, BURROW_S("expect-ticket-supports-early-data"), false,
                  BURROW_S(""));
    flag_bool_var(&unused_bool, BURROW_S("on-resume-shim-writes-first"), false,
                  BURROW_S(""));

    flag_string_var(&advertise_alpn, BURROW_S("advertise-alpn"), BURROW_S(""),
                    BURROW_S(""));
    flag_string_var(&expect_alpn, BURROW_S("expect-alpn"), BURROW_S(""), BURROW_S(""));
    flag_bool_var(&reject_alpn, BURROW_S("reject-alpn"), false, BURROW_S(""));
    flag_bool_var(&decline_alpn, BURROW_S("decline-alpn"), false, BURROW_S(""));
    flag_string_var(&expect_advertised_alpn, BURROW_S("expect-advertised-alpn"),
                    BURROW_S(""), BURROW_S(""));
    flag_string_var(&select_alpn, BURROW_S("select-alpn"), BURROW_S(""), BURROW_S(""));

    flag_string_var(&host_name, BURROW_S("host-name"), BURROW_S(""), BURROW_S(""));

    flag_bool_var(&verify_peer, BURROW_S("verify-peer"), false, BURROW_S(""));
    flag_bool_var(&unused_bool, BURROW_S("use-custom-verify-callback"), false,
                  BURROW_S(""));

    flag_bool_var(&wait_for_debugger, BURROW_S("wait-for-debugger"), false,
                  BURROW_S(""));
}

/* pauseProcess, from bogo_shim_unix_test.go and bogo_shim_notunix_test.go. */
static void pause_process(void) {
#if BURROW_UNIX
    Int pid = os_getpid();
    Error err = BURROW_NO_ERROR;
    OsProcess *process = os_find_process(heap_allocator(), pid, &err);
    (void)os_process_signal(process, os_signal_from_syscall(SYSCALL_SIGSTOP));
#else
    panic_str(BURROW_S("-wait-for-debugger not supported on this OS"));
#endif
}

/* Not in Go: err != io.EOF, which compares the two errors themselves. */
static bool same_error(Error a, Error b) {
    return a.vt == b.vt && a.data == b.data;
}

/* Not in Go: what %s prints for a []SignatureScheme, which has a String
 * method here only for one scheme. */
static Str signature_schemes_string(Alloc *a, Slice schemes) {
    Slice names = slice_nil(TYPE_STRING);
    for (Int i = 0; i < schemes.len; i++) {
        TlsSignatureScheme s = ((const TlsSignatureScheme *)schemes.p)[i];
        names = BURROW_APPEND(Str, a, names, tls_signature_scheme_string(s, a));
    }
    return fmt_sprintf_v(a, "[%s]", strings_join(a, names, BURROW_S(" ")));
}

static TlsConfig *get_config_for_client(void *env, TlsClientHelloInfo *chi,
                                        Error *err) {
    (void)env;
    Alloc *a = heap_allocator();

    if (expect_advertised_alpn.len != 0) {

        CryptobyteString s = slice_from_str(a, expect_advertised_alpn);

        Slice expected_alpns = slice_nil(TYPE_STRING);

        while (!cryptobyte_string_empty(s)) {
            CryptobyteString alpn = slice_nil(TYPE_BYTE);
            if (!cryptobyte_string_read_uint8_length_prefixed(&s, &alpn)) {
                *err = fmt_errorf_v("unexpected error while parsing arguments for "
                                    "-expect-advertised-alpn");
                return NULL;
            }
            expected_alpns =
                BURROW_APPEND(Str, a, expected_alpns, fetchmodule_str(alpn));
        }

        if (!slices_equal(chi->supported_protos, expected_alpns)) {
            *err = fmt_errorf_v("unexpected ALPN: got %q, want %q",
                                chi->supported_protos, expected_alpns);
            return NULL;
        }
    }
    *err = BURROW_NO_ERROR;
    return NULL;
}

/* Not in Go: the certificate Go's two closures return, &pair. */
static TlsCertificate pair;

/* Not in Go: the check Go's GetCertificate and GetClientCertificate make, in
 * one function. */
static bool check_peer_sig_algs(Slice signature_schemes, Error *err) {
    if (expected_peer_sig_alg.p != NULL) {
        const int64_t *want = expected_peer_sig_alg.p;
        const TlsSignatureScheme *got = signature_schemes.p;
        bool ok = signature_schemes.len == expected_peer_sig_alg.len;
        for (Int i = 0; ok && i < expected_peer_sig_alg.len; i++)
            ok = got[i] == (TlsSignatureScheme)want[i];
        if (!ok) {
            Alloc *a = heap_allocator();
            *err = fmt_errorf_v("unexpected signature algorithms: got %s, want %v",
                                signature_schemes_string(a, signature_schemes),
                                expected_peer_sig_alg);
            return false;
        }
    }
    return true;
}

static TlsCertificate *get_certificate(void *env, TlsClientHelloInfo *chi, Error *err) {
    (void)env;
    if (!check_peer_sig_algs(chi->signature_schemes, err))
        return NULL;
    *err = BURROW_NO_ERROR;
    return &pair;
}

static TlsCertificate *get_client_certificate(void *env, TlsCertificateRequestInfo *cri,
                                              Error *err) {
    (void)env;
    if (!check_peer_sig_algs(cri->signature_schemes, err))
        return NULL;
    *err = BURROW_NO_ERROR;
    return &pair;
}

/* If the test case produces an error, we don't want to immediately close the
 * TCP connection after generating an alert. The runner side may try to write
 * additional data to the connection before it reads the alert. If the conn
 * has already been torn down, then these writes will produce an unexpected
 * broken pipe err and fail the test. */
static void orderly_shutdown(TlsConn *tls_conn) {
    /* Flush any pending alert data */
    Error err = BURROW_NO_ERROR;
    (void)burrow__tls_conn_flush(tls_conn, &err);

    NetConn net_conn = tls_conn_net_conn(tls_conn);
    NetTCPConn *tcp_conn = net_conn_as_tcp_conn(net_conn);
    /* Not in Go: Go's type assertion panics when the conn is not TCP. */
    if (tcp_conn == NULL)
        panic_str(BURROW_S("interface conversion: net.Conn is not *net.TCPConn"));
    (void)net_tcp_conn_close_write(tcp_conn);

    /* Read and discard any data that was sent by the peer. */
    Alloc *a = heap_allocator();
    Slice buf = slice_make(a, TYPE_BYTE, TLS_MAX_PLAINTEXT, TLS_MAX_PLAINTEXT);
    for (;;) {
        Int n = net_tcp_conn_read(tcp_conn, buf, &err);
        if (n == 0 || BURROW_FAILED(err))
            break;
    }
    mem_free(a, buf.p, TLS_MAX_PLAINTEXT, 1);

    (void)net_tcp_conn_close_read(tcp_conn);
}

static void bogo_shim(void) {
    Alloc *a = heap_allocator();
    Error err = BURROW_NO_ERROR;

    if (is_handshaker_supported) {
        fmt_println_v("No");
        return;
    }

    Slice args = os_args();
    fmt_printf_v("BoGo shim flags: %q", slice_sub(args, 1, args.len));

    /* Test with both the default and insecure cipher suites. */
    Slice ciphersuites = slice_nil(TYPE_UINT16);
    Slice both[2] = {tls_cipher_suites(a), tls_insecure_cipher_suites(a)};
    Slice suites = slices_concat(a, both, 2);
    for (Int i = 0; i < suites.len; i++) {
        const TlsCipherSuite *s = ((const TlsCipherSuitePtr *)suites.p)[i];
        ciphersuites = BURROW_APPEND(uint16_t, a, ciphersuites, s->id);
    }

    /* Not in Go: the Config is a static, since the connections hold on to it
     * until the program exits. */
    static TlsConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.server_name = BURROW_S("test");

    cfg.min_version = (uint16_t)min_version;
    cfg.max_version = (uint16_t)max_version;

    cfg.client_session_cache = tls_new_lru_client_session_cache(a, 0);

    cfg.cipher_suites = ciphersuites;

    cfg.get_config_for_client =
        BURROW_FN(TlsGetConfigForClientFunc, get_config_for_client, NULL);

    if (no_tls1) {
        cfg.min_version = TLS_VERSION_TLS11;
        if (no_tls11) {
            cfg.min_version = TLS_VERSION_TLS12;
            if (no_tls12) {
                cfg.min_version = TLS_VERSION_TLS13;
                if (no_tls13) {
                    log_fatalf_v("no supported versions enabled");
                }
            }
        }
    } else if (no_tls13) {
        cfg.max_version = TLS_VERSION_TLS12;
        if (no_tls12) {
            cfg.max_version = TLS_VERSION_TLS11;
            if (no_tls11) {
                cfg.max_version = TLS_VERSION_TLS10;
                if (no_tls1) {
                    log_fatalf_v("no supported versions enabled");
                }
            }
        }
    }

    if (advertise_alpn.len != 0) {
        Str alpns = advertise_alpn;
        while (alpns.len > 0) {
            Int alpn_len = (Int)alpns.p[0];
            /* Not in Go: Go's slice expressions panic when the length runs
             * past the end. */
            if (1 + alpn_len > alpns.len)
                panic_str(BURROW_S("runtime error: slice bounds out of range"));
            cfg.next_protos = BURROW_APPEND(Str, a, cfg.next_protos,
                                            str_from_bytes(alpns.p + 1, alpn_len));
            alpns = str_from_bytes(alpns.p + alpn_len + 1, alpns.len - alpn_len - 1);
        }
    }

    if (reject_alpn) {
        cfg.next_protos = slice_nil(TYPE_STRING);
        cfg.next_protos =
            BURROW_APPEND(Str, a, cfg.next_protos, BURROW_S("unnegotiableprotocol"));
    }

    if (decline_alpn) {
        cfg.next_protos = slice_make(a, TYPE_STRING, 0, 0);
    }
    if (select_alpn.len != 0) {
        cfg.next_protos = slice_nil(TYPE_STRING);
        cfg.next_protos = BURROW_APPEND(Str, a, cfg.next_protos, select_alpn);
    }

    if (host_name.len != 0) {
        cfg.server_name = host_name;
    }

    if (keyfile.len != 0 || certfile.len != 0) {
        pair = tls_load_x509_key_pair(a, certfile, keyfile, &err);
        if (BURROW_FAILED(err)) {
            log_fatalf_v("load key-file err: %s", err);
        }
        for (Int i = 0; i < signing_prefs.len; i++) {
            int64_t id = ((const int64_t *)signing_prefs.p)[i];
            pair.supported_signature_algorithms = BURROW_APPEND(
                TlsSignatureScheme, a, pair.supported_signature_algorithms,
                (TlsSignatureScheme)id);
        }
        pair.ocsp_staple = ocsp_response.b;
        /* Use Get[Client]Certificate to force the use of the certificate, which
         * more closely matches the BoGo expectations (e.g. handshake failure if
         * no client certificates are compatible). */
        cfg.get_certificate = BURROW_FN(TlsGetCertificateFunc, get_certificate, NULL);
        cfg.get_client_certificate =
            BURROW_FN(TlsGetClientCertificateFunc, get_client_certificate, NULL);
    }
    if (trust_cert.len != 0) {
        X509CertPool *pool = x509_new_cert_pool(a);
        Slice cert_file = os_read_file(a, trust_cert, &err);
        if (BURROW_FAILED(err)) {
            log_fatalf_v("load trust-cert err: %s", err);
        }
        Slice rest = slice_nil(TYPE_BYTE);
        PemBlock *block = pem_decode(a, cert_file, &rest);
        /* Not in Go: Go's block.Bytes panics on a nil block. */
        if (block == NULL)
            panic_str(BURROW_S(
                "runtime error: invalid memory address or nil pointer dereference"));
        X509Certificate *cert = x509_parse_certificate(a, block->bytes, &err);
        if (BURROW_FAILED(err)) {
            log_fatalf_v("parse trust-cert err: %s", err);
        }
        x509_cert_pool_add_cert(pool, cert);
        cfg.root_cas = pool;
    }

    if (require_any_client_certificate) {
        cfg.client_auth = TLS_REQUIRE_ANY_CLIENT_CERT;
    }
    if (verify_peer) {
        cfg.client_auth = TLS_VERIFY_CLIENT_CERT_IF_GIVEN;
    }

    if (ech_config_list.set) {
        cfg.encrypted_client_hello_config_list = ech_config_list.b;
        cfg.min_version = TLS_VERSION_TLS13;
    }

    if (curves.p != NULL) {
        for (Int i = 0; i < curves.len; i++) {
            int64_t id = ((const int64_t *)curves.p)[i];
            cfg.curve_preferences =
                BURROW_APPEND(TlsCurveID, a, cfg.curve_preferences, (TlsCurveID)id);
        }
    }

    if (verify_prefs.p != NULL) {
        for (Int i = 0; i < verify_prefs.len; i++) {
            int64_t id = ((const int64_t *)verify_prefs.p)[i];
            burrow__tls_testing_only_supported_signature_algorithms =
                BURROW_APPEND(TlsSignatureScheme, a,
                              burrow__tls_testing_only_supported_signature_algorithms,
                              (TlsSignatureScheme)id);
        }
    }

    if (ech_server_config.p != NULL) {
        if (ech_server_config.len != ech_server_key.len ||
            ech_server_config.len != ech_server_retry_config.len) {
            log_fatal_v("-ech-server-config, -ech-server-key, and -ech-is-retry-config "
                        "mismatch");
        }

        for (Int i = 0; i < ech_server_config.len; i++) {
            Str c = ((const Str *)ech_server_config.p)[i];
            Slice config_bytes =
                base64_encoding_decode_string(base64_std_encoding, a, c, &err);
            if (BURROW_FAILED(err)) {
                log_fatalf_v("parse ech-server-config err: %s", err);
            }
            Slice priv_bytes = base64_encoding_decode_string(
                base64_std_encoding, a, ((const Str *)ech_server_key.p)[i], &err);
            if (BURROW_FAILED(err)) {
                log_fatalf_v("parse ech-server-key err: %s", err);
            }

            TlsEncryptedClientHelloKey key = {
                config_bytes, priv_bytes,
                str_eq(((const Str *)ech_server_retry_config.p)[i], BURROW_S("1"))};
            cfg.encrypted_client_hello_keys = BURROW_APPEND(
                TlsEncryptedClientHelloKey, a, cfg.encrypted_client_hello_keys, key);
        }
    }

    /* Not in Go: the connections Go's defer closes when bogoShim returns. */
    NetConn *conns =
        mem_alloc(a, (size_t)(resume_count + 1) * sizeof *conns, _Alignof(NetConn));
    if (conns == NULL)
        log_fatal_v("out of memory");
    for (Int i = 0; i < resume_count + 1; i++) {
        if (i > 0 && on_resume_ech_config_list.set) {
            cfg.encrypted_client_hello_config_list = on_resume_ech_config_list.b;
        }

        NetConn conn =
            net_dial(a, BURROW_S("tcp"),
                     net_join_host_port(a, BURROW_S("localhost"), port), &err);
        if (BURROW_FAILED(err)) {
            log_fatalf_v("dial err: %s", err);
        }
        conns[i] = conn;

        /* Write the shim ID we were passed as a little endian uint64 */
        Slice shim_id_bytes = slice_make(a, TYPE_BYTE, 8, 8);
        binary_little_endian_put_uint64(shim_id_bytes, shim_id);
        (void)conn.vt->writer.write(conn.data, shim_id_bytes, &err);
        if (BURROW_FAILED(err)) {
            log_fatalf_v("failed to write shim id: %s", err);
        }

        TlsConn *tls_conn;
        if (server) {
            tls_conn = tls_server(a, conn, &cfg);
        } else {
            tls_conn = tls_client(a, conn, &cfg);
        }

        if (i == 0 && shim_writes_first) {
            (void)tls_conn_write(tls_conn, slice_from_str(a, BURROW_S("hello")), &err);
            if (BURROW_FAILED(err)) {
                log_fatalf_v("write err: %s", err);
            }
        }

        /* If we were instructed to wait for a debugger, then send SIGSTOP to
         * ourselves. When the debugger attaches it will continue the process. */
        if (wait_for_debugger) {
            pause_process();
        }

        /* Not in Go: one buffer for every read, where Go makes one each time
         * round. */
        Slice buf_mem = slice_make(a, TYPE_BYTE, 500, 500);
        for (;;) {
            Slice buf = buf_mem;
            Int n = tls_conn_read(tls_conn, buf, &err);
            if (BURROW_FAILED(err)) {
                break;
            }
            buf = slice_sub(buf, 0, n);
            Byte *b = buf.p;
            for (Int j = 0; j < buf.len; j++) {
                b[j] ^= 0xff;
            }
            (void)tls_conn_write(tls_conn, buf, &err);
            if (BURROW_FAILED(err)) {
                break;
            }
        }
        if (!same_error(err, io_eof)) {
            /* Flush the TLS conn and then perform a graceful shutdown of the
             * TCP connection to avoid the runner side hitting an unexpected
             * write error before it has processed the alert we may have
             * generated for the error condition. */
            orderly_shutdown(tls_conn);

            /* Not in Go: errors_as, where Go asserts the type of err itself. */
            const TlsECHRejectionError *retry_err =
                (const TlsECHRejectionError *)errors_as(err,
                                                        TYPE_TLS_ECH_REJECTION_ERROR);
            if (retry_err == NULL) {
                log_fatal_v(err);
            }
            if (expect_no_ech_retry_configs && retry_err->retry_config_list.len > 0) {
                log_fatalf_v("expected no ECH retry configs, got some");
            }
            if (expected_ech_retry_configs.len != 0) {
                Error derr = BURROW_NO_ERROR;
                Slice expected_retry_configs = base64_encoding_decode_string(
                    base64_std_encoding, a, expected_ech_retry_configs, &derr);
                if (BURROW_FAILED(derr)) {
                    log_fatalf_v("failed to decode expected retry configs: %s", derr);
                }
                if (!bytes_equal(retry_err->retry_config_list,
                                 expected_retry_configs)) {
                    log_fatalf_v("unexpected retry list returned: got %x, want %x",
                                 retry_err->retry_config_list, expected_retry_configs);
                }
            }
            log_fatalf_v("conn error: %s", err);
        }

        TlsConnectionState cs = tls_conn_connection_state(tls_conn);
        if (cs.handshake_complete) {
            if (expect_alpn.len != 0 && !str_eq(cs.negotiated_protocol, expect_alpn)) {
                log_fatalf_v("unexpected protocol negotiated: want %q, got %q",
                             expect_alpn, cs.negotiated_protocol);
            }

            if (select_alpn.len != 0 && !str_eq(cs.negotiated_protocol, select_alpn)) {
                log_fatalf_v("unexpected protocol negotiated: want %q, got %q",
                             select_alpn, cs.negotiated_protocol);
            }

            if (expect_version != 0 && cs.version != (uint16_t)expect_version) {
                log_fatalf_v("expected ssl version %d, got %d", expect_version,
                             cs.version);
            }
            if (decline_alpn && cs.negotiated_protocol.len != 0) {
                log_fatal_v("unexpected ALPN protocol");
            }
            if (expect_ech_accepted && !cs.ech_accepted) {
                log_fatal_v("expected ECH to be accepted, but connection state shows "
                            "it was not");
            } else if (i == 0 && on_initial_expect_ech_accepted && !cs.ech_accepted) {
                log_fatal_v("expected ECH to be accepted, but connection state shows "
                            "it was not");
            } else if (i > 0 && on_resume_expect_ech_accepted && !cs.ech_accepted) {
                log_fatal_v("expected ECH to be accepted on resumption, but "
                            "connection state shows it was not");
            } else if (i == 0 && !expect_ech_accepted && cs.ech_accepted) {
                log_fatal_v("did not expect ECH, but it was accepted");
            }

            if (expect_hrr && !cs.hello_retry_request) {
                log_fatal_v("expected HRR but did not do it");
            }

            if (expect_no_hrr && cs.hello_retry_request) {
                log_fatal_v("expected no HRR but did do it");
            }

            if (expect_session_miss && cs.did_resume) {
                log_fatal_v("unexpected session resumption");
            }

            if (expected_server_name.len != 0 &&
                !str_eq(cs.server_name, expected_server_name)) {
                log_fatalf_v("unexpected server name: got %q, want %q", cs.server_name,
                             expected_server_name);
            }
        }

        if (expected_curve.len != 0) {
            Int expected_curve_id = strconv_atoi(expected_curve, &err);
            if (BURROW_FAILED(err)) {
                log_fatalf_v("failed to parse -expect-curve-id: %s", err);
            }
            if (cs.curve_id != (TlsCurveID)expected_curve_id) {
                log_fatalf_v("unexpected curve id: want %d, got %d", expected_curve_id,
                             tls_conn->curve_id);
            }
        }

        /* TODO: implement testingOnlyPeerSignatureAlgorithm on resumption. */
        if (expected_sig_alg.len != 0 && !cs.did_resume) {
            Int expected_sig_alg_id = strconv_atoi(expected_sig_alg, &err);
            if (BURROW_FAILED(err)) {
                log_fatalf_v("failed to parse -expect-peer-signature-algorithm: %s",
                             err);
            }
            if (cs.testing_only_peer_signature_algorithm !=
                (TlsSignatureScheme)expected_sig_alg_id) {
                log_fatalf_v("unexpected peer signature algorithm: want %s, got %s",
                             tls_signature_scheme_string(
                                 (TlsSignatureScheme)expected_sig_alg_id, a),
                             tls_signature_scheme_string(
                                 cs.testing_only_peer_signature_algorithm, a));
            }
        }
    }
    for (Int i = resume_count; i >= 0; i--)
        (void)conns[i].vt->closer.close(conns[i].data);
}

/* bogoResults is a copy of boringssl.googlesource.com/boringssl/testresults.Results
 *
 * Not in Go: the struct of a test's result has a name, and so do the maps. */
#define BOGO_TEST_RESULT_FIELDS(F, T)                                                  \
    F(T, Str, Actual, "json:\"actual\"")                                               \
    F(T, Str, Expected, "json:\"expected\"")                                           \
    F(T, bool, IsUnexpected, "json:\"is_unexpected\"")                                 \
    F(T, Str, Error, "json:\"error,omitempty\"")
BURROW_STRUCT(BogoTestResult, BOGO_TEST_RESULT_FIELDS);

BURROW_MAP_TYPE(BogoIntMap, Str, Int);
BURROW_MAP_TYPE(BogoStrMap, Str, Str);
BURROW_MAP_TYPE(BogoTestMap, Str, BogoTestResult);

#define BOGO_RESULTS_FIELDS(F, T)                                                      \
    F(T, Int, Version, "json:\"version\"")                                             \
    F(T, bool, Interrupted, "json:\"interrupted\"")                                    \
    F(T, Str, PathDelimiter, "json:\"path_delimiter\"")                                \
    F(T, double, SecondsSinceEpoch, "json:\"seconds_since_epoch\"")                    \
    F(T, BogoIntMap, NumFailuresByType, "json:\"num_failures_by_type\"")               \
    F(T, BogoTestMap, Tests, "json:\"tests\"")
BURROW_STRUCT(BogoResults, BOGO_RESULTS_FIELDS);

#define REPORT_DATA_FIELDS(F, T)                                                       \
    F(T, BogoResults, Results, "")                                                     \
    F(T, BogoStrMap, SkipReasons, "")                                                  \
    F(T, Str, Timestamp, "")                                                           \
    F(T, Str, Revision, "")
BURROW_STRUCT(ReportData, REPORT_DATA_FIELDS);

static const Str report_template = BURROW_S_INIT(
    "\n"
    "<!DOCTYPE html>\n"
    "<html>\n"
    "<head>\n"
    "    <title>BoGo Results Report</title>\n"
    "    <style>\n"
    "        body { font-family: monospace; margin: 20px; }\n"
    "        .summary { background: #f5f5f5; padding: 10px; margin-bottom: 20px; }\n"
    "        .controls { margin-bottom: 10px; }\n"
    "        .controls input, select { margin-right: 10px; }\n"
    "        table { width: 100%; border-collapse: collapse; table-layout: fixed; }\n"
    "        th, td { border: 1px solid #ddd; padding: 8px; text-align: left; "
    "vertical-align: top; }\n"
    "        th { background-color: #f2f2f2; cursor: pointer; }\n"
    "        .name-col { width: 30%; }\n"
    "        .status-col { width: 8%; }\n"
    "        .actual-col { width: 8%; }\n"
    "        .expected-col { width: 8%; }\n"
    "        .error-col { width: 26%; }\n"
    "        .PASS { background-color: #d4edda; }\n"
    "        .FAIL { background-color: #f8d7da; }\n"
    "        .SKIP { background-color: #fff3cd; }\n"
    "        .error {\n"
    "            font-family: monospace;\n"
    "            font-size: 0.9em;\n"
    "            color: #721c24;\n"
    "            white-space: pre-wrap;\n"
    "            word-break: break-word;\n"
    "        }\n"
    "    </style>\n"
    "</head>\n"
    "<body>\n"
    "<h1>BoGo Results Report</h1>\n"
    "\n"
    "<div class=\"summary\">\n"
    "    <strong>Generated:</strong> {{.Timestamp}} | <strong>BoGo Revision:</strong> "
    "{{.Revision}}<br>\n"
    "    {{range $status, $count := .Results.NumFailuresByType}}\n"
    "    <strong>{{$status}}:</strong> {{$count}} |\n"
    "    {{end}}\n"
    "</div>\n"
    "\n"
    "<div class=\"controls\">\n"
    "    <input type=\"text\" id=\"search\" placeholder=\"Search tests...\" "
    "onkeyup=\"filterTests()\">\n"
    "    <select id=\"statusFilter\" onchange=\"filterTests()\">\n"
    "        <option value=\"\">All</option>\n"
    "        <option value=\"FAIL\">Failed</option>\n"
    "        <option value=\"PASS\">Passed</option>\n"
    "        <option value=\"SKIP\">Skipped</option>\n"
    "    </select>\n"
    "</div>\n"
    "\n"
    "<table id=\"resultsTable\">\n"
    "    <thead>\n"
    "    <tr>\n"
    "        <th class=\"name-col\" onclick=\"sortBy('name')\">Test Name</th>\n"
    "        <th class=\"status-col\" onclick=\"sortBy('status')\">Status</th>\n"
    "        <th class=\"actual-col\" onclick=\"sortBy('actual')\">Actual</th>\n"
    "        <th class=\"expected-col\" onclick=\"sortBy('expected')\">Expected</th>\n"
    "        <th class=\"error-col\">Error</th>\n"
    "    </tr>\n"
    "    </thead>\n"
    "    <tbody>\n"
    "    {{range $name, $test := .Results.Tests}}\n"
    "    <tr class=\"{{$test.Actual}}\" data-name=\"{{$name}}\" "
    "data-status=\"{{$test.Actual}}\">\n"
    "        <td>{{$name}}</td>\n"
    "        <td>{{$test.Actual}}</td>\n"
    "        <td>{{$test.Actual}}</td>\n"
    "        <td>{{$test.Expected}}</td>\n"
    "        <td class=\"error\">{{$test.Error}}</td>\n"
    "    </tr>\n"
    "    {{end}}\n"
    "    </tbody>\n"
    "</table>\n"
    "\n"
    "<script>\n"
    "    function filterTests() {\n"
    "        const search = document.getElementById('search').value.toLowerCase();\n"
    "        const status = document.getElementById('statusFilter').value;\n"
    "        const rows = document.querySelectorAll('#resultsTable tbody tr');\n"
    "\n"
    "        rows.forEach(row => {\n"
    "            const name = row.dataset.name.toLowerCase();\n"
    "            const rowStatus = row.dataset.status;\n"
    "            const matchesSearch = name.includes(search);\n"
    "            const matchesStatus = !status || rowStatus === status;\n"
    "\n"
    "            row.style.display = matchesSearch && matchesStatus ? '' : 'none';\n"
    "        });\n"
    "    }\n"
    "\n"
    "    function sortBy(column) {\n"
    "        const tbody = document.querySelector('#resultsTable tbody');\n"
    "        const rows = Array.from(tbody.querySelectorAll('tr'));\n"
    "\n"
    "        rows.sort((a, b) => {\n"
    "            if (column === 'status') {\n"
    "                const statusOrder = {'FAIL': 0, 'PASS': 1, 'SKIP': 2};\n"
    "                const aStatus = a.dataset.status;\n"
    "                const bStatus = b.dataset.status;\n"
    "                if (aStatus !== bStatus) {\n"
    "                    return statusOrder[aStatus] - statusOrder[bStatus];\n"
    "                }\n"
    "                return a.dataset.name.localeCompare(b.dataset.name);\n"
    "            } else {\n"
    "                return a.dataset.name.localeCompare(b.dataset.name);\n"
    "            }\n"
    "        });\n"
    "\n"
    "        rows.forEach(row => tbody.appendChild(row));\n"
    "        filterTests();\n"
    "    }\n"
    "\n"
    "    sortBy(\"status\");\n"
    "</script>\n"
    "</body>\n"
    "</html>\n");

static Error generate_report(Alloc *a, BogoResults results, Str out_path) {
    ReportData data = {0};
    data.Results = results;
    data.Timestamp = time_format(time_from_unix((int64_t)results.SecondsSinceEpoch, 0),
                                 a, BURROW_S("2006-01-02 15:04:05"));
    data.Revision = boringssl_mod_ver;

    Error err = BURROW_NO_ERROR;
    HtmlTemplate *tmpl =
        html_template_must(html_template_parse(html_template_new(a, BURROW_S("report")),
                                               report_template, &err),
                           err);
    OsFile *file = os_create(a, out_path, &err);
    if (BURROW_FAILED(err)) {
        return err;
    }

    err = html_template_execute(tmpl, os_file_as_io_writer(file),
                                BURROW_ANY(TYPE_OF(ReportData), &data));
    (void)os_file_close(file);
    os_file_free(file);
    return err;
}

/* ensureLocalBogo fetches BoringSSL to localBogoDir at the correct revision
 * (from boringsslModVer) if localBogoDir doesn't already exist.
 *
 * If localBogoDir does exist, ensureLocalBogo fails the test if it isn't
 * a directory. */
static void ensure_local_bogo(TestingT *t, Alloc *a, Str local_bogo_dir) {
    testing_t_helper(t);

    Error err = BURROW_NO_ERROR;
    OsFileInfo stat = os_stat(a, local_bogo_dir, &err);
    if (BURROW_OK(err)) {
        if (!stat.vt->is_dir(stat.data)) {
            testing_t_fatalf_v(t, "local bogo dir (%q) exists but is not a directory",
                               local_bogo_dir);
        }

        testing_t_logf_v(t, "using local bogo checkout from %q", local_bogo_dir);
        return;
    } else if (!errors_is(err, os_err_not_exist)) {
        testing_t_fatalf_v(t, "failed to stat local bogo dir (%q): %v", local_bogo_dir,
                           err);
    }

    /* testenv.MustHaveExecPath */
    Str git = BURROW_S("git");
    SKIP_WITHOUT_EXEC(t);
    (void)exec_look_path(a, git, &err);
    if (BURROW_FAILED(err)) {
        testing_t_skipf_v(t, "skipping test: %s: %s", git, err);
    }

    Int idx = strings_last_index(boringssl_mod_ver, BURROW_S("-"));
    if (idx == -1 || idx == boringssl_mod_ver.len - 1) {
        testing_t_fatalf_v(t, "invalid boringsslModVer format: %q", boringssl_mod_ver);
    }
    Str commit_sha =
        str_from_bytes(boringssl_mod_ver.p + idx + 1, boringssl_mod_ver.len - idx - 1);

    testing_t_logf_v(t, "cloning boringssl@%s to %q", commit_sha, local_bogo_dir);
    ExecCmd *clone_cmd = exec_command_v(
        a, git, 4, BURROW_S("clone"), BURROW_S("--no-checkout"),
        BURROW_S("https://boringssl.googlesource.com/boringssl"), local_bogo_dir);
    err = exec_cmd_run(clone_cmd);
    exec_cmd_free(clone_cmd);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "git clone failed: %v", err);
    }

    ExecCmd *checkout_cmd = exec_command_v(a, git, 2, BURROW_S("checkout"), commit_sha);
    checkout_cmd->dir = local_bogo_dir;
    err = exec_cmd_run(checkout_cmd);
    exec_cmd_free(checkout_cmd);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "git checkout failed: %v", err);
    }

    testing_t_logf_v(t, "using fresh local bogo checkout from %q", local_bogo_dir);
}

/* Not in Go: what Go's closure for each result captures. */
typedef struct BogoRun {
    Str name;
    const BogoTestResult *result;
    Map *assert_results;
} BogoRun;

static void run_result(void *env, TestingT *t) {
    const BogoRun *r = env;
    if (str_eq(r->result->Actual, BURROW_S("FAIL")) && r->result->IsUnexpected) {
        testing_t_fail(t);
    }
    if (r->result->Error.len != 0) {
        testing_t_log_v(t, r->result->Error);
    }
    const Str *exp = BURROW_MAP_GET(Str, Str, r->assert_results, r->name);
    if (exp != NULL && !str_eq(*exp, r->result->Actual)) {
        testing_t_errorf_v(t, "unexpected result: got %s, want %s", r->result->Actual,
                           *exp);
    }
    BURROW_MAP_DEL(Str, r->assert_results, r->name);
    if (str_eq(r->result->Actual, BURROW_S("SKIP"))) {
        testing_t_skip_now(t);
    }
}

static void run_missing(void *env, TestingT *t) {
    const Str *expected_result = env;
    testing_t_fatalf_v(t,
                       "expected test to run with result %s, but it was not present in "
                       "the test results",
                       *expected_result);
}

/* Not in Go: the memory of TestBogoSuite, freed after its other cleanups,
 * even when it is skipped or fails. */
static void bogo_free_arena(void *env) {
    Arena *ar = env;
    arena_free(ar);
    mem_free(heap_allocator(), ar, sizeof *ar, _Alignof(Arena));
}

/* Not in Go: the names and results of assertResults, which Go writes as a map
 * literal. */
static const Str assert_result_names[] = {
    BURROW_S_INIT("CurveTest-Client-X25519MLKEM768-TLS13"),
    BURROW_S_INIT("CurveTest-Server-X25519MLKEM768-TLS13"),
    BURROW_S_INIT("CurveTest-Client-MLKEM1024-TLS13"),
    BURROW_S_INIT("CurveTest-Server-MLKEM1024-TLS13"),

    /* Various signature algorithm tests checking that we enforce our
     * preferences on the peer. */
    BURROW_S_INIT("ClientAuth-Enforced"),
    BURROW_S_INIT("ServerAuth-Enforced"),
    BURROW_S_INIT("ClientAuth-Enforced-TLS13"),
    BURROW_S_INIT("ServerAuth-Enforced-TLS13"),
    BURROW_S_INIT("VerifyPreferences-Advertised"),
    BURROW_S_INIT("VerifyPreferences-Enforced"),
    BURROW_S_INIT("Client-TLS12-NoSign-RSA_PKCS1_MD5_SHA1"),
    BURROW_S_INIT("Server-TLS12-NoSign-RSA_PKCS1_MD5_SHA1"),
    BURROW_S_INIT("Client-TLS13-NoSign-RSA_PKCS1_MD5_SHA1"),
    BURROW_S_INIT("Server-TLS13-NoSign-RSA_PKCS1_MD5_SHA1"),
};

static void TestBogoSuite(TestingT *t) {
    if (testing_short()) {
        testing_t_skip_v(t, "skipping in short mode");
    }
#if defined(BURROW_OS_WINDOWS)
    /* testenv.Builder */
    if (os_getenv(heap_allocator(), BURROW_S("GO_BUILDER_NAME")).len != 0) {
        testing_t_skip_v(t,
                         "#66913: windows network connections are flakey on builders");
    }
#endif
    skip_fips(t);

    Arena *ar = BURROW_NEW(heap_allocator(), Arena);
    if (ar == NULL)
        testing_t_fatal_v(t, "out of memory");
    arena_init(ar, NULL, 0);
    testing_t_cleanup(t, BURROW_FN(Func, bogo_free_arena, ar));
    Alloc *a = arena_allocator(ar);

    /* In order to make Go test caching work as expected, we stat the
     * bogo_config.json file, so that the Go testing hooks know that it is
     * important for this test and will invalidate a cached test result if the
     * file changes. */
    Error err = BURROW_NO_ERROR;
    (void)os_stat(a, bogo_config_file, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
    }

    Str bogo_dir;
    if (bogo_local_dir.len != 0) {
        ensure_local_bogo(t, a, bogo_local_dir);
        bogo_dir = bogo_local_dir;
    } else {
        bogo_dir =
            fetch_module(t, a, BURROW_S("boringssl.googlesource.com/boringssl.git"),
                         boringssl_mod_ver);
    }

    Str cwd = os_getwd(a, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
    }

    Str results_file =
        filepath_join_v(a, 2, fetchmodule_temp_dir(t, a), BURROW_S("results.json"));

    /* Not in Go: testenv.Executable, which is os_executable here. */
    Str executable = os_executable(a, &err);
    if (BURROW_FAILED(err)) {
        testing_t_skipf_v(t, "skipping test: %v", err);
    }

    Slice args = slice_nil(TYPE_STRING);
    args = BURROW_APPEND(Str, a, args, BURROW_S("test"));
    args = BURROW_APPEND(Str, a, args, BURROW_S("."));
    args = BURROW_APPEND(Str, a, args,
                         fmt_sprintf_v(a, "-shim-config=%s",
                                       filepath_join_v(a, 2, cwd, bogo_config_file)));
    args = BURROW_APPEND(Str, a, args, fmt_sprintf_v(a, "-shim-path=%s", executable));
    args = BURROW_APPEND(Str, a, args, BURROW_S("-shim-extra-flags=-bogo-mode"));
    args = BURROW_APPEND(Str, a, args, BURROW_S("-allow-unimplemented"));
    /* TODO(roland): this should be removed eventually */
    args = BURROW_APPEND(Str, a, args, BURROW_S("-loose-errors"));
    args =
        BURROW_APPEND(Str, a, args, fmt_sprintf_v(a, "-json-output=%s", results_file));
    if (bogo_filter.len != 0) {
        args = BURROW_APPEND(Str, a, args, fmt_sprintf_v(a, "-test=%s", bogo_filter));
    }

    ExecCmd *cmd = exec_command(a, go_tool_path(t, a), args);
    cmd->dir = filepath_join_v(a, 2, bogo_dir, BURROW_S("ssl/test/runner"));
    Slice out = exec_cmd_combined_output(cmd, a, &err);
    exec_cmd_free(cmd);
    /* NOTE: we don't immediately check the error, because the failure could be
     * either because the runner failed for some unexpected reason, or because a
     * test case failed, and we cannot easily differentiate these cases. We
     * check if the JSON results file was written, which should only happen if
     * the failure was because of a test failure, and use that to determine the
     * failure mode. */

    Error json_err = BURROW_NO_ERROR;
    Slice results_json = os_read_file(a, results_file, &json_err);
    if (BURROW_FAILED(json_err)) {
        if (BURROW_FAILED(err)) {
            testing_t_fatalf_v(t, "bogo failed: %s\n%s", err, fetchmodule_str(out));
        }
        testing_t_fatalf_v(t, "failed to read results JSON file: %s", json_err);
    }

    BogoResults results = {0};
    err = json_unmarshal(a, results_json, BURROW_ANY(TYPE_OF(BogoResults), &results));
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "failed to parse results JSON: %s", err);
    }

    if (bogo_report.len != 0) {
        err = generate_report(a, results, bogo_report);
        if (BURROW_FAILED(err)) {
            testing_t_fatalf_v(t, "failed to generate report: %v", err);
        }
    }

    /* assertResults contains test results we want to make sure
     * are present in the output. They are only checked if -bogo-filter
     * was not passed. */
    Int n_assert = (Int)(sizeof assert_result_names / sizeof assert_result_names[0]);
    Map *assert_results = map_make(a, TYPE_STRING, TYPE_STRING, n_assert);
    for (Int i = 0; i < n_assert; i++) {
        Str pass = BURROW_S("PASS");
        map_set(assert_results, &assert_result_names[i], &pass);
    }

    MapIter it = map_iter(results.Tests);
    const void *k;
    void *v;
    while (map_next(&it, &k, &v)) {
        /* This is not really the intended way to do this... but... it works? */
        BogoRun *r = BURROW_NEW(a, BogoRun);
        r->name = *(const Str *)k;
        r->result = v;
        r->assert_results = assert_results;
        testing_t_run(t, r->name, BURROW_FN(TestingTFunc, run_result, r));
    }
    if (bogo_filter.len == 0) {
        /* Anything still in assertResults did not show up in the results, so we
         * should fail */
        it = map_iter(assert_results);
        while (map_next(&it, &k, &v)) {
            testing_t_run(t, *(const Str *)k, BURROW_FN(TestingTFunc, run_missing, v));
        }
    }
}

/* The Usage of TestMain. */
static void usage(void *env) {
    (void)env;
    fmt_fprintf_v(flag_flag_set_output(flag_command_line), "Usage of %s:\n", os_args());
    flag_print_defaults();
    if (bogo_mode) {
        os_exit(89);
    }
}

/* Not in Go: the arguments main took out of the command line for TestMain to
 * parse. */
static Slice bogo_args;

static int test_main(TestingM *m) {
    define_flags();

    flag_usage = BURROW_FN(Func, usage, NULL);

    /* Not in Go: flag.Parse, of the arguments main kept for it. */
    (void)flag_flag_set_parse(flag_command_line, bogo_args);

    if (bogo_mode) {
        bogo_shim();
        os_exit(0);
    }

    return testing_m_run(m);
}

#define TESTS(X) X(TestBogoSuite)

/* Not in Go: the -bogo- flags, which the testing package would reject, go to
 * bogo_args, and the rest go to the testing package. When -bogo-mode is
 * there, every argument goes to bogo_args, since they are all the shim's. */
int main(int argc, char **argv) {
    Alloc *h = heap_allocator();
    char **test_argv =
        mem_alloc(h, (size_t)(argc + 1) * sizeof(char *), _Alignof(char *));
    if (test_argv == NULL)
        return 2;
    bool mode = false;
    for (int k = 1; k < argc; k++)
        mode = mode || strcmp(argv[k], "-bogo-mode") == 0 ||
               strcmp(argv[k], "--bogo-mode") == 0 ||
               strncmp(argv[k], "-bogo-mode=", 11) == 0 ||
               strncmp(argv[k], "--bogo-mode=", 12) == 0;
    int test_argc = 0;
    test_argv[test_argc++] = argv[0];
    bogo_args = slice_nil(TYPE_STRING);
    for (int k = 1; k < argc; k++) {
        const char *s = argv[k];
        const char *name = s;
        if (name[0] == '-')
            name++;
        if (name[0] == '-')
            name++;
        if (mode || (s[0] == '-' && strncmp(name, "bogo-", 5) == 0)) {
            bogo_args = BURROW_APPEND(Str, h, bogo_args, str_from_cstr(s));
            /* The value of a string flag can be the next argument. */
            if (!mode && strchr(name, '=') == NULL && strcmp(name, "bogo-mode") != 0 &&
                k + 1 < argc)
                bogo_args = BURROW_APPEND(Str, h, bogo_args, str_from_cstr(argv[++k]));
            continue;
        }
        test_argv[test_argc++] = argv[k];
    }
    test_argv[test_argc] = NULL;
    static const burrow__TestingEntry entries[] = {TESTS(BURROW__TESTING_ENTRY)};
    return burrow__testing_main(test_argc, test_argv, entries,
                                (Int)(sizeof entries / sizeof entries[0]), false,
                                test_main);
}

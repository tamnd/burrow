/* Derived from Go's src/net/smtp/smtp_test.go.
 *
 * StartTLS waits for crypto/tls, so TestNewClientWithTLS, TestTLSClient and
 * TestTLSConnState are left for then, and so is the StartTLS case of
 * TestHello. TestSendMailRefusesSTARTTLS is new, for what smtp_send_mail does
 * meanwhile. Go's fake connection reads from a bufio.Reader and writes to a
 * bufio.Writer that it flushes at the end; here both are BytesBuffers, which
 * need no flush.
 * Go source: go1.27.1.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/net/smtp_internal.h"

#include "burrow/bytes.h"
#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/func.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/net.h"
#include "burrow/net/smtp.h"
#include "burrow/net/textproto.h"
#include "burrow/netpoll.h"
#include "burrow/proc.h"
#include "burrow/slice.h"
#include "burrow/strings.h"
#include "burrow/sync.h"
#include "burrow/type.h"

#include <stdint.h>
#include <string.h>

#if defined(BURROW_NETPOLL_READINESS) && !defined(BURROW_OS_WASI)
#define HAVE_SOCKETS 1
#endif

#define S BURROW_S

/* t.Fatalf, with a return the analyzer can see. */
#define FATALF(...)                                                                    \
    do {                                                                               \
        testing_t_fatalf_v(t, __VA_ARGS__);                                            \
        return;                                                                        \
    } while (0)

#define ARENA_BEGIN                                                                    \
    Arena ar;                                                                          \
    arena_init(&ar, NULL, 0);                                                          \
    Alloc *a = arena_allocator(&ar)
#define ARENA_END arena_free(&ar)

static Slice bytes_of(Str s) {
    return slice_from((Byte *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE);
}

static Str str_of(Slice b) {
    return str_from_bytes(b.p, b.len);
}

/* strings.Join(strings.Split(s, "\n"), "\r\n"). */
static Str crlf(Alloc *a, const char *s) {
    return strings_replace_all(a, str_from_cstr(s), S("\n"), S("\r\n"));
}

/* ------------------------------------------------------------------- Auth */

static void TestAuth(TestingT *t) {
    ARENA_BEGIN;
    struct {
        SmtpAuth auth;
        const char *challenges[1];
        Int nchallenges;
        const char *name;
        Str responses[2];
    } auth_tests[] = {
        {smtp_plain_auth(a, S(""), S("user"), S("pass"), S("testserver")),
         {NULL},
         0,
         "PLAIN",
         {S("\0user\0pass"), BURROW_STR_EMPTY}},
        {smtp_plain_auth(a, S("foo"), S("bar"), S("baz"), S("testserver")),
         {NULL},
         0,
         "PLAIN",
         {S("foo\0bar\0baz"), BURROW_STR_EMPTY}},
        {smtp_crammd5_auth(a, S("user"), S("pass")),
         {"<123456.1322876914@testserver>"},
         1,
         "CRAM-MD5",
         {S(""), S("user 287eb355114cf5c471c26a875f1ca4ae")}},
    };
    for (size_t i = 0; i < sizeof auth_tests / sizeof auth_tests[0]; i++) {
        SmtpAuth auth = auth_tests[i].auth;
        if (auth.vt == NULL)
            FATALF("#%d: out of memory", (Int)i);
        SmtpServerInfo info = {S("testserver"), true, slice_nil(TYPE_STRING)};
        Slice resp;
        Error err;
        Str name = auth.vt->start(auth.data, &info, a, &resp, &err);
        Str want_name = str_from_cstr(auth_tests[i].name);
        if (!str_eq(name, want_name))
            testing_t_errorf_v(t, "#%d got name %s, expected %s", (Int)i, name,
                               want_name);
        if (!str_eq(str_of(resp), auth_tests[i].responses[0]))
            testing_t_errorf_v(t, "#%d got response %s, expected %s", (Int)i,
                               str_of(resp), auth_tests[i].responses[0]);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "#%d error: %v", (Int)i, err);
        for (Int j = 0; j < auth_tests[i].nchallenges; j++) {
            Str challenge = str_from_cstr(auth_tests[i].challenges[j]);
            Str expected = auth_tests[i].responses[j + 1];
            resp = auth.vt->next(auth.data, bytes_of(challenge), true, a, &err);
            if (BURROW_FAILED(err)) {
                testing_t_errorf_v(t, "#%d error: %v", (Int)i, err);
                break;
            }
            if (!str_eq(str_of(resp), expected)) {
                testing_t_errorf_v(t, "#%d got %s, expected %s", (Int)i, str_of(resp),
                                   expected);
                break;
            }
        }
        smtp_auth_free(a, auth);
    }
    ARENA_END;
}

static void TestAuthPlain(TestingT *t) {
    ARENA_BEGIN;
    Str plain[] = {S("PLAIN")};
    Str cram[] = {S("CRAM-MD5")};
    struct {
        Str auth_name;
        SmtpServerInfo server;
        const char *err;
    } tests[] = {
        {S("servername"), {S("servername"), true, slice_nil(TYPE_STRING)}, ""},
        /* OK to use PlainAuth on localhost without TLS */
        {S("localhost"), {S("localhost"), false, slice_nil(TYPE_STRING)}, ""},
        /* NOT OK on non-localhost, even if server says PLAIN is OK.
         * (We don't know that the server is the real server.) */
        {S("servername"),
         {S("servername"), false, slice_from(plain, 1, 1, TYPE_STRING)},
         "unencrypted connection"},
        {S("servername"),
         {S("servername"), false, slice_from(cram, 1, 1, TYPE_STRING)},
         "unencrypted connection"},
        {S("servername"),
         {S("attacker"), true, slice_nil(TYPE_STRING)},
         "wrong host name"},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        SmtpAuth auth =
            smtp_plain_auth(a, S("foo"), S("bar"), S("baz"), tests[i].auth_name);
        if (auth.vt == NULL)
            FATALF("%d. out of memory", (Int)i);
        Slice resp;
        Error err;
        (void)auth.vt->start(auth.data, &tests[i].server, a, &resp, &err);
        Str got = S("");
        if (BURROW_FAILED(err))
            got = error_text(err);
        Str want = str_from_cstr(tests[i].err);
        if (!str_eq(got, want))
            testing_t_errorf_v(t, "%d. got error = %q; want %q", (Int)i, got, want);
        smtp_auth_free(a, auth);
    }
    ARENA_END;
}

/* ------------------------------------------------------------------ faker */

/* A connection that reads the server's side of a conversation from in and
 * writes the client's to out. */
typedef struct Faker {
    BytesBuffer in;
    BytesBuffer out;
} Faker;

static Int faker_read(void *self, Slice p, Error *err) {
    return bytes_buffer_read(&((Faker *)self)->in, p, err);
}

static Int faker_write(void *self, Slice p, Error *err) {
    return bytes_buffer_write(&((Faker *)self)->out, p, err);
}

static Error faker_close(void *self) {
    (void)self;
    return BURROW_NO_ERROR;
}

static const NetConnVT faker_vt = {
    {NULL, faker_read},
    {NULL, faker_write},
    {NULL, faker_close},
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
};

static NetConn faker_conn(Faker *f, Alloc *a, const char *server) {
    f->in = BYTES_BUFFER(a);
    f->out = BYTES_BUFFER(a);
    (void)bytes_buffer_write_string(&f->in, crlf(a, server), NULL);
    return (NetConn){&faker_vt, f};
}

static Str faker_out(Faker *f) {
    return str_of(bytes_buffer_bytes(&f->out));
}

/* &Client{Text: textproto.NewConn(fake), localName: "localhost"}. */
static SmtpClient *fake_client(Faker *f, Alloc *a, const char *server) {
    return burrow__smtp_client_make(a, faker_conn(f, a, server), S(""));
}

/* toServerEmptyAuth is an implementation of Auth that only implements the
 * Start method, and returns "FOOAUTH", nil, nil. Notably, it returns zero
 * bytes for "toServer" so we can test that we don't send spaces at the end
 * of the line. See TestClientAuthTrimSpace. Go's Next panics; this one counts
 * its calls. */
static Str empty_auth_start(void *self, const SmtpServerInfo *server, Alloc *a,
                            Slice *to_server, Error *err) {
    (void)self;
    (void)server;
    (void)a;
    *to_server = slice_nil(TYPE_BYTE);
    *err = BURROW_NO_ERROR;
    return S("FOOAUTH");
}

static Slice empty_auth_next(void *self, Slice from_server, bool more, Alloc *a,
                             Error *err) {
    (void)from_server;
    (void)more;
    (void)a;
    (*(int *)self)++;
    *err = BURROW_NO_ERROR;
    return slice_nil(TYPE_BYTE);
}

static const SmtpAuthVT empty_auth_vt = {NULL, empty_auth_start, empty_auth_next};

/* Issue 17794: don't send a trailing space on AUTH command when there's no
 * password. */
static void TestClientAuthTrimSpace(TestingT *t) {
    ARENA_BEGIN;
    Faker fake;
    NetConn conn = faker_conn(&fake, a, "220 hello world\n200 some more");
    Error err;
    SmtpClient *c = smtp_new_client(a, conn, S("fake.host"), &err);
    if (c == NULL)
        FATALF("NewClient: %v", err);
    c->tls = true;
    c->did_hello = true;
    int next_calls = 0;
    (void)smtp_client_auth(c, (SmtpAuth){&empty_auth_vt, &next_calls});
    (void)smtp_client_close(c);
    Str got = faker_out(&fake);
    Str want = S("AUTH FOOAUTH\r\n*\r\nQUIT\r\n");
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "wrote %q; want %q", got, want);
    if (next_calls != 0)
        testing_t_errorf_v(t, "unexpected call of Next");
    smtp_client_free(c);
    ARENA_END;
}

/* ------------------------------------------------------------------ Basic */

static const char basic_server[] = "250 mx.google.com at your service\n"
                                   "502 Unrecognized command.\n"
                                   "250-mx.google.com at your service\n"
                                   "250-SIZE 35651584\n"
                                   "250-AUTH LOGIN PLAIN\n"
                                   "250 8BITMIME\n"
                                   "530 Authentication required\n"
                                   "252 Send some mail, I'll try my best\n"
                                   "250 User is valid\n"
                                   "235 Accepted\n"
                                   "250 Sender OK\n"
                                   "250 Receiver OK\n"
                                   "354 Go ahead\n"
                                   "250 Data OK\n"
                                   "221 OK\n";

static const char basic_client[] = "HELO localhost\n"
                                   "EHLO localhost\n"
                                   "EHLO localhost\n"
                                   "MAIL FROM:<user@gmail.com> BODY=8BITMIME\n"
                                   "VRFY user1@gmail.com\n"
                                   "VRFY user2@gmail.com\n"
                                   "AUTH PLAIN AHVzZXIAcGFzcw==\n"
                                   "MAIL FROM:<user@gmail.com> BODY=8BITMIME\n"
                                   "RCPT TO:<golang-nuts@googlegroups.com>\n"
                                   "DATA\n"
                                   "From: user@gmail.com\n"
                                   "To: golang-nuts@googlegroups.com\n"
                                   "Subject: Hooray for Go\n"
                                   "\n"
                                   "Line 1\n"
                                   "..Leading dot line .\n"
                                   "Goodbye.\n"
                                   ".\n"
                                   "QUIT\n";

static void TestBasic(TestingT *t) {
    ARENA_BEGIN;
    Faker fake;
    SmtpClient *c = fake_client(&fake, a, basic_server);
    if (c == NULL)
        FATALF("out of memory");
    Str client = crlf(a, basic_client);

    Error err = burrow__smtp_client_helo(c);
    if (BURROW_FAILED(err))
        FATALF("HELO failed: %v", err);
    if (BURROW_OK(burrow__smtp_client_ehlo(c)))
        FATALF("Expected first EHLO to fail");
    err = burrow__smtp_client_ehlo(c);
    if (BURROW_FAILED(err))
        FATALF("Second EHLO failed: %v", err);

    c->did_hello = true;
    Str args;
    if (!smtp_client_extension(c, S("aUtH"), &args) || !str_eq(args, S("LOGIN PLAIN")))
        FATALF("Expected AUTH supported");
    if (smtp_client_extension(c, S("DSN"), NULL))
        FATALF("Shouldn't support DSN");

    if (BURROW_OK(smtp_client_mail(c, S("user@gmail.com"))))
        FATALF("MAIL should require authentication");

    if (BURROW_OK(smtp_client_verify(c, S("user1@gmail.com"))))
        FATALF("First VRFY: expected no verification");
    if (BURROW_OK(smtp_client_verify(
            c, S("user2@gmail.com>\r\nDATA\r\nAnother injected message body\r\n.\r\n"
                 "QUIT\r\n"))))
        FATALF("VRFY should have failed due to a message injection attempt");
    err = smtp_client_verify(c, S("user2@gmail.com"));
    if (BURROW_FAILED(err))
        FATALF("Second VRFY: expected verification, got %v", err);

    /* fake TLS so authentication won't complain */
    c->tls = true;
    c->server_name = S("smtp.google.com");
    SmtpAuth auth =
        smtp_plain_auth(a, S(""), S("user"), S("pass"), S("smtp.google.com"));
    err = smtp_client_auth(c, auth);
    smtp_auth_free(a, auth);
    if (BURROW_FAILED(err))
        FATALF("AUTH failed: %v", err);

    if (BURROW_OK(smtp_client_rcpt(c, S("golang-nuts@googlegroups.com>\r\nDATA\r\n"
                                        "Injected message body\r\n.\r\nQUIT\r\n"))))
        FATALF("RCPT should have failed due to a message injection attempt");
    if (BURROW_OK(smtp_client_mail(c, S("user@gmail.com>\r\nDATA\r\nAnother injected "
                                        "message body\r\n.\r\nQUIT\r\n"))))
        FATALF("MAIL should have failed due to a message injection attempt");
    err = smtp_client_mail(c, S("user@gmail.com"));
    if (BURROW_FAILED(err))
        FATALF("MAIL failed: %v", err);
    err = smtp_client_rcpt(c, S("golang-nuts@googlegroups.com"));
    if (BURROW_FAILED(err))
        FATALF("RCPT failed: %v", err);
    Str msg = S("From: user@gmail.com\n"
                "To: golang-nuts@googlegroups.com\n"
                "Subject: Hooray for Go\n"
                "\n"
                "Line 1\n"
                ".Leading dot line .\n"
                "Goodbye.");
    IoWriteCloser w = smtp_client_data(c, &err);
    if (BURROW_FAILED(err))
        FATALF("DATA failed: %v", err);
    (void)w.vt->writer.write(w.data, bytes_of(msg), &err);
    if (BURROW_FAILED(err))
        FATALF("Data write failed: %v", err);
    err = w.vt->closer.close(w.data);
    if (BURROW_FAILED(err))
        FATALF("Bad data response: %v", err);

    err = smtp_client_quit(c);
    if (BURROW_FAILED(err))
        FATALF("QUIT failed: %v", err);

    Str actualcmds = faker_out(&fake);
    if (!str_eq(client, actualcmds))
        testing_t_fatalf_v(t, "Got:\n%s\nExpected:\n%s", actualcmds, client);
    smtp_client_free(c);
    ARENA_END;
}

static void TestHELOFailed(TestingT *t) {
    ARENA_BEGIN;
    Faker fake;
    SmtpClient *c = fake_client(&fake, a, "502 EH?\n502 EH?\n221 OK\n");
    if (c == NULL)
        FATALF("out of memory");
    Str client = crlf(a, "EHLO localhost\nHELO localhost\nQUIT\n");
    if (BURROW_OK(smtp_client_hello(c, S("localhost"))))
        FATALF("expected EHLO to fail");
    Error err = smtp_client_quit(c);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "QUIT failed: %v", err);
    Str actual = faker_out(&fake);
    if (!str_eq(client, actual))
        testing_t_errorf_v(t, "Got:\n%s\nWant:\n%s", actual, client);
    smtp_client_free(c);
    ARENA_END;
}

/* ------------------------------------------------------------- Extensions */

typedef struct ExtensionTest {
    const char *server;
    const char *client;
    bool use_helo; /* the "helo" case, which calls helo rather than Hello */
    bool bit8, utf8;
    const char *from;
} ExtensionTest;

static void extension_case(void *env, TestingT *t) {
    const ExtensionTest *tt = (const ExtensionTest *)env;
    ARENA_BEGIN;
    Faker fake;
    SmtpClient *c = fake_client(&fake, a, tt->server);
    if (c == NULL) {
        ARENA_END;
        FATALF("out of memory");
    }
    Error err;
    if (tt->use_helo) {
        err = burrow__smtp_client_helo(c);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "HELO failed: %v", err);
        c->did_hello = true;
    } else {
        err = smtp_client_hello(c, S("localhost"));
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "EHLO failed: %v", err);
        if (smtp_client_extension(c, S("8BITMIME"), NULL) != tt->bit8)
            testing_t_errorf_v(t, "8BITMIME support is not %t", tt->bit8);
        if (smtp_client_extension(c, S("SMTPUTF8"), NULL) != tt->utf8)
            testing_t_errorf_v(t, "SMTPUTF8 support is not %t", tt->utf8);
    }
    err = smtp_client_mail(c, str_from_cstr(tt->from));
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "MAIL FROM failed: %v", err);
    err = smtp_client_quit(c);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "QUIT failed: %v", err);
    Str actualcmds = faker_out(&fake);
    Str client = crlf(a, tt->client);
    if (!str_eq(client, actualcmds))
        testing_t_errorf_v(t, "Got:\n%s\nExpected:\n%s", actualcmds, client);
    smtp_client_free(c);
    ARENA_END;
}

static const ExtensionTest extension_tests[] = {
    {"250 mx.google.com at your service\n"
     "250 Sender OK\n"
     "221 Goodbye\n",
     "HELO localhost\n"
     "MAIL FROM:<user@gmail.com>\n"
     "QUIT\n",
     true, false, false, "user@gmail.com"},
    {"250-mx.google.com at your service\n"
     "250 SIZE 35651584\n"
     "250 Sender OK\n"
     "221 Goodbye\n",
     "EHLO localhost\n"
     "MAIL FROM:<user@gmail.com>\n"
     "QUIT\n",
     false, false, false, "user@gmail.com"},
    {"250-mx.google.com at your service\n"
     "250-SIZE 35651584\n"
     "250 8BITMIME\n"
     "250 Sender OK\n"
     "221 Goodbye\n",
     "EHLO localhost\n"
     "MAIL FROM:<user@gmail.com> BODY=8BITMIME\n"
     "QUIT\n",
     false, true, false, "user@gmail.com"},
    {"250-mx.google.com at your service\n"
     "250-SIZE 35651584\n"
     "250 SMTPUTF8\n"
     "250 Sender OK\n"
     "221 Goodbye\n",
     "EHLO localhost\n"
     "MAIL FROM:<user+\xf0\x9f\x93\xa7@gmail.com> SMTPUTF8\n"
     "QUIT\n",
     false, false, true, "user+\xf0\x9f\x93\xa7@gmail.com"},
    {"250-mx.google.com at your service\n"
     "250-SIZE 35651584\n"
     "250-8BITMIME\n"
     "250 SMTPUTF8\n"
     "250 Sender OK\n"
     "221 Goodbye\n"
     "\t",
     "EHLO localhost\n"
     "MAIL FROM:<user+\xf0\x9f\x93\xa7@gmail.com> BODY=8BITMIME SMTPUTF8\n"
     "QUIT\n",
     false, true, true, "user+\xf0\x9f\x93\xa7@gmail.com"},
};

static void TestExtensions(TestingT *t) {
    static const char *const names[] = {"helo", "ehlo", "ehlo 8bitmime",
                                        "ehlo smtputf8", "ehlo 8bitmime smtputf8"};
    for (size_t i = 0; i < sizeof extension_tests / sizeof extension_tests[0]; i++)
        (void)testing_t_run(t, str_from_cstr(names[i]),
                            BURROW_FN(TestingTFunc, extension_case,
                                      (void *)(uintptr_t)&extension_tests[i]));
}

/* -------------------------------------------------------------- NewClient */

/* The end of TestNewClient and TestNewClient2. */
static void quit_and_compare(TestingT *t, SmtpClient *c, Faker *fake, Str client) {
    Error err = smtp_client_quit(c);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "QUIT failed: %v", err);
        return;
    }
    Str actualcmds = faker_out(fake);
    if (!str_eq(client, actualcmds))
        testing_t_errorf_v(t, "Got:\n%s\nExpected:\n%s", actualcmds, client);
}

static void TestNewClient(TestingT *t) {
    ARENA_BEGIN;
    Faker fake;
    NetConn conn = faker_conn(&fake, a,
                              "220 hello world\n"
                              "250-mx.google.com at your service\n"
                              "250-SIZE 35651584\n"
                              "250-AUTH LOGIN PLAIN\n"
                              "250 8BITMIME\n"
                              "221 OK\n");
    Str client = crlf(a, "EHLO localhost\nQUIT\n");
    Error err;
    SmtpClient *c = smtp_new_client(a, conn, S("fake.host"), &err);
    if (c == NULL)
        FATALF("NewClient: %v\n(after %s)", err, faker_out(&fake));
    Str args;
    if (!smtp_client_extension(c, S("aUtH"), &args) || !str_eq(args, S("LOGIN PLAIN")))
        testing_t_errorf_v(t, "Expected AUTH supported");
    else if (smtp_client_extension(c, S("DSN"), NULL))
        testing_t_errorf_v(t, "Shouldn't support DSN");
    else
        quit_and_compare(t, c, &fake, client);
    (void)smtp_client_close(c);
    smtp_client_free(c);
    ARENA_END;
}

static void TestNewClient2(TestingT *t) {
    ARENA_BEGIN;
    Faker fake;
    NetConn conn = faker_conn(&fake, a,
                              "220 hello world\n"
                              "502 EH?\n"
                              "250-mx.google.com at your service\n"
                              "250-SIZE 35651584\n"
                              "250-AUTH LOGIN PLAIN\n"
                              "250 8BITMIME\n"
                              "221 OK\n");
    Str client = crlf(a, "EHLO localhost\nHELO localhost\nQUIT\n");
    Error err;
    SmtpClient *c = smtp_new_client(a, conn, S("fake.host"), &err);
    if (c == NULL)
        FATALF("NewClient: %v", err);
    if (smtp_client_extension(c, S("DSN"), NULL))
        testing_t_errorf_v(t, "Shouldn't support DSN");
    else
        quit_and_compare(t, c, &fake, client);
    (void)smtp_client_close(c);
    smtp_client_free(c);
    ARENA_END;
}

/* ------------------------------------------------------------------ Hello */

static const char base_hello_server[] = "220 hello world\n"
                                        "502 EH?\n"
                                        "250-mx.google.com at your service\n"
                                        "250 FEATURE\n";

static const char *const hello_server[] = {
    "",
    "502 Not implemented\n",
    "250 User is valid\n",
    "235 Accepted\n",
    "250 Sender ok\n",
    "",
    "250 Reset ok\n",
    "221 Goodbye\n",
    "250 Sender ok\n",
    "250 ok\n",
};

static const char base_hello_client[] = "EHLO customhost\n"
                                        "HELO customhost\n";

static const char *const hello_client[] = {
    "",
    "STARTTLS\n",
    "VRFY test@example.com\n",
    "AUTH PLAIN AHVzZXIAcGFzcw==\n",
    "MAIL FROM:<test@example.com>\n",
    "",
    "RSET\n",
    "QUIT\n",
    "VRFY test@example.com\n",
    "NOOP\n",
};

static void TestHello(TestingT *t) {
    ARENA_BEGIN;
    for (int i = 0; i < (int)(sizeof hello_server / sizeof hello_server[0]); i++) {
        if (i == 1)
            continue; /* StartTLS, which waits for crypto/tls */
        Str server = fmt_sprintf_v(a, "%s%s", str_from_cstr(base_hello_server),
                                   str_from_cstr(hello_server[i]));
        Str client = fmt_sprintf_v(a, "%s%s", str_from_cstr(base_hello_client),
                                   str_from_cstr(hello_client[i]));
        client = strings_replace_all(a, client, S("\n"), S("\r\n"));
        Faker fake;
        fake.in = BYTES_BUFFER(a);
        fake.out = BYTES_BUFFER(a);
        (void)bytes_buffer_write_string(
            &fake.in, strings_replace_all(a, server, S("\n"), S("\r\n")), NULL);
        Error err;
        SmtpClient *c =
            smtp_new_client(a, (NetConn){&faker_vt, &fake}, S("fake.host"), &err);
        if (c == NULL)
            FATALF("NewClient: %v", err);
        c->local_name = S("customhost");
        err = BURROW_NO_ERROR;

        switch (i) {
        case 0:
            err = smtp_client_hello(c, S("hostinjection>\n\rDATA\r\nInjected message "
                                         "body\r\n.\r\nQUIT\r\n"));
            if (BURROW_OK(err))
                testing_t_errorf_v(t, "Expected Hello to be rejected due to a message "
                                      "injection attempt");
            err = smtp_client_hello(c, S("customhost"));
            break;
        case 2:
            err = smtp_client_verify(c, S("test@example.com"));
            break;
        case 3: {
            c->tls = true;
            c->server_name = S("smtp.google.com");
            SmtpAuth auth =
                smtp_plain_auth(a, S(""), S("user"), S("pass"), S("smtp.google.com"));
            err = smtp_client_auth(c, auth);
            smtp_auth_free(a, auth);
            break;
        }
        case 4:
            err = smtp_client_mail(c, S("test@example.com"));
            break;
        case 5:
            if (smtp_client_extension(c, S("feature"), NULL))
                testing_t_errorf_v(t, "Expected FEATURE not to be supported");
            break;
        case 6:
            err = smtp_client_reset(c);
            break;
        case 7:
            err = smtp_client_quit(c);
            break;
        case 8:
            err = smtp_client_verify(c, S("test@example.com"));
            if (BURROW_FAILED(err)) {
                err = smtp_client_hello(c, S("customhost"));
                if (BURROW_FAILED(err))
                    testing_t_errorf_v(t, "Want error, got none");
            }
            break;
        case 9:
            err = smtp_client_noop(c);
            break;
        default:
            break;
        }

        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "Command %d failed: %v", i, err);

        Str actualcmds = faker_out(&fake);
        if (!str_eq(client, actualcmds))
            testing_t_errorf_v(t, "Got:\n%s\nExpected:\n%s", actualcmds, client);
        (void)smtp_client_close(c);
        smtp_client_free(c);
    }
    ARENA_END;
}

/* --------------------------------------------------------------- SendMail */

#if defined(HAVE_SOCKETS)

/* A server on the other end of a real connection, which sends the lines of
 * data and writes what it reads to cmds, as the goroutine in Go's
 * TestSendMail does. */
typedef struct ScriptServer {
    NetListener l;
    const char *const *data;
    Int n;
    BytesBuffer cmds;
    Error err;
    SyncWaitGroup wg;
} ScriptServer;

/* conn, as the IoReadWriteCloser that textproto wants. */
static Int conn_read(void *self, Slice p, Error *err) {
    NetConn c = *(NetConn *)self;
    return c.vt->reader.read(c.data, p, err);
}

static Int conn_write(void *self, Slice p, Error *err) {
    NetConn c = *(NetConn *)self;
    return c.vt->writer.write(c.data, p, err);
}

static Error conn_close(void *self) {
    NetConn c = *(NetConn *)self;
    return c.vt->closer.close(c.data);
}

static const IoReadWriteCloserVT conn_rwc_vt = {
    {NULL, conn_read},
    {NULL, conn_write},
    {NULL, conn_close},
};

static void script_serve(void *env) {
    ScriptServer *s = (ScriptServer *)env;
    Arena sar;
    arena_init(&sar, NULL, 0);
    Alloc *a = arena_allocator(&sar);
    Error err;
    NetConn conn = s->l.vt->accept(s->l.data, &err);
    if (BURROW_FAILED(err)) {
        s->err = error_retain(heap_allocator(), err);
        arena_free(&sar);
        return;
    }
    TextprotoConn *tc = textproto_new_conn(a, (IoReadWriteCloser){&conn_rwc_vt, &conn});
    for (Int i = 0; tc != NULL && i < s->n && s->data[i][0] != '\0'; i++) {
        (void)textproto_conn_printf_line_v(tc, "%s", s->data[i]);
        while (strlen(s->data[i]) >= 4 && s->data[i][3] == '-') {
            i++;
            (void)textproto_conn_printf_line_v(tc, "%s", s->data[i]);
        }
        /* The last line is the server's last word, as in Go's
         * TestSendMailWithAuth, which answers the EHLO and hangs up. Reading
         * after it would only find the client's EOF. */
        if (strcmp(s->data[i], "221 Goodbye") == 0 || i == s->n - 1)
            break;
        bool go_ahead = strcmp(s->data[i], "354 Go ahead") == 0;
        bool read = false;
        while (!read || go_ahead) {
            Str msg = textproto_conn_read_line(tc, a, &err);
            (void)bytes_buffer_write_string(&s->cmds, msg, NULL);
            (void)bytes_buffer_write_string(&s->cmds, S("\r\n"), NULL);
            read = true;
            if (BURROW_FAILED(err)) {
                s->err = error_retain(heap_allocator(), err);
                i = s->n;
                break;
            }
            if (go_ahead && str_eq(msg, S(".")))
                break;
        }
    }
    textproto_conn_free(tc);
    net_conn_free(conn);
    arena_free(&sar);
}

/* Listens on 127.0.0.1 and serves data on the first connection, and gives
 * the address. The empty Str on a failure, which it has reported. */
static Str script_start(TestingT *t, Alloc *a, ScriptServer *s, const char *const *data,
                        Int n) {
    memset(s, 0, sizeof *s);
    s->data = data;
    s->n = n;
    s->cmds = BYTES_BUFFER(heap_allocator());
    Error err;
    s->l = net_listen(heap_allocator(), S("tcp"), S("127.0.0.1:0"), &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "Unable to create listener: %v", err);
        return BURROW_STR_EMPTY;
    }
    NetAddr addr = s->l.vt->addr(s->l.data);
    Str hostport = addr.vt->string(addr.data, a);
    if (!sync_wait_group_go(&s->wg, BURROW_FN(Func, script_serve, s))) {
        testing_t_errorf_v(t, "no goroutine for the server");
        net_listener_free(s->l);
        return BURROW_STR_EMPTY;
    }
    return hostport;
}

/* Waits for the server, and gives what it read. */
static Str script_end(ScriptServer *s, Alloc *a) {
    sync_wait_group_wait(&s->wg);
    net_listener_free(s->l);
    Str cmds = str_clone(a, str_of(bytes_buffer_bytes(&s->cmds)));
    bytes_buffer_free(&s->cmds);
    return cmds;
}
#endif

#define SEND_MAIL_MESSAGE                                                              \
    "From: test@example.com\r\n"                                                       \
    "To: other@example.com\r\n"                                                        \
    "Subject: SendMail test\r\n"                                                       \
    "\r\n"                                                                             \
    "SendMail is working for me.\r\n"

static void TestSendMail(TestingT *t) {
#if !defined(HAVE_SOCKETS)
    testing_t_skip_v(t, "sockets here need the readiness poll FD");
#else
    ARENA_BEGIN;
    static const char *const data[] = {
        "220 hello world", "502 EH?",         "250 mx.google.com at your service",
        "250 Sender ok",   "250 Receiver ok", "354 Go ahead",
        "250 Data ok",     "221 Goodbye",     "",
    };
    Str client = crlf(a, "EHLO localhost\n"
                         "HELO localhost\n"
                         "MAIL FROM:<test@example.com>\n"
                         "RCPT TO:<other@example.com>\n"
                         "DATA\n"
                         "From: test@example.com\n"
                         "To: other@example.com\n"
                         "Subject: SendMail test\n"
                         "\n"
                         "SendMail is working for me.\n"
                         ".\n"
                         "QUIT\n");
    ScriptServer s;
    Str addr = script_start(t, a, &s, data, (Int)(sizeof data / sizeof data[0]));
    if (addr.len == 0) {
        ARENA_END;
        return;
    }

    Str injected[] = {S("other@example.com>\n\rDATA\r\nInjected message body\r\n."
                        "\r\nQUIT\r\n")};
    Str msg = S(SEND_MAIL_MESSAGE);
    Error err = smtp_send_mail(addr, (SmtpAuth){NULL, NULL}, S("test@example.com"),
                               slice_from(injected, 1, 1, TYPE_STRING), bytes_of(msg));
    if (BURROW_OK(err))
        testing_t_errorf_v(t, "Expected SendMail to be rejected due to a message "
                              "injection attempt");

    Str to[] = {S("other@example.com")};
    err = smtp_send_mail(addr, (SmtpAuth){NULL, NULL}, S("test@example.com"),
                         slice_from(to, 1, 1, TYPE_STRING), bytes_of(msg));
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "%v", err);

    Str actualcmds = script_end(&s, a);
    if (BURROW_FAILED(s.err))
        testing_t_errorf_v(t, "Read error: %v", s.err);
    if (!str_eq(client, actualcmds))
        testing_t_errorf_v(t, "Got:\n%s\nExpected:\n%s", actualcmds, client);
    ARENA_END;
#endif
}

static void TestSendMailWithAuth(TestingT *t) {
#if !defined(HAVE_SOCKETS)
    testing_t_skip_v(t, "sockets here need the readiness poll FD");
#else
    ARENA_BEGIN;
    /* Go's server reads the EHLO, answers it and hangs up, which the script
     * does by ending there. */
    static const char *const data[] = {
        "220 hello world",
        "250 mx.google.com at your service",
    };
    ScriptServer s;
    Str addr = script_start(t, a, &s, data, (Int)(sizeof data / sizeof data[0]));
    if (addr.len == 0) {
        ARENA_END;
        return;
    }
    SmtpAuth auth =
        smtp_plain_auth(a, S(""), S("user"), S("pass"), S("smtp.google.com"));
    Str to[] = {S("other@example.com")};
    Error err = smtp_send_mail(addr, auth, S("test@example.com"),
                               slice_from(to, 1, 1, TYPE_STRING),
                               bytes_of(S(SEND_MAIL_MESSAGE)));
    smtp_auth_free(a, auth);
    if (BURROW_OK(err))
        testing_t_errorf_v(t, "SendMail: Server doesn't support AUTH, expected to get "
                              "an error, but got none ");
    else if (!str_eq(error_text(err), S("smtp: server doesn't support AUTH")))
        testing_t_errorf_v(t, "Expected: smtp: server doesn't support AUTH, got: %v",
                           err);
    Str cmds = script_end(&s, a);
    if (BURROW_FAILED(s.err))
        testing_t_fatalf_v(t, "server error: %v", s.err);
    else if (!str_eq(cmds, S("EHLO localhost\r\n")))
        testing_t_errorf_v(t, "unexpected response %q; want %q", cmds,
                           S("EHLO localhost\r\n"));
    ARENA_END;
#endif
}

/* Not Go's. Without crypto/tls, a server that offers STARTTLS gets an error
 * and no mail, where Go would have sent the mail over TLS. */
static void TestSendMailRefusesSTARTTLS(TestingT *t) {
#if !defined(HAVE_SOCKETS)
    testing_t_skip_v(t, "sockets here need the readiness poll FD");
#else
    ARENA_BEGIN;
    static const char *const data[] = {
        "220 hello world",
        "250-mx.google.com at your service",
        "250 STARTTLS",
    };
    ScriptServer s;
    Str addr = script_start(t, a, &s, data, (Int)(sizeof data / sizeof data[0]));
    if (addr.len == 0) {
        ARENA_END;
        return;
    }
    Str to[] = {S("other@example.com")};
    Error err = smtp_send_mail(addr, (SmtpAuth){NULL, NULL}, S("test@example.com"),
                               slice_from(to, 1, 1, TYPE_STRING),
                               bytes_of(S(SEND_MAIL_MESSAGE)));
    if (!errors_is(err, smtp_err_no_tls))
        testing_t_errorf_v(t, "SendMail = %v, want %v", err, smtp_err_no_tls);
    Str cmds = script_end(&s, a);
    if (BURROW_FAILED(s.err))
        testing_t_errorf_v(t, "server error: %v", s.err);
    if (!str_eq(cmds, S("EHLO localhost\r\n")))
        testing_t_errorf_v(t, "server read %q; want %q", cmds, S("EHLO localhost\r\n"));
    ARENA_END;
#endif
}

static void TestAuthFailed(TestingT *t) {
    ARENA_BEGIN;
    Faker fake;
    NetConn conn = faker_conn(&fake, a,
                              "220 hello world\n"
                              "250-mx.google.com at your service\n"
                              "250 AUTH LOGIN PLAIN\n"
                              "535-Invalid credentials\n"
                              "535 please see www.example.com\n"
                              "221 Goodbye\n");
    Str client = crlf(a, "EHLO localhost\n"
                         "AUTH PLAIN AHVzZXIAcGFzcw==\n"
                         "*\n"
                         "QUIT\n");
    Error err;
    SmtpClient *c = smtp_new_client(a, conn, S("fake.host"), &err);
    if (c == NULL)
        FATALF("NewClient: %v", err);

    c->tls = true;
    c->server_name = S("smtp.google.com");
    SmtpAuth auth =
        smtp_plain_auth(a, S(""), S("user"), S("pass"), S("smtp.google.com"));
    err = smtp_client_auth(c, auth);
    smtp_auth_free(a, auth);

    Str want = S("535 \"Invalid credentials\\nplease see www.example.com\"");
    if (BURROW_OK(err))
        testing_t_errorf_v(t, "Auth: expected error; got none");
    else if (!str_eq(error_text(err), want))
        testing_t_errorf_v(t, "Auth: got error: %v, want: %s", err, want);

    Str actualcmds = faker_out(&fake);
    if (!str_eq(client, actualcmds))
        testing_t_errorf_v(t, "Got:\n%s\nExpected:\n%s", actualcmds, client);
    (void)smtp_client_close(c);
    smtp_client_free(c);
    ARENA_END;
}

#define TESTS(X)                                                                       \
    X(TestAuth)                                                                        \
    X(TestAuthPlain)                                                                   \
    X(TestClientAuthTrimSpace)                                                         \
    X(TestBasic)                                                                       \
    X(TestHELOFailed)                                                                  \
    X(TestExtensions)                                                                  \
    X(TestNewClient)                                                                   \
    X(TestNewClient2)                                                                  \
    X(TestHello)                                                                       \
    X(TestSendMail)                                                                    \
    X(TestSendMailWithAuth)                                                            \
    X(TestSendMailRefusesSTARTTLS)                                                     \
    X(TestAuthFailed)

TESTING_MAIN(TESTS)

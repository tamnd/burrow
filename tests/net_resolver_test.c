/* The DNS client against a fake name server, from Go's
 * dnsclient_unix_test.go.
 *
 * Copyright 2013 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/burrow.h"
#include "burrow/context.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/mem/arena.h"
#include "burrow/net.h"
#include "burrow/net/netip.h"
#include "burrow/os.h"
#include "burrow/path.h"
#include "burrow/proc.h"
#include "burrow/strings.h"
#include "burrow/sync.h"
#include "burrow/sync/atomic.h"
#include "burrow/testing.h"
#include "burrow/time.h"

#include "../src/net/internal.h"
#include "check.h"

#include <stdint.h>
#include <string.h>

static const Byte test_addr[4] = {0xc0, 0x00, 0x02, 0x01};
static const Byte test_addr6[16] = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0,
                                    0,    0,    0,    0,    0, 0, 0, 1};

/* maxDNSPacketSize */
enum { MAX_DNS_PACKET_SIZE = 1232 };

/* Go's dnsclient_unix_test.go is for Unix only, where the resolver reads
 * resolv.conf. */
static bool unix_only(TestingT *t) {
    if (strcmp(BURROW_OS_NAME, "windows") == 0 ||
        strcmp(BURROW_OS_NAME, "wasip1") == 0) {
        testing_t_skipf_v(t, "not a Unix system");
        return false;
    }
    return true;
}

static DnsmsgName nm(const char *s) {
    return burrow__dnsmsg_must_new_name(str_from_cstr(s));
}

static DnsmsgQuestion must_question(const char *name, DnsmsgType type,
                                    DnsmsgClass class_) {
    DnsmsgQuestion q;
    memset(&q, 0, sizeof q);
    q.name = nm(name);
    q.type = type;
    q.class_ = class_;
    return q;
}

static Time zero_time(void) {
    Time z;
    memset(&z, 0, sizeof z);
    return z;
}

/* ------------------------------------------------------- the fake server */

typedef struct FakeDNSServer FakeDNSServer;

/* rh: what the server at s says to q over network n, made in a. deadline is
 * the one the resolver set on the connection. */
typedef Error (*FakeDNSHandler)(FakeDNSServer *srv, Alloc *a, Str n, Str s,
                                const DnsmsgMessage *q, Time deadline,
                                DnsmsgMessage *resp);

struct FakeDNSServer {
    FakeDNSHandler rh;
    void *env;
    bool always_tcp;
};

/* fakeDNSConn, and fakeDNSPacketConn when tcp is false. */
typedef struct FakeDNSConn {
    FakeDNSServer *server;
    Alloc *a;
    Byte *buf;
    Int nbuf;
    Str n;
    Str s;
    DnsmsgMessage q;
    Time t;
    bool tcp;
} FakeDNSConn;

static const Type fake_conn_type = {
    {(const Byte *)"fakeDNSConn", 11},
    {(const Byte *)"net_resolver_test", 17},
    KIND_STRUCT,
    0,
    1,
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x66646e73U,
    NULL,
};

static Int fake_read(void *self, Slice b, Error *err) {
    FakeDNSConn *f = (FakeDNSConn *)self;
    if (f->nbuf > 0) {
        Int n = b.len < f->nbuf ? b.len : f->nbuf;
        memcpy(b.p, f->buf, (size_t)n);
        f->buf += n;
        f->nbuf -= n;
        return n;
    }

    DnsmsgMessage resp;
    memset(&resp, 0, sizeof resp);
    Error e = f->server->rh(f->server, f->a, f->n, f->s, &f->q, f->t, &resp);
    if (BURROW_FAILED(e)) {
        *err = e;
        return 0;
    }

    Byte *bb = (Byte *)mem_alloc_nozero(f->a, 514, 1);
    if (bb == NULL) {
        *err = burrow_err_out_of_memory;
        return 0;
    }
    Slice out;
    e = burrow__dnsmsg_message_append_pack(&resp, f->a,
                                           slice_from(bb, 2, 514, TYPE_BYTE), &out);
    if (BURROW_FAILED(e)) {
        *err = fmt_errorf_v("cannot marshal DNS message: %v", e);
        return 0;
    }
    Byte *p = (Byte *)out.p;
    Int l = out.len - 2;

    if (f->tcp) {
        p[0] = (Byte)(l >> 8);
        p[1] = (Byte)l;
        f->buf = p;
        f->nbuf = out.len;
        return fake_read(self, b, err);
    }

    if (b.len < l) {
        *err = fmt_errorf_v("read would fragment DNS message");
        return 0;
    }
    memcpy(b.p, p + 2, (size_t)l);
    return l;
}

static Int fake_write(void *self, Slice b, Error *err) {
    FakeDNSConn *f = (FakeDNSConn *)self;
    if (f->tcp && b.len >= 2)
        b = slice_from((Byte *)b.p + 2, b.len - 2, b.len - 2, TYPE_BYTE);
    memset(&f->q, 0, sizeof f->q);
    if (BURROW_FAILED(burrow__dnsmsg_message_unpack(&f->q, f->a, b))) {
        *err = fmt_errorf_v("cannot unmarshal DNS message fake %s (%d)", f->n, b.len);
        return 0;
    }
    return b.len;
}

static Error fake_close(void *self) {
    (void)self;
    return BURROW_NO_ERROR;
}

static NetAddr fake_addr(void *self) {
    (void)self;
    NetAddr none = {NULL, NULL};
    return none;
}

static Error fake_set_deadline(void *self, Time t) {
    ((FakeDNSConn *)self)->t = t;
    return BURROW_NO_ERROR;
}

static Error fake_set_rw_deadline(void *self, Time t) {
    (void)self;
    (void)t;
    return BURROW_NO_ERROR;
}

static const NetConnVT fake_conn_vt = {
    .reader = {&fake_conn_type, fake_read},
    .writer = {&fake_conn_type, fake_write},
    .closer = {&fake_conn_type, fake_close},
    .local_addr = fake_addr,
    .remote_addr = fake_addr,
    .set_deadline = fake_set_deadline,
    .set_read_deadline = fake_set_rw_deadline,
    .set_write_deadline = fake_set_rw_deadline,
};

/* DialContext */
static NetConn fake_dial(void *env, Alloc *a, Context ctx, Str n, Str s, bool *packet,
                         Error *err) {
    (void)ctx;
    FakeDNSServer *server = (FakeDNSServer *)env;
    NetConn c = {NULL, NULL};
    FakeDNSConn *f = (FakeDNSConn *)mem_alloc(a, sizeof *f, _Alignof(FakeDNSConn));
    if (f == NULL) {
        *err = burrow_err_out_of_memory;
        return c;
    }
    f->server = server;
    f->a = a;
    f->n = str_clone(a, n);
    f->s = str_clone(a, s);
    f->tcp = server->always_tcp || str_eq(n, BURROW_S("tcp")) ||
             str_eq(n, BURROW_S("tcp4")) || str_eq(n, BURROW_S("tcp6"));
    *packet = !f->tcp;
    c.vt = &fake_conn_vt;
    c.data = f;
    return c;
}

static NetResolver fake_resolver(FakeDNSServer *server, bool strict) {
    NetResolver r;
    memset(&r, 0, sizeof r);
    r.prefer_go = true;
    r.strict_errors = strict;
    r.dial = BURROW_FN(NetResolverDial, fake_dial, server);
    return r;
}

/* ------------------------------------------------------------ the messages */

/* The header and questions every answer starts with. */
static void resp_init(DnsmsgMessage *r, const DnsmsgMessage *q) {
    memset(r, 0, sizeof *r);
    r->header.id = q->header.id;
    r->header.response = true;
    r->questions = q->questions;
}

static Str qname(const DnsmsgMessage *q) {
    if (q->questions.len == 0)
        return BURROW_STR_EMPTY;
    return burrow__dnsmsg_name_string(&q->questions.p[0].name);
}

static DnsmsgType qtype(const DnsmsgMessage *q) {
    return q->questions.len == 0 ? (DnsmsgType)0 : q->questions.p[0].type;
}

static bool qname_is(const DnsmsgMessage *q, const char *name) {
    return str_eq(qname(q), str_from_cstr(name));
}

/* append, for a section of a message being made in a. Out of memory drops
 * the record, and the test that wanted it fails. */
static void add_rr(Alloc *a, DnsmsgResources *rs, const DnsmsgResource *rr) {
    if (rs->len == rs->cap) {
        Int cap = rs->cap == 0 ? 4 : rs->cap * 2;
        DnsmsgResource *p = (DnsmsgResource *)mem_alloc(
            a, sizeof(DnsmsgResource) * (size_t)cap, _Alignof(DnsmsgResource));
        if (p == NULL)
            return;
        if (rs->len > 0)
            memcpy(p, rs->p, sizeof(DnsmsgResource) * (size_t)rs->len);
        rs->p = p;
        rs->cap = cap;
    }
    rs->p[rs->len++] = *rr;
}

static DnsmsgResource rr_of(DnsmsgName name, DnsmsgType type, uint16_t length) {
    DnsmsgResource r;
    memset(&r, 0, sizeof r);
    r.header.name = name;
    r.header.type = type;
    r.header.class_ = DNSMSG_CLASS_INET;
    r.header.length = length;
    return r;
}

static DnsmsgResource rr_a(DnsmsgName name) {
    DnsmsgResource r = rr_of(name, DNSMSG_TYPE_A, 4);
    r.body.kind = DNSMSG_BODY_A;
    memcpy(r.body.u.a.a, test_addr, sizeof test_addr);
    return r;
}

static DnsmsgResource rr_aaaa(DnsmsgName name) {
    DnsmsgResource r = rr_of(name, DNSMSG_TYPE_AAAA, 16);
    r.body.kind = DNSMSG_BODY_AAAA;
    memcpy(r.body.u.aaaa.aaaa, test_addr6, sizeof test_addr6);
    return r;
}

/* A TXT record with the strings in txt, which it borrows. */
static DnsmsgResource rr_txt(DnsmsgName name, DnsmsgType type, Str *txt, Int n) {
    DnsmsgResource r = rr_of(name, type, 0);
    r.body.kind = DNSMSG_BODY_TXT;
    r.body.u.txt.txt.p = txt;
    r.body.u.txt.txt.len = n;
    r.body.u.txt.txt.cap = n;
    return r;
}

static DnsmsgResource rr_mx(DnsmsgName name, uint16_t length, const char *mx) {
    DnsmsgResource r = rr_of(name, DNSMSG_TYPE_MX, length);
    r.body.kind = DNSMSG_BODY_MX;
    r.body.u.mx.mx = nm(mx);
    return r;
}

static DnsmsgResource rr_ns(DnsmsgName name, uint16_t length, const char *ns) {
    DnsmsgResource r = rr_of(name, DNSMSG_TYPE_NS, length);
    r.body.kind = DNSMSG_BODY_NS;
    r.body.u.ns.ns = nm(ns);
    return r;
}

static DnsmsgResource rr_ptr(DnsmsgName name, uint16_t length, const char *ptr) {
    DnsmsgResource r = rr_of(name, DNSMSG_TYPE_PTR, length);
    r.body.kind = DNSMSG_BODY_PTR;
    r.body.u.ptr.ptr = nm(ptr);
    return r;
}

static DnsmsgResource rr_srv(DnsmsgName name, uint16_t length, const char *target) {
    DnsmsgResource r = rr_of(name, DNSMSG_TYPE_SRV, length);
    r.body.kind = DNSMSG_BODY_SRV;
    r.body.u.srv.target = nm(target);
    return r;
}

static DnsmsgResource rr_cname(DnsmsgName name, const char *cname) {
    DnsmsgResource r = rr_of(name, DNSMSG_TYPE_CNAME, 0);
    r.body.kind = DNSMSG_BODY_CNAME;
    r.body.u.cname.cname = nm(cname);
    return r;
}

/* An OPT record from SetEDNS0. */
static DnsmsgResource rr_edns0(DnsmsgRCode ext_rcode) {
    DnsmsgResource r;
    memset(&r, 0, sizeof r);
    (void)burrow__dnsmsg_resource_header_set_edns0(&r.header, MAX_DNS_PACKET_SIZE,
                                                   ext_rcode, false);
    r.body.kind = DNSMSG_BODY_OPT;
    return r;
}

/* fakeDNSServerSuccessful */
static Error successful_rh(FakeDNSServer *srv, Alloc *a, Str n, Str s,
                           const DnsmsgMessage *q, Time deadline, DnsmsgMessage *r) {
    (void)srv;
    (void)n;
    (void)s;
    (void)deadline;
    resp_init(r, q);
    if (q->questions.len == 1 && q->questions.p[0].type == DNSMSG_TYPE_A) {
        DnsmsgResource rr = rr_a(q->questions.p[0].name);
        add_rr(a, &r->answers, &rr);
    }
    return BURROW_NO_ERROR;
}

static FakeDNSServer fake_dns_server_successful = {successful_rh, NULL, false};

/* mockTXTResponse */
static Str mock_txt_ok = BURROW_S_INIT("ok");

static void mock_txt_response(Alloc *a, const DnsmsgMessage *q, DnsmsgMessage *r) {
    resp_init(r, q);
    r->header.recursion_available = true;
    DnsmsgResource rr =
        rr_txt(q->questions.p[0].name, DNSMSG_TYPE_TXT, &mock_txt_ok, 1);
    add_rr(a, &r->answers, &rr);
}

/* ------------------------------------------------------- resolv.conf */

/* resolvConfTest */
typedef struct ResolvConfTest {
    Arena ar;
    Str dir;
    Str path;
} ResolvConfTest;

static bool rct_new(TestingT *t, ResolvConfTest *c) {
    arena_init(&c->ar, NULL, 0);
    Alloc *a = arena_allocator(&c->ar);
    Error err = BURROW_NO_ERROR;
    c->dir = os_mkdir_temp(a, BURROW_STR_EMPTY, BURROW_S("go-resolvconftest"), &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "MkdirTemp: %v", err);
        arena_free(&c->ar);
        return false;
    }
    c->path = path_join_v(a, 2, c->dir, BURROW_S("resolv.conf"));
    return true;
}

static bool rct_write(TestingT *t, ResolvConfTest *c, const char *const *lines, Int n) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str *ls = (Str *)mem_alloc(a, sizeof(Str) * (size_t)(n > 0 ? n : 1), _Alignof(Str));
    if (ls == NULL) {
        testing_t_errorf_v(t, "out of memory");
        arena_free(&ar);
        return false;
    }
    for (Int i = 0; i < n; i++)
        ls[i] = str_from_cstr(lines[i]);
    Str data = strings_join(a, slice_from(ls, n, n, TYPE_STRING), BURROW_S("\n"));
    Error err = os_write_file(
        c->path, slice_from((void *)(uintptr_t)data.p, data.len, data.len, TYPE_BYTE),
        0600);
    arena_free(&ar);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "WriteFile: %v", err);
        return false;
    }
    return true;
}

static bool rct_write_and_update_at(TestingT *t, ResolvConfTest *c,
                                    const char *const *lines, Int n,
                                    Time last_checked) {
    if (!rct_write(t, c, lines, n))
        return false;
    burrow__net_force_dns_config_file(c->path, last_checked);
    return true;
}

static bool rct_write_and_update(TestingT *t, ResolvConfTest *c,
                                 const char *const *lines, Int n) {
    return rct_write_and_update_at(t, c, lines, n, burrow__net_distant_future());
}

static void rct_teardown(ResolvConfTest *c) {
    burrow__net_force_dns_config_file(BURROW_S("/etc/resolv.conf"), zero_time());
    (void)os_remove_all(c->dir);
    arena_free(&c->ar);
}

#define LINES(...) ((const char *const[]){__VA_ARGS__})
#define NLINES(...) ((Int)(sizeof LINES(__VA_ARGS__) / sizeof(const char *)))
#define UPDATE(t, c, ...)                                                              \
    rct_write_and_update((t), (c), LINES(__VA_ARGS__), NLINES(__VA_ARGS__))

/* A file in a temporary directory, for the hosts file. */
static Str write_temp(TestingT *t, Alloc *a, const char *name, const char *text) {
    Error err = BURROW_NO_ERROR;
    Str dir = os_mkdir_temp(a, BURROW_STR_EMPTY, BURROW_S("net-resolver"), &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "MkdirTemp: %v", err);
        return BURROW_STR_EMPTY;
    }
    Str path = path_join_v(a, 2, dir, str_from_cstr(name));
    Str data = str_from_cstr(text);
    err = os_write_file(
        path, slice_from((void *)(uintptr_t)data.p, data.len, data.len, TYPE_BYTE),
        0600);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "WriteFile: %v", err);
        return BURROW_STR_EMPTY;
    }
    return path;
}

/* Go's testdata/hosts and testdata/aliases. */
static const char testdata_hosts[] = "255.255.255.255\tbroadcasthost\n"
                                     "127.0.0.2\todin\n"
                                     "127.0.0.3\todin  # inline comment \n"
                                     "::2             odin\n"
                                     "127.1.1.1\tthor\n"
                                     "# aliases\n"
                                     "127.1.1.2\tullr ullrhost\n"
                                     "fe80::1%lo0\tlocalhost\n"
                                     "# Bogus entries that must be ignored.\n"
                                     "123.123.123\tloki\n"
                                     "321.321.321.321\n";

static const char testdata_aliases[] =
    "127.0.0.1 test\n"
    "127.0.0.2 test2.example.com 2.test\n"
    "127.0.0.3 3.test test3.example.com\n"
    "127.0.0.4 example.com\n"
    "127.0.0.5 test4.example.com 4.test 5.test test5.example.com\n"
    "\n"
    "# must be a non resolvable domain on the internet\n"
    "127.0.1.1 invalid.test invalid.invalid\n";

static void remove_temp(Str path) {
    if (path.len > 0) {
        Arena ar;
        arena_init(&ar, NULL, 0);
        (void)os_remove_all(path_dir(arena_allocator(&ar), path));
        arena_free(&ar);
    }
}

/* ------------------------------------------------------------ DNS errors */

/* The DNSError a test wants, all of whose fields have to match, with no
 * error underneath. */
typedef struct WantDNSError {
    Str err;
    const char *name;
    const char *server;
    bool is_timeout;
    bool is_temporary;
    bool is_not_found;
} WantDNSError;

static bool dns_error_equal(Error err, const WantDNSError *w) {
    const NetDNSError *d = (const NetDNSError *)errors_as(err, TYPE_NET_DNS_ERROR);
    return d != NULL && str_eq(d->err, w->err) &&
           str_eq(d->name, str_from_cstr(w->name)) &&
           str_eq(d->server, str_from_cstr(w->server)) &&
           d->is_timeout == w->is_timeout && d->is_temporary == w->is_temporary &&
           d->is_not_found == w->is_not_found && BURROW_OK(d->unwrap_err);
}

static Str err_no_such_host(void) {
    return error_text(burrow__net_err_no_such_host);
}

/* ------------------------------------------------------------ exchange */

typedef struct TransportFallbackTest {
    const char *server;
    const char *qname;
    DnsmsgType qtype;
    int timeout;
    DnsmsgRCode rcode;
} TransportFallbackTest;

/* Querying "com." with qtype=255 usually makes an answer which requires more
 * than 512 bytes. */
static const TransportFallbackTest dns_transport_fallback_tests[] = {
    {"8.8.8.8:53", "com.", DNSMSG_TYPE_ALL, 2, DNSMSG_RCODE_SUCCESS},
    {"8.8.4.4:53", "com.", DNSMSG_TYPE_ALL, 4, DNSMSG_RCODE_SUCCESS},
};

static Error transport_fallback_rh(FakeDNSServer *srv, Alloc *a, Str n, Str s,
                                   const DnsmsgMessage *q, Time deadline,
                                   DnsmsgMessage *r) {
    (void)srv;
    (void)a;
    (void)s;
    (void)deadline;
    resp_init(r, q);
    r->header.rcode = DNSMSG_RCODE_SUCCESS;
    if (str_eq(n, BURROW_S("udp")))
        r->header.truncated = true;
    return BURROW_NO_ERROR;
}

static Error exchange(NetResolver *r, Alloc *a, Str server, DnsmsgQuestion q,
                      Duration timeout, bool use_tcp, DnsmsgParser *p,
                      DnsmsgHeader *h) {
    return burrow__net_dns_exchange(r, a, context_background(), server, q, timeout,
                                    use_tcp, false, p, h);
}

static void TestDNSTransportFallback(TestingT *t) {
    FakeDNSServer fake = {transport_fallback_rh, NULL, false};
    NetResolver r = fake_resolver(&fake, false);
    for (size_t i = 0; i < sizeof dns_transport_fallback_tests /
                               sizeof dns_transport_fallback_tests[0];
         i++) {
        const TransportFallbackTest *tt = &dns_transport_fallback_tests[i];
        Arena ar;
        arena_init(&ar, NULL, 0);
        DnsmsgParser p;
        DnsmsgHeader h;
        Error err = exchange(&r, arena_allocator(&ar), str_from_cstr(tt->server),
                             must_question(tt->qname, tt->qtype, DNSMSG_CLASS_INET),
                             TIME_SECOND, false, &p, &h);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "%v", err);
        else if (h.rcode != tt->rcode)
            testing_t_errorf_v(t, "got %d from %s; want %d", (int)h.rcode, tt->server,
                               (int)tt->rcode);
        arena_free(&ar);
    }
}

static Error no_fallback_on_tcp_rh(FakeDNSServer *srv, Alloc *a, Str n, Str s,
                                   const DnsmsgMessage *q, Time deadline,
                                   DnsmsgMessage *r) {
    (void)srv;
    (void)s;
    (void)deadline;
    resp_init(r, q);
    r->header.rcode = DNSMSG_RCODE_SUCCESS;
    r->header.truncated = true;
    if (str_eq(n, BURROW_S("tcp"))) {
        DnsmsgResource rr = rr_a(q->questions.p[0].name);
        add_rr(a, &r->answers, &rr);
    }
    return BURROW_NO_ERROR;
}

static void TestDNSTransportNoFallbackOnTCP(TestingT *t) {
    FakeDNSServer fake = {no_fallback_on_tcp_rh, NULL, false};
    NetResolver r = fake_resolver(&fake, false);
    for (size_t i = 0; i < sizeof dns_transport_fallback_tests /
                               sizeof dns_transport_fallback_tests[0];
         i++) {
        const TransportFallbackTest *tt = &dns_transport_fallback_tests[i];
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        DnsmsgParser p;
        DnsmsgHeader h;
        Error err = exchange(&r, a, str_from_cstr(tt->server),
                             must_question(tt->qname, tt->qtype, DNSMSG_CLASS_INET),
                             TIME_SECOND, false, &p, &h);
        DnsmsgResources as;
        memset(&as, 0, sizeof as);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%v", err);
        } else if (h.rcode != tt->rcode) {
            testing_t_errorf_v(t, "got %d from %s; want %d", (int)h.rcode, tt->server,
                               (int)tt->rcode);
        } else {
            err = burrow__dnsmsg_parser_all_answers(&p, a, &as);
            if (BURROW_FAILED(err))
                testing_t_errorf_v(t, "unexpected error %v getting all answers from %s",
                                   err, tt->server);
            else if (as.len != 1)
                testing_t_errorf_v(t, "got %d answers from %s; want 1", as.len,
                                   tt->server);
        }
        arena_free(&ar);
    }
}

/* See RFC 6761 for further information about the reserved, pseudo domain
 * names. */
static const struct {
    const char *name;
    DnsmsgType type;
    DnsmsgRCode rcode;
} special_domain_name_tests[] = {
    /* Name resolution APIs and libraries should not recognize the followings
     * as special. */
    {"1.0.168.192.in-addr.arpa.", DNSMSG_TYPE_PTR, DNSMSG_RCODE_NAME_ERROR},
    {"test.", DNSMSG_TYPE_ALL, DNSMSG_RCODE_NAME_ERROR},
    {"example.com.", DNSMSG_TYPE_ALL, DNSMSG_RCODE_SUCCESS},

    /* Name resolution APIs and libraries should recognize the followings as
     * special and should not send any queries. Though, we test those names
     * here for verifying negative answers at DNS query-response interaction
     * level. */
    {"localhost.", DNSMSG_TYPE_ALL, DNSMSG_RCODE_NAME_ERROR},
    {"invalid.", DNSMSG_TYPE_ALL, DNSMSG_RCODE_NAME_ERROR},
};

static Error special_domain_name_rh(FakeDNSServer *srv, Alloc *a, Str n, Str s,
                                    const DnsmsgMessage *q, Time deadline,
                                    DnsmsgMessage *r) {
    (void)srv;
    (void)a;
    (void)n;
    (void)s;
    (void)deadline;
    resp_init(r, q);
    r->header.rcode =
        qname_is(q, "example.com.") ? DNSMSG_RCODE_SUCCESS : DNSMSG_RCODE_NAME_ERROR;
    return BURROW_NO_ERROR;
}

static void TestSpecialDomainName(TestingT *t) {
    FakeDNSServer fake = {special_domain_name_rh, NULL, false};
    NetResolver r = fake_resolver(&fake, false);
    const char *server = "8.8.8.8:53";
    for (size_t i = 0;
         i < sizeof special_domain_name_tests / sizeof special_domain_name_tests[0];
         i++) {
        Arena ar;
        arena_init(&ar, NULL, 0);
        DnsmsgParser p;
        DnsmsgHeader h;
        Error err = exchange(&r, arena_allocator(&ar), str_from_cstr(server),
                             must_question(special_domain_name_tests[i].name,
                                           special_domain_name_tests[i].type,
                                           DNSMSG_CLASS_INET),
                             3 * TIME_SECOND, false, &p, &h);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "%v", err);
        else if (h.rcode != special_domain_name_tests[i].rcode)
            testing_t_errorf_v(t, "got %d from %s; want %d", (int)h.rcode, server,
                               (int)special_domain_name_tests[i].rcode);
        arena_free(&ar);
    }
}

/* Issue 13705: don't try to resolve onion addresses, etc */
static void TestAvoidDNSName(TestingT *t) {
    static const struct {
        const char *name;
        bool avoid;
    } tests[] = {
        {"foo.com", false},
        {"foo.com.", false},

        {"foo.onion.", true},
        {"foo.onion", true},
        {"foo.ONION", true},
        {"foo.ONION.", true},

        /* But do resolve *.local address; Issue 16739 */
        {"foo.local.", false},
        {"foo.local", false},
        {"foo.LOCAL", false},
        {"foo.LOCAL.", false},

        {"", true}, /* will be rejected earlier too */

        /* Without stuff before onion/local, they're fine to use DNS. With a
         * search path, "onion.vegetables.com" can use DNS. Without a search
         * path (or with a trailing dot), the queries are just kinda useless,
         * but don't reveal anything private. */
        {"local", false},
        {"onion", false},
        {"local.", false},
        {"onion.", false},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        bool got = burrow__net_avoid_dns(str_from_cstr(tests[i].name));
        if (got != tests[i].avoid)
            testing_t_errorf_v(t, "avoidDNS(%q) = %t; want %t",
                               str_from_cstr(tests[i].name), got, tests[i].avoid);
    }
}

static bool strs_equal(Slice got, const char *const *want, Int n) {
    if (got.len != n)
        return false;
    for (Int i = 0; i < n; i++) {
        if (!str_eq(((const Str *)got.p)[i], str_from_cstr(want[i])))
            return false;
    }
    return true;
}

static void TestNameListAvoidDNS(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str search[] = {BURROW_S_INIT("go.dev."), BURROW_S_INIT("onion.")};
    burrow__DNSConfig c;
    memset(&c, 0, sizeof c);
    c.search = slice_from(search, 2, 2, TYPE_STRING);

    Slice got = burrow__dns_config_name_list(&c, a, BURROW_S("www"));
    static const char *const want1[] = {"www.", "www.go.dev."};
    if (!strs_equal(got, want1, 2)) {
        testing_t_fatalf_v(
            t, "nameList(\"www\") = %d names, want \"www.\", \"www.go.dev.\"", got.len);
        arena_free(&ar);
        return;
    }

    got = burrow__dns_config_name_list(&c, a, BURROW_S("www.onion"));
    static const char *const want2[] = {"www.onion.go.dev."};
    if (!strs_equal(got, want2, 1))
        testing_t_fatalf_v(
            t, "nameList(\"www.onion\") = %d names, want \"www.onion.go.dev.\"",
            got.len);
    arena_free(&ar);
}

/* Issue 13705: don't try to resolve onion addresses, etc */
static void TestLookupTorOnion(TestingT *t) {
    if (!unix_only(t))
        return;
    NetResolver r = fake_resolver(&fake_dns_server_successful, false);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Error err = BURROW_NO_ERROR;
    Slice addrs = net_resolver_lookup_ip_addr(
        &r, arena_allocator(&ar), context_background(), BURROW_S("foo.onion."), &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "lookup = %v; want nil", err);
        arena_free(&ar);
        return;
    }
    if (addrs.len > 0)
        testing_t_errorf_v(t, "unexpected addresses: %d of them", addrs.len);
    arena_free(&ar);
}

/* ------------------------------------------------- reading resolv.conf */

typedef struct UpdateResolvConfTest {
    const char *name;     /* query name */
    const char *lines[1]; /* resolver configuration lines */
    Int nlines;
    const char *servers[2]; /* expected name servers, NULL for defaultNS */
    Int nservers;
} UpdateResolvConfTest;

static const UpdateResolvConfTest update_resolv_conf_tests[] = {
    {"golang.org", {"nameserver 8.8.8.8"}, 1, {"8.8.8.8:53"}, 1},
    /* An empty resolv.conf should use defaultNS as name servers. */
    {"", {NULL}, 0, {NULL}, 0},
    {"www.example.com", {"nameserver 8.8.4.4"}, 1, {"8.8.4.4:53"}, 1},
};

typedef struct LookupJob {
    NetResolver *r;
    Str name;
    Arena ar;
    Str err; /* the error's text, in ar */
    Int n;
    bool failed;
} LookupJob;

static void lookup_job(void *env) {
    LookupJob *j = (LookupJob *)env;
    Alloc *a = arena_allocator(&j->ar);
    Error err = BURROW_NO_ERROR;
    Slice ips =
        net_resolver_lookup_ip_addr(j->r, a, context_background(), j->name, &err);
    j->n = ips.len;
    if (BURROW_FAILED(err)) {
        j->failed = true;
        j->err = str_clone(a, error_text(err));
    }
}

static void TestUpdateResolvConf(TestingT *t) {
    if (!unix_only(t))
        return;
    NetResolver r = fake_resolver(&fake_dns_server_successful, false);

    ResolvConfTest conf;
    if (!rct_new(t, &conf))
        return;

    for (size_t i = 0;
         i < sizeof update_resolv_conf_tests / sizeof update_resolv_conf_tests[0];
         i++) {
        const UpdateResolvConfTest *tt = &update_resolv_conf_tests[i];
        if (!rct_write_and_update(t, &conf, tt->lines, tt->nlines))
            continue;
        if (tt->name[0] != '\0') {
            enum { N = 10 };
            LookupJob jobs[N];
            SyncWaitGroup wg;
            memset(&wg, 0, sizeof wg);
            memset(jobs, 0, sizeof jobs);
            for (int j = 0; j < N; j++) {
                jobs[j].r = &r;
                jobs[j].name = str_from_cstr(tt->name);
                arena_init(&jobs[j].ar, NULL, 0);
                sync_wait_group_go(&wg, BURROW_FN(Func, lookup_job, &jobs[j]));
            }
            sync_wait_group_wait(&wg);
            for (int j = 0; j < N; j++) {
                if (jobs[j].failed)
                    testing_t_errorf_v(t, "%s", jobs[j].err);
                else if (jobs[j].n == 0)
                    testing_t_errorf_v(t, "no records for %s", jobs[j].name);
                arena_free(&jobs[j].ar);
            }
        }
        burrow__DNSConfig *c = burrow__net_system_dns_config();
        bool ok = tt->nservers == 0 ? burrow__dns_config_is_default_ns(c)
                                    : strs_equal(c->servers, tt->servers, tt->nservers);
        if (!ok)
            testing_t_errorf_v(t, "#%d: got %d servers; want %s", (int)i,
                               c->servers.len,
                               tt->nservers == 0 ? "defaultNS" : tt->servers[0]);
        burrow__dns_config_put(c);
    }
    rct_teardown(&conf);
}

typedef struct ResolverConfigTest {
    const char *name;
    const char *lines[3]; /* resolver configuration lines */
    Int nlines;
    const char *err_server;
    bool has_error;
    bool err_timeout;
    bool a, aaaa; /* whether response contains A, AAAA-record */
} ResolverConfigTest;

static const ResolverConfigTest go_lookup_ip_with_resolver_config_tests[] = {
    /* no records, transport timeout */
    {"jgahvsekduiv9bw4b3qhn4ykdfgj0493iohkrjfhdvhjiu4j",
     /* please forgive us for abuse of limited broadcast address */
     {"options timeout:1 attempts:1", "nameserver 255.255.255.255"},
     2,
     "255.255.255.255:53",
     true,
     true,
     false,
     false},

    /* no records, non-existent domain */
    {"jgahvsekduiv9bw4b3qhn4ykdfgj0493iohkrjfhdvhjiu4j",
     {"options timeout:3 attempts:1", "nameserver 8.8.8.8"},
     2,
     "8.8.8.8:53",
     true,
     false,
     false,
     false},

    /* a few A records, no AAAA records */
    {"ipv4.google.com.",
     {"nameserver 8.8.8.8", "nameserver 2001:4860:4860::8888"},
     2,
     NULL,
     false,
     false,
     true,
     false},
    {"ipv4.google.com",
     {"domain golang.org", "nameserver 2001:4860:4860::8888", "nameserver 8.8.8.8"},
     3,
     NULL,
     false,
     false,
     true,
     false},
    {"ipv4.google.com",
     {"search x.golang.org y.golang.org", "nameserver 2001:4860:4860::8888",
      "nameserver 8.8.8.8"},
     3,
     NULL,
     false,
     false,
     true,
     false},

    /* no A records, a few AAAA records */
    {"ipv6.google.com.",
     {"nameserver 2001:4860:4860::8888", "nameserver 8.8.8.8"},
     2,
     NULL,
     false,
     false,
     false,
     true},
    {"ipv6.google.com",
     {"domain golang.org", "nameserver 8.8.8.8", "nameserver 2001:4860:4860::8888"},
     3,
     NULL,
     false,
     false,
     false,
     true},
    {"ipv6.google.com",
     {"search x.golang.org y.golang.org", "nameserver 8.8.8.8",
      "nameserver 2001:4860:4860::8888"},
     3,
     NULL,
     false,
     false,
     false,
     true},

    /* both A and AAAA records */
    {"hostname.as112.net", /* see RFC 7534 */
     {"domain golang.org", "nameserver 2001:4860:4860::8888", "nameserver 8.8.8.8"},
     3,
     NULL,
     false,
     false,
     true,
     true},
    {"hostname.as112.net", /* see RFC 7534 */
     {"search x.golang.org y.golang.org", "nameserver 2001:4860:4860::8888",
      "nameserver 8.8.8.8"},
     3,
     NULL,
     false,
     false,
     true,
     true},
};

static Error resolver_config_rh(FakeDNSServer *srv, Alloc *a, Str n, Str s,
                                const DnsmsgMessage *q, Time deadline,
                                DnsmsgMessage *r) {
    (void)srv;
    (void)n;
    (void)deadline;
    if (!str_eq(s, BURROW_S("[2001:4860:4860::8888]:53")) &&
        !str_eq(s, BURROW_S("8.8.8.8:53"))) {
        time_sleep(10 * TIME_MILLISECOND);
        return os_err_deadline_exceeded;
    }
    resp_init(r, q);
    for (Int i = 0; i < q->questions.len; i++) {
        const DnsmsgQuestion *question = &q->questions.p[i];
        Str name = burrow__dnsmsg_name_string(&question->name);
        if (question->type == DNSMSG_TYPE_A &&
            str_eq(name, BURROW_S("ipv4.google.com."))) {
            DnsmsgResource rr = rr_a(q->questions.p[0].name);
            add_rr(a, &r->answers, &rr);
        } else if (question->type == DNSMSG_TYPE_AAAA &&
                   str_eq(name, BURROW_S("ipv6.google.com."))) {
            DnsmsgResource rr = rr_aaaa(q->questions.p[0].name);
            add_rr(a, &r->answers, &rr);
        }
    }
    return BURROW_NO_ERROR;
}

static void TestGoLookupIPWithResolverConfig(TestingT *t) {
    if (!unix_only(t))
        return;
    FakeDNSServer fake = {resolver_config_rh, NULL, false};
    NetResolver r = fake_resolver(&fake, false);

    ResolvConfTest conf;
    if (!rct_new(t, &conf))
        return;

    for (size_t i = 0; i < sizeof go_lookup_ip_with_resolver_config_tests /
                               sizeof go_lookup_ip_with_resolver_config_tests[0];
         i++) {
        const ResolverConfigTest *tt = &go_lookup_ip_with_resolver_config_tests[i];
        if (!rct_write_and_update(t, &conf, tt->lines, tt->nlines))
            continue;
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        Error err = BURROW_NO_ERROR;
        Slice addrs = net_resolver_lookup_ip_addr(&r, a, context_background(),
                                                  str_from_cstr(tt->name), &err);
        if (BURROW_FAILED(err)) {
            const NetDNSError *d =
                (const NetDNSError *)errors_as(err, TYPE_NET_DNS_ERROR);
            if (d == NULL ||
                (tt->has_error && (!str_eq(d->name, str_from_cstr(tt->name)) ||
                                   !str_eq(d->server, str_from_cstr(tt->err_server)) ||
                                   d->is_timeout != tt->err_timeout)))
                testing_t_errorf_v(t, "got %v; want a DNSError for %s from %s", err,
                                   str_from_cstr(tt->name),
                                   str_from_cstr(tt->has_error ? tt->err_server : ""));
            arena_free(&ar);
            continue;
        }
        if (addrs.len == 0)
            testing_t_errorf_v(t, "no records for %s", str_from_cstr(tt->name));
        if (!tt->a && !tt->aaaa && addrs.len > 0)
            testing_t_errorf_v(t, "unexpected %d addresses for %s", addrs.len,
                               str_from_cstr(tt->name));
        for (Int j = 0; j < addrs.len; j++) {
            const NetIPAddr *addr = &((const NetIPAddr *)addrs.p)[j];
            bool v4 = net_ip_to4(addr->ip).len > 0;
            if (!tt->a && v4)
                testing_t_errorf_v(t, "got %s; must not be IPv4 address",
                                   net_ip_addr_string(addr, a));
            if (!tt->aaaa && net_ip_to16(addr->ip, a).len > 0 && !v4)
                testing_t_errorf_v(t, "got %s; must not be IPv6 address",
                                   net_ip_addr_string(addr, a));
        }
        arena_free(&ar);
    }
    rct_teardown(&conf);
}

static Error empty_rh(FakeDNSServer *srv, Alloc *a, Str n, Str s,
                      const DnsmsgMessage *q, Time deadline, DnsmsgMessage *r) {
    (void)srv;
    (void)a;
    (void)n;
    (void)s;
    (void)deadline;
    resp_init(r, q);
    return BURROW_NO_ERROR;
}

/* Test that goLookupIPOrder falls back to the host file when no DNS servers
 * are available. */
static void TestGoLookupIPOrderFallbackToFile(TestingT *t) {
    if (!unix_only(t))
        return;
    FakeDNSServer fake = {empty_rh, NULL, false};
    NetResolver r = fake_resolver(&fake, false);

    /* Add a config that simulates no dns servers being available. */
    ResolvConfTest conf;
    if (!rct_new(t, &conf))
        return;
    if (!rct_write_and_update(t, &conf, NULL, 0)) {
        rct_teardown(&conf);
        return;
    }
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    /* Redirect host file lookups. */
    Str hosts = write_temp(t, a, "hosts", testdata_hosts);
    burrow__net_set_hosts_file_path(hosts);

    static const burrow__HostLookupOrder orders[] = {BURROW__HOST_LOOKUP_FILES_DNS,
                                                     BURROW__HOST_LOOKUP_DNS_FILES};
    for (size_t i = 0; i < sizeof orders / sizeof orders[0]; i++) {
        Str name = fmt_sprintf_v(
            a, "order %s", burrow__host_lookup_order_string((int32_t)orders[i], a));
        /* First ensure that we get an error when contacting a non-existent
         * host. */
        Error err = BURROW_NO_ERROR;
        (void)burrow__net_go_lookup_ip_cname_order(
            &r, a, context_background(), BURROW_S("ip"), BURROW_S("notarealhost"),
            orders[i], NULL, NULL, &err);
        if (BURROW_OK(err)) {
            testing_t_errorf_v(
                t, "%s: expected error while looking up name not in hosts file", name);
            continue;
        }

        /* Now check that we get an address when the name appears in the hosts
         * file. */
        err = BURROW_NO_ERROR;
        Slice addrs = burrow__net_go_lookup_ip_cname_order(
            &r, a, context_background(), BURROW_S("ip"), BURROW_S("thor"), orders[i],
            NULL, NULL, &err); /* entry is in "testdata/hosts" */
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%s: expected to successfully lookup host entry",
                               name);
            continue;
        }
        if (addrs.len != 1) {
            testing_t_errorf_v(t, "%s: expected exactly one result, but got %d", name,
                               addrs.len);
            continue;
        }
        Str got = net_ip_addr_string((const NetIPAddr *)addrs.p, a);
        if (!str_eq(got, BURROW_S("127.1.1.1")))
            testing_t_errorf_v(t,
                               "%s: address doesn't match expectation. got %s, want %s",
                               name, got, BURROW_S("127.1.1.1"));
    }
    burrow__net_set_hosts_file_path(BURROW_STR_EMPTY);
    remove_temp(hosts);
    arena_free(&ar);
    rct_teardown(&conf);
}

#define FQDN "doesnotexist.domain"

static Error original_name_rh(FakeDNSServer *srv, Alloc *a, Str n, Str s,
                              const DnsmsgMessage *q, Time deadline, DnsmsgMessage *r) {
    (void)srv;
    (void)a;
    (void)n;
    (void)s;
    (void)deadline;
    resp_init(r, q);
    r->header.rcode = qname_is(q, FQDN ".servfail.") ? DNSMSG_RCODE_SERVER_FAILURE
                                                     : DNSMSG_RCODE_NAME_ERROR;
    return BURROW_NO_ERROR;
}

/* Issue 12712. When using search domains, return the error encountered
 * querying the original name instead of an error encountered querying a
 * generated name. */
static void TestErrorForOriginalNameWhenSearching(TestingT *t) {
    if (!unix_only(t))
        return;
    ResolvConfTest conf;
    if (!rct_new(t, &conf))
        return;
    if (!UPDATE(t, &conf, "search servfail")) {
        rct_teardown(&conf);
        return;
    }

    FakeDNSServer fake = {original_name_rh, NULL, false};

    struct {
        bool strict_errors;
        Str err;
        bool is_temporary;
    } cases[] = {
        {true, error_text(burrow__net_err_server_misbehaving), true},
        {false, err_no_such_host(), false},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        NetResolver r = fake_resolver(&fake, cases[i].strict_errors);
        Arena ar;
        arena_init(&ar, NULL, 0);
        Error err = BURROW_NO_ERROR;
        (void)net_resolver_lookup_ip_addr(&r, arena_allocator(&ar),
                                          context_background(), BURROW_S(FQDN), &err);
        arena_free(&ar);
        if (BURROW_OK(err)) {
            testing_t_fatalf_v(t, "expected an error");
            break;
        }
        const NetDNSError *d = (const NetDNSError *)errors_as(err, TYPE_NET_DNS_ERROR);
        if (d == NULL || !str_eq(d->name, BURROW_S(FQDN)) ||
            !str_eq(d->err, cases[i].err) || d->is_temporary != cases[i].is_temporary)
            testing_t_errorf_v(t, "got %v; want lookup %s: %s", err, BURROW_S(FQDN),
                               cases[i].err);
    }
    rct_teardown(&conf);
}

static Error lame_referral_rh(FakeDNSServer *srv, Alloc *a, Str n, Str s,
                              const DnsmsgMessage *q, Time deadline, DnsmsgMessage *r) {
    (void)srv;
    (void)n;
    (void)deadline;
    resp_init(r, q);
    if (str_eq(s, BURROW_S("192.0.2.2:53"))) {
        r->header.recursion_available = true;
        if (qtype(q) == DNSMSG_TYPE_A) {
            DnsmsgResource rr = rr_a(q->questions.p[0].name);
            add_rr(a, &r->answers, &rr);
        }
    } else if (str_eq(s, BURROW_S("192.0.2.1:53"))) {
        if (qtype(q) == DNSMSG_TYPE_A &&
            strings_has_prefix(qname(q), BURROW_S("empty.com."))) {
            DnsmsgResource rr = rr_edns0(DNSMSG_RCODE_SUCCESS);
            add_rr(a, &r->additionals, &rr);
        }
    }
    return BURROW_NO_ERROR;
}

/* Issue 15434. If a name server gives a lame referral, continue to the
 * next. */
static void TestIgnoreLameReferrals(TestingT *t) {
    if (!unix_only(t))
        return;
    ResolvConfTest conf;
    if (!rct_new(t, &conf))
        return;
    if (!UPDATE(t, &conf,
                "nameserver 192.0.2.1", /* the one that will give a lame referral */
                "nameserver 192.0.2.2")) {
        rct_teardown(&conf);
        return;
    }

    FakeDNSServer fake = {lame_referral_rh, NULL, false};
    NetResolver r = fake_resolver(&fake, false);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    Error err = BURROW_NO_ERROR;
    Slice addrs = net_resolver_lookup_ip(&r, a, context_background(), BURROW_S("ip4"),
                                         BURROW_S("www.golang.org"), &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "%v", err);
        goto out;
    }
    if (addrs.len != 1) {
        testing_t_fatalf_v(t, "got %d addresses, want 1", addrs.len);
        goto out;
    }
    Str got = net_ip_string(((const NetIP *)addrs.p)[0], a);
    if (!str_eq(got, BURROW_S("192.0.2.1"))) {
        testing_t_fatalf_v(t, "got address %s, want %s", got, BURROW_S("192.0.2.1"));
        goto out;
    }

    err = BURROW_NO_ERROR;
    (void)net_resolver_lookup_ip(&r, a, context_background(), BURROW_S("ip4"),
                                 BURROW_S("empty.com"), &err);
    const NetDNSError *de = (const NetDNSError *)errors_as(err, TYPE_NET_DNS_ERROR);
    if (de == NULL) {
        testing_t_fatalf_v(t, "err = %v; wanted a *net.DNSError", err);
        goto out;
    }
    if (!str_eq(de->err, err_no_such_host()))
        testing_t_fatalf_v(t, "Err = %q; wanted %q", de->err, err_no_such_host());
out:
    arena_free(&ar);
    rct_teardown(&conf);
}

/* ------------------------------------------------------------ forgeries */

/* A connection that answers the query it is sent with garbage, then with
 * the wrong ID, then with the answer, the way someone else on the network
 * might get in first. */
typedef struct ForgeryConn {
    Alloc *a;
    DnsmsgMessage msg;
    int reads;
    bool bad_query;
} ForgeryConn;

static Int forgery_read(void *self, Slice b, Error *err) {
    ForgeryConn *f = (ForgeryConn *)self;
    if (f->bad_query) {
        *err = fmt_errorf_v("invalid DNS query");
        return 0;
    }
    int n = f->reads++;
    if (n == 0) {
        static const char garbage[] = "garbage DNS response packet";
        Int l = (Int)sizeof garbage - 1;
        memcpy(b.p, garbage, (size_t)l);
        return l;
    }
    DnsmsgMessage m = f->msg;
    m.header.response = true;
    if (n == 1) {
        m.header.id++; /* make invalid ID */
    } else {
        DnsmsgResource rr = rr_a(nm("www.example.com."));
        add_rr(f->a, &m.answers, &rr);
    }
    Slice out;
    Error e = burrow__dnsmsg_message_pack(&m, f->a, &out);
    if (BURROW_FAILED(e)) {
        *err = e;
        return 0;
    }
    memcpy(b.p, out.p, (size_t)out.len);
    return out.len;
}

static Int forgery_write(void *self, Slice b, Error *err) {
    (void)err;
    ForgeryConn *f = (ForgeryConn *)self;
    memset(&f->msg, 0, sizeof f->msg);
    if (BURROW_FAILED(burrow__dnsmsg_message_unpack(&f->msg, f->a, b)))
        f->bad_query = true;
    return b.len;
}

static const NetConnVT forgery_conn_vt = {
    .reader = {&fake_conn_type, forgery_read},
    .writer = {&fake_conn_type, forgery_write},
    .closer = {&fake_conn_type, fake_close},
    .local_addr = fake_addr,
    .remote_addr = fake_addr,
    .set_deadline = fake_set_rw_deadline,
    .set_read_deadline = fake_set_rw_deadline,
    .set_write_deadline = fake_set_rw_deadline,
};

static NetConn forgery_dial(void *env, Alloc *a, Context ctx, Str n, Str s,
                            bool *packet, Error *err) {
    (void)env;
    (void)ctx;
    (void)n;
    (void)s;
    NetConn c = {NULL, NULL};
    ForgeryConn *f = (ForgeryConn *)mem_alloc(a, sizeof *f, _Alignof(ForgeryConn));
    if (f == NULL) {
        *err = burrow_err_out_of_memory;
        return c;
    }
    f->a = a;
    *packet = true;
    c.vt = &forgery_conn_vt;
    c.data = f;
    return c;
}

/* UDP round-tripper algorithm should ignore invalid DNS responses (issue
 * 13281). Go's test drives dnsPacketRoundTrip over a Pipe, and this one
 * goes through exchange, which calls it. */
static void TestIgnoreDNSForgeries(TestingT *t) {
    NetResolver r;
    memset(&r, 0, sizeof r);
    r.prefer_go = true;
    r.dial = BURROW_FN(NetResolverDial, forgery_dial, NULL);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    DnsmsgParser p;
    DnsmsgHeader h;
    Error err =
        exchange(&r, a, BURROW_S("192.0.2.1:53"),
                 must_question("www.example.com.", DNSMSG_TYPE_A, DNSMSG_CLASS_INET),
                 TIME_SECOND, false, &p, &h);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "dnsPacketRoundTrip failed: %v", err);
        arena_free(&ar);
        return;
    }
    DnsmsgResources as;
    memset(&as, 0, sizeof as);
    err = burrow__dnsmsg_parser_all_answers(&p, a, &as);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "AllAnswers failed: %v", err);
        arena_free(&ar);
        return;
    }
    if (as.len == 0 || as.p[0].body.kind != DNSMSG_BODY_A ||
        memcmp(as.p[0].body.u.a.a, test_addr, 4) != 0)
        testing_t_errorf_v(t, "got %d answers, want the address 192.0.2.1", as.len);
    arena_free(&ar);
}

/* ------------------------------------------------------- timeouts, rotate */

typedef struct RetryTimeoutEnv {
    Time deadline0;
    bool zero_deadline;
    bool unchanged;
} RetryTimeoutEnv;

static Error retry_timeout_rh(FakeDNSServer *srv, Alloc *a, Str n, Str s,
                              const DnsmsgMessage *q, Time deadline, DnsmsgMessage *r) {
    (void)n;
    RetryTimeoutEnv *e = (RetryTimeoutEnv *)srv->env;
    if (time_is_zero(deadline))
        e->zero_deadline = true;

    if (str_eq(s, BURROW_S("192.0.2.1:53"))) {
        e->deadline0 = deadline;
        time_sleep(10 * TIME_MILLISECOND);
        return os_err_deadline_exceeded;
    }

    if (time_equal(deadline, e->deadline0))
        e->unchanged = true;

    mock_txt_response(a, q, r);
    return BURROW_NO_ERROR;
}

/* Issue 16865. If a name server times out, continue to the next. */
static void TestRetryTimeout(TestingT *t) {
    if (!unix_only(t))
        return;
    ResolvConfTest conf;
    if (!rct_new(t, &conf))
        return;
    if (!UPDATE(t, &conf, "nameserver 192.0.2.1", /* the one that will timeout */
                "nameserver 192.0.2.2")) {
        rct_teardown(&conf);
        return;
    }

    RetryTimeoutEnv env;
    memset(&env, 0, sizeof env);
    FakeDNSServer fake = {retry_timeout_rh, &env, false};
    NetResolver r = fake_resolver(&fake, false);

    Arena ar;
    arena_init(&ar, NULL, 0);
    Error err = BURROW_NO_ERROR;
    (void)net_resolver_lookup_txt(&r, arena_allocator(&ar), context_background(),
                                  BURROW_S("www.golang.org"), &err);
    arena_free(&ar);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "%v", err);
        rct_teardown(&conf);
        return;
    }
    if (env.zero_deadline)
        testing_t_errorf_v(t, "zero deadline");
    if (env.unchanged)
        testing_t_errorf_v(t, "deadline didn't change");
    if (time_is_zero(env.deadline0))
        testing_t_errorf_v(t, "deadline0 still zero");
    rct_teardown(&conf);
}

typedef struct RotateEnv {
    char used[8][32];
    int n;
} RotateEnv;

static Error rotate_rh(FakeDNSServer *srv, Alloc *a, Str n, Str s,
                       const DnsmsgMessage *q, Time deadline, DnsmsgMessage *r) {
    (void)n;
    (void)deadline;
    RotateEnv *e = (RotateEnv *)srv->env;
    if (e->n < 8 && s.len < 32) {
        memcpy(e->used[e->n], s.p, (size_t)s.len);
        e->used[e->n][s.len] = '\0';
    }
    e->n++;
    mock_txt_response(a, q, r);
    return BURROW_NO_ERROR;
}

static void test_rotate(TestingT *t, bool rotate, const char *const *nameservers,
                        Int nns, const char *const *want_servers, Int nwant) {
    ResolvConfTest conf;
    if (!rct_new(t, &conf))
        return;

    const char *conf_lines[4];
    char lines[3][64];
    Int nlines = 0;
    for (Int i = 0; i < nns && i < 3; i++) {
        (void)snprintf(lines[i], sizeof lines[i], "nameserver %s", nameservers[i]);
        conf_lines[nlines++] = lines[i];
    }
    if (rotate)
        conf_lines[nlines++] = "options rotate";

    if (!rct_write_and_update(t, &conf, conf_lines, nlines)) {
        rct_teardown(&conf);
        return;
    }

    RotateEnv env;
    memset(&env, 0, sizeof env);
    FakeDNSServer fake = {rotate_rh, &env, false};
    NetResolver r = fake_resolver(&fake, false);

    /* len(nameservers) + 1 to allow rotation to get back to start */
    for (Int i = 0; i < nns + 1; i++) {
        Arena ar;
        arena_init(&ar, NULL, 0);
        Error err = BURROW_NO_ERROR;
        (void)net_resolver_lookup_txt(&r, arena_allocator(&ar), context_background(),
                                      BURROW_S("www.golang.org"), &err);
        arena_free(&ar);
        if (BURROW_FAILED(err)) {
            testing_t_fatalf_v(t, "%v", err);
            rct_teardown(&conf);
            return;
        }
    }

    bool ok = env.n == nwant;
    for (Int i = 0; ok && i < nwant; i++)
        ok = strcmp(env.used[i], want_servers[i]) == 0;
    if (!ok)
        testing_t_errorf_v(
            t, "rotate=%t got used servers:\n%s %s %s\nwant:\n%s %s %s", rotate,
            str_from_cstr(env.used[0]), str_from_cstr(env.used[1]),
            str_from_cstr(env.used[2]), str_from_cstr(want_servers[0]),
            str_from_cstr(want_servers[1]), str_from_cstr(want_servers[2]));
    rct_teardown(&conf);
}

static void TestRotate(TestingT *t) {
    if (!unix_only(t))
        return;
    static const char *const ns[] = {"192.0.2.1", "192.0.2.2"};
    /* without rotation, always uses the first server */
    static const char *const want_first[] = {"192.0.2.1:53", "192.0.2.1:53",
                                             "192.0.2.1:53"};
    test_rotate(t, false, ns, 2, want_first, 3);

    /* with rotation, rotates through back to first */
    static const char *const want_rotate[] = {"192.0.2.1:53", "192.0.2.2:53",
                                              "192.0.2.1:53"};
    test_rotate(t, true, ns, 2, want_rotate, 3);
}

/* -------------------------------------------------------- strict errors */

#define SE_NAME "test-issue19592"
#define SE_SERVER "192.0.2.53:53"
#define SE_SEARCH_X "test-issue19592.x.golang.org."
#define SE_SEARCH_Y "test-issue19592.y.golang.org."

typedef enum ResolveWhich {
    RESOLVE_OK,
    RESOLVE_OP_ERROR,
    RESOLVE_SERVFAIL,
    RESOLVE_TIMEOUT
} ResolveWhich;

typedef enum WantErr { WANT_NONE, WANT_TIMEOUT, WANT_TEMP, WANT_NXDOMAIN } WantErr;

typedef struct StrictCase {
    const char *desc;
    /* resolveWhich: what happens to a question for name, of type qtype or
     * of any type when it is 0, and RESOLVE_OK for any other. */
    const char *name;
    DnsmsgType qtype;
    ResolveWhich which;
    const char *temp_err;
    const char *want_ips[2];
    Int nips;
    WantErr want_strict;
    WantErr want_lax;
} StrictCase;

static const StrictCase strict_lookup_ip_cases[] = {
    {"No errors",
     NULL,
     0,
     RESOLVE_OK,
     NULL,
     {"192.0.2.1", "2001:db8::1"},
     2,
     WANT_NONE,
     WANT_NONE},
    {"searchX error fails in strict mode",
     SE_SEARCH_X,
     0,
     RESOLVE_TIMEOUT,
     NULL,
     {"192.0.2.1", "2001:db8::1"},
     2,
     WANT_TIMEOUT,
     WANT_NONE},
    {"searchX IPv4-only timeout fails in strict mode",
     SE_SEARCH_X,
     DNSMSG_TYPE_A,
     RESOLVE_TIMEOUT,
     NULL,
     {"192.0.2.1", "2001:db8::1"},
     2,
     WANT_TIMEOUT,
     WANT_NONE},
    {"searchX IPv6-only servfail fails in strict mode",
     SE_SEARCH_X,
     DNSMSG_TYPE_AAAA,
     RESOLVE_SERVFAIL,
     "server misbehaving",
     {"192.0.2.1", "2001:db8::1"},
     2,
     WANT_TEMP,
     WANT_NONE},
    {"searchY error always fails",
     SE_SEARCH_Y,
     0,
     RESOLVE_TIMEOUT,
     NULL,
     {NULL, NULL},
     0,
     WANT_TIMEOUT,
     WANT_NXDOMAIN}, /* This one reaches the "test." FQDN. */
    {"searchY IPv4-only socket error fails in strict mode",
     SE_SEARCH_Y,
     DNSMSG_TYPE_A,
     RESOLVE_OP_ERROR,
     "write: socket on fire",
     {"2001:db8::1", NULL},
     1,
     WANT_TEMP,
     WANT_NONE},
    {"searchY IPv6-only timeout fails in strict mode",
     SE_SEARCH_Y,
     DNSMSG_TYPE_AAAA,
     RESOLVE_TIMEOUT,
     NULL,
     {"192.0.2.1", NULL},
     1,
     WANT_TIMEOUT,
     WANT_NONE},
};

static ResolveWhich resolve_which(const StrictCase *tt, const DnsmsgQuestion *quest) {
    if (tt->name != NULL &&
        str_eq(burrow__dnsmsg_name_string(&quest->name), str_from_cstr(tt->name)) &&
        (tt->qtype == 0 || quest->type == tt->qtype))
        return tt->which;
    return RESOLVE_OK;
}

static Error strict_lookup_ip_rh(FakeDNSServer *srv, Alloc *a, Str n, Str s,
                                 const DnsmsgMessage *q, Time deadline,
                                 DnsmsgMessage *r) {
    (void)n;
    (void)s;
    (void)deadline;
    const StrictCase *tt = (const StrictCase *)srv->env;
    NetAddr nil_addr = {NULL, NULL};

    ResolveWhich which = resolve_which(tt, &q->questions.p[0]);
    if (which == RESOLVE_OP_ERROR)
        return burrow__net_op_error(BURROW_S("write"), BURROW_STR_EMPTY, nil_addr,
                                    nil_addr, fmt_errorf_v("socket on fire"));
    if (which == RESOLVE_SERVFAIL) {
        resp_init(r, q);
        r->header.rcode = DNSMSG_RCODE_SERVER_FAILURE;
        return BURROW_NO_ERROR;
    }
    if (which == RESOLVE_TIMEOUT)
        return os_err_deadline_exceeded;

    if (qname_is(q, SE_SEARCH_X) || qname_is(q, SE_NAME ".")) {
        /* Return NXDOMAIN to utilize the search list. */
        resp_init(r, q);
        r->header.rcode = DNSMSG_RCODE_NAME_ERROR;
        return BURROW_NO_ERROR;
    }
    if (!qname_is(q, SE_SEARCH_Y))
        return fmt_errorf_v("Unexpected Name: %s", qname(q));

    resp_init(r, q);
    DnsmsgResource rr;
    if (qtype(q) == DNSMSG_TYPE_A)
        rr = rr_a(q->questions.p[0].name);
    else if (qtype(q) == DNSMSG_TYPE_AAAA)
        rr = rr_aaaa(q->questions.p[0].name);
    else
        return fmt_errorf_v("Unexpected Type: %d", (int)qtype(q));
    add_rr(a, &r->answers, &rr);
    return BURROW_NO_ERROR;
}

static bool want_error(Error err, WantErr want, const char *temp_err, const char *name,
                       const char *server) {
    if (want == WANT_NONE)
        return BURROW_OK(err);
    WantDNSError w;
    memset(&w, 0, sizeof w);
    w.name = name;
    w.server = server;
    if (want == WANT_TIMEOUT) {
        w.err = error_text(os_err_deadline_exceeded);
        w.is_timeout = true;
        w.is_temporary = true;
    } else if (want == WANT_TEMP) {
        w.err = str_from_cstr(temp_err);
        w.is_temporary = true;
    } else {
        w.err = err_no_such_host();
        w.is_not_found = true;
    }
    return dns_error_equal(err, &w);
}

/* Issue 17448. With StrictErrors enabled, temporary errors should make
 * LookupIP fail rather than return a partial result. */
static void TestStrictErrorsLookupIP(TestingT *t) {
    if (!unix_only(t))
        return;
    ResolvConfTest conf;
    if (!rct_new(t, &conf))
        return;
    if (!UPDATE(t, &conf, "nameserver 192.0.2.53",
                "search x.golang.org y.golang.org")) {
        rct_teardown(&conf);
        return;
    }

    for (size_t i = 0;
         i < sizeof strict_lookup_ip_cases / sizeof strict_lookup_ip_cases[0]; i++) {
        const StrictCase *tt = &strict_lookup_ip_cases[i];
        FakeDNSServer fake = {strict_lookup_ip_rh, (void *)(uintptr_t)tt, false};
        for (int k = 0; k < 2; k++) {
            bool strict = k == 0;
            NetResolver r = fake_resolver(&fake, strict);
            Arena ar;
            arena_init(&ar, NULL, 0);
            Alloc *a = arena_allocator(&ar);
            Error err = BURROW_NO_ERROR;
            Slice ips = net_resolver_lookup_ip_addr(&r, a, context_background(),
                                                    BURROW_S(SE_NAME), &err);

            WantErr want = strict ? tt->want_strict : tt->want_lax;
            if (!want_error(err, want, tt->temp_err, SE_NAME, SE_SERVER))
                testing_t_errorf_v(t, "#%d (%s) strict=%t: got err %v; want %s", (int)i,
                                   str_from_cstr(tt->desc), strict, err,
                                   want == WANT_NONE ? BURROW_S("nil")
                                                     : BURROW_S("a DNSError"));

            /* The addresses as a set. */
            Int nwant = want == WANT_NONE ? tt->nips : 0;
            Int ngot = 0;
            bool ok = true;
            Str got[8];
            for (Int j = 0; j < ips.len; j++) {
                Str s = net_ip_addr_string(&((const NetIPAddr *)ips.p)[j], a);
                bool dup = false;
                for (Int m = 0; m < ngot; m++)
                    dup = dup || str_eq(got[m], s);
                if (!dup && ngot < 8)
                    got[ngot++] = s;
            }
            if (ngot != nwant)
                ok = false;
            for (Int j = 0; ok && j < nwant; j++) {
                bool found = false;
                for (Int m = 0; m < ngot; m++)
                    found = found || str_eq(got[m], str_from_cstr(tt->want_ips[j]));
                ok = found;
            }
            if (!ok)
                testing_t_errorf_v(t, "#%d (%s) strict=%t: got %d ips; want %d of them",
                                   (int)i, str_from_cstr(tt->desc), strict, ips.len,
                                   nwant);
            arena_free(&ar);
        }
    }
    rct_teardown(&conf);
}

static Error strict_txt_rh(FakeDNSServer *srv, Alloc *a, Str n, Str s,
                           const DnsmsgMessage *q, Time deadline, DnsmsgMessage *r) {
    (void)srv;
    (void)n;
    (void)s;
    (void)deadline;
    if (qname_is(q, "test.x.golang.org."))
        return os_err_deadline_exceeded;
    if (qname_is(q, "test.y.golang.org.")) {
        mock_txt_response(a, q, r);
        return BURROW_NO_ERROR;
    }
    return fmt_errorf_v("Unexpected Name: %s", qname(q));
}

/* Issue 17448. With StrictErrors enabled, temporary errors should make
 * LookupTXT stop walking the search list. */
static void TestStrictErrorsLookupTXT(TestingT *t) {
    if (!unix_only(t))
        return;
    ResolvConfTest conf;
    if (!rct_new(t, &conf))
        return;
    if (!UPDATE(t, &conf, "nameserver 192.0.2.53",
                "search x.golang.org y.golang.org")) {
        rct_teardown(&conf);
        return;
    }

    FakeDNSServer fake = {strict_txt_rh, NULL, false};
    for (int k = 0; k < 2; k++) {
        bool strict = k == 0;
        NetResolver r;
        memset(&r, 0, sizeof r);
        r.strict_errors = strict;
        r.dial = BURROW_FN(NetResolverDial, fake_dial, &fake);
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        DnsmsgParser p;
        Str server;
        Error err =
            burrow__net_dns_lookup(&r, a, context_background(), BURROW_S("test"),
                                   DNSMSG_TYPE_TXT, NULL, &p, &server);
        Int want_rrs = 0;
        bool ok;
        if (strict) {
            WantDNSError w = {error_text(os_err_deadline_exceeded),
                              "test",
                              SE_SERVER,
                              true,
                              true,
                              false};
            ok = dns_error_equal(err, &w);
        } else {
            want_rrs = 1;
            ok = BURROW_OK(err);
        }
        if (!ok)
            testing_t_errorf_v(t, "strict=%t: got err %v", strict, err);
        DnsmsgResources as;
        memset(&as, 0, sizeof as);
        if (BURROW_FAILED(burrow__dnsmsg_parser_all_answers(&p, a, &as)))
            as.len = 0;
        if (as.len != want_rrs)
            testing_t_errorf_v(t, "strict=%t: got %d; want %d", strict, as.len,
                               want_rrs);
        arena_free(&ar);
    }
    rct_teardown(&conf);
}

static Error goroutine_race_rh(FakeDNSServer *srv, Alloc *a, Str n, Str s,
                               const DnsmsgMessage *q, Time deadline,
                               DnsmsgMessage *r) {
    (void)srv;
    (void)a;
    (void)n;
    (void)s;
    (void)q;
    (void)deadline;
    (void)r;
    time_sleep(10 * TIME_MICROSECOND);
    return os_err_deadline_exceeded;
}

/* Test for a race between uninstalling the test hooks and closing a socket
 * connection. This used to fail when testing with -race. */
static void TestDNSGoroutineRace(TestingT *t) {
    FakeDNSServer fake = {goroutine_race_rh, NULL, false};
    NetResolver r = fake_resolver(&fake, false);

    /* The timeout here is less than the timeout used by the server, so the
     * goroutine started to query the (fake) server will hang around after
     * this test is done if we don't call dnsWaitGroup.Wait. */
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    ContextCancelFunc cancel;
    Context ctx =
        context_with_timeout(a, context_background(), 2 * TIME_MICROSECOND, &cancel);
    if (BURROW_CONTEXT_IS_NIL(ctx)) {
        testing_t_fatal_v(t, "out of memory");
        arena_free(&ar);
        return;
    }
    Error err = BURROW_NO_ERROR;
    (void)net_resolver_lookup_ip_addr(&r, a, ctx, BURROW_S("where.are.they.now"), &err);
    if (BURROW_OK(err))
        testing_t_fatalf_v(t, "fake DNS lookup unexpectedly succeeded");
    BURROW_CALLF0(cancel);
    context_release(ctx);
    arena_free(&ar);
}

/* ---------------------------------------------------------- tryOneName */

static Error lookup_with_fake(FakeDNSServer *fake, const char *name, DnsmsgType typ) {
    NetResolver r = fake_resolver(fake, false);
    burrow__DNSConfig *conf = burrow__net_system_dns_config();
    Arena ar;
    arena_init(&ar, NULL, 0);
    DnsmsgParser p;
    Str server;
    Error err =
        burrow__net_dns_try_one_name(&r, arena_allocator(&ar), context_background(),
                                     conf, str_from_cstr(name), typ, &p, &server);
    arena_free(&ar);
    burrow__dns_config_put(conf);
    return err;
}

static Error rcode_rh(FakeDNSServer *srv, Alloc *a, Str n, Str s,
                      const DnsmsgMessage *q, Time deadline, DnsmsgMessage *r) {
    (void)a;
    (void)n;
    (void)s;
    (void)deadline;
    resp_init(r, q);
    r->header.rcode = *(const DnsmsgRCode *)srv->env;
    return BURROW_NO_ERROR;
}

/* Issue 8434: verify that Temporary returns true on an error when rcode is
 * SERVFAIL */
static void TestIssue8434(TestingT *t) {
    if (!unix_only(t))
        return;
    DnsmsgRCode rcode = DNSMSG_RCODE_SERVER_FAILURE;
    FakeDNSServer fake = {rcode_rh, &rcode, false};
    Error err = lookup_with_fake(&fake, "golang.org.", DNSMSG_TYPE_ALL);
    if (BURROW_OK(err)) {
        testing_t_fatalf_v(t, "expected an error");
        return;
    }
    if (!net_is_error(err)) {
        testing_t_fatalf_v(t, "err = %v; wanted something supporting net.Error", err);
        return;
    }
    if (!net_error_temporary(err)) {
        testing_t_fatalf_v(t, "Temporary = false for err = %v; want Temporary == true",
                           err);
        return;
    }
    const NetDNSError *de = (const NetDNSError *)errors_as(err, TYPE_NET_DNS_ERROR);
    if (de == NULL)
        testing_t_fatalf_v(t, "err = %v; wanted a *net.DNSError", err);
    else if (!de->is_temporary)
        testing_t_fatalf_v(
            t, "IsTemporary = false for err = %v; want IsTemporary == true", err);
}

static void TestIssueNoSuchHostExists(TestingT *t) {
    if (!unix_only(t))
        return;
    DnsmsgRCode rcode = DNSMSG_RCODE_NAME_ERROR;
    FakeDNSServer fake = {rcode_rh, &rcode, false};
    Error err = lookup_with_fake(&fake, "golang.org.", DNSMSG_TYPE_ALL);
    if (BURROW_OK(err)) {
        testing_t_fatalf_v(t, "expected an error");
        return;
    }
    if (!net_is_error(err)) {
        testing_t_fatalf_v(t, "err = %v; wanted something supporting net.Error", err);
        return;
    }
    const NetDNSError *de = (const NetDNSError *)errors_as(err, TYPE_NET_DNS_ERROR);
    if (de == NULL)
        testing_t_fatalf_v(t, "err = %v; wanted a *net.DNSError", err);
    else if (!de->is_not_found)
        testing_t_fatalf_v(
            t, "IsNotFound = false for err = %v; want IsNotFound == true", err);
}

typedef struct NoSuchHostEnv {
    int32_t lookups;
    bool authoritative;
} NoSuchHostEnv;

static Error no_such_host_rh(FakeDNSServer *srv, Alloc *a, Str n, Str s,
                             const DnsmsgMessage *q, Time deadline, DnsmsgMessage *r) {
    (void)a;
    (void)n;
    (void)s;
    (void)deadline;
    NoSuchHostEnv *e = (NoSuchHostEnv *)srv->env;
    sync_atomic_add_int32(&e->lookups, 1);
    resp_init(r, q);
    if (e->authoritative) {
        /* no answers */
        r->header.rcode = DNSMSG_RCODE_SUCCESS;
        r->header.authoritative = true;
    } else {
        r->header.rcode = DNSMSG_RCODE_NAME_ERROR;
    }
    return BURROW_NO_ERROR;
}

static void no_such_host_test(void *env, TestingT *t) {
    NoSuchHostEnv *e = (NoSuchHostEnv *)env;
    FakeDNSServer fake = {no_such_host_rh, e, false};
    Error err = lookup_with_fake(&fake, ".", DNSMSG_TYPE_ALL);

    if (e->lookups != 1)
        testing_t_errorf_v(t, "got %d lookups, wanted 1", (int)e->lookups);

    if (BURROW_OK(err)) {
        testing_t_fatalf_v(t, "expected an error");
        return;
    }
    const NetDNSError *de = (const NetDNSError *)errors_as(err, TYPE_NET_DNS_ERROR);
    if (de == NULL) {
        testing_t_fatalf_v(t, "err = %v; wanted a *net.DNSError", err);
        return;
    }
    if (!str_eq(de->err, err_no_such_host())) {
        testing_t_fatalf_v(t, "Err = %q; wanted %q", de->err, err_no_such_host());
        return;
    }
    if (!de->is_not_found)
        testing_t_fatalf_v(t, "IsNotFound = %t wanted true", de->is_not_found);
}

/* TestNoSuchHost verifies that tryOneName works correctly when the domain
 * does not exist.
 *
 * Issue 12778: verify that NXDOMAIN without RA bit errors as "no such host"
 * and not "server misbehaving"
 *
 * Issue 25336: verify that NXDOMAIN errors fail fast.
 *
 * Issue 27525: verify that empty answers fail fast. */
static void TestNoSuchHost(TestingT *t) {
    if (!unix_only(t))
        return;
    NoSuchHostEnv nxdomain = {0, false};
    NoSuchHostEnv no_answers = {0, true};
    testing_t_run(t, BURROW_S("NXDOMAIN"),
                  BURROW_FN(TestingTFunc, no_such_host_test, &nxdomain));
    testing_t_run(t, BURROW_S("no answers"),
                  BURROW_FN(TestingTFunc, no_such_host_test, &no_answers));
}

static Error success_rh(FakeDNSServer *srv, Alloc *a, Str n, Str s,
                        const DnsmsgMessage *q, Time deadline, DnsmsgMessage *r) {
    (void)srv;
    (void)a;
    (void)n;
    (void)s;
    (void)deadline;
    resp_init(r, q);
    r->header.rcode = DNSMSG_RCODE_SUCCESS;
    return BURROW_NO_ERROR;
}

/* Issue 26573: verify that Conns that don't implement PacketConn are treated
 * as streams even when udp was requested. */
static void TestDNSDialTCP(TestingT *t) {
    FakeDNSServer fake = {success_rh, NULL, true};
    NetResolver r = fake_resolver(&fake, false);
    Arena ar;
    arena_init(&ar, NULL, 0);
    DnsmsgParser p;
    DnsmsgHeader h;
    Error err = exchange(&r, arena_allocator(&ar), BURROW_S("0.0.0.0"),
                         must_question("com.", DNSMSG_TYPE_ALL, DNSMSG_CLASS_INET),
                         TIME_SECOND, false, &p, &h);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "exchange failed: %v", err);
    arena_free(&ar);
}

static Str txt_two[] = {BURROW_S_INIT("string1 "), BURROW_S_INIT("string2")};
static Str txt_one[] = {BURROW_S_INIT("onestring")};

static Error two_strings_rh(FakeDNSServer *srv, Alloc *a, Str n, Str s,
                            const DnsmsgMessage *q, Time deadline, DnsmsgMessage *r) {
    (void)srv;
    (void)n;
    (void)s;
    (void)deadline;
    resp_init(r, q);
    r->header.rcode = DNSMSG_RCODE_SUCCESS;
    DnsmsgResource rr1 = rr_txt(q->questions.p[0].name, DNSMSG_TYPE_A, txt_two, 2);
    DnsmsgResource rr2 = rr_txt(q->questions.p[0].name, DNSMSG_TYPE_A, txt_one, 1);
    add_rr(a, &r->answers, &rr1);
    add_rr(a, &r->answers, &rr2);
    return BURROW_NO_ERROR;
}

/* Issue 27763: verify that two strings in one TXT record are concatenated. */
static void TestTXTRecordTwoStrings(TestingT *t) {
    if (!unix_only(t))
        return;
    FakeDNSServer fake = {two_strings_rh, NULL, false};
    NetResolver r = fake_resolver(&fake, false);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Error err = BURROW_NO_ERROR;
    Slice txt = net_resolver_lookup_txt(&r, arena_allocator(&ar), context_background(),
                                        BURROW_S("golang.org"), &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "LookupTXT failed: %v", err);
        arena_free(&ar);
        return;
    }
    if (txt.len != 2) {
        testing_t_fatalf_v(t, "len(txt), got %d, want %d", txt.len, 2);
        arena_free(&ar);
        return;
    }
    const Str *ts = (const Str *)txt.p;
    if (!str_eq(ts[0], BURROW_S("string1 string2")))
        testing_t_errorf_v(t, "txt[0], got %q, want %q", ts[0],
                           BURROW_S("string1 string2"));
    if (!str_eq(ts[1], BURROW_S("onestring")))
        testing_t_errorf_v(t, "txt[1], got %q, want %q", ts[1], BURROW_S("onestring"));
    arena_free(&ar);
}

typedef struct SingleRequestEnv {
    int32_t firstcalled;
    int32_t a_after_aaaa;
} SingleRequestEnv;

static Error single_request_rh(FakeDNSServer *srv, Alloc *a, Str n, Str s,
                               const DnsmsgMessage *q, Time deadline,
                               DnsmsgMessage *r) {
    (void)n;
    (void)s;
    (void)deadline;
    SingleRequestEnv *e = (SingleRequestEnv *)srv->env;
    resp_init(r, q);
    for (Int i = 0; i < q->questions.len; i++) {
        const DnsmsgQuestion *question = &q->questions.p[i];
        if (question->type == DNSMSG_TYPE_A) {
            if (str_eq(burrow__dnsmsg_name_string(&question->name),
                       BURROW_S("slowipv4.example.net.")))
                time_sleep(10 * TIME_MILLISECOND);
            if (!sync_atomic_compare_and_swap_int32(&e->firstcalled, 0, 1))
                sync_atomic_store_int32(&e->a_after_aaaa, 1);
            DnsmsgResource rr = rr_a(q->questions.p[0].name);
            add_rr(a, &r->answers, &rr);
        } else if (question->type == DNSMSG_TYPE_AAAA) {
            (void)sync_atomic_compare_and_swap_int32(&e->firstcalled, 0, 2);
            DnsmsgResource rr = rr_aaaa(q->questions.p[0].name);
            add_rr(a, &r->answers, &rr);
        }
    }
    return BURROW_NO_ERROR;
}

/* Issue 29644: support single-request resolv.conf option in pure Go
 * resolver. The A and AAAA queries will be sent sequentially, not in
 * parallel. */
static void TestSingleRequestLookup(TestingT *t) {
    if (!unix_only(t))
        return;
    SingleRequestEnv env;
    memset(&env, 0, sizeof env);
    FakeDNSServer fake = {single_request_rh, &env, false};
    NetResolver r = fake_resolver(&fake, false);

    ResolvConfTest conf;
    if (!rct_new(t, &conf))
        return;
    if (!UPDATE(t, &conf, "options single-request")) {
        rct_teardown(&conf);
        return;
    }
    static const char *const names[] = {"hostname.example.net", "slowipv4.example.net"};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        sync_atomic_store_int32(&env.firstcalled, 0);
        Arena ar;
        arena_init(&ar, NULL, 0);
        Error err = BURROW_NO_ERROR;
        (void)net_resolver_lookup_ip_addr(&r, arena_allocator(&ar),
                                          context_background(), str_from_cstr(names[i]),
                                          &err);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "%v", err);
        arena_free(&ar);
    }
    if (sync_atomic_load_int32(&env.a_after_aaaa) != 0)
        testing_t_errorf_v(t, "the A query was received after the AAAA query !");
    rct_teardown(&conf);
}

typedef struct UseTCPEnv {
    bool truncated;
    bool used_udp;
} UseTCPEnv;

static Error use_tcp_rh(FakeDNSServer *srv, Alloc *a, Str n, Str s,
                        const DnsmsgMessage *q, Time deadline, DnsmsgMessage *r) {
    (void)s;
    (void)deadline;
    UseTCPEnv *e = (UseTCPEnv *)srv->env;
    resp_init(r, q);
    r->header.rcode = DNSMSG_RCODE_SUCCESS;
    if (e->truncated) {
        r->header.truncated = true;
        DnsmsgResource rr = rr_a(q->questions.p[0].name);
        add_rr(a, &r->answers, &rr);
    }
    if (str_eq(n, BURROW_S("udp"))) {
        e->used_udp = true;
        return fmt_errorf_v("udp protocol was used instead of tcp");
    }
    return BURROW_NO_ERROR;
}

/* Issue 29358. Add configuration knob to force TCP-only DNS requests in the
 * pure Go resolver. */
static void TestDNSUseTCP(TestingT *t) {
    UseTCPEnv env = {false, false};
    FakeDNSServer fake = {use_tcp_rh, &env, false};
    NetResolver r = fake_resolver(&fake, false);
    Arena ar;
    arena_init(&ar, NULL, 0);
    DnsmsgParser p;
    DnsmsgHeader h;
    Error err = exchange(&r, arena_allocator(&ar), BURROW_S("0.0.0.0"),
                         must_question("com.", DNSMSG_TYPE_ALL, DNSMSG_CLASS_INET),
                         TIME_SECOND, true, &p, &h);
    if (env.used_udp)
        testing_t_fatalf_v(t, "udp protocol was used instead of tcp");
    else if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "exchange failed: %v", err);
    arena_free(&ar);
}

static void TestDNSUseTCPTruncated(TestingT *t) {
    UseTCPEnv env = {true, false};
    FakeDNSServer fake = {use_tcp_rh, &env, false};
    NetResolver r = fake_resolver(&fake, false);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    DnsmsgParser p;
    DnsmsgHeader h;
    Error err = exchange(&r, a, BURROW_S("0.0.0.0"),
                         must_question("com.", DNSMSG_TYPE_ALL, DNSMSG_CLASS_INET),
                         TIME_SECOND, true, &p, &h);
    DnsmsgResources as;
    memset(&as, 0, sizeof as);
    if (env.used_udp) {
        testing_t_fatalf_v(t, "udp protocol was used instead of tcp");
    } else if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "exchange failed: %v", err);
    } else {
        err = burrow__dnsmsg_parser_all_answers(&p, a, &as);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "unexpected error %v getting all answers", err);
        else if (as.len != 1)
            testing_t_fatalf_v(t, "got %d answers; want 1", as.len);
    }
    arena_free(&ar);
}

static Str txt_rrsig[] = {BURROW_S_INIT("PTR 8 6 60 ...")}; /* fake RRSIG */

static Error ptr_and_non_ptr_rh(FakeDNSServer *srv, Alloc *a, Str n, Str s,
                                const DnsmsgMessage *q, Time deadline,
                                DnsmsgMessage *r) {
    (void)srv;
    (void)n;
    (void)s;
    (void)deadline;
    resp_init(r, q);
    r->header.rcode = DNSMSG_RCODE_SUCCESS;
    DnsmsgResource rr1 = rr_ptr(q->questions.p[0].name, 0, "golang.org.");
    DnsmsgResource rr2 = rr_txt(q->questions.p[0].name, DNSMSG_TYPE_TXT, txt_rrsig, 1);
    add_rr(a, &r->answers, &rr1);
    add_rr(a, &r->answers, &rr2);
    return BURROW_NO_ERROR;
}

/* Issue 34660: PTR response with non-PTR answers should ignore non-PTR */
static void TestPTRandNonPTR(TestingT *t) {
    if (!unix_only(t))
        return;
    FakeDNSServer fake = {ptr_and_non_ptr_rh, NULL, false};
    NetResolver r = fake_resolver(&fake, false);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Error err = BURROW_NO_ERROR;
    Slice names = net_resolver_lookup_addr(
        &r, arena_allocator(&ar), context_background(), BURROW_S("192.0.2.123"), &err);
    static const char *const want[] = {"golang.org."};
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "LookupAddr: %v", err);
    else if (!strs_equal(names, want, 1))
        testing_t_errorf_v(t, "got %d names; want [golang.org.]", names.len);
    arena_free(&ar);
}

/* ------------------------------------------------------- CVE-2021-33195 */

static Error cve_rh(FakeDNSServer *srv, Alloc *a, Str n, Str s, const DnsmsgMessage *q,
                    Time deadline, DnsmsgMessage *r) {
    (void)srv;
    (void)n;
    (void)s;
    (void)deadline;
    resp_init(r, q);
    r->header.rcode = DNSMSG_RCODE_SUCCESS;
    r->header.recursion_available = true;
    DnsmsgType ty = qtype(q);
    DnsmsgName html = nm("<html>.golang.org.");
    DnsmsgName good = nm("good.golang.org.");
    DnsmsgResource rr;
    if (ty == DNSMSG_TYPE_A) {
        /* CNAME lookup uses a A/AAAA as a proxy */
        rr = rr_a(html);
        add_rr(a, &r->answers, &rr);
    } else if (ty == DNSMSG_TYPE_SRV) {
        DnsmsgName name = q->questions.p[0].name;
        if (qname_is(q, "_hdr._tcp.golang.org."))
            name = html;
        rr = rr_srv(name, 4, "<html>.golang.org.");
        add_rr(a, &r->answers, &rr);
        rr = rr_srv(name, 4, "good.golang.org.");
        add_rr(a, &r->answers, &rr);
    } else if (ty == DNSMSG_TYPE_MX) {
        static const char *const mx[] = {"<html>.golang.org.",
                                         "good.golang.org.",
                                         "127.0.0.1.",
                                         "1.2.3.4.5.",
                                         "2001:4860:0:2001::68.",
                                         "2001:4860:0:2001::68%zone."};
        for (size_t i = 0; i < sizeof mx / sizeof mx[0]; i++) {
            rr = rr_mx(nm(mx[i]), 4, mx[i]);
            add_rr(a, &r->answers, &rr);
        }
    } else if (ty == DNSMSG_TYPE_NS) {
        rr = rr_ns(html, 4, "<html>.golang.org.");
        add_rr(a, &r->answers, &rr);
        rr = rr_ns(good, 4, "good.golang.org.");
        add_rr(a, &r->answers, &rr);
    } else if (ty == DNSMSG_TYPE_PTR) {
        rr = rr_ptr(html, 4, "<html>.golang.org.");
        add_rr(a, &r->answers, &rr);
        rr = rr_ptr(good, 4, "good.golang.org.");
        add_rr(a, &r->answers, &rr);
    }
    return BURROW_NO_ERROR;
}

/* &DNSError{Err: errMalformedDNSRecordsDetail, Name: name}.Error() */
static Str malformed_text(Alloc *a, const char *name) {
    NetDNSError e;
    memset(&e, 0, sizeof e);
    e.err = BURROW_S("DNS response contained records which contain invalid names");
    e.name = str_from_cstr(name);
    return net_dns_error_error(&e, a);
}

static bool err_text_is(Error err, Str want) {
    return BURROW_FAILED(err) && str_eq(error_text(err), want);
}

typedef struct CVEEnv {
    NetResolver *r;
} CVEEnv;

static void cve_cname(void *env, TestingT *t) {
    NetResolver *r = ((CVEEnv *)env)->r;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str want = malformed_text(a, "golang.org");
    Error err = BURROW_NO_ERROR;
    (void)net_resolver_lookup_cname(r, a, context_background(), BURROW_S("golang.org"),
                                    &err);
    if (!err_text_is(err, want)) {
        testing_t_fatalf_v(t, "unexpected error: %v", err);
        arena_free(&ar);
        return;
    }
    err = BURROW_NO_ERROR;
    (void)net_lookup_cname(a, BURROW_S("golang.org"), &err);
    if (!err_text_is(err, want))
        testing_t_fatalf_v(t, "unexpected error: %v", err);
    arena_free(&ar);
}

static bool srv_good(Slice records) {
    if (records.len != 1)
        return false;
    const NetSRV *s = (const NetSRV *)records.p;
    return str_eq(s->target, BURROW_S("good.golang.org.")) && s->port == 0 &&
           s->priority == 0 && s->weight == 0;
}

static void cve_srv_bad_record(void *env, TestingT *t) {
    NetResolver *r = ((CVEEnv *)env)->r;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str want = malformed_text(a, "golang.org");
    Error err = BURROW_NO_ERROR;
    Slice records =
        net_resolver_lookup_srv(r, a, context_background(), BURROW_S("target"),
                                BURROW_S("tcp"), BURROW_S("golang.org"), NULL, &err);
    if (!err_text_is(err, want)) {
        testing_t_fatalf_v(t, "unexpected error: %v", err);
        arena_free(&ar);
        return;
    }
    if (!srv_good(records))
        testing_t_errorf_v(t, "Unexpected record set");
    err = BURROW_NO_ERROR;
    records = net_lookup_srv(a, BURROW_S("target"), BURROW_S("tcp"),
                             BURROW_S("golang.org"), NULL, &err);
    if (!err_text_is(err, want))
        testing_t_errorf_v(t, "unexpected error: %v", err);
    if (!srv_good(records))
        testing_t_errorf_v(t, "Unexpected record set");
    arena_free(&ar);
}

static void cve_srv_bad_header(void *env, TestingT *t) {
    NetResolver *r = ((CVEEnv *)env)->r;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str expected = BURROW_S("lookup golang.org.: SRV header name is invalid");
    Error err = BURROW_NO_ERROR;
    (void)net_resolver_lookup_srv(r, a, context_background(), BURROW_S("hdr"),
                                  BURROW_S("tcp"), BURROW_S("golang.org."), NULL, &err);
    if (!err_text_is(err, expected))
        testing_t_errorf_v(
            t, "Resolver.LookupSRV returned unexpected error, got %v, want %q", err,
            expected);
    err = BURROW_NO_ERROR;
    (void)net_lookup_srv(a, BURROW_S("hdr"), BURROW_S("tcp"), BURROW_S("golang.org."),
                         NULL, &err);
    if (!err_text_is(err, expected))
        testing_t_errorf_v(t, "LookupSRV returned unexpected error, got %v, want %q",
                           err, expected);
    arena_free(&ar);
}

/* The hosts of the MX records, sorted. */
static bool mx_hosts_are(Slice records, const char *const *want, Int n) {
    if (records.len != n)
        return false;
    Str got[8];
    for (Int i = 0; i < n && i < 8; i++)
        got[i] = ((const NetMX *)records.p)[i].host;
    for (Int i = 1; i < n; i++) {
        for (Int j = i; j > 0 && str_cmp(got[j - 1], got[j]) > 0; j--) {
            Str x = got[j];
            got[j] = got[j - 1];
            got[j - 1] = x;
        }
    }
    for (Int i = 0; i < n; i++) {
        if (!str_eq(got[i], str_from_cstr(want[i])))
            return false;
    }
    return true;
}

static void cve_mx(void *env, TestingT *t) {
    NetResolver *r = ((CVEEnv *)env)->r;
    static const char *const expected[] = {"127.0.0.1.", "2001:4860:0:2001::68.",
                                           "good.golang.org."};
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str want = malformed_text(a, "golang.org");
    Error err = BURROW_NO_ERROR;
    Slice records = net_resolver_lookup_mx(r, a, context_background(),
                                           BURROW_S("golang.org"), &err);
    if (!err_text_is(err, want)) {
        testing_t_fatalf_v(t, "unexpected error: %v", err);
        arena_free(&ar);
        return;
    }
    if (!mx_hosts_are(records, expected, 3))
        testing_t_errorf_v(t, "Unexpected record set: got %d records", records.len);
    err = BURROW_NO_ERROR;
    records = net_lookup_mx(a, BURROW_S("golang.org"), &err);
    if (!err_text_is(err, want)) {
        testing_t_fatalf_v(t, "unexpected error: %v", err);
        arena_free(&ar);
        return;
    }
    if (!mx_hosts_are(records, expected, 3))
        testing_t_errorf_v(t, "Unexpected record set: got %d records", records.len);
    arena_free(&ar);
}

static bool ns_good(Slice records) {
    return records.len == 1 &&
           str_eq(((const NetNS *)records.p)[0].host, BURROW_S("good.golang.org."));
}

static void cve_ns(void *env, TestingT *t) {
    NetResolver *r = ((CVEEnv *)env)->r;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str want = malformed_text(a, "golang.org");
    Error err = BURROW_NO_ERROR;
    Slice records = net_resolver_lookup_ns(r, a, context_background(),
                                           BURROW_S("golang.org"), &err);
    if (!err_text_is(err, want)) {
        testing_t_fatalf_v(t, "unexpected error: %v", err);
        arena_free(&ar);
        return;
    }
    if (!ns_good(records))
        testing_t_errorf_v(t, "Unexpected record set");
    err = BURROW_NO_ERROR;
    records = net_lookup_ns(a, BURROW_S("golang.org"), &err);
    if (!err_text_is(err, want)) {
        testing_t_fatalf_v(t, "unexpected error: %v", err);
        arena_free(&ar);
        return;
    }
    if (!ns_good(records))
        testing_t_errorf_v(t, "Unexpected record set");
    arena_free(&ar);
}

static void cve_addr(void *env, TestingT *t) {
    NetResolver *r = ((CVEEnv *)env)->r;
    static const char *const expected[] = {"good.golang.org."};
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str want = malformed_text(a, "192.0.2.42");
    Error err = BURROW_NO_ERROR;
    Slice records = net_resolver_lookup_addr(r, a, context_background(),
                                             BURROW_S("192.0.2.42"), &err);
    if (!err_text_is(err, want)) {
        testing_t_fatalf_v(t, "unexpected error: %v", err);
        arena_free(&ar);
        return;
    }
    if (!strs_equal(records, expected, 1))
        testing_t_errorf_v(t, "Unexpected record set");
    err = BURROW_NO_ERROR;
    records = net_lookup_addr(a, BURROW_S("192.0.2.42"), &err);
    if (!err_text_is(err, want)) {
        testing_t_fatalf_v(t, "unexpected error: %v", err);
        arena_free(&ar);
        return;
    }
    if (!strs_equal(records, expected, 1))
        testing_t_errorf_v(t, "Unexpected record set");
    arena_free(&ar);
}

static void TestCVE202133195(TestingT *t) {
    if (!unix_only(t))
        return;
    FakeDNSServer fake = {cve_rh, NULL, false};
    NetResolver r = fake_resolver(&fake, false);
    /* Change the default resolver to match our manipulated resolver. Only
     * the dial is ours, so the lookups in flight stay the default's. */
    NetResolver *def = net_default_resolver();
    NetResolverDial orig_dial = def->dial;
    bool orig_prefer_go = def->prefer_go;
    def->dial = r.dial;
    def->prefer_go = true;
    /* Redirect host file lookups. */
    Arena ar;
    arena_init(&ar, NULL, 0);
    Str hosts = write_temp(t, arena_allocator(&ar), "hosts", testdata_hosts);
    burrow__net_set_hosts_file_path(hosts);

    CVEEnv env = {&r};
    static const struct {
        const char *name;
        void (*f)(void *env, TestingT *t);
    } tests[] = {
        {"CNAME", cve_cname},
        {"SRV (bad record)", cve_srv_bad_record},
        {"SRV (bad header)", cve_srv_bad_header},
        {"MX", cve_mx},
        {"NS", cve_ns},
        {"Addr", cve_addr},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++)
        testing_t_run(t, str_from_cstr(tests[i].name),
                      BURROW_FN(TestingTFunc, tests[i].f, &env));

    burrow__net_set_hosts_file_path(BURROW_STR_EMPTY);
    remove_temp(hosts);
    arena_free(&ar);
    def->dial = orig_dial;
    def->prefer_go = orig_prefer_go;
}

static Error null_mx_rh(FakeDNSServer *srv, Alloc *a, Str n, Str s,
                        const DnsmsgMessage *q, Time deadline, DnsmsgMessage *r) {
    (void)srv;
    (void)n;
    (void)s;
    (void)deadline;
    resp_init(r, q);
    r->header.rcode = DNSMSG_RCODE_SUCCESS;
    DnsmsgResource rr = rr_mx(q->questions.p[0].name, 0, ".");
    add_rr(a, &r->answers, &rr);
    return BURROW_NO_ERROR;
}

static void TestNullMX(TestingT *t) {
    if (!unix_only(t))
        return;
    FakeDNSServer fake = {null_mx_rh, NULL, false};
    NetResolver r = fake_resolver(&fake, false);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Error err = BURROW_NO_ERROR;
    Slice rrset = net_resolver_lookup_mx(&r, arena_allocator(&ar), context_background(),
                                         BURROW_S("golang.org"), &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "LookupMX: %v", err);
    } else if (rrset.len != 1 ||
               !str_eq(((const NetMX *)rrset.p)[0].host, BURROW_S(".")) ||
               ((const NetMX *)rrset.p)[0].pref != 0) {
        testing_t_errorf_v(t, "got %d records; want [&{. 0}]", rrset.len);
    }
    arena_free(&ar);
}

static Error root_ns_rh(FakeDNSServer *srv, Alloc *a, Str n, Str s,
                        const DnsmsgMessage *q, Time deadline, DnsmsgMessage *r) {
    (void)srv;
    (void)n;
    (void)s;
    (void)deadline;
    resp_init(r, q);
    r->header.rcode = DNSMSG_RCODE_SUCCESS;
    DnsmsgResource rr = rr_ns(q->questions.p[0].name, 0, "i.root-servers.net.");
    add_rr(a, &r->answers, &rr);
    return BURROW_NO_ERROR;
}

static void TestRootNS(TestingT *t) {
    if (!unix_only(t))
        return;
    /* See https://golang.org/issue/45715. */
    FakeDNSServer fake = {root_ns_rh, NULL, false};
    NetResolver r = fake_resolver(&fake, false);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Error err = BURROW_NO_ERROR;
    Slice rrset = net_resolver_lookup_ns(&r, arena_allocator(&ar), context_background(),
                                         BURROW_S("."), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "LookupNS: %v", err);
    else if (rrset.len != 1 ||
             !str_eq(((const NetNS *)rrset.p)[0].host, BURROW_S("i.root-servers.net.")))
        testing_t_errorf_v(t, "got %d records; want [&{i.root-servers.net.}]",
                           rrset.len);
    arena_free(&ar);
}

/* ------------------------------------------------------ hosts aliases */

typedef struct AliasTest {
    const char *lookup, *res;
} AliasTest;

static const AliasTest lookup_static_host_aliases_test[] = {
    /* 127.0.0.1 */
    {"test", "test"},
    /* 127.0.0.2 */
    {"test2.example.com", "test2.example.com"},
    {"2.test", "test2.example.com"},
    /* 127.0.0.3 */
    {"test3.example.com", "3.test"},
    {"3.test", "3.test"},
    /* 127.0.0.4 */
    {"example.com", "example.com"},
    /* 127.0.0.5 */
    {"test5.example.com", "test4.example.com"},
    {"5.test", "test4.example.com"},
    {"4.test", "test4.example.com"},
    {"test4.example.com", "test4.example.com"},
};

static const AliasTest go_lookup_ip_cname_order_dns_files_mode_tests[] = {
    /* 127.0.1.1 */
    {"invalid.invalid", "invalid.test"},
};

typedef struct AliasesEnv {
    burrow__HostLookupOrder mode;
    bool unexpected;
} AliasesEnv;

static Error aliases_rh(FakeDNSServer *srv, Alloc *a, Str n, Str s,
                        const DnsmsgMessage *q, Time deadline, DnsmsgMessage *r) {
    (void)a;
    (void)n;
    (void)s;
    (void)deadline;
    AliasesEnv *e = (AliasesEnv *)srv->env;
    if (e->mode != BURROW__HOST_LOOKUP_DNS_FILES) {
        e->unexpected = true;
        return fmt_errorf_v("received unexpected DNS query");
    }
    resp_init(r, q);
    r->questions.len = 1;
    return BURROW_NO_ERROR;
}

static void test_go_lookup_ip_cname_order_hosts_aliases(TestingT *t,
                                                        burrow__HostLookupOrder mode,
                                                        Str lookup, Str lookup_res) {
    AliasesEnv env = {mode, false};
    FakeDNSServer fake = {aliases_rh, &env, false};
    NetResolver r = fake_resolver(&fake, false);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str ins[4] = {lookup, burrow__net_abs_domain_name(a, lookup),
                  strings_to_lower(a, lookup), strings_to_upper(a, lookup)};
    for (size_t i = 0; i < 4; i++) {
        Str res = BURROW_STR_EMPTY;
        Error err = BURROW_NO_ERROR;
        (void)burrow__net_go_lookup_ip_cname_order(&r, a, context_background(),
                                                   BURROW_S("ip"), ins[i], mode, NULL,
                                                   &res, &err);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "expected err == nil, but got error: %v", err);
        if (!str_eq(res, lookup_res))
            testing_t_errorf_v(t, "goLookupIPCNAMEOrder(%s): got %s, want %s", ins[i],
                               res, lookup_res);
    }
    if (env.unexpected)
        testing_t_fatalf_v(t, "received unexpected DNS query");
    arena_free(&ar);
}

static void hosts_aliases(TestingT *t, burrow__HostLookupOrder mode,
                          const AliasTest *tests, size_t n) {
    if (!unix_only(t))
        return;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str aliases = write_temp(t, a, "aliases", testdata_aliases);
    burrow__net_set_hosts_file_path(aliases);
    for (size_t i = 0; i < n; i++)
        test_go_lookup_ip_cname_order_hosts_aliases(
            t, mode, str_from_cstr(tests[i].lookup),
            burrow__net_abs_domain_name(a, str_from_cstr(tests[i].res)));
    burrow__net_set_hosts_file_path(BURROW_STR_EMPTY);
    remove_temp(aliases);
    arena_free(&ar);
}

static void TestGoLookupIPCNAMEOrderHostsAliasesFilesOnlyMode(TestingT *t) {
    hosts_aliases(t, BURROW__HOST_LOOKUP_FILES, lookup_static_host_aliases_test,
                  sizeof lookup_static_host_aliases_test /
                      sizeof lookup_static_host_aliases_test[0]);
}

static void TestGoLookupIPCNAMEOrderHostsAliasesFilesDNSMode(TestingT *t) {
    hosts_aliases(t, BURROW__HOST_LOOKUP_FILES_DNS, lookup_static_host_aliases_test,
                  sizeof lookup_static_host_aliases_test /
                      sizeof lookup_static_host_aliases_test[0]);
}

static void TestGoLookupIPCNAMEOrderHostsAliasesDNSFilesMode(TestingT *t) {
    hosts_aliases(t, BURROW__HOST_LOOKUP_DNS_FILES,
                  go_lookup_ip_cname_order_dns_files_mode_tests,
                  sizeof go_lookup_ip_cname_order_dns_files_mode_tests /
                      sizeof go_lookup_ip_cname_order_dns_files_mode_tests[0]);
}

/* ------------------------------------------------------------- EDNS(0) */

typedef struct PacketSizeEnv {
    bool disable;
    int32_t bad; /* which check failed, 0 for none */
} PacketSizeEnv;

static Error packet_size_rh(FakeDNSServer *srv, Alloc *a, Str n, Str s,
                            const DnsmsgMessage *q, Time deadline, DnsmsgMessage *r) {
    (void)n;
    (void)s;
    (void)deadline;
    PacketSizeEnv *e = (PacketSizeEnv *)srv->env;
    if (e->disable) {
        if (q->additionals.len > 0)
            sync_atomic_store_int32(&e->bad, 1); /* unexpected additional record */
    } else {
        if (q->additionals.len == 0)
            sync_atomic_store_int32(&e->bad, 2); /* missing EDNS record */
        else if (q->additionals.p[0].body.kind != DNSMSG_BODY_OPT)
            sync_atomic_store_int32(&e->bad, 3);
        else if (q->additionals.p[0].body.u.opt.options.len != 0)
            sync_atomic_store_int32(&e->bad, 4);
        else if ((int)q->additionals.p[0].header.class_ != MAX_DNS_PACKET_SIZE)
            sync_atomic_store_int32(&e->bad, 5);
    }

    /* Hand back a dummy answer to verify that LookupIPAddr completes. */
    resp_init(r, q);
    r->header.rcode = DNSMSG_RCODE_SUCCESS;
    if (qtype(q) == DNSMSG_TYPE_A) {
        DnsmsgResource rr = rr_a(q->questions.p[0].name);
        add_rr(a, &r->answers, &rr);
    }
    return BURROW_NO_ERROR;
}

static void test_dns_packet_size(void *env, TestingT *t) {
    bool disable = *(const bool *)env;
    PacketSizeEnv e = {disable, 0};
    FakeDNSServer fake = {packet_size_rh, &e, false};

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    bool had = false;
    Str old = BURROW_STR_EMPTY;
    if (disable) {
        old = os_lookup_env(a, BURROW_S("GODEBUG"), &had);
        (void)os_setenv(BURROW_S("GODEBUG"), BURROW_S("netedns0=0"));
    }

    NetResolver r = fake_resolver(&fake, false);
    Error err = BURROW_NO_ERROR;
    (void)net_resolver_lookup_ip_addr(&r, a, context_background(), BURROW_S("go.dev"),
                                      &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "lookup failed: %v", err);

    static const char *const what[] = {
        "",
        "unexpected additional record",
        "missing EDNS record",
        "additional record type, expected OPTResource",
        "found Options, expected none",
        "EDNS packet size is not maxDNSPacketSize",
    };
    int32_t bad = sync_atomic_load_int32(&e.bad);
    if (bad > 0 && bad < 6)
        testing_t_errorf_v(t, "%s", str_from_cstr(what[bad]));

    if (disable) {
        if (had)
            (void)os_setenv(BURROW_S("GODEBUG"), old);
        else
            (void)os_unsetenv(BURROW_S("GODEBUG"));
    }
    arena_free(&ar);
}

/* Test that we advertise support for a larger DNS packet size. This isn't a
 * great test as it just tests the dnsmessage package against itself. */
static void TestDNSPacketSize(TestingT *t) {
    if (!unix_only(t))
        return;
    static const bool enabled = false, disabled = true;
    testing_t_run(
        t, BURROW_S("enabled"),
        BURROW_FN(TestingTFunc, test_dns_packet_size, (void *)(uintptr_t)&enabled));
    testing_t_run(
        t, BURROW_S("disabled"),
        BURROW_FN(TestingTFunc, test_dns_packet_size, (void *)(uintptr_t)&disabled));
}

/* ------------------------------------------------------- long names */

static Str txt_dot[] = {BURROW_S_INIT(".")};

static Error long_names_rh(FakeDNSServer *srv, Alloc *a, Str n, Str s,
                           const DnsmsgMessage *q, Time deadline, DnsmsgMessage *r) {
    (void)srv;
    (void)n;
    (void)s;
    (void)deadline;
    resp_init(r, q);
    r->header.rcode = DNSMSG_RCODE_SUCCESS;
    DnsmsgName name = q->questions.p[0].name;
    DnsmsgType ty = qtype(q);
    DnsmsgResource rr;
    if (ty == DNSMSG_TYPE_A)
        rr = rr_a(name);
    else if (ty == DNSMSG_TYPE_AAAA)
        rr = rr_aaaa(name);
    else if (ty == DNSMSG_TYPE_TXT)
        rr = rr_txt(name, ty, txt_dot, 1);
    else if (ty == DNSMSG_TYPE_MX)
        rr = rr_mx(name, 0, "go.dev.");
    else if (ty == DNSMSG_TYPE_NS)
        rr = rr_ns(name, 0, "go.dev.");
    else if (ty == DNSMSG_TYPE_SRV)
        rr = rr_srv(name, 0, "go.dev.");
    else if (ty == DNSMSG_TYPE_CNAME)
        rr = rr_cname(name, "fake.cname.");
    else
        return fmt_errorf_v("unknown dnsmessage type");
    rr.header.length = 0;
    add_rr(a, &r->answers, &rr);
    return BURROW_NO_ERROR;
}

static Error long_names_query(NetResolver *r, Alloc *a, const char *t, Str req) {
    Context ctx = context_background();
    Error err = BURROW_NO_ERROR;
    if (strcmp(t, "CNAME") == 0) {
        (void)net_resolver_lookup_cname(r, a, ctx, req, &err);
    } else if (strcmp(t, "Host") == 0) {
        (void)net_resolver_lookup_host(r, a, ctx, req, &err);
    } else if (strcmp(t, "IP") == 0) {
        (void)net_resolver_lookup_ip(r, a, ctx, BURROW_S("ip"), req, &err);
    } else if (strcmp(t, "IPAddr") == 0) {
        (void)net_resolver_lookup_ip_addr(r, a, ctx, req, &err);
    } else if (strcmp(t, "MX") == 0) {
        (void)net_resolver_lookup_mx(r, a, ctx, req, &err);
    } else if (strcmp(t, "NS") == 0) {
        (void)net_resolver_lookup_ns(r, a, ctx, req, &err);
    } else if (strcmp(t, "NetIP") == 0) {
        (void)net_resolver_lookup_net_ip(r, a, ctx, BURROW_S("ip"), req, &err);
    } else if (strcmp(t, "SRV") == 0) {
        static const char service[] = "service";
        static const char proto[] = "proto";
        Int skip = (Int)sizeof service - 1 + (Int)sizeof proto - 1 + 4;
        req = str_from_bytes(req.p + skip, req.len - skip);
        (void)net_resolver_lookup_srv(r, a, ctx, str_from_cstr(service),
                                      str_from_cstr(proto), req, NULL, &err);
    } else if (strcmp(t, "TXT") == 0) {
        (void)net_resolver_lookup_txt(r, a, ctx, req, &err);
    }
    return err;
}

static void TestLongDNSNames(TestingT *t) {
    if (!unix_only(t))
        return;
    static const char long_dns_suffix[] = ".go.dev.";
    static const char long_dns_suffix_no_ending_dot[] = ".go.dev";
    const Int ls = (Int)sizeof long_dns_suffix - 1;
    const Int lsn = (Int)sizeof long_dns_suffix_no_ending_dot - 1;

    char long_dns_prefix[401];
    for (int i = 0; i < 20; i++)
        memcpy(long_dns_prefix + (size_t)i * 20, "verylongdomainlabel.", 20);
    long_dns_prefix[400] = '\0';

    struct {
        Int prefix;
        const char *suffix;
        bool fail;
    } long_dns_names_tests[] = {
        {255 - ls, long_dns_suffix, true},
        {254 - ls, long_dns_suffix, false},
        {253 - ls, long_dns_suffix, false},

        {253 - lsn, long_dns_suffix_no_ending_dot, false},
        {254 - lsn, long_dns_suffix_no_ending_dot, true},
    };

    FakeDNSServer fake = {long_names_rh, NULL, false};
    NetResolver r = fake_resolver(&fake, false);

    static const char *const method_tests[] = {"CNAME", "Host",  "IP",  "IPAddr", "MX",
                                               "NS",    "NetIP", "SRV", "TXT"};
    for (size_t i = 0; i < sizeof long_dns_names_tests / sizeof long_dns_names_tests[0];
         i++) {
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        Str req = fmt_sprintf_v(a, "%s%s",
                                str_from_bytes((const Byte *)long_dns_prefix,
                                               long_dns_names_tests[i].prefix),
                                str_from_cstr(long_dns_names_tests[i].suffix));
        for (size_t m = 0; m < sizeof method_tests / sizeof method_tests[0]; m++) {
            Error err = long_names_query(&r, a, method_tests[m], req);
            if (long_dns_names_tests[i].fail) {
                if (BURROW_OK(err)) {
                    testing_t_errorf_v(t, "%d: Lookup%s: unexpected success", (int)i,
                                       str_from_cstr(method_tests[m]));
                    break;
                }
                WantDNSError w = {err_no_such_host(), NULL, "", false, false, true};
                const NetDNSError *d =
                    (const NetDNSError *)errors_as(err, TYPE_NET_DNS_ERROR);
                if (d == NULL || !str_eq(d->name, req) || !str_eq(d->err, w.err) ||
                    d->server.len != 0 || d->is_timeout || d->is_temporary ||
                    !d->is_not_found || BURROW_FAILED(d->unwrap_err))
                    testing_t_errorf_v(t, "%d: Lookup%s: unexpected error: %v", (int)i,
                                       str_from_cstr(method_tests[m]), err);
                break;
            }
            if (BURROW_FAILED(err))
                testing_t_errorf_v(t, "%d: Lookup%s: unexpected error: %v", (int)i,
                                   str_from_cstr(method_tests[m]), err);
        }
        arena_free(&ar);
    }
}

/* ------------------------------------------------------------- trust-ad */

typedef struct TrustADEnv {
    int32_t unexpected_ad;
    int32_t missing_ad;
} TrustADEnv;

static Error trust_ad_rh(FakeDNSServer *srv, Alloc *a, Str n, Str s,
                         const DnsmsgMessage *q, Time deadline, DnsmsgMessage *r) {
    (void)n;
    (void)s;
    (void)deadline;
    TrustADEnv *e = (TrustADEnv *)srv->env;
    if (qname_is(q, "notrustad.go.dev.") && q->header.authentic_data)
        sync_atomic_store_int32(&e->unexpected_ad, 1);
    if (qname_is(q, "trustad.go.dev.") && !q->header.authentic_data)
        sync_atomic_store_int32(&e->missing_ad, 1);

    resp_init(r, q);
    r->header.rcode = DNSMSG_RCODE_SUCCESS;
    if (qtype(q) == DNSMSG_TYPE_A) {
        DnsmsgResource rr = rr_a(q->questions.p[0].name);
        add_rr(a, &r->answers, &rr);
    }
    return BURROW_NO_ERROR;
}

static void TestDNSTrustAD(TestingT *t) {
    if (!unix_only(t))
        return;
    TrustADEnv env = {0, 0};
    FakeDNSServer fake = {trust_ad_rh, &env, false};
    NetResolver r = fake_resolver(&fake, false);

    ResolvConfTest conf;
    if (!rct_new(t, &conf))
        return;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    if (UPDATE(t, &conf, "nameserver 127.0.0.1")) {
        Error err = BURROW_NO_ERROR;
        (void)net_resolver_lookup_ip_addr(&r, a, context_background(),
                                          BURROW_S("notrustad.go.dev"), &err);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "lookup failed: %v", err);
    }

    if (UPDATE(t, &conf, "nameserver 127.0.0.1", "options trust-ad")) {
        Error err = BURROW_NO_ERROR;
        (void)net_resolver_lookup_ip_addr(&r, a, context_background(),
                                          BURROW_S("trustad.go.dev"), &err);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "lookup failed: %v", err);
    }

    if (sync_atomic_load_int32(&env.unexpected_ad) != 0)
        testing_t_errorf_v(t, "unexpected AD bit");
    if (sync_atomic_load_int32(&env.missing_ad) != 0)
        testing_t_errorf_v(t, "expected AD bit");
    arena_free(&ar);
    rct_teardown(&conf);
}

static NetConn no_reload_dial(void *env, Alloc *a, Context ctx, Str network,
                              Str address, bool *packet, Error *err) {
    (void)env;
    if (!str_eq(address, BURROW_S("192.0.2.1:53"))) {
        *err = fmt_errorf_v("configuration unexpectedly changed");
        NetConn none = {NULL, NULL};
        return none;
    }
    return fake_dial(&fake_dns_server_successful, a, ctx, network, address, packet,
                     err);
}

static void TestDNSConfigNoReload(TestingT *t) {
    if (!unix_only(t))
        return;
    NetResolver r;
    memset(&r, 0, sizeof r);
    r.prefer_go = true;
    r.dial = BURROW_FN(NetResolverDial, no_reload_dial, NULL);

    ResolvConfTest conf;
    if (!rct_new(t, &conf))
        return;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    if (!rct_write_and_update_at(t, &conf,
                                 LINES("nameserver 192.0.2.1", "options no-reload"), 2,
                                 time_add(time_now(), -TIME_HOUR)))
        goto out;

    Error err = BURROW_NO_ERROR;
    (void)net_resolver_lookup_host(&r, a, context_background(), BURROW_S("go.dev"),
                                   &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "%v", err);
        goto out;
    }

    if (!rct_write(t, &conf, LINES("nameserver 192.0.2.200"), 1))
        goto out;

    err = BURROW_NO_ERROR;
    (void)net_resolver_lookup_host(&r, a, context_background(), BURROW_S("go.dev"),
                                   &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);
out:
    arena_free(&ar);
    rct_teardown(&conf);
}

/* nssStr: s written to a file in dir and read back. */
static burrow__NssConf *nss_str(TestingT *t, Alloc *a, const char *s) {
    Str name = write_temp(t, a, "nss", s);
    if (name.len == 0)
        return NULL;
    burrow__NssConf *conf = burrow__nss_parse_file(name);
    remove_temp(name);
    if (conf == NULL)
        testing_t_errorf_v(t, "parseNSSConfFile(%s): out of memory", name);
    return conf;
}

static void TestLookupOrderFilesNoSuchHost(TestingT *t) {
    if (!unix_only(t))
        return;
    bool openbsd = strcmp(BURROW_OS_NAME, "openbsd") == 0;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    burrow__NssConf *orig_nss = NULL;
    if (!openbsd) {
        orig_nss = burrow__net_system_nss();
        burrow__net_set_system_nss(nss_str(t, a, "hosts: files"), TIME_HOUR);
    }

    ResolvConfTest conf;
    if (!rct_new(t, &conf)) {
        if (!openbsd)
            burrow__net_set_system_nss(orig_nss, 0);
        arena_free(&ar);
        return;
    }

    burrow__DNSConfig resolv_conf;
    memset(&resolv_conf, 0, sizeof resolv_conf);
    resolv_conf.servers =
        slice_from((void *)(uintptr_t)burrow__net_default_ns, 2, 2, TYPE_STRING);
    if (openbsd) {
        /* Set error to ErrNotExist, so that the hostLookupOrder returns
         * hostLookupFiles for openbsd. */
        resolv_conf.err = fs_err_not_exist;
    }
    burrow__net_force_dns_config(&resolv_conf, time_add(time_now(), TIME_HOUR));

    Str tmp_file = write_temp(t, a, "hosts", "");
    burrow__net_set_hosts_file_path(tmp_file);

    Str test_name = BURROW_S("test.invalid");

    burrow__DNSConfig *dc = NULL;
    burrow__HostLookupOrder order = burrow__net_conf_host_lookup_order(
        burrow__net_system_conf(), net_default_resolver(), test_name, &dc);
    burrow__dns_config_put(dc);
    if (order != BURROW__HOST_LOOKUP_FILES) {
        /* skip test for systems which do not return hostLookupFiles */
        burrow__net_set_hosts_file_path(BURROW_STR_EMPTY);
        remove_temp(tmp_file);
        rct_teardown(&conf);
        if (!openbsd)
            burrow__net_set_system_nss(orig_nss, 0);
        arena_free(&ar);
        testing_t_skipf_v(t, "hostLookupOrder did not return hostLookupFiles");
        return;
    }

    static const char *const lookup_tests[] = {"Host", "IP", "IPAddr", "NetIP"};
    NetResolver *def = net_default_resolver();
    for (size_t i = 0; i < sizeof lookup_tests / sizeof lookup_tests[0]; i++) {
        Error err = BURROW_NO_ERROR;
        Context ctx = context_background();
        if (i == 0)
            (void)net_resolver_lookup_host(def, a, ctx, test_name, &err);
        else if (i == 1)
            (void)net_resolver_lookup_ip(def, a, ctx, BURROW_S("ip"), test_name, &err);
        else if (i == 2)
            (void)net_resolver_lookup_ip_addr(def, a, ctx, test_name, &err);
        else
            (void)net_resolver_lookup_net_ip(def, a, ctx, BURROW_S("ip"), test_name,
                                             &err);

        if (BURROW_OK(err)) {
            testing_t_errorf_v(t, "Lookup%s: unexpected success",
                               str_from_cstr(lookup_tests[i]));
            continue;
        }
        WantDNSError w = {err_no_such_host(), "test.invalid", "", false, false, true};
        if (!dns_error_equal(err, &w))
            testing_t_errorf_v(t, "Lookup%s: unexpected error: %v",
                               str_from_cstr(lookup_tests[i]), err);
    }

    burrow__net_set_hosts_file_path(BURROW_STR_EMPTY);
    remove_temp(tmp_file);
    rct_teardown(&conf);
    if (!openbsd)
        burrow__net_set_system_nss(orig_nss, 0);
    arena_free(&ar);
}

static Error extended_rcode_rh(FakeDNSServer *srv, Alloc *a, Str n, Str s,
                               const DnsmsgMessage *q, Time deadline,
                               DnsmsgMessage *r) {
    (void)srv;
    (void)n;
    (void)s;
    (void)deadline;
    DnsmsgRCode fraud_success_code = (DnsmsgRCode)(DNSMSG_RCODE_SUCCESS | 1 << 10);
    resp_init(r, q);
    r->questions.len = 1;
    r->header.rcode = fraud_success_code;
    DnsmsgResource rr = rr_edns0(fraud_success_code);
    add_rr(a, &r->additionals, &rr);
    return BURROW_NO_ERROR;
}

static void TestExtendedRCode(TestingT *t) {
    if (!unix_only(t))
        return;
    FakeDNSServer fake = {extended_rcode_rh, NULL, false};
    Error err = lookup_with_fake(&fake, "go.dev.", DNSMSG_TYPE_A);
    const NetDNSError *d = (const NetDNSError *)errors_as(err, TYPE_NET_DNS_ERROR);
    if (d == NULL || !str_eq(d->err, error_text(burrow__net_err_server_misbehaving)))
        testing_t_fatalf_v(t, "r.tryOneName(): unexpected error: %v", err);
}

/* This test makes sure that we always re-check the resolv.conf no matter the
 * elapsed time in case the default nameservers are used. */
static void TestEmptyResolvConfReplacedWithConfHaingNameservers(TestingT *t) {
    if (!unix_only(t))
        return;
    ResolvConfTest conf;
    if (!rct_new(t, &conf))
        return;

    if (!rct_write_and_update_at(t, &conf, LINES("# empty resolv.conf file"), 1,
                                 time_now()))
        goto out;

    burrow__DNSConfig *c = burrow__net_system_dns_config_named(conf.path);
    bool is_default = burrow__dns_config_is_default_ns(c);
    burrow__dns_config_put(c);
    if (!is_default) {
        testing_t_fatalf_v(t, "resolv.conf was not re-loaded");
        goto out;
    }

    if (!rct_write_and_update_at(t, &conf, LINES("nameserver 192.0.2.1"), 1,
                                 time_now()))
        goto out;

    c = burrow__net_system_dns_config_named(conf.path);
    is_default = burrow__dns_config_is_default_ns(c);
    burrow__dns_config_put(c);
    if (is_default)
        testing_t_fatalf_v(t, "resolv.conf was not re-loaded");
out:
    rct_teardown(&conf);
}

#define TESTS(X)                                                                       \
    X(TestDNSTransportFallback)                                                        \
    X(TestDNSTransportNoFallbackOnTCP)                                                 \
    X(TestSpecialDomainName)                                                           \
    X(TestAvoidDNSName)                                                                \
    X(TestNameListAvoidDNS)                                                            \
    X(TestLookupTorOnion)                                                              \
    X(TestUpdateResolvConf)                                                            \
    X(TestGoLookupIPWithResolverConfig)                                                \
    X(TestGoLookupIPOrderFallbackToFile)                                               \
    X(TestErrorForOriginalNameWhenSearching)                                           \
    X(TestIgnoreLameReferrals)                                                         \
    X(TestIgnoreDNSForgeries)                                                          \
    X(TestRetryTimeout)                                                                \
    X(TestRotate)                                                                      \
    X(TestStrictErrorsLookupIP)                                                        \
    X(TestStrictErrorsLookupTXT)                                                       \
    X(TestDNSGoroutineRace)                                                            \
    X(TestIssue8434)                                                                   \
    X(TestIssueNoSuchHostExists)                                                       \
    X(TestNoSuchHost)                                                                  \
    X(TestDNSDialTCP)                                                                  \
    X(TestTXTRecordTwoStrings)                                                         \
    X(TestSingleRequestLookup)                                                         \
    X(TestDNSUseTCP)                                                                   \
    X(TestDNSUseTCPTruncated)                                                          \
    X(TestPTRandNonPTR)                                                                \
    X(TestCVE202133195)                                                                \
    X(TestNullMX)                                                                      \
    X(TestRootNS)                                                                      \
    X(TestGoLookupIPCNAMEOrderHostsAliasesFilesOnlyMode)                               \
    X(TestGoLookupIPCNAMEOrderHostsAliasesFilesDNSMode)                                \
    X(TestGoLookupIPCNAMEOrderHostsAliasesDNSFilesMode)                                \
    X(TestDNSPacketSize)                                                               \
    X(TestLongDNSNames)                                                                \
    X(TestDNSTrustAD)                                                                  \
    X(TestDNSConfigNoReload)                                                           \
    X(TestLookupOrderFilesNoSuchHost)                                                  \
    X(TestExtendedRCode)                                                               \
    X(TestEmptyResolvConfReplacedWithConfHaingNameservers)

TESTING_MAIN(TESTS)

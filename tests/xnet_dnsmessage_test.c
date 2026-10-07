/* Derived from Go's vendored golang.org/x/net/dns/dnsmessage, its
 * message_test.go and svcb_test.go.
 * Go source: go1.27.1.
 *
 * Go's tests reach into the package for its unexported pieces, and those are
 * the burrow__dnsmsg functions the header lists for them. Go compares whole
 * messages with reflect.DeepEqual and here msg_eq does it field by field. A Go
 * slice that is empty and one that is nil are the same to it, which is the
 * one place it is looser than DeepEqual.
 *
 * Left out: TestNoFmt, which looks for an import C does not have, and
 * TestSVCBParsingAllocs and TestHTTPSBuildAllocs, which count Go's
 * allocations. A parse here copies each SVCB value on its own where Go copies
 * them all at once, so the count is not the same thing. TestParsingAllocs and
 * TestBuildingAllocs stay: the parse they run takes no allocator at all here,
 * and the build runs against a Fixed allocator that has to see no allocation.
 * TestPrintUint16 calls print_uint32, which is what Go's printUint16 does.
 * FuzzUnpackPack runs its two seeds as a test.
 *
 * TestFreeAfterUnpack and TestFreeAfterBuild are new. They run a pack, an
 * unpack and a build under the tracking allocator and check that the _free
 * functions give back everything the parse made.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/xnet/dnsmessage.h"
#include "burrow/burrow.h"
#include "burrow/error.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/fixed.h"
#include "burrow/mem/heap.h"
#include "burrow/mem/track.h"

#include <stdint.h>
#include <string.h>

#define S BURROW_S
#define DM(x) burrow__dnsmsg_##x

#define ARENA_BEGIN                                                                    \
    Arena ar;                                                                          \
    arena_init(&ar, NULL, 0);                                                          \
    Alloc *a = arena_allocator(&ar)
#define ARENA_END arena_free(&ar)

enum { PRIVATE_USE_TYPE = 65362, HEADER_LEN = 12 };

/* ------------------------------------------------------------- helpers */

static DnsmsgName nm(const char *s) {
    return DM(must_new_name)(str_from_cstr(s));
}

static bool is(Error got, Error want) {
    return got.vt == want.vt && got.data == want.data;
}

/* A copy in a, so a message put together in a helper outlives the helper. */
static Slice bytes_dup(Alloc *a, const Byte *p, Int n) {
    Byte *q = (Byte *)mem_alloc_nozero(a, (size_t)(n > 0 ? n : 1), 1);
    if (n > 0)
        memcpy(q, p, (size_t)n);
    return slice_from(q, n, n, TYPE_BYTE);
}

#define BYTES(a, ...)                                                                  \
    bytes_dup((a), (const Byte[]){__VA_ARGS__},                                        \
              (Int)sizeof((const Byte[]){__VA_ARGS__}))
#define LIT(a, s) bytes_dup((a), (const Byte *)(s), (Int)sizeof(s) - 1)

static Slice view(const Byte *p, Int n) {
    return slice_from((void *)(uintptr_t)p, n, n, TYPE_BYTE);
}

#define VIEW(arr) view((arr), (Int)sizeof(arr))

static bool bytes_eq(Slice x, Slice y) {
    return x.len == y.len && (x.len == 0 || memcmp(x.p, y.p, (size_t)x.len) == 0);
}

static DnsmsgQuestions questions(Alloc *a, Int n) {
    DnsmsgQuestion *p = (DnsmsgQuestion *)mem_alloc(a, (size_t)(n + 1) * sizeof *p,
                                                    _Alignof(DnsmsgQuestion));
    return (DnsmsgQuestions){p, n, n};
}

static DnsmsgResources resources(Alloc *a, Int n) {
    DnsmsgResource *p = (DnsmsgResource *)mem_alloc(a, (size_t)(n + 1) * sizeof *p,
                                                    _Alignof(DnsmsgResource));
    return (DnsmsgResources){p, n, n};
}

static DnsmsgStrs strs(Alloc *a, Int n) {
    Str *p = (Str *)mem_alloc(a, (size_t)(n + 1) * sizeof *p, _Alignof(Str));
    return (DnsmsgStrs){p, n, n};
}

static DnsmsgOptions options(Alloc *a, Int n) {
    DnsmsgOption *p = (DnsmsgOption *)mem_alloc(a, (size_t)(n + 1) * sizeof *p,
                                                _Alignof(DnsmsgOption));
    return (DnsmsgOptions){p, n, n};
}

static DnsmsgSVCParams params(Alloc *a, Int n) {
    DnsmsgSVCParam *p = (DnsmsgSVCParam *)mem_alloc(a, (size_t)(n + 1) * sizeof *p,
                                                    _Alignof(DnsmsgSVCParam));
    return (DnsmsgSVCParams){p, n, n};
}

static DnsmsgQuestion question(DnsmsgName name, DnsmsgType type) {
    DnsmsgQuestion q;
    memset(&q, 0, sizeof q);
    q.name = name;
    q.type = type;
    q.class_ = DNSMSG_CLASS_INET;
    return q;
}

static DnsmsgResourceHeader rheader(DnsmsgName name, DnsmsgType type) {
    DnsmsgResourceHeader h;
    memset(&h, 0, sizeof h);
    h.name = name;
    h.type = type;
    h.class_ = DNSMSG_CLASS_INET;
    return h;
}

static DnsmsgResourceBody body_of(DnsmsgBodyKind kind) {
    DnsmsgResourceBody b;
    memset(&b, 0, sizeof b);
    b.kind = kind;
    return b;
}

static DnsmsgResourceBody body_a(Byte b0, Byte b1, Byte b2, Byte b3) {
    DnsmsgResourceBody b = body_of(DNSMSG_BODY_A);
    b.u.a.a[0] = b0;
    b.u.a.a[1] = b1;
    b.u.a.a[2] = b2;
    b.u.a.a[3] = b3;
    return b;
}

static DnsmsgResource resource(DnsmsgResourceHeader h, DnsmsgResourceBody body) {
    DnsmsgResource r;
    r.header = h;
    r.body = body;
    return r;
}

static DnsmsgMessage empty_msg(void) {
    DnsmsgMessage m;
    memset(&m, 0, sizeof m);
    return m;
}

/* mustEDNS0ResourceHeader. */
static DnsmsgResourceHeader edns0_header(int l, DnsmsgRCode extrc, bool dnssec_ok) {
    DnsmsgResourceHeader h;
    memset(&h, 0, sizeof h);
    h.class_ = DNSMSG_CLASS_INET;
    Error e = DM(resource_header_set_edns0)(&h, l, extrc, dnssec_ok);
    if (BURROW_FAILED(e))
        panic_str(error_text(e));
    return h;
}

/* ---------------------------------------------------------- comparing */

static bool name_eq(const DnsmsgName *x, const DnsmsgName *y) {
    return x->length == y->length && memcmp(x->data, y->data, x->length) == 0;
}

static bool header_eq(const DnsmsgHeader *x, const DnsmsgHeader *y) {
    return x->id == y->id && x->response == y->response && x->op_code == y->op_code &&
           x->authoritative == y->authoritative && x->truncated == y->truncated &&
           x->recursion_desired == y->recursion_desired &&
           x->recursion_available == y->recursion_available &&
           x->authentic_data == y->authentic_data &&
           x->checking_disabled == y->checking_disabled && x->rcode == y->rcode;
}

static bool question_eq(const DnsmsgQuestion *x, const DnsmsgQuestion *y) {
    return name_eq(&x->name, &y->name) && x->type == y->type && x->class_ == y->class_;
}

static bool rheader_eq(const DnsmsgResourceHeader *x, const DnsmsgResourceHeader *y) {
    return name_eq(&x->name, &y->name) && x->type == y->type &&
           x->class_ == y->class_ && x->ttl == y->ttl && x->length == y->length;
}

static bool svcb_eq(const DnsmsgSVCBResource *x, const DnsmsgSVCBResource *y) {
    if (x->priority != y->priority || !name_eq(&x->target, &y->target) ||
        x->params.len != y->params.len)
        return false;
    for (Int i = 0; i < x->params.len; i++) {
        if (x->params.p[i].key != y->params.p[i].key ||
            !bytes_eq(x->params.p[i].value, y->params.p[i].value))
            return false;
    }
    return true;
}

static bool body_eq(const DnsmsgResourceBody *x, const DnsmsgResourceBody *y) {
    if (x->kind != y->kind)
        return false;
    switch (x->kind) {
    case DNSMSG_BODY_NONE:
        return true;
    case DNSMSG_BODY_A:
        return memcmp(x->u.a.a, y->u.a.a, 4) == 0;
    case DNSMSG_BODY_AAAA:
        return memcmp(x->u.aaaa.aaaa, y->u.aaaa.aaaa, 16) == 0;
    case DNSMSG_BODY_NS:
        return name_eq(&x->u.ns.ns, &y->u.ns.ns);
    case DNSMSG_BODY_CNAME:
        return name_eq(&x->u.cname.cname, &y->u.cname.cname);
    case DNSMSG_BODY_PTR:
        return name_eq(&x->u.ptr.ptr, &y->u.ptr.ptr);
    case DNSMSG_BODY_MX:
        return x->u.mx.pref == y->u.mx.pref && name_eq(&x->u.mx.mx, &y->u.mx.mx);
    case DNSMSG_BODY_SOA:
        return name_eq(&x->u.soa.ns, &y->u.soa.ns) &&
               name_eq(&x->u.soa.mbox, &y->u.soa.mbox) &&
               x->u.soa.serial == y->u.soa.serial &&
               x->u.soa.refresh == y->u.soa.refresh &&
               x->u.soa.retry == y->u.soa.retry && x->u.soa.expire == y->u.soa.expire &&
               x->u.soa.min_ttl == y->u.soa.min_ttl;
    case DNSMSG_BODY_TXT:
        if (x->u.txt.txt.len != y->u.txt.txt.len)
            return false;
        for (Int i = 0; i < x->u.txt.txt.len; i++) {
            if (!str_eq(x->u.txt.txt.p[i], y->u.txt.txt.p[i]))
                return false;
        }
        return true;
    case DNSMSG_BODY_SRV:
        return x->u.srv.priority == y->u.srv.priority &&
               x->u.srv.weight == y->u.srv.weight && x->u.srv.port == y->u.srv.port &&
               name_eq(&x->u.srv.target, &y->u.srv.target);
    case DNSMSG_BODY_SVCB:
        return svcb_eq(&x->u.svcb, &y->u.svcb);
    case DNSMSG_BODY_HTTPS:
        return svcb_eq(&x->u.https.svcb, &y->u.https.svcb);
    case DNSMSG_BODY_OPT:
        if (x->u.opt.options.len != y->u.opt.options.len)
            return false;
        for (Int i = 0; i < x->u.opt.options.len; i++) {
            if (x->u.opt.options.p[i].code != y->u.opt.options.p[i].code ||
                !bytes_eq(x->u.opt.options.p[i].data, y->u.opt.options.p[i].data))
                return false;
        }
        return true;
    case DNSMSG_BODY_UNKNOWN:
        return x->u.unknown.type == y->u.unknown.type &&
               bytes_eq(x->u.unknown.data, y->u.unknown.data);
    default:
        return false;
    }
}

static bool resource_eq(const DnsmsgResource *x, const DnsmsgResource *y) {
    return rheader_eq(&x->header, &y->header) && body_eq(&x->body, &y->body);
}

static bool resources_eq(const DnsmsgResources *x, const DnsmsgResources *y) {
    if (x->len != y->len)
        return false;
    for (Int i = 0; i < x->len; i++) {
        if (!resource_eq(&x->p[i], &y->p[i]))
            return false;
    }
    return true;
}

static bool msg_eq(const DnsmsgMessage *x, const DnsmsgMessage *y) {
    if (!header_eq(&x->header, &y->header) || x->questions.len != y->questions.len)
        return false;
    for (Int i = 0; i < x->questions.len; i++) {
        if (!question_eq(&x->questions.p[i], &y->questions.p[i]))
            return false;
    }
    return resources_eq(&x->answers, &y->answers) &&
           resources_eq(&x->authorities, &y->authorities) &&
           resources_eq(&x->additionals, &y->additionals);
}

/* Whether err is Go's nestedError with this s, and what it holds. */
static bool nested(Error err, const char *prefix, Error *inner) {
    Str s;
    Error in;
    if (!DM(error_nested)(err, &s, &in) || !str_eq(s, str_from_cstr(prefix)))
        return false;
    if (inner != NULL)
        *inner = in;
    return true;
}

/* ----------------------------------------------------------- messages */

static DnsmsgMessage small_test_msg(Alloc *a) {
    DnsmsgName name = nm("example.com.");
    DnsmsgMessage m = empty_msg();
    m.header.response = true;
    m.header.authoritative = true;
    m.questions = questions(a, 1);
    m.questions.p[0] = question(name, DNSMSG_TYPE_A);
    m.answers = resources(a, 1);
    m.answers.p[0] = resource(rheader(name, DNSMSG_TYPE_A), body_a(127, 0, 0, 1));
    m.authorities = resources(a, 1);
    m.authorities.p[0] = resource(rheader(name, DNSMSG_TYPE_A), body_a(127, 0, 0, 1));
    m.additionals = resources(a, 1);
    m.additionals.p[0] = resource(rheader(name, DNSMSG_TYPE_A), body_a(127, 0, 0, 1));
    return m;
}

static DnsmsgMessage large_test_msg(Alloc *a) {
    DnsmsgName name = nm("foo.bar.example.com.");
    DnsmsgMessage m = empty_msg();
    m.header.response = true;
    m.header.authoritative = true;
    m.questions = questions(a, 1);
    m.questions.p[0] = question(name, DNSMSG_TYPE_A);

    m.answers = resources(a, 9);
    DnsmsgResource *an = m.answers.p;
    an[0] = resource(rheader(name, DNSMSG_TYPE_A), body_a(127, 0, 0, 1));
    an[1] = resource(rheader(name, DNSMSG_TYPE_A), body_a(127, 0, 0, 2));
    DnsmsgResourceBody b = body_of(DNSMSG_BODY_AAAA);
    for (int i = 0; i < 16; i++)
        b.u.aaaa.aaaa[i] = (Byte)(i + 1);
    an[2] = resource(rheader(name, DNSMSG_TYPE_AAAA), b);
    b = body_of(DNSMSG_BODY_CNAME);
    b.u.cname.cname = nm("alias.example.com.");
    an[3] = resource(rheader(name, DNSMSG_TYPE_CNAME), b);
    b = body_of(DNSMSG_BODY_SOA);
    b.u.soa.ns = nm("ns1.example.com.");
    b.u.soa.mbox = nm("mb.example.com.");
    b.u.soa.serial = 1;
    b.u.soa.refresh = 2;
    b.u.soa.retry = 3;
    b.u.soa.expire = 4;
    b.u.soa.min_ttl = 5;
    an[4] = resource(rheader(name, DNSMSG_TYPE_SOA), b);
    b = body_of(DNSMSG_BODY_PTR);
    b.u.ptr.ptr = nm("ptr.example.com.");
    an[5] = resource(rheader(name, DNSMSG_TYPE_PTR), b);
    b = body_of(DNSMSG_BODY_MX);
    b.u.mx.pref = 7;
    b.u.mx.mx = nm("mx.example.com.");
    an[6] = resource(rheader(name, DNSMSG_TYPE_MX), b);
    b = body_of(DNSMSG_BODY_SRV);
    b.u.srv.priority = 8;
    b.u.srv.weight = 9;
    b.u.srv.port = 11;
    b.u.srv.target = nm("srv.example.com.");
    an[7] = resource(rheader(name, DNSMSG_TYPE_SRV), b);
    b = body_of(DNSMSG_BODY_UNKNOWN);
    b.u.unknown.type = PRIVATE_USE_TYPE;
    b.u.unknown.data = BYTES(a, 42, 0, 43, 44);
    an[8] = resource(rheader(name, PRIVATE_USE_TYPE), b);

    m.authorities = resources(a, 2);
    b = body_of(DNSMSG_BODY_NS);
    b.u.ns.ns = nm("ns1.example.com.");
    m.authorities.p[0] = resource(rheader(name, DNSMSG_TYPE_NS), b);
    b.u.ns.ns = nm("ns2.example.com.");
    m.authorities.p[1] = resource(rheader(name, DNSMSG_TYPE_NS), b);

    m.additionals = resources(a, 3);
    b = body_of(DNSMSG_BODY_TXT);
    b.u.txt.txt = strs(a, 1);
    b.u.txt.txt.p[0] = S("So Long, and Thanks for All the Fish");
    m.additionals.p[0] = resource(rheader(name, DNSMSG_TYPE_TXT), b);
    b.u.txt.txt = strs(a, 1);
    b.u.txt.txt.p[0] = S("Hamster Huey and the Gooey Kablooie");
    m.additionals.p[1] = resource(rheader(name, DNSMSG_TYPE_TXT), b);
    b = body_of(DNSMSG_BODY_OPT);
    b.u.opt.options = options(a, 1);
    b.u.opt.options.p[0].code = 10; /* see RFC 7873 */
    b.u.opt.options.p[0].data =
        BYTES(a, 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef);
    m.additionals.p[2] =
        resource(edns0_header(4096, 0xfe0 | DNSMSG_RCODE_SUCCESS, false), b);
    return m;
}

static DnsmsgMessage build_test_svcb_msg(Alloc *a) {
    DnsmsgName name = nm("foo.bar.example.com.");
    DnsmsgMessage m = empty_msg();
    m.answers = resources(a, 2);
    DnsmsgResourceBody b = body_of(DNSMSG_BODY_SVCB);
    b.u.svcb.priority = 1;
    b.u.svcb.target = nm("svc.example.com.");
    b.u.svcb.params = params(a, 1);
    b.u.svcb.params.p[0] = (DnsmsgSVCParam){DNSMSG_SVC_PARAM_ALPN, LIT(a, "h2")};
    m.answers.p[0] = resource(rheader(name, DNSMSG_TYPE_SVCB), b);
    b = body_of(DNSMSG_BODY_HTTPS);
    b.u.https.svcb.priority = 2;
    b.u.https.svcb.target = nm("https.example.com.");
    b.u.https.svcb.params = params(a, 2);
    b.u.https.svcb.params.p[0] =
        (DnsmsgSVCParam){DNSMSG_SVC_PARAM_PORT, BYTES(a, 0x01, 0xbb)};
    b.u.https.svcb.params.p[1] =
        (DnsmsgSVCParam){DNSMSG_SVC_PARAM_IPV4_HINT, BYTES(a, 192, 0, 2, 1)};
    m.answers.p[1] = resource(rheader(name, DNSMSG_TYPE_HTTPS), b);
    return m;
}

static DnsmsgMessage small_test_msg_with_unknown_resource(Alloc *a) {
    DnsmsgMessage m = empty_msg();
    m.answers = resources(a, 1);
    DnsmsgResourceHeader h = rheader(nm("."), PRIVATE_USE_TYPE);
    h.ttl = 123;
    /* The realType() method is called, when packing, so Type must match the
     * type claimed by the Header above. */
    DnsmsgResourceBody b = body_of(DNSMSG_BODY_UNKNOWN);
    b.u.unknown.type = PRIVATE_USE_TYPE;
    b.u.unknown.data = BYTES(a, 42, 42, 42, 42);
    m.answers.p[0] = resource(h, b);
    return m;
}

/* -------------------------------------------------------------- tests */

static void TestPrintPaddedUint8(TestingT *t) {
    static const struct {
        uint8_t num;
        const char *want;
    } tests[] = {
        {0, "000"},   {1, "001"},   {9, "009"},   {10, "010"},  {99, "099"},
        {100, "100"}, {124, "124"}, {104, "104"}, {120, "120"}, {255, "255"},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Byte buf[3];
        Int n = DM(print_padded_uint8)(buf, tests[i].num);
        Str got = str_from_bytes(buf, n);
        if (!str_eq(got, str_from_cstr(tests[i].want)))
            testing_t_errorf_v(t, "got printPaddedUint8(%d) = %s, want = %s",
                               tests[i].num, got, tests[i].want);
    }
}

static void TestPrintUint8Bytes(TestingT *t) {
    static const uint8_t tests[] = {0, 1, 9, 10, 99, 100, 124, 104, 120, 255};
    for (size_t i = 0; i < sizeof tests; i++) {
        Byte buf[3];
        char want[8];
        snprintf(want, sizeof want, "%u", (unsigned)tests[i]);
        Str got = str_from_bytes(buf, DM(print_uint8_bytes)(buf, tests[i]));
        if (!str_eq(got, str_from_cstr(want)))
            testing_t_errorf_v(t, "got printUint8Bytes(%d) = %s, want = %s", tests[i],
                               got, want);
    }
}

static void TestPrintUint16(TestingT *t) {
    static const uint16_t tests[] = {65535, 0, 1, 10, 100, 1000, 10000, 324, 304, 320};
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Byte buf[10];
        char want[16];
        snprintf(want, sizeof want, "%u", (unsigned)tests[i]);
        Str got = str_from_bytes(buf, DM(print_uint32)(buf, tests[i]));
        if (!str_eq(got, str_from_cstr(want)))
            testing_t_errorf_v(t, "got printUint16(%d) = %s, want = %s", tests[i], got,
                               want);
    }
}

static void TestPrintUint32(TestingT *t) {
    static const uint32_t tests[] = {
        4294967295U, 65535,   0,        1,         10,         100, 1000, 10000,
        100000,      1000000, 10000000, 100000000, 1000000000, 324, 304,  320,
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Byte buf[10];
        char want[16];
        snprintf(want, sizeof want, "%lu", (unsigned long)tests[i]);
        Str got = str_from_bytes(buf, DM(print_uint32)(buf, tests[i]));
        if (!str_eq(got, str_from_cstr(want)))
            testing_t_errorf_v(t, "got printUint32(%d) = %s, want = %s", tests[i], got,
                               want);
    }
}

static void TestNameString(TestingT *t) {
    DnsmsgName name = nm("foo");
    Str got = DM(name_string)(&name);
    if (!str_eq(got, S("foo")))
        testing_t_errorf_v(t, "got fmt.Sprint(name) = %s, want = foo", got);
}

static void TestQuestionPackUnpack(TestingT *t) {
    ARENA_BEGIN;
    DnsmsgQuestion want = question(nm("."), DNSMSG_TYPE_A);
    Byte mem[50] = {0};
    DnsmsgBuf buf = DM(buf)(a, slice_from(mem, 1, 50, TYPE_BYTE));
    DnsmsgCompression *c = DM(compression_new)(a);
    Error e = DM(question_pack)(&want, &buf, c, 1);
    if (BURROW_FAILED(e)) {
        testing_t_fatalf_v(t, "Question.pack() = %v", e);
        ARENA_END;
        return;
    }
    DnsmsgParser p;
    memset(&p, 0, sizeof p);
    p.msg = DM(buf_slice)(&buf);
    p.header.questions = 1;
    p.section = DNSMSG_SECTION_QUESTIONS;
    p.off = 1;
    DnsmsgQuestion got;
    e = DM(parser_question)(&p, &got);
    if (BURROW_FAILED(e)) {
        testing_t_fatalf_v(t, "Parser.Question() = %v", e);
        ARENA_END;
        return;
    }
    if (p.off != buf.len)
        testing_t_errorf_v(t,
                           "unpacked different amount than packed: got = %d, want = %d",
                           p.off, buf.len);
    if (!question_eq(&got, &want))
        testing_t_errorf_v(t, "got from Parser.Question() = %s, want = %s",
                           DM(question_go_string)(a, &got),
                           DM(question_go_string)(a, &want));
    DM(compression_free)(c);
    ARENA_END;
}

static void TestName(TestingT *t) {
    static const char *const tests[] = {
        "",
        ".",
        "google..com",
        "google.com",
        "google..com.",
        "google.com.",
        ".google.com.",
        "www..google.com.",
        "www.google.com.",
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        DnsmsgName n;
        Error e = DM(new_name)(str_from_cstr(tests[i]), &n);
        if (BURROW_FAILED(e)) {
            testing_t_errorf_v(t, "NewName(%q) = %v", tests[i], e);
            continue;
        }
        Str ns = DM(name_string)(&n);
        if (!str_eq(ns, str_from_cstr(tests[i])))
            testing_t_errorf_v(t, "got %q.String() = %q, want = %q", tests[i], ns,
                               tests[i]);
    }
}

static void TestNameWithDotsUnpack(TestingT *t) {
    static const Byte name[] = {3, 'w', '.', 'w', 2, 'g', 'o', 3, 'd', 'e', 'v', 0};
    DnsmsgName n;
    Int off;
    Error e = DM(name_unpack)(&n, VIEW(name), 0, &off);
    if (!is(e, DM(err_invalid_name)))
        testing_t_fatalf_v(t, "expected %v, got %v", DM(err_invalid_name), e);
}

static void TestNamePackUnpack(TestingT *t) {
    ARENA_BEGIN;
    static const char suffix[] = ".go.dev.";
    const Int slen = (Int)sizeof suffix - 1;
    char prefix[401];
    for (int i = 0; i < 20; i++)
        memcpy(prefix + i * 20, "verylongdomainlabel.", 20);
    prefix[400] = 0;
    char long254[256], long255[256];
    memcpy(long254, prefix, (size_t)(254 - slen));
    memcpy(long254 + 254 - slen, suffix, (size_t)slen + 1);
    memcpy(long255, prefix, (size_t)(255 - slen));
    memcpy(long255 + 255 - slen, suffix, (size_t)slen + 1);

    const struct {
        const char *in;
        Error err;
    } tests[] = {
        {"", DM(err_non_canonical_name)},
        {".", BURROW_NO_ERROR},
        {"google..com", DM(err_non_canonical_name)},
        {"google.com", DM(err_non_canonical_name)},
        {"google..com.", DM(err_zero_seg_len)},
        {"google.com.", BURROW_NO_ERROR},
        {".google.com.", DM(err_zero_seg_len)},
        {"www..google.com.", DM(err_zero_seg_len)},
        {"www.google.com.", BURROW_NO_ERROR},
        {long254, BURROW_NO_ERROR},       /* 254B name, with ending dot. */
        {long255, DM(err_name_too_long)}, /* 255B name, with ending dot. */
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        DnsmsgName in = nm(tests[i].in);
        Byte mem[30];
        DnsmsgBuf buf = DM(buf)(a, slice_from(mem, 0, 30, TYPE_BYTE));
        DnsmsgCompression *c = DM(compression_new)(a);
        Error e = DM(name_pack)(&in, &buf, c, 0);
        DM(compression_free)(c);
        if (!is(e, tests[i].err)) {
            testing_t_errorf_v(t, "got %q.pack() = %v, want = %v", tests[i].in, e,
                               tests[i].err);
            continue;
        }
        if (BURROW_FAILED(tests[i].err))
            continue;
        DnsmsgName got;
        Int n;
        e = DM(name_unpack)(&got, DM(buf_slice)(&buf), 0, &n);
        if (BURROW_FAILED(e)) {
            testing_t_errorf_v(t, "%q.unpack() = %v", tests[i].in, e);
            continue;
        }
        if (n != buf.len)
            testing_t_errorf_v(t,
                               "unpacked different amount than packed for %q: got = "
                               "%d, want = %d",
                               tests[i].in, n, buf.len);
        if (!name_eq(&got, &in))
            testing_t_errorf_v(t, "unpacking packing of %q: got = %q, want = %q",
                               tests[i].in, DM(name_string)(&got),
                               DM(name_string)(&in));
    }
    ARENA_END;
}

/* prepName: 18 labels of "longdnslabel", one of a's to make up the length,
 * and go.dev. */
static Int prep_name(Byte *out, int length) {
    static const Byte suffix[] = {2, 'g', 'o', 3, 'd', 'e', 'v', 0};
    Int n = 0;
    for (int i = 0; i < 18; i++) {
        out[n++] = 12;
        memcpy(out + n, "longdnslabel", 12);
        n += 12;
    }
    int missing = length - ((int)n + (int)sizeof suffix + 1);
    out[n++] = (Byte)missing;
    memset(out + n, 'a', (size_t)missing);
    n += missing;
    memcpy(out + n, suffix, sizeof suffix);
    return n + (Int)sizeof suffix;
}

static void TestNameUnpackTooLongName(TestingT *t) {
    const struct {
        int length;
        Error err;
    } tests[] = {
        {255, BURROW_NO_ERROR},
        {256, DM(err_name_too_long)},
        /* too large to be valid, return error during unpack. */
        {300, DM(err_name_too_long)},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Byte name[400];
        Int n = prep_name(name, tests[i].length);
        DnsmsgName got;
        Int off;
        Error e = DM(name_unpack)(&got, view(name, n), 0, &off);
        if (!is(e, tests[i].err))
            testing_t_errorf_v(t, "%d: expected error: %v, got %v", (int)i,
                               tests[i].err, e);
    }
}

static void TestHeaderUnpackError(TestingT *t) {
    static const char *const wants[] = {"id",      "bits",        "questions",
                                        "answers", "authorities", "additionals"};
    Byte buf[12] = {0};
    for (int i = 0; i < 6; i++) {
        DnsmsgWireHeader h;
        Int n;
        Error e = DM(wire_header_unpack)(&h, view(buf, (Int)i * 2), 0, &n);
        if (n != 0 || !nested(e, wants[i], NULL))
            testing_t_errorf_v(t,
                               "got header.unpack([%d]byte, 0) = %d, %v, want = 0, %s",
                               i * 2, n, e, wants[i]);
    }
}

static void TestParserStart(TestingT *t) {
    DnsmsgParser p;
    memset(&p, 0, sizeof p);
    for (int i = 0; i <= 1; i++) {
        DnsmsgHeader h;
        Error e = DM(parser_start)(&p, slice_nil(TYPE_BYTE), &h);
        if (!nested(e, "unpacking header", NULL))
            testing_t_errorf_v(t, "got Parser.Start(nil) = _, %v, want = _, %s", e,
                               "unpacking header");
    }
}

static void TestResourceNotStarted(TestingT *t) {
    ARENA_BEGIN;
    DnsmsgParser p;
    memset(&p, 0, sizeof p);
    DnsmsgResourceBody b;
#define NOT_STARTED(label, call)                                                       \
    do {                                                                               \
        memset(&p, 0, sizeof p);                                                       \
        Error e_ = (call);                                                             \
        if (!is(e_, DM(err_not_started)))                                              \
            testing_t_errorf_v(t, "got Parser.%s() = _ , %v, want = _, %v", label, e_, \
                               DM(err_not_started));                                   \
    } while (0)
    NOT_STARTED("CNAMEResource", DM(parser_cname_resource)(&p, &b.u.cname));
    NOT_STARTED("MXResource", DM(parser_mx_resource)(&p, &b.u.mx));
    NOT_STARTED("NSResource", DM(parser_ns_resource)(&p, &b.u.ns));
    NOT_STARTED("PTRResource", DM(parser_ptr_resource)(&p, &b.u.ptr));
    NOT_STARTED("SOAResource", DM(parser_soa_resource)(&p, &b.u.soa));
    NOT_STARTED("TXTResource", DM(parser_txt_resource)(&p, a, &b.u.txt));
    NOT_STARTED("SRVResource", DM(parser_srv_resource)(&p, &b.u.srv));
    NOT_STARTED("AResource", DM(parser_a_resource)(&p, &b.u.a));
    NOT_STARTED("AAAAResource", DM(parser_aaaa_resource)(&p, &b.u.aaaa));
    NOT_STARTED("UnknownResource", DM(parser_unknown_resource)(&p, a, &b.u.unknown));
#undef NOT_STARTED
    ARENA_END;
}

static void TestDNSPackUnpack(TestingT *t) {
    ARENA_BEGIN;
    DnsmsgMessage wants[3];
    wants[0] = empty_msg();
    wants[0].questions = questions(a, 1);
    wants[0].questions.p[0] = question(nm("."), DNSMSG_TYPE_AAAA);
    wants[1] = large_test_msg(a);
    wants[2] = build_test_svcb_msg(a);
    for (int i = 0; i < 3; i++) {
        Slice b;
        Error e = DM(message_pack)(&wants[i], a, &b);
        if (BURROW_FAILED(e)) {
            testing_t_fatalf_v(t, "%d: Message.Pack() = %v", i, e);
            break;
        }
        DnsmsgMessage got;
        e = DM(message_unpack)(&got, a, b);
        if (BURROW_FAILED(e)) {
            testing_t_fatalf_v(t, "%d: Message.Unapck() = %v", i, e);
            break;
        }
        if (!msg_eq(&got, &wants[i])) {
            testing_t_errorf_v(
                t, "%d: Message.Pack/Unpack() roundtrip: got = %s, want = %s", i,
                DM(message_go_string)(a, &got), DM(message_go_string)(a, &wants[i]));
            if (got.answers.len > 0 && wants[i].answers.len > 0 &&
                !body_eq(&got.answers.p[0].body, &wants[i].answers.p[0].body))
                testing_t_errorf_v(t, "Answer 0 Body mismatch");
        }
    }
    ARENA_END;
}

static void TestDNSAppendPackUnpack(TestingT *t) {
    ARENA_BEGIN;
    DnsmsgMessage wants[2];
    wants[0] = empty_msg();
    wants[0].questions = questions(a, 1);
    wants[0].questions.p[0] = question(nm("."), DNSMSG_TYPE_AAAA);
    wants[1] = large_test_msg(a);
    for (int i = 0; i < 2; i++) {
        Byte *mem = (Byte *)mem_alloc(a, 514, 1);
        Slice b;
        Error e = DM(message_append_pack)(&wants[i], a,
                                          slice_from(mem, 2, 514, TYPE_BYTE), &b);
        if (BURROW_FAILED(e)) {
            testing_t_fatalf_v(t, "%d: Message.AppendPack() = %v", i, e);
            break;
        }
        b = slice_from((Byte *)b.p + 2, b.len - 2, b.cap - 2, TYPE_BYTE);
        DnsmsgMessage got;
        e = DM(message_unpack)(&got, a, b);
        if (BURROW_FAILED(e)) {
            testing_t_fatalf_v(t, "%d: Message.Unapck() = %v", i, e);
            break;
        }
        if (!msg_eq(&got, &wants[i]))
            testing_t_errorf_v(
                t, "%d: Message.AppendPack/Unpack() roundtrip: got = %s, want = %s", i,
                DM(message_go_string)(a, &got), DM(message_go_string)(a, &wants[i]));
    }
    ARENA_END;
}

/* Pack m, or fail the test. */
static bool pack(TestingT *t, Alloc *a, DnsmsgMessage *m, Slice *out) {
    Error e = DM(message_pack)(m, a, out);
    if (BURROW_FAILED(e)) {
        testing_t_fatalf_v(t, "Message.Pack() = %v", e);
        return false;
    }
    return true;
}

static bool start(TestingT *t, DnsmsgParser *p, Slice buf) {
    memset(p, 0, sizeof *p);
    DnsmsgHeader h;
    Error e = DM(parser_start)(p, buf, &h);
    if (BURROW_FAILED(e)) {
        testing_t_fatalf_v(t, "Parser.Start(non-nil) = %v", e);
        return false;
    }
    return true;
}

static void TestSkipAll(TestingT *t) {
    ARENA_BEGIN;
    DnsmsgMessage msg = large_test_msg(a);
    Slice buf;
    DnsmsgParser p;
    if (!pack(t, a, &msg, &buf) || !start(t, &p, buf)) {
        ARENA_END;
        return;
    }
    static const struct {
        const char *name;
        Error (*f)(DnsmsgParser *);
    } tests[] = {
        {"SkipAllQuestions", DM(parser_skip_all_questions)},
        {"SkipAllAnswers", DM(parser_skip_all_answers)},
        {"SkipAllAuthorities", DM(parser_skip_all_authorities)},
        {"SkipAllAdditionals", DM(parser_skip_all_additionals)},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        for (int j = 1; j <= 3; j++) {
            Error e = tests[i].f(&p);
            if (BURROW_FAILED(e))
                testing_t_errorf_v(t, "%d: Parser.%s() = %v", j, tests[i].name, e);
        }
    }
    ARENA_END;
}

static void TestSkipEach(TestingT *t) {
    ARENA_BEGIN;
    DnsmsgMessage msg = small_test_msg(a);
    Slice buf;
    DnsmsgParser p;
    if (!pack(t, a, &msg, &buf) || !start(t, &p, buf)) {
        ARENA_END;
        return;
    }
    static const struct {
        const char *name;
        Error (*f)(DnsmsgParser *);
    } tests[] = {
        {"SkipQuestion", DM(parser_skip_question)},
        {"SkipAnswer", DM(parser_skip_answer)},
        {"SkipAuthority", DM(parser_skip_authority)},
        {"SkipAdditional", DM(parser_skip_additional)},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Error e = tests[i].f(&p);
        if (BURROW_FAILED(e))
            testing_t_errorf_v(t, "first Parser.%s() = %v, want = nil", tests[i].name,
                               e);
        e = tests[i].f(&p);
        if (!is(e, DM(err_section_done)))
            testing_t_errorf_v(t, "second Parser.%s() = %v, want = %v", tests[i].name,
                               e, DM(err_section_done));
    }
    ARENA_END;
}

static void TestSkipAfterRead(TestingT *t) {
    ARENA_BEGIN;
    DnsmsgMessage msg = small_test_msg(a);
    Slice buf;
    DnsmsgParser p;
    if (!pack(t, a, &msg, &buf) || !start(t, &p, buf)) {
        ARENA_END;
        return;
    }
    static const struct {
        const char *name;
        Error (*skip)(DnsmsgParser *);
    } tests[] = {
        {"Question", DM(parser_skip_question)},
        {"Answer", DM(parser_skip_answer)},
        {"Authority", DM(parser_skip_authority)},
        {"Additional", DM(parser_skip_additional)},
    };
    for (int i = 0; i < 4; i++) {
        DnsmsgQuestion q;
        DnsmsgResource r;
        Error e;
        if (i == 0)
            e = DM(parser_question)(&p, &q);
        else if (i == 1)
            e = DM(parser_answer)(&p, a, &r);
        else if (i == 2)
            e = DM(parser_authority)(&p, a, &r);
        else
            e = DM(parser_additional)(&p, a, &r);
        if (BURROW_FAILED(e))
            testing_t_errorf_v(t, "got Parser.%s() = _, %v, want = _, nil",
                               tests[i].name, e);
        e = tests[i].skip(&p);
        if (!is(e, DM(err_section_done)))
            testing_t_errorf_v(t, "got Parser.Skip%s() = %v, want = %v", tests[i].name,
                               e, DM(err_section_done));
    }
    ARENA_END;
}

static void TestSkipNotStarted(TestingT *t) {
    static const struct {
        const char *name;
        Error (*f)(DnsmsgParser *);
    } tests[] = {
        {"SkipAllQuestions", DM(parser_skip_all_questions)},
        {"SkipAllAnswers", DM(parser_skip_all_answers)},
        {"SkipAllAuthorities", DM(parser_skip_all_authorities)},
        {"SkipAllAdditionals", DM(parser_skip_all_additionals)},
    };
    DnsmsgParser p;
    memset(&p, 0, sizeof p);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Error e = tests[i].f(&p);
        if (!is(e, DM(err_not_started)))
            testing_t_errorf_v(t, "got Parser.%s() = %v, want = %v", tests[i].name, e,
                               DM(err_not_started));
    }
}

static void TestTooManyRecords(TestingT *t) {
    ARENA_BEGIN;
    /* The count is checked before anything is read, so the arrays need not be
     * there. */
    const Int recs = 0xFFFF + 1;
    for (int i = 0; i < 4; i++) {
        DnsmsgMessage m = empty_msg();
        Error want;
        const char *name;
        if (i == 0) {
            m.questions.len = recs;
            want = DM(err_too_many_questions);
            name = "Questions";
        } else if (i == 1) {
            m.answers.len = recs;
            want = DM(err_too_many_answers);
            name = "Answers";
        } else if (i == 2) {
            m.authorities.len = recs;
            want = DM(err_too_many_authorities);
            name = "Authorities";
        } else {
            m.additionals.len = recs;
            want = DM(err_too_many_additionals);
            name = "Additionals";
        }
        Slice out;
        Error got = DM(message_pack)(&m, a, &out);
        if (!is(got, want))
            testing_t_errorf_v(t, "got Message.Pack() for %d %s = %v, want = %v", recs,
                               name, got, want);
    }
    ARENA_END;
}

static void TestVeryLongTxt(TestingT *t) {
    ARENA_BEGIN;
    static char dots[256];
    memset(dots, '.', 255);
    DnsmsgResourceBody body = body_of(DNSMSG_BODY_TXT);
    body.u.txt.txt = strs(a, 7);
    Str *s = body.u.txt.txt.p;
    s[0] = S("");
    s[1] = S("");
    s[2] = S("foo bar");
    s[3] = S("");
    s[4] = S("www.example.com");
    s[5] = S("www.example.com.");
    s[6] = (Str){(const Byte *)dots, 255};
    DnsmsgResource want =
        resource(rheader(nm("foo.bar.example.com."), DNSMSG_TYPE_TXT), body);
    Byte *mem = (Byte *)mem_alloc(a, 8000, 1);
    DnsmsgBuf buf = DM(buf)(a, slice_from(mem, 0, 8000, TYPE_BYTE));
    DnsmsgCompression *c = DM(compression_new)(a);
    Error e = DM(resource_pack)(&want, &buf, c, 0);
    DM(compression_free)(c);
    if (BURROW_FAILED(e)) {
        testing_t_fatalf_v(t, "Resource.pack() = %v", e);
        ARENA_END;
        return;
    }
    DnsmsgResource got;
    Int off;
    e = DM(resource_header_unpack)(&got.header, DM(buf_slice)(&buf), 0, &off);
    if (BURROW_FAILED(e)) {
        testing_t_fatalf_v(t, "ResourceHeader.unpack() = %v", e);
        ARENA_END;
        return;
    }
    Int n;
    e = DM(unpack_resource_body)(a, DM(buf_slice)(&buf), off, got.header, &got.body,
                                 &n);
    if (BURROW_FAILED(e)) {
        testing_t_fatalf_v(t, "unpackResourceBody() = %v", e);
        ARENA_END;
        return;
    }
    if (n != buf.len)
        testing_t_errorf_v(t,
                           "unpacked different amount than packed: got = %d, want = %d",
                           n, buf.len);
    if (!resource_eq(&got, &want))
        testing_t_errorf_v(t, "Resource.pack/unpack() roundtrip: got = %s, want = %s",
                           DM(resource_go_string)(a, &got),
                           DM(resource_go_string)(a, &want));
    ARENA_END;
}

static void TestTooLongTxt(TestingT *t) {
    ARENA_BEGIN;
    static char dots[257];
    memset(dots, '.', 256);
    DnsmsgResourceBody rb = body_of(DNSMSG_BODY_TXT);
    rb.u.txt.txt = strs(a, 1);
    rb.u.txt.txt.p[0] = (Str){(const Byte *)dots, 256};
    Byte *mem = (Byte *)mem_alloc(a, 8000, 1);
    DnsmsgBuf buf = DM(buf)(a, slice_from(mem, 0, 8000, TYPE_BYTE));
    DnsmsgCompression *c = DM(compression_new)(a);
    Error e = DM(body_pack)(&rb, &buf, c, 0);
    DM(compression_free)(c);
    if (!is(e, DM(err_string_too_long)))
        testing_t_errorf_v(t,
                           "packing TXTResource with 256 character string: got err = "
                           "%v, want = %v",
                           e, DM(err_string_too_long));
    ARENA_END;
}

static void TestStartAppends(TestingT *t) {
    ARENA_BEGIN;
    Byte *mem = (Byte *)mem_alloc(a, 514, 1);
    mem[0] = 4;
    mem[1] = 44;
    DnsmsgHeader h;
    memset(&h, 0, sizeof h);
    DnsmsgBuilder b;
    DM(new_builder)(&b, a, slice_from(mem, 2, 514, TYPE_BYTE), h);
    DM(builder_enable_compression)(&b);
    Slice buf;
    Error e = DM(builder_finish)(&b, &buf);
    if (BURROW_FAILED(e)) {
        testing_t_fatalf_v(t, "Builder.Finish() = %v", e);
        DM(builder_free)(&b);
        ARENA_END;
        return;
    }
    if (buf.len != HEADER_LEN + 2)
        testing_t_errorf_v(t, "got len(buf) = %d, want = %d", buf.len, HEADER_LEN + 2);
    const Byte *got = (const Byte *)buf.p;
    if (got[0] != 4 || got[1] != 44)
        testing_t_errorf_v(t, "original data not preserved, got = %d %d, want = 4 44",
                           got[0], got[1]);
    DM(builder_free)(&b);
    ARENA_END;
}

static void TestStartError(TestingT *t) {
    static const struct {
        const char *name;
        Error (*fn)(DnsmsgBuilder *);
    } tests[] = {
        {"Questions", DM(builder_start_questions)},
        {"Answers", DM(builder_start_answers)},
        {"Authorities", DM(builder_start_authorities)},
        {"Additionals", DM(builder_start_additionals)},
    };
    const struct {
        const char *name;
        DnsmsgSection section;
        Error want;
    } envs[] = {
        {"sectionNotStarted", DNSMSG_SECTION_NOT_STARTED, DM(err_not_started)},
        {"sectionDone", DNSMSG_SECTION_DONE, DM(err_section_done)},
    };
    for (size_t i = 0; i < sizeof envs / sizeof envs[0]; i++) {
        for (size_t j = 0; j < sizeof tests / sizeof tests[0]; j++) {
            DnsmsgBuilder b;
            memset(&b, 0, sizeof b);
            b.section = envs[i].section;
            Error got = tests[j].fn(&b);
            if (!is(got, envs[i].want))
                testing_t_errorf_v(t, "got Builder{%s}.Start%s() = %v, want = %v",
                                   envs[i].name, tests[j].name, got, envs[i].want);
        }
    }
}

static void TestBuilderResourceError(TestingT *t) {
    const struct {
        const char *name;
        DnsmsgSection section;
        Error want;
    } envs[] = {
        {"sectionNotStarted", DNSMSG_SECTION_NOT_STARTED, DM(err_not_started)},
        {"sectionHeader", DNSMSG_SECTION_HEADER, DM(err_not_started)},
        {"sectionQuestions", DNSMSG_SECTION_QUESTIONS, DM(err_not_started)},
        {"sectionDone", DNSMSG_SECTION_DONE, DM(err_section_done)},
    };
    for (size_t i = 0; i < sizeof envs / sizeof envs[0]; i++) {
        DnsmsgResourceHeader h;
        memset(&h, 0, sizeof h);
        DnsmsgResourceBody body = body_of(DNSMSG_BODY_NONE);
        /* Keep it sorted by resource type name. */
#define RESOURCE_ERROR(label, fn)                                                      \
    do {                                                                               \
        DnsmsgBuilder b;                                                               \
        memset(&b, 0, sizeof b);                                                       \
        b.section = envs[i].section;                                                   \
        Error got_ = DM(fn)(&b, h, (const void *)&body.u);                             \
        if (!is(got_, envs[i].want))                                                   \
            testing_t_errorf_v(t, "got Builder{%s}.%s() = %v, want = %v",              \
                               envs[i].name, label, got_, envs[i].want);               \
    } while (0)
        RESOURCE_ERROR("AResource", builder_a_resource);
        RESOURCE_ERROR("AAAAResource", builder_aaaa_resource);
        RESOURCE_ERROR("CNAMEResource", builder_cname_resource);
        RESOURCE_ERROR("HTTPSResource", builder_https_resource);
        RESOURCE_ERROR("MXResource", builder_mx_resource);
        RESOURCE_ERROR("NSResource", builder_ns_resource);
        RESOURCE_ERROR("OPTResource", builder_opt_resource);
        RESOURCE_ERROR("PTRResource", builder_ptr_resource);
        RESOURCE_ERROR("SOAResource", builder_soa_resource);
        RESOURCE_ERROR("SRVResource", builder_srv_resource);
        RESOURCE_ERROR("SVCBResource", builder_svcb_resource);
        RESOURCE_ERROR("TXTResource", builder_txt_resource);
        RESOURCE_ERROR("UnknownResource", builder_unknown_resource);
#undef RESOURCE_ERROR
    }
}

static void TestFinishError(TestingT *t) {
    DnsmsgBuilder b;
    memset(&b, 0, sizeof b);
    Slice out;
    Error got = DM(builder_finish)(&b, &out);
    if (!is(got, DM(err_not_started)))
        testing_t_errorf_v(t, "got Builder.Finish() = %v, want = %v", got,
                           DM(err_not_started));
}

/* The Builder call for r's body, as TestBuilder's switch makes it. */
static Error build_resource(DnsmsgBuilder *b, const DnsmsgResource *r) {
    const DnsmsgResourceBody *body = &r->body;
    switch (body->kind) {
    case DNSMSG_BODY_A:
        return DM(builder_a_resource)(b, r->header, &body->u.a);
    case DNSMSG_BODY_NS:
        return DM(builder_ns_resource)(b, r->header, &body->u.ns);
    case DNSMSG_BODY_CNAME:
        return DM(builder_cname_resource)(b, r->header, &body->u.cname);
    case DNSMSG_BODY_SOA:
        return DM(builder_soa_resource)(b, r->header, &body->u.soa);
    case DNSMSG_BODY_PTR:
        return DM(builder_ptr_resource)(b, r->header, &body->u.ptr);
    case DNSMSG_BODY_MX:
        return DM(builder_mx_resource)(b, r->header, &body->u.mx);
    case DNSMSG_BODY_TXT:
        return DM(builder_txt_resource)(b, r->header, &body->u.txt);
    case DNSMSG_BODY_AAAA:
        return DM(builder_aaaa_resource)(b, r->header, &body->u.aaaa);
    case DNSMSG_BODY_SRV:
        return DM(builder_srv_resource)(b, r->header, &body->u.srv);
    case DNSMSG_BODY_SVCB:
        return DM(builder_svcb_resource)(b, r->header, &body->u.svcb);
    case DNSMSG_BODY_HTTPS:
        return DM(builder_https_resource)(b, r->header, &body->u.https);
    case DNSMSG_BODY_OPT:
        return DM(builder_opt_resource)(b, r->header, &body->u.opt);
    case DNSMSG_BODY_UNKNOWN:
        return DM(builder_unknown_resource)(b, r->header, &body->u.unknown);
    case DNSMSG_BODY_NONE:
    default:
        return DM(err_nil_resource_body);
    }
}

/* Build msg the way TestBuilder does, compressing names. */
static Error build_msg(Alloc *a, const DnsmsgMessage *msg, Slice *out) {
    DnsmsgBuilder b;
    Error e = DM(new_builder)(&b, a, slice_nil(TYPE_BYTE), msg->header);
    if (BURROW_OK(e))
        e = DM(builder_enable_compression)(&b);
    if (BURROW_OK(e))
        e = DM(builder_start_questions)(&b);
    for (Int i = 0; i < msg->questions.len && BURROW_OK(e); i++)
        e = DM(builder_question)(&b, &msg->questions.p[i]);
    if (BURROW_OK(e))
        e = DM(builder_start_answers)(&b);
    for (Int i = 0; i < msg->answers.len && BURROW_OK(e); i++)
        e = build_resource(&b, &msg->answers.p[i]);
    if (BURROW_OK(e))
        e = DM(builder_start_authorities)(&b);
    for (Int i = 0; i < msg->authorities.len && BURROW_OK(e); i++)
        e = build_resource(&b, &msg->authorities.p[i]);
    if (BURROW_OK(e))
        e = DM(builder_start_additionals)(&b);
    for (Int i = 0; i < msg->additionals.len && BURROW_OK(e); i++)
        e = build_resource(&b, &msg->additionals.p[i]);
    if (BURROW_OK(e))
        e = DM(builder_finish)(&b, out);
    DM(builder_free)(&b);
    return e;
}

static void TestBuilder(TestingT *t) {
    ARENA_BEGIN;
    DnsmsgMessage msg = large_test_msg(a);
    Slice want, got;
    if (!pack(t, a, &msg, &want)) {
        ARENA_END;
        return;
    }
    Error e = build_msg(a, &msg, &got);
    if (BURROW_FAILED(e)) {
        testing_t_fatalf_v(t, "building = %v", e);
        ARENA_END;
        return;
    }
    if (!bytes_eq(got, want))
        testing_t_fatalf_v(t, "got from Builder.Finish() = %q\nwant = %q",
                           str_from_bytes((const Byte *)got.p, got.len),
                           str_from_bytes((const Byte *)want.p, want.len));
    ARENA_END;
}

static void TestResourcePack(TestingT *t) {
    ARENA_BEGIN;
    for (int i = 0; i < 3; i++) {
        DnsmsgMessage m = empty_msg();
        m.questions = questions(a, 1);
        m.questions.p[0] = question(nm("."), i == 2 ? DNSMSG_TYPE_A : DNSMSG_TYPE_AAAA);
        DnsmsgResourceHeader zero;
        memset(&zero, 0, sizeof zero);
        if (i == 0) {
            m.answers = resources(a, 1);
            m.answers.p[0] = resource(zero, body_of(DNSMSG_BODY_NONE));
        } else if (i == 1) {
            /* Go's (*NSResource)(nil) is a body, so the header is what fails. */
            m.authorities = resources(a, 1);
            m.authorities.p[0] = resource(zero, body_of(DNSMSG_BODY_NS));
        } else {
            m.additionals = resources(a, 1);
            m.additionals.p[0] = resource(zero, body_of(DNSMSG_BODY_NONE));
        }
        Slice out;
        Error e = DM(message_pack)(&m, a, &out);
        Error in = BURROW_NO_ERROR, in2 = BURROW_NO_ERROR, in3 = BURROW_NO_ERROR;
        bool ok;
        if (i == 0)
            ok = nested(e, "packing Answer", &in) && is(in, DM(err_nil_resource_body));
        else if (i == 1)
            ok = nested(e, "packing Authority", &in) &&
                 nested(in, "ResourceHeader", &in2) && nested(in2, "Name", &in3) &&
                 is(in3, DM(err_non_canonical_name));
        else
            ok = nested(e, "packing Additional", &in) &&
                 is(in, DM(err_nil_resource_body));
        if (!ok)
            testing_t_errorf_v(t, "%d: got Message.Pack() = %v", i, e);
    }
    ARENA_END;
}

static void TestResourcePackLength(TestingT *t) {
    ARENA_BEGIN;
    DnsmsgResource r = resource(rheader(nm("."), DNSMSG_TYPE_A), body_a(127, 0, 0, 2));
    DnsmsgBuf hb = DM(buf)(a, slice_nil(TYPE_BYTE));
    Int len_off;
    Error e = DM(resource_header_pack)(&r.header, &hb, NULL, 0, &len_off);
    if (BURROW_FAILED(e)) {
        testing_t_fatalf_v(t, "ResourceHeader.pack() = %v", e);
        ARENA_END;
        return;
    }
    Byte *mem = (Byte *)mem_alloc(a, (size_t)hb.len, 1);
    DnsmsgBuf buf = DM(buf)(a, slice_from(mem, 0, hb.len, TYPE_BYTE));
    e = DM(resource_pack)(&r, &buf, NULL, 0);
    if (BURROW_FAILED(e)) {
        testing_t_fatalf_v(t, "Resource.pack() = %v", e);
        ARENA_END;
        return;
    }
    DnsmsgResourceHeader hdr;
    Int off;
    e = DM(resource_header_unpack)(&hdr, DM(buf_slice)(&buf), 0, &off);
    if (BURROW_FAILED(e)) {
        testing_t_fatalf_v(t, "ResourceHeader.unpack() = %v", e);
        ARENA_END;
        return;
    }
    if ((Int)hdr.length != buf.len - hb.len)
        testing_t_errorf_v(t, "got hdr.Length = %d, want = %d", hdr.length,
                           buf.len - hb.len);
    DM(buf_free)(&buf);
    DM(buf_free)(&hb);
    ARENA_END;
}

static void TestOptionPackUnpack(TestingT *t) {
    ARENA_BEGIN;
    static const Byte w0[] = {0x00, 0x00, 0x29, 0x10, 0x00, 0xfe,
                              0x00, 0x80, 0x00, 0x00, 0x00};
    static const Byte w1[] = {0x00, 0x00, 0x29, 0x10, 0x00, 0xff, 0x00, 0x00,
                              0x00, 0x00, 0x0c, 0x00, 0x0c, 0x00, 0x02, 0x00,
                              0x00, 0x00, 0x0b, 0x00, 0x02, 0x12, 0x34};
    static const Byte w2[] = {0x00, 0x00, 0x29, 0x10, 0x00, 0xff, 0x00, 0x00, 0x00,
                              0x00, 0x06, 0x00, 0x0b, 0x00, 0x02, 0x12, 0x34, 0x00,
                              0x00, 0x29, 0x10, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00,
                              0x06, 0x00, 0x0c, 0x00, 0x02, 0x00, 0x00};
    static const char *const names[] = {
        "without EDNS(0) options",
        "with EDNS(0) options",
        /* Containing multiple OPT resources in a message is invalid, but it's
         * necessary for protocol conformance testing. */
        "with multiple OPT resources",
    };
    const Slice ws[] = {VIEW(w0), VIEW(w1), VIEW(w2)};
    DnsmsgMessage ms[3];
    for (int i = 0; i < 3; i++)
        ms[i] = empty_msg();

    ms[0].header.rcode = DNSMSG_RCODE_FORMAT_ERROR;
    ms[0].questions = questions(a, 1);
    ms[0].questions.p[0] = question(nm("."), DNSMSG_TYPE_A);
    ms[0].additionals = resources(a, 1);
    ms[0].additionals.p[0] =
        resource(edns0_header(4096, 0xfe0 | DNSMSG_RCODE_FORMAT_ERROR, true),
                 body_of(DNSMSG_BODY_OPT));

    ms[1].header.rcode = DNSMSG_RCODE_SERVER_FAILURE;
    ms[1].questions = questions(a, 1);
    ms[1].questions.p[0] = question(nm("."), DNSMSG_TYPE_AAAA);
    ms[1].additionals = resources(a, 1);
    DnsmsgResourceBody b = body_of(DNSMSG_BODY_OPT);
    b.u.opt.options = options(a, 2);
    b.u.opt.options.p[0] = (DnsmsgOption){12, BYTES(a, 0x00, 0x00)}; /* see RFC 7828 */
    b.u.opt.options.p[1] = (DnsmsgOption){11, BYTES(a, 0x12, 0x34)}; /* see RFC 7830 */
    ms[1].additionals.p[0] =
        resource(edns0_header(4096, 0xff0 | DNSMSG_RCODE_SERVER_FAILURE, false), b);

    ms[2].header.rcode = DNSMSG_RCODE_NAME_ERROR;
    ms[2].questions = questions(a, 1);
    ms[2].questions.p[0] = question(nm("."), DNSMSG_TYPE_AAAA);
    ms[2].additionals = resources(a, 2);
    b = body_of(DNSMSG_BODY_OPT);
    b.u.opt.options = options(a, 1);
    b.u.opt.options.p[0] = (DnsmsgOption){11, BYTES(a, 0x12, 0x34)}; /* see RFC 7830 */
    ms[2].additionals.p[0] =
        resource(edns0_header(4096, 0xff0 | DNSMSG_RCODE_NAME_ERROR, false), b);
    b.u.opt.options = options(a, 1);
    b.u.opt.options.p[0] = (DnsmsgOption){12, BYTES(a, 0x00, 0x00)}; /* see RFC 7828 */
    ms[2].additionals.p[1] =
        resource(edns0_header(4096, 0xff0 | DNSMSG_RCODE_NAME_ERROR, false), b);

    for (int i = 0; i < 3; i++) {
        Slice w;
        Error e = DM(message_pack)(&ms[i], a, &w);
        if (BURROW_FAILED(e)) {
            testing_t_errorf_v(t, "Message.Pack() for %s = %v", names[i], e);
            continue;
        }
        Slice tail = slice_from((Byte *)w.p + w.len - ws[i].len, ws[i].len, ws[i].len,
                                TYPE_BYTE);
        if (w.len < ws[i].len || !bytes_eq(tail, ws[i])) {
            testing_t_errorf_v(t, "got Message.Pack() for %s = %q, want %q", names[i],
                               str_from_bytes((const Byte *)tail.p, tail.len),
                               str_from_bytes((const Byte *)ws[i].p, ws[i].len));
            continue;
        }
        DnsmsgMessage m;
        e = DM(message_unpack)(&m, a, w);
        if (BURROW_FAILED(e)) {
            testing_t_errorf_v(t, "Message.Unpack() for %s = %v", names[i], e);
            continue;
        }
        if (!resources_eq(&m.additionals, &ms[i].additionals))
            testing_t_errorf_v(
                t, "got Message.Pack/Unpack() roundtrip for %s = %s, want %s", names[i],
                DM(message_go_string)(a, &m), DM(message_go_string)(a, &ms[i]));
    }
    ARENA_END;
}

static void TestUnknownPackUnpack(TestingT *t) {
    ARENA_BEGIN;
    DnsmsgMessage msg = small_test_msg_with_unknown_resource(a);
    Slice packed;
    Error e = DM(message_pack)(&msg, a, &packed);
    if (BURROW_FAILED(e)) {
        testing_t_fatalf_v(t, "Failed to pack UnknownResource: %v", e);
        ARENA_END;
        return;
    }
    DnsmsgMessage received;
    e = DM(message_unpack)(&received, a, packed);
    if (BURROW_FAILED(e)) {
        testing_t_fatalf_v(t, "Failed to unpack UnknownResource: %v", e);
        ARENA_END;
        return;
    }
    if (received.answers.len != 1) {
        testing_t_fatalf_v(t, "Got %d answers, wanted 1", received.answers.len);
        ARENA_END;
        return;
    }
    if (received.answers.p[0].body.kind != DNSMSG_BODY_UNKNOWN) {
        testing_t_fatalf_v(t, "Parsed a %s, wanted an UnknownResource",
                           DM(body_go_string)(a, &received.answers.p[0].body));
        ARENA_END;
        return;
    }
    if (!body_eq(&msg.answers.p[0].body, &received.answers.p[0].body))
        testing_t_fatalf_v(t, "Unpacked resource does not match: %s vs %s",
                           DM(body_go_string)(a, &msg.answers.p[0].body),
                           DM(body_go_string)(a, &received.answers.p[0].body));
    ARENA_END;
}

static void TestParseUnknownResource(TestingT *t) {
    ARENA_BEGIN;
    DnsmsgMessage msg = small_test_msg_with_unknown_resource(a);
    Slice packed;
    DnsmsgParser p;
    if (!pack(t, a, &msg, &packed) || !start(t, &p, packed)) {
        ARENA_END;
        return;
    }
    DnsmsgQuestions qs;
    Error e = DM(parser_all_questions)(&p, a, &qs);
    if (BURROW_FAILED(e)) {
        testing_t_fatalf_v(t, "Failed to parse questions: %v", e);
        ARENA_END;
        return;
    }
    DnsmsgResourceHeader parsed_header;
    e = DM(parser_answer_header)(&p, &parsed_header);
    if (BURROW_FAILED(e)) {
        testing_t_fatalf_v(t, "Error reading answer header: %v", e);
        ARENA_END;
        return;
    }
    if (!rheader_eq(&msg.answers.p[0].header, &parsed_header)) {
        testing_t_fatalf_v(t, "Parsed header does not match: %s vs %s",
                           DM(resource_header_go_string)(a, &msg.answers.p[0].header),
                           DM(resource_header_go_string)(a, &parsed_header));
        ARENA_END;
        return;
    }
    DnsmsgResourceBody parsed = body_of(DNSMSG_BODY_UNKNOWN);
    e = DM(parser_unknown_resource)(&p, a, &parsed.u.unknown);
    if (BURROW_FAILED(e)) {
        testing_t_fatalf_v(t, "Failed to parse UnknownResource: %v", e);
        ARENA_END;
        return;
    }
    if (!body_eq(&msg.answers.p[0].body, &parsed)) {
        testing_t_fatalf_v(t, "Parsed resource does not match: %s vs %s",
                           DM(body_go_string)(a, &msg.answers.p[0].body),
                           DM(body_go_string)(a, &parsed));
        ARENA_END;
        return;
    }
    /* Finish parsing the rest of the message to ensure that UnknownResource
     * leaves the parser in a consistent state. */
    e = DM(parser_answer_header)(&p, &parsed_header);
    if (!is(e, DM(err_section_done))) {
        testing_t_fatalf_v(t, "Answer section should be fully parsed");
        ARENA_END;
        return;
    }
    DnsmsgResources rs;
    e = DM(parser_all_authorities)(&p, a, &rs);
    if (BURROW_FAILED(e)) {
        testing_t_fatalf_v(t, "Failed to parse authorities: %v", e);
        ARENA_END;
        return;
    }
    e = DM(parser_all_additionals)(&p, a, &rs);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "Failed to parse additionals: %v", e);
    ARENA_END;
}

/* Go's want, a piece at a time, as it is longer than a C string can be. */
static const char *const go_string_want[] = {
    "dnsmessage.Message{Header: dnsmessage.Header{ID: 0, Response: true, "
    "OpCode: 0, Authoritative: true, Truncated: false, RecursionDesired: "
    "false, RecursionAvailable: false, AuthenticData: false, CheckingDisa"
    "bled: false, RCode: dnsmessage.RCodeSuccess}, Questions: []dnsmessag"
    "e.Question{dnsmessage.Question{Name: dnsmessage.MustNewName(\"foo.bar"
    ".example.com.\"), Type: dnsmessage.TypeA, Class: dnsmessage.ClassINET"
    "}}, Answers: []dnsmessage.Resource{dnsmessage.Resource{Header: dnsme"
    "ssage.ResourceHeader{Name: dnsmessage.MustNewName(\"foo.bar.example.c"
    "om.\"), Type: dnsmessage.TypeA, Class: dnsmessage.ClassINET, TTL: 0, "
    "Length: 0}, Body: &dnsmessage.AResource{A: [4]byte{127, 0, 0, 1}}}",
    ", dnsmessage.Resource{Header: dnsmessage.ResourceHeader{Name: dnsmes"
    "sage.MustNewName(\"foo.bar.example.com.\"), Type: dnsmessage.TypeA, Cl"
    "ass: dnsmessage.ClassINET, TTL: 0, Length: 0}, Body: &dnsmessage.ARe"
    "source{A: [4]byte{127, 0, 0, 2}}}",
    ", dnsmessage.Resource{Header: dnsmessage.ResourceHeader{Name: dnsmes"
    "sage.MustNewName(\"foo.bar.example.com.\"), Type: dnsmessage.TypeAAAA,"
    " Class: dnsmessage.ClassINET, TTL: 0, Length: 0}, Body: &dnsmessage."
    "AAAAResource{AAAA: [16]byte{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 1"
    "3, 14, 15, 16}}}",
    ", dnsmessage.Resource{Header: dnsmessage.ResourceHeader{Name: dnsmes"
    "sage.MustNewName(\"foo.bar.example.com.\"), Type: dnsmessage.TypeCNAME"
    ", Class: dnsmessage.ClassINET, TTL: 0, Length: 0}, Body: &dnsmessage"
    ".CNAMEResource{CNAME: dnsmessage.MustNewName(\"alias.example.com.\")}}",
    ", dnsmessage.Resource{Header: dnsmessage.ResourceHeader{Name: dnsmes"
    "sage.MustNewName(\"foo.bar.example.com.\"), Type: dnsmessage.TypeSOA, "
    "Class: dnsmessage.ClassINET, TTL: 0, Length: 0}, Body: &dnsmessage.S"
    "OAResource{NS: dnsmessage.MustNewName(\"ns1.example.com.\"), MBox: dns"
    "message.MustNewName(\"mb.example.com.\"), Serial: 1, Refresh: 2, Retry"
    ": 3, Expire: 4, MinTTL: 5}}",
    ", dnsmessage.Resource{Header: dnsmessage.ResourceHeader{Name: dnsmes"
    "sage.MustNewName(\"foo.bar.example.com.\"), Type: dnsmessage.TypePTR, "
    "Class: dnsmessage.ClassINET, TTL: 0, Length: 0}, Body: &dnsmessage.P"
    "TRResource{PTR: dnsmessage.MustNewName(\"ptr.example.com.\")}}",
    ", dnsmessage.Resource{Header: dnsmessage.ResourceHeader{Name: dnsmes"
    "sage.MustNewName(\"foo.bar.example.com.\"), Type: dnsmessage.TypeMX, C"
    "lass: dnsmessage.ClassINET, TTL: 0, Length: 0}, Body: &dnsmessage.MX"
    "Resource{Pref: 7, MX: dnsmessage.MustNewName(\"mx.example.com.\")}}",
    ", dnsmessage.Resource{Header: dnsmessage.ResourceHeader{Name: dnsmes"
    "sage.MustNewName(\"foo.bar.example.com.\"), Type: dnsmessage.TypeSRV, "
    "Class: dnsmessage.ClassINET, TTL: 0, Length: 0}, Body: &dnsmessage.S"
    "RVResource{Priority: 8, Weight: 9, Port: 11, Target: dnsmessage.Must"
    "NewName(\"srv.example.com.\")}}",
    ", dnsmessage.Resource{Header: dnsmessage.ResourceHeader{Name: dnsmes"
    "sage.MustNewName(\"foo.bar.example.com.\"), Type: 65362, Class: dnsmes"
    "sage.ClassINET, TTL: 0, Length: 0}, Body: &dnsmessage.UnknownResourc"
    "e{Type: 65362, Data: []byte{42, 0, 43, 44}}}}, Authorities: []dnsmes"
    "sage.Resource{dnsmessage.Resource{Header: dnsmessage.ResourceHeader{"
    "Name: dnsmessage.MustNewName(\"foo.bar.example.com.\"), Type: dnsmessa"
    "ge.TypeNS, Class: dnsmessage.ClassINET, TTL: 0, Length: 0}, Body: &d"
    "nsmessage.NSResource{NS: dnsmessage.MustNewName(\"ns1.example.com.\")}"
    "}",
    ", dnsmessage.Resource{Header: dnsmessage.ResourceHeader{Name: dnsmes"
    "sage.MustNewName(\"foo.bar.example.com.\"), Type: dnsmessage.TypeNS, C"
    "lass: dnsmessage.ClassINET, TTL: 0, Length: 0}, Body: &dnsmessage.NS"
    "Resource{NS: dnsmessage.MustNewName(\"ns2.example.com.\")}}}, Addition"
    "als: []dnsmessage.Resource{dnsmessage.Resource{Header: dnsmessage.Re"
    "sourceHeader{Name: dnsmessage.MustNewName(\"foo.bar.example.com.\"), T"
    "ype: dnsmessage.TypeTXT, Class: dnsmessage.ClassINET, TTL: 0, Length"
    ": 0}, Body: &dnsmessage.TXTResource{TXT: []string{\"So Long\\x2c and T"
    "hanks for All the Fish\"}}}",
    ", dnsmessage.Resource{Header: dnsmessage.ResourceHeader{Name: dnsmes"
    "sage.MustNewName(\"foo.bar.example.com.\"), Type: dnsmessage.TypeTXT, "
    "Class: dnsmessage.ClassINET, TTL: 0, Length: 0}, Body: &dnsmessage.T"
    "XTResource{TXT: []string{\"Hamster Huey and the Gooey Kablooie\"}}}",
    ", dnsmessage.Resource{Header: dnsmessage.ResourceHeader{Name: dnsmes"
    "sage.MustNewName(\".\"), Type: dnsmessage.TypeOPT, Class: 4096, TTL: 4"
    "261412864, Length: 0}, Body: &dnsmessage.OPTResource{Options: []dnsm"
    "essage.Option{dnsmessage.Option{Code: 10, Data: []byte{1, 35, 69, 10"
    "3, 137, 171, 205, 239}}}}}}}",
};

static void TestGoString(TestingT *t) {
    ARENA_BEGIN;
    DnsmsgMessage msg = large_test_msg(a);
    Str got = DM(message_go_string)(a, &msg);
    Int off = 0;
    bool ok = true;
    for (size_t i = 0; i < sizeof go_string_want / sizeof go_string_want[0]; i++) {
        Str piece = str_from_cstr(go_string_want[i]);
        if (got.len - off < piece.len ||
            !str_eq((Str){got.p + off, piece.len}, piece)) {
            ok = false;
            break;
        }
        off += piece.len;
    }
    if (!ok || off != got.len)
        testing_t_errorf_v(t, "got msg1.GoString() = %s\nmismatch at byte %d", got,
                           off);
    ARENA_END;
}

/* benchmarkParsingSetup. */
static Error parsing_setup(Alloc *a, Slice *buf) {
    DnsmsgName name = nm("foo.bar.example.com.");
    DnsmsgMessage msg = empty_msg();
    msg.header.response = true;
    msg.header.authoritative = true;
    msg.questions = questions(a, 1);
    msg.questions.p[0] = question(name, DNSMSG_TYPE_A);
    msg.answers = resources(a, 4);
    msg.answers.p[0] = resource(rheader(name, 0), body_of(DNSMSG_BODY_A));
    msg.answers.p[1] = resource(rheader(name, 0), body_of(DNSMSG_BODY_AAAA));
    DnsmsgResourceBody b = body_of(DNSMSG_BODY_CNAME);
    b.u.cname.cname = name;
    msg.answers.p[2] = resource(rheader(name, 0), b);
    b = body_of(DNSMSG_BODY_NS);
    b.u.ns.ns = name;
    msg.answers.p[3] = resource(rheader(name, 0), b);
    return DM(message_pack)(&msg, a, buf);
}

/* benchmarkParsing, which gives back the first thing to go wrong. The record
 * types it reads take no allocator. */
static Error parse_all(Slice buf, const char **what) {
    DnsmsgParser p;
    memset(&p, 0, sizeof p);
    DnsmsgHeader h;
    Error e = DM(parser_start)(&p, buf, &h);
    if (BURROW_FAILED(e)) {
        *what = "Parser.Start(non-nil)";
        return e;
    }
    for (;;) {
        DnsmsgQuestion q;
        e = DM(parser_question)(&p, &q);
        if (is(e, DM(err_section_done)))
            break;
        if (BURROW_FAILED(e)) {
            *what = "Parser.Question()";
            return e;
        }
    }
    for (;;) {
        DnsmsgResourceHeader rh;
        e = DM(parser_answer_header)(&p, &rh);
        if (is(e, DM(err_section_done)))
            break;
        if (BURROW_FAILED(e)) {
            *what = "Parser.AnswerHeader()";
            return e;
        }
        DnsmsgResourceBody b;
        switch (rh.type) {
        case DNSMSG_TYPE_A:
            e = DM(parser_a_resource)(&p, &b.u.a);
            break;
        case DNSMSG_TYPE_AAAA:
            e = DM(parser_aaaa_resource)(&p, &b.u.aaaa);
            break;
        case DNSMSG_TYPE_CNAME:
            e = DM(parser_cname_resource)(&p, &b.u.cname);
            break;
        case DNSMSG_TYPE_NS:
            e = DM(parser_ns_resource)(&p, &b.u.ns);
            break;
        default:
            *what = "got unknown type";
            return DM(err_not_started);
        }
        if (BURROW_FAILED(e)) {
            *what = "reading a record";
            return e;
        }
    }
    return BURROW_NO_ERROR;
}

/* benchmarkBuilding. */
static Error build_all(Alloc *a, DnsmsgName name, Slice buf) {
    DnsmsgHeader h;
    memset(&h, 0, sizeof h);
    h.response = true;
    h.authoritative = true;
    DnsmsgBuilder bld;
    Error e = DM(new_builder)(&bld, a, buf, h);
    if (BURROW_OK(e))
        e = DM(builder_start_questions)(&bld);
    DnsmsgQuestion q = question(name, DNSMSG_TYPE_A);
    if (BURROW_OK(e))
        e = DM(builder_question)(&bld, &q);
    DnsmsgResourceHeader hdr = rheader(name, 0);
    if (BURROW_OK(e))
        e = DM(builder_start_answers)(&bld);
    DnsmsgResourceBody b = body_of(DNSMSG_BODY_NONE);
    if (BURROW_OK(e))
        e = DM(builder_a_resource)(&bld, hdr, &b.u.a);
    if (BURROW_OK(e))
        e = DM(builder_aaaa_resource)(&bld, hdr, &b.u.aaaa);
    b.u.cname.cname = name;
    if (BURROW_OK(e))
        e = DM(builder_cname_resource)(&bld, hdr, &b.u.cname);
    b.u.ns.ns = name;
    if (BURROW_OK(e))
        e = DM(builder_ns_resource)(&bld, hdr, &b.u.ns);
    if (BURROW_OK(e))
        e = DM(resource_header_set_edns0)(&hdr, 4096,
                                          0xfe0 | DNSMSG_RCODE_NOT_IMPLEMENTED, true);
    DnsmsgOPTResource optr;
    memset(&optr, 0, sizeof optr);
    if (BURROW_OK(e))
        e = DM(builder_opt_resource)(&bld, hdr, &optr);
    Slice out;
    if (BURROW_OK(e))
        e = DM(builder_finish)(&bld, &out);
    DM(builder_free)(&bld);
    return e;
}

static void TestParsingAllocs(TestingT *t) {
    ARENA_BEGIN;
    Slice buf;
    Error e = parsing_setup(a, &buf);
    if (BURROW_FAILED(e)) {
        testing_t_fatalf_v(t, "Message.Pack() = %v", e);
        ARENA_END;
        return;
    }
    const char *what = "";
    e = parse_all(buf, &what);
    if (BURROW_FAILED(e))
        testing_t_errorf_v(t, "%s = %v", what, e);
    ARENA_END;
}

static void TestBuildingAllocs(TestingT *t) {
    static Byte room[64];
    Fixed fx;
    fixed_init(&fx, room, sizeof room);
    Byte buf[512];
    Error e = build_all(fixed_allocator(&fx), nm("foo.bar.example.com."),
                        slice_from(buf, 0, 512, TYPE_BYTE));
    if (BURROW_FAILED(e))
        testing_t_errorf_v(t, "building = %v", e);
    if (fx.allocs != 0)
        testing_t_errorf_v(t, "allocations during building: got = %d, want 0",
                           (int64_t)fx.allocs);
}

static void FuzzUnpackPackSeeds(TestingT *t) {
    ARENA_BEGIN;
    DnsmsgMessage seeds[2] = {small_test_msg(a), large_test_msg(a)};
    for (int i = 0; i < 2; i++) {
        Slice msg;
        if (!pack(t, a, &seeds[i], &msg))
            break;
        DnsmsgMessage m;
        if (BURROW_FAILED(DM(message_unpack)(&m, a, msg)))
            continue;
        Slice packed;
        Error e = DM(message_pack)(&m, a, &packed);
        if (BURROW_FAILED(e)) {
            testing_t_fatalf_v(
                t, "failed to pack message that was successfully unpacked: %v", e);
            break;
        }
        DnsmsgMessage m2;
        e = DM(message_unpack)(&m2, a, packed);
        if (BURROW_FAILED(e)) {
            testing_t_fatalf_v(
                t, "failed to unpack message that was succesfully packed: %v", e);
            break;
        }
        if (!msg_eq(&m, &m2)) {
            testing_t_fatalf_v(
                t, "unpack(msg) is not deep equal to unpack(pack(unpack(msg)))");
            break;
        }
    }
    ARENA_END;
}

static DnsmsgMessage two_go_dev_records(Alloc *a) {
    DnsmsgMessage msg = empty_msg();
    msg.header.response = true;
    msg.header.authoritative = true;
    msg.answers = resources(a, 1);
    msg.answers.p[0] =
        resource(rheader(nm("go.dev."), DNSMSG_TYPE_A), body_a(127, 0, 0, 1));
    msg.authorities = resources(a, 1);
    msg.authorities.p[0] =
        resource(rheader(nm("go.dev."), DNSMSG_TYPE_A), body_a(127, 0, 0, 1));
    return msg;
}

static void TestParseResourceHeaderMultipleTimes(TestingT *t) {
    ARENA_BEGIN;
    DnsmsgMessage msg = two_go_dev_records(a);
    Slice raw;
    DnsmsgParser p;
    if (!pack(t, a, &msg, &raw) || !start(t, &p, raw)) {
        ARENA_END;
        return;
    }
    DnsmsgResourceHeader hdr1, hdr2, hdr3, hdr4;
    DnsmsgAResource ar1;
    Error e = DM(parser_skip_all_questions)(&p);
    if (BURROW_OK(e))
        e = DM(parser_answer_header)(&p, &hdr1);
    if (BURROW_OK(e))
        e = DM(parser_answer_header)(&p, &hdr2);
    if (BURROW_FAILED(e)) {
        testing_t_fatalf_v(t, "%v", e);
        ARENA_END;
        return;
    }
    if (!rheader_eq(&hdr1, &hdr2)) {
        testing_t_fatalf_v(t, "AnswerHeader called multiple times without parsing the "
                              "RData returned different headers");
        ARENA_END;
        return;
    }
    e = DM(parser_a_resource)(&p, &ar1);
    if (BURROW_FAILED(e)) {
        testing_t_fatalf_v(t, "%v", e);
        ARENA_END;
        return;
    }
    e = DM(parser_answer_header)(&p, &hdr1);
    if (!is(e, DM(err_section_done))) {
        testing_t_fatalf_v(t, "unexpected error: %v, want: %v", e,
                           DM(err_section_done));
        ARENA_END;
        return;
    }
    e = DM(parser_authority_header)(&p, &hdr3);
    if (BURROW_OK(e))
        e = DM(parser_authority_header)(&p, &hdr4);
    if (BURROW_FAILED(e)) {
        testing_t_fatalf_v(t, "%v", e);
        ARENA_END;
        return;
    }
    if (!rheader_eq(&hdr3, &hdr4)) {
        testing_t_fatalf_v(t, "AuthorityHeader called multiple times without parsing "
                              "the RData returned different headers");
        ARENA_END;
        return;
    }
    e = DM(parser_a_resource)(&p, &ar1);
    if (BURROW_FAILED(e)) {
        testing_t_fatalf_v(t, "%v", e);
        ARENA_END;
        return;
    }
    e = DM(parser_authority_header)(&p, &hdr3);
    if (!is(e, DM(err_section_done)))
        testing_t_fatalf_v(t, "unexpected error: %v, want: %v", e,
                           DM(err_section_done));
    ARENA_END;
}

static void TestParseDifferentResourceHeadersWithoutParsingRData(TestingT *t) {
    ARENA_BEGIN;
    DnsmsgMessage msg = small_test_msg(a);
    Slice raw;
    DnsmsgParser p;
    if (!pack(t, a, &msg, &raw) || !start(t, &p, raw)) {
        ARENA_END;
        return;
    }
    DnsmsgResourceHeader h;
    Error e = DM(parser_skip_all_questions)(&p);
    if (BURROW_OK(e))
        e = DM(parser_answer_header)(&p, &h);
    if (BURROW_FAILED(e)) {
        testing_t_fatalf_v(t, "%v", e);
        ARENA_END;
        return;
    }
    if (BURROW_OK(DM(parser_additional_header)(&p, &h)))
        testing_t_errorf_v(t, "p.AdditionalHeader() unexpected success");
    if (BURROW_OK(DM(parser_authority_header)(&p, &h)))
        testing_t_errorf_v(t, "p.AuthorityHeader() unexpected success");
    ARENA_END;
}

static void TestParseWrongSection(TestingT *t) {
    ARENA_BEGIN;
    DnsmsgMessage msg = small_test_msg(a);
    Slice raw;
    DnsmsgParser p;
    if (!pack(t, a, &msg, &raw) || !start(t, &p, raw)) {
        ARENA_END;
        return;
    }
    DnsmsgResourceHeader h;
    Error e = DM(parser_skip_all_questions)(&p);
    if (BURROW_FAILED(e)) {
        testing_t_fatalf_v(t, "p.SkipAllQuestions() = %v", e);
        ARENA_END;
        return;
    }
    e = DM(parser_answer_header)(&p, &h);
    if (BURROW_FAILED(e)) {
        testing_t_fatalf_v(t, "p.AnswerHeader() = %v", e);
        ARENA_END;
        return;
    }
    if (BURROW_OK(DM(parser_authority_header)(&p, &h)))
        testing_t_errorf_v(t,
                           "p.AuthorityHeader(): unexpected success in Answer section");
    else if (BURROW_OK(DM(parser_skip_authority)(&p)))
        testing_t_errorf_v(t,
                           "p.SkipAuthority(): unexpected success in Answer section");
    else if (BURROW_OK(DM(parser_skip_all_authorities)(&p)))
        testing_t_errorf_v(
            t, "p.SkipAllAuthorities(): unexpected success in Answer section");
    ARENA_END;
}

static const Byte two_questions[] = {
    0,    0,   0,   0, 0,   2,   0,   0, 0, 0, 0, 0, /* header */
    2,    'g', 'o', 3, 'd', 'e', 'v', 0, 0, 0, 0, 0, /* question 1 */
    0xC0, 12,  0,   0, 0,   0,                       /* question 2 */
};

static void TestBuilderNameCompressionWithNonZeroedName(TestingT *t) {
    ARENA_BEGIN;
    DnsmsgHeader h;
    memset(&h, 0, sizeof h);
    DnsmsgBuilder b;
    Error e = DM(new_builder)(&b, a, slice_nil(TYPE_BYTE), h);
    if (BURROW_OK(e))
        e = DM(builder_enable_compression)(&b);
    if (BURROW_OK(e))
        e = DM(builder_start_questions)(&b);
    DnsmsgQuestion q;
    memset(&q, 0, sizeof q);
    q.name = nm("go.dev.");
    if (BURROW_OK(e))
        e = DM(builder_question)(&b, &q);
    /* Character that is not part of the name (name.Data[:name.Length]),
     * shouldn't affect the compression algorithm. */
    q.name.data[q.name.length] = '1';
    if (BURROW_OK(e))
        e = DM(builder_question)(&b, &q);
    Slice msg;
    if (BURROW_OK(e))
        e = DM(builder_finish)(&b, &msg);
    DM(builder_free)(&b);
    if (BURROW_FAILED(e)) {
        testing_t_fatalf_v(t, "unexpected error: %v", e);
        ARENA_END;
        return;
    }
    if (!bytes_eq(msg, VIEW(two_questions)))
        testing_t_fatalf_v(t, "b.Finish() = %q, want: %q",
                           str_from_bytes((const Byte *)msg.p, msg.len),
                           str_from_bytes(two_questions, (Int)sizeof two_questions));
    ARENA_END;
}

static void TestBuilderCompressionInAppendMode(TestingT *t) {
    ARENA_BEGIN;
    const Int max_ptr = 0xFFFF >> 2;
    Byte *mem = (Byte *)mem_alloc(a, (size_t)max_ptr + 512, 1);
    DnsmsgHeader h;
    memset(&h, 0, sizeof h);
    DnsmsgBuilder b;
    Error e =
        DM(new_builder)(&b, a, slice_from(mem, max_ptr, max_ptr + 512, TYPE_BYTE), h);
    if (BURROW_OK(e))
        e = DM(builder_enable_compression)(&b);
    if (BURROW_OK(e))
        e = DM(builder_start_questions)(&b);
    DnsmsgQuestion q;
    memset(&q, 0, sizeof q);
    q.name = nm("go.dev.");
    if (BURROW_OK(e))
        e = DM(builder_question)(&b, &q);
    if (BURROW_OK(e))
        e = DM(builder_question)(&b, &q);
    Slice msg;
    if (BURROW_OK(e))
        e = DM(builder_finish)(&b, &msg);
    DM(builder_free)(&b);
    if (BURROW_FAILED(e)) {
        testing_t_fatalf_v(t, "unexpected error: %v", e);
        ARENA_END;
        return;
    }
    Slice tail = view((const Byte *)msg.p + max_ptr, msg.len - max_ptr);
    if (!bytes_eq(tail, VIEW(two_questions)))
        testing_t_fatalf_v(t, "msg[maxPtr:] = %q, want: %q",
                           str_from_bytes((const Byte *)tail.p, tail.len),
                           str_from_bytes(two_questions, (Int)sizeof two_questions));
    ARENA_END;
}

static void TestInvalidMessages(TestingT *t) {
    ARENA_BEGIN;
    /* invalid SVCB */
    static const Byte in[] = {
        0x12, 0x34, 0x81, 0x80, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x41, 0x00, 0x01, 0x00, 0x00, 0x41, 0x00, 0x01, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x64, 0x00, 0x01, 0x00, 0x00, 0x01, 0x00, 0x5d,
    };
    DnsmsgParser p;
    memset(&p, 0, sizeof p);
    DnsmsgHeader h;
    DnsmsgQuestions qs;
    DnsmsgResources rs;
    if (BURROW_OK(DM(parser_start)(&p, VIEW(in), &h)) &&
        BURROW_OK(DM(parser_all_questions)(&p, a, &qs)) &&
        BURROW_OK(DM(parser_all_answers)(&p, a, &rs)))
        testing_t_errorf_v(t, "successfully parsed message: want error");
    ARENA_END;
}

/* ---------------------------------------------------------------- SVCB */

static void svcb_param_round_trip(TestingT *t, Alloc *a, DnsmsgSVCParam param) {
    DnsmsgResourceBody rr = body_of(DNSMSG_BODY_SVCB);
    rr.u.svcb.priority = 1;
    rr.u.svcb.target = nm("svc.example.com.");
    rr.u.svcb.params = params(a, 1);
    rr.u.svcb.params.p[0] = param;
    DnsmsgBuf buf = DM(buf)(a, slice_nil(TYPE_BYTE));
    Error e = DM(body_pack)(&rr, &buf, NULL, 0);
    if (BURROW_FAILED(e)) {
        testing_t_errorf_v(t, "key %d: pack() = %v", param.key, e);
        return;
    }
    DnsmsgResourceHeader hdr;
    memset(&hdr, 0, sizeof hdr);
    hdr.type = DNSMSG_TYPE_SVCB;
    hdr.length = (uint16_t)buf.len;
    DnsmsgResourceBody got;
    Int n;
    e = DM(unpack_resource_body)(a, DM(buf_slice)(&buf), 0, hdr, &got, &n);
    if (BURROW_FAILED(e)) {
        testing_t_errorf_v(t, "key %d: unpackResourceBody() = %v", param.key, e);
        return;
    }
    if (n != buf.len)
        testing_t_errorf_v(t,
                           "key %d: unpacked different amount than packed: got = %d, "
                           "want = %d",
                           param.key, n, buf.len);
    if (!body_eq(&got, &rr))
        testing_t_errorf_v(t, "roundtrip mismatch: got = %s, want = %s",
                           DM(body_go_string)(a, &got), DM(body_go_string)(a, &rr));
}

static void TestSVCBParamsRoundTrip(TestingT *t) {
    ARENA_BEGIN;
    svcb_param_round_trip(
        t, a,
        (DnsmsgSVCParam){DNSMSG_SVC_PARAM_MANDATORY,
                         BYTES(a, 0x00, 0x01, 0x00, 0x03, 0x00, 0x05)});
    svcb_param_round_trip(t, a,
                          (DnsmsgSVCParam){DNSMSG_SVC_PARAM_ALPN,
                                           BYTES(a, 0x02, 'h', '2', 0x02, 'h', '3')});
    svcb_param_round_trip(
        t, a,
        (DnsmsgSVCParam){DNSMSG_SVC_PARAM_NO_DEFAULT_ALPN, bytes_dup(a, NULL, 0)});
    svcb_param_round_trip(
        t, a, (DnsmsgSVCParam){DNSMSG_SVC_PARAM_PORT, BYTES(a, 0x1f, 0x90)}); /* 8080 */
    svcb_param_round_trip(t, a,
                          (DnsmsgSVCParam){DNSMSG_SVC_PARAM_IPV4_HINT,
                                           BYTES(a, 192, 0, 2, 1, 198, 51, 100, 2)});
    svcb_param_round_trip(
        t, a, (DnsmsgSVCParam){DNSMSG_SVC_PARAM_ECH, BYTES(a, 0x01, 0x02, 0x03, 0x04)});
    svcb_param_round_trip(
        t, a,
        (DnsmsgSVCParam){DNSMSG_SVC_PARAM_IPV6_HINT,
                         BYTES(a, 0x20, 0x01, 0x0d, 0xb8, 0x00, 0x00, 0x00, 0x00, 0x00,
                               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01)});
    svcb_param_round_trip(
        t, a, (DnsmsgSVCParam){DNSMSG_SVC_PARAM_DOH_PATH, LIT(a, "/dns-query{?dns}")});
    svcb_param_round_trip(
        t, a,
        (DnsmsgSVCParam){DNSMSG_SVC_PARAM_OHTTP, BYTES(a, 0x00, 0x01, 0x02, 0x03)});
    svcb_param_round_trip(t, a,
                          (DnsmsgSVCParam){DNSMSG_SVC_PARAM_TLS_SUPPORTED_GROUPS,
                                           BYTES(a, 0x00, 0x1d, 0x00, 0x17)});
    ARENA_END;
}

static void TestSVCBParams(TestingT *t) {
    ARENA_BEGIN;
    DnsmsgResourceBody body = body_of(DNSMSG_BODY_SVCB);
    DnsmsgSVCBResource *rr = &body.u.svcb;
    rr->priority = 1;
    rr->target = nm("svc.example.com.");
    Slice v;
    if (DM(svcb_get_param)(rr, DNSMSG_SVC_PARAM_ALPN, &v)) {
        testing_t_fatalf_v(t, "GetParam found non-existent param");
        ARENA_END;
        return;
    }
    DM(svcb_set_param)(rr, a, DNSMSG_SVC_PARAM_IPV4_HINT, BYTES(a, 192, 0, 2, 1));
    Slice in_alpn = BYTES(a, 0x02, 'h', '2', 0x02, 'h', '3');
    DM(svcb_set_param)(rr, a, DNSMSG_SVC_PARAM_ALPN, in_alpn);

    /* Check sorting of params */
    static const Byte expected[] = {
        0x00, 0x01,                                                 /* priority */
        0x03, 0x73, 0x76, 0x63, 0x07, 0x65, 0x78, 0x61, 0x6d, 0x70, /* target */
        0x6c, 0x65, 0x03, 0x63, 0x6f, 0x6d, 0x00,                   /* target */
        0x00, 0x01,                                                 /* key 1 */
        0x00, 0x06,                                                 /* length 6 */
        0x02, 'h',  '2',  0x02, 'h',  '3',                          /* value */
        0x00, 0x04,                                                 /* key 4 */
        0x00, 0x04,                                                 /* length 4 */
        192,  0,    2,    1,                                        /* value */
    };
    DnsmsgBuf buf = DM(buf)(a, slice_nil(TYPE_BYTE));
    Error e = DM(body_pack)(&body, &buf, NULL, 0);
    if (BURROW_FAILED(e)) {
        testing_t_fatalf_v(t, "pack() = %v", e);
        ARENA_END;
        return;
    }
    if (!bytes_eq(DM(buf_slice)(&buf), VIEW(expected))) {
        testing_t_fatalf_v(t, "pack() produced unexpected output: want = %q, got = %q",
                           str_from_bytes(expected, (Int)sizeof expected),
                           str_from_bytes(buf.p, buf.len));
        ARENA_END;
        return;
    }

    /* Check GetParam and DeleteParam. */
    if (!DM(svcb_get_param)(rr, DNSMSG_SVC_PARAM_ALPN, &v) || !bytes_eq(v, in_alpn)) {
        testing_t_fatalf_v(t, "GetParam failed to retrieve set param");
        ARENA_END;
        return;
    }
    if (!DM(svcb_delete_param)(rr, DNSMSG_SVC_PARAM_ALPN)) {
        testing_t_fatalf_v(t, "DeleteParam failed to remove existing param");
        ARENA_END;
        return;
    }
    if (DM(svcb_get_param)(rr, DNSMSG_SVC_PARAM_ALPN, &v)) {
        testing_t_fatalf_v(t, "GetParam found deleted param");
        ARENA_END;
        return;
    }
    if (rr->params.len != 1 || rr->params.p[0].key != DNSMSG_SVC_PARAM_IPV4_HINT)
        testing_t_fatalf_v(t, "DeleteParam removed wrong param: got = %s",
                           DM(body_go_string)(a, &body));
    ARENA_END;
}

/* testRecord: unpack in and compare it with want, then pack want and compare
 * that with in. */
static void svcb_record(TestingT *t, Alloc *a, Slice in, DnsmsgSVCBResource want) {
    DnsmsgResourceHeader hdr;
    memset(&hdr, 0, sizeof hdr);
    hdr.type = DNSMSG_TYPE_SVCB;
    hdr.length = (uint16_t)in.len;
    DnsmsgResourceBody got;
    Int n;
    Error e = DM(unpack_resource_body)(a, in, 0, hdr, &got, &n);
    if (BURROW_FAILED(e)) {
        testing_t_errorf_v(t, "unpackResourceBody() = %v", e);
        return;
    }
    if (n != in.len) {
        testing_t_errorf_v(
            t, "unpacked different amount than packed: got = %d, want = %d", n, in.len);
        return;
    }
    DnsmsgResourceBody wb = body_of(DNSMSG_BODY_SVCB);
    wb.u.svcb = want;
    if (!body_eq(&got, &wb)) {
        testing_t_errorf_v(t, "unpack mismatch: got = %s, want = %s",
                           DM(body_go_string)(a, &got), DM(body_go_string)(a, &wb));
        return;
    }
    DnsmsgBuf buf = DM(buf)(a, slice_nil(TYPE_BYTE));
    e = DM(body_pack)(&wb, &buf, NULL, 0);
    if (BURROW_FAILED(e)) {
        testing_t_errorf_v(t, "pack() = %v", e);
        return;
    }
    if (!bytes_eq(DM(buf_slice)(&buf), in))
        testing_t_errorf_v(t, "pack mismatch: got = %q, want = %q",
                           str_from_bytes(buf.p, buf.len),
                           str_from_bytes((const Byte *)in.p, in.len));
}

static DnsmsgSVCBResource svcb(uint16_t priority, const char *target,
                               DnsmsgSVCParams ps) {
    DnsmsgSVCBResource r;
    memset(&r, 0, sizeof r);
    r.priority = priority;
    r.target = nm(target);
    r.params = ps;
    return r;
}

static DnsmsgSVCParams one_param(Alloc *a, DnsmsgSVCParamKey key, Slice value) {
    DnsmsgSVCParams ps = params(a, 1);
    ps.p[0] = (DnsmsgSVCParam){key, value};
    return ps;
}

#define FOO_EXAMPLE_COM                                                                \
    0x03, 0x66, 0x6f, 0x6f, 0x07, 0x65, 0x78, 0x61, 0x6d, 0x70, 0x6c, 0x65, 0x03,      \
        0x63, 0x6f, 0x6d, 0x00
#define FOO_EXAMPLE_ORG                                                                \
    0x03, 0x66, 0x6f, 0x6f, 0x07, 0x65, 0x78, 0x61, 0x6d, 0x70, 0x6c, 0x65, 0x03,      \
        0x6f, 0x72, 0x67, 0x00

static void TestSVCBWireFormat(TestingT *t) {
    ARENA_BEGIN;
    DnsmsgSVCParams none = {NULL, 0, 0};

    /* Test examples from https://datatracker.ietf.org/doc/html/rfc9460#name-test-vectors */

    /* Example D.1. Alias Mode
     * Figure 2: AliasMode
     * example.com.   HTTPS   0 foo.example.com. */
    svcb_record(t, a, BYTES(a, 0x00, 0x00, FOO_EXAMPLE_COM),
                svcb(0, "foo.example.com.", none));

    /* Example D.2. Service Mode
     * Figure 3: TargetName Is "."
     * example.com.   SVCB   1 . */
    svcb_record(t, a, BYTES(a, 0x00, 0x01, 0x00), svcb(1, ".", none));

    /* Figure 4: Specifies a Port
     * example.com.   SVCB   16 foo.example.com. port=53 */
    svcb_record(
        t, a, BYTES(a, 0x00, 0x10, FOO_EXAMPLE_COM, 0x00, 0x03, 0x00, 0x02, 0x00, 0x35),
        svcb(16, "foo.example.com.",
             one_param(a, DNSMSG_SVC_PARAM_PORT, BYTES(a, 0x00, 0x35))));

    /* Figure 5: A Generic Key and Unquoted Value
     * example.com.   SVCB   1 foo.example.com. key667=hello */
    svcb_record(t, a,
                BYTES(a, 0x00, 0x01, FOO_EXAMPLE_COM, 0x02, 0x9b, 0x00, 0x05, 0x68,
                      0x65, 0x6c, 0x6c, 0x6f),
                svcb(1, "foo.example.com.", one_param(a, 667, LIT(a, "hello"))));

    /* Figure 6: A Generic Key and Quoted Value with a Decimal Escape
     * example.com.   SVCB   1 foo.example.com. key667="hello\210qoo" */
    svcb_record(t, a,
                BYTES(a, 0x00, 0x01, FOO_EXAMPLE_COM, 0x02, 0x9b, 0x00, 0x09, 0x68,
                      0x65, 0x6c, 0x6c, 0x6f, 0xd2, 0x71, 0x6f, 0x6f),
                svcb(1, "foo.example.com.", one_param(a, 667, LIT(a, "hello\xd2qoo"))));

    /* Figure 7: Two Quoted IPv6 Hints
     * example.com.   SVCB   1 foo.example.com. (
     *                       ipv6hint="2001:db8::1,2001:db8::53:1"
     *                       ) */
    svcb_record(t, a,
                BYTES(a, 0x00, 0x01, FOO_EXAMPLE_COM, 0x00, 0x06, 0x00, 0x20, 0x20,
                      0x01, 0x0d, 0xb8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                      0x00, 0x00, 0x00, 0x01, 0x20, 0x01, 0x0d, 0xb8, 0x00, 0x00, 0x00,
                      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x53, 0x00, 0x01),
                svcb(1, "foo.example.com.",
                     one_param(a, DNSMSG_SVC_PARAM_IPV6_HINT,
                               BYTES(a, 0x20, 0x01, 0x0d, 0xb8, 0x00, 0x00, 0x00, 0x00,
                                     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
                                     0x20, 0x01, 0x0d, 0xb8, 0x00, 0x00, 0x00, 0x00,
                                     0x00, 0x00, 0x00, 0x00, 0x00, 0x53, 0x00, 0x01))));

    /* Figure 8: An IPv6 Hint Using the Embedded IPv4 Syntax
     * example.com.   SVCB   1 example.com. (
     *                        ipv6hint="2001:db8:122:344::192.0.2.33"
     *                        ) */
    svcb_record(t, a,
                BYTES(a, 0x00, 0x01, 0x07, 0x65, 0x78, 0x61, 0x6d, 0x70, 0x6c, 0x65,
                      0x03, 0x63, 0x6f, 0x6d, 0x00, 0x00, 0x06, 0x00, 0x10, 0x20, 0x01,
                      0x0d, 0xb8, 0x01, 0x22, 0x03, 0x44, 0x00, 0x00, 0x00, 0x00, 0xc0,
                      0x00, 0x02, 0x21),
                svcb(1, "example.com.",
                     one_param(a, DNSMSG_SVC_PARAM_IPV6_HINT,
                               BYTES(a, 0x20, 0x01, 0x0d, 0xb8, 0x01, 0x22, 0x03, 0x44,
                                     0x00, 0x00, 0x00, 0x00, 0xc0, 0x00, 0x02, 0x21))));

    /* Figure 9: SvcParamKey Ordering Is Arbitrary in Presentation Format but
     * Sorted in Wire Format
     * example.com.   SVCB   16 foo.example.org. (
     *                      alpn=h2,h3-19 mandatory=ipv4hint,alpn
     *                      ipv4hint=192.0.2.1
     *                      ) */
    DnsmsgSVCParams ps = params(a, 3);
    ps.p[0] =
        (DnsmsgSVCParam){DNSMSG_SVC_PARAM_MANDATORY, BYTES(a, 0x00, 0x01, 0x00, 0x04)};
    ps.p[1] =
        (DnsmsgSVCParam){DNSMSG_SVC_PARAM_ALPN, BYTES(a, 0x02, 0x68, 0x32, 0x05, 0x68,
                                                      0x33, 0x2d, 0x31, 0x39)};
    ps.p[2] =
        (DnsmsgSVCParam){DNSMSG_SVC_PARAM_IPV4_HINT, BYTES(a, 0xc0, 0x00, 0x02, 0x01)};
    svcb_record(t, a,
                BYTES(a, 0x00, 0x10, FOO_EXAMPLE_ORG, 0x00, 0x00, 0x00, 0x04, 0x00,
                      0x01, 0x00, 0x04, 0x00, 0x01, 0x00, 0x09, 0x02, 0x68, 0x32, 0x05,
                      0x68, 0x33, 0x2d, 0x31, 0x39, 0x00, 0x04, 0x00, 0x04, 0xc0, 0x00,
                      0x02, 0x01),
                svcb(16, "foo.example.org.", ps));

    /* Figure 10: An "alpn" Value with an Escaped Comma and an Escaped Backslash
     * in Two Presentation Formats
     * example.com.   SVCB   16 foo.example.org. alpn=f\\\092oo\092,bar,h2 */
    svcb_record(t, a,
                BYTES(a, 0x00, 0x10, FOO_EXAMPLE_ORG, 0x00, 0x01, 0x00, 0x0c, 0x08,
                      0x66, 0x5c, 0x6f, 0x6f, 0x2c, 0x62, 0x61, 0x72, 0x02, 0x68, 0x32),
                svcb(16, "foo.example.org.",
                     one_param(a, DNSMSG_SVC_PARAM_ALPN,
                               BYTES(a, 0x08, 0x66, 0x5c, 0x6f, 0x6f, 0x2c, 0x62, 0x61,
                                     0x72, 0x02, 0x68, 0x32))));
    ARENA_END;
}

static void TestSVCBPackLongValue(TestingT *t) {
    ARENA_BEGIN;
    DnsmsgHeader h;
    memset(&h, 0, sizeof h);
    DnsmsgBuilder b;
    DM(new_builder)(&b, a, slice_nil(TYPE_BYTE), h);
    DM(builder_start_questions)(&b);
    DM(builder_start_answers)(&b);
    Byte *big = (Byte *)mem_alloc(a, 0xFFFF + 1, 1);
    DnsmsgSVCBResource res =
        svcb(0, "example.com.",
             one_param(a, DNSMSG_SVC_PARAM_MANDATORY,
                       slice_from(big, 0xFFFF + 1, 0xFFFF + 1, TYPE_BYTE)));
    DnsmsgResourceHeader rh;
    memset(&rh, 0, sizeof rh);
    rh.name = nm("example.com.");
    static const char want[] =
        "ResourceBody: SVCBResource.Params: value too long (>65535 bytes)";
    Error e = DM(builder_svcb_resource)(&b, rh, &res);
    if (BURROW_OK(e) || !str_eq(error_text(e), str_from_cstr(want))) {
        testing_t_fatalf_v(t, "b.SVCBResource() = %v; want = %q", e, want);
        DM(builder_free)(&b);
        ARENA_END;
        return;
    }
    DnsmsgHTTPSResource https = {res};
    e = DM(builder_https_resource)(&b, rh, &https);
    if (BURROW_OK(e) || !str_eq(error_text(e), str_from_cstr(want)))
        testing_t_fatalf_v(t, "b.HTTPSResource() = %v; want = %q", e, want);
    DM(builder_free)(&b);
    ARENA_END;
}

/* ----------------------------------------------------------------- new */

static void TestFreeAfterUnpack(TestingT *t) {
    ARENA_BEGIN;
    Track tr;
    track_init(&tr, heap_allocator());
    Alloc *ta = track_allocator(&tr);
    DnsmsgMessage wants[2] = {large_test_msg(a), build_test_svcb_msg(a)};
    for (int i = 0; i < 2; i++) {
        Slice b;
        Error e = DM(message_pack)(&wants[i], ta, &b);
        if (BURROW_FAILED(e)) {
            testing_t_errorf_v(t, "%d: Message.Pack() = %v", i, e);
            continue;
        }
        DnsmsgMessage got;
        e = DM(message_unpack)(&got, ta, b);
        if (BURROW_FAILED(e))
            testing_t_errorf_v(t, "%d: Message.Unpack() = %v", i, e);
        else if (!msg_eq(&got, &wants[i]))
            testing_t_errorf_v(t, "%d: roundtrip mismatch", i);
        DM(message_free)(ta, &got);
        mem_free(ta, b.p, (size_t)b.cap, 1);
    }
    if (track_check(&tr) != 0 || track_live(&tr) != 0)
        testing_t_errorf_v(t, "%d bytes still live after freeing",
                           (int64_t)track_live(&tr));
    track_free(&tr);
    ARENA_END;
}

static void TestFreeAfterBuild(TestingT *t) {
    ARENA_BEGIN;
    Track tr;
    track_init(&tr, heap_allocator());
    Alloc *ta = track_allocator(&tr);
    DnsmsgMessage msg = large_test_msg(a);
    Slice want, got;
    if (!pack(t, a, &msg, &want)) {
        track_free(&tr);
        ARENA_END;
        return;
    }
    Error e = build_msg(ta, &msg, &got);
    if (BURROW_FAILED(e))
        testing_t_errorf_v(t, "building = %v", e);
    else if (!bytes_eq(got, want))
        testing_t_errorf_v(t, "built message differs from the packed one");
    if (BURROW_OK(e))
        mem_free(ta, got.p, (size_t)got.cap, 1);
    if (track_check(&tr) != 0 || track_live(&tr) != 0)
        testing_t_errorf_v(t, "%d bytes still live after freeing",
                           (int64_t)track_live(&tr));
    track_free(&tr);
    ARENA_END;
}

/* ---------------------------------------------------------- benchmarks */

static void BenchmarkParsing(TestingB *b) {
    ARENA_BEGIN;
    Slice buf;
    Error e = parsing_setup(a, &buf);
    if (BURROW_FAILED(e)) {
        testing_b_fatalf_v(b, "%v", e);
        ARENA_END;
        return;
    }
    const char *what = "";
    for (Int i = 0; i < testing_b_n(b); i++) {
        e = parse_all(buf, &what);
        if (BURROW_FAILED(e)) {
            testing_b_fatalf_v(b, "%s = %v", what, e);
            break;
        }
    }
    ARENA_END;
}

static void BenchmarkBuilding(TestingB *b) {
    DnsmsgName name = nm("foo.bar.example.com.");
    Byte buf[512];
    for (Int i = 0; i < testing_b_n(b); i++) {
        Error e = build_all(NULL, name, slice_from(buf, 0, 512, TYPE_BYTE));
        if (BURROW_FAILED(e)) {
            testing_b_fatalf_v(b, "%v", e);
            break;
        }
    }
}

static void BenchmarkPack(TestingB *b) {
    ARENA_BEGIN;
    DnsmsgMessage msg = large_test_msg(a);
    for (Int i = 0; i < testing_b_n(b); i++) {
        Slice out;
        Error e = DM(message_pack)(&msg, heap_allocator(), &out);
        if (BURROW_FAILED(e)) {
            testing_b_fatalf_v(b, "%v", e);
            break;
        }
        mem_free(heap_allocator(), out.p, (size_t)out.cap, 1);
    }
    ARENA_END;
}

static void BenchmarkAppendPack(TestingB *b) {
    ARENA_BEGIN;
    DnsmsgMessage msg = large_test_msg(a);
    Byte *buf = (Byte *)mem_alloc(a, 512, 1);
    for (Int i = 0; i < testing_b_n(b); i++) {
        Slice out;
        Error e =
            DM(message_append_pack)(&msg, a, slice_from(buf, 0, 512, TYPE_BYTE), &out);
        if (BURROW_FAILED(e)) {
            testing_b_fatalf_v(b, "%v", e);
            break;
        }
    }
    ARENA_END;
}

#define TESTS(X)                                                                       \
    X(TestPrintPaddedUint8)                                                            \
    X(TestPrintUint8Bytes)                                                             \
    X(TestPrintUint16)                                                                 \
    X(TestPrintUint32)                                                                 \
    X(TestNameString)                                                                  \
    X(TestQuestionPackUnpack)                                                          \
    X(TestName)                                                                        \
    X(TestNameWithDotsUnpack)                                                          \
    X(TestNamePackUnpack)                                                              \
    X(TestNameUnpackTooLongName)                                                       \
    X(TestHeaderUnpackError)                                                           \
    X(TestParserStart)                                                                 \
    X(TestResourceNotStarted)                                                          \
    X(TestDNSPackUnpack)                                                               \
    X(TestDNSAppendPackUnpack)                                                         \
    X(TestSkipAll)                                                                     \
    X(TestSkipEach)                                                                    \
    X(TestSkipAfterRead)                                                               \
    X(TestSkipNotStarted)                                                              \
    X(TestTooManyRecords)                                                              \
    X(TestVeryLongTxt)                                                                 \
    X(TestTooLongTxt)                                                                  \
    X(TestStartAppends)                                                                \
    X(TestStartError)                                                                  \
    X(TestBuilderResourceError)                                                        \
    X(TestFinishError)                                                                 \
    X(TestBuilder)                                                                     \
    X(TestResourcePack)                                                                \
    X(TestResourcePackLength)                                                          \
    X(TestOptionPackUnpack)                                                            \
    X(TestUnknownPackUnpack)                                                           \
    X(TestParseUnknownResource)                                                        \
    X(TestGoString)                                                                    \
    X(TestParsingAllocs)                                                               \
    X(TestBuildingAllocs)                                                              \
    X(FuzzUnpackPackSeeds)                                                             \
    X(TestParseResourceHeaderMultipleTimes)                                            \
    X(TestParseDifferentResourceHeadersWithoutParsingRData)                            \
    X(TestParseWrongSection)                                                           \
    X(TestBuilderNameCompressionWithNonZeroedName)                                     \
    X(TestBuilderCompressionInAppendMode)                                              \
    X(TestInvalidMessages)                                                             \
    X(TestSVCBParamsRoundTrip)                                                         \
    X(TestSVCBParams)                                                                  \
    X(TestSVCBWireFormat)                                                              \
    X(TestSVCBPackLongValue)                                                           \
    X(TestFreeAfterUnpack)                                                             \
    X(TestFreeAfterBuild)                                                              \
    X(BenchmarkParsing)                                                                \
    X(BenchmarkBuilding)                                                               \
    X(BenchmarkPack)                                                                   \
    X(BenchmarkAppendPack)

TESTING_MAIN(TESTS)

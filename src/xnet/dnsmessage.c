/* Derived from Go's src/vendor/golang.org/x/net/dns/dnsmessage/message.go and
 * svcb.go.
 * Go source: go1.27.1, golang.org/x/net v0.55.1-0.20260731170536-c1d18010be90.
 *
 * Go's append is a DnsmsgBuf here, and every pack that fails puts the buffer's
 * length back where it was, which is what Go gets by handing back the slice it
 * was given. Go's map[string]uint16 for compression is a Map whose keys are
 * copies of the names in an arena of the map's own, because Go's keys are
 * strings that live as long as the map does.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "dnsmessage.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stdint.h>
#include <string.h>

/* ---------------------------------------------------------------- errors */

BURROW_SENTINEL_ERROR(burrow__dnsmsg_err_not_started,
                      "parsing/packing of this type isn't available yet");
BURROW_SENTINEL_ERROR(burrow__dnsmsg_err_section_done,
                      "parsing/packing of this section has completed");
BURROW_SENTINEL_ERROR(burrow__dnsmsg_err_base_len,
                      "insufficient data for base length type");
BURROW_SENTINEL_ERROR(burrow__dnsmsg_err_calc_len,
                      "insufficient data for calculated length type");
BURROW_SENTINEL_ERROR(burrow__dnsmsg_err_reserved, "segment prefix is reserved");
BURROW_SENTINEL_ERROR(burrow__dnsmsg_err_too_many_ptr, "too many pointers (>10)");
BURROW_SENTINEL_ERROR(burrow__dnsmsg_err_invalid_ptr, "invalid pointer");
BURROW_SENTINEL_ERROR(burrow__dnsmsg_err_invalid_name, "invalid dns name");
BURROW_SENTINEL_ERROR(burrow__dnsmsg_err_nil_resource_body, "nil resource body");
BURROW_SENTINEL_ERROR(burrow__dnsmsg_err_resource_len,
                      "insufficient data for resource body length");
BURROW_SENTINEL_ERROR(burrow__dnsmsg_err_seg_too_long, "segment length too long");
BURROW_SENTINEL_ERROR(burrow__dnsmsg_err_name_too_long, "name too long");
BURROW_SENTINEL_ERROR(burrow__dnsmsg_err_zero_seg_len, "zero length segment");
BURROW_SENTINEL_ERROR(burrow__dnsmsg_err_res_too_long, "resource length too long");
BURROW_SENTINEL_ERROR(burrow__dnsmsg_err_too_many_questions,
                      "too many Questions to pack (>65535)");
BURROW_SENTINEL_ERROR(burrow__dnsmsg_err_too_many_answers,
                      "too many Answers to pack (>65535)");
BURROW_SENTINEL_ERROR(burrow__dnsmsg_err_too_many_authorities,
                      "too many Authorities to pack (>65535)");
BURROW_SENTINEL_ERROR(burrow__dnsmsg_err_too_many_additionals,
                      "too many Additionals to pack (>65535)");
BURROW_SENTINEL_ERROR(burrow__dnsmsg_err_non_canonical_name,
                      "name is not in canonical format (it must end with a .)");
BURROW_SENTINEL_ERROR(burrow__dnsmsg_err_string_too_long,
                      "character string exceeds maximum length (255)");
BURROW_SENTINEL_ERROR(burrow__dnsmsg_err_param_out_of_order, "parameter out of order");
BURROW_SENTINEL_ERROR(burrow__dnsmsg_err_too_long_svcb_value,
                      "value too long (>65535 bytes)");

#define DM_OOM burrow_err_out_of_memory

static bool dm_is(Error a, Error b) {
    return a.vt == b.vt && a.data == b.data;
}

/* nestedError. s is always a literal, so only the text needs a home. */
typedef struct DmNested {
    Str s;
    Error inner;
    Str message;
} DmNested;

static Str dm_nested_message(const void *self) {
    return ((const DmNested *)self)->message;
}

static Error dm_nested_clone(const void *self, Alloc *a);

static const ErrorVT dm_nested_vt = {
    .message = dm_nested_message,
    .clone = dm_nested_clone,
};

/* A nestedError in a, except that running out of memory, which Go has no
 * error for, comes back as it is so that the caller can still see it. */
static Error dm_nested_in(Alloc *a, Str s, Error inner) {
    if (dm_is(inner, DM_OOM))
        return inner;
    Str text = error_text(inner);
    Int n = s.len + 2 + text.len;
    DmNested *e = (DmNested *)mem_alloc_nozero(a, sizeof(DmNested) + (size_t)n,
                                               _Alignof(DmNested));
    if (e == NULL)
        return DM_OOM;
    Byte *p = (Byte *)(e + 1);
    memcpy(p, s.p, (size_t)s.len);
    p[s.len] = ':';
    p[s.len + 1] = ' ';
    if (text.len > 0)
        memcpy(p + s.len + 2, text.p, (size_t)text.len);
    e->s = s;
    e->inner = inner;
    e->message = str_from_bytes(p, n);
    return (Error){&dm_nested_vt, e};
}

static Error dm_nested_clone(const void *self, Alloc *a) {
    const DmNested *e = (const DmNested *)self;
    Error inner = error_retain(a, e->inner);
    if (dm_is(inner, DM_OOM))
        return inner;
    return dm_nested_in(a, e->s, inner);
}

static Error dm_nested(Str s, Error inner) {
    return dm_nested_in(error_allocator(), s, inner);
}

bool burrow__dnsmsg_error_nested(Error err, Str *prefix, Error *inner) {
    if (err.vt != &dm_nested_vt)
        return false;
    const DmNested *e = (const DmNested *)err.data;
    if (prefix != NULL)
        *prefix = e->s;
    if (inner != NULL)
        *inner = e->inner;
    return true;
}

/* ------------------------------------------------------------- printing */

Int burrow__dnsmsg_print_padded_uint8(Byte buf[3], uint8_t i) {
    buf[0] = (Byte)(i / 100 + '0');
    buf[1] = (Byte)(i / 10 % 10 + '0');
    buf[2] = (Byte)(i % 10 + '0');
    return 3;
}

Int burrow__dnsmsg_print_uint8_bytes(Byte buf[3], uint8_t i) {
    Int n = 0;
    if (i >= 100)
        buf[n++] = (Byte)(i / 100 + '0');
    if (i >= 10)
        buf[n++] = (Byte)(i / 10 % 10 + '0');
    buf[n++] = (Byte)(i % 10 + '0');
    return n;
}

Int burrow__dnsmsg_print_uint32(Byte buf[10], uint32_t i) {
    Int n = 0;
    for (uint32_t d = 1000000000U; d > 0; d /= 10) {
        Byte c = (Byte)(i / d % 10 + '0');
        if (c != '0' || n > 0 || d == 1)
            buf[n++] = c;
        i %= d;
    }
    return n;
}

static Str dm_decimal(uint16_t v, Byte buf[5]) {
    Byte tmp[10];
    Int n = burrow__dnsmsg_print_uint32(tmp, v);
    memcpy(buf, tmp, (size_t)n);
    return str_from_bytes(buf, n);
}

/* The names Go's maps give, without the prefix String drops. */
static Str dm_type_name(DnsmsgType t) {
    switch (t) {
    case DNSMSG_TYPE_A:
        return BURROW_S("TypeA");
    case DNSMSG_TYPE_NS:
        return BURROW_S("TypeNS");
    case DNSMSG_TYPE_CNAME:
        return BURROW_S("TypeCNAME");
    case DNSMSG_TYPE_SOA:
        return BURROW_S("TypeSOA");
    case DNSMSG_TYPE_PTR:
        return BURROW_S("TypePTR");
    case DNSMSG_TYPE_MX:
        return BURROW_S("TypeMX");
    case DNSMSG_TYPE_TXT:
        return BURROW_S("TypeTXT");
    case DNSMSG_TYPE_AAAA:
        return BURROW_S("TypeAAAA");
    case DNSMSG_TYPE_SRV:
        return BURROW_S("TypeSRV");
    case DNSMSG_TYPE_OPT:
        return BURROW_S("TypeOPT");
    case DNSMSG_TYPE_SVCB:
        return BURROW_S("TypeSVCB");
    case DNSMSG_TYPE_HTTPS:
        return BURROW_S("TypeHTTPS");
    case DNSMSG_TYPE_WKS:
        return BURROW_S("TypeWKS");
    case DNSMSG_TYPE_HINFO:
        return BURROW_S("TypeHINFO");
    case DNSMSG_TYPE_MINFO:
        return BURROW_S("TypeMINFO");
    case DNSMSG_TYPE_AXFR:
        return BURROW_S("TypeAXFR");
    case DNSMSG_TYPE_ALL:
        return BURROW_S("TypeALL");
    default:
        return BURROW_STR_EMPTY;
    }
}

static Str dm_class_name(DnsmsgClass c) {
    switch (c) {
    case DNSMSG_CLASS_INET:
        return BURROW_S("ClassINET");
    case DNSMSG_CLASS_CSNET:
        return BURROW_S("ClassCSNET");
    case DNSMSG_CLASS_CHAOS:
        return BURROW_S("ClassCHAOS");
    case DNSMSG_CLASS_HESIOD:
        return BURROW_S("ClassHESIOD");
    case DNSMSG_CLASS_ANY:
        return BURROW_S("ClassANY");
    default:
        return BURROW_STR_EMPTY;
    }
}

static Str dm_rcode_name(DnsmsgRCode r) {
    switch (r) {
    case DNSMSG_RCODE_SUCCESS:
        return BURROW_S("RCodeSuccess");
    case DNSMSG_RCODE_FORMAT_ERROR:
        return BURROW_S("RCodeFormatError");
    case DNSMSG_RCODE_SERVER_FAILURE:
        return BURROW_S("RCodeServerFailure");
    case DNSMSG_RCODE_NAME_ERROR:
        return BURROW_S("RCodeNameError");
    case DNSMSG_RCODE_NOT_IMPLEMENTED:
        return BURROW_S("RCodeNotImplemented");
    case DNSMSG_RCODE_REFUSED:
        return BURROW_S("RCodeRefused");
    default:
        return BURROW_STR_EMPTY;
    }
}

static Str dm_svc_param_key_name(DnsmsgSVCParamKey k) {
    switch (k) {
    case DNSMSG_SVC_PARAM_MANDATORY:
        return BURROW_S("Mandatory");
    case DNSMSG_SVC_PARAM_ALPN:
        return BURROW_S("ALPN");
    case DNSMSG_SVC_PARAM_NO_DEFAULT_ALPN:
        return BURROW_S("NoDefaultALPN");
    case DNSMSG_SVC_PARAM_PORT:
        return BURROW_S("Port");
    case DNSMSG_SVC_PARAM_IPV4_HINT:
        return BURROW_S("IPv4Hint");
    case DNSMSG_SVC_PARAM_ECH:
        return BURROW_S("ECH");
    case DNSMSG_SVC_PARAM_IPV6_HINT:
        return BURROW_S("IPv6Hint");
    case DNSMSG_SVC_PARAM_DOH_PATH:
        return BURROW_S("DOHPath");
    case DNSMSG_SVC_PARAM_OHTTP:
        return BURROW_S("OHTTP");
    case DNSMSG_SVC_PARAM_TLS_SUPPORTED_GROUPS:
        return BURROW_S("TLSSupportedGroups");
    default:
        return BURROW_STR_EMPTY;
    }
}

Str burrow__dnsmsg_type_string(DnsmsgType t, Byte buf[5]) {
    Str n = dm_type_name(t);
    return n.len > 0 ? n : dm_decimal(t, buf);
}

Str burrow__dnsmsg_class_string(DnsmsgClass c, Byte buf[5]) {
    Str n = dm_class_name(c);
    return n.len > 0 ? n : dm_decimal(c, buf);
}

Str burrow__dnsmsg_rcode_string(DnsmsgRCode r, Byte buf[5]) {
    Str n = dm_rcode_name(r);
    return n.len > 0 ? n : dm_decimal(r, buf);
}

Str burrow__dnsmsg_svc_param_key_string(DnsmsgSVCParamKey k, Byte buf[5]) {
    Str n = dm_svc_param_key_name(k);
    return n.len > 0 ? n : dm_decimal(k, buf);
}

/* A GoString is written twice, once with p NULL to count and once into memory
 * of exactly that size. */
typedef struct DmW {
    Byte *p;
    Int n;
} DmW;

static void dm_w(DmW *w, const Byte *s, Int n) {
    if (w->p != NULL && n > 0)
        memcpy(w->p + w->n, s, (size_t)n);
    w->n += n;
}

static void dm_ws(DmW *w, Str s) {
    dm_w(w, s.p, s.len);
}

#define DM_WL(w, lit) dm_w((w), (const Byte *)("" lit), (Int)sizeof(lit) - 1)

static void dm_w_uint32(DmW *w, uint32_t v) {
    Byte buf[10];
    dm_w(w, buf, burrow__dnsmsg_print_uint32(buf, v));
}

static void dm_w_bool(DmW *w, bool b) {
    if (b)
        DM_WL(w, "true");
    else
        DM_WL(w, "false");
}

/* printByteSlice. */
static void dm_w_byte_slice(DmW *w, const Byte *p, Int n) {
    for (Int i = 0; i < n; i++) {
        if (i > 0)
            DM_WL(w, ", ");
        Byte buf[3];
        dm_w(w, buf, burrow__dnsmsg_print_uint8_bytes(buf, p[i]));
    }
}

/* printString. */
static void dm_w_print_string(DmW *w, const Byte *p, Int n) {
    static const char hex[] = "0123456789abcdef";
    for (Int i = 0; i < n; i++) {
        Byte c = p[i];
        if (c == '.' || c == '-' || c == ' ' || (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            dm_w(w, &c, 1);
            continue;
        }
        Byte e[4] = {'\\', 'x', (Byte)hex[(unsigned)c >> 4U],
                     (Byte)hex[(unsigned)c & 0xFU]};
        dm_w(w, e, 4);
    }
}

/* The GoString of a Type, Class or RCode: the name with the package in front,
 * or the number. */
static void dm_w_code(DmW *w, Str name, uint16_t v) {
    if (name.len > 0) {
        DM_WL(w, "dnsmessage.");
        dm_ws(w, name);
    } else {
        dm_w_uint32(w, v);
    }
}

static void dm_w_name(DmW *w, const DnsmsgName *n) {
    DM_WL(w, "dnsmessage.MustNewName(\"");
    dm_w_print_string(w, n->data, n->length);
    DM_WL(w, "\")");
}

static void dm_w_header(DmW *w, const DnsmsgHeader *h) {
    DM_WL(w, "dnsmessage.Header{ID: ");
    dm_w_uint32(w, h->id);
    DM_WL(w, ", Response: ");
    dm_w_bool(w, h->response);
    DM_WL(w, ", OpCode: ");
    dm_w_uint32(w, h->op_code);
    DM_WL(w, ", Authoritative: ");
    dm_w_bool(w, h->authoritative);
    DM_WL(w, ", Truncated: ");
    dm_w_bool(w, h->truncated);
    DM_WL(w, ", RecursionDesired: ");
    dm_w_bool(w, h->recursion_desired);
    DM_WL(w, ", RecursionAvailable: ");
    dm_w_bool(w, h->recursion_available);
    DM_WL(w, ", AuthenticData: ");
    dm_w_bool(w, h->authentic_data);
    DM_WL(w, ", CheckingDisabled: ");
    dm_w_bool(w, h->checking_disabled);
    DM_WL(w, ", RCode: ");
    dm_w_code(w, dm_rcode_name(h->rcode), h->rcode);
    DM_WL(w, "}");
}

static void dm_w_question(DmW *w, const DnsmsgQuestion *q) {
    DM_WL(w, "dnsmessage.Question{Name: ");
    dm_w_name(w, &q->name);
    DM_WL(w, ", Type: ");
    dm_w_code(w, dm_type_name(q->type), q->type);
    DM_WL(w, ", Class: ");
    dm_w_code(w, dm_class_name(q->class_), q->class_);
    DM_WL(w, "}");
}

static void dm_w_resource_header(DmW *w, const DnsmsgResourceHeader *h) {
    DM_WL(w, "dnsmessage.ResourceHeader{Name: ");
    dm_w_name(w, &h->name);
    DM_WL(w, ", Type: ");
    dm_w_code(w, dm_type_name(h->type), h->type);
    DM_WL(w, ", Class: ");
    dm_w_code(w, dm_class_name(h->class_), h->class_);
    DM_WL(w, ", TTL: ");
    dm_w_uint32(w, h->ttl);
    DM_WL(w, ", Length: ");
    dm_w_uint32(w, h->length);
    DM_WL(w, "}");
}

static void dm_w_svcb(DmW *w, const DnsmsgSVCBResource *r) {
    DM_WL(w, "dnsmessage.SVCBResource{Priority: ");
    dm_w_uint32(w, r->priority);
    DM_WL(w, ", Target: ");
    dm_w_name(w, &r->target);
    DM_WL(w, ", Params: []dnsmessage.SVCParam{");
    for (Int i = 0; i < r->params.len; i++) {
        const DnsmsgSVCParam *p = &r->params.p[i];
        if (i > 0)
            DM_WL(w, ", ");
        DM_WL(w, "dnsmessage.SVCParam{Key: ");
        Str name = dm_svc_param_key_name(p->key);
        if (name.len > 0) {
            DM_WL(w, "dnsmessage.SVCParam");
            dm_ws(w, name);
        } else {
            dm_w_uint32(w, p->key);
        }
        DM_WL(w, ", Value: []byte{");
        dm_w_byte_slice(w, (const Byte *)p->value.p, p->value.len);
        DM_WL(w, "}}");
    }
    DM_WL(w, "}}");
}

static BURROW_NORETURN void dm_nil_body(void) {
    panic_str(
        BURROW_S("runtime error: invalid memory address or nil pointer dereference"));
}

static void dm_w_body(DmW *w, const DnsmsgResourceBody *b) {
    switch (b->kind) {
    case DNSMSG_BODY_A:
        DM_WL(w, "dnsmessage.AResource{A: [4]byte{");
        dm_w_byte_slice(w, b->u.a.a, 4);
        DM_WL(w, "}}");
        break;
    case DNSMSG_BODY_NS:
        DM_WL(w, "dnsmessage.NSResource{NS: ");
        dm_w_name(w, &b->u.ns.ns);
        DM_WL(w, "}");
        break;
    case DNSMSG_BODY_CNAME:
        DM_WL(w, "dnsmessage.CNAMEResource{CNAME: ");
        dm_w_name(w, &b->u.cname.cname);
        DM_WL(w, "}");
        break;
    case DNSMSG_BODY_SOA: {
        const DnsmsgSOAResource *r = &b->u.soa;
        DM_WL(w, "dnsmessage.SOAResource{NS: ");
        dm_w_name(w, &r->ns);
        DM_WL(w, ", MBox: ");
        dm_w_name(w, &r->mbox);
        DM_WL(w, ", Serial: ");
        dm_w_uint32(w, r->serial);
        DM_WL(w, ", Refresh: ");
        dm_w_uint32(w, r->refresh);
        DM_WL(w, ", Retry: ");
        dm_w_uint32(w, r->retry);
        DM_WL(w, ", Expire: ");
        dm_w_uint32(w, r->expire);
        DM_WL(w, ", MinTTL: ");
        dm_w_uint32(w, r->min_ttl);
        DM_WL(w, "}");
        break;
    }
    case DNSMSG_BODY_PTR:
        DM_WL(w, "dnsmessage.PTRResource{PTR: ");
        dm_w_name(w, &b->u.ptr.ptr);
        DM_WL(w, "}");
        break;
    case DNSMSG_BODY_MX:
        DM_WL(w, "dnsmessage.MXResource{Pref: ");
        dm_w_uint32(w, b->u.mx.pref);
        DM_WL(w, ", MX: ");
        dm_w_name(w, &b->u.mx.mx);
        DM_WL(w, "}");
        break;
    case DNSMSG_BODY_TXT: {
        const DnsmsgStrs *t = &b->u.txt.txt;
        DM_WL(w, "dnsmessage.TXTResource{TXT: []string{");
        if (t->len == 0) {
            DM_WL(w, "}}");
            break;
        }
        for (Int i = 0; i < t->len; i++) {
            if (i == 0)
                DM_WL(w, "\"");
            else
                DM_WL(w, "\", \"");
            dm_w_print_string(w, t->p[i].p, t->p[i].len);
        }
        DM_WL(w, "\"}}");
        break;
    }
    case DNSMSG_BODY_AAAA:
        DM_WL(w, "dnsmessage.AAAAResource{AAAA: [16]byte{");
        dm_w_byte_slice(w, b->u.aaaa.aaaa, 16);
        DM_WL(w, "}}");
        break;
    case DNSMSG_BODY_SRV: {
        const DnsmsgSRVResource *r = &b->u.srv;
        DM_WL(w, "dnsmessage.SRVResource{Priority: ");
        dm_w_uint32(w, r->priority);
        DM_WL(w, ", Weight: ");
        dm_w_uint32(w, r->weight);
        DM_WL(w, ", Port: ");
        dm_w_uint32(w, r->port);
        DM_WL(w, ", Target: ");
        dm_w_name(w, &r->target);
        DM_WL(w, "}");
        break;
    }
    case DNSMSG_BODY_SVCB:
        dm_w_svcb(w, &b->u.svcb);
        break;
    case DNSMSG_BODY_HTTPS:
        DM_WL(w, "dnsmessage.HTTPSResource{SVCBResource: ");
        dm_w_svcb(w, &b->u.https.svcb);
        DM_WL(w, "}");
        break;
    case DNSMSG_BODY_OPT: {
        const DnsmsgOptions *o = &b->u.opt.options;
        DM_WL(w, "dnsmessage.OPTResource{Options: []dnsmessage.Option{");
        for (Int i = 0; i < o->len; i++) {
            if (i > 0)
                DM_WL(w, ", ");
            DM_WL(w, "dnsmessage.Option{Code: ");
            dm_w_uint32(w, o->p[i].code);
            DM_WL(w, ", Data: []byte{");
            dm_w_byte_slice(w, (const Byte *)o->p[i].data.p, o->p[i].data.len);
            DM_WL(w, "}}");
        }
        DM_WL(w, "}}");
        break;
    }
    case DNSMSG_BODY_UNKNOWN:
        DM_WL(w, "dnsmessage.UnknownResource{Type: ");
        dm_w_code(w, dm_type_name(b->u.unknown.type), b->u.unknown.type);
        DM_WL(w, ", Data: []byte{");
        dm_w_byte_slice(w, (const Byte *)b->u.unknown.data.p, b->u.unknown.data.len);
        DM_WL(w, "}}");
        break;
    case DNSMSG_BODY_NONE:
    default:
        dm_nil_body();
    }
}

static void dm_w_resource(DmW *w, const DnsmsgResource *r) {
    if (r->body.kind == DNSMSG_BODY_NONE)
        dm_nil_body();
    DM_WL(w, "dnsmessage.Resource{Header: ");
    dm_w_resource_header(w, &r->header);
    DM_WL(w, ", Body: &");
    dm_w_body(w, &r->body);
    DM_WL(w, "}");
}

static void dm_w_resources(DmW *w, const DnsmsgResources *rs) {
    for (Int i = 0; i < rs->len; i++) {
        if (i > 0)
            DM_WL(w, ", ");
        dm_w_resource(w, &rs->p[i]);
    }
}

static void dm_w_message(DmW *w, const DnsmsgMessage *m) {
    DM_WL(w, "dnsmessage.Message{Header: ");
    dm_w_header(w, &m->header);
    DM_WL(w, ", Questions: []dnsmessage.Question{");
    for (Int i = 0; i < m->questions.len; i++) {
        if (i > 0)
            DM_WL(w, ", ");
        dm_w_question(w, &m->questions.p[i]);
    }
    DM_WL(w, "}, Answers: []dnsmessage.Resource{");
    dm_w_resources(w, &m->answers);
    DM_WL(w, "}, Authorities: []dnsmessage.Resource{");
    dm_w_resources(w, &m->authorities);
    DM_WL(w, "}, Additionals: []dnsmessage.Resource{");
    dm_w_resources(w, &m->additionals);
    DM_WL(w, "}}");
}

typedef void (*DmWriter)(DmW *w, const void *x);

static Str dm_go_string(Alloc *a, DmWriter f, const void *x) {
    DmW w = {NULL, 0};
    f(&w, x);
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)w.n, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    w = (DmW){p, 0};
    f(&w, x);
    return str_from_bytes(p, w.n);
}

static void dm_wf_message(DmW *w, const void *x) {
    dm_w_message(w, x);
}
static void dm_wf_header(DmW *w, const void *x) {
    dm_w_header(w, x);
}
static void dm_wf_question(DmW *w, const void *x) {
    dm_w_question(w, x);
}
static void dm_wf_resource(DmW *w, const void *x) {
    dm_w_resource(w, x);
}
static void dm_wf_resource_header(DmW *w, const void *x) {
    dm_w_resource_header(w, x);
}
static void dm_wf_body(DmW *w, const void *x) {
    dm_w_body(w, x);
}
static void dm_wf_name(DmW *w, const void *x) {
    dm_w_name(w, x);
}

Str burrow__dnsmsg_message_go_string(Alloc *a, const DnsmsgMessage *m) {
    return dm_go_string(a, dm_wf_message, m);
}

Str burrow__dnsmsg_header_go_string(Alloc *a, const DnsmsgHeader *h) {
    return dm_go_string(a, dm_wf_header, h);
}

Str burrow__dnsmsg_question_go_string(Alloc *a, const DnsmsgQuestion *q) {
    return dm_go_string(a, dm_wf_question, q);
}

Str burrow__dnsmsg_resource_go_string(Alloc *a, const DnsmsgResource *r) {
    return dm_go_string(a, dm_wf_resource, r);
}

Str burrow__dnsmsg_resource_header_go_string(Alloc *a, const DnsmsgResourceHeader *h) {
    return dm_go_string(a, dm_wf_resource_header, h);
}

Str burrow__dnsmsg_body_go_string(Alloc *a, const DnsmsgResourceBody *b) {
    return dm_go_string(a, dm_wf_body, b);
}

Str burrow__dnsmsg_name_go_string(Alloc *a, const DnsmsgName *n) {
    return dm_go_string(a, dm_wf_name, n);
}

/* ----------------------------------------------------------------- names */

Error burrow__dnsmsg_new_name(Str s, DnsmsgName *out) {
    memset(out, 0, sizeof *out);
    if (s.len > (Int)sizeof out->data)
        return burrow__dnsmsg_err_calc_len;
    if (s.len > 0)
        memcpy(out->data, s.p, (size_t)s.len);
    out->length = (uint8_t)s.len;
    return BURROW_NO_ERROR;
}

DnsmsgName burrow__dnsmsg_must_new_name(Str s) {
    DnsmsgName n;
    if (BURROW_FAILED(burrow__dnsmsg_new_name(s, &n)))
        panic_str(
            BURROW_S("creating name: insufficient data for calculated length type"));
    return n;
}

Str burrow__dnsmsg_name_string(const DnsmsgName *n) {
    return str_from_bytes(n->data, n->length);
}

/* ---------------------------------------------------------- the buffer */

/* Go's packStartingCap. */
#define DM_STARTING_CAP 512
#define DM_HEADER_LEN 12

DnsmsgBuf burrow__dnsmsg_buf(Alloc *a, Slice b) {
    return (DnsmsgBuf){a, (Byte *)b.p, b.len, b.cap, false};
}

Slice burrow__dnsmsg_buf_slice(const DnsmsgBuf *b) {
    return slice_from(b->p, b->len, b->cap, TYPE_BYTE);
}

void burrow__dnsmsg_buf_free(DnsmsgBuf *b) {
    if (b->owned)
        mem_free(b->a, b->p, (size_t)b->cap, 1);
    *b = (DnsmsgBuf){b->a, NULL, 0, 0, false};
}

/* Room for n more, the way append finds it. */
static bool dm_reserve(DnsmsgBuf *b, Int n) {
    if (b->cap - b->len >= n)
        return true;
    if (b->a == NULL)
        return false;
    Int want = b->len + n;
    Int ncap = b->cap * 2;
    if (ncap < want)
        ncap = want;
    Byte *p = (Byte *)mem_alloc_nozero(b->a, (size_t)ncap, 1);
    if (p == NULL)
        return false;
    if (b->len > 0)
        memcpy(p, b->p, (size_t)b->len);
    if (b->owned)
        mem_free(b->a, b->p, (size_t)b->cap, 1);
    b->p = p;
    b->cap = ncap;
    b->owned = true;
    return true;
}

static bool dm_put(DnsmsgBuf *b, const Byte *p, Int n) {
    if (n == 0)
        return true;
    if (!dm_reserve(b, n))
        return false;
    memcpy(b->p + b->len, p, (size_t)n);
    b->len += n;
    return true;
}

static bool dm_put16(DnsmsgBuf *b, uint16_t v) {
    Byte x[2] = {(Byte)((unsigned)v >> 8U), (Byte)v};
    return dm_put(b, x, 2);
}

static bool dm_put32(DnsmsgBuf *b, uint32_t v) {
    Byte x[4] = {(Byte)(v >> 24U), (Byte)(v >> 16U), (Byte)(v >> 8U), (Byte)v};
    return dm_put(b, x, 4);
}

/* Back to len with err, which is how every pack fails. */
static Error dm_undo(DnsmsgBuf *b, Int len, Error err) {
    b->len = len;
    return err;
}

/* ---------------------------------------------------------- compression */

struct DnsmsgCompression {
    Alloc *a;
    Arena ar;
    Map *m; /* Str to uint16_t, keys in ar */
};

DnsmsgCompression *burrow__dnsmsg_compression_new(Alloc *a) {
    DnsmsgCompression *c =
        (DnsmsgCompression *)mem_alloc(a, sizeof *c, _Alignof(DnsmsgCompression));
    if (c == NULL)
        return NULL;
    c->a = a;
    arena_init(&c->ar, a, 0);
    c->m = map_make(arena_allocator(&c->ar), TYPE_STRING, TYPE_UINT16, 0);
    if (c->m == NULL) {
        arena_free(&c->ar);
        mem_free(a, c, sizeof *c, _Alignof(DnsmsgCompression));
        return NULL;
    }
    return c;
}

void burrow__dnsmsg_compression_free(DnsmsgCompression *c) {
    if (c == NULL)
        return;
    arena_free(&c->ar);
    mem_free(c->a, c, sizeof *c, _Alignof(DnsmsgCompression));
}

/* ------------------------------------------------------------ unpacking */

static Error dm_u16(Slice msg, Int *off, uint16_t *v) {
    if (*off + 2 > msg.len)
        return burrow__dnsmsg_err_base_len;
    const Byte *p = (const Byte *)msg.p + *off;
    *v = (uint16_t)((unsigned)p[0] << 8U | p[1]);
    *off += 2;
    return BURROW_NO_ERROR;
}

static Error dm_u32(Slice msg, Int *off, uint32_t *v) {
    if (*off + 4 > msg.len)
        return burrow__dnsmsg_err_base_len;
    const Byte *p = (const Byte *)msg.p + *off;
    *v = (uint32_t)p[0] << 24U | (uint32_t)p[1] << 16U | (uint32_t)p[2] << 8U |
         (uint32_t)p[3];
    *off += 4;
    return BURROW_NO_ERROR;
}

static Error dm_skip(Slice msg, Int *off, Int n) {
    if (*off + n > msg.len)
        return burrow__dnsmsg_err_base_len;
    *off += n;
    return BURROW_NO_ERROR;
}

/* unpackBytes. */
static Error dm_bytes(Slice msg, Int off, Byte *field, Int n) {
    if (off + n > msg.len)
        return burrow__dnsmsg_err_base_len;
    if (n > 0)
        memcpy(field, (const Byte *)msg.p + off, (size_t)n);
    return BURROW_NO_ERROR;
}

/* ---------------------------------------------------------------- names */

/* Go's nonEncodedNameMax. */
#define DM_NAME_MAX 254

Error burrow__dnsmsg_name_pack(const DnsmsgName *n, DnsmsgBuf *msg,
                               DnsmsgCompression *c, Int compression_off) {
    Int old = msg->len;
    if (n->length > DM_NAME_MAX)
        return burrow__dnsmsg_err_name_too_long;
    if (n->length == 0 || n->data[n->length - 1] != '.')
        return burrow__dnsmsg_err_non_canonical_name;
    if (n->data[0] == '.' && n->length == 1)
        return dm_put(msg, (const Byte *)"", 1) ? BURROW_NO_ERROR : DM_OOM;

    Byte *name_as_str = NULL;
    for (Int i = 0, begin = 0; i < n->length; i++) {
        if (n->data[i] == '.') {
            if (i - begin >= 64)
                return dm_undo(msg, old, burrow__dnsmsg_err_seg_too_long);
            if (i - begin == 0)
                return dm_undo(msg, old, burrow__dnsmsg_err_zero_seg_len);
            Byte l = (Byte)(i - begin);
            if (!dm_put(msg, &l, 1) || !dm_put(msg, n->data + begin, i - begin))
                return dm_undo(msg, old, DM_OOM);
            begin = i + 1;
            continue;
        }
        if ((i == 0 || n->data[i - 1] == '.') && c != NULL) {
            Str key = {n->data + i, n->length - i};
            uint16_t ptr;
            if (map_get2(c->m, &key, &ptr)) {
                Byte x[2] = {(Byte)((unsigned)ptr >> 8U | 0xC0U), (Byte)ptr};
                return dm_put(msg, x, 2) ? BURROW_NO_ERROR : dm_undo(msg, old, DM_OOM);
            }
            Int new_ptr = msg->len - compression_off;
            if (new_ptr <= 0x3FFF) {
                if (name_as_str == NULL) {
                    name_as_str =
                        (Byte *)mem_alloc_nozero(arena_allocator(&c->ar), n->length, 1);
                    if (name_as_str == NULL)
                        return dm_undo(msg, old, DM_OOM);
                    memcpy(name_as_str, n->data, n->length);
                }
                Str k = {name_as_str + i, n->length - i};
                uint16_t v = (uint16_t)new_ptr;
                if (!map_set(c->m, &k, &v))
                    return dm_undo(msg, old, DM_OOM);
            }
        }
    }
    return dm_put(msg, (const Byte *)"", 1) ? BURROW_NO_ERROR
                                            : dm_undo(msg, old, DM_OOM);
}

Error burrow__dnsmsg_name_unpack(DnsmsgName *n, Slice msg, Int off, Int *new_off) {
    const Byte *m = (const Byte *)msg.p;
    Int curr = off;
    Int next = off;
    int ptr = 0;
    Int len = 0;
    *new_off = off;
    for (;;) {
        if (curr >= msg.len)
            return burrow__dnsmsg_err_base_len;
        int c = m[curr++];
        switch ((unsigned)c & 0xC0U) {
        case 0x00: {
            if (c == 0x00)
                goto done;
            Int end = curr + c;
            if (end > msg.len)
                return burrow__dnsmsg_err_calc_len;
            if (memchr(m + curr, '.', (size_t)c) != NULL)
                return burrow__dnsmsg_err_invalid_name;
            if (len + c >= DM_NAME_MAX)
                return burrow__dnsmsg_err_name_too_long;
            memcpy(n->data + len, m + curr, (size_t)c);
            len += c;
            n->data[len++] = '.';
            curr = end;
            break;
        }
        case 0xC0: {
            if (curr >= msg.len)
                return burrow__dnsmsg_err_invalid_ptr;
            int c1 = m[curr++];
            if (ptr == 0)
                next = curr;
            if (++ptr > 10)
                return burrow__dnsmsg_err_too_many_ptr;
            curr = (Int)(((unsigned)c ^ 0xC0U) << 8U | (unsigned)c1);
            break;
        }
        default:
            return burrow__dnsmsg_err_reserved;
        }
    }
done:
    if (len == 0)
        n->data[len++] = '.';
    n->length = (uint8_t)len;
    if (ptr == 0)
        next = curr;
    *new_off = next;
    return BURROW_NO_ERROR;
}

/* skipName. */
static Error dm_skip_name(Slice msg, Int *off) {
    const Byte *m = (const Byte *)msg.p;
    Int next = *off;
    for (;;) {
        if (next >= msg.len)
            return burrow__dnsmsg_err_base_len;
        int c = m[next++];
        switch ((unsigned)c & 0xC0U) {
        case 0x00:
            if (c == 0x00) {
                *off = next;
                return BURROW_NO_ERROR;
            }
            next += c;
            if (next > msg.len)
                return burrow__dnsmsg_err_calc_len;
            break;
        case 0xC0:
            *off = next + 1;
            return BURROW_NO_ERROR;
        default:
            return burrow__dnsmsg_err_reserved;
        }
    }
}

/* -------------------------------------------------------- the headers */

static void dm_header_pack(const DnsmsgHeader *h, uint16_t *id, uint16_t *bits) {
    *id = h->id;
    uint16_t b = (uint16_t)((unsigned)h->op_code << 11U | (unsigned)h->rcode);
    if (h->recursion_available)
        b = (uint16_t)(b | 1U << 7U);
    if (h->recursion_desired)
        b = (uint16_t)(b | 1U << 8U);
    if (h->truncated)
        b = (uint16_t)(b | 1U << 9U);
    if (h->authoritative)
        b = (uint16_t)(b | 1U << 10U);
    if (h->response)
        b = (uint16_t)(b | 1U << 15U);
    if (h->authentic_data)
        b = (uint16_t)(b | 1U << 5U);
    if (h->checking_disabled)
        b = (uint16_t)(b | 1U << 4U);
    *bits = b;
}

static DnsmsgHeader dm_header_of(const DnsmsgWireHeader *w) {
    return (DnsmsgHeader){
        .id = w->id,
        .response = ((unsigned)w->bits & 1U << 15U) != 0,
        .op_code = (DnsmsgOpCode)((unsigned)w->bits >> 11U & 0xFU),
        .authoritative = ((unsigned)w->bits & 1U << 10U) != 0,
        .truncated = ((unsigned)w->bits & 1U << 9U) != 0,
        .recursion_desired = ((unsigned)w->bits & 1U << 8U) != 0,
        .recursion_available = ((unsigned)w->bits & 1U << 7U) != 0,
        .authentic_data = ((unsigned)w->bits & 1U << 5U) != 0,
        .checking_disabled = ((unsigned)w->bits & 1U << 4U) != 0,
        .rcode = (DnsmsgRCode)((unsigned)w->bits & 0xFU),
    };
}

static void dm_wire_header_bytes(const DnsmsgWireHeader *h, Byte out[DM_HEADER_LEN]) {
    uint16_t v[6] = {h->id,      h->bits,        h->questions,
                     h->answers, h->authorities, h->additionals};
    for (Int i = 0; i < 6; i++) {
        out[2 * i] = (Byte)((unsigned)v[i] >> 8U);
        out[2 * i + 1] = (Byte)v[i];
    }
}

Error burrow__dnsmsg_wire_header_unpack(DnsmsgWireHeader *h, Slice msg, Int off,
                                        Int *new_off) {
    static const Str names[6] = {
        {(const Byte *)"id", 2},           {(const Byte *)"bits", 4},
        {(const Byte *)"questions", 9},    {(const Byte *)"answers", 7},
        {(const Byte *)"authorities", 11}, {(const Byte *)"additionals", 11},
    };
    uint16_t *fields[6] = {&h->id,      &h->bits,        &h->questions,
                           &h->answers, &h->authorities, &h->additionals};
    Int next = off;
    *new_off = off;
    for (int i = 0; i < 6; i++) {
        Error e = dm_u16(msg, &next, fields[i]);
        if (BURROW_FAILED(e))
            return dm_nested(names[i], e);
    }
    *new_off = next;
    return BURROW_NO_ERROR;
}

Error burrow__dnsmsg_resource_header_pack(const DnsmsgResourceHeader *h, DnsmsgBuf *msg,
                                          DnsmsgCompression *c, Int compression_off,
                                          Int *len_off) {
    Int old = msg->len;
    *len_off = 0;
    Error e = burrow__dnsmsg_name_pack(&h->name, msg, c, compression_off);
    if (BURROW_FAILED(e))
        return dm_nested(BURROW_S("Name"), e);
    if (!dm_put16(msg, h->type) || !dm_put16(msg, h->class_) || !dm_put32(msg, h->ttl))
        return dm_undo(msg, old, DM_OOM);
    Int at = msg->len;
    if (!dm_put16(msg, h->length))
        return dm_undo(msg, old, DM_OOM);
    *len_off = at;
    return BURROW_NO_ERROR;
}

Error burrow__dnsmsg_resource_header_unpack(DnsmsgResourceHeader *h, Slice msg, Int off,
                                            Int *new_off) {
    Int next = off;
    *new_off = off;
    Error e = burrow__dnsmsg_name_unpack(&h->name, msg, next, &next);
    if (BURROW_FAILED(e))
        return dm_nested(BURROW_S("Name"), e);
    e = dm_u16(msg, &next, &h->type);
    if (BURROW_FAILED(e))
        return dm_nested(BURROW_S("Type"), e);
    e = dm_u16(msg, &next, &h->class_);
    if (BURROW_FAILED(e))
        return dm_nested(BURROW_S("Class"), e);
    e = dm_u32(msg, &next, &h->ttl);
    if (BURROW_FAILED(e))
        return dm_nested(BURROW_S("TTL"), e);
    e = dm_u16(msg, &next, &h->length);
    if (BURROW_FAILED(e))
        return dm_nested(BURROW_S("Length"), e);
    *new_off = next;
    return BURROW_NO_ERROR;
}

/* fixLen. */
static Error dm_fix_len(DnsmsgResourceHeader *h, DnsmsgBuf *msg, Int len_off,
                        Int pre_len) {
    Int n = msg->len - pre_len;
    if (n > 0xFFFF)
        return burrow__dnsmsg_err_res_too_long;
    msg->p[len_off] = (Byte)((uint64_t)n >> 8U);
    msg->p[len_off + 1] = (Byte)n;
    h->length = (uint16_t)n;
    return BURROW_NO_ERROR;
}

Error burrow__dnsmsg_resource_header_set_edns0(DnsmsgResourceHeader *h,
                                               int udp_payload_len,
                                               DnsmsgRCode ext_rcode, bool dnssec_ok) {
    memset(&h->name, 0, sizeof h->name);
    h->name.data[0] = '.';
    h->name.length = 1;
    h->type = DNSMSG_TYPE_OPT;
    h->class_ = (DnsmsgClass)udp_payload_len;
    h->ttl = (uint32_t)ext_rcode >> 4U << 24U;
    if (dnssec_ok)
        h->ttl |= 0x00008000U;
    return BURROW_NO_ERROR;
}

bool burrow__dnsmsg_resource_header_dnssec_allowed(const DnsmsgResourceHeader *h) {
    return (h->ttl & 0x00ff8000U) == 0x00008000U;
}

DnsmsgRCode burrow__dnsmsg_resource_header_extended_rcode(const DnsmsgResourceHeader *h,
                                                          DnsmsgRCode rcode) {
    if ((h->ttl & 0x00ff0000U) == 0)
        return (DnsmsgRCode)((h->ttl >> 24U << 4U) | (unsigned)rcode);
    return rcode;
}

/* skipResource. */
static Error dm_skip_resource(Slice msg, Int *off) {
    Int next = *off;
    Error e = dm_skip_name(msg, &next);
    if (BURROW_FAILED(e))
        return dm_nested(BURROW_S("Name"), e);
    e = dm_skip(msg, &next, 2);
    if (BURROW_FAILED(e))
        return dm_nested(BURROW_S("Type"), e);
    e = dm_skip(msg, &next, 2);
    if (BURROW_FAILED(e))
        return dm_nested(BURROW_S("Class"), e);
    e = dm_skip(msg, &next, 4);
    if (BURROW_FAILED(e))
        return dm_nested(BURROW_S("TTL"), e);
    uint16_t length = 0;
    e = dm_u16(msg, &next, &length);
    if (BURROW_FAILED(e))
        return dm_nested(BURROW_S("Length"), e);
    next += length;
    if (next > msg.len)
        return burrow__dnsmsg_err_resource_len;
    *off = next;
    return BURROW_NO_ERROR;
}

Error burrow__dnsmsg_question_pack(const DnsmsgQuestion *q, DnsmsgBuf *msg,
                                   DnsmsgCompression *c, Int compression_off) {
    Int old = msg->len;
    Error e = burrow__dnsmsg_name_pack(&q->name, msg, c, compression_off);
    if (BURROW_FAILED(e))
        return dm_nested(BURROW_S("Name"), e);
    if (!dm_put16(msg, q->type) || !dm_put16(msg, q->class_))
        return dm_undo(msg, old, DM_OOM);
    return BURROW_NO_ERROR;
}

/* --------------------------------------------------------------- bodies */

static DnsmsgType dm_real_type(const DnsmsgResourceBody *b) {
    switch (b->kind) {
    case DNSMSG_BODY_A:
        return DNSMSG_TYPE_A;
    case DNSMSG_BODY_NS:
        return DNSMSG_TYPE_NS;
    case DNSMSG_BODY_CNAME:
        return DNSMSG_TYPE_CNAME;
    case DNSMSG_BODY_SOA:
        return DNSMSG_TYPE_SOA;
    case DNSMSG_BODY_PTR:
        return DNSMSG_TYPE_PTR;
    case DNSMSG_BODY_MX:
        return DNSMSG_TYPE_MX;
    case DNSMSG_BODY_TXT:
        return DNSMSG_TYPE_TXT;
    case DNSMSG_BODY_AAAA:
        return DNSMSG_TYPE_AAAA;
    case DNSMSG_BODY_SRV:
        return DNSMSG_TYPE_SRV;
    case DNSMSG_BODY_SVCB:
        return DNSMSG_TYPE_SVCB;
    case DNSMSG_BODY_HTTPS:
        return DNSMSG_TYPE_HTTPS;
    case DNSMSG_BODY_OPT:
        return DNSMSG_TYPE_OPT;
    case DNSMSG_BODY_UNKNOWN:
        return b->u.unknown.type;
    case DNSMSG_BODY_NONE:
    default:
        return 0;
    }
}

/* SVCBResource.pack. Go's previousKey is never moved on from zero, so the
 * only order it catches is a zero key after the first, and so does this. */
static Error dm_svcb_pack(const DnsmsgSVCBResource *r, DnsmsgBuf *msg) {
    Int old = msg->len;
    if (!dm_put16(msg, r->priority))
        return dm_undo(msg, old, DM_OOM);
    Error e = burrow__dnsmsg_name_pack(&r->target, msg, NULL, 0);
    if (BURROW_FAILED(e))
        return dm_undo(msg, old, dm_nested(BURROW_S("SVCBResource.Target"), e));
    DnsmsgSVCParamKey previous_key = 0;
    for (Int i = 0; i < r->params.len; i++) {
        const DnsmsgSVCParam *p = &r->params.p[i];
        if (i > 0 && p->key <= previous_key)
            return dm_undo(msg, old,
                           dm_nested(BURROW_S("SVCBResource.Params"),
                                     burrow__dnsmsg_err_param_out_of_order));
        if (p->value.len > 0xFFFF)
            return dm_undo(msg, old,
                           dm_nested(BURROW_S("SVCBResource.Params"),
                                     burrow__dnsmsg_err_too_long_svcb_value));
        if (!dm_put16(msg, p->key) || !dm_put16(msg, (uint16_t)p->value.len) ||
            !dm_put(msg, (const Byte *)p->value.p, p->value.len))
            return dm_undo(msg, old, DM_OOM);
    }
    return BURROW_NO_ERROR;
}

Error burrow__dnsmsg_body_pack(const DnsmsgResourceBody *b, DnsmsgBuf *msg,
                               DnsmsgCompression *c, Int compression_off) {
    Int old = msg->len;
    Error e;
    switch (b->kind) {
    case DNSMSG_BODY_A:
        return dm_put(msg, b->u.a.a, 4) ? BURROW_NO_ERROR : DM_OOM;
    case DNSMSG_BODY_AAAA:
        return dm_put(msg, b->u.aaaa.aaaa, 16) ? BURROW_NO_ERROR : DM_OOM;
    case DNSMSG_BODY_NS:
        return burrow__dnsmsg_name_pack(&b->u.ns.ns, msg, c, compression_off);
    case DNSMSG_BODY_CNAME:
        return burrow__dnsmsg_name_pack(&b->u.cname.cname, msg, c, compression_off);
    case DNSMSG_BODY_PTR:
        return burrow__dnsmsg_name_pack(&b->u.ptr.ptr, msg, c, compression_off);
    case DNSMSG_BODY_MX:
        if (!dm_put16(msg, b->u.mx.pref))
            return dm_undo(msg, old, DM_OOM);
        e = burrow__dnsmsg_name_pack(&b->u.mx.mx, msg, c, compression_off);
        if (BURROW_FAILED(e))
            return dm_undo(msg, old, dm_nested(BURROW_S("MXResource.MX"), e));
        return BURROW_NO_ERROR;
    case DNSMSG_BODY_SOA: {
        const DnsmsgSOAResource *r = &b->u.soa;
        e = burrow__dnsmsg_name_pack(&r->ns, msg, c, compression_off);
        if (BURROW_FAILED(e))
            return dm_undo(msg, old, dm_nested(BURROW_S("SOAResource.NS"), e));
        e = burrow__dnsmsg_name_pack(&r->mbox, msg, c, compression_off);
        if (BURROW_FAILED(e))
            return dm_undo(msg, old, dm_nested(BURROW_S("SOAResource.MBox"), e));
        if (!dm_put32(msg, r->serial) || !dm_put32(msg, r->refresh) ||
            !dm_put32(msg, r->retry) || !dm_put32(msg, r->expire) ||
            !dm_put32(msg, r->min_ttl))
            return dm_undo(msg, old, DM_OOM);
        return BURROW_NO_ERROR;
    }
    case DNSMSG_BODY_TXT:
        for (Int i = 0; i < b->u.txt.txt.len; i++) {
            Str s = b->u.txt.txt.p[i];
            if (s.len > 255)
                return dm_undo(msg, old, burrow__dnsmsg_err_string_too_long);
            Byte l = (Byte)s.len;
            if (!dm_put(msg, &l, 1) || !dm_put(msg, s.p, s.len))
                return dm_undo(msg, old, DM_OOM);
        }
        return BURROW_NO_ERROR;
    case DNSMSG_BODY_SRV: {
        const DnsmsgSRVResource *r = &b->u.srv;
        if (!dm_put16(msg, r->priority) || !dm_put16(msg, r->weight) ||
            !dm_put16(msg, r->port))
            return dm_undo(msg, old, DM_OOM);
        e = burrow__dnsmsg_name_pack(&r->target, msg, NULL, compression_off);
        if (BURROW_FAILED(e))
            return dm_undo(msg, old, dm_nested(BURROW_S("SRVResource.Target"), e));
        return BURROW_NO_ERROR;
    }
    case DNSMSG_BODY_SVCB:
        return dm_svcb_pack(&b->u.svcb, msg);
    case DNSMSG_BODY_HTTPS:
        return dm_svcb_pack(&b->u.https.svcb, msg);
    case DNSMSG_BODY_OPT:
        for (Int i = 0; i < b->u.opt.options.len; i++) {
            const DnsmsgOption *o = &b->u.opt.options.p[i];
            if (!dm_put16(msg, o->code) || !dm_put16(msg, (uint16_t)o->data.len) ||
                !dm_put(msg, (const Byte *)o->data.p, o->data.len))
                return dm_undo(msg, old, DM_OOM);
        }
        return BURROW_NO_ERROR;
    case DNSMSG_BODY_UNKNOWN:
        return dm_put(msg, (const Byte *)b->u.unknown.data.p, b->u.unknown.data.len)
                   ? BURROW_NO_ERROR
                   : DM_OOM;
    case DNSMSG_BODY_NONE:
    default:
        return burrow__dnsmsg_err_nil_resource_body;
    }
}

Error burrow__dnsmsg_resource_pack(DnsmsgResource *r, DnsmsgBuf *msg,
                                   DnsmsgCompression *c, Int compression_off) {
    if (r->body.kind == DNSMSG_BODY_NONE)
        return burrow__dnsmsg_err_nil_resource_body;
    Int old = msg->len;
    r->header.type = dm_real_type(&r->body);
    Int len_off;
    Error e = burrow__dnsmsg_resource_header_pack(&r->header, msg, c, compression_off,
                                                  &len_off);
    if (BURROW_FAILED(e))
        return dm_nested(BURROW_S("ResourceHeader"), e);
    Int pre_len = msg->len;
    e = burrow__dnsmsg_body_pack(&r->body, msg, c, compression_off);
    if (BURROW_FAILED(e))
        return dm_undo(msg, old, dm_nested(BURROW_S("content"), e));
    e = dm_fix_len(&r->header, msg, len_off, pre_len);
    if (BURROW_FAILED(e))
        return dm_undo(msg, old, e);
    return BURROW_NO_ERROR;
}

/* What an unpack makes. Zero bytes take no memory. */
static Error dm_copy_bytes(Alloc *a, const Byte *p, Int n, Slice *out) {
    *out = slice_from(NULL, 0, 0, TYPE_BYTE);
    if (n == 0)
        return BURROW_NO_ERROR;
    Byte *q = (Byte *)mem_alloc_nozero(a, (size_t)n, 1);
    if (q == NULL)
        return DM_OOM;
    memcpy(q, p, (size_t)n);
    *out = slice_from(q, n, n, TYPE_BYTE);
    return BURROW_NO_ERROR;
}

static void dm_bytes_free(Alloc *a, Slice *s) {
    if (s->p != NULL)
        mem_free(a, s->p, (size_t)s->cap, 1);
    *s = slice_from(NULL, 0, 0, TYPE_BYTE);
}

/* Room for one more in an array that doubles: the array to use, or NULL with p
 * and cap as they were. */
static void *dm_grow(Alloc *a, void *p, Int len, Int *cap, size_t size, size_t align) {
    if (p != NULL && len < *cap)
        return p;
    Int ncap = *cap == 0 ? 4 : *cap * 2;
    void *q = mem_alloc_nozero(a, (size_t)ncap * size, align);
    if (q == NULL)
        return NULL;
    if (len > 0)
        memcpy(q, p, (size_t)len * size);
    if (p != NULL)
        mem_free(a, p, (size_t)*cap * size, align);
    *cap = ncap;
    return q;
}

static Error dm_unpack_name_body(Slice msg, Int off, DnsmsgName *n) {
    Int next;
    return burrow__dnsmsg_name_unpack(n, msg, off, &next);
}

static Error dm_unpack_mx(Slice msg, Int off, DnsmsgMXResource *r) {
    Error e = dm_u16(msg, &off, &r->pref);
    if (BURROW_FAILED(e))
        return dm_nested(BURROW_S("Pref"), e);
    e = dm_unpack_name_body(msg, off, &r->mx);
    if (BURROW_FAILED(e))
        return dm_nested(BURROW_S("MX"), e);
    return BURROW_NO_ERROR;
}

static Error dm_unpack_soa(Slice msg, Int off, DnsmsgSOAResource *r) {
    Error e = burrow__dnsmsg_name_unpack(&r->ns, msg, off, &off);
    if (BURROW_FAILED(e))
        return dm_nested(BURROW_S("NS"), e);
    e = burrow__dnsmsg_name_unpack(&r->mbox, msg, off, &off);
    if (BURROW_FAILED(e))
        return dm_nested(BURROW_S("MBox"), e);
    e = dm_u32(msg, &off, &r->serial);
    if (BURROW_FAILED(e))
        return dm_nested(BURROW_S("Serial"), e);
    e = dm_u32(msg, &off, &r->refresh);
    if (BURROW_FAILED(e))
        return dm_nested(BURROW_S("Refresh"), e);
    e = dm_u32(msg, &off, &r->retry);
    if (BURROW_FAILED(e))
        return dm_nested(BURROW_S("Retry"), e);
    e = dm_u32(msg, &off, &r->expire);
    if (BURROW_FAILED(e))
        return dm_nested(BURROW_S("Expire"), e);
    e = dm_u32(msg, &off, &r->min_ttl);
    if (BURROW_FAILED(e))
        return dm_nested(BURROW_S("MinTTL"), e);
    return BURROW_NO_ERROR;
}

static Error dm_unpack_srv(Slice msg, Int off, DnsmsgSRVResource *r) {
    Error e = dm_u16(msg, &off, &r->priority);
    if (BURROW_FAILED(e))
        return dm_nested(BURROW_S("Priority"), e);
    e = dm_u16(msg, &off, &r->weight);
    if (BURROW_FAILED(e))
        return dm_nested(BURROW_S("Weight"), e);
    e = dm_u16(msg, &off, &r->port);
    if (BURROW_FAILED(e))
        return dm_nested(BURROW_S("Port"), e);
    e = dm_unpack_name_body(msg, off, &r->target);
    if (BURROW_FAILED(e))
        return dm_nested(BURROW_S("Target"), e);
    return BURROW_NO_ERROR;
}

static Error dm_unpack_txt(Alloc *a, Slice msg, Int off, uint16_t length,
                           DnsmsgTXTResource *r) {
    DnsmsgStrs t = {NULL, 0, 0};
    const Byte *m = (const Byte *)msg.p;
    Error e = BURROW_NO_ERROR;
    for (uint16_t n = 0; n < length;) {
        /* unpackText */
        if (off >= msg.len) {
            e = dm_nested(BURROW_S("text"), burrow__dnsmsg_err_base_len);
            break;
        }
        Int begin = off + 1;
        Int end = begin + m[off];
        if (end > msg.len) {
            e = dm_nested(BURROW_S("text"), burrow__dnsmsg_err_calc_len);
            break;
        }
        off = end;
        Int tlen = end - begin;
        if (length - n < tlen + 1) {
            e = burrow__dnsmsg_err_calc_len;
            break;
        }
        n = (uint16_t)(n + tlen + 1);
        Str s = BURROW_STR_EMPTY;
        if (tlen > 0) {
            Byte *q = (Byte *)mem_alloc_nozero(a, (size_t)tlen, 1);
            if (q == NULL) {
                e = DM_OOM;
                break;
            }
            memcpy(q, m + begin, (size_t)tlen);
            s = str_from_bytes(q, tlen);
        }
        Str *tp = (Str *)dm_grow(a, t.p, t.len, &t.cap, sizeof(Str), _Alignof(Str));
        if (tp == NULL) {
            if (s.p != NULL)
                mem_free(a, (void *)(uintptr_t)s.p, (size_t)s.len, 1);
            e = DM_OOM;
            break;
        }
        t.p = tp;
        tp[t.len++] = s;
    }
    r->txt = t;
    if (BURROW_FAILED(e))
        burrow__dnsmsg_txt_free(a, r);
    return e;
}

static Error dm_unpack_opt(Alloc *a, Slice msg, Int off, uint16_t length,
                           DnsmsgOPTResource *r) {
    DnsmsgOptions o = {NULL, 0, 0};
    Error e = BURROW_NO_ERROR;
    for (Int old = off; off < old + length;) {
        DnsmsgOption opt;
        e = dm_u16(msg, &off, &opt.code);
        if (BURROW_FAILED(e)) {
            e = dm_nested(BURROW_S("Code"), e);
            break;
        }
        uint16_t l;
        e = dm_u16(msg, &off, &l);
        if (BURROW_FAILED(e)) {
            e = dm_nested(BURROW_S("Data"), e);
            break;
        }
        if (msg.len - off < l) {
            e = dm_nested(BURROW_S("Data"), burrow__dnsmsg_err_calc_len);
            break;
        }
        e = dm_copy_bytes(a, (const Byte *)msg.p + off, l, &opt.data);
        if (BURROW_FAILED(e))
            break;
        off += l;
        DnsmsgOption *op = (DnsmsgOption *)dm_grow(
            a, o.p, o.len, &o.cap, sizeof(DnsmsgOption), _Alignof(DnsmsgOption));
        if (op == NULL) {
            dm_bytes_free(a, &opt.data);
            e = DM_OOM;
            break;
        }
        o.p = op;
        op[o.len++] = opt;
    }
    r->options = o;
    if (BURROW_FAILED(e))
        burrow__dnsmsg_opt_free(a, r);
    return e;
}

static Error dm_unpack_unknown(Alloc *a, DnsmsgType type, Slice msg, Int off,
                               uint16_t length, DnsmsgUnknownResource *r) {
    r->type = type;
    r->data = slice_from(NULL, 0, 0, TYPE_BYTE);
    if (off + length > msg.len)
        return burrow__dnsmsg_err_base_len;
    return dm_copy_bytes(a, (const Byte *)msg.p + off, length, &r->data);
}

static Error dm_unpack_svcb(Alloc *a, Slice msg, Int off, uint16_t length,
                            DnsmsgSVCBResource *r) {
    memset(r, 0, sizeof *r);
    Int params_off = off;
    Int body_end = off + length;
    Error e = dm_u16(msg, &params_off, &r->priority);
    if (BURROW_FAILED(e))
        return dm_nested(BURROW_S("Priority"), e);
    e = burrow__dnsmsg_name_unpack(&r->target, msg, params_off, &params_off);
    if (BURROW_FAILED(e))
        return dm_nested(BURROW_S("Target"), e);

    /* Go's previousKey stays zero here too. */
    Int n = 0;
    uint16_t previous_key = 0;
    off = params_off;
    while (off < body_end) {
        uint16_t key, size;
        e = dm_u16(msg, &off, &key);
        if (BURROW_FAILED(e))
            return dm_nested(BURROW_S("Params key"), e);
        if (n > 0 && key <= previous_key)
            return dm_nested(BURROW_S("Params"), burrow__dnsmsg_err_param_out_of_order);
        e = dm_u16(msg, &off, &size);
        if (BURROW_FAILED(e))
            return dm_nested(BURROW_S("Params value length"), e);
        if (off + size > body_end)
            return burrow__dnsmsg_err_resource_len;
        off += size;
        n++;
    }
    if (off != body_end)
        return burrow__dnsmsg_err_resource_len;

    if (n > 0) {
        r->params.p = (DnsmsgSVCParam *)mem_alloc(a, (size_t)n * sizeof(DnsmsgSVCParam),
                                                  _Alignof(DnsmsgSVCParam));
        if (r->params.p == NULL)
            return DM_OOM;
        r->params.cap = n;
    }
    off = params_off;
    for (Int i = 0; i < n; i++) {
        DnsmsgSVCParam *p = &r->params.p[i];
        uint16_t key, size;
        e = dm_u16(msg, &off, &key);
        if (BURROW_FAILED(e)) {
            e = dm_nested(BURROW_S("param key"), e);
            break;
        }
        p->key = key;
        e = dm_u16(msg, &off, &size);
        if (BURROW_FAILED(e)) {
            e = dm_nested(BURROW_S("param length"), e);
            break;
        }
        if (msg.len - off < size) {
            e = dm_nested(BURROW_S("param value"), burrow__dnsmsg_err_calc_len);
            break;
        }
        e = dm_copy_bytes(a, (const Byte *)msg.p + off, size, &p->value);
        if (BURROW_FAILED(e))
            break;
        r->params.len = i + 1;
        off += size;
    }
    if (BURROW_FAILED(e)) {
        burrow__dnsmsg_svcb_free(a, r);
        return e;
    }
    r->params.len = n;
    return BURROW_NO_ERROR;
}

Error burrow__dnsmsg_unpack_resource_body(Alloc *a, Slice msg, Int off,
                                          DnsmsgResourceHeader hdr,
                                          DnsmsgResourceBody *out, Int *new_off) {
    DnsmsgResourceBody b;
    memset(&b, 0, sizeof b);
    Error e;
    Str name;
    switch (hdr.type) {
    case DNSMSG_TYPE_A:
        b.kind = DNSMSG_BODY_A;
        e = dm_bytes(msg, off, b.u.a.a, 4);
        name = BURROW_S("A record");
        break;
    case DNSMSG_TYPE_NS:
        b.kind = DNSMSG_BODY_NS;
        e = dm_unpack_name_body(msg, off, &b.u.ns.ns);
        name = BURROW_S("NS record");
        break;
    case DNSMSG_TYPE_CNAME:
        b.kind = DNSMSG_BODY_CNAME;
        e = dm_unpack_name_body(msg, off, &b.u.cname.cname);
        name = BURROW_S("CNAME record");
        break;
    case DNSMSG_TYPE_SOA:
        b.kind = DNSMSG_BODY_SOA;
        e = dm_unpack_soa(msg, off, &b.u.soa);
        name = BURROW_S("SOA record");
        break;
    case DNSMSG_TYPE_PTR:
        b.kind = DNSMSG_BODY_PTR;
        e = dm_unpack_name_body(msg, off, &b.u.ptr.ptr);
        name = BURROW_S("PTR record");
        break;
    case DNSMSG_TYPE_MX:
        b.kind = DNSMSG_BODY_MX;
        e = dm_unpack_mx(msg, off, &b.u.mx);
        name = BURROW_S("MX record");
        break;
    case DNSMSG_TYPE_TXT:
        b.kind = DNSMSG_BODY_TXT;
        e = dm_unpack_txt(a, msg, off, hdr.length, &b.u.txt);
        name = BURROW_S("TXT record");
        break;
    case DNSMSG_TYPE_AAAA:
        b.kind = DNSMSG_BODY_AAAA;
        e = dm_bytes(msg, off, b.u.aaaa.aaaa, 16);
        name = BURROW_S("AAAA record");
        break;
    case DNSMSG_TYPE_SRV:
        b.kind = DNSMSG_BODY_SRV;
        e = dm_unpack_srv(msg, off, &b.u.srv);
        name = BURROW_S("SRV record");
        break;
    case DNSMSG_TYPE_SVCB:
        b.kind = DNSMSG_BODY_SVCB;
        e = dm_unpack_svcb(a, msg, off, hdr.length, &b.u.svcb);
        name = BURROW_S("SVCB record");
        break;
    case DNSMSG_TYPE_HTTPS:
        b.kind = DNSMSG_BODY_HTTPS;
        e = dm_unpack_svcb(a, msg, off, hdr.length, &b.u.https.svcb);
        name = BURROW_S("HTTPS record");
        break;
    case DNSMSG_TYPE_OPT:
        b.kind = DNSMSG_BODY_OPT;
        e = dm_unpack_opt(a, msg, off, hdr.length, &b.u.opt);
        name = BURROW_S("OPT record");
        break;
    default:
        b.kind = DNSMSG_BODY_UNKNOWN;
        e = dm_unpack_unknown(a, hdr.type, msg, off, hdr.length, &b.u.unknown);
        name = BURROW_S("Unknown record");
        break;
    }
    if (BURROW_FAILED(e)) {
        memset(out, 0, sizeof *out);
        *new_off = off;
        return dm_nested(name, e);
    }
    *out = b;
    *new_off = off + hdr.length;
    return BURROW_NO_ERROR;
}

/* ------------------------------------------------------------- freeing */

void burrow__dnsmsg_txt_free(Alloc *a, DnsmsgTXTResource *r) {
    for (Int i = 0; i < r->txt.len; i++)
        if (r->txt.p[i].p != NULL)
            mem_free(a, (void *)(uintptr_t)r->txt.p[i].p, (size_t)r->txt.p[i].len, 1);
    if (r->txt.p != NULL)
        mem_free(a, r->txt.p, (size_t)r->txt.cap * sizeof(Str), _Alignof(Str));
    r->txt = (DnsmsgStrs){NULL, 0, 0};
}

void burrow__dnsmsg_opt_free(Alloc *a, DnsmsgOPTResource *r) {
    for (Int i = 0; i < r->options.len; i++)
        dm_bytes_free(a, &r->options.p[i].data);
    if (r->options.p != NULL)
        mem_free(a, r->options.p, (size_t)r->options.cap * sizeof(DnsmsgOption),
                 _Alignof(DnsmsgOption));
    r->options = (DnsmsgOptions){NULL, 0, 0};
}

void burrow__dnsmsg_svcb_free(Alloc *a, DnsmsgSVCBResource *r) {
    for (Int i = 0; i < r->params.len; i++)
        dm_bytes_free(a, &r->params.p[i].value);
    if (r->params.p != NULL)
        mem_free(a, r->params.p, (size_t)r->params.cap * sizeof(DnsmsgSVCParam),
                 _Alignof(DnsmsgSVCParam));
    r->params = (DnsmsgSVCParams){NULL, 0, 0};
}

void burrow__dnsmsg_unknown_free(Alloc *a, DnsmsgUnknownResource *r) {
    dm_bytes_free(a, &r->data);
}

void burrow__dnsmsg_body_free(Alloc *a, DnsmsgResourceBody *b) {
    switch (b->kind) {
    case DNSMSG_BODY_TXT:
        burrow__dnsmsg_txt_free(a, &b->u.txt);
        break;
    case DNSMSG_BODY_OPT:
        burrow__dnsmsg_opt_free(a, &b->u.opt);
        break;
    case DNSMSG_BODY_SVCB:
        burrow__dnsmsg_svcb_free(a, &b->u.svcb);
        break;
    case DNSMSG_BODY_HTTPS:
        burrow__dnsmsg_svcb_free(a, &b->u.https.svcb);
        break;
    case DNSMSG_BODY_UNKNOWN:
        burrow__dnsmsg_unknown_free(a, &b->u.unknown);
        break;
    case DNSMSG_BODY_NONE:
    case DNSMSG_BODY_A:
    case DNSMSG_BODY_NS:
    case DNSMSG_BODY_CNAME:
    case DNSMSG_BODY_SOA:
    case DNSMSG_BODY_PTR:
    case DNSMSG_BODY_MX:
    case DNSMSG_BODY_AAAA:
    case DNSMSG_BODY_SRV:
    default:
        break;
    }
    memset(b, 0, sizeof *b);
}

void burrow__dnsmsg_questions_free(Alloc *a, DnsmsgQuestions *qs) {
    if (qs->p != NULL)
        mem_free(a, qs->p, (size_t)qs->cap * sizeof(DnsmsgQuestion),
                 _Alignof(DnsmsgQuestion));
    *qs = (DnsmsgQuestions){NULL, 0, 0};
}

void burrow__dnsmsg_resources_free(Alloc *a, DnsmsgResources *rs) {
    for (Int i = 0; i < rs->len; i++)
        burrow__dnsmsg_body_free(a, &rs->p[i].body);
    if (rs->p != NULL)
        mem_free(a, rs->p, (size_t)rs->cap * sizeof(DnsmsgResource),
                 _Alignof(DnsmsgResource));
    *rs = (DnsmsgResources){NULL, 0, 0};
}

void burrow__dnsmsg_message_free(Alloc *a, DnsmsgMessage *m) {
    burrow__dnsmsg_questions_free(a, &m->questions);
    burrow__dnsmsg_resources_free(a, &m->answers);
    burrow__dnsmsg_resources_free(a, &m->authorities);
    burrow__dnsmsg_resources_free(a, &m->additionals);
}

/* --------------------------------------------------------------- parser */

static uint16_t dm_count(const DnsmsgWireHeader *h, DnsmsgSection sec) {
    switch (sec) {
    case DNSMSG_SECTION_QUESTIONS:
        return h->questions;
    case DNSMSG_SECTION_ANSWERS:
        return h->answers;
    case DNSMSG_SECTION_AUTHORITIES:
        return h->authorities;
    case DNSMSG_SECTION_ADDITIONALS:
        return h->additionals;
    case DNSMSG_SECTION_NOT_STARTED:
    case DNSMSG_SECTION_HEADER:
    case DNSMSG_SECTION_DONE:
    default:
        return 0;
    }
}

Error burrow__dnsmsg_parser_start(DnsmsgParser *p, Slice msg, DnsmsgHeader *h) {
    memset(p, 0, sizeof *p);
    p->msg = msg;
    Error e = burrow__dnsmsg_wire_header_unpack(&p->header, msg, 0, &p->off);
    if (BURROW_FAILED(e)) {
        memset(h, 0, sizeof *h);
        return dm_nested(BURROW_S("unpacking header"), e);
    }
    p->section = DNSMSG_SECTION_QUESTIONS;
    *h = dm_header_of(&p->header);
    return BURROW_NO_ERROR;
}

static Error dm_check_advance(DnsmsgParser *p, DnsmsgSection sec) {
    if (p->section < sec)
        return burrow__dnsmsg_err_not_started;
    if (p->section > sec)
        return burrow__dnsmsg_err_section_done;
    p->res_header_valid = false;
    if (p->index == (Int)dm_count(&p->header, sec)) {
        p->index = 0;
        p->section++;
        return burrow__dnsmsg_err_section_done;
    }
    return BURROW_NO_ERROR;
}

static Error dm_resource_header(DnsmsgParser *p, DnsmsgSection sec,
                                DnsmsgResourceHeader *out) {
    memset(out, 0, sizeof *out);
    if (p->res_header_valid)
        p->off = p->res_header_offset;
    Error e = dm_check_advance(p, sec);
    if (BURROW_FAILED(e))
        return e;
    DnsmsgResourceHeader h;
    memset(&h, 0, sizeof h);
    Int off;
    e = burrow__dnsmsg_resource_header_unpack(&h, p->msg, p->off, &off);
    if (BURROW_FAILED(e))
        return e;
    p->res_header_valid = true;
    p->res_header_offset = p->off;
    p->res_header_type = h.type;
    p->res_header_length = h.length;
    p->off = off;
    *out = h;
    return BURROW_NO_ERROR;
}

static Str dm_unpacking_name(DnsmsgSection sec) {
    switch (sec) {
    case DNSMSG_SECTION_ANSWERS:
        return BURROW_S("unpacking Answer");
    case DNSMSG_SECTION_AUTHORITIES:
        return BURROW_S("unpacking Authority");
    case DNSMSG_SECTION_ADDITIONALS:
        return BURROW_S("unpacking Additional");
    case DNSMSG_SECTION_NOT_STARTED:
    case DNSMSG_SECTION_HEADER:
    case DNSMSG_SECTION_QUESTIONS:
    case DNSMSG_SECTION_DONE:
    default:
        return BURROW_S("unpacking ");
    }
}

static Str dm_skipping_name(DnsmsgSection sec) {
    switch (sec) {
    case DNSMSG_SECTION_ANSWERS:
        return BURROW_S("skipping: Answer");
    case DNSMSG_SECTION_AUTHORITIES:
        return BURROW_S("skipping: Authority");
    case DNSMSG_SECTION_ADDITIONALS:
        return BURROW_S("skipping: Additional");
    case DNSMSG_SECTION_NOT_STARTED:
    case DNSMSG_SECTION_HEADER:
    case DNSMSG_SECTION_QUESTIONS:
    case DNSMSG_SECTION_DONE:
    default:
        return BURROW_S("skipping: ");
    }
}

static Error dm_resource(DnsmsgParser *p, Alloc *a, DnsmsgSection sec,
                         DnsmsgResource *r) {
    memset(r, 0, sizeof *r);
    Error e = dm_resource_header(p, sec, &r->header);
    if (BURROW_FAILED(e))
        return e;
    p->res_header_valid = false;
    e = burrow__dnsmsg_unpack_resource_body(a, p->msg, p->off, r->header, &r->body,
                                            &p->off);
    if (BURROW_FAILED(e)) {
        memset(r, 0, sizeof *r);
        return dm_nested(dm_unpacking_name(sec), e);
    }
    p->index++;
    return BURROW_NO_ERROR;
}

static Error dm_skip_resource_in(DnsmsgParser *p, DnsmsgSection sec) {
    if (p->res_header_valid && p->section == sec) {
        Int next = p->off + p->res_header_length;
        if (next > p->msg.len)
            return burrow__dnsmsg_err_resource_len;
        p->off = next;
        p->res_header_valid = false;
        p->index++;
        return BURROW_NO_ERROR;
    }
    Error e = dm_check_advance(p, sec);
    if (BURROW_FAILED(e))
        return e;
    e = dm_skip_resource(p->msg, &p->off);
    if (BURROW_FAILED(e))
        return dm_nested(dm_skipping_name(sec), e);
    p->index++;
    return BURROW_NO_ERROR;
}

Error burrow__dnsmsg_parser_question(DnsmsgParser *p, DnsmsgQuestion *q) {
    memset(q, 0, sizeof *q);
    Error e = dm_check_advance(p, DNSMSG_SECTION_QUESTIONS);
    if (BURROW_FAILED(e))
        return e;
    DnsmsgQuestion x;
    memset(&x, 0, sizeof x);
    Int off;
    e = burrow__dnsmsg_name_unpack(&x.name, p->msg, p->off, &off);
    if (BURROW_FAILED(e))
        return dm_nested(BURROW_S("unpacking Question.Name"), e);
    e = dm_u16(p->msg, &off, &x.type);
    if (BURROW_FAILED(e))
        return dm_nested(BURROW_S("unpacking Question.Type"), e);
    e = dm_u16(p->msg, &off, &x.class_);
    if (BURROW_FAILED(e))
        return dm_nested(BURROW_S("unpacking Question.Class"), e);
    p->off = off;
    p->index++;
    *q = x;
    return BURROW_NO_ERROR;
}

Error burrow__dnsmsg_parser_all_questions(DnsmsgParser *p, Alloc *a,
                                          DnsmsgQuestions *qs) {
    DnsmsgQuestions out = {NULL, 0, 0};
    *qs = out;
    for (;;) {
        DnsmsgQuestion q;
        Error e = burrow__dnsmsg_parser_question(p, &q);
        if (dm_is(e, burrow__dnsmsg_err_section_done)) {
            *qs = out;
            return BURROW_NO_ERROR;
        }
        if (BURROW_FAILED(e)) {
            burrow__dnsmsg_questions_free(a, &out);
            return e;
        }
        DnsmsgQuestion *qp =
            (DnsmsgQuestion *)dm_grow(a, out.p, out.len, &out.cap,
                                      sizeof(DnsmsgQuestion), _Alignof(DnsmsgQuestion));
        if (qp == NULL) {
            burrow__dnsmsg_questions_free(a, &out);
            return DM_OOM;
        }
        out.p = qp;
        qp[out.len++] = q;
    }
}

Error burrow__dnsmsg_parser_skip_question(DnsmsgParser *p) {
    Error e = dm_check_advance(p, DNSMSG_SECTION_QUESTIONS);
    if (BURROW_FAILED(e))
        return e;
    Int off = p->off;
    e = dm_skip_name(p->msg, &off);
    if (BURROW_FAILED(e))
        return dm_nested(BURROW_S("skipping Question Name"), e);
    e = dm_skip(p->msg, &off, 2);
    if (BURROW_FAILED(e))
        return dm_nested(BURROW_S("skipping Question Type"), e);
    e = dm_skip(p->msg, &off, 2);
    if (BURROW_FAILED(e))
        return dm_nested(BURROW_S("skipping Question Class"), e);
    p->off = off;
    p->index++;
    return BURROW_NO_ERROR;
}

Error burrow__dnsmsg_parser_skip_all_questions(DnsmsgParser *p) {
    for (;;) {
        Error e = burrow__dnsmsg_parser_skip_question(p);
        if (dm_is(e, burrow__dnsmsg_err_section_done))
            return BURROW_NO_ERROR;
        if (BURROW_FAILED(e))
            return e;
    }
}

static Error dm_all_resources(DnsmsgParser *p, Alloc *a, DnsmsgSection sec, Int max,
                              DnsmsgResources *rs) {
    DnsmsgResources out = {NULL, 0, 0};
    *rs = out;
    Int n = dm_count(&p->header, sec);
    if (n > max)
        n = max;
    if (n > 0) {
        out.p = (DnsmsgResource *)mem_alloc_nozero(
            a, (size_t)n * sizeof(DnsmsgResource), _Alignof(DnsmsgResource));
        if (out.p == NULL)
            return DM_OOM;
        out.cap = n;
    }
    for (;;) {
        DnsmsgResource r;
        Error e = dm_resource(p, a, sec, &r);
        if (dm_is(e, burrow__dnsmsg_err_section_done)) {
            *rs = out;
            return BURROW_NO_ERROR;
        }
        if (BURROW_FAILED(e)) {
            burrow__dnsmsg_resources_free(a, &out);
            return e;
        }
        DnsmsgResource *rp =
            (DnsmsgResource *)dm_grow(a, out.p, out.len, &out.cap,
                                      sizeof(DnsmsgResource), _Alignof(DnsmsgResource));
        if (rp == NULL) {
            burrow__dnsmsg_body_free(a, &r.body);
            burrow__dnsmsg_resources_free(a, &out);
            return DM_OOM;
        }
        out.p = rp;
        rp[out.len++] = r;
    }
}

static Error dm_skip_all_resources(DnsmsgParser *p, DnsmsgSection sec) {
    for (;;) {
        Error e = dm_skip_resource_in(p, sec);
        if (dm_is(e, burrow__dnsmsg_err_section_done))
            return BURROW_NO_ERROR;
        if (BURROW_FAILED(e))
            return e;
    }
}

Error burrow__dnsmsg_parser_answer_header(DnsmsgParser *p, DnsmsgResourceHeader *h) {
    return dm_resource_header(p, DNSMSG_SECTION_ANSWERS, h);
}

Error burrow__dnsmsg_parser_answer(DnsmsgParser *p, Alloc *a, DnsmsgResource *r) {
    return dm_resource(p, a, DNSMSG_SECTION_ANSWERS, r);
}

Error burrow__dnsmsg_parser_all_answers(DnsmsgParser *p, Alloc *a,
                                        DnsmsgResources *rs) {
    return dm_all_resources(p, a, DNSMSG_SECTION_ANSWERS, 20, rs);
}

Error burrow__dnsmsg_parser_skip_answer(DnsmsgParser *p) {
    return dm_skip_resource_in(p, DNSMSG_SECTION_ANSWERS);
}

Error burrow__dnsmsg_parser_skip_all_answers(DnsmsgParser *p) {
    return dm_skip_all_resources(p, DNSMSG_SECTION_ANSWERS);
}

Error burrow__dnsmsg_parser_authority_header(DnsmsgParser *p, DnsmsgResourceHeader *h) {
    return dm_resource_header(p, DNSMSG_SECTION_AUTHORITIES, h);
}

Error burrow__dnsmsg_parser_authority(DnsmsgParser *p, Alloc *a, DnsmsgResource *r) {
    return dm_resource(p, a, DNSMSG_SECTION_AUTHORITIES, r);
}

Error burrow__dnsmsg_parser_all_authorities(DnsmsgParser *p, Alloc *a,
                                            DnsmsgResources *rs) {
    return dm_all_resources(p, a, DNSMSG_SECTION_AUTHORITIES, 10, rs);
}

Error burrow__dnsmsg_parser_skip_authority(DnsmsgParser *p) {
    return dm_skip_resource_in(p, DNSMSG_SECTION_AUTHORITIES);
}

Error burrow__dnsmsg_parser_skip_all_authorities(DnsmsgParser *p) {
    return dm_skip_all_resources(p, DNSMSG_SECTION_AUTHORITIES);
}

Error burrow__dnsmsg_parser_additional_header(DnsmsgParser *p,
                                              DnsmsgResourceHeader *h) {
    return dm_resource_header(p, DNSMSG_SECTION_ADDITIONALS, h);
}

Error burrow__dnsmsg_parser_additional(DnsmsgParser *p, Alloc *a, DnsmsgResource *r) {
    return dm_resource(p, a, DNSMSG_SECTION_ADDITIONALS, r);
}

Error burrow__dnsmsg_parser_all_additionals(DnsmsgParser *p, Alloc *a,
                                            DnsmsgResources *rs) {
    return dm_all_resources(p, a, DNSMSG_SECTION_ADDITIONALS, 10, rs);
}

Error burrow__dnsmsg_parser_skip_additional(DnsmsgParser *p) {
    return dm_skip_resource_in(p, DNSMSG_SECTION_ADDITIONALS);
}

Error burrow__dnsmsg_parser_skip_all_additionals(DnsmsgParser *p) {
    return dm_skip_all_resources(p, DNSMSG_SECTION_ADDITIONALS);
}

/* The start of each typed body read: is the header that was read of this
 * type. Unknown passes 0 for any type. */
static bool dm_typed(const DnsmsgParser *p, DnsmsgType t, bool any) {
    return p->res_header_valid && (any || p->res_header_type == t);
}

static void dm_typed_done(DnsmsgParser *p) {
    p->off += p->res_header_length;
    p->res_header_valid = false;
    p->index++;
}

Error burrow__dnsmsg_parser_cname_resource(DnsmsgParser *p, DnsmsgCNAMEResource *r) {
    memset(r, 0, sizeof *r);
    if (!dm_typed(p, DNSMSG_TYPE_CNAME, false))
        return burrow__dnsmsg_err_not_started;
    DnsmsgCNAMEResource x;
    Error e = dm_unpack_name_body(p->msg, p->off, &x.cname);
    if (BURROW_FAILED(e))
        return e;
    dm_typed_done(p);
    *r = x;
    return BURROW_NO_ERROR;
}

Error burrow__dnsmsg_parser_mx_resource(DnsmsgParser *p, DnsmsgMXResource *r) {
    memset(r, 0, sizeof *r);
    if (!dm_typed(p, DNSMSG_TYPE_MX, false))
        return burrow__dnsmsg_err_not_started;
    DnsmsgMXResource x;
    memset(&x, 0, sizeof x);
    Error e = dm_unpack_mx(p->msg, p->off, &x);
    if (BURROW_FAILED(e))
        return e;
    dm_typed_done(p);
    *r = x;
    return BURROW_NO_ERROR;
}

Error burrow__dnsmsg_parser_ns_resource(DnsmsgParser *p, DnsmsgNSResource *r) {
    memset(r, 0, sizeof *r);
    if (!dm_typed(p, DNSMSG_TYPE_NS, false))
        return burrow__dnsmsg_err_not_started;
    DnsmsgNSResource x;
    Error e = dm_unpack_name_body(p->msg, p->off, &x.ns);
    if (BURROW_FAILED(e))
        return e;
    dm_typed_done(p);
    *r = x;
    return BURROW_NO_ERROR;
}

Error burrow__dnsmsg_parser_ptr_resource(DnsmsgParser *p, DnsmsgPTRResource *r) {
    memset(r, 0, sizeof *r);
    if (!dm_typed(p, DNSMSG_TYPE_PTR, false))
        return burrow__dnsmsg_err_not_started;
    DnsmsgPTRResource x;
    Error e = dm_unpack_name_body(p->msg, p->off, &x.ptr);
    if (BURROW_FAILED(e))
        return e;
    dm_typed_done(p);
    *r = x;
    return BURROW_NO_ERROR;
}

Error burrow__dnsmsg_parser_soa_resource(DnsmsgParser *p, DnsmsgSOAResource *r) {
    memset(r, 0, sizeof *r);
    if (!dm_typed(p, DNSMSG_TYPE_SOA, false))
        return burrow__dnsmsg_err_not_started;
    DnsmsgSOAResource x;
    memset(&x, 0, sizeof x);
    Error e = dm_unpack_soa(p->msg, p->off, &x);
    if (BURROW_FAILED(e))
        return e;
    dm_typed_done(p);
    *r = x;
    return BURROW_NO_ERROR;
}

Error burrow__dnsmsg_parser_txt_resource(DnsmsgParser *p, Alloc *a,
                                         DnsmsgTXTResource *r) {
    memset(r, 0, sizeof *r);
    if (!dm_typed(p, DNSMSG_TYPE_TXT, false))
        return burrow__dnsmsg_err_not_started;
    Error e = dm_unpack_txt(a, p->msg, p->off, p->res_header_length, r);
    if (BURROW_FAILED(e))
        return e;
    dm_typed_done(p);
    return BURROW_NO_ERROR;
}

Error burrow__dnsmsg_parser_srv_resource(DnsmsgParser *p, DnsmsgSRVResource *r) {
    memset(r, 0, sizeof *r);
    if (!dm_typed(p, DNSMSG_TYPE_SRV, false))
        return burrow__dnsmsg_err_not_started;
    DnsmsgSRVResource x;
    memset(&x, 0, sizeof x);
    Error e = dm_unpack_srv(p->msg, p->off, &x);
    if (BURROW_FAILED(e))
        return e;
    dm_typed_done(p);
    *r = x;
    return BURROW_NO_ERROR;
}

Error burrow__dnsmsg_parser_a_resource(DnsmsgParser *p, DnsmsgAResource *r) {
    memset(r, 0, sizeof *r);
    if (!dm_typed(p, DNSMSG_TYPE_A, false))
        return burrow__dnsmsg_err_not_started;
    DnsmsgAResource x;
    Error e = dm_bytes(p->msg, p->off, x.a, 4);
    if (BURROW_FAILED(e))
        return e;
    dm_typed_done(p);
    *r = x;
    return BURROW_NO_ERROR;
}

Error burrow__dnsmsg_parser_aaaa_resource(DnsmsgParser *p, DnsmsgAAAAResource *r) {
    memset(r, 0, sizeof *r);
    if (!dm_typed(p, DNSMSG_TYPE_AAAA, false))
        return burrow__dnsmsg_err_not_started;
    DnsmsgAAAAResource x;
    Error e = dm_bytes(p->msg, p->off, x.aaaa, 16);
    if (BURROW_FAILED(e))
        return e;
    dm_typed_done(p);
    *r = x;
    return BURROW_NO_ERROR;
}

Error burrow__dnsmsg_parser_opt_resource(DnsmsgParser *p, Alloc *a,
                                         DnsmsgOPTResource *r) {
    memset(r, 0, sizeof *r);
    if (!dm_typed(p, DNSMSG_TYPE_OPT, false))
        return burrow__dnsmsg_err_not_started;
    Error e = dm_unpack_opt(a, p->msg, p->off, p->res_header_length, r);
    if (BURROW_FAILED(e))
        return e;
    dm_typed_done(p);
    return BURROW_NO_ERROR;
}

Error burrow__dnsmsg_parser_svcb_resource(DnsmsgParser *p, Alloc *a,
                                          DnsmsgSVCBResource *r) {
    memset(r, 0, sizeof *r);
    if (!dm_typed(p, DNSMSG_TYPE_SVCB, false))
        return burrow__dnsmsg_err_not_started;
    Error e = dm_unpack_svcb(a, p->msg, p->off, p->res_header_length, r);
    if (BURROW_FAILED(e))
        return e;
    dm_typed_done(p);
    return BURROW_NO_ERROR;
}

Error burrow__dnsmsg_parser_https_resource(DnsmsgParser *p, Alloc *a,
                                           DnsmsgHTTPSResource *r) {
    memset(r, 0, sizeof *r);
    if (!dm_typed(p, DNSMSG_TYPE_HTTPS, false))
        return burrow__dnsmsg_err_not_started;
    Error e = dm_unpack_svcb(a, p->msg, p->off, p->res_header_length, &r->svcb);
    if (BURROW_FAILED(e))
        return e;
    dm_typed_done(p);
    return BURROW_NO_ERROR;
}

Error burrow__dnsmsg_parser_unknown_resource(DnsmsgParser *p, Alloc *a,
                                             DnsmsgUnknownResource *r) {
    memset(r, 0, sizeof *r);
    if (!dm_typed(p, 0, true))
        return burrow__dnsmsg_err_not_started;
    Error e = dm_unpack_unknown(a, p->res_header_type, p->msg, p->off,
                                p->res_header_length, r);
    if (BURROW_FAILED(e)) {
        memset(r, 0, sizeof *r);
        return e;
    }
    dm_typed_done(p);
    return BURROW_NO_ERROR;
}

/* -------------------------------------------------------------- message */

Error burrow__dnsmsg_message_unpack(DnsmsgMessage *m, Alloc *a, Slice msg) {
    DnsmsgParser p;
    Error e = burrow__dnsmsg_parser_start(&p, msg, &m->header);
    if (BURROW_FAILED(e))
        return e;
    e = burrow__dnsmsg_parser_all_questions(&p, a, &m->questions);
    if (BURROW_FAILED(e))
        return e;
    e = burrow__dnsmsg_parser_all_answers(&p, a, &m->answers);
    if (BURROW_FAILED(e))
        return e;
    e = burrow__dnsmsg_parser_all_authorities(&p, a, &m->authorities);
    if (BURROW_FAILED(e))
        return e;
    return burrow__dnsmsg_parser_all_additionals(&p, a, &m->additionals);
}

static Error dm_pack_resources(DnsmsgResources *rs, DnsmsgBuf *msg,
                               DnsmsgCompression *c, Int compression_off, Str prefix) {
    for (Int i = 0; i < rs->len; i++) {
        Error e = burrow__dnsmsg_resource_pack(&rs->p[i], msg, c, compression_off);
        if (BURROW_FAILED(e))
            return dm_nested(prefix, e);
    }
    return BURROW_NO_ERROR;
}

/* AppendPack onto buf, which is given back on an error. */
static Error dm_append_pack(DnsmsgMessage *m, DnsmsgBuf *buf, Slice *out) {
    *out = slice_nil(TYPE_BYTE);
    Error e = BURROW_NO_ERROR;
    if (m->questions.len > 0xFFFF)
        e = burrow__dnsmsg_err_too_many_questions;
    else if (m->answers.len > 0xFFFF)
        e = burrow__dnsmsg_err_too_many_answers;
    else if (m->authorities.len > 0xFFFF)
        e = burrow__dnsmsg_err_too_many_authorities;
    else if (m->additionals.len > 0xFFFF)
        e = burrow__dnsmsg_err_too_many_additionals;
    if (BURROW_FAILED(e)) {
        burrow__dnsmsg_buf_free(buf);
        return e;
    }

    DnsmsgWireHeader h;
    dm_header_pack(&m->header, &h.id, &h.bits);
    h.questions = (uint16_t)m->questions.len;
    h.answers = (uint16_t)m->answers.len;
    h.authorities = (uint16_t)m->authorities.len;
    h.additionals = (uint16_t)m->additionals.len;
    Int compression_off = buf->len;
    Byte hb[DM_HEADER_LEN];
    dm_wire_header_bytes(&h, hb);
    DnsmsgCompression *c = NULL;
    if (dm_put(buf, hb, DM_HEADER_LEN))
        c = burrow__dnsmsg_compression_new(buf->a);
    if (c == NULL) {
        burrow__dnsmsg_buf_free(buf);
        return DM_OOM;
    }
    for (Int i = 0; i < m->questions.len && BURROW_OK(e); i++) {
        e = burrow__dnsmsg_question_pack(&m->questions.p[i], buf, c, compression_off);
        if (BURROW_FAILED(e))
            e = dm_nested(BURROW_S("packing Question"), e);
    }
    if (BURROW_OK(e))
        e = dm_pack_resources(&m->answers, buf, c, compression_off,
                              BURROW_S("packing Answer"));
    if (BURROW_OK(e))
        e = dm_pack_resources(&m->authorities, buf, c, compression_off,
                              BURROW_S("packing Authority"));
    if (BURROW_OK(e))
        e = dm_pack_resources(&m->additionals, buf, c, compression_off,
                              BURROW_S("packing Additional"));
    burrow__dnsmsg_compression_free(c);
    if (BURROW_FAILED(e)) {
        burrow__dnsmsg_buf_free(buf);
        return e;
    }
    *out = burrow__dnsmsg_buf_slice(buf);
    return BURROW_NO_ERROR;
}

Error burrow__dnsmsg_message_pack(DnsmsgMessage *m, Alloc *a, Slice *out) {
    Byte *p = (Byte *)mem_alloc_nozero(a, DM_STARTING_CAP, 1);
    if (p == NULL) {
        *out = slice_nil(TYPE_BYTE);
        return DM_OOM;
    }
    DnsmsgBuf buf = {a, p, 0, DM_STARTING_CAP, true};
    return dm_append_pack(m, &buf, out);
}

Error burrow__dnsmsg_message_append_pack(DnsmsgMessage *m, Alloc *a, Slice b,
                                         Slice *out) {
    DnsmsgBuf buf = burrow__dnsmsg_buf(a, b);
    return dm_append_pack(m, &buf, out);
}

/* ----------------------------------------------------------------- SVCB */

bool burrow__dnsmsg_svcb_get_param(const DnsmsgSVCBResource *r, DnsmsgSVCParamKey key,
                                   Slice *value) {
    for (Int i = 0; i < r->params.len; i++) {
        if (r->params.p[i].key == key) {
            *value = r->params.p[i].value;
            return true;
        }
        if (r->params.p[i].key > key)
            break;
    }
    *value = slice_nil(TYPE_BYTE);
    return false;
}

bool burrow__dnsmsg_svcb_set_param(DnsmsgSVCBResource *r, Alloc *a,
                                   DnsmsgSVCParamKey key, Slice value) {
    DnsmsgSVCParams *ps = &r->params;
    Int i = 0;
    while (i < ps->len && ps->p[i].key < key)
        i++;
    if (i < ps->len && ps->p[i].key == key) {
        ps->p[i].value = value;
        return true;
    }
    if (ps->len == ps->cap) {
        Int ncap = ps->cap == 0 ? 1 : ps->cap * 2;
        DnsmsgSVCParam *q = (DnsmsgSVCParam *)mem_alloc(
            a, (size_t)ncap * sizeof(DnsmsgSVCParam), _Alignof(DnsmsgSVCParam));
        if (q == NULL)
            return false;
        if (ps->len > 0)
            memcpy(q, ps->p, (size_t)ps->len * sizeof(DnsmsgSVCParam));
        ps->p = q;
        ps->cap = ncap;
    }
    memmove(ps->p + i + 1, ps->p + i, (size_t)(ps->len - i) * sizeof(DnsmsgSVCParam));
    ps->p[i] = (DnsmsgSVCParam){key, value};
    ps->len++;
    return true;
}

bool burrow__dnsmsg_svcb_delete_param(DnsmsgSVCBResource *r, DnsmsgSVCParamKey key) {
    DnsmsgSVCParams *ps = &r->params;
    for (Int i = 0; i < ps->len; i++) {
        if (ps->p[i].key == key) {
            memmove(ps->p + i, ps->p + i + 1,
                    (size_t)(ps->len - i - 1) * sizeof(DnsmsgSVCParam));
            ps->len--;
            memset(&ps->p[ps->len], 0, sizeof(DnsmsgSVCParam));
            return true;
        }
        if (ps->p[i].key > key)
            break;
    }
    return false;
}

/* -------------------------------------------------------------- builder */

Error burrow__dnsmsg_new_builder(DnsmsgBuilder *b, Alloc *a, Slice buf,
                                 DnsmsgHeader h) {
    memset(b, 0, sizeof *b);
    DnsmsgBuf msg = burrow__dnsmsg_buf(a, buf);
    if (buf.p == NULL) {
        Byte *p = (Byte *)mem_alloc_nozero(a, DM_STARTING_CAP, 1);
        if (p == NULL)
            return DM_OOM;
        msg = (DnsmsgBuf){a, p, 0, DM_STARTING_CAP, true};
    }
    Int start = msg.len;
    static const Byte zero[DM_HEADER_LEN] = {0};
    if (!dm_put(&msg, zero, DM_HEADER_LEN)) {
        burrow__dnsmsg_buf_free(&msg);
        return DM_OOM;
    }
    b->msg = msg;
    b->start = start;
    dm_header_pack(&h, &b->header.id, &b->header.bits);
    b->section = DNSMSG_SECTION_HEADER;
    return BURROW_NO_ERROR;
}

Error burrow__dnsmsg_builder_enable_compression(DnsmsgBuilder *b) {
    DnsmsgCompression *c = burrow__dnsmsg_compression_new(b->msg.a);
    if (c == NULL)
        return DM_OOM;
    burrow__dnsmsg_compression_free(b->compression);
    b->compression = c;
    return BURROW_NO_ERROR;
}

static Error dm_start_check(DnsmsgBuilder *b, DnsmsgSection s) {
    if (b->section <= DNSMSG_SECTION_NOT_STARTED)
        return burrow__dnsmsg_err_not_started;
    if (b->section > s)
        return burrow__dnsmsg_err_section_done;
    b->section = s;
    return BURROW_NO_ERROR;
}

Error burrow__dnsmsg_builder_start_questions(DnsmsgBuilder *b) {
    return dm_start_check(b, DNSMSG_SECTION_QUESTIONS);
}

Error burrow__dnsmsg_builder_start_answers(DnsmsgBuilder *b) {
    return dm_start_check(b, DNSMSG_SECTION_ANSWERS);
}

Error burrow__dnsmsg_builder_start_authorities(DnsmsgBuilder *b) {
    return dm_start_check(b, DNSMSG_SECTION_AUTHORITIES);
}

Error burrow__dnsmsg_builder_start_additionals(DnsmsgBuilder *b) {
    return dm_start_check(b, DNSMSG_SECTION_ADDITIONALS);
}

static Error dm_increment_section_count(DnsmsgBuilder *b) {
    uint16_t *count;
    Error e;
    switch (b->section) {
    case DNSMSG_SECTION_QUESTIONS:
        count = &b->header.questions;
        e = burrow__dnsmsg_err_too_many_questions;
        break;
    case DNSMSG_SECTION_ANSWERS:
        count = &b->header.answers;
        e = burrow__dnsmsg_err_too_many_answers;
        break;
    case DNSMSG_SECTION_AUTHORITIES:
        count = &b->header.authorities;
        e = burrow__dnsmsg_err_too_many_authorities;
        break;
    case DNSMSG_SECTION_ADDITIONALS:
        count = &b->header.additionals;
        e = burrow__dnsmsg_err_too_many_additionals;
        break;
    case DNSMSG_SECTION_NOT_STARTED:
    case DNSMSG_SECTION_HEADER:
    case DNSMSG_SECTION_DONE:
    default:
        dm_nil_body();
    }
    if (*count == 0xFFFF)
        return e;
    (*count)++;
    return BURROW_NO_ERROR;
}

Error burrow__dnsmsg_builder_question(DnsmsgBuilder *b, const DnsmsgQuestion *q) {
    if (b->section < DNSMSG_SECTION_QUESTIONS)
        return burrow__dnsmsg_err_not_started;
    if (b->section > DNSMSG_SECTION_QUESTIONS)
        return burrow__dnsmsg_err_section_done;
    Int old = b->msg.len;
    Error e = burrow__dnsmsg_question_pack(q, &b->msg, b->compression, b->start);
    if (BURROW_FAILED(e))
        return e;
    e = dm_increment_section_count(b);
    if (BURROW_FAILED(e))
        return dm_undo(&b->msg, old, e);
    return BURROW_NO_ERROR;
}

/* What every one of Go's Builder record methods does. */
static Error dm_builder_resource(DnsmsgBuilder *b, DnsmsgResourceHeader h,
                                 const DnsmsgResourceBody *body, Str body_name) {
    if (b->section < DNSMSG_SECTION_ANSWERS)
        return burrow__dnsmsg_err_not_started;
    if (b->section > DNSMSG_SECTION_ADDITIONALS)
        return burrow__dnsmsg_err_section_done;
    h.type = dm_real_type(body);
    Int old = b->msg.len;
    Int len_off;
    Error e = burrow__dnsmsg_resource_header_pack(&h, &b->msg, b->compression, b->start,
                                                  &len_off);
    if (BURROW_FAILED(e))
        return dm_nested(BURROW_S("ResourceHeader"), e);
    Int pre_len = b->msg.len;
    e = burrow__dnsmsg_body_pack(body, &b->msg, b->compression, b->start);
    if (BURROW_FAILED(e))
        return dm_undo(&b->msg, old, dm_nested(body_name, e));
    e = dm_fix_len(&h, &b->msg, len_off, pre_len);
    if (BURROW_FAILED(e))
        return dm_undo(&b->msg, old, e);
    e = dm_increment_section_count(b);
    if (BURROW_FAILED(e))
        return dm_undo(&b->msg, old, e);
    return BURROW_NO_ERROR;
}

#define DM_BUILDER(fn, KIND, field, T, body_name)                                      \
    Error burrow__dnsmsg_builder_##fn##_resource(DnsmsgBuilder *b,                     \
                                                 DnsmsgResourceHeader h, const T *r) { \
        DnsmsgResourceBody body;                                                       \
        memset(&body, 0, sizeof body);                                                 \
        body.kind = KIND;                                                              \
        body.u.field = *r;                                                             \
        return dm_builder_resource(b, h, &body, BURROW_S(body_name));                  \
    }

DM_BUILDER(cname, DNSMSG_BODY_CNAME, cname, DnsmsgCNAMEResource, "CNAMEResource body")
DM_BUILDER(mx, DNSMSG_BODY_MX, mx, DnsmsgMXResource, "MXResource body")
DM_BUILDER(ns, DNSMSG_BODY_NS, ns, DnsmsgNSResource, "NSResource body")
DM_BUILDER(ptr, DNSMSG_BODY_PTR, ptr, DnsmsgPTRResource, "PTRResource body")
DM_BUILDER(soa, DNSMSG_BODY_SOA, soa, DnsmsgSOAResource, "SOAResource body")
DM_BUILDER(txt, DNSMSG_BODY_TXT, txt, DnsmsgTXTResource, "TXTResource body")
DM_BUILDER(srv, DNSMSG_BODY_SRV, srv, DnsmsgSRVResource, "SRVResource body")
DM_BUILDER(a, DNSMSG_BODY_A, a, DnsmsgAResource, "AResource body")
DM_BUILDER(aaaa, DNSMSG_BODY_AAAA, aaaa, DnsmsgAAAAResource, "AAAAResource body")
DM_BUILDER(opt, DNSMSG_BODY_OPT, opt, DnsmsgOPTResource, "OPTResource body")
DM_BUILDER(svcb, DNSMSG_BODY_SVCB, svcb, DnsmsgSVCBResource, "ResourceBody")
DM_BUILDER(https, DNSMSG_BODY_HTTPS, https, DnsmsgHTTPSResource, "ResourceBody")
DM_BUILDER(unknown, DNSMSG_BODY_UNKNOWN, unknown, DnsmsgUnknownResource,
           "UnknownResource body")

Error burrow__dnsmsg_builder_finish(DnsmsgBuilder *b, Slice *out) {
    if (b->section < DNSMSG_SECTION_HEADER) {
        *out = slice_nil(TYPE_BYTE);
        return burrow__dnsmsg_err_not_started;
    }
    b->section = DNSMSG_SECTION_DONE;
    dm_wire_header_bytes(&b->header, b->msg.p + b->start);
    b->msg.owned = false;
    *out = burrow__dnsmsg_buf_slice(&b->msg);
    return BURROW_NO_ERROR;
}

void burrow__dnsmsg_builder_free(DnsmsgBuilder *b) {
    burrow__dnsmsg_compression_free(b->compression);
    b->compression = NULL;
    burrow__dnsmsg_buf_free(&b->msg);
}

/* The DNS client that asks the name servers itself, the copy of resolv.conf
 * it works from, and the lookups of addresses, canonical names and PTR
 * records built on it.
 *
 * Derived from Go's src/net/dnsclient_unix.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "internal.h"

#include "../xnet/dnsmessage.h"

#include "burrow/chan.h"
#include "burrow/context.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/io/fs.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/net.h"
#include "burrow/os.h"
#include "burrow/platform.h"
#include "burrow/proc.h"
#include "burrow/runtime.h"
#include "burrow/slice.h"
#include "burrow/sync.h"
#include "burrow/sync/atomic.h"
#include "burrow/time.h"

#include <stdint.h>
#include <string.h>

#define DU_LIT(s) str_from_bytes((const Byte *)(s), (Int)sizeof(s) - 1)

/* maxDNSPacketSize, the largest UDP answer asked for, which is the size DNS
 * Flag Day 2020 settled on. */
enum { DU_MAX_DNS_PACKET_SIZE = 1232 };

static bool du_same(Error a, Error b) {
    return a.vt == b.vt && a.data == b.data;
}

/* ------------------------------------------------------- resolv.conf, held */

/* A config with what it owns. The config is first, so that the pointer the
 * getter hands out is the holder's own. */
typedef struct DuConf {
    burrow__DNSConfig c;
    Arena ar;
    int32_t refs;
    bool pinned; /* the fallback, which is never freed */
} DuConf;

/* resolverConfig. sema is Go's one slot channel, which only tryUpdate tries
 * for, and conf has a reference of its own. */
static struct {
    SyncOnce init_once;
    int32_t sema;
    Time last_checked;
    SyncMutex mu;
    DuConf *conf;
} du_resolv;

/* What the config is when there is no memory for one. */
static DuConf du_fallback;

static const char du_path[] = "/etc/resolv.conf";

static DuConf *du_read(Str filename) {
    DuConf *h = (DuConf *)mem_alloc(heap_allocator(), sizeof(DuConf), _Alignof(DuConf));
    if (h == NULL)
        return NULL;
    arena_init(&h->ar, NULL, 0);
    h->refs = 1;
    burrow__dns_read_config(arena_allocator(&h->ar), filename,
                            burrow__net_get_hostname(), &h->c);
    /* Go's config always has a server, and only running out of memory
     * could leave this one without. */
    if (h->c.servers.len == 0)
        h->c.servers = du_fallback.c.servers;
    return h;
}

static void du_init(void *env) {
    (void)env;
    memset(&du_fallback, 0, sizeof du_fallback);
    du_fallback.c.servers =
        slice_from((void *)(uintptr_t)burrow__net_default_ns, 2, 2, TYPE_STRING);
    du_fallback.c.ndots = 1;
    du_fallback.c.timeout = 5 * TIME_SECOND;
    du_fallback.c.attempts = 2;
    du_fallback.pinned = true;
    DuConf *h = du_read(DU_LIT(du_path));
    du_resolv.conf = h != NULL ? h : &du_fallback;
    du_resolv.last_checked = time_now();
}

static void du_once(void) {
    sync_once_do(&du_resolv.init_once, BURROW_FN(Func, du_init, NULL));
}

void burrow__dns_config_put(burrow__DNSConfig *c) {
    DuConf *h = (DuConf *)(void *)c;
    if (h == NULL || h->pinned || sync_atomic_add_int32(&h->refs, -1) != 0)
        return;
    arena_free(&h->ar);
    mem_free(heap_allocator(), h, sizeof(DuConf), _Alignof(DuConf));
}

static void du_swap(DuConf *h) {
    sync_mutex_lock(&du_resolv.mu);
    DuConf *old = du_resolv.conf;
    du_resolv.conf = h;
    sync_mutex_unlock(&du_resolv.mu);
    burrow__dns_config_put(&old->c);
}

Time burrow__net_distant_future(void) {
    return time_date(3000, TIME_JANUARY, 2, 3, 4, 5, 6, time_utc_loc);
}

/* tryUpdate: read the file again when five seconds have gone by, or every
 * time while the config is the default one, and its modification time has
 * moved. Windows has no modification time to go by and reads it each time. */
static void du_try_update(Str name) {
    du_once();
    /* Only tryUpdate and the test hooks replace conf, and the hooks hold
     * sema to set last_checked, so what is read here is current. */
    if (du_resolv.conf->c.no_reload)
        return;
    if (!sync_atomic_compare_and_swap_int32(&du_resolv.sema, 0, 1))
        return;
    Time now = time_now();
    bool expired = time_after(now, time_add(du_resolv.last_checked, 5 * TIME_SECOND));
    bool rechecks = !time_equal(du_resolv.last_checked, burrow__net_distant_future());
    bool recheck =
        (expired || burrow__dns_config_is_default_ns(&du_resolv.conf->c)) && rechecks;
    if (recheck) {
        du_resolv.last_checked = now;
        bool reread = true;
        if (strcmp(BURROW_OS_NAME, "windows") != 0) {
            Time mtime;
            memset(&mtime, 0, sizeof mtime);
            Arena sar;
            arena_init(&sar, NULL, 0);
            Error err = BURROW_NO_ERROR;
            FsFileInfo fi = os_stat(arena_allocator(&sar), name, &err);
            if (BURROW_OK(err))
                mtime = fi.vt->mod_time(fi.data);
            arena_free(&sar);
            reread = !time_equal(mtime, du_resolv.conf->c.mtime);
        }
        if (reread) {
            DuConf *h = du_read(name);
            if (h != NULL)
                du_swap(h);
        }
    }
    sync_atomic_store_int32(&du_resolv.sema, 0);
}

burrow__DNSConfig *burrow__net_system_dns_config(void) {
    return burrow__net_system_dns_config_named(DU_LIT(du_path));
}

burrow__DNSConfig *burrow__net_system_dns_config_named(Str name) {
    du_try_update(name);
    sync_mutex_lock(&du_resolv.mu);
    DuConf *h = du_resolv.conf;
    if (!h->pinned)
        sync_atomic_add_int32(&h->refs, 1);
    sync_mutex_unlock(&du_resolv.mu);
    return &h->c;
}

/* forceUpdateConf, which waits for sema where tryUpdate would give up. */
static void du_force(DuConf *h, Time last_checked) {
    du_swap(h);
    while (!sync_atomic_compare_and_swap_int32(&du_resolv.sema, 0, 1))
        runtime_gosched();
    du_resolv.last_checked = last_checked;
    sync_atomic_store_int32(&du_resolv.sema, 0);
}

void burrow__net_force_dns_config(const burrow__DNSConfig *c, Time last_checked) {
    du_once();
    DuConf *h = (DuConf *)mem_alloc(heap_allocator(), sizeof(DuConf), _Alignof(DuConf));
    if (h == NULL)
        return;
    arena_init(&h->ar, NULL, 0);
    h->refs = 1;
    h->c = *c;
    du_force(h, last_checked);
}

void burrow__net_force_dns_config_file(Str filename, Time last_checked) {
    du_once();
    DuConf *h = du_read(filename);
    if (h != NULL)
        du_force(h, last_checked);
}

/* ------------------------------------------------------------ the exchange */

/* equalASCIIName: the two names the same but for the case of ASCII
 * letters. */
static bool du_equal_ascii_name(const DnsmsgName *x, const DnsmsgName *y) {
    if (x->length != y->length)
        return false;
    for (int i = 0; i < x->length; i++) {
        Byte a = x->data[i], b = y->data[i];
        if (a >= 'A' && a <= 'Z')
            a += 0x20;
        if (b >= 'A' && b <= 'Z')
            b += 0x20;
        if (a != b)
            return false;
    }
    return true;
}

/* newRequest: the query, as it goes over UDP and with its two byte length
 * for TCP, in buf or in a when it outgrows it. */
static Error du_new_request(Alloc *a, Byte buf[514], const DnsmsgQuestion *q, bool ad,
                            uint16_t *id, Slice *udp_req, Slice *tcp_req) {
    *id = (uint16_t)runtime_rand64();
    DnsmsgHeader h;
    memset(&h, 0, sizeof h);
    h.id = *id;
    h.recursion_desired = true;
    h.authentic_data = ad;
    DnsmsgBuilder b;
    memset(&b, 0, sizeof b);
    Error err =
        burrow__dnsmsg_new_builder(&b, a, slice_from(buf, 2, 514, TYPE_BYTE), h);
    if (BURROW_OK(err))
        err = burrow__dnsmsg_builder_start_questions(&b);
    if (BURROW_OK(err))
        err = burrow__dnsmsg_builder_question(&b, q);
    Str edns0;
    if (BURROW_OK(err) &&
        !(burrow__net_godebug("netedns0", &edns0) && str_eq(edns0, BURROW_S("0")))) {
        err = burrow__dnsmsg_builder_start_additionals(&b);
        DnsmsgResourceHeader rh;
        memset(&rh, 0, sizeof rh);
        if (BURROW_OK(err))
            err = burrow__dnsmsg_resource_header_set_edns0(&rh, DU_MAX_DNS_PACKET_SIZE,
                                                           DNSMSG_RCODE_SUCCESS, false);
        DnsmsgOPTResource opt;
        memset(&opt, 0, sizeof opt);
        if (BURROW_OK(err))
            err = burrow__dnsmsg_builder_opt_resource(&b, rh, &opt);
    }
    Slice out = slice_nil(TYPE_BYTE);
    if (BURROW_OK(err))
        err = burrow__dnsmsg_builder_finish(&b, &out);
    burrow__dnsmsg_builder_free(&b);
    if (BURROW_FAILED(err))
        return err;
    Byte *p = (Byte *)out.p;
    Int l = out.len - 2;
    p[0] = (Byte)(l >> 8);
    p[1] = (Byte)l;
    *tcp_req = out;
    *udp_req = slice_from(p + 2, l, l, TYPE_BYTE);
    return BURROW_NO_ERROR;
}

/* checkResponse: whether the answer is to the question asked. */
static bool du_check_response(uint16_t req_id, const DnsmsgQuestion *req_ques,
                              const DnsmsgHeader *resp_hdr,
                              const DnsmsgQuestion *resp_ques) {
    if (!resp_hdr->response)
        return false;
    if (req_id != resp_hdr->id)
        return false;
    if (req_ques->type != resp_ques->type || req_ques->class_ != resp_ques->class_ ||
        !du_equal_ascii_name(&req_ques->name, &resp_ques->name))
        return false;
    return true;
}

/* dnsPacketRoundTrip: the query, and reads until one is the answer to it,
 * since anyone can send a packet to the port. The answer is in a. */
static Error du_packet_round_trip(NetConn c, Alloc *a, uint16_t id,
                                  const DnsmsgQuestion *query, Slice b, DnsmsgParser *p,
                                  DnsmsgHeader *h) {
    Error err = BURROW_NO_ERROR;
    (void)c.vt->writer.write(c.data, b, &err);
    if (BURROW_FAILED(err))
        return err;
    Byte *buf = (Byte *)mem_alloc_nozero(a, DU_MAX_DNS_PACKET_SIZE, 1);
    if (buf == NULL)
        return burrow_err_out_of_memory;
    for (;;) {
        Int n = c.vt->reader.read(
            c.data,
            slice_from(buf, DU_MAX_DNS_PACKET_SIZE, DU_MAX_DNS_PACKET_SIZE, TYPE_BYTE),
            &err);
        if (BURROW_FAILED(err))
            return err;
        memset(p, 0, sizeof *p);
        if (BURROW_FAILED(
                burrow__dnsmsg_parser_start(p, slice_from(buf, n, n, TYPE_BYTE), h)))
            continue; /* not a DNS message */
        DnsmsgQuestion q;
        if (BURROW_FAILED(burrow__dnsmsg_parser_question(p, &q)) ||
            !du_check_response(id, query, h, &q))
            continue; /* not the answer */
        return BURROW_NO_ERROR;
    }
}

/* dnsStreamRoundTrip: the same over a stream, where each message has its
 * length in front of it. */
static Error du_stream_round_trip(NetConn c, Alloc *a, uint16_t id,
                                  const DnsmsgQuestion *query, Slice b, DnsmsgParser *p,
                                  DnsmsgHeader *h) {
    Error err = BURROW_NO_ERROR;
    (void)c.vt->writer.write(c.data, b, &err);
    if (BURROW_FAILED(err))
        return err;
    /* 1280 is what fits in a packet over Ethernet, which RFC 4035 says is a
     * reasonable size to start with. */
    Int size = 1280;
    Byte *buf = (Byte *)mem_alloc_nozero(a, (size_t)size, 1);
    if (buf == NULL)
        return burrow_err_out_of_memory;
    IoReader rd = net_conn_as_io_reader(c);
    (void)io_read_full(rd, slice_from(buf, 2, 2, TYPE_BYTE), &err);
    if (BURROW_FAILED(err))
        return err;
    Int l = (Int)buf[0] << 8 | (Int)buf[1];
    if (l > size) {
        buf = (Byte *)mem_alloc_nozero(a, (size_t)l, 1);
        if (buf == NULL)
            return burrow_err_out_of_memory;
    }
    Int n = io_read_full(rd, slice_from(buf, l, l, TYPE_BYTE), &err);
    if (BURROW_FAILED(err))
        return err;
    memset(p, 0, sizeof *p);
    if (BURROW_FAILED(
            burrow__dnsmsg_parser_start(p, slice_from(buf, n, n, TYPE_BYTE), h)))
        return burrow__net_err_cannot_unmarshal;
    DnsmsgQuestion q;
    if (BURROW_FAILED(burrow__dnsmsg_parser_question(p, &q)))
        return burrow__net_err_cannot_unmarshal;
    if (!du_check_response(id, query, h, &q))
        return burrow__net_err_invalid_dns_response;
    return BURROW_NO_ERROR;
}

/* splitHostZone: "fe80::1%eth0" as the address and the zone. */
static Str du_split_host_zone(Str host, Str *zone) {
    *zone = BURROW_STR_EMPTY;
    for (Int i = host.len - 1; i > 0; i--) {
        if (host.p[i] == '%') {
            *zone = str_from_bytes(host.p + i + 1, host.len - i - 1);
            return str_from_bytes(host.p, i);
        }
    }
    return host;
}

/* One try over one network. ma has the answer, and sa the rest. */
static Error du_round_trip(NetResolver *r, Alloc *sa, Alloc *ma, Context ctx,
                           Str network, Str server, Duration timeout, uint16_t id,
                           const DnsmsgQuestion *q, Slice udp_req, Slice tcp_req,
                           DnsmsgParser *p, DnsmsgHeader *h) {
    /* The context's deadline, or timeout from now when that is sooner. */
    int64_t now = burrow_nanotime();
    int64_t when = now + timeout;
    int64_t parent;
    if (context_deadline(ctx, &parent) && parent < when)
        when = parent;
    Time deadline = time_add(time_now(), when - now);

    Error err = BURROW_NO_ERROR;
    NetConn c;
    bool packet = false;
    ContextCancelFunc cancel;
    Context dctx = context_with_deadline(sa, ctx, when, &cancel);
    if (dctx.vt == NULL)
        return burrow_err_out_of_memory;
    /* Resolver.dial: the Resolver's Dial, or a zero Dialer's. */
    if (r->dial.f != NULL)
        c = BURROW_CALLF(r->dial, sa, dctx, network, server, &packet, &err);
    else
        c = net_dialer_dial_context(NULL, sa, dctx, network, server, &err);
    BURROW_CALLF0(cancel);
    context_release(dctx);
    if (BURROW_FAILED(err))
        return burrow__net_map_err(err);
    if (net_conn_as_udp_conn(c) != NULL)
        packet = true;
    (void)c.vt->set_deadline(c.data, deadline);
    if (packet)
        err = du_packet_round_trip(c, ma, id, q, udp_req, p, h);
    else
        err = du_stream_round_trip(c, ma, id, q, tcp_req, p, h);
    net_conn_free(c);
    if (BURROW_FAILED(err))
        return burrow__net_map_err(err);
    return BURROW_NO_ERROR;
}

Error burrow__net_dns_exchange(NetResolver *r, Alloc *ma, Context ctx, Str server,
                               DnsmsgQuestion q, Duration timeout, bool use_tcp,
                               bool ad, DnsmsgParser *p, DnsmsgHeader *h) {
    memset(p, 0, sizeof *p);
    memset(h, 0, sizeof *h);
    q.class_ = DNSMSG_CLASS_INET;
    Arena sar;
    arena_init(&sar, NULL, 0);
    Alloc *sa = arena_allocator(&sar);
    Byte buf[514];
    uint16_t id;
    Slice udp_req, tcp_req;
    if (BURROW_FAILED(du_new_request(sa, buf, &q, ad, &id, &udp_req, &tcp_req))) {
        arena_free(&sar);
        return burrow__net_err_cannot_marshal;
    }
    for (int i = use_tcp ? 1 : 0; i < 2; i++) {
        Str network = i == 0 ? BURROW_S("udp") : BURROW_S("tcp");
        Error err = du_round_trip(r, sa, ma, ctx, network, server, timeout, id, &q,
                                  udp_req, tcp_req, p, h);
        if (BURROW_FAILED(err)) {
            arena_free(&sar);
            return err;
        }
        if (!du_same(burrow__dnsmsg_parser_skip_question(p),
                     burrow__dnsmsg_err_section_done)) {
            arena_free(&sar);
            return burrow__net_err_invalid_dns_response;
        }
        /* An answer cut short over UDP is asked for again over TCP. */
        if (h->truncated && i == 0)
            continue;
        arena_free(&sar);
        return BURROW_NO_ERROR;
    }
    arena_free(&sar);
    return burrow__net_err_no_answer_from_dns_server;
}

/* extractExtendedRCode: the RCode with the high bits an OPT record in the
 * additional section adds, and whether that section has anything. p is a
 * copy, so the caller's is not moved. */
static DnsmsgRCode du_extract_extended_rcode(DnsmsgParser p, const DnsmsgHeader *hdr,
                                             bool *has_add) {
    (void)burrow__dnsmsg_parser_skip_all_answers(&p);
    (void)burrow__dnsmsg_parser_skip_all_authorities(&p);
    *has_add = false;
    for (;;) {
        DnsmsgResourceHeader ahdr;
        if (BURROW_FAILED(burrow__dnsmsg_parser_additional_header(&p, &ahdr)))
            return hdr->rcode;
        *has_add = true;
        if (ahdr.type == DNSMSG_TYPE_OPT)
            return burrow__dnsmsg_resource_header_extended_rcode(&ahdr, hdr->rcode);
        if (BURROW_FAILED(burrow__dnsmsg_parser_skip_additional(&p)))
            return hdr->rcode;
    }
}

/* checkHeader: what the header of an answer says went wrong. */
static Error du_check_header(DnsmsgParser *p, const DnsmsgHeader *h) {
    bool has_add;
    DnsmsgRCode rcode = du_extract_extended_rcode(*p, h, &has_add);
    if (rcode == DNSMSG_RCODE_NAME_ERROR)
        return burrow__net_err_no_such_host;

    DnsmsgResourceHeader rh;
    Error err = burrow__dnsmsg_parser_answer_header(p, &rh);
    bool done = du_same(err, burrow__dnsmsg_err_section_done);
    if (BURROW_FAILED(err) && !done)
        return burrow__net_err_cannot_unmarshal;

    /* libresolv keeps going after a lame referral, and Go and this stop:
     * no answer, no authority, not recursive, and nothing additional. */
    if (rcode == DNSMSG_RCODE_SUCCESS && !h->authoritative && !h->recursion_available &&
        done && !has_add)
        return burrow__net_err_lame_referral;

    if (rcode != DNSMSG_RCODE_SUCCESS && rcode != DNSMSG_RCODE_NAME_ERROR) {
        /* A SERVFAIL may be the server or the network being slow, and may
         * not happen again. */
        if (rcode == DNSMSG_RCODE_SERVER_FAILURE)
            return burrow__net_err_server_temporarily_misbehaving;
        return burrow__net_err_server_misbehaving;
    }
    return BURROW_NO_ERROR;
}

/* skipToAnswer: p at the first answer of type qtype. */
static Error du_skip_to_answer(DnsmsgParser *p, DnsmsgType qtype) {
    for (;;) {
        DnsmsgResourceHeader h;
        Error err = burrow__dnsmsg_parser_answer_header(p, &h);
        if (du_same(err, burrow__dnsmsg_err_section_done))
            return burrow__net_err_no_such_host;
        if (BURROW_FAILED(err))
            return burrow__net_err_cannot_unmarshal;
        if (h.type == qtype)
            return BURROW_NO_ERROR;
        if (BURROW_FAILED(burrow__dnsmsg_parser_skip_answer(p)))
            return burrow__net_err_cannot_unmarshal;
    }
}

Error burrow__net_dns_try_one_name(NetResolver *r, Alloc *ma, Context ctx,
                                   burrow__DNSConfig *cfg, Str name, DnsmsgType qtype,
                                   DnsmsgParser *p, Str *server) {
    memset(p, 0, sizeof *p);
    *server = BURROW_STR_EMPTY;
    Error last_err = BURROW_NO_ERROR;
    uint32_t offset = burrow__dns_config_server_offset(cfg);
    uint32_t slen = (uint32_t)cfg->servers.len;
    const Str *servers = (const Str *)cfg->servers.p;

    DnsmsgQuestion q;
    memset(&q, 0, sizeof q);
    if (BURROW_FAILED(burrow__dnsmsg_new_name(name, &q.name))) {
        NetDNSError e = {0};
        e.err = error_text(burrow__net_err_cannot_marshal);
        e.name = name;
        return net_dns_error_as_error(&e, error_allocator());
    }
    q.type = qtype;
    q.class_ = DNSMSG_CLASS_INET;

    for (Int i = 0; i < cfg->attempts; i++) {
        for (uint32_t j = 0; j < slen; j++) {
            Str s = servers[(offset + j) % slen];
            DnsmsgHeader h;
            Error err = burrow__net_dns_exchange(r, ma, ctx, s, q, cfg->timeout,
                                                 cfg->use_tcp, cfg->trust_ad, p, &h);
            if (BURROW_FAILED(err)) {
                NetDNSError e = burrow__net_dns_error_of(err, name, s);
                if (err.vt != NULL && err.vt->self_type == TYPE_NET_OP_ERROR)
                    e.is_temporary = true;
                last_err = net_dns_error_as_error(&e, error_allocator());
                continue;
            }
            err = du_check_header(p, &h);
            if (BURROW_OK(err))
                err = du_skip_to_answer(p, qtype);
            if (BURROW_FAILED(err)) {
                Error de = burrow__net_new_dns_error(error_allocator(), err, name, s);
                if (du_same(err, burrow__net_err_no_such_host)) {
                    *server = s;
                    return de;
                }
                last_err = de;
                continue;
            }
            *server = s;
            return BURROW_NO_ERROR;
        }
    }
    memset(p, 0, sizeof *p);
    return last_err;
}

/* err.Name = name, for err a DNSError, as a new one in error_allocator(). */
static Error du_rename(Error err, Str name) {
    const NetDNSError *d = burrow__net_as_dns_error(err);
    if (d == NULL)
        return err;
    NetDNSError e = *d;
    e.name = name;
    return net_dns_error_as_error(&e, error_allocator());
}

/* A DNSError for an answer that would not parse. */
static Error du_unmarshal_error(Str name, Str server) {
    NetDNSError e = {0};
    e.err = error_text(burrow__net_err_cannot_unmarshal);
    e.name = name;
    e.server = server;
    return net_dns_error_as_error(&e, error_allocator());
}

static bool du_strict(const NetResolver *r, Error err) {
    return r->strict_errors && net_is_error(err) && net_error_temporary(err);
}

Error burrow__net_dns_lookup(NetResolver *r, Alloc *ma, Context ctx, Str name,
                             DnsmsgType qtype, burrow__DNSConfig *conf, DnsmsgParser *p,
                             Str *server) {
    memset(p, 0, sizeof *p);
    *server = BURROW_STR_EMPTY;
    if (!burrow__net_is_domain_name(name))
        return burrow__net_new_dns_error(
            error_allocator(), burrow__net_err_no_such_host, name, BURROW_STR_EMPTY);
    burrow__DNSConfig *held = NULL;
    if (conf == NULL)
        conf = held = burrow__net_system_dns_config();

    Arena lar;
    arena_init(&lar, NULL, 0);
    Slice names = burrow__dns_config_name_list(conf, arena_allocator(&lar), name);
    Error err = BURROW_NO_ERROR;
    Str s = BURROW_STR_EMPTY;
    for (Int i = 0; i < names.len; i++) {
        err = burrow__net_dns_try_one_name(r, ma, ctx, conf, ((const Str *)names.p)[i],
                                           qtype, p, &s);
        if (BURROW_OK(err))
            break;
        if (du_strict(r, err))
            break;
    }
    arena_free(&lar);
    if (BURROW_OK(err))
        *server = str_clone(ma, s);
    burrow__dns_config_put(held);
    if (BURROW_OK(err))
        return BURROW_NO_ERROR;
    memset(p, 0, sizeof *p);
    return du_rename(err, name);
}

/* ------------------------------------------------------------ the lookups */

Slice burrow__net_go_lookup_ip_files(Alloc *a, Str name, Str *canonical) {
    Arena sar;
    arena_init(&sar, NULL, 0);
    Str canon;
    Slice hs = burrow__net_lookup_static_host(arena_allocator(&sar), name, &canon);
    Slice addrs = slice_nil(TYPE_NET_IP_ADDR);
    for (Int i = 0; i < hs.len; i++) {
        Str zone;
        Str host = du_split_host_zone(((const Str *)hs.p)[i], &zone);
        NetIP ip = net_parse_ip(a, host);
        if (ip.p == NULL)
            continue;
        NetIPAddr x = {ip, str_clone(a, zone)};
        addrs = slice_append(a, addrs, &x, 1);
    }
    burrow__net_sort_by_rfc6724((NetIPAddr *)addrs.p, addrs.len);
    *canonical = str_clone(a, canon);
    arena_free(&sar);
    return addrs;
}

Slice burrow__net_go_lookup_host_order(NetResolver *r, Alloc *a, Context ctx, Str name,
                                       burrow__HostLookupOrder order,
                                       burrow__DNSConfig *conf, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (order == BURROW__HOST_LOOKUP_FILES_DNS || order == BURROW__HOST_LOOKUP_FILES) {
        /* Use the entries in the hosts file when there are any. */
        Str canonical;
        Slice addrs = burrow__net_lookup_static_host(a, name, &canonical);
        if (addrs.len > 0)
            return addrs;
        if (order == BURROW__HOST_LOOKUP_FILES) {
            BURROW_OUT(err, burrow__net_new_dns_error(error_allocator(),
                                                      burrow__net_err_no_such_host,
                                                      name, BURROW_STR_EMPTY));
            return slice_nil(TYPE_STRING);
        }
    }
    Arena sar;
    arena_init(&sar, NULL, 0);
    Error e = BURROW_NO_ERROR;
    Slice ips = burrow__net_go_lookup_ip_cname_order(
        r, arena_allocator(&sar), ctx, BURROW_S("ip"), name, order, conf, NULL, &e);
    if (BURROW_FAILED(e)) {
        arena_free(&sar);
        BURROW_OUT(err, e);
        return slice_nil(TYPE_STRING);
    }
    Slice addrs = slice_make(a, TYPE_STRING, 0, ips.len);
    for (Int i = 0; i < ips.len; i++) {
        Str s = net_ip_string(((const NetIPAddr *)ips.p)[i].ip, a);
        addrs = slice_append(a, addrs, &s, 1);
    }
    arena_free(&sar);
    return addrs;
}

/* One query of a goLookupIPCNAMEOrder, which runs on a goroutine of its own
 * unless resolv.conf says single-request. Its answer and its error are in
 * ar, and it is sent over lane by its address. */
typedef struct DuJob {
    NetResolver *r;
    Context ctx;
    burrow__DNSConfig *conf;
    Str fqdn;
    Arena ar;
    DnsmsgParser p;
    Str server;
    Error err;
    Chan *lane;
    DnsmsgType qtype;
} DuJob;

static void du_job_run(DuJob *j) {
    Alloc *a = arena_allocator(&j->ar);
    j->err = burrow__net_dns_try_one_name(j->r, a, j->ctx, j->conf, j->fqdn, j->qtype,
                                          &j->p, &j->server);
    /* The error dies with the goroutine that made it, unless it is moved. */
    if (BURROW_FAILED(j->err))
        j->err = error_retain(a, j->err);
}

static void du_job_go(void *env) {
    DuJob *j = (DuJob *)env;
    du_job_run(j);
    Uintptr v = (Uintptr)j;
    chan_send(j->lane, &v);
}

/* The addresses and the canonical name in one answer, with what went wrong
 * in *last_err. False when there was no memory. */
static bool du_collect(DnsmsgParser *p, Alloc *sa, Str name, Str server, Slice *addrs,
                       DnsmsgName *cname, Error *last_err) {
    for (;;) {
        DnsmsgResourceHeader h;
        Error err = burrow__dnsmsg_parser_answer_header(p, &h);
        if (BURROW_FAILED(err)) {
            if (!du_same(err, burrow__dnsmsg_err_section_done))
                *last_err = du_unmarshal_error(name, server);
            return true;
        }
        Byte ip[16];
        Int iplen = 0;
        if (h.type == DNSMSG_TYPE_A) {
            DnsmsgAResource a;
            if (BURROW_FAILED(burrow__dnsmsg_parser_a_resource(p, &a))) {
                *last_err = du_unmarshal_error(name, server);
                return true;
            }
            memcpy(ip, a.a, 4);
            iplen = 4;
        } else if (h.type == DNSMSG_TYPE_AAAA) {
            DnsmsgAAAAResource aaaa;
            if (BURROW_FAILED(burrow__dnsmsg_parser_aaaa_resource(p, &aaaa))) {
                *last_err = du_unmarshal_error(name, server);
                return true;
            }
            memcpy(ip, aaaa.aaaa, 16);
            iplen = 16;
        } else if (h.type == DNSMSG_TYPE_CNAME) {
            DnsmsgCNAMEResource c;
            if (BURROW_FAILED(burrow__dnsmsg_parser_cname_resource(p, &c))) {
                *last_err = du_unmarshal_error(name, server);
                return true;
            }
            if (cname->length == 0 && c.cname.length > 0)
                *cname = c.cname;
            continue;
        } else {
            if (BURROW_FAILED(burrow__dnsmsg_parser_skip_answer(p))) {
                *last_err = du_unmarshal_error(name, server);
                return true;
            }
            continue;
        }
        Byte *b = (Byte *)mem_alloc_nozero(sa, (size_t)iplen, 1);
        if (b == NULL)
            return false;
        memcpy(b, ip, (size_t)iplen);
        NetIPAddr x;
        memset(&x, 0, sizeof x);
        x.ip = slice_from(b, iplen, iplen, TYPE_BYTE);
        Int n = addrs->len;
        *addrs = slice_append(sa, *addrs, &x, 1);
        if (addrs->len != n + 1)
            return false;
        if (cname->length == 0 && h.name.length != 0)
            *cname = h.name;
    }
}

/* The answers to one name, from the hosts file, as a result of
 * goLookupIPCNAMEOrder. */
static Slice du_files_result(Alloc *a, Str name, Str *cname_out, bool *found,
                             Error *err) {
    Str canonical;
    Slice addrs = burrow__net_go_lookup_ip_files(a, name, &canonical);
    *found = addrs.len > 0;
    if (!*found)
        return addrs;
    DnsmsgName n;
    Error e = burrow__dnsmsg_new_name(canonical, &n);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return slice_nil(TYPE_NET_IP_ADDR);
    }
    if (cname_out != NULL)
        *cname_out = canonical;
    return addrs;
}

Slice burrow__net_go_lookup_ip_cname_order(NetResolver *r, Alloc *a, Context ctx,
                                           Str network, Str name,
                                           burrow__HostLookupOrder order,
                                           burrow__DNSConfig *conf, Str *cname_out,
                                           Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (cname_out != NULL)
        *cname_out = BURROW_STR_EMPTY;
    if (order == BURROW__HOST_LOOKUP_FILES_DNS || order == BURROW__HOST_LOOKUP_FILES) {
        bool found;
        Slice addrs = du_files_result(a, name, cname_out, &found, err);
        if (found)
            return addrs;
        if (order == BURROW__HOST_LOOKUP_FILES) {
            BURROW_OUT(err, burrow__net_new_dns_error(error_allocator(),
                                                      burrow__net_err_no_such_host,
                                                      name, BURROW_STR_EMPTY));
            return slice_nil(TYPE_NET_IP_ADDR);
        }
    }

    if (!burrow__net_is_domain_name(name)) {
        BURROW_OUT(err, burrow__net_new_dns_error(error_allocator(),
                                                  burrow__net_err_no_such_host, name,
                                                  BURROW_STR_EMPTY));
        return slice_nil(TYPE_NET_IP_ADDR);
    }

    burrow__DNSConfig *held = NULL;
    if (conf == NULL)
        conf = held = burrow__net_system_dns_config();

    bool is_cname = str_eq(network, BURROW_S("CNAME"));
    DnsmsgType qtypes[3] = {DNSMSG_TYPE_A, DNSMSG_TYPE_AAAA, DNSMSG_TYPE_CNAME};
    int nq = is_cname ? 3 : 2;
    Byte v = network.len > 0 ? network.p[network.len - 1] : 0;
    if (v == '4') {
        nq = 1;
    } else if (v == '6') {
        qtypes[0] = DNSMSG_TYPE_AAAA;
        nq = 1;
    }

    Arena sar;
    arena_init(&sar, NULL, 0);
    Alloc *sa = arena_allocator(&sar);
    Chan *lane = NULL;
    if (!conf->single_request)
        lane = chan_make(sa, TYPE_UINTPTR, 3);
    Str rooted = burrow__net_cat(sa, 2, name, BURROW_S("."));
    Slice names = burrow__dns_config_name_list(conf, sa, name);
    Slice addrs = slice_nil(TYPE_NET_IP_ADDR);
    DnsmsgName cname;
    memset(&cname, 0, sizeof cname);
    Error last_err = BURROW_NO_ERROR;
    bool oom = false;

    for (Int i = 0; i < names.len && !oom; i++) {
        Str fqdn = ((const Str *)names.p)[i];
        DuJob jobs[3];
        DuJob *ready[3];
        int nready = 0;
        for (int k = 0; k < nq; k++) {
            DuJob *j = &jobs[k];
            memset(j, 0, sizeof *j);
            j->r = r;
            j->ctx = ctx;
            j->conf = conf;
            j->fqdn = fqdn;
            j->qtype = qtypes[k];
            j->lane = lane;
            arena_init(&j->ar, NULL, 0);
            if (lane == NULL || !go(BURROW_FN(Func, du_job_go, j))) {
                du_job_run(j);
                ready[nready++] = j;
            }
        }
        bool hit_strict_error = false;
        for (int k = 0; k < nq; k++) {
            DuJob *j;
            if (k < nready) {
                j = ready[k];
            } else {
                Uintptr got = 0;
                (void)chan_recv(lane, &got);
                j = (DuJob *)got;
            }
            if (BURROW_FAILED(j->err)) {
                Error e = error_retain(error_allocator(), j->err);
                if (du_strict(r, e)) {
                    /* Every query is waited for, and then none of what they
                     * found counts. */
                    hit_strict_error = true;
                    last_err = e;
                } else if (BURROW_OK(last_err) || str_eq(fqdn, rooted)) {
                    /* The error for the name itself wins over one for the
                     * name with a search domain on it. */
                    last_err = e;
                }
            } else if (!oom) {
                oom =
                    !du_collect(&j->p, sa, name, j->server, &addrs, &cname, &last_err);
            }
            arena_free(&j->ar);
        }
        if (hit_strict_error) {
            addrs = slice_nil(TYPE_NET_IP_ADDR);
            break;
        }
        if (addrs.len > 0 || (is_cname && cname.length > 0))
            break;
    }

    Slice out = slice_nil(TYPE_NET_IP_ADDR);
    if (oom) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        goto done;
    }
    last_err = du_rename(last_err, name);
    burrow__net_sort_by_rfc6724((NetIPAddr *)addrs.p, addrs.len);
    if (addrs.len == 0 && !(is_cname && cname.length > 0)) {
        if (order == BURROW__HOST_LOOKUP_DNS_FILES) {
            bool found;
            out = du_files_result(a, name, cname_out, &found, err);
            if (found)
                goto done;
        }
        if (BURROW_FAILED(last_err)) {
            BURROW_OUT(err, last_err);
            goto done;
        }
    }
    if (addrs.len > 0) {
        out = slice_make(a, TYPE_NET_IP_ADDR, addrs.len, addrs.len);
        if (out.p == NULL) {
            BURROW_OUT(err, burrow_err_out_of_memory);
            goto done;
        }
        for (Int i = 0; i < addrs.len; i++) {
            NetIP ip = ((const NetIPAddr *)addrs.p)[i].ip;
            Byte *b = (Byte *)mem_alloc_nozero(a, (size_t)ip.len, 1);
            if (b == NULL) {
                out = slice_nil(TYPE_NET_IP_ADDR);
                BURROW_OUT(err, burrow_err_out_of_memory);
                goto done;
            }
            memcpy(b, ip.p, (size_t)ip.len);
            ((NetIPAddr *)out.p)[i].ip = slice_from(b, ip.len, ip.len, TYPE_BYTE);
        }
    }
    if (cname_out != NULL)
        *cname_out = str_clone(a, burrow__dnsmsg_name_string(&cname));
done:
    if (lane != NULL)
        chan_free(lane);
    arena_free(&sar);
    burrow__dns_config_put(held);
    return out;
}

Str burrow__net_go_lookup_cname(NetResolver *r, Alloc *a, Context ctx, Str host,
                                burrow__HostLookupOrder order, burrow__DNSConfig *conf,
                                Error *err) {
    Arena sar;
    arena_init(&sar, NULL, 0);
    Str cname = BURROW_STR_EMPTY;
    (void)burrow__net_go_lookup_ip_cname_order(r, arena_allocator(&sar), ctx,
                                               BURROW_S("CNAME"), host, order, conf,
                                               &cname, err);
    cname = str_clone(a, cname);
    arena_free(&sar);
    return cname;
}

Slice burrow__net_go_lookup_ptr(NetResolver *r, Alloc *a, Context ctx, Str addr,
                                burrow__HostLookupOrder order, burrow__DNSConfig *conf,
                                Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (order == BURROW__HOST_LOOKUP_FILES || order == BURROW__HOST_LOOKUP_FILES_DNS) {
        Slice names = burrow__net_lookup_static_addr(a, addr);
        if (names.len > 0)
            return names;
        if (order == BURROW__HOST_LOOKUP_FILES) {
            BURROW_OUT(err, burrow__net_new_dns_error(error_allocator(),
                                                      burrow__net_err_no_such_host,
                                                      addr, BURROW_STR_EMPTY));
            return slice_nil(TYPE_STRING);
        }
    }

    Arena sar;
    arena_init(&sar, NULL, 0);
    Alloc *sa = arena_allocator(&sar);
    Slice out = slice_nil(TYPE_STRING);
    Error e = BURROW_NO_ERROR;
    Str arpa = burrow__net_reverseaddr(sa, addr, &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        goto done;
    }
    DnsmsgParser p;
    Str server;
    e = burrow__net_dns_lookup(r, sa, ctx, arpa, DNSMSG_TYPE_PTR, conf, &p, &server);
    if (BURROW_FAILED(e)) {
        const NetDNSError *d = (const NetDNSError *)errors_as(e, TYPE_NET_DNS_ERROR);
        if (d != NULL && d->is_not_found && order == BURROW__HOST_LOOKUP_DNS_FILES) {
            Slice names = burrow__net_lookup_static_addr(a, addr);
            if (names.len > 0) {
                out = names;
                goto done;
            }
        }
        BURROW_OUT(err, e);
        goto done;
    }
    Slice ptrs = slice_nil(TYPE_STRING);
    for (;;) {
        DnsmsgResourceHeader h;
        e = burrow__dnsmsg_parser_answer_header(&p, &h);
        if (du_same(e, burrow__dnsmsg_err_section_done))
            break;
        if (BURROW_FAILED(e)) {
            BURROW_OUT(err, du_unmarshal_error(addr, server));
            goto done;
        }
        if (h.type != DNSMSG_TYPE_PTR) {
            if (BURROW_FAILED(burrow__dnsmsg_parser_skip_answer(&p))) {
                BURROW_OUT(err, du_unmarshal_error(addr, server));
                goto done;
            }
            continue;
        }
        DnsmsgPTRResource ptr;
        if (BURROW_FAILED(burrow__dnsmsg_parser_ptr_resource(&p, &ptr))) {
            BURROW_OUT(err, du_unmarshal_error(addr, server));
            goto done;
        }
        Str s = str_clone(sa, burrow__dnsmsg_name_string(&ptr.ptr));
        ptrs = slice_append(sa, ptrs, &s, 1);
    }
    out = slice_make(a, TYPE_STRING, 0, ptrs.len);
    for (Int i = 0; i < ptrs.len; i++) {
        Str s = str_clone(a, ((const Str *)ptrs.p)[i]);
        out = slice_append(a, out, &s, 1);
    }
done:
    arena_free(&sar);
    return out;
}

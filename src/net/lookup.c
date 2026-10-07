/* The resolver: lookups of hosts, addresses, ports and records.
 *
 * Derived from Go's src/net/lookup.go and src/net/lookup_unix.go, with
 * parseNetwork from src/net/dial.go and the IP part of internetAddrList from
 * src/net/ipsock.go.
 * Go source: go1.27.1.
 *
 * Copyright 2012 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "internal.h"

#include "../xnet/dnsmessage.h"

#include "burrow/chan.h"
#include "burrow/context.h"
#include "burrow/error.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/net.h"
#include "burrow/net/netip.h"
#include "burrow/proc.h"
#include "burrow/slice.h"
#include "burrow/sync.h"
#include "burrow/sync/atomic.h"
#include "burrow/type.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define LK_COUNT(arr) (uint16_t)(sizeof(arr) / sizeof((arr)[0]))

/* errMalformedDNSRecordsDetail */
#define LK_MALFORMED "DNS response contained records which contain invalid names"

/* ------------------------------------------------------------- the records */

static const Field lk_srv_fields[] = {
    {BURROW_S_INIT("Target"),
     {NULL, 0},
     TYPE_STRING,
     (uint32_t)offsetof(NetSRV, target)},
    {BURROW_S_INIT("Port"), {NULL, 0}, TYPE_UINT16, (uint32_t)offsetof(NetSRV, port)},
    {BURROW_S_INIT("Priority"),
     {NULL, 0},
     TYPE_UINT16,
     (uint32_t)offsetof(NetSRV, priority)},
    {BURROW_S_INIT("Weight"),
     {NULL, 0},
     TYPE_UINT16,
     (uint32_t)offsetof(NetSRV, weight)},
};

static const Field lk_mx_fields[] = {
    {BURROW_S_INIT("Host"), {NULL, 0}, TYPE_STRING, (uint32_t)offsetof(NetMX, host)},
    {BURROW_S_INIT("Pref"), {NULL, 0}, TYPE_UINT16, (uint32_t)offsetof(NetMX, pref)},
};

static const Field lk_ns_fields[] = {
    {BURROW_S_INIT("Host"), {NULL, 0}, TYPE_STRING, (uint32_t)offsetof(NetNS, host)},
};

static const Type lk_srv_desc = {
    BURROW_S_INIT("SRV"),
    BURROW_S_INIT("net"),
    KIND_STRUCT,
    (uint32_t)sizeof(NetSRV),
    (uint16_t)_Alignof(NetSRV),
    LK_COUNT(lk_srv_fields),
    0,
    lk_srv_fields,
    NULL,
    NULL,
    NULL,
    0,
    0x6e737276U, /* "nsrv" */
    NULL,
};

static const Type lk_mx_desc = {
    BURROW_S_INIT("MX"),
    BURROW_S_INIT("net"),
    KIND_STRUCT,
    (uint32_t)sizeof(NetMX),
    (uint16_t)_Alignof(NetMX),
    LK_COUNT(lk_mx_fields),
    0,
    lk_mx_fields,
    NULL,
    NULL,
    NULL,
    0,
    0x6e6d7872U, /* "nmxr" */
    NULL,
};

static const Type lk_ns_desc = {
    BURROW_S_INIT("NS"),
    BURROW_S_INIT("net"),
    KIND_STRUCT,
    (uint32_t)sizeof(NetNS),
    (uint16_t)_Alignof(NetNS),
    LK_COUNT(lk_ns_fields),
    0,
    lk_ns_fields,
    NULL,
    NULL,
    NULL,
    0,
    0x6e6e7372U, /* "nnsr" */
    NULL,
};

const Type *const TYPE_NET_SRV = &lk_srv_desc;
const Type *const TYPE_NET_MX = &lk_mx_desc;
const Type *const TYPE_NET_NS = &lk_ns_desc;

/* ---------------------------------------------------------------- helpers */

/* DefaultResolver. Its lookups in flight are the only state it has. */
static NetResolver lk_default;

NetResolver *net_default_resolver(void) {
    return &lk_default;
}

static NetResolver *lk_r(NetResolver *r) {
    return r != NULL ? r : &lk_default;
}

/* systemConf().hostLookupOrder and addrLookupOrder. The C library's order
 * only comes from a conf that says cgo is there, which the system's never
 * does, and Go without cgo goes on to its own lookups in its place. */
static burrow__HostLookupOrder lk_host_order(NetResolver *r, Str host,
                                             burrow__DNSConfig **conf) {
    burrow__HostLookupOrder o =
        burrow__net_conf_host_lookup_order(burrow__net_system_conf(), r, host, conf);
    return o == BURROW__HOST_LOOKUP_CGO ? BURROW__HOST_LOOKUP_FILES_DNS : o;
}

static burrow__HostLookupOrder lk_addr_order(NetResolver *r, Str addr,
                                             burrow__DNSConfig **conf) {
    burrow__HostLookupOrder o =
        burrow__net_conf_addr_lookup_order(burrow__net_system_conf(), r, addr, conf);
    return o == BURROW__HOST_LOOKUP_CGO ? BURROW__HOST_LOOKUP_FILES_DNS : o;
}

static bool lk_is_ip(Str s, NetipAddr *ip) {
    Error e = BURROW_NO_ERROR;
    *ip = netip_parse_addr(s, &e);
    return BURROW_OK(e);
}

/* newDNSError(err, name, "") */
static Error lk_dns_error(Error err, Str name) {
    return burrow__net_new_dns_error(error_allocator(), err, name, BURROW_STR_EMPTY);
}

/* &DNSError{Err: text, Name: name, Server: server} */
static Error lk_detail_error(Str text, Str name, Str server) {
    NetDNSError e;
    memset(&e, 0, sizeof e);
    e.err = text;
    e.name = name;
    e.server = server;
    return net_dns_error_as_error(&e, error_allocator());
}

static Error lk_unmarshal_error(Str name, Str server) {
    return lk_detail_error(BURROW_S("cannot unmarshal DNS message"), name, server);
}

static Error lk_malformed_error(Str name) {
    return lk_detail_error(BURROW_S(LK_MALFORMED), name, BURROW_STR_EMPTY);
}

static Error lk_addr_error(Str text, Str addr) {
    NetAddrError e = {text, addr};
    return net_addr_error_as_error(&e, error_allocator());
}

static bool lk_one_of(Str s, const char *const *list, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (str_eq(s, str_from_cstr(list[i])))
            return true;
    return false;
}

/* IP(ip.AsSlice()).To16(), in a. */
static NetIP lk_ip16(Alloc *a, NetipAddr ip) {
    NetIP out = slice_make(a, TYPE_BYTE, 16, 16);
    if (out.p != NULL) {
        NetipAddrAs16Ret b = netip_addr_as16(ip);
        memcpy(out.p, b.a, 16);
    }
    return out;
}

static NetIP lk_ip_clone(Alloc *a, NetIP ip) {
    NetIP out = slice_make(a, TYPE_BYTE, ip.len, ip.len);
    if (out.p != NULL && ip.len > 0)
        memcpy(out.p, ip.p, (size_t)ip.len);
    return out;
}

/* The NetIPAddr values of src, copied into a, or false when a is out of
 * memory. */
static bool lk_copy_ip_addrs(Alloc *a, Slice src, Slice *out) {
    *out = slice_make(a, TYPE_NET_IP_ADDR, src.len, src.len);
    if (out->p == NULL && src.len > 0)
        return false;
    for (Int i = 0; i < src.len; i++) {
        const NetIPAddr *s = &((const NetIPAddr *)src.p)[i];
        NetIPAddr *d = &((NetIPAddr *)out->p)[i];
        d->ip = lk_ip_clone(a, s->ip);
        d->zone = str_clone(a, s->zone);
        if (d->ip.p == NULL && s->ip.len > 0)
            return false;
    }
    return true;
}

static Slice lk_copy_strs(Alloc *a, Slice src) {
    Slice out = slice_make(a, TYPE_STRING, 0, src.len);
    for (Int i = 0; i < src.len; i++) {
        Str s = str_clone(a, ((const Str *)src.p)[i]);
        out = slice_append(a, out, &s, 1);
    }
    return out;
}

Error burrow__net_parse_network(Str network, bool needs_proto, Str *afnet, Int *proto) {
    static const char *const plain[] = {"tcp",  "tcp4", "tcp6",     "udp",
                                        "udp4", "udp6", "ip",       "ip4",
                                        "ip6",  "unix", "unixgram", "unixpacket"};
    static const char *const ips[] = {"ip", "ip4", "ip6"};
    Int i = network.len - 1;
    while (i >= 0 && network.p[i] != ':')
        i--;
    *proto = 0;
    if (i < 0) {
        if (!lk_one_of(network, plain, sizeof plain / sizeof plain[0]) ||
            (needs_proto && lk_one_of(network, ips, sizeof ips / sizeof ips[0])))
            return net_unknown_network_error(error_allocator(), network);
        *afnet = network;
        return BURROW_NO_ERROR;
    }
    Str af = str_from_bytes(network.p, i);
    if (!lk_one_of(af, ips, sizeof ips / sizeof ips[0]))
        return net_unknown_network_error(error_allocator(), network);
    Str protostr = str_from_bytes(network.p + i + 1, network.len - i - 1);
    Int used = 0;
    if (!burrow__net_dtoi(protostr, proto, &used) || used != protostr.len) {
        Error e = BURROW_NO_ERROR;
        *proto = burrow__net_lookup_protocol(protostr, &e);
        if (BURROW_FAILED(e))
            return e;
    }
    *afnet = af;
    return BURROW_NO_ERROR;
}

/* ------------------------------------------------------------ the IP lookup
 *
 * lookupIPAddr runs the lookup on a goroutine of its own, so that the caller
 * can give up on it when its context is done, and two lookups of the same
 * name at the same time share one, which is Go's singleflight group. The
 * lookup has a context of its own that only the last caller to give up
 * cancels, and its answer is in an arena that the last of the goroutine and
 * the callers frees. */

typedef struct LkCall {
    struct LkCall *next;
    NetResolver *r;
    Str network;
    Str host;
    Arena ar;
    Context ctx;
    ContextCancelFunc cancel;
    Chan *done;
    Slice addrs;
    Error err;
    int32_t refs;
    int32_t dups;
    bool listed;
} LkCall;

/* r->lookupIP */
static Slice lk_lookup_ip(NetResolver *r, Alloc *a, Context ctx, Str network, Str host,
                          Error *err) {
    burrow__DNSConfig *conf = NULL;
    burrow__HostLookupOrder order = lk_host_order(r, host, &conf);
    Slice ips = burrow__net_go_lookup_ip_cname_order(r, a, ctx, network, host, order,
                                                     conf, NULL, err);
    burrow__dns_config_put(conf);
    return ips;
}

/* Takes c out of r's list. r->burrow_mu is held. */
static void lk_unlist(LkCall *c) {
    LkCall **pp = (LkCall **)&c->r->burrow_calls;
    while (*pp != NULL && *pp != c)
        pp = &(*pp)->next;
    if (*pp == c)
        *pp = c->next;
    c->listed = false;
}

static void lk_call_put(LkCall *c) {
    if (sync_atomic_add_int32(&c->refs, -1) != 0)
        return;
    BURROW_CALLF0(c->cancel);
    context_release(c->ctx);
    chan_free(c->done);
    arena_free(&c->ar);
    mem_free(heap_allocator(), c, sizeof(LkCall), _Alignof(LkCall));
}

static void lk_call_go(void *env) {
    LkCall *c = (LkCall *)env;
    Alloc *a = arena_allocator(&c->ar);
    Error e = BURROW_NO_ERROR;
    c->addrs = lk_lookup_ip(c->r, a, c->ctx, c->network, c->host, &e);
    /* The error dies with the goroutine that made it, unless it is moved. */
    if (BURROW_FAILED(e))
        e = error_retain(a, e);
    c->err = e;
    /* A lookup of the name from now on starts a new one. */
    sync_mutex_lock(&c->r->burrow_mu);
    if (c->listed)
        lk_unlist(c);
    sync_mutex_unlock(&c->r->burrow_mu);
    chan_close(c->done);
    lk_call_put(c);
}

/* A new call, with a reference for the goroutine and one for the caller, or
 * NULL when there is no memory for it. */
static LkCall *lk_call_new(NetResolver *r, Str network, Str host) {
    LkCall *c = (LkCall *)mem_alloc(heap_allocator(), sizeof(LkCall), _Alignof(LkCall));
    if (c == NULL)
        return NULL;
    memset(c, 0, sizeof *c);
    arena_init(&c->ar, NULL, 0);
    Alloc *a = arena_allocator(&c->ar);
    c->r = r;
    c->network = str_clone(a, network);
    c->host = str_clone(a, host);
    c->ctx = context_with_cancel(a, context_background(), &c->cancel);
    if (BURROW_CONTEXT_IS_NIL(c->ctx) || c->host.len != host.len ||
        c->network.len != network.len) {
        if (!BURROW_CONTEXT_IS_NIL(c->ctx)) {
            BURROW_CALLF0(c->cancel);
            context_release(c->ctx);
        }
        arena_free(&c->ar);
        mem_free(heap_allocator(), c, sizeof(LkCall), _Alignof(LkCall));
        return NULL;
    }
    c->done = chan_make(a, TYPE_UINTPTR, 0);
    if (c->done == NULL) {
        BURROW_CALLF0(c->cancel);
        context_release(c->ctx);
        arena_free(&c->ar);
        mem_free(heap_allocator(), c, sizeof(LkCall), _Alignof(LkCall));
        return NULL;
    }
    c->refs = 2;
    return c;
}

/* DoChan: the call in flight for network and host, or a new one that has
 * been started. NULL when there is no memory for one. */
static LkCall *lk_join(NetResolver *r, Str network, Str host) {
    sync_mutex_lock(&r->burrow_mu);
    for (LkCall *c = (LkCall *)r->burrow_calls; c != NULL; c = c->next) {
        if (str_eq(c->network, network) && str_eq(c->host, host)) {
            c->dups++;
            sync_atomic_add_int32(&c->refs, 1);
            sync_mutex_unlock(&r->burrow_mu);
            return c;
        }
    }
    LkCall *c = lk_call_new(r, network, host);
    if (c != NULL) {
        c->next = (LkCall *)r->burrow_calls;
        r->burrow_calls = c;
        c->listed = true;
    }
    sync_mutex_unlock(&r->burrow_mu);
    /* Without a goroutine, the lookup runs here, and anyone who joins it in
     * the meantime waits for this caller. */
    if (c != NULL && !go(BURROW_FN(Func, lk_call_go, c)))
        lk_call_go(c);
    return c;
}

/* lookupIPAddr: the NetIPAddr values of host in a. */
static Slice lk_lookup_ip_addr(NetResolver *r, Alloc *a, Context ctx, Str network,
                               Str host, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    Slice out = slice_nil(TYPE_NET_IP_ADDR);
    if (host.len == 0) {
        BURROW_OUT(err, lk_dns_error(burrow__net_err_no_such_host, host));
        return out;
    }
    NetipAddr lit;
    if (lk_is_ip(host, &lit)) {
        NetIPAddr ia = {lk_ip16(a, lit), str_clone(a, netip_addr_zone(lit))};
        out = slice_make(a, TYPE_NET_IP_ADDR, 0, 1);
        out = slice_append(a, out, &ia, 1);
        if (ia.ip.p == NULL || out.p == NULL) {
            BURROW_OUT(err, burrow_err_out_of_memory);
            return slice_nil(TYPE_NET_IP_ADDR);
        }
        return out;
    }

    LkCall *c = lk_join(r, network, host);
    if (c == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return out;
    }
    Chan *cd = context_done(ctx);
    bool gave_up = false;
    if (cd == NULL) {
        (void)chan_recv(c->done, NULL);
    } else {
        SelectCase cases[2] = {BURROW_RECV(cd, NULL), BURROW_RECV(c->done, NULL)};
        gave_up = chan_select(cases, 2) == 0;
    }

    if (gave_up) {
        /* ForgetUnshared: the lookup stops when nobody else waits for it. */
        sync_mutex_lock(&r->burrow_mu);
        bool forget = c->listed && c->dups == 0;
        if (forget)
            lk_unlist(c);
        sync_mutex_unlock(&r->burrow_mu);
        if (forget)
            BURROW_CALLF0(c->cancel);
        lk_call_put(c);
        BURROW_OUT(err, lk_dns_error(burrow__net_map_err(context_err(ctx)), host));
        return out;
    }

    Error e = c->err;
    if (BURROW_FAILED(e)) {
        e = error_retain(error_allocator(), e);
        if (burrow__net_as_dns_error(e) == NULL)
            e = lk_dns_error(burrow__net_map_err(e), host);
    } else if (!lk_copy_ip_addrs(a, c->addrs, &out)) {
        e = burrow_err_out_of_memory;
        out = slice_nil(TYPE_NET_IP_ADDR);
    }
    lk_call_put(c);
    BURROW_OUT(err, e);
    return out;
}

/* ------------------------------------------------------- hosts and addresses */

Slice net_resolver_lookup_host(NetResolver *r, Alloc *a, Context ctx, Str host,
                               Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    r = lk_r(r);
    if (host.len == 0) {
        BURROW_OUT(err, lk_dns_error(burrow__net_err_no_such_host, host));
        return slice_nil(TYPE_STRING);
    }
    NetipAddr ip;
    if (lk_is_ip(host, &ip)) {
        Str s = str_clone(a, host);
        Slice out = slice_make(a, TYPE_STRING, 0, 1);
        out = slice_append(a, out, &s, 1);
        if (out.p == NULL || s.len != host.len) {
            BURROW_OUT(err, burrow_err_out_of_memory);
            return slice_nil(TYPE_STRING);
        }
        return out;
    }
    burrow__DNSConfig *conf = NULL;
    burrow__HostLookupOrder order = lk_host_order(r, host, &conf);
    Slice out = burrow__net_go_lookup_host_order(r, a, ctx, host, order, conf, err);
    burrow__dns_config_put(conf);
    return out;
}

Slice burrow__net_lookup_ip_addr(NetResolver *r, Alloc *a, Context ctx, Str network,
                                 Str host, Error *err) {
    return lk_lookup_ip_addr(lk_r(r), a, ctx, network, host, err);
}

Slice net_resolver_lookup_ip_addr(NetResolver *r, Alloc *a, Context ctx, Str host,
                                  Error *err) {
    return lk_lookup_ip_addr(lk_r(r), a, ctx, BURROW_S("ip"), host, err);
}

Slice net_resolver_lookup_ip(NetResolver *r, Alloc *a, Context ctx, Str network,
                             Str host, Error *err) {
    static const char *const ips[] = {"ip", "ip4", "ip6"};
    BURROW_OUT(err, BURROW_NO_ERROR);
    r = lk_r(r);
    Slice out = slice_nil(TYPE_NET_IP);
    Str afnet = BURROW_STR_EMPTY;
    Int proto = 0;
    Error e = burrow__net_parse_network(network, false, &afnet, &proto);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return out;
    }
    if (!lk_one_of(afnet, ips, sizeof ips / sizeof ips[0])) {
        BURROW_OUT(err, net_unknown_network_error(error_allocator(), network));
        return out;
    }
    if (host.len == 0) {
        BURROW_OUT(err, lk_dns_error(burrow__net_err_no_such_host, host));
        return out;
    }

    /* internetAddrList, for an IP network. */
    Arena sar;
    arena_init(&sar, NULL, 0);
    Slice addrs = lk_lookup_ip_addr(r, arena_allocator(&sar), ctx, afnet, host, &e);
    if (BURROW_FAILED(e)) {
        arena_free(&sar);
        BURROW_OUT(err, e);
        return out;
    }
    Byte last = afnet.len > 0 && afnet.p != NULL ? afnet.p[afnet.len - 1] : 0;
    out = slice_make(a, TYPE_NET_IP, 0, addrs.len + 1);
    for (Int i = 0; i <= addrs.len; i++) {
        NetIP ip;
        if (i < addrs.len) {
            ip = ((const NetIPAddr *)addrs.p)[i].ip;
        } else if (addrs.len == 1 && net_ip_equal(((const NetIPAddr *)addrs.p)[0].ip,
                                                  net_ipv6_unspecified)) {
            /* Issue 18806: a machine that can listen on :: but not connect
             * to it back tries 0.0.0.0 as well. */
            ip = net_ipv4_zero;
        } else {
            break;
        }
        bool is4 = net_ip_to4(ip).len != 0;
        if (last == '4' && !is4)
            continue;
        if (last == '6' && (ip.len != 16 || is4))
            continue;
        NetIP c = lk_ip_clone(a, ip);
        out = slice_append(a, out, &c, 1);
        if (c.p == NULL || out.p == NULL) {
            arena_free(&sar);
            BURROW_OUT(err, burrow_err_out_of_memory);
            return slice_nil(TYPE_NET_IP);
        }
    }
    arena_free(&sar);
    if (out.len == 0)
        BURROW_OUT(
            err, lk_addr_error(error_text(burrow__net_err_no_suitable_address), host));
    return out;
}

Slice net_resolver_lookup_net_ip(NetResolver *r, Alloc *a, Context ctx, Str network,
                                 Str host, Error *err) {
    Arena sar;
    arena_init(&sar, NULL, 0);
    Error e = BURROW_NO_ERROR;
    Slice ips =
        net_resolver_lookup_ip(r, arena_allocator(&sar), ctx, network, host, &e);
    Slice out = slice_nil(TYPE_NETIP_ADDR);
    if (BURROW_OK(e)) {
        out = slice_make(a, TYPE_NETIP_ADDR, 0, ips.len);
        for (Int i = 0; i < ips.len; i++) {
            bool ok = false;
            NetipAddr ip = netip_addr_from_slice(((const NetIP *)ips.p)[i], &ok);
            if (ok)
                out = slice_append(a, out, &ip, 1);
        }
        if (out.p == NULL && ips.len > 0)
            e = burrow_err_out_of_memory;
    }
    arena_free(&sar);
    BURROW_OUT(err, e);
    return out;
}

/* ------------------------------------------------------------------- ports */

Int net_resolver_lookup_port(NetResolver *r, Context ctx, Str network, Str service,
                             Error *err) {
    static const char *const nets[] = {"tcp",  "tcp4", "tcp6", "udp",
                                       "udp4", "udp6", "ip"};
    (void)r;
    (void)ctx;
    BURROW_OUT(err, BURROW_NO_ERROR);
    Int port = 0;
    if (burrow__net_parse_port(service, &port)) {
        if (network.len == 0) {
            /* A hint wildcard for Go 1.0 undocumented behaviour. */
            network = BURROW_S("ip");
        } else if (!lk_one_of(network, nets, sizeof nets / sizeof nets[0])) {
            BURROW_OUT(err, lk_addr_error(BURROW_S("unknown network"), network));
            return 0;
        }
        Error e = BURROW_NO_ERROR;
        port = burrow__net_lookup_port_map(network, service, &e);
        if (BURROW_FAILED(e)) {
            BURROW_OUT(err, e);
            return 0;
        }
    }
    if (port < 0 || port > 65535) {
        BURROW_OUT(err, lk_addr_error(BURROW_S("invalid port"), service));
        return 0;
    }
    return port;
}

/* ----------------------------------------------------------------- records */

Str net_resolver_lookup_cname(NetResolver *r, Alloc *a, Context ctx, Str host,
                              Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    r = lk_r(r);
    Arena sar;
    arena_init(&sar, NULL, 0);
    burrow__DNSConfig *conf = NULL;
    burrow__HostLookupOrder order = lk_host_order(r, host, &conf);
    Error e = BURROW_NO_ERROR;
    Str cname = burrow__net_go_lookup_cname(r, arena_allocator(&sar), ctx, host, order,
                                            conf, &e);
    burrow__dns_config_put(conf);
    Str out = BURROW_STR_EMPTY;
    if (BURROW_FAILED(e))
        BURROW_OUT(err, e);
    else if (!burrow__net_is_domain_name(cname))
        BURROW_OUT(err, lk_malformed_error(host));
    else
        out = str_clone(a, cname);
    arena_free(&sar);
    return out;
}

/* The next answer of type want, past those of other types: 1 with its header
 * in h, 0 at the end of the answers and -1 for an answer that does not
 * parse. */
static int lk_next(DnsmsgParser *p, DnsmsgType want, DnsmsgResourceHeader *h) {
    for (;;) {
        Error e = burrow__dnsmsg_parser_answer_header(p, h);
        if (e.vt == burrow__dnsmsg_err_section_done.vt &&
            e.data == burrow__dnsmsg_err_section_done.data)
            return 0;
        if (BURROW_FAILED(e))
            return -1;
        if (h->type == want)
            return 1;
        if (BURROW_FAILED(burrow__dnsmsg_parser_skip_answer(p)))
            return -1;
    }
}

Slice net_resolver_lookup_srv(NetResolver *r, Alloc *a, Context ctx, Str service,
                              Str proto, Str name, Str *cname, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    BURROW_OUT(cname, BURROW_STR_EMPTY);
    r = lk_r(r);
    Arena sar;
    arena_init(&sar, NULL, 0);
    Alloc *sa = arena_allocator(&sar);
    Slice out = slice_nil(TYPE_NET_SRV);
    Error e = BURROW_NO_ERROR;
    bool partial = false;

    /* goLookupSRV */
    Str target = name;
    if (service.len != 0 || proto.len != 0)
        target = burrow__net_cat(sa, 6, BURROW_S("_"), service, BURROW_S("._"), proto,
                                 BURROW_S("."), name);
    DnsmsgParser p;
    Str server = BURROW_STR_EMPTY;
    e = burrow__net_dns_lookup(r, sa, ctx, target, DNSMSG_TYPE_SRV, NULL, &p, &server);
    if (BURROW_FAILED(e))
        goto done;
    Str header = BURROW_STR_EMPTY;
    Slice srvs = slice_nil(TYPE_NET_SRV);
    for (;;) {
        DnsmsgResourceHeader h;
        int got = lk_next(&p, DNSMSG_TYPE_SRV, &h);
        if (got == 0)
            break;
        DnsmsgSRVResource srv;
        if (got < 0 || BURROW_FAILED(burrow__dnsmsg_parser_srv_resource(&p, &srv))) {
            e = lk_unmarshal_error(name, server);
            goto done;
        }
        if (header.len == 0 && h.name.length != 0)
            header = str_clone(sa, burrow__dnsmsg_name_string(&h.name));
        NetSRV v = {str_clone(sa, burrow__dnsmsg_name_string(&srv.target)), srv.port,
                    srv.priority, srv.weight};
        srvs = slice_append(sa, srvs, &v, 1);
        if (srvs.p == NULL) {
            e = burrow_err_out_of_memory;
            goto done;
        }
    }
    NetSRV **sorted = (NetSRV **)mem_alloc(
        sa, sizeof(NetSRV *) * (size_t)(srvs.len + 1), _Alignof(NetSRV *));
    if (sorted == NULL) {
        e = burrow_err_out_of_memory;
        goto done;
    }
    for (Int i = 0; i < srvs.len; i++)
        sorted[i] = &((NetSRV *)srvs.p)[i];
    burrow__net_srv_sort(sorted, srvs.len);

    /* LookupSRV */
    if (header.len != 0 && !burrow__net_is_domain_name(header)) {
        e = lk_detail_error(BURROW_S("SRV header name is invalid"), name,
                            BURROW_STR_EMPTY);
        goto done;
    }
    out = slice_make(a, TYPE_NET_SRV, 0, srvs.len);
    for (Int i = 0; i < srvs.len; i++) {
        if (!burrow__net_is_domain_name(sorted[i]->target))
            continue;
        NetSRV v = *sorted[i];
        v.target = str_clone(a, v.target);
        out = slice_append(a, out, &v, 1);
    }
    if (out.p == NULL && srvs.len > 0) {
        e = burrow_err_out_of_memory;
        goto done;
    }
    BURROW_OUT(cname, str_clone(a, header));
    if (out.len != srvs.len) {
        e = lk_malformed_error(name);
        partial = true;
    }
done:
    arena_free(&sar);
    if (BURROW_FAILED(e) && !partial) {
        out = slice_nil(TYPE_NET_SRV);
        BURROW_OUT(cname, BURROW_STR_EMPTY);
    }
    BURROW_OUT(err, e);
    return out;
}

Slice net_resolver_lookup_mx(NetResolver *r, Alloc *a, Context ctx, Str name,
                             Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    r = lk_r(r);
    Arena sar;
    arena_init(&sar, NULL, 0);
    Alloc *sa = arena_allocator(&sar);
    Slice out = slice_nil(TYPE_NET_MX);
    bool partial = false;

    /* goLookupMX */
    DnsmsgParser p;
    Str server = BURROW_STR_EMPTY;
    Error e =
        burrow__net_dns_lookup(r, sa, ctx, name, DNSMSG_TYPE_MX, NULL, &p, &server);
    if (BURROW_FAILED(e))
        goto done;
    Slice mxs = slice_nil(TYPE_NET_MX);
    for (;;) {
        DnsmsgResourceHeader h;
        int got = lk_next(&p, DNSMSG_TYPE_MX, &h);
        if (got == 0)
            break;
        DnsmsgMXResource mx;
        if (got < 0 || BURROW_FAILED(burrow__dnsmsg_parser_mx_resource(&p, &mx))) {
            e = lk_unmarshal_error(name, server);
            goto done;
        }
        NetMX v = {str_clone(sa, burrow__dnsmsg_name_string(&mx.mx)), mx.pref};
        mxs = slice_append(sa, mxs, &v, 1);
        if (mxs.p == NULL) {
            e = burrow_err_out_of_memory;
            goto done;
        }
    }
    NetMX **sorted = (NetMX **)mem_alloc(sa, sizeof(NetMX *) * (size_t)(mxs.len + 1),
                                         _Alignof(NetMX *));
    if (sorted == NULL) {
        e = burrow_err_out_of_memory;
        goto done;
    }
    for (Int i = 0; i < mxs.len; i++)
        sorted[i] = &((NetMX *)mxs.p)[i];
    burrow__net_mx_sort(sorted, mxs.len);

    /* LookupMX: a host that is not a domain name may still be an IP
     * address, without a zone. */
    out = slice_make(a, TYPE_NET_MX, 0, mxs.len);
    for (Int i = 0; i < mxs.len; i++) {
        Str host = sorted[i]->host;
        if (!burrow__net_is_domain_name(host)) {
            Str bare = host;
            if (bare.len > 0 && bare.p[bare.len - 1] == '.')
                bare.len--;
            NetipAddr ip;
            if (!lk_is_ip(bare, &ip) || netip_addr_zone(ip).len != 0)
                continue;
        }
        NetMX v = {str_clone(a, host), sorted[i]->pref};
        out = slice_append(a, out, &v, 1);
    }
    if (out.p == NULL && mxs.len > 0) {
        e = burrow_err_out_of_memory;
        goto done;
    }
    if (out.len != mxs.len) {
        e = lk_malformed_error(name);
        partial = true;
    }
done:
    arena_free(&sar);
    if (BURROW_FAILED(e) && !partial)
        out = slice_nil(TYPE_NET_MX);
    BURROW_OUT(err, e);
    return out;
}

Slice net_resolver_lookup_ns(NetResolver *r, Alloc *a, Context ctx, Str name,
                             Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    r = lk_r(r);
    Arena sar;
    arena_init(&sar, NULL, 0);
    Alloc *sa = arena_allocator(&sar);
    Slice out = slice_nil(TYPE_NET_NS);
    bool partial = false;

    /* goLookupNS */
    DnsmsgParser p;
    Str server = BURROW_STR_EMPTY;
    Error e =
        burrow__net_dns_lookup(r, sa, ctx, name, DNSMSG_TYPE_NS, NULL, &p, &server);
    if (BURROW_FAILED(e))
        goto done;
    Slice nss = slice_nil(TYPE_STRING);
    for (;;) {
        DnsmsgResourceHeader h;
        int got = lk_next(&p, DNSMSG_TYPE_NS, &h);
        if (got == 0)
            break;
        DnsmsgNSResource ns;
        if (got < 0 || BURROW_FAILED(burrow__dnsmsg_parser_ns_resource(&p, &ns))) {
            e = lk_unmarshal_error(name, server);
            goto done;
        }
        Str host = str_clone(sa, burrow__dnsmsg_name_string(&ns.ns));
        nss = slice_append(sa, nss, &host, 1);
        if (nss.p == NULL) {
            e = burrow_err_out_of_memory;
            goto done;
        }
    }

    /* LookupNS */
    out = slice_make(a, TYPE_NET_NS, 0, nss.len);
    for (Int i = 0; i < nss.len; i++) {
        Str host = ((const Str *)nss.p)[i];
        if (!burrow__net_is_domain_name(host))
            continue;
        NetNS v = {str_clone(a, host)};
        out = slice_append(a, out, &v, 1);
    }
    if (out.p == NULL && nss.len > 0) {
        e = burrow_err_out_of_memory;
        goto done;
    }
    if (out.len != nss.len) {
        e = lk_malformed_error(name);
        partial = true;
    }
done:
    arena_free(&sar);
    if (BURROW_FAILED(e) && !partial)
        out = slice_nil(TYPE_NET_NS);
    BURROW_OUT(err, e);
    return out;
}

Slice net_resolver_lookup_txt(NetResolver *r, Alloc *a, Context ctx, Str name,
                              Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    r = lk_r(r);
    Arena sar;
    arena_init(&sar, NULL, 0);
    Alloc *sa = arena_allocator(&sar);
    Slice out = slice_nil(TYPE_STRING);

    /* goLookupTXT */
    DnsmsgParser p;
    Str server = BURROW_STR_EMPTY;
    Error e =
        burrow__net_dns_lookup(r, sa, ctx, name, DNSMSG_TYPE_TXT, NULL, &p, &server);
    if (BURROW_FAILED(e))
        goto done;
    Slice txts = slice_nil(TYPE_STRING);
    for (;;) {
        DnsmsgResourceHeader h;
        int got = lk_next(&p, DNSMSG_TYPE_TXT, &h);
        if (got == 0)
            break;
        DnsmsgTXTResource txt;
        if (got < 0 ||
            BURROW_FAILED(burrow__dnsmsg_parser_txt_resource(&p, sa, &txt))) {
            e = lk_unmarshal_error(name, server);
            goto done;
        }
        /* The strings of one record are joined without a separator, which
         * is what the Go resolver always did. */
        Int n = 0;
        for (Int i = 0; i < txt.txt.len; i++)
            n += txt.txt.p[i].len;
        Byte *join = (Byte *)mem_alloc_nozero(sa, (size_t)n + 1, 1);
        if (join == NULL) {
            e = burrow_err_out_of_memory;
            goto done;
        }
        Int at = 0;
        for (Int i = 0; i < txt.txt.len; i++) {
            if (txt.txt.p[i].len > 0)
                memcpy(join + at, txt.txt.p[i].p, (size_t)txt.txt.p[i].len);
            at += txt.txt.p[i].len;
        }
        Str s = str_from_bytes(join, n);
        txts = slice_append(sa, txts, &s, 1);
        if (txts.p == NULL) {
            e = burrow_err_out_of_memory;
            goto done;
        }
    }
    out = lk_copy_strs(a, txts);
    if (out.p == NULL && txts.len > 0)
        e = burrow_err_out_of_memory;
done:
    arena_free(&sar);
    if (BURROW_FAILED(e))
        out = slice_nil(TYPE_STRING);
    BURROW_OUT(err, e);
    return out;
}

Slice net_resolver_lookup_addr(NetResolver *r, Alloc *a, Context ctx, Str addr,
                               Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    r = lk_r(r);
    Arena sar;
    arena_init(&sar, NULL, 0);
    burrow__DNSConfig *conf = NULL;
    burrow__HostLookupOrder order = lk_addr_order(r, addr, &conf);
    Error e = BURROW_NO_ERROR;
    Slice names =
        burrow__net_go_lookup_ptr(r, arena_allocator(&sar), ctx, addr, order, conf, &e);
    burrow__dns_config_put(conf);
    Slice out = slice_nil(TYPE_STRING);
    if (BURROW_OK(e)) {
        out = slice_make(a, TYPE_STRING, 0, names.len);
        for (Int i = 0; i < names.len; i++) {
            Str name = ((const Str *)names.p)[i];
            if (!burrow__net_is_domain_name(name))
                continue;
            Str s = str_clone(a, name);
            out = slice_append(a, out, &s, 1);
        }
        if (out.p == NULL && names.len > 0)
            e = burrow_err_out_of_memory;
        else if (out.len != names.len)
            e = lk_malformed_error(addr);
    }
    arena_free(&sar);
    BURROW_OUT(err, e);
    return out;
}

/* ------------------------------------------------- the package functions */

Slice net_lookup_host(Alloc *a, Str host, Error *err) {
    return net_resolver_lookup_host(NULL, a, context_background(), host, err);
}

Slice net_lookup_ip(Alloc *a, Str host, Error *err) {
    Arena sar;
    arena_init(&sar, NULL, 0);
    Error e = BURROW_NO_ERROR;
    Slice addrs = lk_lookup_ip_addr(&lk_default, arena_allocator(&sar),
                                    context_background(), BURROW_S("ip"), host, &e);
    Slice out = slice_nil(TYPE_NET_IP);
    if (BURROW_OK(e)) {
        out = slice_make(a, TYPE_NET_IP, 0, addrs.len);
        for (Int i = 0; i < addrs.len; i++) {
            NetIP ip = lk_ip_clone(a, ((const NetIPAddr *)addrs.p)[i].ip);
            out = slice_append(a, out, &ip, 1);
            if (ip.p == NULL || out.p == NULL) {
                e = burrow_err_out_of_memory;
                out = slice_nil(TYPE_NET_IP);
                break;
            }
        }
    }
    arena_free(&sar);
    BURROW_OUT(err, e);
    return out;
}

Int net_lookup_port(Str network, Str service, Error *err) {
    return net_resolver_lookup_port(NULL, context_background(), network, service, err);
}

Str net_lookup_cname(Alloc *a, Str host, Error *err) {
    return net_resolver_lookup_cname(NULL, a, context_background(), host, err);
}

Slice net_lookup_srv(Alloc *a, Str service, Str proto, Str name, Str *cname,
                     Error *err) {
    return net_resolver_lookup_srv(NULL, a, context_background(), service, proto, name,
                                   cname, err);
}

Slice net_lookup_mx(Alloc *a, Str name, Error *err) {
    return net_resolver_lookup_mx(NULL, a, context_background(), name, err);
}

Slice net_lookup_ns(Alloc *a, Str name, Error *err) {
    return net_resolver_lookup_ns(NULL, a, context_background(), name, err);
}

Slice net_lookup_txt(Alloc *a, Str name, Error *err) {
    return net_resolver_lookup_txt(NULL, a, context_background(), name, err);
}

Slice net_lookup_addr(Alloc *a, Str addr, Error *err) {
    return net_resolver_lookup_addr(NULL, a, context_background(), addr, err);
}

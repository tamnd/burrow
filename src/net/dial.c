/* Dialer, ListenConfig, and the Resolve functions for TCP, UDP and IP.
 *
 * Derived from Go's src/net/dial.go, with internetAddrList and the addrList
 * methods from src/net/ipsock.go, ResolveTCPAddr from src/net/tcpsock.go,
 * ResolveUDPAddr from src/net/udpsock.go and ResolveIPAddr from
 * src/net/iprawsock.go.
 * Go source: go1.27.1.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file.
 *
 * Go leaves the addresses a dial resolves to the collector, and the errors
 * that name them point at them. Here they go in the calling goroutine's error
 * arena, after a mark that a dial which works releases again, so that a
 * failed dial's error can name them for as long as it lives and a good one
 * leaves nothing behind. */

#include "internal.h"

#include "burrow/chan.h"
#include "burrow/context.h"
#include "burrow/error.h"
#include "burrow/func.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/net.h"
#include "burrow/net/netip.h"
#include "burrow/proc.h"
#include "burrow/slice.h"
#include "burrow/sync.h"
#include "burrow/time.h"
#include "burrow/type.h"

#include "../os/internal.h"

#include <stdint.h>
#include <string.h>

#define DL_LIT(s) str_from_bytes((const Byte *)(s), (Int)sizeof(s) - 1)

/* partialDeadline's saneMinimum, and the delay a zero FallbackDelay means. */
#define DL_SANE_MINIMUM (2 * TIME_SECOND)
#define DL_FALLBACK_DELAY (300 * TIME_MILLISECOND)

static const NetAddr dl_nil_addr = {NULL, NULL};

static bool dl_is_oom(Error e) {
    return e.vt == burrow_err_out_of_memory.vt &&
           e.data == burrow_err_out_of_memory.data;
}

static bool dl_is(Str s, const char *lit) {
    return str_eq(s, str_from_cstr(lit));
}

static Error dl_addr_error(Str text, Str addr) {
    NetAddrError ae = {text, addr};
    return net_addr_error_as_error(&ae, error_allocator());
}

/* ----------------------------------------------------------- the addresses
 *
 * An addrList entry, which is a TCPAddr, a UDPAddr, an IPAddr or a UnixAddr,
 * as kind says. */

typedef enum DlKind { DL_TCP, DL_UDP, DL_IP, DL_UNIX } DlKind;

typedef struct DlAddr {
    NetTCPAddr tcp;
    NetUDPAddr udp;
    NetIPAddr ip;
    NetUnixAddr ux;
    DlKind kind;
} DlAddr;

typedef struct DlList {
    DlAddr *p;
    Int n;
} DlList;

static NetAddr dl_addr(const DlAddr *x) {
    switch (x->kind) {
    case DL_TCP:
        return net_tcp_addr_as_addr(&x->tcp);
    case DL_UDP:
        return net_udp_addr_as_addr(&x->udp);
    case DL_IP:
        return net_ip_addr_as_addr(&x->ip);
    case DL_UNIX:
        return net_unix_addr_as_addr(&x->ux);
    default:
        return dl_nil_addr;
    }
}

static NetIP dl_ip(const DlAddr *x) {
    switch (x->kind) {
    case DL_TCP:
        return x->tcp.ip;
    case DL_UDP:
        return x->udp.ip;
    case DL_IP:
        return x->ip.ip;
    case DL_UNIX:
    default:
        return slice_nil(TYPE_BYTE);
    }
}

static bool dl_ip_is4(NetIP ip) {
    return net_ip_to4(ip).len == 4;
}

/* isIPv4 */
static bool dl_is_ipv4(const DlAddr *x) {
    return x->kind != DL_UNIX && dl_ip_is4(dl_ip(x));
}

/* IP.matchAddrFamily */
static bool dl_match_family(NetIP ip, NetIP x) {
    bool ip16 = ip.len == 4 || ip.len == 16;
    bool x16 = x.len == 4 || x.len == 16;
    return (dl_ip_is4(ip) && dl_ip_is4(x)) ||
           (ip16 && !dl_ip_is4(ip) && x16 && !dl_ip_is4(x));
}

/* isWildcard, for an address with ip, where an empty one counts. */
static bool dl_is_wildcard(NetIP ip) {
    return ip.len == 0 || net_ip_is_unspecified(ip);
}

/* The kind of address the networks Go's internetAddrList knows want. */
static bool dl_inet_kind(Str net, DlKind *kind) {
    if (dl_is(net, "tcp") || dl_is(net, "tcp4") || dl_is(net, "tcp6"))
        *kind = DL_TCP;
    else if (dl_is(net, "udp") || dl_is(net, "udp4") || dl_is(net, "udp6"))
        *kind = DL_UDP;
    else if (dl_is(net, "ip") || dl_is(net, "ip4") || dl_is(net, "ip6"))
        *kind = DL_IP;
    else
        return false;
    return true;
}

static DlAddr dl_inet_addr(DlKind kind, NetIP ip, Int port, Str zone) {
    DlAddr x;
    memset(&x, 0, sizeof x);
    x.kind = kind;
    x.tcp = (NetTCPAddr){ip, port, zone};
    x.udp = (NetUDPAddr){ip, port, zone};
    x.ip = (NetIPAddr){ip, zone};
    return x;
}

static DlAddr *dl_list_make(Alloc *a, Int n) {
    return (DlAddr *)mem_alloc(a, (size_t)n * sizeof(DlAddr), _Alignof(DlAddr));
}

/* internetAddrList: the addresses of addr, made in a, for net, which is one
 * of the tcp, udp and ip networks. */
static Error dl_internet_addr_list(NetResolver *r, Alloc *a, Context ctx, Str net,
                                   Str addr, DlList *out) {
    DlKind kind = DL_TCP;
    if (!dl_inet_kind(net, &kind))
        return net_unknown_network_error(error_allocator(), net);
    Str host = BURROW_STR_EMPTY;
    Int portnum = 0;
    Error e = BURROW_NO_ERROR;
    if (kind == DL_IP) {
        host = addr;
    } else if (addr.len > 0) {
        Str port = BURROW_STR_EMPTY;
        host = net_split_host_port(addr, &port, &e);
        if (BURROW_FAILED(e))
            return e;
        portnum = net_resolver_lookup_port(r, ctx, net, port, &e);
        if (BURROW_FAILED(e))
            return e;
    }
    if (host.len == 0) {
        out->p = dl_list_make(a, 1);
        if (out->p == NULL)
            return burrow_err_out_of_memory;
        out->p[0] = dl_inet_addr(kind, slice_nil(TYPE_BYTE), portnum, BURROW_STR_EMPTY);
        out->n = 1;
        return BURROW_NO_ERROR;
    }

    /* Try as a literal IP address, then as a DNS name. */
    Slice ips = burrow__net_lookup_ip_addr(r, a, ctx, net, host, &e);
    if (BURROW_FAILED(e))
        return e;
    const NetIPAddr *v = (const NetIPAddr *)ips.p;
    Int n = ips.len;
    /* Issue 18806: if the machine has halfway configured IPv6 such that it
     * can bind on "::" (IPv6unspecified) but not connect back to that same
     * address, fall back to dialing 0.0.0.0. */
    bool add_zero = n == 1 && net_ip_equal(v[0].ip, net_ipv6_unspecified);

    Byte last = net.p[net.len - 1];
    out->p = dl_list_make(a, n + 1);
    if (out->p == NULL)
        return burrow_err_out_of_memory;
    out->n = 0;
    for (Int i = 0; i < n + (add_zero ? 1 : 0); i++) {
        NetIPAddr ia = i < n ? v[i] : (NetIPAddr){net_ipv4_zero, BURROW_STR_EMPTY};
        bool is4 = dl_ip_is4(ia.ip);
        if (last == '4' && !is4)
            continue;
        if (last == '6' && (ia.ip.len != 16 || is4))
            continue;
        out->p[out->n++] = dl_inet_addr(kind, ia.ip, portnum, ia.zone);
    }
    if (out->n == 0)
        return dl_addr_error(DL_LIT("no suitable address found"), host);
    return BURROW_NO_ERROR;
}

/* hint's network and text, for the errors about it. */
static Str dl_hint_network(NetAddr hint) {
    return hint.vt->network(hint.data);
}

static Str dl_hint_string(NetAddr hint) {
    return hint.vt->string(hint.data, error_allocator());
}

/* The IP of hint, when it is a TCPAddr, a UDPAddr or an IPAddr, and whether
 * it is one of those. */
static bool dl_hint_ip(NetAddr hint, NetIP *ip) {
    const Type *t = hint.vt->self_type;
    if (t == TYPE_NET_TCP_ADDR)
        *ip = ((const NetTCPAddr *)hint.data)->ip;
    else if (t == TYPE_NET_UDP_ADDR)
        *ip = ((const NetUDPAddr *)hint.data)->ip;
    else if (t == TYPE_NET_IP_ADDR)
        *ip = ((const NetIPAddr *)hint.data)->ip;
    else
        return false;
    return true;
}

/* Resolver.resolveAddrList: the addresses of addr for op, which is "dial" or
 * "listen", made in a. hint is the local address of a dial, which the
 * addresses have to be of the same network and family as. */
static Error dl_resolve_addr_list(NetResolver *r, Alloc *a, Context ctx, Str op,
                                  Str network, Str addr, NetAddr hint, DlList *out) {
    Str afnet = BURROW_STR_EMPTY;
    Int proto = 0;
    Error e = burrow__net_parse_network(network, true, &afnet, &proto);
    if (BURROW_FAILED(e))
        return e;
    bool dial = dl_is(op, "dial");
    if (dial && addr.len == 0)
        return burrow__net_err_missing_address;
    if (dl_is(afnet, "unix") || dl_is(afnet, "unixgram") ||
        dl_is(afnet, "unixpacket")) {
        NetUnixAddr *ua = net_resolve_unix_addr(a, afnet, addr, &e);
        if (BURROW_FAILED(e))
            return e;
        if (dial && hint.vt != NULL &&
            !str_eq(net_unix_addr_network(ua), dl_hint_network(hint)))
            return dl_addr_error(DL_LIT("mismatched local address type"),
                                 dl_hint_string(hint));
        out->p = dl_list_make(a, 1);
        if (out->p == NULL)
            return burrow_err_out_of_memory;
        memset(out->p, 0, sizeof *out->p);
        out->p[0].kind = DL_UNIX;
        out->p[0].ux = *ua;
        out->n = 1;
        return BURROW_NO_ERROR;
    }
    e = dl_internet_addr_list(r, a, ctx, afnet, addr, out);
    if (BURROW_FAILED(e) || !dial || hint.vt == NULL)
        return e;

    NetIP hip = slice_nil(TYPE_BYTE);
    bool wildcard = dl_hint_ip(hint, &hip) && dl_is_wildcard(hip);
    Str hnet = dl_hint_network(hint);
    Int n = 0;
    for (Int i = 0; i < out->n; i++) {
        const DlAddr *x = &out->p[i];
        NetAddr xa = dl_addr(x);
        if (xa.vt == NULL || !str_eq(xa.vt->network(xa.data), hnet))
            return dl_addr_error(DL_LIT("mismatched local address type"),
                                 dl_hint_string(hint));
        if (!wildcard && !dl_is_wildcard(dl_ip(x)) && !dl_match_family(dl_ip(x), hip))
            continue;
        out->p[n++] = *x;
    }
    out->n = n;
    if (n == 0)
        return dl_addr_error(DL_LIT("no suitable address found"), dl_hint_string(hint));
    return BURROW_NO_ERROR;
}

/* addrList.first(isIPv4) */
static const DlAddr *dl_first_ipv4(DlList l) {
    for (Int i = 0; i < l.n; i++) {
        if (dl_is_ipv4(&l.p[i]))
            return &l.p[i];
    }
    return &l.p[0];
}

/* addrList.forResolve: the first IPv6 address for a literal IPv6 address,
 * and the first IPv4 one otherwise. */
static const DlAddr *dl_for_resolve(DlList l, Str network, Str addr) {
    bool want6 = false;
    Byte c = 0;
    if (dl_is(network, "ip"))
        c = ':';
    else if (dl_is(network, "tcp") || dl_is(network, "udp"))
        c = '[';
    for (Int i = 0; c != 0 && i < addr.len; i++) {
        if (addr.p[i] == c)
            want6 = true;
    }
    if (!want6)
        return dl_first_ipv4(l);
    for (Int i = 0; i < l.n; i++) {
        if (!dl_is_ipv4(&l.p[i]))
            return &l.p[i];
    }
    return &l.p[0];
}

/* addrList.partition(isIPv4), into two lists made in a. */
static bool dl_partition(Alloc *a, DlList l, DlList *primaries, DlList *fallbacks) {
    primaries->p = dl_list_make(a, l.n);
    fallbacks->p = dl_list_make(a, l.n);
    if (primaries->p == NULL || fallbacks->p == NULL)
        return false;
    primaries->n = 0;
    fallbacks->n = 0;
    bool primary_label = false;
    for (Int i = 0; i < l.n; i++) {
        bool label = dl_is_ipv4(&l.p[i]);
        if (i == 0 || label == primary_label) {
            primary_label = label;
            primaries->p[primaries->n++] = l.p[i];
        } else {
            fallbacks->p[fallbacks->n++] = l.p[i];
        }
    }
    return true;
}

/* ------------------------------------------------------------ the Resolve
 *
 * ResolveTCPAddr, ResolveUDPAddr and ResolveIPAddr, whose answers are made
 * in the caller's allocator and whose lookups go in the error arena. */

static const DlAddr *dl_resolve(Str network, Str address, Error *err) {
    DlList l = {NULL, 0};
    Error e = dl_internet_addr_list(NULL, error_allocator(), context_background(),
                                    network, address, &l);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return NULL;
    }
    return dl_for_resolve(l, network, address);
}

static burrow__NetInetAddr *dl_resolve_inet(Alloc *a, Str network, Str address,
                                            Error *err) {
    ArenaMark m = error_mark();
    const DlAddr *x = dl_resolve(network, address, err);
    if (x == NULL)
        return NULL;
    burrow__NetInetAddr *out = burrow__net_inet_addr_new(a, x->tcp.ip.p, x->tcp.ip.len,
                                                         x->tcp.port, x->tcp.zone);
    error_release(m);
    BURROW_OUT(err, out != NULL ? BURROW_NO_ERROR : burrow_err_out_of_memory);
    return out;
}

NetTCPAddr *net_resolve_tcp_addr(Alloc *a, Str network, Str address, Error *err) {
    if (network.len == 0) {
        /* A hint wildcard for Go 1.0 undocumented behaviour. */
        network = DL_LIT("tcp");
    } else if (!dl_is(network, "tcp") && !dl_is(network, "tcp4") &&
               !dl_is(network, "tcp6")) {
        BURROW_OUT(err, net_unknown_network_error(error_allocator(), network));
        return NULL;
    }
    return (NetTCPAddr *)(void *)dl_resolve_inet(a, network, address, err);
}

NetUDPAddr *net_resolve_udp_addr(Alloc *a, Str network, Str address, Error *err) {
    if (network.len == 0) {
        network = DL_LIT("udp");
    } else if (!dl_is(network, "udp") && !dl_is(network, "udp4") &&
               !dl_is(network, "udp6")) {
        BURROW_OUT(err, net_unknown_network_error(error_allocator(), network));
        return NULL;
    }
    return (NetUDPAddr *)(void *)dl_resolve_inet(a, network, address, err);
}

NetIPAddr *net_resolve_ip_addr(Alloc *a, Str network, Str host, Error *err) {
    if (network.len == 0)
        network = DL_LIT("ip");
    Str afnet = BURROW_STR_EMPTY;
    Int proto = 0;
    Error e = burrow__net_parse_network(network, false, &afnet, &proto);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return NULL;
    }
    if (!dl_is(afnet, "ip") && !dl_is(afnet, "ip4") && !dl_is(afnet, "ip6")) {
        BURROW_OUT(err, net_unknown_network_error(error_allocator(), network));
        return NULL;
    }
    ArenaMark m = error_mark();
    DlList l = {NULL, 0};
    e = dl_internet_addr_list(NULL, error_allocator(), context_background(), afnet,
                              host, &l);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return NULL;
    }
    const DlAddr *x = dl_for_resolve(l, network, host);
    NetIPAddr *out = burrow__net_ip_addr_new(a, x->ip.ip, x->ip.zone);
    error_release(m);
    BURROW_OUT(err, out != NULL ? BURROW_NO_ERROR : burrow_err_out_of_memory);
    return out;
}

/* ------------------------------------------------------------- the context */

/* now + d, without going past either end. */
static int64_t dl_mono_add(int64_t now, int64_t d) {
    if (d > 0 && now > INT64_MAX - d)
        return INT64_MAX;
    if (d < 0 && now < INT64_MIN - d)
        return INT64_MIN;
    return now + d;
}

/* Dialer.deadline: the earliest of now + timeout, the context's deadline and
 * d->deadline, on the monotonic clock, and false when there is none. */
static bool dl_deadline(const NetDialer *d, Context ctx, int64_t *when) {
    int64_t now = burrow_nanotime();
    bool has = false;
    int64_t earliest = 0;
    /* Including negative, for historical reasons. */
    if (d->timeout != 0) {
        earliest = dl_mono_add(now, d->timeout);
        has = true;
    }
    int64_t c = 0;
    if (context_deadline(ctx, &c) && (!has || c < earliest)) {
        earliest = c;
        has = true;
    }
    if (!time_is_zero(d->deadline)) {
        int64_t t = dl_mono_add(now, time_sub(d->deadline, time_now()));
        if (!has || t < earliest) {
            earliest = t;
            has = true;
        }
    }
    *when = earliest;
    return has;
}

/* What dialCtx makes: the context the dial runs under, the deadline and
 * cancel contexts it may have put on top of the caller's, and the goroutine
 * that watches d->cancel. */
typedef struct DlCtx {
    Context ctx;
    Context deadline_ctx;
    Context cancel_ctx;
    ContextCancelFunc cancel1;
    ContextCancelFunc cancel2;
    Chan *old_cancel;
    SyncWaitGroup wg;
} DlCtx;

static void dl_watch_cancel(void *env) {
    DlCtx *dc = (DlCtx *)env;
    SelectCase cases[2] = {BURROW_RECV(dc->old_cancel, NULL),
                           BURROW_RECV(context_done(dc->cancel_ctx), NULL)};
    if (chan_select(cases, 2) == 0)
        BURROW_CALLF0(dc->cancel2);
    sync_wait_group_done(&dc->wg);
}

/* dialCtx, false when there is no memory for the contexts. */
static bool dl_ctx_begin(const NetDialer *d, Context ctx, DlCtx *dc) {
    memset(dc, 0, sizeof *dc);
    dc->ctx = ctx;
    int64_t when = 0;
    if (dl_deadline(d, ctx, &when)) {
        int64_t parent = 0;
        if (!context_deadline(ctx, &parent) || when < parent) {
            dc->deadline_ctx =
                context_with_deadline(heap_allocator(), ctx, when, &dc->cancel1);
            if (BURROW_CONTEXT_IS_NIL(dc->deadline_ctx))
                return false;
            dc->ctx = dc->deadline_ctx;
        }
    }
    if (d->cancel != NULL) {
        dc->cancel_ctx = context_with_cancel(heap_allocator(), dc->ctx, &dc->cancel2);
        if (BURROW_CONTEXT_IS_NIL(dc->cancel_ctx))
            return false;
        dc->ctx = dc->cancel_ctx;
        dc->old_cancel = d->cancel;
        sync_wait_group_add(&dc->wg, 1);
        if (!go(BURROW_FN(Func, dl_watch_cancel, dc)))
            sync_wait_group_done(&dc->wg);
    }
    return true;
}

/* The deferred cancel of dialCtx, which also waits for the watcher and
 * frees the contexts, the newer one first. */
static void dl_ctx_end(DlCtx *dc) {
    if (!BURROW_CONTEXT_IS_NIL(dc->cancel_ctx)) {
        BURROW_CALLF0(dc->cancel2);
        sync_wait_group_wait(&dc->wg);
        context_release(dc->cancel_ctx);
    }
    if (!BURROW_CONTEXT_IS_NIL(dc->deadline_ctx)) {
        BURROW_CALLF0(dc->cancel1);
        context_release(dc->deadline_ctx);
    }
}

/* ---------------------------------------------------------------- dialing */

/* Dialer.ControlContext, or Dialer.Control without the context. */
static Error dl_control(void *env, Context ctx, Str network, Str address,
                        SyscallRawConn c) {
    const NetDialer *d = (const NetDialer *)env;
    if (d->control_context.f != NULL)
        return BURROW_CALLF(d->control_context, ctx, network, address, c);
    return BURROW_CALLF(d->control, network, address, c);
}

static burrow__NetSysOpts dl_dialer_opts(const NetDialer *d, Context ctx) {
    burrow__NetSysOpts o;
    memset(&o, 0, sizeof o);
    o.ctl.ctx = ctx;
    if (d->control_context.f != NULL || d->control.f != NULL)
        o.ctl.ctrl = (burrow__NetCtrlFn){dl_control, (void *)(uintptr_t)d};
    o.keep_alive = d->keep_alive;
    o.keep_alive_config = d->keep_alive_config;
    o.mptcp = burrow__net_mptcp_dial(d->mptcp_status);
    return o;
}

/* sysDialer: the Dialer, the network and the address, and the lock a
 * parallel dial puts around the allocator. */
typedef struct DlSys {
    const NetDialer *d;
    Str network;
    Str address;
    SyncMutex *alloc_mu;
} DlSys;

static const void *dl_local_as(NetAddr la, const Type *t) {
    return la.vt != NULL && la.vt->self_type == t ? la.data : NULL;
}

/* dialSingle: a connection to ra, made in a. */
static NetConn dl_dial_single(const DlSys *sd, Alloc *a, Context ctx, const DlAddr *ra,
                              Error *err) {
    NetConn c = {NULL, NULL};
    NetAddr la = sd->d->local_addr;
    burrow__NetSysOpts o = dl_dialer_opts(sd->d, ctx);
    o.alloc_mu = sd->alloc_mu;
    Error e = BURROW_NO_ERROR;
    switch (ra->kind) {
    case DL_TCP: {
        NetTCPConn *tc = burrow__net_sys_dial_tcp(
            a, &o, sd->network, (const NetTCPAddr *)dl_local_as(la, TYPE_NET_TCP_ADDR),
            &ra->tcp, &e);
        if (tc != NULL)
            c = net_tcp_conn_as_conn(tc);
        break;
    }
    case DL_UDP: {
        NetUDPConn *uc = burrow__net_sys_dial_udp(
            a, &o, sd->network, (const NetUDPAddr *)dl_local_as(la, TYPE_NET_UDP_ADDR),
            &ra->udp, &e);
        if (uc != NULL)
            c = net_udp_conn_as_conn(uc);
        break;
    }
    case DL_UNIX: {
        NetUnixConn *xc = burrow__net_sys_dial_unix(
            a, &o, sd->network,
            (const NetUnixAddr *)dl_local_as(la, TYPE_NET_UNIX_ADDR), &ra->ux, &e);
        if (xc != NULL)
            c = net_unix_conn_as_conn(xc);
        break;
    }
    case DL_IP: {
        NetIPConn *ic = burrow__net_sys_dial_ip(
            a, &o, sd->network, (const NetIPAddr *)dl_local_as(la, TYPE_NET_IP_ADDR),
            &ra->ip, &e);
        if (ic != NULL)
            c = net_ip_conn_as_conn(ic);
        break;
    }
    default:
        e = dl_addr_error(DL_LIT("unexpected address type"), sd->address);
        break;
    }
    if (BURROW_FAILED(e) && !dl_is_oom(e))
        e = burrow__net_op_error(DL_LIT("dial"), sd->network, la, dl_addr(ra), e);
    BURROW_OUT(err, e);
    return c;
}

Error burrow__net_partial_deadline(int64_t now, int64_t deadline, Int addrs_remaining,
                                   int64_t *out) {
    *out = deadline;
    if (deadline == 0)
        return BURROW_NO_ERROR;
    int64_t remaining = deadline - now;
    if (remaining <= 0) {
        *out = 0;
        return burrow__net_err_timeout;
    }
    /* Tentatively allocate equal time to each remaining address. */
    int64_t timeout = remaining / addrs_remaining;
    /* If the time per address is too short, steal from the end of the list. */
    if (timeout < DL_SANE_MINIMUM)
        timeout = remaining < DL_SANE_MINIMUM ? remaining : DL_SANE_MINIMUM;
    *out = now + timeout;
    return BURROW_NO_ERROR;
}

/* dialSerial: a connection to the first of ras that answers, with the time
 * left split between them, and the first error when none does. */
static NetConn dl_dial_serial(const DlSys *sd, Alloc *a, Context ctx, const DlAddr *ras,
                              Int n, Error *err) {
    NetConn none = {NULL, NULL};
    Error first = BURROW_NO_ERROR;
    for (Int i = 0; i < n; i++) {
        const DlAddr *ra = &ras[i];
        Error ce = context_err(ctx);
        if (BURROW_FAILED(ce)) {
            BURROW_OUT(err, burrow__net_op_error(DL_LIT("dial"), sd->network,
                                                 sd->d->local_addr, dl_addr(ra),
                                                 burrow__net_map_err(ce)));
            return none;
        }

        Context dctx = ctx;
        Context sub = {NULL, NULL};
        ContextCancelFunc cancel = {NULL, NULL};
        int64_t deadline = 0;
        if (context_deadline(ctx, &deadline)) {
            int64_t partial = 0;
            Error pe = burrow__net_partial_deadline(burrow_nanotime(), deadline, n - i,
                                                    &partial);
            if (BURROW_FAILED(pe)) {
                /* Ran out of time. */
                if (BURROW_OK(first))
                    first = burrow__net_op_error(DL_LIT("dial"), sd->network,
                                                 sd->d->local_addr, dl_addr(ra), pe);
                break;
            }
            if (partial < deadline) {
                sub = context_with_deadline(heap_allocator(), ctx, partial, &cancel);
                if (BURROW_CONTEXT_IS_NIL(sub)) {
                    BURROW_OUT(err, burrow_err_out_of_memory);
                    return none;
                }
                dctx = sub;
            }
        }

        Error e = BURROW_NO_ERROR;
        NetConn c = dl_dial_single(sd, a, dctx, ra, &e);
        if (!BURROW_CONTEXT_IS_NIL(sub)) {
            BURROW_CALLF0(cancel);
            context_release(sub);
        }
        if (BURROW_OK(e)) {
            BURROW_OUT(err, BURROW_NO_ERROR);
            return c;
        }
        if (dl_is_oom(e)) {
            BURROW_OUT(err, e);
            return none;
        }
        if (BURROW_OK(first))
            first = e;
    }
    if (BURROW_OK(first))
        first = burrow__net_op_error(DL_LIT("dial"), sd->network, dl_nil_addr,
                                     dl_nil_addr, burrow__net_err_missing_address);
    BURROW_OUT(err, first);
    return none;
}

/* One side of dialParallel's race, which dials its addresses on a goroutine
 * of its own and sends itself on results when it is done. Its error is kept
 * in ar, since the goroutine's own error arena goes with it. */
typedef struct DlRacer {
    const DlSys *sd;
    Alloc *a;
    DlList ras;
    Chan *results;
    Context ctx;
    ContextCancelFunc cancel;
    Arena ar;
    NetConn c;
    Error err;
    bool primary;
    bool started;
    bool done;
} DlRacer;

static void dl_racer_send(DlRacer *r) {
    uintptr_t p = (uintptr_t)r;
    chan_send(r->results, &p);
}

static void dl_racer_go(void *env) {
    DlRacer *r = (DlRacer *)env;
    Error e = BURROW_NO_ERROR;
    r->c = dl_dial_serial(r->sd, r->a, r->ctx, r->ras.p, r->ras.n, &e);
    r->err = BURROW_FAILED(e) ? error_retain(arena_allocator(&r->ar), e) : e;
    dl_racer_send(r);
}

static void dl_racer_start(DlRacer *r, Context parent) {
    r->started = true;
    r->ctx = context_with_cancel(heap_allocator(), parent, &r->cancel);
    if (BURROW_CONTEXT_IS_NIL(r->ctx)) {
        r->err = burrow_err_out_of_memory;
        dl_racer_send(r);
        return;
    }
    /* Without a goroutine, the racer runs here, and the results channel has
     * room for what it sends. */
    if (!go(BURROW_FN(Func, dl_racer_go, r)))
        dl_racer_go(r);
}

/* dialParallel: races the primaries against the fallbacks, which start
 * after the fallback delay or as soon as the primaries fail, and gives the
 * first connection made, or the primaries' error when both fail. The loser
 * is waited for and closed before this returns, so that nothing is left
 * using a. */
static NetConn dl_dial_parallel(DlSys *sd, Alloc *a, Context ctx, DlList primaries,
                                DlList fallbacks, Error *err) {
    if (fallbacks.n == 0)
        return dl_dial_serial(sd, a, ctx, primaries.p, primaries.n, err);

    NetConn none = {NULL, NULL};
    Duration delay =
        sd->d->fallback_delay > 0 ? sd->d->fallback_delay : DL_FALLBACK_DELAY;
    Chan *results = chan_make(heap_allocator(), TYPE_UINTPTR, 2);
    TimeTimer *timer = results != NULL ? time_new_timer(heap_allocator(), delay) : NULL;
    if (timer == NULL) {
        if (results != NULL)
            chan_free(results);
        BURROW_OUT(err, burrow_err_out_of_memory);
        return none;
    }
    SyncMutex mu;
    memset(&mu, 0, sizeof mu);
    sd->alloc_mu = &mu;
    DlRacer racers[2];
    memset(racers, 0, sizeof racers);
    for (int i = 0; i < 2; i++) {
        racers[i].sd = sd;
        racers[i].a = a;
        racers[i].ras = i == 0 ? primaries : fallbacks;
        racers[i].results = results;
        racers[i].primary = i == 0;
        arena_init(&racers[i].ar, NULL, 0);
    }
    DlRacer *primary = &racers[0];
    DlRacer *fallback = &racers[1];

    dl_racer_start(primary, ctx);
    int pending = 1;
    DlRacer *winner = NULL;
    for (;;) {
        Time fired;
        uintptr_t p = 0;
        SelectCase cases[2] = {BURROW_RECV(time_timer_c(timer), &fired),
                               BURROW_RECV(results, &p)};
        if (chan_select(cases, 2) == 0) {
            dl_racer_start(fallback, ctx);
            pending++;
            continue;
        }
        DlRacer *res = (DlRacer *)p;
        res->done = true;
        pending--;
        if (BURROW_OK(res->err)) {
            winner = res;
            break;
        }
        if (primary->done && fallback->done)
            break;
        /* If the timer could be stopped, the fallback has not started, and
         * the primaries just failed, so start it now. */
        if (res->primary && time_timer_stop(timer))
            (void)time_timer_reset(timer, 0, NULL);
    }

    /* The deferred cancels, and then the wait for whatever is still going,
     * which closes a connection the race no longer needs. */
    for (int i = 0; i < 2; i++) {
        if (racers[i].started && !BURROW_CONTEXT_IS_NIL(racers[i].ctx))
            BURROW_CALLF0(racers[i].cancel);
    }
    for (; pending > 0; pending--) {
        uintptr_t p = 0;
        (void)chan_recv(results, &p);
        DlRacer *res = (DlRacer *)p;
        res->done = true;
        if (BURROW_OK(res->err))
            net_conn_free(res->c);
    }
    for (int i = 0; i < 2; i++) {
        if (racers[i].started && !BURROW_CONTEXT_IS_NIL(racers[i].ctx))
            context_release(racers[i].ctx);
    }
    time_timer_free(timer);
    chan_free(results);
    sd->alloc_mu = NULL;

    NetConn c = none;
    Error e = BURROW_NO_ERROR;
    if (winner != NULL)
        c = winner->c;
    else
        e = error_retain(error_allocator(), primary->err);
    arena_free(&racers[0].ar);
    arena_free(&racers[1].ar);
    BURROW_OUT(err, e);
    return c;
}

/* Not const: MSVC wants const objects to have an initializer. */
static NetDialer dl_zero_dialer;

NetConn net_dialer_dial_context(const NetDialer *d, Alloc *a, Context ctx, Str network,
                                Str address, Error *err) {
    NetConn none = {NULL, NULL};
    if (d == NULL)
        d = &dl_zero_dialer;
    DlCtx dc;
    if (!dl_ctx_begin(d, ctx, &dc)) {
        dl_ctx_end(&dc);
        BURROW_OUT(err, burrow_err_out_of_memory);
        return none;
    }
    ArenaMark m = error_mark();
    Alloc *ea = error_allocator();
    DlList addrs = {NULL, 0};
    Error e = dl_resolve_addr_list(d->resolver, ea, dc.ctx, DL_LIT("dial"), network,
                                   address, d->local_addr, &addrs);
    if (BURROW_FAILED(e)) {
        dl_ctx_end(&dc);
        if (!dl_is_oom(e))
            e = burrow__net_op_error(DL_LIT("dial"), network, dl_nil_addr, dl_nil_addr,
                                     e);
        BURROW_OUT(err, e);
        return none;
    }

    DlSys sd = {d, network, address, NULL};
    DlList primaries = addrs;
    DlList fallbacks = {NULL, 0};
    if (d->fallback_delay >= 0 && dl_is(network, "tcp") &&
        !dl_partition(ea, addrs, &primaries, &fallbacks)) {
        dl_ctx_end(&dc);
        BURROW_OUT(err, burrow_err_out_of_memory);
        return none;
    }
    NetConn c = dl_dial_parallel(&sd, a, dc.ctx, primaries, fallbacks, &e);
    dl_ctx_end(&dc);
    if (BURROW_OK(e))
        error_release(m);
    BURROW_OUT(err, e);
    return c;
}

NetConn net_dialer_dial(const NetDialer *d, Alloc *a, Str network, Str address,
                        Error *err) {
    return net_dialer_dial_context(d, a, context_background(), network, address, err);
}

NetConn net_dial(Alloc *a, Str network, Str address, Error *err) {
    return net_dialer_dial_context(NULL, a, context_background(), network, address,
                                   err);
}

NetConn net_dial_timeout(Alloc *a, Str network, Str address, Duration timeout,
                         Error *err) {
    NetDialer d;
    memset(&d, 0, sizeof d);
    d.timeout = timeout;
    return net_dialer_dial_context(&d, a, context_background(), network, address, err);
}

/* Dialer.DialTCP and DialUDP: the AddrPorts as TCPAddrs or UDPAddrs, which
 * TCPAddrFromAddrPort makes even for the zero one, in the error arena, so
 * that the errors can name them. */
NetTCPConn *net_dialer_dial_tcp(const NetDialer *d, Alloc *a, Context ctx, Str network,
                                NetipAddrPort laddr, NetipAddrPort raddr, Error *err) {
    if (d == NULL)
        d = &dl_zero_dialer;
    DlCtx dc;
    ArenaMark m = error_mark();
    NetTCPAddr *la = net_tcp_addr_from_addr_port(error_allocator(), laddr);
    NetTCPAddr *ra = net_tcp_addr_from_addr_port(error_allocator(), raddr);
    if (la == NULL || ra == NULL || !dl_ctx_begin(d, ctx, &dc)) {
        if (la != NULL && ra != NULL)
            dl_ctx_end(&dc);
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    burrow__NetSysOpts o = dl_dialer_opts(d, dc.ctx);
    Error e = BURROW_NO_ERROR;
    NetTCPConn *c = burrow__net_dial_tcp(a, &o, network, la, ra, &e);
    dl_ctx_end(&dc);
    if (c != NULL)
        error_release(m);
    BURROW_OUT(err, e);
    return c;
}

NetUDPConn *net_dialer_dial_udp(const NetDialer *d, Alloc *a, Context ctx, Str network,
                                NetipAddrPort laddr, NetipAddrPort raddr, Error *err) {
    if (d == NULL)
        d = &dl_zero_dialer;
    DlCtx dc;
    ArenaMark m = error_mark();
    NetUDPAddr *la = net_udp_addr_from_addr_port(error_allocator(), laddr);
    NetUDPAddr *ra = net_udp_addr_from_addr_port(error_allocator(), raddr);
    if (la == NULL || ra == NULL || !dl_ctx_begin(d, ctx, &dc)) {
        if (la != NULL && ra != NULL)
            dl_ctx_end(&dc);
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    burrow__NetSysOpts o = dl_dialer_opts(d, dc.ctx);
    Error e = BURROW_NO_ERROR;
    NetUDPConn *c = burrow__net_dial_udp(a, &o, network, la, ra, &e);
    dl_ctx_end(&dc);
    if (c != NULL)
        error_release(m);
    BURROW_OUT(err, e);
    return c;
}

NetUnixConn *net_dialer_dial_unix(const NetDialer *d, Alloc *a, Context ctx,
                                  Str network, const NetUnixAddr *laddr,
                                  const NetUnixAddr *raddr, Error *err) {
    if (d == NULL)
        d = &dl_zero_dialer;
    DlCtx dc;
    if (!dl_ctx_begin(d, ctx, &dc)) {
        dl_ctx_end(&dc);
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    burrow__NetSysOpts o = dl_dialer_opts(d, dc.ctx);
    NetUnixConn *c = burrow__net_dial_unix(a, &o, network, laddr, raddr, err);
    dl_ctx_end(&dc);
    return c;
}

/* ipAddrFromAddr, in the error arena as the TCP and UDP ones are: AsSlice's
 * bytes, none for the zero Addr, and the zone. */
static NetIPAddr *dl_ip_addr_from(NetipAddr ip) {
    Byte bytes[16] = {0};
    Int n = 0;
    if (netip_addr_is4(ip)) {
        NetipAddrAs4Ret b4 = netip_addr_as4(ip);
        memcpy(bytes, b4.a, 4);
        n = 4;
    } else if (netip_addr_is_valid(ip)) {
        NetipAddrAs16Ret b16 = netip_addr_as16(ip);
        memcpy(bytes, b16.a, 16);
        n = 16;
    }
    NetIP s = n > 0 ? slice_from(bytes, n, n, TYPE_BYTE) : (NetIP){0};
    return burrow__net_ip_addr_new(error_allocator(), s, netip_addr_zone(ip));
}

NetIPConn *net_dialer_dial_ip(const NetDialer *d, Alloc *a, Context ctx, Str network,
                              NetipAddr laddr, NetipAddr raddr, Error *err) {
    if (d == NULL)
        d = &dl_zero_dialer;
    DlCtx dc;
    ArenaMark m = error_mark();
    NetIPAddr *la = dl_ip_addr_from(laddr);
    NetIPAddr *ra = dl_ip_addr_from(raddr);
    if (la == NULL || ra == NULL || !dl_ctx_begin(d, ctx, &dc)) {
        if (la != NULL && ra != NULL)
            dl_ctx_end(&dc);
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    burrow__NetSysOpts o = dl_dialer_opts(d, dc.ctx);
    Error e = BURROW_NO_ERROR;
    NetIPConn *c = burrow__net_dial_ip(a, &o, network, la, ra, &e);
    dl_ctx_end(&dc);
    if (c != NULL)
        error_release(m);
    BURROW_OUT(err, e);
    return c;
}

/* ----------------------------------------------------------------- MPTCP */

bool net_dialer_multipath_tcp(const NetDialer *d) {
    return burrow__net_mptcp_dial(d != NULL ? d->mptcp_status : 0);
}

void net_dialer_set_multipath_tcp(NetDialer *d, bool use) {
    if (d != NULL)
        d->mptcp_status = use ? 1 : 2;
}

bool net_listen_config_multipath_tcp(const NetListenConfig *lc) {
    return burrow__net_mptcp_listen(lc != NULL ? lc->mptcp_status : 0);
}

void net_listen_config_set_multipath_tcp(NetListenConfig *lc, bool use) {
    if (lc != NULL)
        lc->mptcp_status = use ? 1 : 2;
}

/* -------------------------------------------------------------- listening */

static Error dl_listen_control(void *env, Context ctx, Str network, Str address,
                               SyscallRawConn c) {
    (void)ctx;
    const NetListenConfig *lc = (const NetListenConfig *)env;
    return BURROW_CALLF(lc->control, network, address, c);
}

/* Not const, for the same reason as dl_zero_dialer. */
static NetListenConfig dl_zero_listen_config;

static burrow__NetSysOpts dl_listen_opts(const NetListenConfig *lc, Context ctx) {
    burrow__NetSysOpts o;
    memset(&o, 0, sizeof o);
    o.ctl.ctx = ctx;
    if (lc->control.f != NULL)
        o.ctl.ctrl = (burrow__NetCtrlFn){dl_listen_control, (void *)(uintptr_t)lc};
    o.keep_alive = lc->keep_alive;
    o.keep_alive_config = lc->keep_alive_config;
    o.mptcp = burrow__net_mptcp_listen(lc->mptcp_status);
    return o;
}

NetListener net_listen_config_listen(const NetListenConfig *lc, Alloc *a, Context ctx,
                                     Str network, Str address, Error *err) {
    NetListener none = {NULL, NULL};
    if (lc == NULL)
        lc = &dl_zero_listen_config;
    ArenaMark m = error_mark();
    DlList addrs = {NULL, 0};
    Error e = dl_resolve_addr_list(NULL, error_allocator(), ctx, DL_LIT("listen"),
                                   network, address, dl_nil_addr, &addrs);
    if (BURROW_FAILED(e)) {
        if (!dl_is_oom(e))
            e = burrow__net_op_error(DL_LIT("listen"), network, dl_nil_addr,
                                     dl_nil_addr, e);
        BURROW_OUT(err, e);
        return none;
    }
    burrow__NetSysOpts o = dl_listen_opts(lc, ctx);
    const DlAddr *la = dl_first_ipv4(addrs);
    NetListener l = none;
    switch (la->kind) {
    case DL_TCP: {
        NetTCPListener *tl = burrow__net_sys_listen_tcp(a, &o, network, &la->tcp, &e);
        if (tl != NULL)
            l = net_tcp_listener_as_listener(tl);
        break;
    }
    case DL_UNIX: {
        NetUnixListener *xl = burrow__net_sys_listen_unix(a, &o, network, &la->ux, &e);
        if (xl != NULL)
            l = net_unix_listener_as_listener(xl);
        break;
    }
    case DL_UDP:
    case DL_IP:
    default:
        e = dl_addr_error(DL_LIT("unexpected address type"), address);
        break;
    }
    if (BURROW_FAILED(e)) {
        if (!dl_is_oom(e))
            e = burrow__net_op_error(DL_LIT("listen"), network, dl_nil_addr,
                                     dl_addr(la), e);
        BURROW_OUT(err, e);
        return none;
    }
    error_release(m);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return l;
}

NetListener net_listen(Alloc *a, Str network, Str address, Error *err) {
    return net_listen_config_listen(NULL, a, context_background(), network, address,
                                    err);
}

NetPacketConn net_listen_config_listen_packet(const NetListenConfig *lc, Alloc *a,
                                              Context ctx, Str network, Str address,
                                              Error *err) {
    NetPacketConn none = {NULL, NULL};
    if (lc == NULL)
        lc = &dl_zero_listen_config;
    ArenaMark m = error_mark();
    DlList addrs = {NULL, 0};
    Error e = dl_resolve_addr_list(NULL, error_allocator(), ctx, DL_LIT("listen"),
                                   network, address, dl_nil_addr, &addrs);
    if (BURROW_FAILED(e)) {
        if (!dl_is_oom(e))
            e = burrow__net_op_error(DL_LIT("listen"), network, dl_nil_addr,
                                     dl_nil_addr, e);
        BURROW_OUT(err, e);
        return none;
    }
    burrow__NetSysOpts o = dl_listen_opts(lc, ctx);
    const DlAddr *la = dl_first_ipv4(addrs);
    NetPacketConn c = none;
    switch (la->kind) {
    case DL_UDP: {
        NetUDPConn *uc = burrow__net_sys_listen_udp(a, &o, network, &la->udp, &e);
        if (uc != NULL)
            c = net_udp_conn_as_packet_conn(uc);
        break;
    }
    case DL_UNIX: {
        NetUnixConn *xc = burrow__net_sys_listen_unixgram(a, &o, network, &la->ux, &e);
        if (xc != NULL)
            c = net_unix_conn_as_packet_conn(xc);
        break;
    }
    case DL_IP: {
        NetIPConn *ic = burrow__net_sys_listen_ip(a, &o, network, &la->ip, &e);
        if (ic != NULL)
            c = net_ip_conn_as_packet_conn(ic);
        break;
    }
    case DL_TCP:
    default:
        e = dl_addr_error(DL_LIT("unexpected address type"), address);
        break;
    }
    if (BURROW_FAILED(e)) {
        if (!dl_is_oom(e))
            e = burrow__net_op_error(DL_LIT("listen"), network, dl_nil_addr,
                                     dl_addr(la), e);
        BURROW_OUT(err, e);
        return none;
    }
    error_release(m);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return c;
}

NetPacketConn net_listen_packet(Alloc *a, Str network, Str address, Error *err) {
    return net_listen_config_listen_packet(NULL, a, context_background(), network,
                                           address, err);
}

/* ------------------------------------------------------------ the freeing */

void net_conn_free(NetConn c) {
    if (c.vt == NULL)
        return;
    NetTCPConn *tc = net_conn_as_tcp_conn(c);
    NetUDPConn *uc = net_conn_as_udp_conn(c);
    NetUnixConn *xc = net_conn_as_unix_conn(c);
    NetIPConn *ic = net_conn_as_ip_conn(c);
    if (tc != NULL)
        net_tcp_conn_free(tc);
    else if (uc != NULL)
        net_udp_conn_free(uc);
    else if (xc != NULL)
        net_unix_conn_free(xc);
    else if (ic != NULL)
        net_ip_conn_free(ic);
    else if (burrow__net_is_pipe(c))
        net_pipe_free(c);
    else
        (void)c.vt->closer.close(c.data);
}

void net_listener_free(NetListener l) {
    if (l.vt == NULL)
        return;
    NetTCPListener *tl = net_listener_as_tcp_listener(l);
    NetUnixListener *xl = net_listener_as_unix_listener(l);
    if (tl != NULL)
        net_tcp_listener_free(tl);
    else if (xl != NULL)
        net_unix_listener_free(xl);
    else
        (void)l.vt->closer.close(l.data);
}

void net_packet_conn_free(NetPacketConn c) {
    if (c.vt == NULL)
        return;
    NetUDPConn *uc = net_packet_conn_as_udp_conn(c);
    NetUnixConn *xc = net_packet_conn_as_unix_conn(c);
    NetIPConn *ic = net_packet_conn_as_ip_conn(c);
    if (uc != NULL)
        net_udp_conn_free(uc);
    else if (xc != NULL)
        net_unix_conn_free(xc);
    else if (ic != NULL)
        net_ip_conn_free(ic);
    else
        (void)c.vt->closer.close(c.data);
}

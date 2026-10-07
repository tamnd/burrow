/* The system's resolver: getaddrinfo, getnameinfo and res_search.
 *
 * Derived from Go's src/net/cgo_unix.go, with acquireThread and releaseThread
 * from src/net/net.go, concurrentThreadsLimit from src/net/rlimit_unix.go and
 * parseCNAMEFromResources from src/net/lookup.go.
 * Go source: go1.27.1.
 *
 * Go makes these calls through cgo, and a cgo call hands the goroutine's P to
 * another thread while the C library blocks. burrow has no such handoff, so a
 * blocking call made on the goroutine's own thread would stop every goroutine
 * queued behind it. The calls run on threads kept here for the purpose instead,
 * and the goroutine waits on a channel, which parks it the way a cgo call
 * would. A thread stays around for a while after its call in case another one
 * comes, and goes away when none does. How many calls run at once is capped
 * the way Go caps it, by a channel with one slot per call.
 *
 * Go names IPv6 zones after their interfaces through a cache of the
 * interface list. burrow does not have that list yet, so a zone is its index
 * as a number, which is also what Go falls back to for an index it cannot
 * name.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "internal.h"

#include "../xnet/dnsmessage.h"

#include "burrow/chan.h"
#include "burrow/context.h"
#include "burrow/error.h"
#include "burrow/func.h"
#include "burrow/lock.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/net.h"
#include "burrow/net/netip.h"
#include "burrow/note.h"
#include "burrow/pal.h"
#include "burrow/platform.h"
#include "burrow/slice.h"
#include "burrow/sync.h"
#include "burrow/sync/atomic.h"
#include "burrow/syscall.h"
#include "burrow/thread.h"
#include "burrow/type.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* cgoAddrInfoFlags. getaddrinfo on macOS and the BSDs refuses a flag outside
 * AI_MASK, and the platform layer masks with it there as Go does. */
#if defined(BURROW_OS_ANDROID) || defined(BURROW_OS_AIX) ||                            \
    defined(BURROW_OS_NETBSD) || defined(BURROW_OS_OPENBSD)
#define CG_AI_FLAGS PAL_AI_CANONNAME
#elif defined(BURROW_OS_LINUX) || defined(BURROW_OS_SOLARIS) ||                        \
    defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS) ||                             \
    defined(BURROW_OS_FREEBSD) || defined(BURROW_OS_DRAGONFLY)
#define CG_AI_FLAGS (PAL_AI_CANONNAME | PAL_AI_V4MAPPED | PAL_AI_ALL)
#else
#define CG_AI_FLAGS PAL_AI_CANONNAME
#endif

/* nameinfoLen and maxNameinfoLen. */
#define CG_NAMEINFO_LEN 64
#define CG_MAX_NAMEINFO_LEN 4096

/* maxDNSPacketSize, the first buffer res_search gets. */
#define CG_MAX_DNS_PACKET 1232

/* How long a thread with nothing to do waits for another call before it
 * exits. */
#define CG_IDLE_NS (30 * (int64_t)1000000000)

/* ------------------------------------------------------- the thread limit */

/* threadLimit and threadOnce. */
typedef struct CgThreads {
    SyncOnce once;
    Chan *limit;
} CgThreads;

static CgThreads cg_threads;

/* concurrentThreadsLimit */
static Int cg_threads_limit(void) {
#if defined(BURROW_OS_LINUX) || defined(BURROW_OS_COSMO) ||                            \
    defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS) || defined(BURROW_OS_FREEBSD)
    SyscallRlimit rlim;
    memset(&rlim, 0, sizeof rlim);
    if (BURROW_FAILED(syscall_getrlimit(SYSCALL_RLIMIT_NOFILE, &rlim)))
        return 500;
    uint64_t r = rlim.cur;
    if (r > 500)
        r = 500;
    else if (r > 30)
        r -= 30;
    return (Int)r;
#else
    return 500;
#endif
}

static void cg_threads_init(void *env) {
    (void)env;
    cg_threads.limit = chan_make(heap_allocator(), TYPE_UINTPTR, cg_threads_limit());
}

/* acquireThread */
static Error cg_acquire(Context ctx) {
    sync_once_do(&cg_threads.once, BURROW_FN(Func, cg_threads_init, NULL));
    if (cg_threads.limit == NULL)
        return burrow_err_out_of_memory;
    uintptr_t v = 0;
    Chan *cd = context_done(ctx);
    if (cd == NULL) {
        chan_send(cg_threads.limit, &v);
        return BURROW_NO_ERROR;
    }
    SelectCase cases[2] = {BURROW_SEND(cg_threads.limit, &v), BURROW_RECV(cd, NULL)};
    if (chan_select(cases, 2) == 0)
        return BURROW_NO_ERROR;
    return context_err(ctx);
}

/* releaseThread. There is always a slot to take back, since the call giving
 * it back is the one that filled it. */
static void cg_release(void) {
    (void)chan_try_recv(cg_threads.limit, NULL, NULL);
}

/* ------------------------------------------------------------------ calls */

typedef enum { CG_ADDRINFO, CG_NAMEINFO, CG_RES } CgKind;

/* One call and its answer. The goroutine that asked holds one reference and
 * the thread making the call holds the other, because the goroutine can give
 * up on the call and go while the thread is still inside it. */
typedef struct CgJob {
    Chan *done;
    char *host;
    char *service;
    PalAddrInfo *ai;
    uint8_t *buf;
    int64_t ai_cap;
    int64_t buf_cap;
    /* The count getaddrinfo gave, whether getnameinfo worked, or the reply
     * length res_search gave. */
    int64_t n;
    PalSockAddr sa;
    PalLookupError gerr;
    PalErrno perr;
    int32_t refs;
    int32_t kind;
    int32_t family;
    int32_t socktype;
    int32_t protocol;
    int32_t rclass;
    int32_t rtype;
    bool oom;
} CgJob;

static void cg_free_cstr(char *s) {
    if (s != NULL)
        mem_free(heap_allocator(), s, strlen(s) + 1, 1);
}

static void cg_job_put(CgJob *j) {
    if (sync_atomic_add_int32(&j->refs, -1) != 0)
        return;
    Alloc *h = heap_allocator();
    chan_free(j->done);
    cg_free_cstr(j->host);
    cg_free_cstr(j->service);
    if (j->ai != NULL)
        mem_free(h, j->ai, (size_t)j->ai_cap * sizeof(PalAddrInfo),
                 _Alignof(PalAddrInfo));
    if (j->buf != NULL)
        mem_free(h, j->buf, (size_t)j->buf_cap, 1);
    mem_free(h, j, sizeof(CgJob), _Alignof(CgJob));
}

/* s with a NUL on the end, or NULL with *bad set when s has a NUL in it,
 * which is what syscall.ByteSliceFromString refuses. */
static char *cg_cstr(Str s, bool *bad) {
    if (s.len > 0 && memchr(s.p, 0, (size_t)s.len) != NULL) {
        *bad = true;
        return NULL;
    }
    char *c = (char *)mem_alloc_nozero(heap_allocator(), (size_t)s.len + 1, 1);
    if (c == NULL)
        return NULL;
    if (s.len > 0)
        memcpy(c, s.p, (size_t)s.len);
    c[s.len] = 0;
    return c;
}

/* A new call of kind, with the goroutine's reference, or NULL when there is
 * no memory for it. */
static CgJob *cg_job_new(int32_t kind) {
    CgJob *j = (CgJob *)mem_alloc(heap_allocator(), sizeof(CgJob), _Alignof(CgJob));
    if (j == NULL)
        return NULL;
    memset(j, 0, sizeof *j);
    j->kind = kind;
    j->refs = 1;
    j->done = chan_make(heap_allocator(), TYPE_UINTPTR, 1);
    if (j->done == NULL) {
        mem_free(heap_allocator(), j, sizeof(CgJob), _Alignof(CgJob));
        return NULL;
    }
    return j;
}

/* Makes the buffer room for cap bytes, which it throws away. */
static bool cg_buf(CgJob *j, int64_t cap) {
    if (j->buf != NULL)
        mem_free(heap_allocator(), j->buf, (size_t)j->buf_cap, 1);
    j->buf_cap = 0;
    j->buf = (uint8_t *)mem_alloc_nozero(heap_allocator(), (size_t)cap, 1);
    if (j->buf == NULL)
        return false;
    j->buf_cap = cap;
    return true;
}

/* getaddrinfo, asked again with more room for as long as it has more
 * answers than there was room for. */
static void cg_run_addrinfo(CgJob *j) {
    Alloc *h = heap_allocator();
    int64_t cap = 16;
    for (;;) {
        j->ai = (PalAddrInfo *)mem_alloc_nozero(h, (size_t)cap * sizeof(PalAddrInfo),
                                                _Alignof(PalAddrInfo));
        if (j->ai == NULL) {
            j->oom = true;
            return;
        }
        j->ai_cap = cap;
        j->n = pal_getaddrinfo(j->host, j->service, j->family, j->socktype, j->protocol,
                               CG_AI_FLAGS, j->ai, cap, &j->gerr);
        if (j->n <= cap)
            return;
        mem_free(h, j->ai, (size_t)cap * sizeof(PalAddrInfo), _Alignof(PalAddrInfo));
        j->ai = NULL;
        j->ai_cap = 0;
        cap = j->n;
    }
}

/* cgoLookupAddrPTR's loop: twice the room each time the name does not fit,
 * up to maxNameinfoLen. */
static void cg_run_nameinfo(CgJob *j) {
    j->n = -1;
    for (int64_t l = CG_NAMEINFO_LEN; l <= CG_MAX_NAMEINFO_LEN; l *= 2) {
        if (!cg_buf(j, l)) {
            j->oom = true;
            return;
        }
        if (pal_getnameinfo(&j->sa, (char *)j->buf, l, &j->gerr)) {
            j->n = 1;
            return;
        }
        if (j->gerr.code != PAL_EAI_OVERFLOW)
            return;
    }
}

/* cgoResSearch's loop: a bigger buffer when the reply did not fit. */
static void cg_run_res(CgJob *j) {
    int64_t size = CG_MAX_DNS_PACKET;
    for (;;) {
        if (!cg_buf(j, size)) {
            j->oom = true;
            return;
        }
        j->n = pal_res_search(j->host, j->rclass, j->rtype, j->buf, size, &j->perr);
        if (j->n <= 0 || j->n > 0xffff || j->n <= size)
            return;
        size = j->n;
    }
}

/* The blocking part of doBlockingWithCtx, on whichever thread runs it. */
static void cg_run(CgJob *j) {
    switch ((CgKind)j->kind) {
    case CG_ADDRINFO:
        cg_run_addrinfo(j);
        break;
    case CG_NAMEINFO:
        cg_run_nameinfo(j);
        break;
    case CG_RES:
        cg_run_res(j);
        break;
    default:
        break;
    }
    uintptr_t v = 1;
    chan_send(j->done, &v);
    cg_release();
    cg_job_put(j);
}

/* ---------------------------------------------------------------- threads */

typedef struct CgWorker {
    struct CgWorker *next;
    CgJob *job;
    burrow__Note note;
} CgWorker;

/* The threads waiting for a call, newest first. */
typedef struct CgPool {
    burrow__Lock mu;
    CgWorker *idle;
} CgPool;

static CgPool cg_pool;

/* Takes w off the idle list if it is still there, which it is not once a
 * caller has picked it. cg_pool.mu is held. */
static bool cg_unidle(CgWorker *w) {
    CgWorker **pp = &cg_pool.idle;
    while (*pp != NULL && *pp != w)
        pp = &(*pp)->next;
    if (*pp != w)
        return false;
    *pp = w->next;
    return true;
}

static void cg_worker_main(void *arg) {
    CgWorker *w = (CgWorker *)arg;
    for (;;) {
        CgJob *j = w->job;
        w->job = NULL;
        cg_run(j);
        burrow__note_clear(&w->note);
        burrow__lock(&cg_pool.mu);
        w->next = cg_pool.idle;
        cg_pool.idle = w;
        burrow__unlock(&cg_pool.mu);
        if (burrow__note_sleep_timeout(&w->note, CG_IDLE_NS))
            continue;
        burrow__lock(&cg_pool.mu);
        bool gone = cg_unidle(w);
        burrow__unlock(&cg_pool.mu);
        if (gone)
            break;
        /* A caller took this thread as the time ran out, and its call is on
         * the way. */
        burrow__note_sleep(&w->note);
    }
    burrow__note_free(&w->note);
    mem_free(heap_allocator(), w, sizeof(CgWorker), _Alignof(CgWorker));
}

/* Hands j to a waiting thread or a new one. False when there is neither, and
 * then the caller runs it. */
static bool cg_submit(CgJob *j) {
    burrow__lock(&cg_pool.mu);
    CgWorker *w = cg_pool.idle;
    if (w != NULL)
        cg_pool.idle = w->next;
    burrow__unlock(&cg_pool.mu);
    if (w != NULL) {
        w->job = j;
        burrow__note_wake(&w->note);
        return true;
    }
    w = (CgWorker *)mem_alloc(heap_allocator(), sizeof(CgWorker), _Alignof(CgWorker));
    if (w == NULL)
        return false;
    memset(w, 0, sizeof *w);
    (void)burrow__note_init_transient(&w->note);
    w->job = j;
    burrow__Thread t;
    if (!burrow__thread_start(&t, cg_worker_main, w, 0)) {
        burrow__note_free(&w->note);
        mem_free(heap_allocator(), w, sizeof(CgWorker), _Alignof(CgWorker));
        return false;
    }
    (void)burrow__thread_detach(&t);
    return true;
}

/* x, y and z one after the other, in a, or empty when a is out of memory. */
static Str cg_cat(Alloc *a, Str x, Str y, Str z) {
    Int n = x.len + y.len + z.len;
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)n + 1, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    if (x.len > 0)
        memcpy(p, x.p, (size_t)x.len);
    if (y.len > 0)
        memcpy(p + x.len, y.p, (size_t)y.len);
    if (z.len > 0)
        memcpy(p + x.len + y.len, z.p, (size_t)z.len);
    return str_from_bytes(p, n);
}

/* newDNSError(err, name, "") */
static Error cg_dns_error(Error err, Str name) {
    return burrow__net_new_dns_error(error_allocator(), err, name, BURROW_STR_EMPTY);
}

/* &DNSError{Err: text, Name: name} */
static Error cg_detail_error(Str text, Str name) {
    NetDNSError e;
    memset(&e, 0, sizeof e);
    e.err = text;
    e.name = name;
    return net_dns_error_as_error(&e, error_allocator());
}

/* doBlockingWithCtx: true once j has run, and false with *err set when the
 * wait ended first. Go makes the call on the caller's own thread when ctx
 * can never be done, and here it goes to another thread either way, for the
 * reason at the top of the file. */
static bool cg_do(Context ctx, Str lookup_name, CgJob *j, Error *err) {
    Error e = cg_acquire(ctx);
    if (BURROW_FAILED(e)) {
        if (e.vt != burrow_err_out_of_memory.vt ||
            e.data != burrow_err_out_of_memory.data)
            e = cg_dns_error(burrow__net_map_err(e), lookup_name);
        *err = e;
        return false;
    }
    sync_atomic_add_int32(&j->refs, 1);
    if (!cg_submit(j))
        cg_run(j);
    Chan *cd = context_done(ctx);
    if (cd == NULL) {
        (void)chan_recv(j->done, NULL);
        return true;
    }
    SelectCase cases[2] = {BURROW_RECV(j->done, NULL), BURROW_RECV(cd, NULL)};
    if (chan_select(cases, 2) == 0)
        return true;
    *err = cg_dns_error(burrow__net_map_err(context_err(ctx)), lookup_name);
    return false;
}

/* addrinfoErrno, as the DNSError newDNSError makes of it: gai_strerror's
 * text, and temporary only for EAI_AGAIN. */
static Error cg_addrinfo_errno(const PalLookupError *g, Str name) {
    NetDNSError e;
    memset(&e, 0, sizeof e);
    e.err = str_from_cstr(g->text);
    e.name = name;
    e.is_temporary = g->code == PAL_EAI_AGAIN;
    return net_dns_error_as_error(&e, error_allocator());
}

/* The errno of an EAI_SYSTEM, which Go takes to be EMFILE when there is
 * none (golang.org/issue/6232). */
static Error cg_system_error(const PalLookupError *g, Str name) {
    PalErrno pe = g->err != PAL_OK ? g->err : PAL_EMFILE;
    Error e = syscall_errno_as_error(syscall_errno_from_pal(pe), error_allocator());
    return cg_dns_error(e, name);
}

/* ipVersion */
static Byte cg_ip_version(Str network) {
    if (network.len == 0)
        return 0;
    Byte n = (Byte)network.p[network.len - 1];
    return n == '4' || n == '6' ? n : 0;
}

/* --------------------------------------------------------------- lookups */

Slice burrow__net_cgo_lookup_ip(Alloc *a, Context ctx, Str network, Str name,
                                Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    Slice out = slice_nil(TYPE_NET_IP_ADDR);
    CgJob *j = cg_job_new(CG_ADDRINFO);
    if (j == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return out;
    }
    bool bad = false;
    j->host = cg_cstr(name, &bad);
    if (j->host == NULL) {
        cg_job_put(j);
        BURROW_OUT(err, bad ? cg_detail_error(BURROW_S("invalid argument"), name)
                            : burrow_err_out_of_memory);
        return out;
    }
    j->socktype = PAL_SOCK_STREAM;
    j->family = PAL_AF_UNSPEC;
    Byte v = cg_ip_version(network);
    if (v == '4')
        j->family = PAL_AF_INET;
    else if (v == '6')
        j->family = PAL_AF_INET6;

    Error e = BURROW_NO_ERROR;
    if (!cg_do(ctx, name, j, &e)) {
        cg_job_put(j);
        BURROW_OUT(err, e);
        return out;
    }
    if (j->oom) {
        e = burrow_err_out_of_memory;
    } else if (j->n < 0) {
        switch (j->gerr.code) {
        case PAL_EAI_SYSTEM:
            e = cg_system_error(&j->gerr, name);
            break;
        case PAL_EAI_NONAME:
        case PAL_EAI_NODATA:
            e = cg_dns_error(burrow__net_err_no_such_host, name);
            break;
        case PAL_EAI_ADDRFAMILY:
#if defined(BURROW_OS_FREEBSD)
            /* FreeBSD 13.2 began answering this for a host with no A record,
             * https://bugs.freebsd.org/bugzilla/show_bug.cgi?id=273912. */
            e = cg_dns_error(burrow__net_err_no_such_host, name);
            break;
#endif
        case PAL_EAI_OTHER:
        case PAL_EAI_AGAIN:
        case PAL_EAI_OVERFLOW:
        case PAL_EAI_SERVICE:
        default:
            e = cg_addrinfo_errno(&j->gerr, name);
            break;
        }
    } else {
        out = slice_make(a, TYPE_NET_IP_ADDR, 0, j->n);
        for (int64_t i = 0; i < j->n; i++) {
            const PalAddrInfo *r = &j->ai[i];
            /* Only SOCK_STREAM was asked for, but check anyhow. */
            if (r->socktype != PAL_SOCK_STREAM)
                continue;
            NetIP ip;
            Int port = 0;
            Str zone;
            burrow__NetInetBytes b;
            if (!burrow__net_inet_from_sockaddr(&r->addr, &ip, &port, &zone, &b))
                continue;
            NetIPAddr x;
            x.ip = slice_make(a, TYPE_BYTE, ip.len, ip.len);
            x.zone = str_clone(a, zone);
            if (x.ip.p == NULL || x.zone.len != zone.len) {
                e = burrow_err_out_of_memory;
                break;
            }
            memcpy(x.ip.p, ip.p, (size_t)ip.len);
            out = slice_append(a, out, &x, 1);
            if (out.p == NULL) {
                e = burrow_err_out_of_memory;
                break;
            }
        }
        if (out.len == 0)
            out = slice_nil(TYPE_NET_IP_ADDR);
    }
    cg_job_put(j);
    if (BURROW_FAILED(e))
        out = slice_nil(TYPE_NET_IP_ADDR);
    BURROW_OUT(err, e);
    return out;
}

Slice burrow__net_cgo_lookup_host(Alloc *a, Context ctx, Str name, Error *err) {
    Arena sar;
    arena_init(&sar, NULL, 0);
    Error e = BURROW_NO_ERROR;
    Slice addrs =
        burrow__net_cgo_lookup_ip(arena_allocator(&sar), ctx, BURROW_S("ip"), name, &e);
    Slice out = slice_nil(TYPE_STRING);
    if (BURROW_OK(e) && addrs.len > 0) {
        out = slice_make(a, TYPE_STRING, 0, addrs.len);
        for (Int i = 0; i < addrs.len && BURROW_OK(e); i++) {
            Str s = net_ip_addr_string(&((const NetIPAddr *)addrs.p)[i], a);
            out = slice_append(a, out, &s, 1);
            if (out.p == NULL || s.len == 0)
                e = burrow_err_out_of_memory;
        }
        if (BURROW_FAILED(e))
            out = slice_nil(TYPE_STRING);
    }
    arena_free(&sar);
    BURROW_OUT(err, e);
    return out;
}

Int burrow__net_cgo_lookup_port(Context ctx, Str network, Str service, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    int32_t socktype = 0;
    int32_t protocol = 0;
    if (str_eq(network, BURROW_S("tcp")) || str_eq(network, BURROW_S("tcp4")) ||
        str_eq(network, BURROW_S("tcp6"))) {
        socktype = PAL_SOCK_STREAM;
        protocol = PAL_IPPROTO_TCP;
    } else if (str_eq(network, BURROW_S("udp")) || str_eq(network, BURROW_S("udp4")) ||
               str_eq(network, BURROW_S("udp6"))) {
        socktype = PAL_SOCK_DGRAM;
        protocol = PAL_IPPROTO_UDP;
    } else if (!str_eq(network, BURROW_S("ip"))) {
        Str name = cg_cat(error_allocator(), network, BURROW_S("/"), service);
        BURROW_OUT(err, cg_detail_error(BURROW_S("unknown network"), name));
        return 0;
    }

    /* network+"/"+service, the name every error has. */
    Arena sar;
    arena_init(&sar, NULL, 0);
    Str lookup_name = cg_cat(arena_allocator(&sar), network, BURROW_S("/"), service);
    Error e = BURROW_NO_ERROR;
    Int port = 0;
    CgJob *j = cg_job_new(CG_ADDRINFO);
    bool bad = false;
    if (j == NULL || lookup_name.len != network.len + 1 + service.len) {
        e = burrow_err_out_of_memory;
        goto out;
    }
    j->socktype = socktype;
    j->protocol = protocol;
    Byte v = cg_ip_version(network);
    j->family = v == '4' ? PAL_AF_INET : v == '6' ? PAL_AF_INET6 : PAL_AF_UNSPEC;
    j->service = cg_cstr(service, &bad);
    if (j->service == NULL) {
        e = bad ? cg_detail_error(BURROW_S("invalid argument"), lookup_name)
                : burrow_err_out_of_memory;
        goto out;
    }
    /* Lowercase the C service name. */
    for (Int i = 0; i < service.len; i++) {
        char c = j->service[i];
        if (c >= 'A' && c <= 'Z')
            j->service[i] = (char)(c + ('a' - 'A'));
    }

    if (!cg_do(ctx, lookup_name, j, &e))
        goto out;
    if (j->oom) {
        e = burrow_err_out_of_memory;
    } else if (j->n < 0) {
        switch (j->gerr.code) {
        case PAL_EAI_SYSTEM:
            e = cg_system_error(&j->gerr, lookup_name);
            break;
        case PAL_EAI_SERVICE:
        case PAL_EAI_NONAME: /* Darwin answers EAI_NONAME. */
            e = cg_dns_error(burrow__net_err_unknown_port, lookup_name);
            break;
        case PAL_EAI_OTHER:
        case PAL_EAI_ADDRFAMILY:
        case PAL_EAI_AGAIN:
        case PAL_EAI_NODATA:
        case PAL_EAI_OVERFLOW:
        default:
            e = cg_addrinfo_errno(&j->gerr, lookup_name);
            break;
        }
    } else {
        int64_t i = 0;
        while (i < j->n && j->ai[i].addr.family != PAL_AF_INET &&
               j->ai[i].addr.family != PAL_AF_INET6)
            i++;
        if (i < j->n)
            port = (Int)j->ai[i].addr.port;
        else
            e = cg_dns_error(burrow__net_err_unknown_port, lookup_name);
    }

out:
    if (j != NULL)
        cg_job_put(j);
    arena_free(&sar);
    BURROW_OUT(err, e);
    return BURROW_OK(e) ? port : 0;
}

Slice burrow__net_cgo_lookup_ptr(Alloc *a, Context ctx, Str addr, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    Slice out = slice_nil(TYPE_STRING);
    Error e = BURROW_NO_ERROR;
    NetipAddr ip = netip_parse_addr(addr, &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, cg_detail_error(BURROW_S("invalid address"), addr));
        return out;
    }
    /* cgoSockaddr: an IPv4 sockaddr for anything To4 takes, and IPv6 with the
     * zone's index for the rest. */
    NetipAddrAs16Ret b16 = netip_addr_as16(ip);
    NetIP ip16 = slice_from(b16.a, 16, 16, TYPE_BYTE);
    bool is4 = netip_addr_is4(ip) || netip_addr_is4_in6(ip);
    PalSockAddr sa;
    e = burrow__net_ip_sockaddr(is4 ? PAL_AF_INET : PAL_AF_INET6, ip16, 0,
                                netip_addr_zone(ip), &sa);
    if (BURROW_FAILED(e)) {
        Arena sar;
        arena_init(&sar, NULL, 0);
        Str text =
            cg_cat(arena_allocator(&sar), BURROW_S("invalid address "),
                   netip_addr_string(ip, arena_allocator(&sar)), BURROW_STR_EMPTY);
        BURROW_OUT(err, cg_detail_error(text, addr));
        arena_free(&sar);
        return out;
    }

    CgJob *j = cg_job_new(CG_NAMEINFO);
    if (j == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return out;
    }
    j->sa = sa;
    if (!cg_do(ctx, addr, j, &e)) {
        cg_job_put(j);
        BURROW_OUT(err, e);
        return out;
    }
    if (j->oom) {
        e = burrow_err_out_of_memory;
    } else if (j->n < 0) {
        switch (j->gerr.code) {
        case PAL_EAI_SYSTEM:
            e = cg_system_error(&j->gerr, addr);
            break;
        case PAL_EAI_NONAME:
            e = cg_dns_error(burrow__net_err_no_such_host, addr);
            break;
        case PAL_EAI_OTHER:
        case PAL_EAI_ADDRFAMILY:
        case PAL_EAI_AGAIN:
        case PAL_EAI_NODATA:
        case PAL_EAI_OVERFLOW:
        case PAL_EAI_SERVICE:
        default:
            e = cg_addrinfo_errno(&j->gerr, addr);
            break;
        }
    } else {
        const char *host = (const char *)j->buf;
        Str name =
            str_from_bytes((const Byte *)host, (Int)strnlen(host, (size_t)j->buf_cap));
        Str s = str_clone(a, burrow__net_abs_domain_name(a, name));
        out = slice_make(a, TYPE_STRING, 0, 1);
        out = slice_append(a, out, &s, 1);
        if (out.p == NULL || (s.len == 0 && name.len > 0)) {
            e = burrow_err_out_of_memory;
            out = slice_nil(TYPE_STRING);
        }
    }
    cg_job_put(j);
    BURROW_OUT(err, e);
    return out;
}

/* parseCNAMEFromResources, on the answers of msg. Go parses every answer
 * before it looks at the first, and a reply none of whose answers parse is an
 * error either way. */
static Str cg_parse_cname(Alloc *a, Slice msg, Error *err) {
    DnsmsgParser p;
    DnsmsgHeader h;
    Error e = burrow__dnsmsg_parser_start(&p, msg, &h);
    if (BURROW_OK(e))
        e = burrow__dnsmsg_parser_skip_all_questions(&p);
    if (BURROW_FAILED(e)) {
        *err = e;
        return BURROW_STR_EMPTY;
    }
    DnsmsgResourceHeader rh;
    e = burrow__dnsmsg_parser_answer_header(&p, &rh);
    if (e.vt == burrow__dnsmsg_err_section_done.vt &&
        e.data == burrow__dnsmsg_err_section_done.data) {
        *err = errors_new(error_allocator(), BURROW_S("no CNAME record received"));
        return BURROW_STR_EMPTY;
    }
    if (BURROW_FAILED(e)) {
        *err = e;
        return BURROW_STR_EMPTY;
    }
    Str cname = BURROW_STR_EMPTY;
    bool is_cname = rh.type == DNSMSG_TYPE_CNAME;
    if (is_cname) {
        DnsmsgCNAMEResource c;
        e = burrow__dnsmsg_parser_cname_resource(&p, &c);
        if (BURROW_OK(e))
            cname = str_clone(a, burrow__dnsmsg_name_string(&c.cname));
    } else {
        e = burrow__dnsmsg_parser_skip_answer(&p);
    }
    /* AllAnswers: the rest have to parse too. */
    while (BURROW_OK(e)) {
        e = burrow__dnsmsg_parser_answer_header(&p, &rh);
        if (e.vt == burrow__dnsmsg_err_section_done.vt &&
            e.data == burrow__dnsmsg_err_section_done.data) {
            e = BURROW_NO_ERROR;
            break;
        }
        if (BURROW_OK(e))
            e = burrow__dnsmsg_parser_skip_answer(&p);
    }
    if (BURROW_FAILED(e)) {
        *err = e;
        return BURROW_STR_EMPTY;
    }
    if (!is_cname) {
        *err = errors_new(error_allocator(), BURROW_S("could not parse CNAME record"));
        return BURROW_STR_EMPTY;
    }
    return cname;
}

Str burrow__net_cgo_lookup_cname(Alloc *a, Context ctx, Str name, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    CgJob *j = cg_job_new(CG_RES);
    if (j == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return BURROW_STR_EMPTY;
    }
    bool bad = false;
    j->host = cg_cstr(name, &bad);
    if (j->host == NULL) {
        cg_job_put(j);
        BURROW_OUT(err, bad ? syscall_errno_as_error(syscall_errno_from_pal(PAL_EINVAL),
                                                     error_allocator())
                            : burrow_err_out_of_memory);
        return BURROW_STR_EMPTY;
    }
    j->rclass = (int32_t)DNSMSG_CLASS_INET;
    j->rtype = (int32_t)DNSMSG_TYPE_CNAME;
    Error e = BURROW_NO_ERROR;
    Str out = BURROW_STR_EMPTY;
    if (!cg_do(ctx, name, j, &e)) {
        cg_job_put(j);
        BURROW_OUT(err, e);
        return out;
    }
    if (j->oom) {
        e = burrow_err_out_of_memory;
    } else if (j->n == -2) {
        Error why =
            syscall_errno_as_error(syscall_errno_from_pal(j->perr), error_allocator());
        Str text = cg_cat(error_allocator(), BURROW_S("res_ninit failure: "),
                          error_text(why), BURROW_STR_EMPTY);
        e = errors_new(error_allocator(), text);
    } else if (j->n <= 0 || j->n > 0xffff) {
        e = errors_new(error_allocator(), BURROW_S("res_nsearch failure"));
    } else {
        out = cg_parse_cname(a, slice_from(j->buf, j->n, j->n, TYPE_BYTE), &e);
    }
    cg_job_put(j);
    if (BURROW_FAILED(e))
        out = BURROW_STR_EMPTY;
    BURROW_OUT(err, e);
    return out;
}

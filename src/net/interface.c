/* The machine's network interfaces, from Go's interface.go, with the parts of
 * interface_linux.go, interface_bsd.go and interface_windows.go that turn what
 * the system says into Interfaces and Addrs. Asking the system is the PAL's
 * job, in src/pal/iface_*.c.
 *
 * The zone cache is here too. It is what turns an IPv6 zone such as "eth0"
 * into the interface index a sockaddr carries and back, and like Go's it asks
 * for the interface list again when it is more than a minute old or when a
 * lookup misses. The names it hands out are kept for the life of the process,
 * so that an address made from a sockaddr can point at its zone without
 * owning a copy.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/declare.h"
#include "burrow/error.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/net.h"
#include "burrow/os.h"
#include "burrow/pal.h"
#include "burrow/platform.h"
#include "burrow/slice.h"
#include "burrow/sync.h"
#include "burrow/time.h"

#include "../os/internal.h"
#include "internal.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define IF_LIT(s) str_from_bytes((const Byte *)(s), (Int)sizeof(s) - 1)

BURROW_SENTINEL_ERROR(burrow__net_err_invalid_interface, "invalid network interface");
BURROW_SENTINEL_ERROR(burrow__net_err_invalid_interface_index,
                      "invalid network interface index");
BURROW_SENTINEL_ERROR(burrow__net_err_invalid_interface_name,
                      "invalid network interface name");
BURROW_SENTINEL_ERROR(burrow__net_err_no_such_interface, "no such network interface");
BURROW_SENTINEL_ERROR(burrow__net_err_no_such_multicast_interface,
                      "no such multicast network interface");

/* Where an interface whose index is 0 has the addresses of every interface,
 * which is the BSDs, where Go hands the index to the sysctl and 0 there means
 * all of them. Everywhere else Go compares indexes and 0 matches nothing. */
#if defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS) ||                             \
    defined(BURROW_OS_FREEBSD) || defined(BURROW_OS_NETBSD) ||                         \
    defined(BURROW_OS_OPENBSD) || defined(BURROW_OS_DRAGONFLY)
#define IF_ZERO_IS_ALL 1
#else
#define IF_ZERO_IS_ALL 0
#endif

static bool if_is_oom(Error e) {
    return e.vt == burrow_err_out_of_memory.vt &&
           e.data == burrow_err_out_of_memory.data;
}

/* &OpError{Op: "route", Net: "ip+net", Err: err}. */
static Error if_op_error(Error err) {
    if (if_is_oom(err))
        return err;
    NetAddr none = {NULL, NULL};
    return burrow__net_op_error(IF_LIT("route"), IF_LIT("ip+net"), none, none, err);
}

/* What the PAL said went wrong, as Go has it before the OpError goes round
 * it: a SyscallError named for the call, or the bare errno. */
static Error if_pal_error(const PalIfError *pe) {
    if (pe->err == PAL_ENOMEM && pe->call == NULL)
        return burrow_err_out_of_memory;
    Error e = burrow__os_errno(pe->err);
    if (pe->call != NULL)
        e = os_new_syscall_error(error_allocator(), str_from_cstr(pe->call), e);
    return e;
}

/* ------------------------------------------------------------------- flags */

Str net_flags_string(NetFlags f, Alloc *a) {
    static const char *const names[] = {"up",           "broadcast", "loopback",
                                        "pointtopoint", "multicast", "running"};
    Byte buf[64];
    Int n = 0;
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        if ((f & ((NetFlags)1 << i)) == 0)
            continue;
        if (n > 0)
            buf[n++] = '|';
        size_t l = strlen(names[i]);
        memcpy(buf + n, names[i], l);
        n += (Int)l;
    }
    if (n == 0)
        buf[n++] = '0';
    return str_clone(a, str_from_bytes(buf, n));
}

/* ------------------------------------------------------- the PAL's tables */

/* What pal_if_enumerate filled in, in a buffer from the heap. */
typedef struct IfTable {
    PalInterface *p;
    int64_t n;
    int64_t cap;
} IfTable;

static void if_table_free(IfTable *t) {
    if (t->p != NULL)
        mem_free(heap_allocator(), t->p, (size_t)t->cap * sizeof(PalInterface),
                 _Alignof(PalInterface));
    t->p = NULL;
}

/* interfaceTable(index): every interface for 0, or the one with this index. */
static Error if_table(Int index, IfTable *t) {
    memset(t, 0, sizeof *t);
    if (index < 0 || index > INT32_MAX)
        return BURROW_NO_ERROR;
    int64_t cap = 16;
    for (;;) {
        PalInterface *p = (PalInterface *)mem_alloc_nozero(
            heap_allocator(), (size_t)cap * sizeof(PalInterface),
            _Alignof(PalInterface));
        if (p == NULL)
            return burrow_err_out_of_memory;
        PalIfError pe = {NULL, PAL_OK};
        int64_t got = pal_if_enumerate((int32_t)index, p, cap, &pe);
        if (got >= 0 && got <= cap) {
            *t = (IfTable){p, got, cap};
            return BURROW_NO_ERROR;
        }
        mem_free(heap_allocator(), p, (size_t)cap * sizeof(PalInterface),
                 _Alignof(PalInterface));
        if (got < 0)
            return if_pal_error(&pe);
        cap = got + 4;
    }
}

/* The same for the addresses, from pal_if_addrs or pal_if_multicast_addrs. */
typedef struct IfAddrs {
    PalIfAddr *p;
    int64_t n;
    int64_t cap;
} IfAddrs;

typedef int64_t (*IfAddrsFn)(int32_t index, PalIfAddr *out, int64_t cap,
                             PalIfError *err);

static void if_addrs_free(IfAddrs *t) {
    if (t->p != NULL)
        mem_free(heap_allocator(), t->p, (size_t)t->cap * sizeof(PalIfAddr),
                 _Alignof(PalIfAddr));
    t->p = NULL;
}

static Error if_addrs_table(IfAddrsFn fn, int32_t index, IfAddrs *t) {
    memset(t, 0, sizeof *t);
    int64_t cap = 32;
    for (;;) {
        PalIfAddr *p = (PalIfAddr *)mem_alloc_nozero(
            heap_allocator(), (size_t)cap * sizeof(PalIfAddr), _Alignof(PalIfAddr));
        if (p == NULL)
            return burrow_err_out_of_memory;
        PalIfError pe = {NULL, PAL_OK};
        int64_t got = fn(index, p, cap, &pe);
        if (got >= 0 && got <= cap) {
            *t = (IfAddrs){p, got, cap};
            return BURROW_NO_ERROR;
        }
        mem_free(heap_allocator(), p, (size_t)cap * sizeof(PalIfAddr),
                 _Alignof(PalIfAddr));
        if (got < 0)
            return if_pal_error(&pe);
        cap = got + 8;
    }
}

/* ------------------------------------------------------------- interfaces */

/* One PAL record as an Interface whose name and hardware address are in a. */
static bool if_fill(Alloc *a, const PalInterface *p, NetInterface *out) {
    size_t nlen = strlen(p->name);
    size_t hlen = p->hwaddr_len > 0 ? (size_t)p->hwaddr_len : 0;
    memset(out, 0, sizeof *out);
    out->index = (Int)p->index;
    out->mtu = (Int)p->mtu;
    out->flags = (NetFlags)p->flags;
    out->hardware_addr = slice_nil(TYPE_BYTE);
    if (nlen + hlen > 0) {
        Byte *b = (Byte *)mem_alloc_nozero(a, nlen + hlen, 1);
        if (b == NULL)
            return false;
        memcpy(b, p->name, nlen);
        out->name = str_from_bytes(b, (Int)nlen);
        if (hlen > 0) {
            memcpy(b + nlen, p->hwaddr, hlen);
            out->hardware_addr = slice_from(b + nlen, (Int)hlen, (Int)hlen, TYPE_BYTE);
        }
    }
    return true;
}

static NetInterface *if_one(Alloc *a, const PalInterface *p, Error *err) {
    NetInterface *ifi =
        (NetInterface *)mem_alloc_nozero(a, sizeof *ifi, _Alignof(NetInterface));
    if (ifi == NULL || !if_fill(a, p, ifi)) {
        if (ifi != NULL)
            mem_free(a, ifi, sizeof *ifi, _Alignof(NetInterface));
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    return ifi;
}

static void if_zone_update(const IfTable *ift, bool force);

Slice net_interfaces(Alloc *a, Error *err) {
    IfTable t;
    Error e = if_table(0, &t);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, if_op_error(e));
        return slice_nil(TYPE_NET_INTERFACE);
    }
    if (t.n != 0)
        if_zone_update(&t, false);
    Slice out = slice_nil(TYPE_NET_INTERFACE);
    if (t.n > 0) {
        out = slice_make(a, TYPE_NET_INTERFACE, (Int)t.n, (Int)t.n);
        bool ok = out.p != NULL;
        for (int64_t i = 0; ok && i < t.n; i++)
            ok = if_fill(a, &t.p[i], &((NetInterface *)out.p)[i]);
        if (!ok) {
            if_table_free(&t);
            BURROW_OUT(err, burrow_err_out_of_memory);
            return slice_nil(TYPE_NET_INTERFACE);
        }
    }
    if_table_free(&t);
    return out;
}

NetInterface *net_interface_by_index(Alloc *a, Int index, Error *err) {
    if (index <= 0) {
        BURROW_OUT(err, if_op_error(burrow__net_err_invalid_interface_index));
        return NULL;
    }
    IfTable t;
    Error e = if_table(index, &t);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, if_op_error(e));
        return NULL;
    }
    NetInterface *ifi = NULL;
    for (int64_t i = 0; i < t.n; i++) {
        if ((Int)t.p[i].index == index) {
            ifi = if_one(a, &t.p[i], err);
            if_table_free(&t);
            return ifi;
        }
    }
    if_table_free(&t);
    BURROW_OUT(err, if_op_error(burrow__net_err_no_such_interface));
    return NULL;
}

NetInterface *net_interface_by_name(Alloc *a, Str name, Error *err) {
    if (name.len == 0) {
        BURROW_OUT(err, if_op_error(burrow__net_err_invalid_interface_name));
        return NULL;
    }
    IfTable t;
    Error e = if_table(0, &t);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, if_op_error(e));
        return NULL;
    }
    if (t.n != 0)
        if_zone_update(&t, false);
    for (int64_t i = 0; i < t.n; i++) {
        if (str_eq(str_from_cstr(t.p[i].name), name)) {
            NetInterface *ifi = if_one(a, &t.p[i], err);
            if_table_free(&t);
            return ifi;
        }
    }
    if_table_free(&t);
    BURROW_OUT(err, if_op_error(burrow__net_err_no_such_interface));
    return NULL;
}

/* -------------------------------------------------------------- addresses */

/* An IPNet or an IPAddr with its bytes after it. */
typedef struct IfNetBox {
    NetIPNet n;
    Byte ip[16];
    Byte mask[16];
} IfNetBox;

typedef struct IfAddrBox {
    NetIPAddr x;
    Byte ip[16];
} IfAddrBox;

/* The address in its 16 byte form, which is what Go's IPv4 makes of an IPv4
 * one. */
static void if_ip16(int32_t family, const uint8_t *addr, Byte *out) {
    if (family == PAL_AF_INET) {
        memset(out, 0, 10);
        out[10] = 0xff;
        out[11] = 0xff;
        memcpy(out + 12, addr, 4);
    } else {
        memcpy(out, addr, 16);
    }
}

/* One PAL address as an Addr made in a. */
static bool if_addr(Alloc *a, const PalIfAddr *p, NetAddr *out) {
    if (p->kind == PAL_IFA_NET) {
        IfNetBox *b = (IfNetBox *)mem_alloc_nozero(a, sizeof *b, _Alignof(IfNetBox));
        if (b == NULL)
            return false;
        if_ip16(p->family, p->addr, b->ip);
        b->n.ip = slice_from(b->ip, 16, 16, TYPE_BYTE);
        b->n.mask = slice_nil(TYPE_BYTE);
        if (p->mask_len == 4 || p->mask_len == 16) {
            memcpy(b->mask, p->mask, (size_t)p->mask_len);
            b->n.mask = slice_from(b->mask, p->mask_len, p->mask_len, TYPE_BYTE);
        }
        *out = net_ip_net_as_addr(&b->n);
        return true;
    }
    IfAddrBox *b = (IfAddrBox *)mem_alloc_nozero(a, sizeof *b, _Alignof(IfAddrBox));
    if (b == NULL)
        return false;
    if_ip16(p->family, p->addr, b->ip);
    b->x.ip = slice_from(b->ip, 16, 16, TYPE_BYTE);
    b->x.zone = BURROW_STR_EMPTY;
    *out = net_ip_addr_as_addr(&b->x);
    return true;
}

/* interfaceAddrTable or interfaceMulticastAddrTable through the PAL, with
 * the OpError round a failure. */
static Slice if_addr_list(IfAddrsFn fn, int32_t index, Alloc *a, Error *err) {
    IfAddrs t;
    Error e = if_addrs_table(fn, index, &t);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, if_op_error(e));
        return slice_nil(TYPE_NET_ADDR);
    }
    Slice out = slice_nil(TYPE_NET_ADDR);
    if (t.n > 0) {
        out = slice_make(a, TYPE_NET_ADDR, (Int)t.n, (Int)t.n);
        bool ok = out.p != NULL;
        for (int64_t i = 0; ok && i < t.n; i++)
            ok = if_addr(a, &t.p[i], &((NetAddr *)out.p)[i]);
        if (!ok) {
            if_addrs_free(&t);
            BURROW_OUT(err, burrow_err_out_of_memory);
            return slice_nil(TYPE_NET_ADDR);
        }
    }
    if_addrs_free(&t);
    return out;
}

Slice net_interface_addrs(Alloc *a, Error *err) {
    return if_addr_list(pal_if_addrs, 0, a, err);
}

Slice net_interface_addrs_of(const NetInterface *ifi, Alloc *a, Error *err) {
    if (ifi == NULL) {
        BURROW_OUT(err, if_op_error(burrow__net_err_invalid_interface));
        return slice_nil(TYPE_NET_ADDR);
    }
    if (ifi->index < 0 || ifi->index > INT32_MAX ||
        (ifi->index == 0 && !IF_ZERO_IS_ALL))
        return slice_nil(TYPE_NET_ADDR);
    return if_addr_list(pal_if_addrs, (int32_t)ifi->index, a, err);
}

#if defined(BURROW_OS_LINUX)

/* The Linux kernel lists the groups in /proc rather than answering for them
 * over netlink, and Go reads the files, as this does. A file that cannot be
 * read has no groups in it. */
static bool if_proc_groups(Alloc *a, const NetInterface *ifi, Slice *out) {
    if (!burrow__net_parse_proc_net_igmp(a, IF_LIT("/proc/net/igmp"), ifi, out))
        return false;
    return burrow__net_parse_proc_net_igmp6(a, IF_LIT("/proc/net/igmp6"), ifi, out);
}

#endif

Slice net_interface_multicast_addrs(const NetInterface *ifi, Alloc *a, Error *err) {
    if (ifi == NULL) {
        BURROW_OUT(err, if_op_error(burrow__net_err_invalid_interface));
        return slice_nil(TYPE_NET_ADDR);
    }
#if defined(BURROW_OS_LINUX)
    Slice out = slice_nil(TYPE_NET_ADDR);
    if (!if_proc_groups(a, ifi, &out)) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return slice_nil(TYPE_NET_ADDR);
    }
    return out;
#else
    if (ifi->index <= 0 || ifi->index > INT32_MAX)
        return slice_nil(TYPE_NET_ADDR);
    return if_addr_list(pal_if_multicast_addrs, (int32_t)ifi->index, a, err);
#endif
}

/* ------------------------------------------------- /proc/net/igmp and igmp6 */

/* Room for one more Addr in *out, whose cap is grown by doubling. */
static bool if_push(Alloc *a, Slice *out, NetAddr x) {
    if (out->len == out->cap) {
        Int c = out->cap == 0 ? 8 : out->cap * 2;
        Slice n = slice_make(a, TYPE_NET_ADDR, out->len, c);
        if (n.p == NULL)
            return false;
        if (out->len > 0)
            memcpy(n.p, out->p, (size_t)out->len * sizeof(NetAddr));
        *out = n;
    }
    ((NetAddr *)out->p)[out->len++] = x;
    return true;
}

static bool if_push_ip(Alloc *a, Slice *out, int32_t family, const uint8_t *ip) {
    PalIfAddr p;
    memset(&p, 0, sizeof p);
    p.family = family;
    p.kind = PAL_IFA_ADDR;
    memcpy(p.addr, ip, family == PAL_AF_INET ? 4 : 16);
    NetAddr x;
    return if_addr(a, &p, &x) && if_push(a, out, x);
}

static Str if_field(Slice f, Int i) {
    return ((const Str *)f.p)[i];
}

bool burrow__net_parse_proc_net_igmp(Alloc *a, Str path, const NetInterface *ifi,
                                     Slice *out) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *t = arena_allocator(&ar);
    Error e = BURROW_NO_ERROR;
    burrow__NetFile *fd = burrow__net_open(t, path, &e);
    if (fd == NULL) {
        arena_free(&ar);
        return !if_is_oom(e);
    }
    bool ok = true;
    /* The name lives in the arena, since the line it came from does not
     * outlast the next read. */
    Str name = BURROW_STR_EMPTY;
    Str l;
    (void)burrow__net_file_read_line(fd, &l); /* the heading */
    while (ok && burrow__net_file_read_line(fd, &l)) {
        Slice f = burrow__net_split_at_bytes(t, l, IF_LIT(" :\r\t\n"));
        if (f.len < 4)
            continue;
        if (l.p[0] != ' ' && l.p[0] != '\t') {
            /* A new interface. */
            name = str_clone(t, if_field(f, 1));
        } else if (if_field(f, 0).len == 8) {
            if (ifi != NULL && !str_eq(name, ifi->name))
                continue;
            /* The kernel writes the address in its own byte order. */
            Str h = if_field(f, 0);
            uint8_t b[4] = {0, 0, 0, 0};
            for (Int i = 0; i + 1 < h.len; i += 2)
                (void)burrow__net_xtoi2(str_from_bytes(h.p + i, 2), 0, &b[i / 2]);
            uint32_t v;
            memcpy(&v, b, 4);
            uint8_t ip[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8),
                             (uint8_t)v};
            ok = if_push_ip(a, out, PAL_AF_INET, ip);
        }
    }
    burrow__net_file_close(fd, t);
    arena_free(&ar);
    return ok;
}

bool burrow__net_parse_proc_net_igmp6(Alloc *a, Str path, const NetInterface *ifi,
                                      Slice *out) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *t = arena_allocator(&ar);
    Error e = BURROW_NO_ERROR;
    burrow__NetFile *fd = burrow__net_open(t, path, &e);
    if (fd == NULL) {
        arena_free(&ar);
        return !if_is_oom(e);
    }
    bool ok = true;
    /* Go reads every line into the same 16 bytes, so a short address keeps
     * the end of the one before it. */
    uint8_t b[16];
    memset(b, 0, sizeof b);
    Str l;
    while (ok && burrow__net_file_read_line(fd, &l)) {
        Slice f = burrow__net_split_at_bytes(t, l, IF_LIT(" \r\t\n"));
        if (f.len < 6)
            continue;
        if (ifi != NULL && !str_eq(if_field(f, 1), ifi->name))
            continue;
        Str h = if_field(f, 2);
        for (Int i = 0; i + 1 < h.len && i / 2 < 16; i += 2)
            (void)burrow__net_xtoi2(str_from_bytes(h.p + i, 2), 0, &b[i / 2]);
        ok = if_push_ip(a, out, PAL_AF_INET6, b);
    }
    burrow__net_file_close(fd, t);
    arena_free(&ar);
    return ok;
}

/* ------------------------------------------------------------- zone cache */

/* A name the cache has handed out, kept until the process ends. */
typedef struct IfZoneName {
    struct IfZoneName *next;
    Int len;
    Byte bytes[];
} IfZoneName;

typedef struct IfZoneEntry {
    Str name;
    Int index;
} IfZoneEntry;

/* ipv6ZoneCache. Go's two maps are one list here, in the order the system
 * gave the interfaces, which is all that is needed to answer the way the maps
 * do: toIndex keeps the last index a name had and toName the first name an
 * index had. */
typedef struct IfZoneCache {
    SyncRWMutex mu;
    Time last_fetched;
    IfZoneEntry *entries;
    Int n;
    IfZoneName *names;
} IfZoneCache;

static IfZoneCache if_zone_cache;

/* The kept copy of name, made if there is none. Called with the write lock
 * held. Empty when the heap is out of memory. */
static Str if_zone_intern(const char *name) {
    Int len = (Int)strlen(name);
    for (IfZoneName *z = if_zone_cache.names; z != NULL; z = z->next)
        if (z->len == len && memcmp(z->bytes, name, (size_t)len) == 0)
            return str_from_bytes(z->bytes, len);
    IfZoneName *z = (IfZoneName *)mem_alloc_nozero(
        heap_allocator(), sizeof *z + (size_t)len, _Alignof(IfZoneName));
    if (z == NULL)
        return BURROW_STR_EMPTY;
    z->len = len;
    memcpy(z->bytes, name, (size_t)len);
    z->next = if_zone_cache.names;
    if_zone_cache.names = z;
    return str_from_bytes(z->bytes, len);
}

/* update: refills the cache from ift, or from a fresh interface list when ift
 * is NULL, if it is more than a minute old or force is set. Whether it did. */
static bool if_zone_update_from(const IfTable *ift, bool force) {
    IfZoneCache *zc = &if_zone_cache;
    sync_rw_mutex_lock(&zc->mu);
    Time now = time_now();
    if (!force && time_after(zc->last_fetched, time_add(now, -60 * TIME_SECOND))) {
        sync_rw_mutex_unlock(&zc->mu);
        return false;
    }
    zc->last_fetched = now;
    IfTable own = {NULL, 0, 0};
    if (ift == NULL || ift->n == 0) {
        if (BURROW_FAILED(if_table(0, &own))) {
            sync_rw_mutex_unlock(&zc->mu);
            return false;
        }
        ift = &own;
    }
    IfZoneEntry *entries = NULL;
    if (ift->n > 0)
        entries = (IfZoneEntry *)mem_alloc_nozero(heap_allocator(),
                                                  (size_t)ift->n * sizeof(IfZoneEntry),
                                                  _Alignof(IfZoneEntry));
    Int n = 0;
    for (int64_t i = 0; entries != NULL && i < ift->n; i++) {
        if (ift->p[i].name[0] == 0)
            continue;
        Str name = if_zone_intern(ift->p[i].name);
        if (name.len == 0)
            continue;
        entries[n++] = (IfZoneEntry){name, (Int)ift->p[i].index};
    }
    if (zc->entries != NULL)
        mem_free(heap_allocator(), zc->entries, (size_t)zc->n * sizeof(IfZoneEntry),
                 _Alignof(IfZoneEntry));
    /* The array is freed by its size, which is the number of interfaces it
     * was made for, and n is how many of them have names. */
    zc->entries = entries;
    zc->n = entries == NULL ? 0 : (Int)ift->n;
    for (Int i = n; i < zc->n; i++)
        entries[i] = (IfZoneEntry){BURROW_STR_EMPTY, 0};
    if_table_free(&own);
    sync_rw_mutex_unlock(&zc->mu);
    return true;
}

static void if_zone_update(const IfTable *ift, bool force) {
    (void)if_zone_update_from(ift, force);
}

/* toName[index], with the first name the index had. */
static bool if_zone_find_name(Int index, Str *name) {
    IfZoneCache *zc = &if_zone_cache;
    bool ok = false;
    sync_rw_mutex_r_lock(&zc->mu);
    for (Int i = 0; i < zc->n; i++) {
        if (zc->entries[i].name.len > 0 && zc->entries[i].index == index) {
            *name = zc->entries[i].name;
            ok = true;
            break;
        }
    }
    sync_rw_mutex_r_unlock(&zc->mu);
    return ok;
}

/* toIndex[name], with the last index the name had. */
static bool if_zone_find_index(Str name, Int *index) {
    IfZoneCache *zc = &if_zone_cache;
    bool ok = false;
    sync_rw_mutex_r_lock(&zc->mu);
    for (Int i = zc->n; i > 0; i--) {
        if (zc->entries[i - 1].name.len > 0 && str_eq(zc->entries[i - 1].name, name)) {
            *index = zc->entries[i - 1].index;
            ok = true;
            break;
        }
    }
    sync_rw_mutex_r_unlock(&zc->mu);
    return ok;
}

Str burrow__net_zone_name(Int index, Byte *buf) {
    if (index == 0)
        return BURROW_STR_EMPTY;
    Str name = BURROW_STR_EMPTY;
    bool updated = if_zone_update_from(NULL, false);
    bool ok = if_zone_find_name(index, &name);
    if (!ok && !updated) {
        (void)if_zone_update_from(NULL, true);
        ok = if_zone_find_name(index, &name);
    }
    if (ok)
        return name;
    /* The last resort, the number itself. */
    Byte tmp[24];
    Int n = 0;
    uint64_t v = index < 0 ? (uint64_t)-index : (uint64_t)index;
    do {
        tmp[n++] = (Byte)('0' + v % 10);
        v /= 10;
    } while (v != 0);
    if (index < 0)
        tmp[n++] = '-';
    for (Int i = 0; i < n; i++)
        buf[i] = tmp[n - 1 - i];
    return str_from_bytes(buf, n);
}

Int burrow__net_zone_index(Str name) {
    if (name.len == 0)
        return 0;
    Int index = 0;
    bool updated = if_zone_update_from(NULL, false);
    bool ok = if_zone_find_index(name, &index);
    if (!ok && !updated) {
        (void)if_zone_update_from(NULL, true);
        ok = if_zone_find_index(name, &index);
    }
    if (!ok) {
        /* The last resort, a number. */
        Int used = 0;
        (void)burrow__net_dtoi(name, &index, &used);
    }
    return index;
}

void burrow__net_zone_cache_update(bool force) {
    (void)if_zone_update_from(NULL, force);
}

/* ------------------------------------------------------------ descriptors */

static Str if_flags_m_string(NetFlags *self) {
    return net_flags_string(*self, error_allocator());
}

#define IF_SIG_STRING(IN, OUT) OUT(Str)

#define IF_FLAGS_METHODS(M, T) M(T, String, if_flags_m_string, IF_SIG_STRING)

BURROW_METHODS_DEFINE(NetFlags, IF_FLAGS_METHODS);

#define IF_COUNT(arr) (uint16_t)(sizeof(arr) / sizeof((arr)[0]))

const Type burrow_type_NetFlags = {
    BURROW_S_INIT("Flags"),
    BURROW_S_INIT("net"),
    KIND_UINT,
    (uint32_t)sizeof(NetFlags),
    (uint16_t)_Alignof(NetFlags),
    0,
    IF_COUNT(burrow__methods_NetFlags),
    NULL,
    burrow__methods_NetFlags,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

static const Field if_interface_fields[] = {
    {BURROW_S_INIT("Index"),
     {NULL, 0},
     TYPE_INT,
     (uint32_t)offsetof(NetInterface, index)},
    {BURROW_S_INIT("MTU"), {NULL, 0}, TYPE_INT, (uint32_t)offsetof(NetInterface, mtu)},
    {BURROW_S_INIT("Name"),
     {NULL, 0},
     TYPE_STRING,
     (uint32_t)offsetof(NetInterface, name)},
    {BURROW_S_INIT("HardwareAddr"),
     {NULL, 0},
     &burrow_type_NetHardwareAddr,
     (uint32_t)offsetof(NetInterface, hardware_addr)},
    {BURROW_S_INIT("Flags"),
     {NULL, 0},
     &burrow_type_NetFlags,
     (uint32_t)offsetof(NetInterface, flags)},
};

static const Type if_interface_desc = {
    BURROW_S_INIT("Interface"),
    BURROW_S_INIT("net"),
    KIND_STRUCT,
    (uint32_t)sizeof(NetInterface),
    (uint16_t)_Alignof(NetInterface),
    IF_COUNT(if_interface_fields),
    0,
    if_interface_fields,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

const Type *const TYPE_NET_INTERFACE = &if_interface_desc;

static const Type if_addr_desc = {
    BURROW_S_INIT("Addr"),
    BURROW_S_INIT("net"),
    KIND_INTERFACE,
    (uint32_t)sizeof(NetAddr),
    (uint16_t)_Alignof(NetAddr),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

const Type *const TYPE_NET_ADDR = &if_addr_desc;

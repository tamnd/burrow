/* The hosts file, read through a cache.
 *
 * Derived from Go's src/net/hosts.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "internal.h"

#include "burrow/error.h"
#include "burrow/io/fs.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/net.h"
#include "burrow/net/netip.h"
#include "burrow/os.h"
#include "burrow/pal.h"
#include "burrow/slice.h"
#include "burrow/sync.h"
#include "burrow/time.h"

#include <stdint.h>
#include <string.h>

/* How long a reading is trusted before the file is looked at again. */
#define NH_MAX_AGE (5 * TIME_SECOND)

/* Go's byName: the addresses for one name and the first name on the line it
 * first appeared on. */
typedef struct NhName {
    Slice addrs; /* of Str */
    Str canonical;
} NhName;

/* What the last reading found. Everything in it lives in ar, so a new reading
 * builds a new one and frees the old in one go. */
typedef struct NhTable {
    Arena ar;
    Map *by_name; /* lower case absolute name -> index into names */
    Map *by_addr; /* literal address -> index into addrs */
    NhName *names;
    Int nnames;
    Int cap_names;
    Slice *addrs; /* each of Str */
    Int naddrs;
    Int cap_addrs;
} NhTable;

typedef struct NhHosts {
    SyncMutex mu;
    NhTable *t;
    Time expire;
    Str path; /* in the heap */
    Time mtime;
    int64_t size;
    Str override; /* in the heap, set by tests, empty for the system's file */
} NhHosts;

static NhHosts nh_hosts;

/* parseLiteralIP: the address in its usual form, or empty. */
static Str nh_literal_ip(Alloc *a, Str addr) {
    Error err = BURROW_NO_ERROR;
    NetipAddr ip = netip_parse_addr(addr, &err);
    if (BURROW_FAILED(err))
        return BURROW_STR_EMPTY;
    return netip_addr_string(ip, a);
}

static void nh_table_free(NhTable *t) {
    if (t == NULL)
        return;
    arena_free(&t->ar);
    mem_free(heap_allocator(), t, sizeof *t, _Alignof(NhTable));
}

/* Room for one more in an array that lives in an arena, where the old copy
 * is left behind for the arena to take back. */
static void *nh_grow(Alloc *a, void *p, Int n, Int *cap, size_t size, size_t align) {
    if (n < *cap)
        return p;
    Int c = *cap == 0 ? 16 : *cap * 2;
    void *q = mem_alloc_nozero(a, (size_t)c * size, align);
    if (q == NULL)
        return NULL;
    if (n > 0)
        memcpy(q, p, (size_t)n * size);
    *cap = c;
    return q;
}

static bool nh_add(NhTable *t, Str addr, Str name, Str key, Str *canonical,
                   bool first) {
    Alloc *a = arena_allocator(&t->ar);
    if (first)
        *canonical = key;

    Int *ai = (Int *)map_get(t->by_addr, &addr);
    if (ai == NULL) {
        Slice *addrs = (Slice *)nh_grow(a, t->addrs, t->naddrs, &t->cap_addrs,
                                        sizeof(Slice), _Alignof(Slice));
        if (addrs == NULL)
            return false;
        t->addrs = addrs;
        t->addrs[t->naddrs] = slice_nil(TYPE_STRING);
        Int i = t->naddrs++;
        if (!map_set(t->by_addr, &addr, &i))
            return false;
        ai = (Int *)map_get(t->by_addr, &addr);
    }
    t->addrs[*ai] = slice_append(a, t->addrs[*ai], &name, 1);

    Int *ni = (Int *)map_get(t->by_name, &key);
    if (ni != NULL) {
        NhName *e = &t->names[*ni];
        e->addrs = slice_append(a, e->addrs, &addr, 1);
        return true;
    }
    NhName *names = (NhName *)nh_grow(a, t->names, t->nnames, &t->cap_names,
                                      sizeof(NhName), _Alignof(NhName));
    if (names == NULL)
        return false;
    t->names = names;
    NhName *e = &t->names[t->nnames];
    e->addrs = slice_append(a, slice_nil(TYPE_STRING), &addr, 1);
    e->canonical = *canonical;
    Int i = t->nnames++;
    return map_set(t->by_name, &key, &i);
}

/* One line of the file, with any comment already cut off. */
static bool nh_line(NhTable *t, Str line) {
    Alloc *a = arena_allocator(&t->ar);
    Slice f = burrow__net_get_fields(a, line);
    if (f.len < 2)
        return true;
    const Str *fs = (const Str *)f.p;
    Str addr = nh_literal_ip(a, fs[0]);
    if (addr.len == 0)
        return true;
    Str canonical = BURROW_STR_EMPTY;
    for (Int i = 1; i < f.len; i++) {
        Str name = burrow__net_abs_domain_name(a, str_clone(a, fs[i]));
        Str h = str_clone(a, fs[i]);
        burrow__net_lower_ascii_bytes((Byte *)(uintptr_t)h.p, h.len);
        Str key = burrow__net_abs_domain_name(a, h);
        if (!nh_add(t, addr, name, key, &canonical, i == 1))
            return false;
    }
    return true;
}

static NhTable *nh_table_new(void) {
    NhTable *t =
        (NhTable *)mem_alloc(heap_allocator(), sizeof(NhTable), _Alignof(NhTable));
    if (t == NULL)
        return NULL;
    arena_init(&t->ar, NULL, 0);
    Alloc *a = arena_allocator(&t->ar);
    t->by_name = map_make(a, TYPE_STRING, TYPE_INT, 0);
    t->by_addr = map_make(a, TYPE_STRING, TYPE_INT, 0);
    if (t->by_name == NULL || t->by_addr == NULL) {
        nh_table_free(t);
        return NULL;
    }
    return t;
}

/* The file to read, into buf. */
static Str nh_path(char *buf, int64_t cap) {
    if (nh_hosts.override.len > 0)
        return nh_hosts.override;
    PalErrno pe = PAL_OK;
    int64_t n = pal_hosts_path(buf, cap, &pe);
    if (n < 0)
        return BURROW_STR_EMPTY;
    return str_from_bytes(buf, n);
}

static void nh_set_path(Str hp) {
    if (str_eq(nh_hosts.path, hp))
        return;
    Alloc *h = heap_allocator();
    if (nh_hosts.path.len > 0)
        mem_free(h, (void *)(uintptr_t)nh_hosts.path.p, (size_t)nh_hosts.path.len + 1,
                 1);
    nh_hosts.path = BURROW_STR_EMPTY;
    Byte *p = (Byte *)mem_alloc_nozero(h, (size_t)hp.len + 1, 1);
    if (p == NULL)
        return;
    if (hp.len > 0)
        memcpy(p, hp.p, (size_t)hp.len);
    p[hp.len] = 0;
    nh_hosts.path = str_from_bytes(p, hp.len);
}

/* readHosts. Called with the lock held. */
static void nh_read(void) {
    Time now = time_now();
    char pbuf[1024];
    Str hp = nh_path(pbuf, (int64_t)sizeof pbuf);

    if (time_before(now, nh_hosts.expire) && str_eq(nh_hosts.path, hp) &&
        nh_hosts.t != NULL && map_len(nh_hosts.t->by_name) > 0)
        return;

    Time mtime = {0};
    int64_t size = 0;
    {
        Arena sar;
        arena_init(&sar, NULL, 0);
        Error err = BURROW_NO_ERROR;
        FsFileInfo fi = os_stat(arena_allocator(&sar), hp, &err);
        if (BURROW_OK(err)) {
            mtime = fi.vt->mod_time(fi.data);
            size = fi.vt->size(fi.data);
        }
        arena_free(&sar);
        if (BURROW_OK(err) && str_eq(nh_hosts.path, hp) &&
            time_equal(nh_hosts.mtime, mtime) && nh_hosts.size == size) {
            nh_hosts.expire = time_add(now, NH_MAX_AGE);
            return;
        }
    }

    NhTable *t = nh_table_new();
    if (t == NULL)
        return;
    Error err = BURROW_NO_ERROR;
    Alloc *a = arena_allocator(&t->ar);
    burrow__NetFile *file = burrow__net_open(a, hp, &err);
    if (file == NULL && !errors_is(err, fs_err_not_exist) &&
        !errors_is(err, fs_err_permission)) {
        nh_table_free(t);
        return;
    }
    if (file != NULL) {
        Str line;
        bool ok = true;
        while (ok && burrow__net_file_read_line(file, &line)) {
            const Byte *hash = line.len > 0
                                   ? (const Byte *)memchr(line.p, '#', (size_t)line.len)
                                   : NULL;
            if (hash != NULL)
                line.len = (Int)(hash - line.p); /* a comment */
            ok = nh_line(t, line);
        }
        burrow__net_file_close(file, a);
        if (!ok) {
            nh_table_free(t);
            return;
        }
    }
    nh_table_free(nh_hosts.t);
    nh_hosts.t = t;
    nh_hosts.expire = time_add(now, NH_MAX_AGE);
    nh_set_path(hp);
    nh_hosts.mtime = mtime;
    nh_hosts.size = size;
}

/* A copy of a slice of Str, strings and all, in a. */
static Slice nh_copy(Alloc *a, Slice s) {
    Slice out = slice_make(a, TYPE_STRING, 0, s.len);
    const Str *p = (const Str *)s.p;
    for (Int i = 0; i < s.len; i++) {
        Str v = str_clone(a, p[i]);
        out = slice_append(a, out, &v, 1);
    }
    return out;
}

Slice burrow__net_lookup_static_host(Alloc *a, Str host, Str *canonical) {
    *canonical = BURROW_STR_EMPTY;
    Slice out = slice_nil(TYPE_STRING);
    sync_mutex_lock(&nh_hosts.mu);
    nh_read();
    NhTable *t = nh_hosts.t;
    if (t != NULL && map_len(t->by_name) != 0) {
        Arena sar;
        arena_init(&sar, NULL, 0);
        Alloc *s = arena_allocator(&sar);
        if (burrow__net_has_upper_case(host)) {
            host = str_clone(s, host);
            burrow__net_lower_ascii_bytes((Byte *)(uintptr_t)host.p, host.len);
        }
        Str key = burrow__net_abs_domain_name(s, host);
        Int *i = (Int *)map_get(t->by_name, &key);
        if (i != NULL) {
            out = nh_copy(a, t->names[*i].addrs);
            *canonical = str_clone(a, t->names[*i].canonical);
        }
        arena_free(&sar);
    }
    sync_mutex_unlock(&nh_hosts.mu);
    return out;
}

Slice burrow__net_lookup_static_addr(Alloc *a, Str addr) {
    Slice out = slice_nil(TYPE_STRING);
    sync_mutex_lock(&nh_hosts.mu);
    nh_read();
    Arena sar;
    arena_init(&sar, NULL, 0);
    Str lit = nh_literal_ip(arena_allocator(&sar), addr);
    NhTable *t = nh_hosts.t;
    if (lit.len > 0 && t != NULL && map_len(t->by_addr) != 0) {
        Int *i = (Int *)map_get(t->by_addr, &lit);
        if (i != NULL)
            out = nh_copy(a, t->addrs[*i]);
    }
    arena_free(&sar);
    sync_mutex_unlock(&nh_hosts.mu);
    return out;
}

void burrow__net_set_hosts_file_path(Str path) {
    sync_mutex_lock(&nh_hosts.mu);
    Alloc *h = heap_allocator();
    if (nh_hosts.override.len > 0)
        mem_free(h, (void *)(uintptr_t)nh_hosts.override.p,
                 (size_t)nh_hosts.override.len + 1, 1);
    nh_hosts.override = BURROW_STR_EMPTY;
    if (path.len > 0) {
        Byte *p = (Byte *)mem_alloc_nozero(h, (size_t)path.len + 1, 1);
        if (p != NULL) {
            memcpy(p, path.p, (size_t)path.len);
            p[path.len] = 0;
            nh_hosts.override = str_from_bytes(p, path.len);
        }
    }
    sync_mutex_unlock(&nh_hosts.mu);
}

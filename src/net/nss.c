/* nsswitch.conf, which is what says whether a host name is looked for in the
 * hosts file, in DNS or in both and in which order.
 *
 * Derived from Go's src/net/nss.go.
 * Go source: go1.27.1.
 *
 * Copyright 2015 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "internal.h"

#include "burrow/error.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/os.h"
#include "burrow/proc.h"
#include "burrow/slice.h"
#include "burrow/sync.h"
#include "burrow/sync/atomic.h"
#include "burrow/time.h"
#include "burrow/type.h"

#include <stdint.h>
#include <string.h>

#define NW_LIT(s) str_from_bytes((const Byte *)(s), (Int)sizeof(s) - 1)

/* What the Slices below hold. */

BURROW__NET_ELEM_DESC(nw_criterion_type, "nssCriterion", burrow__NssCriterion,
                      0x6e776372U);
BURROW__NET_ELEM_DESC(nw_source_type, "nssSource", burrow__NssSource, 0x6e777372U);
BURROW__NET_ELEM_DESC(nw_database_type, "nssDatabase", burrow__NssDatabase,
                      0x6e776462U);

/* -------------------------------------------------------------- criteria */

bool burrow__nss_source_standard_criteria(const burrow__NssSource *s) {
    const burrow__NssCriterion *c = (const burrow__NssCriterion *)s->criteria.p;
    for (Int i = 0; i < s->criteria.len; i++) {
        /* standardStatusAction */
        bool last = i == s->criteria.len - 1;
        if (c[i].negate)
            return false;
        Str def;
        if (str_eq(c[i].status, NW_LIT("success")))
            def = NW_LIT("return");
        else if (str_eq(c[i].status, NW_LIT("notfound")) ||
                 str_eq(c[i].status, NW_LIT("unavail")) ||
                 str_eq(c[i].status, NW_LIT("tryagain")))
            def = NW_LIT("continue");
        else
            return false; /* an unknown status */
        if (last && str_eq(c[i].action, NW_LIT("return")))
            continue;
        if (!str_eq(c[i].action, def))
            return false;
    }
    return true;
}

/* --------------------------------------------------------------- parsing */

static bool nw_is_space(Byte b) {
    return b == ' ' || b == '\t' || b == '\n' || b == '\r';
}

/* trimSpace */
static Str nw_trim(Str x) {
    while (x.len > 0 && nw_is_space(x.p[0])) {
        x.p++;
        x.len--;
    }
    while (x.len > 0 && nw_is_space(x.p[x.len - 1]))
        x.len--;
    return x;
}

static Int nw_index(Str s, Byte c) {
    const Byte *p = s.len > 0 ? (const Byte *)memchr(s.p, c, (size_t)s.len) : NULL;
    return p == NULL ? -1 : (Int)(p - s.p);
}

static Str nw_slice(Str s, Int from, Int to) {
    return str_from_bytes(s.p + from, to - from);
}

/* One criterion of parseCriteria, appended to c. False when it is not one. */
static bool nw_criterion(Alloc *a, Str f, Slice *c) {
    bool negate = false;
    if (f.len > 0 && f.p[0] == '!') {
        negate = true;
        f = nw_slice(f, 1, f.len);
    }
    if (f.len < 3)
        return false; /* criterion too short */
    Int eq = nw_index(f, '=');
    if (eq == -1)
        return false; /* criterion lacks equal sign */
    f = str_clone(a, f);
    if (f.len > 0 && f.p == NULL)
        return false;
    burrow__net_lower_ascii_bytes((Byte *)(uintptr_t)f.p, f.len);
    burrow__NssCriterion v = {nw_slice(f, 0, eq), nw_slice(f, eq + 1, f.len), negate};
    Slice n = slice_append(a, *c, &v, 1);
    if (n.p == NULL)
        return false;
    *c = n;
    return true;
}

/* parseCriteria, with foreachField. */
static bool nw_parse_criteria(Alloc *a, Str x, Slice *c) {
    *c = slice_nil(&nw_criterion_type);
    x = nw_trim(x);
    while (x.len > 0) {
        Int sp = nw_index(x, ' ');
        if (sp == -1)
            return nw_criterion(a, x, c);
        Str field = nw_trim(nw_slice(x, 0, sp));
        if (field.len > 0 && !nw_criterion(a, field, c))
            return false;
        x = nw_trim(nw_slice(x, sp + 1, x.len));
    }
    return true;
}

static burrow__NssDatabase *nw_database(Alloc *a, burrow__NssConf *conf, Str db) {
    burrow__NssDatabase *d = (burrow__NssDatabase *)conf->dbs.p;
    for (Int i = 0; i < conf->dbs.len; i++)
        if (str_eq(d[i].name, db))
            return &d[i];
    burrow__NssDatabase v = {str_clone(a, db), slice_nil(&nw_source_type)};
    Slice n = slice_append(a, conf->dbs, &v, 1);
    if (n.p == NULL)
        return NULL;
    conf->dbs = n;
    return &((burrow__NssDatabase *)n.p)[n.len - 1];
}

static void nw_fail(burrow__NssConf *conf, Str text) {
    conf->err = errors_new(arena_allocator(&conf->ar), text);
}

/* parseNSSConf */
static void nw_parse(burrow__NssConf *conf, burrow__NetFile *f) {
    Alloc *a = arena_allocator(&conf->ar);
    Str line;
    while (burrow__net_file_read_line(f, &line)) {
        Int hash = nw_index(line, '#'); /* removeComment */
        if (hash >= 0)
            line.len = hash;
        line = nw_trim(line);
        if (line.len == 0)
            continue;
        Int colon = nw_index(line, ':');
        if (colon == -1) {
            nw_fail(conf, NW_LIT("no colon on line"));
            return;
        }
        Str db = nw_trim(nw_slice(line, 0, colon));
        Str srcs = nw_slice(line, colon + 1, line.len);
        for (;;) {
            srcs = nw_trim(srcs);
            if (srcs.len == 0)
                break;
            Int sp = nw_index(srcs, ' ');
            Str src;
            if (sp == -1) {
                src = srcs;
                srcs = BURROW_STR_EMPTY; /* done */
            } else {
                src = nw_slice(srcs, 0, sp);
                srcs = nw_trim(nw_slice(srcs, sp + 1, srcs.len));
            }
            Slice criteria = slice_nil(&nw_criterion_type);
            if (srcs.len > 0 && srcs.p[0] == '[') {
                Int bclose = nw_index(srcs, ']');
                if (bclose == -1) {
                    nw_fail(conf, NW_LIT("unclosed criterion bracket"));
                    return;
                }
                Str inner = nw_slice(srcs, 1, bclose);
                if (!nw_parse_criteria(a, inner, &criteria)) {
                    Arena sar;
                    arena_init(&sar, NULL, 0);
                    nw_fail(conf, burrow__net_cat(arena_allocator(&sar), 2,
                                                  NW_LIT("invalid criteria: "), inner));
                    arena_free(&sar);
                    return;
                }
                srcs = nw_slice(srcs, bclose + 1, srcs.len);
            }
            burrow__NssDatabase *d = nw_database(a, conf, db);
            burrow__NssSource s = {str_clone(a, src), criteria};
            Slice n = d != NULL ? slice_append(a, d->sources, &s, 1) : slice_nil(NULL);
            if (n.p == NULL) {
                conf->err = burrow_err_out_of_memory;
                return;
            }
            d->sources = n;
        }
    }
}

burrow__NssConf *burrow__nss_parse_file(Str file) {
    burrow__NssConf *conf = (burrow__NssConf *)mem_alloc(
        heap_allocator(), sizeof(burrow__NssConf), _Alignof(burrow__NssConf));
    if (conf == NULL)
        return NULL;
    arena_init(&conf->ar, NULL, 0);
    conf->refs = 1;
    conf->dbs = slice_nil(&nw_database_type);
    Alloc *a = arena_allocator(&conf->ar);
    Error err = BURROW_NO_ERROR;
    burrow__NetFile *f = burrow__net_open(a, file, &err);
    if (f == NULL) {
        conf->err = error_retain(a, err);
        return conf;
    }
    FsFileInfo fi = os_file_stat(f->file, a, &err);
    if (BURROW_FAILED(err)) {
        conf->err = error_retain(a, err);
        burrow__net_file_close(f, a);
        return conf;
    }
    Time mtime = fi.vt->mod_time(fi.data);
    nw_parse(conf, f);
    conf->mtime = mtime;
    burrow__net_file_close(f, a);
    return conf;
}

void burrow__nss_conf_put(burrow__NssConf *conf) {
    if (conf == NULL || sync_atomic_add_int32(&conf->refs, -1) != 0)
        return;
    arena_free(&conf->ar);
    mem_free(heap_allocator(), conf, sizeof(burrow__NssConf),
             _Alignof(burrow__NssConf));
}

Slice burrow__nss_conf_sources(const burrow__NssConf *conf, Str db) {
    const burrow__NssDatabase *d = (const burrow__NssDatabase *)conf->dbs.p;
    for (Int i = 0; i < conf->dbs.len; i++)
        if (str_eq(d[i].name, db))
            return d[i].sources;
    return slice_nil(&nw_source_type);
}

/* --------------------------------------------------------- system copy */

/* nssConfig: what /etc/nsswitch.conf said when it was last read, read again
 * when its modification time moves, and looked at no more than every five
 * seconds. sema is Go's one slot channel, which only tryUpdate tries for. */
static struct {
    SyncOnce init_once;
    int32_t sema;
    Time last_checked;
    SyncMutex mu;
    burrow__NssConf *conf;
} nw_config;

static const char nw_path[] = "/etc/nsswitch.conf";

static void nw_init(void *env) {
    (void)env;
    nw_config.conf = burrow__nss_parse_file(NW_LIT(nw_path));
    nw_config.last_checked = time_now();
}

static void nw_swap(burrow__NssConf *conf) {
    sync_mutex_lock(&nw_config.mu);
    burrow__NssConf *old = nw_config.conf;
    nw_config.conf = conf;
    sync_mutex_unlock(&nw_config.mu);
    burrow__nss_conf_put(old);
}

/* tryUpdate */
static void nw_try_update(void) {
    sync_once_do(&nw_config.init_once, BURROW_FN(Func, nw_init, NULL));
    if (!sync_atomic_compare_and_swap_int32(&nw_config.sema, 0, 1))
        return;
    Time now = time_now();
    if (time_after(nw_config.last_checked, time_add(now, -5 * TIME_SECOND))) {
        sync_atomic_store_int32(&nw_config.sema, 0);
        return;
    }
    nw_config.last_checked = now;

    Time mtime = {0};
    {
        Arena sar;
        arena_init(&sar, NULL, 0);
        Error err = BURROW_NO_ERROR;
        FsFileInfo fi = os_stat(arena_allocator(&sar), NW_LIT(nw_path), &err);
        if (BURROW_OK(err))
            mtime = fi.vt->mod_time(fi.data);
        arena_free(&sar);
    }
    /* Only tryUpdate replaces conf and it holds sema, so reading it here
     * without the lock is safe. */
    if (nw_config.conf == NULL || !time_equal(mtime, nw_config.conf->mtime)) {
        burrow__NssConf *conf = burrow__nss_parse_file(NW_LIT(nw_path));
        if (conf != NULL)
            nw_swap(conf);
    }
    sync_atomic_store_int32(&nw_config.sema, 0);
}

burrow__NssConf *burrow__net_system_nss(void) {
    nw_try_update();
    sync_mutex_lock(&nw_config.mu);
    burrow__NssConf *conf = nw_config.conf;
    if (conf != NULL)
        sync_atomic_add_int32(&conf->refs, 1);
    sync_mutex_unlock(&nw_config.mu);
    return conf;
}

void burrow__net_set_system_nss(burrow__NssConf *conf, Duration add_dur) {
    sync_once_do(&nw_config.init_once, BURROW_FN(Func, nw_init, NULL));
    nw_swap(conf);
    /* acquireSema, which waits where tryUpdate would give up. */
    while (!sync_atomic_compare_and_swap_int32(&nw_config.sema, 0, 1))
        runtime_gosched();
    nw_config.last_checked = time_add(time_now(), add_dur);
    sync_atomic_store_int32(&nw_config.sema, 0);
}

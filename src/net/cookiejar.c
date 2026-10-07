/* Derived from Go's src/net/http/cookiejar/jar.go and punycode.go.
 * Go source: go1.27.1.
 *
 * Copyright 2012 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "cookiejar_internal.h"

#include "http_ascii.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/func.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/fixed.h"
#include "burrow/net.h"
#include "burrow/net/http.h"
#include "burrow/net/http/cookiejar.h"
#include "burrow/net/url.h"
#include "burrow/slice.h"
#include "burrow/slices.h"
#include "burrow/strings.h"
#include "burrow/sync.h"
#include "burrow/time.h"
#include "burrow/type.h"
#include "burrow/utf8.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

struct CookiejarJar {
    Alloc *a;
    CookiejarPublicSuffixList ps_list;
    /* mu locks the rest. */
    SyncMutex mu;
    /* The cookies, keyed by their eTLD+1, each a CjSub keyed in turn by
     * domain;path;name. */
    Map *entries;
    /* The sequence number of the next new cookie, so that cookies with the
     * same path and the same creation time still come out in one order. */
    uint64_t next_seq_num;
};

/* A submap of entries, with the key it is under. */
typedef struct CjSub {
    Str key; /* the bytes after the struct */
    Map *m;  /* of CjStored * */
    size_t size;
} CjSub;

/* An entry as the jar keeps it, in one block with its strings after it. */
typedef struct CjStored {
    burrow__CookiejarEntry e;
    Str id;
    size_t size;
} CjStored;

static const Type cj_jar_desc = {
    {(const Byte *)"Jar", 3},
    {(const Byte *)"net/http/cookiejar", 18},
    KIND_STRUCT,
    (uint32_t)sizeof(struct CookiejarJar),
    (uint16_t)_Alignof(struct CookiejarJar),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x636a6a72U, /* "cjjr" */
    NULL,
};

const Type *const TYPE_COOKIEJAR_JAR = &cj_jar_desc;

BURROW_SENTINEL_ERROR(burrow__cookiejar_err_illegal_domain,
                      "cookiejar: illegal cookie domain attribute");
BURROW_SENTINEL_ERROR(burrow__cookiejar_err_malformed_domain,
                      "cookiejar: malformed cookie domain attribute");

#define CJ_LIT(s) BURROW_S(s)

CookiejarJar *cookiejar_new(Alloc *a, const CookiejarOptions *o, Error *err) {
    *err = BURROW_NO_ERROR;
    CookiejarJar *j = BURROW_NEW(a, CookiejarJar);
    if (j == NULL) {
        *err = burrow_err_out_of_memory;
        return NULL;
    }
    memset(j, 0, sizeof *j);
    j->a = a;
    j->entries = map_make(a, TYPE_STRING, TYPE_UNSAFE_POINTER, 0);
    if (j->entries == NULL) {
        mem_free(a, j, sizeof *j, _Alignof(CookiejarJar));
        *err = burrow_err_out_of_memory;
        return NULL;
    }
    if (o != NULL)
        j->ps_list = o->public_suffix_list;
    return j;
}

static void cj_stored_free(Alloc *a, CjStored *s) {
    mem_free(a, s, s->size, _Alignof(CjStored));
}

static void cj_sub_free(Alloc *a, CjSub *sub) {
    void *v;
    for (MapIter it = map_iter(sub->m); map_next(&it, NULL, &v);)
        cj_stored_free(a, *(CjStored **)v);
    map_free(sub->m);
    mem_free(a, sub, sub->size, _Alignof(CjSub));
}

void cookiejar_jar_free(CookiejarJar *j) {
    if (j == NULL)
        return;
    void *v;
    for (MapIter it = map_iter(j->entries); map_next(&it, NULL, &v);)
        cj_sub_free(j->a, *(CjSub **)v);
    map_free(j->entries);
    mem_free(j->a, j, sizeof *j, _Alignof(CookiejarJar));
}

/* ------------------------------------------------------------- the entries */

/* shouldSend and the matches it is made of. */

static bool cj_domain_match(const burrow__CookiejarEntry *e, Str host) {
    if (str_eq(e->domain, host))
        return true;
    return !e->host_only && burrow__cookiejar_has_dot_suffix(host, e->domain);
}

/* path-match, RFC 6265 section 5.1.4. */
static bool cj_path_match(const burrow__CookiejarEntry *e, Str request_path) {
    if (str_eq(request_path, e->path))
        return true;
    if (strings_has_prefix(request_path, e->path)) {
        if (e->path.p[e->path.len - 1] == '/')
            return true; /* "/any/" matches "/any/path" */
        if (request_path.p[e->path.len] == '/')
            return true; /* "/any" matches "/any/path" */
    }
    return false;
}

static bool cj_is_localhost(Str host) {
    host = strings_trim_suffix(host, CJ_LIT("."));
    Int idx = strings_last_index(host, CJ_LIT("."));
    if (idx >= 0)
        host = str_from_bytes(host.p + idx + 1, host.len - idx - 1);
    return burrow__http_ascii_equal_fold(host, CJ_LIT("localhost"));
}

/* Parses s the way net.ParseIP does into b, without the allocator it wants. */
static bool cj_parse_ip(Str s, Byte b[16]) {
    union {
        Byte b[64];
        uint64_t align;
    } buf;
    Fixed fx;
    fixed_init(&fx, buf.b, sizeof buf.b);
    NetIP ip = net_parse_ip(fixed_allocator(&fx), s);
    if (ip.len != 16)
        return false;
    memcpy(b, ip.p, 16);
    return true;
}

/* netip.ParseAddr(s) giving no error and an address that IsLoopback. A zone
 * is allowed after an IPv6 address, as netip has it, and makes no difference
 * to the answer. */
static bool cj_is_loopback_addr(Str s) {
    Int i = strings_index_any(s, CJ_LIT(".:%"));
    if (i < 0 || s.p[i] == '%')
        return false;
    if (s.p[i] == ':') {
        Int z = strings_index_any(s, CJ_LIT("%"));
        if (z >= 0) {
            if (z == s.len - 1)
                return false; /* netip wants a zone after the % */
            s = str_from_bytes(s.p, z);
        }
    }
    Byte b[16];
    if (!cj_parse_ip(s, b))
        return false;
    static const Byte v4_prefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    if (memcmp(b, v4_prefix, 12) == 0)
        return b[12] == 127;
    for (int k = 0; k < 15; k++)
        if (b[k] != 0)
            return false;
    return b[15] == 1;
}

/* secureMatch. A cookie that is not secure always goes, and one that is goes
 * over https and to localhost, which browsers count as secure whatever the
 * protocol. */
static bool cj_secure_match(const burrow__CookiejarEntry *e, bool https) {
    if (!e->secure || https)
        return true;
    if (cj_is_localhost(e->domain))
        return true;
    return cj_is_loopback_addr(e->domain);
}

static bool cj_should_send(const burrow__CookiejarEntry *e, bool https, Str host,
                           Str path) {
    return cj_domain_match(e, host) && cj_path_match(e, path) &&
           cj_secure_match(e, https);
}

bool burrow__cookiejar_has_dot_suffix(Str s, Str suffix) {
    return s.len > suffix.len && s.p[s.len - suffix.len - 1] == '.' &&
           memcmp(s.p + s.len - suffix.len, suffix.p, (size_t)suffix.len) == 0;
}

/* ----------------------------------------------------------------- Cookies */

static Str cj_entry_key(const CookiejarJar *j, Alloc *t, const Url *u, Str *host,
                        bool *ok) {
    *ok = false;
    if (!str_eq(u->scheme, CJ_LIT("http")) && !str_eq(u->scheme, CJ_LIT("https")))
        return BURROW_STR_EMPTY;
    Error e = BURROW_NO_ERROR;
    *host = burrow__cookiejar_canonical_host(t, u->host, &e);
    if (BURROW_FAILED(e))
        return BURROW_STR_EMPTY;
    *ok = true;
    return burrow__cookiejar_jar_key(*host, j->ps_list);
}

static CjSub *cj_sub_get(const CookiejarJar *j, Str key) {
    void *p = NULL;
    if (!map_get2(j->entries, &key, &p))
        return NULL;
    return (CjSub *)p;
}

/* Deletes the submap under key from the jar and frees it. */
static void cj_sub_drop(CookiejarJar *j, CjSub *sub) {
    map_del(j->entries, &sub->key);
    cj_sub_free(j->a, sub);
}

static int cj_entry_cmp(void *env, const void *x, const void *y) {
    (void)env;
    const burrow__CookiejarEntry *a = *(burrow__CookiejarEntry *const *)x;
    const burrow__CookiejarEntry *b = *(burrow__CookiejarEntry *const *)y;
    /* RFC 6265 section 5.4 point 2: the longest path first, then the earliest
     * creation time. */
    int r = str_cmp(b->path, a->path);
    if (r != 0)
        return r < 0 ? -1 : 1;
    r = (int)time_compare(a->creation, b->creation);
    if (r != 0)
        return r;
    if (a->seq_num != b->seq_num)
        return a->seq_num < b->seq_num ? -1 : 1;
    return 0;
}

Slice burrow__cookiejar_cookies_at(CookiejarJar *j, Alloc *a, const Url *u, Time now) {
    Slice cookies = slice_nil(TYPE_HTTP_COOKIE);
    Arena ar;
    arena_init(&ar, j->a, 0);
    Alloc *t = arena_allocator(&ar);
    Str host;
    bool ok;
    Str key = cj_entry_key(j, t, u, &host, &ok);
    if (!ok) {
        arena_free(&ar);
        return cookies;
    }

    sync_mutex_lock(&j->mu);
    CjSub *sub = cj_sub_get(j, key);
    if (sub == NULL) {
        sync_mutex_unlock(&j->mu);
        arena_free(&ar);
        return cookies;
    }

    bool https = str_eq(u->scheme, CJ_LIT("https"));
    Str path = u->path;
    if (path.len == 0)
        path = CJ_LIT("/");

    Int n = map_len(sub->m);
    burrow__CookiejarEntry **selected = (burrow__CookiejarEntry **)mem_alloc(
        t, (size_t)n * sizeof *selected, _Alignof(burrow__CookiejarEntry *));
    if (selected == NULL) {
        sync_mutex_unlock(&j->mu);
        arena_free(&ar);
        return cookies;
    }
    Int nsel = 0;
    void *v;
    for (MapIter it = map_iter(sub->m); map_next(&it, NULL, &v);) {
        CjStored *s = *(CjStored **)v;
        burrow__CookiejarEntry *e = &s->e;
        if (e->persistent && !time_after(e->expires, now)) {
            map_del(sub->m, &s->id);
            cj_stored_free(j->a, s);
            continue;
        }
        if (!cj_should_send(e, https, host, path))
            continue;
        e->last_access = now;
        selected[nsel++] = e;
    }
    if (map_len(sub->m) == 0)
        cj_sub_drop(j, sub);

    if (nsel > 0) {
        slices_sort_func(slice_from(selected, nsel, nsel, TYPE_UNSAFE_POINTER),
                         BURROW_FN(SlicesCmpFunc, cj_entry_cmp, NULL));
        Slice out = slice_make(a, TYPE_HTTP_COOKIE, nsel, nsel);
        if (out.len == nsel) {
            HttpCookie *c = (HttpCookie *)out.p;
            for (Int i = 0; i < nsel; i++) {
                c[i].name = str_clone(a, selected[i]->name);
                c[i].value = str_clone(a, selected[i]->value);
                c[i].quoted = selected[i]->quoted;
            }
            cookies = out;
        }
    }
    sync_mutex_unlock(&j->mu);
    arena_free(&ar);
    return cookies;
}

Slice cookiejar_jar_cookies(CookiejarJar *j, Alloc *a, const Url *u) {
    return burrow__cookiejar_cookies_at(j, a, u, time_now());
}

/* -------------------------------------------------------------- SetCookies */

/* The time a session cookie expires, far enough away and still something
 * most date formats can hold. */
static Time cj_end_of_time(void) {
    return time_date(9999, TIME_DECEMBER, 31, 23, 59, 59, 0, time_utc_loc);
}

/* newEntry. The entry for c, a cookie from host with defPath as the default
 * path, with its strings still pointing into c, host and t. *remove says the
 * cookie has expired and deletes the one it names, and then only name, domain
 * and path are filled in. A domain attribute that is not allowed is an
 * error. */
static burrow__CookiejarEntry cj_new_entry(CookiejarJar *j, Alloc *t,
                                           const HttpCookie *c, Time now, Str def_path,
                                           Str host, bool *remove, Error *err) {
    burrow__CookiejarEntry e;
    memset(&e, 0, sizeof e);
    *remove = false;
    e.name = c->name;
    if (c->path.len == 0 || c->path.p[0] != '/')
        e.path = def_path;
    else
        e.path = c->path;

    e.domain =
        burrow__cookiejar_domain_and_type(j, t, host, c->domain, &e.host_only, err);
    if (BURROW_FAILED(*err))
        return e;

    /* Max-Age wins over Expires. */
    if (c->max_age < 0) {
        *remove = true;
        return e;
    }
    if (c->max_age > 0) {
        /* time.Duration(c.MaxAge) * time.Second, wrapping as Go's does. */
        Duration d = (Duration)((uint64_t)c->max_age * (uint64_t)TIME_SECOND);
        e.expires = time_add(now, d);
        e.persistent = true;
    } else if (time_is_zero(c->expires)) {
        e.expires = cj_end_of_time();
        e.persistent = false;
    } else {
        if (!time_after(c->expires, now)) {
            *remove = true;
            return e;
        }
        e.expires = c->expires;
        e.persistent = true;
    }

    e.value = c->value;
    e.quoted = c->quoted;
    e.secure = c->secure;
    e.http_only = c->http_only;

    if (c->same_site == HTTP_SAME_SITE_DEFAULT_MODE)
        e.same_site = CJ_LIT("SameSite");
    else if (c->same_site == HTTP_SAME_SITE_STRICT_MODE)
        e.same_site = CJ_LIT("SameSite=Strict");
    else if (c->same_site == HTTP_SAME_SITE_LAX_MODE)
        e.same_site = CJ_LIT("SameSite=Lax");
    return e;
}

/* id, the domain;path;name of e, in t. */
static Str cj_id(Alloc *t, const burrow__CookiejarEntry *e) {
    Int n = e->domain.len + 1 + e->path.len + 1 + e->name.len;
    Byte *p = (Byte *)mem_alloc(t, (size_t)n, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    Byte *q = p;
    if (e->domain.len > 0)
        memcpy(q, e->domain.p, (size_t)e->domain.len);
    q += e->domain.len;
    *q++ = ';';
    if (e->path.len > 0)
        memcpy(q, e->path.p, (size_t)e->path.len);
    q += e->path.len;
    *q++ = ';';
    if (e->name.len > 0)
        memcpy(q, e->name.p, (size_t)e->name.len);
    return str_from_bytes(p, n);
}

static Str cj_put(Byte **q, Str s) {
    Str out = str_from_bytes(*q, s.len);
    if (s.len > 0)
        memcpy(*q, s.p, (size_t)s.len);
    *q += s.len;
    return out;
}

/* The jar's own copy of e, with id, in one block from a. */
static CjStored *cj_store(Alloc *a, const burrow__CookiejarEntry *e, Str id) {
    size_t size = sizeof(CjStored) + (size_t)e->name.len + (size_t)e->value.len +
                  (size_t)e->domain.len + (size_t)e->path.len + (size_t)id.len;
    CjStored *s = (CjStored *)mem_alloc(a, size, _Alignof(CjStored));
    if (s == NULL)
        return NULL;
    s->e = *e;
    s->size = size;
    Byte *q = (Byte *)(s + 1);
    s->e.name = cj_put(&q, e->name);
    s->e.value = cj_put(&q, e->value);
    s->e.domain = cj_put(&q, e->domain);
    s->e.path = cj_put(&q, e->path);
    s->id = cj_put(&q, id);
    return s;
}

static CjSub *cj_sub_new(Alloc *a, Str key) {
    size_t size = sizeof(CjSub) + (size_t)key.len;
    CjSub *sub = (CjSub *)mem_alloc(a, size, _Alignof(CjSub));
    if (sub == NULL)
        return NULL;
    sub->m = map_make(a, TYPE_STRING, TYPE_UNSAFE_POINTER, 0);
    if (sub->m == NULL) {
        mem_free(a, sub, size, _Alignof(CjSub));
        return NULL;
    }
    sub->size = size;
    Byte *q = (Byte *)(sub + 1);
    sub->key = cj_put(&q, key);
    return sub;
}

void burrow__cookiejar_set_cookies_at(CookiejarJar *j, const Url *u, Slice cookies,
                                      Time now) {
    if (cookies.len == 0)
        return;
    Arena ar;
    arena_init(&ar, j->a, 0);
    Alloc *t = arena_allocator(&ar);
    Str host;
    bool ok;
    Str key = cj_entry_key(j, t, u, &host, &ok);
    if (!ok) {
        arena_free(&ar);
        return;
    }
    Str def_path = burrow__cookiejar_default_path(u->path);

    sync_mutex_lock(&j->mu);
    CjSub *sub = cj_sub_get(j, key);
    bool fresh = false;

    const HttpCookie *cs = (const HttpCookie *)cookies.p;
    for (Int i = 0; i < cookies.len; i++) {
        bool remove;
        Error err = BURROW_NO_ERROR;
        burrow__CookiejarEntry e =
            cj_new_entry(j, t, &cs[i], now, def_path, host, &remove, &err);
        if (BURROW_FAILED(err))
            continue;
        Str id = cj_id(t, &e);
        if (id.len == 0)
            continue; /* t said no */
        if (remove) {
            void *p = NULL;
            if (sub != NULL && map_get2(sub->m, &id, &p)) {
                map_del(sub->m, &id);
                cj_stored_free(j->a, (CjStored *)p);
            }
            continue;
        }
        if (sub == NULL) {
            sub = cj_sub_new(j->a, key);
            if (sub == NULL)
                continue;
            fresh = true;
        }

        void *p = NULL;
        CjStored *old = map_get2(sub->m, &id, &p) ? (CjStored *)p : NULL;
        if (old != NULL) {
            e.creation = old->e.creation;
            e.seq_num = old->e.seq_num;
        } else {
            e.creation = now;
            e.seq_num = j->next_seq_num++;
        }
        e.last_access = now;
        CjStored *s = cj_store(j->a, &e, id);
        if (s == NULL)
            continue;
        /* The key in the map points into the entry it is the key of, so the
         * old one goes before the new one comes in. */
        if (old != NULL) {
            map_del(sub->m, &old->id);
            cj_stored_free(j->a, old);
        }
        if (!map_set(sub->m, &s->id, &s))
            cj_stored_free(j->a, s);
    }

    if (sub != NULL) {
        if (map_len(sub->m) == 0) {
            if (fresh)
                cj_sub_free(j->a, sub);
            else
                cj_sub_drop(j, sub);
        } else if (fresh && !map_set(j->entries, &sub->key, &sub)) {
            cj_sub_free(j->a, sub);
        }
    }
    sync_mutex_unlock(&j->mu);
    arena_free(&ar);
}

void cookiejar_jar_set_cookies(CookiejarJar *j, const Url *u, Slice cookies) {
    burrow__cookiejar_set_cookies_at(j, u, cookies, time_now());
}

/* ---------------------------------------------------------- hosts and keys */

Str burrow__cookiejar_canonical_host(Alloc *a, Str host, Error *err) {
    *err = BURROW_NO_ERROR;
    if (burrow__cookiejar_has_port(host)) {
        host = net_split_host_port(host, NULL, err);
        if (BURROW_FAILED(*err))
            return BURROW_STR_EMPTY;
    }
    /* A fully qualified name loses its trailing dot. */
    host = strings_trim_suffix(host, CJ_LIT("."));
    Str encoded = burrow__cookiejar_to_ascii(a, host, err);
    if (BURROW_FAILED(*err))
        return BURROW_STR_EMPTY;
    /* encoded is ASCII, so there is nothing to ask. */
    bool ok;
    return burrow__http_ascii_to_lower(a, encoded, &ok);
}

bool burrow__cookiejar_has_port(Str host) {
    Int colons = strings_count(host, CJ_LIT(":"));
    if (colons == 0)
        return false;
    if (colons == 1)
        return true;
    return host.p[0] == '[' && strings_contains(host, CJ_LIT("]:"));
}

Str burrow__cookiejar_jar_key(Str host, CookiejarPublicSuffixList psl) {
    if (burrow__cookiejar_is_ip(host))
        return host;

    Int i;
    if (psl.vt == NULL) {
        i = strings_last_index(host, CJ_LIT("."));
        if (i <= 0)
            return host;
    } else {
        Str suffix = cookiejar_public_suffix_list_public_suffix(psl, host);
        if (str_eq(suffix, host))
            return host;
        i = host.len - suffix.len;
        if (i <= 0 || host.p[i - 1] != '.') {
            /* The list is broken, and keeping the cookies under host is the
             * safe thing to do about it. */
            return host;
        }
        /* Only the length of suffix counts from here on, so a list that says
         * "com" for www.buggy.psl still gives a key made out of host. */
    }
    Int prev_dot = strings_last_index(str_from_bytes(host.p, i - 1), CJ_LIT("."));
    return str_from_bytes(host.p + prev_dot + 1, host.len - prev_dot - 1);
}

bool burrow__cookiejar_is_ip(Str host) {
    /* A name cannot have a : or a % in it, so this is likely an IPv6 address
     * and certainly not a name. Taking it for an address is the careful
     * choice, and keeps ::1%.www.example.com from being under
     * www.example.com. */
    if (strings_contains_any(host, CJ_LIT(":%")))
        return true;
    Byte b[16];
    return cj_parse_ip(host, b);
}

Str burrow__cookiejar_default_path(Str path) {
    if (path.len == 0 || path.p[0] != '/')
        return CJ_LIT("/"); /* empty or malformed */
    Int i =
        strings_last_index(path, CJ_LIT("/")); /* not -1, since path starts with / */
    if (i == 0)
        return CJ_LIT("/");           /* "/abc" */
    return str_from_bytes(path.p, i); /* "/abc/xyz" or "/abc/xyz/" */
}

Str burrow__cookiejar_domain_and_type(CookiejarJar *j, Alloc *a, Str host, Str domain,
                                      bool *host_only, Error *err) {
    *err = BURROW_NO_ERROR;
    *host_only = false;
    if (domain.len == 0) {
        /* No domain attribute means a host cookie. */
        *host_only = true;
        return host;
    }

    if (burrow__cookiejar_is_ip(host)) {
        /* RFC 6265 is not clear here, and the sensible reading is that a
         * cookie with an IP address in its domain attribute is allowed.
         *
         * It says to strip a leading dot from the attribute first, which most
         * browsers do not do for an IP address, and it makes no sense on one,
         * so the other steps come down to this. */
        if (!str_eq(host, domain)) {
            *err = burrow__cookiejar_err_illegal_domain;
            return BURROW_STR_EMPTY;
        }
        /* RFC 6265 would have it be a domain cookie, which for an address
         * with no subdomains is the same as a host cookie. Browsers and curl
         * call it a host cookie, since there is no domain to speak of. */
        *host_only = true;
        return host;
    }

    /* From here on a valid cookie is a domain cookie, but for the one public
     * suffix case below. RFC 6265 section 5.2.3. */
    domain = strings_trim_prefix(domain, CJ_LIT("."));

    if (domain.len == 0 || domain.p[0] == '.') {
        /* "Domain=." or "Domain=..some.thing". */
        *err = burrow__cookiejar_err_malformed_domain;
        return BURROW_STR_EMPTY;
    }

    bool is_ascii;
    domain = burrow__http_ascii_to_lower(a, domain, &is_ascii);
    if (!is_ascii) {
        /* Such as "perché.com" for "xn--perch-fsa.com". */
        *err = burrow__cookiejar_err_malformed_domain;
        return BURROW_STR_EMPTY;
    }

    if (domain.p[domain.len - 1] == '.') {
        /* "Domain=www.example.com.". Browsers take it, in different ways, but
         * RFC 6265 sections 5.1.2 and 5.1.3 say no. */
        *err = burrow__cookiejar_err_malformed_domain;
        return BURROW_STR_EMPTY;
    }

    /* RFC 6265 section 5.3 step 5. */
    if (j->ps_list.vt != NULL) {
        Str ps = cookiejar_public_suffix_list_public_suffix(j->ps_list, domain);
        if (ps.len != 0 && !burrow__cookiejar_has_dot_suffix(domain, ps)) {
            if (str_eq(host, domain)) {
                /* The one case of a host cookie with a domain attribute. */
                *host_only = true;
                return host;
            }
            *err = burrow__cookiejar_err_illegal_domain;
            return BURROW_STR_EMPTY;
        }
    }

    /* The domain has to domain-match host, so www.mycompany.com cannot set a
     * cookie for .ourcompetitors.com. */
    if (!str_eq(host, domain) && !burrow__cookiejar_has_dot_suffix(host, domain)) {
        *err = burrow__cookiejar_err_illegal_domain;
        return BURROW_STR_EMPTY;
    }
    return domain;
}

/* ---------------------------------------------------------------- the jar */

Int burrow__cookiejar_entries(CookiejarJar *j, Alloc *a, burrow__CookiejarEntry **out) {
    sync_mutex_lock(&j->mu);
    Int n = 0;
    void *v;
    for (MapIter it = map_iter(j->entries); map_next(&it, NULL, &v);)
        n += map_len((*(CjSub **)v)->m);
    *out = (burrow__CookiejarEntry *)mem_alloc(
        a, (size_t)(n > 0 ? n : 1) * sizeof **out, _Alignof(burrow__CookiejarEntry));
    if (*out == NULL) {
        n = 0;
    } else {
        Int k = 0;
        for (MapIter it = map_iter(j->entries); map_next(&it, NULL, &v);) {
            void *w;
            for (MapIter it2 = map_iter((*(CjSub **)v)->m); map_next(&it2, NULL, &w);)
                (*out)[k++] = (*(CjStored **)w)->e;
        }
    }
    sync_mutex_unlock(&j->mu);
    return n;
}

static void cj_vt_set_cookies(void *self, const Url *u, Slice cookies) {
    cookiejar_jar_set_cookies((CookiejarJar *)self, u, cookies);
}

static Slice cj_vt_cookies(void *self, Alloc *a, const Url *u) {
    return cookiejar_jar_cookies((CookiejarJar *)self, a, u);
}

static const HttpCookieJarVT cj_cookie_jar_vt = {&cj_jar_desc, cj_vt_set_cookies,
                                                 cj_vt_cookies};

HttpCookieJar cookiejar_jar_as_cookie_jar(CookiejarJar *j) {
    return (HttpCookieJar){&cj_cookie_jar_vt, j};
}

/* --------------------------------------------------------------- punycode */

/* The parameters of RFC 3492 section 5. Everything is done in int32_t, so
 * that what overflows is the same on every machine. */
enum {
    CJ_BASE = 36,
    CJ_DAMP = 700,
    CJ_INITIAL_BIAS = 72,
    CJ_INITIAL_N = 128,
    CJ_SKEW = 38,
    CJ_TMAX = 26,
    CJ_TMIN = 1,
};

static Byte cj_encode_digit(int32_t digit) {
    if (0 <= digit && digit < 26)
        return (Byte)(digit + 'a');
    if (26 <= digit && digit < 36)
        return (Byte)(digit + ('0' - 26));
    panic_str(CJ_LIT("cookiejar: internal error in punycode encoding"));
}

/* adapt, the bias adaptation function of section 6.1. */
static int32_t cj_adapt(int32_t delta, int32_t num_points, bool first_time) {
    if (first_time)
        delta /= CJ_DAMP;
    else
        delta /= 2;
    delta += delta / num_points;
    int32_t k = 0;
    while (delta > ((CJ_BASE - CJ_TMIN) * CJ_TMAX) / 2) {
        delta /= CJ_BASE - CJ_TMIN;
        k += CJ_BASE;
    }
    return k + (CJ_BASE - CJ_TMIN + 1) * delta / (delta + CJ_SKEW);
}

/* The runes of s one at a time, as Go's range over a string has them. */
static Rune cj_next_rune(Str s, Int *i) {
    Int size;
    Rune r = utf8_decode_rune_in_string(str_from_bytes(s.p + *i, s.len - *i), &size);
    *i += size;
    return r;
}

/* Go's int32 addition, which wraps. */
static int32_t cj_add32(int32_t x, int32_t y) {
    return (int32_t)((uint32_t)x + (uint32_t)y);
}

static int32_t cj_mul32(int32_t x, int32_t y) {
    return (int32_t)((uint32_t)x * (uint32_t)y);
}

/* WriteByte on b, remembering in *bad that the allocator said no. */
static void cj_write_byte(StringsBuilder *b, Byte c, bool *bad) {
    if (BURROW_FAILED(strings_builder_write_byte(b, c)))
        *bad = true;
}

static Error cj_invalid_label(Str s) {
    return fmt_errorf_v("cookiejar: invalid label %q", s);
}

Str burrow__cookiejar_encode(Alloc *a, Str prefix, Str s, Error *err) {
    *err = BURROW_NO_ERROR;
    StringsBuilder out = STRINGS_BUILDER(a);
    Error we = BURROW_NO_ERROR;
    if (!strings_builder_grow(&out, prefix.len + 1 + 2 * s.len)) {
        *err = burrow_err_out_of_memory;
        return BURROW_STR_EMPTY;
    }
    strings_builder_write_string(&out, prefix, &we);
    bool bad = false;
    int32_t delta = 0;
    int32_t n = CJ_INITIAL_N;
    int32_t bias = CJ_INITIAL_BIAS;
    int32_t b = 0;
    int32_t remaining = 0;
    for (Int i = 0; i < s.len;) {
        Rune r = cj_next_rune(s, &i);
        if (r < UTF8_RUNE_SELF) {
            b++;
            cj_write_byte(&out, (Byte)r, &bad);
        } else {
            remaining++;
        }
    }
    int32_t h = b;
    if (b > 0)
        cj_write_byte(&out, '-', &bad);
    while (remaining != 0) {
        int32_t m = 0x7fffffff;
        for (Int i = 0; i < s.len;) {
            Rune r = cj_next_rune(s, &i);
            if (m > r && r >= n)
                m = r;
        }
        delta = cj_add32(delta, cj_mul32(m - n, h + 1));
        if (delta < 0) {
            *err = cj_invalid_label(s);
            return BURROW_STR_EMPTY;
        }
        n = m;
        for (Int i = 0; i < s.len;) {
            Rune r = cj_next_rune(s, &i);
            if (r < n) {
                delta = cj_add32(delta, 1);
                if (delta < 0) {
                    *err = cj_invalid_label(s);
                    return BURROW_STR_EMPTY;
                }
                continue;
            }
            if (r > n)
                continue;
            int32_t q = delta;
            for (int32_t k = CJ_BASE;; k += CJ_BASE) {
                int32_t tt = k - bias;
                if (tt < CJ_TMIN)
                    tt = CJ_TMIN;
                else if (tt > CJ_TMAX)
                    tt = CJ_TMAX;
                if (q < tt)
                    break;
                cj_write_byte(&out, cj_encode_digit(tt + (q - tt) % (CJ_BASE - tt)),
                              &bad);
                q = (q - tt) / (CJ_BASE - tt);
            }
            cj_write_byte(&out, cj_encode_digit(q), &bad);
            bias = cj_adapt(delta, h + 1, h == b);
            delta = 0;
            h++;
            remaining--;
        }
        delta = cj_add32(delta, 1);
        n++;
    }
    if (bad || BURROW_FAILED(we)) {
        *err = burrow_err_out_of_memory;
        return BURROW_STR_EMPTY;
    }
    return strings_builder_string(&out);
}

/* The ACE prefix, for ASCII Compatible Encoding. What follows is IDNA, RFC
 * 5890 and the rest, rather than Punycode itself. */
#define CJ_ACE_PREFIX CJ_LIT("xn--")

Str burrow__cookiejar_to_ascii(Alloc *a, Str s, Error *err) {
    *err = BURROW_NO_ERROR;
    if (burrow__http_ascii_is(s))
        return s;
    StringsBuilder out = STRINGS_BUILDER(a);
    Error we = BURROW_NO_ERROR;
    bool bad = false;
    Int start = 0;
    for (Int i = 0; i <= s.len; i++) {
        if (i < s.len && s.p[i] != '.')
            continue;
        Str label = str_from_bytes(s.p + start, i - start);
        if (start > 0)
            cj_write_byte(&out, '.', &bad);
        if (!burrow__http_ascii_is(label)) {
            Str enc = burrow__cookiejar_encode(a, CJ_ACE_PREFIX, label, err);
            if (BURROW_FAILED(*err))
                return BURROW_STR_EMPTY;
            label = enc;
        }
        strings_builder_write_string(&out, label, &we);
        start = i + 1;
    }
    if (bad || BURROW_FAILED(we)) {
        *err = burrow_err_out_of_memory;
        return BURROW_STR_EMPTY;
    }
    return strings_builder_string(&out);
}

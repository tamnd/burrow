/* resolv.conf, and the list of names a lookup tries.
 *
 * Derived from Go's src/net/dnsconfig.go and dnsconfig_unix.go, and avoidDNS
 * and nameList from dnsclient_unix.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "internal.h"

#include "burrow/error.h"
#include "burrow/io/fs.h"
#include "burrow/mem.h"
#include "burrow/net.h"
#include "burrow/net/netip.h"
#include "burrow/os.h"
#include "burrow/slice.h"
#include "burrow/sync/atomic.h"
#include "burrow/time.h"

#include <stdint.h>
#include <string.h>

const Str burrow__net_default_ns[2] = {BURROW_S_INIT("127.0.0.1:53"),
                                       BURROW_S_INIT("[::1]:53")};

static Slice nc_default_ns(void) {
    return slice_from((void *)(uintptr_t)burrow__net_default_ns, 2, 2, TYPE_STRING);
}

bool burrow__dns_config_is_default_ns(const burrow__DNSConfig *c) {
    return c->servers.len == 2 && c->servers.p == (const void *)burrow__net_default_ns;
}

uint32_t burrow__dns_config_server_offset(burrow__DNSConfig *c) {
    if (c->rotate)
        return sync_atomic_add_uint32(&c->soffset, 1) - 1; /* 0 to start */
    return 0;
}

static bool nc_has_prefix(Str s, const char *prefix, Int n) {
    return s.len >= n && memcmp(s.p, prefix, (size_t)n) == 0;
}

static bool nc_is(Str s, const char *lit) {
    return str_eq(s, str_from_cstr(lit));
}

static Str nc_at(Slice f, Int i) {
    return ((const Str *)f.p)[i];
}

static Slice nc_push(Alloc *a, Slice s, Str v) {
    return slice_append(a, s, &v, 1);
}

/* The number after an option's colon, which is Go's dtoi with its two other
 * answers thrown away. */
static Int nc_option(Str s, Int skip) {
    Int n = 0;
    Int used = 0;
    (void)burrow__net_dtoi(str_from_bytes(s.p + skip, s.len - skip), &n, &used);
    return n;
}

static void nc_options(burrow__DNSConfig *conf, Slice f) {
    for (Int k = 1; k < f.len; k++) {
        Str s = nc_at(f, k);
        if (nc_has_prefix(s, "ndots:", 6)) {
            Int n = nc_option(s, 6);
            if (n < 0)
                n = 0;
            else if (n > 15)
                n = 15;
            conf->ndots = n;
        } else if (nc_has_prefix(s, "timeout:", 8)) {
            Int n = nc_option(s, 8);
            if (n < 1)
                n = 1;
            conf->timeout = (Duration)n * TIME_SECOND;
        } else if (nc_has_prefix(s, "attempts:", 9)) {
            Int n = nc_option(s, 9);
            if (n < 1)
                n = 1;
            conf->attempts = n;
        } else if (nc_is(s, "rotate")) {
            conf->rotate = true;
        } else if (nc_is(s, "single-request") || nc_is(s, "single-request-reopen")) {
            /* Linux: glibc asks for A and AAAA at the same time unless this
             * says to ask one after the other. */
            conf->single_request = true;
        } else if (nc_is(s, "use-vc") || nc_is(s, "usevc") || nc_is(s, "tcp")) {
            /* Linux (use-vc), FreeBSD (usevc) and OpenBSD (tcp): ask over TCP
             * only. */
            conf->use_tcp = true;
        } else if (nc_is(s, "trust-ad")) {
            conf->trust_ad = true;
        } else if (nc_is(s, "edns0")) {
            /* EDNS is on anyway, so this changes nothing. */
        } else if (nc_is(s, "no-reload")) {
            conf->no_reload = true;
        } else {
            conf->unknown_opt = true;
        }
    }
}

/* One line, already split. What is kept is copied into a, since the fields
 * point into the file's buffer. */
static void nc_line(Alloc *a, burrow__DNSConfig *conf, Slice f) {
    Str key = nc_at(f, 0);
    if (nc_is(key, "nameserver")) {
        /* Add one server, if it is an IP address and there are fewer than
         * three, which is small but the usual limit. A name would need DNS to
         * look up. */
        if (f.len > 1 && conf->servers.len < 3) {
            Error err = BURROW_NO_ERROR;
            (void)netip_parse_addr(nc_at(f, 1), &err);
            if (BURROW_OK(err))
                conf->servers =
                    nc_push(a, conf->servers,
                            net_join_host_port(a, nc_at(f, 1), BURROW_S("53")));
        }
    } else if (nc_is(key, "domain")) {
        /* The search list becomes just this domain. */
        if (f.len > 1) {
            conf->search = slice_make(a, TYPE_STRING, 0, 1);
            conf->search =
                nc_push(a, conf->search,
                        str_clone(a, burrow__net_ensure_rooted(a, nc_at(f, 1))));
        }
    } else if (nc_is(key, "search")) {
        conf->search = slice_make(a, TYPE_STRING, 0, f.len - 1);
        for (Int i = 1; i < f.len; i++) {
            Str name = burrow__net_ensure_rooted(a, nc_at(f, i));
            if (name.len == 1 && name.p[0] == '.')
                continue;
            conf->search = nc_push(a, conf->search, str_clone(a, name));
        }
    } else if (nc_is(key, "options")) {
        nc_options(conf, f);
    } else if (nc_is(key, "lookup")) {
        /* OpenBSD: some of bind, file and yp, in the order to try them. */
        conf->lookup = slice_make(a, TYPE_STRING, 0, f.len - 1);
        for (Int i = 1; i < f.len; i++)
            conf->lookup = nc_push(a, conf->lookup, str_clone(a, nc_at(f, i)));
    } else {
        conf->unknown_opt = true;
    }
}

/* The defaults Go falls back to when the file cannot be read. */
static void nc_fallback(Alloc *a, burrow__DNSConfig *conf,
                        burrow__NetHostnameFunc hostname, Error err) {
    conf->servers = nc_default_ns();
    conf->search = burrow__dns_default_search(a, hostname);
    conf->err = err;
}

void burrow__dns_read_config(Alloc *a, Str filename, burrow__NetHostnameFunc hostname,
                             burrow__DNSConfig *conf) {
    memset(conf, 0, sizeof *conf);
    /* Typed, so the nameserver lines can append to it. */
    conf->servers = slice_nil(TYPE_STRING);
    conf->ndots = 1;
    conf->timeout = 5 * TIME_SECOND;
    conf->attempts = 2;
    Error err = BURROW_NO_ERROR;
    burrow__NetFile *file = burrow__net_open(a, filename, &err);
    if (file == NULL) {
        nc_fallback(a, conf, hostname, err);
        return;
    }
    FsFileInfo fi = os_file_stat(file->file, a, &err);
    if (BURROW_FAILED(err)) {
        burrow__net_file_close(file, a);
        nc_fallback(a, conf, hostname, err);
        return;
    }
    conf->mtime = fi.vt->mod_time(fi.data);
    Str line;
    while (burrow__net_file_read_line(file, &line)) {
        if (line.len > 0 && (line.p[0] == ';' || line.p[0] == '#'))
            continue; /* a comment */
        Slice f = burrow__net_get_fields(a, line);
        if (f.len >= 1)
            nc_line(a, conf, f);
        if (f.p != NULL)
            mem_free(a, f.p, (size_t)f.cap * sizeof(Str), _Alignof(Str));
    }
    burrow__net_file_close(file, a);
    if (conf->servers.len == 0)
        conf->servers = nc_default_ns();
    if (conf->search.len == 0)
        conf->search = burrow__dns_default_search(a, hostname);
}

Slice burrow__dns_default_search(Alloc *a, burrow__NetHostnameFunc hostname) {
    Error err = BURROW_NO_ERROR;
    Str hn = hostname != NULL ? hostname(a, &err) : os_hostname(a, &err);
    if (BURROW_FAILED(err))
        return slice_nil(TYPE_STRING); /* best effort */
    const Byte *dot =
        hn.len > 0 ? (const Byte *)memchr(hn.p, '.', (size_t)hn.len) : NULL;
    if (dot == NULL || dot - hn.p >= hn.len - 1)
        return slice_nil(TYPE_STRING);
    Int i = (Int)(dot - hn.p);
    Str domain = str_from_bytes(hn.p + i + 1, hn.len - i - 1);
    Slice out = slice_make(a, TYPE_STRING, 0, 1);
    return nc_push(a, out, str_clone(a, burrow__net_ensure_rooted(a, domain)));
}

Str burrow__net_ensure_rooted(Alloc *a, Str s) {
    if (s.len > 0 && s.p[s.len - 1] == '.')
        return s;
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)s.len + 2, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    if (s.len > 0)
        memcpy(p, s.p, (size_t)s.len);
    p[s.len] = '.';
    p[s.len + 1] = 0;
    return str_from_bytes(p, s.len + 1);
}

bool burrow__net_avoid_dns(Str name) {
    if (name.len == 0)
        return true;
    if (name.p[name.len - 1] == '.')
        name.len--;
    return burrow__net_has_suffix_fold(name, BURROW_S(".onion"));
}

/* name + suffix, in a. */
static Str nc_concat(Alloc *a, Str name, Str suffix) {
    Int n = name.len + suffix.len;
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)n + 1, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    if (name.len > 0)
        memcpy(p, name.p, (size_t)name.len);
    if (suffix.len > 0)
        memcpy(p + name.len, suffix.p, (size_t)suffix.len);
    p[n] = 0;
    return str_from_bytes(p, n);
}

Slice burrow__dns_config_name_list(const burrow__DNSConfig *c, Alloc *a, Str name) {
    /* Check the length, as isDomainName does. */
    bool rooted = name.len > 0 && name.p[name.len - 1] == '.';
    if (name.len > 254 || (name.len == 254 && !rooted))
        return slice_nil(TYPE_STRING);

    /* A rooted name, with its trailing dot, is tried as it is and only so. */
    if (rooted) {
        if (burrow__net_avoid_dns(name))
            return slice_nil(TYPE_STRING);
        Slice one = slice_make(a, TYPE_STRING, 0, 1);
        return nc_push(a, one, str_clone(a, name));
    }

    Int dots = 0;
    for (Int i = 0; i < name.len; i++)
        if (name.p[i] == '.')
            dots++;
    bool has_ndots = dots >= c->ndots;
    Str rname = nc_concat(a, name, BURROW_S("."));

    Slice names = slice_make(a, TYPE_STRING, 0, 1 + c->search.len);
    /* With enough dots, the name as it is goes first. */
    if (has_ndots && !burrow__net_avoid_dns(rname))
        names = nc_push(a, names, rname);
    /* Then the suffixes that do not make it too long. */
    for (Int i = 0; i < c->search.len; i++) {
        Str fqdn = nc_concat(a, rname, nc_at(c->search, i));
        if (!burrow__net_avoid_dns(fqdn) && fqdn.len <= 254)
            names = nc_push(a, names, fqdn);
    }
    /* And the name as it is last, if it did not go first. */
    if (!has_ndots && !burrow__net_avoid_dns(rname))
        names = nc_push(a, names, rname);
    return names;
}

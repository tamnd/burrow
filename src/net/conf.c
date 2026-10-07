/* Which resolver, and in which order the hosts file and DNS are asked.
 *
 * Derived from Go's src/net/conf.go.
 * Go source: go1.27.1.
 *
 * Go can hand a lookup to the C library through cgo, and decides here when it
 * should. burrow has no such path, so the system's conf says cgo is not
 * available, and every answer is one of the orders Go's own resolver can
 * follow. The rest of the logic is kept whole, with cgo_available as a field,
 * so that Go's tests of it run as they are.
 *
 * Copyright 2015 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "internal.h"

#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/io/fs.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/net.h"
#include "burrow/os.h"
#include "burrow/platform.h"
#include "burrow/slice.h"
#include "burrow/strings.h"
#include "burrow/sync.h"

#include <stdint.h>
#include <string.h>

#define CF_LIT(s) str_from_bytes((const Byte *)("" s), (Int)(sizeof(s) - 1))

Str burrow__host_lookup_order_string(int32_t o, Alloc *a) {
    static const char *const names[] = {"cgo", "files,dns", "dns,files", "files",
                                        "dns"};
    if (o >= 0 && o < (int32_t)(sizeof names / sizeof names[0]))
        return str_from_cstr(names[o]);
    return fmt_sprintf_v(a, "hostLookupOrder=%d??", (int)o);
}

/* ------------------------------------------------------------- systemConf */

static struct {
    SyncOnce once;
    burrow__NetConf val;
} cf_system;

void burrow__net_go_debug_net_dns(Str *mode, Int *level) {
    *mode = BURROW_STR_EMPTY;
    *level = 0;
    Str v;
    if (!burrow__net_godebug("netdns", &v))
        return;
    Str parts[2] = {v, BURROW_STR_EMPTY};
    int n = 1;
    Int i = strings_index_byte(v, '+');
    if (i >= 0) {
        parts[0] = str_from_bytes(v.p, i);
        parts[1] = str_from_bytes(v.p + i + 1, v.len - i - 1);
        n = 2;
    }
    for (int k = 0; k < n; k++) {
        Str s = parts[k];
        if (s.len == 0)
            continue;
        if (s.p[0] >= '0' && s.p[0] <= '9') {
            Int d = 0, used = 0;
            (void)burrow__net_dtoi(s, &d, &used);
            *level = d;
        } else {
            *mode = s;
        }
    }
}

static void cf_print(Str line) {
    Error err = BURROW_NO_ERROR;
    (void)os_file_write(
        os_stderr_file(),
        slice_from((void *)(uintptr_t)line.p, line.len, line.len, TYPE_UINT8), &err);
}

static void cf_init(void *env) {
    (void)env;
    burrow__NetConf *c = &cf_system.val;
    c->goos = CF_LIT(BURROW_OS_NAME);
    Str mode;
    Int level;
    burrow__net_go_debug_net_dns(&mode, &level);
    bool is_go = str_eq(mode, CF_LIT("go"));
    bool is_cgo = str_eq(mode, CF_LIT("cgo"));
    /* Neither the netgo nor the netcgo build tag. */
    c->net_go = is_go;
    c->net_cgo = is_cgo;
    c->dns_debug_level = level > INT32_MAX ? INT32_MAX : (int32_t)level;
    c->prefer_cgo = false;
    c->cgo_available = false;

    if (c->dns_debug_level > 0) {
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        if (c->dns_debug_level > 1)
            cf_print(fmt_sprintf_v(
                a, "go package net: confVal.netCgo = %s  netGo = %s\n",
                c->net_cgo ? "true" : "false", c->net_go ? "true" : "false"));
        if (!is_go && !is_cgo && mode.len > 0)
            cf_print(
                CF_LIT("go package net: GODEBUG=netdns contains an invalid dns mode, "
                       "ignoring it\n"));
        /* cgoAvailable is false, so this is always Go's first case. */
        if (is_cgo)
            cf_print(CF_LIT("go package net: ignoring GODEBUG=netdns=cgo as the binary "
                            "was compiled without support for the cgo resolver\n"));
        else
            cf_print(CF_LIT("go package net: using the Go DNS resolver\n"));
        arena_free(&ar);
    }
}

const burrow__NetConf *burrow__net_system_conf(void) {
    sync_once_do(&cf_system.once, BURROW_FN(Func, cf_init, NULL));
    return &cf_system.val;
}

/* --------------------------------------------------------------- the order */

static bool cf_goos(const burrow__NetConf *c, const char *name) {
    return str_eq(c->goos, str_from_cstr(name));
}

bool burrow__net_conf_must_use_go_resolver(const burrow__NetConf *c,
                                           const NetResolver *r) {
    if (!c->cgo_available)
        return true;
    if (cf_goos(c, "plan9")) {
        if (r == NULL || r->dial.f == NULL)
            return false;
    }
    return c->net_go || (r != NULL && r->prefer_go);
}

static Str cf_os_hostname(Alloc *a, Error *err) {
    return os_hostname(a, err);
}

static burrow__NetHostnameFunc cf_get_hostname = cf_os_hostname;

void burrow__net_set_get_hostname(burrow__NetHostnameFunc fn) {
    cf_get_hostname = fn != NULL ? fn : cf_os_hostname;
}

burrow__NetHostnameFunc burrow__net_get_hostname(void) {
    return cf_get_hostname;
}

static bool cf_is_localhost(Str h) {
    return burrow__net_equal_fold(h, CF_LIT("localhost")) ||
           burrow__net_equal_fold(h, CF_LIT("localhost.localdomain")) ||
           burrow__net_has_suffix_fold(h, CF_LIT(".localhost")) ||
           burrow__net_has_suffix_fold(h, CF_LIT(".localhost.localdomain"));
}

static bool cf_is_gateway(Str h) {
    return burrow__net_equal_fold(h, CF_LIT("_gateway"));
}

static bool cf_is_outbound(Str h) {
    return burrow__net_equal_fold(h, CF_LIT("_outbound"));
}

static bool cf_has_prefix(Str s, const char *prefix) {
    size_t n = strlen(prefix);
    return (size_t)s.len >= n && memcmp(s.p, prefix, n) == 0;
}

static bool cf_is(Str s, const char *lit) {
    return str_eq(s, str_from_cstr(lit));
}

/* The openbsd half of lookupOrder, from the lookup line of resolv.conf. */
static burrow__HostLookupOrder cf_openbsd(const burrow__DNSConfig *dc,
                                          burrow__HostLookupOrder fallback) {
    if (errors_is(dc->err, fs_err_not_exist))
        return BURROW__HOST_LOOKUP_FILES;
    const Str *lookup = (const Str *)dc->lookup.p;
    Int n = dc->lookup.len;
    if (n == 0)
        return BURROW__HOST_LOOKUP_DNS_FILES;
    if (n > 2)
        return fallback;
    if (cf_is(lookup[0], "bind")) {
        if (n == 2)
            return cf_is(lookup[1], "file") ? BURROW__HOST_LOOKUP_DNS_FILES : fallback;
        return BURROW__HOST_LOOKUP_DNS;
    }
    if (cf_is(lookup[0], "file")) {
        if (n == 2)
            return cf_is(lookup[1], "bind") ? BURROW__HOST_LOOKUP_FILES_DNS : fallback;
        return BURROW__HOST_LOOKUP_FILES;
    }
    return fallback;
}

/* What a source that is neither files nor dns means when cgo could take the
 * lookup: true to hand it to cgo, false to go on to the next source. */
static bool cf_cgo_source(const burrow__NetConf *c, Str hostname, Str source) {
    if (hostname.len > 0 && cf_is(source, "myhostname")) {
        if (cf_is_localhost(hostname) || cf_is_gateway(hostname) ||
            cf_is_outbound(hostname))
            return true;
        Arena ar;
        arena_init(&ar, NULL, 0);
        Error err = BURROW_NO_ERROR;
        Str hn = cf_get_hostname(arena_allocator(&ar), &err);
        bool cgo = BURROW_FAILED(err) || burrow__net_equal_fold(hostname, hn);
        arena_free(&ar);
        return cgo;
    }
    if (hostname.len > 0 && cf_has_prefix(source, "mdns")) {
        if (burrow__net_has_suffix_fold(hostname, CF_LIT(".local")))
            return true;
        bool have_mdns_allow = false;
        if (c->mdns_test == BURROW__MDNS_FROM_SYSTEM) {
            Arena ar;
            arena_init(&ar, NULL, 0);
            Error err = BURROW_NO_ERROR;
            (void)os_stat(arena_allocator(&ar), CF_LIT("/etc/mdns.allow"), &err);
            bool other = BURROW_FAILED(err) && !errors_is(err, fs_err_not_exist);
            have_mdns_allow = BURROW_OK(err);
            arena_free(&ar);
            if (other)
                return true;
        } else if (c->mdns_test == BURROW__MDNS_ASSUME_EXISTS) {
            have_mdns_allow = true;
        }
        return have_mdns_allow;
    }
    return true;
}

/* lookupOrder, with the nsswitch.conf half here. */
static burrow__HostLookupOrder cf_nss_order(const burrow__NetConf *c, Str hostname,
                                            bool can_use_cgo,
                                            burrow__HostLookupOrder fallback) {
    if (hostname.len > 0 && hostname.p[hostname.len - 1] == '.')
        hostname.len--;

    burrow__NssConf *nss = burrow__net_system_nss();
    if (nss == NULL)
        return fallback;
    Slice srcs_s = burrow__nss_conf_sources(nss, CF_LIT("hosts"));
    const burrow__NssSource *srcs = (const burrow__NssSource *)srcs_s.p;
    Int nsrc = srcs_s.len;
    burrow__HostLookupOrder ret = fallback;
    if (errors_is(nss->err, fs_err_not_exist) || (BURROW_OK(nss->err) && nsrc == 0)) {
        if (can_use_cgo && cf_goos(c, "solaris"))
            ret = BURROW__HOST_LOOKUP_CGO;
        else
            ret = BURROW__HOST_LOOKUP_FILES_DNS;
        burrow__nss_conf_put(nss);
        return ret;
    }
    if (BURROW_FAILED(nss->err)) {
        burrow__nss_conf_put(nss);
        return fallback;
    }

    bool has_dns_source = false;
    bool has_dns_source_checked = false;
    bool files_source = false;
    bool dns_source = false;
    Str first = BURROW_STR_EMPTY;
    for (Int i = 0; i < nsrc; i++) {
        const burrow__NssSource *src = &srcs[i];
        if (cf_is(src->source, "files") || cf_is(src->source, "dns")) {
            if (can_use_cgo && !burrow__nss_source_standard_criteria(src)) {
                burrow__nss_conf_put(nss);
                return BURROW__HOST_LOOKUP_CGO;
            }
            if (cf_is(src->source, "files")) {
                files_source = true;
            } else {
                has_dns_source = true;
                has_dns_source_checked = true;
                dns_source = true;
            }
            if (first.len == 0)
                first = src->source;
            continue;
        }

        if (can_use_cgo) {
            if (cf_cgo_source(c, hostname, src->source)) {
                burrow__nss_conf_put(nss);
                return BURROW__HOST_LOOKUP_CGO;
            }
            continue;
        }

        /* A source Go cannot follow, which it takes to be DNS unless the line
         * names dns itself further on. */
        if (!has_dns_source_checked) {
            has_dns_source_checked = true;
            for (Int j = i + 1; j < nsrc; j++) {
                if (cf_is(srcs[j].source, "dns")) {
                    has_dns_source = true;
                    break;
                }
            }
        }
        if (!has_dns_source) {
            dns_source = true;
            if (first.len == 0)
                first = CF_LIT("dns");
        }
    }

    if (files_source && dns_source)
        ret = cf_is(first, "files") ? BURROW__HOST_LOOKUP_FILES_DNS
                                    : BURROW__HOST_LOOKUP_DNS_FILES;
    else if (files_source)
        ret = BURROW__HOST_LOOKUP_FILES;
    else if (dns_source)
        ret = BURROW__HOST_LOOKUP_DNS;
    burrow__nss_conf_put(nss);
    return ret;
}

static burrow__HostLookupOrder cf_lookup_order(const burrow__NetConf *c,
                                               const NetResolver *r, Str hostname,
                                               burrow__DNSConfig **dns_conf) {
    *dns_conf = NULL;
    burrow__HostLookupOrder fallback;
    bool can_use_cgo;
    if (burrow__net_conf_must_use_go_resolver(c, r)) {
        fallback = BURROW__HOST_LOOKUP_FILES_DNS;
        can_use_cgo = false;
    } else if (c->net_cgo || c->prefer_cgo) {
        return BURROW__HOST_LOOKUP_CGO;
    } else {
        /* Go's resolver has no idea what to do with a backslash or a
         * percent sign in a name. */
        if (strings_index_byte(hostname, '\\') != -1 ||
            strings_index_byte(hostname, '%') != -1)
            return BURROW__HOST_LOOKUP_CGO;
        fallback = BURROW__HOST_LOOKUP_CGO;
        can_use_cgo = true;
    }

    if (cf_goos(c, "windows") || cf_goos(c, "plan9") || cf_goos(c, "android") ||
        cf_goos(c, "ios"))
        return fallback;

    burrow__DNSConfig *dc = burrow__net_system_dns_config();
    *dns_conf = dc;

    if (can_use_cgo && BURROW_FAILED(dc->err) &&
        !errors_is(dc->err, fs_err_not_exist) && !errors_is(dc->err, fs_err_permission))
        return BURROW__HOST_LOOKUP_CGO;
    if (can_use_cgo && dc->unknown_opt)
        return BURROW__HOST_LOOKUP_CGO;

    if (cf_goos(c, "openbsd"))
        return cf_openbsd(dc, fallback);

    return cf_nss_order(c, hostname, can_use_cgo, fallback);
}

static void cf_debug_order(const burrow__NetConf *c, const char *what, Str name,
                           burrow__HostLookupOrder ret) {
    if (c->dns_debug_level <= 1)
        return;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    cf_print(fmt_sprintf_v(a, "go package net: %s(%s) = %s\n", what, name,
                           burrow__host_lookup_order_string((int32_t)ret, a)));
    arena_free(&ar);
}

burrow__HostLookupOrder
burrow__net_conf_host_lookup_order(const burrow__NetConf *c, const NetResolver *r,
                                   Str hostname, burrow__DNSConfig **dns_conf) {
    burrow__HostLookupOrder ret = cf_lookup_order(c, r, hostname, dns_conf);
    cf_debug_order(c, "hostLookupOrder", hostname, ret);
    return ret;
}

burrow__HostLookupOrder
burrow__net_conf_addr_lookup_order(const burrow__NetConf *c, const NetResolver *r,
                                   Str addr, burrow__DNSConfig **dns_conf) {
    burrow__HostLookupOrder ret = cf_lookup_order(c, r, BURROW_STR_EMPTY, dns_conf);
    cf_debug_order(c, "addrLookupOrder", addr, ret);
    return ret;
}

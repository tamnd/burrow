/* The resolver's parts that need no name server, from Go's conf_test.go,
 * nss_test.go, addrselect_test.go, port_test.go and the tests in
 * lookup_test.go that stay on the machine.
 *
 * Copyright 2015 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/burrow.h"
#include "burrow/context.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/io/fs.h"
#include "burrow/mem/arena.h"
#include "burrow/net.h"
#include "burrow/net/netip.h"
#include "burrow/os.h"
#include "burrow/path.h"
#include "burrow/strings.h"
#include "burrow/sync/atomic.h"
#include "burrow/testing.h"
#include "burrow/time.h"

#include "../src/net/internal.h"
#include "check.h"

#include <stdint.h>
#include <string.h>

/* nssStr: s written to a file in dir and read back. */
static burrow__NssConf *nss_str(TestingT *t, Alloc *a, Str dir, Int n, const char *s) {
    Str name = path_join_v(a, 2, dir, fmt_sprintf_v(a, "nss%d", n));
    Str data = str_from_cstr(s);
    Error err = os_write_file(
        name, slice_from((void *)(uintptr_t)data.p, data.len, data.len, TYPE_BYTE),
        0644);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "WriteFile: %v", err);
        return NULL;
    }
    burrow__NssConf *conf = burrow__nss_parse_file(name);
    if (conf == NULL)
        testing_t_errorf_v(t, "parseNSSConfFile(%s): out of memory", name);
    return conf;
}

static Str temp_dir(TestingT *t, Alloc *a) {
    Error err = BURROW_NO_ERROR;
    Str dir = os_mkdir_temp(a, BURROW_STR_EMPTY, BURROW_S("net-lookup"), &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "MkdirTemp: %v", err);
        return BURROW_STR_EMPTY;
    }
    return dir;
}

static Time zero_time(void) {
    Time z;
    memset(&z, 0, sizeof z);
    return z;
}

/* defaultResolvConf, what a resolv.conf that is not there gives. */
static burrow__DNSConfig default_resolv_conf(void) {
    burrow__DNSConfig c;
    memset(&c, 0, sizeof c);
    c.servers =
        slice_from((void *)(uintptr_t)burrow__net_default_ns, 2, 2, TYPE_STRING);
    c.ndots = 1;
    c.timeout = 5;
    c.attempts = 2;
    c.err = fs_err_not_exist;
    return c;
}

/* --------------------------------------------------------- conf_test.go */

static const char *test_hostname_name = "myhostname";

static Str test_hostname(Alloc *a, Error *err) {
    *err = BURROW_NO_ERROR;
    return str_clone(a, str_from_cstr(test_hostname_name));
}

typedef struct NssHostTest {
    const char *host;
    const char *localhost;
    burrow__HostLookupOrder want;
} NssHostTest;

enum { NSS_NIL, NSS_NOT_EXIST, NSS_TEXT };
enum { RESOLV_DEFAULT, RESOLV_LOOKUP, RESOLV_UNKNOWN_OPT };

typedef struct ConfTest {
    const char *name;
    burrow__NetConf c;
    int nss_kind;
    const char *nss;
    bool prefer_go;
    int resolv_kind;
    const char *lookup[3];
    Int nlookup;
    NssHostTest host_tests[18];
    Int nhost_tests;
} ConfTest;

#define CGO BURROW__HOST_LOOKUP_CGO
#define FILES_DNS BURROW__HOST_LOOKUP_FILES_DNS
#define DNS_FILES BURROW__HOST_LOOKUP_DNS_FILES
#define FILES BURROW__HOST_LOOKUP_FILES
#define DNS BURROW__HOST_LOOKUP_DNS

/* Every conf has cgo_available set, since Go's test is written for a
 * system with cgo. */
static const ConfTest conf_tests[] = {
    {.name = "force",
     .c = {.net_cgo = true, .prefer_cgo = true, .cgo_available = true},
     .nss_kind = NSS_TEXT,
     .nss = "foo: bar",
     .host_tests = {{"foo.local", "myhostname", CGO},
                    {"google.com", "myhostname", CGO}},
     .nhost_tests = 2},
    {.name = "netgo_dns_before_files",
     .c = {.net_go = true, .cgo_available = true},
     .nss_kind = NSS_TEXT,
     .nss = "hosts: dns files",
     .host_tests = {{"x.com", "myhostname", DNS_FILES}},
     .nhost_tests = 1},
    {.name = "netgo_fallback_on_cgo",
     .c = {.net_go = true, .cgo_available = true},
     .nss_kind = NSS_TEXT,
     .nss = "hosts: dns files something_custom",
     .host_tests = {{"x.com", "myhostname", DNS_FILES}},
     .nhost_tests = 1},
    {.name = "ubuntu_trusty_avahi",
     .c = {.mdns_test = BURROW__MDNS_ASSUME_DOES_NOT_EXIST, .cgo_available = true},
     .nss_kind = NSS_TEXT,
     .nss = "hosts: files mdns4_minimal [NOTFOUND=return] dns mdns4",
     .host_tests = {{"foo.local", "myhostname", CGO},
                    {"foo.local.", "myhostname", CGO},
                    {"foo.LOCAL", "myhostname", CGO},
                    {"foo.LOCAL.", "myhostname", CGO},
                    {"google.com", "myhostname", FILES_DNS}},
     .nhost_tests = 5},
    {.name = "freebsdlinux_no_resolv_conf",
     .c = {.goos = BURROW_S_INIT("freebsd"), .cgo_available = true},
     .nss_kind = NSS_TEXT,
     .nss = "foo: bar",
     .host_tests = {{"google.com", "myhostname", FILES_DNS}},
     .nhost_tests = 1},
    /* On OpenBSD, no resolv.conf means no DNS. */
    {.name = "openbsd_no_resolv_conf",
     .c = {.goos = BURROW_S_INIT("openbsd"), .cgo_available = true},
     .host_tests = {{"google.com", "myhostname", FILES}},
     .nhost_tests = 1},
    {.name = "solaris_no_nsswitch",
     .c = {.goos = BURROW_S_INIT("solaris"), .cgo_available = true},
     .nss_kind = NSS_NOT_EXIST,
     .host_tests = {{"google.com", "myhostname", CGO}},
     .nhost_tests = 1},
    {.name = "openbsd_lookup_bind_file",
     .c = {.goos = BURROW_S_INIT("openbsd"), .cgo_available = true},
     .resolv_kind = RESOLV_LOOKUP,
     .lookup = {"bind", "file"},
     .nlookup = 2,
     .host_tests = {{"google.com", "myhostname", DNS_FILES},
                    {"foo.local", "myhostname", DNS_FILES}},
     .nhost_tests = 2},
    {.name = "openbsd_lookup_file_bind",
     .c = {.goos = BURROW_S_INIT("openbsd"), .cgo_available = true},
     .resolv_kind = RESOLV_LOOKUP,
     .lookup = {"file", "bind"},
     .nlookup = 2,
     .host_tests = {{"google.com", "myhostname", FILES_DNS}},
     .nhost_tests = 1},
    {.name = "openbsd_lookup_bind",
     .c = {.goos = BURROW_S_INIT("openbsd"), .cgo_available = true},
     .resolv_kind = RESOLV_LOOKUP,
     .lookup = {"bind"},
     .nlookup = 1,
     .host_tests = {{"google.com", "myhostname", DNS}},
     .nhost_tests = 1},
    {.name = "openbsd_lookup_file",
     .c = {.goos = BURROW_S_INIT("openbsd"), .cgo_available = true},
     .resolv_kind = RESOLV_LOOKUP,
     .lookup = {"file"},
     .nlookup = 1,
     .host_tests = {{"google.com", "myhostname", FILES}},
     .nhost_tests = 1},
    {.name = "openbsd_lookup_yp",
     .c = {.goos = BURROW_S_INIT("openbsd"), .cgo_available = true},
     .resolv_kind = RESOLV_LOOKUP,
     .lookup = {"file", "bind", "yp"},
     .nlookup = 3,
     .host_tests = {{"google.com", "myhostname", CGO}},
     .nhost_tests = 1},
    {.name = "openbsd_lookup_two",
     .c = {.goos = BURROW_S_INIT("openbsd"), .cgo_available = true},
     .resolv_kind = RESOLV_LOOKUP,
     .lookup = {"file", "foo"},
     .nlookup = 2,
     .host_tests = {{"google.com", "myhostname", CGO}},
     .nhost_tests = 1},
    {.name = "openbsd_lookup_empty",
     .c = {.goos = BURROW_S_INIT("openbsd"), .cgo_available = true},
     .resolv_kind = RESOLV_LOOKUP,
     .host_tests = {{"google.com", "myhostname", DNS_FILES}},
     .nhost_tests = 1},
    {.name = "linux_no_nsswitch.conf",
     .c = {.goos = BURROW_S_INIT("linux"), .cgo_available = true},
     .nss_kind = NSS_NOT_EXIST,
     .host_tests = {{"google.com", "myhostname", FILES_DNS}},
     .nhost_tests = 1},
    {.name = "linux_empty_nsswitch.conf",
     .c = {.goos = BURROW_S_INIT("linux"), .cgo_available = true},
     .nss_kind = NSS_TEXT,
     .nss = "",
     .host_tests = {{"google.com", "myhostname", FILES_DNS}},
     .nhost_tests = 1},
    {.name = "files_mdns_dns",
     .c = {.mdns_test = BURROW__MDNS_ASSUME_DOES_NOT_EXIST, .cgo_available = true},
     .nss_kind = NSS_TEXT,
     .nss = "hosts: files mdns dns",
     .host_tests = {{"x.com", "myhostname", FILES_DNS}, {"x.local", "myhostname", CGO}},
     .nhost_tests = 2},
    {.name = "dns_special_hostnames",
     .c = {.cgo_available = true},
     .nss_kind = NSS_TEXT,
     .nss = "hosts: dns",
     .host_tests = {{"x.com", "myhostname", DNS},
                    /* Punt on weird glibc escape, and IPv6 zones. */
                    {"x\\.com", "myhostname", CGO},
                    {"foo.com%en0", "myhostname", CGO}},
     .nhost_tests = 3},
    {.name = "mdns_allow",
     .c = {.mdns_test = BURROW__MDNS_ASSUME_EXISTS, .cgo_available = true},
     .nss_kind = NSS_TEXT,
     .nss = "hosts: files mdns dns",
     .host_tests = {{"x.com", "myhostname", CGO}, {"x.local", "myhostname", CGO}},
     .nhost_tests = 2},
    {.name = "files_dns",
     .c = {.cgo_available = true},
     .nss_kind = NSS_TEXT,
     .nss = "hosts: files dns",
     .host_tests = {{"x.com", "myhostname", FILES_DNS},
                    {"x", "myhostname", FILES_DNS},
                    {"x.local", "myhostname", FILES_DNS}},
     .nhost_tests = 3},
    {.name = "dns_files",
     .c = {.cgo_available = true},
     .nss_kind = NSS_TEXT,
     .nss = "hosts: dns files",
     .host_tests = {{"x.com", "myhostname", DNS_FILES},
                    {"x", "myhostname", DNS_FILES},
                    {"x.local", "myhostname", DNS_FILES}},
     .nhost_tests = 3},
    {.name = "something_custom",
     .c = {.cgo_available = true},
     .nss_kind = NSS_TEXT,
     .nss = "hosts: dns files something_custom",
     .host_tests = {{"x.com", "myhostname", CGO}},
     .nhost_tests = 1},
    {.name = "myhostname",
     .c = {.cgo_available = true},
     .nss_kind = NSS_TEXT,
     .nss = "hosts: files dns myhostname",
     .host_tests = {{"x.com", "myhostname", FILES_DNS},
                    {"myhostname", "myhostname", CGO},
                    {"myHostname", "myhostname", CGO},
                    {"myhostname.dot", "myhostname.dot", CGO},
                    {"myHostname.dot", "myhostname.dot", CGO},
                    {"_gateway", "myhostname", CGO},
                    {"_Gateway", "myhostname", CGO},
                    {"_outbound", "myhostname", CGO},
                    {"_Outbound", "myhostname", CGO},
                    {"localhost", "myhostname", CGO},
                    {"Localhost", "myhostname", CGO},
                    {"anything.localhost", "myhostname", CGO},
                    {"Anything.localhost", "myhostname", CGO},
                    {"localhost.localdomain", "myhostname", CGO},
                    {"Localhost.Localdomain", "myhostname", CGO},
                    {"anything.localhost.localdomain", "myhostname", CGO},
                    {"Anything.Localhost.Localdomain", "myhostname", CGO},
                    {"somehostname", "myhostname", FILES_DNS}},
     .nhost_tests = 18},
    {.name = "ubuntu14.04.02",
     .c = {.mdns_test = BURROW__MDNS_ASSUME_DOES_NOT_EXIST, .cgo_available = true},
     .nss_kind = NSS_TEXT,
     .nss = "hosts: files myhostname mdns4_minimal [NOTFOUND=return] dns mdns4",
     .host_tests = {{"x.com", "myhostname", FILES_DNS},
                    {"somehostname", "myhostname", FILES_DNS},
                    {"myhostname", "myhostname", CGO}},
     .nhost_tests = 3},
    /* Debian Squeeze is just "dns,files", but lists all the default criteria
     * for dns, but then has a non-standard but redundant notfound=return for
     * the files. */
    {.name = "debian_squeeze",
     .c = {.cgo_available = true},
     .nss_kind = NSS_TEXT,
     .nss = "hosts: dns [success=return notfound=continue unavail=continue "
            "tryagain=continue] files [notfound=return]",
     .host_tests = {{"x.com", "myhostname", DNS_FILES},
                    {"somehostname", "myhostname", DNS_FILES}},
     .nhost_tests = 2},
    {.name = "resolv.conf-unknown",
     .c = {.cgo_available = true},
     .nss_kind = NSS_TEXT,
     .nss = "foo: bar",
     .resolv_kind = RESOLV_UNKNOWN_OPT,
     .host_tests = {{"google.com", "myhostname", CGO}},
     .nhost_tests = 1},
    /* Issue 24393: make sure "Resolver.PreferGo = true" acts like netgo. */
    {.name = "resolver-prefergo",
     .c = {.net_cgo = true, .prefer_cgo = true, .cgo_available = true},
     .nss_kind = NSS_TEXT,
     .nss = "",
     .prefer_go = true,
     .host_tests = {{"localhost", "myhostname", FILES_DNS}},
     .nhost_tests = 1},
    {.name = "unknown-source",
     .c = {.cgo_available = true},
     .nss_kind = NSS_TEXT,
     .nss = "hosts: resolve files",
     .prefer_go = true,
     .host_tests = {{"x.com", "myhostname", DNS_FILES}},
     .nhost_tests = 1},
    {.name = "dns-among-unknown-sources",
     .c = {.cgo_available = true},
     .nss_kind = NSS_TEXT,
     .nss = "hosts: mymachines files dns",
     .prefer_go = true,
     .host_tests = {{"x.com", "myhostname", FILES_DNS}},
     .nhost_tests = 1},
    {.name = "dns-among-unknown-sources-2",
     .c = {.cgo_available = true},
     .nss_kind = NSS_TEXT,
     .nss = "hosts: dns mymachines files",
     .prefer_go = true,
     .host_tests = {{"x.com", "myhostname", DNS_FILES}},
     .nhost_tests = 1},
};

static burrow__DNSConfig conf_test_resolv(Alloc *a, const ConfTest *tt) {
    burrow__DNSConfig c = default_resolv_conf();
    if (tt->resolv_kind == RESOLV_LOOKUP) {
        memset(&c, 0, sizeof c);
        c.servers =
            slice_from((void *)(uintptr_t)burrow__net_default_ns, 2, 2, TYPE_STRING);
        if (tt->nlookup > 0) {
            Str *l =
                (Str *)mem_alloc(a, sizeof(Str) * (size_t)tt->nlookup, _Alignof(Str));
            if (l != NULL) {
                for (Int i = 0; i < tt->nlookup; i++)
                    l[i] = str_from_cstr(tt->lookup[i]);
                c.lookup = slice_from(l, tt->nlookup, tt->nlookup, TYPE_STRING);
            }
        }
    } else if (tt->resolv_kind == RESOLV_UNKNOWN_OPT) {
        c.err = BURROW_NO_ERROR;
        c.unknown_opt = true;
    }
    return c;
}

static void TestConfHostLookupOrder(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str dir = temp_dir(t, a);
    if (dir.len == 0) {
        arena_free(&ar);
        return;
    }
    burrow__NssConf *orig_nss = burrow__net_system_nss();
    Str missing = path_join_v(a, 2, dir, BURROW_S("does-not-exist"));

    for (size_t k = 0; k < sizeof conf_tests / sizeof conf_tests[0]; k++) {
        const ConfTest *tt = &conf_tests[k];
        burrow__NssConf *nss = NULL;
        if (tt->nss_kind == NSS_TEXT)
            nss = nss_str(t, a, dir, (Int)k, tt->nss);
        else if (tt->nss_kind == NSS_NOT_EXIST)
            nss = burrow__nss_parse_file(missing);
        burrow__DNSConfig resolv = conf_test_resolv(a, tt);
        burrow__net_force_dns_config(&resolv, burrow__net_distant_future());

        NetResolver r;
        memset(&r, 0, sizeof r);
        r.prefer_go = true;
        for (Int i = 0; i < tt->nhost_tests; i++) {
            const NssHostTest *ht = &tt->host_tests[i];
            test_hostname_name = ht->localhost;
            burrow__net_set_get_hostname(test_hostname);
            if (nss != NULL)
                sync_atomic_add_int32(&nss->refs, 1);
            burrow__net_set_system_nss(nss, TIME_HOUR);

            burrow__DNSConfig *dc = NULL;
            burrow__HostLookupOrder got = burrow__net_conf_host_lookup_order(
                &tt->c, tt->prefer_go ? &r : NULL, str_from_cstr(ht->host), &dc);
            if (dc != NULL)
                burrow__dns_config_put(dc);
            if (got != ht->want)
                testing_t_errorf_v(
                    t, "%s: hostLookupOrder(%q) = %s; want %s", tt->name,
                    str_from_cstr(ht->host),
                    burrow__host_lookup_order_string((int32_t)got, a),
                    burrow__host_lookup_order_string((int32_t)ht->want, a));
        }
        burrow__nss_conf_put(nss);
    }

    burrow__net_set_get_hostname(NULL);
    burrow__net_set_system_nss(orig_nss, 0);
    burrow__net_force_dns_config_file(BURROW_S("/etc/resolv.conf"), zero_time());
    (void)os_remove_all(dir);
    arena_free(&ar);
}

static void TestAddrLookupOrder(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str dir = temp_dir(t, a);
    if (dir.len == 0) {
        arena_free(&ar);
        return;
    }
    burrow__NssConf *orig_nss = burrow__net_system_nss();
    burrow__DNSConfig resolv = default_resolv_conf();
    burrow__net_force_dns_config(&resolv, time_add(time_now(), TIME_HOUR));

    static const char *const nss[] = {"hosts: files myhostname dns",
                                      "hosts: files mdns4 dns"};
    burrow__NetConf cnf;
    memset(&cnf, 0, sizeof cnf);
    cnf.cgo_available = true;
    for (size_t i = 0; i < sizeof nss / sizeof nss[0]; i++) {
        burrow__net_set_system_nss(nss_str(t, a, dir, (Int)i, nss[i]), TIME_HOUR);
        burrow__DNSConfig *dc = NULL;
        burrow__HostLookupOrder order =
            burrow__net_conf_addr_lookup_order(&cnf, NULL, BURROW_S("192.0.2.1"), &dc);
        if (dc != NULL)
            burrow__dns_config_put(dc);
        if (order != CGO)
            testing_t_errorf_v(t, "addrLookupOrder returned: %s, want cgo",
                               burrow__host_lookup_order_string((int32_t)order, a));
    }

    burrow__net_set_system_nss(orig_nss, 0);
    burrow__net_force_dns_config_file(BURROW_S("/etc/resolv.conf"), zero_time());
    (void)os_remove_all(dir);
    arena_free(&ar);
}

static void TestSystemConf(TestingT *t) {
    (void)t;
    (void)burrow__net_system_conf();
}

/* ---------------------------------------------------------- nss_test.go */

/* A conf as text, a line for each database in the order the file has them,
 * which is what the comparison needs and Go's map does not keep. */
static Str nss_conf_string(Alloc *a, const burrow__NssConf *conf) {
    Str out = BURROW_STR_EMPTY;
    const burrow__NssDatabase *dbs = (const burrow__NssDatabase *)conf->dbs.p;
    for (Int i = 0; i < conf->dbs.len; i++) {
        if (i > 0)
            out = burrow__net_cat(a, 2, out, BURROW_S("\n"));
        out = burrow__net_cat(a, 3, out, dbs[i].name, BURROW_S(":"));
        const burrow__NssSource *srcs = (const burrow__NssSource *)dbs[i].sources.p;
        for (Int j = 0; j < dbs[i].sources.len; j++) {
            out = burrow__net_cat(a, 3, out, BURROW_S(" "), srcs[j].source);
            const burrow__NssCriterion *cr =
                (const burrow__NssCriterion *)srcs[j].criteria.p;
            for (Int k = 0; k < srcs[j].criteria.len; k++) {
                out = burrow__net_cat(a, 6, out, str_from_cstr(k == 0 ? "[" : " "),
                                      str_from_cstr(cr[k].negate ? "!" : ""),
                                      cr[k].status, BURROW_S("="), cr[k].action);
                if (k == srcs[j].criteria.len - 1)
                    out = burrow__net_cat(a, 2, out, BURROW_S("]"));
            }
        }
    }
    return out;
}

static const char ubuntu_trusty_avahi[] =
    "# /etc/nsswitch.conf\n"
    "#\n"
    "# Example configuration of GNU Name Service Switch functionality.\n"
    "# If you have the libc-doc-reference' and nfo' packages installed, try:\n"
    "# nfo libc \"Name Service Switch\"' for information about this file.\n"
    "\n"
    "passwd:         compat\n"
    "group:          compat\n"
    "shadow:         compat\n"
    "\n"
    "hosts:          files mdns4_minimal [NOTFOUND=return] dns mdns4\n"
    "networks:       files\n"
    "\n"
    "protocols:      db files\n"
    "services:       db files\n"
    "ethers:         db files\n"
    "rpc:            db files\n"
    "\n"
    "netgroup:       nis\n";

static void TestParseNSSConf(TestingT *t) {
    static const struct {
        const char *name;
        const char *in;
        const char *want;
    } tests[] = {
        {"no_newline", "foo: a b", "foo: a b"},
        {"newline", "foo: a b\n", "foo: a b"},
        {"whitespace", "   foo:a    b    \n", "foo: a b"},
        {"comment1", "   foo:a    b#c\n", "foo: a b"},
        {"comment2", "   foo:a    b #c \n", "foo: a b"},
        {"crit", "   foo:a    b [!a=b    X=Y ] c#d \n", "foo: a b[!a=b x=y] c"},
        /* Ubuntu Trusty w/ avahi-daemon, libavahi-* etc installed. */
        {"ubuntu_trusty_avahi", ubuntu_trusty_avahi,
         "passwd: compat\n"
         "group: compat\n"
         "shadow: compat\n"
         "hosts: files mdns4_minimal[notfound=return] dns mdns4\n"
         "networks: files\n"
         "protocols: db files\n"
         "services: db files\n"
         "ethers: db files\n"
         "rpc: db files\n"
         "netgroup: nis"},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str dir = temp_dir(t, a);
    if (dir.len == 0) {
        arena_free(&ar);
        return;
    }
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        burrow__NssConf *conf = nss_str(t, a, dir, (Int)i, tests[i].in);
        if (conf == NULL)
            continue;
        Str got = nss_conf_string(a, conf);
        if (BURROW_FAILED(conf->err))
            testing_t_errorf_v(t, "%s: err = %v", tests[i].name, conf->err);
        else if (!str_eq(got, str_from_cstr(tests[i].want)))
            testing_t_errorf_v(t, "%s: mismatch\n got %q\nwant %q", tests[i].name, got,
                               str_from_cstr(tests[i].want));
        burrow__nss_conf_put(conf);
    }
    (void)os_remove_all(dir);
    arena_free(&ar);
}

/* --------------------------------------------------- addrselect_test.go */

typedef struct SortTest {
    const char *in[6];
    const char *srcs[6];
    const char *want[6];
    Int n;
    bool reverse; /* also test it starting backwards */
} SortTest;

static bool ip_addrs_eq(const NetIPAddr *x, const NetIPAddr *y, Int n) {
    for (Int i = 0; i < n; i++)
        if (!net_ip_equal(x[i].ip, y[i].ip) || !str_eq(x[i].zone, y[i].zone))
            return false;
    return true;
}

static Str ip_addrs_string(Alloc *a, const NetIPAddr *x, Int n) {
    Str out = BURROW_S("[");
    for (Int i = 0; i < n; i++)
        out = burrow__net_cat(a, 3, out, str_from_cstr(i > 0 ? " " : ""),
                              net_ip_string(x[i].ip, a));
    return burrow__net_cat(a, 2, out, BURROW_S("]"));
}

static void TestSortByRFC6724(TestingT *t) {
    static const SortTest tests[] = {
        /* Examples from RFC 6724 section 10.2: Prefer a matching scope over
         * a matching label. */
        {{"2001:db8:1::1", "198.51.100.121"},
         {"2001:db8:1::2", "169.254.13.78"},
         {"2001:db8:1::1", "198.51.100.121"},
         2,
         true},
        /* Prefer a matching scope. */
        {{"2001:db8:1::1", "198.51.100.121"},
         {"fe80::1", "198.51.100.117"},
         {"198.51.100.121", "2001:db8:1::1"},
         2,
         true},
        /* Prefer higher precedence. */
        {{"2001:db8:1::1", "10.1.2.3"},
         {"2001:db8:1::2", "10.1.2.4"},
         {"2001:db8:1::1", "10.1.2.3"},
         2,
         true},
        /* Prefer smaller scope. */
        {{"2001:db8:1::1", "fe80::1"},
         {"2001:db8:1::2", "fe80::2"},
         {"fe80::1", "2001:db8:1::1"},
         2,
         true},
        /* Issue 13283. Having a 10/8 source address does not mean we should
         * prefer 23/8 destination addresses. */
        {{"54.83.193.112", "184.72.238.214", "23.23.172.185", "75.101.148.21",
          "23.23.134.56", "23.21.50.150"},
         {"10.2.3.4", "10.2.3.4", "10.2.3.4", "10.2.3.4", "10.2.3.4", "10.2.3.4"},
         {"54.83.193.112", "184.72.238.214", "23.23.172.185", "75.101.148.21",
          "23.23.134.56", "23.21.50.150"},
         6,
         false},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        const SortTest *tt = &tests[i];
        NetIPAddr in[6], got[6], want[6];
        NetipAddr srcs[6];
        memset(in, 0, sizeof in);
        memset(want, 0, sizeof want);
        for (Int j = 0; j < tt->n; j++) {
            in[j].ip = net_parse_ip(a, str_from_cstr(tt->in[j]));
            want[j].ip = net_parse_ip(a, str_from_cstr(tt->want[j]));
            srcs[j] = netip_must_parse_addr(str_from_cstr(tt->srcs[j]));
        }
        memcpy(got, in, sizeof got);
        burrow__net_sort_by_rfc6724_with_srcs(got, srcs, tt->n);
        if (!ip_addrs_eq(got, want, tt->n))
            testing_t_errorf_v(t, "test %d:\nin = %s\ngot: %s\nwant: %s\n", (Int)i,
                               ip_addrs_string(a, in, tt->n),
                               ip_addrs_string(a, got, tt->n),
                               ip_addrs_string(a, want, tt->n));
        if (!tt->reverse)
            continue;
        for (Int j = 0; j < tt->n; j++) {
            got[j] = in[tt->n - j - 1];
            srcs[j] = netip_must_parse_addr(str_from_cstr(tt->srcs[tt->n - j - 1]));
        }
        burrow__net_sort_by_rfc6724_with_srcs(got, srcs, tt->n);
        if (!ip_addrs_eq(got, want, tt->n))
            testing_t_errorf_v(
                t, "test %d, starting backwards:\nin = %s\ngot: %s\nwant: %s\n", (Int)i,
                ip_addrs_string(a, in, tt->n), ip_addrs_string(a, got, tt->n),
                ip_addrs_string(a, want, tt->n));
    }
    arena_free(&ar);
}

static void TestRFC6724PolicyTableClassify(TestingT *t) {
    static const struct {
        const char *ip;
        const char *prefix;
        uint8_t precedence;
        uint8_t label;
    } tests[] = {
        {"127.0.0.1", "::ffff:0:0/96", 35, 4},
        {"2601:645:8002:a500:986f:1db8:c836:bd65", "::/0", 40, 1},
        {"::1", "::1/128", 50, 0},
        {"2002::ab12", "2002::/16", 30, 2},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        NetipAddr ip = netip_must_parse_addr(str_from_cstr(tests[i].ip));
        NetipPrefix want = netip_must_parse_prefix(str_from_cstr(tests[i].prefix));
        burrow__NetPolicyEntry got = burrow__net_policy_classify(ip);
        if (!netip_prefix_eq(got.prefix, want) ||
            got.precedence != tests[i].precedence || got.label != tests[i].label)
            testing_t_errorf_v(t, "%d. Classify(%s) = {%s %d %d}; want {%s %d %d}",
                               (Int)i, str_from_cstr(tests[i].ip),
                               netip_prefix_string(got.prefix, a), (Int)got.precedence,
                               (Int)got.label, str_from_cstr(tests[i].prefix),
                               (Int)tests[i].precedence, (Int)tests[i].label);
    }
    arena_free(&ar);
}

static void TestRFC6724ClassifyScope(TestingT *t) {
    static const Byte v6_e0[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xe0, 0, 0, 0};
    static const Byte v6_e2[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xe0, 2, 2, 2};
    static const Byte v4_e0[4] = {0xe0, 0, 0, 0};
    static const Byte v4_e2[4] = {0xe0, 2, 2, 2};
    struct {
        NetipAddr ip;
        uint8_t want;
    } tests[] = {
        {netip_must_parse_addr(BURROW_S("127.0.0.1")), BURROW__NET_SCOPE_LINK_LOCAL},
        {netip_must_parse_addr(BURROW_S("::1")), BURROW__NET_SCOPE_LINK_LOCAL},
        {netip_must_parse_addr(BURROW_S("169.254.1.2")), BURROW__NET_SCOPE_LINK_LOCAL},
        {netip_must_parse_addr(BURROW_S("fec0::1")), BURROW__NET_SCOPE_SITE_LOCAL},
        {netip_must_parse_addr(BURROW_S("8.8.8.8")), BURROW__NET_SCOPE_GLOBAL},

        /* IPv6 multicast */
        {netip_must_parse_addr(BURROW_S("ff02::")), BURROW__NET_SCOPE_LINK_LOCAL},
        {netip_must_parse_addr(BURROW_S("ff05::")), BURROW__NET_SCOPE_SITE_LOCAL},
        {netip_must_parse_addr(BURROW_S("ff04::")), BURROW__NET_SCOPE_ADMIN_LOCAL},
        {netip_must_parse_addr(BURROW_S("ff0e::")), BURROW__NET_SCOPE_GLOBAL},

        /* IPv4 link-local and global multicast, as 16 bytes and as 4. */
        {netip_addr_from16(v6_e0), BURROW__NET_SCOPE_GLOBAL},
        {netip_addr_from16(v6_e2), BURROW__NET_SCOPE_GLOBAL},
        {netip_addr_from4(v4_e0), BURROW__NET_SCOPE_GLOBAL},
        {netip_addr_from4(v4_e2), BURROW__NET_SCOPE_GLOBAL},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        uint8_t got = burrow__net_classify_scope(tests[i].ip);
        if (got != tests[i].want)
            testing_t_errorf_v(t, "%d. classifyScope(%s) = %x; want %x", (Int)i,
                               netip_addr_string(tests[i].ip, a), (Int)got,
                               (Int)tests[i].want);
    }
    arena_free(&ar);
}

static void TestRFC6724CommonPrefixLength(TestingT *t) {
    static const struct {
        const char *a;
        const char *b;
        Int want;
        Byte a4[4];
        Byte b4[4];
    } tests[] = {
        {"fe80::1", "fe80::2", 64, {0}, {0}},
        {"fe81::1", "fe80::2", 15, {0}, {0}},
        {"127.0.0.1", "fe80::1", 0, {0}, {0}}, /* diff size */
        {NULL, NULL, 32, {1, 2, 3, 4}, {1, 2, 3, 4}},
        {NULL, NULL, 16, {1, 2, 255, 255}, {1, 2, 0, 0}},
        {NULL, NULL, 17, {1, 2, 127, 255}, {1, 2, 0, 0}},
        {NULL, NULL, 18, {1, 2, 63, 255}, {1, 2, 0, 0}},
        {NULL, NULL, 19, {1, 2, 31, 255}, {1, 2, 0, 0}},
        {NULL, NULL, 20, {1, 2, 15, 255}, {1, 2, 0, 0}},
        {NULL, NULL, 21, {1, 2, 7, 255}, {1, 2, 0, 0}},
        {NULL, NULL, 22, {1, 2, 3, 255}, {1, 2, 0, 0}},
        {NULL, NULL, 23, {1, 2, 1, 255}, {1, 2, 0, 0}},
        {NULL, NULL, 24, {1, 2, 0, 255}, {1, 2, 0, 0}},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        NetipAddr x = tests[i].a != NULL
                          ? netip_must_parse_addr(str_from_cstr(tests[i].a))
                          : netip_addr_from4(tests[i].a4);
        /* IP{1, 2, 3, 4}, the four byte form ParseIP never gives. */
        NetIP y = tests[i].b != NULL
                      ? net_parse_ip(a, str_from_cstr(tests[i].b))
                      : slice_from((void *)(uintptr_t)tests[i].b4, 4, 4, TYPE_BYTE);
        Int got = burrow__net_common_prefix_len(x, y);
        if (got != tests[i].want)
            testing_t_errorf_v(t, "%d. commonPrefixLen(%s, %s) = %d; want %d", (Int)i,
                               netip_addr_string(x, a), net_ip_string(y, a), got,
                               tests[i].want);
    }
    arena_free(&ar);
}

/* --------------------------------------------------------- port_test.go */

static void TestParsePort(TestingT *t) {
    static const struct {
        const char *service;
        Int port;
        bool needs_lookup;
    } tests[] = {
        {"", 0, false},

        /* Decimal number literals */
        {"-1073741825", -(1 << 30), false},
        {"-1073741824", -(1 << 30), false},
        {"-1073741823", -((1 << 30) - 1), false},
        {"-123456789", -123456789, false},
        {"-1", -1, false},
        {"-0", 0, false},
        {"0", 0, false},
        {"+0", 0, false},
        {"+1", 1, false},
        {"65535", 65535, false},
        {"65536", 65536, false},
        {"123456789", 123456789, false},
        {"1073741822", (1 << 30) - 2, false},
        {"1073741823", (1 << 30) - 1, false},
        {"1073741824", (1 << 30) - 1, false},
        {"1073741825", (1 << 30) - 1, false},

        /* Others */
        {"abc", 0, true},
        {"9pfs", 0, true},
        {"123badport", 0, true},
        {"bad123port", 0, true},
        {"badport123", 0, true},
        {"123456789badport", 0, true},
        {"-2147483649badport", 0, true},
        {"2147483649badport", 0, true},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Int port = 0;
        bool needs_lookup =
            burrow__net_parse_port(str_from_cstr(tests[i].service), &port);
        if (port != tests[i].port || needs_lookup != tests[i].needs_lookup)
            testing_t_errorf_v(t, "parsePort(%q) = %d, %t; want %d, %t",
                               str_from_cstr(tests[i].service), port, needs_lookup,
                               tests[i].port, tests[i].needs_lookup);
    }
}

/* ------------------------------------------------------- lookup_test.go */

static bool is_dns_error(Error err) {
    return errors_as(err, TYPE_NET_DNS_ERROR) != NULL;
}

static void TestLookupPort(TestingT *t) {
    struct {
        Str network;
        Str name;
        Int port;
        bool ok;
    } tests[] = {
        {BURROW_S("tcp"), BURROW_S("0"), 0, true},
        {BURROW_S("udp"), BURROW_S("0"), 0, true},
        {BURROW_S("udp"), BURROW_S("domain"), 53, true},

        {BURROW_S("--badnet--"), BURROW_S("zzz"), 0, false},
        {BURROW_S("tcp"), BURROW_S("--badport--"), 0, false},
        {BURROW_S("tcp"), BURROW_S("-1"), 0, false},
        {BURROW_S("tcp"), BURROW_S("65536"), 0, false},
        {BURROW_S("udp"), BURROW_S("-1"), 0, false},
        {BURROW_S("udp"), BURROW_S("65536"), 0, false},
        {BURROW_S("tcp"), BURROW_S("123456789"), 0, false},
        {BURROW_S("tcp"), BURROW_S("bad\0port"), 0, false},

        /* Issue 13610: LookupPort("tcp", "") */
        {BURROW_S("tcp"), BURROW_S(""), 0, true},
        {BURROW_S("tcp4"), BURROW_S(""), 0, true},
        {BURROW_S("tcp6"), BURROW_S(""), 0, true},
        {BURROW_S("udp"), BURROW_S(""), 0, true},
        {BURROW_S("udp4"), BURROW_S(""), 0, true},
        {BURROW_S("udp6"), BURROW_S(""), 0, true},

        {BURROW_S("tcp"), BURROW_S("http"), 80, true},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Error err = BURROW_NO_ERROR;
        Int port = net_lookup_port(tests[i].network, tests[i].name, &err);
        if (port != tests[i].port || BURROW_OK(err) != tests[i].ok)
            testing_t_errorf_v(t, "LookupPort(%q, %q) = %d, %v; want %d, error=%t",
                               tests[i].network, tests[i].name, port, err,
                               tests[i].port, !tests[i].ok);
        /* parseLookupPortError */
        if (BURROW_FAILED(err) && !is_dns_error(err) &&
            errors_as(err, TYPE_NET_ADDR_ERROR) == NULL)
            testing_t_errorf_v(t, "unexpected type: %v", err);
    }
}

/* Like TestLookupPort but with minimal tests that should always pass because
 * the answers are baked-in to the net package. */
static void TestLookupPort_Minimal(TestingT *t) {
    static const struct {
        const char *network;
        const char *name;
        Int port;
    } tests[] = {
        {"tcp", "http", 80},   {"tcp", "HTTP", 80}, /* case shouldn't matter */
        {"tcp", "https", 443}, {"tcp", "ssh", 22},   {"tcp", "gopher", 70},
        {"tcp4", "http", 80},  {"tcp6", "http", 80},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Error err = BURROW_NO_ERROR;
        Str network = str_from_cstr(tests[i].network),
            name = str_from_cstr(tests[i].name);
        Int port = net_lookup_port(network, name, &err);
        if (port != tests[i].port || BURROW_FAILED(err))
            testing_t_errorf_v(t, "LookupPort(%q, %q) = %d, %v; want %d, error=nil",
                               network, name, port, err, tests[i].port);
    }
}

static void TestLookupProtocol_Minimal(TestingT *t) {
    static const struct {
        const char *name;
        Int want;
    } tests[] = {
        {"tcp", 6},  {"TcP", 6}, /* case shouldn't matter */
        {"icmp", 1}, {"igmp", 2}, {"udp", 17}, {"ipv6-icmp", 58},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Error err = BURROW_NO_ERROR;
        Str name = str_from_cstr(tests[i].name);
        Int got = burrow__net_lookup_protocol(name, &err);
        if (got != tests[i].want || BURROW_FAILED(err))
            testing_t_errorf_v(t, "LookupProtocol(%q) = %d, %v; want %d, error=nil",
                               name, got, err, tests[i].want);
    }
}

/* LookupHost on a name that is not letters, digits and hyphens is not found,
 * rather than an invalid name. */
static void TestLookupNonLDH(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Error err = BURROW_NO_ERROR;
    Slice addrs =
        net_lookup_host(arena_allocator(&ar), BURROW_S("!!!.###.bogus..domain."), &err);
    if (BURROW_OK(err)) {
        testing_t_fatalf_v(t, "lookup succeeded: %d addresses", addrs.len);
        arena_free(&ar);
        return;
    }
    if (!strings_has_suffix(error_text(err), BURROW_S("no such host")))
        testing_t_fatalf_v(t, "lookup error = %v, want %v", err,
                           burrow__net_err_no_such_host);
    const NetDNSError *de = (const NetDNSError *)errors_as(err, TYPE_NET_DNS_ERROR);
    if (de == NULL || !de->is_not_found)
        testing_t_fatalf_v(t, "lookup error = %v, want IsNotFound", err);
    arena_free(&ar);
}

/* Issue 53995: Resolver.LookupIP should return error for empty host name. */
static void TestResolverLookupIPWithEmptyHost(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Error err = BURROW_NO_ERROR;
    (void)net_resolver_lookup_ip(NULL, arena_allocator(&ar), context_background(),
                                 BURROW_S("ip"), BURROW_STR_EMPTY, &err);
    if (BURROW_OK(err))
        testing_t_fatal_v(
            t, "DefaultResolver.LookupIP for empty host success, want no host "
               "error");
    else if (!strings_has_suffix(error_text(err), BURROW_S("no such host")))
        testing_t_fatalf_v(t, "lookup error = %v, want %v", err,
                           burrow__net_err_no_such_host);
    arena_free(&ar);
}

/* Issue 31597: don't panic on null byte in name. */
static void TestLookupPortNotFound(TestingT *t) {
    Error err = BURROW_NO_ERROR;
    (void)net_lookup_port(BURROW_S("udp"), BURROW_S("_-unknown-service-"), &err);
    const NetDNSError *de = (const NetDNSError *)errors_as(err, TYPE_NET_DNS_ERROR);
    if (de == NULL || !de->is_not_found)
        testing_t_fatalf_v(t, "unexpected error: %v", err);
}

/* The submissions service is only available through a tcp network, see
 * https://www.iana.org/assignments/service-names-port-numbers/. */
static void TestLookupPortDifferentNetwork(TestingT *t) {
    Error err = BURROW_NO_ERROR;
    (void)net_lookup_port(BURROW_S("udp"), BURROW_S("submissions"), &err);
    const NetDNSError *de = (const NetDNSError *)errors_as(err, TYPE_NET_DNS_ERROR);
    if (de == NULL || !de->is_not_found)
        testing_t_fatalf_v(t, "unexpected error: %v", err);
}

static void TestLookupPortEmptyNetworkString(TestingT *t) {
    Error err = BURROW_NO_ERROR;
    (void)net_lookup_port(BURROW_STR_EMPTY, BURROW_S("submissions"), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "unexpected error: %v", err);
}

static void TestLookupPortIPNetworkString(TestingT *t) {
    Error err = BURROW_NO_ERROR;
    (void)net_lookup_port(BURROW_S("ip"), BURROW_S("submissions"), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "unexpected error: %v", err);
}

static NetConn dial_fails(void *env, Alloc *a, Context ctx, Str network, Str address,
                          bool *packet, Error *err) {
    (void)a;
    (void)ctx;
    (void)network;
    (void)address;
    (void)packet;
    *err = *(const Error *)env;
    NetConn c;
    memset(&c, 0, sizeof c);
    return c;
}

static void TestDNSErrorUnwrap(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    NetResolver r_deadline_exceeded, r_cancelled;
    memset(&r_deadline_exceeded, 0, sizeof r_deadline_exceeded);
    memset(&r_cancelled, 0, sizeof r_cancelled);
    r_deadline_exceeded.prefer_go = true;
    r_deadline_exceeded.dial = BURROW_FN(NetResolverDial, dial_fails,
                                         (void *)(uintptr_t)&context_deadline_exceeded);
    r_cancelled.prefer_go = true;
    r_cancelled.dial =
        BURROW_FN(NetResolverDial, dial_fails, (void *)(uintptr_t)&context_canceled);

    Error err = BURROW_NO_ERROR;
    (void)net_resolver_lookup_host(&r_deadline_exceeded, a, context_background(),
                                   BURROW_S("test.go.dev"), &err);
    if (!errors_is(err, context_deadline_exceeded))
        testing_t_errorf_v(
            t, "errors.Is(err, context.DeadlineExceeded) = false; want = true");

    err = BURROW_NO_ERROR;
    (void)net_resolver_lookup_host(&r_cancelled, a, context_background(),
                                   BURROW_S("test.go.dev"), &err);
    if (!errors_is(err, context_canceled))
        testing_t_errorf_v(t, "errors.Is(err, context.Canceled) = false; want = true");

    ContextCancelFunc cancel;
    Context ctx = context_with_cancel(a, context_background(), &cancel);
    if (BURROW_CONTEXT_IS_NIL(ctx)) {
        testing_t_fatal_v(t, "out of memory");
        arena_free(&ar);
        return;
    }
    BURROW_CALLF0(cancel);
    NetResolver r;
    memset(&r, 0, sizeof r);
    r.prefer_go = true;
    err = BURROW_NO_ERROR;
    (void)net_resolver_lookup_host(&r, a, ctx, BURROW_S("text.go.dev"), &err);
    if (!errors_is(err, context_canceled))
        testing_t_errorf_v(t, "errors.Is(err, context.Canceled) = false; want = true");
    context_release(ctx);
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestConfHostLookupOrder)                                                         \
    X(TestAddrLookupOrder)                                                             \
    X(TestSystemConf)                                                                  \
    X(TestParseNSSConf)                                                                \
    X(TestSortByRFC6724)                                                               \
    X(TestRFC6724PolicyTableClassify)                                                  \
    X(TestRFC6724ClassifyScope)                                                        \
    X(TestRFC6724CommonPrefixLength)                                                   \
    X(TestParsePort)                                                                   \
    X(TestLookupPort)                                                                  \
    X(TestLookupPort_Minimal)                                                          \
    X(TestLookupProtocol_Minimal)                                                      \
    X(TestLookupNonLDH)                                                                \
    X(TestResolverLookupIPWithEmptyHost)                                               \
    X(TestLookupPortNotFound)                                                          \
    X(TestLookupPortDifferentNetwork)                                                  \
    X(TestLookupPortEmptyNetworkString)                                                \
    X(TestLookupPortIPNetworkString)                                                   \
    X(TestDNSErrorUnwrap)

TESTING_MAIN(TESTS)

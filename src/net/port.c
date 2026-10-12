/* Ports and protocols by name: parsePort, the services and protocols tables
 * and what reads /etc/services and /etc/protocols into them.
 *
 * Derived from Go's src/net/port.go, port_unix.go, the tables and
 * lookupPortMap in lookup.go and readProtocols in lookup_unix.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "internal.h"

#include "burrow/error.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/net.h"
#include "burrow/slice.h"
#include "burrow/strings.h"
#include "burrow/sync.h"

#include <stdint.h>
#include <string.h>

#define PO_LIT(s) str_from_bytes((const Byte *)(s), (Int)sizeof(s) - 1)

/* ---------------------------------------------------------------- parsePort */

bool burrow__net_parse_port(Str service, Int *port) {
    *port = 0;
    if (service.len == 0)
        return false; /* Go 1.0 took "" for 0 and so do we */
    const uint32_t max = UINT32_MAX;
    const uint32_t cutoff = (uint32_t)1 << 30;
    bool neg = false;
    Int i = 0;
    if (service.p[0] == '+') {
        i = 1;
    } else if (service.p[0] == '-') {
        neg = true;
        i = 1;
    }
    uint32_t n = 0;
    for (; i < service.len; i++) {
        Byte d = service.p[i];
        if (d < '0' || d > '9')
            return true;
        if (n >= cutoff) {
            n = max;
            break;
        }
        n *= 10;
        uint32_t nn = n + (uint32_t)(d - '0');
        if (nn < n) {
            n = max;
            break;
        }
        n = nn;
    }
    /* Anything that does not fit comes out as cutoff-1 or -cutoff, which no
     * caller takes for a port, the same as Go. A digit after the overflow
     * is still not checked, also as in Go. */
    Int p;
    if (!neg && n >= cutoff)
        p = (Int)(cutoff - 1);
    else if (neg && n > cutoff)
        p = (Int)cutoff;
    else
        p = (Int)n;
    *port = neg ? -p : p;
    return false;
}

/* ------------------------------------------------------------------- tables */

/* services and protocols, with what the system's files say added the first
 * time each is needed. A service is keyed by its network, a NUL and its name,
 * so that one map does for every network. */
static struct {
    SyncOnce services_once;
    SyncOnce protocols_once;
    Arena ar;
    Map *services;
    Map *networks;
    Map *protocols;
} po_tables;

/* The services once fills the arena and is over before the protocols once
 * starts on it, so the two never use it at the same time. */
static Alloc *po_alloc(void) {
    return arena_allocator(&po_tables.ar);
}

static Str po_key(Alloc *a, Str network, Str name) {
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)(network.len + 1 + name.len), 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    if (network.len > 0)
        memcpy(p, network.p, (size_t)network.len);
    p[network.len] = 0;
    if (name.len > 0)
        memcpy(p + network.len + 1, name.p, (size_t)name.len);
    return str_from_bytes(p, network.len + 1 + name.len);
}

static void po_set_service(Str network, Str name, Int port) {
    Alloc *a = po_alloc();
    Str key = po_key(a, network, name);
    if (key.len == 0)
        return;
    map_set(po_tables.services, &key, &port);
    Str nk = str_clone(a, network);
    Int one = 1;
    map_set(po_tables.networks, &nk, &one);
}

typedef struct PoEntry {
    const char *network;
    const char *name;
    Int port;
} PoEntry;

static const PoEntry po_services[] = {
    {"udp", "domain", 53},       {"tcp", "ftp", 21},    {"tcp", "ftps", 990},
    {"tcp", "gopher", 70},       {"tcp", "http", 80},   {"tcp", "https", 443},
    {"tcp", "imap2", 143},       {"tcp", "imap3", 220}, {"tcp", "imaps", 993},
    {"tcp", "pop3", 110},        {"tcp", "pop3s", 995}, {"tcp", "smtp", 25},
    {"tcp", "submissions", 465}, {"tcp", "ssh", 22},    {"tcp", "telnet", 23},
};

/* readServices, over the tables Go starts with. Windows has no
 * /etc/services and Go asks the system there instead, which burrow does not
 * do yet, so there the table is all there is. */
static void po_read_services(void *env) {
    (void)env;
    arena_init(&po_tables.ar, NULL, 0);
    Alloc *a = po_alloc();
    po_tables.services = map_make(a, TYPE_STRING, TYPE_INT, 64);
    po_tables.networks = map_make(a, TYPE_STRING, TYPE_INT, 4);
    if (po_tables.services == NULL || po_tables.networks == NULL)
        return;
    for (size_t i = 0; i < sizeof po_services / sizeof po_services[0]; i++)
        po_set_service(str_from_cstr(po_services[i].network),
                       str_from_cstr(po_services[i].name), po_services[i].port);
#ifndef _WIN32
    Arena sar;
    arena_init(&sar, NULL, 0);
    Alloc *s = arena_allocator(&sar);
    Error err = BURROW_NO_ERROR;
    burrow__NetFile *file = burrow__net_open(s, PO_LIT("/etc/services"), &err);
    if (file != NULL) {
        Str line;
        while (burrow__net_file_read_line(file, &line)) {
            /* "http 80/tcp www www-http # World Wide Web HTTP" */
            Int hash = strings_index_byte(line, '#');
            if (hash >= 0)
                line.len = hash;
            Slice f = burrow__net_get_fields(s, line);
            if (f.len < 2)
                continue;
            const Str *fs = (const Str *)f.p;
            Str portnet = fs[1]; /* "80/tcp" */
            Int port = 0, j = 0;
            if (!burrow__net_dtoi(portnet, &port, &j) || port <= 0 ||
                j >= portnet.len || portnet.p[j] != '/')
                continue;
            Str netw =
                str_from_bytes(portnet.p + j + 1, portnet.len - j - 1); /* "tcp" */
            for (Int i = 0; i < f.len; i++)
                if (i != 1) /* f[1] was port/net */
                    po_set_service(netw, fs[i], port);
        }
        burrow__net_file_close(file, s);
    }
    arena_free(&sar);
#endif
}

static void po_services_init(void) {
    sync_once_do(&po_tables.services_once, BURROW_FN(Func, po_read_services, NULL));
}

typedef struct PoProto {
    const char *name;
    Int proto;
} PoProto;

static const PoProto po_protocols[] = {
    {"icmp", 1}, {"igmp", 2}, {"tcp", 6}, {"udp", 17}, {"ipv6-icmp", 58},
};

/* readProtocols. Unlike a service, a name the table has already keeps its
 * number. */
static void po_read_protocols(void *env) {
    (void)env;
    po_services_init(); /* the arena */
    Alloc *a = po_alloc();
    po_tables.protocols = map_make(a, TYPE_STRING, TYPE_INT, 16);
    if (po_tables.protocols == NULL)
        return;
    for (size_t i = 0; i < sizeof po_protocols / sizeof po_protocols[0]; i++) {
        Str k = str_from_cstr(po_protocols[i].name);
        map_set(po_tables.protocols, &k, &po_protocols[i].proto);
    }
#ifndef _WIN32
    Arena sar;
    arena_init(&sar, NULL, 0);
    Alloc *s = arena_allocator(&sar);
    Error err = BURROW_NO_ERROR;
    burrow__NetFile *file = burrow__net_open(s, PO_LIT("/etc/protocols"), &err);
    if (file != NULL) {
        Str line;
        while (burrow__net_file_read_line(file, &line)) {
            /* "tcp 6 TCP # transmission control protocol" */
            Int hash = strings_index_byte(line, '#');
            if (hash >= 0)
                line.len = hash;
            Slice f = burrow__net_get_fields(s, line);
            if (f.len < 2)
                continue;
            const Str *fs = (const Str *)f.p;
            Int proto = 0, used = 0;
            if (!burrow__net_dtoi(fs[1], &proto, &used))
                continue;
            for (Int i = 0; i < f.len; i++) {
                if (i == 1)
                    continue;
                if (map_get(po_tables.protocols, &fs[i]) == NULL) {
                    Str k = str_clone(a, fs[i]);
                    map_set(po_tables.protocols, &k, &proto);
                }
            }
        }
        burrow__net_file_close(file, s);
    }
    arena_free(&sar);
#endif
}

/* ---------------------------------------------------------- lookupPortMap */

/* maxPortBufSize and maxProtoLength, which Go lowers into a buffer of that
 * size so that a longer name can never match. */
enum { PO_MAX_PORT_BUF = 25, PO_MAX_PROTO = 25 };

/* lookupPortMapWithNetwork. */
static Int po_lookup_with_network(Str network, Str err_network, Str service,
                                  Error *err) {
    Byte nbuf[PO_MAX_PORT_BUF + 1 + 8];
    Byte lower[PO_MAX_PORT_BUF];
    Int n = service.len < PO_MAX_PORT_BUF ? service.len : PO_MAX_PORT_BUF;
    if (n > 0)
        memcpy(lower, service.p, (size_t)n);
    burrow__net_lower_ascii_bytes(lower, n);
    Alloc *ea = error_allocator();
    Str name = burrow__net_cat(ea, 3, err_network, PO_LIT("/"), service);
    if (map_get(po_tables.networks, &network) != NULL) {
        Int *port = NULL;
        if (network.len <= 8 && n == service.len) {
            memcpy(nbuf, network.p, (size_t)network.len);
            nbuf[network.len] = 0;
            if (n > 0)
                memcpy(nbuf + network.len + 1, lower, (size_t)n);
            Str key = str_from_bytes(nbuf, network.len + 1 + n);
            port = (Int *)map_get(po_tables.services, &key);
        }
        if (port != NULL) {
            *err = BURROW_NO_ERROR;
            return *port;
        }
        *err = burrow__net_new_dns_error(ea, burrow__net_err_unknown_port, name,
                                         BURROW_STR_EMPTY);
        return 0;
    }
    NetDNSError e = {0};
    e.err = PO_LIT("unknown network");
    e.name = name;
    *err = net_dns_error_as_error(&e, ea);
    return 0;
}

Int burrow__net_lookup_port_map(Str network, Str service, Error *err) {
    po_services_init();
    if (po_tables.services == NULL) {
        *err = burrow_err_out_of_memory;
        return 0;
    }
    if (str_eq(network, PO_LIT("ip"))) { /* no hints */
        Int p = po_lookup_with_network(PO_LIT("tcp"), PO_LIT("ip"), service, err);
        if (BURROW_OK(*err))
            return p;
        return po_lookup_with_network(PO_LIT("udp"), PO_LIT("ip"), service, err);
    }
    if (str_eq(network, PO_LIT("tcp")) || str_eq(network, PO_LIT("tcp4")) ||
        str_eq(network, PO_LIT("tcp6")))
        return po_lookup_with_network(PO_LIT("tcp"), PO_LIT("tcp"), service, err);
    if (str_eq(network, PO_LIT("udp")) || str_eq(network, PO_LIT("udp4")) ||
        str_eq(network, PO_LIT("udp6")))
        return po_lookup_with_network(PO_LIT("udp"), PO_LIT("udp"), service, err);
    Alloc *ea = error_allocator();
    NetDNSError e = {0};
    e.err = PO_LIT("unknown network");
    e.name = burrow__net_cat(ea, 3, network, PO_LIT("/"), service);
    *err = net_dns_error_as_error(&e, ea);
    return 0;
}

/* ------------------------------------------------------- lookupProtocolMap */

Int burrow__net_lookup_protocol(Str name, Error *err) {
    sync_once_do(&po_tables.protocols_once, BURROW_FN(Func, po_read_protocols, NULL));
    *err = BURROW_NO_ERROR;
    Byte lower[PO_MAX_PROTO];
    Int n = name.len < PO_MAX_PROTO ? name.len : PO_MAX_PROTO;
    if (n > 0)
        memcpy(lower, name.p, (size_t)n);
    burrow__net_lower_ascii_bytes(lower, n);
    Str key = str_from_bytes(lower, n);
    Int *proto = po_tables.protocols != NULL && n == name.len
                     ? (Int *)map_get(po_tables.protocols, &key)
                     : NULL;
    if (proto != NULL)
        return *proto;
    NetAddrError e = {PO_LIT("unknown IP protocol specified"), name};
    *err = net_addr_error_as_error(&e, error_allocator());
    return 0;
}

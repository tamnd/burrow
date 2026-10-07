/* IPAddr, the address of an end of an IP connection and what the resolver
 * hands back for each address it finds.
 *
 * Derived from Go's src/net/iprawsock.go. IPConn is still to come.
 * Go source: go1.27.1.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "internal.h"

#include "burrow/declare.h"
#include "burrow/error.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/net.h"
#include "burrow/type.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define IR_LIT(s) str_from_bytes((const Byte *)(s), (Int)sizeof(s) - 1)
#define IR_COUNT(arr) (uint16_t)(sizeof(arr) / sizeof((arr)[0]))
#define IR_SIG_STRING(IN, OUT) OUT(Str)

Str net_ip_addr_network(const NetIPAddr *a) {
    (void)a;
    return IR_LIT("ip");
}

Str net_ip_addr_string(const NetIPAddr *a, Alloc *al) {
    if (a == NULL)
        return str_clone(al, IR_LIT("<nil>"));
    Arena ar;
    arena_init(&ar, NULL, 0);
    Str ip =
        a->ip.len == 0 ? BURROW_STR_EMPTY : net_ip_string(a->ip, arena_allocator(&ar));
    Str s = a->zone.len > 0 ? burrow__net_cat(al, 3, ip, IR_LIT("%"), a->zone)
                            : str_clone(al, ip);
    arena_free(&ar);
    return s;
}

static Str ir_addr_m_network(NetIPAddr *self) {
    return net_ip_addr_network(self);
}

static Str ir_addr_m_string(NetIPAddr *self) {
    return net_ip_addr_string(self, error_allocator());
}

#define IR_ADDR_METHODS(M, T)                                                          \
    M(T, Network, ir_addr_m_network, IR_SIG_STRING)                                    \
    M(T, String, ir_addr_m_string, IR_SIG_STRING)

BURROW_METHODS_DEFINE(NetIPAddr, IR_ADDR_METHODS);

static const Field ir_addr_fields[] = {
    {BURROW_S_INIT("IP"),
     {NULL, 0},
     &burrow_type_NetIP,
     (uint32_t)offsetof(NetIPAddr, ip)},
    {BURROW_S_INIT("Zone"),
     {NULL, 0},
     TYPE_STRING,
     (uint32_t)offsetof(NetIPAddr, zone)},
};

static const Type ir_addr_desc = {
    BURROW_S_INIT("IPAddr"),
    BURROW_S_INIT("net"),
    KIND_STRUCT,
    (uint32_t)sizeof(NetIPAddr),
    (uint16_t)_Alignof(NetIPAddr),
    IR_COUNT(ir_addr_fields),
    IR_COUNT(burrow__methods_NetIPAddr),
    ir_addr_fields,
    burrow__methods_NetIPAddr,
    NULL,
    NULL,
    0,
    0x6e697261U, /* "nira" */
    NULL,
};

const Type *const TYPE_NET_IP_ADDR = &ir_addr_desc;

static Str ir_addr_network(void *self) {
    return net_ip_addr_network((const NetIPAddr *)self);
}

static Str ir_addr_string(void *self, Alloc *a) {
    return net_ip_addr_string((const NetIPAddr *)self, a);
}

static const NetAddrVT ir_addr_vt = {&ir_addr_desc, ir_addr_network, ir_addr_string};

NetAddr net_ip_addr_as_addr(const NetIPAddr *a) {
    NetAddr addr = {NULL, NULL};
    if (a != NULL) {
        addr.vt = &ir_addr_vt;
        addr.data = (void *)(uintptr_t)a;
    }
    return addr;
}

/* A NetIPAddr this package made, with the bytes its ip and zone point at
 * after it, and the size to give back. */
typedef struct IrAddrBox {
    NetIPAddr a;
    size_t size;
} IrAddrBox;

NetIPAddr *burrow__net_ip_addr_new(Alloc *a, NetIP ip, Str zone) {
    size_t size = sizeof(IrAddrBox) + (size_t)ip.len + (size_t)zone.len;
    IrAddrBox *b = (IrAddrBox *)mem_alloc_nozero(a, size, _Alignof(IrAddrBox));
    if (b == NULL)
        return NULL;
    b->size = size;
    Byte *p = (Byte *)(b + 1);
    if (ip.len > 0)
        memcpy(p, ip.p, (size_t)ip.len);
    b->a.ip = ip.len > 0 ? slice_from(p, ip.len, ip.len, TYPE_BYTE) : (NetIP){0};
    if (zone.len > 0)
        memcpy(p + ip.len, zone.p, (size_t)zone.len);
    b->a.zone = str_from_bytes(p + ip.len, zone.len);
    return &b->a;
}

void net_ip_addr_free(Alloc *a, NetIPAddr *addr) {
    if (addr == NULL)
        return;
    IrAddrBox *b = (IrAddrBox *)(void *)addr;
    mem_free(a, b, b->size, _Alignof(IrAddrBox));
}

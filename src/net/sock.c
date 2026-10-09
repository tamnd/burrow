/* What every kind of socket shares: Go's conn, the methods TCPConn, UDPConn
 * and UnixConn all have, and UnknownNetworkError and errMissingAddress.
 *
 * Derived from Go's src/net/net.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "internal.h"

#include "burrow/declare.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/net.h"
#include "burrow/os.h"
#include "burrow/syscall.h"
#include "burrow/type.h"

#include "../os/internal.h"

#include <stdint.h>
#include <string.h>

#define SK_LIT(s) str_from_bytes((const Byte *)(s), (Int)sizeof(s) - 1)
#define SK_COUNT(arr) (uint16_t)(sizeof(arr) / sizeof((arr)[0]))

static Byte *sk_put(Byte *p, const void *s, Int n) {
    if (n > 0)
        memcpy(p, s, (size_t)n);
    return p + n;
}

/* ----------------------------------------------------- UnknownNetworkError */

/* The name first, so that errors_as gives a Str *, and the text after it,
 * built once since the message slot cannot allocate. */
typedef struct SkUnknownBox {
    NetUnknownNetworkError name;
    Str message;
} SkUnknownBox;

static Str sk_unknown_message(const void *self) {
    return ((const SkUnknownBox *)self)->message;
}

static Str sk_unknown_m_error(NetUnknownNetworkError *self) {
    return ((const SkUnknownBox *)(const void *)self)->message;
}

static bool sk_unknown_m_false(NetUnknownNetworkError *self) {
    (void)self;
    return false;
}

#define SK_SIG_STRING(IN, OUT) OUT(Str)
#define SK_SIG_BOOL(IN, OUT) OUT(bool)

#define SK_UNKNOWN_METHODS(M, T)                                                       \
    M(T, Error, sk_unknown_m_error, SK_SIG_STRING)                                     \
    M(T, Temporary, sk_unknown_m_false, SK_SIG_BOOL)                                   \
    M(T, Timeout, sk_unknown_m_false, SK_SIG_BOOL)

BURROW_METHODS_DEFINE(NetUnknownNetworkError, SK_UNKNOWN_METHODS);

static const Type sk_unknown_desc = {
    BURROW_S_INIT("UnknownNetworkError"),
    BURROW_S_INIT("net"),
    KIND_STRING,
    (uint32_t)sizeof(Str),
    (uint16_t)_Alignof(Str),
    0,
    SK_COUNT(burrow__methods_NetUnknownNetworkError),
    NULL,
    burrow__methods_NetUnknownNetworkError,
    NULL,
    NULL,
    0,
    0x6e756e6bU, /* "nunk" */
    NULL,
};

const Type *const TYPE_NET_UNKNOWN_NETWORK_ERROR = &sk_unknown_desc;

static bool sk_unknown_is(const void *self, Error target);
static Error sk_unknown_clone(const void *self, Alloc *a);

static const ErrorVT sk_unknown_vt = {
    &sk_unknown_desc, sk_unknown_message, NULL, NULL, sk_unknown_is, NULL,
    sk_unknown_clone,
};

/* Go compares two of them with ==, which for a string is the text. */
static bool sk_unknown_is(const void *self, Error target) {
    if (target.vt != &sk_unknown_vt || target.data == NULL)
        return false;
    Str a = ((const SkUnknownBox *)self)->name;
    Str b = ((const SkUnknownBox *)target.data)->name;
    return a.len == b.len && (a.len == 0 || memcmp(a.p, b.p, (size_t)a.len) == 0);
}

static const char sk_msg_unknown[] = "unknown network ";

Error net_unknown_network_error(Alloc *a, Str network) {
    Int plen = (Int)sizeof sk_msg_unknown - 1;
    size_t size =
        sizeof(SkUnknownBox) + (size_t)network.len + (size_t)plen + (size_t)network.len;
    SkUnknownBox *b = (SkUnknownBox *)mem_alloc_nozero(a, size, _Alignof(SkUnknownBox));
    if (b == NULL)
        return burrow_err_out_of_memory;
    Byte *p = (Byte *)(b + 1);
    b->name = str_from_bytes(p, network.len);
    p = sk_put(p, network.p, network.len);
    b->message = str_from_bytes(p, plen + network.len);
    p = sk_put(p, sk_msg_unknown, plen);
    sk_put(p, network.p, network.len);
    return (Error){&sk_unknown_vt, b};
}

static Error sk_unknown_clone(const void *self, Alloc *a) {
    return net_unknown_network_error(a, ((const SkUnknownBox *)self)->name);
}

BURROW_SENTINEL_ERROR(burrow__net_err_missing_address, "missing address");

/* ------------------------------------------------------------------- conn */

Error burrow__net_op_error(Str op, Str net, NetAddr source, NetAddr addr, Error err) {
    NetOpError oe = {op, net, source, addr, err};
    return net_op_error_as_error(&oe, error_allocator());
}

Error burrow__net_einval(void) {
    return burrow__os_errno_value(SYSCALL_EINVAL);
}

static const NetAddr sk_nil_addr = {NULL, NULL};

Int burrow__conn_read(burrow__NetConnCore *c, Slice p, Error *err) {
    Error e = BURROW_NO_ERROR;
    Int n = burrow__netfd_read(&c->fd, p, &e);
    if (BURROW_FAILED(e) && !(e.vt == io_eof.vt && e.data == io_eof.data))
        e = burrow__net_op_error(SK_LIT("read"), c->fd.net, c->laddr, c->raddr, e);
    BURROW_OUT(err, e);
    return n;
}

Int burrow__conn_write(burrow__NetConnCore *c, Slice p, Error *err) {
    Error e = BURROW_NO_ERROR;
    Int n = burrow__netfd_write(&c->fd, p, &e);
    if (BURROW_FAILED(e))
        e = burrow__net_op_error(SK_LIT("write"), c->fd.net, c->laddr, c->raddr, e);
    BURROW_OUT(err, e);
    return n;
}

Error burrow__conn_close(burrow__NetConnCore *c) {
    Error e = burrow__netfd_close(&c->fd);
    if (BURROW_FAILED(e))
        e = burrow__net_op_error(SK_LIT("close"), c->fd.net, c->laddr, c->raddr, e);
    return e;
}

/* conn's setters put the local address where the remote one would go and
 * leave the source out, as Go's do. */
static Error sk_set_op(burrow__NetConnCore *c, Error e) {
    if (BURROW_OK(e))
        return e;
    return burrow__net_op_error(SK_LIT("set"), c->fd.net, sk_nil_addr, c->laddr, e);
}

Error burrow__conn_set_deadline(burrow__NetConnCore *c, Time t, uint32_t mode) {
    return sk_set_op(c, burrow__pfd_set_deadline(&c->fd.pfd, t, mode));
}

Error burrow__conn_set_buffer(burrow__NetConnCore *c, int32_t opt, Int bytes) {
    return sk_set_op(c, burrow__netfd_setsockopt(&c->fd, opt, bytes));
}

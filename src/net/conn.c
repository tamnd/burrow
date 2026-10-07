/* Derived from Go's src/net/net.go, the interfaces, net.Error, OpError and
 * ErrClosed, from errNetClosing in src/internal/poll/fd.go, and from
 * isConnError in src/net/error_unix.go and error_windows.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/net.h"

#include "internal.h"

#include "burrow/context.h"
#include "burrow/declare.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/os.h"
#include "burrow/syscall.h"
#include "burrow/type.h"

#include <stdint.h>
#include <string.h>

#define NC_LIT(s) str_from_bytes((const Byte *)(s), (Int)sizeof(s) - 1)
#define NC_COUNT(arr) (uint16_t)(sizeof(arr) / sizeof((arr)[0]))

/* ------------------------------------------------------------- conversions */

IoReader net_conn_as_io_reader(NetConn c) {
    IoReader r = {c.vt != NULL ? &c.vt->reader : NULL, c.data};
    return r;
}

IoWriter net_conn_as_io_writer(NetConn c) {
    IoWriter w = {c.vt != NULL ? &c.vt->writer : NULL, c.data};
    return w;
}

IoCloser net_conn_as_io_closer(NetConn c) {
    IoCloser cl = {c.vt != NULL ? &c.vt->closer : NULL, c.data};
    return cl;
}

IoCloser net_listener_as_io_closer(NetListener l) {
    IoCloser cl = {l.vt != NULL ? &l.vt->closer : NULL, l.data};
    return cl;
}

IoCloser net_packet_conn_as_io_closer(NetPacketConn c) {
    IoCloser cl = {c.vt != NULL ? &c.vt->closer : NULL, c.data};
    return cl;
}

/* --------------------------------------------------------------- net.Error */

/* The method called name on err's type, if it is a func() bool, which is
 * the only shape Timeout and Temporary have. */
static const Method *nc_bool_method(Error err, Str name) {
    if (err.vt == NULL || err.vt->self_type == NULL)
        return NULL;
    const Method *m = type_method_by_name(err.vt->self_type, name);
    if (m == NULL || m->ftype == NULL || m->thunk == NULL)
        return NULL;
    if (type_num_in(m->ftype) != 0 || type_num_out(m->ftype) != 1 ||
        type_out(m->ftype, 0) != TYPE_BOOL)
        return NULL;
    return m;
}

/* Go's err.(timeout) and err.(temporary) and the call after them: false when
 * the type has no such method, and what the method says when it has. */
static bool nc_ask(Error err, Str name) {
    const Method *m = nc_bool_method(err, name);
    if (m == NULL)
        return false;
    bool out = false;
    void *rets[1] = {&out};
    method_call(m, (void *)(uintptr_t)err.data, NULL, rets);
    return out;
}

bool net_is_error(Error err) {
    return nc_bool_method(err, NC_LIT("Timeout")) != NULL &&
           nc_bool_method(err, NC_LIT("Temporary")) != NULL;
}

Str net_error_error(NetError err) {
    return error_text(err);
}

bool net_error_timeout(NetError err) {
    return net_is_error(err) && nc_ask(err, NC_LIT("Timeout"));
}

bool net_error_temporary(NetError err) {
    return net_is_error(err) && nc_ask(err, NC_LIT("Temporary"));
}

/* ---------------------------------------------------------------- ErrClosed */

/* internal/poll's errNetClosing, which is a type of its own so that it can be
 * a net.Error that is neither a timeout nor temporary. */
typedef struct NetErrNetClosing {
    Byte unused;
} NetErrNetClosing;

static Str nc_closing_m_error(NetErrNetClosing *self) {
    (void)self;
    return NC_LIT("use of closed network connection");
}

static bool nc_closing_m_false(NetErrNetClosing *self) {
    (void)self;
    return false;
}

#define NC_SIG_STRING(IN, OUT) OUT(Str)
#define NC_SIG_BOOL(IN, OUT) OUT(bool)
#define NC_SIG_ERROR(IN, OUT) OUT(Error)

#define NC_CLOSING_METHODS(M, T)                                                       \
    M(T, Error, nc_closing_m_error, NC_SIG_STRING)                                     \
    M(T, Temporary, nc_closing_m_false, NC_SIG_BOOL)                                   \
    M(T, Timeout, nc_closing_m_false, NC_SIG_BOOL)

BURROW_METHODS_DEFINE(NetErrNetClosing, NC_CLOSING_METHODS);

static const Type nc_closing_desc = {
    BURROW_S_INIT("errNetClosing"),
    BURROW_S_INIT("internal/poll"),
    KIND_STRUCT,
    (uint32_t)sizeof(NetErrNetClosing),
    (uint16_t)_Alignof(NetErrNetClosing),
    0,
    NC_COUNT(burrow__methods_NetErrNetClosing),
    NULL,
    burrow__methods_NetErrNetClosing,
    NULL,
    NULL,
    0,
    0x706e6563U, /* "pnec" */
    NULL,
};

static Str nc_closing_message(const void *self) {
    (void)self;
    return NC_LIT("use of closed network connection");
}

static const ErrorVT nc_closing_vt = {
    &nc_closing_desc, nc_closing_message, NULL, NULL, NULL, NULL, NULL,
};

static const NetErrNetClosing nc_closing_value = {0};

const Error net_err_closed = {&nc_closing_vt, &nc_closing_value};

/* ------------------------------------------------- errTimeout, errCanceled */

/* timeoutError and canceledError, which keep the words Go has always used for
 * a lookup or a dial whose context ran out, "i/o timeout" and "operation was
 * canceled", while errors_is still finds the context's own error in them. */
typedef struct NetErrContext {
    Byte timeout;
} NetErrContext;

static Str nc_ctx_m_error(NetErrContext *self) {
    return self->timeout ? NC_LIT("i/o timeout") : NC_LIT("operation was canceled");
}

static bool nc_ctx_m_timeout(NetErrContext *self) {
    return self->timeout != 0;
}

#define NC_CTX_METHODS(M, T)                                                           \
    M(T, Error, nc_ctx_m_error, NC_SIG_STRING)                                         \
    M(T, Temporary, nc_ctx_m_timeout, NC_SIG_BOOL)                                     \
    M(T, Timeout, nc_ctx_m_timeout, NC_SIG_BOOL)

BURROW_METHODS_DEFINE(NetErrContext, NC_CTX_METHODS);

/* Only the timeout is a net.Error. canceledError has no Timeout or Temporary
 * in Go, so its type lists Error alone. */
#define NC_CANCELED_METHODS(M, T) M(T, Error, nc_ctx_m_error, NC_SIG_STRING)

typedef NetErrContext NetErrCanceled;

BURROW_METHODS_DEFINE(NetErrCanceled, NC_CANCELED_METHODS);

static const Type nc_timeout_desc = {
    BURROW_S_INIT("timeoutError"),
    BURROW_S_INIT("net"),
    KIND_STRUCT,
    (uint32_t)sizeof(NetErrContext),
    (uint16_t)_Alignof(NetErrContext),
    0,
    NC_COUNT(burrow__methods_NetErrContext),
    NULL,
    burrow__methods_NetErrContext,
    NULL,
    NULL,
    0,
    0x6e657469U, /* "neti" */
    NULL,
};

static const Type nc_canceled_desc = {
    BURROW_S_INIT("canceledError"),
    BURROW_S_INIT("net"),
    KIND_STRUCT,
    (uint32_t)sizeof(NetErrContext),
    (uint16_t)_Alignof(NetErrContext),
    0,
    NC_COUNT(burrow__methods_NetErrCanceled),
    NULL,
    burrow__methods_NetErrCanceled,
    NULL,
    NULL,
    0,
    0x6e657463U, /* "netc" */
    NULL,
};

static Str nc_ctx_message(const void *self) {
    return nc_ctx_m_error((NetErrContext *)(uintptr_t)self);
}

static bool nc_same(Error a, Error b) {
    return a.vt == b.vt && a.data == b.data;
}

static bool nc_timeout_is(const void *self, Error target) {
    (void)self;
    return nc_same(target, context_deadline_exceeded);
}

static bool nc_canceled_is(const void *self, Error target) {
    (void)self;
    return nc_same(target, context_canceled);
}

static const ErrorVT nc_timeout_vt = {
    &nc_timeout_desc, nc_ctx_message, NULL, NULL, nc_timeout_is, NULL, NULL,
};

static const ErrorVT nc_canceled_vt = {
    &nc_canceled_desc, nc_ctx_message, NULL, NULL, nc_canceled_is, NULL, NULL,
};

static const NetErrContext nc_timeout_value = {1};
static const NetErrContext nc_canceled_value = {0};

const Error burrow__net_err_timeout = {&nc_timeout_vt, &nc_timeout_value};
const Error burrow__net_err_canceled = {&nc_canceled_vt, &nc_canceled_value};

Error burrow__net_map_err(Error err) {
    if (nc_same(err, context_canceled))
        return burrow__net_err_canceled;
    if (nc_same(err, context_deadline_exceeded))
        return burrow__net_err_timeout;
    return err;
}

/* ------------------------------------------------------------------ OpError */

typedef struct NetOpErrorBox {
    NetOpError e;
    Str message;
} NetOpErrorBox;

static Str nc_op_error_message(const void *self) {
    return ((const NetOpErrorBox *)self)->message;
}

static Error nc_op_error_unwrap_slot(const void *self) {
    return ((const NetOpError *)self)->err;
}

static Byte *nc_put(Byte *p, Str s) {
    if (s.len > 0)
        memcpy(p, s.p, (size_t)s.len);
    return p + s.len;
}

/* The two addresses' text, made in a, and the length of the whole message. */
static Int nc_op_error_parts(const NetOpError *e, Alloc *a, Str *src, Str *dst,
                             Str *text) {
    *src = e->source.vt != NULL ? e->source.vt->string(e->source.data, a)
                                : BURROW_STR_EMPTY;
    *dst = e->addr.vt != NULL ? e->addr.vt->string(e->addr.data, a) : BURROW_STR_EMPTY;
    *text = error_text(e->err);
    Int n = e->op.len;
    if (e->net.len != 0)
        n += 1 + e->net.len;
    if (e->source.vt != NULL)
        n += 1 + src->len;
    if (e->addr.vt != NULL)
        n += (e->source.vt != NULL ? 2 : 1) + dst->len;
    return n + 2 + text->len;
}

/* op, " " net, " " source, "->" or " " and addr, ": " err. */
static void nc_op_error_write(Byte *p, const NetOpError *e, Str src, Str dst,
                              Str text) {
    p = nc_put(p, e->op);
    if (e->net.len != 0) {
        *p++ = ' ';
        p = nc_put(p, e->net);
    }
    if (e->source.vt != NULL) {
        *p++ = ' ';
        p = nc_put(p, src);
    }
    if (e->addr.vt != NULL) {
        if (e->source.vt != NULL) {
            *p++ = '-';
            *p++ = '>';
        } else {
            *p++ = ' ';
        }
        p = nc_put(p, dst);
    }
    *p++ = ':';
    *p++ = ' ';
    nc_put(p, text);
}

Str net_op_error_error(const NetOpError *e, Alloc *a) {
    if (e == NULL)
        return NC_LIT("<nil>");
    Str src, dst, text;
    Int n = nc_op_error_parts(e, a, &src, &dst, &text);
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)n + 1, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    nc_op_error_write(p, e, src, dst, text);
    p[n] = 0;
    return str_from_bytes(p, n);
}

Error net_op_error_unwrap(const NetOpError *e) {
    return e->err;
}

bool net_op_error_timeout(const NetOpError *e) {
    if (e->err.vt != NULL && e->err.vt->self_type == TYPE_OS_SYSCALL_ERROR)
        return nc_ask(((const OsSyscallError *)e->err.data)->err, NC_LIT("Timeout"));
    return nc_ask(e->err, NC_LIT("Timeout"));
}

/* Go's isConnError, which asks for an Errno itself and not one inside
 * something else. */
static bool nc_is_conn_error(Error err) {
    if (err.vt == NULL || err.vt->self_type != TYPE_SYSCALL_ERRNO)
        return false;
    SyscallErrno no = *(const SyscallErrno *)err.data;
    return no == SYSCALL_ECONNRESET || no == SYSCALL_ECONNABORTED;
}

bool net_op_error_temporary(const NetOpError *e) {
    if (str_eq(e->op, NC_LIT("accept")) && nc_is_conn_error(e->err))
        return true;
    if (e->err.vt != NULL && e->err.vt->self_type == TYPE_OS_SYSCALL_ERROR)
        return nc_ask(((const OsSyscallError *)e->err.data)->err, NC_LIT("Temporary"));
    return nc_ask(e->err, NC_LIT("Temporary"));
}

static Str nc_op_error_m_error(NetOpError *self) {
    return ((const NetOpErrorBox *)self)->message;
}

static bool nc_op_error_m_temporary(NetOpError *self) {
    return net_op_error_temporary(self);
}

static bool nc_op_error_m_timeout(NetOpError *self) {
    return net_op_error_timeout(self);
}

static Error nc_op_error_m_unwrap(NetOpError *self) {
    return self->err;
}

#define NC_OP_ERROR_METHODS(M, T)                                                      \
    M(T, Error, nc_op_error_m_error, NC_SIG_STRING)                                    \
    M(T, Temporary, nc_op_error_m_temporary, NC_SIG_BOOL)                              \
    M(T, Timeout, nc_op_error_m_timeout, NC_SIG_BOOL)                                  \
    M(T, Unwrap, nc_op_error_m_unwrap, NC_SIG_ERROR)

BURROW_METHODS_DEFINE(NetOpError, NC_OP_ERROR_METHODS);

static const Type nc_op_error_desc = {
    BURROW_S_INIT("OpError"),
    BURROW_S_INIT("net"),
    KIND_STRUCT,
    (uint32_t)sizeof(NetOpError),
    (uint16_t)_Alignof(NetOpError),
    0,
    NC_COUNT(burrow__methods_NetOpError),
    NULL,
    burrow__methods_NetOpError,
    NULL,
    NULL,
    0,
    0x6e65746fU, /* "neto" */
    NULL,
};

const Type *const TYPE_NET_OP_ERROR = &nc_op_error_desc;

static Error nc_op_error_clone(const void *self, Alloc *a);

static const ErrorVT nc_op_error_vt = {
    &nc_op_error_desc, nc_op_error_message, nc_op_error_unwrap_slot, NULL, NULL, NULL,
    nc_op_error_clone,
};

/* One allocation for the box, op, net and a message of mlen bytes, which the
 * caller writes. */
static NetOpErrorBox *nc_op_error_box(const NetOpError *e, Int mlen, Alloc *a) {
    size_t size =
        sizeof(NetOpErrorBox) + (size_t)e->op.len + (size_t)e->net.len + (size_t)mlen;
    NetOpErrorBox *b =
        (NetOpErrorBox *)mem_alloc_nozero(a, size, _Alignof(NetOpErrorBox));
    if (b == NULL)
        return NULL;
    b->e = *e;
    Byte *p = (Byte *)(b + 1);
    b->e.op = str_from_bytes(p, e->op.len);
    p = nc_put(p, e->op);
    b->e.net = str_from_bytes(p, e->net.len);
    p = nc_put(p, e->net);
    b->message = str_from_bytes(p, mlen);
    return b;
}

/* The addresses' text comes from their own string methods, in a, and is
 * copied into the message. */
Error net_op_error_as_error(const NetOpError *e, Alloc *a) {
    Str src, dst, text;
    Int mlen = nc_op_error_parts(e, a, &src, &dst, &text);
    NetOpErrorBox *b = nc_op_error_box(e, mlen, a);
    if (b == NULL)
        return burrow_err_out_of_memory;
    nc_op_error_write((Byte *)(uintptr_t)b->message.p, e, src, dst, text);
    return (Error){&nc_op_error_vt, b};
}

/* The copy keeps the message it has rather than asking the addresses again,
 * since they may be gone, and keeps what it wraps by way of error_retain so
 * that nothing in it points back at the original's memory. */
static Error nc_op_error_clone(const void *self, Alloc *a) {
    const NetOpErrorBox *old = (const NetOpErrorBox *)self;
    NetOpError e = old->e;
    e.err = error_retain(a, e.err);
    NetOpErrorBox *b = nc_op_error_box(&e, old->message.len, a);
    if (b == NULL)
        return burrow_err_out_of_memory;
    nc_put((Byte *)(uintptr_t)b->message.p, old->message);
    return (Error){&nc_op_error_vt, b};
}

/* os's errors. See include/burrow/os.h.
 *
 * Derived from Go's src/os/error.go.
 * Go source: go1.27.1.
 *
 * The deadline errors are from internal/poll/fd.go in the same release.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/os.h"

#include "burrow/declare.h"
#include "burrow/syscall.h"

#include "internal.h"

#include <string.h>

#define OS_LIT(s) str_from_bytes((const Byte *)(s), (Int)(sizeof(s) - 1))

BURROW_SENTINEL_ERROR(os_err_no_deadline, "file type does not support deadline");

/* ---------------------------------------------------- DeadlineExceededError */

/* poll.DeadlineExceededError, an empty struct whose Timeout and Temporary
 * both say true. C has no empty struct, so it has a byte nobody reads. */
typedef struct OsDeadlineExceededError {
    Byte unused;
} OsDeadlineExceededError;

static Str deadline_m_error(OsDeadlineExceededError *self) {
    (void)self;
    return OS_LIT("i/o timeout");
}

static bool deadline_m_true(OsDeadlineExceededError *self) {
    (void)self;
    return true;
}

#define OS_SIG_STRING(IN, OUT) OUT(Str)
#define OS_SIG_BOOL(IN, OUT) OUT(bool)

#define DEADLINE_METHODS(M, T)                                                         \
    M(T, Error, deadline_m_error, OS_SIG_STRING)                                       \
    M(T, Temporary, deadline_m_true, OS_SIG_BOOL)                                      \
    M(T, Timeout, deadline_m_true, OS_SIG_BOOL)

BURROW_METHODS_DEFINE(OsDeadlineExceededError, DEADLINE_METHODS);

static const Type deadline_desc = {
    {(const Byte *)"DeadlineExceededError", 21},
    {(const Byte *)"internal/poll", 13},
    KIND_STRUCT,
    (uint32_t)sizeof(OsDeadlineExceededError),
    (uint16_t)_Alignof(OsDeadlineExceededError),
    0,
    (uint16_t)(sizeof burrow__methods_OsDeadlineExceededError /
               sizeof burrow__methods_OsDeadlineExceededError[0]),
    NULL,
    burrow__methods_OsDeadlineExceededError,
    NULL,
    NULL,
    0,
    0x706f6465U, /* "pode" */
    NULL,
};

static const Str deadline_text = {(const Byte *)"i/o timeout", 11};

static Str deadline_message(const void *self) {
    (void)self;
    return deadline_text;
}

static Error deadline_clone(const void *self, Alloc *a);

static const ErrorVT deadline_vt = {
    &deadline_desc, deadline_message, NULL, NULL, NULL, NULL, deadline_clone,
};

static const OsDeadlineExceededError deadline_value = {0};

const Error os_err_deadline_exceeded = {&deadline_vt, &deadline_value};

/* There is only the one, so a retained copy is the same error. */
static Error deadline_clone(const void *self, Alloc *a) {
    (void)self;
    (void)a;
    return os_err_deadline_exceeded;
}

/* ---------------------------------------------------------------- LinkError */

static Byte *os_put(Byte *p, Str s) {
    if (s.len > 0)
        memcpy(p, s.p, (size_t)s.len);
    return p + s.len;
}

/* The OsLinkError first, so errors_as hands back a pointer to it, and the
 * message after, built once because the message slot cannot allocate. */
typedef struct LinkErrorBox {
    OsLinkError e;
    Str message;
} LinkErrorBox;

static Str link_error_message(const void *self) {
    return ((const LinkErrorBox *)self)->message;
}

static Error link_error_unwrap_slot(const void *self) {
    return ((const OsLinkError *)self)->err;
}

static const Type link_error_desc = {
    {(const Byte *)"LinkError", 9},
    {(const Byte *)"os", 2},
    KIND_STRUCT,
    (uint32_t)sizeof(OsLinkError),
    (uint16_t)_Alignof(OsLinkError),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x6f736c65U, /* "osle" */
    NULL,
};

const Type *const TYPE_OS_LINK_ERROR = &link_error_desc;

static Error link_error_clone(const void *self, Alloc *a);

static const ErrorVT link_error_vt = {
    &link_error_desc, link_error_message, link_error_unwrap_slot, NULL, NULL, NULL,
    link_error_clone,
};

/* One allocation: the box, then op, old, new and the message, which is
 * op + " " + old + " " + new + ": " + the text of err. */
Error os_link_error_new(Alloc *a, Str op, Str old, Str new_, Error err) {
    Str text = error_text(err);
    Int mlen = op.len + 1 + old.len + 1 + new_.len + 2 + text.len;
    size_t size = sizeof(LinkErrorBox) + (size_t)op.len + (size_t)old.len +
                  (size_t)new_.len + (size_t)mlen;
    LinkErrorBox *b = (LinkErrorBox *)mem_alloc_nozero(a, size, _Alignof(LinkErrorBox));
    if (b == NULL)
        return burrow_err_out_of_memory;
    Byte *p = (Byte *)(b + 1);
    b->e.op = str_from_bytes(p, op.len);
    p = os_put(p, op);
    b->e.old = str_from_bytes(p, old.len);
    p = os_put(p, old);
    b->e.new_ = str_from_bytes(p, new_.len);
    p = os_put(p, new_);
    b->e.err = err;
    b->message = str_from_bytes(p, mlen);
    p = os_put(p, op);
    *p++ = ' ';
    p = os_put(p, old);
    *p++ = ' ';
    p = os_put(p, new_);
    *p++ = ':';
    *p++ = ' ';
    os_put(p, text);
    return (Error){&link_error_vt, b};
}

static Error link_error_clone(const void *self, Alloc *a) {
    const OsLinkError *e = (const OsLinkError *)self;
    return os_link_error_new(a, e->op, e->old, e->new_, error_retain(a, e->err));
}

Str os_link_error_error(const OsLinkError *e, Alloc *a) {
    Str text = error_text(e->err);
    Int n = e->op.len + 1 + e->old.len + 1 + e->new_.len + 2 + text.len;
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)n + 1, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    Byte *q = os_put(p, e->op);
    *q++ = ' ';
    q = os_put(q, e->old);
    *q++ = ' ';
    q = os_put(q, e->new_);
    *q++ = ':';
    *q++ = ' ';
    q = os_put(q, text);
    *q = 0;
    return str_from_bytes(p, n);
}

Error os_link_error_unwrap(const OsLinkError *e) {
    return e->err;
}

/* ------------------------------------------------------------- SyscallError */

typedef struct SyscallErrorBox {
    OsSyscallError e;
    Str message;
} SyscallErrorBox;

static Str syscall_error_message(const void *self) {
    return ((const SyscallErrorBox *)self)->message;
}

static Error syscall_error_unwrap_slot(const void *self) {
    return ((const OsSyscallError *)self)->err;
}

/* Whether err's type has a Timeout() bool that says true, which is Go's
 * err.(timeout) assertion. */
static bool os_has_timeout(Error err) {
    if (err.vt == NULL || err.vt->self_type == NULL)
        return false;
    const Method *m = type_method_by_name(err.vt->self_type, OS_LIT("Timeout"));
    if (m == NULL || m->ftype == NULL || m->thunk == NULL)
        return false;
    if (type_num_in(m->ftype) != 0 || type_num_out(m->ftype) != 1 ||
        type_out(m->ftype, 0) != TYPE_BOOL)
        return false;
    bool out = false;
    void *rets[1] = {&out};
    method_call(m, (void *)(uintptr_t)err.data, NULL, rets);
    return out;
}

bool os_syscall_error_timeout(const OsSyscallError *e) {
    return e != NULL && os_has_timeout(e->err);
}

Str os_syscall_error_error(const OsSyscallError *e, Alloc *a) {
    Str text = error_text(e->err);
    Int n = e->syscall.len + 2 + text.len;
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)n + 1, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    Byte *q = os_put(p, e->syscall);
    *q++ = ':';
    *q++ = ' ';
    q = os_put(q, text);
    *q = 0;
    return str_from_bytes(p, n);
}

Error os_syscall_error_unwrap(const OsSyscallError *e) {
    return e->err;
}

static Str syscall_error_m_error(OsSyscallError *self) {
    return os_syscall_error_error(self, error_allocator());
}

static bool syscall_error_m_timeout(OsSyscallError *self) {
    return os_syscall_error_timeout(self);
}

static Error syscall_error_m_unwrap(OsSyscallError *self) {
    return self->err;
}

#define OS_SIG_ERROR(IN, OUT) OUT(Error)

#define SYSCALL_ERROR_METHODS(M, T)                                                    \
    M(T, Error, syscall_error_m_error, OS_SIG_STRING)                                  \
    M(T, Timeout, syscall_error_m_timeout, OS_SIG_BOOL)                                \
    M(T, Unwrap, syscall_error_m_unwrap, OS_SIG_ERROR)

BURROW_METHODS_DEFINE(OsSyscallError, SYSCALL_ERROR_METHODS);

static const Type syscall_error_desc = {
    {(const Byte *)"SyscallError", 12},
    {(const Byte *)"os", 2},
    KIND_STRUCT,
    (uint32_t)sizeof(OsSyscallError),
    (uint16_t)_Alignof(OsSyscallError),
    0,
    (uint16_t)(sizeof burrow__methods_OsSyscallError /
               sizeof burrow__methods_OsSyscallError[0]),
    NULL,
    burrow__methods_OsSyscallError,
    NULL,
    NULL,
    0,
    0x6f737365U, /* "osse" */
    NULL,
};

const Type *const TYPE_OS_SYSCALL_ERROR = &syscall_error_desc;

static Error syscall_error_clone(const void *self, Alloc *a);

static const ErrorVT syscall_error_vt = {
    &syscall_error_desc,
    syscall_error_message,
    syscall_error_unwrap_slot,
    NULL,
    NULL,
    NULL,
    syscall_error_clone,
};

Error os_new_syscall_error(Alloc *a, Str syscall, Error err) {
    if (BURROW_OK(err))
        return BURROW_NO_ERROR;
    Str text = error_text(err);
    Int mlen = syscall.len + 2 + text.len;
    size_t size = sizeof(SyscallErrorBox) + (size_t)syscall.len + (size_t)mlen;
    SyscallErrorBox *b =
        (SyscallErrorBox *)mem_alloc_nozero(a, size, _Alignof(SyscallErrorBox));
    if (b == NULL)
        return burrow_err_out_of_memory;
    Byte *p = (Byte *)(b + 1);
    b->e.syscall = str_from_bytes(p, syscall.len);
    p = os_put(p, syscall);
    b->e.err = err;
    b->message = str_from_bytes(p, mlen);
    p = os_put(p, syscall);
    *p++ = ':';
    *p++ = ' ';
    os_put(p, text);
    return (Error){&syscall_error_vt, b};
}

static Error syscall_error_clone(const void *self, Alloc *a) {
    const OsSyscallError *e = (const OsSyscallError *)self;
    return os_new_syscall_error(a, e->syscall, error_retain(a, e->err));
}

/* ------------------------------------------------------------------ IsExist */

/* underlyingError: one level inside the three wrapping types os has always
 * looked inside, and no further. */
static Error os_underlying(Error err) {
    if (err.vt == NULL || err.vt->self_type == NULL)
        return err;
    const Type *t = err.vt->self_type;
    if (t == TYPE_FS_PATH_ERROR)
        return ((const FsPathError *)err.data)->err;
    if (t == TYPE_OS_LINK_ERROR)
        return ((const OsLinkError *)err.data)->err;
    if (t == TYPE_OS_SYSCALL_ERROR)
        return ((const OsSyscallError *)err.data)->err;
    return err;
}

/* underlyingErrorIs: the sentinel itself, or an Errno that says it is. */
static bool os_underlying_is(Error err, Error target) {
    err = os_underlying(err);
    if (err.vt == target.vt && err.data == target.data)
        return true;
    if (err.vt != NULL && err.vt->self_type == TYPE_SYSCALL_ERRNO)
        return syscall_errno_is(*(const SyscallErrno *)err.data, target);
    return false;
}

bool os_is_exist(Error err) {
    return os_underlying_is(err, fs_err_exist);
}

bool os_is_not_exist(Error err) {
    return os_underlying_is(err, fs_err_not_exist);
}

bool os_is_permission(Error err) {
    return os_underlying_is(err, fs_err_permission);
}

bool os_is_timeout(Error err) {
    return os_has_timeout(os_underlying(err));
}

/* -------------------------------------------------------------- PAL errors */

Error burrow__os_errno_value(SyscallErrno e) {
    return syscall_errno_as_error(e, error_allocator());
}

Error burrow__os_errno(PalErrno e) {
    SyscallErrno n = syscall_errno_from_pal(e);
    if (n == 0) {
        int64_t native = pal_errno_native(e);
        if (native != 0)
            n = (SyscallErrno)native;
    }
    if (n == 0)
        return errors_new(error_allocator(), str_from_cstr(pal_errno_string(e)));
    return burrow__os_errno_value(n);
}

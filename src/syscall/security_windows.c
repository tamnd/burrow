/* The account and token calls Go writes by hand for Windows: SIDs, looking
 * accounts up, and what a process's access token says about its user.
 *
 * Derived from Go's src/syscall/security_windows.go.
 * Go source: go1.27.1.
 *
 * Copyright 2012 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/syscall.h"

#if defined(BURROW_OS_WINDOWS)

#include "burrow/mem.h"
#include "burrow/mem/heap.h"

#include "internal.h"

#include <string.h>

/* Go's SIDs and token buffers are byte slices the collector frees. Here
 * they come from the caller's allocator with their size in a header in
 * front, so the free functions know what to give back. */
enum { SEC_HEADER = 16 };

static void *sec_alloc(Alloc *a, size_t n) {
    uint8_t *p = mem_alloc(a, SEC_HEADER + n, SEC_HEADER);
    if (p == NULL)
        return NULL;
    memcpy(p, &n, sizeof n);
    return p + SEC_HEADER;
}

static void sec_free(Alloc *a, void *p) {
    if (p == NULL)
        return;
    uint8_t *base = (uint8_t *)p - SEC_HEADER;
    size_t n;
    memcpy(&n, base, sizeof n);
    mem_free(a, base, SEC_HEADER + n, SEC_HEADER);
}

static bool sec_insufficient(Error e) {
    const SyscallErrno *n = errors_as(e, TYPE_SYSCALL_ERRNO);
    return n != NULL && *n == SYSCALL_ERROR_INSUFFICIENT_BUFFER;
}

static uint16_t *sec_units(uint32_t n) {
    return BURROW_NEW_N(heap_allocator(), uint16_t, n > 0 ? n : 1);
}

static void sec_units_free(uint16_t *b, uint32_t n) {
    mem_free(heap_allocator(), b, (size_t)(n > 0 ? n : 1) * sizeof(uint16_t),
             _Alignof(uint16_t));
}

/* The NUL-terminated string at p, from a, as Go's utf16PtrToString. */
static Str sec_ptr_to_string(Alloc *a, const uint16_t *p) {
    Int n = 0;
    while (p[n] != 0)
        n++;
    return syscall_utf16_to_string(a, (Slice){(void *)(Uintptr)p, n, n, TYPE_UINT16});
}

Str syscall_translate_account_name(Alloc *a, Str username, uint32_t from, uint32_t to,
                                   Int init_size, Error *err) {
    (void)init_size;
    burrow__SyscallWString w;
    uint16_t *u = burrow__syscall_wstring(&w, username, err);
    if (u == NULL)
        return BURROW_STR_EMPTY;
    uint32_t n = 50;
    for (;;) {
        uint32_t size = n;
        uint16_t *b = sec_units(size);
        if (b == NULL) {
            burrow__syscall_wstring_free(&w);
            BURROW_OUT(err, burrow_err_out_of_memory);
            return BURROW_STR_EMPTY;
        }
        Error e = syscall_translate_name(u, from, to, b, &n);
        if (BURROW_OK(e)) {
            Str s =
                syscall_utf16_to_string(a, (Slice){b, (Int)n, (Int)size, TYPE_UINT16});
            sec_units_free(b, size);
            burrow__syscall_wstring_free(&w);
            BURROW_OUT(err, BURROW_NO_ERROR);
            return s;
        }
        sec_units_free(b, size);
        if (!sec_insufficient(e) || n <= size) {
            burrow__syscall_wstring_free(&w);
            BURROW_OUT(err, e);
            return BURROW_STR_EMPTY;
        }
    }
}

SyscallSID *syscall_string_to_sid(Alloc *a, Str s, Error *err) {
    burrow__SyscallWString w;
    uint16_t *p = burrow__syscall_wstring(&w, s, err);
    if (p == NULL)
        return NULL;
    SyscallSID *sid = NULL;
    Error e = syscall_convert_string_sid_to_sid(p, &sid);
    burrow__syscall_wstring_free(&w);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return NULL;
    }
    SyscallSID *c = syscall_sid_copy(sid, a, err);
    (void)syscall_local_free((SyscallHandle)(Uintptr)(void *)sid, NULL);
    return c;
}

SyscallSID *syscall_lookup_sid(Alloc *a, Str system, Str account, Str *domain,
                               uint32_t *acc_type, Error *err) {
    if (domain != NULL)
        *domain = BURROW_STR_EMPTY;
    if (acc_type != NULL)
        *acc_type = 0;
    if (account.len == 0) {
        BURROW_OUT(err, burrow__syscall_errno_err(SYSCALL_EINVAL));
        return NULL;
    }
    burrow__SyscallWString wacc, wsys;
    uint16_t *acc = burrow__syscall_wstring(&wacc, account, err);
    if (acc == NULL)
        return NULL;
    uint16_t *sys = NULL;
    if (system.len > 0) {
        sys = burrow__syscall_wstring(&wsys, system, err);
        if (sys == NULL) {
            burrow__syscall_wstring_free(&wacc);
            return NULL;
        }
    }
    uint32_t n = 50, dn = 50, use = 0;
    SyscallSID *sid = NULL;
    Error e = BURROW_NO_ERROR;
    for (;;) {
        uint32_t size = n, dsize = dn;
        sid = sec_alloc(a, size > 0 ? size : 1);
        uint16_t *db = sec_units(dsize);
        if (sid == NULL || db == NULL) {
            sec_free(a, sid);
            if (db != NULL)
                sec_units_free(db, dsize);
            sid = NULL;
            e = burrow_err_out_of_memory;
            break;
        }
        e = syscall_lookup_account_name(sys, acc, sid, &n, db, &dn, &use);
        if (BURROW_OK(e)) {
            if (domain != NULL)
                *domain = syscall_utf16_to_string(
                    a, (Slice){db, (Int)dsize, (Int)dsize, TYPE_UINT16});
            if (acc_type != NULL)
                *acc_type = use;
            sec_units_free(db, dsize);
            break;
        }
        sec_units_free(db, dsize);
        sec_free(a, sid);
        sid = NULL;
        if (!sec_insufficient(e) || n <= size)
            break;
    }
    if (sys != NULL)
        burrow__syscall_wstring_free(&wsys);
    burrow__syscall_wstring_free(&wacc);
    BURROW_OUT(err, e);
    return sid;
}

Str syscall_sid_string(SyscallSID *sid, Alloc *a, Error *err) {
    uint16_t *s = NULL;
    Error e = syscall_convert_sid_to_string_sid(sid, &s);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return BURROW_STR_EMPTY;
    }
    Str r = sec_ptr_to_string(a, s);
    (void)syscall_local_free((SyscallHandle)(Uintptr)(void *)s, NULL);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return r;
}

Int syscall_sid_len(SyscallSID *sid) {
    return (Int)syscall_get_length_sid(sid);
}

SyscallSID *syscall_sid_copy(SyscallSID *sid, Alloc *a, Error *err) {
    uint32_t n = (uint32_t)syscall_sid_len(sid);
    SyscallSID *c = sec_alloc(a, n > 0 ? n : 1);
    if (c == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    Error e = syscall_copy_sid(n, c, sid);
    if (BURROW_FAILED(e)) {
        sec_free(a, c);
        BURROW_OUT(err, e);
        return NULL;
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return c;
}

Str syscall_sid_lookup_account(SyscallSID *sid, Alloc *a, Str system, Str *domain,
                               uint32_t *acc_type, Error *err) {
    if (domain != NULL)
        *domain = BURROW_STR_EMPTY;
    if (acc_type != NULL)
        *acc_type = 0;
    burrow__SyscallWString wsys;
    uint16_t *sys = NULL;
    if (system.len > 0) {
        sys = burrow__syscall_wstring(&wsys, system, err);
        if (sys == NULL)
            return BURROW_STR_EMPTY;
    }
    uint32_t n = 50, dn = 50, use = 0;
    Str account = BURROW_STR_EMPTY;
    Error e = BURROW_NO_ERROR;
    for (;;) {
        uint32_t size = n, dsize = dn;
        uint16_t *b = sec_units(size);
        uint16_t *db = sec_units(dsize);
        if (b == NULL || db == NULL) {
            if (b != NULL)
                sec_units_free(b, size);
            if (db != NULL)
                sec_units_free(db, dsize);
            e = burrow_err_out_of_memory;
            break;
        }
        e = syscall_lookup_account_sid(sys, sid, b, &n, db, &dn, &use);
        if (BURROW_OK(e)) {
            account = syscall_utf16_to_string(
                a, (Slice){b, (Int)size, (Int)size, TYPE_UINT16});
            if (domain != NULL)
                *domain = syscall_utf16_to_string(
                    a, (Slice){db, (Int)dsize, (Int)dsize, TYPE_UINT16});
            if (acc_type != NULL)
                *acc_type = use;
        }
        sec_units_free(b, size);
        sec_units_free(db, dsize);
        if (BURROW_OK(e) || !sec_insufficient(e) || n <= size)
            break;
    }
    if (sys != NULL)
        burrow__syscall_wstring_free(&wsys);
    BURROW_OUT(err, e);
    return account;
}

void syscall_sid_free(Alloc *a, SyscallSID *sid) {
    sec_free(a, sid);
}

SyscallToken syscall_open_current_process_token(Error *err) {
    Error e = BURROW_NO_ERROR;
    SyscallHandle p = syscall_get_current_process(&e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return 0;
    }
    SyscallToken t = 0;
    e = syscall_open_process_token(p, (uint32_t)SYSCALL_TOKEN_QUERY, &t);
    BURROW_OUT(err, e);
    return t;
}

Error syscall_token_close(SyscallToken t) {
    return syscall_close_handle((SyscallHandle)t);
}

/* Go's Token.getInfo: what GetTokenInformation says for class, in memory
 * from a, starting with init_size bytes and growing to what it asks for. */
static void *sec_token_info(SyscallToken t, uint32_t class_, uint32_t init_size,
                            Alloc *a, Error *err) {
    uint32_t n = init_size;
    for (;;) {
        uint32_t size = n;
        uint8_t *b = sec_alloc(a, size > 0 ? size : 1);
        if (b == NULL) {
            BURROW_OUT(err, burrow_err_out_of_memory);
            return NULL;
        }
        Error e = syscall_get_token_information(t, class_, b, size, &n);
        if (BURROW_OK(e)) {
            BURROW_OUT(err, BURROW_NO_ERROR);
            return b;
        }
        sec_free(a, b);
        if (!sec_insufficient(e) || n <= size) {
            BURROW_OUT(err, e);
            return NULL;
        }
    }
}

SyscallTokenuser *syscall_token_get_token_user(SyscallToken t, Alloc *a, Error *err) {
    return sec_token_info(t, (uint32_t)SYSCALL_TOKEN_USER, 50, a, err);
}

SyscallTokenprimarygroup *syscall_token_get_token_primary_group(SyscallToken t,
                                                                Alloc *a, Error *err) {
    return sec_token_info(t, (uint32_t)SYSCALL_TOKEN_PRIMARY_GROUP, 50, a, err);
}

void syscall_token_info_free(Alloc *a, void *info) {
    sec_free(a, info);
}

Str syscall_token_get_user_profile_directory(SyscallToken t, Alloc *a, Error *err) {
    uint32_t n = 100;
    for (;;) {
        uint32_t size = n;
        uint16_t *b = sec_units(size);
        if (b == NULL) {
            BURROW_OUT(err, burrow_err_out_of_memory);
            return BURROW_STR_EMPTY;
        }
        Error e = syscall_get_user_profile_directory(t, b, &n);
        if (BURROW_OK(e)) {
            Str s = syscall_utf16_to_string(
                a, (Slice){b, (Int)size, (Int)size, TYPE_UINT16});
            sec_units_free(b, size);
            BURROW_OUT(err, BURROW_NO_ERROR);
            return s;
        }
        sec_units_free(b, size);
        if (!sec_insufficient(e) || n <= size) {
            BURROW_OUT(err, e);
            return BURROW_STR_EMPTY;
        }
    }
}

#endif /* BURROW_OS_WINDOWS */

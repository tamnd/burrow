/* syscall's DLLs on Windows: SyscallN and the numbered Syscalls under it,
 * DLL, Proc, LazyDLL and LazyProc, and the UTF-16 conversions every Windows
 * call that takes a name needs.
 *
 * Go calls LoadLibraryExW and GetProcAddress through SyscallN, with their
 * addresses filled in by the runtime. burrow has the C library link them, and
 * calls them through pal_load_library and pal_proc_address, which give the
 * same handles and the same errors.
 *
 * Derived from Go's src/syscall/dll_windows.go, syscall_windows.go and
 * wtf8_windows.go, and runtime/syscall_windows.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/syscall.h"

#if defined(BURROW_OS_WINDOWS)

#include "burrow/atomic.h"
#include "burrow/mem/heap.h"
#include "burrow/slice.h"
#include "burrow/unicode/utf16.h"
#include "burrow/utf8.h"

#include "internal.h"

#include <string.h>

/* ------------------------------------------------------------- SyscallN */

/* Go's maxArgs on Windows. */
enum { MAX_ARGS = 42 };

Uintptr burrow__syscall_n(Uintptr fn, const Uintptr *args, Int n, Uintptr *r2,
                          SyscallErrno *err) {
    if (n > MAX_ARGS)
        panic_str(BURROW_S("runtime: SyscallN has too many arguments"));
    uintptr_t e = 0;
    Uintptr r = (Uintptr)pal_call((void *)(uintptr_t)fn, (const uintptr_t *)args,
                                  (int32_t)n, -1, &e);
    BURROW_OUT(r2, 0);
    BURROW_OUT(err, (SyscallErrno)e);
    return r;
}

Uintptr syscall_syscall_n(Uintptr p, Slice args, Uintptr *r2, SyscallErrno *err) {
    return burrow__syscall_n(p, (const Uintptr *)args.p, args.len, r2, err);
}

/* Go's syscalln, which the numbered ones are: the first nargs of what they
 * were given. */
static Uintptr syscalln(Uintptr trap, Uintptr nargs, const Uintptr *args, Int max,
                        Uintptr *r2, SyscallErrno *err) {
    if (nargs > (Uintptr)max)
        panic_str(BURROW_S("syscall: n > len(args)"));
    return burrow__syscall_n(trap, args, (Int)nargs, r2, err);
}

Uintptr syscall_syscall(Uintptr trap, Uintptr nargs, Uintptr a1, Uintptr a2, Uintptr a3,
                        Uintptr *r2, SyscallErrno *err) {
    const Uintptr args[] = {a1, a2, a3};
    return syscalln(trap, nargs, args, 3, r2, err);
}

Uintptr syscall_syscall6(Uintptr trap, Uintptr nargs, Uintptr a1, Uintptr a2,
                         Uintptr a3, Uintptr a4, Uintptr a5, Uintptr a6, Uintptr *r2,
                         SyscallErrno *err) {
    const Uintptr args[] = {a1, a2, a3, a4, a5, a6};
    return syscalln(trap, nargs, args, 6, r2, err);
}

Uintptr syscall_syscall9(Uintptr trap, Uintptr nargs, Uintptr a1, Uintptr a2,
                         Uintptr a3, Uintptr a4, Uintptr a5, Uintptr a6, Uintptr a7,
                         Uintptr a8, Uintptr a9, Uintptr *r2, SyscallErrno *err) {
    const Uintptr args[] = {a1, a2, a3, a4, a5, a6, a7, a8, a9};
    return syscalln(trap, nargs, args, 9, r2, err);
}

Uintptr syscall_syscall12(Uintptr trap, Uintptr nargs, Uintptr a1, Uintptr a2,
                          Uintptr a3, Uintptr a4, Uintptr a5, Uintptr a6, Uintptr a7,
                          Uintptr a8, Uintptr a9, Uintptr a10, Uintptr a11, Uintptr a12,
                          Uintptr *r2, SyscallErrno *err) {
    const Uintptr args[] = {a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12};
    return syscalln(trap, nargs, args, 12, r2, err);
}

Uintptr syscall_syscall15(Uintptr trap, Uintptr nargs, Uintptr a1, Uintptr a2,
                          Uintptr a3, Uintptr a4, Uintptr a5, Uintptr a6, Uintptr a7,
                          Uintptr a8, Uintptr a9, Uintptr a10, Uintptr a11, Uintptr a12,
                          Uintptr a13, Uintptr a14, Uintptr a15, Uintptr *r2,
                          SyscallErrno *err) {
    const Uintptr args[] = {a1, a2,  a3,  a4,  a5,  a6,  a7, a8,
                            a9, a10, a11, a12, a13, a14, a15};
    return syscalln(trap, nargs, args, 15, r2, err);
}

Uintptr syscall_syscall18(Uintptr trap, Uintptr nargs, Uintptr a1, Uintptr a2,
                          Uintptr a3, Uintptr a4, Uintptr a5, Uintptr a6, Uintptr a7,
                          Uintptr a8, Uintptr a9, Uintptr a10, Uintptr a11, Uintptr a12,
                          Uintptr a13, Uintptr a14, Uintptr a15, Uintptr a16,
                          Uintptr a17, Uintptr a18, Uintptr *r2, SyscallErrno *err) {
    const Uintptr args[] = {a1,  a2,  a3,  a4,  a5,  a6,  a7,  a8,  a9,
                            a10, a11, a12, a13, a14, a15, a16, a17, a18};
    return syscalln(trap, nargs, args, 18, r2, err);
}

/* Go's errnoErr on Windows: a call that failed without setting the last
 * error is EINVAL. */
Error burrow__syscall_errno_err(SyscallErrno e) {
    return syscall_errno_as_error(e == 0 ? SYSCALL_EINVAL : e, error_allocator());
}

/* ------------------------------------------------------------- DLLError */

/* The DLLError first, so errors_as hands back a pointer to it. */
typedef struct DLLErrorBox {
    SyscallDLLError e;
} DLLErrorBox;

static Str dll_error_message(const void *self) {
    return ((const SyscallDLLError *)self)->msg;
}

static Error dll_error_unwrap_slot(const void *self) {
    return ((const SyscallDLLError *)self)->err;
}

static const Type dll_error_desc = {
    {(const Byte *)"DLLError", 8},
    {(const Byte *)"syscall", 7},
    KIND_STRUCT,
    (uint32_t)sizeof(SyscallDLLError),
    (uint16_t)_Alignof(SyscallDLLError),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x73646c65U, /* "sdle" */
    NULL,
};

const Type *const TYPE_SYSCALL_DLL_ERROR = &dll_error_desc;

static Error dll_error_clone(const void *self, Alloc *a);

static const ErrorVT dll_error_vt = {
    &dll_error_desc, dll_error_message, dll_error_unwrap_slot, NULL, NULL, NULL,
    dll_error_clone,
};

static Byte *dll_put(Byte *p, Str s) {
    if (s.len > 0)
        memcpy(p, s.p, (size_t)s.len);
    return p + s.len;
}

/* One allocation: the box, then obj_name, then the message, which is the
 * parts one after another. */
static Error dll_error_new(Alloc *a, Error err, Str obj_name, const Str *parts, int n) {
    Int mlen = 0;
    for (int i = 0; i < n; i++)
        mlen += parts[i].len;
    DLLErrorBox *b = (DLLErrorBox *)mem_alloc_nozero(
        a, sizeof(DLLErrorBox) + (size_t)obj_name.len + (size_t)mlen,
        _Alignof(DLLErrorBox));
    if (b == NULL)
        return burrow_err_out_of_memory;
    Byte *p = (Byte *)(b + 1);
    b->e.err = err;
    b->e.obj_name = str_from_bytes(p, obj_name.len);
    p = dll_put(p, obj_name);
    b->e.msg = str_from_bytes(p, mlen);
    for (int i = 0; i < n; i++)
        p = dll_put(p, parts[i]);
    return (Error){&dll_error_vt, b};
}

static Error dll_error_clone(const void *self, Alloc *a) {
    const SyscallDLLError *e = (const SyscallDLLError *)self;
    return dll_error_new(a, error_retain(a, e->err), e->obj_name, &e->msg, 1);
}

Str syscall_dll_error_error(const SyscallDLLError *e) {
    return e->msg;
}

Error syscall_dll_error_unwrap(const SyscallDLLError *e) {
    return e->err;
}

/* ------------------------------------------------------------------ DLL */

/* Panics with err, kept on the heap since the panic leaves this frame. */
static BURROW_NORETURN void panic_error(Error err) {
    Error *box = BURROW_NEW(heap_allocator(), Error);
    if (box == NULL)
        panic_str(error_text(err));
    *box = error_retain(heap_allocator(), err);
    panic((Any){TYPE_ERROR, box});
}

/* Go's sysdll.IsSystemDLL: one of the DLLs syscall loads itself, which is
 * only looked for in the system directory. */
static bool is_system_dll(Str name) {
    for (Int i = 0; i < BURROW__SYSCALL_NMODS; i++)
        if (str_eq(burrow__syscall_mods[i].name, name))
            return true;
    return false;
}

SyscallDLL *syscall_load_dll(Alloc *a, Str name, Error *err) {
    burrow__SyscallWString w;
    uint16_t *namep = burrow__syscall_wstring(&w, name, err);
    if (namep == NULL)
        return NULL;
    uintptr_t e = 0;
    uintptr_t h = pal_load_library(namep, is_system_dll(name), &e);
    burrow__syscall_wstring_free(&w);
    if (h == 0) {
        Error en = syscall_errno_as_error((SyscallErrno)e, error_allocator());
        const Str parts[] = {BURROW_S("Failed to load "), name, BURROW_S(": "),
                             error_text(en)};
        BURROW_OUT(err, dll_error_new(error_allocator(), en, name, parts, 4));
        return NULL;
    }
    SyscallDLL *d = mem_alloc_nozero(a, sizeof(SyscallDLL) + (size_t)name.len,
                                     _Alignof(SyscallDLL));
    if (d == NULL) {
        (void)syscall_free_library((SyscallHandle)h);
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    Byte *p = (Byte *)(d + 1);
    dll_put(p, name);
    d->name = str_from_bytes(p, name.len);
    d->handle = (SyscallHandle)h;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return d;
}

SyscallDLL *syscall_must_load_dll(Alloc *a, Str name) {
    Error err = BURROW_NO_ERROR;
    SyscallDLL *d = syscall_load_dll(a, name, &err);
    if (BURROW_FAILED(err))
        panic_error(err);
    return d;
}

SyscallProc *syscall_dll_find_proc(SyscallDLL *d, Alloc *a, Str name, Error *err) {
    burrow__SyscallCString c;
    uint8_t *namep = burrow__syscall_cstring(&c, name, err);
    if (namep == NULL)
        return NULL;
    uintptr_t e = 0;
    void *addr = pal_proc_address((uintptr_t)d->handle, (const char *)namep, &e);
    burrow__syscall_cstring_free(&c);
    if (addr == NULL) {
        Error en = syscall_errno_as_error((SyscallErrno)e, error_allocator());
        const Str parts[] = {BURROW_S("Failed to find "),
                             name,
                             BURROW_S(" procedure in "),
                             d->name,
                             BURROW_S(": "),
                             error_text(en)};
        BURROW_OUT(err, dll_error_new(error_allocator(), en, name, parts, 6));
        return NULL;
    }
    SyscallProc *p = mem_alloc_nozero(a, sizeof(SyscallProc) + (size_t)name.len,
                                      _Alignof(SyscallProc));
    if (p == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    Byte *s = (Byte *)(p + 1);
    dll_put(s, name);
    p->dll = d;
    p->name = str_from_bytes(s, name.len);
    p->addr = (Uintptr)(uintptr_t)addr;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return p;
}

SyscallProc *syscall_dll_must_find_proc(SyscallDLL *d, Alloc *a, Str name) {
    Error err = BURROW_NO_ERROR;
    SyscallProc *p = syscall_dll_find_proc(d, a, name, &err);
    if (BURROW_FAILED(err))
        panic_error(err);
    return p;
}

Error syscall_dll_release(SyscallDLL *d) {
    return syscall_free_library(d->handle);
}

void syscall_dll_free(Alloc *a, SyscallDLL *d) {
    if (d != NULL)
        mem_free(a, d, sizeof(SyscallDLL) + (size_t)d->name.len, _Alignof(SyscallDLL));
}

void syscall_proc_free(Alloc *a, SyscallProc *p) {
    if (p != NULL)
        mem_free(a, p, sizeof(SyscallProc) + (size_t)p->name.len,
                 _Alignof(SyscallProc));
}

Uintptr syscall_proc_addr(const SyscallProc *p) {
    return p->addr;
}

Uintptr syscall_proc_call(const SyscallProc *p, Slice args, Uintptr *r2, Error *err) {
    SyscallErrno e = 0;
    Uintptr r = syscall_syscall_n(p->addr, args, r2, &e);
    BURROW_OUT(err, syscall_errno_as_error(e, error_allocator()));
    return r;
}

/* -------------------------------------------------------------- LazyDLL */

SyscallLazyDLL *syscall_new_lazy_dll(Alloc *a, Str name) {
    SyscallLazyDLL *d = BURROW_NEW(a, SyscallLazyDLL);
    if (d != NULL)
        d->name = name;
    return d;
}

Error syscall_lazy_dll_load(SyscallLazyDLL *d) {
    if (burrow__atomic_load_acquire_ptr((void *const *)&d->dll) != NULL)
        return BURROW_NO_ERROR;
    Error err = BURROW_NO_ERROR;
    sync_mutex_lock(&d->mu);
    if (d->dll == NULL) {
        SyscallDLL *dll = syscall_load_dll(heap_allocator(), d->name, &err);
        if (BURROW_OK(err))
            burrow__atomic_store_release_ptr((void **)&d->dll, dll);
    }
    sync_mutex_unlock(&d->mu);
    return err;
}

Uintptr syscall_lazy_dll_handle(SyscallLazyDLL *d) {
    Error err = syscall_lazy_dll_load(d);
    if (BURROW_FAILED(err))
        panic_error(err);
    return (Uintptr)d->dll->handle;
}

SyscallLazyProc *syscall_lazy_dll_new_proc(SyscallLazyDLL *d, Alloc *a, Str name) {
    SyscallLazyProc *p = BURROW_NEW(a, SyscallLazyProc);
    if (p != NULL) {
        p->l = d;
        p->name = name;
    }
    return p;
}

Error syscall_lazy_proc_find(SyscallLazyProc *p) {
    if (burrow__atomic_load_acquire_ptr((void *const *)&p->proc) != NULL)
        return BURROW_NO_ERROR;
    Error err = BURROW_NO_ERROR;
    sync_mutex_lock(&p->mu);
    if (p->proc == NULL) {
        err = syscall_lazy_dll_load(p->l);
        if (BURROW_OK(err)) {
            SyscallProc *proc =
                syscall_dll_find_proc(p->l->dll, heap_allocator(), p->name, &err);
            if (BURROW_OK(err))
                burrow__atomic_store_release_ptr((void **)&p->proc, proc);
        }
    }
    sync_mutex_unlock(&p->mu);
    return err;
}

Uintptr syscall_lazy_proc_addr(SyscallLazyProc *p) {
    Error err = syscall_lazy_proc_find(p);
    if (BURROW_FAILED(err))
        panic_error(err);
    return p->proc->addr;
}

Uintptr syscall_lazy_proc_call(SyscallLazyProc *p, Slice args, Uintptr *r2,
                               Error *err) {
    Error ferr = syscall_lazy_proc_find(p);
    if (BURROW_FAILED(ferr))
        panic_error(ferr);
    return syscall_proc_call(p->proc, args, r2, err);
}

/* ---------------------------------------------------------------- UTF-16 */

/* The bytes of a WTF-8 surrogate, ED A0 80 to ED BF BF, which UTF-8 says are
 * not valid. */
static bool wtf8_surrogate(const Byte *s, Int n) {
    return n >= 3 && s[0] == 0xED && s[1] >= 0xA0 && s[1] <= 0xBF && s[2] >= 0x80 &&
           s[2] <= 0xBF;
}

/* Go's encodeWTF16: s into buf, which has room for s.len units, and how many
 * it wrote. */
static Int encode_wtf16(Str s, uint16_t *buf) {
    Int n = 0;
    for (Int i = 0; i < s.len;) {
        Int size = 0;
        Rune r = utf8_decode_rune_in_string(str_from_bytes(s.p + i, s.len - i), &size);
        if (r == UTF8_RUNE_ERROR && wtf8_surrogate(s.p + i, s.len - i)) {
            const Byte *c = s.p + i;
            buf[n++] =
                (uint16_t)((c[0] & 0x0F) << 12 | (c[1] & 0x3F) << 6 | (c[2] & 0x3F));
            i += 3;
            continue;
        }
        i += size;
        if ((r >= 0 && r < 0xD800) || (r >= 0xE000 && r < 0x10000)) {
            buf[n++] = (uint16_t)r;
        } else if (r >= 0x10000 && r <= 0x10FFFF) {
            Rune r2 = 0;
            buf[n++] = (uint16_t)utf16_encode_rune(r, &r2);
            buf[n++] = (uint16_t)r2;
        } else {
            buf[n++] = (uint16_t)UTF8_RUNE_ERROR;
        }
    }
    return n;
}

Slice syscall_utf16_from_string(Alloc *a, Str s, Error *err) {
    if (s.len > 0 && memchr(s.p, 0, (size_t)s.len) != NULL) {
        BURROW_OUT(err, syscall_errno_as_error(SYSCALL_EINVAL, error_allocator()));
        return (Slice){NULL, 0, 0, TYPE_UINT16};
    }
    Slice b = slice_make(a, TYPE_UINT16, 0, s.len + 1);
    if (b.p == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return b;
    }
    uint16_t *u = (uint16_t *)b.p;
    Int n = encode_wtf16(s, u);
    u[n] = 0;
    b.len = n + 1;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return b;
}

uint16_t *syscall_utf16_ptr_from_string(Alloc *a, Str s, Error *err) {
    Slice b = syscall_utf16_from_string(a, s, err);
    return (uint16_t *)b.p;
}

Slice syscall_string_to_utf16(Alloc *a, Str s) {
    Error err = BURROW_NO_ERROR;
    Slice b = syscall_utf16_from_string(a, s, &err);
    if (BURROW_FAILED(err))
        panic_str(BURROW_S("syscall: string with NUL passed to StringToUTF16"));
    return b;
}

uint16_t *syscall_string_to_utf16_ptr(Alloc *a, Str s) {
    return (uint16_t *)syscall_string_to_utf16(a, s).p;
}

/* Go's decodeWTF16: units into buf, which has room, and how many bytes it
 * wrote. */
static Int decode_wtf16(const uint16_t *s, Int n, Byte *buf, Int room) {
    Int w = 0;
    for (Int i = 0; i < n; i++) {
        Rune r = s[i];
        if (r >= 0xD800 && r < 0xDC00 && i + 1 < n && s[i + 1] >= 0xDC00 &&
            s[i + 1] < 0xE000) {
            r = utf16_decode_rune(r, s[i + 1]);
            i++;
        } else if (r >= 0xD800 && r < 0xE000) {
            buf[w++] = (Byte)(0xE0 | (r >> 12));
            buf[w++] = (Byte)(0x80 | ((r >> 6) & 0x3F));
            buf[w++] = (Byte)(0x80 | (r & 0x3F));
            continue;
        }
        w += utf8_encode_rune((Slice){buf + w, room - w, room - w, TYPE_BYTE}, r);
    }
    return w;
}

Str syscall_utf16_to_string(Alloc *a, Slice s) {
    const uint16_t *u = (const uint16_t *)s.p;
    Int n = s.len;
    Int max = 0;
    for (Int i = 0; i < s.len; i++) {
        if (u[i] == 0) {
            n = i;
            break;
        }
        max += u[i] <= 0x7F ? 1 : u[i] <= 0x7FF ? 2 : 3;
    }
    if (max == 0)
        return BURROW_STR_EMPTY;
    Byte *buf = mem_alloc_nozero(a, (size_t)max, 1);
    if (buf == NULL)
        return BURROW_STR_EMPTY;
    Int w = decode_wtf16(u, n, buf, max);
    return str_from_bytes(buf, w);
}

/* A string as UTF-16 for the length of one call, the way
 * burrow__syscall_cstring does it for a C string. A string of n bytes is at
 * most n units, and the 0 after it is one more. */
uint16_t *burrow__syscall_wstring(burrow__SyscallWString *h, Str s, Error *err) {
    h->heap = NULL;
    if (s.len > 0 && memchr(s.p, 0, (size_t)s.len) != NULL) {
        BURROW_OUT(err, syscall_errno_as_error(SYSCALL_EINVAL, error_allocator()));
        return NULL;
    }
    uint16_t *p = h->buf;
    if ((size_t)s.len >= sizeof h->buf / sizeof h->buf[0]) {
        h->size = ((size_t)s.len + 1) * sizeof(uint16_t);
        h->heap = mem_alloc(heap_allocator(), h->size, _Alignof(uint16_t));
        if (h->heap == NULL) {
            BURROW_OUT(err, burrow_err_out_of_memory);
            return NULL;
        }
        p = h->heap;
    }
    p[encode_wtf16(s, p)] = 0;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return p;
}

void burrow__syscall_wstring_free(burrow__SyscallWString *h) {
    if (h->heap != NULL)
        mem_free(heap_allocator(), h->heap, h->size, _Alignof(uint16_t));
    h->heap = NULL;
}

#endif /* BURROW_OS_WINDOWS */

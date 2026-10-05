/* syscall.Getenv, Setenv, Unsetenv, Clearenv and Environ, which os's are
 * built on.
 *
 * Derived from Go's src/syscall/env_unix.go and env_windows.go.
 * Go source: go1.27.1.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/syscall.h"

#include "burrow/mem/heap.h"
#include "burrow/slice.h"
#include "burrow/sync.h"

#include "internal.h"

#include <string.h>

static Str env_copy(Alloc *a, const Byte *p, Int n) {
    if (n <= 0)
        return (Str){(const Byte *)"", 0};
    Byte *b = (Byte *)mem_alloc_nozero(a, (size_t)n, 1);
    if (b == NULL)
        return (Str){NULL, 0};
    memcpy(b, p, (size_t)n);
    return str_from_bytes(b, n);
}

/* s as a NUL terminated string on the heap, for the PAL. */
static char *env_cstr(Str s) {
    char *c = (char *)mem_alloc_nozero(heap_allocator(), (size_t)s.len + 1, 1);
    if (c == NULL)
        return NULL;
    if (s.len > 0)
        memcpy(c, s.p, (size_t)s.len);
    c[s.len] = 0;
    return c;
}

static void env_cstr_free(char *c) {
    if (c != NULL)
        mem_free(heap_allocator(), c, strlen(c) + 1, 1);
}

static bool env_has_nul(Str s) {
    return s.len > 0 && memchr(s.p, 0, (size_t)s.len) != NULL;
}

#if !defined(BURROW_OS_WINDOWS)

/* envs: every "key=value" in the order the process started with, then the
 * new ones on the end. An empty entry is one that was unset, or a later
 * duplicate of a key, which copyenv clears so that unsetting the first cannot
 * uncover the second. Go keeps a map from key to index beside it, and an
 * environment is small enough that a walk does the same job. */
static SyncOnce syscall_env_once;
static SyncRWMutex env_mu;
static Str *env_envs;
static Int env_envs_len;
static Int env_envs_cap;

/* The key of an entry, or a negative length when it has no '='. */
static Int env_key_len(Str s) {
    const void *eq = s.len > 0 ? memchr(s.p, '=', (size_t)s.len) : NULL;
    return eq == NULL ? -1 : (Int)((const Byte *)eq - s.p);
}

static Int env_find(Str key) {
    for (Int i = 0; i < env_envs_len; i++) {
        Str s = env_envs[i];
        Int k = env_key_len(s);
        if (k == key.len && memcmp(s.p, key.p, (size_t)k) == 0)
            return i;
    }
    return -1;
}

static void env_free_entry(Int i) {
    if (env_envs[i].len > 0)
        mem_free(heap_allocator(), (void *)(uintptr_t)env_envs[i].p,
                 (size_t)env_envs[i].len, 1);
    env_envs[i] = (Str){NULL, 0};
}

static bool env_grow(void) {
    if (env_envs_len < env_envs_cap)
        return true;
    Int ncap = env_envs_cap < 16 ? 16 : env_envs_cap * 2;
    Str *n = (Str *)mem_alloc_nozero(heap_allocator(), (size_t)ncap * sizeof(Str),
                                     _Alignof(Str));
    if (n == NULL)
        return false;
    if (env_envs_len > 0)
        memcpy(n, env_envs, (size_t)env_envs_len * sizeof(Str));
    if (env_envs != NULL)
        mem_free(heap_allocator(), env_envs, (size_t)env_envs_cap * sizeof(Str),
                 _Alignof(Str));
    env_envs = n;
    env_envs_cap = ncap;
    return true;
}

/* copyenv. An entry that cannot be copied is left out, which is the closest
 * this can get to Go, where the copy cannot fail. */
static void env_init(void *env) {
    (void)env;
    for (const char *const *e = pal_environ(); e != NULL && *e != NULL; e++) {
        if (!env_grow())
            break;
        Str s = env_copy(heap_allocator(), (const Byte *)*e, (Int)strlen(*e));
        if (s.p == NULL)
            break;
        Int k = env_key_len(s);
        if (k >= 0 && env_find(str_from_bytes(s.p, k)) >= 0) {
            if (s.len > 0)
                mem_free(heap_allocator(), (void *)(uintptr_t)s.p, (size_t)s.len, 1);
            s = (Str){NULL, 0};
        }
        env_envs[env_envs_len++] = s;
    }
}

static void env_start(void) {
    sync_once_do(&syscall_env_once, BURROW_FN(Func, env_init, NULL));
}

Str syscall_getenv(Alloc *a, Str key, bool *found) {
    BURROW_OUT(found, false);
    env_start();
    if (key.len == 0)
        return (Str){NULL, 0};
    sync_rw_mutex_r_lock(&env_mu);
    Int i = env_find(key);
    Str v = {NULL, 0};
    if (i >= 0) {
        Str s = env_envs[i];
        v = env_copy(a, s.p + key.len + 1, s.len - key.len - 1);
        BURROW_OUT(found, true);
    }
    sync_rw_mutex_r_unlock(&env_mu);
    return v;
}

Error syscall_setenv(Str key, Str value) {
    env_start();
    if (key.len == 0 || env_has_nul(key) || env_has_nul(value) ||
        memchr(key.p, '=', (size_t)key.len) != NULL)
        return burrow__syscall_errno_err(SYSCALL_EINVAL);
    Int n = key.len + 1 + value.len;
    Byte *kv = (Byte *)mem_alloc_nozero(heap_allocator(), (size_t)n, 1);
    if (kv == NULL)
        return burrow_err_out_of_memory;
    memcpy(kv, key.p, (size_t)key.len);
    kv[key.len] = '=';
    if (value.len > 0)
        memcpy(kv + key.len + 1, value.p, (size_t)value.len);

    sync_rw_mutex_lock(&env_mu);
    Int i = env_find(key);
    if (i < 0) {
        if (!env_grow()) {
            sync_rw_mutex_unlock(&env_mu);
            mem_free(heap_allocator(), kv, (size_t)n, 1);
            return burrow_err_out_of_memory;
        }
        i = env_envs_len++;
    } else {
        env_free_entry(i);
    }
    env_envs[i] = str_from_bytes(kv, n);
    /* runtimeSetenv. Go does this when cgo is in, so that C code sees the
     * change, and a C program always has C code in it. */
    char *ck = env_cstr(key);
    char *cv = env_cstr(value);
    if (ck != NULL && cv != NULL)
        pal_setenv(ck, cv, NULL);
    sync_rw_mutex_unlock(&env_mu);
    env_cstr_free(ck);
    env_cstr_free(cv);
    return BURROW_NO_ERROR;
}

Error syscall_unsetenv(Str key) {
    env_start();
    sync_rw_mutex_lock(&env_mu);
    Int i = env_find(key);
    if (i >= 0)
        env_free_entry(i);
    char *ck = env_has_nul(key) ? NULL : env_cstr(key);
    if (ck != NULL)
        pal_setenv(ck, NULL, NULL);
    sync_rw_mutex_unlock(&env_mu);
    env_cstr_free(ck);
    return BURROW_NO_ERROR;
}

void syscall_clearenv(void) {
    env_start();
    sync_rw_mutex_lock(&env_mu);
    for (Int i = 0; i < env_envs_len; i++) {
        Int k = env_key_len(env_envs[i]);
        if (k >= 0) {
            char *ck = env_cstr(str_from_bytes(env_envs[i].p, k));
            if (ck != NULL)
                pal_setenv(ck, NULL, NULL);
            env_cstr_free(ck);
        }
        env_free_entry(i);
    }
    env_envs_len = 0;
    sync_rw_mutex_unlock(&env_mu);
}

Slice syscall_environ(Alloc *a) {
    env_start();
    Slice out = slice_nil(TYPE_STRING);
    sync_rw_mutex_r_lock(&env_mu);
    for (Int i = 0; i < env_envs_len; i++) {
        if (env_envs[i].len == 0)
            continue;
        Str s = env_copy(a, env_envs[i].p, env_envs[i].len);
        if (s.p == NULL)
            break;
        out = slice_append(a, out, &s, 1);
    }
    sync_rw_mutex_r_unlock(&env_mu);
    return out;
}

#else

/* Windows keeps the environment itself, and Go asks it every time. */

/* The Errno for a failed PAL call, or its text when it has none. */
static Error env_pal_err(PalErrno pe) {
    SyscallErrno n = syscall_errno_from_pal(pe);
    if (n == 0)
        n = (SyscallErrno)pal_errno_native(pe);
    if (n == 0)
        return errors_new(error_allocator(), str_from_cstr(pal_errno_string(pe)));
    return burrow__syscall_errno_err(n);
}

Str syscall_getenv(Alloc *a, Str key, bool *found) {
    BURROW_OUT(found, false);
    if (env_has_nul(key))
        return (Str){NULL, 0};
    char *ck = env_cstr(key);
    if (ck == NULL)
        return (Str){NULL, 0};
    Str v = {NULL, 0};
    int64_t cap = 256;
    for (;;) {
        Byte *buf = (Byte *)mem_alloc_nozero(a, (size_t)cap, 1);
        if (buf == NULL)
            break;
        PalErrno pe = PAL_OK;
        int64_t n = pal_getenv(ck, (char *)buf, cap, &pe);
        if (n >= 0) {
            v = n == 0 ? (Str){(const Byte *)"", 0} : str_from_bytes(buf, (Int)n);
            if (n == 0)
                mem_free(a, buf, (size_t)cap, 1);
            BURROW_OUT(found, true);
            break;
        }
        mem_free(a, buf, (size_t)cap, 1);
        if (pe != PAL_ERANGE || cap > ((int64_t)1 << 24))
            break;
        cap *= 4;
    }
    env_cstr_free(ck);
    return v;
}

static Error env_set(Str key, const Str *value) {
    if (env_has_nul(key) || (value != NULL && env_has_nul(*value)))
        return burrow__syscall_errno_err(SYSCALL_EINVAL);
    Error e = BURROW_NO_ERROR;
    char *ck = env_cstr(key);
    char *cv = value == NULL ? NULL : env_cstr(*value);
    PalErrno pe = PAL_OK;
    if (ck == NULL || (value != NULL && cv == NULL))
        e = burrow_err_out_of_memory;
    else if (!pal_setenv(ck, cv, &pe))
        e = env_pal_err(pe);
    env_cstr_free(ck);
    env_cstr_free(cv);
    return e;
}

Error syscall_setenv(Str key, Str value) {
    return env_set(key, &value);
}

Error syscall_unsetenv(Str key) {
    return env_set(key, NULL);
}

/* The whole block, NUL after each entry, on the heap. */
static Byte *env_block(int64_t *size, int64_t *cap_out) {
    int64_t cap = 4096;
    for (;;) {
        Byte *buf = (Byte *)mem_alloc_nozero(heap_allocator(), (size_t)cap, 1);
        if (buf == NULL)
            return NULL;
        PalErrno pe = PAL_OK;
        int64_t n = pal_environ_read((char *)buf, cap, &pe);
        if (n >= 0) {
            *size = n;
            *cap_out = cap;
            return buf;
        }
        mem_free(heap_allocator(), buf, (size_t)cap, 1);
        if (pe != PAL_ERANGE || cap > ((int64_t)1 << 26))
            return NULL;
        cap *= 4;
    }
}

Slice syscall_environ(Alloc *a) {
    Slice out = slice_nil(TYPE_STRING);
    int64_t size = 0;
    int64_t cap = 0;
    Byte *buf = env_block(&size, &cap);
    if (buf == NULL)
        return out;
    for (int64_t i = 0; i < size;) {
        Int n = (Int)strlen((const char *)buf + i);
        Str s = env_copy(a, buf + i, n);
        if (n > 0 && s.p == NULL)
            break;
        out = slice_append(a, out, &s, 1);
        i += n + 1;
    }
    mem_free(heap_allocator(), buf, (size_t)cap, 1);
    return out;
}

/* Go starts looking for the '=' at 1, because a name can begin with one: the
 * per-drive working directories are kept as "=C:=C:\dir". */
void syscall_clearenv(void) {
    int64_t size = 0;
    int64_t cap = 0;
    Byte *buf = env_block(&size, &cap);
    if (buf == NULL)
        return;
    for (int64_t i = 0; i < size;) {
        Int n = (Int)strlen((const char *)buf + i);
        const void *eq = n > 1 ? memchr(buf + i + 1, '=', (size_t)n - 1) : NULL;
        if (eq != NULL)
            syscall_unsetenv(
                str_from_bytes(buf + i, (Int)((const Byte *)eq - (buf + i))));
        i += n + 1;
    }
    mem_free(heap_allocator(), buf, (size_t)cap, 1);
}

#endif

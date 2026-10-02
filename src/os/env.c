/* os.Getenv, os.Setenv and the rest of the environment, and os.Expand.
 *
 * Derived from Go's src/os/env.go.
 * Go source: go1.27.1.
 *
 * The rest is syscall/env_unix.go and syscall/env_windows.go, from the same
 * release.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/os.h"

#include "burrow/mem/heap.h"
#include "burrow/sync.h"

#include "internal.h"

#include <string.h>

#define OS_LIT(s) str_from_bytes((const Byte *)(s), (Int)(sizeof(s) - 1))

static Str os_env_copy(Alloc *a, const Byte *p, Int n) {
    if (n == 0)
        return (Str){(const Byte *)"", 0};
    Byte *b = (Byte *)mem_alloc_nozero(a, (size_t)n, 1);
    if (b == NULL)
        return (Str){NULL, 0};
    memcpy(b, p, (size_t)n);
    return str_from_bytes(b, n);
}

/* s as a NUL terminated string on the heap, for the PAL. */
static char *os_env_cstr(Str s) {
    char *c = (char *)mem_alloc_nozero(heap_allocator(), (size_t)s.len + 1, 1);
    if (c == NULL)
        return NULL;
    if (s.len > 0)
        memcpy(c, s.p, (size_t)s.len);
    c[s.len] = 0;
    return c;
}

static void os_env_cstr_free(char *c) {
    if (c != NULL)
        mem_free(heap_allocator(), c, strlen(c) + 1, 1);
}

static bool os_env_has_nul(Str s) {
    return s.len > 0 && memchr(s.p, 0, (size_t)s.len) != NULL;
}

#if !defined(BURROW_OS_WINDOWS)

/* syscall's envs: every "key=value" in the order the process started with,
 * then the new ones on the end. An empty entry is one that was unset, or a
 * later duplicate of a key, which copyenv clears so that unsetting the first
 * cannot uncover the second. Go keeps a map from key to index beside it, and
 * an environment is small enough that a walk does the same job. */
static SyncOnce os_env_once;
static SyncRWMutex os_env_mu;
static Str *os_envs;
static Int os_envs_len;
static Int os_envs_cap;

/* The key of an entry, or a negative length when it has no '='. */
static Int os_env_key_len(Str s) {
    const void *eq = s.len > 0 ? memchr(s.p, '=', (size_t)s.len) : NULL;
    return eq == NULL ? -1 : (Int)((const Byte *)eq - s.p);
}

static Int os_env_find(Str key) {
    for (Int i = 0; i < os_envs_len; i++) {
        Str s = os_envs[i];
        Int k = os_env_key_len(s);
        if (k == key.len && memcmp(s.p, key.p, (size_t)k) == 0)
            return i;
    }
    return -1;
}

static void os_env_free_entry(Int i) {
    if (os_envs[i].len > 0)
        mem_free(heap_allocator(), (void *)(uintptr_t)os_envs[i].p,
                 (size_t)os_envs[i].len, 1);
    os_envs[i] = (Str){NULL, 0};
}

static bool os_env_grow(void) {
    if (os_envs_len < os_envs_cap)
        return true;
    Int ncap = os_envs_cap < 16 ? 16 : os_envs_cap * 2;
    Str *n = (Str *)mem_alloc_nozero(heap_allocator(), (size_t)ncap * sizeof(Str),
                                     _Alignof(Str));
    if (n == NULL)
        return false;
    if (os_envs_len > 0)
        memcpy(n, os_envs, (size_t)os_envs_len * sizeof(Str));
    if (os_envs != NULL)
        mem_free(heap_allocator(), os_envs, (size_t)os_envs_cap * sizeof(Str),
                 _Alignof(Str));
    os_envs = n;
    os_envs_cap = ncap;
    return true;
}

/* copyenv. An entry that cannot be copied is left out, which is the closest
 * this can get to Go, where the copy cannot fail. */
static void os_env_init(void *env) {
    (void)env;
    for (const char *const *e = pal_environ(); e != NULL && *e != NULL; e++) {
        if (!os_env_grow())
            break;
        Str s = os_env_copy(heap_allocator(), (const Byte *)*e, (Int)strlen(*e));
        if (s.p == NULL)
            break;
        Int k = os_env_key_len(s);
        if (k >= 0 && os_env_find(str_from_bytes(s.p, k)) >= 0) {
            if (s.len > 0)
                mem_free(heap_allocator(), (void *)(uintptr_t)s.p, (size_t)s.len, 1);
            s = (Str){NULL, 0};
        }
        os_envs[os_envs_len++] = s;
    }
}

static void os_env_start(void) {
    sync_once_do(&os_env_once, BURROW_FN(Func, os_env_init, NULL));
}

Str os_lookup_env(Alloc *a, Str key, bool *found) {
    BURROW_OUT(found, false);
    os_env_start();
    if (key.len == 0)
        return (Str){NULL, 0};
    sync_rw_mutex_r_lock(&os_env_mu);
    Int i = os_env_find(key);
    Str v = {NULL, 0};
    if (i >= 0) {
        Str s = os_envs[i];
        v = os_env_copy(a, s.p + key.len + 1, s.len - key.len - 1);
        BURROW_OUT(found, true);
    }
    sync_rw_mutex_r_unlock(&os_env_mu);
    return v;
}

Error os_setenv(Str key, Str value) {
    os_env_start();
    bool bad = key.len == 0 || os_env_has_nul(key) || os_env_has_nul(value) ||
               memchr(key.p, '=', (size_t)key.len) != NULL;
    if (bad)
        return os_new_syscall_error(error_allocator(), OS_LIT("setenv"),
                                    burrow__os_errno_value(SYSCALL_EINVAL));
    Int n = key.len + 1 + value.len;
    Byte *kv = (Byte *)mem_alloc_nozero(heap_allocator(), (size_t)n, 1);
    if (kv == NULL)
        return burrow_err_out_of_memory;
    memcpy(kv, key.p, (size_t)key.len);
    kv[key.len] = '=';
    if (value.len > 0)
        memcpy(kv + key.len + 1, value.p, (size_t)value.len);

    sync_rw_mutex_lock(&os_env_mu);
    Int i = os_env_find(key);
    if (i < 0) {
        if (!os_env_grow()) {
            sync_rw_mutex_unlock(&os_env_mu);
            mem_free(heap_allocator(), kv, (size_t)n, 1);
            return burrow_err_out_of_memory;
        }
        i = os_envs_len++;
    } else {
        os_env_free_entry(i);
    }
    os_envs[i] = str_from_bytes(kv, n);
    /* runtimeSetenv. Go does this when cgo is in, so that C code sees the
     * change, and a C program always has C code in it. */
    char *ck = os_env_cstr(key);
    char *cv = os_env_cstr(value);
    if (ck != NULL && cv != NULL)
        pal_setenv(ck, cv, NULL);
    sync_rw_mutex_unlock(&os_env_mu);
    os_env_cstr_free(ck);
    os_env_cstr_free(cv);
    return BURROW_NO_ERROR;
}

Error os_unsetenv(Str key) {
    os_env_start();
    sync_rw_mutex_lock(&os_env_mu);
    Int i = os_env_find(key);
    if (i >= 0)
        os_env_free_entry(i);
    char *ck = os_env_has_nul(key) ? NULL : os_env_cstr(key);
    if (ck != NULL)
        pal_setenv(ck, NULL, NULL);
    sync_rw_mutex_unlock(&os_env_mu);
    os_env_cstr_free(ck);
    return BURROW_NO_ERROR;
}

void os_clearenv(void) {
    os_env_start();
    sync_rw_mutex_lock(&os_env_mu);
    for (Int i = 0; i < os_envs_len; i++) {
        Int k = os_env_key_len(os_envs[i]);
        if (k >= 0) {
            char *ck = os_env_cstr(str_from_bytes(os_envs[i].p, k));
            if (ck != NULL)
                pal_setenv(ck, NULL, NULL);
            os_env_cstr_free(ck);
        }
        os_env_free_entry(i);
    }
    os_envs_len = 0;
    sync_rw_mutex_unlock(&os_env_mu);
}

Slice os_environ(Alloc *a) {
    os_env_start();
    Slice out = slice_nil(TYPE_STRING);
    sync_rw_mutex_r_lock(&os_env_mu);
    for (Int i = 0; i < os_envs_len; i++) {
        if (os_envs[i].len == 0)
            continue;
        Str s = os_env_copy(a, os_envs[i].p, os_envs[i].len);
        if (s.p == NULL)
            break;
        out = slice_append(a, out, &s, 1);
    }
    sync_rw_mutex_r_unlock(&os_env_mu);
    return out;
}

#else

/* Windows keeps the environment itself, and Go asks it every time. */

Str os_lookup_env(Alloc *a, Str key, bool *found) {
    BURROW_OUT(found, false);
    if (os_env_has_nul(key))
        return (Str){NULL, 0};
    char *ck = os_env_cstr(key);
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
    os_env_cstr_free(ck);
    return v;
}

static Error os_env_set(Str key, const Str *value, bool wrap) {
    Error e = BURROW_NO_ERROR;
    if (os_env_has_nul(key) || (value != NULL && os_env_has_nul(*value)))
        e = burrow__os_errno_value(SYSCALL_EINVAL);
    if (BURROW_FAILED(e))
        return wrap ? os_new_syscall_error(error_allocator(), OS_LIT("setenv"), e) : e;
    char *ck = os_env_cstr(key);
    char *cv = value == NULL ? NULL : os_env_cstr(*value);
    PalErrno pe = PAL_OK;
    if (ck == NULL || (value != NULL && cv == NULL))
        e = burrow_err_out_of_memory;
    else if (!pal_setenv(ck, cv, &pe))
        e = burrow__os_errno(pe);
    os_env_cstr_free(ck);
    os_env_cstr_free(cv);
    if (wrap && BURROW_FAILED(e) && !errors_is(e, burrow_err_out_of_memory))
        return os_new_syscall_error(error_allocator(), OS_LIT("setenv"), e);
    return e;
}

Error os_setenv(Str key, Str value) {
    return os_env_set(key, &value, true);
}

Error os_unsetenv(Str key) {
    return os_env_set(key, NULL, false);
}

/* The whole block, NUL after each entry, on the heap. */
static Byte *os_env_block(int64_t *size, int64_t *cap_out) {
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

Slice os_environ(Alloc *a) {
    Slice out = slice_nil(TYPE_STRING);
    int64_t size = 0;
    int64_t cap = 0;
    Byte *buf = os_env_block(&size, &cap);
    if (buf == NULL)
        return out;
    for (int64_t i = 0; i < size;) {
        Int n = (Int)strlen((const char *)buf + i);
        Str s = os_env_copy(a, buf + i, n);
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
void os_clearenv(void) {
    int64_t size = 0;
    int64_t cap = 0;
    Byte *buf = os_env_block(&size, &cap);
    if (buf == NULL)
        return;
    for (int64_t i = 0; i < size;) {
        Int n = (Int)strlen((const char *)buf + i);
        const void *eq = n > 1 ? memchr(buf + i + 1, '=', (size_t)n - 1) : NULL;
        if (eq != NULL)
            os_unsetenv(str_from_bytes(buf + i, (Int)((const Byte *)eq - (buf + i))));
        i += n + 1;
    }
    mem_free(heap_allocator(), buf, (size_t)cap, 1);
}

#endif

Str os_getenv(Alloc *a, Str key) {
    return os_lookup_env(a, key, NULL);
}

/* ------------------------------------------------------------------ Expand */

static bool os_is_shell_special_var(Byte c) {
    switch (c) {
    case '*':
    case '#':
    case '$':
    case '@':
    case '!':
    case '?':
    case '-':
        return true;
    default:
        return c >= '0' && c <= '9';
    }
}

static bool os_is_alpha_num(Byte c) {
    return c == '_' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z');
}

/* getShellName: the name after a '$' at the front of s, and how many bytes of
 * s it took. An empty name that took bytes is bad syntax, which Expand eats. */
static Str os_get_shell_name(Str s, Int *w) {
    if (s.p[0] == '{') {
        if (s.len > 2 && os_is_shell_special_var(s.p[1]) && s.p[2] == '}') {
            *w = 3;
            return str_from_bytes(s.p + 1, 1);
        }
        for (Int i = 1; i < s.len; i++) {
            if (s.p[i] == '}') {
                if (i == 1) {
                    *w = 2;
                    return (Str){NULL, 0};
                }
                *w = i + 1;
                return str_from_bytes(s.p + 1, i - 1);
            }
        }
        *w = 1;
        return (Str){NULL, 0};
    }
    if (os_is_shell_special_var(s.p[0])) {
        *w = 1;
        return str_from_bytes(s.p, 1);
    }
    Int i = 0;
    while (i < s.len && os_is_alpha_num(s.p[i]))
        i++;
    *w = i;
    return str_from_bytes(s.p, i);
}

Str os_expand(Alloc *a, Str s, StrFunc mapping) {
    Slice buf = slice_nil(TYPE_BYTE);
    bool started = false;
    Int i = 0;
    for (Int j = 0; j < s.len; j++) {
        if (s.p[j] == '$' && j + 1 < s.len) {
            if (!started) {
                buf = slice_make(a, TYPE_BYTE, 0, 2 * s.len);
                started = true;
            }
            buf = slice_append(a, buf, s.p + i, j - i);
            Int w = 0;
            Str name =
                os_get_shell_name(str_from_bytes(s.p + j + 1, s.len - j - 1), &w);
            if (name.len == 0 && w > 0) {
                /* Bad syntax, eaten. */
            } else if (name.len == 0) {
                buf = slice_append(a, buf, s.p + j, 1);
            } else {
                Str v = mapping.f(mapping.env, name);
                buf = slice_append(a, buf, v.p, v.len);
            }
            j += w;
            i = j + 1;
        }
    }
    if (!started)
        return os_env_copy(a, s.p, s.len);
    buf = slice_append(a, buf, s.p + i, s.len - i);
    return str_from_bytes((const Byte *)buf.p, buf.len);
}

/* The value goes in a too, beside the result, since the mapping has nowhere
 * else to put it. */
static Str os_expand_getenv(void *env, Str name) {
    return os_getenv((Alloc *)env, name);
}

Str os_expand_env(Alloc *a, Str s) {
    return os_expand(a, s, BURROW_FN(StrFunc, os_expand_getenv, a));
}

/* os.Getenv, os.Setenv and the rest of the environment, and os.Expand.
 *
 * Derived from Go's src/os/env.go.
 * Go source: go1.27.1.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/os.h"

#include "internal.h"

#include <string.h>

#define OS_LIT(s) str_from_bytes((const Byte *)(s), (Int)(sizeof(s) - 1))

static Str os_env_copy(Alloc *a, const Byte *p, Int n) {
    /* n is never negative, but gcc can't see that through os_expand and warns
     * about the memcpy below (-Wstringop-overflow), so the test says <=. */
    if (n <= 0)
        return (Str){(const Byte *)"", 0};
    Byte *b = (Byte *)mem_alloc_nozero(a, (size_t)n, 1);
    if (b == NULL)
        return (Str){NULL, 0};
    memcpy(b, p, (size_t)n);
    return str_from_bytes(b, n);
}

/* The environment is syscall's, and os's functions are Go's thin wrappers
 * around it. */

Str os_lookup_env(Alloc *a, Str key, bool *found) {
    return syscall_getenv(a, key, found);
}

Error os_setenv(Str key, Str value) {
    Error err = syscall_setenv(key, value);
    if (BURROW_FAILED(err) && !errors_is(err, burrow_err_out_of_memory))
        return os_new_syscall_error(error_allocator(), OS_LIT("setenv"), err);
    return err;
}

Error os_unsetenv(Str key) {
    return syscall_unsetenv(key);
}

void os_clearenv(void) {
    syscall_clearenv();
}

Slice os_environ(Alloc *a) {
    return syscall_environ(a);
}

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
    /* Expand never passes an empty s, but the compiler can't see that. */
    if (s.len == 0) {
        *w = 0;
        return s;
    }
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

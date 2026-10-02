/* os.TempDir, os.CreateTemp and os.MkdirTemp.
 *
 * Derived from Go's src/os/tempfile.go.
 * Go source: go1.27.1.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/os.h"

#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/runtime.h"

#include "internal.h"

#include <string.h>

#define OS_LIT(s) str_from_bytes((const Byte *)(s), (Int)(sizeof(s) - 1))

BURROW_SENTINEL_ERROR(burrow__os_err_pattern_has_separator,
                      "pattern contains path separator");

Str os_temp_dir(Alloc *a) {
    char small[512];
    char *buf = small;
    int64_t cap = (int64_t)sizeof small;
    Str dir = {NULL, 0};
    for (;;) {
        PalErrno pe = PAL_OK;
        int64_t n = pal_temp_dir(buf, cap, &pe);
        if (n >= 0) {
            dir = str_from_bytes((const Byte *)buf, (Int)n);
            break;
        }
        if (buf != small)
            mem_free(heap_allocator(), buf, (size_t)cap, 1);
        buf = small;
        if (pe != PAL_ERANGE || cap >= (1 << 16))
            break;
        cap *= 2;
        char *nbuf = (char *)mem_alloc_nozero(heap_allocator(), (size_t)cap, 1);
        if (nbuf == NULL)
            return dir;
        buf = nbuf;
    }
    /* The PAL could not say, which Go's TempDir never admits to. */
    if (dir.p == NULL) {
#if defined(BURROW_OS_WINDOWS)
        dir = OS_LIT("C:\\Windows\\Temp");
#else
        dir = OS_LIT("/tmp");
#endif
    }
    Str out = burrow__os_cat3(a, dir, (Str){NULL, 0}, (Str){NULL, 0});
    if (buf != small)
        mem_free(heap_allocator(), buf, (size_t)cap, 1);
    return out;
}

/* nextRandom: a random uint32 in decimal, into buf, which holds ten digits. */
static Str os_next_random(Byte buf[10]) {
    uint32_t v = (uint32_t)runtime_rand64();
    int i = 10;
    do {
        buf[--i] = (Byte)('0' + v % 10);
        v /= 10;
    } while (v != 0);
    return str_from_bytes(buf + i, 10 - i);
}

/* prefixAndSuffix: pattern split at its last "*". */
static bool os_prefix_and_suffix(Str pattern, Str *prefix, Str *suffix) {
    for (Int i = 0; i < pattern.len; i++)
        if (os_is_path_separator(pattern.p[i]))
            return false;
    *prefix = pattern;
    *suffix = (Str){NULL, 0};
    for (Int i = pattern.len - 1; i >= 0; i--) {
        if (pattern.p[i] == '*') {
            *prefix = str_from_bytes(pattern.p, i);
            *suffix = str_from_bytes(pattern.p + i + 1, pattern.len - i - 1);
            break;
        }
    }
    return true;
}

/* joinPath: dir and name with one separator between. */
static Str os_join_path(Alloc *a, Str dir, Str name) {
    if (dir.len > 0 && os_is_path_separator(dir.p[dir.len - 1]))
        return burrow__os_cat3(a, dir, name, (Str){NULL, 0});
    Byte sep = (Byte)OS_PATH_SEPARATOR;
    return burrow__os_cat3(a, dir, str_from_bytes(&sep, 1), name);
}

/* What both functions start with: dir defaulted, the pattern split and the
 * prefix joined to dir, all in the scratch arena t. False with *err set when
 * the pattern will not do. */
static bool os_temp_prefix(Alloc *t, Str *dir, Str pattern, Str op, Str *prefix,
                           Str *suffix, Error *err) {
    if (dir->len == 0)
        *dir = os_temp_dir(t);
    Str pre;
    if (!os_prefix_and_suffix(pattern, &pre, suffix)) {
        BURROW_OUT(err, fs_path_error_new(error_allocator(), op, pattern,
                                          burrow__os_err_pattern_has_separator));
        return false;
    }
    *prefix = os_join_path(t, *dir, pre);
    if (prefix->p == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return false;
    }
    return true;
}

/* The error after ten thousand names that were all taken. */
static Error os_temp_exhausted(Alloc *t, Str op, Str prefix, Str suffix) {
    Str shown = burrow__os_cat3(t, prefix, OS_LIT("*"), suffix);
    return fs_path_error_new(error_allocator(), op, shown, fs_err_exist);
}

OsFile *os_create_temp(Alloc *a, Str dir, Str pattern, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    Str op = OS_LIT("createtemp");
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *t = arena_allocator(&ar);
    Str prefix, suffix;
    OsFile *f = NULL;
    if (!os_temp_prefix(t, &dir, pattern, op, &prefix, &suffix, err)) {
        arena_free(&ar);
        return NULL;
    }
    for (int try_ = 0;;) {
        Byte digits[10];
        Str name = burrow__os_cat3(t, prefix, os_next_random(digits), suffix);
        if (name.p == NULL) {
            BURROW_OUT(err, burrow_err_out_of_memory);
            break;
        }
        Error e = BURROW_NO_ERROR;
        f = os_open_file(a, name, OS_O_RDWR | OS_O_CREATE | OS_O_EXCL, 0600, &e);
        if (os_is_exist(e)) {
            if (++try_ < 10000)
                continue;
            BURROW_OUT(err, os_temp_exhausted(t, op, prefix, suffix));
            break;
        }
        BURROW_OUT(err, e);
        break;
    }
    arena_free(&ar);
    return f;
}

Str os_mkdir_temp(Alloc *a, Str dir, Str pattern, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    Str op = OS_LIT("mkdirtemp");
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *t = arena_allocator(&ar);
    Str prefix, suffix;
    Str out = {NULL, 0};
    if (!os_temp_prefix(t, &dir, pattern, op, &prefix, &suffix, err)) {
        arena_free(&ar);
        return out;
    }
    for (int try_ = 0;;) {
        Byte digits[10];
        Str name = burrow__os_cat3(t, prefix, os_next_random(digits), suffix);
        if (name.p == NULL) {
            BURROW_OUT(err, burrow_err_out_of_memory);
            break;
        }
        Error e = os_mkdir(name, 0700);
        if (BURROW_OK(e)) {
            out = burrow__os_cat3(a, name, (Str){NULL, 0}, (Str){NULL, 0});
            if (out.p == NULL) {
                os_remove(name);
                BURROW_OUT(err, burrow_err_out_of_memory);
            }
            break;
        }
        if (os_is_exist(e)) {
            if (++try_ < 10000)
                continue;
            BURROW_OUT(err, os_temp_exhausted(t, op, prefix, suffix));
            break;
        }
        /* When dir itself is missing, the error is the stat of dir. */
        if (os_is_not_exist(e)) {
            Error se = BURROW_NO_ERROR;
            os_stat(t, dir, &se);
            if (os_is_not_exist(se)) {
                BURROW_OUT(err, se);
                break;
            }
        }
        BURROW_OUT(err, e);
        break;
    }
    arena_free(&ar);
    return out;
}

/* Dynamic loading on everything but Windows: realpath, dlopen, dlsym and
 * dlclose, for the plugin package.
 *
 * glibc only has the dl calls in libc from 2.34, and in libdl before, so on
 * Linux they are weak references and this layer links nothing beyond libc, the
 * same as resolve_posix.c does for res_search. Where they did not get linked
 * in, the calls fail with PAL_ENOSYS. wasip1, js and Cosmopolitan have no
 * dlopen of their own, and fail the same way.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#if !defined(_WIN32)
#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE 1
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#endif
#endif

#include "burrow/platform.h"

#if !defined(BURROW_OS_WINDOWS)

#include "burrow/pal.h"

#include "internal.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(BURROW_OS_WASI) || defined(BURROW_OS_JS) || defined(BURROW_OS_COSMO)

bool pal_dl_realpath(const char *path, char *out, size_t cap, PalErrno *err) {
    (void)path;
    if (out != NULL && cap > 0)
        out[0] = '\0';
    BURROW_OUT(err, PAL_ENOSYS);
    return false;
}

int64_t pal_dl_open(const char *path, char *msg, size_t cap) {
    (void)path;
    if (msg != NULL && cap > 0) {
        const char *s = pal_errno_string(PAL_ENOSYS);
        size_t n = strlen(s);
        if (n >= cap)
            n = cap - 1;
        memcpy(msg, s, n);
        msg[n] = '\0';
    }
    return 0;
}

void *pal_dl_sym(int64_t handle, const char *name, PalErrno *err) {
    (void)handle;
    (void)name;
    BURROW_OUT(err, PAL_ENOSYS);
    return NULL;
}

bool pal_dl_close(int64_t handle, PalErrno *err) {
    (void)handle;
    BURROW_OUT(err, PAL_ENOSYS);
    return false;
}

#else

#include <dlfcn.h>
#include <limits.h>
#include <stdlib.h>

#if defined(BURROW_OS_LINUX)
#pragma weak dlopen
#pragma weak dlsym
#pragma weak dlclose
#pragma weak dlerror
#define PDL_LINKED                                                                     \
    (dlopen != NULL && dlsym != NULL && dlclose != NULL && dlerror != NULL)
#else
#define PDL_LINKED 1
#endif

/* s into msg, cut short to fit, with a NUL on the end. */
static void pdl_message(char *msg, size_t cap, const char *s) {
    if (msg == NULL || cap == 0)
        return;
    size_t n = s == NULL ? 0 : strlen(s);
    if (n >= cap)
        n = cap - 1;
    if (n > 0)
        memcpy(msg, s, n);
    msg[n] = '\0';
}

bool pal_dl_realpath(const char *path, char *out, size_t cap, PalErrno *err) {
    if (out != NULL && cap > 0)
        out[0] = '\0';
    if (path == NULL || out == NULL) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }
    /* realpath writes up to PATH_MAX bytes, whatever cap is. */
    char r[PATH_MAX];
    if (realpath(path, r) == NULL) {
        BURROW_OUT(err, burrow__pal_errno(errno));
        return false;
    }
    size_t n = strlen(r);
    if (n >= cap) {
        BURROW_OUT(err, PAL_ERANGE);
        return false;
    }
    memcpy(out, r, n + 1);
    BURROW_OUT(err, PAL_OK);
    return true;
}

int64_t pal_dl_open(const char *path, char *msg, size_t cap) {
    pdl_message(msg, cap, "");
    if (!PDL_LINKED) {
        pdl_message(msg, cap, pal_errno_string(PAL_ENOSYS));
        return 0;
    }
    void *h = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    if (h == NULL)
        pdl_message(msg, cap, dlerror());
    return (int64_t)(intptr_t)h;
}

void *pal_dl_sym(int64_t handle, const char *name, PalErrno *err) {
    if (!PDL_LINKED) {
        BURROW_OUT(err, PAL_ENOSYS);
        return NULL;
    }
    /* Cleared first, so that a NULL with no error after it is a symbol whose
     * address is NULL and not one that is missing. */
    (void)dlerror();
    void *r = dlsym((void *)(intptr_t)handle, name);
    if (r == NULL && dlerror() != NULL) {
        BURROW_OUT(err, PAL_ENOENT);
        return NULL;
    }
    BURROW_OUT(err, PAL_OK);
    return r;
}

bool pal_dl_close(int64_t handle, PalErrno *err) {
    if (!PDL_LINKED) {
        BURROW_OUT(err, PAL_ENOSYS);
        return false;
    }
    if (dlclose((void *)(intptr_t)handle) != 0) {
        (void)dlerror();
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }
    BURROW_OUT(err, PAL_OK);
    return true;
}

#endif /* dlopen */

#endif /* !BURROW_OS_WINDOWS */

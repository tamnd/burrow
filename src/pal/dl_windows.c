/* Dynamic loading on Windows: GetFullPathNameW, LoadLibraryExW, GetProcAddress
 * and FreeLibrary, for the plugin package. Go has no plugins on Windows, so the
 * choices here are burrow's: the path is made absolute but links are left as
 * they are, and a failure to load reports the system's message for it.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/platform.h"

#if defined(BURROW_OS_WINDOWS)

#include "burrow/pal.h"

#include "internal.h"

#include <string.h>
#include <windows.h>

/* s into msg, cut short to fit, with a NUL on the end. */
static void wdl_message(char *msg, size_t cap, const char *s, size_t n) {
    if (msg == NULL || cap == 0)
        return;
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
    wchar_t w[PAL_WPATH_MAX];
    if (!burrow__pal_widen(path, w, PAL_WPATH_MAX, err))
        return false;
    wchar_t full[PAL_WPATH_MAX];
    DWORD n = GetFullPathNameW(w, PAL_WPATH_MAX, full, NULL);
    if (n == 0) {
        BURROW_OUT(err, burrow__pal_errno_win(GetLastError()));
        return false;
    }
    if (n >= PAL_WPATH_MAX) {
        BURROW_OUT(err, PAL_ERANGE);
        return false;
    }
    if (GetFileAttributesW(full) == INVALID_FILE_ATTRIBUTES) {
        BURROW_OUT(err, burrow__pal_errno_win(GetLastError()));
        return false;
    }
    if (cap == 0) {
        BURROW_OUT(err, PAL_ERANGE);
        return false;
    }
    int64_t len = burrow__pal_narrow(full, n, out, cap - 1);
    if (len < 0) {
        out[0] = '\0';
        BURROW_OUT(err, PAL_ERANGE);
        return false;
    }
    out[len] = '\0';
    BURROW_OUT(err, PAL_OK);
    return true;
}

int64_t pal_dl_open(const char *path, char *msg, size_t cap) {
    wdl_message(msg, cap, "", 0);
    wchar_t w[PAL_WPATH_MAX];
    PalErrno e = PAL_OK;
    if (!burrow__pal_widen(path, w, PAL_WPATH_MAX, &e)) {
        const char *s = pal_errno_string(e);
        wdl_message(msg, cap, s, strlen(s));
        return 0;
    }
    HMODULE h = LoadLibraryExW(w, NULL, 0);
    if (h == NULL) {
        DWORD code = GetLastError();
        char buf[600];
        int64_t n = pal_errno_message((int64_t)code, buf, (int64_t)sizeof buf, NULL);
        if (n < 0) {
            const char *s = pal_errno_string(burrow__pal_errno_win(code));
            wdl_message(msg, cap, s, strlen(s));
        } else {
            wdl_message(msg, cap, buf, (size_t)n);
        }
        return 0;
    }
    return (int64_t)(intptr_t)h;
}

void *pal_dl_sym(int64_t handle, const char *name, PalErrno *err) {
    union {
        FARPROC f;
        void *object;
    } u;
    u.f = GetProcAddress((HMODULE)(intptr_t)handle, name);
    if (u.f == NULL) {
        DWORD code = GetLastError();
        BURROW_OUT(err, code == ERROR_PROC_NOT_FOUND ? PAL_ENOENT
                                                     : burrow__pal_errno_win(code));
        return NULL;
    }
    BURROW_OUT(err, PAL_OK);
    return u.object;
}

bool pal_dl_close(int64_t handle, PalErrno *err) {
    if (!FreeLibrary((HMODULE)(intptr_t)handle)) {
        BURROW_OUT(err, burrow__pal_errno_win(GetLastError()));
        return false;
    }
    BURROW_OUT(err, PAL_OK);
    return true;
}

#endif /* BURROW_OS_WINDOWS */

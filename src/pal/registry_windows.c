/* The content types in the Windows registry, for mime.
 *
 * Go's mime reads them through internal/syscall/windows/registry: every subkey
 * name of HKEY_CLASSES_ROOT, then the "Content Type" value of each one that
 * starts with a dot. This does the same walk. The functions are in advapi32,
 * and burrow links nothing but kernel32, so they come from LoadLibrary the
 * first time, which is what src/pal/random_windows.c says to do in this case.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/platform.h"

#if defined(BURROW_OS_WINDOWS)

#include "burrow/pal.h"

#include "internal.h"

#include <windows.h>

typedef LSTATUS(WINAPI *RegEnumKeyExWFn)(HKEY, DWORD, LPWSTR, LPDWORD, LPDWORD, LPWSTR,
                                         LPDWORD, PFILETIME);
typedef LSTATUS(WINAPI *RegOpenKeyExWFn)(HKEY, LPCWSTR, DWORD, REGSAM, PHKEY);
typedef LSTATUS(WINAPI *RegQueryValueExWFn)(HKEY, LPCWSTR, LPDWORD, LPDWORD, LPBYTE,
                                            LPDWORD);
typedef LSTATUS(WINAPI *RegCloseKeyFn)(HKEY);

/* A key name is at most 255 units. */
#define REG_NAME_MAX 256

/* Go's UTF16ToString turns an unpaired surrogate into U+FFFD, and
 * burrow__pal_narrow keeps it, so it is replaced here first. */
static void reg_fix_surrogates(wchar_t *w, size_t n) {
    for (size_t i = 0; i < n; i++) {
        uint32_t c = (uint32_t)w[i];
        if (c >= 0xd800 && c < 0xdc00 && i + 1 < n && (uint32_t)w[i + 1] >= 0xdc00 &&
            (uint32_t)w[i + 1] < 0xe000) {
            i++;
            continue;
        }
        if (c >= 0xd800 && c < 0xe000)
            w[i] = (wchar_t)0xfffd;
    }
}

/* n units of w as UTF-8 in a buffer from the process heap, or NULL. */
static char *reg_narrow(wchar_t *w, size_t n, int64_t *len) {
    reg_fix_surrogates(w, n);
    char *out = (char *)HeapAlloc(GetProcessHeap(), 0, n * 3 + 1);
    if (out == NULL)
        return NULL;
    *len = burrow__pal_narrow(w, n, out, n * 3);
    return out;
}

/* The "Content Type" string of key, in a buffer from the process heap, or NULL
 * when there is none or it is not a string. Like Go's GetStringValue, the
 * value stops at its first NUL. */
static wchar_t *reg_content_type(RegQueryValueExWFn query, HKEY key, size_t *units) {
    DWORD type = 0, size = 0;
    if (query(key, L"Content Type", NULL, &type, NULL, &size) != ERROR_SUCCESS)
        return NULL;
    if (type != REG_SZ && type != REG_EXPAND_SZ)
        return NULL;
    for (;;) {
        wchar_t *buf = (wchar_t *)HeapAlloc(GetProcessHeap(), 0, (size_t)size + 2);
        if (buf == NULL)
            return NULL;
        DWORD got = size;
        LSTATUS st = query(key, L"Content Type", NULL, &type, (LPBYTE)buf, &got);
        if (st == ERROR_MORE_DATA) {
            HeapFree(GetProcessHeap(), 0, buf);
            size = got;
            continue;
        }
        if (st != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
            HeapFree(GetProcessHeap(), 0, buf);
            return NULL;
        }
        size_t n = got / sizeof(wchar_t), i = 0;
        while (i < n && buf[i] != 0)
            i++;
        *units = i;
        return buf;
    }
}

bool pal_registry_content_types(PalContentTypeFn fn, void *env, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    HMODULE lib = LoadLibraryW(L"advapi32.dll");
    if (lib == NULL) {
        BURROW_OUT(err, burrow__pal_errno_win(GetLastError()));
        return false;
    }
    RegEnumKeyExWFn enum_key =
        (RegEnumKeyExWFn)(void (*)(void))GetProcAddress(lib, "RegEnumKeyExW");
    RegOpenKeyExWFn open_key =
        (RegOpenKeyExWFn)(void (*)(void))GetProcAddress(lib, "RegOpenKeyExW");
    RegQueryValueExWFn query =
        (RegQueryValueExWFn)(void (*)(void))GetProcAddress(lib, "RegQueryValueExW");
    RegCloseKeyFn close_key =
        (RegCloseKeyFn)(void (*)(void))GetProcAddress(lib, "RegCloseKey");
    if (enum_key == NULL || open_key == NULL || query == NULL || close_key == NULL) {
        BURROW_OUT(err, burrow__pal_errno_win(GetLastError()));
        FreeLibrary(lib);
        return false;
    }
    wchar_t name[REG_NAME_MAX];
    for (DWORD i = 0;; i++) {
        DWORD n = REG_NAME_MAX;
        LSTATUS st = enum_key(HKEY_CLASSES_ROOT, i, name, &n, NULL, NULL, NULL, NULL);
        if (st == ERROR_NO_MORE_ITEMS)
            break;
        if (st != ERROR_SUCCESS)
            continue;
        if (n < 2 || name[0] != L'.')
            continue;
        HKEY key;
        if (open_key(HKEY_CLASSES_ROOT, name, 0, KEY_READ, &key) != ERROR_SUCCESS)
            continue;
        size_t units = 0;
        wchar_t *value = reg_content_type(query, key, &units);
        close_key(key);
        if (value == NULL)
            continue;
        int64_t ext_len = 0, type_len = 0;
        char *ext = reg_narrow(name, n, &ext_len);
        char *type = reg_narrow(value, units, &type_len);
        if (ext != NULL && type != NULL && ext_len >= 0 && type_len >= 0)
            fn(env, ext, ext_len, type, type_len);
        if (ext != NULL)
            HeapFree(GetProcessHeap(), 0, ext);
        if (type != NULL)
            HeapFree(GetProcessHeap(), 0, type);
        HeapFree(GetProcessHeap(), 0, value);
    }
    FreeLibrary(lib);
    return true;
}

#endif /* BURROW_OS_WINDOWS */

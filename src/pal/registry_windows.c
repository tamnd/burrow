/* The content types and time zones in the Windows registry, for mime and
 * time.
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

#include <string.h>

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

/* The string value called name in key, in a buffer from the process heap, or
 * NULL when there is none or it is not a string. Like Go's GetStringValue, the
 * value stops at its first NUL. */
static wchar_t *reg_string(RegQueryValueExWFn query, HKEY key, const wchar_t *name,
                           size_t *units) {
    DWORD type = 0, size = 0;
    if (query(key, name, NULL, &type, NULL, &size) != ERROR_SUCCESS)
        return NULL;
    if (type != REG_SZ && type != REG_EXPAND_SZ)
        return NULL;
    for (;;) {
        wchar_t *buf = (wchar_t *)HeapAlloc(GetProcessHeap(), 0, (size_t)size + 2);
        if (buf == NULL)
            return NULL;
        DWORD got = size;
        LSTATUS st = query(key, name, NULL, &type, (LPBYTE)buf, &got);
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
        wchar_t *value = reg_string(query, key, L"Content Type", &units);
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

/* ---------------------------------------------------------------- time zones */

bool pal_tz_info(PalTzInfo *out, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    TIME_ZONE_INFORMATION i;
    if (GetTimeZoneInformation(&i) == TIME_ZONE_ID_INVALID) {
        BURROW_OUT(err, burrow__pal_errno_win(GetLastError()));
        return false;
    }
    out->bias = (int32_t)i.Bias;
    out->standard_bias = (int32_t)i.StandardBias;
    out->daylight_bias = (int32_t)i.DaylightBias;
    size_t n = 0;
    while (n < 32 && i.StandardName[n] != 0)
        n++;
    reg_fix_surrogates(i.StandardName, n);
    out->standard_name_len =
        burrow__pal_narrow(i.StandardName, n, out->standard_name, PAL_TZ_NAME_MAX);
    n = 0;
    while (n < 32 && i.DaylightName[n] != 0)
        n++;
    reg_fix_surrogates(i.DaylightName, n);
    out->daylight_name_len =
        burrow__pal_narrow(i.DaylightName, n, out->daylight_name, PAL_TZ_NAME_MAX);
    if (out->standard_name_len < 0)
        out->standard_name_len = 0;
    if (out->daylight_name_len < 0)
        out->daylight_name_len = 0;
    out->standard_date = (PalTzDate){i.StandardDate.wMonth,  i.StandardDate.wDayOfWeek,
                                     i.StandardDate.wDay,    i.StandardDate.wHour,
                                     i.StandardDate.wMinute, i.StandardDate.wSecond};
    out->daylight_date = (PalTzDate){i.DaylightDate.wMonth,  i.DaylightDate.wDayOfWeek,
                                     i.DaylightDate.wDay,    i.DaylightDate.wHour,
                                     i.DaylightDate.wMinute, i.DaylightDate.wSecond};
    return true;
}

typedef LSTATUS(WINAPI *RegLoadMUIStringWFn)(HKEY, LPCWSTR, LPWSTR, DWORD, LPDWORD,
                                             DWORD, LPCWSTR);

typedef struct RegApi {
    HMODULE lib;
    RegEnumKeyExWFn enum_key;
    RegOpenKeyExWFn open_key;
    RegQueryValueExWFn query;
    RegCloseKeyFn close_key;
    RegLoadMUIStringWFn load_mui;
} RegApi;

static bool reg_api_open(RegApi *r, PalErrno *err) {
    r->lib = LoadLibraryW(L"advapi32.dll");
    if (r->lib == NULL) {
        BURROW_OUT(err, burrow__pal_errno_win(GetLastError()));
        return false;
    }
    r->enum_key =
        (RegEnumKeyExWFn)(void (*)(void))GetProcAddress(r->lib, "RegEnumKeyExW");
    r->open_key =
        (RegOpenKeyExWFn)(void (*)(void))GetProcAddress(r->lib, "RegOpenKeyExW");
    r->query =
        (RegQueryValueExWFn)(void (*)(void))GetProcAddress(r->lib, "RegQueryValueExW");
    r->close_key = (RegCloseKeyFn)(void (*)(void))GetProcAddress(r->lib, "RegCloseKey");
    r->load_mui = (RegLoadMUIStringWFn)(void (*)(void))GetProcAddress(
        r->lib, "RegLoadMUIStringW");
    if (r->enum_key == NULL || r->open_key == NULL || r->query == NULL ||
        r->close_key == NULL) {
        BURROW_OUT(err, burrow__pal_errno_win(GetLastError()));
        FreeLibrary(r->lib);
        return false;
    }
    return true;
}

/* Go's GetMUIStringValue: the value through RegLoadMUIStringW, and when that
 * cannot find the file, again with the system directory to look in, since the
 * value is usually @tzres.dll,-320 with no path. */
static bool reg_mui(RegApi *r, HKEY key, const wchar_t *name, char *out, int64_t *len) {
    if (r->load_mui == NULL)
        return false;
    wchar_t buf[1024];
    DWORD got = 0;
    LSTATUS st = r->load_mui(key, name, buf, sizeof buf, &got, 0, NULL);
    if (st == ERROR_FILE_NOT_FOUND) {
        wchar_t dir[MAX_PATH + 2];
        UINT n = GetSystemDirectoryW(dir, MAX_PATH);
        if (n == 0 || n >= MAX_PATH)
            return false;
        dir[n] = L'\\';
        dir[n + 1] = 0;
        st = r->load_mui(key, name, buf, sizeof buf, &got, 0, dir);
    }
    if (st != ERROR_SUCCESS)
        return false;
    size_t n = 0;
    while (n < sizeof buf / sizeof buf[0] && buf[n] != 0)
        n++;
    reg_fix_surrogates(buf, n);
    *len = burrow__pal_narrow(buf, n, out, PAL_TZ_NAME_MAX);
    return *len >= 0;
}

static bool reg_sz(RegApi *r, HKEY key, const wchar_t *name, char *out, int64_t *len) {
    size_t units = 0;
    wchar_t *v = reg_string(r->query, key, name, &units);
    if (v == NULL)
        return false;
    reg_fix_surrogates(v, units);
    *len = burrow__pal_narrow(v, units, out, PAL_TZ_NAME_MAX);
    HeapFree(GetProcessHeap(), 0, v);
    return *len >= 0;
}

#define TZ_KEY L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Time Zones"

/* matchZoneKey's reading half: MUI_Std and MUI_Dlt first, Std and Dlt if
 * either of those fails. */
static bool reg_zone_names(RegApi *r, HKEY zones, const wchar_t *kname, char *std,
                           int64_t *std_len, char *dst, int64_t *dst_len) {
    HKEY k;
    if (r->open_key(zones, kname, 0, KEY_READ, &k) != ERROR_SUCCESS)
        return false;
    bool ok = reg_mui(r, k, L"MUI_Std", std, std_len) &&
              reg_mui(r, k, L"MUI_Dlt", dst, dst_len);
    if (!ok)
        ok = reg_sz(r, k, L"Std", std, std_len) && reg_sz(r, k, L"Dlt", dst, dst_len);
    r->close_key(k);
    return ok;
}

bool pal_tz_key_names(const char *key, int64_t key_len, char *std, int64_t *std_len,
                      char *dst, int64_t *dst_len, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    char name[PAL_TZ_NAME_MAX];
    wchar_t wname[REG_NAME_MAX];
    if (key_len < 0 || key_len >= PAL_TZ_NAME_MAX) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }
    memcpy(name, key, (size_t)key_len);
    name[key_len] = 0;
    if (!burrow__pal_widen(name, wname, REG_NAME_MAX, err))
        return false;
    RegApi r;
    if (!reg_api_open(&r, err))
        return false;
    HKEY zones;
    bool ok = false;
    if (r.open_key(HKEY_LOCAL_MACHINE, TZ_KEY, 0, KEY_READ, &zones) == ERROR_SUCCESS) {
        ok = reg_zone_names(&r, zones, wname, std, std_len, dst, dst_len);
        r.close_key(zones);
    }
    if (!ok)
        BURROW_OUT(err, PAL_ENOENT);
    FreeLibrary(r.lib);
    return ok;
}

static bool tz_same(const char *a, int64_t alen, const char *b, int64_t blen) {
    return alen == blen && memcmp(a, b, (size_t)alen) == 0;
}

bool pal_tz_english_name(const char *std, int64_t std_len, const char *dst,
                         int64_t dst_len, char *out, int64_t *out_len, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    RegApi r;
    if (!reg_api_open(&r, err))
        return false;
    HKEY zones;
    LSTATUS st = r.open_key(HKEY_LOCAL_MACHINE, TZ_KEY, 0,
                            KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE, &zones);
    if (st != ERROR_SUCCESS) {
        BURROW_OUT(err, burrow__pal_errno_win((unsigned long)st));
        FreeLibrary(r.lib);
        return false;
    }
    bool found = false;
    wchar_t name[REG_NAME_MAX];
    char *kstd = (char *)HeapAlloc(GetProcessHeap(), 0, 2 * PAL_TZ_NAME_MAX);
    if (kstd == NULL) {
        BURROW_OUT(err, PAL_ENOMEM);
        r.close_key(zones);
        FreeLibrary(r.lib);
        return false;
    }
    char *kdst = kstd + PAL_TZ_NAME_MAX;
    for (DWORD i = 0; !found; i++) {
        DWORD n = REG_NAME_MAX;
        st = r.enum_key(zones, i, name, &n, NULL, NULL, NULL, NULL);
        if (st == ERROR_NO_MORE_ITEMS)
            break;
        if (st != ERROR_SUCCESS)
            continue;
        int64_t sl = 0, dl = 0;
        if (!reg_zone_names(&r, zones, name, kstd, &sl, kdst, &dl))
            continue;
        if (!tz_same(kstd, sl, std, std_len))
            continue;
        if (!tz_same(kdst, dl, dst, dst_len) && !tz_same(dst, dst_len, std, std_len))
            continue;
        reg_fix_surrogates(name, n);
        *out_len = burrow__pal_narrow(name, n, out, PAL_TZ_NAME_MAX);
        found = *out_len >= 0;
    }
    HeapFree(GetProcessHeap(), 0, kstd);
    r.close_key(zones);
    FreeLibrary(r.lib);
    if (!found)
        BURROW_OUT(err, PAL_ENOENT);
    return found;
}

#endif /* BURROW_OS_WINDOWS */

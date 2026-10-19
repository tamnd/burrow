/* The modules loaded into the running process on Windows, from a toolhelp
 * snapshot.
 *
 * Derived from Go's src/runtime/pprof/proto_windows.go. Go source: go1.27.1.
 *
 * Go takes a TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32 snapshot of its own process,
 * trying again for as long as it fails with ERROR_BAD_LENGTH, and reports every
 * module in it with offset 0. The functions are all in kernel32. The build ID,
 * which Go makes from the file's modification time, is left to the caller.
 *
 * Copyright 2022 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/platform.h"

#if defined(BURROW_OS_WINDOWS)

#include "burrow/pal.h"

#include "internal.h"

#include <windows.h>

#include <tlhelp32.h>

#include <string.h>

/* Go's UTF16ToString turns an unpaired surrogate into U+FFFD, and
 * burrow__pal_narrow keeps it, so it is replaced here first. */
static void pmap_fix_surrogates(wchar_t *w, size_t n) {
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

static HANDLE pmap_snapshot(void) {
    for (;;) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                               GetCurrentProcessId());
        if (snap == INVALID_HANDLE_VALUE && GetLastError() == ERROR_BAD_LENGTH)
            continue;
        return snap;
    }
}

bool pal_text_mappings(PalMappingFn fn, void *env) {
    HANDLE snap = pmap_snapshot();
    if (snap == INVALID_HANDLE_VALUE)
        return false;
    MODULEENTRY32W module;
    memset(&module, 0, sizeof module);
    module.dwSize = sizeof module;
    if (!Module32FirstW(snap, &module)) {
        CloseHandle(snap);
        return false;
    }
    char exe[MAX_PATH * 3 + 1];
    do {
        size_t n = wcsnlen(module.szExePath, MAX_PATH);
        pmap_fix_surrogates(module.szExePath, n);
        int64_t len = burrow__pal_narrow(module.szExePath, n, exe, sizeof exe - 1);
        if (len < 0)
            len = 0;
        uint64_t base = (uint64_t)(uintptr_t)module.modBaseAddr;
        fn(env, base, base + (uint64_t)module.modBaseSize, 0, exe, len);
    } while (Module32NextW(snap, &module));
    CloseHandle(snap);
    return true;
}

#endif /* BURROW_OS_WINDOWS */

/* The command line on Windows, where a process is given one string and every
 * program splits it for itself. This splits it the way Go's os package does,
 * which is the rule from before 2008 that the Microsoft C runtime used, with
 * two double quotes inside quotes standing for one:
 * http://daviddeley.com/autohotkey/parameters/parameters.htm#WINARGV
 *
 * The line goes into buf as UTF-8 first and is split there, in place. Every
 * byte written comes from a byte already read, so the write position never
 * passes the read position.
 *
 * Derived from Go's src/os/exec_windows.go. Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/platform.h"

#if defined(BURROW_OS_WINDOWS)

#include "burrow/pal.h"

#include "internal.h"

#include <string.h>
#include <wchar.h>

#include <windows.h>

/* Go's readNextArg and commandLineToArgv, over buf[0, n). */
static int64_t args_split(char *buf, int64_t n) {
    int64_t r = 0;
    int64_t w = 0;
    while (r < n) {
        if (buf[r] == ' ' || buf[r] == '\t') {
            r++;
            continue;
        }
        bool inquote = false;
        int64_t nslash = 0;
        for (; r < n; r++) {
            char c = buf[r];
            if ((c == ' ' || c == '\t') && !inquote) {
                r++;
                break;
            }
            if (c == '"') {
                for (int64_t i = 0; i < nslash / 2; i++)
                    buf[w++] = '\\';
                if (nslash % 2 == 0) {
                    if (inquote && r + 1 < n && buf[r + 1] == '"') {
                        buf[w++] = c;
                        r++;
                    }
                    inquote = !inquote;
                } else {
                    buf[w++] = c;
                }
                nslash = 0;
                continue;
            }
            if (c == '\\') {
                nslash++;
                continue;
            }
            for (; nslash > 0; nslash--)
                buf[w++] = '\\';
            buf[w++] = c;
        }
        for (; nslash > 0; nslash--)
            buf[w++] = '\\';
        buf[w++] = 0;
    }
    return w;
}

int64_t pal_args(char *buf, int64_t cap, PalErrno *err) {
    const wchar_t *line = GetCommandLineW();
    size_t len = line != NULL ? wcslen(line) : 0;
    if (len == 0) {
        /* Go's answer for an empty command line is the executable's path as
         * the only argument. */
        wchar_t path[1024];
        DWORD pn = GetModuleFileNameW(NULL, path, 1024);
        if (pn == 0 || pn >= 1024)
            return 0;
        int64_t m = cap > 0 ? burrow__pal_narrow(path, pn, buf, (size_t)cap - 1) : -1;
        if (m < 0) {
            BURROW_OUT(err, PAL_ERANGE);
            return -1;
        }
        buf[m] = 0;
        return m + 1;
    }
    /* The split is done in a buffer of its own, since quotes make it shorter
     * than the line, and a buf that holds what the split answers is enough.
     * One byte more than the line is for the NUL after the last argument,
     * which is the only byte the split can add, and a UTF-16 unit is at most
     * three bytes of WTF-8. */
    size_t room = len * 3 + 1;
    char *tmp = HeapAlloc(GetProcessHeap(), 0, room);
    if (tmp == NULL) {
        BURROW_OUT(err, PAL_ENOMEM);
        return -1;
    }
    int64_t m = burrow__pal_narrow(line, len, tmp, room - 1);
    int64_t n = m < 0 ? -1 : args_split(tmp, m);
    if (n < 0 || n > cap) {
        HeapFree(GetProcessHeap(), 0, tmp);
        BURROW_OUT(err, PAL_ERANGE);
        return -1;
    }
    memcpy(buf, tmp, (size_t)n);
    HeapFree(GetProcessHeap(), 0, tmp);
    return n;
}

#endif /* BURROW_OS_WINDOWS */

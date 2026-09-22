/* The system generator on Windows: RtlGenRandom.
 *
 * It is declared as SystemFunction036 in ntsecapi.h and documented under the
 * other name, which is one of the odder corners of the Win32 API and is why the
 * prototype is written out here rather than included. Microsoft has called it
 * deprecated for twenty years and has never removed it, and Go's
 * crypto/rand used it on Windows for most of that time.
 *
 * BCryptGenRandom is the supported call and is deliberately not used. It lives
 * in bcrypt.dll, which would be a library to link, and burrow links nothing but
 * kernel32 today. That is worth keeping: a single file amalgamation that needs
 * no link line is one of the things this library is for. If Windows ever does
 * remove this, the answer is a runtime LoadLibrary of bcrypt, which keeps the
 * link line empty and is what PAL rule 1 would have us do anyway.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/platform.h"

#if defined(BURROW_OS_WINDOWS)

#include "burrow/pal.h"

#include "internal.h"

#include <windows.h>

/* The real name, with the signature ntsecapi.h gives it. BOOLEAN rather than
 * BOOL, and ULONG for the length, both of which matter on a 64 bit build. */
BOOLEAN NTAPI SystemFunction036(PVOID buf, ULONG len);

/* ULONG is 32 bits on every Windows target, so a request larger than that has
 * to be split. Nobody asks for two gigabytes of randomness in one call, and a
 * loop is cheaper than a comment explaining why the cast is safe. */
#define RTLGENRANDOM_MAX 0x40000000u

bool pal_random_bytes(void *buf, int64_t n, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);

    if (n < 0 || (n > 0 && buf == NULL)) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }
    if (n == 0)
        return true;

    unsigned char *p = (unsigned char *)buf;
    uint64_t want = (uint64_t)n;

    while (want > 0) {
        ULONG chunk = want < RTLGENRANDOM_MAX ? (ULONG)want : RTLGENRANDOM_MAX;

        /* It answers a boolean and sets no error code worth reading, so a
         * failure here is reported as PAL_EIO: the system was asked for
         * randomness and would not give it, and there is nothing more specific
         * to say. */
        if (!SystemFunction036(p, chunk)) {
            BURROW_OUT(err, PAL_EIO);
            return false;
        }

        p += chunk;
        want -= chunk;
    }

    return true;
}

#endif /* BURROW_OS_WINDOWS */

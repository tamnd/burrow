/* What the PAL backends share with each other and with nobody else.
 *
 * One thing lives here: turning the platform's idea of a failure into ours. It
 * is in a header rather than in each backend because every group needs it and
 * because a second copy of the mapping is a second place for a code to go
 * missing.
 *
 * Nothing outside src/pal/ may include this. There is no include guard against
 * that, because the check that matters is tools/check-pal.sh, which fails the
 * build if anything above this layer reaches for an operating system at all.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_PAL_INTERNAL_H
#define BURROW_PAL_INTERNAL_H

/* core.h is here for BURROW_OUT, which is how every out parameter in burrow is
 * written and is no less true down here. It brings Str and the rest with it,
 * and none of that reaches the boundary: what burrow/pal.h declares is C and
 * nothing else, and what a backend uses to implement it is its own business. */
#include "burrow/core.h"
#include "burrow/pal.h"
#include "burrow/platform.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(BURROW_OS_WINDOWS)

/* A Win32 error, which is what GetLastError answers, into one of ours.
 *
 * unsigned long rather than DWORD so that this header does not have to drag
 * windows.h in behind it. They are the same type and the compiler will say so
 * if a platform ever disagrees. */
PalErrno burrow__pal_errno_win(unsigned long native);

/* A Winsock error, which is what WSAGetLastError answers. It is a different
 * numbering from the one above, sharing only the range, so it is a different
 * function. */
PalErrno burrow__pal_errno_wsa(int native);

/* UTF-8 into NUL terminated UTF-16 in cap units, and n units of UTF-16 back
 * into UTF-8 without a NUL, answering the length or -1 when it does not fit.
 * Both are WTF-8, which is Go's rule. They live in file_windows.c. */
bool burrow__pal_widen(const char *s, wchar_t *out, size_t cap, PalErrno *err);
int64_t burrow__pal_narrow(const wchar_t *w, size_t n, char *out, size_t cap);

#else

/* A POSIX errno into one of ours. Zero in gives PAL_EOTHER rather than PAL_OK,
 * because a backend only calls this after something has already failed, and
 * answering success there would turn a failure into a silent zero. */
PalErrno burrow__pal_errno(int native);

/* A native signal number into one of ours, or the native number itself when
 * there is no PAL name for it, for the status pal_wait reports. */
int32_t burrow__pal_signal_from_native(int native);

/* The two halves of the read side of the fork lock in proc_posix.c, held
 * across any moment where a descriptor is open without close on exec. */
void burrow__pal_fork_rlock(void);
void burrow__pal_fork_runlock(void);

#endif

#ifdef __cplusplus
}
#endif

#endif /* BURROW_PAL_INTERNAL_H */

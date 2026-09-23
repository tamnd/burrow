/* Capturing standard output on Windows.
 *
 * The stdout stream writes through the C runtime's descriptor 1, and the
 * runtime keeps that descriptor's handle in a table of its own that it filled
 * in at startup. Moving the Win32 standard handle with SetStdHandle leaves the
 * table alone and so leaves printf writing where it always did. So this is
 * the one backend that uses the runtime's descriptor calls: _pipe, _dup and
 * _dup2, which update the table and the standard handle together.
 *
 * Both ends of the pipe are binary, so what an example prints arrives with
 * the line endings it wrote. Descriptor 1 goes back to its own mode when it is
 * put back.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/platform.h"

#if defined(BURROW_OS_WINDOWS)

#include "burrow/pal.h"

#include "internal.h"

#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <stdio.h>

static PalErrno capture_errno(int e) {
    switch (e) {
    case EMFILE:
        return PAL_EMFILE;
    case ENFILE:
        return PAL_ENFILE;
    case EBADF:
        return PAL_EBADF;
    case ENOMEM:
        return PAL_ENOMEM;
    case EINVAL:
        return PAL_EINVAL;
    default:
        return PAL_EOTHER;
    }
}

static void capture_reader(void *arg) {
    PalStdoutCapture *c = arg;
    char buf[4096];
    for (;;) {
        int n = _read((int)c->read, buf, (unsigned)sizeof buf);
        if (n <= 0)
            return;
        c->sink(c->env, buf, (int64_t)n);
    }
}

bool pal_stdout_capture_begin(PalStdoutCapture *c,
                              void (*sink)(void *env, const void *p, int64_t n),
                              void *env, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (c == NULL || sink == NULL) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }
    c->saved = PAL_INVALID_HANDLE;
    c->read = PAL_INVALID_HANDLE;
    c->thread = PAL_INVALID_HANDLE;
    c->sink = sink;
    c->env = env;

    int fds[2];
    if (_pipe(fds, 65536, _O_BINARY | _O_NOINHERIT) != 0) {
        BURROW_OUT(err, capture_errno(errno));
        return false;
    }

    (void)fflush(stdout);
    /* A process with no standard output, which a GUI program is, has nothing
     * to save, and gets descriptor 1 closed again at the end. */
    int saved = _dup(1);
    if (_dup2(fds[1], 1) != 0) {
        BURROW_OUT(err, capture_errno(errno));
        if (saved >= 0)
            (void)_close(saved);
        (void)_close(fds[0]);
        (void)_close(fds[1]);
        return false;
    }
    (void)_close(fds[1]);
    c->saved = saved;
    c->read = fds[0];

    c->thread = pal_thread_create(capture_reader, c, 0, err);
    if (c->thread == PAL_INVALID_HANDLE) {
        if (saved >= 0) {
            (void)_dup2(saved, 1);
            (void)_close(saved);
        } else {
            (void)_close(1);
        }
        (void)_close(fds[0]);
        c->saved = PAL_INVALID_HANDLE;
        c->read = PAL_INVALID_HANDLE;
        return false;
    }
    return true;
}

bool pal_stdout_capture_end(PalStdoutCapture *c, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (c == NULL || c->read == PAL_INVALID_HANDLE) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }
    (void)fflush(stdout);
    bool ok = true;
    if (c->saved >= 0) {
        if (_dup2((int)c->saved, 1) != 0) {
            BURROW_OUT(err, capture_errno(errno));
            ok = false;
            (void)_close(1);
        }
        (void)_close((int)c->saved);
    } else {
        (void)_close(1);
    }
    PalErrno jerr = PAL_OK;
    if (!pal_thread_join(c->thread, &jerr) && ok) {
        BURROW_OUT(err, jerr);
        ok = false;
    }
    (void)_close((int)c->read);
    c->saved = PAL_INVALID_HANDLE;
    c->read = PAL_INVALID_HANDLE;
    c->thread = PAL_INVALID_HANDLE;
    return ok;
}

#endif

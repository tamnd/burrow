/* Capturing standard output on wasip1.
 *
 * There are no pipes and no threads here, so this does what Go's
 * run_example_wasm.go does and writes to a temporary file. WASI has no dup or
 * dup2 either. What it has is fd_renumber, which moves one descriptor onto
 * another number and closes whatever was there. wasmtime only moves onto a
 * number that is open already, so the old descriptor 1 goes onto a spare
 * descriptor on the file, and the file is opened again to take its place. The
 * lowest free number is 1 by then, and that is the one wasmtime hands out; a
 * runtime that hands out another gets it moved.
 *
 * The file is unlinked as soon as it is open, and read back when the capture
 * ends, so sink gets everything in one go at the end rather than as it is
 * written.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/platform.h"

#if defined(BURROW_OS_WASI)

#include "burrow/pal.h"

#include "internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <wasi/api.h>

/* A new file in $TMPDIR, or /tmp as os.TempDir has it, with its name in path.
 * The descriptor is for writing, and is the spare the old descriptor 1 goes
 * to. */
static int capture_create(char *path, size_t cap, PalErrno *err) {
    const char *dir = getenv("TMPDIR");
    if (dir == NULL || dir[0] == '\0')
        dir = "/tmp";
    for (int tries = 0; tries < 100; tries++) {
        uint32_t n = 0;
        if (!pal_random_bytes(&n, (int64_t)sizeof n, err))
            return -1;
        int len = snprintf(path, cap, "%s/burrow-example-stdout-%u", dir, (unsigned)n);
        if (len < 0 || (size_t)len >= cap) {
            BURROW_OUT(err, PAL_ENAMETOOLONG);
            return -1;
        }
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd >= 0)
            return fd;
        if (errno != EEXIST) {
            BURROW_OUT(err, burrow__pal_errno(errno));
            return -1;
        }
    }
    BURROW_OUT(err, PAL_EEXIST);
    return -1;
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

    char path[4096];
    int saved = capture_create(path, sizeof path, err);
    if (saved < 0)
        return false;
    int r = open(path, O_RDONLY | O_CLOEXEC);
    if (r < 0) {
        BURROW_OUT(err, burrow__pal_errno(errno));
        (void)close(saved);
        (void)unlink(path);
        return false;
    }

    (void)fflush(stdout);
    __wasi_errno_t e = __wasi_fd_renumber(1, (__wasi_fd_t)saved);
    if (e == __WASI_ERRNO_BADF) {
        /* A process started with descriptor 1 closed has nothing to save, and
         * gets it closed again at the end. */
        (void)close(saved);
        saved = -1;
    } else if (e != 0) {
        BURROW_OUT(err, burrow__pal_errno((int)e));
        (void)close(saved);
        (void)close(r);
        (void)unlink(path);
        return false;
    }

    int w = open(path, O_WRONLY | O_CLOEXEC);
    int oerr = errno;
    (void)unlink(path);
    e = 0;
    if (w < 0)
        e = (__wasi_errno_t)oerr;
    else if (w != 1)
        e = __wasi_fd_renumber((__wasi_fd_t)w, 1);
    if (e != 0) {
        BURROW_OUT(err, burrow__pal_errno((int)e));
        if (w >= 0)
            (void)close(w);
        /* Which wasmtime refuses, with nothing at 1 to move onto, but there is
         * no other way back. */
        if (saved >= 0)
            (void)__wasi_fd_renumber((__wasi_fd_t)saved, 1);
        (void)close(r);
        return false;
    }
    c->saved = saved;
    c->read = r;
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
        __wasi_errno_t e = __wasi_fd_renumber((__wasi_fd_t)c->saved, 1);
        if (e != 0) {
            BURROW_OUT(err, burrow__pal_errno((int)e));
            ok = false;
            (void)close(1);
            (void)close((int)c->saved);
        }
    } else {
        (void)close(1);
    }

    char buf[4096];
    for (;;) {
        ssize_t n = read((int)c->read, buf, sizeof buf);
        if (n > 0) {
            c->sink(c->env, buf, (int64_t)n);
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && ok) {
            BURROW_OUT(err, burrow__pal_errno(errno));
            ok = false;
        }
        break;
    }
    (void)close((int)c->read);
    c->saved = PAL_INVALID_HANDLE;
    c->read = PAL_INVALID_HANDLE;
    return ok;
}

#endif

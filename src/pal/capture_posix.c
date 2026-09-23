/* Capturing standard output everywhere but Windows.
 *
 * A pipe, descriptor 1 moved onto its write end with dup2 and back again
 * afterwards, and a thread reading the other end until it sees the end of the
 * file. The end of the file comes when the capture puts descriptor 1 back,
 * because that closes the last write end the process holds.
 *
 * A child started during the capture inherits descriptor 1 and keeps the pipe
 * open for as long as it runs, and pal_stdout_capture_end waits for it. That is
 * what Go does too, since its pipe goes to children through os.Stdout the same
 * way.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#if !defined(_WIN32)
#define _DEFAULT_SOURCE 1
#endif

#include "burrow/platform.h"

#if !defined(BURROW_OS_WINDOWS)

#include "burrow/pal.h"

#include "internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

static void capture_close(int fd) {
    while (close(fd) != 0 && errno == EINTR) {
    }
}

static void capture_reader(void *arg) {
    PalStdoutCapture *c = arg;
    char buf[4096];
    for (;;) {
        ssize_t n = read((int)c->read, buf, sizeof buf);
        if (n > 0) {
            c->sink(c->env, buf, (int64_t)n);
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        return;
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
    if (pipe(fds) != 0) {
        BURROW_OUT(err, burrow__pal_errno(errno));
        return false;
    }
    (void)fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    (void)fcntl(fds[1], F_SETFD, FD_CLOEXEC);

    (void)fflush(stdout);
    /* A process started with descriptor 1 closed has nothing to save, and gets
     * it closed again at the end. */
    int saved = fcntl(1, F_DUPFD_CLOEXEC, 0);
    if (saved < 0 && errno != EBADF) {
        BURROW_OUT(err, burrow__pal_errno(errno));
        capture_close(fds[0]);
        capture_close(fds[1]);
        return false;
    }
    int rc;
    do {
        rc = dup2(fds[1], 1);
    } while (rc < 0 && errno == EINTR);
    if (rc < 0) {
        BURROW_OUT(err, burrow__pal_errno(errno));
        if (saved >= 0)
            capture_close(saved);
        capture_close(fds[0]);
        capture_close(fds[1]);
        return false;
    }
    capture_close(fds[1]);
    c->saved = saved;
    c->read = fds[0];

    c->thread = pal_thread_create(capture_reader, c, 0, err);
    if (c->thread == PAL_INVALID_HANDLE) {
        if (saved >= 0) {
            (void)dup2(saved, 1);
            capture_close(saved);
        } else {
            capture_close(1);
        }
        capture_close(fds[0]);
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
        int rc;
        do {
            rc = dup2((int)c->saved, 1);
        } while (rc < 0 && errno == EINTR);
        if (rc < 0) {
            BURROW_OUT(err, burrow__pal_errno(errno));
            ok = false;
            capture_close(1);
        }
        capture_close((int)c->saved);
    } else {
        capture_close(1);
    }
    PalErrno jerr = PAL_OK;
    if (!pal_thread_join(c->thread, &jerr) && ok) {
        BURROW_OUT(err, jerr);
        ok = false;
    }
    capture_close((int)c->read);
    c->saved = PAL_INVALID_HANDLE;
    c->read = PAL_INVALID_HANDLE;
    c->thread = PAL_INVALID_HANDLE;
    return ok;
}

#endif

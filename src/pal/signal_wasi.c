/* Signals on wasip1, which has none.
 *
 * Nothing can be caught, masked or sent, so a handler is never installed and a
 * relayed signal is accepted and never arrives, which is what Go's os/signal
 * does there. The one thing Go does do is kill: a signal a program sends itself
 * ends it, with 128 plus the signal's number as the exit code, and any other
 * process is ESRCH, since there are no others.
 *
 * The numbers are Go's for wasip1, which run in order from SIGHUP with no gaps
 * and so part from Linux's at SIGCHLD.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/platform.h"

#if defined(BURROW_OS_WASI)

#include "burrow/atomic.h"
#include "burrow/pal.h"

#include "internal.h"

#include <stdint.h>
#include <unistd.h>

/* Ours on the left, Go's wasip1 numbers on the right. */
static const struct {
    int32_t pal;
    int native;
} signal_pairs[] = {
    {PAL_SIGHUP, 1},   {PAL_SIGINT, 2},   {PAL_SIGQUIT, 3},     {PAL_SIGILL, 4},
    {PAL_SIGTRAP, 5},  {PAL_SIGABRT, 6},  {PAL_SIGBUS, 7},      {PAL_SIGFPE, 8},
    {PAL_SIGKILL, 9},  {PAL_SIGUSR1, 10}, {PAL_SIGSEGV, 11},    {PAL_SIGUSR2, 12},
    {PAL_SIGPIPE, 13}, {PAL_SIGALRM, 14}, {PAL_SIGTERM, 15},    {PAL_SIGCHLD, 16},
    {PAL_SIGCONT, 17}, {PAL_SIGSTOP, 18}, {PAL_SIGPREEMPT, 22},
};

static int to_native(int32_t sig) {
    for (size_t i = 0; i < sizeof signal_pairs / sizeof signal_pairs[0]; i++) {
        if (signal_pairs[i].pal == sig)
            return signal_pairs[i].native;
    }
    return -1;
}

int32_t burrow__pal_signal_from_native(int native) {
    for (size_t i = 0; i < sizeof signal_pairs / sizeof signal_pairs[0]; i++) {
        if (signal_pairs[i].native == native)
            return signal_pairs[i].pal;
    }
    return (int32_t)native;
}

static bool nosig(PalErrno *err) {
    BURROW_OUT(err, PAL_ENOTSUP);
    return false;
}

/* Go's Kill: this process is getpid's 3, and anything not positive means this
 * one too. */
bool pal_kill_native(int64_t pid, int32_t sig, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (pid > 0 && pid != pal_getpid()) {
        BURROW_OUT(err, PAL_ESRCH);
        return false;
    }
    _exit(128 + (int)(uint8_t)sig);
}

bool pal_kill(int64_t pid, int32_t sig, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    int native = sig == 0 ? 0 : to_native(sig);
    if (native < 0 || pid <= 0) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }
    return pal_kill_native(pid, (int32_t)native, err);
}

bool pal_signal_install(int32_t sig, PalSignalHandler handler, PalErrno *err) {
    (void)sig;
    if (handler == NULL) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }
    return nosig(err);
}

bool pal_signal_mask(int32_t sig, bool block, PalErrno *err) {
    (void)sig;
    (void)block;
    return nosig(err);
}

bool pal_signal_stack_install(PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    return true;
}

void pal_signal_stack_remove(void) {}

const void *pal_signal_fault_addr(const void *info) {
    (void)info;
    return NULL;
}

bool pal_signal_relay_init(PalSignalRelay send, PalErrno *err) {
    (void)send;
    BURROW_OUT(err, PAL_OK);
    return true;
}

bool pal_signal_relay(int32_t native, int32_t how, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);
    if (native <= 0 || native == 9 || native == 18 ||
        (how != PAL_RELAY_CATCH && how != PAL_RELAY_DEFAULT &&
         how != PAL_RELAY_IGNORE)) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }
    return true;
}

bool pal_signal_ignored(int32_t native) {
    (void)native;
    return false;
}

/* Nothing ever wakes the note, since nothing is ever caught, but a wake is
 * still kept for the next sleep as the contract says. A sleep with none
 * pending would wait forever, and the runtime parks the reading goroutine on
 * wasip1 instead of calling it. */
static uint32_t note_pending;

void pal_signal_note_wake(void) {
    burrow__atomic_store_release_u32(&note_pending, 1);
}

void pal_signal_note_sleep(void) {
    while (burrow__atomic_swap_u32(&note_pending, 0) == 0)
        (void)sleep(3600);
}

#endif /* BURROW_OS_WASI */

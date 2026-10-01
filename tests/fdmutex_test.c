/* Derived from Go's src/internal/poll/fd_mutex_test.go.
 * Go source: go1.27.1.
 *
 * TestMutexStress does not set GOMAXPROCS, since burrow cannot change it once
 * the runtime is going. It runs on however many Ps the test binary has.
 *
 * Copyright 2013 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/fdmutex.h"

#include "burrow/burrow.h"
#include "burrow/chan.h"
#include "burrow/func.h"
#include "burrow/mem/heap.h"
#include "burrow/proc.h"
#include "burrow/runtime.h"
#include "burrow/sync/atomic.h"

#include "check.h"
#include "fatal.h"

#include <string.h>

static void TestMutexLock(TestingT *t) {
    burrow__FdMutex mu;
    memset(&mu, 0, sizeof mu);

    CHECK(burrow__fdmu_incref(&mu));
    CHECK(!burrow__fdmu_decref(&mu));

    CHECK(burrow__fdmu_rwlock(&mu, true, true));
    CHECK(!burrow__fdmu_rwunlock(&mu, true));

    CHECK(burrow__fdmu_rwlock(&mu, false, true));
    CHECK(!burrow__fdmu_rwunlock(&mu, false));
}

static void TestMutexClose(TestingT *t) {
    burrow__FdMutex mu;
    memset(&mu, 0, sizeof mu);

    CHECK(burrow__fdmu_incref_and_close(&mu));
    CHECK(!burrow__fdmu_incref(&mu));
    CHECK(!burrow__fdmu_rwlock(&mu, true, true));
    CHECK(!burrow__fdmu_rwlock(&mu, false, true));
    CHECK(!burrow__fdmu_incref_and_close(&mu));
}

/* -------------------------------------------------------- CloseUnblock */

static burrow__FdMutex unblock_mu;
static Chan *unblock_c;
static SyncAtomicUint32 unblock_broken;

static void unblock_reader(void *env) {
    (void)env;
    if (burrow__fdmu_rwlock(&unblock_mu, true, true)) {
        sync_atomic_uint32_add(&unblock_broken, 1);
        return;
    }
    bool v = true;
    chan_send(unblock_c, &v);
}

static void TestMutexCloseUnblock(TestingT *t) {
    memset(&unblock_mu, 0, sizeof unblock_mu);
    sync_atomic_uint32_store(&unblock_broken, 0);
    unblock_c = chan_make(heap_allocator(), TYPE_BOOL, 4);

    burrow__fdmu_rwlock(&unblock_mu, true, true);
    for (int i = 0; i < 4; i++)
        CHECK(go(BURROW_FN(Func, unblock_reader, NULL)));

    /* Concurrent goroutines must not be able to read lock the mutex. */
    time_sleep(TIME_MILLISECOND);
    bool ok = false;
    CHECK(!chan_try_recv(unblock_c, NULL, &ok));

    burrow__fdmu_incref_and_close(&unblock_mu); /* Must unblock the readers. */
    for (int i = 0; i < 4; i++) {
        int64_t deadline = burrow_nanotime() + 10 * TIME_SECOND;
        while (!chan_try_recv(unblock_c, NULL, &ok)) {
            if (burrow_nanotime() > deadline)
                testing_t_fatal_v(t, "broken");
            time_sleep(TIME_MILLISECOND);
        }
    }
    CHECK_INT_EQ(sync_atomic_uint32_load(&unblock_broken), 0);
    CHECK(!burrow__fdmu_decref(&unblock_mu));
    CHECK(burrow__fdmu_rwunlock(&unblock_mu, true));
    chan_free(unblock_c);
}

/* ---------------------------------------------------------------- Panic */

static void TestMutexPanic(TestingT *t) {
    burrow__FdMutex mu;
    memset(&mu, 0, sizeof mu);

    const char *msg = "inconsistent poll.fdMutex";
    CHECK_PANIC(burrow__fdmu_decref(&mu), msg);
    CHECK_PANIC(burrow__fdmu_rwunlock(&mu, true), msg);
    CHECK_PANIC(burrow__fdmu_rwunlock(&mu, false), msg);

    CHECK_PANIC(
        (burrow__fdmu_incref(&mu), burrow__fdmu_decref(&mu), burrow__fdmu_decref(&mu)),
        msg);
    CHECK_PANIC((burrow__fdmu_rwlock(&mu, true, true), burrow__fdmu_rwunlock(&mu, true),
                 burrow__fdmu_rwunlock(&mu, true)),
                msg);
    CHECK_PANIC((burrow__fdmu_rwlock(&mu, false, true),
                 burrow__fdmu_rwunlock(&mu, false), burrow__fdmu_rwunlock(&mu, false)),
                msg);

    /* ensure that it's still not broken */
    CHECK(burrow__fdmu_incref(&mu));
    CHECK(!burrow__fdmu_decref(&mu));
    CHECK(burrow__fdmu_rwlock(&mu, true, true));
    CHECK(!burrow__fdmu_rwunlock(&mu, true));
    CHECK(burrow__fdmu_rwlock(&mu, false, true));
    CHECK(!burrow__fdmu_rwunlock(&mu, false));
}

static void overflow(burrow__FdMutex *mu) {
    for (int i = 0; i < 1 << 21; i++)
        burrow__fdmu_incref(mu);
}

static void TestMutexOverflowPanic(TestingT *t) {
    burrow__FdMutex mu1;
    memset(&mu1, 0, sizeof mu1);
    CHECK_PANIC(
        overflow(&mu1),
        "too many concurrent operations on a single file or socket (max 1048575)");
}

/* --------------------------------------------------------------- Stress */

static burrow__FdMutex stress_mu;
static uint64_t read_state[2];
static uint64_t write_state[2];
static Chan *stress_done;
static Int stress_n;

static void stress_body(void *env) {
    (void)env;
    bool good = false;
    for (Int i = 0; i < stress_n; i++) {
        switch (runtime_rand64() % 3) {
        case 0:
            if (!burrow__fdmu_incref(&stress_mu))
                goto out;
            if (burrow__fdmu_decref(&stress_mu))
                goto out;
            break;
        case 1:
            if (!burrow__fdmu_rwlock(&stress_mu, true, true))
                goto out;
            /* Ensure that it provides mutual exclusion for readers. */
            if (read_state[0] != read_state[1])
                goto out;
            read_state[0]++;
            read_state[1]++;
            if (burrow__fdmu_rwunlock(&stress_mu, true))
                goto out;
            break;
        default:
            if (!burrow__fdmu_rwlock(&stress_mu, false, true))
                goto out;
            /* Ensure that it provides mutual exclusion for writers. */
            if (write_state[0] != write_state[1])
                goto out;
            write_state[0]++;
            write_state[1]++;
            if (burrow__fdmu_rwunlock(&stress_mu, false))
                goto out;
            break;
        }
    }
    good = true;
out:
    chan_send(stress_done, &good);
}

static void TestMutexStress(TestingT *t) {
    int p = 8;
    stress_n = 1000000;
    if (testing_short()) {
        p = 4;
        stress_n = 10000;
    }
    memset(&stress_mu, 0, sizeof stress_mu);
    memset(read_state, 0, sizeof read_state);
    memset(write_state, 0, sizeof write_state);
    stress_done = chan_make(heap_allocator(), TYPE_BOOL, p);
    for (int i = 0; i < p; i++)
        CHECK(go(BURROW_FN(Func, stress_body, NULL)));
    for (int i = 0; i < p; i++) {
        bool good = false;
        chan_recv(stress_done, &good);
        if (!good)
            testing_t_error_v(t, "broken");
    }
    chan_free(stress_done);
    CHECK(burrow__fdmu_incref_and_close(&stress_mu));
    CHECK(burrow__fdmu_decref(&stress_mu));
}

#define TESTS(X)                                                                       \
    X(TestMutexLock)                                                                   \
    X(TestMutexClose)                                                                  \
    X(TestMutexCloseUnblock)                                                           \
    X(TestMutexPanic)                                                                  \
    X(TestMutexOverflowPanic)                                                          \
    X(TestMutexStress)

TESTING_MAIN(TESTS)

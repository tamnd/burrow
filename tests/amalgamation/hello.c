/* The smallest program a user of the amalgamation writes, compiled the way
 * they would compile it: burrow.c and this file, one -pthread, no include path
 * and no build system. CI builds it against a freshly generated burrow.c and
 * burrow.h and runs it, so a public header that is missing from burrow.h, or a
 * source file that only compiles with -Iinclude, fails there rather than in
 * somebody else's project.
 *
 * It starts some goroutines, sends their answers down a channel and checks the
 * total, which touches the scheduler, the context switch, channels and sync in
 * a dozen lines.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow.h"

#include <stdio.h>

#define WORKERS 8

static Chan *results;
static SyncWaitGroup wg;
static Int total;

static void square(void *env) {
    Int n = (Int)(intptr_t)env;
    Int sq = n * n;
    chan_send(results, &sq);
}

static void body(void *env) {
    (void)env;
    results = chan_make(heap_allocator(), TYPE_INT, WORKERS);
    for (int i = 1; i <= WORKERS; i++)
        sync_wait_group_go(&wg, BURROW_FN(Func, square, (void *)(intptr_t)i));
    sync_wait_group_wait(&wg);

    for (int i = 0; i < WORKERS; i++) {
        Int v = 0;
        chan_recv(results, &v);
        total += v;
    }
    chan_free(results);
}

int main(void) {
    runtime_main(BURROW_FN(Func, body, NULL));
    if (total != 204) {
        fprintf(stderr, "hello: got %lld, want 204\n", (long long)total);
        return 1;
    }
    printf("hello: burrow %s, %d goroutines, total %lld\n", BURROW_VERSION_STRING,
           WORKERS, (long long)total);
    return 0;
}

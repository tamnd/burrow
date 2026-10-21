/* Derived from Go's src/testing/cryptotest/rand.go. See
 * include/burrow/testing/cryptotest.h.
 * Go source: go1.27.1.
 *
 * Copyright 2025 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/testing/cryptotest.h"

#include "burrow/crypto/rand.h"
#include "burrow/encoding/binary.h"
#include "burrow/func.h"
#include "burrow/io.h"
#include "burrow/math/rand/v2.h"
#include "burrow/mem.h"
#include "burrow/mem/heap.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/sync.h"

#include "../crypto/rand_internal.h"

#include <stdint.h>
#include <string.h>

typedef struct CryptotestLockedReader {
    SyncMutex mu;
    Mathrand2ChaCha8 r;
    /* What the cleanup puts back. */
    IoReader previous;
} CryptotestLockedReader;

static Int cryptotest_locked_read(void *self, Slice b, Error *err) {
    CryptotestLockedReader *lr = self;
    sync_mutex_lock(&lr->mu);
    Int n = mathrand2_cha_cha8_read(&lr->r, b, err);
    sync_mutex_unlock(&lr->mu);
    return n;
}

static const IoReaderVT cryptotest_locked_reader_vt = {NULL, cryptotest_locked_read};

static void cryptotest_restore(void *env) {
    CryptotestLockedReader *lr = env;
    crypto_rand_reader = lr->previous;
    burrow__crypto_rand_set_testing_reader((IoReader){0});
    mem_free(heap_allocator(), lr, sizeof *lr, _Alignof(CryptotestLockedReader));
}

void cryptotest_set_global_random(TestingT *t, uint64_t seed) {
    if (t == NULL)
        panic_str(BURROW_S("cryptotest: SetGlobalRandom called with a nil *testing.T"));
    if (!testing_testing())
        panic_str(BURROW_S("cryptotest: SetGlobalRandom used in a non-test binary"));
    burrow__testing_t_check_parallel(t);

    Byte s[32];
    memset(s, 0, sizeof s);
    binary_little_endian_put_uint64(slice_from(s, 8, 8, TYPE_BYTE), seed);
    CryptotestLockedReader *lr =
        mem_alloc(heap_allocator(), sizeof *lr, _Alignof(CryptotestLockedReader));
    if (lr == NULL)
        panic_str(BURROW_S("cryptotest: out of memory"));
    memset(lr, 0, sizeof *lr);
    mathrand2_cha_cha8_seed(&lr->r, s);
    IoReader r = {&cryptotest_locked_reader_vt, lr};

    burrow__crypto_rand_set_testing_reader(r);
    lr->previous = crypto_rand_reader;
    crypto_rand_reader = r;
    testing_t_cleanup(t, BURROW_FN(Func, cryptotest_restore, lr));
}

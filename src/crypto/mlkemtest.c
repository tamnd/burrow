/* Derived from Go's src/crypto/mlkem/mlkemtest/mlkemtest.go.
 * Go source: go1.27.1.
 *
 * Go makes a new key of its FIPS module from ek.Bytes(), since the key it is
 * given is the one of crypto/mlkem. Here they are one key, so it is used as it
 * is, and the error Go has for a key that will not go back together cannot
 * happen.
 *
 * Copyright 2025 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/crypto/mlkem/mlkemtest.h"

#include "burrow/core.h"
#include "burrow/crypto/mlkem.h"
#include "burrow/error.h"
#include "burrow/mem.h"
#include "burrow/slice.h"

#include "mlkem_internal.h"

static Slice mlkemtest_done(Slice key, Slice *ciphertext, Error *err) {
    if (key.p == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        *ciphertext = slice_nil(TYPE_BYTE);
        return slice_nil(TYPE_BYTE);
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return key;
}

static bool mlkemtest_check(Slice random, const char *msg, Slice *ciphertext,
                            Error *err) {
    if (random.len == 32)
        return true;
    BURROW_OUT(err, errors_new(error_allocator(), str_from_cstr(msg)));
    *ciphertext = slice_nil(TYPE_BYTE);
    return false;
}

Slice mlkemtest_encapsulate768(const MlkemEncapsulationKey768 *ek, Alloc *a,
                               Slice random, Slice *ciphertext, Error *err) {
    if (!mlkemtest_check(random, "mlkemtest: Encapsulate768: random must be 32 bytes",
                         ciphertext, err))
        return slice_nil(TYPE_BYTE);
    Slice key = burrow__mlkem_encapsulate_internal768(ek, a, (const Byte *)random.p,
                                                      ciphertext);
    return mlkemtest_done(key, ciphertext, err);
}

Slice mlkemtest_encapsulate1024(const MlkemEncapsulationKey1024 *ek, Alloc *a,
                                Slice random, Slice *ciphertext, Error *err) {
    if (!mlkemtest_check(random, "mlkemtest: Encapsulate1024: random must be 32 bytes",
                         ciphertext, err))
        return slice_nil(TYPE_BYTE);
    Slice key = burrow__mlkem_encapsulate_internal1024(ek, a, (const Byte *)random.p,
                                                       ciphertext);
    return mlkemtest_done(key, ciphertext, err);
}

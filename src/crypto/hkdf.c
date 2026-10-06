/* Derived from Go's src/crypto/hkdf/hkdf.go and
 * src/crypto/internal/fips140/hkdf/hkdf.go.
 * Go source: go1.27.1.
 *
 * Copyright 2014 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/crypto/hkdf.h"

#include "burrow/core.h"
#include "burrow/crypto/hmac.h"
#include "burrow/error.h"
#include "burrow/hash.h"
#include "burrow/mem.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stdint.h>

Slice hkdf_extract(Alloc *a, HashNewFunc h, Slice secret, Slice salt, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    /* A missing salt is a block of zeros as long as the sum, and HMAC pads
     * an empty key with zeros to the block size anyway, so an empty salt
     * gives the same answer without making the zeros. */
    Hash ext = hmac_new(a, h, salt);
    if (ext.vt == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return slice_nil(TYPE_BYTE);
    }
    hash_write(ext, secret, NULL);
    return hash_sum(a, ext, slice_nil(TYPE_BYTE));
}

static Slice hkdf_expand_unchecked(Alloc *a, HashNewFunc h, Slice prk, Str info,
                                   Int key_length, Error *err) {
    Slice out = slice_make(a, TYPE_BYTE, 0, key_length);
    Hash exp = hmac_new(a, h, prk);
    if (exp.vt == NULL || (out.p == NULL && key_length > 0)) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return slice_nil(TYPE_BYTE);
    }
    Slice inf = slice_from((void *)(uintptr_t)info.p, info.len, info.len, TYPE_BYTE);
    Byte counter = 0;
    Slice buf = slice_nil(TYPE_BYTE);
    while (out.len < key_length) {
        counter++;
        if (counter == 0)
            panic_str(BURROW_S("hkdf: counter overflow"));
        if (counter > 1)
            hash_reset(exp);
        hash_write(exp, buf, NULL);
        hash_write(exp, inf, NULL);
        hash_write(exp, slice_from(&counter, 1, 1, TYPE_BYTE), NULL);
        buf = hash_sum(a, exp, slice_sub(buf, 0, 0));
        Int remain = key_length - out.len;
        if (remain > buf.len)
            remain = buf.len;
        out = slice_append(a, out, buf.p, remain);
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return out;
}

/* Go checks the length against the hash's size in the exported functions,
 * before the FIPS module that does the work ever sees it. */
static bool hkdf_too_long(Alloc *a, HashNewFunc h, Int key_length, Error *err) {
    Hash probe = h(a);
    if (probe.vt == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return true;
    }
    if (key_length > hash_size(probe) * 255) {
        BURROW_OUT(err, errors_new(error_allocator(),
                                   BURROW_S("hkdf: requested key length too large")));
        return true;
    }
    return false;
}

Slice hkdf_expand(Alloc *a, HashNewFunc h, Slice pseudorandom_key, Str info,
                  Int key_length, Error *err) {
    if (hkdf_too_long(a, h, key_length, err))
        return slice_nil(TYPE_BYTE);
    return hkdf_expand_unchecked(a, h, pseudorandom_key, info, key_length, err);
}

Slice hkdf_key(Alloc *a, HashNewFunc h, Slice secret, Slice salt, Str info,
               Int key_length, Error *err) {
    if (hkdf_too_long(a, h, key_length, err))
        return slice_nil(TYPE_BYTE);
    Slice prk = hkdf_extract(a, h, secret, salt, err);
    if (prk.p == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return slice_nil(TYPE_BYTE);
    }
    return hkdf_expand_unchecked(a, h, prk, info, key_length, err);
}

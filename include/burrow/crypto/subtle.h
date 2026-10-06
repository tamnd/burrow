/* crypto/subtle, the operations cryptographic code needs to do in a time that
 * does not depend on the secrets it works on.
 *
 *     if (subtle_constant_time_compare(mac, expected) != 1)
 *         return errors_new(a, BURROW_S("bad MAC"));
 *
 * A plain comparison stops at the first byte that differs, and how long it
 * took tells whoever is measuring how many bytes were right. These take the
 * same time whatever the bytes are, as long as the lengths are the same.
 *
 * C compilers are happy to turn arithmetic back into a branch when they can
 * prove it gives the same answer, which is exactly what has to not happen
 * here, so the functions are written with the masks hidden from the optimiser
 * behind an empty asm statement on GCC and Clang. MSVC has nothing like that,
 * and gets this file with its optimiser turned off around them instead.
 *
 * The answers are Ints that are 1 or 0, as in Go, so that they can be fed
 * straight back in as the v of a select or a copy.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package crypto/subtle */

#ifndef BURROW_CRYPTO_SUBTLE_H
#define BURROW_CRYPTO_SUBTLE_H

#include "burrow/core.h"
#include "burrow/func.h"
#include "burrow/slice.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 1 if x and y, Slices of Byte, hold the same bytes and 0 otherwise. The time
 * depends on the length and not on the contents. Slices of different lengths
 * give 0 at once. */
Int subtle_constant_time_compare(Slice x, Slice y);

/* x if v is 1 and y if v is 0. Go leaves any other v undefined, and here, as
 * in Go's own code, anything other than 0 picks x. */
Int subtle_constant_time_select(Int v, Int x, Int y);

/* 1 if x == y and 0 otherwise. */
Int subtle_constant_time_byte_eq(uint8_t x, uint8_t y);

/* 1 if x == y and 0 otherwise. */
Int subtle_constant_time_eq(int32_t x, int32_t y);

/* Copies y into x if v is 1 and leaves x alone if v is 0. x and y are Slices
 * of Byte of the same length. A different length panics, as it does in Go.
 * Any other v is undefined. */
void subtle_constant_time_copy(Int v, Slice x, Slice y);

/* 1 if x <= y and 0 otherwise. Go leaves it undefined for a negative x or y
 * or one above 2**31 - 1, and gives the right answer anyway, as this does. */
Int subtle_constant_time_less_or_eq(Int x, Int y);

/* Sets dst[i] = x[i] ^ y[i] for every i below n, the shorter of x's and y's
 * lengths, and returns n. All three are Slices of Byte.
 *
 * A dst shorter than n panics without writing anything. dst may be x or y, or
 * not overlap them at all, and anything in between panics. */
Int subtle_xor_bytes(Slice dst, Slice x, Slice y);

/* Runs f with the processor's data independent timing mode on, if it has one,
 * and turns the mode back off afterwards, also when f panics, unless it was
 * already on. Goroutines f starts run with it on as well.
 *
 * Only arm64 has the mode, as DIT. Everywhere else f just runs. The mode makes
 * the instructions that have it take the same time whatever their operands
 * are. It does nothing to code that branches on a secret, or reads memory at
 * an address made from one.
 *
 * GODEBUG=dataindependenttiming=1 has it on for the whole program instead. */
void subtle_with_data_independent_timing(Func f);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_CRYPTO_SUBTLE_H */

/* crypto/rand, random numbers that are safe for keys.
 *
 *     Byte key[32];
 *     crypto_rand_read(slice_from(key, 32, 32, TYPE_BYTE), NULL);
 *
 * The bytes come straight from the operating system: getrandom on Linux,
 * arc4random_buf or getentropy on macOS and the BSDs, RtlGenRandom on Windows
 * and random_get on WASI. burrow keeps no generator of its own in the
 * process, so there is no state for fork to copy into two processes and
 * nothing to fall back on when the system one fails.
 *
 * It does not fail. Go decided that a program asking for random bytes and not
 * getting them should stop rather than carry on with a key that is not random,
 * and burrow does the same, so crypto_rand_read either fills the whole buffer
 * or ends the process the way runtime_throw does. The error parameter is there
 * because Go's Read has one, and it is always BURROW_NO_ERROR.
 *
 * The package is called crypto_rand and not rand because math/rand is also a
 * rand, and math/rand is never what you want for a secret.
 *
 * Go's FIPS 140 mode and BoringCrypto are not here, so the bytes never go
 * through a DRBG of burrow's own.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package crypto/rand */

#ifndef BURROW_CRYPTO_RAND_H
#define BURROW_CRYPTO_RAND_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/math/big.h"
#include "burrow/mem.h"
#include "burrow/slice.h"

#ifdef __cplusplus
extern "C" {
#endif

/* rand.Reader, the system generator as an io.Reader. It is safe to use from
 * any number of threads. A program can point it somewhere else, the way Go code
 * can assign Reader, and crypto_rand_read and crypto_rand_text then read from
 * that instead. Assigning it is not synchronised, as in Go, so do it before
 * other threads start reading. */
extern IoReader crypto_rand_reader;

/* rand.Read: fills b, a Slice of Byte, with random bytes and returns its
 * length. If the reader fails it ends the process with
 *
 *     fatal error: crypto/rand: failed to read random data (see https://go.dev/issue/66821): ...
 *
 * and never returns. *err, which may be NULL, is always BURROW_NO_ERROR. */
Int crypto_rand_read(Slice b, Error *err);

/* rand.Text: 26 random characters from the RFC 4648 base32 alphabet, which is
 * at least 128 bits of randomness, for a token, a password or anything else
 * that has to be text. The string comes from a, and is empty only when a runs
 * out of memory. */
BURROW_OWNS(ret) Str crypto_rand_text(Alloc *a);

/* rand.Int: a uniform random value in [0, max), read from rand, made from a.
 * It panics if max is not positive. On an error from rand it returns NULL and
 * *err, which may be NULL, says what it was. */
BURROW_OWNS(ret) BigInt *crypto_rand_int(Alloc *a, IoReader rand, const BigInt *max,
                                         Error *err);

/* rand.Prime: a number of exactly bits bits that is prime with high
 * probability, made from a. It fails when bits is under 2.
 *
 * rand is ignored and the system generator is used, as in Go 1.26 and later.
 * GODEBUG=cryptocustomrand=1 brings back the old behaviour, which reads from
 * rand and sometimes reads one byte more on purpose so that callers do not
 * come to depend on exactly which bytes are used. */
BURROW_OWNS(ret) BigInt *crypto_rand_prime(Alloc *a, IoReader rand, Int bits,
                                           Error *err);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_CRYPTO_RAND_H */

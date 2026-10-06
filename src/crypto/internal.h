/* Which of the hardware paths for the hashes and ciphers this compiler can
 * build, and how.
 *
 * Each path is a function that uses one instruction set extension through the
 * compiler's intrinsics. It is built with a target attribute naming the
 * extension, so the rest of the library keeps the flags it was given and a
 * program built for any x86-64 or arm64 still runs everywhere. Whether the
 * function is called is decided at run time from pal_cpu_features. Where the
 * compiler already assumes the extension, as clang does for Apple silicon, the
 * attribute is empty.
 *
 * A compiler too old to take the attribute, or one this has not been tried
 * with, gets no hardware path and runs the portable code, which is always
 * there. Defining BURROW_PUREGO leaves the hardware paths out altogether, the
 * way Go's purego build tag does.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_CRYPTO_INTERNAL_H
#define BURROW_CRYPTO_INTERNAL_H

/* hash.h comes first so that the amalgamation files this header with the hash
 * package, which everything that includes it needs anyway. */
#include "burrow/hash.h"
#include "burrow/pal.h"
#include "burrow/platform.h"

#if !defined(BURROW_PUREGO)

#if defined(BURROW_ARCH_AMD64) || defined(BURROW_ARCH_386)
/* SHA-NI, which does SHA-1 and SHA-256 and needs SSSE3 and SSE4.1 for the
 * shuffles around it. */
#if defined(_MSC_VER) && !defined(__clang__)
#define CRYPTO_X86_SHA 1
#define CRYPTO_TARGET_X86_SHA
#elif (defined(__clang__) && __clang_major__ >= 9) ||                                  \
    (!defined(__clang__) && defined(__GNUC__) && __GNUC__ >= 5)
#define CRYPTO_X86_SHA 1
#define CRYPTO_TARGET_X86_SHA __attribute__((target("sha,sse4.1,ssse3")))
#endif
/* AES-NI, and the carryless multiply GCM uses next to it. The SSE2 and SSSE3
 * the code around them uses are in every processor that has either. */
#if defined(_MSC_VER) && !defined(__clang__)
#define CRYPTO_X86_AES 1
#define CRYPTO_TARGET_X86_AES
#define CRYPTO_TARGET_X86_GCM
#elif (defined(__clang__) && __clang_major__ >= 9) ||                                  \
    (!defined(__clang__) && defined(__GNUC__) && __GNUC__ >= 5)
#define CRYPTO_X86_AES 1
#define CRYPTO_TARGET_X86_AES __attribute__((target("aes,sse2")))
#define CRYPTO_TARGET_X86_GCM __attribute__((target("aes,pclmul,sse2,ssse3")))
#endif
#if defined(CRYPTO_X86_AES)
#define CRYPTO_X86_GCM_NEEDS                                                           \
    (PAL_CPU_X86_AES | PAL_CPU_X86_PCLMULQDQ | PAL_CPU_X86_SSSE3)
#endif
#if defined(CRYPTO_X86_SHA) || defined(CRYPTO_X86_AES)
#include <immintrin.h>
#endif
#if defined(CRYPTO_X86_SHA)
#define CRYPTO_X86_SHA_NEEDS (PAL_CPU_X86_SHA | PAL_CPU_X86_SSSE3 | PAL_CPU_X86_SSE41)
#include <string.h>

/* Sixteen bytes in and out of a vector at any alignment. memcpy says that
 * without a cast, and compilers turn it into one unaligned load or store. */
CRYPTO_TARGET_X86_SHA static inline __m128i crypto_x86_load(const void *p) {
    __m128i v;
    memcpy(&v, p, sizeof v);
    return v;
}

CRYPTO_TARGET_X86_SHA static inline void crypto_x86_store(void *p, __m128i v) {
    memcpy(p, &v, sizeof v);
}
#endif
#endif

#if defined(BURROW_ARCH_ARM64) && !defined(_MSC_VER)
/* The SHA-1 and SHA-256 instructions from Armv8.0 and the SHA-512 ones from
 * Armv8.2. Clang's arm_neon.h has declared its intrinsics behind target
 * attributes since clang 16, and GCC's behind target pragmas since GCC 10.
 * Apple's clang numbers its releases differently, but it always targets a
 * processor that has both, so it takes the first branch. */
#if defined(__ARM_FEATURE_SHA2) || defined(__ARM_FEATURE_CRYPTO)
#define CRYPTO_ARM64_SHA2 1
#define CRYPTO_TARGET_ARM64_SHA2
#elif defined(__clang__) && __clang_major__ >= 16 && !defined(__apple_build_version__)
#define CRYPTO_ARM64_SHA2 1
#define CRYPTO_TARGET_ARM64_SHA2 __attribute__((target("sha2")))
#elif !defined(__clang__) && defined(__GNUC__) && __GNUC__ >= 10
#define CRYPTO_ARM64_SHA2 1
#define CRYPTO_TARGET_ARM64_SHA2 __attribute__((target("+crypto")))
#endif
#if defined(__ARM_FEATURE_SHA512)
#define CRYPTO_ARM64_SHA512 1
#define CRYPTO_TARGET_ARM64_SHA512
#elif defined(__clang__) && __clang_major__ >= 16 && !defined(__apple_build_version__)
#define CRYPTO_ARM64_SHA512 1
#define CRYPTO_TARGET_ARM64_SHA512 __attribute__((target("sha3")))
#elif !defined(__clang__) && defined(__GNUC__) && __GNUC__ >= 10
#define CRYPTO_ARM64_SHA512 1
#define CRYPTO_TARGET_ARM64_SHA512 __attribute__((target("+sha3")))
#endif
/* The Armv8.0 AES instructions and the 64 bit carryless multiply, PMULL,
 * which compilers put behind the same feature. */
#if defined(__ARM_FEATURE_AES) || defined(__ARM_FEATURE_CRYPTO)
#define CRYPTO_ARM64_AES 1
#define CRYPTO_TARGET_ARM64_AES
#elif defined(__clang__) && __clang_major__ >= 16 && !defined(__apple_build_version__)
#define CRYPTO_ARM64_AES 1
#define CRYPTO_TARGET_ARM64_AES __attribute__((target("aes")))
#elif !defined(__clang__) && defined(__GNUC__) && __GNUC__ >= 10
#define CRYPTO_ARM64_AES 1
#define CRYPTO_TARGET_ARM64_AES __attribute__((target("+crypto")))
#endif
#if defined(CRYPTO_ARM64_SHA2) || defined(CRYPTO_ARM64_SHA512) ||                      \
    defined(CRYPTO_ARM64_AES)
#include <arm_neon.h>
#endif
#endif

#endif /* !BURROW_PUREGO */

#endif /* BURROW_CRYPTO_INTERNAL_H */

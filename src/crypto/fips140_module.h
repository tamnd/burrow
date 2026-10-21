/* The FIPS 140-3 module boundary: which code and constants are the module, and
 * the check that they are the bytes they were when the program was linked.
 *
 * Go marks every symbol of the packages under crypto/internal/fips140 as FIPS
 * code or data, and its linker gathers them into sections of their own,
 * computes an HMAC over them and writes it into the program. With
 * GODEBUG=fips140=on, crypto/internal/fips140/check computes the HMAC again at
 * startup and stops the program if the two differ.
 *
 * A C compiler can do the first part when it is told to, one function or
 * object at a time. CRYPTO_FIPS_TEXT goes in front of each function of the
 * module and CRYPTO_FIPS_RODATA in front of each of its constant tables, and
 * the linker gathers them into two sections, one of code and one of read-only
 * data. They are burrow_fips_text and burrow_fips_rodata on ELF, __bfips_text
 * and __bfips_rodata in the __TEXT segment on Mach-O, and .bfipt and .bfipr on
 * PE. No C linker computes the HMAC, so "burrow-gen fips PROGRAM" does that
 * after linking, the way Go's linker does it after its own. A program it has
 * not been run on has no checksum, and FIPS mode refuses to start in it, as in
 * Go when the linker info is missing.
 *
 * The constant tables only go in the section when they hold no pointers. A
 * pointer in a position independent program is fixed up when it is loaded, so
 * the bytes in memory are not the bytes in the file, and the HMAC could never
 * match. Go hashes its FIPS data too, because its compiler keeps such pointers
 * out of it and leaves them for the package's init function to fill in. burrow
 * hashes the code and the read-only tables without pointers, which are the
 * same in memory as in the file.
 *
 * What the compiler makes on its own, such as string literals and constant
 * pools, stays in the usual sections. So do the small constants declared inside
 * functions, the thunks BURROW_METHODS_DEFINE writes for calling methods
 * through reflection, and any function from a header that the compiler chose
 * not to inline. That is fiat-crypto's larger field functions with gcc at -O2,
 * and burrow's own inline functions as well at -O0. A pass of the HMAC means
 * the module's own functions and tables are unchanged, not every byte they use.
 *
 * Where the compiler or the format has no way to name a section, both macros
 * are empty, and burrow__fips140_supported says FIPS mode is not supported.
 *
 * Not in Go.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SRC_CRYPTO_FIPS140_MODULE_H
#define BURROW_SRC_CRYPTO_FIPS140_MODULE_H

/* First, for the amalgamation, which puts a header in the package of the first
 * burrow header it includes. crypto/subtle needs nothing outside the core, so
 * every file of the module can depend on it. */
#include "burrow/crypto/subtle.h"

#include "burrow/core.h"
#include "burrow/error.h"

#include <stdbool.h>
#include <stdint.h>

#if defined(__COSMOPOLITAN__) || defined(__wasm__) || defined(BURROW_NO_FIPS_SECTIONS)
/* No sections: an APE program is rewritten after linking, and wasm has none of
 * the kind this needs. BURROW_NO_FIPS_SECTIONS is for make FIPS_SECTIONS=0,
 * which make ctgrind needs. */
#elif defined(_MSC_VER) && !defined(__clang__)
#define CRYPTO_FIPS_TEXT __declspec(code_seg(".bfipt"))
#define CRYPTO_FIPS_RODATA                                                             \
    __pragma(section(".bfipr", read)) __declspec(allocate(".bfipr"))
#define CRYPTO_FIPS_SECTIONS 1
#elif defined(__APPLE__) && defined(__GNUC__)
#define CRYPTO_FIPS_TEXT                                                               \
    __attribute__((section("__TEXT,__bfips_text,regular,pure_instructions")))
#define CRYPTO_FIPS_RODATA __attribute__((section("__TEXT,__bfips_rodata")))
#define CRYPTO_FIPS_SECTIONS 1
#elif defined(_WIN32) && defined(__GNUC__)
#define CRYPTO_FIPS_TEXT __attribute__((section(".bfipt")))
#define CRYPTO_FIPS_RODATA __attribute__((section(".bfipr")))
#define CRYPTO_FIPS_SECTIONS 1
#elif defined(__ELF__) && defined(__GNUC__)
#define CRYPTO_FIPS_TEXT __attribute__((section("burrow_fips_text")))
#define CRYPTO_FIPS_RODATA __attribute__((section("burrow_fips_rodata")))
#define CRYPTO_FIPS_SECTIONS 1
#endif

#if !defined(CRYPTO_FIPS_SECTIONS)
#define CRYPTO_FIPS_TEXT
#define CRYPTO_FIPS_RODATA
#endif

/* NULL when FIPS 140-3 mode can work in this build, and otherwise why not,
 * such as "FIPS 140-3 mode is incompatible with ASAN". Go's fips140.Supported,
 * which returns an error. */
BURROW_STATIC(ret) const char *burrow__fips140_supported(void);

/* Computes the HMAC over the module's sections and compares it with the one
 * burrow-gen fips wrote into the program. NULL when they match, and otherwise
 * what went wrong: "no verification checksum found" for a program burrow-gen
 * fips has not been run on, and "verification mismatch" when the bytes have
 * changed. The check in Go's crypto/internal/fips140/check, without the panic,
 * which is the caller's. */
BURROW_STATIC(ret) const char *burrow__fips140_verify(void);

/* Writes the HMAC of the module's sections, as they are in memory now, to sum.
 * False when there are no sections or no memory. For burrow__fips140_verify,
 * and for tests/fips140_check_test.c, which writes it into a copy of itself. */
bool burrow__fips140_module_sum(Byte sum[32]);

/* The bounds of the two sections, the code and the read-only data, all NULL
 * when there are none. For tests/fips140_check_test.c. */
void burrow__fips140_sections(const Byte *bounds[4]);

/* Whether FIPS 140-3 mode is on, Go's fips140.Enabled. Nothing turns it on yet
 * but burrow__fips140_force_on, which is for tests. */
bool burrow__fips140_enabled(void);

/* A self-test, Go's func() error, with arg, what was passed along with it. */
typedef Error (*FipsSelfTest)(void *arg);

/* Runs the named Cryptographic Algorithm Self-Test when FIPS 140-3 mode is on,
 * and ends the program, with runtime_throw, when it fails. Go's fips140.CAST.
 * The name must not contain commas, colons, hashes or equal signs. */
void burrow__fips140_cast(const char *name, FipsSelfTest f, void *arg);

/* The same for a Pairwise Consistency Test, which every key pair the module
 * generates goes through. Go's fips140.PCT. */
void burrow__fips140_pct(const char *name, FipsSelfTest f, void *arg);

/* Not in Go: turns FIPS 140-3 mode on, as GODEBUG=fips140=on does in Go, or
 * fips140=debug with debug true. Like Go's init functions it then checks that
 * the mode is supported, runs the integrity check, and runs the self-tests Go
 * runs at init, panicking or ending the program when one fails. Once only, and
 * before anything else uses the module: it is for tests, which run a copy of
 * themselves for it. */
void burrow__fips140_force_on(bool debug);

/* The self-tests Go runs from the init functions of its packages, each in the
 * file of its package, as in Go, for burrow__fips140_force_on. */
Error burrow__sha256_cast(void *arg);
Error burrow__sha512_cast(void *arg);
Error burrow__sha3_cast(void *arg);
Error burrow__hmac_cast(void *arg);
Error burrow__hkdf_cast(void *arg);
Error burrow__pbkdf2_cast(void *arg);
Error burrow__aes_cast(void *arg);
Error burrow__drbg_cast(void *arg);
Error burrow__ctrkdf_cast(void *arg);
Error burrow__tls12_cast(void *arg);
Error burrow__tls13_cast(void *arg);

/* Not in Go: one function and one constant of the module that nothing uses,
 * for tests/fips140_check_test.c to look for in the sections, which is what
 * Go's crypto/internal/fips140/check/checktest is for. RODATA is 2, as there. */
void burrow__fips140_checktest_text(void);
extern const int32_t burrow__fips140_checktest_rodata;

#endif /* BURROW_SRC_CRYPTO_FIPS140_MODULE_H */

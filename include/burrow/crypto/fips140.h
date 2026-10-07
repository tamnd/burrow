/* crypto/fips140, which says whether the cryptography is running in FIPS 140-3
 * mode.
 *
 *     if (fips140_enabled())
 *         ...
 *
 * Go has a FIPS 140-3 module inside it, a fixed set of packages that has been
 * through validation, and GODEBUG=fips140=on makes a program use only that
 * module, check its own code against a checksum at startup and run the self
 * tests the standard asks for. burrow has no validated module and makes no
 * claim to one. Its code follows Go's module, but it has not been through
 * validation and saying that it was would be false.
 *
 * So the answer here is always no. GODEBUG=fips140=on, only or debug makes
 * fips140_enabled and fips140_enforced panic, the way a Go program panics at
 * startup on a platform where FIPS 140-3 mode is not supported, rather than
 * let a program that asked for FIPS mode run without it. Any other value
 * except off panics too, as it does in Go. A program that never calls these
 * does not look at the setting at all.
 *
 * The setting is read once, the first time it is needed, and changing GODEBUG
 * after that changes nothing, as in Go.
 *
 * Copyright 2024 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package crypto/fips140 */

#ifndef BURROW_CRYPTO_FIPS140_H
#define BURROW_CRYPTO_FIPS140_H

#include "burrow/core.h"
#include "burrow/func.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Whether the cryptography libraries are operating in FIPS 140-3 mode, which
 * in burrow they never are. Panics when GODEBUG asks for the mode, as said at
 * the top. */
bool fips140_enabled(void);

/* The version of the FIPS 140-3 module. Go gives "v1.0.0" and the like for a
 * program built against a frozen module, and "latest" otherwise, which is
 * what this gives. */
BURROW_STATIC(ret) Str fips140_version(void);

/* Whether strict FIPS 140-3 enforcement is on, which is what
 * GODEBUG=fips140=only asks for in Go. Always false, and panics like
 * fips140_enabled. */
bool fips140_enforced(void);

/* Runs f with strict enforcement turned off. Without enforcement on, which is
 * always in burrow, it just runs f. */
void fips140_without_enforcement(Func f);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_CRYPTO_FIPS140_H */

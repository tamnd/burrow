/* crypto/fips140, which says whether the cryptography is running in FIPS 140-3
 * mode.
 *
 *     if (fips140_enabled())
 *         ...
 *
 * Go has a FIPS 140-3 module inside it, a fixed set of packages that has been
 * through validation, and GODEBUG=fips140=on makes a program use only that
 * module, check its own code against a checksum at startup and run the self
 * tests the standard asks for. burrow has the same module, in the files that
 * port Go's, and GODEBUG=fips140=on, only and debug turn the mode on as they
 * do in Go. Any other value except off panics, as it does in Go.
 *
 * burrow is not FIPS 140-3 validated. Its module follows Go's, but it has not
 * been through validation, and saying that it was would be false. The mode is
 * there for what it does: the self-tests, the integrity check and the service
 * indicator. fips140=only also makes fips140_enforced true, and then the
 * packages outside the module refuse what is not approved, with an error or a
 * panic, as they do in Go.
 *
 * The integrity check needs the module's checksum in the program, which Go's
 * linker writes and burrow-gen fips writes here, after linking:
 *
 *     tools/burrow-gen fips PROGRAM
 *
 * With the mode on, a program without it panics with "fips140: no verification
 * checksum found".
 *
 * Go reads the setting when the program starts. burrow has nothing that runs
 * then, so it reads it the first time the program uses the module or calls one
 * of these, and changing GODEBUG after that changes nothing, as in Go.
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

/* Enabled reports whether the cryptography libraries are operating in FIPS
 * 140-3 mode.
 *
 * It can be controlled at runtime using the GODEBUG setting "fips140". If set
 * to "on", FIPS 140-3 mode is enabled. If set to "only", non-approved
 * cryptography functions will additionally return errors or panic.
 *
 * This can't be changed after the program has started. */
bool fips140_enabled(void);

/* The version of the FIPS 140-3 module. Go gives "v1.0.0" and the like for a
 * program built against a frozen module, and "latest" otherwise, which is
 * what this gives. */
BURROW_STATIC(ret) Str fips140_version(void);

/* Enforced indicates if strict FIPS 140-3 enforcement is enabled. Strict
 * enforcement is enabled when a program is run with GODEBUG=fips140=only and
 * enforcement has not been disabled by a call to fips140_without_enforcement. */
bool fips140_enforced(void);

/* WithoutEnforcement disables strict FIPS 140-3 enforcement while executing f.
 * Calling WithoutEnforcement without strict enforcement enabled
 * (GODEBUG=fips140=only is not set or already inside of a call to
 * WithoutEnforcement) is a no-op.
 *
 * WithoutEnforcement is inherited by any goroutines spawned while executing f.
 *
 * As this disables enforcement, it should be applied carefully to tightly
 * scoped functions. */
void fips140_without_enforcement(Func f);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_CRYPTO_FIPS140_H */

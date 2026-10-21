/* Derived from Go's src/crypto/fips140/fips140.go and enforcement.go.
 * Go source: go1.27.1.
 *
 * Copyright 2024 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/crypto/fips140.h"

#include "burrow/core.h"
#include "burrow/defer.h"
#include "burrow/func.h"
#include "burrow/panic.h"
#include "burrow/sched.h"

#include "fips140_module.h"

bool fips140_enabled(void) {
    if (burrow__fips140_enabled() && !burrow__fips140_verified())
        panic_str(BURROW_S("crypto/fips140: FIPS 140-3 mode enabled, but integrity "
                           "check didn't pass"));
    return burrow__fips140_enabled();
}

Str fips140_version(void) {
    return BURROW_S("latest");
}

bool fips140_enforced(void) {
    return burrow__fips140_mode() == FIPS140_ONLY && !burrow__fips140_is_bypassed();
}

static void fips140_unset_bypass(void *arg) {
    (void)arg;
    burrow__fips140_unset_bypass();
}

void fips140_without_enforcement(Func f) {
    if (!fips140_enabled() || !fips140_enforced()) {
        f.f(f.env);
        return;
    }
    burrow__fips140_set_bypass();
    BURROW_SCOPE {
        BURROW_DEFER(fips140_unset_bypass, NULL);
        f.f(f.env);
    }
    BURROW_SCOPE_END;
}

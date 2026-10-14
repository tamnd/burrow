/* expvar's hook for its tests.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SRC_EXPVAR_EXPVAR_INTERNAL_H
#define BURROW_SRC_EXPVAR_EXPVAR_INTERNAL_H

#include "burrow/expvar.h"

/* RemoveAll from Go's expvar_test.go: unpublishes everything, cmdline and
 * memstats included, so that a test sees only what it published. */
void burrow__expvar_remove_all(void);

/* Puts things back the way the first use finds them: cmdline and memstats and
 * nothing else, for a test that runs after another one removed them. */
void burrow__expvar_publish_defaults(void);

#endif /* BURROW_SRC_EXPVAR_EXPVAR_INTERNAL_H */

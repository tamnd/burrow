/* go/version, comparing Go versions.
 *
 * Go's go/version. It works on versions in Go's toolchain name syntax, strings
 * like "go1.20", "go1.21.0", "go1.22rc2" and "go1.23.4-custom":
 *
 *     version_compare(BURROW_S("go1.21rc1"), BURROW_S("go1.21.0")) // -1
 *     version_is_valid(BURROW_S("go1.22"))                         // true
 *     version_lang(a, BURROW_S("go1.21.2"))                        // "go1.21"
 *
 * Copyright 2023 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package go/version */

#ifndef BURROW_GO_VERSION_H
#define BURROW_GO_VERSION_H

#include "burrow/core.h"
#include "burrow/mem.h"
#include "burrow/own.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The Go language version for version x: "go1.21" for "go1.21rc2", "go1.21.2"
 * and "go1.21", and "go1" for "go1". If x is not a valid version the result is
 * empty, and so it is for "1.21", which has no "go" prefix.
 *
 * The result is the front of x, except when x leaves out a minor version that
 * the language version has to spell out: "go222" is language "go222.0". That
 * one is built in a. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, x) Str version_lang(Alloc *a, Str x);

/* -1, 0 or +1 as x is less than, equal to or greater than y as Go versions.
 * Both need the "go" prefix: "go1.21", not "1.21". Invalid versions, the empty
 * string included, compare less than valid ones and equal to each other. The
 * language version "go1.21" is less than the release candidate "go1.21rc1" and
 * the release "go1.21.0". */
Int version_compare(Str x, Str y);

/* Whether x is a valid version. */
bool version_is_valid(Str x);

#ifdef __cplusplus
}
#endif

#endif

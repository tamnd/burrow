/* What the files of the mime package share and nobody else sees.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SRC_MIME_INTERNAL_H
#define BURROW_SRC_MIME_INTERNAL_H

#include "burrow/mime.h"

#include "burrow/core.h"
#include "burrow/error.h"

/* Go's needsEncoding: whether s has a byte that is not printable ASCII, other
 * than tab. */
bool burrow__mime_needs_encoding(Str s);

/* The error for an allocation that failed. */
extern const Error burrow__mime_err_no_memory;

#endif /* BURROW_SRC_MIME_INTERNAL_H */

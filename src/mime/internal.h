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

/* For the tests, which do what Go's tests do with testInitMime. reset skips the
 * system's types for good and empties the table, then puts the built in types
 * back when builtin is set. The loaders read one globs2 file, false when it
 * cannot be opened, or one mime.types file, and set is setExtensionType. */
void burrow__mime_types_reset(bool builtin);
bool burrow__mime_load_globs_file(const char *path);
void burrow__mime_load_types_file(const char *path);
Error burrow__mime_set_extension_type(Str ext, Str type);

#endif /* BURROW_SRC_MIME_INTERNAL_H */

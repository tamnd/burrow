/* net/http/internal/ascii, the ASCII only string helpers net/http uses where
 * a header's rules are about bytes and not about Unicode.
 *
 * Copyright 2021 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package net/http/internal/ascii */

#ifndef BURROW_SRC_NET_HTTP_ASCII_H
#define BURROW_SRC_NET_HTTP_ASCII_H

#include "burrow/core.h"
#include "burrow/mem.h"
#include "burrow/own.h"

#include <stdbool.h>

/* ascii.EqualFold. Whether s and t are the same when only the ASCII letters
 * are folded, so the Kelvin sign is not a K. */
bool burrow__http_ascii_equal_fold(Str s, Str t);

/* ascii.IsPrint. Whether every byte of s is a printable ASCII character, space
 * to tilde. */
bool burrow__http_ascii_is_print(Str s);

/* ascii.Is. Whether every byte of s is ASCII. */
bool burrow__http_ascii_is(Str s);

/* ascii.ToLower. s in lower case, and true, when it is printable ASCII, and ""
 * and false when it is not. The result is s when it has no capitals and a copy
 * in a otherwise, and *ok is false as well when a says no. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, s) Str burrow__http_ascii_to_lower(Alloc *a, Str s,
                                                                        bool *ok);

#endif /* BURROW_SRC_NET_HTTP_ASCII_H */

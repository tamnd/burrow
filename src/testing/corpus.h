/* What corpus.c gives testing.c besides the codec, which is in
 * burrow/testing.h so that the tests can reach it: the copies of fuzz values
 * that an F and a decoded corpus entry own.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_TESTING_CORPUS_H
#define BURROW_TESTING_CORPUS_H

#include "burrow/testing.h"

#include "burrow/core.h"
#include "burrow/type.h"

/* A copy of the value at src, of type t, on the heap. A string's bytes and a
 * []byte's backing array are copied too. */
Any burrow__testing_value_copy(const Type *t, const void *src);

/* Frees one value from burrow__testing_value_copy. */
void burrow__testing_value_free(Any v);

#endif

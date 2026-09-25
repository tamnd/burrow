/* What the files of the sort package share and nobody else sees.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SRC_SORT_INTERNAL_H
#define BURROW_SRC_SORT_INTERNAL_H

#include "burrow/sort.h"

#include "burrow/core.h"

/* What export_test.go gives Go's tests, for tests/sort_test.c: Heapsort and
 * ReverseRange, which sort uses inside and does not export. */
void burrow__sort_heapsort(SortInterface data);
void burrow__sort_reverse_range(SortInterface data, Int a, Int b);

#endif /* BURROW_SRC_SORT_INTERNAL_H */

/* What the files of the strings package share and nobody else sees.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SRC_STRINGS_INTERNAL_H
#define BURROW_SRC_STRINGS_INTERNAL_H

#include "burrow/strings.h"

#include "burrow/core.h"
#include "burrow/mem.h"
#include "burrow/slice.h"

/* s as a Slice of Byte without a copy, to hand to an io.Writer, which only
 * reads it. */
Slice burrow__strings_bytes(Str s);

/* What export_test.go gives Go's tests, for tests/strings_replace_test.c. The
 * first is the Go type name of the replacer NewReplacer picked, the second is
 * Go's PrintTrie, and the last two are StringFind and DumpTables. */
const char *burrow__strings_replacer_kind(StringsReplacer *r);
Str burrow__strings_print_trie(StringsReplacer *r, Alloc *a);
Int burrow__strings_string_find(Alloc *a, Str pattern, Str text);
bool burrow__strings_dump_tables(Alloc *a, Str pattern, Int bad[256], Int **good);

#endif /* BURROW_SRC_STRINGS_INTERNAL_H */

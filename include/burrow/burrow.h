/* burrow: the Go standard library, in C.
 *
 * This is the umbrella header for the split form of the tree, the one you use
 * when you have checked the repository out. If you downloaded the amalgamation
 * you have a single burrow.h at the top of your project and you do not need
 * this file, because the generator produced that one from these.
 *
 * Include what you need instead of this, if you care about compile time:
 *
 *     #include "burrow/strings.h"
 *     #include "burrow/net/http.h"
 *
 * Two things to know before you read any other header.
 *
 * First, there is no prefix on anything you call. strings.Contains is
 * strings_contains, not burrow_strings_contains, because the package segment is
 * already a namespace and a second one on top of it would be noise repeated
 * 23,730 times. Types are CamelCase, functions are snake_case, and macros keep a
 * BURROW_ prefix because the preprocessor ignores every scoping mechanism C has.
 *
 * Second, every function that can allocate takes an allocator as its first
 * parameter. No exceptions, no hidden global, no per-object free function to
 * remember. Make an arena, pass it down, free it once.
 *
 *     Arena ar;
 *     arena_init(&ar, NULL, 0);
 *     Alloc *a = arena_allocator(&ar);
 *     Slice parts = strings_split(a, path, S("/"));
 *     arena_free(&ar);
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_H
#define BURROW_H

#include "burrow/platform.h"
#include "burrow/version.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/iface.h"
#include "burrow/io.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/fixed.h"
#include "burrow/mem/heap.h"
#include "burrow/runtime.h"
#include "burrow/slice.h"
#include "burrow/type.h"

/* The rest of the library arrives here as it is written. The order is the
 * construction order from docs/design/06-runtime.md section 12, because the
 * headers have the same dependency shape the code does:
 *
 *   core types, allocators, runtime, reflect, then the packages in tier order.
 *
 * Allocators are done. Str, the type descriptor, Slice, Error, Map and the
 * interface machinery are done, which is all of the core types. io.Reader and
 * io.Writer are here as the first interfaces built on it, with nothing behind
 * them yet: os and bytes are what fill them in.
 *
 * Function values and closures are next, then the numeric conversions and
 * rune handling, and then the runtime. See the milestone issues. */

#endif /* BURROW_H */

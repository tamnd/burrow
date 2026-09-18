/* Ownership annotations, which answer the one question the allocator rule
 * leaves open.
 *
 * Every function in burrow that can allocate takes an allocator, so you always
 * know where memory came from. What that does not tell you is whether the Str
 * or the Slice coming back is fresh memory or a view into something you passed
 * in. Go's collector makes the difference invisible and it does not matter
 * there. In C it decides whether you can free the input while still holding the
 * output, so every declaration says which it is:
 *
 *     BURROW_OWNS(ret)        Str strings_to_upper(Alloc *a, Str s);
 *     BURROW_BORROWS(ret, s)  Str strings_trim_space(Str s);
 *     BURROW_STATIC(ret)      const char *burrow_version(void);
 *
 * BURROW_OWNS(ret) means the result is fresh memory from the allocator and does
 * not depend on the input. BURROW_BORROWS(ret, s) means it points into s and
 * dies when s does. BURROW_RETAINS(s) means the function kept a reference to an
 * argument past the call. BURROW_STATIC(ret) means the result has static
 * storage duration or is nil, so there is nothing to free and nothing it can
 * outlive, which is a different statement from a borrow and not a weaker one: a
 * borrow has to name what it came from and these have nothing to name.
 *
 * OWNS and BORROWS can both appear on one declaration, and on append they do,
 * because append writes into the existing array when there is room and
 * allocates when there is not. The caller cannot tell which happened, so the
 * caller has to keep the input alive and also free the result.
 *
 * They expand to nothing, and three tools read them: the documentation
 * generator, which writes the lifetime sentence on every reference page, the
 * conformance harness, which turns each one into an AddressSanitizer test, and
 * an optional clang plugin. tools/check-annotations.sh checks they are present
 * and name real parameters, and tests/lifetime_test.c checks they are true.
 * docs/guides/allocators.md is the long version of all of this.
 *
 * This is a header of its own rather than a section of mem.h because platform.h
 * and version.h have functions to annotate and no business depending on the
 * allocator interface to do it.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_OWN_H
#define BURROW_OWN_H

#define BURROW_OWNS(...)
#define BURROW_BORROWS(...)
#define BURROW_RETAINS(...)
#define BURROW_STATIC(...)

#endif /* BURROW_OWN_H */

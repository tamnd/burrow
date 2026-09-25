/* path, for slash separated paths like the ones in URLs and archives.
 *
 * Go's path package. It only knows about forward slashes and never looks at
 * the file system, so it gives the same answer on every platform. For the
 * paths of the operating system, with backslashes and volume names on
 * Windows, use path/filepath instead.
 *
 *     Str c = path_clean(a, BURROW_S("a/c/../b//"));        // "a/b"
 *     Str j = path_join_v(a, 3, BURROW_S("a"), BURROW_S("b"), BURROW_S("c"));
 *     Str e = path_ext(BURROW_S("/x/photo.jpg"));             // ".jpg"
 *
 * The functions that only cut a path up return views into the one you gave
 * them and never allocate. path_clean, path_join and path_dir take an
 * allocator, and like Go they hand back their input when it is already clean,
 * so a clean path costs nothing. Treat the result as borrowed from both the
 * allocator and the input. A failed allocation gives the empty string.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package path */

#ifndef BURROW_PATH_H
#define BURROW_PATH_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/mem.h"
#include "burrow/slice.h"

#ifdef __cplusplus
extern "C" {
#endif

/* path.ErrBadPattern, from path_match for a pattern that is malformed. */
extern const Error path_err_bad_pattern;

/* The shortest path that names the same thing as path, by purely lexical
 * processing. It turns runs of slashes into one, drops "." elements, removes
 * each ".." along with the element before it, drops ".." at the start of a
 * rooted path and drops a trailing slash. The empty path becomes ".". */
BURROW_OWNS(ret) BURROW_BORROWS(ret, path) Str path_clean(Alloc *a, Str path);

/* path cut after its last slash, so dir ends in a slash or is empty and
 * file has no slash in it. dir + file is always path. */
BURROW_BORROWS(ret, path) Str path_split(Str path, Str *file);

/* The elements of elem, a Slice of Str, joined with slashes and then
 * cleaned. Empty elements are ignored, and if they all are the result is the
 * empty string and not ".". */
BURROW_OWNS(ret) Str path_join(Alloc *a, Slice elem);

/* path_join without building a Slice first:
 *
 *     Str p = path_join_v(a, 2, BURROW_S("usr"), BURROW_S("lib"));
 *
 * n is the count because C variadics cannot be counted at runtime, and every
 * argument after it must be a Str. */
BURROW_OWNS(ret) Str path_join_v(Alloc *a, int n, ...);

/* The extension of the last element, from its last dot, or empty if it has
 * none. */
BURROW_BORROWS(ret, path) Str path_ext(Str path);

/* The last element, with trailing slashes removed first. The empty path gives
 * "." and a path of only slashes gives "/". */
BURROW_BORROWS(ret, path) Str path_base(Str path);

bool path_is_abs(Str path);

/* All but the last element, cleaned. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, path) Str path_dir(Alloc *a, Str path);

/* Whether name matches the shell pattern, which is Go's syntax: '*' is any
 * run of non-slash bytes, '?' is one non-slash character, [abc] and [a-z] and
 * [^a-z] are classes, and a backslash quotes the character after it. The
 * whole of name has to match.
 *
 * A malformed pattern gives false and sets *err to path_err_bad_pattern, and
 * err may be NULL. Go checks the rest of the pattern only when the match
 * fails, so a pattern can match before it reaches its bad part and report no
 * error, and the same is true here. */
bool path_match(Str pattern, Str name, Error *err);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_PATH_H */

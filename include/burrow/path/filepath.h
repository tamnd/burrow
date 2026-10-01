/* path/filepath, for the paths of the operating system this runs on.
 *
 * Go's path/filepath. Unlike path, it knows the platform: the separator is a
 * backslash on Windows and a slash everywhere else, and on Windows a path can
 * start with a volume name, a drive letter like C: or a UNC share like
 * \\host\share, and forward slashes count as separators too.
 *
 *     Str c = filepath_clean(a, BURROW_S("a/c/../b//"));   // "a/b", or "a\b" on Windows
 *     Str j = filepath_join_v(a, 2, BURROW_S("usr"), BURROW_S("lib"));
 *     Str r = filepath_rel(a, BURROW_S("/a"), BURROW_S("/a/b/c"), &err);   // "b/c"
 *
 * Everything here is lexical, the same as in path: nothing looks at the file
 * system. The functions that only cut a path up return views into the one you
 * gave them and never allocate. The ones that take an allocator hand back
 * their input when it is already what they would have built, so the result is
 * borrowed from both the allocator and the input. A failed allocation gives
 * the empty string.
 *
 * Go's Abs, EvalSymlinks, Glob, Walk and WalkDir need os, and come with it.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package path/filepath */

#ifndef BURROW_PATH_FILEPATH_H
#define BURROW_PATH_FILEPATH_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/mem.h"
#include "burrow/platform.h"
#include "burrow/slice.h"

#ifdef __cplusplus
extern "C" {
#endif

/* filepath.Separator and filepath.ListSeparator. */
#if defined(BURROW_OS_WINDOWS)
#define FILEPATH_SEPARATOR '\\'
#define FILEPATH_LIST_SEPARATOR ';'
#else
#define FILEPATH_SEPARATOR '/'
#define FILEPATH_LIST_SEPARATOR ':'
#endif

/* filepath.ErrBadPattern, from filepath_match for a malformed pattern. It is
 * its own error, as in Go, and not path_err_bad_pattern. */
extern const Error filepath_err_bad_pattern;

/* os.IsPathSeparator: c == '/' everywhere, and c == '\\' as well on Windows. */
bool filepath_is_path_separator(Byte c);

/* The shortest path that names the same thing, by lexical processing alone:
 * runs of separators become one, "." elements go, each ".." goes along with
 * the element before it, and ".." at the start of a rooted path goes. The
 * result ends in a separator only if it is the root, and uses the platform's
 * separator. The empty path becomes ".".
 *
 * On Windows the volume name is kept as it is, apart from slashes turned into
 * backslashes, and a path is never cleaned into one that means something
 * else: "a/../c:" becomes ".\c:" and not "c:". */
BURROW_OWNS(ret) BURROW_BORROWS(ret, path) Str filepath_clean(Alloc *a, Str path);

/* Whether path stays inside the directory it is evaluated in: it is not
 * empty, not absolute, and does not climb out with "..". On Windows it also
 * has no colon, does not start with a separator, and names no reserved device
 * such as NUL or COM1. Purely lexical, so a symlink can still lead out. */
bool filepath_is_local(Str path);

/* A slash separated path, the kind fs_valid_path accepts, turned into one for
 * this operating system. Fails with an error when the path is not valid or
 * cannot be said here: a NUL byte anywhere, and on Windows a colon, a
 * backslash or a reserved device name. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, path) Str filepath_localize(Alloc *a, Str path,
                                                                 Error *err);

/* path with every separator replaced by a slash, and the reverse. Both return
 * path itself where the separator is already a slash. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, path) Str filepath_to_slash(Alloc *a, Str path);
BURROW_OWNS(ret) BURROW_BORROWS(ret, path) Str filepath_from_slash(Alloc *a, Str path);

/* A list joined by FILEPATH_LIST_SEPARATOR, the way PATH is, cut into a Slice
 * of Str. The empty string gives an empty Slice and not one empty element. On
 * Windows a separator inside double quotes does not count, and the quotes are
 * then removed. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, path) Slice filepath_split_list(Alloc *a,
                                                                     Str path);

/* path cut after its last separator, so that dir is empty or ends in one and
 * file has none. On Windows the volume name stays with dir. dir + file is
 * always path. */
BURROW_BORROWS(ret, path) Str filepath_split(Str path, Str *file);

/* The elements of elem, a Slice of Str, joined with the separator and then
 * cleaned. Empty elements are ignored, and if they all are the result is the
 * empty string. On Windows a first element of "C:" keeps the result relative
 * to that drive, so "C:" and "f" give "C:f". */
BURROW_OWNS(ret) Str filepath_join(Alloc *a, Slice elem);

/* filepath_join without a Slice. Every argument after n must be a Str. */
BURROW_OWNS(ret) Str filepath_join_v(Alloc *a, int n, ...);

/* The extension of the last element, from its last dot, or empty. */
BURROW_BORROWS(ret, path) Str filepath_ext(Str path);

/* The last element, with trailing separators removed first. The empty path
 * gives "." and a path of only separators gives a single separator. */
BURROW_BORROWS(ret, path) Str filepath_base(Str path);

/* All but the last element, cleaned. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, path) Str filepath_dir(Alloc *a, Str path);

/* Whether path is absolute. On Windows that needs a volume name and a root,
 * as in C:\x or \\host\share\x, so \x and C:x are not. */
bool filepath_is_abs(Str path);

/* The leading volume name on Windows, with slashes turned into backslashes:
 * "C:" for "C:\foo", "\\host\share" for "//host/share/foo". It is empty
 * everywhere else. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, path) Str filepath_volume_name(Alloc *a, Str path);

/* A relative path that names targ when joined onto base, so that
 * filepath_join(base, rel) is the same as targ once both are cleaned. Fails
 * when there is none: one is absolute and the other is not, they are on
 * different volumes, or base climbs out with ".." where targ does not follow.
 * Windows compares without regard to case. */
BURROW_OWNS(ret) Str filepath_rel(Alloc *a, Str base, Str targ, Error *err);

/* Whether name matches the shell pattern: '*' is any run of non-separator
 * bytes, '?' is one non-separator character, and [abc], [a-z] and [^a-z] are
 * classes. A backslash quotes the character after it, except on Windows,
 * where it is the separator and stands for itself. The whole of name has to
 * match.
 *
 * A malformed pattern gives false and sets *err to filepath_err_bad_pattern,
 * and err may be NULL. Go's filepath, unlike its path, reports that only when
 * it reaches the bad part, so a pattern that fails to match before then
 * reports no error, and the same is true here. */
bool filepath_match(Str pattern, Str name, Error *err);

/* filepath.HasPrefix, which Go keeps only for old code: a byte prefix test
 * that ignores path boundaries, and on Windows compares again in lower case
 * if the first test fails. Deprecated, as it is in Go. */
bool filepath_has_prefix(Str p, Str prefix);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_PATH_FILEPATH_H */

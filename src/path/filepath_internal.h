/* path/filepath for either kind of system, picked by an argument.
 *
 * Go builds one of path_unix.go and path_windows.go, and so its Windows rules
 * are only ever tested on Windows. Here both are always built, and the public
 * functions pass the host's choice, so that tests/filepath_test.c can run Go's
 * Windows tables on every machine and its Unix tables on Windows. win is true
 * for the Windows rules. Each function is the public one of the same name.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SRC_PATH_FILEPATH_INTERNAL_H
#define BURROW_SRC_PATH_FILEPATH_INTERNAL_H

#include "burrow/path/filepath.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/mem.h"
#include "burrow/slice.h"

/* filepathlite's errInvalidPath, which Localize returns and Go does not
 * export. */
extern const Error burrow__filepath_err_invalid_path;

BURROW_OWNS(ret) BURROW_BORROWS(ret, path) Str burrow__filepath_clean(Alloc *a,
                                                                      Str path,
                                                                      bool win);
bool burrow__filepath_is_local(Str path, bool win);
BURROW_OWNS(ret) BURROW_BORROWS(ret, path) Str burrow__filepath_localize(Alloc *a,
                                                                         Str path,
                                                                         bool win,
                                                                         Error *err);
BURROW_OWNS(ret) BURROW_BORROWS(ret, path) Str burrow__filepath_to_slash(Alloc *a,
                                                                         Str path,
                                                                         bool win);
BURROW_OWNS(ret) BURROW_BORROWS(ret, path) Str burrow__filepath_from_slash(Alloc *a,
                                                                           Str path,
                                                                           bool win);
BURROW_OWNS(ret) BURROW_BORROWS(ret, path) Slice burrow__filepath_split_list(Alloc *a,
                                                                             Str path,
                                                                             bool win);
BURROW_BORROWS(ret, path) Str burrow__filepath_split(Str path, Str *file, bool win);
BURROW_OWNS(ret) Str burrow__filepath_join(Alloc *a, Slice elem, bool win);
BURROW_BORROWS(ret, path) Str burrow__filepath_ext(Str path, bool win);
BURROW_BORROWS(ret, path) Str burrow__filepath_base(Str path, bool win);
BURROW_OWNS(ret) BURROW_BORROWS(ret, path) Str burrow__filepath_dir(Alloc *a, Str path,
                                                                    bool win);
bool burrow__filepath_is_abs(Str path, bool win);
BURROW_OWNS(ret) BURROW_BORROWS(ret, path) Str burrow__filepath_volume_name(Alloc *a,
                                                                            Str path,
                                                                            bool win);
BURROW_OWNS(ret) Str burrow__filepath_rel(Alloc *a, Str base, Str targ, bool win,
                                          Error *err);
bool burrow__filepath_match(Str pattern, Str name, bool win, Error *err);
bool burrow__filepath_has_prefix(Str p, Str prefix, bool win);

#endif /* BURROW_SRC_PATH_FILEPATH_INTERNAL_H */

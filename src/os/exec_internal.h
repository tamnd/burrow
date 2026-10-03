/* What src/os/cmd.c and src/os/lookpath.c share.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_OS_EXEC_INTERNAL_H
#define BURROW_OS_EXEC_INTERNAL_H

#include "burrow/os/exec.h"

/* An ExecError for name and err, from error_allocator. */
Error burrow__exec_error(Str name, Error err);

/* lookExtensions: on Windows, path with the extension from %PATHEXT% that
 * Start would run it with, looking in dir when path is relative. From a. It
 * answers path as it is everywhere else. */
Str burrow__exec_look_extensions(Alloc *a, Str path, Str dir, Error *err);

#endif /* BURROW_OS_EXEC_INTERNAL_H */

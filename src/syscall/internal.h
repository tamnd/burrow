/* What the syscall sources share and the public header does not show.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SRC_SYSCALL_INTERNAL_H
#define BURROW_SRC_SYSCALL_INTERNAL_H

#include "burrow/syscall.h"

#include "burrow/core.h"

/* Go's errors table for this system, from src/syscall/zerrors.c. Entry i is
 * the text for the Errno burrow__syscall_errors_base + i, and NULL where Go
 * has none. The base is 0 except on Windows, where the table only covers the
 * numbers Go made up, from APPLICATION_ERROR on. */
extern const Uintptr burrow__syscall_errors_base;
extern const char *const burrow__syscall_errors[];
extern const Int burrow__syscall_nerrors;

#endif /* BURROW_SRC_SYSCALL_INTERNAL_H */

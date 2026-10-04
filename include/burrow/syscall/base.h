/* The two types of syscall's own that the generated headers use, Errno and
 * Signal. syscall.h includes this, and they are here rather than there because
 * the amalgamation puts a header after everything it includes, so zsyscall.h
 * would come before anything syscall.h defines itself.
 *
 * Derived from Go's src/syscall/syscall_unix.go and syscall_windows.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SYSCALL_BASE_H
#define BURROW_SYSCALL_BASE_H

#include "burrow/core.h"

/* syscall.Errno. 0 means no error. */
typedef Uintptr SyscallErrno;

/* syscall.Signal: a signal number, the system's own, so SYSCALL_SIGUSR1 is 10
 * on Linux and 30 on macOS. Windows has no signals, and its SIG constants are
 * the numbers Go gives them there. */
typedef Int SyscallSignal;

#endif

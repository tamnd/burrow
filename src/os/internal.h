/* What the os sources share and the public header does not show.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SRC_OS_INTERNAL_H
#define BURROW_SRC_OS_INTERNAL_H

#include "burrow/os.h"

#include "burrow/fdmutex.h"
#include "burrow/pal.h"
#include "burrow/syscall.h"

/* os.File, which in Go is a *file holding a poll.FD. The fields of poll.FD
 * that matter without a poller are here: the descriptor, its fdMutex, and the
 * semaphore Close waits on until the last user has let go. */
struct OsFile {
    burrow__FdMutex mu;
    uint32_t csema;
    int64_t fd;
    Str name;
    Alloc *a;
    void *dirinfo; /* an OsDirInfo once the file is read as a directory */
    bool append_mode;
    bool stdio; /* one of the three os_stdin and the others hand out */
};

/* poll.FD's incref and decref. The decref that lets go of the last reference
 * of a closed file closes the descriptor, and gives the close error. */
bool burrow__os_incref(OsFile *f);
Error burrow__os_decref(OsFile *f);

/* poll.ErrFileClosing, which os mostly turns into os_err_closed. */
extern const Error burrow__os_err_file_closing;

/* The directory state of f, if it has any, back to the start, as Go's seek
 * does, and freed, as Go's close does. */
void burrow__os_dirinfo_reset(OsFile *f);
void burrow__os_dirinfo_free(OsFile *f);

/* A Str as a NUL terminated path for the PAL, on the stack when it fits and
 * from the heap when it does not. */
#define OS_CPATH_SMALL 512

typedef struct OsCPath {
    char *p;
    Int cap;
    char small[OS_CPATH_SMALL];
} OsCPath;

/* Fills c with s. False with *err set to EINVAL when s has a NUL in it, which
 * is what Go's syscall.BytePtrFromString says, or to out of memory. */
bool burrow__os_cpath(OsCPath *c, Str s, Error *err);
void burrow__os_cpath_free(OsCPath *c);

/* The SyscallErrno for a PAL failure as an Error, straight after the failed
 * call. A failure the PAL has no number for keeps its PAL text. */
Error burrow__os_errno(PalErrno e);

/* The Error for one SyscallErrno, from error_allocator. */
Error burrow__os_errno_value(SyscallErrno e);

/* x, y and z one after the other, from a, or an empty Str when a refuses. */
Str burrow__os_cat3(Alloc *a, Str x, Str y, Str z);

/* A PalStat mode as a FileMode. */
FsFileMode burrow__os_mode_of(uint32_t m);

/* The FileInfo for a PalStat, with the last element of name as its name. */
OsFileInfo burrow__os_file_info(Alloc *a, Str name, const PalStat *st, Error *err);

#endif /* BURROW_SRC_OS_INTERNAL_H */

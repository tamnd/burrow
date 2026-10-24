/* io/ioutil, some I/O utility functions.
 *
 *     Slice data = ioutil_read_file(a, BURROW_S("notes.txt"), &err);
 *     Slice infos = ioutil_read_dir(a, BURROW_S("."), &err);
 *
 * Deprecated: As of Go 1.16, the same functionality is now provided by
 * package io or package os, and those implementations should be preferred in
 * new code. Every function here but ioutil_read_dir is the io or os function
 * it names under another name, and is kept so that code ported from Go that
 * still calls ioutil has something to call.
 *
 * What a function gives back is allocated from the Alloc it was handed, the
 * same as the function it calls.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package io/ioutil */

#ifndef BURROW_IO_IOUTIL_H
#define BURROW_IO_IOUTIL_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/os.h"
#include "burrow/slice.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ioutil.ReadAll: reads from r until an error or the end and returns the data
 * it read. The end is not an error, so a nil error means all of it arrived.
 *
 * Deprecated: As of Go 1.16, this function simply calls io_read_all. */
BURROW_OWNS(ret) Slice ioutil_read_all(Alloc *a, IoReader r, Error *err);

/* ioutil.ReadFile: the whole of the file named by filename. Reaching the end
 * is not an error.
 *
 * Deprecated: As of Go 1.16, this function simply calls os_read_file. */
BURROW_OWNS(ret) Slice ioutil_read_file(Alloc *a, Str filename, Error *err);

/* ioutil.WriteFile: data as the whole of filename. If the file does not exist
 * it is created with perm, less the umask. Otherwise it is truncated before
 * the write, and its permissions stay as they were.
 *
 * Deprecated: As of Go 1.16, this function simply calls os_write_file. */
BURROW_STATIC(ret) Error ioutil_write_file(Str filename, Slice data, OsFileMode perm);

/* ioutil.ReadDir: the directory named by dirname as a Slice of OsFileInfo
 * from a, sorted by filename. If an error occurs reading the directory, it
 * returns no directory entries along with the error.
 *
 * Deprecated: As of Go 1.16, os_read_dir is a more efficient and correct
 * choice: it returns a Slice of FsDirEntry instead of FsFileInfo, and it
 * returns partial results in the case of an error midway through reading a
 * directory. */
BURROW_OWNS(ret) Slice ioutil_read_dir(Alloc *a, Str dirname, Error *err);

/* ioutil.NopCloser: a ReadCloser with a no-op Close wrapping r. Like
 * io_nop_closer it gives the value, and io_nop_closer_as_io_read_closer makes
 * the ReadCloser from its address.
 *
 * Deprecated: As of Go 1.16, this function simply calls io_nop_closer. */
IoNopCloser ioutil_nop_closer(IoReader r);

/* ioutil.Discard: an IoWriter on which all writes succeed without doing
 * anything.
 *
 * Deprecated: As of Go 1.16, this value is simply io_discard. */
#define ioutil_discard io_discard

/* ioutil.TempFile: a new temporary file in dir, or os_temp_dir when dir is
 * empty, open for reading and writing. Its name is pattern with a random
 * string put in place of the last "*", or added at the end when there is no
 * "*". Programs calling it at the same time never get the same file.
 * os_file_name says what it is called, and removing it when it is no longer
 * needed is up to the caller.
 *
 * Deprecated: As of Go 1.17, this function simply calls os_create_temp. */
BURROW_OWNS(ret) OsFile *ioutil_temp_file(Alloc *a, Str dir, Str pattern, Error *err);

/* ioutil.TempDir: a new temporary directory in dir, or os_temp_dir when dir
 * is empty, named the way ioutil_temp_file names a file, and its name from a.
 * Programs calling it at the same time never get the same directory, and
 * removing it when it is no longer needed is up to the caller.
 *
 * Deprecated: As of Go 1.17, this function simply calls os_mkdir_temp. */
BURROW_OWNS(ret) Str ioutil_temp_dir(Alloc *a, Str dir, Str pattern, Error *err);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_IO_IOUTIL_H */

/* debug/buildinfo, the build information Go writes into its binaries.
 *
 * Every Go binary from Go 1.13 on carries the Go release that built it and,
 * when it was built with modules, the module information that runtime/debug
 * describes: the main package, its module, the modules it depends on and the
 * settings of the build. This finds that record in an ELF, PE, Mach-O, XCOFF
 * or Plan 9 binary, whichever operating system and architecture it is for, in
 * both the layout of Go 1.18 and later and the older one:
 *
 *     Arena ar;
 *     arena_init(&ar, NULL, 0);
 *     Alloc *a = arena_allocator(&ar);
 *     Error err = BURROW_NO_ERROR;
 *     Buildinfo *bi = buildinfo_read_file(a, BURROW_S("/usr/local/bin/gopls"), &err);
 *     if (bi == NULL)
 *         ...
 *     fmt_printf_v("%s built with %s\n", bi->path, bi->go_version);
 *     debug_build_info_free(bi);
 *     arena_free(&ar);
 *
 * A file that is not a binary of any of those kinds gives "unrecognized file
 * format", and one that is but has no Go build information in it, such as a C
 * program, gives "not a Go executable".
 *
 * Copyright 2021 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package debug/buildinfo */

#ifndef BURROW_DEBUG_BUILDINFO_H
#define BURROW_DEBUG_BUILDINFO_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/runtime/debug.h"

#ifdef __cplusplus
extern "C" {
#endif

/* buildinfo.BuildInfo, which is runtime/debug's. */
typedef DebugBuildInfo Buildinfo;

/* buildinfo.ReadFile: the build information of the Go binary at name. The
 * error says which file it was about, as "could not read Go build info from
 * NAME: not a Go executable", or, when the file could not be opened or read,
 * wraps the FsPathError, which names it already. debug_build_info_free gives
 * the result back. */
BURROW_OWNS(ret) DebugBuildInfo *buildinfo_read_file(Alloc *a, Str name, Error *err);

/* buildinfo.Read: the build information of the Go binary r reads, from
 * offset 0. r is only read during the call. */
BURROW_OWNS(ret) DebugBuildInfo *buildinfo_read(Alloc *a, IoReaderAt r, Error *err);

#ifdef __cplusplus
}
#endif

#endif

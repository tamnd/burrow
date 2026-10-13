/* runtime/debug, the build information half of it.
 *
 * A Go binary records how it was built: the Go release, the main package and
 * its module, every module it depends on, and the settings of the build, such
 * as the target and whether cgo was on. debug/buildinfo reads that record out
 * of a binary and hands it back as a DebugBuildInfo. The record itself is
 * text, one line per item, and debug_parse_build_info and
 * debug_build_info_string turn it into the struct and back:
 *
 *     Arena ar;
 *     arena_init(&ar, NULL, 0);
 *     Alloc *a = arena_allocator(&ar);
 *     Error err = BURROW_NO_ERROR;
 *     DebugBuildInfo *bi = debug_parse_build_info(a, text, &err);
 *     if (bi == NULL)
 *         ...
 *     for (Int i = 0; i < bi->deps.len; i++) {
 *         DebugModule *m = ((DebugModule **)bi->deps.p)[i];
 *         ...
 *     }
 *     Str again = debug_build_info_string(bi, a);
 *     debug_build_info_free(bi);
 *     arena_free(&ar);
 *
 * A parsed DebugBuildInfo keeps its strings, modules and settings in an arena
 * of its own on the allocator it was made with, and debug_build_info_free
 * gives all of it back. One you fill in yourself, with the fields after
 * settings left zero, works with debug_build_info_string the same, and is
 * yours to free.
 *
 * The rest of runtime/debug is about Go's collector, stacks and threads, and
 * is not here yet.
 *
 * Copyright 2018 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package runtime/debug */

#ifndef BURROW_RUNTIME_DEBUG_H
#define BURROW_RUNTIME_DEBUG_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* debug.Module, one module of a build. replace is the module it was replaced
 * with, or NULL. */
typedef struct DebugModule {
    Str path;
    Str version;
    Str sum;
    struct DebugModule *replace;
} DebugModule;

/* debug.BuildSetting, one setting that influenced a build, such as
 * "-compiler" and "gc", or "GOOS" and "linux". */
typedef struct DebugBuildSetting {
    Str key;
    Str value;
} DebugBuildSetting;
extern const Type burrow_type_DebugBuildSetting;
#define TYPE_DEBUG_BUILD_SETTING TYPE_OF(DebugBuildSetting)

/* debug.BuildInfo. go_version is the release that built the binary, such as
 * "go1.27.1", path the package path of the main package, and main its module.
 * deps holds a DebugModule * for each module the build used, and settings a
 * DebugBuildSetting for each setting. The rest is the BuildInfo's. */
typedef struct DebugBuildInfo {
    Str go_version;
    Str path;
    DebugModule main;
    Slice deps;
    Slice settings;

    Alloc *a;
    Alloc *parent;
    Arena arena;
} DebugBuildInfo;

/* debug.ReadBuildInfo: the build information of the running binary. Go's
 * linker writes it into every binary built with modules. A C program has none,
 * so this gives NULL and false, which is what Go gives for a binary built
 * without module support. */
BURROW_OWNS(ret) DebugBuildInfo *debug_read_build_info(Alloc *a, bool *ok);

/* debug.ParseBuildInfo: the DebugBuildInfo that data, the text
 * debug_build_info_string makes, describes, with go_version left empty, as Go
 * leaves it. Lines it does not know are skipped, and so is a last line with no
 * newline. A malformed line gives NULL and an error such as "could not parse Go
 * build info: line 3: expected 2 or 3 columns; got 1". */
BURROW_OWNS(ret) DebugBuildInfo *debug_parse_build_info(Alloc *a, Str data, Error *err);

/* BuildInfo.String: bi as text, made in a. */
BURROW_OWNS(ret) Str debug_build_info_string(const DebugBuildInfo *bi, Alloc *a);

/* Gives back a DebugBuildInfo that debug_parse_build_info or debug/buildinfo
 * made, and everything in it. NULL is fine. */
void debug_build_info_free(DebugBuildInfo *bi);

#ifdef __cplusplus
}
#endif

#endif

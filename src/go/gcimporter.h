/* go/internal/gcimporter and internal/exportdata, which go/importer's "gc"
 * importer and the tests share.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_GO_GCIMPORTER_H
#define BURROW_GO_GCIMPORTER_H

#include "burrow/bufio.h"
#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/go/importer.h"
#include "burrow/go/token.h"
#include "burrow/go/types.h"
#include "burrow/map.h"
#include "burrow/own.h"

/* ReadUnified reads the contents of the unified export data from a reader r
 * that contains the contents of a GC-created archive file.
 *
 * On success, the reader will be positioned after the end-of-section marker
 * "\n$$\n".
 *
 * Supported GC-created archive files have 4 layers of nesting:
 *   - An archive file containing a package definition file.
 *   - The package definition file contains headers followed by a data section.
 *     Headers are lines (at most 4kb) that do not start with "$$".
 *   - The data section starts with "$$B\n" followed by export data followed
 *     by an end of section marker "\n$$\n". (The section start "$$\n" is no
 *     longer supported.)
 *   - The export data starts with a format byte ('u') followed by the <data>
 *     in the given format. (See ReadExportDataHeader for older formats.) */
BURROW_OWNS(ret) Slice burrow__exportdata_read_unified(Alloc *a, BufioReader *r,
                                                       Error *err);

/* FindPackageDefinition positions the reader r at the beginning of a package
 * definition file ("__.PKGDEF") within a GC-created archive by reading from
 * it, and returns the size of the package definition file in the archive.
 *
 * The reader must be positioned at the start of the archive file before
 * calling this function, and "__.PKGDEF" is assumed to be the first file in
 * the archive.
 *
 * See cmd/internal/archive for details on the archive format. */
Int burrow__exportdata_find_package_definition(BufioReader *r, Error *err);

/* ReadObjectHeaders reads object headers from the reader. Object headers are
 * lines that do not start with an end-of-section marker "$$". The first header
 * is the objabi header. On success, the reader will be positioned at the
 * beginning of the end-of-section marker.
 *
 * It returns an error if any header does not fit in r.Size() bytes.
 *
 * Not in Go: *headers is a Slice of Str. */
BURROW_OWNS(ret) Str burrow__exportdata_read_object_headers(Alloc *a, BufioReader *r,
                                                            Slice *headers, Error *err);

/* ReadExportDataHeader reads the export data header and format from r. It
 * returns the number of bytes read, or an error if the format is no longer
 * supported or it failed to read.
 *
 * The only currently supported format is binary export data in the unified
 * export format. */
Int burrow__exportdata_read_export_data_header(BufioReader *r, Error *err);

/* FindPkg returns the filename and unique package id for an import path based
 * on package information provided by build.Import (using the build.Default
 * build.Context). A relative srcDir is interpreted relative to the current
 * working directory.
 *
 * Not in Go: exports is where the answers of go list are kept, by package
 * directory, which is a package variable there. It may be NULL, and then
 * nothing is kept. Make it with TYPE_STRING keys and TYPE_UNSAFE_POINTER
 * values. */
BURROW_OWNS(ret) Str burrow__exportdata_find_pkg(Alloc *a, Str path, Str src_dir,
                                                 Map *exports, Str *id, Error *err);

/* Import imports a gc-generated package given its import path and srcDir, adds
 * the corresponding package object to the packages map, and returns the object.
 * The packages map must contain all packages already imported.
 *
 * Not in Go: packages is a map of Str to TypesPackage *, made with TYPE_STRING
 * keys and TYPE_UNSAFE_POINTER values, and exports is the cache that
 * burrow__exportdata_find_pkg takes. */
TypesPackage *burrow__gcimporter_import(Alloc *a, TokenFileSet *fset, Map *packages,
                                        Str path, Str src_dir, ImporterLookup lookup,
                                        Map *exports, Error *err);

#endif /* BURROW_GO_GCIMPORTER_H */

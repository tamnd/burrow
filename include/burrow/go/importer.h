/* go/importer, access to export data importers.
 *
 * Go's go/importer. An importer is what TypesConfig.importer needs to check a
 * package that imports others. importer_for_compiler with "source" gives one
 * that finds each imported package's source with go/build, the way
 * build_default sees the Go trees, and type checks it:
 *
 *     TokenFileSet *fset = token_new_file_set(a);
 *     TypesConfig conf = {0};
 *     conf.importer = importer_for_compiler(a, fset, BURROW_S("source"),
 *                                           (ImporterLookup){0});
 *
 * The packages it checks are kept, so each is checked once however many
 * imports name it. That makes the first import of something like fmt slow,
 * since everything fmt imports is checked as well, and later ones free. The
 * standard library's packages are found under $GOROOT/src, so GOROOT has to be
 * set, as build_default explains.
 *
 * "gc" reads the export data the compiler wrote instead of checking the
 * source. It needs the go command to say where that data is, unless a lookup
 * is given, as importer_for_compiler explains.
 *
 * Go marks most of this package deprecated, because it predates modules.
 * It is still the simplest way to get the standard library's types, and that
 * is what it is here for.
 *
 * Memory. Everything the importer makes, the files it parses and the packages
 * it checks, comes from a, so it lives as long as a does.
 *
 * Copyright 2015 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package go/importer */

#ifndef BURROW_GO_IMPORTER_H
#define BURROW_GO_IMPORTER_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/func.h"
#include "burrow/go/token.h"
#include "burrow/go/types.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/own.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Lookup: a reader for the package data of an import path, or an error if no
 * matching package is found. */
BURROW_FUNC(ImporterLookup, IoReadCloser, Alloc *a, Str path, Error *err);

/* ForCompiler: an importer for the compiler named. "gc" imports from the
 * export data the gc compiler writes, and "source" imports directly from the
 * source. The source importer may fail for a package whose exported API is not
 * entirely defined in pure Go source code, since the type checker cannot see
 * what cgo defines. It finds packages itself, so lookup has to be nil for it,
 * and a lookup with "source" panics, as in Go.
 *
 * With "gc", lookup is called each time the importer needs to resolve an
 * import path, and gives a reader for the archive the compiler wrote for it.
 * The importer can then only be used with canonical import paths, not
 * relative or absolute ones, since turning paths into canonical ones is left
 * to the caller. Go deprecates a nil lookup but still allows it, and so does
 * this: the importer then asks go/build where the package is and runs
 * "go list -export" to find the export data of a standard library package,
 * which needs the go command under GOROOT/bin. Export data that is not well
 * formed panics, as in Go.
 *
 * Go also has "gccgo", which reads the export data of gccgo. It is not here
 * yet, and for it, as for any compiler that is not supported, the result is
 * the zero TypesImporter, Go's nil. */
BURROW_OWNS(ret) TypesImporter importer_for_compiler(Alloc *a, TokenFileSet *fset,
                                                     Str compiler,
                                                     ImporterLookup lookup);

/* For: importer_for_compiler with a new FileSet. Deprecated in Go, since
 * positions in what it imports cannot be looked up. */
BURROW_OWNS(ret) TypesImporter importer_for(Alloc *a, Str compiler,
                                            ImporterLookup lookup);

/* Default: an importer for the compiler that built the running binary, which
 * is For(runtime.Compiler, nil) in Go. A program using burrow was not built by
 * a Go compiler, so this is importer_for with "gc" and no lookup, the importer
 * for the export data of the go command's own compiler. As with For, positions
 * in what it imports cannot be looked up, and importer_for_compiler with a
 * FileSet of the caller's is better for anything but the simplest cases. */
BURROW_OWNS(ret) TypesImporter importer_default(Alloc *a);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_GO_IMPORTER_H */

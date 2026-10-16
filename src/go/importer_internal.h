/* go/internal/srcimporter, which go/importer and the tests share.
 *
 * Copyright 2017 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_GO_IMPORTER_INTERNAL_H
#define BURROW_GO_IMPORTER_INTERNAL_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/go/build.h"
#include "burrow/go/token.h"
#include "burrow/go/types.h"
#include "burrow/map.h"

/* An Importer provides the context for importing packages from source code.
 *
 * Not in Go: a is where the importer, the files it parses and the packages it
 * checks come from, and importing is the sentinel, here one for each importer
 * instead of a package variable. */
typedef struct burrow__SrcImporter {
    BuildContext *ctxt;
    TokenFileSet *fset;
    TypesSizes sizes;
    Map *packages; /* Str to TypesPackage * */
    Alloc *a;
    char importing;
} burrow__SrcImporter;

/* New returns a new Importer for the given context, file set, and map of
 * packages. The context is used to resolve import paths to package paths, and
 * identifying the files belonging to the package. If the context provides
 * non-nil file system functions, they are used instead of the regular package
 * os functions. The file set is used to track position information of package
 * files; and imported packages are added to the packages map.
 *
 * packages is made with TYPE_STRING keys and TYPE_UNSAFE_POINTER values. */
BURROW_OWNS(ret) burrow__SrcImporter *burrow__srcimporter_new(Alloc *a,
                                                              BuildContext *ctxt,
                                                              TokenFileSet *fset,
                                                              Map *packages);

/* Import(path) is a shortcut for ImportFrom(path, ".", 0). */
TypesPackage *burrow__srcimporter_import(burrow__SrcImporter *p, Str path, Error *err);

/* ImportFrom imports the package with the given import path resolved from the
 * given srcDir, adds the new package to the set of packages maintained by the
 * importer, and returns the package. The import mode must be zero but is
 * otherwise ignored. */
TypesPackage *burrow__srcimporter_import_from(burrow__SrcImporter *p, Str path,
                                              Str src_dir, TypesImportMode mode,
                                              Error *err);

/* The importer as a types.Importer, which is also a types.ImporterFrom. */
TypesImporter burrow__srcimporter_importer(burrow__SrcImporter *p);

#endif /* BURROW_GO_IMPORTER_INTERNAL_H */

/* go/importer: access to export data importers.
 *
 * Derived from Go's src/go/importer/importer.go.
 * Go source: go1.27.1.
 *
 * Copyright 2015 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/go/importer.h"

#include "gcimporter.h"
#include "importer_internal.h"

#include "burrow/core.h"
#include "burrow/func.h"
#include "burrow/go/build.h"
#include "burrow/go/token.h"
#include "burrow/go/types.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/runtime.h"
#include "burrow/type.h"

#include <stddef.h>

/* gc importer */

typedef struct GiGcImports {
    TokenFileSet *fset;
    Map *packages; /* Str to TypesPackage * */
    ImporterLookup lookup;
    Map *exports; /* Not in Go: what go list said, by package directory */
    Alloc *a;
} GiGcImports;

static TypesPackage *gi_gc_import_from(void *self, Str path, Str src_dir,
                                       TypesImportMode mode, Error *err) {
    GiGcImports *m = self;
    if (mode != 0)
        runtime_panic(BURROW_S("mode must be 0"));
    return burrow__gcimporter_import(m->a, m->fset, m->packages, path, src_dir,
                                     m->lookup, m->exports, err);
}

static TypesPackage *gi_gc_import(void *self, Str path, Error *err) {
    return gi_gc_import_from(self, path, BURROW_S("") /* no vendoring */, 0, err);
}

static const TypesImporterVT gi_gc_vt = {NULL, gi_gc_import, gi_gc_import_from};

TypesImporter importer_for_compiler(Alloc *a, TokenFileSet *fset, Str compiler,
                                    ImporterLookup lookup) {
    if (str_eq(compiler, BURROW_S("gc"))) {
        GiGcImports *m = BURROW_NEW(a, GiGcImports);
        if (m == NULL)
            return (TypesImporter){0};
        m->fset = fset;
        m->packages = map_make(a, TYPE_STRING, TYPE_UNSAFE_POINTER, 0);
        m->lookup = lookup;
        m->exports = map_make(a, TYPE_STRING, TYPE_UNSAFE_POINTER, 0);
        m->a = a;
        if (m->packages == NULL || m->exports == NULL)
            return (TypesImporter){0};
        return (TypesImporter){&gi_gc_vt, m};
    }
    if (str_eq(compiler, BURROW_S("source"))) {
        if (!BURROW_FUNC_IS_NIL(lookup))
            runtime_panic(BURROW_S("source importer for custom import path lookup not "
                                   "supported (issue #13847)."));
        /* Not in Go: build.Default is a package variable there and is
         * computed here, into a context of the importer's own. */
        BuildContext *ctxt = BURROW_NEW(a, BuildContext);
        Map *packages = map_make(a, TYPE_STRING, TYPE_UNSAFE_POINTER, 0);
        if (ctxt == NULL || packages == NULL)
            return (TypesImporter){0};
        *ctxt = build_default(a);
        burrow__SrcImporter *p = burrow__srcimporter_new(a, ctxt, fset, packages);
        if (p == NULL)
            return (TypesImporter){0};
        return burrow__srcimporter_importer(p);
    }
    /* compiler not supported */
    return (TypesImporter){0};
}

TypesImporter importer_for(Alloc *a, Str compiler, ImporterLookup lookup) {
    return importer_for_compiler(a, token_new_file_set(a), compiler, lookup);
}

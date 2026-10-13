/* go/build internals shared by build.c, build_read.c and the tests.
 *
 * Copyright 2012 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_GO_BUILD_INTERNAL_H
#define BURROW_GO_BUILD_INTERNAL_H

#include "burrow/go/ast.h"
#include "burrow/go/build.h"
#include "burrow/go/token.h"
#include "burrow/io.h"

/* fileImport, one import of a file. */
typedef struct burrow__BuildFileImport {
    Str path;
    TokenPos pos;
    AstCommentGroup *doc; /* or NULL */
} burrow__BuildFileImport;

/* fileEmbed, one //go:embed pattern. */
typedef struct burrow__BuildFileEmbed {
    Str pattern;
    TokenPosition pos;
} burrow__BuildFileEmbed;

/* fileInfo, what was learned about a file of a package. */
typedef struct burrow__BuildFileInfo {
    Str name;           /* the full name, directory included */
    Str header;         /* the file up to the end of the imports */
    TokenFileSet *fset; /* NULL for nothing parsed */
    AstFile *parsed;    /* or NULL */
    Error parse_err;    /* why parsed is partial or missing */
    Slice imports;      /* of burrow__BuildFileImport */
    Slice embeds;       /* of burrow__BuildFileEmbed */
    Slice directives;   /* of BuildDirective */
} burrow__BuildFileInfo;

/* readGoInfo: reads a Go file from f up to the end of its imports into
 * info->header, and with info->fset set parses that and fills in the rest.
 * The error is only for a failed read, never for bad syntax. */
Error burrow__build_read_go_info(Alloc *a, IoReader f, burrow__BuildFileInfo *info);

/* readComments: the leading run of comments and space in f. */
Str burrow__build_read_comments(Alloc *a, IoReader f, Error *err);

/* What the tests reach for: ctxt.matchAuto, ctxt.shouldBuild,
 * ctxt.goodOSArchFile, expandSrcDir, ctxt.importGo and errNoModules. A NULL
 * all_tags is Go's nil map. */
bool burrow__build_match_auto(BuildContext *ctxt, Alloc *a, Str text, Map *all_tags);
bool burrow__build_should_build(BuildContext *ctxt, Alloc *a, Str content,
                                Map *all_tags, bool *binary_only, Error *err);
bool burrow__build_good_os_arch_file(BuildContext *ctxt, Alloc *a, Str name,
                                     Map *all_tags);
Str burrow__build_expand_src_dir(Alloc *a, Str str, Str srcdir, bool *ok);
Error burrow__build_import_go(BuildContext *ctxt, Alloc *a, BuildPackage *p, Str path,
                              Str src_dir, BuildImportMode mode);
Error burrow__build_err_no_modules(void);

#endif /* BURROW_GO_BUILD_INTERNAL_H */

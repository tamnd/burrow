/* The importer the go/types tests share, in place of Go's defaultImporter
 * from api_test.go.
 *
 * Not in Go: Go's defaultImporter is importer.ForCompiler(fset,
 * runtime.Compiler, nil), which reads the export data the gc compiler wrote
 * for the installed packages. burrow has no gc importer yet, so this is the
 * source importer instead, the one Go's stdlib_test.go uses. It needs Go's
 * sources, from $GOROOT or from what go env says, and without them only
 * unsafe can be imported.
 *
 * There is one importer for the whole test program, and it has a file set
 * of its own. Go makes one for each test, which with the gc importer is
 * cheap, but the source importer would check fmt and everything it imports
 * again for every test that imports fmt. The tests run one after another,
 * so sharing it is safe.
 *
 * Copyright 2013 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef GO_TYPES_IMPORTER_H
#define GO_TYPES_IMPORTER_H

#include "../src/go/importer_internal.h"
#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/go/build.h"
#include "burrow/go/token.h"
#include "burrow/go/types.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/os.h"
#include "burrow/os/exec.h"
#include "burrow/path/filepath.h"
#include "burrow/strings.h"
#include "burrow/type.h"

#include <stdbool.h>
#include <stddef.h>

static Arena gtt_arena;
static bool gtt_ready;
static bool gtt_source;
static TypesImporter gtt_importer;

/* What testenv.GOROOT finds: $GOROOT, or else what go env says, or "" when
 * there is no go command to ask. */
static Str gtt_find_goroot(Alloc *a) {
    Str goroot = os_getenv(a, BURROW_S("GOROOT"));
    if (goroot.len > 0)
        return goroot;
    Error e = BURROW_NO_ERROR;
    Str go = exec_look_path(a, BURROW_S("go"), &e);
    if (BURROW_FAILED(e))
        return BURROW_STR_EMPTY;
    ExecCmd *cmd = exec_command_v(a, go, 2, BURROW_S("env"), BURROW_S("GOROOT"));
    if (cmd == NULL)
        return BURROW_STR_EMPTY;
    Slice out = exec_cmd_output(cmd, a, &e);
    if (BURROW_OK(e) && out.len > 0)
        goroot = str_clone(
            a, strings_trim_space(str_from_bytes((const Byte *)out.p, out.len)));
    exec_cmd_free(cmd);
    return goroot;
}

/* The importer without Go's sources, which knows unsafe and nothing else. */
static TypesPackage *gtt_unsafe_import(void *self, Str path, Error *err) {
    if (str_eq(path, BURROW_S("unsafe")))
        return types_unsafe();
    *err = errors_new((Alloc *)self,
                      fmt_sprintf_v((Alloc *)self, "can't find import: %q", path));
    return NULL;
}

static const TypesImporterVT gtt_unsafe_vt = {NULL, gtt_unsafe_import, NULL};

static void gtt_init(void) {
    if (gtt_ready)
        return;
    gtt_ready = true;
    arena_init(&gtt_arena, NULL, 0);
    Alloc *a = arena_allocator(&gtt_arena);
    gtt_importer = (TypesImporter){&gtt_unsafe_vt, a};
    Str goroot = gtt_find_goroot(a);
    if (goroot.len == 0)
        return;
    Error e = BURROW_NO_ERROR;
    (void)os_stat(a, filepath_join_v(a, 3, goroot, BURROW_S("src"), BURROW_S("go")),
                  &e);
    if (BURROW_FAILED(e))
        return;
    BuildContext *ctxt = BURROW_NEW(a, BuildContext);
    Map *packages = map_make(a, TYPE_STRING, TYPE_UNSAFE_POINTER, 0);
    if (ctxt == NULL || packages == NULL)
        return;
    *ctxt = build_default(a);
    ctxt->goroot = goroot;
    burrow__SrcImporter *p =
        burrow__srcimporter_new(a, ctxt, token_new_file_set(a), packages);
    if (p == NULL)
        return;
    gtt_importer = burrow__srcimporter_importer(p);
    gtt_source = true;
}

/* defaultImporter: the shared importer. The fset is not used, since the
 * importer has its own. */
static inline TypesImporter default_importer(TokenFileSet *fset) {
    (void)fset;
    gtt_init();
    return gtt_importer;
}

/* Not in Go: whether default_importer can import more than unsafe. */
static inline bool have_source_importer(void) {
    gtt_init();
    return gtt_source;
}

#endif /* GO_TYPES_IMPORTER_H */

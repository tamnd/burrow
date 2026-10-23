/* go/internal/srcimporter: importing directly from source files rather than
 * installed packages.
 *
 * Derived from Go's src/go/internal/srcimporter/srcimporter.go.
 * Go source: go1.27.1.
 *
 * Go parses the files of a package on goroutines of their own. Here they are
 * parsed one after another, since they all take from the importer's allocator.
 *
 * Copyright 2017 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "importer_internal.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/go/ast.h"
#include "burrow/go/build.h"
#include "burrow/go/parser.h"
#include "burrow/go/token.h"
#include "burrow/go/types.h"
#include "burrow/iface.h"
#include "burrow/io.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/os.h"
#include "burrow/os/exec.h"
#include "burrow/path/filepath.h"
#include "burrow/runtime.h"
#include "burrow/slice.h"
#include "burrow/strings.h"

#include <stdbool.h>
#include <stddef.h>

#define GSI_S(lit) BURROW_S(lit)

burrow__SrcImporter *burrow__srcimporter_new(Alloc *a, BuildContext *ctxt,
                                             TokenFileSet *fset, Map *packages) {
    burrow__SrcImporter *p = BURROW_NEW(a, burrow__SrcImporter);
    if (p == NULL)
        return NULL;
    p->ctxt = ctxt;
    p->fset = fset;
    /* uses go/types default if GOARCH not found */
    p->sizes = types_sizes_for(ctxt->compiler, ctxt->goarch);
    p->packages = packages;
    p->a = a;
    return p;
}

TypesPackage *burrow__srcimporter_import(burrow__SrcImporter *p, Str path, Error *err) {
    /* use "." rather than "" (see issue #24441) */
    return burrow__srcimporter_import_from(p, path, GSI_S("."), 0, err);
}

/* context-controlled file system operations */

static Str gsi_abs_path(burrow__SrcImporter *p, Str path, Error *err) {
    /* TODO(gri) This should be using p.ctxt.AbsPath which doesn't exist but
     * probably should. See also issue #14282. */
    return filepath_abs(p->a, path, err);
}

static Str gsi_join_path(burrow__SrcImporter *p, Str dir, Str name) {
    Str elem[2] = {dir, name};
    Slice s = {elem, 2, 2, TYPE_STRING};
    if (!BURROW_FUNC_IS_NIL(p->ctxt->join_path))
        return BURROW_CALLF(p->ctxt->join_path, p->a, s);
    return filepath_join(p->a, s);
}

static void gsi_push(Alloc *a, Slice *s, Str x) {
    *s = slice_append(a, *s, &x, 1);
}

static void gsi_push_all(Alloc *a, Slice *s, Slice xs) {
    if (xs.len > 0)
        *s = slice_append(a, *s, xs.p, xs.len);
}

/* One file, through the context's OpenFile if there is one. */
static AstFile *gsi_parse_file(burrow__SrcImporter *p, Str filename, Error *err) {
    Alloc *a = p->a;
    Error e = BURROW_NO_ERROR;
    AstFile *f = NULL;
    if (!BURROW_FUNC_IS_NIL(p->ctxt->open_file)) {
        IoReadCloser src = BURROW_CALLF(p->ctxt->open_file, a, filename, &e);
        if (BURROW_FAILED(e)) {
            *err = e; /* open provides operation and filename in error */
            return NULL;
        }
        IoReader r = io_read_closer_as_io_reader(src);
        f = parser_parse_file(a, p->fset, filename,
                              BURROW_ANY(&burrow_type_IoReader, &r),
                              PARSER_SKIP_OBJECT_RESOLUTION, err);
        /* ignore Close error - parsing may have succeeded which is all we need */
        (void)src.vt->closer.close(src.data);
        return f;
    }
    OsFile *src = os_open(a, filename, &e);
    if (BURROW_FAILED(e)) {
        *err = e;
        return NULL;
    }
    IoReader r = os_file_as_io_reader(src);
    f = parser_parse_file(a, p->fset, filename, BURROW_ANY(&burrow_type_IoReader, &r),
                          PARSER_SKIP_OBJECT_RESOLUTION, err);
    (void)os_file_close(src);
    os_file_free(src);
    return f;
}

/* parseFiles. It gives back a Slice of AstFile *. */
static Slice gsi_parse_files(burrow__SrcImporter *p, Str dir, Slice filenames,
                             Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    Slice files = slice_nil(TYPE_AST_FILE_PTR);
    for (Int i = 0; i < filenames.len; i++) {
        Str name = gsi_join_path(p, dir, ((Str *)filenames.p)[i]);
        Error e = BURROW_NO_ERROR;
        AstFile *f = gsi_parse_file(p, name, &e);
        /* if there are errors, return the first one for deterministic results */
        if (BURROW_FAILED(e)) {
            *err = e;
            return slice_nil(TYPE_AST_FILE_PTR);
        }
        files = slice_append(p->a, files, &f, 1);
    }
    return files;
}

static AstFile *gsi_run_cgo(burrow__SrcImporter *p, BuildPackage *bp, Str tmpdir,
                            Error *err) {
    Alloc *a = p->a;
    Str go_cmd = GSI_S("go");
    if (p->ctxt->goroot.len > 0)
        go_cmd = filepath_join_v(a, 3, p->ctxt->goroot, GSI_S("bin"), GSI_S("go"));
    /* Not in Go: args leaves out go_cmd, which exec_command takes on its own. */
    Slice args = slice_nil(TYPE_STRING);
    gsi_push(a, &args, GSI_S("tool"));
    gsi_push(a, &args, GSI_S("cgo"));
    gsi_push(a, &args, GSI_S("-objdir"));
    gsi_push(a, &args, tmpdir);
    if (bp->goroot) {
        if (str_eq(bp->import_path, GSI_S("runtime/cgo"))) {
            gsi_push(a, &args, GSI_S("-import_runtime_cgo=false"));
            gsi_push(a, &args, GSI_S("-import_syscall=false"));
        } else if (str_eq(bp->import_path, GSI_S("runtime/race"))) {
            gsi_push(a, &args, GSI_S("-import_syscall=false"));
        }
    }
    gsi_push(a, &args, GSI_S("--"));
    gsi_push_all(a, &args, strings_fields(a, os_getenv(a, GSI_S("CGO_CPPFLAGS"))));
    gsi_push_all(a, &args, bp->cgo_cppflags);
    if (bp->cgo_pkg_config.len > 0) {
        Slice pargs = slice_nil(TYPE_STRING);
        gsi_push(a, &pargs, GSI_S("--cflags"));
        gsi_push_all(a, &pargs, bp->cgo_pkg_config);
        ExecCmd *cmd = exec_command(a, GSI_S("pkg-config"), pargs);
        if (cmd == NULL) {
            *err = errors_new(error_allocator(), GSI_S("out of memory"));
            return NULL;
        }
        Error e = BURROW_NO_ERROR;
        Slice out = exec_cmd_output(cmd, a, &e);
        exec_cmd_free(cmd);
        if (BURROW_FAILED(e)) {
            *err = fmt_errorf_v("pkg-config --cflags: %w", e);
            return NULL;
        }
        gsi_push_all(a, &args,
                     strings_fields(a, str_from_bytes((const Byte *)out.p, out.len)));
    }
    gsi_push(a, &args, GSI_S("-I"));
    gsi_push(a, &args, tmpdir);
    gsi_push_all(a, &args, strings_fields(a, os_getenv(a, GSI_S("CGO_CFLAGS"))));
    gsi_push_all(a, &args, bp->cgo_cflags);
    gsi_push_all(a, &args, bp->cgo_files);
    ExecCmd *cmd = exec_command(a, go_cmd, args);
    if (cmd == NULL) {
        *err = errors_new(error_allocator(), GSI_S("out of memory"));
        return NULL;
    }
    cmd->dir = bp->dir;
    Error e = exec_cmd_run(cmd);
    exec_cmd_free(cmd);
    if (BURROW_FAILED(e)) {
        *err = fmt_errorf_v("go tool cgo: %w", e);
        return NULL;
    }
    return parser_parse_file(
        a, p->fset, filepath_join_v(a, 2, tmpdir, GSI_S("_cgo_gotypes.go")),
        BURROW_ANY(NULL, NULL), PARSER_SKIP_OBJECT_RESOLUTION, err);
}

static AstFile *gsi_cgo(burrow__SrcImporter *p, BuildPackage *bp, Error *err) {
    Error e = BURROW_NO_ERROR;
    Str tmpdir = os_mkdir_temp(p->a, BURROW_STR_EMPTY, GSI_S("srcimporter"), &e);
    if (BURROW_FAILED(e)) {
        *err = e;
        return NULL;
    }
    AstFile *f = gsi_run_cgo(p, bp, tmpdir, err);
    (void)os_remove_all(tmpdir);
    return f;
}

/* What the Config.Error function keeps. */
typedef struct GsiErrors {
    bool hard;
    TypesError first_hard_err;
} GsiErrors;

/* continue type-checking after the first error */
static void gsi_on_error(void *env, const TypesError *e) {
    GsiErrors *errs = env;
    if (!errs->hard && !e->soft) {
        errs->hard = true;
        errs->first_hard_err = *e;
    }
}

/* ImportFrom, from the point where the package is marked as being imported.
 * The caller does what Go's deferred function does. */
static TypesPackage *gsi_import(burrow__SrcImporter *p, BuildPackage *bp, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    Alloc *a = p->a;
    Slice filenames = slice_nil(TYPE_STRING);
    gsi_push_all(a, &filenames, bp->go_files);
    gsi_push_all(a, &filenames, bp->cgo_files);
    Error e = BURROW_NO_ERROR;
    Slice files = gsi_parse_files(p, bp->dir, filenames, &e);
    if (BURROW_FAILED(e)) {
        *err = e;
        return NULL;
    }

    /* type-check package files */
    GsiErrors errs = {0};
    TypesConfig conf = {0};
    conf.ignore_func_bodies = true;
    conf.error = (TypesErrorFunc){gsi_on_error, &errs};
    conf.importer = burrow__srcimporter_importer(p);
    conf.sizes = p->sizes;
    if (bp->cgo_files.len > 0) {
        if (!BURROW_FUNC_IS_NIL(p->ctxt->open_file)) {
            /* cgo, gcc, pkg-config, etc. do not support build.Context's VFS. */
            conf.fake_import_c = true;
        } else {
            conf.go115_uses_cgo = true;
            AstFile *file = gsi_cgo(p, bp, &e);
            if (BURROW_FAILED(e)) {
                *err = fmt_errorf_v("error processing cgo for package %q: %w",
                                    bp->import_path, e);
                return NULL;
            }
            files = slice_append(a, files, &file, 1);
        }
    }
    TypesPackage *pkg =
        types_config_check(&conf, a, bp->import_path, p->fset, files, NULL, &e);
    if (BURROW_FAILED(e)) {
        /* If there was a hard error it is possibly unsafe to use the package as
         * it may not be fully populated. Do not return it (see also #20837,
         * #20855). */
        if (errs.hard) {
            pkg = NULL;
            /* give preference to first hard error over any soft error */
            e = types_error_as_error(&errs.first_hard_err, a);
        }
        *err = fmt_errorf_v("type-checking package %q failed (%v)", bp->import_path, e);
        return pkg;
    }
    if (errs.hard) {
        /* this can only happen if we have a bug in go/types */
        runtime_panic(GSI_S("package is not safe yet no error was returned"));
    }

    map_set(p->packages, &bp->import_path, &pkg);
    return pkg;
}

TypesPackage *burrow__srcimporter_import_from(burrow__SrcImporter *p, Str path,
                                              Str src_dir, TypesImportMode mode,
                                              Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (mode != 0)
        runtime_panic(GSI_S("non-zero import mode"));

    Error e = BURROW_NO_ERROR;
    Str abs = gsi_abs_path(p, src_dir, &e);
    if (BURROW_OK(e)) /* see issue #14282 */
        src_dir = abs;
    e = BURROW_NO_ERROR;
    BuildPackage *bp = build_context_import(p->ctxt, p->a, path, src_dir, 0, &e);
    if (BURROW_FAILED(e)) {
        *err = e; /* err may be *build.NoGoError - return as is */
        return NULL;
    }

    /* package unsafe is known to the type checker */
    if (str_eq(bp->import_path, GSI_S("unsafe")))
        return types_unsafe();

    /* Not in Go: the import path can be the caller's path, which may not live
     * as long as the importer, and it is kept as the key and the package's
     * path. */
    bp->import_path = str_clone(p->a, bp->import_path);

    /* no need to re-import if the package was imported completely before */
    void *importing = &p->importing;
    TypesPackage **found = map_get(p->packages, &bp->import_path);
    TypesPackage *pkg = found != NULL ? *found : NULL;
    if (pkg != NULL) {
        if ((void *)pkg == importing) {
            *err = fmt_errorf_v("import cycle through package %q", bp->import_path);
            return NULL;
        }
        if (!types_package_complete(pkg)) {
            /* Package exists but is not complete - we cannot handle this at the
             * moment since the source importer replaces the package wholesale
             * rather than augmenting it (see #19337 for details). Return
             * incomplete package with error (see #16088). */
            *err = fmt_errorf_v("reimported partially imported package %q",
                                bp->import_path);
            return pkg;
        }
        return pkg;
    }

    map_set(p->packages, &bp->import_path, &importing);
    pkg = gsi_import(p, bp, err);
    /* clean up in case of error
     * TODO(gri) Eventually we may want to leave a (possibly empty) package in
     * the map in all cases (and use that package to identify cycles). See also
     * issue 16088. */
    found = map_get(p->packages, &bp->import_path);
    if (found != NULL && (void *)*found == importing) {
        TypesPackage *none = NULL;
        map_set(p->packages, &bp->import_path, &none);
    }
    return pkg;
}

static TypesPackage *gsi_vt_import(void *self, Str path, Error *err) {
    return burrow__srcimporter_import(self, path, err);
}

static TypesPackage *gsi_vt_import_from(void *self, Str path, Str dir,
                                        TypesImportMode mode, Error *err) {
    return burrow__srcimporter_import_from(self, path, dir, mode, err);
}

static const TypesImporterVT gsi_vt = {NULL, gsi_vt_import, gsi_vt_import_from};

TypesImporter burrow__srcimporter_importer(burrow__SrcImporter *p) {
    return (TypesImporter){&gsi_vt, p};
}

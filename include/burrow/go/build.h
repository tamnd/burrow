/* go/build, finding Go packages and the files that make them up.
 *
 * Go's go/build. A BuildContext says which system the build is for and where
 * the Go trees are, and build_context_import finds the package an import path
 * names, reads the top of each of its files and sorts them by what they are
 * and whether they would be built:
 *
 *     BuildContext ctxt = build_default(a);
 *     Error err = BURROW_NO_ERROR;
 *     BuildPackage *p = build_context_import_dir(&ctxt, a, BURROW_S("."), 0, &err);
 *     if (BURROW_OK(err)) {
 *         // p->name, p->go_files, p->imports ...
 *     }
 *
 * Files are chosen as the go command chooses them, by their _GOOS and _GOARCH
 * suffixes and their //go:build lines (and // +build lines in files without
 * one), against the context's tags. Only the package clause and the imports
 * of a .go file are parsed.
 *
 * In module mode, which is any import path that is not local, with GOROOT set
 * and no hooks or custom tags in the context, Go asks the go command where the
 * package is with go list. So does this, which means GOROOT has to name a Go
 * installation with a bin/go in it. Without one the lookup falls back to GOROOT
 * and GOPATH, just as Go's does when GO111MODULE=off.
 *
 * Memory. A package, its strings, its maps and the errors in it are made in
 * the allocator passed in, with nothing that frees one part alone, and an
 * import also leaves the scratch it needed for stats and reads there. Use an
 * arena and free it when done. Error values come from error_allocator, as
 * everywhere else.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package go/build */

#ifndef BURROW_GO_BUILD_H
#define BURROW_GO_BUILD_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/func.h"
#include "burrow/go/token.h"
#include "burrow/io.h"
#include "burrow/io/fs.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/own.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------------- context */

/* The file system hooks of a BuildContext. A nil one means the local file
 * system, through path/filepath and os. Each gets the allocator its caller
 * was given, for whatever it returns. */

/* JoinPath: elem, a Slice of Str, joined into one path. */
BURROW_FUNC(BuildJoinPathFunc, Str, Alloc *a, Slice elem);
/* SplitPathList: a list of paths, like GOPATH, as a Slice of Str. */
BURROW_FUNC(BuildSplitPathListFunc, Slice, Alloc *a, Str list);
/* IsAbsPath: whether path is absolute. */
BURROW_FUNC(BuildIsAbsPathFunc, bool, Str path);
/* IsDir: whether path names a directory. */
BURROW_FUNC(BuildIsDirFunc, bool, Str path);
/* HasSubdir: whether dir is inside root, with *ok set to that and the
 * slash separated path of dir below root returned when it is. */
BURROW_FUNC(BuildHasSubdirFunc, Str, Alloc *a, Str root, Str dir, bool *ok);
/* ReadDir: the entries of dir as a Slice of FsFileInfo, sorted by name. */
BURROW_FUNC(BuildReadDirFunc, Slice, Alloc *a, Str dir, Error *err);
/* OpenFile: path opened for reading. The caller closes it. */
BURROW_FUNC(BuildOpenFileFunc, IoReadCloser, Alloc *a, Str path, Error *err);

/* build.Context, the support code for a build. */
typedef struct BuildContext {
    Str goarch; /* the target architecture */
    Str goos;   /* the target operating system */
    Str goroot; /* the Go root */
    Str gopath; /* the Go paths, a list */

    /* The directory the go command runs in, for module mode. Empty means the
     * current one. */
    Str dir;

    bool cgo_enabled;   /* whether cgo files are included */
    bool use_all_files; /* use files whatever their build lines and names */
    Str compiler;       /* the compiler to assume, "gc" or "gccgo" */

    /* Tags that //go:build lines are matched against, each a Slice of Str.
     * build_tags is for the user, tool_tags is what the toolchain sets, and
     * release_tags is go1.1 up to the current release. */
    Slice build_tags;
    Slice tool_tags;
    Slice release_tags;

    /* Added to the package install directory, as with -installsuffix. */
    Str install_suffix;

    BuildJoinPathFunc join_path;
    BuildSplitPathListFunc split_path_list;
    BuildIsAbsPathFunc is_abs_path;
    BuildIsDirFunc is_dir;
    BuildHasSubdirFunc has_subdir;
    BuildReadDirFunc read_dir;
    BuildOpenFileFunc open_file;
} BuildContext;

/* Default, the context for the system the program runs on, set from GOARCH,
 * GOOS, GOROOT, GOPATH and CGO_ENABLED in the environment, with the tool tags
 * that GOEXPERIMENT and the GOAMD64 style variables give. Go computes it once
 * at start up, and this computes it on each call. There is no Go installation
 * to take a default GOROOT from, so goroot is empty when GOROOT is not set,
 * and compiler is "gc". */
BURROW_OWNS(ret) BuildContext build_default(Alloc *a);

/* ToolDir, the directory with the build tools in it: $GOROOT/pkg/tool/
 * GOOS_GOARCH for the system the program runs on. */
BURROW_OWNS(ret) Str build_tool_dir(Alloc *a);

/* Context.SrcDirs: the src directories of GOROOT and each GOPATH entry that
 * exist, as a Slice of Str. */
BURROW_OWNS(ret) Slice build_context_src_dirs(BuildContext *ctxt, Alloc *a);

/* ----------------------------------------------------------------- package */

/* build.ImportMode, the flags of an import. */
typedef Uint BuildImportMode;

enum {
    /* Stop once the directory is found, without reading it. */
    BUILD_FIND_ONLY = 1 << 0,
    /* Accept a package found only as a compiled object. Deprecated in Go. */
    BUILD_ALLOW_BINARY = 1 << 1,
    /* Read the import comments on the package clauses, as in
     * package foo // import "example.com/foo". */
    BUILD_IMPORT_COMMENT = 1 << 2,
    /* Do not look in vendor directories. */
    BUILD_IGNORE_VENDOR = 1 << 3
};

/* build.Directive, a //go: line comment before a package clause. */
typedef struct BuildDirective {
    Str text;          /* the whole comment, slashes included */
    TokenPosition pos; /* where it is */
} BuildDirective;

extern const Type *const TYPE_BUILD_DIRECTIVE;

/* build.Package, what an import found. Every Slice of files holds the names of
 * the files as Strs, without the directory. The position maps take a Str
 * import path or embed pattern to a Slice of TokenPosition. */
typedef struct BuildPackage {
    Str dir;             /* the directory with the sources */
    Str name;            /* the package name */
    Str import_comment;  /* the path in an import comment on the package clause */
    Str doc;             /* the synopsis of the package documentation */
    Str import_path;     /* the import path, or "" if not known */
    Str root;            /* the root of the Go tree the package is in */
    Str src_root;        /* the package source root, or "" */
    Str pkg_root;        /* the package install root, or "" */
    Str pkg_target_root; /* the install root for the target, or "" */
    Str bin_dir;         /* the command install directory, or "" */
    bool goroot;         /* found in the Go root */
    Str pkg_obj;         /* the installed .a file */
    Slice all_tags;      /* the tags that can change which files are used */
    Str conflict_dir;    /* the directory in GOPATH this one hides */
    bool binary_only;    /* has a //go:binary-only-package comment */

    Slice go_files;            /* .go files, apart from the four lists below */
    Slice cgo_files;           /* .go files that import "C" */
    Slice ignored_go_files;    /* .go files left out of this build */
    Slice invalid_go_files;    /* .go files with problems */
    Slice ignored_other_files; /* other source files left out of this build */
    Slice c_files;             /* .c files */
    Slice cxx_files;           /* .cc, .cpp and .cxx files */
    Slice m_files;             /* .m files */
    Slice h_files;             /* .h, .hh, .hpp and .hxx files */
    Slice f_files;             /* .f, .F, .for and .f90 files */
    Slice s_files;             /* .s files */
    Slice swig_files;          /* .swig files */
    Slice swig_cxx_files;      /* .swigcxx files */
    Slice syso_files;          /* .syso object files */

    /* The #cgo directives, each a Slice of Str. */
    Slice cgo_cflags;
    Slice cgo_cppflags;
    Slice cgo_cxxflags;
    Slice cgo_fflags;
    Slice cgo_ldflags;
    Slice cgo_pkg_config;

    Slice test_go_files;   /* _test.go files in the package */
    Slice x_test_go_files; /* _test.go files in package name_test */

    /* Slices of BuildDirective from the three kinds of file. */
    Slice directives;
    Slice test_directives;
    Slice x_test_directives;

    Slice imports;      /* sorted import paths from go_files and cgo_files */
    Map *import_pos;    /* where each of imports is imported */
    Slice test_imports; /* from test_go_files */
    Map *test_import_pos;
    Slice x_test_imports; /* from x_test_go_files */
    Map *x_test_import_pos;

    Slice embed_patterns; /* sorted //go:embed patterns from go_files and cgo_files */
    Map *embed_pattern_pos;
    Slice test_embed_patterns;
    Map *test_embed_pattern_pos;
    Slice x_test_embed_patterns;
    Map *x_test_embed_pattern_pos;
} BuildPackage;

/* Package.IsCommand: whether the package is a command, package main. */
bool build_package_is_command(const BuildPackage *p);

/* ------------------------------------------------------------------ errors */

/* build.NoGoError: a directory with no buildable Go files in it, although it
 * may have test or ignored files. errors_as with TYPE_BUILD_NO_GO_ERROR gives
 * a pointer to the one in an error from an import. */
typedef struct BuildNoGoError {
    Str dir;
} BuildNoGoError;

extern const Type *const TYPE_BUILD_NO_GO_ERROR;

/* NoGoError.Error: "no buildable Go source files in " and the directory. */
BURROW_OWNS(ret) Str build_no_go_error_error(const BuildNoGoError *e, Alloc *a);

/* build.MultiplePackageError: Go files from more than one package in one
 * directory. packages and files are Slices of Str, and file i declares
 * package i. */
typedef struct BuildMultiplePackageError {
    Str dir;
    Slice packages;
    Slice files;
} BuildMultiplePackageError;

extern const Type *const TYPE_BUILD_MULTIPLE_PACKAGE_ERROR;

/* MultiplePackageError.Error: "found packages a (a.go) and b (b.go) in dir". */
BURROW_OWNS(ret) Str
build_multiple_package_error_error(const BuildMultiplePackageError *e, Alloc *a);

/* ---------------------------------------------------------------- imports */

/* Context.Import: the package path names, looked for from the directory
 * src_dir, which is only needed for a local path like "./x" and for vendor
 * directories.
 *
 * A package is always returned, with what was found filled in, even with an
 * error. The errors are a NoGoError when no Go files are buildable, a
 * MultiplePackageError for Go files from different packages, and errors that
 * say why a file or a directory could not be used. Files with problems are
 * listed in invalid_go_files and the first problem is the error. */
BURROW_OWNS(ret) BuildPackage *build_context_import(BuildContext *ctxt, Alloc *a,
                                                    Str path, Str src_dir,
                                                    BuildImportMode mode, Error *err);

/* Context.ImportDir: the package in the directory dir. */
BURROW_OWNS(ret) BuildPackage *build_context_import_dir(BuildContext *ctxt, Alloc *a,
                                                        Str dir, BuildImportMode mode,
                                                        Error *err);

/* Import and ImportDir, which use build_default. */
BURROW_OWNS(ret) BuildPackage *build_import(Alloc *a, Str path, Str src_dir,
                                            BuildImportMode mode, Error *err);
BURROW_OWNS(ret) BuildPackage *build_import_dir(Alloc *a, Str dir, BuildImportMode mode,
                                                Error *err);

/* Context.MatchFile: whether the file name in dir would be part of the
 * package an import of dir gives. It reads the top of the file through the
 * context's OpenFile hook to see its build lines. */
bool build_context_match_file(BuildContext *ctxt, Alloc *a, Str dir, Str name,
                              Error *err);

/* IsLocalImport: whether path is a local import path, ".", "..", or one that
 * starts with "./" or "../". */
bool build_is_local_import(Str path);

/* ArchChar: "?" and an error, as in Go since 1.5, where the letters are no
 * longer used. */
BURROW_STATIC(ret) Str build_arch_char(Str goarch, Error *err);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_GO_BUILD_H */

/* gofmt: the part of Go's cmd/gofmt that formats files, on top of go/parser,
 * go/ast and go/printer, for the gate in tools/check-gofmt.sh.
 *
 * Derived from Go's src/cmd/gofmt/gofmt.go.
 * Go source: go1.27.1.
 *
 *     gofmt [-l] [path ...]
 *
 * Each path is a file, formatted whatever its name, or a directory, walked for
 * the files whose names end in ".go" and do not start with a dot. With -l the
 * names of the files whose formatting differs are printed, and without it the
 * formatted source. Errors go to standard error the way scanner.PrintError
 * prints them, and the exit code is 2 after one. Standard input, -w, -d, -s,
 * -r and -e are left out, since the gate does not need them.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/burrow.h"
#include "burrow/mem/arena.h"

#include <stdbool.h>
#include <string.h>

#define S(lit) BURROW_S(lit)

/* Keep these in sync with go/format, as Go's comment says. */
enum { GOFMT_TAB_WIDTH = 8 };
#define GOFMT_PRINTER_MODE                                                             \
    (PRINTER_USE_SPACES | PRINTER_TAB_INDENT | BURROW__PRINTER_NORMALIZE_NUMBERS)
#define GOFMT_PARSER_MODE (PARSER_PARSE_COMMENTS | PARSER_SKIP_OBJECT_RESOLUTION)

typedef struct Gofmt {
    bool list;
    int exit_code;
    IoWriter out;
    IoWriter err;
    Str arg; /* the argument being walked */
} Gofmt;

/* reporter.Report. */
static void gofmt_report(Gofmt *g, Error err) {
    go_scanner_print_error(g->err, err);
    g->exit_code = 2;
}

/* processFile, for a file and not standard input, so no fragments. */
static Error gofmt_process_file(Gofmt *g, Str filename) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    Error err = BURROW_NO_ERROR;
    Slice src = os_read_file(a, filename, &err);
    if (BURROW_FAILED(err)) {
        gofmt_report(g, err);
        arena_free(&ar);
        return BURROW_NO_ERROR;
    }

    TokenFileSet *fset = token_new_file_set(a);
    AstFile *file = parser_parse_file(a, fset, filename, BURROW_ANY(TYPE_BYTES, &src),
                                      GOFMT_PARSER_MODE, &err);
    if (BURROW_FAILED(err)) {
        gofmt_report(g, err);
        arena_free(&ar);
        return BURROW_NO_ERROR;
    }

    ast_sort_imports(a, fset, file);

    PrinterConfig cfg = {GOFMT_PRINTER_MODE, GOFMT_TAB_WIDTH, 0};
    BytesBuffer buf = BYTES_BUFFER(a);
    err = printer_config_fprint(&cfg, a, bytes_buffer_as_io_writer(&buf), fset,
                                BURROW_ANY(TYPE_AST_FILE_PTR, &file));
    if (BURROW_FAILED(err)) {
        gofmt_report(g, err);
        arena_free(&ar);
        return BURROW_NO_ERROR;
    }
    Slice res = bytes_buffer_bytes(&buf);

    bool same = src.len == res.len &&
                (res.len == 0 || memcmp(src.p, res.p, (size_t)res.len) == 0);
    if (!same && g->list)
        (void)fmt_fprintf_v(g->out, "%s\n", filename); /* formatting has changed */
    if (!g->list)
        (void)io_write_string(g->out, str_from_bytes(res.p, res.len), &err);
    if (BURROW_FAILED(err))
        gofmt_report(g, err);
    arena_free(&ar);
    return BURROW_NO_ERROR;
}

static bool gofmt_is_go_filename(Str name) {
    return !strings_has_prefix(name, S(".")) && strings_has_suffix(name, S(".go"));
}

/* The function gofmtMain hands filepath.WalkDir. */
static Error gofmt_visit(void *env, Str path, FsDirEntry d, Error err) {
    Gofmt *g = (Gofmt *)env;
    if (BURROW_FAILED(err))
        return err;
    if (d.vt->is_dir(d.data))
        return BURROW_NO_ERROR; /* simply recurse into directories */
    /* non-directories given as explicit arguments are always formatted */
    if (!str_eq(path, g->arg) && !gofmt_is_go_filename(d.vt->name(d.data)))
        return BURROW_NO_ERROR; /* skip walked non-Go files */
    return gofmt_process_file(g, path);
}

int main(int argc, char **argv) {
    Gofmt g = {false, 0, os_file_as_io_writer(os_stdout),
               os_file_as_io_writer(os_stderr), BURROW_STR_EMPTY};
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (strcmp(argv[i], "-l") == 0) {
            g.list = true;
        } else if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        } else {
            (void)fmt_fprintf_v(g.err, "usage: gofmt [-l] [path ...]\n");
            return 2;
        }
    }
    if (i == argc) {
        (void)fmt_fprintf_v(g.err, "gofmt: standard input is not supported here\n");
        return 2;
    }

    Arena ar;
    arena_init(&ar, NULL, 0);
    for (; i < argc; i++) {
        /* Walk each argument as a directory tree. A non-directory is always
         * formatted as a Go file, and in a directory non-Go files are left. */
        g.arg = str_from_cstr(argv[i]);
        Error err = filepath_walk_dir(arena_allocator(&ar), g.arg,
                                      BURROW_FN(FsWalkDirFunc, gofmt_visit, &g));
        if (BURROW_FAILED(err))
            gofmt_report(&g, err);
    }
    arena_free(&ar);
    return g.exit_code;
}

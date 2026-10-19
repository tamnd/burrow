/* internal/exportdata: reading the export data in the archives the gc
 * compiler writes, and finding them.
 *
 * Derived from Go's src/internal/exportdata/exportdata.go and support.go.
 * Go source: go1.27.1.
 *
 * Copyright 2024 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "gcimporter.h"

#include "../debug/objfile_internal.h"

#include "burrow/bufio.h"
#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/go/build.h"
#include "burrow/io.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/os.h"
#include "burrow/os/exec.h"
#include "burrow/path/filepath.h"
#include "burrow/slice.h"
#include "burrow/strconv.h"
#include "burrow/strings.h"
#include "burrow/type.h"

#include <stdbool.h>
#include <stdint.h>

#define GED_S(lit) BURROW_S(lit)

static Str ged_str(Slice b) {
    return str_from_bytes(b.p, b.len);
}

Slice burrow__exportdata_read_unified(Alloc *a, BufioReader *r0, Error *err) {
    Slice data = slice_nil(TYPE_BYTE);
    *err = BURROW_NO_ERROR;

    /* We historically guaranteed headers at the default buffer size (4096)
     * work. This ensures we can use ReadSlice throughout. */
    const Int min_buffer_size = 4096;
    BufioReader *r =
        bufio_new_reader_size(a, bufio_reader_as_io_reader(r0), min_buffer_size);
    if (r == NULL) {
        *err = errors_new(a, GED_S("out of memory"));
        return data;
    }

    Int size = burrow__exportdata_find_package_definition(r, err);
    if (BURROW_FAILED(*err))
        goto done;
    Int n = size;

    Slice headers = slice_nil(TYPE_STRING);
    Str objapi = burrow__exportdata_read_object_headers(a, r, &headers, err);
    if (BURROW_FAILED(*err))
        goto done;
    n -= objapi.len;
    for (Int i = 0; i < headers.len; i++)
        n -= BURROW_AT(Str, headers, i).len;

    Int hdrlen = burrow__exportdata_read_export_data_header(r, err);
    if (BURROW_FAILED(*err))
        goto done;
    n -= hdrlen;

    /* size also includes the end of section marker. Remove that many bytes from
     * the end. */
    const Str marker = GED_S("\n$$\n");
    n -= marker.len;

    if (n < 0) {
        *err = fmt_errorf_v("invalid size (%d) in the archive file: %d bytes remain "
                            "without section headers (recompile package)",
                            size, n);
        goto done;
    }

    /* Read n bytes from buf. */
    data = burrow__saferio_read_data(a, bufio_reader_as_io_reader(r), (uint64_t)n, err);
    if (BURROW_FAILED(*err))
        goto done;

    /* Check for marker at the end. */
    Byte suffix[4];
    io_read_full(bufio_reader_as_io_reader(r), (Slice){suffix, 4, 4, TYPE_BYTE}, err);
    if (BURROW_FAILED(*err))
        goto done;
    Str s = str_from_bytes(suffix, 4);
    if (!str_eq(s, marker)) {
        *err = fmt_errorf_v("read %q instead of end-of-section marker (%q)", s, marker);
        goto done;
    }

done:
    bufio_reader_free(r);
    return data;
}

/* Copy of cmd/internal/archive.ReadHeader. */
static Int ged_read_archive_header(BufioReader *b, Str name) {
    /* architecture-independent object file output */
    enum { header_size = 60 };

    Byte buf[header_size];
    Error err = BURROW_NO_ERROR;
    io_read_full(bufio_reader_as_io_reader(b),
                 (Slice){buf, header_size, header_size, TYPE_BYTE}, &err);
    if (BURROW_FAILED(err))
        return -1;
    Str aname = strings_trim(str_from_bytes(buf, 16), GED_S(" "));
    if (!strings_has_prefix(aname, name))
        return -1;
    Str asize = strings_trim(str_from_bytes(buf + 48, 10), GED_S(" "));
    Int i = strconv_atoi(asize, &err);
    if (BURROW_FAILED(err))
        i = 0;
    return i;
}

Int burrow__exportdata_find_package_definition(BufioReader *r, Error *err) {
    *err = BURROW_NO_ERROR;

    /* Uses ReadSlice to limit risk of malformed inputs. */

    /* Read first line to make sure this is an object file. */
    Slice line = bufio_reader_read_slice(r, '\n', err);
    if (BURROW_FAILED(*err)) {
        *err = fmt_errorf_v("can't find export data (%v)", *err);
        return 0;
    }

    /* Is the first line an archive file signature? */
    if (!str_eq(ged_str(line), GED_S("!<arch>\n"))) {
        *err = fmt_errorf_v("not the start of an archive file (%q)", ged_str(line));
        return 0;
    }

    /* package export block should be first */
    Int size = ged_read_archive_header(r, GED_S("__.PKGDEF"));
    if (size <= 0) {
        *err = fmt_errorf_v("not a package file");
        return size;
    }

    return size;
}

Str burrow__exportdata_read_object_headers(Alloc *a, BufioReader *r, Slice *headers,
                                           Error *err) {
    *err = BURROW_NO_ERROR;
    *headers = slice_nil(TYPE_STRING);

    /* line is a temporary buffer for headers.
     * Use bounded reads (ReadSlice, Peek) to limit risk of malformed inputs. */

    /* objapi header should be the first line */
    Slice line = bufio_reader_read_slice(r, '\n', err);
    if (BURROW_FAILED(*err)) {
        *err = fmt_errorf_v("can't find export data (%v)", *err);
        return GED_S("");
    }
    Str objapi = str_clone(a, ged_str(line));

    /* objapi header begins with "go object ". */
    if (!strings_has_prefix(objapi, GED_S("go object "))) {
        *err = fmt_errorf_v("not a go object file: %s", objapi);
        return objapi;
    }

    /* process remaining object header lines */
    for (;;) {
        /* check for an end of section marker "$$" */
        line = bufio_reader_peek(r, 2, err);
        if (BURROW_FAILED(*err))
            return objapi;
        if (str_eq(ged_str(line), GED_S("$$")))
            return objapi; /* stop */

        /* read next header */
        line = bufio_reader_read_slice(r, '\n', err);
        if (BURROW_FAILED(*err))
            return objapi;
        Str h = str_clone(a, ged_str(line));
        Slice grown = slice_append(a, *headers, &h, 1);
        if (grown.p == NULL) {
            *err = errors_new(a, GED_S("out of memory"));
            return objapi;
        }
        *headers = grown;
    }
}

Int burrow__exportdata_read_export_data_header(BufioReader *r, Error *err) {
    *err = BURROW_NO_ERROR;

    /* Read export data header. */
    Slice line = bufio_reader_read_slice(r, '\n', err);
    if (BURROW_FAILED(*err))
        return 0;

    Str hdr = ged_str(line);
    if (str_eq(hdr, GED_S("$$\n"))) {
        *err = fmt_errorf_v("old textual export format no longer supported (recompile "
                            "package)");
        return 0;
    }
    if (!str_eq(hdr, GED_S("$$B\n"))) {
        *err = fmt_errorf_v("unknown export data header: %q", hdr);
        return 0;
    }
    {
        Byte format = bufio_reader_read_byte(r, err);
        if (BURROW_FAILED(*err))
            return 0;
        /* The unified export format starts with a 'u'. */
        switch (format) {
        case 'u':
            break;
        default:
            /* Older no longer supported export formats include:
             * indexed export format which started with an 'i'; and
             * the older binary export format which started with a 'c', 'd', or
             * 'v' (from "version"). */
            *err = fmt_errorf_v("binary export format %q is no longer supported "
                                "(recompile package)",
                                format);
            return 0;
        }
    }

    return hdr.len + 1; /* + 1 is for 'u' */
}

/* What lookupGorootExport found for a directory. */
typedef struct GedExport {
    Str path;
    Error err;
} GedExport;

/* lookupGorootExport returns the location of the export data (normally found
 * in the build cache, but located in GOROOT/pkg in prior Go releases) for the
 * package located in pkgDir.
 *
 * (We use the package's directory instead of its import path mainly to
 * simplify handling of the packages in src/vendor and cmd/vendor.) */
static Str ged_lookup_goroot_export(Alloc *a, Str pkg_dir, Map *exports, Error *err) {
    if (exports != NULL) {
        GedExport **found = map_get(exports, &pkg_dir);
        if (found != NULL && *found != NULL) {
            *err = (*found)->err;
            return (*found)->path;
        }
    }

    Str export_path = GED_S("");
    *err = BURROW_NO_ERROR;
    Str goroot = build_default(a).goroot;
    ExecCmd *cmd = exec_command_v(
        a, filepath_join_v(a, 3, goroot, GED_S("bin"), GED_S("go")), 5, GED_S("list"),
        GED_S("-export"), GED_S("-f"), GED_S("{{.Export}}"), pkg_dir);
    if (cmd == NULL) {
        *err = errors_new(a, GED_S("out of memory"));
        return export_path;
    }
    cmd->dir = goroot;
    Slice env = os_environ(a);
    Str pwd = fmt_sprintf_v(a, "PWD=%s", cmd->dir);
    Str gr = fmt_sprintf_v(a, "GOROOT=%s", goroot);
    env = slice_append(a, env, &pwd, 1);
    env = slice_append(a, env, &gr, 1);
    cmd->env = env;
    Slice output = exec_cmd_output(cmd, a, err);
    exec_cmd_free(cmd);
    if (BURROW_FAILED(*err)) {
        const ExecExitError *ee = errors_as(*err, TYPE_EXEC_EXIT_ERROR);
        if (ee != NULL && ee->stderr_.len > 0)
            *err = errors_new(a, ged_str(ee->stderr_));
    } else {
        Slice exports_list =
            strings_split(a, strings_trim_space(ged_str(output)), GED_S("\n"));
        if (exports_list.len != 1)
            *err = fmt_errorf_v("go list reported %d exports; expected 1",
                                exports_list.len);
        else
            export_path = BURROW_AT(Str, exports_list, 0);
    }

    if (exports != NULL) {
        GedExport *e = BURROW_NEW(a, GedExport);
        Str key = str_clone(a, pkg_dir);
        if (e != NULL && (pkg_dir.len == 0 || key.p != NULL)) {
            *e = (GedExport){export_path, *err};
            (void)map_set(exports, &key, &e);
        }
    }
    return export_path;
}

/* a file from the build cache will have no extension */
static const Str ged_pkg_exts[] = {BURROW_S_INIT(".a"), BURROW_S_INIT(".o")};

Str burrow__exportdata_find_pkg(Alloc *a, Str path, Str src_dir, Map *exports, Str *id,
                                Error *err) {
    *err = BURROW_NO_ERROR;
    *id = GED_S("");
    Str filename = GED_S("");
    if (path.len == 0)
        return *err = errors_new(a, GED_S("path is empty")), filename;

    Str noext = GED_S("");
    if (build_is_local_import(path)) {
        /* "./x" -> "/this/directory/x.ext", "/this/directory/x" */
        noext = filepath_join_v(a, 2, src_dir, path);
        *id = noext;
    } else if (filepath_is_abs(path)) {
        /* for completeness only - go/build.Import does not support absolute
         * imports
         * "/x" -> "/x.ext", "/x" */
        noext = path;
        *id = path;
    } else {
        /* "x" -> "$GOPATH/pkg/$GOOS_$GOARCH/x.ext", "x" */
        /* Don't require the source files to be present. */
        Error abs_err = BURROW_NO_ERROR;
        Str abs = filepath_abs(a, src_dir, &abs_err);
        if (BURROW_OK(abs_err)) /* see issue 14282 */
            src_dir = abs;
        BuildPackage *bp =
            build_import(a, path, src_dir, BUILD_FIND_ONLY | BUILD_ALLOW_BINARY, err);
        if (bp == NULL)
            goto notfound;
        if (bp->pkg_obj.len == 0) {
            if (bp->goroot && bp->dir.len > 0) {
                filename = ged_lookup_goroot_export(a, bp->dir, exports, err);
                if (BURROW_OK(*err))
                    (void)os_stat(a, filename, err);
                if (BURROW_OK(*err)) {
                    *id = bp->import_path;
                    return filename;
                }
            }
            goto notfound;
        } else {
            noext = strings_trim_suffix(bp->pkg_obj, GED_S(".a"));
        }
        *id = bp->import_path;
    }

    /* try extensions */
    for (Int i = 0; i < (Int)(sizeof ged_pkg_exts / sizeof ged_pkg_exts[0]); i++) {
        filename = fmt_sprintf_v(a, "%s%s", noext, ged_pkg_exts[i]);
        Error stat_err = BURROW_NO_ERROR;
        OsFileInfo f = os_stat(a, filename, &stat_err);
        if (BURROW_OK(stat_err) && !f.vt->is_dir(f.data))
            return filename;
        if (BURROW_OK(*err))
            *err = stat_err;
    }

notfound:
    filename = GED_S("");
    *id = path;
    if (BURROW_OK(*err)) {
        *err = fmt_errorf_v("can't find import: %q", path);
        return filename;
    }
    *err = fmt_errorf_v("can't find import: %q: %w", path, *err);
    return filename;
}

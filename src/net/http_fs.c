/* Derived from Go's src/net/http/fs.go, the file server.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "http_internal.h"

#include "burrow/core.h"
#include "burrow/defer.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/func.h"
#include "burrow/io.h"
#include "burrow/io/fs.h"
#include "burrow/log.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/mime.h"
#include "burrow/mime/multipart.h"
#include "burrow/net/http.h"
#include "burrow/net/textproto.h"
#include "burrow/net/url.h"
#include "burrow/os.h"
#include "burrow/panic.h"
#include "burrow/path.h"
#include "burrow/path/filepath.h"
#include "burrow/slice.h"
#include "burrow/slices.h"
#include "burrow/strconv.h"
#include "burrow/strings.h"
#include "burrow/time.h"
#include "burrow/type.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static const Str hd_text_invalid_unsafe_path =
    BURROW_S_INIT("http: invalid or unsafe file path");
static const Str hd_text_seeker = BURROW_S_INIT("seeker can't seek");
static const Str hd_text_no_overlap = BURROW_S_INIT("invalid range: failed to overlap");
static const Str hd_text_invalid_range = BURROW_S_INIT("invalid range");
static const Str hd_text_missing_seek = BURROW_S_INIT("io.File missing Seek method");
static const Str hd_text_missing_read_dir =
    BURROW_S_INIT("io.File directory missing ReadDir method");
static const Str hd_text_no_memory = BURROW_S_INIT("net/http: out of memory");

/* errInvalidUnsafePath, errSeeker, errNoOverlap, errMissingSeek and
 * errMissingReadDir. Go makes the "invalid range" errors afresh each time, and
 * nothing compares against them, so one does for all of them here. */
static Error hd_error(const Str *text) {
    return (Error){&burrow_sentinel_error_vt, text};
}

BURROW_NORETURN static void hd_out_of_memory(void) {
    panic_str(hd_text_no_memory);
}

/* x and y in one string from a. Panics when a says no. */
static Str hd_cat(Alloc *a, Str x, Str y) {
    if (y.len == 0)
        return x;
    if (x.len == 0)
        return y;
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)(x.len + y.len), 1);
    if (p == NULL)
        hd_out_of_memory();
    memcpy(p, x.p, (size_t)x.len);
    memcpy(p + x.len, y.p, (size_t)y.len);
    return str_from_bytes(p, x.len + y.len);
}

/* s in memory that lasts as long as h, for a header value. */
static Str hd_header_value(HttpHeader h, Str s) {
    Str v = str_clone(burrow__map_allocator(h), s);
    if (v.p == NULL && s.len > 0)
        hd_out_of_memory();
    return v;
}

static void hd_set(HttpHeader h, Str key, Str value) {
    if (!http_header_set(h, key, value))
        hd_out_of_memory();
}

static void hd_arena_free(void *ar) {
    arena_free((Arena *)ar);
}

static Str hd_url_path(const HttpRequest *r) {
    return r->url != NULL ? r->url->path : BURROW_STR_EMPTY;
}

static Str hd_request_header(const HttpRequest *r, Str key) {
    return burrow__http_header_get(r->header, key);
}

/* ---------------------------------------------------------- mapOpenError */

/* mapOpenError. A better error than orig for opening name, which is that
 * fs_err_not_exist when a file that is not a directory is on the way to name,
 * as some systems say something else for that. stat is os_stat when fsys is
 * NULL and fs_stat on fsys otherwise. */
static Error hd_map_open_error(Alloc *t, Error orig, Str name, Byte sep,
                               const Fs *fsys) {
    if (errors_is(orig, fs_err_not_exist) || errors_is(orig, fs_err_permission))
        return orig;

    /* Each element of name in turn, with everything before it. Go joins the
     * elements again, which gives the same string as cutting name after the
     * element. */
    Int start = 0;
    for (Int i = 0; i <= name.len; i++) {
        if (i < name.len && name.p[i] != sep)
            continue;
        if (i > start) {
            Str prefix = str_from_bytes(name.p, i);
            Error e = BURROW_NO_ERROR;
            FsFileInfo fi =
                fsys != NULL ? fs_stat(t, *fsys, prefix, &e) : os_stat(t, prefix, &e);
            if (BURROW_FAILED(e))
                return orig;
            if (!fi.vt->is_dir(fi.data))
                return fs_err_not_exist;
        }
        start = i + 1;
    }
    return orig;
}

/* -------------------------------------------------------------------- Dir */

static Int hd_os_read(void *self, Slice p, Error *err) {
    return os_file_read((OsFile *)self, p, err);
}

static Error hd_os_close(void *self) {
    OsFile *f = (OsFile *)self;
    Error e = os_file_close(f);
    os_file_free(f);
    return e;
}

static int64_t hd_os_seek(void *self, int64_t offset, int whence, Error *err) {
    return os_file_seek((OsFile *)self, offset, whence, err);
}

static Slice hd_os_readdir(void *self, Alloc *a, Int count, Error *err) {
    return os_file_readdir((OsFile *)self, a, count, err);
}

static FsFileInfo hd_os_stat(void *self, Alloc *a, Error *err) {
    return os_file_stat((OsFile *)self, a, err);
}

static Slice hd_os_read_dir(void *self, Alloc *a, Int count, Error *err) {
    return os_file_read_dir((OsFile *)self, a, count, err);
}

static const HttpFileVT hd_os_file_vt = {
    {{&burrow__os_file_desc, hd_os_read}, {&burrow__os_file_desc, hd_os_close}},
    hd_os_seek,
    hd_os_readdir,
    hd_os_stat,
    hd_os_read_dir,
};

static HttpFile hd_dir_open(Alloc *t, Str dir, Alloc *a, Str name, Error *err) {
    HttpFile none = {NULL, NULL};
    Str p = path_clean(t, hd_cat(t, BURROW_S("/"), name));
    p = str_from_bytes(p.p + 1, p.len - 1);
    if (p.len == 0)
        p = BURROW_S(".");
    Error le = BURROW_NO_ERROR;
    p = filepath_localize(t, p, &le);
    if (BURROW_FAILED(le)) {
        BURROW_OUT(err, hd_error(&hd_text_invalid_unsafe_path));
        return none;
    }
    if (dir.len == 0)
        dir = BURROW_S(".");
    Str full = filepath_join_v(t, 2, dir, p);
    Error oe = BURROW_NO_ERROR;
    OsFile *f = os_open(a, full, &oe);
    if (BURROW_FAILED(oe)) {
        BURROW_OUT(err, hd_map_open_error(t, oe, full, (Byte)FILEPATH_SEPARATOR, NULL));
        return none;
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return (HttpFile){&hd_os_file_vt, f};
}

HttpFile http_dir_open(HttpDir d, Alloc *a, Str name, Error *err) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    HttpFile f = {NULL, NULL};
    BURROW_SCOPE {
        BURROW_DEFER(hd_arena_free, &ar);
        f = hd_dir_open(arena_allocator(&ar), d, a, name, err);
    }
    BURROW_SCOPE_END;
    return f;
}

static const Type hd_dir_desc = {
    {(const Byte *)"Dir", 3},
    {(const Byte *)"net/http", 8},
    KIND_STRING,
    (uint32_t)sizeof(HttpDir),
    (uint16_t)_Alignof(HttpDir),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x68646972U, /* "hdir" */
    NULL,
};

const Type *const TYPE_HTTP_DIR = &hd_dir_desc;

static HttpFile hd_dir_fs_open(void *self, Alloc *a, Str name, Error *err) {
    return http_dir_open(*(const HttpDir *)self, a, name, err);
}

static const HttpFileSystemVT hd_dir_fs_vt = {&hd_dir_desc, hd_dir_fs_open};

HttpFileSystem http_dir_as_file_system(const HttpDir *d) {
    /* Open only reads the Dir. */
    return (HttpFileSystem){&hd_dir_fs_vt, (void *)(uintptr_t)d};
}

/* --------------------------------------------------------------- ioFS */

typedef struct hd_IoFS {
    Fs fsys;
} hd_IoFS;

/* ioFile, an fs.File as an http.File, with the Seek method of the file's type
 * found once when it is opened. */
typedef struct hd_IoFile {
    FsFile file;
    const Method *seek;
} hd_IoFile;

static const Type hd_io_fs_desc = {
    {(const Byte *)"ioFS", 4},
    {(const Byte *)"net/http", 8},
    KIND_STRUCT,
    (uint32_t)sizeof(hd_IoFS),
    (uint16_t)_Alignof(hd_IoFS),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x68696f66U, /* "hiof" */
    NULL,
};

static const Type hd_io_file_desc = {
    {(const Byte *)"ioFile", 6},
    {(const Byte *)"net/http", 8},
    KIND_STRUCT,
    (uint32_t)sizeof(hd_IoFile),
    (uint16_t)_Alignof(hd_IoFile),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x6869666cU, /* "hifl" */
    NULL,
};

static Int hd_io_file_read(void *self, Slice p, Error *err) {
    const hd_IoFile *f = (const hd_IoFile *)self;
    return f->file.vt->read_closer.reader.read(f->file.data, p, err);
}

static Error hd_io_file_close(void *self) {
    const hd_IoFile *f = (const hd_IoFile *)self;
    return f->file.vt->read_closer.closer.close(f->file.data);
}

static int64_t hd_io_file_seek(void *self, int64_t offset, int whence, Error *err) {
    const hd_IoFile *f = (const hd_IoFile *)self;
    if (f->seek == NULL) {
        BURROW_OUT(err, hd_error(&hd_text_missing_seek));
        return 0;
    }
    return burrow__io_seek(f->seek, f->file.data, offset, whence, err);
}

static Slice hd_io_file_read_dir(void *self, Alloc *a, Int count, Error *err) {
    const hd_IoFile *f = (const hd_IoFile *)self;
    if (f->file.vt->read_dir == NULL) {
        BURROW_OUT(err, hd_error(&hd_text_missing_read_dir));
        return slice_nil(TYPE_FS_DIR_ENTRY);
    }
    return f->file.vt->read_dir(f->file.data, a, count, err);
}

/* ioFile.Readdir: ReadDir, with the info of each entry, and the entries whose
 * info fails left out, as os.File's Readdir leaves them. */
static Slice hd_io_file_readdir(void *self, Alloc *a, Int count, Error *err) {
    const hd_IoFile *f = (const hd_IoFile *)self;
    Slice list = slice_nil(TYPE_FS_FILE_INFO);
    if (f->file.vt->read_dir == NULL) {
        BURROW_OUT(err, hd_error(&hd_text_missing_read_dir));
        return list;
    }
    for (;;) {
        Error e = BURROW_NO_ERROR;
        Slice dirs = f->file.vt->read_dir(f->file.data, a, count - list.len, &e);
        const FsDirEntry *ents = (const FsDirEntry *)dirs.p;
        for (Int i = 0; i < dirs.len; i++) {
            Error ie = BURROW_NO_ERROR;
            FsFileInfo info = ents[i].vt->info(ents[i].data, a, &ie);
            if (BURROW_FAILED(ie))
                continue;
            Slice grown = slice_append(a, list, &info, 1);
            if (grown.len != list.len + 1) {
                BURROW_OUT(err, burrow_err_out_of_memory);
                return list;
            }
            list = grown;
        }
        if (BURROW_FAILED(e)) {
            BURROW_OUT(err, e);
            return list;
        }
        if (count < 0 || list.len >= count)
            break;
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return list;
}

static FsFileInfo hd_io_file_stat(void *self, Alloc *a, Error *err) {
    const hd_IoFile *f = (const hd_IoFile *)self;
    return f->file.vt->stat(f->file.data, a, err);
}

static const HttpFileVT hd_io_file_vt = {
    {{&hd_io_file_desc, hd_io_file_read}, {&hd_io_file_desc, hd_io_file_close}},
    hd_io_file_seek,
    hd_io_file_readdir,
    hd_io_file_stat,
    hd_io_file_read_dir,
};

static HttpFile hd_io_fs_open(void *self, Alloc *a, Str name, Error *err) {
    const hd_IoFS *s = (const hd_IoFS *)self;
    HttpFile none = {NULL, NULL};
    if (str_eq(name, BURROW_S("/")))
        name = BURROW_S(".");
    else
        name = strings_trim_prefix(name, BURROW_S("/"));
    Error e = BURROW_NO_ERROR;
    FsFile file = s->fsys.vt->open(s->fsys.data, a, name, &e);
    if (BURROW_FAILED(e)) {
        Arena ar;
        arena_init(&ar, NULL, 0);
        BURROW_SCOPE {
            BURROW_DEFER(hd_arena_free, &ar);
            BURROW_OUT(err,
                       hd_map_open_error(arena_allocator(&ar), e, name, '/', &s->fsys));
        }
        BURROW_SCOPE_END;
        return none;
    }
    hd_IoFile *f = (hd_IoFile *)mem_alloc(a, sizeof *f, _Alignof(hd_IoFile));
    if (f == NULL) {
        (void)file.vt->read_closer.closer.close(file.data);
        BURROW_OUT(err, burrow_err_out_of_memory);
        return none;
    }
    f->file = file;
    f->seek = burrow__io_seek_method(file.vt->read_closer.reader.self_type);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return (HttpFile){&hd_io_file_vt, f};
}

static const HttpFileSystemVT hd_io_fs_vt = {&hd_io_fs_desc, hd_io_fs_open};

HttpFileSystem http_fs(Alloc *a, Fs fsys) {
    hd_IoFS *s = (hd_IoFS *)mem_alloc(a, sizeof *s, _Alignof(hd_IoFS));
    if (s == NULL)
        return (HttpFileSystem){&hd_io_fs_vt, NULL};
    s->fsys = fsys;
    return (HttpFileSystem){&hd_io_fs_vt, s};
}

/* ---------------------------------------------------------- serveError */

/* serveError. http_error, after taking out the fields a caller may have set
 * for a response that worked, unless GODEBUG says to keep them. */
static void hd_serve_error(HttpResponseWriter w, Str text, Int code) {
    static const Str keys[] = {
        BURROW_S_INIT("Cache-Control"),
        BURROW_S_INIT("Content-Encoding"),
        BURROW_S_INIT("Etag"),
        BURROW_S_INIT("Last-Modified"),
    };
    HttpHeader h = http_response_writer_header(w);
    bool keep = burrow__http_godebug_serve_content_keep_headers();
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        if (!burrow__http_header_has(h, keys[i]))
            continue;
        if (!keep)
            http_header_del(h, keys[i]);
    }
    http_error(w, text, code);
}

/* toHTTPError. A message and a status for err that say no more than they have
 * to, since they go to whoever asked. */
static void hd_serve_http_error(HttpResponseWriter w, Error err) {
    if (errors_is(err, fs_err_not_exist) ||
        errors_is(err, hd_error(&hd_text_invalid_unsafe_path))) {
        hd_serve_error(w, BURROW_S("404 page not found"), HTTP_STATUS_NOT_FOUND);
        return;
    }
    if (errors_is(err, fs_err_permission)) {
        hd_serve_error(w, BURROW_S("403 Forbidden"), HTTP_STATUS_FORBIDDEN);
        return;
    }
    hd_serve_error(w, BURROW_S("500 Internal Server Error"),
                   HTTP_STATUS_INTERNAL_SERVER_ERROR);
}

/* ----------------------------------------------------------------- ETags */

Str burrow__http_scan_etag(Str s, Str *remain) {
    *remain = BURROW_STR_EMPTY;
    s = textproto_trim_string(s);
    Int start = 0;
    if (strings_has_prefix(s, BURROW_S("W/")))
        start = 2;
    if (s.len - start < 2 || s.p[start] != '"')
        return BURROW_STR_EMPTY;
    /* W/"text" or "text", RFC 7232 section 2.3. */
    for (Int i = start + 1; i < s.len; i++) {
        Byte c = s.p[i];
        if (c == 0x21 || (c >= 0x23 && c <= 0x7E) || c >= 0x80)
            continue;
        if (c == '"') {
            *remain = str_from_bytes(s.p + i + 1, s.len - i - 1);
            return str_from_bytes(s.p, i + 1);
        }
        return BURROW_STR_EMPTY;
    }
    return BURROW_STR_EMPTY;
}

static bool hd_etag_strong_match(Str a, Str b) {
    return str_eq(a, b) && a.len > 0 && a.p[0] == '"';
}

static bool hd_etag_weak_match(Str a, Str b) {
    return str_eq(strings_trim_prefix(a, BURROW_S("W/")),
                  strings_trim_prefix(b, BURROW_S("W/")));
}

/* ---------------------------------------------------------- Preconditions */

/* condResult, RFC 7232 section 3. */
typedef enum hd_Cond { HD_COND_NONE, HD_COND_TRUE, HD_COND_FALSE } hd_Cond;

static bool hd_get_or_head(const HttpRequest *r) {
    return str_eq(r->method, BURROW_S("GET")) || str_eq(r->method, BURROW_S("HEAD"));
}

static hd_Cond hd_check_if_match(HttpResponseWriter w, const HttpRequest *r) {
    Str im = hd_request_header(r, BURROW_S("If-Match"));
    if (im.len == 0)
        return HD_COND_NONE;
    HttpHeader h = http_response_writer_header(w);
    for (;;) {
        im = textproto_trim_string(im);
        if (im.len == 0)
            break;
        if (im.p[0] == ',') {
            im = str_from_bytes(im.p + 1, im.len - 1);
            continue;
        }
        if (im.p[0] == '*')
            return HD_COND_TRUE;
        Str remain;
        Str etag = burrow__http_scan_etag(im, &remain);
        if (etag.len == 0)
            break;
        if (hd_etag_strong_match(etag, burrow__http_header_get(h, BURROW_S("Etag"))))
            return HD_COND_TRUE;
        im = remain;
    }
    return HD_COND_FALSE;
}

/* Whether a Last-Modified field could say modtime, which is when it is neither
 * the zero time nor the Unix epoch. */
static bool hd_is_zero_time(Time t) {
    return time_is_zero(t) || time_equal(t, time_from_unix(0, 0));
}

static hd_Cond hd_check_if_unmodified_since(const HttpRequest *r, Time modtime,
                                            Alloc *t) {
    Str ius = hd_request_header(r, BURROW_S("If-Unmodified-Since"));
    if (ius.len == 0 || hd_is_zero_time(modtime))
        return HD_COND_NONE;
    Error e = BURROW_NO_ERROR;
    Time tm = http_parse_time(t, ius, &e);
    if (BURROW_FAILED(e))
        return HD_COND_NONE;
    /* Last-Modified has no fractions of a second, so modtime cannot either. */
    modtime = time_truncate(modtime, TIME_SECOND);
    return time_compare(modtime, tm) <= 0 ? HD_COND_TRUE : HD_COND_FALSE;
}

static hd_Cond hd_check_if_none_match(HttpResponseWriter w, const HttpRequest *r) {
    Str buf = hd_request_header(r, BURROW_S("If-None-Match"));
    if (buf.len == 0)
        return HD_COND_NONE;
    HttpHeader h = http_response_writer_header(w);
    for (;;) {
        buf = textproto_trim_string(buf);
        if (buf.len == 0)
            break;
        if (buf.p[0] == ',') {
            buf = str_from_bytes(buf.p + 1, buf.len - 1);
            continue;
        }
        if (buf.p[0] == '*')
            return HD_COND_FALSE;
        Str remain;
        Str etag = burrow__http_scan_etag(buf, &remain);
        if (etag.len == 0)
            break;
        if (hd_etag_weak_match(etag, burrow__http_header_get(h, BURROW_S("Etag"))))
            return HD_COND_FALSE;
        buf = remain;
    }
    return HD_COND_TRUE;
}

static hd_Cond hd_check_if_modified_since(const HttpRequest *r, Time modtime,
                                          Alloc *t) {
    if (!hd_get_or_head(r))
        return HD_COND_NONE;
    Str ims = hd_request_header(r, BURROW_S("If-Modified-Since"));
    if (ims.len == 0 || hd_is_zero_time(modtime))
        return HD_COND_NONE;
    Error e = BURROW_NO_ERROR;
    Time tm = http_parse_time(t, ims, &e);
    if (BURROW_FAILED(e))
        return HD_COND_NONE;
    modtime = time_truncate(modtime, TIME_SECOND);
    return time_compare(modtime, tm) <= 0 ? HD_COND_FALSE : HD_COND_TRUE;
}

static hd_Cond hd_check_if_range(HttpResponseWriter w, const HttpRequest *r,
                                 Time modtime, Alloc *t) {
    if (!hd_get_or_head(r))
        return HD_COND_NONE;
    Str ir = hd_request_header(r, BURROW_S("If-Range"));
    if (ir.len == 0)
        return HD_COND_NONE;
    Str remain;
    Str etag = burrow__http_scan_etag(ir, &remain);
    if (etag.len > 0) {
        HttpHeader h = http_response_writer_header(w);
        return hd_etag_strong_match(etag, http_header_get(h, BURROW_S("Etag")))
                   ? HD_COND_TRUE
                   : HD_COND_FALSE;
    }
    /* The value is usually an ETag, but it can be a date too, golang.org/issue/8367. */
    if (time_is_zero(modtime))
        return HD_COND_FALSE;
    Error e = BURROW_NO_ERROR;
    Time tm = http_parse_time(t, ir, &e);
    if (BURROW_FAILED(e))
        return HD_COND_FALSE;
    return time_unix(tm) == time_unix(modtime) ? HD_COND_TRUE : HD_COND_FALSE;
}

static void hd_set_last_modified(HttpResponseWriter w, Time modtime) {
    if (hd_is_zero_time(modtime))
        return;
    HttpHeader h = http_response_writer_header(w);
    Str v = time_format(time_utc(modtime), burrow__map_allocator(h), HTTP_TIME_FORMAT);
    if (v.len == 0)
        hd_out_of_memory();
    hd_set(h, BURROW_S("Last-Modified"), v);
}

static void hd_write_not_modified(HttpResponseWriter w) {
    /* RFC 7232 section 4.1: no representation metadata but what helps a cache,
     * and Last-Modified only when there is no ETag. */
    HttpHeader h = http_response_writer_header(w);
    http_header_del(h, BURROW_S("Content-Type"));
    http_header_del(h, BURROW_S("Content-Length"));
    http_header_del(h, BURROW_S("Content-Encoding"));
    if (http_header_get(h, BURROW_S("Etag")).len > 0)
        http_header_del(h, BURROW_S("Last-Modified"));
    http_response_writer_write_header(w, HTTP_STATUS_NOT_MODIFIED);
}

/* checkPreconditions, which follows RFC 7232 section 6. Whether it has
 * answered the request with a 304 or a 412, and the Range header to use when
 * it has not. */
static bool hd_check_preconditions(HttpResponseWriter w, const HttpRequest *r,
                                   Time modtime, Alloc *t, Str *range_header) {
    *range_header = BURROW_STR_EMPTY;
    hd_Cond ch = hd_check_if_match(w, r);
    if (ch == HD_COND_NONE)
        ch = hd_check_if_unmodified_since(r, modtime, t);
    if (ch == HD_COND_FALSE) {
        http_response_writer_write_header(w, HTTP_STATUS_PRECONDITION_FAILED);
        return true;
    }
    hd_Cond cn = hd_check_if_none_match(w, r);
    if (cn == HD_COND_FALSE) {
        if (hd_get_or_head(r))
            hd_write_not_modified(w);
        else
            http_response_writer_write_header(w, HTTP_STATUS_PRECONDITION_FAILED);
        return true;
    }
    if (cn == HD_COND_NONE &&
        hd_check_if_modified_since(r, modtime, t) == HD_COND_FALSE) {
        hd_write_not_modified(w);
        return true;
    }

    Str rh = hd_request_header(r, BURROW_S("Range"));
    if (rh.len > 0 && hd_check_if_range(w, r, modtime, t) == HD_COND_FALSE)
        rh = BURROW_STR_EMPTY;
    *range_header = rh;
    return false;
}

/* ---------------------------------------------------------------- Ranges */

Int burrow__http_parse_range(Alloc *a, Str s, int64_t size, burrow__HttpRange **out,
                             Error *err) {
    *out = NULL;
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (s.len == 0)
        return 0; /* no Range header */
    const Str b = BURROW_S("bytes=");
    if (!strings_has_prefix(s, b)) {
        BURROW_OUT(err, hd_error(&hd_text_invalid_range));
        return 0;
    }
    s = str_from_bytes(s.p + b.len, s.len - b.len);

    /* At most one range for each comma and one more. */
    Int max = 1;
    for (Int i = 0; i < s.len; i++)
        if (s.p[i] == ',')
            max++;
    burrow__HttpRange *ranges = (burrow__HttpRange *)mem_alloc(
        a, (size_t)max * sizeof *ranges, _Alignof(burrow__HttpRange));
    if (ranges == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return 0;
    }

    Int n = 0;
    bool no_overlap = false;
    Int from = 0;
    for (Int i = 0; i <= s.len; i++) {
        if (i < s.len && s.p[i] != ',')
            continue;
        Str ra = textproto_trim_string(str_from_bytes(s.p + from, i - from));
        from = i + 1;
        if (ra.len == 0)
            continue;
        Int dash = strings_index_byte(ra, '-');
        if (dash < 0) {
            BURROW_OUT(err, hd_error(&hd_text_invalid_range));
            return 0;
        }
        Str start = textproto_trim_string(str_from_bytes(ra.p, dash));
        Str end =
            textproto_trim_string(str_from_bytes(ra.p + dash + 1, ra.len - dash - 1));
        burrow__HttpRange r;
        if (start.len == 0) {
            /* No start, so end is how far from the end of the content the range
             * starts, a suffix-length, which RFC 7233 section 2.1 says is not
             * negative. */
            if (end.len == 0 || end.p[0] == '-') {
                BURROW_OUT(err, hd_error(&hd_text_invalid_range));
                return 0;
            }
            Error pe = BURROW_NO_ERROR;
            int64_t v = strconv_parse_int(end, 10, 64, &pe);
            if (v < 0 || BURROW_FAILED(pe)) {
                BURROW_OUT(err, hd_error(&hd_text_invalid_range));
                return 0;
            }
            if (v > size)
                v = size;
            r.start = size - v;
            r.length = size - r.start;
        } else {
            Error pe = BURROW_NO_ERROR;
            int64_t v = strconv_parse_int(start, 10, 64, &pe);
            if (BURROW_FAILED(pe) || v < 0) {
                BURROW_OUT(err, hd_error(&hd_text_invalid_range));
                return 0;
            }
            if (v >= size) {
                /* A range that starts after the content does not overlap it. */
                no_overlap = true;
                continue;
            }
            r.start = v;
            if (end.len == 0) {
                /* No end, so the range goes to the end of the content. */
                r.length = size - r.start;
            } else {
                v = strconv_parse_int(end, 10, 64, &pe);
                if (BURROW_FAILED(pe) || r.start > v) {
                    BURROW_OUT(err, hd_error(&hd_text_invalid_range));
                    return 0;
                }
                if (v >= size)
                    v = size - 1;
                r.length = v - r.start + 1;
            }
        }
        ranges[n++] = r;
    }
    if (no_overlap && n == 0) {
        BURROW_OUT(err, hd_error(&hd_text_no_overlap));
        return 0;
    }
    *out = ranges;
    return n;
}

static Str hd_content_range(Alloc *a, burrow__HttpRange r, int64_t size) {
    return fmt_sprintf_v(a, "bytes %d-%d/%d", r.start, r.start + r.length - 1, size);
}

static TextprotoMIMEHeader hd_mime_header(Alloc *a, burrow__HttpRange r,
                                          Str content_type, int64_t size) {
    TextprotoMIMEHeader h = textproto_mime_header_make(a);
    if (h == NULL ||
        !textproto_mime_header_set(h, BURROW_S("Content-Range"),
                                   hd_content_range(a, r, size)) ||
        !textproto_mime_header_set(h, BURROW_S("Content-Type"), content_type))
        hd_out_of_memory();
    return h;
}

/* countingWriter, which counts what is written to it. */
static Int hd_counting_write(void *self, Slice p, Error *err) {
    *(int64_t *)self += p.len;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return p.len;
}

static const Type hd_counting_writer_desc = {
    {(const Byte *)"countingWriter", 14},
    {(const Byte *)"net/http", 8},
    KIND_INT64,
    (uint32_t)sizeof(int64_t),
    (uint16_t)_Alignof(int64_t),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x68637772U, /* "hcwr" */
    NULL,
};

static const IoWriterVT hd_counting_writer_vt = {&hd_counting_writer_desc,
                                                 hd_counting_write};

/* rangesMIMESize. How many bytes the ranges take as a multipart response. */
static int64_t hd_ranges_mime_size(Alloc *t, const burrow__HttpRange *ranges, Int n,
                                   Str content_type, int64_t content_size) {
    int64_t written = 0;
    MultipartWriter *mw =
        multipart_new_writer(t, (IoWriter){&hd_counting_writer_vt, &written});
    if (mw == NULL)
        hd_out_of_memory();
    int64_t enc = 0;
    for (Int i = 0; i < n; i++) {
        Error e = BURROW_NO_ERROR;
        (void)multipart_writer_create_part(
            mw, hd_mime_header(t, ranges[i], content_type, content_size), &e);
        enc += ranges[i].length;
    }
    (void)multipart_writer_close(mw);
    multipart_writer_free(mw);
    return enc + written;
}

static int64_t hd_sum_ranges_size(const burrow__HttpRange *ranges, Int n) {
    int64_t size = 0;
    for (Int i = 0; i < n; i++)
        size += ranges[i].length;
    return size;
}

/* -------------------------------------------------------------- serveContent */

/* What serveContent serves: a reader and the seek that goes with it, and the
 * size when it is known, which is sizeFunc. */
typedef struct hd_Content {
    IoReader reader;
    void *self;
    int64_t (*seek)(void *self, int64_t offset, int whence, Error *err);
    int64_t size;
    bool have_size;
} hd_Content;

/* The multipart/byteranges body for several ranges, written to w. Go writes it
 * into a pipe from a goroutine and copies sendSize bytes out of the pipe, and
 * stops when a part fails, which is what this does too. */
static void hd_write_ranges(MultipartWriter *mw, Alloc *t, const hd_Content *c,
                            const burrow__HttpRange *ranges, Int n, Str ctype,
                            int64_t size) {
    for (Int i = 0; i < n; i++) {
        Error e = BURROW_NO_ERROR;
        IoWriter part = multipart_writer_create_part(
            mw, hd_mime_header(t, ranges[i], ctype, size), &e);
        if (BURROW_FAILED(e))
            return;
        (void)c->seek(c->self, ranges[i].start, BURROW_IO_SEEK_START, &e);
        if (BURROW_FAILED(e))
            return;
        (void)io_copy_n(t, part, c->reader, ranges[i].length, &e);
        if (BURROW_FAILED(e))
            return;
    }
    (void)multipart_writer_close(mw);
}

/* serveContent. name is for the type and may be empty, and a zero modtime is
 * not known. content is at its start. */
static void hd_serve_content(HttpResponseWriter w, const HttpRequest *r, Str name,
                             Time modtime, const hd_Content *c, Alloc *t) {
    hd_set_last_modified(w, modtime);
    Str range_req;
    if (hd_check_preconditions(w, r, modtime, t, &range_req))
        return;

    Int code = HTTP_STATUS_OK;
    HttpHeader h = http_response_writer_header(w);

    /* With no Content-Type, the extension says, or the first bytes do. A
     * Content-Type with no values at all stops both. */
    Str ct_key = BURROW_S("Content-Type");
    const Slice *ctypes = (const Slice *)map_get(h, &ct_key);
    Str ctype = BURROW_STR_EMPTY;
    if (ctypes == NULL) {
        ctype = mime_type_by_extension(filepath_ext(name));
        if (ctype.len == 0) {
            /* A block to choose between UTF-8 text and binary. */
            Byte buf[BURROW__HTTP_SNIFF_LEN];
            Error re = BURROW_NO_ERROR;
            Int n = io_read_full(c->reader,
                                 slice_from(buf, BURROW__HTTP_SNIFF_LEN,
                                            BURROW__HTTP_SNIFF_LEN, TYPE_BYTE),
                                 &re);
            if (n < 0)
                n = 0;
            ctype = http_detect_content_type(slice_from(buf, n, n, TYPE_BYTE));
            Error se = BURROW_NO_ERROR;
            (void)c->seek(c->self, 0, BURROW_IO_SEEK_START,
                          &se); /* back for the rest */
            if (BURROW_FAILED(se)) {
                hd_serve_error(w, hd_text_seeker, HTTP_STATUS_INTERNAL_SERVER_ERROR);
                return;
            }
        }
        hd_set(h, ct_key, ctype);
    } else if (ctypes->len > 0) {
        ctype = ((const Str *)ctypes->p)[0];
    }

    int64_t size = c->size;
    if (!c->have_size) {
        /* The error text is not the seeker's, which is not for users. */
        Error se = BURROW_NO_ERROR;
        size = c->seek(c->self, 0, BURROW_IO_SEEK_END, &se);
        if (BURROW_OK(se))
            (void)c->seek(c->self, 0, BURROW_IO_SEEK_START, &se);
        if (BURROW_FAILED(se)) {
            hd_serve_error(w, hd_text_seeker, HTTP_STATUS_INTERNAL_SERVER_ERROR);
            return;
        }
    }
    if (size < 0) {
        /* Should never happen, as Go says. */
        hd_serve_error(w, BURROW_S("negative content size computed"),
                       HTTP_STATUS_INTERNAL_SERVER_ERROR);
        return;
    }

    int64_t send_size = size;
    burrow__HttpRange *ranges;
    Error pe = BURROW_NO_ERROR;
    Int nranges = burrow__http_parse_range(t, range_req, size, &ranges, &pe);
    if (BURROW_FAILED(pe)) {
        if (!errors_is(pe, hd_error(&hd_text_no_overlap))) {
            hd_serve_error(w, error_text(pe),
                           HTTP_STATUS_REQUESTED_RANGE_NOT_SATISFIABLE);
            return;
        }
        if (size != 0) {
            hd_set(h, BURROW_S("Content-Range"),
                   fmt_sprintf_v(burrow__map_allocator(h), "bytes */%d", size));
            hd_serve_error(w, error_text(pe),
                           HTTP_STATUS_REQUESTED_RANGE_NOT_SATISFIABLE);
            return;
        }
        /* Some clients send a Range with every request to cap the size of the
         * response. An empty file gets a 200 rather than a 416. */
        nranges = 0;
    }

    if (hd_sum_ranges_size(ranges, nranges) > size) {
        /* More bytes in the ranges than in the file, which is an attack or a
         * client that does not know better. The ranges are ignored. */
        nranges = 0;
    }
    MultipartWriter *mw = NULL;
    if (nranges == 1) {
        /* RFC 7233 section 4.1: one range has a Content-Range field and is not
         * multipart, since a client that asks for one range may not know what
         * to do with a multipart response. */
        burrow__HttpRange ra = ranges[0];
        Error se = BURROW_NO_ERROR;
        (void)c->seek(c->self, ra.start, BURROW_IO_SEEK_START, &se);
        if (BURROW_FAILED(se)) {
            hd_serve_error(w, error_text(se),
                           HTTP_STATUS_REQUESTED_RANGE_NOT_SATISFIABLE);
            return;
        }
        send_size = ra.length;
        code = HTTP_STATUS_PARTIAL_CONTENT;
        hd_set(h, BURROW_S("Content-Range"),
               hd_content_range(burrow__map_allocator(h), ra, size));
    } else if (nranges > 1) {
        send_size = hd_ranges_mime_size(t, ranges, nranges, ctype, size);
        code = HTTP_STATUS_PARTIAL_CONTENT;
        mw = multipart_new_writer(t, http_response_writer_as_io_writer(w));
        if (mw == NULL)
            hd_out_of_memory();
        hd_set(h, ct_key,
               hd_header_value(h, hd_cat(t, BURROW_S("multipart/byteranges; boundary="),
                                         multipart_writer_boundary(mw))));
    }

    hd_set(h, BURROW_S("Accept-Ranges"), BURROW_S("bytes"));

    /* Content-Length could always be set here, but some people wrap the writer
     * in one that compresses, and set Content-Encoding, and a length would
     * break that. So with a Content-Encoding it is left out, unless this is a
     * range request, where it is always set. */
    if (nranges > 0 || http_header_get(h, BURROW_S("Content-Encoding")).len == 0)
        hd_set(h, BURROW_S("Content-Length"),
               hd_header_value(h, strconv_format_int(t, send_size, 10)));
    http_response_writer_write_header(w, code);

    if (!str_eq(r->method, BURROW_S("HEAD"))) {
        if (mw != NULL) {
            hd_write_ranges(mw, t, c, ranges, nranges, ctype, size);
        } else {
            Error ce = BURROW_NO_ERROR;
            (void)io_copy_n(t, http_response_writer_as_io_writer(w), c->reader,
                            send_size, &ce);
        }
    }
    if (mw != NULL)
        multipart_writer_free(mw);
}

void http_serve_content(HttpResponseWriter w, HttpRequest *r, Str name, Time modtime,
                        IoReadSeeker content) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    hd_Content c = {io_read_seeker_as_io_reader(content), content.data,
                    content.vt->seeker.seek, 0, false};
    BURROW_SCOPE {
        BURROW_DEFER(hd_arena_free, &ar);
        hd_serve_content(w, r, name, modtime, &c, arena_allocator(&ar));
    }
    BURROW_SCOPE_END;
}

/* ------------------------------------------------------------- serveFile */

/* localRedirect, a 301 to new_path as it is, without making it absolute the
 * way http_redirect does. */
static void hd_local_redirect(HttpResponseWriter w, HttpRequest *r, Str new_path,
                              Alloc *t) {
    /* With escaped slashes in the path there is no telling where the redirect
     * should go, since StripPrefix may be in use, so it is a 404. */
    Url zero;
    memset(&zero, 0, sizeof zero);
    Str p = url_escaped_path(r->url != NULL ? r->url : &zero, t);
    if (strings_contains(p, BURROW_S("%2f")) || strings_contains(p, BURROW_S("%2F"))) {
        http_not_found(w, r);
        return;
    }
    if (r->url != NULL && r->url->raw_query.len > 0)
        new_path = hd_cat(t, hd_cat(t, new_path, BURROW_S("?")), r->url->raw_query);
    HttpHeader h = http_response_writer_header(w);
    hd_set(h, BURROW_S("Location"), hd_header_value(h, new_path));
    http_response_writer_write_header(w, HTTP_STATUS_MOVED_PERMANENTLY);
}

static int hd_dir_entry_cmp(void *env, const void *x, const void *y) {
    (void)env;
    const FsDirEntry *a = (const FsDirEntry *)x;
    const FsDirEntry *b = (const FsDirEntry *)y;
    return str_cmp(a->vt->name(a->data), b->vt->name(b->data));
}

static int hd_file_info_cmp(void *env, const void *x, const void *y) {
    (void)env;
    const FsFileInfo *a = (const FsFileInfo *)x;
    const FsFileInfo *b = (const FsFileInfo *)y;
    return str_cmp(a->vt->name(a->data), b->vt->name(b->data));
}

static void hd_write(IoWriter w, Str s) {
    Error e = BURROW_NO_ERROR;
    (void)io_write_string(w, s, &e);
}

/* dirList. The entries of f, sorted by name, as links in an HTML page. ReadDir
 * is used when f has it, since it does not need a stat of every entry. */
static void hd_dir_list(HttpResponseWriter w, HttpFile f, Alloc *t) {
    bool entries = f.vt->read_dir != NULL;
    Error e = BURROW_NO_ERROR;
    Slice dirs =
        entries ? f.vt->read_dir(f.data, t, -1, &e) : f.vt->readdir(f.data, t, -1, &e);
    if (BURROW_FAILED(e)) {
        log_printf_v("http: error reading directory: %v", e);
        http_error(w, BURROW_S("Error reading directory"),
                   HTTP_STATUS_INTERNAL_SERVER_ERROR);
        return;
    }
    slices_sort_func(
        dirs,
        BURROW_FN(SlicesCmpFunc, entries ? hd_dir_entry_cmp : hd_file_info_cmp, NULL));

    HttpHeader h = http_response_writer_header(w);
    hd_set(h, BURROW_S("Content-Type"), BURROW_S("text/html; charset=utf-8"));
    IoWriter iw = http_response_writer_as_io_writer(w);
    hd_write(iw, BURROW_S("<!doctype html>\n"));
    hd_write(iw, BURROW_S("<meta name=\"viewport\" content=\"width=device-width\">\n"));
    hd_write(iw, BURROW_S("<pre>\n"));
    for (Int i = 0; i < dirs.len; i++) {
        Str name;
        bool is_dir;
        if (entries) {
            const FsDirEntry *d = (const FsDirEntry *)dirs.p + i;
            name = d->vt->name(d->data);
            is_dir = d->vt->is_dir(d->data);
        } else {
            const FsFileInfo *d = (const FsFileInfo *)dirs.p + i;
            name = d->vt->name(d->data);
            is_dir = d->vt->is_dir(d->data);
        }
        if (is_dir)
            name = hd_cat(t, name, BURROW_S("/"));
        /* name may have a '?' or a '#', which have to be escaped to stay in
         * the path rather than start a query or a fragment. */
        Url u;
        memset(&u, 0, sizeof u);
        u.path = name;
        Str href = url_string(&u, t);
        hd_write(iw, BURROW_S("<a href=\""));
        hd_write(iw, href);
        hd_write(iw, BURROW_S("\">"));
        hd_write(iw, burrow__http_html_escape(t, name));
        hd_write(iw, BURROW_S("</a>\n"));
    }
    hd_write(iw, BURROW_S("</pre>\n"));
}

static void hd_file_close(void *p) {
    const HttpFile *f = (const HttpFile *)p;
    (void)f->vt->read_closer.closer.close(f->data);
}

static bool hd_is_dir(FsFileInfo fi) {
    return fi.vt->is_dir(fi.data);
}

static bool hd_has_slash_suffix(Str s) {
    return s.len > 0 && s.p[s.len - 1] == '/';
}

/* serveFile. name is '/'-separated, whatever the system's separator is. */
static void hd_serve_file(HttpResponseWriter w, HttpRequest *r, HttpFileSystem fs,
                          Str name, bool redirect, Alloc *t) {
    const Str index_page = BURROW_S("/index.html");
    Str upath = hd_url_path(r);

    /* .../index.html goes to .../, which cannot be http_redirect since that
     * makes the path absolute and StripPrefix may be in use. */
    if (strings_has_suffix(upath, index_page)) {
        hd_local_redirect(w, r, BURROW_S("./"), t);
        return;
    }

    BURROW_SCOPE {
        HttpFile f;
        HttpFile ff;
        Error e = BURROW_NO_ERROR;
        f = fs.vt->open(fs.data, t, name, &e);
        if (BURROW_FAILED(e)) {
            hd_serve_http_error(w, e);
            return;
        }
        BURROW_DEFER(hd_file_close, &f);

        FsFileInfo d = f.vt->stat(f.data, t, &e);
        if (BURROW_FAILED(e)) {
            hd_serve_http_error(w, e);
            return;
        }

        if (redirect) {
            /* To the canonical path, with a '/' at the end for a directory and
             * none for a file. The path starts with '/'. */
            if (hd_is_dir(d)) {
                if (!hd_has_slash_suffix(upath)) {
                    hd_local_redirect(w, r, hd_cat(t, path_base(upath), BURROW_S("/")),
                                      t);
                    return;
                }
            } else if (hd_has_slash_suffix(upath)) {
                Str base = path_base(upath);
                if (str_eq(base, BURROW_S("/")) || str_eq(base, BURROW_S("."))) {
                    /* The FileSystem has "/" or "/./" as a file and not a
                     * directory. */
                    hd_serve_error(
                        w, BURROW_S("http: attempting to traverse a non-directory"),
                        HTTP_STATUS_INTERNAL_SERVER_ERROR);
                    return;
                }
                hd_local_redirect(w, r, hd_cat(t, BURROW_S("../"), base), t);
                return;
            }
        }

        HttpFile cur = f;
        if (hd_is_dir(d)) {
            /* A directory whose name does not end in a slash is redirected. */
            if (!hd_has_slash_suffix(upath)) {
                hd_local_redirect(w, r, hd_cat(t, path_base(upath), BURROW_S("/")), t);
                return;
            }

            /* index.html stands for the directory when there is one. */
            Str index = hd_cat(t, strings_trim_suffix(name, BURROW_S("/")), index_page);
            Error ie = BURROW_NO_ERROR;
            ff = fs.vt->open(fs.data, t, index, &ie);
            if (BURROW_OK(ie)) {
                BURROW_DEFER(hd_file_close, &ff);
                FsFileInfo dd = ff.vt->stat(ff.data, t, &ie);
                if (BURROW_OK(ie)) {
                    d = dd;
                    cur = ff;
                }
            }
        }

        /* Still a directory, so there was no index.html. */
        if (hd_is_dir(d)) {
            Time mt = d.vt->mod_time(d.data);
            if (hd_check_if_modified_since(r, mt, t) == HD_COND_FALSE) {
                hd_write_not_modified(w);
                return;
            }
            hd_set_last_modified(w, mt);
            hd_dir_list(w, cur, t);
            return;
        }

        /* serveContent checks the modification time. */
        hd_Content c = {{&cur.vt->read_closer.reader, cur.data},
                        cur.data,
                        cur.vt->seek,
                        d.vt->size(d.data),
                        true};
        hd_serve_content(w, r, d.vt->name(d.data), d.vt->mod_time(d.data), &c, t);
    }
    BURROW_SCOPE_END;
}

void burrow__http_serve_file(HttpResponseWriter w, HttpRequest *r, HttpFileSystem fs,
                             Str name, bool redirect) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    BURROW_SCOPE {
        BURROW_DEFER(hd_arena_free, &ar);
        hd_serve_file(w, r, fs, name, redirect, arena_allocator(&ar));
    }
    BURROW_SCOPE_END;
}

/* containsDotDot. Whether v has a ".." element, with '/' and '\\' both taken
 * as separators. */
static bool hd_contains_dot_dot(Str v) {
    if (!strings_contains(v, BURROW_S("..")))
        return false;
    Int from = 0;
    for (Int i = 0; i <= v.len; i++) {
        if (i < v.len && v.p[i] != '/' && v.p[i] != '\\')
            continue;
        if (i - from == 2 && v.p[from] == '.' && v.p[from + 1] == '.')
            return true;
        from = i + 1;
    }
    return false;
}

void http_serve_file(HttpResponseWriter w, HttpRequest *r, Str name) {
    /* Too many programs make name from the request path, so a ".." in the path
     * is refused, even though name may not have one. */
    if (hd_contains_dot_dot(hd_url_path(r))) {
        hd_serve_error(w, BURROW_S("invalid URL path"), HTTP_STATUS_BAD_REQUEST);
        return;
    }
    Str file;
    HttpDir dir = filepath_split(name, &file);
    burrow__http_serve_file(w, r, http_dir_as_file_system(&dir), file, false);
}

void http_serve_file_fs(HttpResponseWriter w, HttpRequest *r, Fs fsys, Str name) {
    if (hd_contains_dot_dot(hd_url_path(r))) {
        hd_serve_error(w, BURROW_S("invalid URL path"), HTTP_STATUS_BAD_REQUEST);
        return;
    }
    hd_IoFS s = {fsys};
    burrow__http_serve_file(w, r, (HttpFileSystem){&hd_io_fs_vt, &s}, name, false);
}

/* ---------------------------------------------------------- FileServer */

typedef struct hd_FileHandler {
    HttpFileSystem root;
} hd_FileHandler;

static const Type hd_file_handler_desc = {
    {(const Byte *)"fileHandler", 11},
    {(const Byte *)"net/http", 8},
    KIND_STRUCT,
    (uint32_t)sizeof(hd_FileHandler),
    (uint16_t)_Alignof(hd_FileHandler),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x68666864U, /* "hfhd" */
    NULL,
};

/* The request path as it was, put back when the handler is done. */
typedef struct hd_SavedPath {
    Url *url;
    Str path;
} hd_SavedPath;

static void hd_restore_path(void *p) {
    const hd_SavedPath *s = (const hd_SavedPath *)p;
    s->url->path = s->path;
}

static void hd_file_handler_serve(void *self, HttpResponseWriter w, HttpRequest *r) {
    const hd_FileHandler *fh = (const hd_FileHandler *)self;
    if (r->url == NULL) {
        http_not_found(w, r);
        return;
    }
    Arena ar;
    arena_init(&ar, NULL, 0);
    hd_SavedPath saved = {r->url, r->url->path};
    BURROW_SCOPE {
        BURROW_DEFER(hd_arena_free, &ar);
        Alloc *t = arena_allocator(&ar);
        Str upath = r->url->path;
        if (!strings_has_prefix(upath, BURROW_S("/"))) {
            /* The new path lasts as long as the request when the request has an
             * arena to put it in, as it does in Go, and only while the handler
             * runs when it does not. */
            if (r->a != NULL) {
                upath = hd_cat(arena_allocator(&r->arena), BURROW_S("/"), upath);
            } else {
                upath = hd_cat(t, BURROW_S("/"), upath);
                BURROW_DEFER(hd_restore_path, &saved);
            }
            r->url->path = upath;
        }
        hd_serve_file(w, r, fh->root, path_clean(t, upath), true, t);
    }
    BURROW_SCOPE_END;
}

static const HttpHandlerVT hd_file_handler_vt = {&hd_file_handler_desc,
                                                 hd_file_handler_serve};

HttpHandler http_file_server(Alloc *a, HttpFileSystem root) {
    hd_FileHandler *fh =
        (hd_FileHandler *)mem_alloc(a, sizeof *fh, _Alignof(hd_FileHandler));
    if (fh == NULL)
        return (HttpHandler){&hd_file_handler_vt, NULL};
    fh->root = root;
    return (HttpHandler){&hd_file_handler_vt, fh};
}

HttpHandler http_file_server_fs(Alloc *a, Fs root) {
    HttpFileSystem fs = http_fs(a, root);
    if (fs.data == NULL)
        return (HttpHandler){&hd_file_handler_vt, NULL};
    return http_file_server(a, fs);
}

/* mime/multipart: a port of Go's multipart.go, formdata.go and writer.go.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/mime/multipart.h"
#include "burrow/atomic.h"
#include "burrow/bufio.h"
#include "burrow/bytes.h"
#include "burrow/fmt.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/mime.h"
#include "burrow/mime/quotedprintable.h"
#include "burrow/pal.h"
#include "burrow/panic.h"
#include "burrow/sort.h"
#include "burrow/strconv.h"
#include "burrow/strings.h"

#include "internal.h"

#include <stdint.h>
#include <string.h>

static bool mp_same_error(Error a, Error b) {
    return a.vt == b.vt && a.data == b.data;
}

static Slice mp_slice(const Byte *p, Int n) {
    return slice_from((void *)(uintptr_t)p, n, n, TYPE_BYTE);
}

static Str mp_str(Slice s) {
    return (Str){(const Byte *)s.p, s.len};
}

static const Str mp_too_large__text = {(const Byte *)"multipart: message too large",
                                       28};
const Error multipart_err_message_too_large = {&burrow_sentinel_error_vt,
                                               &mp_too_large__text};

/* ------------------------------------------------------------------ GODEBUG */

enum {
    MP_DEBUG_KNOWN = 1 << 0,
    MP_DEBUG_DISTINCT = 1 << 1,    /* #multipartfiles=distinct */
    MP_DEBUG_HEADERS_SET = 1 << 2, /* multipartmaxheaders is a number */
    MP_DEBUG_PARTS_SET = 1 << 3,   /* multipartmaxparts is a number */
};

static uint32_t mp_debug_flags;
static uint64_t mp_debug_max_headers;
static uint64_t mp_debug_max_parts;

/* The value of key in GODEBUG, the last one when it is there twice. */
static bool mp_godebug(const char *env, const char *key, Str *val) {
    size_t kl = strlen(key);
    bool found = false;
    const char *p = env;
    while (*p != '\0') {
        const char *end = strchr(p, ',');
        if (end == NULL)
            end = p + strlen(p);
        if ((size_t)(end - p) > kl && memcmp(p, key, kl) == 0 && p[kl] == '=') {
            *val = str_from_bytes(p + kl + 1, (Int)(end - p - (ptrdiff_t)kl - 1));
            found = true;
        }
        p = *end == ',' ? end + 1 : end;
    }
    return found;
}

static uint32_t mp_debug_parse(const char *v) {
    uint32_t f = MP_DEBUG_KNOWN;
    uint64_t headers = 0, parts = 0;
    Str s;
    if (v != NULL && mp_godebug(v, "multipartfiles", &s) &&
        str_eq(s, BURROW_S("distinct")))
        f |= MP_DEBUG_DISTINCT;
    if (v != NULL && mp_godebug(v, "multipartmaxheaders", &s) && s.len > 0) {
        /* strconv.ParseInt, and a value that is not a number or is negative
         * leaves the default in place. */
        Error err;
        int64_t n = strconv_parse_int(s, 10, 64, &err);
        if (BURROW_OK(err) && n >= 0) {
            f |= MP_DEBUG_HEADERS_SET;
            headers = (uint64_t)n;
        }
    }
    if (v != NULL && mp_godebug(v, "multipartmaxparts", &s) && s.len > 0) {
        Error err;
        int64_t n = strconv_atoi(s, &err);
        if (BURROW_OK(err) && n >= 0) {
            f |= MP_DEBUG_PARTS_SET;
            parts = (uint64_t)n;
        }
    }
    burrow__atomic64_store(&mp_debug_max_headers, headers);
    burrow__atomic64_store(&mp_debug_max_parts, parts);
    burrow__atomic_store_relaxed_u32(&mp_debug_flags, f);
    return f;
}

static uint32_t mp_debug_load(void) {
    uint32_t f = burrow__atomic_load_relaxed_u32(&mp_debug_flags);
    if ((f & MP_DEBUG_KNOWN) != 0)
        return f;
    const char *v = NULL;
    for (const char *const *env = pal_environ(); env != NULL && *env != NULL; env++) {
        if (strncmp(*env, "GODEBUG=", 8) == 0) {
            v = *env + 8;
            break;
        }
    }
    return mp_debug_parse(v);
}

void burrow__multipart_godebug_set(const char *value) {
    if (value == NULL)
        burrow__atomic_store_relaxed_u32(&mp_debug_flags, 0);
    else
        (void)mp_debug_parse(value);
}

enum { MP_MAX_MIME_HEADER_SIZE = 10 << 20, MP_PEEK_BUFFER_SIZE = 4096 };

static int64_t mp_max_mime_headers(void) {
    uint32_t f = mp_debug_load();
    if ((f & MP_DEBUG_HEADERS_SET) != 0)
        return (int64_t)burrow__atomic64_load(&mp_debug_max_headers);
    return 10000;
}

/* ---------------------------------------------------------------- the types */

#define MP_DESC(var, name, T, hash)                                                    \
    static const Type var = {                                                          \
        {(const Byte *)name, (Int)(sizeof name - 1)},                                  \
        {(const Byte *)"mime/multipart", 14},                                          \
        KIND_STRUCT,                                                                   \
        (uint32_t)sizeof(T),                                                           \
        (uint16_t)_Alignof(T),                                                         \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
        NULL,                                                                          \
        NULL,                                                                          \
        NULL,                                                                          \
        0,                                                                             \
        hash,                                                                          \
        NULL,                                                                          \
    }

struct MultipartReader {
    Alloc *a;
    IoReader src;
    Error sticky; /* stickyErrorReader's err */
    BufioReader *br;
    const char *temp_dir; /* for the tests, NULL for the system's */

    MultipartPart *current;
    Int parts_read;
    Arena part_arena; /* the current part and all it holds, reset for the next */

    Byte *b; /* "\r\n--" boundary "--", which the slices below point into */
    Int b_len;
    Slice nl;                 /* "\r\n" or "\n", once the first boundary is seen */
    Slice nl_dash_boundary;   /* nl + "--boundary" */
    Slice dash_boundary_dash; /* "--boundary--" */
    Slice dash_boundary;      /* "--boundary" */
};

struct MultipartPart {
    Alloc *pa;
    TextprotoMIMEHeader header;
    MultipartReader *mr;

    bool parsed; /* disposition and disposition_params are set */
    Str disposition;
    Map *disposition_params; /* NULL for none */

    QuotedprintableReader *qp; /* the body goes through this when set */

    Int n;          /* known data bytes waiting in mr->br */
    int64_t total;  /* total data bytes read already */
    Error err;      /* error to return when n == 0 */
    Error read_err; /* read error seen while peeking */
};

MP_DESC(mp_reader_desc, "Reader", MultipartReader, 0x6d707264U);
MP_DESC(mp_part_desc, "Part", MultipartPart, 0x6d707074U);
MP_DESC(mp_file_header_desc, "FileHeader", MultipartFileHeader, 0x6d706668U);
MP_DESC(mp_form_desc, "Form", MultipartForm, 0x6d70666dU);

const Type *const TYPE_MULTIPART_READER = &mp_reader_desc;
const Type *const TYPE_MULTIPART_PART = &mp_part_desc;
const Type *const TYPE_MULTIPART_FILE_HEADER = &mp_file_header_desc;
const Type *const TYPE_MULTIPART_FORM = &mp_form_desc;

/* []string and []*FileHeader, the values of the form's two maps. */
static const Type mp_strings_desc = {
    {NULL, 0},
    {NULL, 0},
    KIND_SLICE,
    (uint32_t)sizeof(Slice),
    (uint16_t)_Alignof(Slice),
    0,
    0,
    NULL,
    NULL,
    TYPE_STRING,
    NULL,
    0,
    0x6d707373U,
    NULL,
};

static const Type mp_fh_ptr_desc = {
    {NULL, 0},
    {NULL, 0},
    KIND_POINTER,
    (uint32_t)sizeof(void *),
    (uint16_t)_Alignof(void *),
    0,
    0,
    NULL,
    NULL,
    &mp_file_header_desc,
    NULL,
    0,
    0x6d706670U,
    NULL,
};

static const Type mp_fhs_desc = {
    {NULL, 0},
    {NULL, 0},
    KIND_SLICE,
    (uint32_t)sizeof(Slice),
    (uint16_t)_Alignof(Slice),
    0,
    0,
    NULL,
    NULL,
    &mp_fh_ptr_desc,
    NULL,
    0,
    0x6d706673U,
    NULL,
};

/* ------------------------------------------------------------------ reading */

TextprotoMIMEHeader multipart_part_header(const MultipartPart *p) {
    return p->header;
}

static void mp_parse_content_disposition(MultipartPart *p) {
    Str v = textproto_mime_header_get(p->header, BURROW_S("Content-Disposition"));
    Error err;
    p->disposition = mime_parse_media_type(p->pa, v, &p->disposition_params, &err);
    if (BURROW_FAILED(err))
        p->disposition_params = NULL;
    p->parsed = true;
}

static Str mp_param(MultipartPart *p, Str key) {
    if (p->disposition_params == NULL)
        return (Str){NULL, 0};
    const Str *v = (const Str *)map_get(p->disposition_params, &key);
    return v == NULL ? (Str){NULL, 0} : *v;
}

Str multipart_part_form_name(MultipartPart *p) {
    /* See https://tools.ietf.org/html/rfc2183 section 2 for EBNF of
     * Content-Disposition. */
    if (!p->parsed)
        mp_parse_content_disposition(p);
    if (!str_eq(p->disposition, BURROW_S("form-data")))
        return (Str){NULL, 0};
    return mp_param(p, BURROW_S("name"));
}

#if defined(BURROW_OS_WINDOWS)
#define MP_SEP "\\"
#else
#define MP_SEP "/"
#endif

static bool mp_is_sep(Byte c) {
#if defined(BURROW_OS_WINDOWS)
    return c == '\\' || c == '/';
#else
    return c == '/';
#endif
}

#if defined(BURROW_OS_WINDOWS)
/* The volume name code is Go's volumeNameLen from internal/filepathlite. */
static Byte mp_upper(Byte c) {
    return 'a' <= c && c <= 'z' ? (Byte)(c - ('a' - 'A')) : c;
}

static bool mp_prefix_fold(const Byte *s, Int n, const char *prefix) {
    Int m = (Int)strlen(prefix);
    if (n < m)
        return false;
    for (Int i = 0; i < m; i++) {
        Byte c = (Byte)prefix[i];
        if (mp_is_sep(c)) {
            if (!mp_is_sep(s[i]))
                return false;
        } else if (mp_upper(c) != mp_upper(s[i])) {
            return false;
        }
    }
    return n == m || mp_is_sep(s[m]);
}

static Int mp_unc_len(const Byte *p, Int n, Int prefix) {
    int count = 0;
    for (Int i = prefix; i < n; i++) {
        if (mp_is_sep(p[i]) && ++count == 2)
            return i;
    }
    return n;
}

/* A volume name with a .. element in it is not one. */
static Int mp_valid_volume_len(const Byte *p, Int n) {
    for (Int i = 0; i < n;) {
        Int j = i;
        while (j < n && !mp_is_sep(p[j]))
            j++;
        if (j - i == 2 && p[i] == '.' && p[i + 1] == '.')
            return 0;
        i = j + 1;
    }
    return n;
}

/* The length of the volume name at the start of path: a drive letter and its
 * colon, the \\host\share of a UNC path, or a \\.\ or \\?\ device path with
 * its first element. */
static Int mp_volume_len(Str path) {
    const Byte *p = path.p;
    Int n = path.len;
    if (n >= 2 && p[1] == ':')
        return 2;
    if (n == 0 || !mp_is_sep(p[0]))
        return 0;
    if (mp_prefix_fold(p, n, "\\\\.") || mp_prefix_fold(p, n, "\\\\?") ||
        mp_prefix_fold(p, n, "\\??")) {
        if (n == 3)
            return 3;
        if (mp_prefix_fold(p + 4, n - 4, "UNC"))
            return mp_valid_volume_len(p, mp_unc_len(p, n, 8));
        Int i = 4;
        while (i < n && !mp_is_sep(p[i]))
            i++;
        return mp_valid_volume_len(p, i);
    }
    if (n >= 2 && mp_is_sep(p[1]))
        return mp_valid_volume_len(p, mp_unc_len(p, n, 2));
    return 0;
}
#endif

/* filepath.Base. */
static Str mp_filepath_base(Str path) {
    if (path.len == 0)
        return BURROW_S(".");
    while (path.len > 0 && mp_is_sep(path.p[path.len - 1]))
        path.len--;
#if defined(BURROW_OS_WINDOWS)
    Int v = mp_volume_len(path);
    path.p += v;
    path.len -= v;
#endif
    Int i = path.len - 1;
    while (i >= 0 && !mp_is_sep(path.p[i]))
        i--;
    if (i >= 0) {
        path.p += i + 1;
        path.len -= i + 1;
    }
    if (path.len == 0) {
#if defined(BURROW_OS_WINDOWS)
        return BURROW_S("\\");
#else
        return BURROW_S("/");
#endif
    }
    return path;
}

Str multipart_part_file_name(MultipartPart *p) {
    if (!p->parsed)
        mp_parse_content_disposition(p);
    Str filename = mp_param(p, BURROW_S("filename"));
    if (filename.len == 0)
        return (Str){NULL, 0};
    /* RFC 7578, Section 4.2 requires that if a filename is provided, the
     * directory path information must not be used. */
    return mp_filepath_base(filename);
}

static Int mp_sticky_read(void *self, Slice p, Error *err) {
    MultipartReader *r = (MultipartReader *)self;
    if (BURROW_FAILED(r->sticky)) {
        *err = r->sticky;
        return 0;
    }
    Error e = BURROW_NO_ERROR;
    Int n = r->src.vt->read(r->src.data, p, &e);
    r->sticky = e;
    *err = e;
    return n;
}

static const IoReaderVT mp_sticky_vt = {&mp_reader_desc, mp_sticky_read};

MultipartReader *multipart_new_reader(Alloc *a, IoReader r, Str boundary) {
    MultipartReader *z =
        (MultipartReader *)mem_alloc(a, sizeof *z, _Alignof(MultipartReader));
    if (z == NULL)
        return NULL;
    z->a = a;
    arena_init(&z->part_arena, a, 0);
    z->src = r;
    z->b_len = boundary.len + 6;
    z->b = (Byte *)mem_alloc_nozero(a, (size_t)z->b_len, 1);
    z->br = bufio_new_reader_size(a, (IoReader){&mp_sticky_vt, z}, MP_PEEK_BUFFER_SIZE);
    if (z->b == NULL || z->br == NULL) {
        if (z->b != NULL)
            mem_free(a, z->b, (size_t)z->b_len, 1);
        bufio_reader_free(z->br);
        mem_free(a, z, sizeof *z, _Alignof(MultipartReader));
        return NULL;
    }
    memcpy(z->b, "\r\n--", 4);
    if (boundary.len > 0)
        memcpy(z->b + 4, boundary.p, (size_t)boundary.len);
    memcpy(z->b + 4 + boundary.len, "--", 2);
    z->nl = mp_slice(z->b, 2);
    z->nl_dash_boundary = mp_slice(z->b, z->b_len - 2);
    z->dash_boundary_dash = mp_slice(z->b + 2, z->b_len - 2);
    z->dash_boundary = mp_slice(z->b + 2, z->b_len - 4);
    return z;
}

void burrow__multipart_reader_set_temp_dir(MultipartReader *r, const char *dir) {
    r->temp_dir = dir;
}

static void mp_part_free(MultipartPart *p) {
    if (p == NULL)
        return;
    arena_reset(&p->mr->part_arena);
}

void multipart_reader_free(MultipartReader *r) {
    if (r == NULL)
        return;
    mp_part_free(r->current);
    arena_free(&r->part_arena);
    bufio_reader_free(r->br);
    mem_free(r->a, r->b, (size_t)r->b_len, 1);
    mem_free(r->a, r, sizeof *r, _Alignof(MultipartReader));
}

/* matchAfterPrefix checks whether buf should be considered to match the
 * boundary. The prefix is "--boundary" or "\r\n--boundary" or
 * "\n--boundary", and the caller has verified already that
 * bytes.HasPrefix(buf, prefix) is true.
 *
 * It returns +1 if the buffer does match the boundary, meaning the prefix is
 * followed by a double dash, space, tab, cr, nl, or end of input. It returns -1
 * if the buffer definitely does NOT match the boundary, meaning the prefix is
 * followed by some other character. For example, "--foobar" does not match
 * "--foo". It returns 0 more input needs to be read to make the decision,
 * meaning that len(buf) == len(prefix) and readErr == nil. */
static int mp_match_after_prefix(Slice buf, Slice prefix, Error read_err) {
    const Byte *b = (const Byte *)buf.p;
    if (buf.len == prefix.len)
        return BURROW_FAILED(read_err) ? +1 : 0;
    Byte c = b[prefix.len];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
        return +1;
    /* Try to detect boundaryDash. */
    if (c == '-') {
        if (buf.len == prefix.len + 1) {
            if (BURROW_FAILED(read_err)) {
                /* Prefix + "-" does not match. */
                return -1;
            }
            return 0;
        }
        if (b[prefix.len + 1] == '-')
            return +1;
    }
    return -1;
}

static Slice mp_tail(Slice s, Int i) {
    return mp_slice((const Byte *)s.p + i, s.len - i);
}

/* scanUntilBoundary scans buf to identify how much of it can be safely returned
 * as part of the Part body. dashBoundary is "--boundary". nlDashBoundary is
 * "\r\n--boundary" or "\n--boundary", depending on what mode we are in. The
 * comments below (and the name) assume "\n--boundary", but either is accepted.
 * total is the number of bytes read out so far. If total == 0, then a leading
 * "--boundary" is recognized. readErr is the read error, if any, that followed
 * reading the bytes in buf. scanUntilBoundary returns the number of data bytes
 * from buf that can be returned as part of the Part body and also the error to
 * return (if any) once those data bytes are done. */
static Int mp_scan_until_boundary(Slice buf, Slice dash_boundary,
                                  Slice nl_dash_boundary, int64_t total, Error read_err,
                                  Error *err) {
    *err = BURROW_NO_ERROR;
    if (total == 0) {
        /* At beginning of body, allow dashBoundary. */
        if (bytes_has_prefix(buf, dash_boundary)) {
            switch (mp_match_after_prefix(buf, dash_boundary, read_err)) {
            case -1:
                return dash_boundary.len;
            case 0:
                return 0;
            default:
                *err = io_eof;
                return 0;
            }
        }
        if (bytes_has_prefix(dash_boundary, buf)) {
            *err = read_err;
            return 0;
        }
    }

    /* Search for "\n--boundary". */
    Int i = bytes_index(buf, nl_dash_boundary);
    if (i >= 0) {
        switch (mp_match_after_prefix(mp_tail(buf, i), nl_dash_boundary, read_err)) {
        case -1:
            return i + nl_dash_boundary.len;
        case 0:
            return i;
        default:
            *err = io_eof;
            return i;
        }
    }
    if (bytes_has_prefix(nl_dash_boundary, buf)) {
        *err = read_err;
        return 0;
    }

    /* Otherwise, anything up to the final \n is not part of the boundary and
     * so must be part of the body. Also if the section from the final \n
     * onward is not a prefix of the boundary, it too must be part of the
     * body. */
    i = bytes_last_index_byte(buf, ((const Byte *)nl_dash_boundary.p)[0]);
    if (i >= 0 && bytes_has_prefix(nl_dash_boundary, mp_tail(buf, i)))
        return i;
    *err = read_err;
    return buf.len;
}

/* partReader.Read: the body of the part, up to the boundary. */
static Int mp_part_raw_read(MultipartPart *p, Slice d, Error *err) {
    BufioReader *br = p->mr->br;

    /* Read into buffer until we identify some data to return, or we find a
     * reason to stop (boundary or read error). */
    while (p->n == 0 && BURROW_OK(p->err)) {
        Error e;
        Slice peek = bufio_reader_peek(br, bufio_reader_buffered(br), &e);
        p->n =
            mp_scan_until_boundary(peek, p->mr->dash_boundary, p->mr->nl_dash_boundary,
                                   p->total, p->read_err, &p->err);
        if (p->n == 0 && BURROW_OK(p->err)) {
            /* Force buffered I/O to read more into buffer. */
            (void)bufio_reader_peek(br, peek.len + 1, &p->read_err);
            if (mp_same_error(p->read_err, io_eof))
                p->read_err = io_err_unexpected_eof;
        }
    }

    /* Read out from "data to return" part of buffer. */
    if (p->n == 0) {
        *err = p->err;
        return 0;
    }
    Int n = d.len;
    if (n > p->n)
        n = p->n;
    Error e;
    n = bufio_reader_read(br, mp_slice((const Byte *)d.p, n), &e);
    p->total += n;
    p->n -= n;
    *err = p->n == 0 ? p->err : BURROW_NO_ERROR;
    return n;
}

static Int mp_part_raw_vt_read(void *self, Slice d, Error *err) {
    return mp_part_raw_read((MultipartPart *)self, d, err);
}

static const IoReaderVT mp_part_raw_vt = {&mp_part_desc, mp_part_raw_vt_read};

Int multipart_part_read(MultipartPart *p, Slice d, Error *err) {
    if (p->qp != NULL)
        return quotedprintable_reader_read(p->qp, d, err);
    return mp_part_raw_read(p, d, err);
}

Error multipart_part_close(MultipartPart *p) {
    Byte buf[512];
    for (;;) {
        Error err = BURROW_NO_ERROR;
        (void)multipart_part_read(p, mp_slice(buf, (Int)sizeof buf), &err);
        if (BURROW_FAILED(err))
            break;
    }
    return BURROW_NO_ERROR;
}

static Int mp_part_vt_read(void *self, Slice d, Error *err) {
    return multipart_part_read((MultipartPart *)self, d, err);
}

static Error mp_part_vt_close(void *self) {
    return multipart_part_close((MultipartPart *)self);
}

static const IoReaderVT mp_part_vt = {&mp_part_desc, mp_part_vt_read};
static const IoReadCloserVT mp_part_rc_vt = {{&mp_part_desc, mp_part_vt_read},
                                             {&mp_part_desc, mp_part_vt_close}};

IoReader multipart_part_as_io_reader(MultipartPart *p) {
    return (IoReader){&mp_part_vt, p};
}

IoReadCloser multipart_part_as_io_read_closer(MultipartPart *p) {
    return (IoReadCloser){&mp_part_rc_vt, p};
}

static MultipartPart *mp_new_part(MultipartReader *mr, bool raw_part,
                                  int64_t max_mime_header_size,
                                  int64_t max_mime_headers, Error *err) {
    arena_reset(&mr->part_arena);
    Alloc *pa = arena_allocator(&mr->part_arena);
    MultipartPart *bp =
        (MultipartPart *)mem_alloc(pa, sizeof *bp, _Alignof(MultipartPart));
    if (bp == NULL) {
        *err = burrow__mime_err_no_memory;
        return NULL;
    }
    bp->pa = pa;
    bp->mr = mr;

    TextprotoReader *tr = textproto_new_reader(bp->pa, mr->br);
    if (tr == NULL) {
        mp_part_free(bp);
        *err = burrow__mime_err_no_memory;
        return NULL;
    }
    Error e = BURROW_NO_ERROR;
    TextprotoMIMEHeader header = burrow__textproto_read_mime_header(
        tr, bp->pa, max_mime_header_size, max_mime_headers, &e);
    textproto_reader_free(tr);
    if (BURROW_FAILED(e)) {
        if (str_eq(error_text(e), BURROW_S("message too large")))
            e = multipart_err_message_too_large;
        mp_part_free(bp);
        *err = e;
        return NULL;
    }
    bp->header = header;

    if (!raw_part) {
        Str cte = BURROW_S("Content-Transfer-Encoding");
        if (strings_equal_fold(textproto_mime_header_get(bp->header, cte),
                               BURROW_S("quoted-printable"))) {
            textproto_mime_header_del(bp->header, cte);
            bp->qp =
                quotedprintable_new_reader(bp->pa, (IoReader){&mp_part_raw_vt, bp});
            if (bp->qp == NULL) {
                mp_part_free(bp);
                *err = burrow__mime_err_no_memory;
                return NULL;
            }
        }
    }
    *err = BURROW_NO_ERROR;
    return bp;
}

static Slice mp_skip_lwsp_char(Slice b) {
    Int i = 0;
    const Byte *p = (const Byte *)b.p;
    while (i < b.len && (p[i] == ' ' || p[i] == '\t'))
        i++;
    return mp_tail(b, i);
}

static bool mp_is_final_boundary(MultipartReader *r, Slice line) {
    if (!bytes_has_prefix(line, r->dash_boundary_dash))
        return false;
    Slice rest = mp_skip_lwsp_char(mp_tail(line, r->dash_boundary_dash.len));
    return rest.len == 0 || bytes_equal(rest, r->nl);
}

static bool mp_is_boundary_delimiter_line(MultipartReader *r, Slice line) {
    /* https://tools.ietf.org/html/rfc2046#section-5.1
     *   The boundary delimiter line is then defined as a line consisting
     *   entirely of two hyphen characters ("-", decimal value 45) followed by
     *   the boundary parameter value from the Content-Type header field,
     *   optional linear whitespace, and a terminating CRLF. */
    if (!bytes_has_prefix(line, r->dash_boundary))
        return false;
    Slice rest = mp_skip_lwsp_char(mp_tail(line, r->dash_boundary.len));

    /* On the first part, see our lines are ending in \n instead of \r\n and
     * switch into that mode if so. This is a violation of the spec, but
     * occurs in practice. */
    if (r->parts_read == 0 && rest.len == 1 && ((const Byte *)rest.p)[0] == '\n') {
        r->nl = mp_tail(r->nl, 1);
        r->nl_dash_boundary = mp_tail(r->nl_dash_boundary, 1);
    }
    return bytes_equal(rest, r->nl);
}

static MultipartPart *mp_next_part(MultipartReader *r, bool raw_part,
                                   int64_t max_mime_header_size,
                                   int64_t max_mime_headers, Error *err) {
    if (r->current != NULL) {
        (void)multipart_part_close(r->current);
        mp_part_free(r->current);
        r->current = NULL;
    }
    if (r->dash_boundary.len == 2) {
        *err = fmt_errorf_v("multipart: boundary is empty");
        return NULL;
    }
    bool expect_new_part = false;
    for (;;) {
        Error e = BURROW_NO_ERROR;
        Slice line = bufio_reader_read_slice(r->br, '\n', &e);

        if (mp_same_error(e, io_eof) && mp_is_final_boundary(r, line)) {
            /* If the buffer ends in "--boundary--" without the trailing
             * "\r\n", ReadSlice will return an error (since it's missing the
             * '\n'), but this is a valid multipart EOF so we need to return
             * io.EOF instead of a fmt-wrapped one. */
            *err = io_eof;
            return NULL;
        }
        if (BURROW_FAILED(e)) {
            *err = fmt_errorf_v("multipart: NextPart: %w", e);
            return NULL;
        }

        if (mp_is_boundary_delimiter_line(r, line)) {
            r->parts_read++;
            MultipartPart *bp =
                mp_new_part(r, raw_part, max_mime_header_size, max_mime_headers, err);
            if (bp == NULL)
                return NULL;
            r->current = bp;
            return bp;
        }

        if (mp_is_final_boundary(r, line)) {
            /* Expected EOF. */
            *err = io_eof;
            return NULL;
        }

        if (expect_new_part) {
            *err = fmt_errorf_v("multipart: expecting a new Part; got line %q",
                                mp_str(line));
            return NULL;
        }

        if (r->parts_read == 0) {
            /* Skip line. */
            continue;
        }

        /* Consume the "\n" or "\r\n" separator between the body of the
         * previous part and the boundary line we now expect will follow. (either
         * a new part or the end boundary) */
        if (bytes_equal(line, r->nl)) {
            expect_new_part = true;
            continue;
        }

        *err = fmt_errorf_v("multipart: unexpected line in Next(): %q", mp_str(line));
        return NULL;
    }
}

MultipartPart *multipart_reader_next_part(MultipartReader *r, Error *err) {
    return mp_next_part(r, false, MP_MAX_MIME_HEADER_SIZE, mp_max_mime_headers(), err);
}

MultipartPart *multipart_reader_next_raw_part(MultipartReader *r, Error *err) {
    return mp_next_part(r, true, MP_MAX_MIME_HEADER_SIZE, mp_max_mime_headers(), err);
}

/* ---------------------------------------------------------------- the form */

typedef struct MpFormBox {
    MultipartForm form;
    Arena arena;
    Alloc *parent;
} MpFormBox;

static MpFormBox *mp_box(MultipartForm *f) {
    return (MpFormBox *)(void *)f;
}

void multipart_form_free(MultipartForm *f) {
    if (f == NULL)
        return;
    MpFormBox *b = mp_box(f);
    Alloc *a = b->parent;
    arena_free(&b->arena);
    mem_free(a, b, sizeof *b, _Alignof(MpFormBox));
}

Error multipart_form_remove_all(MultipartForm *f) {
    Error err = BURROW_NO_ERROR;
    const void *k;
    void *v;
    for (MapIter it = map_iter(f->file); map_next(&it, &k, &v);) {
        const Slice *fhs = (const Slice *)v;
        MultipartFileHeader *const *p = (MultipartFileHeader *const *)fhs->p;
        for (Int i = 0; i < fhs->len; i++) {
            if (p[i]->tmpfile == NULL)
                continue;
            PalErrno e = PAL_OK;
            if (!pal_unlink(p[i]->tmpfile, &e) && e != PAL_ENOENT && BURROW_OK(err))
                err = fmt_errorf_v("remove %s: %s", str_from_cstr(p[i]->tmpfile),
                                   str_from_cstr(pal_errno_string(e)));
        }
    }
    return err;
}

/* The in-memory size of a header, which counts against max_memory. */
static int64_t mp_mime_header_size(TextprotoMIMEHeader h) {
    int64_t size = 400;
    const void *k;
    void *v;
    for (MapIter it = map_iter(h); map_next(&it, &k, &v);) {
        size += ((const Str *)k)->len;
        size += 200; /* map entry overhead */
        const Slice *vs = (const Slice *)v;
        for (Int i = 0; i < vs->len; i++)
            size += ((const Str *)vs->p)[i].len;
    }
    return size;
}

/* A temporary file being written. */
typedef struct MpTemp {
    int64_t fd;
    const char *name;
} MpTemp;

/* os.CreateTemp(dir, "multipart-"): a new file with a random number after the
 * prefix, made with O_EXCL so it cannot be anybody else's. */
static bool mp_create_temp(Alloc *a, const char *dir, MpTemp *t, Error *err) {
    char tmp[4096];
    if (dir == NULL || dir[0] == 0) {
        PalErrno e = PAL_OK;
        if (pal_temp_dir(tmp, (int64_t)sizeof tmp, &e) < 0) {
            *err = fmt_errorf_v("createtemp: %s", str_from_cstr(pal_errno_string(e)));
            return false;
        }
        dir = tmp;
    }
    size_t dl = strlen(dir);
    bool sep = dl > 0 && mp_is_sep((Byte)dir[dl - 1]);
    PalErrno e = PAL_OK;
    for (int try_ = 0; try_ < 10000; try_++) {
        uint32_t r = 0;
        pal_random_bytes(&r, sizeof r, NULL);
        Str name = fmt_sprintf_v(a, "%s%smultipart-%d", str_from_cstr(dir),
                                 sep ? BURROW_S("") : BURROW_S(MP_SEP), (uint64_t)r);
        if (name.p == NULL) {
            *err = burrow__mime_err_no_memory;
            return false;
        }
        /* fmt_sprintf's result has a NUL after it only by luck, so copy. */
        char *c = (char *)mem_alloc_nozero(a, (size_t)name.len + 1, 1);
        if (c == NULL) {
            *err = burrow__mime_err_no_memory;
            return false;
        }
        memcpy(c, name.p, (size_t)name.len);
        c[name.len] = 0;
        e = PAL_OK;
        int64_t fd = pal_open(c, PAL_O_RDWR | PAL_O_CREATE | PAL_O_EXCL, 0600, &e);
        if (fd >= 0) {
            t->fd = fd;
            t->name = c;
            return true;
        }
        if (e != PAL_EEXIST)
            break;
    }
    *err = fmt_errorf_v("open %s%smultipart-*: %s", str_from_cstr(dir),
                        str_from_cstr(sep ? "" : MP_SEP),
                        str_from_cstr(pal_errno_string(e)));
    return false;
}

static bool mp_write_all(int64_t fd, const Byte *p, Int n, const char *name,
                         Error *err) {
    while (n > 0) {
        PalErrno e = PAL_OK;
        int64_t w = pal_write(fd, p, n, &e);
        if (w <= 0) {
            *err = fmt_errorf_v("write %s: %s", str_from_cstr(name),
                                str_from_cstr(pal_errno_string(e)));
            return false;
        }
        p += w;
        n -= (Int)w;
    }
    return true;
}

static bool mp_append(Alloc *a, Map *m, Str key, const void *elem,
                      const Type *slice_type) {
    Slice *s = (Slice *)map_get(m, &key);
    Slice ns = slice_append(
        a, s == NULL ? slice_from(NULL, 0, 0, slice_type->elem) : *s, elem, 1);
    if (ns.p == NULL)
        return false;
    return map_set(m, &key, &ns);
}

MultipartForm *multipart_reader_read_form(MultipartReader *r, int64_t max_memory,
                                          Error *err) {
    *err = BURROW_NO_ERROR;
    MpFormBox *box = (MpFormBox *)mem_alloc(r->a, sizeof *box, _Alignof(MpFormBox));
    if (box == NULL) {
        *err = burrow__mime_err_no_memory;
        return NULL;
    }
    box->parent = r->a;
    arena_init(&box->arena, r->a, 0);
    Alloc *fa = arena_allocator(&box->arena);
    MultipartForm *form = &box->form;
    form->value = map_make(fa, TYPE_STRING, &mp_strings_desc, 0);
    form->file = map_make(fa, TYPE_STRING, &mp_fhs_desc, 0);

    MpTemp file = {-1, NULL};
    int64_t file_off = 0;
    int num_disk_files = 0;
    uint32_t dbg = mp_debug_load();
    bool combine_files = (dbg & MP_DEBUG_DISTINCT) == 0;
    int64_t max_parts = 1000;
    if ((dbg & MP_DEBUG_PARTS_SET) != 0)
        max_parts = (int64_t)burrow__atomic64_load(&mp_debug_max_parts);
    int64_t max_headers = mp_max_mime_headers();

    BytesBuffer b = BYTES_BUFFER(r->a);
    Byte *copy_buf = NULL;
    enum { COPY_BUF = 32 * 1024 };
    Error e = BURROW_NO_ERROR;

    if (form->value == NULL || form->file == NULL) {
        e = burrow__mime_err_no_memory;
        goto done;
    }

    /* Reserve an additional 10 MB for non-file parts. */
    int64_t max_file_memory_bytes = max_memory;
    if (max_file_memory_bytes == INT64_MAX)
        max_file_memory_bytes--;
    int64_t max_memory_bytes;
    if (max_memory > INT64_MAX - (int64_t)(10 << 20))
        max_memory_bytes = INT64_MAX;
    else
        max_memory_bytes = max_memory + (int64_t)(10 << 20);
    if (max_memory_bytes <= 0) {
        if (max_memory < 0)
            max_memory_bytes = 0;
        else
            max_memory_bytes = INT64_MAX;
    }
    for (;;) {
        MultipartPart *p = mp_next_part(r, false, max_memory_bytes, max_headers, &e);
        if (mp_same_error(e, io_eof)) {
            e = BURROW_NO_ERROR;
            break;
        }
        if (BURROW_FAILED(e))
            goto done;
        if (max_parts <= 0) {
            e = multipart_err_message_too_large;
            goto done;
        }
        max_parts--;

        Str name = multipart_part_form_name(p);
        if (name.len == 0)
            continue;
        Str filename = multipart_part_file_name(p);

        /* Multiple values for the same key (one map entry, longer slice) are
         * cheaper than the same number of values for different keys (many
         * map entries), but using a consistent per-value cost for overhead is
         * simpler. */
        const int64_t map_entry_overhead = 200;
        max_memory_bytes -= name.len;
        max_memory_bytes -= map_entry_overhead;
        if (max_memory_bytes < 0) {
            /* We can't actually take this path, since nextPart would already
             * have rejected the MIME headers for being too large. Check anyway. */
            e = multipart_err_message_too_large;
            goto done;
        }

        bytes_buffer_reset(&b);
        IoWriter bw = bytes_buffer_as_io_writer(&b);
        IoReader pr = multipart_part_as_io_reader(p);

        if (filename.len == 0) {
            /* value, store as string in memory */
            Error ce = BURROW_NO_ERROR;
            int64_t n = io_copy_n(r->a, bw, pr, max_memory_bytes + 1, &ce);
            if (BURROW_FAILED(ce) && !mp_same_error(ce, io_eof)) {
                e = ce;
                goto done;
            }
            max_memory_bytes -= n;
            if (max_memory_bytes < 0) {
                e = multipart_err_message_too_large;
                goto done;
            }
            Str key = strings_clone(fa, name);
            Str val = strings_clone(fa, mp_str(bytes_buffer_bytes(&b)));
            if ((key.p == NULL && name.len > 0) ||
                (val.p == NULL && bytes_buffer_len(&b) > 0) ||
                !mp_append(fa, form->value, key, &val, &mp_strings_desc)) {
                e = burrow__mime_err_no_memory;
                goto done;
            }
            continue;
        }

        /* file, store in memory or on disk */
        const int64_t file_header_size = 100;
        max_memory_bytes -= mp_mime_header_size(p->header);
        max_memory_bytes -= map_entry_overhead;
        max_memory_bytes -= file_header_size;
        if (max_memory_bytes < 0) {
            e = multipart_err_message_too_large;
            goto done;
        }
        {
            const void *hk;
            void *hv;
            for (MapIter it = map_iter(p->header); map_next(&it, &hk, &hv);)
                max_headers -= ((const Slice *)hv)->len;
        }
        MultipartFileHeader *fh = (MultipartFileHeader *)mem_alloc(
            fa, sizeof *fh, _Alignof(MultipartFileHeader));
        TextprotoMIMEHeader hcopy = fh == NULL ? NULL : textproto_mime_header_make(fa);
        if (fh == NULL || hcopy == NULL) {
            e = burrow__mime_err_no_memory;
            goto done;
        }
        {
            const void *hk;
            void *hv;
            for (MapIter it = map_iter(p->header); map_next(&it, &hk, &hv);) {
                const Slice *vs = (const Slice *)hv;
                Str key = strings_clone(fa, *(const Str *)hk);
                Slice nv = slice_make(fa, TYPE_STRING, vs->len, vs->len);
                for (Int i = 0; i < vs->len; i++)
                    ((Str *)nv.p)[i] = strings_clone(fa, ((const Str *)vs->p)[i]);
                map_set(hcopy, &key, &nv);
            }
        }
        fh->filename = strings_clone(fa, filename);
        fh->header = hcopy;

        Error ce = BURROW_NO_ERROR;
        int64_t n = io_copy_n(r->a, bw, pr, max_file_memory_bytes + 1, &ce);
        if (BURROW_FAILED(ce) && !mp_same_error(ce, io_eof)) {
            e = ce;
            goto done;
        }
        if (n > max_file_memory_bytes) {
            if (file.fd < 0) {
                if (!mp_create_temp(fa, r->temp_dir, &file, &e))
                    goto done;
            }
            num_disk_files++;
            Slice head = bytes_buffer_bytes(&b);
            if (!mp_write_all(file.fd, (const Byte *)head.p, head.len, file.name, &e))
                goto done;
            if (copy_buf == NULL) {
                /* same buffer size as io.Copy uses */
                copy_buf = (Byte *)mem_alloc_nozero(r->a, COPY_BUF, 1);
                if (copy_buf == NULL) {
                    e = burrow__mime_err_no_memory;
                    goto done;
                }
            }
            int64_t remaining_size = 0;
            for (;;) {
                Error re = BURROW_NO_ERROR;
                Int got = multipart_part_read(p, mp_slice(copy_buf, COPY_BUF), &re);
                if (got > 0) {
                    if (!mp_write_all(file.fd, copy_buf, got, file.name, &e))
                        goto done;
                    remaining_size += got;
                }
                if (mp_same_error(re, io_eof))
                    break;
                if (BURROW_FAILED(re)) {
                    e = re;
                    goto done;
                }
            }
            fh->tmpfile = file.name;
            fh->size = (int64_t)head.len + remaining_size;
            fh->tmpoff = file_off;
            file_off += fh->size;
            if (!combine_files) {
                PalErrno pe = PAL_OK;
                bool ok = pal_close(file.fd, &pe);
                file.fd = -1;
                file.name = NULL;
                if (!ok) {
                    e = fmt_errorf_v("close %s: %s", str_from_cstr(fh->tmpfile),
                                     str_from_cstr(pal_errno_string(pe)));
                    goto done;
                }
            }
        } else {
            Slice got = bytes_buffer_bytes(&b);
            Byte *c = (Byte *)mem_alloc_nozero(fa, (size_t)got.len + 1, 1);
            if (c == NULL) {
                e = burrow__mime_err_no_memory;
                goto done;
            }
            if (got.len > 0)
                memcpy(c, got.p, (size_t)got.len);
            fh->content = mp_slice(c, got.len);
            fh->size = got.len;
            max_file_memory_bytes -= n;
            max_memory_bytes -= n;
        }
        Str key = strings_clone(fa, name);
        if (!mp_append(fa, form->file, key, &fh, &mp_fhs_desc)) {
            e = burrow__mime_err_no_memory;
            goto done;
        }
    }

done:
    bytes_buffer_free(&b);
    if (copy_buf != NULL)
        mem_free(r->a, copy_buf, COPY_BUF, 1);
    if (file.fd >= 0) {
        PalErrno pe = PAL_OK;
        if (!pal_close(file.fd, &pe) && BURROW_OK(e))
            e = fmt_errorf_v("close %s: %s", str_from_cstr(file.name),
                             str_from_cstr(pal_errno_string(pe)));
    }
    if (combine_files && num_disk_files > 1 && form->file != NULL) {
        const void *k;
        void *v;
        for (MapIter it = map_iter(form->file); map_next(&it, &k, &v);) {
            const Slice *fhs = (const Slice *)v;
            for (Int i = 0; i < fhs->len; i++)
                ((MultipartFileHeader **)fhs->p)[i]->tmpshared = true;
        }
    }
    if (BURROW_FAILED(e)) {
        if (form->file != NULL)
            (void)multipart_form_remove_all(form);
        if (file.name != NULL)
            pal_unlink(file.name, NULL);
        multipart_form_free(form);
        *err = e;
        return NULL;
    }
    return form;
}

/* ---------------------------------------------------------------- the file */

struct MultipartFile {
    Alloc *a;
    int64_t fd; /* -1 when the body is in memory */
    BytesReader mem;
    IoSectionReader sr; /* the body, unless whole is set */
    bool whole;         /* the body is all of fd, read like an os.File */
    int64_t pos;        /* where the next read starts, when whole is set */
    const char *name;
};

MP_DESC(mp_file_desc, "File", MultipartFile, 0x6d70666cU);
const Type *const TYPE_MULTIPART_FILE = &mp_file_desc;

static Int mp_fd_read_at(void *self, Slice p, int64_t off, Error *err) {
    MultipartFile *f = (MultipartFile *)self;
    Int n = 0;
    *err = BURROW_NO_ERROR;
    if (off < 0) {
        *err = fmt_errorf_v("readat %s: negative offset", str_from_cstr(f->name));
        return 0;
    }
    while (n < p.len) {
        PalErrno e = PAL_OK;
        int64_t got = pal_pread(f->fd, (Byte *)p.p + n, p.len - n, off + n, &e);
        if (got < 0) {
            *err = fmt_errorf_v("read %s: %s", str_from_cstr(f->name),
                                str_from_cstr(pal_errno_string(e)));
            return n;
        }
        if (got == 0) {
            *err = io_eof;
            return n;
        }
        n += (Int)got;
    }
    return n;
}

static const IoReaderAtVT mp_fd_reader_at_vt = {&mp_file_desc, mp_fd_read_at};

MultipartFile *multipart_file_header_open(const MultipartFileHeader *fh, Alloc *a,
                                          Error *err) {
    *err = BURROW_NO_ERROR;
    MultipartFile *f =
        (MultipartFile *)mem_alloc(a, sizeof *f, _Alignof(MultipartFile));
    if (f == NULL) {
        *err = burrow__mime_err_no_memory;
        return NULL;
    }
    f->a = a;
    f->fd = -1;
    if (fh->tmpfile == NULL) {
        bytes_reader_reset(&f->mem, fh->content);
        f->sr = io_new_section_reader(bytes_reader_as_io_reader_at(&f->mem), 0,
                                      fh->content.len);
        return f;
    }
    PalErrno e = PAL_OK;
    f->fd = pal_open(fh->tmpfile, PAL_O_RDONLY, 0, &e);
    if (f->fd < 0) {
        *err = fmt_errorf_v("open %s: %s", str_from_cstr(fh->tmpfile),
                            str_from_cstr(pal_errno_string(e)));
        mem_free(a, f, sizeof *f, _Alignof(MultipartFile));
        return NULL;
    }
    f->name = fh->tmpfile;
    if (!fh->tmpshared) {
        /* Go hands back the *os.File itself, so this reads and seeks the way
         * one does. */
        f->whole = true;
        return f;
    }
    f->sr = io_new_section_reader((IoReaderAt){&mp_fd_reader_at_vt, f}, fh->tmpoff,
                                  fh->size);
    return f;
}

static Error mp_file_error(MultipartFile *f, const char *op, PalErrno e) {
    return fmt_errorf_v("%s %s: %s", str_from_cstr(op), str_from_cstr(f->name),
                        str_from_cstr(pal_errno_string(e)));
}

Int multipart_file_read(MultipartFile *f, Slice p, Error *err) {
    if (!f->whole)
        return io_section_reader_read(&f->sr, p, err);
    *err = BURROW_NO_ERROR;
    if (p.len == 0)
        return 0;
    PalErrno e = PAL_OK;
    int64_t got = pal_pread(f->fd, p.p, p.len, f->pos, &e);
    if (got < 0) {
        *err = mp_file_error(f, "read", e);
        return 0;
    }
    if (got == 0) {
        *err = io_eof;
        return 0;
    }
    f->pos += got;
    return (Int)got;
}

Int multipart_file_read_at(MultipartFile *f, Slice p, int64_t off, Error *err) {
    if (!f->whole)
        return io_section_reader_read_at(&f->sr, p, off, err);
    return mp_fd_read_at(f, p, off, err);
}

int64_t multipart_file_seek(MultipartFile *f, int64_t offset, Int whence, Error *err) {
    if (!f->whole)
        return io_section_reader_seek(&f->sr, offset, whence, err);
    *err = BURROW_NO_ERROR;
    int64_t base;
    switch (whence) {
    case BURROW_IO_SEEK_START:
        base = 0;
        break;
    case BURROW_IO_SEEK_CURRENT:
        base = f->pos;
        break;
    case BURROW_IO_SEEK_END: {
        PalStat st;
        PalErrno e = PAL_OK;
        if (!pal_fstat(f->fd, &st, &e)) {
            *err = mp_file_error(f, "seek", e);
            return 0;
        }
        base = st.size;
        break;
    }
    default:
        *err = mp_file_error(f, "seek", PAL_EINVAL);
        return 0;
    }
    if ((offset > 0 && base > INT64_MAX - offset) || base + offset < 0) {
        *err = mp_file_error(f, "seek", PAL_EINVAL);
        return 0;
    }
    f->pos = base + offset;
    return f->pos;
}

Error multipart_file_close(MultipartFile *f) {
    if (f == NULL)
        return BURROW_NO_ERROR;
    Error err = BURROW_NO_ERROR;
    if (f->fd >= 0) {
        PalErrno e = PAL_OK;
        if (!pal_close(f->fd, &e))
            err = fmt_errorf_v("close %s: %s", str_from_cstr(f->name),
                               str_from_cstr(pal_errno_string(e)));
    }
    mem_free(f->a, f, sizeof *f, _Alignof(MultipartFile));
    return err;
}

static Int mp_file_vt_read(void *self, Slice p, Error *err) {
    return multipart_file_read((MultipartFile *)self, p, err);
}

static Int mp_file_vt_read_at(void *self, Slice p, int64_t off, Error *err) {
    return multipart_file_read_at((MultipartFile *)self, p, off, err);
}

static int64_t mp_file_vt_seek(void *self, int64_t offset, int whence, Error *err) {
    return multipart_file_seek((MultipartFile *)self, offset, whence, err);
}

static Error mp_file_vt_close(void *self) {
    return multipart_file_close((MultipartFile *)self);
}

static const IoReaderVT mp_file_reader_vt = {&mp_file_desc, mp_file_vt_read};
static const IoReaderAtVT mp_file_reader_at_vt = {&mp_file_desc, mp_file_vt_read_at};
static const IoSeekerVT mp_file_seeker_vt = {&mp_file_desc, mp_file_vt_seek};
static const IoReadSeekCloserVT mp_file_rsc_vt = {{&mp_file_desc, mp_file_vt_read},
                                                  {&mp_file_desc, mp_file_vt_seek},
                                                  {&mp_file_desc, mp_file_vt_close}};

IoReader multipart_file_as_io_reader(MultipartFile *f) {
    return (IoReader){&mp_file_reader_vt, f};
}

IoReaderAt multipart_file_as_io_reader_at(MultipartFile *f) {
    return (IoReaderAt){&mp_file_reader_at_vt, f};
}

IoSeeker multipart_file_as_io_seeker(MultipartFile *f) {
    return (IoSeeker){&mp_file_seeker_vt, f};
}

IoReadSeekCloser multipart_file_as_io_read_seek_closer(MultipartFile *f) {
    return (IoReadSeekCloser){&mp_file_rsc_vt, f};
}

/* ------------------------------------------------------------------ writing */

typedef struct MpWPart MpWPart;

struct MpWPart {
    MultipartWriter *mw;
    bool closed;
    Error we; /* last error that occurred writing */
    MpWPart *next;
};

struct MultipartWriter {
    Alloc *a;
    IoWriter w;
    Byte boundary[70];
    Int boundary_len;
    MpWPart *lastpart;
    MpWPart *parts; /* every part made, for multipart_writer_free */
    Arena scratch;  /* for the headers of a part, given back after each one */
};

MP_DESC(mp_writer_desc, "Writer", MultipartWriter, 0x6d707772U);
MP_DESC(mp_wpart_desc, "part", MpWPart, 0x6d707770U);
const Type *const TYPE_MULTIPART_WRITER = &mp_writer_desc;

MultipartWriter *multipart_new_writer(Alloc *a, IoWriter w) {
    MultipartWriter *z =
        (MultipartWriter *)mem_alloc(a, sizeof *z, _Alignof(MultipartWriter));
    if (z == NULL)
        return NULL;
    z->a = a;
    z->w = w;
    z->lastpart = NULL;
    z->parts = NULL;
    arena_init(&z->scratch, a, 0);
    Byte buf[30];
    PalErrno e = PAL_OK;
    if (!pal_random_bytes(buf, (int64_t)sizeof buf, &e))
        runtime_panic(str_from_cstr(pal_errno_string(e)));
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 30; i++) {
        z->boundary[2 * i] = (Byte)hex[buf[i] >> 4];
        z->boundary[2 * i + 1] = (Byte)hex[buf[i] & 15];
    }
    z->boundary_len = 60;
    return z;
}

void multipart_writer_free(MultipartWriter *w) {
    if (w == NULL)
        return;
    for (MpWPart *p = w->parts; p != NULL;) {
        MpWPart *next = p->next;
        mem_free(w->a, p, sizeof *p, _Alignof(MpWPart));
        p = next;
    }
    arena_free(&w->scratch);
    mem_free(w->a, w, sizeof *w, _Alignof(MultipartWriter));
}

Str multipart_writer_boundary(const MultipartWriter *w) {
    return (Str){w->boundary, w->boundary_len};
}

Error multipart_writer_set_boundary(MultipartWriter *w, Str boundary) {
    if (w->lastpart != NULL)
        return fmt_errorf_v("mime: SetBoundary called after write");
    /* rfc2046#section-5.1.1 */
    if (boundary.len < 1 || boundary.len > 70)
        return fmt_errorf_v("mime: invalid boundary length");
    Int end = boundary.len - 1;
    /* Go ranges over runes, and anything past ASCII is not in the set. */
    for (Int i = 0; i < boundary.len; i++) {
        Byte b = boundary.p[i];
        if (('A' <= b && b <= 'Z') || ('a' <= b && b <= 'z') || ('0' <= b && b <= '9'))
            continue;
        switch (b) {
        case '\'':
        case '(':
        case ')':
        case '+':
        case '_':
        case ',':
        case '-':
        case '.':
        case '/':
        case ':':
        case '=':
        case '?':
            continue;
        case ' ':
            if (i != end)
                continue;
            break;
        default:
            break;
        }
        return fmt_errorf_v("mime: invalid boundary character");
    }
    memcpy(w->boundary, boundary.p, (size_t)boundary.len);
    w->boundary_len = boundary.len;
    return BURROW_NO_ERROR;
}

Str multipart_writer_form_data_content_type(const MultipartWriter *w, Alloc *a) {
    Str b = multipart_writer_boundary(w);
    /* We must quote the boundary if it contains any of the tspecials
     * characters defined by RFC 2045, or space. */
    if (strings_contains_any(b, BURROW_S("()<>@,;:\\\"/[]?= ")))
        return fmt_sprintf_v(a, "multipart/form-data; boundary=\"%s\"", b);
    return fmt_sprintf_v(a, "multipart/form-data; boundary=%s", b);
}

static Error mp_wpart_close(MpWPart *p) {
    p->closed = true;
    return p->we;
}

static Int mp_wpart_write(void *self, Slice d, Error *err) {
    MpWPart *p = (MpWPart *)self;
    if (p->closed) {
        *err = fmt_errorf_v("multipart: can't write to finished part");
        return 0;
    }
    Error e = BURROW_NO_ERROR;
    Int n = p->mw->w.vt->write(p->mw->w.data, d, &e);
    if (BURROW_FAILED(e))
        p->we = e;
    *err = e;
    return n;
}

static const IoWriterVT mp_wpart_vt = {&mp_wpart_desc, mp_wpart_write};

IoWriter multipart_writer_create_part(MultipartWriter *w, TextprotoMIMEHeader header,
                                      Error *err) {
    *err = BURROW_NO_ERROR;
    if (w->lastpart != NULL) {
        Error e = mp_wpart_close(w->lastpart);
        if (BURROW_FAILED(e)) {
            *err = e;
            return (IoWriter){0};
        }
    }
    ArenaMark mark = arena_mark(&w->scratch);
    Alloc *sa = arena_allocator(&w->scratch);
    BytesBuffer b = BYTES_BUFFER(sa);
    Str boundary = multipart_writer_boundary(w);
    if (w->lastpart != NULL)
        fmt_fprintf_v(bytes_buffer_as_io_writer(&b), "\r\n--%s\r\n", boundary);
    else
        fmt_fprintf_v(bytes_buffer_as_io_writer(&b), "--%s\r\n", boundary);

    Int nk = header == NULL ? 0 : map_len(header);
    Slice keys = slice_make(sa, TYPE_STRING, nk, nk);
    Int i = 0;
    const void *k;
    void *v;
    for (MapIter it = map_iter(header); nk > 0 && map_next(&it, &k, &v);)
        ((Str *)keys.p)[i++] = *(const Str *)k;
    sort_strings(keys);
    for (i = 0; i < nk; i++) {
        Str key = ((const Str *)keys.p)[i];
        const Slice *vs = (const Slice *)map_get(header, &key);
        for (Int j = 0; j < vs->len; j++)
            fmt_fprintf_v(bytes_buffer_as_io_writer(&b), "%s: %s\r\n", key,
                          ((const Str *)vs->p)[j]);
    }
    fmt_fprintf_v(bytes_buffer_as_io_writer(&b), "\r\n");
    Error e = BURROW_NO_ERROR;
    Slice out = bytes_buffer_bytes(&b);
    Int off = 0;
    while (off < out.len) {
        Int n = w->w.vt->write(w->w.data, mp_tail(out, off), &e);
        off += n;
        if (BURROW_FAILED(e))
            break;
        if (n <= 0) {
            e = io_err_short_write;
            break;
        }
    }
    arena_release(&w->scratch, mark);
    if (BURROW_FAILED(e)) {
        *err = e;
        return (IoWriter){0};
    }
    MpWPart *p = (MpWPart *)mem_alloc(w->a, sizeof *p, _Alignof(MpWPart));
    if (p == NULL) {
        *err = burrow__mime_err_no_memory;
        return (IoWriter){0};
    }
    p->mw = w;
    p->next = w->parts;
    w->parts = p;
    w->lastpart = p;
    return (IoWriter){&mp_wpart_vt, p};
}

/* escapeQuotes: backslashes and quotes escaped, CR and LF percent encoded. */
static Str mp_escape_quotes(Alloc *a, Str s) {
    StringsBuilder b = STRINGS_BUILDER(a);
    for (Int i = 0; i < s.len; i++) {
        Byte c = s.p[i];
        switch (c) {
        case '\\':
            strings_builder_write_string(&b, BURROW_S("\\\\"), NULL);
            break;
        case '"':
            strings_builder_write_string(&b, BURROW_S("\\\""), NULL);
            break;
        case '\r':
            strings_builder_write_string(&b, BURROW_S("%0D"), NULL);
            break;
        case '\n':
            strings_builder_write_string(&b, BURROW_S("%0A"), NULL);
            break;
        default:
            strings_builder_write_byte(&b, c);
            break;
        }
    }
    return strings_builder_string(&b);
}

Str multipart_file_content_disposition(Alloc *a, Str fieldname, Str filename) {
    Arena scratch;
    arena_init(&scratch, heap_allocator(), 0);
    Alloc *sa = arena_allocator(&scratch);
    Str r =
        fmt_sprintf_v(a, "form-data; name=\"%s\"; filename=\"%s\"",
                      mp_escape_quotes(sa, fieldname), mp_escape_quotes(sa, filename));
    arena_free(&scratch);
    return r;
}

IoWriter multipart_writer_create_form_file(MultipartWriter *w, Str fieldname,
                                           Str filename, Error *err) {
    ArenaMark mark = arena_mark(&w->scratch);
    Alloc *sa = arena_allocator(&w->scratch);
    TextprotoMIMEHeader h = textproto_mime_header_make(sa);
    textproto_mime_header_set(
        h, BURROW_S("Content-Disposition"),
        multipart_file_content_disposition(sa, fieldname, filename));
    textproto_mime_header_set(h, BURROW_S("Content-Type"),
                              BURROW_S("application/octet-stream"));
    IoWriter r = multipart_writer_create_part(w, h, err);
    arena_release(&w->scratch, mark);
    return r;
}

IoWriter multipart_writer_create_form_field(MultipartWriter *w, Str fieldname,
                                            Error *err) {
    ArenaMark mark = arena_mark(&w->scratch);
    Alloc *sa = arena_allocator(&w->scratch);
    TextprotoMIMEHeader h = textproto_mime_header_make(sa);
    textproto_mime_header_set(
        h, BURROW_S("Content-Disposition"),
        fmt_sprintf_v(sa, "form-data; name=\"%s\"", mp_escape_quotes(sa, fieldname)));
    IoWriter r = multipart_writer_create_part(w, h, err);
    arena_release(&w->scratch, mark);
    return r;
}

Error multipart_writer_write_field(MultipartWriter *w, Str fieldname, Str value) {
    Error err = BURROW_NO_ERROR;
    IoWriter p = multipart_writer_create_form_field(w, fieldname, &err);
    if (BURROW_FAILED(err))
        return err;
    (void)p.vt->write(p.data, mp_slice(value.p, value.len), &err);
    return err;
}

Error multipart_writer_close(MultipartWriter *w) {
    if (w->lastpart != NULL) {
        Error e = mp_wpart_close(w->lastpart);
        if (BURROW_FAILED(e))
            return e;
        w->lastpart = NULL;
    }
    Error err = BURROW_NO_ERROR;
    Any arg = BURROW_ANY_OF(multipart_writer_boundary(w));
    fmt_fprintf(w->w, BURROW_S("\r\n--%s--\r\n"), slice_from(&arg, 1, 1, TYPE_ANY),
                &err);
    return err;
}

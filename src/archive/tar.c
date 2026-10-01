/* archive/tar, from common.go, format.go, strconv.go, reader.go and writer.go.
 *
 * A header block is 512 bytes of fixed fields, with each format adding its own
 * after the ones V7 had. Numbers are octal text, or base-256 in GNU's format
 * when they do not fit, and whatever a block cannot hold goes in a PAX header
 * or a GNU long name in front of it. A reader folds those back into the header
 * of the file they belong to.
 *
 * Headers a reader returns hold their strings and maps in memory of their own
 * from the reader's allocator, so they outlive the reader. A writer only reads
 * the header it is given, and frees whatever it built on the way before
 * WriteHeader returns.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "tar_internal.h"

#include "burrow/atomic.h"
#include "burrow/fmt.h"
#include "burrow/mem/arena.h"
#include "burrow/pal.h"
#include "burrow/path.h"
#include "burrow/sort.h"
#include "burrow/strconv.h"
#include "burrow/strings.h"

#include <string.h>

BURROW_SENTINEL_ERROR(tar_err_header, "archive/tar: invalid tar header");
BURROW_SENTINEL_ERROR(tar_err_write_too_long, "archive/tar: write too long");
BURROW_SENTINEL_ERROR(tar_err_field_too_long, "archive/tar: header field too long");
BURROW_SENTINEL_ERROR(tar_err_write_after_close, "archive/tar: write after close");
BURROW_SENTINEL_ERROR(tar_err_insecure_path, "archive/tar: insecure file path");
BURROW_SENTINEL_ERROR(burrow__tar_err_miss_data,
                      "archive/tar: sparse file references non-existent data");
BURROW_SENTINEL_ERROR(burrow__tar_err_unref_data,
                      "archive/tar: sparse file contains unreferenced data");
BURROW_SENTINEL_ERROR(burrow__tar_err_write_hole,
                      "archive/tar: write non-NUL byte in sparse hole");
BURROW_SENTINEL_ERROR(burrow__tar_err_sparse_too_long,
                      "archive/tar: sparse map too long");

#define TAR_LIT(s) ((Str){(const Byte *)("" s), (Int)(sizeof(s) - 1)})

/* The PAX keys. */
#define TAR_PAX_PATH TAR_LIT("path")
#define TAR_PAX_LINKPATH TAR_LIT("linkpath")
#define TAR_PAX_SIZE TAR_LIT("size")
#define TAR_PAX_UID TAR_LIT("uid")
#define TAR_PAX_GID TAR_LIT("gid")
#define TAR_PAX_UNAME TAR_LIT("uname")
#define TAR_PAX_GNAME TAR_LIT("gname")
#define TAR_PAX_MTIME TAR_LIT("mtime")
#define TAR_PAX_ATIME TAR_LIT("atime")
#define TAR_PAX_CTIME TAR_LIT("ctime")
#define TAR_PAX_SCHILY_XATTR TAR_LIT("SCHILY.xattr.")
#define TAR_PAX_GNU_SPARSE TAR_LIT("GNU.sparse.")
#define TAR_PAX_GNU_SPARSE_NUM_BLOCKS TAR_LIT("GNU.sparse.numblocks")
#define TAR_PAX_GNU_SPARSE_OFFSET TAR_LIT("GNU.sparse.offset")
#define TAR_PAX_GNU_SPARSE_NUM_BYTES TAR_LIT("GNU.sparse.numbytes")
#define TAR_PAX_GNU_SPARSE_MAP TAR_LIT("GNU.sparse.map")
#define TAR_PAX_GNU_SPARSE_NAME TAR_LIT("GNU.sparse.name")
#define TAR_PAX_GNU_SPARSE_MAJOR TAR_LIT("GNU.sparse.major")
#define TAR_PAX_GNU_SPARSE_MINOR TAR_LIT("GNU.sparse.minor")
#define TAR_PAX_GNU_SPARSE_SIZE TAR_LIT("GNU.sparse.size")
#define TAR_PAX_GNU_SPARSE_REAL_SIZE TAR_LIT("GNU.sparse.realsize")

/* The mode bits of a Unix stat, which is what a header's mode holds. */
enum {
    TAR_C_ISUID = 04000,
    TAR_C_ISGID = 02000,
    TAR_C_ISVTX = 01000,
    TAR_C_ISDIR = 040000,
    TAR_C_ISFIFO = 010000,
    TAR_C_ISREG = 0100000,
    TAR_C_ISLNK = 0120000,
    TAR_C_ISBLK = 060000,
    TAR_C_ISCHR = 020000,
    TAR_C_ISSOCK = 0140000,
};

static const Byte tar_zero_block[TAR_BLOCK_SIZE] = {0};

/* What an empty string that came from an allocator points at, so that no Str
 * this package hands out has a NULL pointer. */
static const Byte tar_empty[1] = {0};

static inline bool tar_same(Error a, Error b) {
    return a.vt == b.vt && a.data == b.data;
}

static inline Slice tar_bytes(const Byte *p, Int n) {
    return slice_from((void *)(uintptr_t)p, n, n, TYPE_BYTE);
}

static inline int64_t tar_min64(int64_t a, int64_t b) {
    return a < b ? a : b;
}

static inline int64_t tar_block_padding(int64_t offset) {
    return (int64_t)((0U - (uint64_t)offset) & (TAR_BLOCK_SIZE - 1));
}

static bool tar_has_nul(Str s) {
    return s.len > 0 && memchr(s.p, 0, (size_t)s.len) != NULL;
}

/* Go ranges over the runes, and a byte of 0x80 or more is never a rune below
 * 0x80, so bytes do as well. */
static bool tar_is_ascii(Str s) {
    for (Int i = 0; i < s.len; i++)
        if (s.p[i] >= 0x80 || s.p[i] == 0)
            return false;
    return true;
}

static bool tar_is_header_only_type(Byte flag) {
    switch (flag) {
    case TAR_TYPE_LINK:
    case TAR_TYPE_SYMLINK:
    case TAR_TYPE_CHAR:
    case TAR_TYPE_BLOCK:
    case TAR_TYPE_DIR:
    case TAR_TYPE_FIFO:
        return true;
    default:
        return false;
    }
}

/* ----------------------------------------------------------------- strings */

/* A copy of s in a. Empty is not an allocation. */
static Str tar_dup(Alloc *a, Str s, bool *ok) {
    if (s.len == 0)
        return (Str){tar_empty, 0};
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)s.len, 1);
    if (p == NULL) {
        *ok = false;
        return (Str){tar_empty, 0};
    }
    memcpy(p, s.p, (size_t)s.len);
    return str_from_bytes(p, s.len);
}

static void tar_str_free(Alloc *a, Str s) {
    if (s.len > 0)
        mem_free(a, (void *)(uintptr_t)s.p, (size_t)s.len, 1);
}

/* *dst = a copy of s, giving back what it held. */
static bool tar_set(Alloc *a, Str *dst, Str s) {
    bool ok = true;
    Str c = tar_dup(a, s, &ok);
    if (!ok)
        return false;
    tar_str_free(a, *dst);
    *dst = c;
    return true;
}

/* A string built up a piece at a time, in a. */
typedef struct TarBuf {
    Alloc *a;
    Byte *p;
    Int len;
    Int cap;
    bool oom;
} TarBuf;

static bool tar_buf_grow(TarBuf *b, Int n) {
    if (b->oom)
        return false;
    if (b->cap - b->len >= n)
        return true;
    Int want = b->len + n;
    Int ncap = b->cap < 64 ? 64 : b->cap;
    while (ncap < want)
        ncap = ncap + ncap / 2;
    Byte *np = b->p == NULL
                   ? (Byte *)mem_alloc_nozero(b->a, (size_t)ncap, 1)
                   : (Byte *)mem_realloc(b->a, b->p, (size_t)b->cap, (size_t)ncap, 1);
    if (np == NULL) {
        b->oom = true;
        return false;
    }
    b->p = np;
    b->cap = ncap;
    return true;
}

static void tar_buf_put(TarBuf *b, const void *p, Int n) {
    if (n > 0 && tar_buf_grow(b, n)) {
        memcpy(b->p + b->len, p, (size_t)n);
        b->len += n;
    }
}

static void tar_buf_str(TarBuf *b, Str s) {
    tar_buf_put(b, s.p, s.len);
}

static void tar_buf_cstr(TarBuf *b, const char *s) {
    tar_buf_put(b, s, (Int)strlen(s));
}

static void tar_buf_byte(TarBuf *b, Byte c) {
    tar_buf_put(b, &c, 1);
}

/* The decimal digits of x at the end of buf, which has room for 21, and where
 * they start. */
static Int tar_itoa(Byte *buf, int64_t x) {
    Int i = 21;
    uint64_t u = x < 0 ? 0U - (uint64_t)x : (uint64_t)x;
    do {
        buf[--i] = (Byte)('0' + (int)(u % 10));
        u /= 10;
    } while (u != 0);
    if (x < 0)
        buf[--i] = '-';
    return i;
}

static void tar_buf_int(TarBuf *b, int64_t x) {
    Byte d[21];
    Int i = tar_itoa(d, x);
    tar_buf_put(b, d + i, 21 - i);
}

static void tar_buf_quote(TarBuf *b, Str s) {
    Int n = burrow__strconv_quote_into(NULL, s);
    if (tar_buf_grow(b, n)) {
        burrow__strconv_quote_into(b->p + b->len, s);
        b->len += n;
    }
}

static Str tar_buf_view(const TarBuf *b) {
    return b->len == 0 ? (Str){tar_empty, 0} : str_from_bytes(b->p, b->len);
}

static void tar_buf_free(TarBuf *b) {
    if (b->p != NULL)
        mem_free(b->a, b->p, (size_t)b->cap, 1);
    b->p = NULL;
    b->len = b->cap = 0;
}

/* ------------------------------------------------------------------ Format */

static const char *const tar_format_names[] = {"V7", "USTAR", "PAX", "GNU", "STAR"};

Str tar_format_string(TarFormat f, Alloc *a) {
    TarBuf b = {a, NULL, 0, 0, false};
    Int count = 0;
    for (int i = 0; (1 << i) < TAR_FORMAT_MAX; i++) {
        if ((f & (1 << i)) == 0)
            continue;
        if (count > 0)
            tar_buf_cstr(&b, " | ");
        tar_buf_cstr(&b, tar_format_names[i]);
        count++;
    }
    Str out;
    if (count == 0) {
        out = BURROW_S("<unknown>");
    } else if (count == 1) {
        out = tar_buf_view(&b);
    } else {
        TarBuf c = {a, NULL, 0, 0, false};
        tar_buf_byte(&c, '(');
        tar_buf_str(&c, tar_buf_view(&b));
        tar_buf_byte(&c, ')');
        out = tar_buf_view(&c);
        if (c.oom)
            tar_buf_free(&c);
        else
            c.p = NULL;
        tar_buf_free(&b);
        if (c.oom)
            return BURROW_STR_EMPTY;
        return str_clone(a, out);
    }
    Str r = str_clone(a, out);
    tar_buf_free(&b);
    return r;
}

/* ------------------------------------------------------------ headerError */

/* One of the reasons a header cannot be written, kept as the pieces of the
 * sentence rather than the sentence, so that a header that can be written
 * costs nothing to explain. */
enum {
    TAR_WHY_NONE,
    TAR_WHY_TEXT, /* what */
    TAR_WHY_STR,  /* what name=%q */
    TAR_WHY_NUM,  /* what name=%d */
    TAR_WHY_TIME, /* what name=%v */
};

typedef struct TarWhy {
    int kind;
    const char *what;
    const char *name;
    Str s;
    int64_t n;
    Time t;
} TarWhy;

typedef struct TarHeaderErrorBox {
    Str message;
} TarHeaderErrorBox;

static Str tar_header_error_message(const void *self) {
    return ((const TarHeaderErrorBox *)self)->message;
}

static const Type tar_header_error_desc = {
    {(const Byte *)"headerError", 11},
    {(const Byte *)"archive/tar", 11},
    KIND_STRUCT,
    (uint32_t)sizeof(TarHeaderErrorBox),
    (uint16_t)_Alignof(TarHeaderErrorBox),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x74686572U, /* "ther" */
    NULL,
};

const Type *const burrow__TYPE_TAR_HEADER_ERROR = &tar_header_error_desc;

static Error tar_header_error_box(Alloc *a, Str text);

static Error tar_header_error_clone(const void *self, Alloc *a) {
    return tar_header_error_box(a, ((const TarHeaderErrorBox *)self)->message);
}

static const ErrorVT tar_header_error_vt = {
    &tar_header_error_desc, tar_header_error_message, NULL, NULL, NULL, NULL,
    tar_header_error_clone,
};

static Error tar_header_error_box(Alloc *a, Str text) {
    TarHeaderErrorBox *b = (TarHeaderErrorBox *)mem_alloc_nozero(
        a, sizeof(TarHeaderErrorBox) + (size_t)text.len, _Alignof(TarHeaderErrorBox));
    if (b == NULL)
        return burrow_err_out_of_memory;
    Byte *p = (Byte *)(b + 1);
    if (text.len > 0)
        memcpy(p, text.p, (size_t)text.len);
    b->message = str_from_bytes(p, text.len);
    return (Error){&tar_header_error_vt, b};
}

static void tar_why_format(TarBuf *b, const TarWhy *w) {
    tar_buf_cstr(b, w->what);
    if (w->kind == TAR_WHY_TEXT)
        return;
    tar_buf_cstr(b, w->name);
    tar_buf_byte(b, '=');
    switch (w->kind) {
    case TAR_WHY_STR:
        tar_buf_quote(b, w->s);
        break;
    case TAR_WHY_NUM:
        tar_buf_int(b, w->n);
        break;
    default: {
        Str ts = time_string(w->t, b->a);
        if (ts.len == 0) {
            b->oom = true;
            break;
        }
        tar_buf_str(b, ts);
        tar_str_free(b->a, ts);
        break;
    }
    }
}

/* headerError(whys).Error(), in the error arena, with a for the building. */
static Error tar_header_error(Alloc *a, const TarWhy *whys, int n) {
    TarBuf b = {a, NULL, 0, 0, false};
    tar_buf_cstr(&b, "archive/tar: cannot encode header");
    int count = 0;
    for (int i = 0; i < n; i++) {
        if (whys[i].kind == TAR_WHY_NONE)
            continue;
        tar_buf_cstr(&b, count == 0 ? ": " : "; and ");
        tar_why_format(&b, &whys[i]);
        count++;
    }
    Error err = b.oom ? burrow_err_out_of_memory
                      : tar_header_error_box(error_allocator(), tar_buf_view(&b));
    tar_buf_free(&b);
    return err;
}

static Error tar_header_error_text(Alloc *a, const char *text) {
    TarWhy w = {TAR_WHY_TEXT, text, NULL, {tar_empty, 0}, 0, {0}};
    return tar_header_error(a, &w, 1);
}

/* ---------------------------------------------------------------- strconv */

Str burrow__tar_parse_string(const Byte *b, Int n) {
    const Byte *z = n > 0 ? (const Byte *)memchr(b, 0, (size_t)n) : NULL;
    return str_from_bytes(b, z != NULL ? (Int)(z - b) : n);
}

/* formatString with only the first min(slen, n) bytes of s at hand, which is
 * all it looks at. */
static void tar_format_string_n(Byte *b, Int n, const Byte *s, Int slen, Error *ferr) {
    if (slen > n)
        *ferr = tar_err_field_too_long;
    Int c = slen < n ? slen : n;
    if (c > 0)
        memcpy(b, s, (size_t)c);
    if (slen < n)
        b[slen] = 0;
    if (slen > n && n > 0 && b[n - 1] == '/') {
        Int m = n - 1;
        while (m > 0 && s[m - 1] == '/')
            m--;
        b[m] = 0;
    }
}

void burrow__tar_format_string(Byte *b, Int n, Str s, Error *ferr) {
    tar_format_string_n(b, n, s.p, s.len, ferr);
}

/* formatString(b, toASCII(s)), without building toASCII(s): the first n + 1
 * bytes of it are enough. */
static void tar_format_ascii(Byte *b, Int n, Str s, Error *ferr) {
    Byte tmp[TAR_NAME_SIZE + 2];
    Int total = 0;
    Int kept = 0;
    for (Int i = 0; i < s.len; i++) {
        if (s.p[i] >= 0x80 || s.p[i] == 0)
            continue;
        if (kept < (Int)sizeof tmp)
            tmp[kept++] = s.p[i];
        total++;
    }
    tar_format_string_n(b, n, tmp, total, ferr);
}

bool burrow__tar_fits_in_base256(Int n, int64_t x) {
    if (n >= 9)
        return true;
    if (n <= 0)
        return false;
    unsigned bits = (unsigned)(n - 1) * 8U;
    int64_t lim = (int64_t)1 << bits;
    return x >= -lim && x < lim;
}

bool burrow__tar_fits_in_octal(Int n, int64_t x) {
    if (x < 0 || n <= 0)
        return false;
    if (n >= 22)
        return true;
    unsigned bits = (unsigned)(n - 1) * 3U;
    return x < (int64_t)1 << bits;
}

int64_t burrow__tar_parse_octal(const Byte *b, Int n, Error *perr) {
    Int lo = 0;
    Int hi = n;
    while (lo < hi && (b[lo] == ' ' || b[lo] == 0))
        lo++;
    while (hi > lo && (b[hi - 1] == ' ' || b[hi - 1] == 0))
        hi--;
    if (lo == hi)
        return 0;
    Str s = burrow__tar_parse_string(b + lo, hi - lo);
    Error e = BURROW_NO_ERROR;
    uint64_t x = strconv_parse_uint(s, 8, 64, &e);
    if (BURROW_FAILED(e))
        *perr = tar_err_header;
    return (int64_t)x;
}

int64_t burrow__tar_parse_numeric(const Byte *b, Int n, Error *perr) {
    if (n > 0 && (b[0] & 0x80) != 0) {
        Byte inv = (b[0] & 0x40) != 0 ? 0xff : 0x00;
        uint64_t x = 0;
        for (Int i = 0; i < n; i++) {
            Byte c = (Byte)(b[i] ^ inv);
            if (i == 0)
                c &= 0x7f;
            if ((x >> 56) > 0) {
                *perr = tar_err_header;
                return 0;
            }
            x = x << 8 | c;
        }
        if ((x >> 63) > 0) {
            *perr = tar_err_header;
            return 0;
        }
        if (inv == 0xff)
            return ~(int64_t)x;
        return (int64_t)x;
    }
    return burrow__tar_parse_octal(b, n, perr);
}

void burrow__tar_format_octal(Byte *b, Int n, int64_t x, Error *ferr) {
    if (!burrow__tar_fits_in_octal(n, x)) {
        x = 0;
        *ferr = tar_err_field_too_long;
    }
    Byte d[32];
    Int i = (Int)sizeof d;
    uint64_t u = (uint64_t)x;
    do {
        d[--i] = (Byte)('0' + (int)(u & 7));
        u >>= 3;
    } while (u != 0);
    Int len = (Int)sizeof d - i;
    for (Int pad = n - len - 1; pad > 0 && i > 0; pad--) {
        d[--i] = '0';
        len++;
    }
    tar_format_string_n(b, n, d + i, len, ferr);
}

void burrow__tar_format_numeric(Byte *b, Int n, int64_t x, Error *ferr) {
    if (burrow__tar_fits_in_octal(n, x)) {
        burrow__tar_format_octal(b, n, x, ferr);
        return;
    }
    if (burrow__tar_fits_in_base256(n, x)) {
        for (Int i = n - 1; i >= 0; i--) {
            b[i] = (Byte)x;
            x >>= 8;
        }
        b[0] |= 0x80;
        return;
    }
    burrow__tar_format_octal(b, n, 0, ferr);
    *ferr = tar_err_field_too_long;
}

Time burrow__tar_parse_pax_time(Str s, Error *err) {
    Str sn;
    bool found = false;
    Str ss = strings_cut(s, TAR_LIT("."), &sn, &found);
    Error e = BURROW_NO_ERROR;
    int64_t secs = strconv_parse_int(ss, 10, 64, &e);
    if (BURROW_FAILED(e)) {
        *err = tar_err_header;
        return (Time){0};
    }
    *err = BURROW_NO_ERROR;
    if (sn.len == 0)
        return time_from_unix(secs, 0);
    int64_t nsecs = 0;
    for (Int i = 0; i < sn.len; i++) {
        Byte c = sn.p[i];
        if (c < '0' || c > '9') {
            *err = tar_err_header;
            return (Time){0};
        }
    }
    for (Int i = 0; i < 9; i++)
        nsecs = nsecs * 10 + (i < sn.len ? sn.p[i] - '0' : 0);
    if (ss.len > 0 && ss.p[0] == '-')
        return time_from_unix(secs, -nsecs);
    return time_from_unix(secs, nsecs);
}

/* formatPAXTime into buf, which has room for 32, returning the length. */
static Int tar_pax_time_into(Byte *buf, Time ts) {
    int64_t secs = time_unix(ts);
    int64_t nsecs = (int64_t)time_nanosecond(ts);
    Byte d[21];
    Int n = 0;
    if (nsecs == 0) {
        Int i = tar_itoa(d, secs);
        memcpy(buf, d + i, (size_t)(21 - i));
        return 21 - i;
    }
    if (secs < 0) {
        buf[n++] = '-';
        secs = -(secs + 1);
        nsecs = -(nsecs - 1000000000);
    }
    Int i = tar_itoa(d, secs);
    memcpy(buf + n, d + i, (size_t)(21 - i));
    n += 21 - i;
    buf[n++] = '.';
    for (int k = 8; k >= 0; k--) {
        buf[n + k] = (Byte)('0' + (int)(nsecs % 10));
        nsecs /= 10;
    }
    n += 9;
    while (n > 0 && buf[n - 1] == '0')
        n--;
    return n;
}

Str burrow__tar_format_pax_time(Alloc *a, Time t) {
    Byte buf[32];
    Int n = tar_pax_time_into(buf, t);
    return str_clone(a, str_from_bytes(buf, n));
}

bool burrow__tar_valid_pax_record(Str k, Str v) {
    if (k.len == 0 || memchr(k.p, '=', (size_t)k.len) != NULL)
        return false;
    if (str_eq(k, TAR_PAX_PATH) || str_eq(k, TAR_PAX_LINKPATH) ||
        str_eq(k, TAR_PAX_UNAME) || str_eq(k, TAR_PAX_GNAME))
        return !tar_has_nul(v);
    return !tar_has_nul(k);
}

Str burrow__tar_parse_pax_record(Str s, Str *k, Str *v, Error *err) {
    *k = *v = (Str){tar_empty, 0};
    *err = tar_err_header;
    Str rest;
    bool ok = false;
    Str nstr = strings_cut(s, TAR_LIT(" "), &rest, &ok);
    if (!ok)
        return s;
    Error e = BURROW_NO_ERROR;
    int64_t n = strconv_parse_int(nstr, 10, 0, &e);
    if (BURROW_FAILED(e) || n < 5 || n > (int64_t)s.len)
        return s;
    n -= (int64_t)(nstr.len + 1);
    if (n <= 0)
        return s;
    Str rec = str_from_bytes(rest.p, (Int)n - 1);
    if (rest.p[n - 1] != '\n')
        return s;
    Str rem = str_from_bytes(rest.p + n, rest.len - (Int)n);
    Str val;
    Str key = strings_cut(rec, TAR_LIT("="), &val, &ok);
    if (!ok || !burrow__tar_valid_pax_record(key, val))
        return s;
    *k = key;
    *v = val;
    *err = BURROW_NO_ERROR;
    return rem;
}

static Int tar_digits(int64_t x) {
    Byte d[21];
    return 21 - tar_itoa(d, x);
}

/* formatPAXRecord, appended to b. */
static bool tar_pax_record_into(TarBuf *b, Str k, Str v) {
    if (!burrow__tar_valid_pax_record(k, v))
        return false;
    int64_t size = (int64_t)k.len + (int64_t)v.len + 3;
    size += tar_digits(size);
    int64_t actual = tar_digits(size) + 1 + k.len + 1 + v.len + 1;
    if (actual != size)
        size = actual;
    tar_buf_int(b, size);
    tar_buf_byte(b, ' ');
    tar_buf_str(b, k);
    tar_buf_byte(b, '=');
    tar_buf_str(b, v);
    tar_buf_byte(b, '\n');
    return true;
}

Str burrow__tar_format_pax_record(Alloc *a, Str k, Str v, Error *err) {
    TarBuf b = {a, NULL, 0, 0, false};
    if (!tar_pax_record_into(&b, k, v)) {
        *err = tar_err_header;
        return (Str){tar_empty, 0};
    }
    if (b.oom) {
        tar_buf_free(&b);
        *err = burrow_err_out_of_memory;
        return (Str){tar_empty, 0};
    }
    *err = BURROW_NO_ERROR;
    Str out = str_clone(a, tar_buf_view(&b));
    tar_buf_free(&b);
    if (out.len == 0)
        *err = burrow_err_out_of_memory;
    return out;
}

/* ------------------------------------------------------------ string maps */

Map *burrow__tar_str_map(Alloc *a) {
    return map_make(a, TYPE_STRING, TYPE_STRING, 0);
}

bool burrow__tar_str_map_set(Alloc *a, Map *m, Str k, Str v) {
    bool ok = true;
    Str *old = (Str *)map_get(m, &k);
    if (old != NULL) {
        Str nv = tar_dup(a, v, &ok);
        if (!ok)
            return false;
        tar_str_free(a, *old);
        *old = nv;
        return true;
    }
    Str nk = tar_dup(a, k, &ok);
    Str nv = tar_dup(a, v, &ok);
    if (!ok || !map_set(m, &nk, &nv)) {
        tar_str_free(a, nk);
        tar_str_free(a, nv);
        return false;
    }
    return true;
}

void burrow__tar_str_map_free(Alloc *a, Map *m) {
    if (m == NULL)
        return;
    MapIter it = map_iter(m);
    const void *k;
    void *v;
    while (map_next(&it, &k, &v)) {
        tar_str_free(a, *(const Str *)k);
        tar_str_free(a, *(const Str *)v);
    }
    map_free(m);
}

/* maps.Clone, deep. NULL stays NULL. */
static Map *tar_str_map_clone(Alloc *a, Map *m, bool *ok) {
    if (m == NULL)
        return NULL;
    Map *c = burrow__tar_str_map(a);
    if (c == NULL) {
        *ok = false;
        return NULL;
    }
    MapIter it = map_iter(m);
    const void *k;
    void *v;
    while (map_next(&it, &k, &v)) {
        if (!burrow__tar_str_map_set(a, c, *(const Str *)k, *(const Str *)v)) {
            burrow__tar_str_map_free(a, c);
            *ok = false;
            return NULL;
        }
    }
    return c;
}

/* m[k], and whether it was there. */
static bool tar_map_lookup(Map *m, Str k, Str *v) {
    *v = (Str){tar_empty, 0};
    if (m == NULL)
        return false;
    const Str *p = (const Str *)map_get(m, &k);
    if (p == NULL)
        return false;
    *v = *p;
    return true;
}

static Str tar_map_value(Map *m, Str k) {
    Str v;
    tar_map_lookup(m, k, &v);
    return v;
}

/* ------------------------------------------------------------------ header */

static const Type tar_header_desc = {
    {(const Byte *)"Header", 6},
    {(const Byte *)"archive/tar", 11},
    KIND_STRUCT,
    (uint32_t)sizeof(TarHeader),
    (uint16_t)_Alignof(TarHeader),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x74686472U, /* "thdr" */
    NULL,
};

const Type *const TYPE_TAR_HEADER = &tar_header_desc;

void tar_header_free(Alloc *a, TarHeader *h) {
    if (h == NULL)
        return;
    tar_str_free(a, h->name);
    tar_str_free(a, h->linkname);
    tar_str_free(a, h->uname);
    tar_str_free(a, h->gname);
    burrow__tar_str_map_free(a, h->xattrs);
    burrow__tar_str_map_free(a, h->pax_records);
    mem_free(a, h, sizeof *h, _Alignof(TarHeader));
}

static TarHeader *tar_header_new(Alloc *a) {
    TarHeader *h = (TarHeader *)mem_alloc(a, sizeof *h, _Alignof(TarHeader));
    if (h == NULL)
        return NULL;
    h->name = h->linkname = h->uname = h->gname = (Str){tar_empty, 0};
    return h;
}

/* ------------------------------------------------------------------- blocks */

static void tar_block_checksum(const Byte *b, int64_t *unsigned_sum,
                               int64_t *signed_sum) {
    int64_t u = 0;
    int64_t s = 0;
    for (Int i = 0; i < TAR_BLOCK_SIZE; i++) {
        Byte c = b[i];
        if (i >= 148 && i < 156)
            c = ' ';
        u += c;
        s += (int8_t)c;
    }
    *unsigned_sum = u;
    *signed_sum = s;
}

TarFormat burrow__tar_block_get_format(const Byte *blk) {
    Error perr = BURROW_NO_ERROR;
    int64_t value = burrow__tar_parse_octal(blk + TAR_V7_CHKSUM, 8, &perr);
    int64_t c1;
    int64_t c2;
    tar_block_checksum(blk, &c1, &c2);
    if (BURROW_FAILED(perr) || (value != c1 && value != c2))
        return TAR_FORMAT_UNKNOWN;
    const Byte *magic = blk + TAR_USTAR_MAGIC;
    const Byte *version = blk + TAR_USTAR_VERSION;
    const Byte *trailer = blk + TAR_STAR_TRAILER;
    bool ustar = memcmp(magic, "ustar\0", 6) == 0;
    if (ustar && memcmp(trailer, "tar\0", 4) == 0)
        return TAR_FORMAT_STAR;
    if (ustar)
        return TAR_FORMAT_USTAR | TAR_FORMAT_PAX;
    if (memcmp(magic, "ustar ", 6) == 0 && memcmp(version, " \0", 2) == 0)
        return TAR_FORMAT_GNU;
    return TAR_FORMAT_V7;
}

void burrow__tar_block_set_format(Byte *blk, TarFormat f) {
    if ((f & TAR_FORMAT_V7) != 0) {
        /* Nothing to set. */
    } else if ((f & TAR_FORMAT_GNU) != 0) {
        memcpy(blk + TAR_USTAR_MAGIC, "ustar ", 6);
        memcpy(blk + TAR_USTAR_VERSION, " \0", 2);
    } else if ((f & TAR_FORMAT_STAR) != 0) {
        memcpy(blk + TAR_USTAR_MAGIC, "ustar\0", 6);
        memcpy(blk + TAR_USTAR_VERSION, "00", 2);
        memcpy(blk + TAR_STAR_TRAILER, "tar\0", 4);
    } else if ((f & (TAR_FORMAT_USTAR | TAR_FORMAT_PAX)) != 0) {
        memcpy(blk + TAR_USTAR_MAGIC, "ustar\0", 6);
        memcpy(blk + TAR_USTAR_VERSION, "00", 2);
    } else {
        panic_str(BURROW_S("invalid format"));
    }
    int64_t sum;
    int64_t unused;
    tar_block_checksum(blk, &sum, &unused);
    Error ferr = BURROW_NO_ERROR;
    burrow__tar_format_octal(blk + TAR_V7_CHKSUM, 7, sum, &ferr);
    blk[TAR_V7_CHKSUM + 7] = ' ';
}

/* ------------------------------------------------------------ sparse maps */

void burrow__tar_sparse_list_free(Alloc *a, TarSparseList *l) {
    if (l->p != NULL)
        mem_free(a, l->p, (size_t)l->cap * sizeof(TarSparseEntry),
                 _Alignof(TarSparseEntry));
    l->p = NULL;
    l->len = l->cap = 0;
}

static bool tar_sparse_list_reserve(Alloc *a, TarSparseList *l, Int n) {
    if (l->cap >= n)
        return true;
    Int ncap = l->cap < 4 ? 4 : l->cap * 2;
    if (ncap < n)
        ncap = n;
    TarSparseEntry *np =
        l->p == NULL
            ? (TarSparseEntry *)mem_alloc_nozero(
                  a, (size_t)ncap * sizeof(TarSparseEntry), _Alignof(TarSparseEntry))
            : (TarSparseEntry *)mem_realloc(
                  a, l->p, (size_t)l->cap * sizeof(TarSparseEntry),
                  (size_t)ncap * sizeof(TarSparseEntry), _Alignof(TarSparseEntry));
    if (np == NULL)
        return false;
    l->p = np;
    l->cap = ncap;
    return true;
}

static Error tar_append_sparse_entry(Alloc *a, TarSparseList *l, int64_t offset,
                                     int64_t length) {
    if (l->len >= TAR_MAX_SPARSE_FILE_ENTRIES)
        return burrow__tar_err_sparse_too_long;
    if (!tar_sparse_list_reserve(a, l, l->len + 1))
        return burrow_err_out_of_memory;
    l->p[l->len++] = (TarSparseEntry){offset, length};
    return BURROW_NO_ERROR;
}

bool burrow__tar_validate_sparse_entries(const TarSparseEntry *sp, Int n,
                                         int64_t size) {
    if (size < 0)
        return false;
    TarSparseEntry pre = {0, 0};
    for (Int i = 0; i < n; i++) {
        TarSparseEntry cur = sp[i];
        if (cur.offset < 0 || cur.length < 0)
            return false;
        if (cur.offset > INT64_MAX - cur.length)
            return false;
        if (cur.offset + cur.length > size)
            return false;
        if (pre.offset + pre.length > cur.offset)
            return false;
        pre = cur;
    }
    return true;
}

Int burrow__tar_align_sparse_entries(TarSparseEntry *sp, Int n, int64_t size) {
    Int d = 0;
    for (Int i = 0; i < n; i++) {
        int64_t pos = sp[i].offset;
        int64_t end = sp[i].offset + sp[i].length;
        pos += tar_block_padding(pos);
        if (end != size)
            end -= tar_block_padding(-end);
        if (pos < end)
            sp[d++] = (TarSparseEntry){pos, end - pos};
    }
    return d;
}

Int burrow__tar_invert_sparse_entries(TarSparseEntry *sp, Int n, int64_t size) {
    Int d = 0;
    TarSparseEntry pre = {0, 0};
    for (Int i = 0; i < n; i++) {
        TarSparseEntry cur = sp[i];
        if (cur.length == 0)
            continue;
        pre.length = cur.offset - pre.offset;
        if (pre.length > 0)
            sp[d++] = pre;
        pre.offset = cur.offset + cur.length;
    }
    pre.length = size - pre.offset;
    sp[d++] = pre;
    return d;
}

/* ------------------------------------------------------------ file reading */

/* regFileReader.Read, which a sparse file reads its data through. */
static Int tar_reg_read(TarFileReader *fr, Slice b, Error *err) {
    Int n = 0;
    Error e = BURROW_NO_ERROR;
    if ((int64_t)b.len > fr->nb)
        b.len = (Int)fr->nb;
    if (b.len > 0) {
        n = BURROW_CALL(fr->r, read, b, &e);
        fr->nb -= n;
    }
    if (tar_same(e, io_eof) && fr->nb > 0)
        e = io_err_unexpected_eof;
    else if (BURROW_OK(e) && fr->nb == 0)
        e = io_eof;
    *err = e;
    return n;
}

static Int tar_reg_try_read_full(TarFileReader *fr, Byte *p, Int len, Error *err) {
    Int n = 0;
    Error e = BURROW_NO_ERROR;
    while (len > n && BURROW_OK(e))
        n += tar_reg_read(fr, tar_bytes(p + n, len - n), &e);
    if (len == n && tar_same(e, io_eof))
        e = BURROW_NO_ERROR;
    *err = e;
    return n;
}

int64_t burrow__tar_file_reader_logical_remaining(const TarFileReader *fr) {
    if (fr->sp == NULL)
        return fr->nb;
    const TarSparseEntry *last = &fr->sp[fr->sp_len - 1];
    return last->offset + last->length - fr->pos;
}

int64_t burrow__tar_file_reader_physical_remaining(const TarFileReader *fr) {
    return fr->nb;
}

static Int tar_sparse_read(TarFileReader *sr, Slice b, Error *err) {
    Error e = BURROW_NO_ERROR;
    bool finished = (int64_t)b.len >= burrow__tar_file_reader_logical_remaining(sr);
    if (finished)
        b.len = (Int)burrow__tar_file_reader_logical_remaining(sr);
    Byte *p = (Byte *)b.p;
    Int left = b.len;
    int64_t end_pos = sr->pos + b.len;
    while (end_pos > sr->pos && BURROW_OK(e)) {
        Int nf;
        int64_t hole_start = sr->sp[0].offset;
        int64_t hole_end = sr->sp[0].offset + sr->sp[0].length;
        if (sr->pos < hole_start) {
            Int bf = (Int)tar_min64(left, hole_start - sr->pos);
            nf = tar_reg_try_read_full(sr, p, bf, &e);
        } else {
            Int bf = (Int)tar_min64(left, hole_end - sr->pos);
            if (bf > 0)
                memset(p, 0, (size_t)bf);
            nf = bf;
        }
        p += nf;
        left -= nf;
        sr->pos += nf;
        if (sr->pos >= hole_end && sr->sp_len > 1) {
            sr->sp++;
            sr->sp_len--;
        }
    }
    Int n = b.len - left;
    if (tar_same(e, io_eof))
        e = burrow__tar_err_miss_data;
    else if (BURROW_FAILED(e))
        (void)0;
    else if (burrow__tar_file_reader_logical_remaining(sr) == 0 &&
             burrow__tar_file_reader_physical_remaining(sr) > 0)
        e = burrow__tar_err_unref_data;
    else if (finished)
        e = io_eof;
    *err = e;
    return n;
}

Int burrow__tar_file_reader_read(TarFileReader *fr, Slice b, Error *err) {
    if (fr->sp == NULL)
        return tar_reg_read(fr, b, err);
    return tar_sparse_read(fr, b, err);
}

/* The two readers io.Copy gets, struct{ io.Reader }{fr} with none of fr's
 * other methods, and the data under a sparse file on its own. */
static const Type tar_file_reader_desc = {
    {(const Byte *)"fileReader", 10},
    {(const Byte *)"archive/tar", 11},
    KIND_STRUCT,
    (uint32_t)sizeof(TarFileReader),
    (uint16_t)_Alignof(TarFileReader),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x7466726EU, /* "tfrn" */
    NULL,
};

static Int tar_vt_file_read(void *self, Slice b, Error *err) {
    return burrow__tar_file_reader_read((TarFileReader *)self, b, err);
}

static Int tar_vt_reg_read(void *self, Slice b, Error *err) {
    return tar_reg_read((TarFileReader *)self, b, err);
}

static const IoReaderVT tar_file_reader_vt = {&tar_file_reader_desc, tar_vt_file_read};
static const IoReaderVT tar_reg_reader_vt = {&tar_file_reader_desc, tar_vt_reg_read};

static int64_t tar_sparse_write_to(TarFileReader *sr, Alloc *a, IoWriter w,
                                   Error *err) {
    const Method *seek = burrow__io_seek_method(w.vt->self_type);
    if (seek != NULL) {
        Error se = BURROW_NO_ERROR;
        burrow__io_seek(seek, w.data, 0, BURROW_IO_SEEK_CURRENT, &se);
        if (BURROW_FAILED(se))
            seek = NULL;
    }
    if (seek == NULL)
        return io_copy(a, w, (IoReader){&tar_file_reader_vt, sr}, err);

    Error e = BURROW_NO_ERROR;
    bool write_last_byte = false;
    int64_t pos0 = sr->pos;
    while (burrow__tar_file_reader_logical_remaining(sr) > 0 && !write_last_byte &&
           BURROW_OK(e)) {
        int64_t nf;
        int64_t hole_start = sr->sp[0].offset;
        int64_t hole_end = sr->sp[0].offset + sr->sp[0].length;
        if (sr->pos < hole_start) {
            nf = hole_start - sr->pos;
            nf = io_copy_n(a, w, (IoReader){&tar_reg_reader_vt, sr}, nf, &e);
        } else {
            nf = hole_end - sr->pos;
            if (burrow__tar_file_reader_physical_remaining(sr) == 0) {
                write_last_byte = true;
                nf--;
            }
            burrow__io_seek(seek, w.data, nf, BURROW_IO_SEEK_CURRENT, &e);
        }
        sr->pos += nf;
        if (sr->pos >= hole_end && sr->sp_len > 1) {
            sr->sp++;
            sr->sp_len--;
        }
    }
    if (write_last_byte && BURROW_OK(e)) {
        static const Byte zero[1] = {0};
        BURROW_CALL(w, write, tar_bytes(zero, 1), &e);
        sr->pos++;
    }
    int64_t n = sr->pos - pos0;
    if (tar_same(e, io_eof))
        e = burrow__tar_err_miss_data;
    else if (BURROW_OK(e) && burrow__tar_file_reader_logical_remaining(sr) == 0 &&
             burrow__tar_file_reader_physical_remaining(sr) > 0)
        e = burrow__tar_err_unref_data;
    *err = e;
    return n;
}

int64_t burrow__tar_file_reader_write_to(TarFileReader *fr, Alloc *a, IoWriter w,
                                         Error *err) {
    if (fr->sp == NULL)
        return io_copy(a, w, (IoReader){&tar_file_reader_vt, fr}, err);
    return tar_sparse_write_to(fr, a, w, err);
}

/* ------------------------------------------------------------ file writing */

static Int tar_reg_write(TarFileWriter *fw, Slice b, Error *err) {
    Int n = 0;
    Error e = BURROW_NO_ERROR;
    bool overwrite = (int64_t)b.len > fw->nb;
    if (overwrite)
        b.len = (Int)fw->nb;
    if (b.len > 0) {
        n = BURROW_CALL(fw->w, write, b, &e);
        fw->nb -= n;
    }
    if (BURROW_OK(e) && overwrite)
        e = tar_err_write_too_long;
    *err = e;
    return n;
}

int64_t burrow__tar_file_writer_logical_remaining(const TarFileWriter *fw) {
    if (fw->sp == NULL)
        return fw->nb;
    const TarSparseEntry *last = &fw->sp[fw->sp_len - 1];
    return last->offset + last->length - fw->pos;
}

int64_t burrow__tar_file_writer_physical_remaining(const TarFileWriter *fw) {
    return fw->nb;
}

/* zeroWriter.Write. */
static Int tar_zero_write(const Byte *p, Int n, Error *err) {
    for (Int i = 0; i < n; i++) {
        if (p[i] != 0) {
            *err = burrow__tar_err_write_hole;
            return i;
        }
    }
    *err = BURROW_NO_ERROR;
    return n;
}

static Int tar_sparse_write(TarFileWriter *sw, Slice b, Error *err) {
    Error e = BURROW_NO_ERROR;
    bool overwrite = (int64_t)b.len > burrow__tar_file_writer_logical_remaining(sw);
    if (overwrite)
        b.len = (Int)burrow__tar_file_writer_logical_remaining(sw);
    const Byte *p = (const Byte *)b.p;
    Int left = b.len;
    int64_t end_pos = sw->pos + b.len;
    while (end_pos > sw->pos && BURROW_OK(e)) {
        Int nf;
        int64_t data_start = sw->sp[0].offset;
        int64_t data_end = sw->sp[0].offset + sw->sp[0].length;
        if (sw->pos < data_start) {
            Int bf = (Int)tar_min64(left, data_start - sw->pos);
            nf = tar_zero_write(p, bf, &e);
        } else {
            Int bf = (Int)tar_min64(left, data_end - sw->pos);
            nf = tar_reg_write(sw, tar_bytes(p, bf), &e);
        }
        p += nf;
        left -= nf;
        sw->pos += nf;
        if (sw->pos >= data_end && sw->sp_len > 1) {
            sw->sp++;
            sw->sp_len--;
        }
    }
    Int n = b.len - left;
    if (tar_same(e, tar_err_write_too_long))
        e = burrow__tar_err_miss_data;
    else if (BURROW_FAILED(e))
        (void)0;
    else if (burrow__tar_file_writer_logical_remaining(sw) == 0 &&
             burrow__tar_file_writer_physical_remaining(sw) > 0)
        e = burrow__tar_err_unref_data;
    else if (overwrite)
        e = tar_err_write_too_long;
    *err = e;
    return n;
}

Int burrow__tar_file_writer_write(TarFileWriter *fw, Slice b, Error *err) {
    if (fw->sp == NULL)
        return tar_reg_write(fw, b, err);
    return tar_sparse_write(fw, b, err);
}

static const Type tar_file_writer_desc = {
    {(const Byte *)"fileWriter", 10},
    {(const Byte *)"archive/tar", 11},
    KIND_STRUCT,
    (uint32_t)sizeof(TarFileWriter),
    (uint16_t)_Alignof(TarFileWriter),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x7466776EU, /* "tfwn" */
    NULL,
};

static Int tar_vt_file_write(void *self, Slice b, Error *err) {
    return burrow__tar_file_writer_write((TarFileWriter *)self, b, err);
}

static Int tar_vt_reg_write(void *self, Slice b, Error *err) {
    return tar_reg_write((TarFileWriter *)self, b, err);
}

static const IoWriterVT tar_file_writer_vt = {&tar_file_writer_desc, tar_vt_file_write};
static const IoWriterVT tar_reg_writer_vt = {&tar_file_writer_desc, tar_vt_reg_write};

/* tryReadFull, on any reader. */
static Int tar_try_read_full(IoReader r, Byte *p, Int len, Error *err) {
    Int n = 0;
    Error e = BURROW_NO_ERROR;
    while (len > n && BURROW_OK(e))
        n += BURROW_CALL(r, read, tar_bytes(p + n, len - n), &e);
    if (len == n && tar_same(e, io_eof))
        e = BURROW_NO_ERROR;
    *err = e;
    return n;
}

static Int tar_must_read_full(IoReader r, Byte *p, Int len, Error *err) {
    Int n = tar_try_read_full(r, p, len, err);
    if (tar_same(*err, io_eof))
        *err = io_err_unexpected_eof;
    return n;
}

static Error tar_ensure_eof(IoReader r) {
    Byte b[1];
    Error e;
    Int n = tar_try_read_full(r, b, 1, &e);
    if (n > 0)
        return tar_err_write_too_long;
    if (tar_same(e, io_eof))
        return BURROW_NO_ERROR;
    return e;
}

static int64_t tar_sparse_read_from(TarFileWriter *sw, Alloc *a, IoReader r,
                                    Error *err) {
    const Method *seek = burrow__io_seek_method(r.vt->self_type);
    if (seek != NULL) {
        Error se = BURROW_NO_ERROR;
        burrow__io_seek(seek, r.data, 0, BURROW_IO_SEEK_CURRENT, &se);
        if (BURROW_FAILED(se))
            seek = NULL;
    }
    if (seek == NULL)
        return io_copy(a, (IoWriter){&tar_file_writer_vt, sw}, r, err);

    Error e = BURROW_NO_ERROR;
    bool read_last_byte = false;
    int64_t pos0 = sw->pos;
    while (burrow__tar_file_writer_logical_remaining(sw) > 0 && !read_last_byte &&
           BURROW_OK(e)) {
        int64_t nf;
        int64_t data_start = sw->sp[0].offset;
        int64_t data_end = sw->sp[0].offset + sw->sp[0].length;
        if (sw->pos < data_start) {
            nf = data_start - sw->pos;
            if (burrow__tar_file_writer_physical_remaining(sw) == 0) {
                read_last_byte = true;
                nf--;
            }
            burrow__io_seek(seek, r.data, nf, BURROW_IO_SEEK_CURRENT, &e);
        } else {
            nf = data_end - sw->pos;
            nf = io_copy_n(a, (IoWriter){&tar_reg_writer_vt, sw}, r, nf, &e);
        }
        sw->pos += nf;
        if (sw->pos >= data_end && sw->sp_len > 1) {
            sw->sp++;
            sw->sp_len--;
        }
    }
    if (read_last_byte && BURROW_OK(e)) {
        Byte b[1];
        tar_must_read_full(r, b, 1, &e);
        sw->pos++;
    }
    int64_t n = sw->pos - pos0;
    if (tar_same(e, io_eof))
        e = io_err_unexpected_eof;
    else if (tar_same(e, tar_err_write_too_long))
        e = burrow__tar_err_miss_data;
    else if (BURROW_FAILED(e))
        (void)0;
    else if (burrow__tar_file_writer_logical_remaining(sw) == 0 &&
             burrow__tar_file_writer_physical_remaining(sw) > 0)
        e = burrow__tar_err_unref_data;
    else
        e = tar_ensure_eof(r);
    *err = e;
    return n;
}

int64_t burrow__tar_file_writer_read_from(TarFileWriter *fw, Alloc *a, IoReader r,
                                          Error *err) {
    if (fw->sp == NULL)
        return io_copy(a, (IoWriter){&tar_file_writer_vt, fw}, r, err);
    return tar_sparse_read_from(fw, a, r, err);
}

/* ---------------------------------------------------------- allowedFormats */

typedef struct TarVerify {
    Alloc *a;
    const TarHeader *h;
    Map *pax;
    TarFormat format;
    bool prefer_pax;
    bool oom;
    TarWhy no_ustar;
    TarWhy no_pax;
    TarWhy no_gnu;
} TarVerify;

static void tar_verify_set(TarVerify *v, Str k, Str val) {
    if (!v->oom && !burrow__tar_str_map_set(v->a, v->pax, k, val))
        v->oom = true;
}

static void tar_why(TarWhy *w, int kind, const char *what, const char *name) {
    *w = (TarWhy){kind, what, name, {tar_empty, 0}, 0, {0}};
}

static void tar_verify_string(TarVerify *v, Str s, Int size, const char *name,
                              Str pax_key) {
    bool too_long = s.len > size;
    bool allow_long_gnu =
        str_eq(pax_key, TAR_PAX_PATH) || str_eq(pax_key, TAR_PAX_LINKPATH);
    if (tar_has_nul(s) || (too_long && !allow_long_gnu)) {
        tar_why(&v->no_gnu, TAR_WHY_STR, "GNU cannot encode ", name);
        v->no_gnu.s = s;
        v->format &= ~TAR_FORMAT_GNU;
    }
    if (!tar_is_ascii(s) || too_long) {
        bool can_split = str_eq(pax_key, TAR_PAX_PATH);
        Str pre;
        Str suf;
        if (!can_split || !burrow__tar_split_ustar_path(s, &pre, &suf)) {
            tar_why(&v->no_ustar, TAR_WHY_STR, "USTAR cannot encode ", name);
            v->no_ustar.s = s;
            v->format &= ~TAR_FORMAT_USTAR;
        }
        if (pax_key.len == 0) {
            tar_why(&v->no_pax, TAR_WHY_STR, "PAX cannot encode ", name);
            v->no_pax.s = s;
            v->format &= ~TAR_FORMAT_PAX;
        } else {
            tar_verify_set(v, pax_key, s);
        }
    }
    Str have;
    if (tar_map_lookup(v->h->pax_records, pax_key, &have) && str_eq(have, s))
        tar_verify_set(v, pax_key, have);
}

static void tar_verify_numeric(TarVerify *v, int64_t n, Int size, const char *name,
                               Str pax_key) {
    Byte d[21];
    Int i = tar_itoa(d, n);
    Str text = str_from_bytes(d + i, 21 - i);
    if (!burrow__tar_fits_in_base256(size, n)) {
        tar_why(&v->no_gnu, TAR_WHY_NUM, "GNU cannot encode ", name);
        v->no_gnu.n = n;
        v->format &= ~TAR_FORMAT_GNU;
    }
    if (!burrow__tar_fits_in_octal(size, n)) {
        tar_why(&v->no_ustar, TAR_WHY_NUM, "USTAR cannot encode ", name);
        v->no_ustar.n = n;
        v->format &= ~TAR_FORMAT_USTAR;
        if (pax_key.len == 0) {
            tar_why(&v->no_pax, TAR_WHY_NUM, "PAX cannot encode ", name);
            v->no_pax.n = n;
            v->format &= ~TAR_FORMAT_PAX;
        } else {
            tar_verify_set(v, pax_key, text);
        }
    }
    Str have;
    if (tar_map_lookup(v->h->pax_records, pax_key, &have) && str_eq(have, text))
        tar_verify_set(v, pax_key, have);
}

static void tar_verify_time(TarVerify *v, Time ts, Int size, const char *name,
                            Str pax_key) {
    if (time_is_zero(ts))
        return;
    Byte buf[32];
    Str text = str_from_bytes(buf, tar_pax_time_into(buf, ts));
    int64_t secs = time_unix(ts);
    if (!burrow__tar_fits_in_base256(size, secs)) {
        tar_why(&v->no_gnu, TAR_WHY_TIME, "GNU cannot encode ", name);
        v->no_gnu.t = ts;
        v->format &= ~TAR_FORMAT_GNU;
    }
    bool is_mtime = str_eq(pax_key, TAR_PAX_MTIME);
    bool fits_octal = burrow__tar_fits_in_octal(size, secs);
    if ((is_mtime && !fits_octal) || !is_mtime) {
        tar_why(&v->no_ustar, TAR_WHY_TIME, "USTAR cannot encode ", name);
        v->no_ustar.t = ts;
        v->format &= ~TAR_FORMAT_USTAR;
    }
    bool needs_nano = time_nanosecond(ts) != 0;
    if (!is_mtime || !fits_octal || needs_nano) {
        v->prefer_pax = true;
        if (pax_key.len == 0) {
            tar_why(&v->no_pax, TAR_WHY_TIME, "PAX cannot encode ", name);
            v->no_pax.t = ts;
            v->format &= ~TAR_FORMAT_PAX;
        } else {
            tar_verify_set(v, pax_key, text);
        }
    }
    Str have;
    if (tar_map_lookup(v->h->pax_records, pax_key, &have) && str_eq(have, text))
        tar_verify_set(v, pax_key, have);
}

static bool tar_is_basic_key(Str k) {
    const Str keys[] = {
        TAR_PAX_PATH,  TAR_PAX_LINKPATH, TAR_PAX_SIZE,  TAR_PAX_UID,   TAR_PAX_GID,
        TAR_PAX_UNAME, TAR_PAX_GNAME,    TAR_PAX_MTIME, TAR_PAX_ATIME, TAR_PAX_CTIME,
    };
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++)
        if (str_eq(k, keys[i]))
            return true;
    return false;
}

static Error tar_invalid_pax_record(Alloc *a, Str k, Str v) {
    TarBuf b = {a, NULL, 0, 0, false};
    tar_buf_str(&b, k);
    tar_buf_cstr(&b, " = ");
    tar_buf_str(&b, v);
    TarWhy w = {TAR_WHY_STR, "invalid PAX record: ", "", tar_buf_view(&b), 0, {0}};
    /* The why has no name and no "=", so it is written by hand. */
    TarBuf m = {a, NULL, 0, 0, false};
    tar_buf_cstr(&m, "archive/tar: cannot encode header: ");
    tar_buf_cstr(&m, w.what);
    tar_buf_quote(&m, w.s);
    Error err = b.oom || m.oom
                    ? burrow_err_out_of_memory
                    : tar_header_error_box(error_allocator(), tar_buf_view(&m));
    tar_buf_free(&m);
    tar_buf_free(&b);
    return err;
}

TarFormat burrow__tar_allowed_formats(Alloc *a, const TarHeader *h, Map **pax,
                                      Error *err) {
    *pax = NULL;
    *err = BURROW_NO_ERROR;
    TarVerify v;
    memset(&v, 0, sizeof v);
    v.a = a;
    v.h = h;
    v.pax = burrow__tar_str_map(a);
    v.format = TAR_FORMAT_USTAR | TAR_FORMAT_PAX | TAR_FORMAT_GNU;
    if (v.pax == NULL) {
        *err = burrow_err_out_of_memory;
        return TAR_FORMAT_UNKNOWN;
    }
    tar_verify_string(&v, h->name, 100, "Name", TAR_PAX_PATH);
    tar_verify_string(&v, h->linkname, 100, "Linkname", TAR_PAX_LINKPATH);
    tar_verify_string(&v, h->uname, 32, "Uname", TAR_PAX_UNAME);
    tar_verify_string(&v, h->gname, 32, "Gname", TAR_PAX_GNAME);
    tar_verify_numeric(&v, h->mode, 8, "Mode", TAR_LIT(""));
    tar_verify_numeric(&v, (int64_t)h->uid, 8, "Uid", TAR_PAX_UID);
    tar_verify_numeric(&v, (int64_t)h->gid, 8, "Gid", TAR_PAX_GID);
    tar_verify_numeric(&v, h->size, 12, "Size", TAR_PAX_SIZE);
    tar_verify_numeric(&v, h->devmajor, 8, "Devmajor", TAR_LIT(""));
    tar_verify_numeric(&v, h->devminor, 8, "Devminor", TAR_LIT(""));
    tar_verify_time(&v, h->mod_time, 12, "ModTime", TAR_PAX_MTIME);
    tar_verify_time(&v, h->access_time, 12, "AccessTime", TAR_PAX_ATIME);
    tar_verify_time(&v, h->change_time, 12, "ChangeTime", TAR_PAX_CTIME);

    TarWhy only_pax;
    TarWhy only_gnu;
    tar_why(&only_pax, TAR_WHY_NONE, "", NULL);
    tar_why(&only_gnu, TAR_WHY_NONE, "", NULL);
    Error fail = BURROW_NO_ERROR;
    switch (h->typeflag) {
    case TAR_TYPE_REG:
    case TAR_TYPE_CHAR:
    case TAR_TYPE_BLOCK:
    case TAR_TYPE_FIFO:
    case TAR_TYPE_GNU_SPARSE:
        if (h->name.len > 0 && h->name.p[h->name.len - 1] == '/')
            fail = tar_header_error_text(a, "filename may not have trailing slash");
        break;
    case TAR_TYPE_X_HEADER:
    case TAR_TYPE_GNU_LONG_NAME:
    case TAR_TYPE_GNU_LONG_LINK:
        fail = tar_header_error_text(a, "cannot manually encode TypeXHeader, "
                                        "TypeGNULongName, or TypeGNULongLink headers");
        break;
    case TAR_TYPE_X_GLOBAL_HEADER:
        if (h->linkname.len != 0 || h->size != 0 || h->mode != 0 || h->uid != 0 ||
            h->gid != 0 || h->uname.len != 0 || h->gname.len != 0 ||
            !time_is_zero(h->mod_time) || !time_is_zero(h->access_time) ||
            !time_is_zero(h->change_time) || h->devmajor != 0 || h->devminor != 0) {
            fail = tar_header_error_text(
                a, "only PAXRecords should be set for TypeXGlobalHeader");
            break;
        }
        tar_why(&only_pax, TAR_WHY_TEXT, "only PAX supports TypeXGlobalHeader", NULL);
        v.format &= TAR_FORMAT_PAX;
        break;
    default:
        break;
    }
    if (BURROW_OK(fail) && !tar_is_header_only_type(h->typeflag) && h->size < 0)
        fail = tar_header_error_text(a, "negative size on header-only type");
    if (BURROW_FAILED(fail)) {
        burrow__tar_str_map_free(a, v.pax);
        *err = fail;
        return TAR_FORMAT_UNKNOWN;
    }

    if (h->xattrs != NULL && map_len(h->xattrs) > 0) {
        TarBuf key = {a, NULL, 0, 0, false};
        MapIter it = map_iter(h->xattrs);
        const void *k;
        void *val;
        while (map_next(&it, &k, &val)) {
            key.len = 0;
            tar_buf_str(&key, TAR_PAX_SCHILY_XATTR);
            tar_buf_str(&key, *(const Str *)k);
            if (key.oom)
                v.oom = true;
            else
                tar_verify_set(&v, tar_buf_view(&key), *(const Str *)val);
        }
        tar_buf_free(&key);
        tar_why(&only_pax, TAR_WHY_TEXT, "only PAX supports Xattrs", NULL);
        v.format &= TAR_FORMAT_PAX;
    }
    if (h->pax_records != NULL && map_len(h->pax_records) > 0) {
        MapIter it = map_iter(h->pax_records);
        const void *kp;
        void *vp;
        while (map_next(&it, &kp, &vp)) {
            Str k = *(const Str *)kp;
            Str unused;
            if (tar_map_lookup(v.pax, k, &unused))
                continue;
            if (h->typeflag == TAR_TYPE_X_GLOBAL_HEADER ||
                (!tar_is_basic_key(k) && !strings_has_prefix(k, TAR_PAX_GNU_SPARSE)))
                tar_verify_set(&v, k, *(const Str *)vp);
        }
        tar_why(&only_pax, TAR_WHY_TEXT, "only PAX supports PAXRecords", NULL);
        v.format &= TAR_FORMAT_PAX;
    }
    if (v.oom) {
        burrow__tar_str_map_free(a, v.pax);
        *err = burrow_err_out_of_memory;
        return TAR_FORMAT_UNKNOWN;
    }
    {
        MapIter it = map_iter(v.pax);
        const void *kp;
        void *vp;
        while (map_next(&it, &kp, &vp)) {
            if (!burrow__tar_valid_pax_record(*(const Str *)kp, *(const Str *)vp)) {
                Error e = tar_invalid_pax_record(a, *(const Str *)kp, *(const Str *)vp);
                burrow__tar_str_map_free(a, v.pax);
                *err = e;
                return TAR_FORMAT_UNKNOWN;
            }
        }
    }

    TarFormat want = h->format;
    if (want != TAR_FORMAT_UNKNOWN) {
        if ((want & TAR_FORMAT_PAX) != 0 && !v.prefer_pax)
            want |= TAR_FORMAT_USTAR;
        v.format &= want;
    }
    if (v.format == TAR_FORMAT_UNKNOWN) {
        TarWhy whys[5];
        int n = 0;
        switch (h->format) {
        case TAR_FORMAT_USTAR:
            tar_why(&whys[n++], TAR_WHY_TEXT, "Format specifies USTAR", NULL);
            whys[n++] = v.no_ustar;
            whys[n++] = only_pax;
            whys[n++] = only_gnu;
            break;
        case TAR_FORMAT_PAX:
            tar_why(&whys[n++], TAR_WHY_TEXT, "Format specifies PAX", NULL);
            whys[n++] = v.no_pax;
            whys[n++] = only_gnu;
            break;
        case TAR_FORMAT_GNU:
            tar_why(&whys[n++], TAR_WHY_TEXT, "Format specifies GNU", NULL);
            whys[n++] = v.no_gnu;
            whys[n++] = only_pax;
            break;
        default:
            whys[n++] = v.no_ustar;
            whys[n++] = v.no_pax;
            whys[n++] = v.no_gnu;
            whys[n++] = only_pax;
            whys[n++] = only_gnu;
            break;
        }
        *err = tar_header_error(a, whys, n);
    }
    *pax = v.pax;
    return v.format;
}

bool burrow__tar_split_ustar_path(Str name, Str *prefix, Str *suffix) {
    *prefix = *suffix = (Str){tar_empty, 0};
    Int length = name.len;
    if (length <= TAR_NAME_SIZE || !tar_is_ascii(name))
        return false;
    if (length > TAR_PREFIX_SIZE + 1)
        length = TAR_PREFIX_SIZE + 1;
    else if (name.p[length - 1] == '/')
        length--;
    Int i = length - 1;
    while (i >= 0 && name.p[i] != '/')
        i--;
    Int nlen = name.len - i - 1;
    Int plen = i;
    if (i <= 0 || nlen > TAR_NAME_SIZE || nlen == 0 || plen > TAR_PREFIX_SIZE)
        return false;
    *prefix = str_from_bytes(name.p, i);
    *suffix = str_from_bytes(name.p + i + 1, nlen);
    return true;
}

/* -------------------------------------------------------------- FileInfo */

/* path.Base(path.Clean(name)) without building the clean path: each ".."
 * cancels the nearest real element before it, so walking from the end and
 * counting them finds the element that survives. */
static Str tar_clean_base(Str name) {
    if (name.len == 0)
        return BURROW_S(".");
    Int skip = 0;
    Int end = name.len;
    while (end > 0) {
        Int start = end;
        while (start > 0 && name.p[start - 1] != '/')
            start--;
        Str el = str_from_bytes(name.p + start, end - start);
        end = start > 0 ? start - 1 : 0;
        if (el.len == 0 || str_eq(el, BURROW_S(".")))
            continue;
        if (str_eq(el, BURROW_S(".."))) {
            skip++;
            continue;
        }
        if (skip > 0) {
            skip--;
            continue;
        }
        return el;
    }
    if (name.p[0] == '/')
        return BURROW_S("/");
    return skip > 0 ? BURROW_S("..") : BURROW_S(".");
}

static FsFileMode tar_header_mode(const TarHeader *h) {
    FsFileMode mode = fs_file_mode_perm((FsFileMode)h->mode);
    if ((h->mode & TAR_C_ISUID) != 0)
        mode |= FS_MODE_SETUID;
    if ((h->mode & TAR_C_ISGID) != 0)
        mode |= FS_MODE_SETGID;
    if ((h->mode & TAR_C_ISVTX) != 0)
        mode |= FS_MODE_STICKY;
    switch ((FsFileMode)h->mode & ~(FsFileMode)07777) {
    case TAR_C_ISDIR:
        mode |= FS_MODE_DIR;
        break;
    case TAR_C_ISFIFO:
        mode |= FS_MODE_NAMED_PIPE;
        break;
    case TAR_C_ISLNK:
        mode |= FS_MODE_SYMLINK;
        break;
    case TAR_C_ISBLK:
        mode |= FS_MODE_DEVICE;
        break;
    case TAR_C_ISCHR:
        mode |= FS_MODE_DEVICE | FS_MODE_CHAR_DEVICE;
        break;
    case TAR_C_ISSOCK:
        mode |= FS_MODE_SOCKET;
        break;
    default:
        break;
    }
    switch (h->typeflag) {
    case TAR_TYPE_SYMLINK:
        mode |= FS_MODE_SYMLINK;
        break;
    case TAR_TYPE_CHAR:
        mode |= FS_MODE_DEVICE | FS_MODE_CHAR_DEVICE;
        break;
    case TAR_TYPE_BLOCK:
        mode |= FS_MODE_DEVICE;
        break;
    case TAR_TYPE_DIR:
        mode |= FS_MODE_DIR;
        break;
    case TAR_TYPE_FIFO:
        mode |= FS_MODE_NAMED_PIPE;
        break;
    default:
        break;
    }
    return mode;
}

static FsFileMode tar_fi_mode(void *self) {
    return tar_header_mode((const TarHeader *)self);
}

static bool tar_fi_is_dir(void *self) {
    return fs_file_mode_is_dir(tar_fi_mode(self));
}

static Str tar_fi_name(void *self) {
    const TarHeader *h = (const TarHeader *)self;
    if (tar_fi_is_dir(self))
        return tar_clean_base(h->name);
    return path_base(h->name);
}

static int64_t tar_fi_size(void *self) {
    return ((const TarHeader *)self)->size;
}

static Time tar_fi_mod_time(void *self) {
    return ((const TarHeader *)self)->mod_time;
}

static Any tar_fi_sys(void *self) {
    return BURROW_ANY(TYPE_TAR_HEADER, self);
}

static const Type tar_header_file_info_desc = {
    {(const Byte *)"headerFileInfo", 14},
    {(const Byte *)"archive/tar", 11},
    KIND_STRUCT,
    (uint32_t)sizeof(TarHeader *),
    (uint16_t)_Alignof(TarHeader *),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x74686669U, /* "thfi" */
    NULL,
};

static const FsFileInfoVT tar_header_file_info_vt = {
    &tar_header_file_info_desc,
    tar_fi_name,
    tar_fi_size,
    tar_fi_mode,
    tar_fi_mod_time,
    tar_fi_is_dir,
    tar_fi_sys,
};

FsFileInfo tar_header_file_info(TarHeader *h) {
    return (FsFileInfo){&tar_header_file_info_vt, h};
}

/* Uname or Gname on fi's type, when it has one of the FileInfoNames shape. */
static const Method *tar_names_method(const Type *t, Str name) {
    if (t == NULL)
        return NULL;
    const Method *m = type_method_by_name(t, name);
    if (m == NULL || m->ftype == NULL || m->thunk == NULL)
        return NULL;
    if (type_num_in(m->ftype) != 1 || type_num_out(m->ftype) != 1 ||
        type_in(m->ftype, 0) != &burrow_type_IoErrorArg ||
        type_out(m->ftype, 0) != TYPE_STRING)
        return NULL;
    return m;
}

static Str tar_call_name(const Method *m, void *data, Error *err) {
    Error e = BURROW_NO_ERROR;
    IoErrorArg ea = &e;
    Str out = {tar_empty, 0};
    void *args[1] = {(void *)&ea};
    void *rets[1] = {&out};
    method_call(m, data, args, rets);
    *err = e;
    return out;
}

TarHeader *tar_file_info_header(Alloc *a, FsFileInfo fi, Str link, Error *err) {
    if (fi.vt == NULL) {
        BURROW_OUT(err, errors_new(error_allocator(),
                                   BURROW_S("archive/tar: FileInfo is nil")));
        return NULL;
    }
    FsFileMode fm = fi.vt->mode(fi.data);
    TarHeader *h = tar_header_new(a);
    if (h == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    bool ok = true;
    Error e = BURROW_NO_ERROR;
    Str name = fi.vt->name(fi.data);
    h->mod_time = fi.vt->mod_time(fi.data);
    h->mode = (int64_t)fs_file_mode_perm(fm);
    if (fs_file_mode_is_regular(fm)) {
        h->typeflag = TAR_TYPE_REG;
        h->size = fi.vt->size(fi.data);
        h->name = tar_dup(a, name, &ok);
    } else if (fi.vt->is_dir(fi.data)) {
        h->typeflag = TAR_TYPE_DIR;
        TarBuf b = {a, NULL, 0, 0, false};
        tar_buf_str(&b, name);
        tar_buf_byte(&b, '/');
        if (b.oom)
            ok = false;
        else
            h->name = tar_dup(a, tar_buf_view(&b), &ok);
        tar_buf_free(&b);
    } else if ((fm & FS_MODE_SYMLINK) != 0) {
        h->typeflag = TAR_TYPE_SYMLINK;
        h->name = tar_dup(a, name, &ok);
        h->linkname = tar_dup(a, link, &ok);
    } else if ((fm & FS_MODE_DEVICE) != 0) {
        h->typeflag = (fm & FS_MODE_CHAR_DEVICE) != 0 ? TAR_TYPE_CHAR : TAR_TYPE_BLOCK;
        h->name = tar_dup(a, name, &ok);
    } else if ((fm & FS_MODE_NAMED_PIPE) != 0) {
        h->typeflag = TAR_TYPE_FIFO;
        h->name = tar_dup(a, name, &ok);
    } else if ((fm & FS_MODE_SOCKET) != 0) {
        e = errors_new(error_allocator(),
                       BURROW_S("archive/tar: sockets not supported"));
    } else {
        Str ms = fs_file_mode_string(fm, a);
        e = ms.len == 0 ? burrow_err_out_of_memory
                        : fmt_errorf_v("archive/tar: unknown file mode %s", ms);
        tar_str_free(a, ms);
    }
    if (BURROW_OK(e) && ok) {
        if ((fm & FS_MODE_SETUID) != 0)
            h->mode |= TAR_C_ISUID;
        if ((fm & FS_MODE_SETGID) != 0)
            h->mode |= TAR_C_ISGID;
        if ((fm & FS_MODE_STICKY) != 0)
            h->mode |= TAR_C_ISVTX;
        Any sys = fi.vt->sys(fi.data);
        if (sys.t == TYPE_TAR_HEADER && sys.data != NULL) {
            const TarHeader *s = (const TarHeader *)sys.data;
            h->uid = s->uid;
            h->gid = s->gid;
            h->uname = tar_dup(a, s->uname, &ok);
            h->gname = tar_dup(a, s->gname, &ok);
            h->access_time = s->access_time;
            h->change_time = s->change_time;
            h->xattrs = tar_str_map_clone(a, s->xattrs, &ok);
            if (s->typeflag == TAR_TYPE_LINK) {
                h->typeflag = TAR_TYPE_LINK;
                h->size = 0;
                ok = ok && tar_set(a, &h->linkname, s->linkname);
            }
            h->pax_records = tar_str_map_clone(a, s->pax_records, &ok);
        }
    }
    if (BURROW_OK(e) && ok) {
        const Method *gname = tar_names_method(fi.vt->self_type, BURROW_S("Gname"));
        const Method *uname = tar_names_method(fi.vt->self_type, BURROW_S("Uname"));
        if (gname != NULL && uname != NULL) {
            Str g = tar_call_name(gname, fi.data, &e);
            if (BURROW_OK(e))
                ok = tar_set(a, &h->gname, g);
            if (BURROW_OK(e) && ok) {
                Str u = tar_call_name(uname, fi.data, &e);
                if (BURROW_OK(e))
                    ok = tar_set(a, &h->uname, u);
            }
        }
    }
    if (BURROW_OK(e) && !ok)
        e = burrow_err_out_of_memory;
    if (BURROW_FAILED(e)) {
        tar_header_free(a, h);
        BURROW_OUT(err, e);
        return NULL;
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return h;
}

/* -------------------------------------------------------------- IsLocal */

#if defined(BURROW_OS_WINDOWS)
static Byte tar_upper(Byte c) {
    return c >= 'a' && c <= 'z' ? (Byte)(c - 'a' + 'A') : c;
}

static bool tar_equal_fold(Str s, const char *t) {
    if ((size_t)s.len != strlen(t))
        return false;
    for (Int i = 0; i < s.len; i++)
        if (tar_upper(s.p[i]) != tar_upper((Byte)t[i]))
            return false;
    return true;
}

static bool tar_is_reserved_base_name(Str name) {
    if (name.len == 3) {
        Str three = str_from_bytes(name.p, 3);
        if (tar_equal_fold(three, "CON") || tar_equal_fold(three, "PRN") ||
            tar_equal_fold(three, "AUX") || tar_equal_fold(three, "NUL"))
            return true;
    }
    if (name.len >= 4) {
        Str three = str_from_bytes(name.p, 3);
        if (tar_equal_fold(three, "COM") || tar_equal_fold(three, "LPT")) {
            if (name.len == 4 && name.p[3] >= '1' && name.p[3] <= '9')
                return true;
            Str rest = str_from_bytes(name.p + 3, name.len - 3);
            return str_eq(rest, BURROW_S("\xc2\xb2")) ||
                   str_eq(rest, BURROW_S("\xc2\xb3")) ||
                   str_eq(rest, BURROW_S("\xc2\xb9"));
        }
    }
    if (name.len == 6 && name.p[5] == '$' && tar_equal_fold(name, "CONIN$"))
        return true;
    if (name.len == 7 && name.p[6] == '$' && tar_equal_fold(name, "CONOUT$"))
        return true;
    return false;
}

/* isReservedName. Go asks Windows about a reserved name with more after it,
 * like "NUL.txt", and the answer has changed between versions. Here it is
 * reserved, which is the answer that keeps such a name out of a directory. */
static bool tar_is_reserved_name(Str name) {
    Str base = name;
    for (Int i = 0; i < base.len; i++) {
        if (base.p[i] == ':' || base.p[i] == '.') {
            base.len = i;
            break;
        }
    }
    while (base.len > 0 && base.p[base.len - 1] == ' ')
        base.len--;
    return tar_is_reserved_base_name(base);
}

static bool tar_is_sep(Byte c) {
    return c == '/' || c == '\\';
}
#else
static bool tar_is_sep(Byte c) {
    return c == '/';
}
#endif

bool burrow__tar_is_local(Str path) {
    if (path.len == 0 || tar_is_sep(path.p[0]))
        return false;
#if defined(BURROW_OS_WINDOWS)
    if (memchr(path.p, ':', (size_t)path.len) != NULL)
        return false;
#endif
    /* Clean(path) starts with ".." exactly when some ".." has nothing left
     * before it to take away. */
    Int depth = 0;
    bool escapes = false;
    Int i = 0;
    while (i < path.len) {
        Int start = i;
        while (i < path.len && !tar_is_sep(path.p[i]))
            i++;
        Str el = str_from_bytes(path.p + start, i - start);
        if (i < path.len)
            i++;
#if defined(BURROW_OS_WINDOWS)
        if (tar_is_reserved_name(el))
            return false;
#endif
        if (el.len == 0 || str_eq(el, BURROW_S(".")))
            continue;
        if (str_eq(el, BURROW_S(".."))) {
            if (depth == 0)
                escapes = true;
            else
                depth--;
            continue;
        }
        depth++;
    }
    return !escapes;
}

/* ------------------------------------------------------------------ GODEBUG */

enum {
    TAR_DEBUG_KNOWN = 1 << 0,
    TAR_DEBUG_INSECURE_OFF = 1 << 1, /* tarinsecurepath=0 */
};

static uint32_t tar_debug_flags;

/* The value of key in GODEBUG, the last one when it is there twice. */
static bool tar_godebug(const char *env, const char *key, Str *val) {
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

static uint32_t tar_debug_parse(const char *v) {
    uint32_t f = TAR_DEBUG_KNOWN;
    Str s;
    if (v != NULL && tar_godebug(v, "tarinsecurepath", &s) && s.len == 1 &&
        s.p[0] == '0')
        f |= TAR_DEBUG_INSECURE_OFF;
    burrow__atomic_store_relaxed_u32(&tar_debug_flags, f);
    return f;
}

static uint32_t tar_debug_load(void) {
    uint32_t f = burrow__atomic_load_relaxed_u32(&tar_debug_flags);
    if ((f & TAR_DEBUG_KNOWN) != 0)
        return f;
    const char *v = NULL;
    for (const char *const *env = pal_environ(); env != NULL && *env != NULL; env++) {
        if (strncmp(*env, "GODEBUG=", 8) == 0) {
            v = *env + 8;
            break;
        }
    }
    return tar_debug_parse(v);
}

void burrow__tar_godebug_set(const char *value) {
    if (value == NULL)
        burrow__atomic_store_relaxed_u32(&tar_debug_flags, 0);
    else
        (void)tar_debug_parse(value);
}

/* ------------------------------------------------------------------ reading */

static const Type tar_reader_desc = {
    {(const Byte *)"Reader", 6},
    {(const Byte *)"archive/tar", 11},
    KIND_STRUCT,
    (uint32_t)sizeof(TarReader),
    (uint16_t)_Alignof(TarReader),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x74726472U, /* "trdr" */
    NULL,
};

const Type *const TYPE_TAR_READER = &tar_reader_desc;

static Int tar_vt_reader_read(void *self, Slice b, Error *err) {
    return tar_reader_read((TarReader *)self, b, err);
}

static const IoReaderVT tar_reader_vt = {&tar_reader_desc, tar_vt_reader_read};

IoReader tar_reader_as_io_reader(TarReader *tr) {
    return (IoReader){&tar_reader_vt, tr};
}

TarReader *tar_new_reader(Alloc *a, IoReader r) {
    TarReader *tr = (TarReader *)mem_alloc(a, sizeof *tr, _Alignof(TarReader));
    if (tr == NULL)
        return NULL;
    tr->a = a;
    tr->r = r;
    tr->curr.r = r;
    return tr;
}

void tar_reader_free(TarReader *tr) {
    if (tr == NULL)
        return;
    burrow__tar_sparse_list_free(tr->a, &tr->holes);
    mem_free(tr->a, tr, sizeof *tr, _Alignof(TarReader));
}

Int tar_reader_read(TarReader *tr, Slice b, Error *err) {
    if (BURROW_FAILED(tr->err)) {
        BURROW_OUT(err, tr->err);
        return 0;
    }
    Error e = BURROW_NO_ERROR;
    Int n = burrow__tar_file_reader_read(&tr->curr, b, &e);
    if (BURROW_FAILED(e) && !tar_same(e, io_eof))
        tr->err = e;
    BURROW_OUT(err, e);
    return n;
}

int64_t burrow__tar_reader_write_to(TarReader *tr, IoWriter w, Error *err) {
    if (BURROW_FAILED(tr->err)) {
        *err = tr->err;
        return 0;
    }
    int64_t n = burrow__tar_file_reader_write_to(&tr->curr, tr->a, w, err);
    if (BURROW_FAILED(*err))
        tr->err = *err;
    return n;
}

/* readSpecialFile: the rest of r, up to the limit, from a. */
static Slice tar_read_special_file(Alloc *a, IoReader r, Error *err) {
    IoLimitedReader lr = io_limit_reader(r, (int64_t)TAR_MAX_SPECIAL_FILE_SIZE + 1);
    Slice buf = io_read_all(a, io_limited_reader_as_io_reader(&lr), err);
    if (buf.len > TAR_MAX_SPECIAL_FILE_SIZE) {
        mem_free(a, buf.p, (size_t)buf.cap, 1);
        *err = tar_err_field_too_long;
        return slice_nil(TYPE_BYTE);
    }
    return buf;
}

static void tar_slice_free(Alloc *a, Slice s) {
    if (s.p != NULL)
        mem_free(a, s.p, (size_t)s.cap, 1);
}

Map *burrow__tar_parse_pax(Alloc *a, IoReader r, Error *err) {
    Slice buf = tar_read_special_file(a, r, err);
    if (BURROW_FAILED(*err)) {
        tar_slice_free(a, buf);
        return NULL;
    }
    Map *pax = burrow__tar_str_map(a);
    TarBuf sparse = {a, NULL, 0, 0, false};
    Int nsparse = 0;
    Error e = BURROW_NO_ERROR;
    if (pax == NULL)
        e = burrow_err_out_of_memory;
    Str sbuf = str_from_bytes(buf.p, buf.len);
    while (BURROW_OK(e) && sbuf.len > 0) {
        Str key;
        Str value;
        Error re;
        sbuf = burrow__tar_parse_pax_record(sbuf, &key, &value, &re);
        if (BURROW_FAILED(re)) {
            e = tar_err_header;
            break;
        }
        if (str_eq(key, TAR_PAX_GNU_SPARSE_OFFSET) ||
            str_eq(key, TAR_PAX_GNU_SPARSE_NUM_BYTES)) {
            if ((nsparse % 2 == 0 && !str_eq(key, TAR_PAX_GNU_SPARSE_OFFSET)) ||
                (nsparse % 2 == 1 && !str_eq(key, TAR_PAX_GNU_SPARSE_NUM_BYTES)) ||
                (value.len > 0 && memchr(value.p, ',', (size_t)value.len) != NULL)) {
                e = tar_err_header;
                break;
            }
            if (nsparse > 0)
                tar_buf_byte(&sparse, ',');
            tar_buf_str(&sparse, value);
            nsparse++;
        } else if (!burrow__tar_str_map_set(a, pax, key, value)) {
            e = burrow_err_out_of_memory;
        }
    }
    if (BURROW_OK(e) && sparse.oom)
        e = burrow_err_out_of_memory;
    if (BURROW_OK(e) && nsparse > 0 &&
        !burrow__tar_str_map_set(a, pax, TAR_PAX_GNU_SPARSE_MAP, tar_buf_view(&sparse)))
        e = burrow_err_out_of_memory;
    tar_buf_free(&sparse);
    tar_slice_free(a, buf);
    if (BURROW_FAILED(e)) {
        burrow__tar_str_map_free(a, pax);
        *err = e;
        return NULL;
    }
    *err = BURROW_NO_ERROR;
    return pax;
}

Error burrow__tar_merge_pax(Alloc *a, TarHeader *hdr, Map *pax) {
    if (pax != NULL) {
        MapIter it = map_iter(pax);
        const void *kp;
        void *vp;
        while (map_next(&it, &kp, &vp)) {
            Str k = *(const Str *)kp;
            Str v = *(const Str *)vp;
            if (v.len == 0)
                continue;
            Error e = BURROW_NO_ERROR;
            bool ok = true;
            if (str_eq(k, TAR_PAX_PATH)) {
                ok = tar_set(a, &hdr->name, v);
            } else if (str_eq(k, TAR_PAX_LINKPATH)) {
                ok = tar_set(a, &hdr->linkname, v);
            } else if (str_eq(k, TAR_PAX_UNAME)) {
                ok = tar_set(a, &hdr->uname, v);
            } else if (str_eq(k, TAR_PAX_GNAME)) {
                ok = tar_set(a, &hdr->gname, v);
            } else if (str_eq(k, TAR_PAX_UID)) {
                hdr->uid = (Int)strconv_parse_int(v, 10, 64, &e);
            } else if (str_eq(k, TAR_PAX_GID)) {
                hdr->gid = (Int)strconv_parse_int(v, 10, 64, &e);
            } else if (str_eq(k, TAR_PAX_ATIME)) {
                hdr->access_time = burrow__tar_parse_pax_time(v, &e);
            } else if (str_eq(k, TAR_PAX_MTIME)) {
                hdr->mod_time = burrow__tar_parse_pax_time(v, &e);
            } else if (str_eq(k, TAR_PAX_CTIME)) {
                hdr->change_time = burrow__tar_parse_pax_time(v, &e);
            } else if (str_eq(k, TAR_PAX_SIZE)) {
                hdr->size = strconv_parse_int(v, 10, 64, &e);
            } else if (strings_has_prefix(k, TAR_PAX_SCHILY_XATTR)) {
                if (hdr->xattrs == NULL) {
                    hdr->xattrs = burrow__tar_str_map(a);
                    ok = hdr->xattrs != NULL;
                }
                Str name = str_from_bytes(k.p + TAR_PAX_SCHILY_XATTR.len,
                                          k.len - TAR_PAX_SCHILY_XATTR.len);
                ok = ok && burrow__tar_str_map_set(a, hdr->xattrs, name, v);
            }
            if (!ok)
                return burrow_err_out_of_memory;
            if (BURROW_FAILED(e))
                return tar_err_header;
        }
    }
    if (hdr->pax_records != pax)
        burrow__tar_str_map_free(a, hdr->pax_records);
    hdr->pax_records = pax;
    return BURROW_NO_ERROR;
}

static Error tar_handle_regular_file(TarReader *tr, TarHeader *hdr) {
    int64_t nb = hdr->size;
    if (tar_is_header_only_type(hdr->typeflag))
        nb = 0;
    if (nb < 0)
        return tar_err_header;
    tr->pad = tar_block_padding(nb);
    tr->curr = (TarFileReader){tr->r, nb, NULL, 0, 0};
    return BURROW_NO_ERROR;
}

static Error tar_read_old_gnu_sparse_entries(TarReader *tr, TarHeader *hdr, Byte *blk,
                                             TarSparseList *out) {
    if (burrow__tar_block_get_format(blk) != TAR_FORMAT_GNU)
        return tar_err_header;
    hdr->format &= TAR_FORMAT_GNU;
    Error perr = BURROW_NO_ERROR;
    hdr->size = burrow__tar_parse_numeric(blk + TAR_GNU_REAL_SIZE, 12, &perr);
    if (BURROW_FAILED(perr))
        return perr;
    const Byte *s = blk + TAR_GNU_SPARSE;
    Int slen = 24 * 4 + 1;
    Int total = slen;
    while (total < TAR_MAX_SPECIAL_FILE_SIZE) {
        Int max = slen / 24;
        for (Int i = 0; i < max; i++) {
            const Byte *ent = s + i * 24;
            if (ent[0] == 0)
                break;
            int64_t offset = burrow__tar_parse_numeric(ent, 12, &perr);
            int64_t length = burrow__tar_parse_numeric(ent + 12, 12, &perr);
            if (BURROW_FAILED(perr))
                return perr;
            Error e = tar_append_sparse_entry(tr->a, out, offset, length);
            if (BURROW_FAILED(e))
                return e;
        }
        if (s[24 * max] > 0) {
            Error e;
            tar_must_read_full(tr->r, blk, TAR_BLOCK_SIZE, &e);
            if (BURROW_FAILED(e))
                return e;
            s = blk;
            slen = TAR_BLOCK_SIZE;
            total += slen;
            continue;
        }
        return BURROW_NO_ERROR;
    }
    return burrow__tar_err_sparse_too_long;
}

/* readOldGNUSparseMap: the map in the GNU header and the extension blocks
 * after it. On an error the entries read so far are dropped, as Go returns a
 * nil map. */
Error burrow__tar_read_old_gnu_sparse_map(TarReader *tr, TarHeader *hdr, Byte *blk,
                                          TarSparseList *out) {
    Error e = tar_read_old_gnu_sparse_entries(tr, hdr, blk, out);
    if (BURROW_FAILED(e))
        burrow__tar_sparse_list_free(tr->a, out);
    return e;
}

static bool tar_parse_native_int(Str s, int64_t *n) {
    Error e = BURROW_NO_ERROR;
    *n = strconv_parse_int(s, 10, 0, &e);
    return BURROW_OK(e);
}

/* readGNUSparseMap1x0: the map as lines of decimal numbers at the start of the
 * file's data, in whole blocks. */
static Error tar_read_gnu_sparse_map_1x0(TarReader *tr, TarSparseList *out) {
    IoReader r = {&tar_file_reader_vt, &tr->curr};
    TarBuf buf = {tr->a, NULL, 0, 0, false};
    Int rd = 0;
    int64_t newlines = 0;
    Int total = 0;
    Byte blk[TAR_BLOCK_SIZE];
    Error e = BURROW_NO_ERROR;

#define TAR_FEED(want)                                                                 \
    do {                                                                               \
        while (newlines < (want)) {                                                    \
            total += TAR_BLOCK_SIZE;                                                   \
            if (total > TAR_MAX_SPECIAL_FILE_SIZE) {                                   \
                e = burrow__tar_err_sparse_too_long;                                   \
                break;                                                                 \
            }                                                                          \
            tar_must_read_full(r, blk, TAR_BLOCK_SIZE, &e);                            \
            if (BURROW_FAILED(e))                                                      \
                break;                                                                 \
            tar_buf_put(&buf, blk, TAR_BLOCK_SIZE);                                    \
            if (buf.oom) {                                                             \
                e = burrow_err_out_of_memory;                                          \
                break;                                                                 \
            }                                                                          \
            for (Int i_ = 0; i_ < TAR_BLOCK_SIZE; i_++)                                \
                if (blk[i_] == '\n')                                                   \
                    newlines++;                                                        \
        }                                                                              \
    } while (0)

    TAR_FEED(1);
    int64_t num = 0;
    if (BURROW_OK(e)) {
        Str tok = {tar_empty, 0};
        newlines--;
        {
            Int start = rd;
            while (rd < buf.len && buf.p[rd] != '\n')
                rd++;
            tok = str_from_bytes(buf.p + start, rd - start);
            if (rd < buf.len)
                rd++;
        }
        if (!tar_parse_native_int(tok, &num) || num < 0 || num > INT64_MAX / 2)
            e = tar_err_header;
    }
    if (BURROW_OK(e))
        TAR_FEED(2 * num);
    for (int64_t i = 0; BURROW_OK(e) && i < num; i++) {
        int64_t vals[2];
        bool good = true;
        for (int j = 0; j < 2; j++) {
            newlines--;
            Int start = rd;
            while (rd < buf.len && buf.p[rd] != '\n')
                rd++;
            Str tok = str_from_bytes(buf.p + start, rd - start);
            if (rd < buf.len)
                rd++;
            Error pe = BURROW_NO_ERROR;
            vals[j] = strconv_parse_int(tok, 10, 64, &pe);
            if (BURROW_FAILED(pe))
                good = false;
        }
        if (!good) {
            e = tar_err_header;
            break;
        }
        e = tar_append_sparse_entry(tr->a, out, vals[0], vals[1]);
    }
#undef TAR_FEED
    tar_buf_free(&buf);
    return e;
}

/* readGNUSparseMap0x1: the map in the PAX records. */
static Error tar_read_gnu_sparse_map_0x1(Alloc *a, Map *pax, TarSparseList *out) {
    int64_t num = 0;
    if (!tar_parse_native_int(tar_map_value(pax, TAR_PAX_GNU_SPARSE_NUM_BLOCKS),
                              &num) ||
        num < 0 || num > INT64_MAX / 2)
        return tar_err_header;
    Str map = tar_map_value(pax, TAR_PAX_GNU_SPARSE_MAP);
    int64_t parts = 0;
    if (map.len > 0) {
        parts = 1;
        for (Int i = 0; i < map.len; i++)
            if (map.p[i] == ',')
                parts++;
    }
    if (parts != 2 * num)
        return tar_err_header;
    Int i = 0;
    for (int64_t k = 0; k + 2 <= parts; k += 2) {
        int64_t vals[2];
        bool good = true;
        for (int j = 0; j < 2; j++) {
            Int start = i;
            while (i < map.len && map.p[i] != ',')
                i++;
            Str tok = str_from_bytes(map.p + start, i - start);
            if (i < map.len)
                i++;
            Error pe = BURROW_NO_ERROR;
            vals[j] = strconv_parse_int(tok, 10, 64, &pe);
            if (BURROW_FAILED(pe))
                good = false;
        }
        if (!good)
            return tar_err_header;
        Error e = tar_append_sparse_entry(a, out, vals[0], vals[1]);
        if (BURROW_FAILED(e))
            return e;
    }
    return BURROW_NO_ERROR;
}

bool burrow__tar_read_gnu_sparse_pax_headers(TarReader *tr, TarHeader *hdr,
                                             TarSparseList *out, Error *err) {
    *err = BURROW_NO_ERROR;
    bool is1x0;
    Map *pax = hdr->pax_records;
    Str major = tar_map_value(pax, TAR_PAX_GNU_SPARSE_MAJOR);
    Str minor = tar_map_value(pax, TAR_PAX_GNU_SPARSE_MINOR);
    bool v0x = str_eq(major, BURROW_S("0")) &&
               (str_eq(minor, BURROW_S("0")) || str_eq(minor, BURROW_S("1")));
    bool v1x0 = str_eq(major, BURROW_S("1")) && str_eq(minor, BURROW_S("0"));
    /* Version 0.0 archives can leave the version out and only have a map. */
    bool bare = major.len == 0 && minor.len == 0 &&
                tar_map_value(pax, TAR_PAX_GNU_SPARSE_MAP).len != 0;
    if (v1x0)
        is1x0 = true;
    else if (v0x || bare)
        is1x0 = false;
    else
        return false;

    hdr->format &= TAR_FORMAT_PAX;
    Str name = tar_map_value(pax, TAR_PAX_GNU_SPARSE_NAME);
    if (name.len != 0 && !tar_set(tr->a, &hdr->name, name)) {
        *err = burrow_err_out_of_memory;
        return true;
    }
    Str size = tar_map_value(pax, TAR_PAX_GNU_SPARSE_SIZE);
    if (size.len == 0)
        size = tar_map_value(pax, TAR_PAX_GNU_SPARSE_REAL_SIZE);
    if (size.len != 0) {
        Error pe = BURROW_NO_ERROR;
        int64_t n = strconv_parse_int(size, 10, 64, &pe);
        if (BURROW_FAILED(pe)) {
            *err = tar_err_header;
            return true;
        }
        hdr->size = n;
    }
    if (is1x0)
        *err = tar_read_gnu_sparse_map_1x0(tr, out);
    else
        *err = tar_read_gnu_sparse_map_0x1(tr->a, pax, out);
    return true;
}

static Error tar_handle_sparse_file(TarReader *tr, TarHeader *hdr, Byte *raw) {
    TarSparseList spd = {NULL, 0, 0};
    Error e = BURROW_NO_ERROR;
    bool have;
    if (hdr->typeflag == TAR_TYPE_GNU_SPARSE) {
        e = burrow__tar_read_old_gnu_sparse_map(tr, hdr, raw, &spd);
        have = true;
    } else {
        have = burrow__tar_read_gnu_sparse_pax_headers(tr, hdr, &spd, &e);
    }
    if (BURROW_OK(e) && have) {
        if (tar_is_header_only_type(hdr->typeflag) ||
            !burrow__tar_validate_sparse_entries(spd.p, spd.len, hdr->size)) {
            e = tar_err_header;
        } else if (!tar_sparse_list_reserve(tr->a, &spd, spd.len + 1)) {
            e = burrow_err_out_of_memory;
        } else {
            spd.len = burrow__tar_invert_sparse_entries(spd.p, spd.len, hdr->size);
            burrow__tar_sparse_list_free(tr->a, &tr->holes);
            tr->holes = spd;
            spd = (TarSparseList){NULL, 0, 0};
            tr->curr.sp = tr->holes.p;
            tr->curr.sp_len = tr->holes.len;
            tr->curr.pos = 0;
        }
    }
    burrow__tar_sparse_list_free(tr->a, &spd);
    return e;
}

/* prefix + "/" + name, into *dst from a. */
static bool tar_join_prefix(Alloc *a, Str *dst, Str prefix, Str name) {
    TarBuf b = {a, NULL, 0, 0, false};
    tar_buf_str(&b, prefix);
    tar_buf_byte(&b, '/');
    tar_buf_str(&b, name);
    bool ok = !b.oom && tar_set(a, dst, tar_buf_view(&b));
    tar_buf_free(&b);
    return ok;
}

/* readHeader: the next header block, parsed into a header from tr->a. */
static TarHeader *tar_read_header(TarReader *tr, Error *err) {
    Byte *blk = tr->blk;
    Error e = BURROW_NO_ERROR;
    io_read_full(tr->r, tar_bytes(blk, TAR_BLOCK_SIZE), &e);
    if (BURROW_FAILED(e)) {
        *err = e;
        return NULL;
    }
    if (memcmp(blk, tar_zero_block, TAR_BLOCK_SIZE) == 0) {
        io_read_full(tr->r, tar_bytes(blk, TAR_BLOCK_SIZE), &e);
        if (BURROW_FAILED(e)) {
            *err = e;
            return NULL;
        }
        *err =
            memcmp(blk, tar_zero_block, TAR_BLOCK_SIZE) == 0 ? io_eof : tar_err_header;
        return NULL;
    }
    TarFormat format = burrow__tar_block_get_format(blk);
    if (format == TAR_FORMAT_UNKNOWN) {
        *err = tar_err_header;
        return NULL;
    }

    Alloc *a = tr->a;
    TarHeader *hdr = tar_header_new(a);
    if (hdr == NULL) {
        *err = burrow_err_out_of_memory;
        return NULL;
    }
    bool ok = true;
    Error perr = BURROW_NO_ERROR;
    hdr->typeflag = blk[TAR_V7_TYPE_FLAG];
    hdr->name = tar_dup(a, burrow__tar_parse_string(blk + TAR_V7_NAME, 100), &ok);
    hdr->linkname =
        tar_dup(a, burrow__tar_parse_string(blk + TAR_V7_LINK_NAME, 100), &ok);
    hdr->size = burrow__tar_parse_numeric(blk + TAR_V7_SIZE, 12, &perr);
    hdr->mode = burrow__tar_parse_numeric(blk + TAR_V7_MODE, 8, &perr);
    hdr->uid = (Int)burrow__tar_parse_numeric(blk + TAR_V7_UID, 8, &perr);
    hdr->gid = (Int)burrow__tar_parse_numeric(blk + TAR_V7_GID, 8, &perr);
    hdr->mod_time =
        time_from_unix(burrow__tar_parse_numeric(blk + TAR_V7_MOD_TIME, 12, &perr), 0);

    if (format > TAR_FORMAT_V7) {
        hdr->uname =
            tar_dup(a, burrow__tar_parse_string(blk + TAR_USTAR_USER_NAME, 32), &ok);
        hdr->gname =
            tar_dup(a, burrow__tar_parse_string(blk + TAR_USTAR_GROUP_NAME, 32), &ok);
        hdr->devmajor = burrow__tar_parse_numeric(blk + TAR_USTAR_DEV_MAJOR, 8, &perr);
        hdr->devminor = burrow__tar_parse_numeric(blk + TAR_USTAR_DEV_MINOR, 8, &perr);
        Str prefix = {tar_empty, 0};
        if ((format & (TAR_FORMAT_USTAR | TAR_FORMAT_PAX)) != 0) {
            hdr->format = format;
            prefix = burrow__tar_parse_string(blk + TAR_USTAR_PREFIX, 155);
            for (Int i = 0; i < TAR_BLOCK_SIZE; i++) {
                if (blk[i] >= 0x80) {
                    hdr->format = TAR_FORMAT_UNKNOWN;
                    break;
                }
            }
            static const Int ends[] = {TAR_V7_SIZE + 11,       TAR_V7_MODE + 7,
                                       TAR_V7_UID + 7,         TAR_V7_GID + 7,
                                       TAR_V7_MOD_TIME + 11,   TAR_USTAR_DEV_MAJOR + 7,
                                       TAR_USTAR_DEV_MINOR + 7};
            for (size_t i = 0; i < sizeof ends / sizeof ends[0]; i++)
                if (blk[ends[i]] != 0)
                    hdr->format = TAR_FORMAT_UNKNOWN;
        } else if ((format & TAR_FORMAT_STAR) != 0) {
            prefix = burrow__tar_parse_string(blk + TAR_STAR_PREFIX, 131);
            hdr->access_time = time_from_unix(
                burrow__tar_parse_numeric(blk + TAR_STAR_ACCESS_TIME, 12, &perr), 0);
            hdr->change_time = time_from_unix(
                burrow__tar_parse_numeric(blk + TAR_STAR_CHANGE_TIME, 12, &perr), 0);
        } else if ((format & TAR_FORMAT_GNU) != 0) {
            hdr->format = format;
            Error p2 = BURROW_NO_ERROR;
            if (blk[TAR_GNU_ACCESS_TIME] != 0)
                hdr->access_time = time_from_unix(
                    burrow__tar_parse_numeric(blk + TAR_GNU_ACCESS_TIME, 12, &p2), 0);
            if (blk[TAR_GNU_CHANGE_TIME] != 0)
                hdr->change_time = time_from_unix(
                    burrow__tar_parse_numeric(blk + TAR_GNU_CHANGE_TIME, 12, &p2), 0);
            if (BURROW_FAILED(p2)) {
                hdr->access_time = (Time){0};
                hdr->change_time = (Time){0};
                Str s = burrow__tar_parse_string(blk + TAR_USTAR_PREFIX, 155);
                if (tar_is_ascii(s))
                    prefix = s;
                hdr->format = TAR_FORMAT_UNKNOWN;
            }
        }
        if (prefix.len > 0 && ok) {
            Str name = hdr->name;
            hdr->name = (Str){tar_empty, 0};
            ok = tar_join_prefix(a, &hdr->name, prefix, name);
            tar_str_free(a, name);
        }
    }
    if (!ok)
        perr = burrow_err_out_of_memory;
    if (BURROW_FAILED(perr)) {
        tar_header_free(a, hdr);
        *err = perr;
        return NULL;
    }
    *err = BURROW_NO_ERROR;
    return hdr;
}

/* discard: skips n bytes of r, seeking over all but the last when r can. */
static Error tar_discard(Alloc *a, IoReader r, int64_t n) {
    int64_t seek_skipped = 0;
    const Method *seek = burrow__io_seek_method(r.vt->self_type);
    if (seek != NULL && n > 1) {
        Error e1 = BURROW_NO_ERROR;
        int64_t pos1 = burrow__io_seek(seek, r.data, 0, BURROW_IO_SEEK_CURRENT, &e1);
        if (pos1 >= 0 && BURROW_OK(e1)) {
            Error e2 = BURROW_NO_ERROR;
            int64_t pos2 =
                burrow__io_seek(seek, r.data, n - 1, BURROW_IO_SEEK_CURRENT, &e2);
            if (pos2 < 0 || BURROW_FAILED(e2))
                return e2;
            seek_skipped = pos2 - pos1;
        }
    }
    Error e = BURROW_NO_ERROR;
    int64_t copy_skipped = io_copy_n(a, io_discard, r, n - seek_skipped, &e);
    if (tar_same(e, io_eof) && seek_skipped + copy_skipped < n)
        e = io_err_unexpected_eof;
    return e;
}

/* Reader.next. */
static TarHeader *tar_next(TarReader *tr, Error *err) {
    Alloc *a = tr->a;
    Map *pax = NULL;
    Str long_name = {tar_empty, 0};
    Str long_link = {tar_empty, 0};
    TarFormat format = TAR_FORMAT_USTAR | TAR_FORMAT_PAX | TAR_FORMAT_GNU;
    TarHeader *hdr = NULL;
    Error e = BURROW_NO_ERROR;
    for (;;) {
        e = tar_discard(a, tr->r,
                        burrow__tar_file_reader_physical_remaining(&tr->curr));
        if (BURROW_FAILED(e))
            break;
        tar_try_read_full(tr->r, tr->blk, (Int)tr->pad, &e);
        if (BURROW_FAILED(e))
            break;
        tr->pad = 0;

        hdr = tar_read_header(tr, &e);
        if (BURROW_FAILED(e))
            break;
        e = tar_handle_regular_file(tr, hdr);
        if (BURROW_FAILED(e))
            break;
        format &= hdr->format;

        if (hdr->typeflag == TAR_TYPE_X_HEADER ||
            hdr->typeflag == TAR_TYPE_X_GLOBAL_HEADER) {
            format &= TAR_FORMAT_PAX;
            Map *next = burrow__tar_parse_pax(a, tar_reader_as_io_reader(tr), &e);
            if (BURROW_FAILED(e))
                break;
            burrow__tar_str_map_free(a, pax);
            pax = next;
            if (hdr->typeflag == TAR_TYPE_X_GLOBAL_HEADER) {
                if (BURROW_FAILED(burrow__tar_merge_pax(a, hdr, pax)))
                    burrow__tar_str_map_free(a, pax);
                pax = NULL;
                /* Only the name, the flag and the records. */
                tar_str_free(a, hdr->linkname);
                tar_str_free(a, hdr->uname);
                tar_str_free(a, hdr->gname);
                TarHeader g = {0};
                g.typeflag = hdr->typeflag;
                g.name = hdr->name;
                g.linkname = g.uname = g.gname = (Str){tar_empty, 0};
                g.xattrs = hdr->xattrs;
                g.pax_records = hdr->pax_records;
                g.format = format;
                *hdr = g;
                tar_str_free(a, long_name);
                tar_str_free(a, long_link);
                *err = BURROW_NO_ERROR;
                return hdr;
            }
            tar_header_free(a, hdr);
            hdr = NULL;
            continue;
        }
        if (hdr->typeflag == TAR_TYPE_GNU_LONG_NAME ||
            hdr->typeflag == TAR_TYPE_GNU_LONG_LINK) {
            format &= TAR_FORMAT_GNU;
            Slice real = tar_read_special_file(a, tar_reader_as_io_reader(tr), &e);
            if (BURROW_FAILED(e)) {
                tar_slice_free(a, real);
                break;
            }
            Str s = burrow__tar_parse_string((const Byte *)real.p, real.len);
            bool ok = tar_set(
                a, hdr->typeflag == TAR_TYPE_GNU_LONG_NAME ? &long_name : &long_link,
                s);
            tar_slice_free(a, real);
            if (!ok) {
                e = burrow_err_out_of_memory;
                break;
            }
            tar_header_free(a, hdr);
            hdr = NULL;
            continue;
        }

        e = burrow__tar_merge_pax(a, hdr, pax);
        if (BURROW_FAILED(e))
            break;
        pax = NULL;
        if (long_name.len > 0) {
            tar_str_free(a, hdr->name);
            hdr->name = long_name;
            long_name = (Str){tar_empty, 0};
        }
        if (long_link.len > 0) {
            tar_str_free(a, hdr->linkname);
            hdr->linkname = long_link;
            long_link = (Str){tar_empty, 0};
        }
        if (hdr->typeflag == TAR_TYPE_REG_A) {
            if (hdr->name.len > 0 && hdr->name.p[hdr->name.len - 1] == '/')
                hdr->typeflag = TAR_TYPE_DIR;
            else
                hdr->typeflag = TAR_TYPE_REG;
        }
        e = tar_handle_regular_file(tr, hdr);
        if (BURROW_FAILED(e))
            break;
        e = tar_handle_sparse_file(tr, hdr, tr->blk);
        if (BURROW_FAILED(e))
            break;
        if ((format & TAR_FORMAT_USTAR) != 0 && (format & TAR_FORMAT_PAX) != 0)
            format &= TAR_FORMAT_USTAR;
        hdr->format = format;
        *err = BURROW_NO_ERROR;
        return hdr;
    }
    tar_header_free(a, hdr);
    burrow__tar_str_map_free(a, pax);
    tar_str_free(a, long_name);
    tar_str_free(a, long_link);
    *err = e;
    return NULL;
}

TarHeader *tar_reader_next(TarReader *tr, Error *err) {
    if (BURROW_FAILED(tr->err)) {
        BURROW_OUT(err, tr->err);
        return NULL;
    }
    Error e = BURROW_NO_ERROR;
    TarHeader *hdr = tar_next(tr, &e);
    tr->err = e;
    if (BURROW_OK(e) && hdr != NULL && !burrow__tar_is_local(hdr->name) &&
        (tar_debug_load() & TAR_DEBUG_INSECURE_OFF) != 0)
        e = tar_err_insecure_path;
    BURROW_OUT(err, e);
    return hdr;
}

/* ------------------------------------------------------------------ writing */

static const Type tar_writer_desc = {
    {(const Byte *)"Writer", 6},
    {(const Byte *)"archive/tar", 11},
    KIND_STRUCT,
    (uint32_t)sizeof(TarWriter),
    (uint16_t)_Alignof(TarWriter),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x74777472U, /* "twtr" */
    NULL,
};

const Type *const TYPE_TAR_WRITER = &tar_writer_desc;

static Int tar_vt_writer_write(void *self, Slice b, Error *err) {
    return tar_writer_write((TarWriter *)self, b, err);
}

static const IoWriterVT tar_writer_vt = {&tar_writer_desc, tar_vt_writer_write};

IoWriter tar_writer_as_io_writer(TarWriter *tw) {
    return (IoWriter){&tar_writer_vt, tw};
}

TarWriter *tar_new_writer(Alloc *a, IoWriter w) {
    TarWriter *tw = (TarWriter *)mem_alloc(a, sizeof *tw, _Alignof(TarWriter));
    if (tw == NULL)
        return NULL;
    tw->a = a;
    tw->w = w;
    tw->curr.w = w;
    return tw;
}

void tar_writer_free(TarWriter *tw) {
    if (tw == NULL)
        return;
    mem_free(tw->a, tw, sizeof *tw, _Alignof(TarWriter));
}

Error tar_writer_flush(TarWriter *tw) {
    if (BURROW_FAILED(tw->err))
        return tw->err;
    int64_t nb = burrow__tar_file_writer_logical_remaining(&tw->curr);
    if (nb > 0)
        return fmt_errorf_v("archive/tar: missed writing %d bytes", nb);
    BURROW_CALL(tw->w, write, tar_bytes(tar_zero_block, (Int)tw->pad), &tw->err);
    if (BURROW_FAILED(tw->err))
        return tw->err;
    tw->pad = 0;
    return BURROW_NO_ERROR;
}

static Error tar_write_raw_header(TarWriter *tw, int64_t size, Byte flag) {
    Error e = tar_writer_flush(tw);
    if (BURROW_FAILED(e))
        return e;
    BURROW_CALL(tw->w, write, tar_bytes(tw->blk, TAR_BLOCK_SIZE), &e);
    if (BURROW_FAILED(e))
        return e;
    if (tar_is_header_only_type(flag))
        size = 0;
    tw->curr = (TarFileWriter){tw->w, size, NULL, 0, 0};
    tw->pad = tar_block_padding(size);
    return BURROW_NO_ERROR;
}

enum {
    TAR_TEMPLATE_USTAR,
    TAR_TEMPLATE_PAX,
    TAR_TEMPLATE_GNU,
};

/* templateV7Plus, with the formatters picked by which. */
static void tar_template_v7_plus(TarWriter *tw, const TarHeader *hdr, int which,
                                 Error *ferr) {
    Byte *blk = tw->blk;
    memset(blk, 0, TAR_BLOCK_SIZE);
    Time mod_time = hdr->mod_time;
    if (time_is_zero(mod_time))
        mod_time = time_from_unix(0, 0);

#define TAR_STR(off, n, s)                                                             \
    (which == TAR_TEMPLATE_PAX                                                         \
         ? tar_format_ascii(blk + (off), (n), (s), ferr)                               \
         : burrow__tar_format_string(blk + (off), (n), (s), ferr))
#define TAR_NUM(off, n, x)                                                             \
    (which == TAR_TEMPLATE_GNU                                                         \
         ? burrow__tar_format_numeric(blk + (off), (n), (x), ferr)                     \
         : burrow__tar_format_octal(blk + (off), (n), (x), ferr))

    blk[TAR_V7_TYPE_FLAG] = hdr->typeflag;
    TAR_STR(TAR_V7_NAME, 100, hdr->name);
    TAR_STR(TAR_V7_LINK_NAME, 100, hdr->linkname);
    TAR_NUM(TAR_V7_MODE, 8, hdr->mode);
    TAR_NUM(TAR_V7_UID, 8, (int64_t)hdr->uid);
    TAR_NUM(TAR_V7_GID, 8, (int64_t)hdr->gid);
    TAR_NUM(TAR_V7_SIZE, 12, hdr->size);
    TAR_NUM(TAR_V7_MOD_TIME, 12, time_unix(mod_time));
    TAR_STR(TAR_USTAR_USER_NAME, 32, hdr->uname);
    TAR_STR(TAR_USTAR_GROUP_NAME, 32, hdr->gname);
    TAR_NUM(TAR_USTAR_DEV_MAJOR, 8, hdr->devmajor);
    TAR_NUM(TAR_USTAR_DEV_MINOR, 8, hdr->devminor);
#undef TAR_STR
#undef TAR_NUM
}

static Error tar_write_raw_file(TarWriter *tw, Str name, Str data, Byte flag,
                                TarFormat format) {
    Byte *blk = tw->blk;
    memset(blk, 0, TAR_BLOCK_SIZE);
    /* toASCII, cut to the field, and no slash at the end. */
    Byte tmp[TAR_NAME_SIZE];
    Int n = 0;
    for (Int i = 0; i < name.len && n < TAR_NAME_SIZE; i++)
        if (name.p[i] < 0x80 && name.p[i] != 0)
            tmp[n++] = name.p[i];
    while (n > 0 && tmp[n - 1] == '/')
        n--;
    Error ferr = BURROW_NO_ERROR;
    blk[TAR_V7_TYPE_FLAG] = flag;
    tar_format_string_n(blk + TAR_V7_NAME, 100, tmp, n, &ferr);
    burrow__tar_format_octal(blk + TAR_V7_MODE, 8, 0, &ferr);
    burrow__tar_format_octal(blk + TAR_V7_UID, 8, 0, &ferr);
    burrow__tar_format_octal(blk + TAR_V7_GID, 8, 0, &ferr);
    burrow__tar_format_octal(blk + TAR_V7_SIZE, 12, (int64_t)data.len, &ferr);
    burrow__tar_format_octal(blk + TAR_V7_MOD_TIME, 12, 0, &ferr);
    burrow__tar_block_set_format(blk, format);
    if (BURROW_FAILED(ferr))
        return ferr;
    Error e = tar_write_raw_header(tw, (int64_t)data.len, flag);
    if (BURROW_FAILED(e))
        return e;
    tar_writer_write(tw, tar_bytes(data.p, data.len), &e);
    return e;
}

static Error tar_write_ustar_header(TarWriter *tw, TarHeader *hdr) {
    Str prefix = {tar_empty, 0};
    Str suffix;
    Str p;
    if (burrow__tar_split_ustar_path(hdr->name, &p, &suffix)) {
        prefix = p;
        hdr->name = suffix;
    }
    Error ferr = BURROW_NO_ERROR;
    tar_template_v7_plus(tw, hdr, TAR_TEMPLATE_USTAR, &ferr);
    burrow__tar_format_string(tw->blk + TAR_USTAR_PREFIX, 155, prefix, &ferr);
    burrow__tar_block_set_format(tw->blk, TAR_FORMAT_USTAR);
    if (BURROW_FAILED(ferr))
        return ferr;
    return tar_write_raw_header(tw, hdr->size, hdr->typeflag);
}

/* Clean(dir + "PaxHeaders.0/" + file), where dir is empty or ends in a
 * slash, into out, from a. That is path.Join(dir, "PaxHeaders.0", file). */
static bool tar_pax_header_name(Alloc *a, Str real, TarBuf *out) {
    Str file;
    Str dir = path_split(real, &file);
    TarBuf j = {a, NULL, 0, 0, false};
    tar_buf_str(&j, dir);
    tar_buf_cstr(&j, "PaxHeaders.0");
    if (file.len > 0) {
        tar_buf_byte(&j, '/');
        tar_buf_str(&j, file);
    }
    if (j.oom) {
        tar_buf_free(&j);
        return false;
    }
    /* Clean never makes a path longer, so the joined bytes are room enough,
     * and it is built a byte at a time in place of the path it reads. */
    Str path = tar_buf_view(&j);
    Int n = path.len;
    const Byte *s = path.p;
    Byte *w = j.p;
    bool rooted = n > 0 && s[0] == '/';
    Int r = 0;
    Int wl = 0;
    Int dotdot = 0;
    if (rooted) {
        w[wl++] = '/';
        r = 1;
        dotdot = 1;
    }
    while (r < n) {
        if (s[r] == '/' || (s[r] == '.' && (r + 1 == n || s[r + 1] == '/'))) {
            r++;
        } else if (s[r] == '.' && r + 1 < n && s[r + 1] == '.' &&
                   (r + 2 == n || s[r + 2] == '/')) {
            r += 2;
            if (wl > dotdot) {
                wl--;
                while (wl > dotdot && w[wl] != '/')
                    wl--;
            } else if (!rooted) {
                if (wl > 0)
                    w[wl++] = '/';
                w[wl++] = '.';
                w[wl++] = '.';
                dotdot = wl;
            }
        } else {
            if ((rooted && wl != 1) || (!rooted && wl != 0))
                w[wl++] = '/';
            for (; r < n && s[r] != '/'; r++)
                w[wl++] = s[r];
        }
    }
    if (wl == 0)
        w[wl++] = '.';
    j.len = wl;
    *out = j;
    return true;
}

static Error tar_write_pax_header(TarWriter *tw, TarHeader *hdr, Map *pax) {
    Alloc *a = tw->a;
    Str real_name = hdr->name;
    bool is_global = hdr->typeflag == TAR_TYPE_X_GLOBAL_HEADER;
    Int count = map_len(pax);
    if (count > 0 || is_global) {
        Str *keys = NULL;
        if (count > 0) {
            keys =
                (Str *)mem_alloc_nozero(a, (size_t)count * sizeof(Str), _Alignof(Str));
            if (keys == NULL)
                return burrow_err_out_of_memory;
            MapIter it = map_iter(pax);
            const void *kp;
            void *vp;
            Int i = 0;
            while (map_next(&it, &kp, &vp))
                keys[i++] = *(const Str *)kp;
            sort_strings(slice_from(keys, count, count, TYPE_STRING));
        }
        TarBuf data = {a, NULL, 0, 0, false};
        Error e = BURROW_NO_ERROR;
        for (Int i = 0; i < count; i++) {
            if (!tar_pax_record_into(&data, keys[i], tar_map_value(pax, keys[i]))) {
                e = tar_err_header;
                break;
            }
        }
        if (keys != NULL)
            mem_free(a, keys, (size_t)count * sizeof(Str), _Alignof(Str));
        TarBuf name = {a, NULL, 0, 0, false};
        Byte flag;
        if (BURROW_OK(e) && data.oom)
            e = burrow_err_out_of_memory;
        if (BURROW_OK(e) && data.len > TAR_MAX_SPECIAL_FILE_SIZE)
            e = tar_err_field_too_long;
        if (BURROW_OK(e)) {
            if (is_global) {
                tar_buf_str(&name,
                            real_name.len > 0 ? real_name : BURROW_S("GlobalHead.0.0"));
                flag = TAR_TYPE_X_GLOBAL_HEADER;
            } else {
                if (!tar_pax_header_name(a, real_name, &name))
                    name.oom = true;
                flag = TAR_TYPE_X_HEADER;
            }
            if (name.oom)
                e = burrow_err_out_of_memory;
            else
                e = tar_write_raw_file(tw, tar_buf_view(&name), tar_buf_view(&data),
                                       flag, TAR_FORMAT_PAX);
        }
        tar_buf_free(&name);
        tar_buf_free(&data);
        if (BURROW_FAILED(e) || is_global)
            return e;
    }
    Error ferr = BURROW_NO_ERROR;
    tar_template_v7_plus(tw, hdr, TAR_TEMPLATE_PAX, &ferr);
    burrow__tar_block_set_format(tw->blk, TAR_FORMAT_PAX);
    return tar_write_raw_header(tw, hdr->size, hdr->typeflag);
}

static Error tar_write_gnu_header(TarWriter *tw, TarHeader *hdr) {
    static const Str long_name = {(const Byte *)"././@LongLink", 13};
    if (hdr->name.len > TAR_NAME_SIZE || hdr->linkname.len > TAR_NAME_SIZE) {
        TarBuf data = {tw->a, NULL, 0, 0, false};
        Error e = BURROW_NO_ERROR;
        if (hdr->name.len > TAR_NAME_SIZE) {
            tar_buf_str(&data, hdr->name);
            tar_buf_byte(&data, 0);
            e = data.oom ? burrow_err_out_of_memory
                         : tar_write_raw_file(tw, long_name, tar_buf_view(&data),
                                              TAR_TYPE_GNU_LONG_NAME, TAR_FORMAT_GNU);
        }
        if (BURROW_OK(e) && hdr->linkname.len > TAR_NAME_SIZE) {
            data.len = 0;
            tar_buf_str(&data, hdr->linkname);
            tar_buf_byte(&data, 0);
            e = data.oom ? burrow_err_out_of_memory
                         : tar_write_raw_file(tw, long_name, tar_buf_view(&data),
                                              TAR_TYPE_GNU_LONG_LINK, TAR_FORMAT_GNU);
        }
        tar_buf_free(&data);
        if (BURROW_FAILED(e))
            return e;
    }
    Error ferr = BURROW_NO_ERROR;
    tar_template_v7_plus(tw, hdr, TAR_TEMPLATE_GNU, &ferr);
    if (!time_is_zero(hdr->access_time))
        burrow__tar_format_numeric(tw->blk + TAR_GNU_ACCESS_TIME, 12,
                                   time_unix(hdr->access_time), &ferr);
    if (!time_is_zero(hdr->change_time))
        burrow__tar_format_numeric(tw->blk + TAR_GNU_CHANGE_TIME, 12,
                                   time_unix(hdr->change_time), &ferr);
    burrow__tar_block_set_format(tw->blk, TAR_FORMAT_GNU);
    return tar_write_raw_header(tw, hdr->size, hdr->typeflag);
}

Error tar_writer_write_header(TarWriter *tw, const TarHeader *h) {
    Error e = tar_writer_flush(tw);
    if (BURROW_FAILED(e))
        return e;
    TarHeader hdr = *h;
    if (hdr.typeflag == TAR_TYPE_REG_A) {
        if (hdr.name.len > 0 && hdr.name.p[hdr.name.len - 1] == '/')
            hdr.typeflag = TAR_TYPE_DIR;
        else
            hdr.typeflag = TAR_TYPE_REG;
    }
    if (hdr.format == TAR_FORMAT_UNKNOWN) {
        hdr.mod_time = time_round(hdr.mod_time, TIME_SECOND);
        hdr.access_time = (Time){0};
        hdr.change_time = (Time){0};
    }
    Map *pax = NULL;
    TarFormat allowed = burrow__tar_allowed_formats(tw->a, &hdr, &pax, &e);
    if ((allowed & TAR_FORMAT_USTAR) != 0) {
        tw->err = tar_write_ustar_header(tw, &hdr);
        e = tw->err;
    } else if ((allowed & TAR_FORMAT_PAX) != 0) {
        tw->err = tar_write_pax_header(tw, &hdr, pax);
        e = tw->err;
    } else if ((allowed & TAR_FORMAT_GNU) != 0) {
        tw->err = tar_write_gnu_header(tw, &hdr);
        e = tw->err;
    }
    burrow__tar_str_map_free(tw->a, pax);
    return e;
}

Int tar_writer_write(TarWriter *tw, Slice b, Error *err) {
    if (BURROW_FAILED(tw->err)) {
        BURROW_OUT(err, tw->err);
        return 0;
    }
    Error e = BURROW_NO_ERROR;
    Int n = burrow__tar_file_writer_write(&tw->curr, b, &e);
    if (BURROW_FAILED(e) && !tar_same(e, tar_err_write_too_long))
        tw->err = e;
    BURROW_OUT(err, e);
    return n;
}

int64_t burrow__tar_writer_read_from(TarWriter *tw, IoReader r, Error *err) {
    if (BURROW_FAILED(tw->err)) {
        *err = tw->err;
        return 0;
    }
    int64_t n = burrow__tar_file_writer_read_from(&tw->curr, tw->a, r, err);
    if (BURROW_FAILED(*err) && !tar_same(*err, tar_err_write_too_long))
        tw->err = *err;
    return n;
}

Error tar_writer_close(TarWriter *tw) {
    if (tar_same(tw->err, tar_err_write_after_close))
        return BURROW_NO_ERROR;
    if (BURROW_FAILED(tw->err))
        return tw->err;
    Error e = tar_writer_flush(tw);
    for (int i = 0; i < 2 && BURROW_OK(e); i++)
        BURROW_CALL(tw->w, write, tar_bytes(tar_zero_block, TAR_BLOCK_SIZE), &e);
    tw->err = tar_err_write_after_close;
    return e;
}

/* ------------------------------------------------------------------- AddFS */

typedef struct TarAddFs {
    TarWriter *tw;
    Fs fsys;
    Arena scratch;
} TarAddFs;

static Error tar_add_fs_entry(TarAddFs *env, Alloc *a, Str name, FsDirEntry d) {
    TarWriter *tw = env->tw;
    Error e = BURROW_NO_ERROR;
    FsFileInfo info = d.vt->info(d.data, a, &e);
    if (BURROW_FAILED(e))
        return e;
    Str link = {tar_empty, 0};
    FsFileMode typ = d.vt->type(d.data);
    if (typ == FS_MODE_SYMLINK) {
        link = fs_read_link(a, env->fsys, name, &e);
        if (BURROW_FAILED(e))
            return e;
    } else if (!fs_file_mode_is_regular(typ) && typ != FS_MODE_DIR) {
        return errors_new(error_allocator(),
                          BURROW_S("tar: cannot add non-regular file"));
    }
    TarHeader *h = tar_file_info_header(a, info, link, &e);
    if (BURROW_FAILED(e))
        return e;
    if (d.vt->is_dir(d.data)) {
        TarBuf b = {a, NULL, 0, 0, false};
        tar_buf_str(&b, name);
        tar_buf_byte(&b, '/');
        if (b.oom)
            return burrow_err_out_of_memory;
        h->name = tar_buf_view(&b);
    } else {
        h->name = name;
    }
    e = tar_writer_write_header(tw, h);
    if (BURROW_FAILED(e))
        return e;
    if (!fs_file_mode_is_regular(d.vt->type(d.data)))
        return BURROW_NO_ERROR;
    FsFile f = env->fsys.vt->open(env->fsys.data, a, name, &e);
    if (BURROW_FAILED(e))
        return e;
    io_copy(a, tar_writer_as_io_writer(tw), fs_file_as_io_reader(f), &e);
    Error ce = f.vt->read_closer.closer.close(f.data);
    (void)ce;
    return e;
}

/* Each entry's scratch comes from an arena that is emptied after it, which
 * also takes care of the headers and names the FS hands out, whether it
 * allocated them or not. */
static Error tar_add_fs_visit(void *envp, Str name, FsDirEntry d, Error err) {
    TarAddFs *env = (TarAddFs *)envp;
    if (BURROW_FAILED(err))
        return err;
    if (str_eq(name, BURROW_S(".")))
        return BURROW_NO_ERROR;
    Error e = tar_add_fs_entry(env, arena_allocator(&env->scratch), name, d);
    arena_reset(&env->scratch);
    return e;
}

Error tar_writer_add_fs(TarWriter *tw, Fs fsys) {
    TarAddFs env;
    env.tw = tw;
    env.fsys = fsys;
    arena_init(&env.scratch, tw->a, 0);
    Error e = fs_walk_dir(tw->a, fsys, BURROW_S("."),
                          BURROW_FN(FsWalkDirFunc, tar_add_fs_visit, &env));
    arena_free(&env.scratch);
    return e;
}

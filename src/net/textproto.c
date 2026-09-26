/* net/textproto. See include/burrow/net/textproto.h.
 *
 * Derived from Go's src/net/textproto, go1.27.1.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/net/textproto.h"

#include "burrow/bufio.h"
#include "burrow/fmt.h"
#include "burrow/io.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/mem/heap.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/strconv.h"
#include "burrow/strings.h"
#include "burrow/sync.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static Byte *tp_put(Byte *p, const void *src, Int n) {
    if (n > 0)
        memcpy(p, src, (size_t)n);
    return p + n;
}

static bool tp_same_error(Error a, Error b) {
    return a.vt == b.vt && a.data == b.data;
}

static bool tp_str_eq(Str a, Str b) {
    return a.len == b.len && (a.len == 0 || memcmp(a.p, b.p, (size_t)a.len) == 0);
}

/* A copy of n bytes at p in a, or "" for none. */
static Str tp_copy(Alloc *a, const Byte *p, Int n, Error *err) {
    if (n == 0)
        return BURROW_STR_EMPTY;
    Byte *q = (Byte *)mem_alloc_nozero(a, (size_t)n, 1);
    if (q == NULL) {
        *err = burrow_err_out_of_memory;
        return BURROW_STR_EMPTY;
    }
    memcpy(q, p, (size_t)n);
    return str_from_bytes(q, n);
}

/* ------------------------------------------------------------------- errors */

BURROW_SENTINEL_ERROR(burrow__textproto_err_message_too_large, "message too large");

/* The TextprotoError first, so errors_as hands back a pointer to it, and the
 * message after. */
typedef struct TpErrorBox {
    TextprotoError e;
    Str message;
} TpErrorBox;

static Str tp_error_message(const void *self) {
    return ((const TpErrorBox *)self)->message;
}

static const Type tp_error_desc = {
    {(const Byte *)"Error", 5},
    {(const Byte *)"net/textproto", 13},
    KIND_STRUCT,
    (uint32_t)sizeof(TextprotoError),
    (uint16_t)_Alignof(TextprotoError),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x74707265U, /* "tpre" */
    NULL,
};

const Type *const TYPE_TEXTPROTO_ERROR = &tp_error_desc;

static Error tp_error_clone(const void *self, Alloc *a);

static const ErrorVT tp_error_vt = {
    &tp_error_desc, tp_error_message, NULL, NULL, NULL, NULL, tp_error_clone,
};

/* fmt.Sprintf("%03d %q", code, msg). */
static Int tp_error_text_len(const TextprotoError *e, char *code, size_t size) {
    int n = snprintf(code, size, "%03lld", (long long)e->code);
    return (Int)n + 1 + burrow__strconv_quote_into(NULL, e->msg);
}

static void tp_error_text_write(Byte *p, const TextprotoError *e, const char *code) {
    p = tp_put(p, code, (Int)strlen(code));
    *p++ = ' ';
    burrow__strconv_quote_into(p, e->msg);
}

Error textproto_error_as_error(const TextprotoError *e, Alloc *a) {
    char code[32];
    Int mlen = tp_error_text_len(e, code, sizeof code);
    size_t size = sizeof(TpErrorBox) + (size_t)e->msg.len + (size_t)mlen;
    TpErrorBox *b = (TpErrorBox *)mem_alloc_nozero(a, size, _Alignof(TpErrorBox));
    if (b == NULL)
        return burrow_err_out_of_memory;
    Byte *p = (Byte *)(b + 1);
    b->e.code = e->code;
    b->e.msg = str_from_bytes(p, e->msg.len);
    p = tp_put(p, e->msg.p, e->msg.len);
    tp_error_text_write(p, e, code);
    b->message = str_from_bytes(p, mlen);
    return (Error){&tp_error_vt, b};
}

static Error tp_error_clone(const void *self, Alloc *a) {
    return textproto_error_as_error(&((const TpErrorBox *)self)->e, a);
}

Str textproto_error_error(const TextprotoError *e, Alloc *a) {
    char code[32];
    Int mlen = tp_error_text_len(e, code, sizeof code);
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)mlen, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    tp_error_text_write(p, e, code);
    return str_from_bytes(p, mlen);
}

/* A ProtocolError is a string, and the box is the string, so errors_as gives
 * a Str * and the message is the same Str. */
static Str tp_protocol_error_message(const void *self) {
    return *(const Str *)self;
}

static const Type tp_protocol_error_desc = {
    {(const Byte *)"ProtocolError", 13},
    {(const Byte *)"net/textproto", 13},
    KIND_STRING,
    (uint32_t)sizeof(Str),
    (uint16_t)_Alignof(Str),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x74707070U, /* "tppp" */
    NULL,
};

const Type *const TYPE_TEXTPROTO_PROTOCOL_ERROR = &tp_protocol_error_desc;

static bool tp_protocol_error_is(const void *self, Error target);
static Error tp_protocol_error_clone(const void *self, Alloc *a);

static const ErrorVT tp_protocol_error_vt = {
    &tp_protocol_error_desc,
    tp_protocol_error_message,
    NULL,
    NULL,
    tp_protocol_error_is,
    NULL,
    tp_protocol_error_clone,
};

/* Go compares the two values with ==, which for a string is the text. */
static bool tp_protocol_error_is(const void *self, Error target) {
    if (target.vt != &tp_protocol_error_vt || target.data == NULL)
        return false;
    return tp_str_eq(*(const Str *)self, *(const Str *)target.data);
}

Str textproto_protocol_error_error(TextprotoProtocolError p) {
    return p;
}

Error textproto_protocol_error_as_error(TextprotoProtocolError s, Alloc *a) {
    Str *b = (Str *)mem_alloc_nozero(a, sizeof(Str) + (size_t)s.len, _Alignof(Str));
    if (b == NULL)
        return burrow_err_out_of_memory;
    Byte *p = (Byte *)(b + 1);
    tp_put(p, s.p, s.len);
    *b = str_from_bytes(p, s.len);
    return (Error){&tp_protocol_error_vt, b};
}

static Error tp_protocol_error_clone(const void *self, Alloc *a) {
    return textproto_protocol_error_as_error(*(const Str *)self, a);
}

/* ProtocolError(fmt.Sprintf(prefix + "%q", s)), all in one block. */
static Error tp_protocol_errorf(Alloc *a, Str prefix, const Byte *s, Int n) {
    Str q = str_from_bytes(s, n);
    Int mlen = prefix.len + burrow__strconv_quote_into(NULL, q);
    Str *b = (Str *)mem_alloc_nozero(a, sizeof(Str) + (size_t)mlen, _Alignof(Str));
    if (b == NULL)
        return burrow_err_out_of_memory;
    Byte *p = (Byte *)(b + 1);
    burrow__strconv_quote_into(tp_put(p, prefix.p, prefix.len), q);
    *b = str_from_bytes(p, mlen);
    return (Error){&tp_protocol_error_vt, b};
}

/* ------------------------------------------------------------------ helpers */

static bool tp_is_space(Byte b) {
    return b == ' ' || b == '\t' || b == '\n' || b == '\r';
}

static bool tp_is_letter(Byte b) {
    b |= 0x20;
    return 'a' <= b && b <= 'z';
}

Str textproto_trim_string(Str s) {
    Int i = 0, j = s.len;
    while (i < j && tp_is_space(s.p[i]))
        i++;
    while (j > i && tp_is_space(s.p[j - 1]))
        j--;
    if (i == j)
        return (Str){s.p == NULL ? NULL : s.p + i, 0};
    return str_from_bytes(s.p + i, j - i);
}

Slice textproto_trim_bytes(Slice b) {
    const Byte *p = (const Byte *)b.p;
    Int i = 0, j = b.len;
    while (i < j && tp_is_space(p[i]))
        i++;
    while (j > i && tp_is_space(p[j - 1]))
        j--;
    if (p == NULL)
        return b;
    return slice_from((Byte *)b.p + i, j - i, b.cap - i, b.elem);
}

/* --------------------------------------------------------------- canonical */

/* token = 1*tchar, from RFC 7230 section 3.2.6. */
static const uint8_t tp_header_field_byte[256] = {
    ['0'] = 1, ['1'] = 1, ['2'] = 1, ['3'] = 1, ['4'] = 1,  ['5'] = 1, ['6'] = 1,
    ['7'] = 1, ['8'] = 1, ['9'] = 1, ['a'] = 1, ['b'] = 1,  ['c'] = 1, ['d'] = 1,
    ['e'] = 1, ['f'] = 1, ['g'] = 1, ['h'] = 1, ['i'] = 1,  ['j'] = 1, ['k'] = 1,
    ['l'] = 1, ['m'] = 1, ['n'] = 1, ['o'] = 1, ['p'] = 1,  ['q'] = 1, ['r'] = 1,
    ['s'] = 1, ['t'] = 1, ['u'] = 1, ['v'] = 1, ['w'] = 1,  ['x'] = 1, ['y'] = 1,
    ['z'] = 1, ['A'] = 1, ['B'] = 1, ['C'] = 1, ['D'] = 1,  ['E'] = 1, ['F'] = 1,
    ['G'] = 1, ['H'] = 1, ['I'] = 1, ['J'] = 1, ['K'] = 1,  ['L'] = 1, ['M'] = 1,
    ['N'] = 1, ['O'] = 1, ['P'] = 1, ['Q'] = 1, ['R'] = 1,  ['S'] = 1, ['T'] = 1,
    ['U'] = 1, ['V'] = 1, ['W'] = 1, ['X'] = 1, ['Y'] = 1,  ['Z'] = 1, ['!'] = 1,
    ['#'] = 1, ['$'] = 1, ['%'] = 1, ['&'] = 1, ['\''] = 1, ['*'] = 1, ['+'] = 1,
    ['-'] = 1, ['.'] = 1, ['^'] = 1, ['_'] = 1, ['`'] = 1,  ['|'] = 1, ['~'] = 1,
};

/* field-vchar, SP and HTAB, and obs-text, which is everything from 0x80. */
static bool tp_valid_header_value_byte(Byte c) {
    return (c >= 0x20 && c != 0x7f) || c == '\t';
}

/* Go keeps these in a map built once, so that a common key costs no
 * allocation. A sorted table does the same without the once. */
static const Str tp_common_header[] = {
    BURROW_S_INIT("Accept"),
    BURROW_S_INIT("Accept-Charset"),
    BURROW_S_INIT("Accept-Encoding"),
    BURROW_S_INIT("Accept-Language"),
    BURROW_S_INIT("Accept-Ranges"),
    BURROW_S_INIT("Cache-Control"),
    BURROW_S_INIT("Cc"),
    BURROW_S_INIT("Connection"),
    BURROW_S_INIT("Content-Id"),
    BURROW_S_INIT("Content-Language"),
    BURROW_S_INIT("Content-Length"),
    BURROW_S_INIT("Content-Transfer-Encoding"),
    BURROW_S_INIT("Content-Type"),
    BURROW_S_INIT("Cookie"),
    BURROW_S_INIT("Date"),
    BURROW_S_INIT("Dkim-Signature"),
    BURROW_S_INIT("Etag"),
    BURROW_S_INIT("Expires"),
    BURROW_S_INIT("From"),
    BURROW_S_INIT("Host"),
    BURROW_S_INIT("If-Modified-Since"),
    BURROW_S_INIT("If-None-Match"),
    BURROW_S_INIT("In-Reply-To"),
    BURROW_S_INIT("Last-Modified"),
    BURROW_S_INIT("Location"),
    BURROW_S_INIT("Message-Id"),
    BURROW_S_INIT("Mime-Version"),
    BURROW_S_INIT("Pragma"),
    BURROW_S_INIT("Received"),
    BURROW_S_INIT("Referer"),
    BURROW_S_INIT("Return-Path"),
    BURROW_S_INIT("Server"),
    BURROW_S_INIT("Set-Cookie"),
    BURROW_S_INIT("Subject"),
    BURROW_S_INIT("To"),
    BURROW_S_INIT("User-Agent"),
    BURROW_S_INIT("Via"),
    BURROW_S_INIT("X-Forwarded-For"),
    BURROW_S_INIT("X-Imforwards"),
    BURROW_S_INIT("X-Powered-By"),
};

#define TP_NCOMMON ((Int)(sizeof tp_common_header / sizeof tp_common_header[0]))

Str burrow__textproto_common_header(Int i) {
    return i >= 0 && i < TP_NCOMMON ? tp_common_header[i] : BURROW_STR_EMPTY;
}

/* The same table grouped by length, so that a lookup only compares against
 * the few keys of the right length. tp_common_by_len lists indexes into
 * tp_common_header shortest first, and the keys of length n are the entries
 * from tp_common_len_start[n] up to tp_common_len_start[n + 1]. */
#define TP_COMMON_MAX_LEN 25

static const uint8_t tp_common_by_len[] = {
    6,  34, 36, 14, 16, 18, 19, 0, 13, 27, 31, 17, 29, 33, 24, 28, 7,  8, 25, 32,
    35, 22, 30, 12, 26, 38, 39, 4, 5,  21, 23, 1,  10, 15, 2,  3,  37, 9, 20, 11,
};

static const uint8_t tp_common_len_start[TP_COMMON_MAX_LEN + 2] = {
    0,  0,  0,  2,  3,  7,  7,  11, 14, 16, 16, 21, 23, 27,
    31, 34, 37, 38, 39, 39, 39, 39, 39, 39, 39, 39, 40,
};

static const Str *tp_common(const Byte *p, Int n) {
    if (n > TP_COMMON_MAX_LEN)
        return NULL;
    for (Int i = tp_common_len_start[n]; i < tp_common_len_start[n + 1]; i++) {
        const Str *h = &tp_common_header[tp_common_by_len[i]];
        if (h->p[0] == p[0] && memcmp(h->p, p, (size_t)n) == 0)
            return h;
    }
    return NULL;
}

/* Go's canonicalMIMEHeaderKey in one pass: checks every byte, and writes the
 * canonical form into dst, upper case after the start and after each hyphen
 * and lower case elsewhere. dst may be src. Returns -1 when a byte is not
 * allowed in a key, 0 when the key has a space in it and is left alone, 1
 * when dst holds the canonical form and 2 when that is the same as src. */
static int tp_canonical_pass(const Byte *src, Byte *dst, Int n) {
    /* No branches in the loop, and nothing carried from one byte to the next
     * but the totals, so the compiler can do it several bytes at a time. The
     * case flip is an xor with 0x20 or 0, picked by whether the byte is a
     * letter in the wrong case for where it sits, and where it sits is read
     * off the byte before, which the flip never turns into or out of a '-'.
     * That last part is why dst may be src. */
    unsigned bad = 0, space = 0, diff = 0;
    for (Int i = 0; i < n; i++) {
        Byte c = src[i];
        unsigned upper = i == 0 || src[i - 1] == '-';
        unsigned lower_letter = (unsigned)(c - 'a') < 26u;
        unsigned upper_letter = (unsigned)(c - 'A') < 26u;
        unsigned flip = upper ? lower_letter : upper_letter;
        bad |= !tp_header_field_byte[c] & (c != ' ');
        space |= c == ' ';
        diff |= flip;
        dst[i] = (Byte)(c ^ (flip << 5));
    }
    if (bad)
        return -1;
    if (space)
        return 0;
    return diff ? 1 : 2;
}

/* The in-place form, for keys read off the wire. What comes back is a static
 * string from the table or a[:n] itself, and *copy says whether it is a[:n],
 * which the caller has to copy out. A key with a space in it is left as it
 * was, and false means a byte that is not allowed, with a left as it was. */
static bool tp_canonical_key_bytes(Byte *a, Int n, Str *out, bool *copy) {
    *copy = true;
    *out = str_from_bytes(a, n);
    if (n == 0)
        return false;
    Byte tmp[128];
    int r;
    if (n <= (Int)sizeof tmp) {
        r = tp_canonical_pass(a, tmp, n);
        if (r == 1)
            memcpy(a, tmp, (size_t)n);
    } else {
        /* Too long to stage, and too long to be a common key: check it all
         * first so that a is only written once it is known to be good. */
        bool space = false;
        for (Int i = 0; i < n; i++) {
            if (tp_header_field_byte[a[i]])
                continue;
            if (a[i] != ' ')
                return false;
            space = true;
        }
        if (space)
            return true;
        tp_canonical_pass(a, a, n);
        return true;
    }
    if (r <= 0)
        return r == 0;
    const Str *common = tp_common(a, n);
    if (common != NULL) {
        *out = *common;
        *copy = false;
    }
    return true;
}

/* The canonical key for s. When it differs from s it is written into buf if
 * it fits, and into memory from a otherwise, and *owned says so. */
static Str tp_canonical_key(Alloc *a, Str s, Byte *buf, Int size, bool *owned,
                            bool *oom) {
    *owned = false;
    *oom = false;
    if (s.len == 0)
        return s;
    Byte *p = buf;
    if (s.len > size) {
        p = (Byte *)mem_alloc_nozero(a, (size_t)s.len, 1);
        if (p == NULL) {
            *oom = true;
            return s;
        }
        *owned = true;
    }
    int r = tp_canonical_pass(s.p, p, s.len);
    if (r != 1) {
        if (*owned)
            mem_free(a, p, (size_t)s.len, 1);
        *owned = false;
        return s;
    }
    const Str *common = tp_common(p, s.len);
    if (common != NULL) {
        if (*owned)
            mem_free(a, p, (size_t)s.len, 1);
        *owned = false;
        return *common;
    }
    return str_from_bytes(p, s.len);
}

Str textproto_canonical_mime_header_key(Alloc *a, Str s) {
    Byte buf[64];
    bool owned, oom;
    Str k = tp_canonical_key(a, s, buf, (Int)sizeof buf, &owned, &oom);
    if (oom || owned || k.p != buf)
        return k;
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)k.len, 1);
    if (p == NULL)
        return s;
    memcpy(p, buf, (size_t)k.len);
    return str_from_bytes(p, k.len);
}

/* --------------------------------------------------------------- MIMEHeader */

/* []string, the value type of the map. */
static const Type tp_strings_desc = {
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
    0x74707373U, /* "tpss" */
    NULL,
};

TextprotoMIMEHeader textproto_mime_header_make(Alloc *a) {
    return map_make(a, TYPE_STRING, &tp_strings_desc, 0);
}

/* The canonical key for a lookup, in buf when it fits. The caller gives back
 * what *owned says came from the map's allocator. */
#define TP_KEY_BUF 128

static Str tp_lookup_key(TextprotoMIMEHeader h, Str key, Byte *buf, bool *owned) {
    bool oom;
    return tp_canonical_key(burrow__map_allocator(h), key, buf, TP_KEY_BUF, owned,
                            &oom);
}

static void tp_lookup_done(TextprotoMIMEHeader h, Str k, bool owned) {
    if (owned)
        mem_free(burrow__map_allocator(h), (void *)(uintptr_t)k.p, (size_t)k.len, 1);
}

/* The canonical key to keep in h, and *fresh when it was made here and
 * belongs to h's allocator. */
static bool tp_stored_key(TextprotoMIMEHeader h, Str key, Str *out, bool *fresh) {
    Alloc *a = burrow__map_allocator(h);
    Byte buf[TP_KEY_BUF];
    bool oom;
    Str k = tp_canonical_key(a, key, buf, TP_KEY_BUF, fresh, &oom);
    if (oom)
        return false;
    if (k.p == buf) {
        Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)k.len, 1);
        if (p == NULL)
            return false;
        memcpy(p, buf, (size_t)k.len);
        k = str_from_bytes(p, k.len);
        *fresh = true;
    }
    *out = k;
    return true;
}

bool textproto_mime_header_add(TextprotoMIMEHeader h, Str key, Str value) {
    Alloc *a = burrow__map_allocator(h);
    Str k;
    bool fresh;
    if (!tp_stored_key(h, key, &k, &fresh))
        return false;
    Slice *vs = (Slice *)map_get(h, &k);
    if (vs != NULL) {
        /* The map keeps the key it has. */
        tp_lookup_done(h, k, fresh);
        Slice grown = slice_append(a, *vs, &value, 1);
        if (grown.len != vs->len + 1)
            return false;
        *vs = grown;
        return true;
    }
    Slice s = slice_append(a, slice_from(NULL, 0, 0, TYPE_STRING), &value, 1);
    if (s.len != 1 || !map_set(h, &k, &s)) {
        if (s.len == 1)
            mem_free(a, s.p, sizeof(Str) * (size_t)s.cap, _Alignof(Str));
        tp_lookup_done(h, k, fresh);
        return false;
    }
    return true;
}

bool textproto_mime_header_set(TextprotoMIMEHeader h, Str key, Str value) {
    Alloc *a = burrow__map_allocator(h);
    Str k;
    bool fresh;
    if (!tp_stored_key(h, key, &k, &fresh))
        return false;
    Str *p = (Str *)mem_alloc_nozero(a, sizeof(Str), _Alignof(Str));
    if (p == NULL) {
        tp_lookup_done(h, k, fresh);
        return false;
    }
    *p = value;
    Slice s = slice_from(p, 1, 1, TYPE_STRING);
    if (!map_set(h, &k, &s)) {
        mem_free(a, p, sizeof(Str), _Alignof(Str));
        tp_lookup_done(h, k, fresh);
        return false;
    }
    return true;
}

Str textproto_mime_header_get(TextprotoMIMEHeader h, Str key) {
    if (h == NULL)
        return BURROW_STR_EMPTY;
    Byte buf[TP_KEY_BUF];
    bool owned;
    Str k = tp_lookup_key(h, key, buf, &owned);
    Slice *vs = (Slice *)map_get(h, &k);
    tp_lookup_done(h, k, owned);
    if (vs == NULL || vs->len == 0)
        return BURROW_STR_EMPTY;
    return ((const Str *)vs->p)[0];
}

Slice textproto_mime_header_values(TextprotoMIMEHeader h, Str key) {
    if (h == NULL)
        return slice_from(NULL, 0, 0, TYPE_STRING);
    Byte buf[TP_KEY_BUF];
    bool owned;
    Str k = tp_lookup_key(h, key, buf, &owned);
    Slice *vs = (Slice *)map_get(h, &k);
    tp_lookup_done(h, k, owned);
    if (vs == NULL)
        return slice_from(NULL, 0, 0, TYPE_STRING);
    return *vs;
}

void textproto_mime_header_del(TextprotoMIMEHeader h, Str key) {
    if (h == NULL)
        return;
    Byte buf[TP_KEY_BUF];
    bool owned;
    Str k = tp_lookup_key(h, key, buf, &owned);
    map_del(h, &k);
    tp_lookup_done(h, k, owned);
}

/* ------------------------------------------------------------------- Reader */

TextprotoReader *textproto_new_reader(Alloc *a, BufioReader *br) {
    TextprotoReader *r = BURROW_NEW(a, TextprotoReader);
    if (r == NULL)
        return NULL;
    r->r = br;
    r->a = a;
    r->own = true;
    return r;
}

static Alloc *tp_reader_alloc(TextprotoReader *r) {
    if (r->a == NULL)
        r->a = heap_allocator();
    return r->a;
}

static void tp_reader_release(TextprotoReader *r) {
    if (r->buf != NULL)
        mem_free(r->a, r->buf, (size_t)r->buf_cap, 1);
    if (r->line != NULL)
        mem_free(r->a, r->line, (size_t)r->line_cap, 1);
    r->buf = r->line = NULL;
    r->buf_len = r->buf_cap = r->line_len = r->line_cap = 0;
}

void textproto_reader_free(TextprotoReader *r) {
    if (r == NULL)
        return;
    tp_reader_release(r);
    if (r->own)
        mem_free(r->a, r, sizeof *r, _Alignof(TextprotoReader));
}

/* Makes room for n more bytes in *p, which holds *len of *cap. */
static bool tp_grow(Alloc *a, Byte **p, Int *len, Int *cap, Int n) {
    if (*len + n <= *cap)
        return true;
    Int want = *cap * 2;
    if (want < *len + n)
        want = *len + n;
    if (want < 64)
        want = 64;
    Byte *q = (Byte *)mem_realloc(a, *p, (size_t)*cap, (size_t)want, 1);
    if (q == NULL)
        return false;
    *p = q;
    *cap = want;
    return true;
}

static void tp_close_dot(TextprotoReader *r);

/* Go's readLineSlice. The line points into the bufio buffer or into r->line,
 * and is good until the next read. lim < 0 means no limit. */
static bool tp_read_line_slice(TextprotoReader *r, int64_t lim, const Byte **out,
                               Int *n, Error *err) {
    tp_close_dot(r);
    bool first = true;
    r->line_len = 0;
    for (;;) {
        bool more = false;
        Error e = BURROW_NO_ERROR;
        Slice l = bufio_reader_read_line(r->r, &more, &e);
        if (!BURROW_OK(e)) {
            *err = e;
            return false;
        }
        if (lim >= 0 && (int64_t)r->line_len + (int64_t)l.len > lim) {
            *err = burrow__textproto_err_message_too_large;
            return false;
        }
        if (first && !more) {
            *out = (const Byte *)l.p;
            *n = l.len;
            return true;
        }
        first = false;
        if (!tp_grow(tp_reader_alloc(r), &r->line, &r->line_len, &r->line_cap, l.len)) {
            *err = burrow_err_out_of_memory;
            return false;
        }
        tp_put(r->line + r->line_len, l.p, l.len);
        r->line_len += l.len;
        if (!more)
            break;
    }
    *out = r->line;
    *n = r->line_len;
    return true;
}

Str textproto_reader_read_line(TextprotoReader *r, Alloc *a, Error *err) {
    Error e = BURROW_NO_ERROR;
    const Byte *p;
    Int n;
    Str s = BURROW_STR_EMPTY;
    if (tp_read_line_slice(r, -1, &p, &n, &e))
        s = tp_copy(a, p, n, &e);
    BURROW_OUT(err, e);
    return s;
}

static Slice tp_bytes(Alloc *a, const Byte *p, Int n, Error *err) {
    /* bytes.Clone of an empty line is an empty slice, not nil. */
    Byte *q = (Byte *)mem_alloc_nozero(a, (size_t)(n > 0 ? n : 1), 1);
    if (q == NULL) {
        *err = burrow_err_out_of_memory;
        return slice_from(NULL, 0, 0, TYPE_BYTE);
    }
    tp_put(q, p, n);
    return slice_from(q, n, n, TYPE_BYTE);
}

Slice textproto_reader_read_line_bytes(TextprotoReader *r, Alloc *a, Error *err) {
    Error e = BURROW_NO_ERROR;
    const Byte *p;
    Int n;
    Slice s = slice_from(NULL, 0, 0, TYPE_BYTE);
    if (tp_read_line_slice(r, -1, &p, &n, &e))
        s = tp_bytes(a, p, n, &e);
    BURROW_OUT(err, e);
    return s;
}

/* Go's trim, which takes spaces and tabs off both ends. */
static void tp_trim(const Byte **p, Int *n) {
    Int i = 0, j = *n;
    while (i < j && ((*p)[i] == ' ' || (*p)[i] == '\t'))
        i++;
    while (j > i && ((*p)[j - 1] == ' ' || (*p)[j - 1] == '\t'))
        j--;
    *p += i;
    *n = j - i;
}

/* Go's skipSpace. How many spaces and tabs it read. */
static Int tp_skip_space(TextprotoReader *r) {
    Int n = 0;
    for (;;) {
        Error e = BURROW_NO_ERROR;
        Byte c = bufio_reader_read_byte(r->r, &e);
        if (!BURROW_OK(e))
            break;
        if (c != ' ' && c != '\t') {
            (void)bufio_reader_unread_byte(r->r);
            break;
        }
        n++;
    }
    return n;
}

/* The two first line checks Go passes readContinuedLineSlice. */
static Error tp_must_have_colon(const Byte *p, Int n, Alloc *a) {
    if (n > 0 && memchr(p, ':', (size_t)n) != NULL)
        return BURROW_NO_ERROR;
    return tp_protocol_errorf(a, BURROW_S("malformed MIME header: missing colon: "), p,
                              n);
}

/* Go's readContinuedLineSlice. The line points into the bufio buffer or into
 * r->buf. On an error *n is 0. */
static bool tp_read_continued_line_slice(TextprotoReader *r, int64_t lim, bool colon,
                                         Alloc *ea, const Byte **out, Int *n,
                                         Error *err) {
    const Byte *line;
    Int len;
    *n = 0;
    if (!tp_read_line_slice(r, lim, &line, &len, err))
        return false;
    if (len == 0) {
        *out = line;
        return true;
    }
    if (colon) {
        Error e = tp_must_have_colon(line, len, ea);
        if (!BURROW_OK(e)) {
            *err = e;
            return false;
        }
    }

    /* Optimistically assume there is no continuation, and give the line
     * without copying it when the next one plainly starts afresh. */
    if (bufio_reader_buffered(r->r) > 1) {
        Error e = BURROW_NO_ERROR;
        Slice peek = bufio_reader_peek(r->r, 2, &e);
        const Byte *pk = (const Byte *)peek.p;
        if ((peek.len > 0 && (tp_is_letter(pk[0]) || pk[0] == '\n')) ||
            (peek.len == 2 && pk[0] == '\r' && pk[1] == '\n')) {
            tp_trim(&line, &len);
            *out = line;
            *n = len;
            return true;
        }
    }

    Alloc *a = tp_reader_alloc(r);
    tp_trim(&line, &len);
    r->buf_len = 0;
    if (!tp_grow(a, &r->buf, &r->buf_len, &r->buf_cap, len)) {
        *err = burrow_err_out_of_memory;
        return false;
    }
    tp_put(r->buf, line, len);
    r->buf_len = len;

    if (lim < 0)
        lim = INT64_MAX;
    lim -= r->buf_len;

    while (tp_skip_space(r) > 0) {
        if (!tp_grow(a, &r->buf, &r->buf_len, &r->buf_cap, 1)) {
            *err = burrow_err_out_of_memory;
            return false;
        }
        r->buf[r->buf_len++] = ' ';
        if ((int64_t)r->buf_len >= lim) {
            *err = burrow__textproto_err_message_too_large;
            return false;
        }
        Error e = BURROW_NO_ERROR;
        if (!tp_read_line_slice(r, lim - r->buf_len, &line, &len, &e))
            break;
        tp_trim(&line, &len);
        if (!tp_grow(a, &r->buf, &r->buf_len, &r->buf_cap, len)) {
            *err = burrow_err_out_of_memory;
            return false;
        }
        tp_put(r->buf + r->buf_len, line, len);
        r->buf_len += len;
    }
    *out = r->buf;
    *n = r->buf_len;
    return true;
}

Str textproto_reader_read_continued_line(TextprotoReader *r, Alloc *a, Error *err) {
    Error e = BURROW_NO_ERROR;
    const Byte *p;
    Int n;
    Str s = BURROW_STR_EMPTY;
    if (tp_read_continued_line_slice(r, -1, false, a, &p, &n, &e))
        s = tp_copy(a, p, n, &e);
    BURROW_OUT(err, e);
    return s;
}

Slice textproto_reader_read_continued_line_bytes(TextprotoReader *r, Alloc *a,
                                                 Error *err) {
    Error e = BURROW_NO_ERROR;
    const Byte *p;
    Int n;
    Slice s = slice_from(NULL, 0, 0, TYPE_BYTE);
    if (tp_read_continued_line_slice(r, -1, false, a, &p, &n, &e))
        s = tp_bytes(a, p, n, &e);
    BURROW_OUT(err, e);
    return s;
}

/* Go's parseCodeLine. */
static Int tp_parse_code_line(Str line, Int expect, Alloc *a, bool *continued,
                              Str *message, Error *err) {
    *continued = false;
    *message = BURROW_STR_EMPTY;
    if (line.len < 4 || (line.p[3] != ' ' && line.p[3] != '-')) {
        *err = tp_protocol_errorf(a, BURROW_S("short response: "), line.p, line.len);
        return 0;
    }
    *continued = line.p[3] == '-';
    /* strconv.Atoi(line[0:3]), which takes a sign. */
    const Byte *d = line.p;
    Int code = 0;
    bool ok = true;
    Int i = (d[0] == '+' || d[0] == '-') ? 1 : 0;
    for (Int j = i; j < 3; j++) {
        if (d[j] < '0' || d[j] > '9') {
            ok = false;
            break;
        }
        code = code * 10 + (d[j] - '0');
    }
    if (!ok)
        code = 0;
    else if (d[0] == '-')
        code = -code;
    if (!ok || code < 100) {
        *err = tp_protocol_errorf(a, BURROW_S("invalid response code: "), line.p,
                                  line.len);
        return code;
    }
    *message = str_from_bytes(line.p + 4, line.len - 4);
    if ((1 <= expect && expect < 10 && code / 100 != expect) ||
        (10 <= expect && expect < 100 && code / 10 != expect) ||
        (100 <= expect && expect < 1000 && code != expect)) {
        TextprotoError te = {code, *message};
        *err = textproto_error_as_error(&te, a);
    }
    return code;
}

/* Go's readCodeLine. */
static Int tp_read_code_line(TextprotoReader *r, Alloc *a, Int expect, bool *continued,
                             Str *message, Error *err) {
    *continued = false;
    *message = BURROW_STR_EMPTY;
    Str line = textproto_reader_read_line(r, a, err);
    if (!BURROW_OK(*err))
        return 0;
    return tp_parse_code_line(line, expect, a, continued, message, err);
}

Int textproto_reader_read_code_line(TextprotoReader *r, Alloc *a, Int expect_code,
                                    Str *message, Error *err) {
    Error e = BURROW_NO_ERROR;
    bool continued;
    Str msg;
    Int code = tp_read_code_line(r, a, expect_code, &continued, &msg, &e);
    if (BURROW_OK(e) && continued)
        e = tp_protocol_errorf(a, BURROW_S("unexpected multi-line response: "), msg.p,
                               msg.len);
    if (message != NULL)
        *message = msg;
    BURROW_OUT(err, e);
    return code;
}

Int textproto_reader_read_response(TextprotoReader *r, Alloc *a, Int expect_code,
                                   Str *message, Error *err) {
    Error e = BURROW_NO_ERROR;
    bool continued;
    Str first;
    Int code = tp_read_code_line(r, a, expect_code, &continued, &first, &e);
    bool multi = continued;
    StringsBuilder b = STRINGS_BUILDER(a);
    Str msg = first;
    if (continued)
        strings_builder_write_string(&b, first, NULL);
    while (continued) {
        Error le = BURROW_NO_ERROR;
        Str line = textproto_reader_read_line(r, a, &le);
        if (!BURROW_OK(le)) {
            if (message != NULL)
                *message = BURROW_STR_EMPTY;
            BURROW_OUT(err, le);
            return 0;
        }
        Error pe = BURROW_NO_ERROR;
        Str more;
        Int code2 = tp_parse_code_line(line, 0, a, &continued, &more, &pe);
        strings_builder_write_byte(&b, '\n');
        if (!BURROW_OK(pe) || code2 != code) {
            while (line.len > 0 &&
                   (line.p[line.len - 1] == '\r' || line.p[line.len - 1] == '\n'))
                line.len--;
            strings_builder_write_string(&b, line, NULL);
            continued = true;
            continue;
        }
        strings_builder_write_string(&b, more, NULL);
    }
    if (multi)
        msg = strings_builder_string(&b);
    if (!BURROW_OK(e) && multi && msg.len > 0) {
        TextprotoError te = {code, msg};
        e = textproto_error_as_error(&te, a);
    }
    if (message != NULL)
        *message = msg;
    BURROW_OUT(err, e);
    return code;
}

/* The dot reader's states, Go's stateBeginLine and the rest. */
enum {
    TP_DOT_BEGIN_LINE, /* at the start of a line, and where it starts */
    TP_DOT_DOT,        /* read "." at the start of a line */
    TP_DOT_DOT_CR,     /* read ".\r" at the start of a line */
    TP_DOT_CR,         /* read "\r", maybe at the end of a line */
    TP_DOT_DATA,       /* in the middle of a line */
    TP_DOT_EOF,        /* read the ".\r\n" line that ends the block */
};

static Int tp_dot_read(void *self, Slice p, Error *err) {
    TextprotoReader *r = (TextprotoReader *)self;
    BufioReader *br = r->r;
    Byte *b = (Byte *)p.p;
    Int n = 0;
    Error e = BURROW_NO_ERROR;
    while (n < p.len && r->dot_state != TP_DOT_EOF) {
        Byte c = bufio_reader_read_byte(br, &e);
        if (!BURROW_OK(e)) {
            if (tp_same_error(e, io_eof))
                e = io_err_unexpected_eof;
            break;
        }
        switch (r->dot_state) {
        case TP_DOT_BEGIN_LINE:
            if (c == '.') {
                r->dot_state = TP_DOT_DOT;
                continue;
            }
            if (c == '\r') {
                r->dot_state = TP_DOT_CR;
                continue;
            }
            r->dot_state = TP_DOT_DATA;
            break;
        case TP_DOT_DOT:
            if (c == '\r') {
                r->dot_state = TP_DOT_DOT_CR;
                continue;
            }
            if (c == '\n') {
                r->dot_state = TP_DOT_EOF;
                continue;
            }
            r->dot_state = TP_DOT_DATA;
            break;
        case TP_DOT_DOT_CR:
            if (c == '\n') {
                r->dot_state = TP_DOT_EOF;
                continue;
            }
            /* Not part of .\r\n. Consume the leading dot and give the \r. */
            (void)bufio_reader_unread_byte(br);
            c = '\r';
            r->dot_state = TP_DOT_DATA;
            break;
        case TP_DOT_CR:
            if (c == '\n') {
                r->dot_state = TP_DOT_BEGIN_LINE;
                break;
            }
            /* Not part of \r\n. Give the \r and read c again next time. */
            (void)bufio_reader_unread_byte(br);
            c = '\r';
            r->dot_state = TP_DOT_DATA;
            break;
        case TP_DOT_DATA:
            if (c == '\r') {
                r->dot_state = TP_DOT_CR;
                continue;
            }
            if (c == '\n')
                r->dot_state = TP_DOT_BEGIN_LINE;
            break;
        default:
            break;
        }
        b[n++] = c;
    }
    if (BURROW_OK(e) && r->dot_state == TP_DOT_EOF)
        e = io_eof;
    if (!BURROW_OK(e))
        r->dot_open = false;
    BURROW_OUT(err, e);
    return n;
}

static const IoReaderVT tp_dot_reader_vt = {NULL, tp_dot_read};

static void tp_close_dot(TextprotoReader *r) {
    Byte buf[128];
    while (r->dot_open)
        (void)tp_dot_read(
            r, slice_from(buf, (Int)sizeof buf, (Int)sizeof buf, TYPE_BYTE), NULL);
}

IoReader textproto_reader_dot_reader(TextprotoReader *r) {
    tp_close_dot(r);
    r->dot_state = TP_DOT_BEGIN_LINE;
    r->dot_open = true;
    return (IoReader){&tp_dot_reader_vt, r};
}

Slice textproto_reader_read_dot_bytes(TextprotoReader *r, Alloc *a, Error *err) {
    return io_read_all(a, textproto_reader_dot_reader(r), err);
}

Slice textproto_reader_read_dot_lines(TextprotoReader *r, Alloc *a, Error *err) {
    Slice v = slice_from(NULL, 0, 0, TYPE_STRING);
    Error e = BURROW_NO_ERROR;
    for (;;) {
        Str line = textproto_reader_read_line(r, a, &e);
        if (!BURROW_OK(e)) {
            if (tp_same_error(e, io_eof))
                e = io_err_unexpected_eof;
            break;
        }
        /* Dot by itself marks the end, and otherwise a leading dot is
         * dropped. */
        if (line.len > 0 && line.p[0] == '.') {
            if (line.len == 1)
                break;
            line = str_from_bytes(line.p + 1, line.len - 1);
        }
        Slice grown = slice_append(a, v, &line, 1);
        if (grown.len != v.len + 1) {
            e = burrow_err_out_of_memory;
            break;
        }
        v = grown;
    }
    BURROW_OUT(err, e);
    return v;
}

/* Go's upcomingHeaderKeys: how many header lines are already buffered, as a
 * hint for sizing the map. */
Int burrow__textproto_upcoming_header_keys(TextprotoReader *r) {
    Error e = BURROW_NO_ERROR;
    (void)bufio_reader_peek(r->r, 1, &e); /* load the buffer if it is empty */
    Int s = bufio_reader_buffered(r->r);
    if (s == 0)
        return 0;
    Slice peek = bufio_reader_peek(r->r, s, &e);
    const Byte *p = (const Byte *)peek.p;
    Int left = peek.len, n = 0;
    while (left > 0 && n < 1000) {
        const Byte *nl = (const Byte *)memchr(p, '\n', (size_t)left);
        Int len = nl != NULL ? (Int)(nl - p) : left;
        const Byte *line = p;
        if (nl != NULL) {
            p = nl + 1;
            left -= len + 1;
        } else {
            left = 0;
        }
        if (len == 0 || (len == 1 && line[0] == '\r'))
            break;
        if (line[0] == ' ' || line[0] == '\t')
            continue; /* a continuation */
        n++;
    }
    return n;
}

TextprotoMIMEHeader textproto_reader_read_mime_header(TextprotoReader *r, Alloc *a,
                                                      Error *err) {
    return burrow__textproto_read_mime_header(r, a, INT64_MAX, INT64_MAX, err);
}

#define TP_MALFORMED BURROW_S("malformed MIME header line: ")

TextprotoMIMEHeader burrow__textproto_read_mime_header(TextprotoReader *r, Alloc *a,
                                                       int64_t max_memory,
                                                       int64_t max_headers,
                                                       Error *err) {
    Error e = BURROW_NO_ERROR;

    /* Most headers have one value, so the values come from one block and each
     * key's slice starts out as one entry of it. */
    Str *strs = NULL;
    Int nstrs = 0;
    Int hint = burrow__textproto_upcoming_header_keys(r);
    if (hint > 0) {
        if (hint > 1000)
            hint = 1000;
        strs = (Str *)mem_alloc_nozero(a, sizeof(Str) * (size_t)hint, _Alignof(Str));
        if (strs != NULL)
            nstrs = hint;
    }

    TextprotoMIMEHeader m = map_make(a, TYPE_STRING, &tp_strings_desc, hint);
    if (m == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }

    /* The first line cannot be a continuation. */
    max_memory -= 400;
    const int64_t map_entry_overhead = 200;

    {
        Slice pk = bufio_reader_peek(r->r, 1, &e);
        if (BURROW_OK(e) && pk.len > 0 &&
            (((const Byte *)pk.p)[0] == ' ' || ((const Byte *)pk.p)[0] == '\t')) {
            const Byte *line;
            Int len;
            if (tp_read_line_slice(r, 80, &line, &len, &e))
                e = tp_protocol_errorf(
                    a, BURROW_S("malformed MIME header initial line: "), line, len);
            BURROW_OUT(err, e);
            return m;
        }
        e = BURROW_NO_ERROR;
    }

    for (;;) {
        const Byte *kv;
        Int kvlen;
        tp_read_continued_line_slice(r, max_memory, true, a, &kv, &kvlen, &e);
        if (kvlen == 0) {
            BURROW_OUT(err, e);
            return m;
        }

        /* Key ends at the first colon. */
        const Byte *colon = (const Byte *)memchr(kv, ':', (size_t)kvlen);
        if (colon == NULL) {
            BURROW_OUT(err, tp_protocol_errorf(a, TP_MALFORMED, kv, kvlen));
            return m;
        }
        Int klen = (Int)(colon - kv);
        /* The key is canonicalised where it lies, which is the buffer of the
         * bufio reader or r->buf, and the message quotes it as it is now. */
        Str key;
        bool copy;
        if (!tp_canonical_key_bytes((Byte *)(uintptr_t)kv, klen, &key, &copy)) {
            BURROW_OUT(err, tp_protocol_errorf(a, TP_MALFORMED, kv, kvlen));
            return m;
        }
        const Byte *v = colon + 1;
        Int vlen = kvlen - klen - 1;
        for (Int i = 0; i < vlen; i++) {
            if (!tp_valid_header_value_byte(v[i])) {
                BURROW_OUT(err, tp_protocol_errorf(a, TP_MALFORMED, kv, kvlen));
                return m;
            }
        }

        max_headers--;
        if (max_headers < 0) {
            BURROW_OUT(err, burrow__textproto_err_message_too_large);
            return NULL;
        }

        /* Skip initial spaces in value. */
        while (vlen > 0 && (v[0] == ' ' || v[0] == '\t')) {
            v++;
            vlen--;
        }

        Slice *vv = (Slice *)map_get(m, &key);
        if (vv == NULL) {
            max_memory -= key.len;
            max_memory -= map_entry_overhead;
        }
        max_memory -= vlen;
        if (max_memory < 0) {
            BURROW_OUT(err, burrow__textproto_err_message_too_large);
            return m;
        }

        Error ce = BURROW_NO_ERROR;
        Str value = tp_copy(a, v, vlen, &ce);
        if (vv == NULL && copy)
            key = tp_copy(a, key.p, key.len, &ce);
        if (!BURROW_OK(ce)) {
            BURROW_OUT(err, ce);
            return m;
        }
        if (vv == NULL && nstrs > 0) {
            *strs = value;
            Slice one = slice_from(strs, 1, 1, TYPE_STRING);
            strs++;
            nstrs--;
            if (!map_set(m, &key, &one)) {
                BURROW_OUT(err, burrow_err_out_of_memory);
                return m;
            }
        } else if (vv == NULL) {
            Slice one = slice_append(a, slice_from(NULL, 0, 0, TYPE_STRING), &value, 1);
            if (one.len != 1 || !map_set(m, &key, &one)) {
                BURROW_OUT(err, burrow_err_out_of_memory);
                return m;
            }
        } else {
            Slice grown = slice_append(a, *vv, &value, 1);
            if (grown.len != vv->len + 1) {
                BURROW_OUT(err, burrow_err_out_of_memory);
                return m;
            }
            *vv = grown;
        }

        if (!BURROW_OK(e)) {
            BURROW_OUT(err, e);
            return m;
        }
    }
}

/* ------------------------------------------------------------------- Writer */

TextprotoWriter *textproto_new_writer(Alloc *a, BufioWriter *bw) {
    TextprotoWriter *w = BURROW_NEW(a, TextprotoWriter);
    if (w == NULL)
        return NULL;
    w->w = bw;
    w->a = a;
    w->own = true;
    return w;
}

void textproto_writer_free(TextprotoWriter *w) {
    if (w != NULL && w->own)
        mem_free(w->a, w, sizeof *w, _Alignof(TextprotoWriter));
}

/* The dot writer's states, Go's wstateBegin and the rest. */
enum {
    TP_WDOT_BEGIN,      /* the start, and where it starts */
    TP_WDOT_BEGIN_LINE, /* the start of a line */
    TP_WDOT_CR,         /* wrote "\r", maybe at the end of a line */
    TP_WDOT_DATA,       /* in the middle of a line */
};

static Int tp_dot_write(void *self, Slice p, Error *err) {
    TextprotoWriter *w = (TextprotoWriter *)self;
    BufioWriter *bw = w->w;
    const Byte *b = (const Byte *)p.p;
    Int n = 0;
    Error e = BURROW_NO_ERROR;
    while (n < p.len) {
        Byte c = b[n];
        switch (w->dot_state) {
        case TP_WDOT_BEGIN:
        case TP_WDOT_BEGIN_LINE:
            w->dot_state = TP_WDOT_DATA;
            if (c == '.')
                (void)bufio_writer_write_byte(bw, '.'); /* escape a leading dot */
            /* fallthrough */
        case TP_WDOT_DATA:
            if (c == '\r')
                w->dot_state = TP_WDOT_CR;
            if (c == '\n') {
                (void)bufio_writer_write_byte(bw, '\r');
                w->dot_state = TP_WDOT_BEGIN_LINE;
            }
            break;
        case TP_WDOT_CR:
            w->dot_state = TP_WDOT_DATA;
            if (c == '\n')
                w->dot_state = TP_WDOT_BEGIN_LINE;
            break;
        default:
            break;
        }
        e = bufio_writer_write_byte(bw, c);
        if (!BURROW_OK(e))
            break;
        n++;
    }
    BURROW_OUT(err, e);
    return n;
}

static Error tp_dot_close(void *self) {
    TextprotoWriter *w = (TextprotoWriter *)self;
    if (!w->dot_open)
        return BURROW_NO_ERROR;
    w->dot_open = false;
    BufioWriter *bw = w->w;
    switch (w->dot_state) {
    default:
        (void)bufio_writer_write_byte(bw, '\r');
        /* fallthrough */
    case TP_WDOT_CR:
        (void)bufio_writer_write_byte(bw, '\n');
        /* fallthrough */
    case TP_WDOT_BEGIN_LINE: {
        static const Byte dotcrnl[] = {'.', '\r', '\n'};
        (void)bufio_writer_write(
            bw, slice_from((Byte *)(uintptr_t)dotcrnl, 3, 3, TYPE_BYTE), NULL);
        break;
    }
    }
    return bufio_writer_flush(bw);
}

static const IoWriteCloserVT tp_dot_writer_vt = {
    {NULL, tp_dot_write},
    {NULL, tp_dot_close},
};

static void tp_writer_close_dot(TextprotoWriter *w) {
    if (w->dot_open)
        (void)tp_dot_close(w);
}

IoWriteCloser textproto_writer_dot_writer(TextprotoWriter *w) {
    tp_writer_close_dot(w);
    w->dot_state = TP_WDOT_BEGIN;
    w->dot_open = true;
    return (IoWriteCloser){&tp_dot_writer_vt, w};
}

Error textproto_writer_printf_line(TextprotoWriter *w, Str format, Slice args) {
    tp_writer_close_dot(w);
    (void)fmt_fprintf(bufio_writer_as_io_writer(w->w), format, args, NULL);
    static const Byte crnl[] = {'\r', '\n'};
    (void)bufio_writer_write(w->w, slice_from((Byte *)(uintptr_t)crnl, 2, 2, TYPE_BYTE),
                             NULL);
    return bufio_writer_flush(w->w);
}

/* ----------------------------------------------------------------- Pipeline */

/* Go's sequencer hands each waiter a channel of its own. One condition
 * variable that every waiter checks does the same job. */
static void tp_seq_ready(burrow__TextprotoSequencer *s) {
    if (!s->ready) {
        s->cond = SYNC_COND(sync_mutex_locker(&s->mu));
        s->ready = true;
    }
}

static void tp_seq_start(burrow__TextprotoSequencer *s, Uint id) {
    sync_mutex_lock(&s->mu);
    tp_seq_ready(s);
    while (s->id != id)
        sync_cond_wait(&s->cond);
    sync_mutex_unlock(&s->mu);
}

static void tp_seq_end(burrow__TextprotoSequencer *s, Uint id) {
    sync_mutex_lock(&s->mu);
    if (s->id != id) {
        sync_mutex_unlock(&s->mu);
        panic_str(BURROW_S("out of sync"));
    }
    tp_seq_ready(s);
    s->id = id + 1;
    sync_cond_broadcast(&s->cond);
    sync_mutex_unlock(&s->mu);
}

Uint textproto_pipeline_next(TextprotoPipeline *p) {
    sync_mutex_lock(&p->mu);
    Uint id = p->id++;
    sync_mutex_unlock(&p->mu);
    return id;
}

void textproto_pipeline_start_request(TextprotoPipeline *p, Uint id) {
    tp_seq_start(&p->request, id);
}

void textproto_pipeline_end_request(TextprotoPipeline *p, Uint id) {
    tp_seq_end(&p->request, id);
}

void textproto_pipeline_start_response(TextprotoPipeline *p, Uint id) {
    tp_seq_start(&p->response, id);
}

void textproto_pipeline_end_response(TextprotoPipeline *p, Uint id) {
    tp_seq_end(&p->response, id);
}

/* --------------------------------------------------------------------- Conn */

TextprotoConn *textproto_new_conn(Alloc *a, IoReadWriteCloser conn) {
    TextprotoConn *c = BURROW_NEW(a, TextprotoConn);
    if (c == NULL)
        return NULL;
    c->a = a;
    c->conn = conn;
    BufioReader *br = bufio_new_reader(a, (IoReader){&conn.vt->reader, conn.data});
    BufioWriter *bw = bufio_new_writer(a, (IoWriter){&conn.vt->writer, conn.data});
    if (br == NULL || bw == NULL) {
        bufio_reader_free(br);
        bufio_writer_free(bw);
        mem_free(a, c, sizeof *c, _Alignof(TextprotoConn));
        return NULL;
    }
    c->reader.r = br;
    c->reader.a = a;
    c->writer.w = bw;
    c->writer.a = a;
    return c;
}

Error textproto_conn_close(TextprotoConn *c) {
    return c->conn.vt->closer.close(c->conn.data);
}

void textproto_conn_free(TextprotoConn *c) {
    if (c == NULL)
        return;
    tp_reader_release(&c->reader);
    bufio_reader_free(c->reader.r);
    bufio_writer_free(c->writer.w);
    mem_free(c->a, c, sizeof *c, _Alignof(TextprotoConn));
}

Uint textproto_conn_cmd(TextprotoConn *c, Str format, Slice args, Error *err) {
    Uint id = textproto_pipeline_next(&c->pipeline);
    textproto_pipeline_start_request(&c->pipeline, id);
    Error e = textproto_writer_printf_line(&c->writer, format, args);
    textproto_pipeline_end_request(&c->pipeline, id);
    BURROW_OUT(err, e);
    return BURROW_OK(e) ? id : 0;
}

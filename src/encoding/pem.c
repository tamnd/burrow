/* Derived from Go's src/encoding/pem/pem.go.
 * Go source: go1.27.1.
 *
 * Decode is a straight port that walks the input the same way, so it finds the
 * same block and leaves the same rest on every input, broken ones included.
 * The block and all its text are one allocation, the decoded bytes another,
 * and the headers map a third.
 *
 * Encode writes the same bytes Go's does without allocating. Go runs a base64
 * encoder into a writer that breaks lines at 64 characters. 48 bytes encode to
 * exactly 64 characters with no padding, so encoding 48 bytes at a time into a
 * buffer on the stack gives the same lines.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/encoding/pem.h"

#include "burrow/bytes.h"
#include "burrow/encoding/base64.h"
#include "burrow/strings.h"

#include <string.h>

BURROW_SENTINEL_ERROR(pem_err_colon_key,
                      "pem: cannot encode a header key that contains a colon");

static const Type pem_block_desc = {
    {(const Byte *)"Block", 5},
    {(const Byte *)"pem", 3},
    KIND_STRUCT,
    (uint32_t)sizeof(PemBlock),
    (uint16_t)_Alignof(PemBlock),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x70656d62U, /* "pemb" */
    NULL,
};

const Type *const TYPE_PEM_BLOCK = &pem_block_desc;

/* ------------------------------------------------------------------- decode */

#define PEM_START "\n-----BEGIN "
#define PEM_START_LEN 12
#define PEM_BEGIN "-----BEGIN " /* PEM_START without its newline */
#define PEM_END "\n-----END "
#define PEM_END_LEN 10
#define PEM_END_LINE "-----END " /* PEM_END without its newline */
#define PEM_END_OF_LINE "-----"
#define PEM_END_OF_LINE_LEN 5

/* getLine. The line at the start of d, without its \n or \r\n and without
 * trailing spaces and tabs, in *line_len. Returns how many bytes the line
 * took, the line ending included. */
static Int pem_get_line(const Byte *d, Int n, Int *line_len) {
    const Byte *nl = n > 0 ? (const Byte *)memchr(d, '\n', (size_t)n) : NULL;
    Int i, j;
    if (nl == NULL) {
        i = n;
        j = i;
    } else {
        i = (Int)(nl - d);
        j = i + 1;
        if (i > 0 && d[i - 1] == '\r')
            i--;
    }
    while (i > 0 && (d[i - 1] == ' ' || d[i - 1] == '\t'))
        i--;
    *line_len = i;
    return j;
}

static Slice pem_bytes(const Byte *p, Int n) {
    return slice_from((void *)(uintptr_t)p, n, n, TYPE_BYTE);
}

static bool pem_has_prefix(const Byte *p, Int n, const Byte *pre, Int m) {
    return n >= m && (m == 0 || memcmp(p, pre, (size_t)m) == 0);
}

static bool pem_has_suffix(const Byte *p, Int n, const char *suf, Int m) {
    return n >= m && memcmp(p + n - m, suf, (size_t)m) == 0;
}

/* One header line cut at its first colon, both halves trimmed as Go does. */
static void pem_cut_header(const Byte *line, Int n, Slice *key, Slice *val) {
    Int c = (Int)((const Byte *)memchr(line, ':', (size_t)n) - line);
    *key = bytes_trim_space(pem_bytes(line, c));
    *val = bytes_trim_space(pem_bytes(line + c + 1, n - c - 1));
}

/* removeSpacesAndTabs into a buffer from a, which *buf is set to when one was
 * needed. The base64 decoder skips line breaks on its own. */
static Slice pem_remove_spaces(Alloc *a, Slice data, Byte **buf, bool *oom) {
    *buf = NULL;
    if (!bytes_contains_any(data, BURROW_S(" \t")))
        return data;
    Byte *out = (Byte *)mem_alloc_nozero(a, (size_t)data.len, 1);
    if (out == NULL) {
        *oom = true;
        return data;
    }
    Int n = 0;
    const Byte *p = (const Byte *)data.p;
    for (Int i = 0; i < data.len; i++)
        if (p[i] != ' ' && p[i] != '\t')
            out[n++] = p[i];
    *buf = out;
    return pem_bytes(out, n);
}

/* Builds the block once Decode has found one: the struct with the type and the
 * header text after it, the headers map and the bytes, which are handed over
 * already decoded. NULL when a refuses, with nothing left allocated. */
static PemBlock *pem_build(Alloc *a, const Byte *type, Int type_len, const Byte *hdr,
                           Int hdr_len, Int hdr_count, Slice bytes) {
    size_t size = sizeof(PemBlock) + (size_t)type_len;
    const Byte *p = hdr;
    Int left = hdr_len;
    for (Int h = 0; h < hdr_count; h++) {
        Int ll;
        Int c = pem_get_line(p, left, &ll);
        Slice k, v;
        pem_cut_header(p, ll, &k, &v);
        size += (size_t)(k.len + v.len);
        p += c;
        left -= c;
    }

    PemBlock *b = (PemBlock *)mem_alloc_nozero(a, size, _Alignof(PemBlock));
    if (b == NULL)
        return NULL;
    Byte *text = (Byte *)(b + 1);
    if (type_len > 0)
        memcpy(text, type, (size_t)type_len);
    b->type = str_from_bytes(text, type_len);
    text += type_len;
    b->bytes = bytes;
    b->size = size;
    b->headers = map_make(a, TYPE_STRING, TYPE_STRING, hdr_count);
    if (b->headers == NULL)
        goto oom;
    p = hdr;
    left = hdr_len;
    for (Int h = 0; h < hdr_count; h++) {
        Int ll;
        Int c = pem_get_line(p, left, &ll);
        Slice k, v;
        pem_cut_header(p, ll, &k, &v);
        if (k.len > 0)
            memcpy(text, k.p, (size_t)k.len);
        Str key = str_from_bytes(text, k.len);
        text += k.len;
        if (v.len > 0)
            memcpy(text, v.p, (size_t)v.len);
        Str val = str_from_bytes(text, v.len);
        text += v.len;
        if (!map_set(b->headers, &key, &val))
            goto oom;
        p += c;
        left -= c;
    }
    return b;

oom:
    map_free(b->headers);
    mem_free(a, b, size, _Alignof(PemBlock));
    return NULL;
}

PemBlock *pem_decode(Alloc *a, Slice data, Slice *rest_out) {
    const Byte *rest = (const Byte *)data.p;
    Int rlen = data.len;
    Int end_trailer_index = 0;

    for (;;) {
        /* A bad block can leave the index anywhere, so look before using it. */
        if (end_trailer_index < 0 || end_trailer_index > rlen)
            break;
        rest += end_trailer_index;
        rlen -= end_trailer_index;

        /* Find the first END line and work back to its BEGIN. Searching from
         * the front for BEGIN instead goes quadratic on input made of nothing
         * but BEGIN lines, which was CVE-2022-24675. */
        Int end_index = bytes_index(pem_bytes(rest, rlen), BURROW_B(PEM_END));
        if (end_index < 0)
            break;
        end_trailer_index = end_index + PEM_END_LEN;
        Int begin_index =
            bytes_last_index(pem_bytes(rest, end_index), BURROW_B(PEM_BEGIN));
        if (begin_index < 0 || (begin_index > 0 && rest[begin_index - 1] != '\n'))
            continue;
        Int skip = begin_index + PEM_START_LEN - 1;
        rest += skip;
        rlen -= skip;
        end_index -= skip;
        end_trailer_index -= skip;

        const Byte *type_line = rest;
        Int type_len;
        Int consumed = pem_get_line(rest, rlen, &type_len);
        rest += consumed;
        rlen -= consumed;
        end_index -= consumed;
        end_trailer_index -= consumed;
        if (!pem_has_suffix(type_line, type_len, PEM_END_OF_LINE, PEM_END_OF_LINE_LEN))
            continue;
        type_len -= PEM_END_OF_LINE_LEN;

        /* The headers, if any, one per line up to the first without a colon. */
        const Byte *hdr = rest;
        Int hdr_count = 0;
        bool truncated = false;
        for (;;) {
            if (rlen == 0) {
                truncated = true;
                break;
            }
            Int ll;
            Int c = pem_get_line(rest, rlen, &ll);
            if (ll <= 0 || memchr(rest, ':', (size_t)ll) == NULL)
                break;
            hdr_count++;
            rest += c;
            rlen -= c;
            end_index -= c;
            end_trailer_index -= c;
        }
        if (truncated)
            break;

        /* The headers ran past the END line, which means the block has no
         * blank line after them and is not one. */
        if (hdr_count > 0 && end_index < 0)
            continue;

        /* The END line has to say the same type and nothing after it but
         * white space. */
        Int trailer_len = type_len + PEM_END_OF_LINE_LEN;
        if (rlen - end_trailer_index < trailer_len)
            continue;
        const Byte *end_trailer = rest + end_trailer_index;
        const Byte *rest_of_end_line = end_trailer + trailer_len;
        if (!pem_has_prefix(end_trailer, trailer_len, type_line, type_len) ||
            !pem_has_suffix(end_trailer, trailer_len, PEM_END_OF_LINE,
                            PEM_END_OF_LINE_LEN))
            continue;
        Int tail_len;
        pem_get_line(rest_of_end_line, rlen - end_trailer_index - trailer_len,
                     &tail_len);
        if (tail_len != 0)
            continue;

        Slice bytes = slice_nil(TYPE_BYTE);
        if (end_index > 0) {
            Byte *tmp;
            bool oom = false;
            Slice b64 = pem_remove_spaces(a, pem_bytes(rest, end_index), &tmp, &oom);
            if (oom)
                break;
            Int cap = base64_encoding_decoded_len(base64_std_encoding, b64.len);
            Byte *out = NULL;
            if (cap > 0) {
                out = (Byte *)mem_alloc_nozero(a, (size_t)cap, 1);
                if (out == NULL) {
                    if (tmp != NULL)
                        mem_free(a, tmp, (size_t)end_index, 1);
                    break;
                }
            }
            Error err = BURROW_NO_ERROR;
            Int n = base64_encoding_decode(
                base64_std_encoding, slice_from(out, cap, cap, TYPE_BYTE), b64, &err);
            if (tmp != NULL)
                mem_free(a, tmp, (size_t)end_index, 1);
            if (BURROW_FAILED(err)) {
                if (out != NULL)
                    mem_free(a, out, (size_t)cap, 1);
                continue;
            }
            bytes = slice_from(out, n, cap, TYPE_BYTE);
        }

        PemBlock *b =
            pem_build(a, type_line, type_len, hdr, (Int)(rest - hdr), hdr_count, bytes);
        if (b == NULL) {
            if (bytes.cap > 0)
                mem_free(a, bytes.p, (size_t)bytes.cap, 1);
            break;
        }

        const Byte *after = rest + end_index + PEM_END_LEN - 1;
        Int after_len = rlen - (end_index + PEM_END_LEN - 1);
        Int ll;
        Int c = pem_get_line(after, after_len, &ll);
        if (rest_out != NULL)
            *rest_out = pem_bytes(after + c, after_len - c);
        return b;
    }

    if (rest_out != NULL)
        *rest_out = data;
    return NULL;
}

void pem_block_free(Alloc *a, PemBlock *b) {
    if (b == NULL)
        return;
    map_free(b->headers);
    if (b->bytes.cap > 0)
        mem_free(a, b->bytes.p, (size_t)b->bytes.cap, 1);
    mem_free(a, b, b->size, _Alignof(PemBlock));
}

/* ------------------------------------------------------------------- encode */

#define PEM_LINE_LENGTH 64

static Error pem_write(IoWriter out, const void *p, Int n) {
    Error err = BURROW_NO_ERROR;
    (void)BURROW_CALL(out, write, pem_bytes((const Byte *)p, n), &err);
    return err;
}

static Error pem_write_str(IoWriter out, Str s) {
    return pem_write(out, s.p, s.len);
}

#define PEM_TRY(call)                                                                  \
    do {                                                                               \
        Error pem_e_ = (call);                                                         \
        if (BURROW_FAILED(pem_e_))                                                     \
            return pem_e_;                                                             \
    } while (0)

static Error pem_write_header(IoWriter out, Str k, Str v) {
    PEM_TRY(pem_write_str(out, k));
    PEM_TRY(pem_write(out, ": ", 2));
    PEM_TRY(pem_write_str(out, v));
    return pem_write(out, "\n", 1);
}

Error pem_encode(IoWriter out, const PemBlock *b) {
    Map *headers = b->headers;
    const void *kp;
    void *vp;
    for (MapIter it = map_iter(headers); map_next(&it, &kp, NULL);)
        if (strings_contains(*(const Str *)kp, BURROW_S(":")))
            return pem_err_colon_key;

    PEM_TRY(pem_write(out, PEM_BEGIN, PEM_START_LEN - 1));
    PEM_TRY(pem_write_str(out, b->type));
    PEM_TRY(pem_write(out, "-----\n", 6));

    if (map_len(headers) > 0) {
        /* Proc-Type first when there is one, since RFC 1421 wants it there,
         * and then the rest in order. Picking the next smallest key each time
         * is quadratic, and a block has a handful of headers. */
        const Str proc_type = BURROW_S("Proc-Type");
        const Str *pt = BURROW_MAP_GET(Str, Str, headers, proc_type);
        if (pt != NULL)
            PEM_TRY(pem_write_header(out, proc_type, *pt));
        const Str *last = NULL;
        for (;;) {
            const Str *next = NULL;
            const Str *next_val = NULL;
            for (MapIter it = map_iter(headers); map_next(&it, &kp, &vp);) {
                const Str *k = (const Str *)kp;
                if (str_eq(*k, proc_type))
                    continue;
                if (last != NULL && str_cmp(*k, *last) <= 0)
                    continue;
                if (next == NULL || str_cmp(*k, *next) < 0) {
                    next = k;
                    next_val = (const Str *)vp;
                }
            }
            if (next == NULL)
                break;
            PEM_TRY(pem_write_header(out, *next, *next_val));
            last = next;
        }
        PEM_TRY(pem_write(out, "\n", 1));
    }

    const Byte *p = (const Byte *)b->bytes.p;
    Int n = b->bytes.len;
    Byte line[PEM_LINE_LENGTH + 1];
    const Int chunk = (Int)PEM_LINE_LENGTH / 4 * 3;
    while (n > 0) {
        Int take = n < chunk ? n : chunk;
        Int w = base64_encoding_encoded_len(base64_std_encoding, take);
        base64_encoding_encode(base64_std_encoding, slice_from(line, w, w, TYPE_BYTE),
                               pem_bytes(p, take));
        line[w] = '\n';
        PEM_TRY(pem_write(out, line, w + 1));
        p += take;
        n -= take;
    }

    PEM_TRY(pem_write(out, PEM_END_LINE, PEM_END_LEN - 1));
    PEM_TRY(pem_write_str(out, b->type));
    return pem_write(out, "-----\n", 6);
}

Slice pem_encode_to_memory(Alloc *a, const PemBlock *b) {
    BytesBuffer buf = BYTES_BUFFER(a);
    if (BURROW_FAILED(pem_encode(bytes_buffer_as_io_writer(&buf), b))) {
        bytes_buffer_free(&buf);
        return slice_nil(TYPE_BYTE);
    }
    return buf.buf;
}

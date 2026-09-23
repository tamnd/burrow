/* What format.c and print.c share: the output buffer, the flags of the verb
 * being formatted, and the printer that holds both.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_FMT_FORMAT_H
#define BURROW_FMT_FORMAT_H

#include "burrow/fmt.h"

#include "burrow/core.h"
#include "burrow/iface.h"
#include "burrow/mem/heap.h"
#include "burrow/panic.h"

#include <string.h>

/* The output. It starts in storage the caller owns, usually on its stack, and
 * moves to the heap the first time it outgrows it. A failed allocation sets
 * failed and drops everything after it, so the printer never has to check. */
typedef struct FmtBuf {
    Byte *p;
    Int len;
    Int cap;
    bool heap;
    bool failed;
} FmtBuf;

void burrow__fmt_buf_grow(FmtBuf *b, Int n);
void burrow__fmt_buf_free(FmtBuf *b);

static inline void fmt_buf_write(FmtBuf *b, const Byte *p, Int n) {
    if (n <= 0)
        return;
    if (b->cap - b->len < n) {
        burrow__fmt_buf_grow(b, n);
        if (b->failed)
            return;
    }
    memcpy(b->p + b->len, p, (size_t)n);
    b->len += n;
}

static inline void fmt_buf_write_str(FmtBuf *b, Str s) {
    fmt_buf_write(b, s.p, s.len);
}

static inline void fmt_buf_write_byte(FmtBuf *b, Byte c) {
    if (b->len == b->cap) {
        burrow__fmt_buf_grow(b, 1);
        if (b->failed)
            return;
    }
    b->p[b->len++] = c;
}

void burrow__fmt_buf_write_rune(FmtBuf *b, Rune r);

/* Go's fmtFlags. plus_v and sharp_v are %+v and %#v, which are kept apart from
 * plus and sharp because they mean something else. */
typedef struct FmtFlags {
    bool wid_present;
    bool prec_present;
    bool minus;
    bool plus;
    bool sharp;
    bool space;
    bool zero;
    bool plus_v;
    bool sharp_v;
} FmtFlags;

/* Go's fmt struct, the part that knows how to lay out one value. */
typedef struct Fmt {
    FmtBuf *buf;
    FmtFlags f;
    Int wid;
    Int prec;
    Byte intbuf[68];
} Fmt;

void burrow__fmt_clearflags(Fmt *f);
void burrow__fmt_write_padding(Fmt *f, Int n);
void burrow__fmt_pad(Fmt *f, const Byte *b, Int n);
void burrow__fmt_pad_string(Fmt *f, Str s);
void burrow__fmt_boolean(Fmt *f, bool v);
void burrow__fmt_unicode(Fmt *f, uint64_t u);
void burrow__fmt_integer(Fmt *f, uint64_t u, Int base, bool is_signed, Rune verb,
                         const char *digits);
void burrow__fmt_s(Fmt *f, Str s);
void burrow__fmt_bs(Fmt *f, const Byte *b, Int n);
void burrow__fmt_sbx(Fmt *f, const Byte *s, Int n, const char *digits);
void burrow__fmt_q(Fmt *f, Str s);
void burrow__fmt_c(Fmt *f, uint64_t c);
void burrow__fmt_qc(Fmt *f, uint64_t c);
void burrow__fmt_float(Fmt *f, double v, Int size, Rune verb, Int prec);

#define FMT_LDIGITS "0123456789abcdefx"
#define FMT_UDIGITS "0123456789ABCDEFX"

#endif /* BURROW_FMT_FORMAT_H */

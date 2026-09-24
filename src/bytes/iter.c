/* Derived from Go's src/bytes/iter.go.
 * Go source: go1.27.1.
 *
 * The same closures as strings' iterators, over a Slice. Every piece they
 * yield has its capacity cut to its length, as Go's do, so a caller appending
 * to one cannot write over the next.
 *
 * Copyright 2024 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/bytes.h"

#include "burrow/core.h"
#include "burrow/func.h"
#include "burrow/iter.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/type.h"
#include "burrow/unicode.h"
#include "burrow/utf8.h"

/* What one of these closures captures. */
typedef struct BytesSeqState {
    Slice s;
    Slice sep;
    Int sep_save;
    RuneFunc f;
} BytesSeqState;

static const Byte bseq_ascii_space[256] = {
    ['\t'] = 1, ['\n'] = 1, ['\v'] = 1, ['\f'] = 1, ['\r'] = 1, [' '] = 1};

static BytesSeqState *bseq_state(Alloc *a, Slice s) {
    BytesSeqState *st = BURROW_NEW(a, BytesSeqState);
    if (st != NULL)
        st->s = s;
    return st;
}

static IterSeq bseq_of(void (*body)(void *, IterYield), BytesSeqState *st) {
    IterSeq seq = {NULL, NULL};
    if (st != NULL) {
        seq.f = body;
        seq.env = st;
    }
    return seq;
}

/* Go's s[lo:hi], and s[lo:hi:hi] when capped. */
static Slice bseq_sub(Slice s, Int lo, Int hi, bool capped) {
    Slice t = {lo == 0 ? s.p : (Byte *)s.p + lo, hi - lo, (capped ? hi : s.cap) - lo,
               TYPE_BYTE};
    return t;
}

static Rune bseq_decode(Slice s, Int i, Int *size) {
    Str rest = {(const Byte *)s.p + i, s.len - i};
    return utf8_decode_rune_in_string(rest, size);
}

static bool bseq_yield(IterYield yield, Slice v) {
    return BURROW_CALLF(yield, &v);
}

static void bytes_lines_run(void *env, IterYield yield) {
    BytesSeqState *st = (BytesSeqState *)env;
    while (st->s.len > 0) {
        Slice line;
        Int i = bytes_index_byte(st->s, '\n');
        if (i >= 0) {
            line = bseq_sub(st->s, 0, i + 1, false);
            st->s = bseq_sub(st->s, i + 1, st->s.len, false);
        } else {
            line = st->s;
            st->s = slice_nil(TYPE_BYTE);
        }
        if (!bseq_yield(yield, bseq_sub(line, 0, line.len, true)))
            return;
    }
}

IterSeq bytes_lines(Alloc *a, Slice s) {
    return bseq_of(bytes_lines_run, bseq_state(a, s));
}

static void bytes_split_run(void *env, IterYield yield) {
    BytesSeqState *st = (BytesSeqState *)env;
    if (st->sep.len == 0) {
        while (st->s.len > 0) {
            Int size;
            bseq_decode(st->s, 0, &size);
            if (!bseq_yield(yield, bseq_sub(st->s, 0, size, true)))
                return;
            st->s = bseq_sub(st->s, size, st->s.len, false);
        }
        return;
    }
    for (;;) {
        Int i = bytes_index(st->s, st->sep);
        if (i < 0)
            break;
        if (!bseq_yield(yield, bseq_sub(st->s, 0, i + st->sep_save, true)))
            return;
        st->s = bseq_sub(st->s, i + st->sep.len, st->s.len, false);
    }
    bseq_yield(yield, bseq_sub(st->s, 0, st->s.len, true));
}

static IterSeq bytes_split_seq_of(Alloc *a, Slice s, Slice sep, Int sep_save) {
    BytesSeqState *st = bseq_state(a, s);
    if (st != NULL) {
        st->sep = sep;
        st->sep_save = sep_save;
    }
    return bseq_of(bytes_split_run, st);
}

IterSeq bytes_split_seq(Alloc *a, Slice s, Slice sep) {
    return bytes_split_seq_of(a, s, sep, 0);
}

IterSeq bytes_split_after_seq(Alloc *a, Slice s, Slice sep) {
    return bytes_split_seq_of(a, s, sep, sep.len);
}

static void bytes_fields_run(void *env, IterYield yield) {
    const BytesSeqState *st = (const BytesSeqState *)env;
    Slice s = st->s;
    const Byte *p = (const Byte *)s.p;
    Int start = -1;
    for (Int i = 0; i < s.len;) {
        Int size = 1;
        Rune r = p[i];
        bool is_space = bseq_ascii_space[p[i]] != 0;
        if (r >= UTF8_RUNE_SELF) {
            r = bseq_decode(s, i, &size);
            is_space = unicode_is_space(r);
        }
        if (is_space) {
            if (start >= 0) {
                if (!bseq_yield(yield, bseq_sub(s, start, i, true)))
                    return;
                start = -1;
            }
        } else if (start < 0) {
            start = i;
        }
        i += size;
    }
    if (start >= 0)
        bseq_yield(yield, bseq_sub(s, start, s.len, true));
}

IterSeq bytes_fields_seq(Alloc *a, Slice s) {
    return bseq_of(bytes_fields_run, bseq_state(a, s));
}

static void bytes_fields_func_run(void *env, IterYield yield) {
    const BytesSeqState *st = (const BytesSeqState *)env;
    Slice s = st->s;
    Int start = -1;
    for (Int i = 0; i < s.len;) {
        Int size;
        Rune r = bseq_decode(s, i, &size);
        if (BURROW_CALLF(st->f, r)) {
            if (start >= 0) {
                if (!bseq_yield(yield, bseq_sub(s, start, i, true)))
                    return;
                start = -1;
            }
        } else if (start < 0) {
            start = i;
        }
        i += size;
    }
    if (start >= 0)
        bseq_yield(yield, bseq_sub(s, start, s.len, true));
}

IterSeq bytes_fields_func_seq(Alloc *a, Slice s, RuneFunc f) {
    BytesSeqState *st = bseq_state(a, s);
    if (st != NULL)
        st->f = f;
    return bseq_of(bytes_fields_func_run, st);
}

void bytes_seq_free(Alloc *a, IterSeq seq) {
    if (seq.env != NULL)
        mem_free(a, seq.env, sizeof(BytesSeqState), _Alignof(BytesSeqState));
}

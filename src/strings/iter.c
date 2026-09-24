/* Derived from Go's src/strings/iter.go.
 * Go source: go1.27.1.
 *
 * Go's closures capture s and, for Lines and the splits, cut it down as they
 * go, which is why those are single use. The state here is the same few
 * variables in a block from the allocator, cut down the same way.
 *
 * Copyright 2024 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/strings.h"

#include "burrow/core.h"
#include "burrow/func.h"
#include "burrow/iter.h"
#include "burrow/mem.h"
#include "burrow/unicode.h"
#include "burrow/utf8.h"

/* What one of these closures captures. */
typedef struct StringsSeqState {
    Str s;
    Str sep;
    Int sep_save;
    RuneFunc f;
} StringsSeqState;

static const Byte seq_ascii_space[256] = {
    ['\t'] = 1, ['\n'] = 1, ['\v'] = 1, ['\f'] = 1, ['\r'] = 1, [' '] = 1};

static StringsSeqState *seq_state(Alloc *a, Str s) {
    StringsSeqState *st = BURROW_NEW(a, StringsSeqState);
    if (st != NULL)
        st->s = s;
    return st;
}

static IterSeq seq_of(void (*body)(void *, IterYield), StringsSeqState *st) {
    IterSeq seq = {NULL, NULL};
    if (st != NULL) {
        seq.f = body;
        seq.env = st;
    }
    return seq;
}

static Str seq_sub(Str s, Int lo, Int hi) {
    Str t = {lo == 0 ? s.p : s.p + lo, hi - lo};
    return t;
}

static bool seq_yield(IterYield yield, Str v) {
    return BURROW_CALLF(yield, &v);
}

static void lines_run(void *env, IterYield yield) {
    StringsSeqState *st = (StringsSeqState *)env;
    while (st->s.len > 0) {
        Str line;
        Int i = strings_index_byte(st->s, '\n');
        if (i >= 0) {
            line = seq_sub(st->s, 0, i + 1);
            st->s = seq_sub(st->s, i + 1, st->s.len);
        } else {
            line = st->s;
            st->s = BURROW_STR_EMPTY;
        }
        if (!seq_yield(yield, line))
            return;
    }
}

IterSeq strings_lines(Alloc *a, Str s) {
    return seq_of(lines_run, seq_state(a, s));
}

static void split_run(void *env, IterYield yield) {
    StringsSeqState *st = (StringsSeqState *)env;
    if (st->sep.len == 0) {
        while (st->s.len > 0) {
            Int size;
            utf8_decode_rune_in_string(st->s, &size);
            if (!seq_yield(yield, seq_sub(st->s, 0, size)))
                return;
            st->s = seq_sub(st->s, size, st->s.len);
        }
        return;
    }
    for (;;) {
        Int i = strings_index(st->s, st->sep);
        if (i < 0)
            break;
        Str frag = seq_sub(st->s, 0, i + st->sep_save);
        if (!seq_yield(yield, frag))
            return;
        st->s = seq_sub(st->s, i + st->sep.len, st->s.len);
    }
    seq_yield(yield, st->s);
}

static IterSeq split_seq(Alloc *a, Str s, Str sep, Int sep_save) {
    StringsSeqState *st = seq_state(a, s);
    if (st != NULL) {
        st->sep = sep;
        st->sep_save = sep_save;
    }
    return seq_of(split_run, st);
}

IterSeq strings_split_seq(Alloc *a, Str s, Str sep) {
    return split_seq(a, s, sep, 0);
}

IterSeq strings_split_after_seq(Alloc *a, Str s, Str sep) {
    return split_seq(a, s, sep, sep.len);
}

static void fields_run(void *env, IterYield yield) {
    const StringsSeqState *st = (const StringsSeqState *)env;
    Str s = st->s;
    Int start = -1;
    for (Int i = 0; i < s.len;) {
        Int size = 1;
        Rune r = s.p[i];
        bool is_space = seq_ascii_space[s.p[i]] != 0;
        if (r >= UTF8_RUNE_SELF) {
            r = utf8_decode_rune_in_string(seq_sub(s, i, s.len), &size);
            is_space = unicode_is_space(r);
        }
        if (is_space) {
            if (start >= 0) {
                if (!seq_yield(yield, seq_sub(s, start, i)))
                    return;
                start = -1;
            }
        } else if (start < 0) {
            start = i;
        }
        i += size;
    }
    if (start >= 0)
        seq_yield(yield, seq_sub(s, start, s.len));
}

IterSeq strings_fields_seq(Alloc *a, Str s) {
    return seq_of(fields_run, seq_state(a, s));
}

static void fields_func_run(void *env, IterYield yield) {
    const StringsSeqState *st = (const StringsSeqState *)env;
    Str s = st->s;
    Int start = -1;
    for (Int i = 0; i < s.len;) {
        Int size;
        Rune r = utf8_decode_rune_in_string(seq_sub(s, i, s.len), &size);
        if (BURROW_CALLF(st->f, r)) {
            if (start >= 0) {
                if (!seq_yield(yield, seq_sub(s, start, i)))
                    return;
                start = -1;
            }
        } else if (start < 0) {
            start = i;
        }
        i += size;
    }
    if (start >= 0)
        seq_yield(yield, seq_sub(s, start, s.len));
}

IterSeq strings_fields_func_seq(Alloc *a, Str s, RuneFunc f) {
    StringsSeqState *st = seq_state(a, s);
    if (st != NULL)
        st->f = f;
    return seq_of(fields_func_run, st);
}

void strings_seq_free(Alloc *a, IterSeq seq) {
    if (seq.env != NULL)
        mem_free(a, seq.env, sizeof(StringsSeqState), _Alignof(StringsSeqState));
}

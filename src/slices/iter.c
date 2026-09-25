/* slices, derived from Go's src/slices/iter.go (go1.27.1).
 *
 * All, Backward and Values capture only the slice, so their sequences point
 * at the caller's Slice and allocate nothing. Chunk needs n as well, and takes
 * a small block from the allocator for the two.
 *
 * Copyright 2024 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/slices.h"

#include "burrow/core.h"
#include "burrow/func.h"
#include "burrow/iter.h"
#include "burrow/mem.h"
#include "burrow/panic.h"

#include <stdint.h>

static const void *slices_iter_at(const Slice *s, Int i) {
    return (const Byte *)s->p + (size_t)i * s->elem->size;
}

static void slices_all_run(void *env, IterYield2 yield) {
    const Slice *s = env;
    for (Int i = 0; i < s->len; i++) {
        if (!yield.f(yield.env, &i, slices_iter_at(s, i)))
            return;
    }
}

static void slices_backward_run(void *env, IterYield2 yield) {
    const Slice *s = env;
    for (Int i = s->len - 1; i >= 0; i--) {
        if (!yield.f(yield.env, &i, slices_iter_at(s, i)))
            return;
    }
}

static void slices_values_run(void *env, IterYield yield) {
    const Slice *s = env;
    for (Int i = 0; i < s->len; i++) {
        if (!yield.f(yield.env, slices_iter_at(s, i)))
            return;
    }
}

IterSeq2 slices_all(const Slice *s) {
    return BURROW_FN(IterSeq2, slices_all_run, (void *)(uintptr_t)s);
}

IterSeq2 slices_backward(const Slice *s) {
    return BURROW_FN(IterSeq2, slices_backward_run, (void *)(uintptr_t)s);
}

IterSeq slices_values(const Slice *s) {
    return BURROW_FN(IterSeq, slices_values_run, (void *)(uintptr_t)s);
}

/* What AppendSeq's loop body captures. */
typedef struct SlicesAppend {
    Alloc *a;
    Slice s;
} SlicesAppend;

static bool slices_append_yield(void *env, const void *v) {
    SlicesAppend *st = env;
    st->s = slice_append(st->a, st->s, v, 1);
    return true;
}

Slice slices_append_seq(Alloc *a, Slice s, IterSeq seq) {
    SlicesAppend st = {a, s};
    seq.f(seq.env, BURROW_FN(IterYield, slices_append_yield, &st));
    return st.s;
}

Slice slices_collect(Alloc *a, const Type *elem, IterSeq seq) {
    return slices_append_seq(a, slice_nil(elem), seq);
}

Slice slices_sorted(Alloc *a, const Type *elem, IterSeq seq) {
    Slice s = slices_collect(a, elem, seq);
    slices_sort(s);
    return s;
}

Slice slices_sorted_func(Alloc *a, const Type *elem, IterSeq seq, SlicesCmpFunc cmp) {
    Slice s = slices_collect(a, elem, seq);
    slices_sort_func(s, cmp);
    return s;
}

Slice slices_sorted_stable_func(Alloc *a, const Type *elem, IterSeq seq,
                                SlicesCmpFunc cmp) {
    Slice s = slices_collect(a, elem, seq);
    slices_sort_stable_func(s, cmp);
    return s;
}

/* What Chunk's closure captures. */
typedef struct SlicesChunk {
    Slice s;
    Int n;
} SlicesChunk;

static void slices_chunk_run(void *env, IterYield yield) {
    const SlicesChunk *st = env;
    Slice s = st->s;
    for (Int i = 0; i < s.len; i += st->n) {
        /* Clamp the last chunk to the slice bound as necessary, and set the
         * capacity of each chunk so that appending to a chunk does not
         * modify the original slice. */
        Int end = s.len - i < st->n ? s.len - i : st->n;
        Slice c = slice_sub3(s, i, i + end, i + end);
        if (!yield.f(yield.env, &c))
            return;
    }
}

IterSeq slices_chunk(Alloc *a, Slice s, Int n) {
    if (n < 1)
        panic_str(BURROW_S("cannot be less than 1"));
    IterSeq seq = {NULL, NULL};
    SlicesChunk *st = BURROW_NEW(a, SlicesChunk);
    if (st != NULL) {
        st->s = s;
        st->n = n;
        seq = BURROW_FN(IterSeq, slices_chunk_run, st);
    }
    return seq;
}

void slices_seq_free(Alloc *a, IterSeq seq) {
    if (seq.env != NULL)
        mem_free(a, seq.env, sizeof(SlicesChunk), _Alignof(SlicesChunk));
}

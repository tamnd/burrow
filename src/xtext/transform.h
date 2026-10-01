/* golang.org/x/text/transform, the copy Go vendors.
 *
 * Go's standard library uses a few packages from golang.org/x/text without
 * exporting them: the IDNA code under net/http needs Unicode normalisation and
 * the bidi rule, and those are written in terms of this package's Transformer.
 * So these are burrow's internals in the same way, under src/xtext, with names
 * that start with burrow__ and nothing in include/.
 *
 * A Transformer is the interface below. Transform writes the transformed bytes
 * of src into dst and returns how many it wrote, with how many of src it read in
 * *n_src. The errors are Go's: burrow__transform_err_short_dst when dst is full,
 * burrow__transform_err_short_src when src ends in the middle of something and
 * at_eof is false. Reset forgets any state between inputs.
 *
 * Copyright 2013 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package xtext/transform */

#ifndef BURROW_SRC_XTEXT_TRANSFORM_H
#define BURROW_SRC_XTEXT_TRANSFORM_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/func.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/type.h"

/* ErrShortDst, ErrShortSrc and ErrEndOfSpan, and the two Go keeps to itself,
 * which its tests compare against. */
extern const Error burrow__transform_err_short_dst;
extern const Error burrow__transform_err_short_src;
extern const Error burrow__transform_err_end_of_span;
extern const Error burrow__transform_err_inconsistent_byte_count;
extern const Error burrow__transform_err_short_internal;

/* Go's err == target, for the sentinels above and io_eof. */
static inline bool burrow__transform_err_eq(Error a, Error b) {
    return a.vt == b.vt && a.data == b.data;
}

typedef struct TransformTransformerVT {
    const Type *self_type;
    Int (*transform)(void *self, Slice dst, Slice src, bool at_eof, Int *n_src,
                     Error *err);
    void (*reset)(void *self);
} TransformTransformerVT;

typedef struct TransformTransformer {
    const TransformTransformerVT *vt;
    void *data;
} TransformTransformer;

/* A Transformer that can also say how much of src it would leave alone. */
typedef struct TransformSpanningTransformerVT {
    TransformTransformerVT transformer;
    Int (*span)(void *self, Slice src, bool at_eof, Error *err);
} TransformSpanningTransformerVT;

typedef struct TransformSpanningTransformer {
    const TransformSpanningTransformerVT *vt;
    void *data;
} TransformSpanningTransformer;

/* The Transformer half of a SpanningTransformer. */
static inline TransformTransformer
burrow__transform_of(TransformSpanningTransformer t) {
    TransformTransformer out = {&t.vt->transformer, t.data};
    return out;
}

/* Calls, so the n_src out parameter can be left NULL. */
Int burrow__transform_call(TransformTransformer t, Slice dst, Slice src, bool at_eof,
                           Int *n_src, Error *err);
Int burrow__transform_span(TransformSpanningTransformer t, Slice src, bool at_eof,
                           Error *err);

/* Nop copies src to dst and Discard reads everything and writes nothing. */
BURROW_STATIC(ret) TransformSpanningTransformer burrow__transform_nop(void);
BURROW_STATIC(ret) TransformTransformer burrow__transform_discard(void);

/* Chain applies t[0] to t[n-1] in turn, through buffers of 4096 bytes between
 * them. The result is allocated from a and so are the buffers, and
 * burrow__transform_chain_free gives them back. NULL from a gives a nil
 * Transformer. */
BURROW_OWNS(ret) TransformTransformer
burrow__transform_chain(Alloc *a, const TransformTransformer *t, Int n);
void burrow__transform_chain_free(Alloc *a, TransformTransformer t);

/* For tests/xtext_transform_test.c, which sizes the buffers between links the
 * way Go's mkChain does: link j's buffer becomes b, for j from 1 to n-1. */
void burrow__transform_chain_set_buf(TransformTransformer t, Int j, Slice b);

/* RemoveFunc, which Go has deprecated in favour of runes.Remove and still has.
 * It drops every rune f says yes to, and treats a byte that is not UTF-8 as
 * U+FFFD. */
BURROW_OWNS(ret) TransformTransformer burrow__transform_remove_func(Alloc *a,
                                                                    RuneFunc f);
void burrow__transform_remove_func_free(Alloc *a, TransformTransformer t);

/* String, Bytes and Append. Each calls Reset on t first and says how much of
 * the input it converted in *n, which is all of it when *err is nil.
 *
 * String hands back s itself when the transformation leaves it alone, so the
 * result borrows from s or is a fresh copy from a. Bytes always allocates.
 * Append writes into dst's spare capacity when it is enough, the way Go's append
 * does, and otherwise into a new array from a, leaving dst alone. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, s) Str
burrow__transform_string(Alloc *a, TransformTransformer t, Str s, Int *n, Error *err);
BURROW_OWNS(ret) Slice burrow__transform_bytes(Alloc *a, TransformTransformer t,
                                               Slice b, Int *n, Error *err);
BURROW_OWNS(ret) BURROW_BORROWS(ret, dst) Slice burrow__transform_append(
    Alloc *a, TransformTransformer t, Slice dst, Slice src, Int *n, Error *err);

/* Reader wraps an io.Reader and transforms what is read through it. */
typedef struct TransformReader {
    IoReader r;
    TransformTransformer t;
    Error err;

    /* dst[dst0:dst1] has been transformed and not yet read. The buffers are
     * dst_len and src_len bytes long, which is 4096 unless a test says
     * otherwise, as Go's do by swapping the slices. */
    Byte *dst;
    Int dst0, dst1;

    /* src[src0:src1] has been read and not yet transformed. */
    Byte *src;
    Int src0, src1;

    Int dst_len, src_len;

    /* Whether the transformation is over, whether or not it worked. */
    bool transform_complete;
} TransformReader;

/* NewReader. It calls Reset on t. NULL when a has no memory. */
BURROW_OWNS(ret) TransformReader *burrow__transform_new_reader(Alloc *a, IoReader r,
                                                               TransformTransformer t);
void burrow__transform_reader_free(Alloc *a, TransformReader *r);
Int burrow__transform_reader_read(TransformReader *r, Slice p, Error *err);
BURROW_BORROWS(ret, r) IoReader
burrow__transform_reader_as_io_reader(TransformReader *r);

/* Writer wraps an io.Writer. It holds back what it cannot transform yet, so
 * Close has to be called to flush the end. */
typedef struct TransformWriter {
    IoWriter w;
    TransformTransformer t;
    Byte *dst;

    /* src[:n] has not been through t yet. */
    Byte *src;
    Int n;

    /* The lengths of dst and src, as with the Reader. */
    Int dst_len, src_len;
} TransformWriter;

BURROW_OWNS(ret) TransformWriter *burrow__transform_new_writer(Alloc *a, IoWriter w,
                                                               TransformTransformer t);
void burrow__transform_writer_free(Alloc *a, TransformWriter *w);
Int burrow__transform_writer_write(TransformWriter *w, Slice data, Error *err);
Error burrow__transform_writer_close(TransformWriter *w);
BURROW_BORROWS(ret, w) IoWriteCloser
burrow__transform_writer_as_io_write_closer(TransformWriter *w);

#endif /* BURROW_SRC_XTEXT_TRANSFORM_H */

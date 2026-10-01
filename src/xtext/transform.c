/* Derived from Go's src/vendor/golang.org/x/text/transform/transform.go.
 * Go source: go1.27.1, golang.org/x/text v0.37.0.
 *
 * Copyright 2013 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "transform.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/func.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/type.h"
#include "burrow/utf8.h"

#include <string.h>

BURROW_SENTINEL_ERROR(burrow__transform_err_short_dst,
                      "transform: short destination buffer");
BURROW_SENTINEL_ERROR(burrow__transform_err_short_src,
                      "transform: short source buffer");
BURROW_SENTINEL_ERROR(burrow__transform_err_end_of_span,
                      "transform: input and output are not identical");
BURROW_SENTINEL_ERROR(burrow__transform_err_inconsistent_byte_count,
                      "transform: inconsistent byte count returned");
BURROW_SENTINEL_ERROR(burrow__transform_err_short_internal,
                      "transform: short internal buffer");

#define err_eq burrow__transform_err_eq

enum {
    TRANSFORM_DEFAULT_BUF_SIZE = 4096,
    TRANSFORM_INITIAL_BUF_SIZE = 128,
};

static Slice transform_bytes_of(Byte *p, Int len, Int cap) {
    Slice s = {p, len, cap, TYPE_BYTE};
    return s;
}

/* b[lo:hi], with Go's bounds check, since the arithmetic in here is Go's and a
 * mistake in it should say so rather than read past the end. */
static Slice transform_sub(Slice b, Int lo, Int hi) {
    return slice_sub(b, lo, hi);
}

static Slice transform_tail(Slice b, Int lo) {
    return slice_sub(b, lo, b.len);
}

static Int transform_copy_bytes(Slice dst, Slice src) {
    Int n = dst.len < src.len ? dst.len : src.len;
    /* A slice with a length always has an array; the checks are for the
     * analyser. */
    if (n > 0 && dst.p != NULL && src.p != NULL)
        memmove(dst.p, src.p, (size_t)n);
    return n;
}

Int burrow__transform_call(TransformTransformer t, Slice dst, Slice src, bool at_eof,
                           Int *n_src, Error *err) {
    Int ns = 0;
    Error e = BURROW_NO_ERROR;
    Int nd = t.vt->transform(t.data, dst, src, at_eof, &ns, &e);
    if (n_src != NULL)
        *n_src = ns;
    if (err != NULL)
        *err = e;
    return nd;
}

Int burrow__transform_span(TransformSpanningTransformer t, Slice src, bool at_eof,
                           Error *err) {
    Error e = BURROW_NO_ERROR;
    Int n = t.vt->span(t.data, src, at_eof, &e);
    if (err != NULL)
        *err = e;
    return n;
}

static void reset(TransformTransformer t) {
    t.vt->reset(t.data);
}

/* -------------------------------------------------------------- nop, discard */

static const Type nop_desc = {
    {(const Byte *)"nop", 3},
    {(const Byte *)"golang.org/x/text/transform", 27},
    KIND_STRUCT,
    0,
    1,
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

static const Type transform_discard_desc = {
    {(const Byte *)"discard", 7},
    {(const Byte *)"golang.org/x/text/transform", 27},
    KIND_STRUCT,
    0,
    1,
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

static void nop_reset(void *self) {
    (void)self;
}

static Int nop_transform(void *self, Slice dst, Slice src, bool at_eof, Int *n_src,
                         Error *err) {
    (void)self;
    (void)at_eof;
    Int n = transform_copy_bytes(dst, src);
    if (n < src.len)
        *err = burrow__transform_err_short_dst;
    *n_src = n;
    return n;
}

static Int nop_span(void *self, Slice src, bool at_eof, Error *err) {
    (void)self;
    (void)at_eof;
    (void)err;
    return src.len;
}

static const TransformSpanningTransformerVT nop_vt = {
    {&nop_desc, nop_transform, nop_reset}, nop_span};

TransformSpanningTransformer burrow__transform_nop(void) {
    TransformSpanningTransformer t = {&nop_vt, NULL};
    return t;
}

static Int transform_discard_transform(void *self, Slice dst, Slice src, bool at_eof,
                                       Int *n_src, Error *err) {
    (void)self;
    (void)dst;
    (void)at_eof;
    (void)err;
    *n_src = src.len;
    return 0;
}

static const TransformTransformerVT transform_discard_vt = {
    &transform_discard_desc, transform_discard_transform, nop_reset};

TransformTransformer burrow__transform_discard(void) {
    TransformTransformer t = {&transform_discard_vt, NULL};
    return t;
}

/* -------------------------------------------------------------------- chain */

/* A chain with N Transformers has N+1 links and N+1 buffers. The first and
 * last are the src and dst given to Transform and the N-1 between them belong
 * to the chain. Link i transforms link[i].b[p:n] into link[i+1].b at
 * link[i+1].n. */
typedef struct ChainLink {
    TransformTransformer t;
    Slice b;
    Int p;
    Int n;
} ChainLink;

typedef struct TransformChain {
    ChainLink *link;
    Int nlink;
    Error err;
    /* The index the error happened at plus one. While it is above zero the
     * chain takes no more source bytes. */
    Int err_start;
} TransformChain;

static const Type chain_desc = {
    {(const Byte *)"chain", 5},
    {(const Byte *)"golang.org/x/text/transform", 27},
    KIND_STRUCT,
    (uint32_t)sizeof(TransformChain),
    (uint16_t)_Alignof(TransformChain),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

static void chain_fatal_error(TransformChain *c, Int err_index, Error err) {
    Int i = err_index + 1;
    if (i > c->err_start) {
        c->err_start = i;
        c->err = err;
    }
}

static Slice link_src(const ChainLink *l) {
    return transform_sub(l->b, l->p, l->n);
}

static Slice link_dst(const ChainLink *l) {
    return transform_tail(l->b, l->n);
}

static void chain_reset(void *self) {
    TransformChain *c = (TransformChain *)self;
    for (Int i = 0; i < c->nlink; i++) {
        if (c->link[i].t.vt != NULL)
            reset(c->link[i].t);
        c->link[i].p = 0;
        c->link[i].n = 0;
    }
}

static Int chain_transform(void *self, Slice dst, Slice src, bool at_eof, Int *n_src,
                           Error *err) {
    TransformChain *c = (TransformChain *)self;
    Error ret_err = BURROW_NO_ERROR;

    /* Set up src and dst in the chain. */
    ChainLink *src_l = &c->link[0];
    ChainLink *dst_l = &c->link[c->nlink - 1];
    src_l->b = src;
    src_l->p = 0;
    src_l->n = src.len;
    dst_l->b = dst;
    dst_l->n = 0;
    bool last_full = false, need_progress = false; /* for detecting progress */

    /* i is the index of the next Transformer to apply, for i in [low, high].
     * low is the lowest index for which c.link[low] may still produce bytes.
     * high is the highest index for which c.link[high] has a Transformer. The
     * error returned by Transform determines whether to increase or decrease
     * i. We try to completely fill a buffer before converting it. */
    for (Int low = c->err_start, i = c->err_start, high = c->nlink - 2;
         low <= i && i <= high;) {
        ChainLink *in = &c->link[i], *out = &c->link[i + 1];
        Int n_src0 = 0;
        Error err0 = BURROW_NO_ERROR;
        Int n_dst0 = in->t.vt->transform(in->t.data, link_dst(out), link_src(in),
                                         at_eof && low == i, &n_src0, &err0);
        out->n += n_dst0;
        in->p += n_src0;
        if (i > 0 && in->p == in->n) {
            in->p = 0;
            in->n = 0;
        }
        need_progress = last_full;
        last_full = false;
        bool advance = false; /* the code after Go's switch */
        if (err_eq(err0, burrow__transform_err_short_dst)) {
            /* Process the destination buffer next. Return if we are already
             * at the high index. */
            if (i == high) {
                *n_src = src_l->p;
                *err = burrow__transform_err_short_dst;
                return dst_l->n;
            }
            if (out->n != 0) {
                i++;
                /* If the Transformer at the next index is not able to process
                 * any source bytes there is nothing that can be done to make
                 * progress and the bytes will remain unprocessed. last_full is
                 * used to detect this and break out of the loop with a fatal
                 * error. */
                last_full = true;
                continue;
            }
            /* The destination buffer was too small, but is completely empty.
             * Return a fatal error as this transformation can never
             * complete. */
            chain_fatal_error(c, i, burrow__transform_err_short_internal);
            advance = true;
        } else if (err_eq(err0, burrow__transform_err_short_src) || BURROW_OK(err0)) {
            bool fall = BURROW_OK(err0);
            if (!fall) {
                if (i == 0) {
                    /* Save ErrShortSrc in err. All other errors take
                     * precedence. */
                    ret_err = burrow__transform_err_short_src;
                    advance = true;
                } else if ((need_progress && n_src0 == 0) ||
                           in->n - in->p == in->b.len) {
                    /* There were not enough source bytes to proceed while the
                     * source buffer cannot hold any more bytes. Return a fatal
                     * error as this transformation can never complete. */
                    chain_fatal_error(c, i, burrow__transform_err_short_internal);
                    advance = true;
                } else {
                    /* in.b is an internal buffer and we can make progress. */
                    Int m = transform_copy_bytes(in->b, link_src(in));
                    in->p = 0;
                    in->n = m;
                    fall = true;
                }
            }
            if (fall) {
                /* if i == low, we have depleted the bytes at index i or any
                 * lower levels. In that case we increase low and i. In all
                 * other cases we decrease i to fetch more bytes before
                 * proceeding to the next index. */
                if (i > low) {
                    i--;
                    continue;
                }
                advance = true;
            }
        } else {
            chain_fatal_error(c, i, err0);
            advance = true;
        }
        (void)advance;
        /* Exhausted level low or fatal error: increase low and continue to
         * process the bytes accepted so far. */
        i++;
        low = i;
    }

    /* If c.errStart > 0, this means we found a fatal error. We will clear all
     * upstream buffers. At this point, no more progress can be made
     * downstream, as Transform would have bailed while handling
     * ErrShortDst. */
    if (c->err_start > 0) {
        for (Int i = 1; i < c->err_start; i++) {
            c->link[i].p = 0;
            c->link[i].n = 0;
        }
        ret_err = c->err;
        c->err_start = 0;
        c->err = BURROW_NO_ERROR;
    }
    *n_src = src_l->p;
    *err = ret_err;
    return dst_l->n;
}

static const TransformTransformerVT chain_vt = {&chain_desc, chain_transform,
                                                chain_reset};

TransformTransformer burrow__transform_chain(Alloc *a, const TransformTransformer *t,
                                             Int n) {
    TransformTransformer nil = {NULL, NULL};
    if (n == 0) {
        TransformTransformer out = {&nop_vt.transformer, NULL};
        return out;
    }
    Int nbuf = n - 1;
    size_t size = sizeof(TransformChain) + (size_t)(n + 1) * sizeof(ChainLink) +
                  (size_t)nbuf * TRANSFORM_DEFAULT_BUF_SIZE;
    Byte *mem = (Byte *)mem_alloc(a, size, _Alignof(TransformChain));
    if (mem == NULL)
        return nil;
    TransformChain *c = (TransformChain *)(void *)mem;
    c->link = (ChainLink *)(void *)(mem + sizeof(TransformChain));
    c->nlink = n + 1;
    for (Int i = 0; i < n; i++)
        c->link[i].t = t[i];
    /* Allocate intermediate buffers. */
    Byte *bufs = mem + sizeof(TransformChain) + (size_t)(n + 1) * sizeof(ChainLink);
    for (Int i = 0; i < nbuf; i++)
        c->link[i + 1].b =
            transform_bytes_of(bufs + (size_t)i * TRANSFORM_DEFAULT_BUF_SIZE,
                               TRANSFORM_DEFAULT_BUF_SIZE, TRANSFORM_DEFAULT_BUF_SIZE);
    TransformTransformer out = {&chain_vt, c};
    return out;
}

void burrow__transform_chain_free(Alloc *a, TransformTransformer t) {
    if (t.vt != &chain_vt)
        return;
    TransformChain *c = (TransformChain *)t.data;
    Int n = c->nlink - 1;
    size_t size = sizeof(TransformChain) + (size_t)(n + 1) * sizeof(ChainLink) +
                  (size_t)(n - 1) * TRANSFORM_DEFAULT_BUF_SIZE;
    mem_free(a, c, size, _Alignof(TransformChain));
}

void burrow__transform_chain_set_buf(TransformTransformer t, Int j, Slice b) {
    TransformChain *c = (TransformChain *)t.data;
    c->link[j].b = b;
}

/* ------------------------------------------------------------------ removeF */

typedef struct RemoveF {
    RuneFunc f;
} RemoveF;

static const Type remove_f_desc = {
    {(const Byte *)"removeF", 7},
    {(const Byte *)"golang.org/x/text/transform", 27},
    KIND_FUNC,
    (uint32_t)sizeof(RemoveF),
    (uint16_t)_Alignof(RemoveF),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

static Int remove_f_transform(void *self, Slice dst, Slice src, bool at_eof, Int *n_src,
                              Error *err) {
    RuneFunc t = ((RemoveF *)self)->f;
    Int n_dst = 0, ns = 0;
    const Byte *s = (const Byte *)src.p;
    Int len = src.len;
    for (Int sz = 0; len > 0; s += sz, len -= sz) {
        Rune r = s[0];
        if (r < 0x80) {
            sz = 1;
        } else {
            Slice rest = {(void *)(uintptr_t)s, len, len, TYPE_BYTE};
            r = utf8_decode_rune(rest, &sz);
            if (sz == 1) {
                /* Invalid rune. */
                if (!at_eof && !utf8_full_rune(rest)) {
                    *err = burrow__transform_err_short_src;
                    break;
                }
                /* We replace illegal bytes with RuneError. Not doing so might
                 * otherwise turn a sequence of invalid UTF-8 into valid UTF-8.
                 * The resulting byte sequence may subsequently contain runes
                 * for which t(r) is true that were passed unnoticed. */
                if (!BURROW_CALLF(t, r)) {
                    if (n_dst + 3 > dst.len) {
                        *err = burrow__transform_err_short_dst;
                        break;
                    }
                    static const Byte rune_error[3] = {0xEF, 0xBF, 0xBD};
                    memcpy((Byte *)dst.p + n_dst, rune_error, 3);
                    n_dst += 3;
                }
                ns++;
                continue;
            }
        }

        if (!BURROW_CALLF(t, r)) {
            if (n_dst + sz > dst.len) {
                *err = burrow__transform_err_short_dst;
                break;
            }
            memcpy((Byte *)dst.p + n_dst, s, (size_t)sz);
            n_dst += sz;
        }
        ns += sz;
    }
    *n_src = ns;
    return n_dst;
}

static const TransformTransformerVT remove_f_vt = {&remove_f_desc, remove_f_transform,
                                                   nop_reset};

TransformTransformer burrow__transform_remove_func(Alloc *a, RuneFunc f) {
    TransformTransformer out = {NULL, NULL};
    RemoveF *r = (RemoveF *)mem_alloc(a, sizeof *r, _Alignof(RemoveF));
    if (r == NULL)
        return out;
    r->f = f;
    out.vt = &remove_f_vt;
    out.data = r;
    return out;
}

void burrow__transform_remove_func_free(Alloc *a, TransformTransformer t) {
    if (t.vt == &remove_f_vt)
        mem_free(a, t.data, sizeof(RemoveF), _Alignof(RemoveF));
}

/* ------------------------------------------------------------------- Reader */

static const Type transform_reader_desc = {
    {(const Byte *)"Reader", 6},
    {(const Byte *)"golang.org/x/text/transform", 27},
    KIND_STRUCT,
    (uint32_t)sizeof(TransformReader),
    (uint16_t)_Alignof(TransformReader),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

TransformReader *burrow__transform_new_reader(Alloc *a, IoReader r,
                                              TransformTransformer t) {
    reset(t);
    size_t size = sizeof(TransformReader) + (size_t)2 * TRANSFORM_DEFAULT_BUF_SIZE;
    Byte *mem = (Byte *)mem_alloc(a, size, _Alignof(TransformReader));
    if (mem == NULL)
        return NULL;
    TransformReader *tr = (TransformReader *)(void *)mem;
    tr->r = r;
    tr->t = t;
    tr->dst = mem + sizeof(TransformReader);
    tr->src = tr->dst + TRANSFORM_DEFAULT_BUF_SIZE;
    tr->dst_len = TRANSFORM_DEFAULT_BUF_SIZE;
    tr->src_len = TRANSFORM_DEFAULT_BUF_SIZE;
    return tr;
}

void burrow__transform_reader_free(Alloc *a, TransformReader *r) {
    if (r != NULL)
        mem_free(a, r, sizeof(TransformReader) + (size_t)2 * TRANSFORM_DEFAULT_BUF_SIZE,
                 _Alignof(TransformReader));
}

Int burrow__transform_reader_read(TransformReader *r, Slice p, Error *err) {
    Slice dst = transform_bytes_of(r->dst, r->dst_len, r->dst_len);
    Slice src = transform_bytes_of(r->src, r->src_len, r->src_len);
    Int n = 0;
    for (;;) {
        /* Copy out any transformed bytes and return the final error if we are
         * done. */
        if (r->dst0 != r->dst1) {
            n = transform_copy_bytes(p, transform_sub(dst, r->dst0, r->dst1));
            r->dst0 += n;
            if (r->dst0 == r->dst1 && r->transform_complete) {
                *err = r->err;
                return n;
            }
            *err = BURROW_NO_ERROR;
            return n;
        }
        if (r->transform_complete) {
            *err = r->err;
            return 0;
        }

        /* Try to transform some source bytes, or to flush the transformer if
         * we are out of source bytes. We do this even if r.r.Read returned an
         * error. As the io.Reader documentation says, "process the n > 0 bytes
         * returned before considering the error". */
        if (r->src0 != r->src1 || BURROW_FAILED(r->err)) {
            Error e = BURROW_NO_ERROR;
            r->dst0 = 0;
            r->dst1 =
                r->t.vt->transform(r->t.data, dst, transform_sub(src, r->src0, r->src1),
                                   err_eq(r->err, io_eof), &n, &e);
            r->src0 += n;

            if (BURROW_OK(e)) {
                if (r->src0 != r->src1)
                    r->err = burrow__transform_err_inconsistent_byte_count;
                /* The Transform call was successful; we are complete if we
                 * cannot read more bytes into src. */
                r->transform_complete = BURROW_FAILED(r->err);
                continue;
            }
            if (err_eq(e, burrow__transform_err_short_dst) &&
                (r->dst1 != 0 || n != 0)) {
                /* Make room in dst by copying out, and try again. */
                continue;
            }
            /* On a short src with room left and no reader error, read more
             * bytes into src via the code below, and try again. */
            if (!(err_eq(e, burrow__transform_err_short_src) &&
                  r->src1 - r->src0 != src.len && BURROW_OK(r->err))) {
                r->transform_complete = true;
                /* The reader error (r.err) takes precedence over the
                 * transformer error (err) unless r.err is nil or io.EOF. */
                if (BURROW_OK(r->err) || err_eq(r->err, io_eof))
                    r->err = e;
                continue;
            }
        }

        /* Move any untransformed source bytes to the start of the buffer and
         * read more bytes. */
        if (r->src0 != 0) {
            r->src1 = transform_copy_bytes(src, transform_sub(src, r->src0, r->src1));
            r->src0 = 0;
        }
        Error re = BURROW_NO_ERROR;
        n = BURROW_CALL(r->r, read, transform_tail(src, r->src1), &re);
        r->err = re;
        r->src1 += n;
    }
}

static Int transform_reader_io_read(void *self, Slice p, Error *err) {
    return burrow__transform_reader_read((TransformReader *)self, p, err);
}

static const IoReaderVT transform_reader_vt = {&transform_reader_desc,
                                               transform_reader_io_read};

IoReader burrow__transform_reader_as_io_reader(TransformReader *r) {
    IoReader out = {&transform_reader_vt, r};
    return out;
}

/* ------------------------------------------------------------------- Writer */

static const Type transform_writer_desc = {
    {(const Byte *)"Writer", 6},
    {(const Byte *)"golang.org/x/text/transform", 27},
    KIND_STRUCT,
    (uint32_t)sizeof(TransformWriter),
    (uint16_t)_Alignof(TransformWriter),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

TransformWriter *burrow__transform_new_writer(Alloc *a, IoWriter w,
                                              TransformTransformer t) {
    reset(t);
    size_t size = sizeof(TransformWriter) + (size_t)2 * TRANSFORM_DEFAULT_BUF_SIZE;
    Byte *mem = (Byte *)mem_alloc(a, size, _Alignof(TransformWriter));
    if (mem == NULL)
        return NULL;
    TransformWriter *tw = (TransformWriter *)(void *)mem;
    tw->w = w;
    tw->t = t;
    tw->dst = mem + sizeof(TransformWriter);
    tw->src = tw->dst + TRANSFORM_DEFAULT_BUF_SIZE;
    tw->dst_len = TRANSFORM_DEFAULT_BUF_SIZE;
    tw->src_len = TRANSFORM_DEFAULT_BUF_SIZE;
    return tw;
}

void burrow__transform_writer_free(Alloc *a, TransformWriter *w) {
    if (w != NULL)
        mem_free(a, w, sizeof(TransformWriter) + (size_t)2 * TRANSFORM_DEFAULT_BUF_SIZE,
                 _Alignof(TransformWriter));
}

static Error write_all(IoWriter w, Slice b) {
    Error e = BURROW_NO_ERROR;
    BURROW_CALL(w, write, b, &e);
    return e;
}

Int burrow__transform_writer_write(TransformWriter *w, Slice data, Error *err) {
    Slice wdst = transform_bytes_of(w->dst, w->dst_len, w->dst_len);
    Slice wsrc = transform_bytes_of(w->src, w->src_len, w->src_len);
    Int n = 0;
    Slice src = data;
    if (w->n > 0) {
        /* Append bytes from data to the last remainder. */
        n = transform_copy_bytes(transform_tail(wsrc, w->n), data);
        w->n += n;
        src = transform_sub(wsrc, 0, w->n);
    }
    for (;;) {
        Int n_src = 0;
        Error e = BURROW_NO_ERROR;
        Int n_dst = w->t.vt->transform(w->t.data, wdst, src, false, &n_src, &e);
        Error werr = write_all(w->w, transform_sub(wdst, 0, n_dst));
        if (BURROW_FAILED(werr)) {
            *err = werr;
            return n;
        }
        src = transform_tail(src, n_src);
        if (w->n == 0) {
            n += n_src;
        } else if (src.len <= n) {
            /* Enough bytes from w.src have been consumed. We make src point
             * to data instead to reduce the copying. */
            w->n = 0;
            n -= src.len;
            src = transform_tail(data, n);
            if (n < data.len &&
                (BURROW_OK(e) || err_eq(e, burrow__transform_err_short_src)))
                continue;
        }
        if (err_eq(e, burrow__transform_err_short_dst)) {
            /* This error is okay as long as we are making progress. */
            if (n_dst > 0 || n_src > 0)
                continue;
        } else if (err_eq(e, burrow__transform_err_short_src)) {
            if (src.len < wsrc.len) {
                Int m = transform_copy_bytes(wsrc, src);
                /* If w.n > 0, bytes from data were already copied to w.src
                 * and n was already set to the number of bytes consumed. */
                if (w->n == 0)
                    n += m;
                w->n = m;
                e = BURROW_NO_ERROR;
            } else if (n_dst > 0 || n_src > 0) {
                /* Not enough buffer to store the remainder. Keep processing
                 * as long as there is progress. Without this case, transforms
                 * that require a lookahead larger than the buffer may result
                 * in an error. This is not something one may expect to be
                 * common in practice, but it may occur when buffers are set to
                 * small sizes during testing. */
                continue;
            }
        } else if (BURROW_OK(e)) {
            if (w->n > 0)
                e = burrow__transform_err_inconsistent_byte_count;
        }
        *err = e;
        return n;
    }
}

Error burrow__transform_writer_close(TransformWriter *w) {
    Slice wdst = transform_bytes_of(w->dst, w->dst_len, w->dst_len);
    Slice src = transform_bytes_of(w->src, w->n, w->src_len);
    for (;;) {
        Int n_src = 0;
        Error e = BURROW_NO_ERROR;
        Int n_dst = w->t.vt->transform(w->t.data, wdst, src, true, &n_src, &e);
        Error werr = write_all(w->w, transform_sub(wdst, 0, n_dst));
        if (BURROW_FAILED(werr))
            return werr;
        if (!err_eq(e, burrow__transform_err_short_dst))
            return e;
        src = transform_tail(src, n_src);
    }
}

static Int transform_writer_io_write(void *self, Slice p, Error *err) {
    return burrow__transform_writer_write((TransformWriter *)self, p, err);
}

static Error transform_writer_io_close(void *self) {
    return burrow__transform_writer_close((TransformWriter *)self);
}

static const IoWriteCloserVT transform_writer_vt = {
    {&transform_writer_desc, transform_writer_io_write},
    {&transform_writer_desc, transform_writer_io_close}};

IoWriteCloser burrow__transform_writer_as_io_write_closer(TransformWriter *w) {
    IoWriteCloser out = {&transform_writer_vt, w};
    return out;
}

/* ------------------------------------------------------ String, Bytes, Append */

/* grow returns a new buffer that is longer than b, with the first n bytes of b
 * copied to its start, or a nil slice when a has no memory. */
static Slice grow(Alloc *a, Slice b, Int n) {
    Int m = b.len;
    if (m <= 32)
        m = 64;
    else if (m <= 256)
        m *= 2;
    else
        m += m >> 1;
    Byte *p = (Byte *)mem_alloc(a, (size_t)m, 1);
    if (p == NULL)
        return slice_nil(TYPE_BYTE);
    transform_copy_bytes(transform_bytes_of(p, m, m), transform_sub(b, 0, n));
    return transform_bytes_of(p, m, m);
}

/* Frees a buffer this file allocated, which own says it did. */
static void transform_drop(Alloc *a, Slice b, bool own) {
    if (own && b.p != NULL)
        mem_free(a, b.p, (size_t)b.cap, 1);
}

static Str fail_oom(Error *err, Int *n) {
    *err = burrow_err_out_of_memory;
    *n = 0;
    return BURROW_STR_EMPTY;
}

/* The bytes of dst[:n] as a new Str from a. */
static Str to_str(Alloc *a, Slice dst, Int n, Error *err, Int *nout) {
    if (n == 0)
        return BURROW_STR_EMPTY;
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)n, 1);
    if (p == NULL)
        return fail_oom(err, nout);
    memcpy(p, dst.p, (size_t)n);
    return str_from_bytes(p, n);
}

Str burrow__transform_string(Alloc *a, TransformTransformer t, Str s, Int *nout,
                             Error *errout) {
    Int n_dummy = 0;
    Error e_dummy = BURROW_NO_ERROR;
    Int *pn = nout != NULL ? nout : &n_dummy;
    Error *perr = errout != NULL ? errout : &e_dummy;
    reset(t);
    Slice sb = {(void *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE};
    if (s.len == 0) {
        /* Fast path for the common case for empty input. Results in about a
         * 86% reduction of running time for BenchmarkStringLowerEmpty. */
        Error e = BURROW_NO_ERROR;
        Int ns = 0;
        t.vt->transform(t.data, slice_nil(TYPE_BYTE), slice_nil(TYPE_BYTE), true, &ns,
                        &e);
        if (BURROW_OK(e)) {
            *pn = 0;
            *perr = BURROW_NO_ERROR;
            return BURROW_STR_EMPTY;
        }
    }

    /* Allocate only once. */
    Byte buf[2 * TRANSFORM_INITIAL_BUF_SIZE] = {0};
    Slice dst =
        transform_bytes_of(buf, TRANSFORM_INITIAL_BUF_SIZE, TRANSFORM_INITIAL_BUF_SIZE);
    Slice src =
        transform_bytes_of(buf + TRANSFORM_INITIAL_BUF_SIZE, TRANSFORM_INITIAL_BUF_SIZE,
                           TRANSFORM_INITIAL_BUF_SIZE);
    bool own_dst = false, own_src = false;

    /* The input string s is transformed in multiple chunks (starting with a
     * chunk size of initialBufSize). nDst and nSrc are per-chunk (or
     * per-Transform-call) indexes, pDst and pSrc are overall indexes. */
    Int n_dst = 0, n_src = 0;
    Int p_dst = 0, p_src = 0;
    Error err = BURROW_NO_ERROR;

    /* pPrefix is the length of a common prefix: the first pPrefix bytes of the
     * result will equal the first pPrefix bytes of s. It is not guaranteed to
     * be the largest such value, but if pPrefix, len(result) and len(s) are
     * all equal after the final transform (i.e. calling Transform with atEOF
     * being true returned nil error) then we don't need to allocate a new
     * result string. */
    Int p_prefix = 0;
    for (;;) {
        /* Invariant: pDst == pPrefix && pSrc == pPrefix. */
        Int n = transform_copy_bytes(src, transform_tail(sb, p_src));
        err = BURROW_NO_ERROR;
        n_dst = t.vt->transform(t.data, dst, transform_sub(src, 0, n),
                                p_src + n == s.len, &n_src, &err);
        p_dst += n_dst;
        p_src += n_src;

        if (n_dst != n_src || (n_dst > 0 && memcmp(dst.p, src.p, (size_t)n_dst) != 0))
            break;
        p_prefix = p_src;
        if (err_eq(err, burrow__transform_err_short_dst)) {
            /* A buffer can only be short if a transformer modifies its
             * input. */
            break;
        }
        if (err_eq(err, burrow__transform_err_short_src)) {
            if (n_src == 0)
                break; /* No progress was made. */
            /* Equal so far and !atEOF, so continue checking. */
        } else if (BURROW_FAILED(err) || p_prefix == s.len) {
            *pn = p_prefix;
            *perr = err;
            return str_from_bytes(s.p, p_prefix);
        }
    }
    /* Post-condition: pDst == pPrefix + nDst && pSrc == pPrefix + nSrc.
     *
     * We have transformed the first pSrc bytes of the input s to become pDst
     * transformed bytes. Those transformed bytes are discontiguous: the first
     * pPrefix of them equal s[:pPrefix] and the last nDst of them equal
     * dst[:nDst]. We copy them around, into a new dst buffer if necessary, so
     * that they become one contiguous slice: dst[:pDst]. */
    if (p_prefix != 0) {
        Slice new_dst = dst;
        bool own_new = own_dst;
        if (p_dst > new_dst.len) {
            Int m = s.len + n_dst - n_src;
            Byte *p = (Byte *)mem_alloc(a, (size_t)(m > 0 ? m : 1), 1);
            if (p == NULL)
                return fail_oom(perr, pn);
            new_dst = transform_bytes_of(p, m, m > 0 ? m : 1);
            own_new = true;
        }
        transform_copy_bytes(transform_sub(new_dst, p_prefix, p_dst),
                             transform_sub(dst, 0, n_dst));
        transform_copy_bytes(transform_sub(new_dst, 0, p_prefix),
                             transform_sub(sb, 0, p_prefix));
        if (new_dst.p != dst.p)
            transform_drop(a, dst, own_dst);
        dst = new_dst;
        own_dst = own_new;
    }

    /* Prevent duplicate Transform calls with atEOF being true at the end of
     * the input. Also return if we have an unrecoverable error. */
    if ((BURROW_OK(err) && p_src == s.len) ||
        (BURROW_FAILED(err) && !err_eq(err, burrow__transform_err_short_dst) &&
         !err_eq(err, burrow__transform_err_short_src))) {
        *pn = p_src;
        *perr = err;
        Str out = to_str(a, dst, p_dst, perr, pn);
        transform_drop(a, dst, own_dst);
        return out;
    }

    /* Transform the remaining input, growing dst and src buffers as
     * necessary. */
    for (;;) {
        Int n = transform_copy_bytes(src, transform_tail(sb, p_src));
        bool at_eof = p_src + n == s.len;
        Error e = BURROW_NO_ERROR;
        n_dst = t.vt->transform(t.data, transform_tail(dst, p_dst),
                                transform_sub(src, 0, n), at_eof, &n_src, &e);
        p_dst += n_dst;
        p_src += n_src;

        /* If we got ErrShortDst or ErrShortSrc, do not grow as long as we can
         * make progress. This may avoid excessive allocations. */
        if (err_eq(e, burrow__transform_err_short_dst)) {
            if (n_dst == 0) {
                Slice g = grow(a, dst, p_dst);
                if (g.p == NULL) {
                    transform_drop(a, dst, own_dst);
                    transform_drop(a, src, own_src);
                    return fail_oom(perr, pn);
                }
                transform_drop(a, dst, own_dst);
                dst = g;
                own_dst = true;
            }
        } else if (err_eq(e, burrow__transform_err_short_src)) {
            if (at_eof) {
                *pn = p_src;
                *perr = e;
                Str out = to_str(a, dst, p_dst, perr, pn);
                transform_drop(a, dst, own_dst);
                transform_drop(a, src, own_src);
                return out;
            }
            if (n_src == 0) {
                Slice g = grow(a, src, 0);
                if (g.p == NULL) {
                    transform_drop(a, dst, own_dst);
                    transform_drop(a, src, own_src);
                    return fail_oom(perr, pn);
                }
                transform_drop(a, src, own_src);
                src = g;
                own_src = true;
            }
        } else if (BURROW_FAILED(e) || p_src == s.len) {
            *pn = p_src;
            *perr = e;
            Str out = to_str(a, dst, p_dst, perr, pn);
            transform_drop(a, dst, own_dst);
            transform_drop(a, src, own_src);
            return out;
        }
    }
}

static Slice transform_do_append(Alloc *a, TransformTransformer t, Int p_dst, Slice dst,
                                 bool own, Slice src, Int *nout, Error *errout) {
    reset(t);
    Int p_src = 0;
    for (;;) {
        Int n_src = 0;
        Error e = BURROW_NO_ERROR;
        Int n_dst = t.vt->transform(t.data, transform_tail(dst, p_dst),
                                    transform_tail(src, p_src), true, &n_src, &e);
        p_dst += n_dst;
        p_src += n_src;
        if (!err_eq(e, burrow__transform_err_short_dst)) {
            *nout = p_src;
            *errout = e;
            return transform_sub(dst, 0, p_dst);
        }

        /* Grow the destination buffer, but do not grow as long as we can
         * make progress. This may avoid excessive allocations. */
        if (n_dst == 0) {
            Slice g = grow(a, dst, p_dst);
            if (g.p == NULL) {
                transform_drop(a, dst, own);
                *nout = 0;
                *errout = burrow_err_out_of_memory;
                return slice_nil(TYPE_BYTE);
            }
            transform_drop(a, dst, own);
            dst = g;
            own = true;
        }
    }
}

Slice burrow__transform_bytes(Alloc *a, TransformTransformer t, Slice b, Int *nout,
                              Error *errout) {
    Int n_dummy = 0;
    Error e_dummy = BURROW_NO_ERROR;
    Int *pn = nout != NULL ? nout : &n_dummy;
    Error *perr = errout != NULL ? errout : &e_dummy;
    Slice dst = slice_nil(TYPE_BYTE);
    if (b.len > 0) {
        Byte *p = (Byte *)mem_alloc(a, (size_t)b.len, 1);
        if (p == NULL) {
            *pn = 0;
            *perr = burrow_err_out_of_memory;
            return dst;
        }
        dst = transform_bytes_of(p, b.len, b.len);
    }
    return transform_do_append(a, t, 0, dst, true, b, pn, perr);
}

Slice burrow__transform_append(Alloc *a, TransformTransformer t, Slice dst, Slice src,
                               Int *nout, Error *errout) {
    Int n_dummy = 0;
    Error e_dummy = BURROW_NO_ERROR;
    Int *pn = nout != NULL ? nout : &n_dummy;
    Error *perr = errout != NULL ? errout : &e_dummy;
    bool own = false;
    if (dst.len == dst.cap) {
        Int n = src.len + dst.len; /* It is okay for this to be 0. */
        Slice b = slice_nil(TYPE_BYTE);
        if (n > 0) {
            Byte *p = (Byte *)mem_alloc(a, (size_t)n, 1);
            if (p == NULL) {
                *pn = 0;
                *perr = burrow_err_out_of_memory;
                return slice_nil(TYPE_BYTE);
            }
            b = transform_bytes_of(p, n, n);
            own = true;
        }
        dst = transform_sub(b, 0, transform_copy_bytes(b, dst));
    }
    Int p_dst = dst.len;
    return transform_do_append(a, t, p_dst, slice_sub3(dst, 0, dst.cap, dst.cap), own,
                               src, pn, perr);
}

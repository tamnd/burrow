/* golang.org/x/text/unicode/norm, the copy Go vendors.
 *
 * Unicode normalisation, in the four forms: NFC, NFD, NFKC and NFKD. It is an
 * internal of burrow's for the same reason it is one of Go's, which is that the
 * IDNA code needs it and nothing exports it. See transform.h for the rest of
 * that story.
 *
 * Everything Go's package has is here, under burrow__norm_ names that take the
 * form as their first argument where Go has it as the receiver. Results that
 * Go returns as the input itself when the input is already normal do the same
 * here, so they borrow from it.
 *
 * Go's code panics in a few places on input it was not written for, an index
 * out of range in the reorder buffer being the one a long enough run of
 * combining marks reaches through Append. This panics in the same places with
 * the same text, as a burrow runtime panic.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package xtext/unicode/norm */

#ifndef BURROW_SRC_XTEXT_NORM_H
#define BURROW_SRC_XTEXT_NORM_H

#include "transform.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/slice.h"

#include <stdint.h>

typedef enum NormForm {
    NORM_NFC,
    NORM_NFD,
    NORM_NFKC,
    NORM_NFKD,
} NormForm;

enum {
    /* The most a segment Iter returns can be, in bytes. */
    NORM_MAX_SEGMENT_SIZE = 128,
    /* The most runes the reorder buffer holds. */
    NORM_MAX_NON_STARTERS = 30,
    NORM_MAX_BUFFER_SIZE = NORM_MAX_NON_STARTERS + 2,
};

/* U+034F COMBINING GRAPHEME JOINER, which normalisation puts in to break up a
 * run of more than 30 non-starters. */
#define NORM_GRAPHEME_JOINER "\xCD\x8F"

/* What the tables say about one rune. */
typedef struct NormProperties {
    uint8_t pos;    /* start position in the reorder buffer */
    uint8_t size;   /* length of the rune's UTF-8 */
    uint8_t ccc;    /* leading canonical combining class */
    uint8_t tccc;   /* trailing canonical combining class */
    uint8_t n_lead; /* number of leading non-starters */
    uint8_t flags;  /* quick check flags */
    uint16_t index;
} NormProperties;

/* Properties and PropertiesString: the first rune of s. s must not be empty. */
NormProperties burrow__norm_properties(NormForm f, Slice s);
NormProperties burrow__norm_properties_string(NormForm f, Str s);

bool burrow__norm_boundary_before(NormProperties p);
bool burrow__norm_boundary_after(NormProperties p);
Int burrow__norm_size(NormProperties p);
uint8_t burrow__norm_ccc(NormProperties p);
uint8_t burrow__norm_lead_ccc(NormProperties p);
uint8_t burrow__norm_trail_ccc(NormProperties p);

/* The decomposition from the tables, or a nil slice when there is none. It
 * points into static data. */
BURROW_STATIC(ret) Slice burrow__norm_decomposition(NormProperties p);

/* The input to an algorithm, which is bytes or a string. Go keeps the two
 * apart because a few things are done differently for each. */
typedef struct NormInput {
    const Byte *p;
    Int len;
    bool is_str;
} NormInput;

typedef struct NormFormInfo NormFormInfo;
typedef struct NormReorderBuffer NormReorderBuffer;

struct NormReorderBuffer {
    NormProperties rune[NORM_MAX_BUFFER_SIZE];
    Byte byte[NORM_MAX_BUFFER_SIZE * 4];
    uint8_t nbyte;
    uint8_t ss;
    Int nrune;
    const NormFormInfo *f;

    NormInput src;
    Int nsrc;
    NormInput tmp_bytes;

    Slice out;
    bool (*flush_f)(NormReorderBuffer *rb);

    /* What Go keeps in closures and the collector, made explicit. a is where
     * out grows from, own_out is whether out's array came from a, oom says an
     * allocation failed, and pin and retired keep an array alive while a
     * slice of it is still being read. cmp_s and cmp_bp are the state of
     * IsNormalString's flusher. */
    Alloc *a;
    bool own_out;
    bool oom;
    const void *pin;
    Slice retired;
    Str cmp_s;
    Int cmp_bp;
};

/* String, Bytes, Append and AppendString.
 *
 * String and Bytes return s itself when it is already normal. Append writes
 * into out's spare capacity when there is enough, the way Go's append does, so
 * out itself is never freed, and grows into memory from a otherwise. A
 * failed allocation gives the empty string or a nil slice. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, s) Str burrow__norm_string(Alloc *a, NormForm f,
                                                                Str s);
BURROW_OWNS(ret) BURROW_BORROWS(ret, b) Slice burrow__norm_bytes(Alloc *a, NormForm f,
                                                                 Slice b);
BURROW_OWNS(ret) BURROW_BORROWS(ret, out) Slice burrow__norm_append(Alloc *a,
                                                                    NormForm f,
                                                                    Slice out,
                                                                    Slice src);
BURROW_OWNS(ret) BURROW_BORROWS(ret, out) Slice burrow__norm_append_string(Alloc *a,
                                                                           NormForm f,
                                                                           Slice out,
                                                                           Str src);

bool burrow__norm_is_normal(NormForm f, Slice b);
bool burrow__norm_is_normal_string(NormForm f, Str s);

Int burrow__norm_quick_span(NormForm f, Slice b);
Int burrow__norm_quick_span_string(NormForm f, Str s);
Int burrow__norm_span(NormForm f, Slice b, bool at_eof, Error *err);
Int burrow__norm_span_string(NormForm f, Str s, bool at_eof, Error *err);

Int burrow__norm_first_boundary(NormForm f, Slice b);
Int burrow__norm_first_boundary_in_string(NormForm f, Str s);
Int burrow__norm_next_boundary(NormForm f, Slice b, bool at_eof);
Int burrow__norm_next_boundary_in_string(NormForm f, Str s, bool at_eof);
Int burrow__norm_last_boundary(NormForm f, Slice b);

/* Form's Transform, and the form as a SpanningTransformer, which has no state
 * and so needs no memory. */
Int burrow__norm_transform(NormForm f, Slice dst, Slice src, bool at_eof, Int *n_src,
                           Error *err);
BURROW_STATIC(ret) TransformSpanningTransformer burrow__norm_transformer(NormForm f);

/* Iter walks the segments of a normalised input. Each Next returns at most
 * NORM_MAX_SEGMENT_SIZE bytes, which point into the input, the tables or the
 * Iter itself, and last until the next call. */
typedef struct NormIter NormIter;
typedef Slice (*NormIterFunc)(NormIter *i);

struct NormIter {
    NormReorderBuffer rb;
    Byte buf[NORM_MAX_SEGMENT_SIZE];
    NormProperties info; /* first character saved from previous iteration */
    NormIterFunc next;   /* implementation of next depends on form */
    NormIterFunc ascii_f;

    Int p;           /* current position in input source */
    Slice multi_seg; /* remainder of multi-segment decomposition, nil when p is NULL */
};

/* The input has to outlive the Iter. */
void burrow__norm_iter_init(NormIter *i, NormForm f, Slice src);
void burrow__norm_iter_init_string(NormIter *i, NormForm f, Str src);
int64_t burrow__norm_iter_seek(NormIter *i, int64_t offset, int whence, Error *err);
Int burrow__norm_iter_pos(const NormIter *i);
bool burrow__norm_iter_done(const NormIter *i);
BURROW_BORROWS(ret, i) Slice burrow__norm_iter_next(NormIter *i);

/* Reader and Writer. Free them with the matching _free. */
typedef struct NormReader {
    NormReorderBuffer rb;
    IoReader r;
    Slice inbuf;
    Slice outbuf;
    Int buf_start;
    Int last_boundary;
    Error err;
    Alloc *a;
} NormReader;

typedef struct NormWriter {
    NormReorderBuffer rb;
    IoWriter w;
    Slice buf;
    Alloc *a;
} NormWriter;

BURROW_OWNS(ret) NormReader *burrow__norm_new_reader(Alloc *a, NormForm f, IoReader r);
void burrow__norm_reader_free(NormReader *r);
Int burrow__norm_reader_read(NormReader *r, Slice p, Error *err);
BURROW_BORROWS(ret, r) IoReader burrow__norm_reader_as_io_reader(NormReader *r);

BURROW_OWNS(ret) NormWriter *burrow__norm_new_writer(Alloc *a, NormForm f, IoWriter w);
void burrow__norm_writer_free(NormWriter *w);
Int burrow__norm_writer_write(NormWriter *w, Slice data, Error *err);
Error burrow__norm_writer_close(NormWriter *w);
BURROW_BORROWS(ret, w) IoWriteCloser
burrow__norm_writer_as_io_write_closer(NormWriter *w);

/* The internals Go's tests reach, for tests/xtext_norm_test.c. */
void burrow__norm_rb_init(NormReorderBuffer *rb, NormForm f, Slice src);
void burrow__norm_rb_init_string(NormReorderBuffer *rb, NormForm f, Str src);
void burrow__norm_rb_set_flusher(NormReorderBuffer *rb, Alloc *a, Slice out,
                                 bool (*f)(NormReorderBuffer *rb));
void burrow__norm_rb_set_src(NormReorderBuffer *rb, NormInput src);
void burrow__norm_rb_reset(NormReorderBuffer *rb);
bool burrow__norm_rb_append_flush(NormReorderBuffer *rb);
bool burrow__norm_rb_do_flush(NormReorderBuffer *rb);
/* flush appends the buffer to out, growing it from a, and flushCopy copies it
 * into buf. Both empty the buffer. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, out) Slice
burrow__norm_rb_flush(NormReorderBuffer *rb, Alloc *a, Slice out);
Int burrow__norm_rb_flush_copy(NormReorderBuffer *rb, Slice buf);
/* streamSafe's first and next on rb.ss. next returns 0 for ssSuccess, 1 for
 * ssStarter and 2 for ssOverflow. */
void burrow__norm_rb_ss_first(NormReorderBuffer *rb, NormProperties p);
int burrow__norm_rb_ss_next(NormReorderBuffer *rb, NormProperties p);
Rune burrow__norm_rb_rune_at(const NormReorderBuffer *rb, Int n);
/* insertFlush, iSuccess being 0, iShortDst -1 and iShortSrc -2. */
int burrow__norm_rb_insert_flush(NormReorderBuffer *rb, NormInput src, Int i,
                                 NormProperties info);
void burrow__norm_rb_insert_unsafe(NormReorderBuffer *rb, NormInput src, Int i,
                                   NormProperties info);
int burrow__norm_rb_insert_decomposed(NormReorderBuffer *rb, Slice dcomp);
void burrow__norm_rb_compose(NormReorderBuffer *rb);
void burrow__norm_rb_free_out(NormReorderBuffer *rb);
NormProperties burrow__norm_rb_info(const NormReorderBuffer *rb, NormInput src, Int i);
Int burrow__norm_decompose_segment(NormReorderBuffer *rb, Int sp, bool at_eof);
void burrow__norm_decompose_to_last_boundary(NormReorderBuffer *rb);
Int burrow__norm_quick_span_input(NormForm f, NormInput src, Int i, Int end,
                                  bool at_eof, bool *ok);
bool burrow__norm_is_hangul(Slice b);
Int burrow__norm_decompose_hangul(Byte *buf, Rune r);
Rune burrow__norm_combine(Rune a, Rune b);
NormInput burrow__norm_input_bytes(Slice b);
NormInput burrow__norm_input_string(Str s);

#endif /* BURROW_SRC_XTEXT_NORM_H */

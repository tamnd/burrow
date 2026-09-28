/* What the compress/flate files share with tests/flate_test.c and nobody
 * else: the LZ77 window the decompressor writes into, and the hooks Go's
 * tests get by being in the package.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SRC_COMPRESS_FLATE_INTERNAL_H
#define BURROW_SRC_COMPRESS_FLATE_INTERNAL_H

#include "burrow/compress/flate.h"

#include "burrow/core.h"
#include "burrow/io.h"
#include "burrow/slice.h"

#include <string.h>

/* ----------------------------------------------------------- dictDecoder
 *
 * The LZ77 sliding dictionary as used in decompression. LZ77 decompresses
 * data through sequences of two forms of commands:
 *
 *   - Literal insertions: Runs of one or more symbols are inserted into the
 *     data stream as is. This is accomplished through the write_byte method
 *     for a single symbol, or combinations of write_slice and write_mark for
 *     multiple symbols. Any valid stream must start with a literal insertion
 *     if no preset dictionary is used.
 *
 *   - Backward copies: Runs of one or more symbols are copied from previously
 *     emitted data. Backward copies come as the tuple (dist, length) where
 *     dist determines how far back in the stream to copy from and length
 *     determines how many bytes to copy. Note that it is valid for the length
 *     to be greater than the distance. Since LZ77 uses forward copies, that
 *     situation is used to perform a form of run-length encoding on repeated
 *     runs of symbols. The write_copy and try_write_copy are used to implement
 *     this command.
 *
 * For performance reasons, this implementation performs little to no sanity
 * checks about the arguments. As such, the invariants documented for each
 * method call must be respected. */
typedef struct FlateDict {
    Byte *hist; /* sliding window history */
    Int size;

    Int wr_pos; /* current output position in buffer */
    Int rd_pos; /* have emitted hist[:rd_pos] already */
    bool full;  /* has a full window length been written yet? */
} FlateDict;

/* Initializes the dictionary with the preset dict, which hist already has
 * room for. */
static inline void flate_dict_init(FlateDict *dd, Slice dict) {
    dd->wr_pos = 0;
    dd->rd_pos = 0;
    dd->full = false;
    const Byte *p = (const Byte *)dict.p;
    Int n = dict.len;
    if (n > dd->size) {
        p += n - dd->size;
        n = dd->size;
    }
    if (n > 0)
        memcpy(dd->hist, p, (size_t)n);
    dd->wr_pos = n;
    if (dd->wr_pos == dd->size) {
        dd->wr_pos = 0;
        dd->full = true;
    }
    dd->rd_pos = dd->wr_pos;
}

/* The total amount of historical data in the history buffer. */
static inline Int flate_dict_hist_size(const FlateDict *dd) {
    return dd->full ? dd->size : dd->wr_pos;
}

/* The number of bytes of uncompressed data ready to be read out. */
static inline Int flate_dict_avail_read(const FlateDict *dd) {
    return dd->wr_pos - dd->rd_pos;
}

/* The available amount of output buffer space. */
static inline Int flate_dict_avail_write(const FlateDict *dd) {
    return dd->size - dd->wr_pos;
}

/* Inserts a single byte into the history buffer. This must only be called
 * when avail_write > 0. */
static inline void flate_dict_write_byte(FlateDict *dd, Byte c) {
    dd->hist[dd->wr_pos++] = c;
}

/* Copies [dst, end) from src, forward and a byte at a time in effect, so that
 * an overlapping copy repeats the pattern as LZ77 wants. Go gets the same
 * with copy in a loop that doubles what it copies each time round, and so does
 * this, since memcpy cannot take overlapping ranges. */
static inline void flate_forward_copy(Byte *hist, Int dst, Int end, Int src) {
    while (dst < end) {
        Int n = dst - src;
        if (n > end - dst)
            n = end - dst;
        memcpy(hist + dst, hist + src, (size_t)n);
        dst += n;
    }
}

/* Copies a string at a given (dist, length) to the output. This returns the
 * number of bytes copied and may be less than the requested length if the
 * available space in the output buffer is too small.
 *
 * This invariant must be kept: 0 < dist <= hist_size. */
static inline Int flate_dict_write_copy(FlateDict *dd, Int dist, Int length) {
    Int dst_base = dd->wr_pos;
    Int dst_pos = dst_base;
    Int src_pos = dst_pos - dist;
    Int end_pos = dst_pos + length;
    if (end_pos > dd->size)
        end_pos = dd->size;

    /* Copy non-overlapping section after destination position.
     *
     * This section is non-overlapping in that the copy length for this
     * section is always less than or equal to the backwards distance. This
     * can occur if a distance refers to data that wraps-around in the buffer.
     * Thus, a backwards copy is performed here; that is, the exact bytes in
     * the source prior to the copy is placed in the destination. */
    if (src_pos < 0) {
        src_pos += dd->size;
        Int n = dd->size - src_pos;
        if (n > end_pos - dst_pos)
            n = end_pos - dst_pos;
        memmove(dd->hist + dst_pos, dd->hist + src_pos, (size_t)n);
        dst_pos += n;
        src_pos = 0;
    }

    /* Copy possibly overlapping section before destination position. */
    flate_forward_copy(dd->hist, dst_pos, end_pos, src_pos);

    dd->wr_pos = end_pos;
    return end_pos - dst_base;
}

/* A specialized version of write_copy that tries to perform the copy without
 * checking whether it wraps around. Returns the number of bytes copied, or 0
 * when it could not do the copy that way.
 *
 * This invariant must be kept: 0 < dist <= hist_size. */
static inline Int flate_dict_try_write_copy(FlateDict *dd, Int dist, Int length) {
    Int dst_pos = dd->wr_pos;
    Int end_pos = dst_pos + length;
    if (dst_pos < dist || end_pos > dd->size)
        return 0;
    flate_forward_copy(dd->hist, dst_pos, end_pos, dst_pos - dist);
    dd->wr_pos = end_pos;
    return length;
}

/* The slice of the history buffer that is ready to be emitted to the user.
 * The data returned by read_flush must be fully consumed before calling any
 * other dict methods. */
static inline Slice flate_dict_read_flush(FlateDict *dd) {
    Slice s = slice_from(dd->hist + dd->rd_pos, dd->wr_pos - dd->rd_pos,
                         dd->wr_pos - dd->rd_pos, TYPE_BYTE);
    dd->rd_pos = dd->wr_pos;
    if (dd->wr_pos == dd->size) {
        dd->wr_pos = 0;
        dd->rd_pos = 0;
        dd->full = true;
    }
    return s;
}

/* ------------------------------------------------------------ test hooks */

/* Whether the Huffman decoder takes lengths, TestIssue5915 and the others. */
bool burrow__flate_huff_init_ok(const int *lengths, int n);

/* One symbol read from r with a decoder built from lengths, for
 * TestInvalidEncoding. The error, or none. */
BURROW_STATIC(ret) Error burrow__flate_huff_sym(const int *lengths, int n, IoReader r);

/* The reader's dict.availWrite(), for TestReaderEarlyEOF. */
Int burrow__flate_avail_write(IoReadCloser rc);

/* What the reader reads bytes from, its own BufioReader or the reader it was
 * given, for TestReaderReusesReaderBuffer. */
IoReader burrow__flate_source(IoReadCloser rc);

/* The error Close leaves behind, Go's errWriterClosed. */
Error burrow__flate_err_writer_closed(void);

/* Go's bulkHash4 and hash4, for TestBulkHash4. */
void burrow__flate_bulk_hash4(const Byte *b, Int n, uint32_t *dst);
uint32_t burrow__flate_hash4(const Byte *b);

/* Go's reverseBits, for TestReverseBits. */
uint16_t burrow__flate_reverse_bits(uint16_t x, uint8_t b);

/* A huffmanBitWriter writing to w, for the huffman_bit_writer tests, and the
 * three ways it writes a block. tokens are Go's token values, which the block
 * functions index the way indexTokens does. input NULL is Go's nil. */
typedef struct BurrowFlateBitWriter BurrowFlateBitWriter;
BURROW_OWNS(ret) BurrowFlateBitWriter *burrow__flate_bw_new(IoWriter w);
void burrow__flate_bw_free(BurrowFlateBitWriter *bw);
void burrow__flate_bw_write_block(BurrowFlateBitWriter *bw, const uint32_t *tokens,
                                  Int n, bool eof, const Byte *input, Int input_len);
void burrow__flate_bw_write_block_dynamic(BurrowFlateBitWriter *bw,
                                          const uint32_t *tokens, Int n, bool eof,
                                          const Byte *input, Int input_len, bool sync);
void burrow__flate_bw_write_block_huff(BurrowFlateBitWriter *bw, bool eof,
                                       const Byte *input, Int input_len, bool sync);
void burrow__flate_bw_flush(BurrowFlateBitWriter *bw);
BURROW_STATIC(ret) Error burrow__flate_bw_err(BurrowFlateBitWriter *bw);

/* A fast encoder of level, 1 to 6, on its own, for TestBestSpeedMatch and
 * TestBestSpeedShiftOffsets. encode gives the tokens it made in out, up to cap
 * of them, and how many. drop_hist is Go's e.hist = nil. */
typedef struct BurrowFlateFast BurrowFlateFast;
BURROW_OWNS(ret) BurrowFlateFast *burrow__flate_fast_new(int level);
void burrow__flate_fast_free(BurrowFlateFast *f);
Int burrow__flate_fast_encode(BurrowFlateFast *f, const Byte *src, Int n, uint32_t *out,
                              Int cap);
int32_t burrow__flate_fast_add_block(BurrowFlateFast *f, const Byte *src, Int n);
int32_t burrow__flate_fast_match_len_limited(BurrowFlateFast *f, int32_t s, int32_t t);
int32_t burrow__flate_fast_cur(BurrowFlateFast *f);
void burrow__flate_fast_set_cur(BurrowFlateFast *f, int32_t cur);
Int burrow__flate_fast_hist_len(BurrowFlateFast *f);
void burrow__flate_fast_drop_hist(BurrowFlateFast *f);
int32_t burrow__flate_fast_buffer_reset(void);

/* Go's w.d.fast.reset(), false for a writer with no fast encoder, and
 * w.d.reset(nil), for the wraparound tests. */
bool burrow__flate_writer_fast_reset(FlateWriter *w);
void burrow__flate_writer_reset_nil(FlateWriter *w);

/* Whether a writer is back where flate_new_writer left it, apart from the
 * window's bytes, for TestWriterReset. Compares against a fresh writer of the
 * same level and dict. */
bool burrow__flate_writer_state_eq(FlateWriter *a, FlateWriter *b);

#endif /* BURROW_SRC_COMPRESS_FLATE_INTERNAL_H */

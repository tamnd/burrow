/* internal/zstd, from zstd.go, bits.go, window.go, xxhash.go, fse.go, huff.go,
 * literals.go and block.go.
 *
 * Go's decoder line for line, with the same error messages at the same
 * offsets. The slices Go grows with append are ZsBuf here, a pointer, a length
 * and a capacity from the reader's allocator. The tables Go allocates the
 * first time a block needs them, the Huffman table, the FSE scratch table and
 * the three sequence tables, live inside the reader, since they are small and
 * every compressed block wants at least one of them. A failed allocation is
 * burrow_err_out_of_memory, which Go never returns.
 *
 * Derived from Go's src/internal/zstd. Go source: go1.27.1.
 *
 * Copyright 2023 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "zstd_internal.h"

#include "burrow/fmt.h"
#include "burrow/math/bits.h"
#include "burrow/runtime.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <string.h>

#define ZS_ERR(name, text)                                                             \
    static const Str name##__text = {(const Byte *)("" text),                          \
                                     (Int)(sizeof(text) - 1)};                         \
    static const Error name = {&burrow_sentinel_error_vt, &name##__text}

ZS_ERR(zs_err_magic, "invalid magic number");
ZS_ERR(zs_err_reserved_bit, "reserved bit set in frame header descriptor");
ZS_ERR(zs_err_dictionaries, "dictionaries are not supported");
ZS_ERR(zs_err_block_size, "block size too large");
ZS_ERR(zs_err_block_type, "invalid block type");
ZS_ERR(zs_err_too_many_bytes, "too many uncompressed bytes in frame");
ZS_ERR(zs_err_not_enough_bytes, "not enough uncompressed bytes for frame");
ZS_ERR(zs_err_zero_byte, "zero byte at reverse bit stream start");
ZS_ERR(zs_err_fse_accuracy, "FSE accuracy log too large");
ZS_ERR(zs_err_fse_index, "FSE symbol index overflow");
ZS_ERR(zs_err_fse_sym, "FSE sym overflow");
ZS_ERR(zs_err_fse_symbols, "too many symbols in FSE table");
ZS_ERR(zs_err_fse_count, "FSE count error");
ZS_ERR(zs_err_fse_state, "FSE state error");
ZS_ERR(zs_err_fse_baseline, "FSE baseline symbol overflow");
ZS_ERR(zs_err_fse_offset, "FSE offset symbol overflow");
ZS_ERR(zs_err_huff_count, "Huffman count overflow");
ZS_ERR(zs_err_huff_weight, "Huffman weight overflow");
ZS_ERR(zs_err_huff_bad, "bad Huffman weights");
ZS_ERR(zs_err_huff_small, "Huffman table too small");
ZS_ERR(zs_err_lit_size, "literal size too large");
ZS_ERR(zs_err_lit_raw, "raw literal size too large");
ZS_ERR(zs_err_lit_rle, "RLE literal missing");
ZS_ERR(zs_err_lit_table_big, "Huffman table too big");
ZS_ERR(zs_err_lit_no_tree, "missing literals Huffman tree");
ZS_ERR(zs_err_lit_bits, "literals Huffman stream out of bits");
ZS_ERR(zs_err_lit_jump, "total streams size too small for jump table");
ZS_ERR(zs_err_lit_regen, "regenerated size too small to decode streams");
ZS_ERR(zs_err_no_seqs, "extraneous data after no sequences");
ZS_ERR(zs_err_sym_mode, "invalid symbol compression mode");
ZS_ERR(zs_err_repeat_table, "missing repeat sequence FSE table");
ZS_ERR(zs_err_uncompressed, "uncompressed size too big");
ZS_ERR(zs_err_literal_overflow, "literal byte overflow");
ZS_ERR(zs_err_after_seqs, "extraneous data after sequences");
ZS_ERR(zs_err_zero_offset, "invalid zero offset");
ZS_ERR(zs_err_past_window, "offset past window");

#define ZS_MAX_HUFFMAN_BITS 11
#define ZS_MAX_SEQ_BITS 9
#define ZS_MAX_BLOCK (128 << 10)
#define ZS_MAX_WINDOW (8 << 20)

/* ------------------------------------------------------------------- types */

/* A slice Go grows with append. */
typedef struct ZsBuf {
    Byte *p;
    Int len;
    Int cap;
} ZsBuf;

/* The bytes of one block, which the bit readers read. */
typedef struct ZsData {
    const Byte *p;
    Int len;
} ZsData;

/* fseEntry: one entry in an FSE table. */
typedef struct ZsFseEntry {
    uint8_t sym;   /* value that this entry records */
    uint8_t bits;  /* number of bits to read to determine next state */
    uint16_t base; /* add those bits to this state to get the next state */
} ZsFseEntry;

/* fseBaselineEntry: an FSE table entry for the sequence codes. */
typedef struct ZsBaseline {
    uint32_t baseline; /* baseline for value that this entry represents */
    uint8_t basebits;  /* number of bits to read to add to baseline */
    uint8_t bits;      /* number of bits to read to determine next state */
    uint16_t base;     /* add the bits to this base to get the next state */
} ZsBaseline;

typedef struct ZsXxhash {
    uint64_t len;  /* total length hashed */
    uint64_t v[4]; /* accumulators */
    Byte buf[32];  /* buffer */
    Int cnt;       /* number of bytes in buffer */
} ZsXxhash;

/* window: the last size bytes of the frame, kept for matches that reach back
 * past the start of the current block. data is a ring once it is full, and
 * off is where the oldest byte is. */
typedef struct ZsWindow {
    Int size;
    ZsBuf data;
    Int off;
} ZsWindow;

enum { ZS_SEQ_LITERAL, ZS_SEQ_OFFSET, ZS_SEQ_MATCH };

struct ZstdReader {
    Alloc *a;
    IoReader r;

    bool saw_frame_header;
    bool has_checksum;
    bool read_one_frame;
    bool frame_size_unknown;
    uint64_t remaining_frame_size;

    /* Offset of the current block in the input, for error messages. */
    int64_t block_offset;

    /* The decompressed contents of the current block, and how much of it has
     * been handed out. */
    ZsBuf buffer;
    Int off;

    uint32_t repeated_offset1;
    uint32_t repeated_offset2;
    uint32_t repeated_offset3;

    uint16_t huffman_table[1 << ZS_MAX_HUFFMAN_BITS];
    Int huffman_table_bits;

    ZsWindow window;
    ZsBuf compressed_buf;
    ZsBuf literals;

    /* The sequence tables in use, either the predefined ones or the buffers
     * below, with their lengths. A length of zero is Go's nil. */
    const ZsBaseline *seq_tables[3];
    Int seq_table_lens[3];
    uint8_t seq_table_bits[3];
    ZsBaseline seq_table_buffers[3][1 << ZS_MAX_SEQ_BITS];

    Byte scratch[16];
    ZsFseEntry fse_scratch[1 << ZS_MAX_SEQ_BITS];
    ZsXxhash checksum;
};

/* ------------------------------------------------------------------ errors */

static bool zs_same_error(Error x, Error y) {
    return x.vt == y.vt && x.data == y.data;
}

/* wrapError: io.EOF as it is, anything else as a zstdError at the current
 * block offset plus off. */
static Error zs_wrap_error(const ZstdReader *r, Int off, Error err) {
    if (zs_same_error(err, io_eof))
        return err;
    int64_t at = r->block_offset + (int64_t)off;
    return fmt_errorf_v("zstd decompression error at %d: %w", at, err);
}

static Error zs_make_eof_error(const ZstdReader *r, Int off) {
    return zs_wrap_error(r, off, io_err_unexpected_eof);
}

static Error zs_wrap_non_eof_error(const ZstdReader *r, Int off, Error err) {
    if (zs_same_error(err, io_eof))
        err = io_err_unexpected_eof;
    return zs_wrap_error(r, off, err);
}

/* ----------------------------------------------------------------- buffers */

/* Makes room for n bytes in b. What it adds is zeroed, as append's is. */
static bool zs_buf_reserve(Alloc *a, ZsBuf *b, Int n) {
    if (b->cap >= n)
        return true;
    Int ncap = b->cap * 2;
    if (ncap < n)
        ncap = n;
    if (ncap < 64)
        ncap = 64;
    Byte *p = (Byte *)mem_realloc(a, b->p, (size_t)b->cap, (size_t)ncap, 1);
    if (p == NULL)
        return false;
    b->p = p;
    b->cap = ncap;
    return true;
}

/* append(b, src[:n]...), for a src that is not inside b. */
static bool zs_buf_append(Alloc *a, ZsBuf *b, const Byte *src, Int n) {
    if (n == 0)
        return true;
    if (!zs_buf_reserve(a, b, b->len + n))
        return false;
    memcpy(b->p + b->len, src, (size_t)n);
    b->len += n;
    return true;
}

static void zs_buf_free(Alloc *a, ZsBuf *b) {
    if (b->p != NULL)
        mem_free(a, b->p, (size_t)b->cap, 1);
    b->p = NULL;
    b->len = 0;
    b->cap = 0;
}

static uint16_t zs_le16(const Byte *b) {
    return (uint16_t)(b[0] | (b[1] << 8));
}

static uint32_t zs_le32(const Byte *b) {
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
           ((uint32_t)b[3] << 24);
}

static uint64_t zs_le64(const Byte *b) {
    return (uint64_t)zs_le32(b) | ((uint64_t)zs_le32(b + 4) << 32);
}

/* (1 << b) - 1 in uint32, which is all ones for b of 32 or more in Go. */
static uint32_t zs_mask(uint32_t b) {
    return b >= 32 ? 0xffffffffU : ((uint32_t)1 << b) - 1;
}

/* x >> n in uint32, which is zero for n of 32 or more in Go. */
static uint32_t zs_shr(uint32_t x, uint32_t n) {
    return n >= 32 ? 0 : x >> n;
}

/* ------------------------------------------------------------------- bits */

/* bitReader: reads a bit stream going forward. */
typedef struct ZsBitReader {
    ZstdReader *r; /* for error reporting */
    ZsData data;   /* the bits to read */
    uint32_t off;  /* current offset into data */
    uint32_t bits; /* bits ready to be returned */
    uint32_t cnt;  /* number of valid bits in the bits field */
} ZsBitReader;

static ZsBitReader zs_make_bit_reader(ZstdReader *r, ZsData data, Int off) {
    ZsBitReader br = {r, data, (uint32_t)off, 0, 0};
    return br;
}

/* moreBits: makes sure there are at least 16 bits ready. */
static Error zs_br_more_bits(ZsBitReader *br) {
    while (br->cnt < 16) {
        if (br->off >= (uint32_t)br->data.len)
            return zs_make_eof_error(br->r, (Int)br->off);
        Byte c = br->data.p[br->off];
        br->off++;
        br->bits |= (uint32_t)c << br->cnt;
        br->cnt += 8;
    }
    return BURROW_NO_ERROR;
}

/* val: the next b bits. moreBits must have been called first. */
static uint32_t zs_br_val(ZsBitReader *br, uint32_t b) {
    uint32_t v = br->bits & zs_mask(b);
    br->bits = zs_shr(br->bits, b);
    br->cnt -= b;
    return v;
}

/* backup: steps back to the first byte that has not been entirely used. */
static void zs_br_backup(ZsBitReader *br) {
    while (br->cnt >= 8) {
        br->off--;
        br->cnt -= 8;
    }
}

static Error zs_br_make_error(const ZsBitReader *br, Error msg) {
    return zs_wrap_error(br->r, (Int)br->off, msg);
}

/* reverseBitReader: reads a bit stream backward, from off down to start. */
typedef struct ZsRevBitReader {
    ZstdReader *r;  /* for error reporting */
    ZsData data;    /* the bits to read */
    uint32_t off;   /* current offset into data */
    uint32_t start; /* start in data; we read backward to start */
    uint32_t bits;  /* bits ready to be returned */
    uint32_t cnt;   /* number of valid bits in bits field */
} ZsRevBitReader;

/* makeReverseBitReader. The last byte of the stream, at off, has a 1 bit
 * marking where the stream starts, so it cannot be zero. */
static Error zs_make_rev_bit_reader(ZstdReader *r, ZsData data, Int off, Int start,
                                    ZsRevBitReader *out) {
    ZsRevBitReader zero = {r, data, 0, 0, 0, 0};
    *out = zero;
    if (off < 0 || off >= data.len)
        runtime_index_out_of_range(off, data.len);
    Byte stream_start = data.p[off];
    if (stream_start == 0)
        return zs_wrap_error(r, off, zs_err_zero_byte);
    out->r = r;
    out->data = data;
    out->off = (uint32_t)off;
    out->start = (uint32_t)start;
    out->bits = stream_start;
    out->cnt = (uint32_t)(7 - bits_leading_zeros8(stream_start));
    return BURROW_NO_ERROR;
}

/* fetch: makes sure there are at least b bits ready, and says whether there
 * were enough left in the stream for that. */
static bool zs_rbr_fetch(ZsRevBitReader *rbr, uint32_t b) {
    while (rbr->cnt < b) {
        if (rbr->off <= rbr->start)
            return false;
        rbr->off--;
        Byte c = rbr->data.p[rbr->off];
        rbr->bits <<= 8;
        rbr->bits |= c;
        rbr->cnt += 8;
    }
    return true;
}

static Error zs_rbr_val(ZsRevBitReader *rbr, uint32_t b, uint32_t *v) {
    if (!zs_rbr_fetch(rbr, b))
        return zs_make_eof_error(rbr->r, (Int)rbr->off);
    rbr->cnt -= b;
    *v = zs_shr(rbr->bits, rbr->cnt) & zs_mask(b);
    return BURROW_NO_ERROR;
}

static Error zs_rbr_make_error(const ZsRevBitReader *rbr, Error msg) {
    return zs_wrap_error(rbr->r, (Int)rbr->off, msg);
}

/* ----------------------------------------------------------------- window */

/* window.reset. The data has room for size bytes from here on, so save never
 * grows it. False when that room cannot be had. */
static bool zs_window_reset(Alloc *a, ZsWindow *w, Int size) {
    w->data.len = 0;
    if (w->data.cap < size) {
        zs_buf_free(a, &w->data);
        Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)size, 1);
        if (p == NULL)
            return false;
        w->data.p = p;
        w->data.cap = size;
    }
    w->off = 0;
    w->size = size;
    return true;
}

/* window.save: adds buf to the window, dropping the oldest bytes once it is
 * full. */
static void zs_window_save(ZsWindow *w, const Byte *buf, Int n) {
    if (w->size == 0)
        return;
    if (n == 0)
        return;
    if (n >= w->size) {
        memcpy(w->data.p, buf + (n - w->size), (size_t)w->size);
        w->data.len = w->size;
        w->off = 0;
        return;
    }
    Int space = w->size - w->data.len;
    if (space == 0) {
        Int k = w->data.len - w->off;
        if (k > n)
            k = n;
        memcpy(w->data.p + w->off, buf, (size_t)k);
        if (k == n) {
            w->off += k;
        } else {
            Int m = n - k < w->data.len ? n - k : w->data.len;
            memcpy(w->data.p, buf + k, (size_t)m);
            w->off = m;
        }
    } else if (space >= n) {
        memcpy(w->data.p + w->data.len, buf, (size_t)n);
        w->data.len += n;
    } else {
        memcpy(w->data.p + w->data.len, buf, (size_t)space);
        w->data.len += space;
        Int m = n - space < w->data.len ? n - space : w->data.len;
        memcpy(w->data.p, buf + space, (size_t)m);
        w->off = m;
    }
}

/* window.appendTo: appends the window's bytes from from to to, counted from
 * the oldest, to buf. */
static bool zs_window_append_to(Alloc *a, const ZsWindow *w, ZsBuf *buf, uint32_t from,
                                uint32_t to) {
    uint32_t data_len = (uint32_t)w->data.len;
    from += (uint32_t)w->off;
    to += (uint32_t)w->off;
    bool wrap = false;
    if (from > data_len) {
        from -= data_len;
        wrap = !wrap;
    }
    if (to > data_len) {
        to -= data_len;
        wrap = !wrap;
    }
    if (wrap) {
        return zs_buf_append(a, buf, w->data.p + from, (Int)(data_len - from)) &&
               zs_buf_append(a, buf, w->data.p, (Int)to);
    }
    return zs_buf_append(a, buf, w->data.p + from, (Int)(to - from));
}

/* --------------------------------------------------------------- xxhash64 */

#define ZS_PRIME64_1 UINT64_C(0x9e3779b185ebca87)
#define ZS_PRIME64_2 UINT64_C(0xc2b2ae3d27d4eb4f)
#define ZS_PRIME64_3 UINT64_C(0x165667b19e3779f9)
#define ZS_PRIME64_4 UINT64_C(0x85ebca77c2b2ae63)
#define ZS_PRIME64_5 UINT64_C(0x27d4eb2f165667c5)

static uint64_t zs_rotl64(uint64_t x, unsigned k) {
    return (x << k) | (x >> (64 - k));
}

static uint64_t zs_xxh_round(uint64_t v, uint64_t n) {
    v += n * ZS_PRIME64_2;
    v = zs_rotl64(v, 31);
    v *= ZS_PRIME64_1;
    return v;
}

static uint64_t zs_xxh_merge_round(uint64_t v, uint64_t n) {
    n = zs_xxh_round(0, n);
    v ^= n;
    v = v * ZS_PRIME64_1 + ZS_PRIME64_4;
    return v;
}

static void zs_xxh_reset(ZsXxhash *xh) {
    xh->len = 0;
    xh->v[0] = ZS_PRIME64_1 + ZS_PRIME64_2;
    xh->v[1] = ZS_PRIME64_2;
    xh->v[2] = 0;
    xh->v[3] = 0 - ZS_PRIME64_1;
    memset(xh->buf, 0, sizeof xh->buf);
    xh->cnt = 0;
}

static void zs_xxh_rounds(ZsXxhash *xh, const Byte *b) {
    xh->v[0] = zs_xxh_round(xh->v[0], zs_le64(b));
    xh->v[1] = zs_xxh_round(xh->v[1], zs_le64(b + 8));
    xh->v[2] = zs_xxh_round(xh->v[2], zs_le64(b + 16));
    xh->v[3] = zs_xxh_round(xh->v[3], zs_le64(b + 24));
}

static void zs_xxh_update(ZsXxhash *xh, const Byte *b, Int n) {
    xh->len += (uint64_t)n;
    if (xh->cnt + n < (Int)sizeof xh->buf) {
        if (n > 0)
            memcpy(xh->buf + xh->cnt, b, (size_t)n);
        xh->cnt += n;
        return;
    }
    if (xh->cnt > 0) {
        Int k = (Int)sizeof xh->buf - xh->cnt;
        memcpy(xh->buf + xh->cnt, b, (size_t)k);
        b += k;
        n -= k;
        zs_xxh_rounds(xh, xh->buf);
        xh->cnt = 0;
    }
    while (n >= 32) {
        zs_xxh_rounds(xh, b);
        b += 32;
        n -= 32;
    }
    if (n > 0) {
        memcpy(xh->buf, b, (size_t)n);
        xh->cnt = n;
    }
}

static uint64_t zs_xxh_digest(const ZsXxhash *xh) {
    uint64_t h64;
    if (xh->len < 32) {
        h64 = xh->v[2] + ZS_PRIME64_5;
    } else {
        h64 = zs_rotl64(xh->v[0], 1) + zs_rotl64(xh->v[1], 7) +
              zs_rotl64(xh->v[2], 12) + zs_rotl64(xh->v[3], 18);
        h64 = zs_xxh_merge_round(h64, xh->v[0]);
        h64 = zs_xxh_merge_round(h64, xh->v[1]);
        h64 = zs_xxh_merge_round(h64, xh->v[2]);
        h64 = zs_xxh_merge_round(h64, xh->v[3]);
    }

    h64 += xh->len;

    uint64_t len = xh->len & 31;
    const Byte *buf = xh->buf;
    while (len >= 8) {
        uint64_t k1 = zs_xxh_round(0, zs_le64(buf));
        buf += 8;
        h64 ^= k1;
        h64 = zs_rotl64(h64, 27) * ZS_PRIME64_1 + ZS_PRIME64_4;
        len -= 8;
    }
    if (len >= 4) {
        h64 ^= (uint64_t)zs_le32(buf) * ZS_PRIME64_1;
        buf += 4;
        h64 = zs_rotl64(h64, 23) * ZS_PRIME64_2 + ZS_PRIME64_3;
        len -= 4;
    }
    while (len > 0) {
        h64 ^= (uint64_t)buf[0] * ZS_PRIME64_5;
        buf++;
        h64 = zs_rotl64(h64, 11) * ZS_PRIME64_1;
        len--;
    }

    h64 ^= h64 >> 33;
    h64 *= ZS_PRIME64_2;
    h64 ^= h64 >> 29;
    h64 *= ZS_PRIME64_3;
    h64 ^= h64 >> 32;
    return h64;
}

/* -------------------------------------------------------------------- FSE */

/* readFSE: reads an FSE table description at off and builds the table in
 * table, which has room for 1 << maxBits entries. Returns the table's bits
 * and the offset just past the description. */
static Error zs_build_fse(ZstdReader *r, Int off, const int16_t *norm, Int nnorm,
                          ZsFseEntry *table, Int table_bits);

static Error zs_read_fse(ZstdReader *r, ZsData data, Int off, Int max_sym, Int max_bits,
                         ZsFseEntry *table, Int *table_bits, Int *roff) {
    ZsBitReader br = zs_make_bit_reader(r, data, off);
    Error e = zs_br_more_bits(&br);
    if (BURROW_FAILED(e))
        return e;

    Int accuracy_log = (Int)zs_br_val(&br, 4) + 5;
    if (accuracy_log > max_bits)
        return zs_br_make_error(&br, zs_err_fse_accuracy);

    /* The number of remaining probabilities, plus 1. This determines the
     * number of bits to read for the next value. */
    Int remaining = ((Int)1 << accuracy_log) + 1;

    /* The current difference between small and large values, which depends
     * on the number of remaining values. Small values use one less bit. */
    Int threshold = (Int)1 << accuracy_log;

    /* The number of bits needed to compute threshold. */
    Int bits_needed = accuracy_log + 1;

    /* The next character value. */
    Int sym = 0;

    /* Whether the last count was 0. */
    bool prev0 = false;

    int16_t norm[256] = {0};
    for (; remaining > 1 && sym <= max_sym;) {
        e = zs_br_more_bits(&br);
        if (BURROW_FAILED(e))
            return e;

        if (prev0) {
            /* Previous count was 0, so there is a 2-bit repeat flag. A
             * repeat flag of 3 means another 2-bit repeat flag follows. */
            Int zsym = sym;
            while ((br.bits & 0xfff) == 0xfff) {
                zsym += (Int)3 * 6;
                br.bits >>= 12;
                br.cnt -= 12;
                e = zs_br_more_bits(&br);
                if (BURROW_FAILED(e))
                    return e;
            }
            while ((br.bits & 3) == 3) {
                zsym += 3;
                br.bits >>= 2;
                br.cnt -= 2;
                e = zs_br_more_bits(&br);
                if (BURROW_FAILED(e))
                    return e;
            }

            /* We have at least 14 bits here, no need to call moreBits. */
            zsym += (Int)zs_br_val(&br, 2);

            if (zsym > max_sym)
                return zs_br_make_error(&br, zs_err_fse_index);

            for (; sym < zsym; sym++)
                norm[(uint8_t)sym] = 0;

            prev0 = false;
            continue;
        }

        Int max = (2 * threshold - 1) - remaining;
        Int count;
        if ((Int)(br.bits & (uint32_t)(threshold - 1)) < max) {
            /* A small value. */
            count = (Int)(br.bits & (uint32_t)(threshold - 1));
            br.bits >>= bits_needed - 1;
            br.cnt -= (uint32_t)(bits_needed - 1);
        } else {
            /* A large value. */
            count = (Int)(br.bits & (uint32_t)(2 * threshold - 1));
            if (count >= threshold)
                count -= max;
            br.bits >>= bits_needed;
            br.cnt -= (uint32_t)bits_needed;
        }

        count--;
        if (count >= 0)
            remaining -= count;
        else
            remaining--;
        if (sym >= 256)
            return zs_br_make_error(&br, zs_err_fse_sym);
        norm[(uint8_t)sym] = (int16_t)count;
        sym++;

        prev0 = count == 0;

        while (remaining < threshold) {
            bits_needed--;
            threshold >>= 1;
        }
    }

    if (remaining != 1)
        return zs_br_make_error(&br, zs_err_fse_symbols);

    for (; sym <= max_sym; sym++)
        norm[(uint8_t)sym] = 0;

    zs_br_backup(&br);

    e = zs_build_fse(r, off, norm, max_sym + 1, table, accuracy_log);
    if (BURROW_FAILED(e))
        return e;

    *table_bits = accuracy_log;
    *roff = (Int)br.off;
    return BURROW_NO_ERROR;
}

/* buildFSE: builds an FSE decoding table from a list of probabilities. */
static Error zs_build_fse(ZstdReader *r, Int off, const int16_t *norm, Int nnorm,
                          ZsFseEntry *table, Int table_bits) {
    Int table_size = (Int)1 << table_bits;
    Int high_threshold = table_size - 1;

    uint16_t next[256] = {0};

    for (Int i = 0; i < nnorm; i++) {
        int16_t n = norm[i];
        if (n >= 0) {
            next[(uint8_t)i] = (uint16_t)n;
        } else {
            if (high_threshold < 0)
                runtime_index_out_of_range(high_threshold, table_size);
            table[high_threshold].sym = (uint8_t)i;
            high_threshold--;
            next[(uint8_t)i] = 1;
        }
    }

    Int pos = 0;
    Int step = (table_size >> 1) + (table_size >> 3) + 3;
    Int mask = table_size - 1;
    for (Int i = 0; i < nnorm; i++) {
        for (Int j = 0; j < (Int)norm[i]; j++) {
            table[pos].sym = (uint8_t)i;
            pos = (pos + step) & mask;
            while (pos > high_threshold)
                pos = (pos + step) & mask;
        }
    }
    if (pos != 0)
        return zs_wrap_error(r, off, zs_err_fse_count);

    for (Int i = 0; i < table_size; i++) {
        uint8_t sym = table[i].sym;
        uint16_t next_state = next[sym];
        next[sym]++;

        if (next_state == 0)
            return zs_wrap_error(r, off, zs_err_fse_state);

        Int high_bit = 15 - bits_leading_zeros16(next_state);

        Int bits = table_bits - high_bit;
        table[i].bits = (uint8_t)bits;
        table[i].base =
            (uint16_t)(((uint32_t)next_state << bits) - (uint32_t)table_size);
    }

    return BURROW_NO_ERROR;
}

/* The baselines and extra bits of the literal length codes from 16 up, and of
 * the match length codes from 32 up, packed as baseline | bits << 24. */
#define ZS_LITERAL_LENGTH_OFFSET 16
#define ZS_MATCH_LENGTH_OFFSET 32

static const ZsBaseline zs_predef_literal[64] = {
    {0, 0, 4, 0},      {0, 0, 4, 16},     {1, 0, 5, 32},     {3, 0, 5, 0},
    {4, 0, 5, 0},      {6, 0, 5, 0},      {7, 0, 5, 0},      {9, 0, 5, 0},
    {10, 0, 5, 0},     {12, 0, 5, 0},     {14, 0, 6, 0},     {16, 1, 5, 0},
    {20, 1, 5, 0},     {22, 1, 5, 0},     {28, 2, 5, 0},     {32, 3, 5, 0},
    {48, 4, 5, 0},     {64, 6, 5, 32},    {128, 7, 5, 0},    {256, 8, 6, 0},
    {1024, 10, 6, 0},  {4096, 12, 6, 0},  {0, 0, 4, 32},     {1, 0, 4, 0},
    {2, 0, 5, 0},      {4, 0, 5, 32},     {5, 0, 5, 0},      {7, 0, 5, 32},
    {8, 0, 5, 0},      {10, 0, 5, 32},    {11, 0, 5, 0},     {13, 0, 6, 0},
    {16, 1, 5, 32},    {18, 1, 5, 0},     {22, 1, 5, 32},    {24, 2, 5, 0},
    {32, 3, 5, 32},    {40, 3, 5, 0},     {64, 6, 4, 0},     {64, 6, 4, 16},
    {128, 7, 5, 32},   {512, 9, 6, 0},    {2048, 11, 6, 0},  {0, 0, 4, 48},
    {1, 0, 4, 16},     {2, 0, 5, 32},     {3, 0, 5, 32},     {5, 0, 5, 32},
    {6, 0, 5, 32},     {8, 0, 5, 32},     {9, 0, 5, 32},     {11, 0, 5, 32},
    {12, 0, 5, 32},    {15, 0, 6, 0},     {18, 1, 5, 32},    {20, 1, 5, 32},
    {24, 2, 5, 32},    {28, 2, 5, 32},    {40, 3, 5, 32},    {48, 4, 5, 32},
    {65536, 16, 6, 0}, {32768, 15, 6, 0}, {16384, 14, 6, 0}, {8192, 13, 6, 0},
};

static const ZsBaseline zs_predef_offset[32] = {
    {1, 0, 5, 0},          {61, 6, 4, 0},         {509, 9, 5, 0},
    {32765, 15, 5, 0},     {2097149, 21, 5, 0},   {5, 3, 5, 0},
    {125, 7, 4, 0},        {4093, 12, 5, 0},      {262141, 18, 5, 0},
    {8388605, 23, 5, 0},   {29, 5, 5, 0},         {253, 8, 4, 0},
    {16381, 14, 5, 0},     {1048573, 20, 5, 0},   {1, 2, 5, 0},
    {125, 7, 4, 16},       {2045, 11, 5, 0},      {131069, 17, 5, 0},
    {4194301, 22, 5, 0},   {13, 4, 5, 0},         {253, 8, 4, 16},
    {8189, 13, 5, 0},      {524285, 19, 5, 0},    {2, 1, 5, 0},
    {61, 6, 4, 16},        {1021, 10, 5, 0},      {65533, 16, 5, 0},
    {268435453, 28, 5, 0}, {134217725, 27, 5, 0}, {67108861, 26, 5, 0},
    {33554429, 25, 5, 0},  {16777213, 24, 5, 0},
};

static const ZsBaseline zs_predef_match[64] = {
    {3, 0, 6, 0},     {4, 0, 4, 0},      {5, 0, 5, 32},     {6, 0, 5, 0},
    {8, 0, 5, 0},     {9, 0, 5, 0},      {11, 0, 5, 0},     {13, 0, 6, 0},
    {16, 0, 6, 0},    {19, 0, 6, 0},     {22, 0, 6, 0},     {25, 0, 6, 0},
    {28, 0, 6, 0},    {31, 0, 6, 0},     {34, 0, 6, 0},     {37, 1, 6, 0},
    {41, 1, 6, 0},    {47, 2, 6, 0},     {59, 3, 6, 0},     {83, 4, 6, 0},
    {131, 7, 6, 0},   {515, 9, 6, 0},    {4, 0, 4, 16},     {5, 0, 4, 0},
    {6, 0, 5, 32},    {7, 0, 5, 0},      {9, 0, 5, 32},     {10, 0, 5, 0},
    {12, 0, 6, 0},    {15, 0, 6, 0},     {18, 0, 6, 0},     {21, 0, 6, 0},
    {24, 0, 6, 0},    {27, 0, 6, 0},     {30, 0, 6, 0},     {33, 0, 6, 0},
    {35, 1, 6, 0},    {39, 1, 6, 0},     {43, 2, 6, 0},     {51, 3, 6, 0},
    {67, 4, 6, 0},    {99, 5, 6, 0},     {259, 8, 6, 0},    {4, 0, 4, 32},
    {4, 0, 4, 48},    {5, 0, 4, 16},     {7, 0, 5, 32},     {8, 0, 5, 32},
    {10, 0, 5, 32},   {11, 0, 5, 32},    {14, 0, 6, 0},     {17, 0, 6, 0},
    {20, 0, 6, 0},    {23, 0, 6, 0},     {26, 0, 6, 0},     {29, 0, 6, 0},
    {32, 0, 6, 0},    {65539, 16, 6, 0}, {32771, 15, 6, 0}, {16387, 14, 6, 0},
    {8195, 13, 6, 0}, {4099, 12, 6, 0},  {2051, 11, 6, 0},  {1027, 10, 6, 0},
};

static const uint32_t zs_literal_length_base[20] = {
    16 | (1U << 24),    18 | (1U << 24),     20 | (1U << 24),     22 | (1U << 24),
    24 | (2U << 24),    28 | (2U << 24),     32 | (3U << 24),     40 | (3U << 24),
    48 | (4U << 24),    64 | (6U << 24),     128 | (7U << 24),    256 | (8U << 24),
    512 | (9U << 24),   1024 | (10U << 24),  2048 | (11U << 24),  4096 | (12U << 24),
    8192 | (13U << 24), 16384 | (14U << 24), 32768 | (15U << 24), 65536 | (16U << 24),
};

static const uint32_t zs_match_length_base[21] = {
    35 | (1U << 24),     37 | (1U << 24),    39 | (1U << 24),     41 | (1U << 24),
    43 | (2U << 24),     47 | (2U << 24),    51 | (3U << 24),     59 | (3U << 24),
    67 | (4U << 24),     83 | (4U << 24),    99 | (5U << 24),     131 | (7U << 24),
    259 | (8U << 24),    515 | (9U << 24),   1027 | (10U << 24),  2051 | (11U << 24),
    4099 | (12U << 24),  8195 | (13U << 24), 16387 | (14U << 24), 32771 | (15U << 24),
    65539 | (16U << 24),
};
/* makeLiteralBaselineFSE: converts the literal length FSE table to a
 * baseline table. */
static Error zs_literal_baseline(ZstdReader *r, Int off, const ZsFseEntry *fse, Int n,
                                 ZsBaseline *out) {
    for (Int i = 0; i < n; i++) {
        ZsFseEntry e = fse[i];
        ZsBaseline be = {0, 0, e.bits, e.base};
        if (e.sym < ZS_LITERAL_LENGTH_OFFSET) {
            be.baseline = e.sym;
            be.basebits = 0;
        } else {
            if (e.sym > 35)
                return zs_wrap_error(r, off, zs_err_fse_baseline);
            uint32_t basebits =
                zs_literal_length_base[e.sym - ZS_LITERAL_LENGTH_OFFSET];
            be.baseline = basebits & 0xffffff;
            be.basebits = (uint8_t)(basebits >> 24);
        }
        out[i] = be;
    }
    return BURROW_NO_ERROR;
}

/* makeOffsetBaselineFSE: converts the offset FSE table to a baseline table. */
static Error zs_offset_baseline(ZstdReader *r, Int off, const ZsFseEntry *fse, Int n,
                                ZsBaseline *out) {
    for (Int i = 0; i < n; i++) {
        ZsFseEntry e = fse[i];
        ZsBaseline be = {0, 0, e.bits, e.base};
        if (e.sym > 31)
            return zs_wrap_error(r, off, zs_err_fse_offset);

        /* The baseline is 1 << sym, less 3 for the symbols that are not
         * repeat offsets. */
        be.baseline = (uint32_t)1 << e.sym;
        if (e.sym >= 2)
            be.baseline -= 3;
        be.basebits = e.sym;
        out[i] = be;
    }
    return BURROW_NO_ERROR;
}

/* makeMatchBaselineFSE: converts the match length FSE table to a baseline
 * table. */
static Error zs_match_baseline(ZstdReader *r, Int off, const ZsFseEntry *fse, Int n,
                               ZsBaseline *out) {
    for (Int i = 0; i < n; i++) {
        ZsFseEntry e = fse[i];
        ZsBaseline be = {0, 0, e.bits, e.base};
        if (e.sym < ZS_MATCH_LENGTH_OFFSET) {
            be.baseline = (uint32_t)e.sym + 3;
            be.basebits = 0;
        } else {
            if (e.sym > 52)
                return zs_wrap_error(r, off, zs_err_fse_baseline);
            uint32_t basebits = zs_match_length_base[e.sym - ZS_MATCH_LENGTH_OFFSET];
            be.baseline = basebits & 0xffffff;
            be.basebits = (uint8_t)(basebits >> 24);
        }
        out[i] = be;
    }
    return BURROW_NO_ERROR;
}

/* ---------------------------------------------------------------- Huffman */

/* readHuff: reads a Huffman table description at off and builds the decoding
 * table in table, which has table_len entries. */
static Error zs_read_huff(ZstdReader *r, ZsData data, Int off, uint16_t *table,
                          Int table_len, Int *table_bits_out, Int *roff) {
    if (off >= data.len)
        return zs_make_eof_error(r, off);

    Byte hdr = data.p[off];
    off++;

    uint8_t weights[256] = {0};
    Int count = 0;
    if (hdr < 128) {
        /* The table is compressed using an FSE. No more than 6 bits. */
        ZsFseEntry *fse_table = r->fse_scratch;
        Int fse_bits = 0;
        Int noff = 0;
        Error e = zs_read_fse(r, data, off, 255, 6, fse_table, &fse_bits, &noff);
        if (BURROW_FAILED(e))
            return e;

        if (off + (Int)hdr > data.len)
            return zs_make_eof_error(r, off);

        ZsRevBitReader rbr;
        e = zs_make_rev_bit_reader(r, data, off + (Int)hdr - 1, noff, &rbr);
        if (BURROW_FAILED(e))
            return e;

        uint32_t state1 = 0;
        uint32_t state2 = 0;
        e = zs_rbr_val(&rbr, (uint32_t)fse_bits, &state1);
        if (BURROW_FAILED(e))
            return e;
        e = zs_rbr_val(&rbr, (uint32_t)fse_bits, &state2);
        if (BURROW_FAILED(e))
            return e;

        /* There are two independent FSE streams, tracked by state1 and
         * state2. We decode them alternately. */
        for (;;) {
            const ZsFseEntry *pt = &fse_table[state1];
            if (!zs_rbr_fetch(&rbr, pt->bits)) {
                if (count >= 254)
                    return zs_rbr_make_error(&rbr, zs_err_huff_count);
                weights[count] = pt->sym;
                weights[count + 1] = fse_table[state2].sym;
                count += 2;
                break;
            }

            uint32_t v = 0;
            e = zs_rbr_val(&rbr, pt->bits, &v);
            if (BURROW_FAILED(e))
                return e;
            state1 = (uint32_t)pt->base + v;

            if (count >= 255)
                return zs_rbr_make_error(&rbr, zs_err_huff_count);

            weights[count] = pt->sym;
            count++;

            pt = &fse_table[state2];

            if (!zs_rbr_fetch(&rbr, pt->bits)) {
                if (count >= 254)
                    return zs_rbr_make_error(&rbr, zs_err_huff_count);
                weights[count] = pt->sym;
                weights[count + 1] = fse_table[state1].sym;
                count += 2;
                break;
            }

            e = zs_rbr_val(&rbr, pt->bits, &v);
            if (BURROW_FAILED(e))
                return e;
            state2 = (uint32_t)pt->base + v;

            if (count >= 255)
                return zs_rbr_make_error(&rbr, zs_err_huff_count);

            weights[count] = pt->sym;
            count++;
        }

        off += (Int)hdr;
    } else {
        /* The table is not compressed. Each weight is 4 bits. */
        count = (Int)hdr - 127;
        if (off + ((count + 1) / 2) >= data.len)
            return io_err_unexpected_eof;
        for (Int i = 0; i < count; i += 2) {
            Byte b = data.p[off];
            off++;
            weights[i] = b >> 4;
            weights[i + 1] = b & 0xf;
        }
    }

    /* weightMark is the number of symbols for each weight, and later the
     * start of each weight's run in the table. */
    uint32_t weight_mark[13] = {0};
    uint32_t weight_mask = 0;
    for (Int i = 0; i < count; i++) {
        uint8_t w = weights[i];
        if (w > 12)
            return zs_wrap_error(r, off, zs_err_huff_weight);
        weight_mark[w]++;
        if (w > 0)
            weight_mask += (uint32_t)1 << (w - 1);
    }
    if (weight_mask == 0)
        return zs_wrap_error(r, off, zs_err_huff_bad);

    Int table_bits = 32 - bits_leading_zeros32(weight_mask);
    if (table_bits > ZS_MAX_HUFFMAN_BITS)
        return zs_wrap_error(r, off, zs_err_huff_bad);

    if (table_len < (Int)1 << table_bits)
        return zs_wrap_error(r, off, zs_err_huff_small);

    /* The last weight is not in the description. It is whatever brings the
     * total up to the next power of two. */
    uint32_t left = ((uint32_t)1 << table_bits) - weight_mask;
    if (left == 0)
        return zs_wrap_error(r, off, zs_err_huff_bad);
    Int high_bit = 31 - bits_leading_zeros32(left);
    if ((uint32_t)1 << high_bit != left)
        return zs_wrap_error(r, off, zs_err_huff_bad);
    if (count >= 256)
        return zs_wrap_error(r, off, zs_err_huff_weight);
    weights[count] = (uint8_t)(high_bit + 1);
    count++;
    weight_mark[high_bit + 1]++;

    if (weight_mark[1] < 2 || (weight_mark[1] & 1) != 0)
        return zs_wrap_error(r, off, zs_err_huff_bad);

    /* Change weightMark from a count of weights to the index of the first
     * symbol for that weight. */
    uint32_t next = 0;
    for (Int i = 0; i < table_bits; i++) {
        uint32_t cur = next;
        next += weight_mark[i + 1] << i;
        weight_mark[i + 1] = cur;
    }

    for (Int i = 0; i < count; i++) {
        uint8_t w = weights[i];
        if (w == 0)
            continue;
        uint32_t length = (uint32_t)1 << (w - 1);
        uint16_t tval = (uint16_t)(((uint32_t)i << 8) | (uint32_t)(table_bits + 1 - w));
        uint32_t start = weight_mark[w];
        for (uint32_t j = 0; j < length; j++)
            table[start + j] = tval;
        weight_mark[w] += length;
    }

    *table_bits_out = table_bits;
    *roff = off;
    return BURROW_NO_ERROR;
}

/* --------------------------------------------------------------- literals */

static Error zs_read_raw_rle_literals(ZstdReader *r, ZsData data, Int off, Byte hdr,
                                      ZsBuf *outbuf, Int *roff);
static Error zs_read_huff_literals(ZstdReader *r, ZsData data, Int off, Byte hdr,
                                   ZsBuf *outbuf, Int *roff);

/* readLiterals: reads and decompresses the literals section at off, appending
 * them to outbuf. Returns the offset just past the section. */
static Error zs_read_literals(ZstdReader *r, ZsData data, Int off, ZsBuf *outbuf,
                              Int *roff) {
    if (off >= data.len)
        return zs_make_eof_error(r, off);

    /* Literals section header. */
    Byte hdr = data.p[off];
    off++;

    if ((hdr & 3) == 0 || (hdr & 3) == 1)
        return zs_read_raw_rle_literals(r, data, off, hdr, outbuf, roff);
    return zs_read_huff_literals(r, data, off, hdr, outbuf, roff);
}

static Error zs_read_raw_rle_literals(ZstdReader *r, ZsData data, Int off, Byte hdr,
                                      ZsBuf *outbuf, Int *roff) {
    bool raw = (hdr & 3) == 0;

    Int regenerated_size = 0;
    switch ((hdr >> 2) & 3) {
    case 0:
    case 2:
        regenerated_size = (Int)(hdr >> 3);
        break;
    case 1:
        if (off >= data.len)
            return zs_make_eof_error(r, off);
        regenerated_size = (Int)(hdr >> 4) + ((Int)data.p[off] << 4);
        off++;
        break;
    case 3:
        if (off + 1 >= data.len)
            return zs_make_eof_error(r, off);
        regenerated_size =
            (Int)(hdr >> 4) + ((Int)data.p[off] << 4) + ((Int)data.p[off + 1] << 12);
        off += 2;
        break;
    default:
        break;
    }

    /* We are going to use the entire literal block in the output. The maximum
     * size of one decompressed block is 128K, so we can't have more literals
     * than that. */
    if (regenerated_size > ZS_MAX_BLOCK)
        return zs_wrap_error(r, off, zs_err_lit_size);

    if (raw) {
        /* RawLiteralsBlock. */
        if (off + regenerated_size > data.len)
            return zs_wrap_error(r, off, zs_err_lit_raw);
        if (!zs_buf_append(r->a, outbuf, data.p + off, regenerated_size))
            return burrow_err_out_of_memory;
        off += regenerated_size;
    } else {
        /* RLELiteralsBlock. */
        if (off >= data.len)
            return zs_wrap_error(r, off, zs_err_lit_rle);
        Byte rle = data.p[off];
        off++;
        if (!zs_buf_reserve(r->a, outbuf, outbuf->len + regenerated_size))
            return burrow_err_out_of_memory;
        memset(outbuf->p + outbuf->len, rle, (size_t)regenerated_size);
        outbuf->len += regenerated_size;
    }

    *roff = off;
    return BURROW_NO_ERROR;
}

static Error zs_read_literals_one_stream(ZstdReader *r, ZsData data, Int off,
                                         Int compressed_size, Int regenerated_size,
                                         ZsBuf *outbuf);
static Error zs_read_literals_four_streams(ZstdReader *r, ZsData data, Int off,
                                           Int total_streams_size, Int regenerated_size,
                                           ZsBuf *outbuf);

static Error zs_read_huff_literals(ZstdReader *r, ZsData data, Int off, Byte hdr,
                                   ZsBuf *outbuf, Int *roff) {
    Int regenerated_size = 0;
    Int compressed_size = 0;
    Int streams = 0;
    switch ((hdr >> 2) & 3) {
    case 0:
    case 1:
        if (off + 1 >= data.len)
            return zs_make_eof_error(r, off);
        regenerated_size = ((Int)hdr >> 4) | (((Int)data.p[off] & 0x3f) << 4);
        compressed_size = ((Int)data.p[off] >> 6) | ((Int)data.p[off + 1] << 2);
        off += 2;
        if (((hdr >> 2) & 3) == 0)
            streams = 1;
        else
            streams = 4;
        break;
    case 2:
        if (off + 2 >= data.len)
            return zs_make_eof_error(r, off);
        regenerated_size = ((Int)hdr >> 4) | ((Int)data.p[off] << 4) |
                           (((Int)data.p[off + 1] & 3) << 12);
        compressed_size = ((Int)data.p[off + 1] >> 2) | ((Int)data.p[off + 2] << 6);
        off += 3;
        streams = 4;
        break;
    case 3:
        if (off + 3 >= data.len)
            return zs_make_eof_error(r, off);
        regenerated_size = ((Int)hdr >> 4) | ((Int)data.p[off] << 4) |
                           (((Int)data.p[off + 1] & 0x3f) << 12);
        compressed_size = ((Int)data.p[off + 1] >> 6) | ((Int)data.p[off + 2] << 2) |
                          ((Int)data.p[off + 3] << 10);
        off += 4;
        streams = 4;
        break;
    default:
        break;
    }

    /* We are going to use the entire literal block in the output. The maximum
     * size of one decompressed block is 128K, so we can't have more literals
     * than that. */
    if (regenerated_size > ZS_MAX_BLOCK)
        return zs_wrap_error(r, off, zs_err_lit_size);

    Int end = off + compressed_size;
    if (end > data.len || end < 0)
        return zs_make_eof_error(r, off);

    Int total_streams_size = compressed_size;
    if ((hdr & 3) == 2) {
        /* CompressedLiteralsBlock. Read new Huffman tree. */
        Int huffman_table_bits = 0;
        Int hoff = 0;
        Error e =
            zs_read_huff(r, data, off, r->huffman_table,
                         (Int)(sizeof r->huffman_table / sizeof r->huffman_table[0]),
                         &huffman_table_bits, &hoff);
        if (BURROW_FAILED(e))
            return e;
        r->huffman_table_bits = huffman_table_bits;

        if (total_streams_size < hoff - off)
            return zs_wrap_error(r, off, zs_err_lit_table_big);

        total_streams_size -= hoff - off;
        off = hoff;
    } else {
        /* TreelessLiteralsBlock. Reuse previous Huffman tree. */
        if (r->huffman_table_bits == 0)
            return zs_wrap_error(r, off, zs_err_lit_no_tree);
    }

    /* Decompress compressedSize bytes of data at off using the Huffman
     * tree. */
    Error e = BURROW_NO_ERROR;
    if (streams == 1) {
        e = zs_read_literals_one_stream(r, data, off, total_streams_size,
                                        regenerated_size, outbuf);
    } else {
        e = zs_read_literals_four_streams(r, data, off, total_streams_size,
                                          regenerated_size, outbuf);
    }
    if (BURROW_FAILED(e))
        return e;

    *roff = end;
    return BURROW_NO_ERROR;
}

/* readLiteralsOneStream: reads a single stream of compressed literals. */
static Error zs_read_literals_one_stream(ZstdReader *r, ZsData data, Int off,
                                         Int compressed_size, Int regenerated_size,
                                         ZsBuf *outbuf) {
    /* We let the reverse bit reader read earlier bytes, because the Huffman
     * table ignores bits that it doesn't need. */
    ZsRevBitReader rbr;
    Error e = zs_make_rev_bit_reader(r, data, off + compressed_size - 1, off - 2, &rbr);
    if (BURROW_FAILED(e))
        return e;

    if (!zs_buf_reserve(r->a, outbuf, outbuf->len + regenerated_size))
        return burrow_err_out_of_memory;

    const uint16_t *huff_table = r->huffman_table;
    uint32_t huff_bits = (uint32_t)r->huffman_table_bits;
    uint32_t huff_mask = zs_mask(huff_bits);

    for (Int i = 0; i < regenerated_size; i++) {
        if (!zs_rbr_fetch(&rbr, huff_bits))
            return zs_rbr_make_error(&rbr, zs_err_lit_bits);

        uint32_t idx = zs_shr(rbr.bits, rbr.cnt - huff_bits) & huff_mask;
        uint16_t t = huff_table[idx];
        outbuf->p[outbuf->len] = (Byte)(t >> 8);
        outbuf->len++;
        rbr.cnt -= (uint32_t)(t & 0xff);
    }

    return BURROW_NO_ERROR;
}

/* fetchHuff: the table entry for the next code in rbr, without using its
 * bits up. */
static Error zs_fetch_huff(ZsRevBitReader *rbr, const uint16_t *huff_table,
                           uint32_t huff_bits, uint32_t huff_mask, uint16_t *t) {
    if (!zs_rbr_fetch(rbr, huff_bits))
        return zs_rbr_make_error(rbr, zs_err_lit_bits);
    uint32_t idx = zs_shr(rbr->bits, rbr->cnt - huff_bits) & huff_mask;
    *t = huff_table[idx];
    return BURROW_NO_ERROR;
}

/* readLiteralsFourStreams: reads four interleaved streams of compressed
 * literals. */
static Error zs_read_literals_four_streams(ZstdReader *r, ZsData data, Int off,
                                           Int total_streams_size, Int regenerated_size,
                                           ZsBuf *outbuf) {
    /* Read the jump table to find out where the streams are. */
    if (off + 5 >= data.len)
        return zs_make_eof_error(r, off);
    if (total_streams_size < 6)
        return zs_wrap_error(r, off, zs_err_lit_jump);

    Int regenerated_stream_size = (regenerated_size + 3) / 4;
    if (regenerated_size < regenerated_stream_size * 3)
        return zs_wrap_error(r, off, zs_err_lit_regen);

    uint16_t stream_size1 = zs_le16(data.p + off);
    uint16_t stream_size2 = zs_le16(data.p + off + 2);
    uint16_t stream_size3 = zs_le16(data.p + off + 4);
    off += 6;

    uint64_t tot =
        (uint64_t)stream_size1 + (uint64_t)stream_size2 + (uint64_t)stream_size3;
    if (tot > (uint64_t)total_streams_size - 6)
        return zs_make_eof_error(r, off);
    uint32_t stream_size4 = (uint32_t)total_streams_size - 6 - (uint32_t)tot;

    off--;
    Int off1 = off + (Int)stream_size1;
    Int start1 = off + 1;

    Int off2 = off1 + (Int)stream_size2;
    Int start2 = off1 + 1;

    Int off3 = off2 + (Int)stream_size3;
    Int start3 = off2 + 1;

    Int off4 = off3 + (Int)stream_size4;
    Int start4 = off3 + 1;

    /* We let the reverse bit readers read earlier bytes, because the Huffman
     * table ignores bits that it doesn't need. */
    ZsRevBitReader rbr1;
    ZsRevBitReader rbr2;
    ZsRevBitReader rbr3;
    ZsRevBitReader rbr4;
    Error e = zs_make_rev_bit_reader(r, data, off1, start1 - 2, &rbr1);
    if (BURROW_FAILED(e))
        return e;
    e = zs_make_rev_bit_reader(r, data, off2, start2 - 2, &rbr2);
    if (BURROW_FAILED(e))
        return e;
    e = zs_make_rev_bit_reader(r, data, off3, start3 - 2, &rbr3);
    if (BURROW_FAILED(e))
        return e;
    e = zs_make_rev_bit_reader(r, data, off4, start4 - 2, &rbr4);
    if (BURROW_FAILED(e))
        return e;

    Int out1 = outbuf->len;
    Int out2 = out1 + regenerated_stream_size;
    Int out3 = out2 + regenerated_stream_size;
    Int out4 = out3 + regenerated_stream_size;

    Int regenerated_stream_size4 = regenerated_size - regenerated_stream_size * 3;

    if (!zs_buf_reserve(r->a, outbuf, outbuf->len + regenerated_size))
        return burrow_err_out_of_memory;
    memset(outbuf->p + outbuf->len, 0, (size_t)regenerated_size);
    outbuf->len += regenerated_size;
    Byte *out = outbuf->p;

    const uint16_t *huff_table = r->huffman_table;
    uint32_t huff_bits = (uint32_t)r->huffman_table_bits;
    uint32_t huff_mask = zs_mask(huff_bits);

    for (Int i = 0; i < regenerated_stream_size; i++) {
        bool use4 = i < regenerated_stream_size4;

        uint16_t t1 = 0;
        uint16_t t2 = 0;
        uint16_t t3 = 0;
        e = zs_fetch_huff(&rbr1, huff_table, huff_bits, huff_mask, &t1);
        if (BURROW_FAILED(e))
            return e;
        e = zs_fetch_huff(&rbr2, huff_table, huff_bits, huff_mask, &t2);
        if (BURROW_FAILED(e))
            return e;
        e = zs_fetch_huff(&rbr3, huff_table, huff_bits, huff_mask, &t3);
        if (BURROW_FAILED(e))
            return e;

        if (use4) {
            uint16_t t4 = 0;
            e = zs_fetch_huff(&rbr4, huff_table, huff_bits, huff_mask, &t4);
            if (BURROW_FAILED(e))
                return e;
            out[out4] = (Byte)(t4 >> 8);
            out4++;
            rbr4.cnt -= (uint32_t)(t4 & 0xff);
        }

        out[out1] = (Byte)(t1 >> 8);
        out1++;
        rbr1.cnt -= (uint32_t)(t1 & 0xff);

        out[out2] = (Byte)(t2 >> 8);
        out2++;
        rbr2.cnt -= (uint32_t)(t2 & 0xff);

        out[out3] = (Byte)(t3 >> 8);
        out3++;
        rbr3.cnt -= (uint32_t)(t3 & 0xff);
    }

    return BURROW_NO_ERROR;
}

/* ------------------------------------------------------------------ block */

typedef Error (*ZsToBaseline)(ZstdReader *r, Int off, const ZsFseEntry *fse, Int n,
                              ZsBaseline *out);

/* seqCodeInfoData: what setSeqTable needs to know about each kind of
 * sequence code. */
typedef struct ZsSeqCodeInfo {
    const ZsBaseline *predef_table; /* predefined FSE */
    Int predef_table_len;
    Int predef_table_bits; /* number of bits in predefTable */
    Int max_sym;           /* max symbol value in FSE */
    Int max_bits;          /* max bits for FSE */

    /* toBaseline converts from an FSE table to an FSE baseline table. */
    ZsToBaseline to_baseline;
} ZsSeqCodeInfo;

static const ZsSeqCodeInfo zs_seq_code_info[3] = {
    {zs_predef_literal, 64, 6, 35, 9, zs_literal_baseline},
    {zs_predef_offset, 32, 5, 31, 8, zs_offset_baseline},
    {zs_predef_match, 64, 6, 52, 9, zs_match_baseline},
};

/* setSeqTable: uses mode to set the table for kind, reading a table
 * description at off when there is one. Returns the new offset. */
static Error zs_set_seq_table(ZstdReader *r, ZsData data, Int off, int kind, Byte mode,
                              Int *roff) {
    const ZsSeqCodeInfo *info = &zs_seq_code_info[kind];
    switch (mode) {
    case 0:
        /* Predefined_Mode */
        r->seq_tables[kind] = info->predef_table;
        r->seq_table_lens[kind] = info->predef_table_len;
        r->seq_table_bits[kind] = (uint8_t)info->predef_table_bits;
        *roff = off;
        return BURROW_NO_ERROR;

    case 1: {
        /* RLE_Mode */
        if (off >= data.len)
            return zs_make_eof_error(r, off);
        Byte rle = data.p[off];
        off++;

        /* Build a simple baseline table that always returns rle. */
        ZsFseEntry entry = {rle, 0, 0};
        Error e = info->to_baseline(r, off, &entry, 1, r->seq_table_buffers[kind]);
        if (BURROW_FAILED(e))
            return e;
        r->seq_tables[kind] = r->seq_table_buffers[kind];
        r->seq_table_lens[kind] = 1;
        r->seq_table_bits[kind] = 0;
        *roff = off;
        return BURROW_NO_ERROR;
    }

    case 2: {
        /* FSE_Compressed_Mode */
        Int table_bits = 0;
        Int noff = 0;
        Error e = zs_read_fse(r, data, off, info->max_sym, info->max_bits,
                              r->fse_scratch, &table_bits, &noff);
        if (BURROW_FAILED(e))
            return e;

        Int n = (Int)1 << table_bits;
        e = info->to_baseline(r, noff, r->fse_scratch, n, r->seq_table_buffers[kind]);
        if (BURROW_FAILED(e))
            return e;

        r->seq_tables[kind] = r->seq_table_buffers[kind];
        r->seq_table_lens[kind] = n;
        r->seq_table_bits[kind] = (uint8_t)table_bits;
        *roff = noff;
        return BURROW_NO_ERROR;
    }

    case 3:
        /* Repeat_Mode */
        if (r->seq_table_lens[kind] == 0)
            return zs_wrap_error(r, off, zs_err_repeat_table);
        *roff = off;
        return BURROW_NO_ERROR;

    default:
        break;
    }
    runtime_panic(BURROW_S("unreachable"));
}

/* initSeqs: reads the Sequences_Section_Header and sets up the FSE tables.
 * Returns the number of sequences and the new offset. */
static Error zs_init_seqs(ZstdReader *r, ZsData data, Int off, Int *seq_count_out,
                          Int *roff) {
    if (off >= data.len)
        return zs_make_eof_error(r, off);

    Byte seq_hdr = data.p[off];
    off++;
    if (seq_hdr == 0) {
        *seq_count_out = 0;
        *roff = off;
        return BURROW_NO_ERROR;
    }

    Int seq_count = 0;
    if (seq_hdr < 128) {
        seq_count = (Int)seq_hdr;
    } else if (seq_hdr < 255) {
        if (off >= data.len)
            return zs_make_eof_error(r, off);
        seq_count = (((Int)seq_hdr - 128) << 8) + (Int)data.p[off];
        off++;
    } else {
        if (off + 1 >= data.len)
            return zs_make_eof_error(r, off);
        seq_count = (Int)data.p[off] + ((Int)data.p[off + 1] << 8) + 0x7f00;
        off += 2;
    }

    /* Read the Symbol_Compression_Modes byte. */

    if (off >= data.len)
        return zs_make_eof_error(r, off);
    Byte sym_mode = data.p[off];
    if ((sym_mode & 3) != 0)
        return zs_wrap_error(r, off, zs_err_sym_mode);
    off++;

    /* Set up the FSE tables used to decode the sequence codes. */

    Error e = zs_set_seq_table(r, data, off, ZS_SEQ_LITERAL, (sym_mode >> 6) & 3, &off);
    if (BURROW_FAILED(e))
        return e;

    e = zs_set_seq_table(r, data, off, ZS_SEQ_OFFSET, (sym_mode >> 4) & 3, &off);
    if (BURROW_FAILED(e))
        return e;

    e = zs_set_seq_table(r, data, off, ZS_SEQ_MATCH, (sym_mode >> 2) & 3, &off);
    if (BURROW_FAILED(e))
        return e;

    *seq_count_out = seq_count;
    *roff = off;
    return BURROW_NO_ERROR;
}

/* The entry for state in the table for kind. */
static const ZsBaseline *zs_seq_entry(const ZstdReader *r, int kind, uint32_t state) {
    if ((Int)state >= r->seq_table_lens[kind])
        runtime_index_out_of_range((Int)state, r->seq_table_lens[kind]);
    return &r->seq_tables[kind][state];
}

/* copyFromWindow: copies match bytes from offset back in the output, which
 * may reach into the window of earlier blocks. */
static Error zs_copy_from_window(ZstdReader *r, const ZsRevBitReader *rbr,
                                 uint32_t offset, uint32_t match) {
    if (offset == 0)
        return zs_rbr_make_error(rbr, zs_err_zero_offset);

    /* Offset may point into the buffer or the window and match may extend
     * past the end of the initial buffer. */

    uint32_t buffer_offset = 0;
    uint32_t len_block = (uint32_t)r->buffer.len;
    if (len_block < offset) {
        uint32_t len_window = (uint32_t)r->window.data.len;
        uint32_t copy = offset - len_block;
        if (copy > len_window)
            return zs_rbr_make_error(rbr, zs_err_past_window);
        uint32_t window_offset = len_window - copy;
        if (copy > match)
            copy = match;
        if (!zs_window_append_to(r->a, &r->window, &r->buffer, window_offset,
                                 window_offset + copy))
            return burrow_err_out_of_memory;
        match -= copy;
    } else {
        buffer_offset = len_block - offset;
    }

    /* We are being asked to copy data that we are adding to the buffer in the
     * same copy. */
    while (match > 0) {
        uint32_t copy = (uint32_t)r->buffer.len - buffer_offset;
        if (copy > match)
            copy = match;
        if (!zs_buf_reserve(r->a, &r->buffer, r->buffer.len + (Int)copy))
            return burrow_err_out_of_memory;
        memcpy(r->buffer.p + r->buffer.len, r->buffer.p + buffer_offset, copy);
        r->buffer.len += (Int)copy;
        match -= copy;
    }
    return BURROW_NO_ERROR;
}

/* execSeqs: reads and executes the sequences, appending the output to
 * r->buffer. */
static Error zs_exec_seqs(ZstdReader *r, ZsData data, Int off, Int seq_count) {
    /* Set up the initial states for the sequence code readers. */

    ZsRevBitReader rbr;
    Error e = zs_make_rev_bit_reader(r, data, data.len - 1, off, &rbr);
    if (BURROW_FAILED(e))
        return e;

    uint32_t literal_state = 0;
    uint32_t offset_state = 0;
    uint32_t match_state = 0;
    e = zs_rbr_val(&rbr, r->seq_table_bits[ZS_SEQ_LITERAL], &literal_state);
    if (BURROW_FAILED(e))
        return e;
    e = zs_rbr_val(&rbr, r->seq_table_bits[ZS_SEQ_OFFSET], &offset_state);
    if (BURROW_FAILED(e))
        return e;
    e = zs_rbr_val(&rbr, r->seq_table_bits[ZS_SEQ_MATCH], &match_state);
    if (BURROW_FAILED(e))
        return e;

    /* Read and perform all the sequences. RFC 4.1.4. */

    const Byte *litbuf = r->literals.p;
    Int litlen = r->literals.len;

    Int seq = 0;
    while (seq < seq_count) {
        if (r->buffer.len + litlen > ZS_MAX_BLOCK)
            return zs_rbr_make_error(&rbr, zs_err_uncompressed);

        const ZsBaseline *ptoffset = zs_seq_entry(r, ZS_SEQ_OFFSET, offset_state);
        const ZsBaseline *ptmatch = zs_seq_entry(r, ZS_SEQ_MATCH, match_state);
        const ZsBaseline *ptliteral = zs_seq_entry(r, ZS_SEQ_LITERAL, literal_state);

        uint32_t add = 0;
        e = zs_rbr_val(&rbr, ptoffset->basebits, &add);
        if (BURROW_FAILED(e))
            return e;
        uint32_t offset = ptoffset->baseline + add;

        e = zs_rbr_val(&rbr, ptmatch->basebits, &add);
        if (BURROW_FAILED(e))
            return e;
        uint32_t match = ptmatch->baseline + add;

        e = zs_rbr_val(&rbr, ptliteral->basebits, &add);
        if (BURROW_FAILED(e))
            return e;
        uint32_t literal = ptliteral->baseline + add;

        /* Handle repeat offsets. RFC 3.1.1.5. See the comment in
         * makeOffsetBaselineFSE. */
        if (ptoffset->basebits > 1) {
            r->repeated_offset3 = r->repeated_offset2;
            r->repeated_offset2 = r->repeated_offset1;
            r->repeated_offset1 = offset;
        } else {
            if (literal == 0)
                offset++;
            switch (offset) {
            case 1:
                offset = r->repeated_offset1;
                break;
            case 2:
                offset = r->repeated_offset2;
                r->repeated_offset2 = r->repeated_offset1;
                r->repeated_offset1 = offset;
                break;
            case 3:
                offset = r->repeated_offset3;
                r->repeated_offset3 = r->repeated_offset2;
                r->repeated_offset2 = r->repeated_offset1;
                r->repeated_offset1 = offset;
                break;
            case 4:
                offset = r->repeated_offset1 - 1;
                r->repeated_offset3 = r->repeated_offset2;
                r->repeated_offset2 = r->repeated_offset1;
                r->repeated_offset1 = offset;
                break;
            default:
                break;
            }
        }

        seq++;
        if (seq < seq_count) {
            /* Update the states. */
            e = zs_rbr_val(&rbr, ptliteral->bits, &add);
            if (BURROW_FAILED(e))
                return e;
            literal_state = (uint32_t)ptliteral->base + add;

            e = zs_rbr_val(&rbr, ptmatch->bits, &add);
            if (BURROW_FAILED(e))
                return e;
            match_state = (uint32_t)ptmatch->base + add;

            e = zs_rbr_val(&rbr, ptoffset->bits, &add);
            if (BURROW_FAILED(e))
                return e;
            offset_state = (uint32_t)ptoffset->base + add;
        }

        /* The next sequence is now in literal, offset, match. */

        if ((Int)literal > litlen)
            return zs_rbr_make_error(&rbr, zs_err_literal_overflow);
        if (literal > 0) {
            if (!zs_buf_append(r->a, &r->buffer, litbuf, (Int)literal))
                return burrow_err_out_of_memory;
            litbuf += literal;
            litlen -= (Int)literal;
        }

        if (match > 0) {
            e = zs_copy_from_window(r, &rbr, offset, match);
            if (BURROW_FAILED(e))
                return e;
        }
    }

    if (!zs_buf_append(r->a, &r->buffer, litbuf, litlen))
        return burrow_err_out_of_memory;

    if (rbr.cnt != 0)
        return zs_wrap_error(r, off, zs_err_after_seqs);

    return BURROW_NO_ERROR;
}

/* compressedBlock: decompresses a compressed block of blockSize bytes,
 * storing the result in r->buffer. */
static Error zs_compressed_block(ZstdReader *r, Int block_size) {
    if (!zs_buf_reserve(r->a, &r->compressed_buf, block_size))
        return burrow_err_out_of_memory;
    r->compressed_buf.len = block_size;
    Error e = BURROW_NO_ERROR;
    Slice cb = {r->compressed_buf.p, block_size, block_size, TYPE_BYTE};
    io_read_full(r->r, cb, &e);
    if (BURROW_FAILED(e))
        return zs_wrap_non_eof_error(r, 0, e);

    ZsData data = {r->compressed_buf.p, block_size};
    Int off = 0;
    r->buffer.len = 0;

    r->literals.len = 0;
    e = zs_read_literals(r, data, off, &r->literals, &off);
    if (BURROW_FAILED(e))
        return e;

    Int seq_count = 0;
    e = zs_init_seqs(r, data, off, &seq_count, &off);
    if (BURROW_FAILED(e))
        return e;

    if (seq_count == 0) {
        /* No sequences, just literals. */
        if (off < data.len)
            return zs_wrap_error(r, off, zs_err_no_seqs);

        if (!zs_buf_append(r->a, &r->buffer, r->literals.p, r->literals.len))
            return burrow_err_out_of_memory;

        return BURROW_NO_ERROR;
    }

    return zs_exec_seqs(r, data, off, seq_count);
}

/* ------------------------------------------------------------------ frames */

/* setBufferSize: sets the decompressed buffer size. What it adds is zeroed,
 * as in Go. */
static bool zs_set_buffer_size(ZstdReader *r, Int size) {
    if (!zs_buf_reserve(r->a, &r->buffer, size))
        return false;
    r->buffer.len = size;
    return true;
}

/* io.ReadFull into the first n bytes of the scratch space. */
static Error zs_read_scratch(ZstdReader *r, Int n) {
    Error e = BURROW_NO_ERROR;
    Slice s = {r->scratch, n, n, TYPE_BYTE};
    io_read_full(r->r, s, &e);
    return e;
}

/* skipFrame: skips a skippable frame. RFC 3.1.2. */
static Error zs_skip_frame(ZstdReader *r) {
    Int relative_offset = 0;

    Error e = zs_read_scratch(r, 4);
    if (BURROW_FAILED(e))
        return zs_wrap_non_eof_error(r, relative_offset, e);

    relative_offset += 4;

    uint32_t size = zs_le32(r->scratch);
    if (size == 0) {
        r->block_offset += (int64_t)relative_offset;
        return BURROW_NO_ERROR;
    }

    const Method *seek = burrow__io_seek_method(r->r.vt->self_type);
    if (seek != NULL) {
        r->block_offset += (int64_t)relative_offset;
        int64_t prev = burrow__io_seek(seek, r->r.data, 0, BURROW_IO_SEEK_CURRENT, &e);
        if (BURROW_FAILED(e))
            return zs_wrap_error(r, 0, e);
        int64_t end = burrow__io_seek(seek, r->r.data, 0, BURROW_IO_SEEK_END, &e);
        if (BURROW_FAILED(e))
            return zs_wrap_error(r, 0, e);
        if (prev > end - (int64_t)size) {
            r->block_offset += end - prev;
            return zs_make_eof_error(r, 0);
        }
        burrow__io_seek(seek, r->r.data, prev + (int64_t)size, BURROW_IO_SEEK_START,
                        &e);
        if (BURROW_FAILED(e))
            return zs_wrap_error(r, 0, e);
        r->block_offset += (int64_t)size;
        return BURROW_NO_ERROR;
    }

    int64_t n = io_copy_n(r->a, io_discard, r->r, (int64_t)size, &e);
    relative_offset += (Int)n;
    if (BURROW_FAILED(e))
        return zs_wrap_non_eof_error(r, relative_offset, e);
    r->block_offset += (int64_t)relative_offset;
    return BURROW_NO_ERROR;
}

/* readFrameHeader: reads the frame header and prepares to read a block. */
static Error zs_read_frame_header(ZstdReader *r) {
    for (;;) {
        Int relative_offset = 0;

        /* Read magic number. RFC 3.1.1. */
        Error e = zs_read_scratch(r, 4);
        if (BURROW_FAILED(e)) {
            /* We require that the stream contains at least one frame. */
            if (zs_same_error(e, io_eof) && !r->read_one_frame)
                e = io_err_unexpected_eof;
            return zs_wrap_error(r, relative_offset, e);
        }

        uint32_t magic = zs_le32(r->scratch);
        if (magic != 0xfd2fb528U) {
            if (magic >= 0x184d2a50U && magic <= 0x184d2a5fU) {
                /* This is a skippable frame. */
                r->block_offset += (int64_t)relative_offset + 4;
                e = zs_skip_frame(r);
                if (BURROW_FAILED(e))
                    return e;
                r->read_one_frame = true;
                continue;
            }

            return zs_wrap_error(r, relative_offset, zs_err_magic);
        }

        relative_offset += 4;

        /* Read Frame_Header_Descriptor. RFC 3.1.1.1.1. */
        e = zs_read_scratch(r, 1);
        if (BURROW_FAILED(e))
            return zs_wrap_non_eof_error(r, relative_offset, e);
        Byte descriptor = r->scratch[0];

        bool single_segment = (descriptor & (1 << 5)) != 0;

        Int fcs_field_size = (Int)1 << (descriptor >> 6);
        if (fcs_field_size == 1 && !single_segment)
            fcs_field_size = 0;

        Int window_descriptor_size = single_segment ? 0 : 1;

        if ((descriptor & (1 << 3)) != 0)
            return zs_wrap_error(r, relative_offset, zs_err_reserved_bit);

        r->has_checksum = (descriptor & (1 << 2)) != 0;
        if (r->has_checksum)
            zs_xxh_reset(&r->checksum);

        /* Dictionary_ID_Flag. RFC 3.1.1.1.1.6. */
        Int dictionary_id_size = 0;
        Byte dict_id_flag = descriptor & 3;
        if (dict_id_flag != 0)
            dictionary_id_size = (Int)1 << (dict_id_flag - 1);

        relative_offset++;

        Int header_size = window_descriptor_size + dictionary_id_size + fcs_field_size;

        e = zs_read_scratch(r, header_size);
        if (BURROW_FAILED(e))
            return zs_wrap_non_eof_error(r, relative_offset, e);

        /* Figure out the maximum amount of data we need to retain for
         * backreferences. */
        uint64_t window_size = 0;
        if (!single_segment) {
            /* Window descriptor. RFC 3.1.1.1.2. */
            Byte window_descriptor = r->scratch[0];
            uint64_t exponent = (uint64_t)(window_descriptor >> 3);
            uint64_t mantissa = (uint64_t)(window_descriptor & 7);
            uint64_t window_log = exponent + 10;
            uint64_t window_base = (uint64_t)1 << window_log;
            uint64_t window_add = (window_base / 8) * mantissa;
            window_size = window_base + window_add;
        }

        /* Dictionary_ID. RFC 3.1.1.1.3. */
        if (dictionary_id_size != 0) {
            const Byte *dictionary_id = r->scratch + window_descriptor_size;
            /* Allow only zero Dictionary ID. */
            for (Int i = 0; i < dictionary_id_size; i++) {
                if (dictionary_id[i] != 0)
                    return zs_wrap_error(r, relative_offset, zs_err_dictionaries);
            }
        }

        /* Frame_Content_Size. RFC 3.1.1.1.4. */
        r->frame_size_unknown = false;
        r->remaining_frame_size = 0;
        const Byte *fb = r->scratch + window_descriptor_size + dictionary_id_size;
        switch (fcs_field_size) {
        case 0:
            r->frame_size_unknown = true;
            break;
        case 1:
            r->remaining_frame_size = (uint64_t)fb[0];
            break;
        case 2:
            r->remaining_frame_size = 256 + (uint64_t)zs_le16(fb);
            break;
        case 4:
            r->remaining_frame_size = (uint64_t)zs_le32(fb);
            break;
        case 8:
            r->remaining_frame_size = zs_le64(fb);
            break;
        default:
            runtime_panic(BURROW_S("unreachable"));
        }

        /* RFC 3.1.1.1.2. When Single_Segment_Flag is set, Window_Descriptor
         * is not present. In this case, Window_Size is Frame_Content_Size. */
        if (single_segment)
            window_size = r->remaining_frame_size;

        /* RFC 8878 3.1.1.1.1.2. Permits us to set an 8M max on window size. */
        if (window_size > ZS_MAX_WINDOW)
            window_size = ZS_MAX_WINDOW;

        /* Go makes the window after the fields below are set, and nothing
         * there can fail. Here making it can, so it goes first. */
        if (!zs_window_reset(r->a, &r->window, (Int)window_size))
            return burrow_err_out_of_memory;

        relative_offset += header_size;

        r->saw_frame_header = true;
        r->read_one_frame = true;
        r->block_offset += (int64_t)relative_offset;

        /* Prepare to read blocks from the frame. */
        r->repeated_offset1 = 1;
        r->repeated_offset2 = 4;
        r->repeated_offset3 = 8;
        r->huffman_table_bits = 0;
        r->seq_table_lens[0] = 0;
        r->seq_table_lens[1] = 0;
        r->seq_table_lens[2] = 0;

        return BURROW_NO_ERROR;
    }
}

/* readBlock: reads the next block from a frame. */
static Error zs_read_block(ZstdReader *r) {
    Int relative_offset = 0;

    /* Read Block_Header. RFC 3.1.1.2. */
    Error e = zs_read_scratch(r, 3);
    if (BURROW_FAILED(e))
        return zs_wrap_non_eof_error(r, relative_offset, e);

    relative_offset += 3;

    uint32_t header = (uint32_t)r->scratch[0] | ((uint32_t)r->scratch[1] << 8) |
                      ((uint32_t)r->scratch[2] << 16);

    bool last_block = (header & 1) != 0;
    uint32_t block_type = (header >> 1) & 3;
    Int block_size = (Int)(header >> 3);

    /* Maximum block size is smaller of window size and 128K. We don't record
     * the window size for a single segment frame, so just use 128K. RFC
     * 3.1.1.2.3, 3.1.1.2.4. */
    if (block_size > ZS_MAX_BLOCK ||
        (r->window.size > 0 && block_size > r->window.size))
        return zs_wrap_error(r, relative_offset, zs_err_block_size);

    /* Handle different block types. RFC 3.1.1.2.2. */
    switch (block_type) {
    case 0: {
        if (!zs_set_buffer_size(r, block_size))
            return burrow_err_out_of_memory;
        Slice b = {r->buffer.p, block_size, block_size, TYPE_BYTE};
        io_read_full(r->r, b, &e);
        if (BURROW_FAILED(e))
            return zs_wrap_non_eof_error(r, relative_offset, e);
        relative_offset += block_size;
        r->block_offset += (int64_t)relative_offset;
        break;
    }
    case 1: {
        if (!zs_set_buffer_size(r, block_size))
            return burrow_err_out_of_memory;
        e = zs_read_scratch(r, 1);
        if (BURROW_FAILED(e))
            return zs_wrap_non_eof_error(r, relative_offset, e);
        relative_offset++;
        if (block_size > 0)
            memset(r->buffer.p, r->scratch[0], (size_t)block_size);
        r->block_offset += (int64_t)relative_offset;
        break;
    }
    case 2:
        r->block_offset += (int64_t)relative_offset;
        e = zs_compressed_block(r, block_size);
        if (BURROW_FAILED(e))
            return e;
        r->block_offset += (int64_t)block_size;
        break;
    case 3:
        return zs_wrap_error(r, relative_offset, zs_err_block_type);
    default:
        break;
    }

    if (!r->frame_size_unknown) {
        if ((uint64_t)r->buffer.len > r->remaining_frame_size)
            return zs_wrap_error(r, relative_offset, zs_err_too_many_bytes);
        r->remaining_frame_size -= (uint64_t)r->buffer.len;
    }

    if (r->has_checksum)
        zs_xxh_update(&r->checksum, r->buffer.p, r->buffer.len);

    if (!last_block) {
        zs_window_save(&r->window, r->buffer.p, r->buffer.len);
    } else {
        if (!r->frame_size_unknown && r->remaining_frame_size != 0)
            return zs_wrap_error(r, relative_offset, zs_err_not_enough_bytes);
        /* Check for checksum at end of frame. RFC 3.1.1. */
        if (r->has_checksum) {
            e = zs_read_scratch(r, 4);
            if (BURROW_FAILED(e))
                return zs_wrap_non_eof_error(r, 0, e);

            uint32_t input_checksum = zs_le32(r->scratch);
            uint32_t data_checksum = (uint32_t)zs_xxh_digest(&r->checksum);
            if (input_checksum != data_checksum) {
                Error ce = fmt_errorf_v("invalid checksum: got %#x want %#x",
                                        data_checksum, input_checksum);
                return zs_wrap_error(r, 0, ce);
            }

            r->block_offset += 4;
        }
        r->saw_frame_header = false;
    }

    return BURROW_NO_ERROR;
}

/* refill: reads more data into the buffer. */
static Error zs_refill(ZstdReader *r) {
    if (!r->saw_frame_header) {
        Error e = zs_read_frame_header(r);
        if (BURROW_FAILED(e))
            return e;
    }
    return zs_read_block(r);
}

/* refillIfNeeded: reads the next block if necessary. */
static Error zs_refill_if_needed(ZstdReader *r) {
    while (r->off >= r->buffer.len) {
        Error e = zs_refill(r);
        if (BURROW_FAILED(e))
            return e;
        r->off = 0;
    }
    return BURROW_NO_ERROR;
}

/* ----------------------------------------------------------------- Reader */

static const Type zs_reader_desc = {
    {(const Byte *)"Reader", 6},
    {(const Byte *)"internal/zstd", 13},
    KIND_STRUCT,
    (uint32_t)sizeof(ZstdReader),
    (uint16_t)_Alignof(ZstdReader),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x7a737264U, /* "zsrd" */
    NULL,
};

ZstdReader *burrow__zstd_new_reader(Alloc *a, IoReader input) {
    ZstdReader *r = (ZstdReader *)mem_alloc(a, sizeof *r, _Alignof(ZstdReader));
    if (r == NULL)
        return NULL;
    r->a = a;
    burrow__zstd_reader_reset(r, input);
    return r;
}

void burrow__zstd_reader_reset(ZstdReader *r, IoReader input) {
    r->r = input;

    /* Several fields are preserved to avoid allocation. Others are always set
     * before they are used. */
    r->saw_frame_header = false;
    r->has_checksum = false;
    r->read_one_frame = false;
    r->frame_size_unknown = false;
    r->remaining_frame_size = 0;
    r->block_offset = 0;
    r->buffer.len = 0;
    r->off = 0;
}

Int burrow__zstd_reader_read(ZstdReader *r, Slice p, Error *err) {
    Error e = zs_refill_if_needed(r);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return 0;
    }
    Int n = r->buffer.len - r->off;
    if (n > p.len)
        n = p.len;
    if (n > 0)
        memcpy(p.p, r->buffer.p + r->off, (size_t)n);
    r->off += n;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return n;
}

Byte burrow__zstd_reader_read_byte(ZstdReader *r, Error *err) {
    Error e = zs_refill_if_needed(r);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return 0;
    }
    Byte ret = r->buffer.p[r->off];
    r->off++;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return ret;
}

static Int zs_vt_read(void *self, Slice p, Error *err) {
    return burrow__zstd_reader_read((ZstdReader *)self, p, err);
}

static const IoReaderVT zs_reader_vt = {&zs_reader_desc, zs_vt_read};

IoReader burrow__zstd_reader_as_io_reader(ZstdReader *r) {
    IoReader ir = {&zs_reader_vt, r};
    return ir;
}

void burrow__zstd_reader_free(ZstdReader *r) {
    if (r == NULL)
        return;
    Alloc *a = r->a;
    zs_buf_free(a, &r->buffer);
    zs_buf_free(a, &r->window.data);
    zs_buf_free(a, &r->compressed_buf);
    zs_buf_free(a, &r->literals);
    mem_free(a, r, sizeof *r, _Alignof(ZstdReader));
}

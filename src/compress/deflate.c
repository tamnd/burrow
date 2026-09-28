/* compress/flate: the compressor, from deflate.go, deflatefast.go, level1.go
 * to level6.go, huffman_bit_writer.go, huffman_code.go and token.go.
 *
 * The shape is Go's. Levels 1 to 6 hand each block of up to 64 KiB to one of
 * six fast encoders that turn it into tokens with a hash table or two. Levels 7
 * to 9 run the older lazy matcher with hash chains over a 64 KiB window. Level
 * -2 only Huffman codes the bytes and level 0 only stores them. Every level
 * ends in the Huffman bit writer, which picks the smallest of a stored block, a
 * block with the fixed codes and one with codes of its own, and can keep using
 * the last block's codes when that looks cheaper than sending new ones.
 *
 * The output is the same as Go's byte for byte, which is why the float
 * estimates below are written one operation at a time with contraction off.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/compress/flate.h"

#include "burrow/math.h"
#include "burrow/math/bits.h"
#include "burrow/mem/heap.h"
#include "burrow/sync.h"

#include "flate_internal.h"

#include <string.h>

#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#elif defined(_MSC_VER)
#pragma fp_contract(off)
#endif

enum {
    DEFL_NO_COMPRESSION = 0,
    DEFL_HUFFMAN_ONLY = -2,
    DEFL_DEFAULT_COMPRESSION = -1,

    DEFL_LOG_WINDOW_SIZE = 15,
    DEFL_WINDOW_SIZE = 1 << DEFL_LOG_WINDOW_SIZE,
    DEFL_WINDOW_MASK = DEFL_WINDOW_SIZE - 1,
    DEFL_MIN_MATCH_LENGTH = 4,   /* the smallest match the compressor looks for */
    DEFL_MAX_MATCH_LENGTH = 258, /* the longest match for the compressor */
    DEFL_MIN_OFFSET_SIZE = 1,    /* the shortest offset that makes any sense */

    DEFL_MAX_FLATE_BLOCK_TOKENS = 1 << 15,
    DEFL_MAX_STORE_BLOCK_SIZE = 65535,
    DEFL_HASH_BITS = 17, /* after 17 performance degrades */
    DEFL_HASH_SIZE = 1 << DEFL_HASH_BITS,
    DEFL_HASH_MASK = (1 << DEFL_HASH_BITS) - 1,
    DEFL_MAX_HASH_OFFSET = 1 << 28,

    DEFL_TABLE_BITS = 15,
    DEFL_TABLE_SIZE = 1 << DEFL_TABLE_BITS,
    DEFL_L2_TABLE_BITS = 17,
    DEFL_L2_TABLE_SIZE = 1 << DEFL_L2_TABLE_BITS,
    DEFL_L3_TABLE_BITS = 16,
    DEFL_L3_TABLE_SIZE = 1 << DEFL_L3_TABLE_BITS,
    DEFL_HASH_LONG_BYTES = 7,
    DEFL_BASE_MATCH_OFFSET = 1,
    DEFL_BASE_MATCH_LENGTH = 3,
    DEFL_MAX_MATCH_OFFSET = 1 << 15,
    DEFL_ALLOC_HISTORY = DEFL_MAX_STORE_BLOCK_SIZE * 5,
    DEFL_BUFFER_RESET =
        (int32_t)((1U << 31) - DEFL_ALLOC_HISTORY - DEFL_MAX_STORE_BLOCK_SIZE - 1),
    DEFL_INPUT_MARGIN = 12 - 1,
    DEFL_MIN_NON_LITERAL_BLOCK_SIZE = 1 + 1 + DEFL_INPUT_MARGIN,

    DEFL_MAX_BITS_LIMIT = 16,
    DEFL_LITERAL_COUNT = 286,
    DEFL_OFFSET_CODE_COUNT = 30,
    DEFL_END_BLOCK_MARKER = 256,
    DEFL_LENGTH_CODES_START = 257,
    DEFL_CODEGEN_CODE_COUNT = 19,
    DEFL_BAD_CODE = 255,
    DEFL_MAX_PREDEFINED_TOKENS = 250,
    DEFL_BUFFER_FLUSH_SIZE = 246,
    DEFL_LENGTH_EXTRA_BITS_MIN_CODE = 8,
    DEFL_OFFSET_EXTRA_BITS_MIN_CODE = 4,
    DEFL_MAX_INT32 = 0x7fffffff,
};

#define DEFL_LENGTH_SHIFT 22
#define DEFL_OFFSET_MASK ((1U << DEFL_LENGTH_SHIFT) - 1)
#define DEFL_MATCH_TYPE (1U << 30)
#define DEFL_MATCH_OFFSET_ONLY_MASK 0xffffU

#define DEFL_PRIME3 506832829U
#define DEFL_PRIME4 2654435761U
#define DEFL_PRIME5 889523592379ULL
#define DEFL_PRIME6 227718039650203ULL
#define DEFL_PRIME7 58295818150454627ULL
#define DEFL_PRIME8 0xcf1bbcdcb7a56463ULL

static const uint8_t defl_length_codes[256] = {
    0,  1,  2,  3,  4,  5,  6,  7,  8,  8,  9,  9,  10, 10, 11, 11, 12, 12, 12, 12,
    13, 13, 13, 13, 14, 14, 14, 14, 15, 15, 15, 15, 16, 16, 16, 16, 16, 16, 16, 16,
    17, 17, 17, 17, 17, 17, 17, 17, 18, 18, 18, 18, 18, 18, 18, 18, 19, 19, 19, 19,
    19, 19, 19, 19, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20, 20,
    21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 22, 22, 22, 22,
    22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 23, 23, 23, 23, 23, 23, 23, 23,
    23, 23, 23, 23, 23, 23, 23, 23, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24,
    24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24,
    25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25,
    25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 26, 26, 26, 26, 26, 26, 26, 26,
    26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26,
    26, 26, 26, 26, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27,
    27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 28,
};

static const uint8_t defl_length_codes1[256] = {
    1,  2,  3,  4,  5,  6,  7,  8,  9,  9,  10, 10, 11, 11, 12, 12, 13, 13, 13, 13,
    14, 14, 14, 14, 15, 15, 15, 15, 16, 16, 16, 16, 17, 17, 17, 17, 17, 17, 17, 17,
    18, 18, 18, 18, 18, 18, 18, 18, 19, 19, 19, 19, 19, 19, 19, 19, 20, 20, 20, 20,
    20, 20, 20, 20, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
    22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 23, 23, 23, 23,
    23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 24, 24, 24, 24, 24, 24, 24, 24,
    24, 24, 24, 24, 24, 24, 24, 24, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25,
    25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25,
    26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26,
    26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 27, 27, 27, 27, 27, 27, 27, 27,
    27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27,
    27, 27, 27, 27, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28,
    28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 29,
};

static const uint32_t defl_offset_codes[256] = {
    0,  1,  2,  3,  4,  4,  5,  5,  6,  6,  6,  6,  7,  7,  7,  7,  8,  8,  8,  8,
    8,  8,  8,  8,  9,  9,  9,  9,  9,  9,  9,  9,  10, 10, 10, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11,
    11, 11, 11, 11, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 13, 13, 13, 13,
    13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13,
    13, 13, 13, 13, 13, 13, 13, 13, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14,
    14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14,
    14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14,
    14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 15, 15, 15, 15, 15, 15, 15, 15,
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
};

static const uint32_t defl_offset_codes14[256] = {
    14, 15, 16, 17, 18, 18, 19, 19, 20, 20, 20, 20, 21, 21, 21, 21, 22, 22, 22, 22,
    22, 22, 22, 22, 23, 23, 23, 23, 23, 23, 23, 23, 24, 24, 24, 24, 24, 24, 24, 24,
    24, 24, 24, 24, 24, 24, 24, 24, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25,
    25, 25, 25, 25, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26,
    26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 27, 27, 27, 27,
    27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27,
    27, 27, 27, 27, 27, 27, 27, 27, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28,
    28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28,
    28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28,
    28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
};

static const uint8_t defl_length_extra_bits[32] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
    2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0,
};

static const uint8_t defl_length_base[32] = {
    0,  1,  2,  3,  4,  5,  6,  7,  8,   10,  12,  14,  16,  20,  24,
    28, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 255,
};

static const int8_t defl_offset_extra_bits[32] = {
    0, 0, 0, 0, 1, 1, 2,  2,  3,  3,  4,  4,  5,  5,  6,  6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13, 14, 14,
};

static const uint32_t defl_offset_combined[32] = {
    0x0,      0x0,      0x0,      0x0,      0x401,    0x601,    0x802,   0xc02,
    0x1003,   0x1803,   0x2004,   0x3004,   0x4005,   0x6005,   0x8006,  0xc006,
    0x10007,  0x18007,  0x20008,  0x30008,  0x40009,  0x60009,  0x8000a, 0xc000a,
    0x10000b, 0x18000b, 0x20000c, 0x30000c, 0x40000d, 0x60000d, 0x0,     0x0,
};

static const uint32_t defl_codegen_order[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15,
};

/* ------------------------------------------------------------------ tokens
 *
 * A token is a literal byte below 256, or a match. A match has
 * DEFL_MATCH_TYPE set, the length less 3 in bits 22 to 29, the offset code in
 * bits 16 to 21 and the offset less 1 in bits 0 to 15. The histograms are kept
 * as the tokens go in, so a block knows its symbol counts without a pass over
 * them. */

typedef uint32_t DeflToken;

typedef struct DeflTokens {
    uint16_t extra_hist[32]; /* codes 256 to the last length code */
    uint16_t off_hist[32];   /* offset codes */
    uint16_t lit_hist[256];  /* codes 0 to 255 */
    Int n_filled;
    uint16_t n; /* must be able to hold DEFL_MAX_STORE_BLOCK_SIZE */
    DeflToken tokens[65536];
} DeflTokens;

static void defl_tokens_reset(DeflTokens *t) {
    if (t->n == 0)
        return;
    t->n = 0;
    t->n_filled = 0;
    memset(t->lit_hist, 0, sizeof t->lit_hist);
    memset(t->extra_hist, 0, sizeof t->extra_hist);
    memset(t->off_hist, 0, sizeof t->off_hist);
}

static uint32_t defl_offset_code(uint32_t off) {
    if (off < 256)
        return defl_offset_codes[(uint8_t)off];
    return defl_offset_codes14[(uint8_t)(off >> 7)];
}

static void defl_emit_literals(DeflTokens *dst, const Byte *lit, Int n) {
    for (Int i = 0; i < n; i++) {
        Byte v = lit[i];
        dst->tokens[dst->n] = v;
        dst->lit_hist[v]++;
        dst->n++;
    }
}

static void defl_tokens_add_literal(DeflTokens *t, Byte lit) {
    t->tokens[t->n] = lit;
    t->lit_hist[lit]++;
    t->n++;
}

/* log2 of val, near enough for guessing the size of a block. */
static float defl_fast_log2(float val) {
    int32_t ux = (int32_t)math_float32bits(val);
    float log2 = (float)(((ux >> 23) & 255) - 128);
    ux &= -0x7f800001;
    ux += 127 << 23;
    float uval = math_float32frombits((uint32_t)ux);
    float t = -0.34484843f * uval;
    t = t + 2.02466578f;
    t = t * uval;
    t = t - 0.67487759f;
    log2 += t;
    return log2;
}

/* The bits one symbol seen n times out of every 1/inv_total costs. */
static float defl_shannon_bits(float n, float inv_total) {
    float p = n * inv_total;
    float b = -defl_fast_log2(p);
    if (b < 1)
        b = 1;
    if (b > 15)
        b = 15;
    return b * n;
}

/* A guess at how many bits the tokens take coded with a table of their own. */
static Int defl_tokens_estimated_bits(const DeflTokens *t) {
    float shannon = 0;
    Int bits = 0;
    Int n_matches = 0;
    Int total = (Int)t->n + t->n_filled;
    if (total > 0) {
        float inv_total = 1.0f / (float)total;
        for (int i = 0; i < 256; i++) {
            uint16_t v = t->lit_hist[i];
            if (v > 0)
                shannon += defl_shannon_bits((float)v, inv_total);
        }
        /* Just add 15 for EOB. */
        shannon += 15;
        for (int i = 0; i < DEFL_LITERAL_COUNT - 256 - 1; i++) {
            uint16_t v = t->extra_hist[1 + i];
            if (v > 0) {
                shannon += defl_shannon_bits((float)v, inv_total);
                bits += (Int)defl_length_extra_bits[i & 31] * (Int)v;
                n_matches += (Int)v;
            }
        }
    }
    if (n_matches > 0) {
        float inv_total = 1.0f / (float)n_matches;
        for (int i = 0; i < DEFL_OFFSET_CODE_COUNT; i++) {
            uint16_t v = t->off_hist[i];
            if (v > 0) {
                shannon += defl_shannon_bits((float)v, inv_total);
                bits += (Int)defl_offset_extra_bits[i & 31] * (Int)v;
            }
        }
    }
    return (Int)shannon + bits;
}

/* Adds a match of xlength (less 3) at xoffset (less 1). */
static void defl_tokens_add_match(DeflTokens *t, uint32_t xlength, uint32_t xoffset) {
    uint32_t oc = defl_offset_code(xoffset);
    xoffset |= oc << 16;
    t->extra_hist[defl_length_codes1[(uint8_t)xlength]]++;
    t->off_hist[oc & 31]++;
    t->tokens[t->n] = DEFL_MATCH_TYPE | xlength << DEFL_LENGTH_SHIFT | xoffset;
    t->n++;
}

/* Adds a match of any length, split into matches of 258 or less. xlength is
 * the whole length here, not less 3. */
static void defl_tokens_add_match_long(DeflTokens *t, int32_t xlength,
                                       uint32_t xoffset) {
    uint32_t oc = defl_offset_code(xoffset);
    xoffset |= oc << 16;
    while (xlength > 0) {
        int32_t xl = xlength;
        if (xl > 258) {
            /* We need to have at least DEFL_BASE_MATCH_LENGTH left over for
             * the next loop. */
            if (xl > 258 + DEFL_BASE_MATCH_LENGTH)
                xl = 258;
            else
                xl = 258 - DEFL_BASE_MATCH_LENGTH;
        }
        xlength -= xl;
        xl -= DEFL_BASE_MATCH_LENGTH;
        t->extra_hist[defl_length_codes1[(uint8_t)xl]]++;
        t->off_hist[oc & 31]++;
        t->tokens[t->n] = DEFL_MATCH_TYPE | (uint32_t)xl << DEFL_LENGTH_SHIFT | xoffset;
        t->n++;
    }
}

static void defl_tokens_add_eob(DeflTokens *t) {
    t->tokens[t->n] = DEFL_END_BLOCK_MARKER;
    t->extra_hist[0]++;
    t->n++;
}

/* ---------------------------------------------------------- huffman codes
 *
 * An hcode is the code length in the low 8 bits and the bit-reversed code
 * above it, ready to be ORed into the bit buffer. */

typedef uint32_t DeflHcode;

static inline uint8_t defl_hcode_len(DeflHcode h) {
    return (uint8_t)h;
}
static inline uint64_t defl_hcode_code64(DeflHcode h) {
    return (uint64_t)(h >> 8);
}
static inline DeflHcode defl_new_hcode(uint16_t code, uint8_t length) {
    return (DeflHcode)length | (DeflHcode)code << 8;
}

typedef struct DeflLitNode {
    uint16_t literal;
    uint16_t freq;
} DeflLitNode;

/* Go's codes slice has a capacity of the next power of two above its length,
 * 512 for literals, and the token writer reads up to index 257+31. Codes past
 * the last symbol a block used are never cleared, as in Go. */
typedef struct DeflHuffEnc {
    DeflHcode codes[320];
    int32_t bit_count[17];
    /* Allocate a reusable buffer with the longest possible frequency table:
     * DEFL_LITERAL_COUNT plus the sentinel. */
    DeflLitNode freqcache[DEFL_LITERAL_COUNT + 1];
} DeflHuffEnc;

typedef struct DeflLevelInfo {
    int32_t level;          /* our level, for better printing */
    int32_t last_freq;      /* the frequency of the last node at this level */
    int32_t next_char_freq; /* the frequency of the next character to add */
    int32_t next_pair_freq; /* the frequency of the next pair from the level above */
    int32_t needed;         /* how many chains this level needs to fill in */
} DeflLevelInfo;

static uint16_t defl_reverse_bits(uint16_t x, uint8_t b) {
    return bits_reverse16((uint16_t)(x << ((16 - b) & 15)));
}

/* Both keys are unique, so any sort gives the order Go's does. */
static uint32_t defl_node_key(DeflLitNode n, bool by_freq) {
    return by_freq ? (uint32_t)n.freq << 10 | n.literal : n.literal;
}

static void defl_sift_down(DeflLitNode *a, Int root, Int n, bool by_freq) {
    for (;;) {
        Int child = 2 * root + 1;
        if (child >= n)
            return;
        if (child + 1 < n &&
            defl_node_key(a[child], by_freq) < defl_node_key(a[child + 1], by_freq))
            child++;
        if (defl_node_key(a[root], by_freq) >= defl_node_key(a[child], by_freq))
            return;
        DeflLitNode t = a[root];
        a[root] = a[child];
        a[child] = t;
        root = child;
    }
}

static void defl_sort_nodes(DeflLitNode *a, Int n, bool by_freq) {
    for (Int i = n / 2 - 1; i >= 0; i--)
        defl_sift_down(a, i, n, by_freq);
    for (Int i = n - 1; i > 0; i--) {
        DeflLitNode t = a[0];
        a[0] = a[i];
        a[i] = t;
        defl_sift_down(a, 0, i, by_freq);
    }
}

static DeflHuffEnc defl_fixed_lit_enc;
static DeflHuffEnc defl_fixed_off_enc;
static DeflHuffEnc defl_huff_off_enc;
static SyncOnce defl_fixed_once;

static void defl_generate(DeflHuffEnc *h, const uint16_t *freq, Int nfreq,
                          int32_t max_bits);

static void defl_init_fixed(void *arg) {
    (void)arg;
    /* The fixed literal table of RFC 1951 3.2.6. */
    for (uint16_t ch = 0; ch < DEFL_LITERAL_COUNT; ch++) {
        uint16_t bits;
        uint8_t size;
        if (ch < 144) {
            bits = (uint16_t)(ch + 48);
            size = 8;
        } else if (ch < 256) {
            bits = (uint16_t)(ch + 400 - 144);
            size = 9;
        } else if (ch < 280) {
            bits = (uint16_t)(ch - 256);
            size = 7;
        } else {
            bits = (uint16_t)(ch + 192 - 280);
            size = 8;
        }
        defl_fixed_lit_enc.codes[ch] =
            defl_new_hcode(defl_reverse_bits(bits, size), size);
    }
    /* The fixed offset table: five bits each. */
    for (uint16_t ch = 0; ch < DEFL_OFFSET_CODE_COUNT; ch++)
        defl_fixed_off_enc.codes[ch] = defl_new_hcode(defl_reverse_bits(ch, 5), 5);
    /* The offset table huffman-only blocks send, with only offset 0 in it. */
    uint16_t off_freq[DEFL_OFFSET_CODE_COUNT] = {1};
    defl_generate(&defl_huff_off_enc, off_freq, DEFL_OFFSET_CODE_COUNT, 15);
}

static void defl_fixed(void) {
    sync_once_do(&defl_fixed_once, BURROW_FN(Func, defl_init_fixed, NULL));
}

static Int defl_bit_length(const DeflHuffEnc *h, const uint16_t *freq, Int n) {
    Int total = 0;
    for (Int i = 0; i < n; i++)
        if (freq[i] != 0)
            total += (Int)freq[i] * (Int)defl_hcode_len(h->codes[i]);
    return total;
}

/* The bits b takes with h, counting a symbol with no code as 1. */
static Int defl_bit_length_raw(const DeflHuffEnc *h, const Byte *b, Int n) {
    Int total = 0;
    for (Int i = 0; i < n; i++) {
        Int l = (Int)defl_hcode_len(h->codes[b[i]]);
        total += l > 1 ? l : 1;
    }
    return total;
}

/* The bits freq takes with h, or DEFL_MAX_INT32 when h has no code for one of
 * its symbols. */
static Int defl_can_encode_len(const DeflHuffEnc *h, const uint16_t *freq, Int n) {
    Int total = 0;
    for (Int i = 0; i < n; i++) {
        if (freq[i] != 0) {
            DeflHcode code = h->codes[i];
            if (code == 0)
                return DEFL_MAX_INT32;
            total += (Int)freq[i] * (Int)defl_hcode_len(code);
        }
    }
    return total;
}

/* Works out how many codes of each length the n literals in list should get,
 * none longer than max_bits, with the package-merge algorithm. list is sorted
 * by frequency and has room for a sentinel after its n entries. The result is
 * in h->bit_count[1 .. max_bits]. */
static int32_t defl_bit_counts(DeflHuffEnc *h, DeflLitNode *list, int32_t n,
                               int32_t max_bits) {
    list[n] = (DeflLitNode){UINT16_MAX, UINT16_MAX};

    /* The tree can't have greater depth than n - 1, no matter what. This
     * saves a little bit of work in some small cases. */
    if (max_bits > n - 1)
        max_bits = n - 1;

    /* Create information about each of the levels. A bogus "Level 0" whose
     * sole purpose is so that level1.prev.needed == 0. This makes level1.next_pair_freq
     * be a legitimate value that never gets chosen. */
    DeflLevelInfo levels[DEFL_MAX_BITS_LIMIT];
    memset(levels, 0, sizeof levels);
    /* leaf_counts[i] counts the number of literals at the left of
     * ancestors of the rightmost node at level i.
     * leaf_counts[i][j] is the number of literals at the left of the level j
     * ancestor. */
    int32_t leaf_counts[DEFL_MAX_BITS_LIMIT][DEFL_MAX_BITS_LIMIT];
    memset(leaf_counts, 0, sizeof leaf_counts);

    for (int32_t level = 1; level <= max_bits; level++) {
        /* For every level, the first two items are the first two characters.
         * We initialize the levels as if we had already figured this out. */
        levels[level] = (DeflLevelInfo){
            level,
            (int32_t)list[1].freq,
            (int32_t)list[2].freq,
            (int32_t)list[0].freq + (int32_t)list[1].freq,
            0,
        };
        leaf_counts[level][level] = 2;
        if (level == 1)
            levels[level].next_pair_freq = DEFL_MAX_INT32;
    }

    /* We need a total of 2*n - 2 items at top level and have already
     * generated 2. */
    levels[max_bits].needed = 2 * n - 4;

    uint32_t level = (uint32_t)max_bits;
    while (level < 16) {
        DeflLevelInfo *l = &levels[level];
        if (l->next_pair_freq == DEFL_MAX_INT32 &&
            l->next_char_freq == DEFL_MAX_INT32) {
            /* We've run out of both leaves and pairs. End all calculations
             * for this level. To make sure we never come back to this level
             * or any lower level, set next_pair_freq impossibly large. */
            l->needed = 0;
            levels[level + 1].next_pair_freq = DEFL_MAX_INT32;
            level++;
            continue;
        }

        int32_t prev_freq = l->last_freq;
        if (l->next_char_freq < l->next_pair_freq) {
            /* The next item on this row is a leaf node. */
            int32_t nn = leaf_counts[level][level] + 1;
            l->last_freq = l->next_char_freq;
            /* Lower leaf_counts are the same of the previous node. */
            leaf_counts[level][level] = nn;
            DeflLitNode e = list[nn];
            if (e.literal < UINT16_MAX)
                l->next_char_freq = (int32_t)e.freq;
            else
                l->next_char_freq = DEFL_MAX_INT32;
        } else {
            /* The next item on this row is a pair from the previous row.
             * next_pair_freq isn't valid until we generate two more values in
             * the level below. */
            l->last_freq = l->next_pair_freq;
            /* Take leaf counts from the lower level, except counts[level]
             * remains the same. */
            int32_t save = leaf_counts[level][level];
            memcpy(leaf_counts[level], leaf_counts[level - 1],
                   sizeof leaf_counts[level]);
            leaf_counts[level][level] = save;
            levels[l->level - 1].needed = 2;
        }

        if (--l->needed == 0) {
            /* We've done everything we need to do for this level. Continue
             * calculating one level up. Fill in next_pair_freq of that level
             * with the sum of the two nodes we've just calculated on this
             * level. */
            if (l->level == max_bits)
                /* All done! */
                break;
            levels[l->level + 1].next_pair_freq = prev_freq + l->last_freq;
            level++;
        } else {
            /* If we stole from below, move down temporarily to replenish
             * it. */
            while (levels[level - 1].needed > 0)
                level--;
        }
    }

    /* Something is wrong if at the end, the top level is null or hasn't used
     * all of the leaves. Go panics here, and so the tables it is given never
     * get here. */

    int32_t bits = 1;
    const int32_t *counts = leaf_counts[max_bits];
    for (int32_t lv = max_bits; lv > 0; lv--) {
        /* counts[lv] gives the number of literals requiring at least "bits"
         * bits to encode. */
        h->bit_count[bits] = counts[lv] - counts[lv - 1];
        bits++;
    }
    return max_bits;
}

/* Gives each literal in list a code, list being sorted by frequency and
 * bit_count[1 .. max_bits] saying how many get each length. */
static void defl_assign_encoding_and_size(DeflHuffEnc *h, int32_t max_bits,
                                          DeflLitNode *list, Int len) {
    uint16_t code = 0;
    for (int32_t n = 0; n <= max_bits; n++) {
        int32_t bits = h->bit_count[n];
        code = (uint16_t)(code << 1);
        if (n == 0 || bits == 0)
            continue;
        /* The literals list[len-bits] .. list[len-1] are encoded using "bits"
         * bits, and get the values code, code + 1, .... The code values are
         * assigned in literal order (not frequency order). */
        DeflLitNode *chunk = list + len - bits;
        defl_sort_nodes(chunk, bits, false);
        for (int32_t i = 0; i < bits; i++) {
            h->codes[chunk[i].literal] =
                defl_new_hcode(defl_reverse_bits(code, (uint8_t)n), (uint8_t)n);
            code++;
        }
        len -= bits;
    }
}

/* Update this Huffman code object to be the minimum code for the specified
 * frequency count. freq has nfreq entries, and max_bits is the maximum
 * number of bits to use for any literal. */
static void defl_generate(DeflHuffEnc *h, const uint16_t *freq, Int nfreq,
                          int32_t max_bits) {
    DeflLitNode *list = h->freqcache;
    Int count = 0;
    /* Set list to be the set of all non-zero literals and their
     * frequencies. */
    for (Int i = 0; i < nfreq; i++) {
        if (freq[i] != 0) {
            list[count] = (DeflLitNode){(uint16_t)i, freq[i]};
            count++;
        } else {
            h->codes[i] = 0;
        }
    }
    list[count] = (DeflLitNode){0, 0};

    if (count <= 2) {
        /* Handle the small cases here, because they are awkward for the
         * general case code. With two or fewer literals, everything has bit
         * length 1. */
        for (Int i = 0; i < count; i++)
            h->codes[list[i].literal] = defl_new_hcode((uint16_t)i, 1);
        return;
    }
    defl_sort_nodes(list, count, true);

    /* Get the number of literals for each bit count. */
    int32_t mb = defl_bit_counts(h, list, (int32_t)count, max_bits);
    /* And do the assignment. */
    defl_assign_encoding_and_size(h, mb, list, count);
}

static void defl_histogram(const Byte *b, Int n, uint16_t *h) {
    for (Int i = 0; i < n; i++)
        h[b[i]]++;
}

/* ------------------------------------------------------ huffman bit writer */

typedef struct DeflBitWriter {
    /* writer is the underlying writer. Do not use it directly; use the write
     * function, which will sticky-set the error. */
    IoWriter writer;

    /* Data waiting to be written is bytes[0:nbytes] and then the low nbits
     * of bits. */
    uint64_t bits;
    uint8_t nbits;
    uint8_t nbytes;

    bool wrote_huffman;
    DeflHuffEnc *literal_encoding;
    DeflHuffEnc *tmp_lit_encoding;
    DeflHuffEnc *offset_encoding;
    DeflHuffEnc *codegen_encoding;
    Error err;
    Int prev_header;
    unsigned log_new_table_penalty;
    Byte bytes[256 + 8];
    uint16_t literal_freq[DEFL_LENGTH_CODES_START + 32];
    uint16_t offset_freq[32];
    uint16_t codegen_freq[DEFL_CODEGEN_CODE_COUNT];

    /* codegen must have an extra space for the final symbol. */
    uint8_t codegen[DEFL_LITERAL_COUNT + DEFL_OFFSET_CODE_COUNT + 1];

    DeflHuffEnc encoders[4];
} DeflBitWriter;

static void defl_bw_init(DeflBitWriter *w, IoWriter writer) {
    w->writer = writer;
    w->literal_encoding = &w->encoders[0];
    w->tmp_lit_encoding = &w->encoders[1];
    w->codegen_encoding = &w->encoders[2];
    w->offset_encoding = &w->encoders[3];
}

static void defl_bw_reset(DeflBitWriter *w, IoWriter writer) {
    w->writer = writer;
    w->bits = 0;
    w->nbits = 0;
    w->nbytes = 0;
    w->err = BURROW_NO_ERROR;
    w->prev_header = 0;
    w->wrote_huffman = false;
}

static void defl_store_le64(Byte *b, uint64_t v) {
    for (int i = 0; i < 8; i++)
        b[i] = (Byte)(v >> (8 * i));
}

static uint32_t defl_load_le32(const Byte *b, Int i) {
    b += i;
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 |
           (uint32_t)b[3] << 24;
}

static uint64_t defl_load_le64(const Byte *b, Int i) {
    b += i;
    return (uint64_t)b[0] | (uint64_t)b[1] << 8 | (uint64_t)b[2] << 16 |
           (uint64_t)b[3] << 24 | (uint64_t)b[4] << 32 | (uint64_t)b[5] << 40 |
           (uint64_t)b[6] << 48 | (uint64_t)b[7] << 56;
}

/* Whether the literal and offset codes of the last block can code t. */
static bool defl_bw_can_reuse(const DeflBitWriter *w, const DeflTokens *t) {
    for (int i = 0; i < DEFL_OFFSET_CODE_COUNT; i++)
        if (t->off_hist[i] != 0 && w->offset_encoding->codes[i] == 0)
            return false;
    for (int i = 0; i < DEFL_LITERAL_COUNT - 256; i++)
        if (t->extra_hist[i] != 0 && w->literal_encoding->codes[256 + i] == 0)
            return false;
    for (int i = 0; i < 256; i++)
        if (t->lit_hist[i] != 0 && w->literal_encoding->codes[i] == 0)
            return false;
    return true;
}

/* The one place bytes go to the writer. It keeps the first error. */
static void defl_bw_write(DeflBitWriter *w, const Byte *b, Int n) {
    if (BURROW_FAILED(w->err))
        return;
    w->writer.vt->write(w->writer.data,
                        slice_from((void *)(uintptr_t)b, n, n, TYPE_BYTE), &w->err);
}

static void defl_bw_flush_bits(DeflBitWriter *w) {
    uint64_t bits = w->bits;
    w->bits >>= 48;
    w->nbits = (uint8_t)(w->nbits - 48);
    uint8_t n = w->nbytes;
    defl_store_le64(w->bytes + n, bits);
    n = (uint8_t)(n + 6);
    if (n >= DEFL_BUFFER_FLUSH_SIZE) {
        /* Go leaves nbytes alone here, and so does this. */
        if (BURROW_FAILED(w->err))
            return;
        defl_bw_write(w, w->bytes, n);
        n = 0;
    }
    w->nbytes = n;
}

static void defl_bw_write_bits(DeflBitWriter *w, int32_t b, uint8_t nb) {
    w->bits |= (uint64_t)(int64_t)b << (w->nbits & 63);
    w->nbits = (uint8_t)(w->nbits + nb);
    if (w->nbits >= 48)
        defl_bw_flush_bits(w);
}

static void defl_bw_write_code(DeflBitWriter *w, DeflHcode c) {
    w->bits |= defl_hcode_code64(c) << (w->nbits & 63);
    w->nbits = (uint8_t)(w->nbits + defl_hcode_len(c));
    if (w->nbits >= 48)
        defl_bw_flush_bits(w);
}

/* Writes out what is buffered, ending the last block that kept its codes
 * open, and pads to a byte. */
static void defl_bw_flush(DeflBitWriter *w) {
    if (BURROW_FAILED(w->err)) {
        w->nbits = 0;
        return;
    }
    if (w->prev_header > 0) {
        defl_bw_write_code(w, w->literal_encoding->codes[DEFL_END_BLOCK_MARKER]);
        w->prev_header = 0;
    }
    uint8_t n = w->nbytes;
    while (w->nbits != 0) {
        w->bytes[n] = (Byte)w->bits;
        w->bits >>= 8;
        if (w->nbits > 8) /* avoid underflow */
            w->nbits = (uint8_t)(w->nbits - 8);
        else
            w->nbits = 0;
        n++;
    }
    w->bits = 0;
    if (n > 0)
        defl_bw_write(w, w->bytes, n);
    w->nbytes = 0;
}

static const Byte defl_msg_unfinished[] = "writeBytes with unfinished bits";

static Error defl_internal_error(const Byte *text, Int n) {
    return flate_internal_error_as_error((FlateInternalError){text, n},
                                         error_allocator());
}

static void defl_bw_write_bytes(DeflBitWriter *w, const Byte *b, Int len) {
    if (BURROW_FAILED(w->err))
        return;
    uint8_t n = w->nbytes;
    if ((w->nbits & 7) != 0) {
        w->err = defl_internal_error(defl_msg_unfinished,
                                     (Int)sizeof defl_msg_unfinished - 1);
        return;
    }
    while (w->nbits != 0) {
        w->bytes[n] = (Byte)w->bits;
        w->bits >>= 8;
        w->nbits = (uint8_t)(w->nbits - 8);
        n++;
    }
    if (n != 0)
        defl_bw_write(w, w->bytes, n);
    w->nbytes = 0;
    defl_bw_write(w, b, len);
}

/* RFC 1951 3.2.7 specifies a special run-length encoding for specifying the
 * literal and offset lengths arrays (which are concatenated into a single
 * array). This method generates that run-length encoding.
 *
 * The result is written into the codegen array, and the frequencies of each
 * code is written into the codegen_freq array. Codes 0-15 are single byte
 * codes. Codes 16-18 are followed by additional information. Code
 * DEFL_BAD_CODE is an end marker. */
static void defl_bw_generate_codegen(DeflBitWriter *w, Int num_literals,
                                     Int num_offsets, const DeflHuffEnc *lit_enc,
                                     const DeflHuffEnc *off_enc) {
    memset(w->codegen_freq, 0, sizeof w->codegen_freq);
    /* Note that we are using codegen both as a temporary variable for
     * holding a copy of the frequencies, and as the place where we put the
     * result. This is fine because the output is always shorter than the
     * input used so far. */
    uint8_t *codegen = w->codegen;
    for (Int i = 0; i < num_literals; i++)
        codegen[i] = defl_hcode_len(lit_enc->codes[i]);
    for (Int i = 0; i < num_offsets; i++)
        codegen[num_literals + i] = defl_hcode_len(off_enc->codes[i]);
    codegen[num_literals + num_offsets] = DEFL_BAD_CODE;

    uint8_t size = codegen[0];
    Int count = 1;
    Int out_index = 0;
    for (Int in_index = 1; size != DEFL_BAD_CODE; in_index++) {
        /* INVARIANT: We have seen "count" copies of size that have not yet
         * had output generated for them. */
        uint8_t next_size = codegen[in_index];
        if (next_size == size) {
            count++;
            continue;
        }
        /* We need to generate codegen indicating "count" of size. */
        if (size != 0) {
            codegen[out_index++] = size;
            w->codegen_freq[size]++;
            count--;
            while (count >= 3) {
                Int n = count < 6 ? count : 6;
                codegen[out_index++] = 16;
                codegen[out_index++] = (uint8_t)(n - 3);
                w->codegen_freq[16]++;
                count -= n;
            }
        } else {
            while (count >= 11) {
                Int n = count < 138 ? count : 138;
                codegen[out_index++] = 18;
                codegen[out_index++] = (uint8_t)(n - 11);
                w->codegen_freq[18]++;
                count -= n;
            }
            if (count >= 3) {
                /* count >= 3 && count <= 10 */
                codegen[out_index++] = 17;
                codegen[out_index++] = (uint8_t)(count - 3);
                w->codegen_freq[17]++;
                count = 0;
            }
        }
        count--;
        for (; count >= 0; count--) {
            codegen[out_index++] = size;
            w->codegen_freq[size]++;
        }
        /* Set up invariant for next time through the loop. */
        size = next_size;
        count = 1;
    }
    /* Marker indicating the end of the codegen. */
    codegen[out_index] = DEFL_BAD_CODE;
}

static Int defl_bw_codegens(const DeflBitWriter *w) {
    Int num_codegens = DEFL_CODEGEN_CODE_COUNT;
    while (num_codegens > 4 &&
           w->codegen_freq[defl_codegen_order[num_codegens - 1]] == 0)
        num_codegens--;
    return num_codegens;
}

static Int defl_bw_header_size(const DeflBitWriter *w, Int *num_codegens) {
    Int nc = defl_bw_codegens(w);
    if (num_codegens != NULL)
        *num_codegens = nc;
    return 3 + 5 + 5 + 4 + (3 * nc) +
           defl_bit_length(w->codegen_encoding, w->codegen_freq,
                           DEFL_CODEGEN_CODE_COUNT) +
           (Int)w->codegen_freq[16] * 2 + (Int)w->codegen_freq[17] * 3 +
           (Int)w->codegen_freq[18] * 7;
}

/* The size of the block's symbols with the codes in use, header aside. */
static Int defl_bw_dynamic_reuse_size(const DeflBitWriter *w,
                                      const DeflHuffEnc *lit_enc,
                                      const DeflHuffEnc *off_enc) {
    return defl_bit_length(lit_enc, w->literal_freq, DEFL_LENGTH_CODES_START + 32) +
           defl_bit_length(off_enc, w->offset_freq, 32);
}

/* The size of a dynamic block with the codes just generated. */
static Int defl_bw_dynamic_size(const DeflBitWriter *w, const DeflHuffEnc *lit_enc,
                                const DeflHuffEnc *off_enc, Int extra_bits,
                                Int *num_codegens) {
    Int header = defl_bw_header_size(w, num_codegens);
    return header +
           defl_bit_length(lit_enc, w->literal_freq, DEFL_LENGTH_CODES_START + 32) +
           defl_bit_length(off_enc, w->offset_freq, 32) + extra_bits;
}

/* The extra bits the lengths and offsets add, whatever the codes. */
static Int defl_bw_extra_bit_size(const DeflBitWriter *w) {
    Int total = 0;
    for (int i = 0; i < DEFL_LITERAL_COUNT - 257; i++)
        total += (Int)w->literal_freq[257 + i] * (Int)defl_length_extra_bits[i & 31];
    for (int i = 0; i < DEFL_OFFSET_CODE_COUNT; i++)
        total += (Int)w->offset_freq[i] * (Int)defl_offset_extra_bits[i & 31];
    return total;
}

/* The size of a block with the fixed codes. */
static Int defl_bw_fixed_size(const DeflBitWriter *w, Int extra_bits) {
    return 3 +
           defl_bit_length(&defl_fixed_lit_enc, w->literal_freq,
                           DEFL_LENGTH_CODES_START + 32) +
           defl_bit_length(&defl_fixed_off_enc, w->offset_freq, 32) + extra_bits;
}

/* The size of the stored block for in, when it fits in one. in being NULL
 * means there is nothing to store, which Go says with a nil slice. */
static Int defl_bw_stored_size(const Byte *in, Int n, bool *ok) {
    if (in == NULL || n > DEFL_MAX_STORE_BLOCK_SIZE) {
        *ok = false;
        return 0;
    }
    *ok = true;
    return (n + 5) * 8;
}

/* Write the header of a dynamic Huffman block to the output stream. */
static void defl_bw_write_dynamic_header(DeflBitWriter *w, Int num_literals,
                                         Int num_offsets, Int num_codegens,
                                         bool is_eof) {
    if (BURROW_FAILED(w->err))
        return;
    int32_t first_bits = is_eof ? 5 : 4;
    defl_bw_write_bits(w, first_bits, 3);
    defl_bw_write_bits(w, (int32_t)(num_literals - 257), 5);
    defl_bw_write_bits(w, (int32_t)(num_offsets - 1), 5);
    defl_bw_write_bits(w, (int32_t)(num_codegens - 4), 4);

    for (Int i = 0; i < num_codegens; i++) {
        uint8_t value =
            defl_hcode_len(w->codegen_encoding->codes[defl_codegen_order[i]]);
        defl_bw_write_bits(w, (int32_t)value, 3);
    }

    Int i = 0;
    for (;;) {
        uint32_t code_word = w->codegen[i];
        i++;
        if (code_word == DEFL_BAD_CODE)
            break;
        defl_bw_write_code(w, w->codegen_encoding->codes[code_word]);

        switch (code_word) {
        case 16:
            defl_bw_write_bits(w, (int32_t)w->codegen[i], 2);
            i++;
            break;
        case 17:
            defl_bw_write_bits(w, (int32_t)w->codegen[i], 3);
            i++;
            break;
        case 18:
            defl_bw_write_bits(w, (int32_t)w->codegen[i], 7);
            i++;
            break;
        default:
            break;
        }
    }
}

static void defl_bw_write_fixed_header(DeflBitWriter *w, bool is_eof) {
    if (BURROW_FAILED(w->err))
        return;
    if (w->prev_header > 0) {
        defl_bw_write_code(w, w->literal_encoding->codes[DEFL_END_BLOCK_MARKER]);
        w->prev_header = 0;
    }
    /* Indicate that we are a fixed Huffman block. */
    defl_bw_write_bits(w, is_eof ? 3 : 2, 3);
}

/* Write a stored block header. length is how many bytes follow, and is_eof
 * says whether this is the last block. */
static void defl_bw_write_stored_header(DeflBitWriter *w, Int length, bool is_eof) {
    if (BURROW_FAILED(w->err))
        return;
    if (w->prev_header > 0) {
        defl_bw_write_code(w, w->literal_encoding->codes[DEFL_END_BLOCK_MARKER]);
        w->prev_header = 0;
    }

    /* To write EOF, use a fixed encoding block. 10 bits instead of 5
     * bytes. */
    if (length == 0 && is_eof) {
        defl_bw_write_fixed_header(w, is_eof);
        /* EOB: 7 bits, value: 0 */
        defl_bw_write_bits(w, 0, 7);
        defl_bw_flush(w);
        return;
    }

    defl_bw_write_bits(w, is_eof ? 1 : 0, 3);
    defl_bw_flush(w);
    defl_bw_write_bits(w, (int32_t)length, 16);
    defl_bw_write_bits(w, (int32_t)(uint16_t)~(uint16_t)length, 16);
}

/* Copies the token histograms into the writer's frequency tables and gives
 * the number of literal and offset codes a header has to cover. */
static void defl_bw_index_tokens(DeflBitWriter *w, const DeflTokens *t,
                                 Int *num_literals, Int *num_offsets) {
    memcpy(w->literal_freq, t->lit_hist, sizeof t->lit_hist);
    memcpy(w->literal_freq + 256, t->extra_hist, sizeof t->extra_hist);
    memcpy(w->offset_freq, t->off_hist, sizeof t->off_hist);
    *num_literals = 0;
    *num_offsets = 0;
    if (t->n == 0)
        return;

    Int nl = DEFL_LENGTH_CODES_START + 32;
    while (w->literal_freq[nl - 1] == 0)
        nl--;
    /* Get the number of offsets in use. */
    Int no = 32;
    while (no > 0 && w->offset_freq[no - 1] == 0)
        no--;
    if (no == 0) {
        /* We haven't found a single match. If we want to go with the dynamic
         * encoding, we should count at least one offset to be sure that the
         * offset huffman tree could be encoded. */
        w->offset_freq[0] = 1;
        no = 1;
    }
    *num_literals = nl;
    *num_offsets = no;
}

static void defl_bw_generate(DeflBitWriter *w) {
    defl_generate(w->literal_encoding, w->literal_freq, DEFL_LITERAL_COUNT, 15);
    defl_generate(w->offset_encoding, w->offset_freq, DEFL_OFFSET_CODE_COUNT, 15);
}

/* Puts 48 bits of the local bit buffer out to bytes, and bytes out to the
 * writer when it is nearly full. False when an earlier error stops it, and
 * then the caller returns without saving its state, as Go's does. */
#define DEFL_PUT48()                                                                   \
    do {                                                                               \
        if (nbits >= 48) {                                                             \
            defl_store_le64(w->bytes + nbytes, bits);                                  \
            bits >>= 48;                                                               \
            nbits = (uint8_t)(nbits - 48);                                             \
            nbytes = (uint8_t)(nbytes + 6);                                            \
            if (nbytes >= DEFL_BUFFER_FLUSH_SIZE) {                                    \
                if (BURROW_FAILED(w->err))                                             \
                    return;                                                            \
                w->writer.vt->write(w->writer.data,                                    \
                                    slice_from(w->bytes, nbytes, nbytes, TYPE_BYTE),   \
                                    &w->err);                                          \
                nbytes = 0;                                                            \
            }                                                                          \
        }                                                                              \
    } while (0)

/* Writes tokens with the given codes. An end of block token at the end is
 * written last, after the bit buffer is saved. */
static void defl_bw_write_tokens(DeflBitWriter *w, const DeflToken *tokens, Int n,
                                 const DeflHcode *len_codes,
                                 const DeflHcode *off_codes) {
    if (BURROW_FAILED(w->err))
        return;
    if (n == 0)
        return;

    /* Only last token should be DEFL_END_BLOCK_MARKER. */
    bool defer_eob = false;
    if (tokens[n - 1] == DEFL_END_BLOCK_MARKER) {
        n--;
        defer_eob = true;
    }

    const DeflHcode *lits = len_codes;
    const DeflHcode *offs = off_codes;
    const DeflHcode *lengths = len_codes + DEFL_LENGTH_CODES_START;

    /* To keep the bits in registers, as Go does. */
    uint64_t bits = w->bits;
    uint8_t nbits = w->nbits;
    uint8_t nbytes = w->nbytes;

    for (Int i = 0; i < n; i++) {
        DeflToken t = tokens[i];
        if (t < 256) {
            DeflHcode c = lits[t];
            bits |= defl_hcode_code64(c) << (nbits & 63);
            nbits = (uint8_t)(nbits + defl_hcode_len(c));
            DEFL_PUT48();
            continue;
        }

        /* Write the length. */
        uint8_t length = (uint8_t)(t >> DEFL_LENGTH_SHIFT);
        uint8_t len_code = defl_length_codes[length] & 31;
        DeflHcode c = lengths[len_code];
        bits |= defl_hcode_code64(c) << (nbits & 63);
        nbits = (uint8_t)(nbits + defl_hcode_len(c));
        DEFL_PUT48();

        if (len_code >= DEFL_LENGTH_EXTRA_BITS_MIN_CODE) {
            uint8_t extra_length_bits = defl_length_extra_bits[len_code];
            int32_t extra_length =
                (int32_t)(uint8_t)(length - defl_length_base[len_code]);
            bits |= (uint64_t)(int64_t)extra_length << (nbits & 63);
            nbits = (uint8_t)(nbits + extra_length_bits);
            DEFL_PUT48();
        }
        /* Write the offset. */
        uint32_t offset = t & DEFL_OFFSET_MASK;
        uint32_t off_code = (offset >> 16) & 31;
        c = offs[off_code];
        bits |= defl_hcode_code64(c) << (nbits & 63);
        nbits = (uint8_t)(nbits + defl_hcode_len(c));
        DEFL_PUT48();

        if (off_code >= DEFL_OFFSET_EXTRA_BITS_MIN_CODE) {
            uint32_t offset_comb = defl_offset_combined[off_code];
            bits |=
                (uint64_t)((offset - (offset_comb >> 8)) & DEFL_MATCH_OFFSET_ONLY_MASK)
                << (nbits & 63);
            nbits = (uint8_t)(nbits + (uint8_t)offset_comb);
            DEFL_PUT48();
        }
    }
    /* Restore... */
    w->bits = bits;
    w->nbits = nbits;
    w->nbytes = nbytes;

    if (defer_eob)
        defl_bw_write_code(w, len_codes[DEFL_END_BLOCK_MARKER]);
}

/* Writes a block of tokens with the smallest of the fixed codes, codes of
 * its own and storing input, when input is not NULL. */
static void defl_bw_write_block(DeflBitWriter *w, DeflTokens *tokens, bool eof,
                                const Byte *input, Int input_len) {
    if (BURROW_FAILED(w->err))
        return;

    defl_tokens_add_eob(tokens);

    /* We cannot reuse the codes of an open block. */
    if (w->prev_header > 0) {
        defl_bw_write_code(w, w->literal_encoding->codes[DEFL_END_BLOCK_MARKER]);
        w->prev_header = 0;
    }
    Int num_literals, num_offsets;
    defl_bw_index_tokens(w, tokens, &num_literals, &num_offsets);
    defl_bw_generate(w);
    Int extra_bits = 0;
    bool storable;
    Int stored_size = defl_bw_stored_size(input, input_len, &storable);
    if (storable)
        extra_bits = defl_bw_extra_bit_size(w);

    /* Figure out smallest code. Fixed Huffman baseline. */
    const DeflHuffEnc *literal_encoding = &defl_fixed_lit_enc;
    const DeflHuffEnc *offset_encoding = &defl_fixed_off_enc;
    Int size = DEFL_MAX_INT32;
    if (tokens->n < DEFL_MAX_PREDEFINED_TOKENS)
        size = defl_bw_fixed_size(w, extra_bits);

    /* Dynamic Huffman? */
    Int num_codegens;

    /* Generate codegen and codegen_freq, which indicates how to encode the
     * literal_encoding and the offset_encoding. */
    defl_bw_generate_codegen(w, num_literals, num_offsets, w->literal_encoding,
                             w->offset_encoding);
    defl_generate(w->codegen_encoding, w->codegen_freq, DEFL_CODEGEN_CODE_COUNT, 7);
    Int dynamic_size = defl_bw_dynamic_size(w, w->literal_encoding, w->offset_encoding,
                                            extra_bits, &num_codegens);

    if (dynamic_size < size) {
        size = dynamic_size;
        literal_encoding = w->literal_encoding;
        offset_encoding = w->offset_encoding;
    }

    /* Stored bytes? */
    if (storable && stored_size <= size) {
        defl_bw_write_stored_header(w, input_len, eof);
        defl_bw_write_bytes(w, input, input_len);
        return;
    }

    /* Huffman. */
    if (literal_encoding == &defl_fixed_lit_enc)
        defl_bw_write_fixed_header(w, eof);
    else
        defl_bw_write_dynamic_header(w, num_literals, num_offsets, num_codegens, eof);

    /* Write the tokens. */
    defl_bw_write_tokens(w, tokens->tokens, tokens->n, literal_encoding->codes,
                         offset_encoding->codes);
}

/* Writes a block with codes of its own, or the fixed ones, or stored, or
 * goes on with the codes of the last block when that is cheaper. The block
 * stays open for more tokens when neither sync nor eof is set. */
static void defl_bw_write_block_dynamic(DeflBitWriter *w, DeflTokens *tokens, bool eof,
                                        const Byte *input, Int input_len, bool sync) {
    if (BURROW_FAILED(w->err))
        return;

    sync = sync || eof;
    if (sync)
        defl_tokens_add_eob(tokens);
    else
        /* We cannot reuse the codes of a block that did not have an EOB. */
        tokens->extra_hist[0] = 1;

    /* We cannot reuse pure huffman table, and must mark as EOF. */
    if ((w->wrote_huffman || eof) && w->prev_header > 0) {
        defl_bw_write_code(w, w->literal_encoding->codes[DEFL_END_BLOCK_MARKER]);
        w->prev_header = 0;
        w->wrote_huffman = false;
    }

    /* See if we can reuse. */
    if (w->prev_header > 0 && !defl_bw_can_reuse(w, tokens)) {
        defl_bw_write_code(w, w->literal_encoding->codes[DEFL_END_BLOCK_MARKER]);
        w->prev_header = 0;
    }

    Int num_literals, num_offsets;
    defl_bw_index_tokens(w, tokens, &num_literals, &num_offsets);
    Int extra_bits = 0;
    bool storable;
    Int ssize = defl_bw_stored_size(input, input_len, &storable);

    if (storable || w->prev_header > 0)
        extra_bits = defl_bw_extra_bit_size(w);

    Int size;

    /* Check if we should reuse. */
    if (w->prev_header > 0) {
        /* Estimate size for using a new table. Use the previous header size
         * as the estimate for the new one. */
        Int new_size = w->prev_header + defl_tokens_estimated_bits(tokens);

        /* The estimated size is calculated as an optimal table. We add a
         * penalty to make it more realistic and re-use a bit more. */
        new_size +=
            (Int)defl_hcode_len(w->literal_encoding->codes[DEFL_END_BLOCK_MARKER]) +
            (new_size >> w->log_new_table_penalty);

        /* Calculate the size for reusing the current table. */
        Int reuse_size =
            defl_bw_dynamic_reuse_size(w, w->literal_encoding, w->offset_encoding) +
            extra_bits;

        /* Check if a new table is better. */
        if (new_size < reuse_size) {
            /* Write the EOB we owe. */
            defl_bw_write_code(w, w->literal_encoding->codes[DEFL_END_BLOCK_MARKER]);
            size = new_size;
            w->prev_header = 0;
        } else {
            size = reuse_size;
        }

        if (tokens->n < DEFL_MAX_PREDEFINED_TOKENS) {
            Int pre_size = defl_bw_fixed_size(w, extra_bits) + 7;
            if (pre_size < size) {
                /* Check if we get a reasonable size decrease. */
                if (storable && ssize <= size) {
                    defl_bw_write_stored_header(w, input_len, eof);
                    defl_bw_write_bytes(w, input, input_len);
                    return;
                }
                defl_bw_write_fixed_header(w, eof);
                if (!sync)
                    defl_tokens_add_eob(tokens);
                defl_bw_write_tokens(w, tokens->tokens, tokens->n,
                                     defl_fixed_lit_enc.codes,
                                     defl_fixed_off_enc.codes);
                return;
            }
        }

        /* Check if we get a reasonable size decrease. */
        if (storable && ssize <= size) {
            defl_bw_write_stored_header(w, input_len, eof);
            defl_bw_write_bytes(w, input, input_len);
            return;
        }
    }

    /* We want a new block/table. */
    if (w->prev_header == 0) {
        w->literal_freq[DEFL_END_BLOCK_MARKER] = 1;

        defl_bw_generate(w);
        /* Generate codegen and codegen_freq, which indicates how to encode
         * the literal_encoding and the offset_encoding. */
        defl_bw_generate_codegen(w, num_literals, num_offsets, w->literal_encoding,
                                 w->offset_encoding);
        defl_generate(w->codegen_encoding, w->codegen_freq, DEFL_CODEGEN_CODE_COUNT, 7);

        Int num_codegens;
        size = defl_bw_dynamic_size(w, w->literal_encoding, w->offset_encoding,
                                    extra_bits, &num_codegens);

        /* Store predefined, if we don't get a reasonable improvement. */
        if (tokens->n < DEFL_MAX_PREDEFINED_TOKENS) {
            Int pre_size = defl_bw_fixed_size(w, extra_bits);
            if (pre_size <= size) {
                /* Store bytes, if we don't get an improvement. */
                if (storable && ssize <= pre_size) {
                    defl_bw_write_stored_header(w, input_len, eof);
                    defl_bw_write_bytes(w, input, input_len);
                    return;
                }
                defl_bw_write_fixed_header(w, eof);
                if (!sync)
                    defl_tokens_add_eob(tokens);
                defl_bw_write_tokens(w, tokens->tokens, tokens->n,
                                     defl_fixed_lit_enc.codes,
                                     defl_fixed_off_enc.codes);
                return;
            }
        }

        if (storable && ssize <= size) {
            /* Store bytes, if we don't get an improvement. */
            defl_bw_write_stored_header(w, input_len, eof);
            defl_bw_write_bytes(w, input, input_len);
            return;
        }

        /* Write the Huffman table into the stream. */
        defl_bw_write_dynamic_header(w, num_literals, num_offsets, num_codegens, eof);
        if (!sync)
            w->prev_header = defl_bw_header_size(w, NULL);
        w->wrote_huffman = false;
    }

    if (sync)
        w->prev_header = 0;

    defl_bw_write_tokens(w, tokens->tokens, tokens->n, w->literal_encoding->codes,
                         w->offset_encoding->codes);
}

/* Huffman codes input alone, with no matches, or stores it when that does
 * no good. The block stays open for more when neither eof nor sync is set. */
static void defl_bw_write_block_huff(DeflBitWriter *w, bool eof, const Byte *input,
                                     Int input_len, bool sync) {
    if (BURROW_FAILED(w->err))
        return;

    /* Clear histogram. */
    memset(w->literal_freq, 0, sizeof w->literal_freq);
    if (!w->wrote_huffman)
        memset(w->offset_freq, 0, sizeof w->offset_freq);

    enum { NUM_LITERALS = DEFL_END_BLOCK_MARKER + 1, NUM_OFFSETS = 1 };

    /* Add everything as literals. We have to estimate the header size.
     * Assume header is around 70 bytes. */
    const Int guess_header_size_bits = 70 * 8;
    defl_histogram(input, input_len, w->literal_freq);
    bool storable;
    Int ssize = defl_bw_stored_size(input, input_len, &storable);
    if (storable && input_len > 1024) {
        /* Quick check for incompressible content. */
        double abs = 0;
        double avg = (double)input_len / 256;
        double max = (double)(input_len * 2);
        for (int i = 0; i < 256; i++) {
            double diff = (double)w->literal_freq[i] - avg;
            double sq = diff * diff;
            abs += sq;
            if (abs >= max)
                break;
        }
        if (abs < max) {
            /* No chance we can compress this... */
            defl_bw_write_stored_header(w, input_len, eof);
            defl_bw_write_bytes(w, input, input_len);
            return;
        }
    }
    w->literal_freq[DEFL_END_BLOCK_MARKER] = 1;
    defl_generate(w->tmp_lit_encoding, w->literal_freq, NUM_LITERALS, 15);
    Int est_bits =
        defl_can_encode_len(w->tmp_lit_encoding, w->literal_freq, NUM_LITERALS);
    if (est_bits < DEFL_MAX_INT32) {
        est_bits += w->prev_header;
        if (w->prev_header == 0)
            est_bits += guess_header_size_bits;
        est_bits += est_bits >> w->log_new_table_penalty;
    }

    /* Store bytes, if we don't get a reasonable improvement. */
    if (storable && ssize <= est_bits) {
        defl_bw_write_stored_header(w, input_len, eof);
        defl_bw_write_bytes(w, input, input_len);
        return;
    }

    if (w->prev_header > 0) {
        Int reuse_size = defl_can_encode_len(w->literal_encoding, w->literal_freq, 256);
        if (est_bits < reuse_size) {
            /* We owe an EOB. */
            defl_bw_write_code(w, w->literal_encoding->codes[DEFL_END_BLOCK_MARKER]);
            w->prev_header = 0;
        }
    }

    if (w->prev_header == 0) {
        /* Use the temp encoding, so swap. */
        DeflHuffEnc *t = w->literal_encoding;
        w->literal_encoding = w->tmp_lit_encoding;
        w->tmp_lit_encoding = t;
        /* Generate codegen and codegen_freq, which indicates how to encode
         * the literal_encoding and the offset_encoding. */
        defl_bw_generate_codegen(w, NUM_LITERALS, NUM_OFFSETS, w->literal_encoding,
                                 &defl_huff_off_enc);
        defl_generate(w->codegen_encoding, w->codegen_freq, DEFL_CODEGEN_CODE_COUNT, 7);
        Int num_codegens = defl_bw_codegens(w);

        /* Huffman. */
        defl_bw_write_dynamic_header(w, NUM_LITERALS, NUM_OFFSETS, num_codegens, eof);
        w->wrote_huffman = true;
        w->prev_header = defl_bw_header_size(w, NULL);
    }

    const DeflHcode *encoding = w->literal_encoding->codes;
    /* Go 1.16 LOVES having these on stack. At least 1.5x the speed. */
    uint64_t bits = w->bits;
    uint8_t nbits = w->nbits;
    uint8_t nbytes = w->nbytes;

    /* Unroll, write 3 codes/loop. Fastest number of codes per loop is
     * uncertain, so it has been tuned the same way Go's is. */
    const Byte *in = input;
    Int left = input_len;
    while (left > 3) {
        /* We must have at least 48 bits free. */
        if (nbits >= 8) {
            uint8_t n = nbits >> 3;
            defl_store_le64(w->bytes + nbytes, bits);
            bits >>= (n * 8) & 63;
            nbits = (uint8_t)(nbits - n * 8);
            nbytes = (uint8_t)(nbytes + n);
        }
        if (nbytes >= DEFL_BUFFER_FLUSH_SIZE) {
            if (BURROW_FAILED(w->err))
                return;
            w->writer.vt->write(w->writer.data,
                                slice_from(w->bytes, nbytes, nbytes, TYPE_BYTE),
                                &w->err);
            nbytes = 0;
        }
        DeflHcode a = encoding[in[0]], b = encoding[in[1]];
        bits |= defl_hcode_code64(a) << (nbits & 63);
        bits |= defl_hcode_code64(b) << ((uint8_t)(nbits + defl_hcode_len(a)) & 63);
        DeflHcode c = encoding[in[2]];
        nbits = (uint8_t)(nbits + defl_hcode_len(b) + defl_hcode_len(a));
        bits |= defl_hcode_code64(c) << (nbits & 63);
        nbits = (uint8_t)(nbits + defl_hcode_len(c));
        in += 3;
        left -= 3;
    }

    /* Remaining... */
    for (Int i = 0; i < left; i++) {
        DEFL_PUT48();
        /* Bitwriting inlined, ~30% speedup. */
        DeflHcode c = encoding[in[i]];
        bits |= defl_hcode_code64(c) << (nbits & 63);
        nbits = (uint8_t)(nbits + defl_hcode_len(c));
    }
    /* Restore... */
    w->bits = bits;
    w->nbits = nbits;
    w->nbytes = nbytes;

    /* Flush if needed to have space. */
    if (w->nbits >= 48)
        defl_bw_flush_bits(w);

    if (eof || sync) {
        defl_bw_write_code(w, w->literal_encoding->codes[DEFL_END_BLOCK_MARKER]);
        w->prev_header = 0;
        w->wrote_huffman = false;
    }
}

/* ---------------------------------------------------------- fast encoders
 *
 * Levels 1 to 6. Each keeps the blocks it has seen in hist, up to five of
 * them, and table entries hold positions plus cur, so that sliding hist down
 * does not have to touch the tables until cur gets near overflowing. */

typedef struct DeflPrevEntry {
    int32_t cur;
    int32_t prev;
} DeflPrevEntry;

typedef struct DeflFast {
    int level;
    Byte *hist; /* DEFL_ALLOC_HISTORY bytes, of which hist_len are in use */
    Int hist_len;
    int32_t cur;
    int32_t *table;        /* levels 1, 2, 4, 5 and 6 */
    int32_t *btable;       /* level 4 */
    DeflPrevEntry *ptable; /* level 3, and the long table of levels 5 and 6 */
    Int table_len;
    Int ptable_len;
} DeflFast;

/* Adds src to hist, sliding hist down to its last 32 KiB first when src
 * would not fit, and gives where in hist src starts. */
static int32_t defl_fast_add_block(DeflFast *e, const Byte *src, Int n) {
    /* Check if we have space. */
    if (e->hist_len + n > DEFL_ALLOC_HISTORY) {
        /* Move down. */
        int32_t offset = (int32_t)e->hist_len - DEFL_MAX_MATCH_OFFSET;
        memmove(e->hist, e->hist + offset, DEFL_MAX_MATCH_OFFSET);
        e->cur += offset;
        e->hist_len = DEFL_MAX_MATCH_OFFSET;
    }
    int32_t s = (int32_t)e->hist_len;
    if (n > 0)
        memcpy(e->hist + e->hist_len, src, (size_t)n);
    e->hist_len += n;
    return s;
}

/* How many bytes a and b have in common at the start, a having alen of them
 * and b at least as many. */
static Int defl_match_len(const Byte *a, Int alen, const Byte *b) {
    Int n = 0;
    Int left = alen;
    while (left >= 8) {
        uint64_t diff = defl_load_le64(a, n) ^ defl_load_le64(b, n);
        if (diff != 0)
            return n + (Int)(bits_trailing_zeros64(diff) >> 3);
        n += 8;
        left -= 8;
    }
    while (n < alen && a[n] == b[n])
        n++;
    return n;
}

/* The match length at s and t in src, no longer than 254 bytes. */
static int32_t defl_match_len_limited(int32_t s, int32_t t, const Byte *src, Int len) {
    Int end = (Int)s + DEFL_MAX_MATCH_LENGTH - 4;
    if (end > len)
        end = len;
    return (int32_t)defl_match_len(src + s, end - s, src + t);
}

/* The match length at s and t in src, as long as it goes. */
static int32_t defl_match_len_long(int32_t s, int32_t t, const Byte *src, Int len) {
    return (int32_t)defl_match_len(src + s, len - s, src + t);
}

static void defl_fast_reset(DeflFast *e) {
    /* Protect against e->cur wraparound. */
    if (e->cur <= DEFL_BUFFER_RESET)
        e->cur += DEFL_MAX_MATCH_OFFSET + (int32_t)e->hist_len;
    e->hist_len = 0;
}

/* The hash of the n low bytes of u, b bits long. */
static uint32_t defl_hash_len(uint64_t u, uint8_t b, uint8_t n) {
    switch (n) {
    case 3:
        return ((uint32_t)(u << 8) * DEFL_PRIME3) >> (32 - b);
    case 5:
        return (uint32_t)(((u << (64 - 40)) * DEFL_PRIME5) >> (64 - b));
    case 6:
        return (uint32_t)(((u << (64 - 48)) * DEFL_PRIME6) >> (64 - b));
    case 7:
        return (uint32_t)(((u << (64 - 56)) * DEFL_PRIME7) >> (64 - b));
    case 8:
        return (uint32_t)((u * DEFL_PRIME8) >> (64 - b));
    default:
        return ((uint32_t)u * DEFL_PRIME4) >> (32 - b);
    }
}

/* Moves table entries down when cur gets near overflowing: entries too far
 * back to use become 0 and the rest keep pointing where they did. */
static void defl_shift_table(int32_t *table, Int n, int32_t min_off, int32_t cur) {
    for (Int i = 0; i < n; i++) {
        int32_t v = table[i];
        if (v <= min_off)
            v = 0;
        else
            v = v - cur + DEFL_MAX_MATCH_OFFSET;
        table[i] = v;
    }
}

/* The start of each encoder: moving the tables down when cur has grown too
 * large, which Go does in a loop that only ever runs once. */
static void defl_fast_shift(DeflFast *e) {
    while (e->cur >= DEFL_BUFFER_RESET) {
        if (e->hist_len == 0) {
            if (e->table != NULL)
                memset(e->table, 0, (size_t)e->table_len * sizeof *e->table);
            if (e->btable != NULL)
                memset(e->btable, 0, (size_t)e->table_len * sizeof *e->btable);
            if (e->ptable != NULL)
                memset(e->ptable, 0, (size_t)e->ptable_len * sizeof *e->ptable);
            e->cur = DEFL_MAX_MATCH_OFFSET;
            break;
        }
        /* Shift down everything in the table that isn't already too far
         * away. */
        int32_t min_off = e->cur + (int32_t)e->hist_len - DEFL_MAX_MATCH_OFFSET;
        if (e->table != NULL)
            defl_shift_table(e->table, e->table_len, min_off, e->cur);
        if (e->btable != NULL)
            defl_shift_table(e->btable, e->table_len, min_off, e->cur);
        for (Int i = 0; i < e->ptable_len; i++) {
            DeflPrevEntry v = e->ptable[i];
            if (e->level == 3) {
                v.cur = v.cur <= min_off ? 0 : v.cur - e->cur + DEFL_MAX_MATCH_OFFSET;
                v.prev =
                    v.prev <= min_off ? 0 : v.prev - e->cur + DEFL_MAX_MATCH_OFFSET;
            } else if (v.cur <= min_off) {
                v.cur = 0;
                v.prev = 0;
            } else {
                v.cur = v.cur - e->cur + DEFL_MAX_MATCH_OFFSET;
                v.prev =
                    v.prev <= min_off ? 0 : v.prev - e->cur + DEFL_MAX_MATCH_OFFSET;
            }
            e->ptable[i] = v;
        }
        e->cur = DEFL_MAX_MATCH_OFFSET;
    }
}

/* Emits the literals from src[from:to] as tokens. */
static void defl_emit_range(DeflTokens *dst, const Byte *src, int32_t from,
                            int32_t to) {
    for (int32_t i = from; i < to; i++) {
        Byte v = src[i];
        dst->tokens[dst->n] = v;
        dst->lit_hist[v]++;
        dst->n++;
    }
}

static void defl_emit_remainder(DeflTokens *dst, const Byte *src, Int len,
                                int32_t next_emit) {
    if ((Int)next_emit < len) {
        /* If nothing was added, don't encode literals. */
        if (dst->n == 0)
            return;
        defl_emit_literals(dst, src + next_emit, len - next_emit);
    }
}

static inline void defl_ptable_push(DeflPrevEntry *e, int32_t off) {
    e->prev = e->cur;
    e->cur = off;
}

/* Level 1 uses a single small table with 5 byte hashes. */
static void defl_encode_l1(DeflFast *e, DeflTokens *dst, const Byte *in, Int in_len) {
    enum { HASH_BYTES = 5 };
    int32_t *table = e->table;

    defl_fast_shift(e);

    int32_t s = defl_fast_add_block(e, in, in_len);

    /* This check isn't in the Snappy implementation, but there, the caller
     * instead of the callee handles this case. */
    if (in_len < DEFL_MIN_NON_LITERAL_BLOCK_SIZE) {
        /* We do not fill the token table. This will be picked up by caller. */
        dst->n = (uint16_t)in_len;
        return;
    }

    /* Override src. */
    const Byte *src = e->hist;
    Int len = e->hist_len;
    int32_t next_emit = s;

    /* s_limit is when to stop looking for offset/length copies. The
     * DEFL_INPUT_MARGIN lets us use a fast path for emitting literals. */
    int32_t s_limit = (int32_t)(len - DEFL_INPUT_MARGIN);

    /* next_emit is where in src the next emit_literal should start from. */
    uint64_t cv = defl_load_le64(src, s);

    for (;;) {
        enum { SKIP_LOG = 5, DO_EVERY = 2 };

        int32_t next_s = s;
        int32_t candidate;
        int32_t t;
        for (;;) {
            uint32_t next_hash = defl_hash_len(cv, DEFL_TABLE_BITS, HASH_BYTES);
            candidate = table[next_hash];
            next_s = s + DO_EVERY + ((s - next_emit) >> SKIP_LOG);
            if (next_s > s_limit)
                goto emit_remainder;

            uint64_t now = defl_load_le64(src, next_s);
            table[next_hash] = s + e->cur;
            next_hash = defl_hash_len(now, DEFL_TABLE_BITS, HASH_BYTES);
            t = candidate - e->cur;
            if (s - t < DEFL_MAX_MATCH_OFFSET &&
                (uint32_t)cv == defl_load_le32(src, t)) {
                table[next_hash] = next_s + e->cur;
                break;
            }

            /* Do one right away... */
            cv = now;
            s = next_s;
            next_s++;
            candidate = table[next_hash];
            now >>= 8;
            table[next_hash] = s + e->cur;

            t = candidate - e->cur;
            if (s - t < DEFL_MAX_MATCH_OFFSET &&
                (uint32_t)cv == defl_load_le32(src, t)) {
                table[next_hash] = next_s + e->cur;
                break;
            }
            cv = now;
            s = next_s;
        }

        /* A 4-byte match has been found. We'll later see if more than 4
         * bytes match. But, prior to the match, src[next_emit:s] are
         * unmatched. Emit them as literal bytes. */
        for (;;) {
            /* Invariant: we have a 4-byte match at s, and no need to emit
             * any literal bytes prior to s. */

            /* Extend the 4-byte match as long as possible. */
            int32_t l = defl_match_len_long(s + 4, t + 4, src, len) + 4;

            /* Extend backwards. */
            while (t > 0 && s > next_emit && src[t - 1] == src[s - 1]) {
                s--;
                t--;
                l++;
            }
            if (next_emit < s)
                defl_emit_range(dst, src, next_emit, s);

            /* Save the match found. Same as defl_tokens_add_match_long,
             * inlined in Go. */
            defl_tokens_add_match_long(dst, l,
                                       (uint32_t)(s - t - DEFL_BASE_MATCH_OFFSET));
            s += l;
            next_emit = s;
            if (next_s >= s)
                s = next_s + 1;
            if (s >= s_limit) {
                /* Index first pair after match end. */
                if ((Int)s + l + 8 < len) {
                    uint64_t cv2 = defl_load_le64(src, s);
                    table[defl_hash_len(cv2, DEFL_TABLE_BITS, HASH_BYTES)] = s + e->cur;
                }
                goto emit_remainder;
            }

            /* We could immediately start working at s now, but to improve
             * compression we first update the hash table at s-2 and at s. If
             * another emit_copy is not our next move, also calculate
             * next_hash at s+1. At least on GOARCH=amd64, these three hash
             * calculations are faster as one load64 call (with some shifts)
             * instead of three load32 calls. */
            uint64_t x = defl_load_le64(src, s - 2);
            int32_t o = e->cur + s - 2;
            uint32_t prev_hash = defl_hash_len(x, DEFL_TABLE_BITS, HASH_BYTES);
            table[prev_hash] = o;
            x >>= 16;
            uint32_t curr_hash = defl_hash_len(x, DEFL_TABLE_BITS, HASH_BYTES);
            candidate = table[curr_hash];
            table[curr_hash] = o + 2;

            t = candidate - e->cur;
            if (s - t > DEFL_MAX_MATCH_OFFSET ||
                (uint32_t)x != defl_load_le32(src, t)) {
                cv = x >> 8;
                s++;
                break;
            }
        }
    }

emit_remainder:
    defl_emit_remainder(dst, src, len, next_emit);
}

/* Level 2 uses a single larger table with 5 byte hashes, and indexes more
 * of each match. */
static void defl_encode_l2(DeflFast *e, DeflTokens *dst, const Byte *in, Int in_len) {
    enum { HASH_BYTES = 5 };
    int32_t *table = e->table;

    defl_fast_shift(e);

    int32_t s = defl_fast_add_block(e, in, in_len);

    if (in_len < DEFL_MIN_NON_LITERAL_BLOCK_SIZE) {
        dst->n = (uint16_t)in_len;
        return;
    }

    const Byte *src = e->hist;
    Int len = e->hist_len;
    int32_t next_emit = s;
    int32_t s_limit = (int32_t)(len - DEFL_INPUT_MARGIN);
    uint64_t cv = defl_load_le64(src, s);

    for (;;) {
        /* When should we start skipping if we haven't found matches in a
         * long while. */
        enum { SKIP_LOG = 5, DO_EVERY = 2 };

        int32_t next_s = s;
        int32_t candidate;
        for (;;) {
            uint32_t next_hash = defl_hash_len(cv, DEFL_L2_TABLE_BITS, HASH_BYTES);
            s = next_s;
            next_s = s + DO_EVERY + ((s - next_emit) >> SKIP_LOG);
            if (next_s > s_limit)
                goto emit_remainder;
            candidate = table[next_hash];
            uint64_t now = defl_load_le64(src, next_s);
            table[next_hash] = s + e->cur;
            next_hash = defl_hash_len(now, DEFL_L2_TABLE_BITS, HASH_BYTES);

            int32_t offset = s - (candidate - e->cur);
            if (offset < DEFL_MAX_MATCH_OFFSET &&
                (uint32_t)cv == defl_load_le32(src, candidate - e->cur)) {
                table[next_hash] = next_s + e->cur;
                break;
            }

            /* Do one right away... */
            cv = now;
            s = next_s;
            next_s++;
            candidate = table[next_hash];
            now >>= 8;
            table[next_hash] = s + e->cur;

            offset = s - (candidate - e->cur);
            if (offset < DEFL_MAX_MATCH_OFFSET &&
                (uint32_t)cv == defl_load_le32(src, candidate - e->cur))
                break;
            cv = now;
        }

        /* Call emit_copy, and then see if another emit_copy could be our
         * next move. Repeat until we find no match for the input
         * immediately after what was consumed by the last emit_copy call. */
        for (;;) {
            /* Invariant: we have a 4-byte match at s, and no need to emit
             * any literal bytes prior to s. */

            /* Extend the 4-byte match as long as possible. */
            int32_t t = candidate - e->cur;
            int32_t l = defl_match_len_long(s + 4, t + 4, src, len) + 4;

            /* Extend backwards. */
            while (t > 0 && s > next_emit && src[t - 1] == src[s - 1]) {
                s--;
                t--;
                l++;
            }
            if (next_emit < s)
                defl_emit_range(dst, src, next_emit, s);

            defl_tokens_add_match_long(dst, l,
                                       (uint32_t)(s - t - DEFL_BASE_MATCH_OFFSET));
            s += l;
            next_emit = s;
            if (next_s >= s)
                s = next_s + 1;

            if (s >= s_limit) {
                /* Index first pair after match end. */
                if ((Int)s + l + 8 < len) {
                    uint64_t cv2 = defl_load_le64(src, s);
                    table[defl_hash_len(cv2, DEFL_L2_TABLE_BITS, HASH_BYTES)] =
                        s + e->cur;
                }
                goto emit_remainder;
            }

            /* Store every second hash in-between, but offset by 1. */
            for (int32_t i = s - l + 2; i < s - 5; i += 7) {
                uint64_t x = defl_load_le64(src, i);
                uint32_t next_hash = defl_hash_len(x, DEFL_L2_TABLE_BITS, HASH_BYTES);
                table[next_hash] = e->cur + i;
                /* Skip one. */
                x >>= 16;
                next_hash = defl_hash_len(x, DEFL_L2_TABLE_BITS, HASH_BYTES);
                table[next_hash] = e->cur + i + 2;
                /* Skip one. */
                x >>= 16;
                next_hash = defl_hash_len(x, DEFL_L2_TABLE_BITS, HASH_BYTES);
                table[next_hash] = e->cur + i + 4;
            }

            /* We could immediately start working at s now, but to improve
             * compression we first update the hash table at s-2 to s. If
             * another emit_copy is not our next move, also calculate
             * next_hash at s+1. */
            uint64_t x = defl_load_le64(src, s - 2);
            int32_t o = e->cur + s - 2;
            uint32_t prev_hash = defl_hash_len(x, DEFL_L2_TABLE_BITS, HASH_BYTES);
            uint32_t prev_hash2 = defl_hash_len(x >> 8, DEFL_L2_TABLE_BITS, HASH_BYTES);
            table[prev_hash] = o;
            table[prev_hash2] = o + 1;
            uint32_t curr_hash = defl_hash_len(x >> 16, DEFL_L2_TABLE_BITS, HASH_BYTES);
            candidate = table[curr_hash];
            table[curr_hash] = o + 2;

            int32_t offset = s - (candidate - e->cur);
            if (offset > DEFL_MAX_MATCH_OFFSET ||
                (uint32_t)(x >> 16) != defl_load_le32(src, candidate - e->cur)) {
                cv = x >> 24;
                s++;
                break;
            }
        }
    }

emit_remainder:
    defl_emit_remainder(dst, src, len, next_emit);
}

/* Level 3 keeps the last two positions for each hash. */
static void defl_encode_l3(DeflFast *e, DeflTokens *dst, const Byte *in, Int in_len) {
    enum { HASH_BYTES = 5 };
    DeflPrevEntry *table = e->ptable;

    defl_fast_shift(e);

    int32_t s = defl_fast_add_block(e, in, in_len);

    if (in_len < DEFL_MIN_NON_LITERAL_BLOCK_SIZE) {
        dst->n = (uint16_t)in_len;
        return;
    }

    const Byte *src = e->hist;
    Int len = e->hist_len;
    int32_t next_emit = s;
    int32_t s_limit = (int32_t)(len - DEFL_INPUT_MARGIN);
    uint64_t cv = defl_load_le64(src, s);

    for (;;) {
        enum { SKIP_LOG = 7 };
        int32_t next_s = s;
        int32_t candidate;
        for (;;) {
            uint32_t next_hash = defl_hash_len(cv, DEFL_L3_TABLE_BITS, HASH_BYTES);
            s = next_s;
            next_s = s + 1 + ((s - next_emit) >> SKIP_LOG);
            if (next_s > s_limit)
                goto emit_remainder;
            DeflPrevEntry candidates = table[next_hash];
            uint64_t now = defl_load_le64(src, next_s);

            /* Safe offset distance until s + 4... */
            int32_t min_offset = e->cur + s - (DEFL_MAX_MATCH_OFFSET - 4);
            table[next_hash] = (DeflPrevEntry){s + e->cur, candidates.cur};

            /* Check both candidates. */
            candidate = candidates.cur;
            if (candidate < min_offset) {
                cv = now;
                /* Previous will also be invalid, we have nothing. */
                continue;
            }

            if ((uint32_t)cv == defl_load_le32(src, candidate - e->cur)) {
                if (candidates.prev < min_offset ||
                    (uint32_t)cv != defl_load_le32(src, candidates.prev - e->cur))
                    break;
                /* Both match and are valid, pick longest. */
                int32_t offset = s - (candidate - e->cur);
                int32_t o2 = s - (candidates.prev - e->cur);
                Int l1 = defl_match_len(src + s + 4, len - s - 4, src + s - offset + 4);
                Int l2 = defl_match_len(src + s + 4, len - s - 4, src + s - o2 + 4);
                if (l2 > l1)
                    candidate = candidates.prev;
                break;
            } else {
                /* We only check if value mismatches. Offset will always be
                 * invalid in other cases. */
                candidate = candidates.prev;
                if (candidate > min_offset &&
                    (uint32_t)cv == defl_load_le32(src, candidate - e->cur))
                    break;
            }
            cv = now;
        }

        for (;;) {
            /* Extend the 4-byte match as long as possible. */
            int32_t t = candidate - e->cur;
            int32_t l = defl_match_len_long(s + 4, t + 4, src, len) + 4;

            /* Extend backwards. */
            while (t > 0 && s > next_emit && src[t - 1] == src[s - 1]) {
                s--;
                t--;
                l++;
            }
            if (next_emit < s)
                defl_emit_range(dst, src, next_emit, s);

            defl_tokens_add_match_long(dst, l,
                                       (uint32_t)(s - t - DEFL_BASE_MATCH_OFFSET));
            s += l;
            next_emit = s;
            if (next_s >= s)
                s = next_s + 1;

            if (s >= s_limit) {
                t += l;
                /* Index first pair after match end. */
                if ((Int)t + 8 < len && t > 0) {
                    cv = defl_load_le64(src, t);
                    uint32_t next_hash =
                        defl_hash_len(cv, DEFL_L3_TABLE_BITS, HASH_BYTES);
                    defl_ptable_push(&table[next_hash], e->cur + t);
                }
                goto emit_remainder;
            }

            /* Store every 5th hash in-between. */
            for (int32_t i = s - l + 2; i < s - 5; i += 6) {
                uint32_t next_hash = defl_hash_len(defl_load_le64(src, i),
                                                   DEFL_L3_TABLE_BITS, HASH_BYTES);
                defl_ptable_push(&table[next_hash], e->cur + i);
            }
            /* We could immediately start working at s now, but to improve
             * compression we first update the hash table at s-2 to s. */
            uint64_t x = defl_load_le64(src, s - 2);
            uint32_t prev_hash = defl_hash_len(x, DEFL_L3_TABLE_BITS, HASH_BYTES);

            defl_ptable_push(&table[prev_hash], e->cur + s - 2);
            x >>= 8;
            prev_hash = defl_hash_len(x, DEFL_L3_TABLE_BITS, HASH_BYTES);

            defl_ptable_push(&table[prev_hash], e->cur + s - 1);
            x >>= 8;
            uint32_t curr_hash = defl_hash_len(x, DEFL_L3_TABLE_BITS, HASH_BYTES);
            DeflPrevEntry candidates = table[curr_hash];
            cv = x;
            table[curr_hash] = (DeflPrevEntry){s + e->cur, candidates.cur};

            /* Check both candidates. */
            candidate = candidates.cur;
            int32_t min_offset = e->cur + s - (DEFL_MAX_MATCH_OFFSET - 4);

            if (candidate > min_offset) {
                if ((uint32_t)cv == defl_load_le32(src, candidate - e->cur))
                    /* Found a match... */
                    continue;
                candidate = candidates.prev;
                if (candidate > min_offset &&
                    (uint32_t)cv == defl_load_le32(src, candidate - e->cur))
                    /* Match at prev... */
                    continue;
            }
            cv = x >> 8;
            s++;
            break;
        }
    }

emit_remainder:
    defl_emit_remainder(dst, src, len, next_emit);
}

/* Level 4 has a short table with 4 byte hashes and a long one with 7 byte
 * hashes. */
static void defl_encode_l4(DeflFast *e, DeflTokens *dst, const Byte *in, Int in_len) {
    enum { HASH_SHORT_BYTES = 4 };
    int32_t *table = e->table;
    int32_t *btable = e->btable;

    defl_fast_shift(e);

    int32_t s = defl_fast_add_block(e, in, in_len);

    if (in_len < DEFL_MIN_NON_LITERAL_BLOCK_SIZE) {
        dst->n = (uint16_t)in_len;
        return;
    }

    const Byte *src = e->hist;
    Int len = e->hist_len;
    int32_t next_emit = s;
    int32_t s_limit = (int32_t)(len - DEFL_INPUT_MARGIN);
    uint64_t cv = defl_load_le64(src, s);

    for (;;) {
        enum { SKIP_LOG = 6, DO_EVERY = 1 };

        int32_t next_s = s;
        int32_t t;
        for (;;) {
            uint32_t next_hash_s = defl_hash_len(cv, DEFL_TABLE_BITS, HASH_SHORT_BYTES);
            uint32_t next_hash_l =
                defl_hash_len(cv, DEFL_TABLE_BITS, DEFL_HASH_LONG_BYTES);

            s = next_s;
            next_s = s + DO_EVERY + ((s - next_emit) >> SKIP_LOG);
            if (next_s > s_limit)
                goto emit_remainder;
            /* Fetch a short+long candidate. */
            int32_t s_candidate = table[next_hash_s];
            int32_t l_candidate = btable[next_hash_l];
            uint64_t next = defl_load_le64(src, next_s);
            int32_t entry = s + e->cur;
            table[next_hash_s] = entry;
            btable[next_hash_l] = entry;

            t = l_candidate - e->cur;
            if (s - t < DEFL_MAX_MATCH_OFFSET && (uint32_t)cv == defl_load_le32(src, t))
                /* We got a long match. Use that. */
                break;

            t = s_candidate - e->cur;
            if (s - t < DEFL_MAX_MATCH_OFFSET &&
                (uint32_t)cv == defl_load_le32(src, t)) {
                /* Found a 4 match... */
                l_candidate =
                    btable[defl_hash_len(next, DEFL_TABLE_BITS, DEFL_HASH_LONG_BYTES)];

                /* If the next long is a candidate, check if we should use
                 * that instead... */
                int32_t l_off = l_candidate - e->cur;
                if (next_s - l_off < DEFL_MAX_MATCH_OFFSET &&
                    defl_load_le32(src, l_off) == (uint32_t)next) {
                    Int l1 = defl_match_len(src + s + 4, len - s - 4, src + t + 4);
                    Int l2 = defl_match_len(src + next_s + 4, len - next_s - 4,
                                            src + next_s - l_off + 4);
                    if (l2 > l1) {
                        s = next_s;
                        t = l_candidate - e->cur;
                    }
                }
                break;
            }
            cv = next;
        }

        /* A 4-byte match has been found. Extend the 4-byte match as long as
         * possible. */
        int32_t l = defl_match_len_long(s + 4, t + 4, src, len) + 4;

        /* Extend backwards. */
        while (t > 0 && s > next_emit && src[t - 1] == src[s - 1]) {
            s--;
            t--;
            l++;
        }
        if (next_emit < s)
            defl_emit_range(dst, src, next_emit, s);

        defl_tokens_add_match_long(dst, l, (uint32_t)(s - t - DEFL_BASE_MATCH_OFFSET));
        s += l;
        next_emit = s;
        if (next_s >= s)
            s = next_s + 1;

        if (s >= s_limit) {
            /* Index first pair after match end. */
            if ((Int)s + 8 < len) {
                uint64_t cv2 = defl_load_le64(src, s);
                table[defl_hash_len(cv2, DEFL_TABLE_BITS, HASH_SHORT_BYTES)] =
                    s + e->cur;
                btable[defl_hash_len(cv2, DEFL_TABLE_BITS, DEFL_HASH_LONG_BYTES)] =
                    s + e->cur;
            }
            goto emit_remainder;
        }

        /* Store every 3rd hash in-between. */
        int32_t i = next_s;
        if (i < s - 1) {
            uint64_t cv2 = defl_load_le64(src, i);
            int32_t te = i + e->cur;
            int32_t t2 = te + 1;
            btable[defl_hash_len(cv2, DEFL_TABLE_BITS, DEFL_HASH_LONG_BYTES)] = te;
            btable[defl_hash_len(cv2 >> 8, DEFL_TABLE_BITS, DEFL_HASH_LONG_BYTES)] = t2;
            table[defl_hash_len(cv2 >> 8, DEFL_TABLE_BITS, HASH_SHORT_BYTES)] = t2;

            i += 3;
            for (; i < s - 1; i += 3) {
                cv2 = defl_load_le64(src, i);
                te = i + e->cur;
                t2 = te + 1;
                btable[defl_hash_len(cv2, DEFL_TABLE_BITS, DEFL_HASH_LONG_BYTES)] = te;
                btable[defl_hash_len(cv2 >> 8, DEFL_TABLE_BITS, DEFL_HASH_LONG_BYTES)] =
                    t2;
                table[defl_hash_len(cv2 >> 8, DEFL_TABLE_BITS, HASH_SHORT_BYTES)] = t2;
            }
        }

        /* We could immediately start working at s now, but to improve
         * compression we first update the hash table at s-1 and at s. */
        uint64_t x = defl_load_le64(src, s - 1);
        int32_t o = e->cur + s - 1;
        uint32_t prev_hash_s = defl_hash_len(x, DEFL_TABLE_BITS, HASH_SHORT_BYTES);
        uint32_t prev_hash_l = defl_hash_len(x, DEFL_TABLE_BITS, DEFL_HASH_LONG_BYTES);
        table[prev_hash_s] = o;
        btable[prev_hash_l] = o;
        cv = x >> 8;
    }

emit_remainder:
    defl_emit_remainder(dst, src, len, next_emit);
}

/* Level 5 is level 4 with the last two positions kept for each long hash,
 * and a look for a better match just after each one it finds. */
static void defl_encode_l5(DeflFast *e, DeflTokens *dst, const Byte *in, Int in_len) {
    enum { HASH_SHORT_BYTES = 4 };
    int32_t *table = e->table;
    DeflPrevEntry *btable = e->ptable;

    defl_fast_shift(e);

    int32_t s = defl_fast_add_block(e, in, in_len);

    if (in_len < DEFL_MIN_NON_LITERAL_BLOCK_SIZE) {
        dst->n = (uint16_t)in_len;
        return;
    }

    const Byte *src = e->hist;
    Int len = e->hist_len;
    int32_t next_emit = s;
    int32_t s_limit = (int32_t)(len - DEFL_INPUT_MARGIN);
    uint64_t cv = defl_load_le64(src, s);

    for (;;) {
        enum { SKIP_LOG = 6, DO_EVERY = 1 };

        int32_t next_s = s;
        int32_t l = 0;
        int32_t t;
        for (;;) {
            uint32_t next_hash_s = defl_hash_len(cv, DEFL_TABLE_BITS, HASH_SHORT_BYTES);
            uint32_t next_hash_l =
                defl_hash_len(cv, DEFL_TABLE_BITS, DEFL_HASH_LONG_BYTES);

            s = next_s;
            next_s = s + DO_EVERY + ((s - next_emit) >> SKIP_LOG);
            if (next_s > s_limit)
                goto emit_remainder;
            /* Fetch a short+long candidate. */
            int32_t s_candidate = table[next_hash_s];
            DeflPrevEntry l_candidate = btable[next_hash_l];
            uint64_t next = defl_load_le64(src, next_s);
            int32_t entry = s + e->cur;
            table[next_hash_s] = entry;
            defl_ptable_push(&btable[next_hash_l], entry);

            next_hash_s = defl_hash_len(next, DEFL_TABLE_BITS, HASH_SHORT_BYTES);
            next_hash_l = defl_hash_len(next, DEFL_TABLE_BITS, DEFL_HASH_LONG_BYTES);

            t = l_candidate.cur - e->cur;
            if (s - t < DEFL_MAX_MATCH_OFFSET) {
                if ((uint32_t)cv == defl_load_le32(src, t)) {
                    /* Store the next match. */
                    table[next_hash_s] = next_s + e->cur;
                    defl_ptable_push(&btable[next_hash_l], next_s + e->cur);

                    int32_t t2 = l_candidate.prev - e->cur;
                    if (s - t2 < DEFL_MAX_MATCH_OFFSET &&
                        (uint32_t)cv == defl_load_le32(src, t2)) {
                        l = defl_match_len_limited(s + 4, t + 4, src, len) + 4;
                        int32_t ml1 =
                            defl_match_len_limited(s + 4, t2 + 4, src, len) + 4;
                        if (ml1 > l) {
                            t = t2;
                            l = ml1;
                            break;
                        }
                    }
                    break;
                }
                t = l_candidate.prev - e->cur;
                if (s - t < DEFL_MAX_MATCH_OFFSET &&
                    (uint32_t)cv == defl_load_le32(src, t)) {
                    /* Store the next match. */
                    table[next_hash_s] = next_s + e->cur;
                    defl_ptable_push(&btable[next_hash_l], next_s + e->cur);
                    break;
                }
            }

            t = s_candidate - e->cur;
            if (s - t < DEFL_MAX_MATCH_OFFSET &&
                (uint32_t)cv == defl_load_le32(src, t)) {
                /* Found a 4 match... */
                l = defl_match_len_limited(s + 4, t + 4, src, len) + 4;
                l_candidate = btable[next_hash_l];

                /* Store the next match. */
                table[next_hash_s] = next_s + e->cur;
                defl_ptable_push(&btable[next_hash_l], next_s + e->cur);

                /* If the next long is a candidate, use that... */
                int32_t t2 = l_candidate.cur - e->cur;
                if (next_s - t2 < DEFL_MAX_MATCH_OFFSET) {
                    if (defl_load_le32(src, t2) == (uint32_t)next) {
                        int32_t ml =
                            defl_match_len_limited(next_s + 4, t2 + 4, src, len) + 4;
                        if (ml > l) {
                            t = t2;
                            s = next_s;
                            l = ml;
                            break;
                        }
                    }
                    /* If the previous long is a candidate, use that... */
                    t2 = l_candidate.prev - e->cur;
                    if (next_s - t2 < DEFL_MAX_MATCH_OFFSET &&
                        defl_load_le32(src, t2) == (uint32_t)next) {
                        int32_t ml =
                            defl_match_len_limited(next_s + 4, t2 + 4, src, len) + 4;
                        if (ml > l) {
                            t = t2;
                            s = next_s;
                            l = ml;
                            break;
                        }
                    }
                }
                break;
            }
            cv = next;
        }

        /* A 4-byte match has been found. Extend it as long as possible. */
        if (l == 0)
            l = defl_match_len_long(s + 4, t + 4, src, len) + 4;
        else if (l == DEFL_MAX_MATCH_LENGTH)
            l += defl_match_len_long(s + l, t + l, src, len);

        /* Try to locate a better match by checking the end of the best
         * match... */
        int32_t s_at = s + l;
        if (l < 30 && s_at < s_limit) {
            /* Allow some bytes at the beginning to mismatch. Sweet spot is
             * 2/3 bytes depending on input. 3 is only a little better when
             * it is but sometimes a lot worse. The skipped bytes are tested
             * in Extend backwards, and still picked up as part of the
             * match if they do. */
            enum { SKIP_BEGINNING = 2 };
            int32_t e_long =
                btable[defl_hash_len(defl_load_le64(src, s_at), DEFL_TABLE_BITS,
                                     DEFL_HASH_LONG_BYTES)]
                    .cur;
            int32_t t2 = e_long - e->cur - l + SKIP_BEGINNING;
            int32_t s2 = s + SKIP_BEGINNING;
            int32_t off = s2 - t2;
            if (t2 >= 0 && off < DEFL_MAX_MATCH_OFFSET && off > 0) {
                int32_t l2 = defl_match_len_long(s2, t2, src, len);
                if (l2 > l) {
                    t = t2;
                    l = l2;
                    s = s2;
                }
            }
        }

        /* Extend backwards. */
        while (t > 0 && s > next_emit && src[t - 1] == src[s - 1]) {
            s--;
            t--;
            l++;
        }
        if (next_emit < s)
            defl_emit_range(dst, src, next_emit, s);

        defl_tokens_add_match_long(dst, l, (uint32_t)(s - t - DEFL_BASE_MATCH_OFFSET));
        s += l;
        next_emit = s;
        if (next_s >= s)
            s = next_s + 1;

        if (s >= s_limit)
            goto emit_remainder;

        /* Store every 3rd hash in-between. */
        enum { HASH_EVERY = 3 };
        int32_t i = s - l + 1;
        if (i < s - 1) {
            uint64_t cv2 = defl_load_le64(src, i);
            int32_t te = i + e->cur;
            table[defl_hash_len(cv2, DEFL_TABLE_BITS, HASH_SHORT_BYTES)] = te;
            defl_ptable_push(
                &btable[defl_hash_len(cv2, DEFL_TABLE_BITS, DEFL_HASH_LONG_BYTES)], te);

            /* Do an long at i+1 */
            cv2 >>= 8;
            te = te + 1;
            defl_ptable_push(
                &btable[defl_hash_len(cv2, DEFL_TABLE_BITS, DEFL_HASH_LONG_BYTES)], te);

            /* We only have enough bits for a short entry at i+2 */
            cv2 >>= 8;
            te = te + 1;
            table[defl_hash_len(cv2, DEFL_TABLE_BITS, HASH_SHORT_BYTES)] = te;

            /* Skip one - otherwise we risk hitting 's'. */
            i += 4;
            for (; i < s - 1; i += HASH_EVERY) {
                cv2 = defl_load_le64(src, i);
                te = i + e->cur;
                int32_t t2 = te + 1;
                defl_ptable_push(
                    &btable[defl_hash_len(cv2, DEFL_TABLE_BITS, DEFL_HASH_LONG_BYTES)],
                    te);
                table[defl_hash_len(cv2 >> 8, DEFL_TABLE_BITS, HASH_SHORT_BYTES)] = t2;
            }
        }

        /* We could immediately start working at s now, but to improve
         * compression we first update the hash table at s-1 and at s. */
        uint64_t x = defl_load_le64(src, s - 1);
        int32_t o = e->cur + s - 1;
        uint32_t prev_hash_s = defl_hash_len(x, DEFL_TABLE_BITS, HASH_SHORT_BYTES);
        uint32_t prev_hash_l = defl_hash_len(x, DEFL_TABLE_BITS, DEFL_HASH_LONG_BYTES);
        table[prev_hash_s] = o;
        defl_ptable_push(&btable[prev_hash_l], o);
        cv = x >> 8;
    }

emit_remainder:
    defl_emit_remainder(dst, src, len, next_emit);
}

/* Level 6 is level 5 with a check for a repeat of the last offset, and more
 * of each match indexed. */
static void defl_encode_l6(DeflFast *e, DeflTokens *dst, const Byte *in, Int in_len) {
    enum { HASH_SHORT_BYTES = 4 };
    int32_t *table = e->table;
    DeflPrevEntry *btable = e->ptable;

    defl_fast_shift(e);

    int32_t s = defl_fast_add_block(e, in, in_len);

    if (in_len < DEFL_MIN_NON_LITERAL_BLOCK_SIZE) {
        dst->n = (uint16_t)in_len;
        return;
    }

    const Byte *src = e->hist;
    Int len = e->hist_len;
    int32_t next_emit = s;
    int32_t s_limit = (int32_t)(len - DEFL_INPUT_MARGIN);
    uint64_t cv = defl_load_le64(src, s);
    /* Repeat MUST be > 1 and within range. */
    int32_t repeat = 1;

    for (;;) {
        enum { SKIP_LOG = 7, DO_EVERY = 1 };

        int32_t next_s = s;
        int32_t l = 0;
        int32_t t;
        for (;;) {
            uint32_t next_hash_s = defl_hash_len(cv, DEFL_TABLE_BITS, HASH_SHORT_BYTES);
            uint32_t next_hash_l =
                defl_hash_len(cv, DEFL_TABLE_BITS, DEFL_HASH_LONG_BYTES);
            s = next_s;
            next_s = s + DO_EVERY + ((s - next_emit) >> SKIP_LOG);
            if (next_s > s_limit)
                goto emit_remainder;
            /* Fetch a short+long candidate. */
            int32_t s_candidate = table[next_hash_s];
            DeflPrevEntry l_candidate = btable[next_hash_l];
            uint64_t next = defl_load_le64(src, next_s);
            int32_t entry = s + e->cur;
            table[next_hash_s] = entry;
            defl_ptable_push(&btable[next_hash_l], entry);

            /* Calculate hashes of 'next'. */
            next_hash_s = defl_hash_len(next, DEFL_TABLE_BITS, HASH_SHORT_BYTES);
            next_hash_l = defl_hash_len(next, DEFL_TABLE_BITS, DEFL_HASH_LONG_BYTES);

            t = l_candidate.cur - e->cur;
            if (s - t < DEFL_MAX_MATCH_OFFSET) {
                if ((uint32_t)cv == defl_load_le32(src, t)) {
                    /* Long candidate matches at least 4 bytes. Store the
                     * next match. */
                    table[next_hash_s] = next_s + e->cur;
                    defl_ptable_push(&btable[next_hash_l], next_s + e->cur);

                    /* Check the previous long candidate as well. */
                    int32_t t2 = l_candidate.prev - e->cur;
                    if (s - t2 < DEFL_MAX_MATCH_OFFSET &&
                        (uint32_t)cv == defl_load_le32(src, t2)) {
                        l = defl_match_len_limited(s + 4, t + 4, src, len) + 4;
                        int32_t ml1 =
                            defl_match_len_limited(s + 4, t2 + 4, src, len) + 4;
                        if (ml1 > l) {
                            t = t2;
                            l = ml1;
                            break;
                        }
                    }
                    break;
                }
                /* Current value did not match, but check if previous long
                 * value does. */
                t = l_candidate.prev - e->cur;
                if (s - t < DEFL_MAX_MATCH_OFFSET &&
                    (uint32_t)cv == defl_load_le32(src, t)) {
                    /* Store the next match. */
                    table[next_hash_s] = next_s + e->cur;
                    defl_ptable_push(&btable[next_hash_l], next_s + e->cur);
                    break;
                }
            }

            t = s_candidate - e->cur;
            if (s - t < DEFL_MAX_MATCH_OFFSET &&
                (uint32_t)cv == defl_load_le32(src, t)) {
                /* Found a 4 match... */
                l = defl_match_len_limited(s + 4, t + 4, src, len) + 4;

                /* Look up next long candidate (at next_s). */
                l_candidate = btable[next_hash_l];

                /* Store the next match. */
                table[next_hash_s] = next_s + e->cur;
                defl_ptable_push(&btable[next_hash_l], next_s + e->cur);

                /* Check repeat at s + rep_off. */
                enum { REP_OFF = 1 };
                int32_t t2 = s - repeat + REP_OFF;
                if (defl_load_le32(src, t2) == (uint32_t)(cv >> (8 * REP_OFF))) {
                    int32_t ml =
                        defl_match_len_limited(s + 4 + REP_OFF, t2 + 4, src, len) + 4;
                    if (ml > l) {
                        t = t2;
                        l = ml;
                        s += REP_OFF;
                        /* Not worth checking more. */
                        break;
                    }
                }

                /* If the next long is a candidate, use that... */
                t2 = l_candidate.cur - e->cur;
                if (next_s - t2 < DEFL_MAX_MATCH_OFFSET) {
                    if (defl_load_le32(src, t2) == (uint32_t)next) {
                        int32_t ml =
                            defl_match_len_limited(next_s + 4, t2 + 4, src, len) + 4;
                        if (ml > l) {
                            t = t2;
                            s = next_s;
                            l = ml;
                            /* This is ok, but check previous as well. */
                        }
                    }
                    /* If the previous long is a candidate, use that... */
                    t2 = l_candidate.prev - e->cur;
                    if (next_s - t2 < DEFL_MAX_MATCH_OFFSET &&
                        defl_load_le32(src, t2) == (uint32_t)next) {
                        int32_t ml =
                            defl_match_len_limited(next_s + 4, t2 + 4, src, len) + 4;
                        if (ml > l) {
                            t = t2;
                            s = next_s;
                            l = ml;
                            break;
                        }
                    }
                }
                break;
            }
            cv = next;
        }

        /* A 4-byte match has been found. Extend it as long as possible. */
        if (l == 0)
            l = defl_match_len_long(s + 4, t + 4, src, len) + 4;
        else if (l == DEFL_MAX_MATCH_LENGTH)
            l += defl_match_len_long(s + l, t + l, src, len);

        /* Try to locate a better match by checking the end-of-match... */
        int32_t s_at = s + l;
        if (s_at < s_limit) {
            /* Allow some bytes at the beginning to mismatch. Sweet spot is
             * 2/3 bytes depending on input. 3 is only a little better when
             * it is but sometimes a lot worse. The skipped bytes are tested
             * in Extend backwards, and still picked up as part of the
             * match if they do. */
            enum { SKIP_BEGINNING = 2 };
            const DeflPrevEntry *e_long = &btable[defl_hash_len(
                defl_load_le64(src, s_at), DEFL_TABLE_BITS, DEFL_HASH_LONG_BYTES)];
            /* Test current. */
            int32_t t2 = e_long->cur - e->cur - l + SKIP_BEGINNING;
            int32_t s2 = s + SKIP_BEGINNING;
            int32_t off = s2 - t2;
            if (off < DEFL_MAX_MATCH_OFFSET) {
                if (off > 0 && t2 >= 0) {
                    int32_t l2 = defl_match_len_long(s2, t2, src, len);
                    if (l2 > l) {
                        t = t2;
                        l = l2;
                        s = s2;
                    }
                }
                /* Test previous entry. */
                t2 = e_long->prev - e->cur - l + SKIP_BEGINNING;
                off = s2 - t2;
                if (off > 0 && off < DEFL_MAX_MATCH_OFFSET && t2 >= 0) {
                    int32_t l2 = defl_match_len_long(s2, t2, src, len);
                    if (l2 > l) {
                        t = t2;
                        l = l2;
                        s = s2;
                    }
                }
            }
        }

        /* Extend backwards. */
        while (t > 0 && s > next_emit && src[t - 1] == src[s - 1]) {
            s--;
            t--;
            l++;
        }
        if (next_emit < s)
            defl_emit_range(dst, src, next_emit, s);

        defl_tokens_add_match_long(dst, l, (uint32_t)(s - t - DEFL_BASE_MATCH_OFFSET));
        repeat = s - t;
        s += l;
        next_emit = s;
        if (next_s >= s)
            s = next_s + 1;

        if (s >= s_limit) {
            /* Index after match end. */
            for (int32_t i = next_s + 1; i < (int32_t)len - 8; i += 2) {
                uint64_t cv2 = defl_load_le64(src, i);
                table[defl_hash_len(cv2, DEFL_TABLE_BITS, HASH_SHORT_BYTES)] =
                    i + e->cur;
                defl_ptable_push(
                    &btable[defl_hash_len(cv2, DEFL_TABLE_BITS, DEFL_HASH_LONG_BYTES)],
                    i + e->cur);
            }
            goto emit_remainder;
        }

        /* Store every long hash in-between and every second short. */
        for (int32_t i = next_s + 1; i < s - 1; i += 2) {
            uint64_t cv2 = defl_load_le64(src, i);
            int32_t te = i + e->cur;
            int32_t t2 = te + 1;
            DeflPrevEntry *e_long =
                &btable[defl_hash_len(cv2, DEFL_TABLE_BITS, DEFL_HASH_LONG_BYTES)];
            DeflPrevEntry *e_long2 =
                &btable[defl_hash_len(cv2 >> 8, DEFL_TABLE_BITS, DEFL_HASH_LONG_BYTES)];
            table[defl_hash_len(cv2, DEFL_TABLE_BITS, HASH_SHORT_BYTES)] = te;
            defl_ptable_push(e_long, te);
            defl_ptable_push(e_long2, t2);
        }
        cv = defl_load_le64(src, s);
    }

emit_remainder:
    defl_emit_remainder(dst, src, len, next_emit);
}

static void defl_fast_encode(DeflFast *e, DeflTokens *dst, const Byte *src, Int n) {
    switch (e->level) {
    case 1:
        defl_encode_l1(e, dst, src, n);
        break;
    case 2:
        defl_encode_l2(e, dst, src, n);
        break;
    case 3:
        defl_encode_l3(e, dst, src, n);
        break;
    case 4:
        defl_encode_l4(e, dst, src, n);
        break;
    case 5:
        defl_encode_l5(e, dst, src, n);
        break;
    default:
        defl_encode_l6(e, dst, src, n);
        break;
    }
}

/* -------------------------------------------------------------- compressor */

typedef struct DeflLevel {
    int32_t good;  /* "good enough" match length */
    int32_t lazy;  /* don't try to find a later, better match above this length */
    int32_t nice;  /* stop looking for a better match above this length */
    int32_t chain; /* maximum number of hash chain entries to search */
} DeflLevel;

static const DeflLevel defl_levels[10] = {
    {0, 0, 0, 0},
    {0, 0, 0, 0},
    {0, 0, 0, 0},
    {0, 0, 0, 0},
    {0, 0, 0, 0},
    {0, 0, 0, 0},
    {0, 0, 0, 0},
    /* Levels 7-9 use increasingly more lazy matching and increasingly
     * stringent conditions for "good enough". */
    {8, 12, 16, 24},
    {16, 30, 40, 64},
    {32, 258, 258, 1024},
};

/* The state of the lazy matcher of levels 7 to 9. */
typedef struct DeflAdvanced {
    /* Deflate state. */
    int32_t length;
    int32_t offset;
    int32_t max_insert_index;
    int32_t chain_head;
    int32_t hash_offset;

    uint16_t
        literal_counter; /* consecutive literals; overflows to reset after 64 KiB */

    /* Input hash chains. hash_head[hash_value] contains the largest input
     * index with the given hash value. hash_prev[index & DEFL_WINDOW_MASK]
     * contains the previous input index with the same hash value. */
    int32_t index;
    uint32_t hash_match[DEFL_MAX_MATCH_LENGTH + DEFL_MIN_MATCH_LENGTH];

    int32_t hash_head[DEFL_HASH_SIZE];
    int32_t hash_prev[DEFL_WINDOW_SIZE];
} DeflAdvanced;

typedef enum DeflStep {
    DEFL_STEP_STORE,
    DEFL_STEP_HUFF,
    DEFL_STEP_FAST,
    DEFL_STEP_LAZY,
} DeflStep;

struct FlateWriter {
    Alloc *a;
    DeflLevel lv;
    int level;

    DeflHuffEnc *h;    /* the byte costs level 9 weighs matches with */
    DeflBitWriter w;   /* writer for blocks */
    DeflStep step;     /* how the window is processed */
    bool fill_deflate; /* whether the window is filled the way of levels 7-9 */

    Byte *window; /* current window, the size of which depends on the level */
    Int window_len;
    int32_t window_end;  /* filled bytes in window */
    int32_t block_start; /* window index where current tokens start */
    Error err;           /* stateful error */

    DeflTokens *tokens;  /* tokens store for each block */
    DeflFast *fast;      /* encoder to use for blocks of levels 1 to 6 */
    DeflAdvanced *state; /* chained encoder for levels 7 to 9 */

    bool sync;           /* requesting flush */
    bool byte_available; /* if true, still need to process window[index-1] */

    Byte *dict;
    Int dict_len;
    DeflFast fast_store;
};

static const Str defl_err_writer_closed__text = {(const Byte *)"flate: closed writer",
                                                 20};
static const Error defl_err_writer_closed = {&burrow_sentinel_error_vt,
                                             &defl_err_writer_closed__text};

static bool defl_same_error(Error a, Error b) {
    return a.vt == b.vt && a.data == b.data;
}

static void defl_fill_deflate(FlateWriter *d, const Byte *b, Int n, Int *used) {
    DeflAdvanced *s = d->state;
    if (s->index >=
        2 * DEFL_WINDOW_SIZE - (DEFL_MIN_MATCH_LENGTH + DEFL_MAX_MATCH_LENGTH)) {
        /* Shift the window by DEFL_WINDOW_SIZE. */
        memcpy(d->window, d->window + DEFL_WINDOW_SIZE, DEFL_WINDOW_SIZE);
        s->index -= DEFL_WINDOW_SIZE;
        d->window_end -= DEFL_WINDOW_SIZE;
        if (d->block_start >= DEFL_WINDOW_SIZE)
            d->block_start -= DEFL_WINDOW_SIZE;
        else
            d->block_start = DEFL_MAX_INT32;
        s->hash_offset += DEFL_WINDOW_SIZE;
        if (s->hash_offset > DEFL_MAX_HASH_OFFSET) {
            int32_t delta = s->hash_offset - 1;
            s->hash_offset -= delta;
            s->chain_head -= delta;
            /* Iterate over slices instead of arrays to avoid copying the
             * entire table onto the stack (Issue #18625). */
            for (Int i = 0; i < DEFL_WINDOW_SIZE; i++) {
                int32_t v = s->hash_prev[i] - delta;
                s->hash_prev[i] = v > 0 ? v : 0;
            }
            for (Int i = 0; i < DEFL_HASH_SIZE; i++) {
                int32_t v = s->hash_head[i] - delta;
                s->hash_head[i] = v > 0 ? v : 0;
            }
        }
    }
    Int room = d->window_len - d->window_end;
    Int c = n < room ? n : room;
    if (c > 0)
        memcpy(d->window + d->window_end, b, (size_t)c);
    d->window_end += (int32_t)c;
    *used = c;
}

static void defl_fill_block(FlateWriter *d, const Byte *b, Int n, Int *used) {
    Int room = d->window_len - d->window_end;
    Int c = n < room ? n : room;
    if (c > 0)
        memcpy(d->window + d->window_end, b, (size_t)c);
    d->window_end += (int32_t)c;
    *used = c;
}

static Error defl_write_block(FlateWriter *d, DeflTokens *tok, int32_t index,
                              bool eof) {
    if (index > 0 || eof) {
        const Byte *window = NULL;
        Int n = 0;
        if (d->block_start <= index) {
            window = d->window + d->block_start;
            n = index - d->block_start;
        }
        d->block_start = index;
        defl_bw_write_block_dynamic(&d->w, tok, eof, window, n, d->sync);
        return d->w.err;
    }
    return BURROW_NO_ERROR;
}

static uint32_t defl_hash4u(uint32_t u, uint8_t h) {
    return (u * DEFL_PRIME4) >> (32 - h);
}

static uint32_t defl_hash4(const Byte *b) {
    return defl_hash4u(defl_load_le32(b, 0), DEFL_HASH_BITS);
}

/* Applies hash4 to each consecutive 4 bytes of b, n of them, into dst. */
static void defl_bulk_hash4(const Byte *b, Int n, uint32_t *dst) {
    if (n < 4)
        return;
    uint32_t hb = defl_load_le32(b, 0);

    dst[0] = defl_hash4u(hb, DEFL_HASH_BITS);
    Int end = n - 4 + 1;
    for (Int i = 1; i < end; i++) {
        hb = (hb >> 8) | (uint32_t)b[i + 3] << 24;
        dst[i] = defl_hash4u(hb, DEFL_HASH_BITS);
    }
}

/* Primes the compressor with b, a preset dictionary, without writing
 * anything. */
static void defl_fill_window(FlateWriter *d, const Byte *b, Int n) {
    /* Do not fill window if we are in store-only or huffman mode. */
    if (d->level <= 0)
        return;
    if (d->fast != NULL) {
        /* Encode the last data, but discard the result. */
        if (n > DEFL_MAX_MATCH_OFFSET) {
            b += n - DEFL_MAX_MATCH_OFFSET;
            n = DEFL_MAX_MATCH_OFFSET;
        }
        defl_fast_encode(d->fast, d->tokens, b, n);
        defl_tokens_reset(d->tokens);
        return;
    }
    DeflAdvanced *s = d->state;
    /* If we are given too much, cut it. */
    if (n > DEFL_WINDOW_SIZE) {
        b += n - DEFL_WINDOW_SIZE;
        n = DEFL_WINDOW_SIZE;
    }
    /* Add all to window. */
    Int room = d->window_len - d->window_end;
    int32_t c = (int32_t)(n < room ? n : room);
    if (c > 0)
        memcpy(d->window + d->window_end, b, (size_t)c);

    /* Calculate 256 hashes at the time (more L1 cache hits). */
    int32_t loops = (c + 256 - DEFL_MIN_MATCH_LENGTH) / 256;
    for (int32_t j = 0; j < loops; j++) {
        int32_t startindex = j * 256;
        int32_t end = startindex + 256 + DEFL_MIN_MATCH_LENGTH - 1;
        if (end > c)
            end = c;
        const Byte *tocheck = d->window + startindex;
        Int tocheck_len = end - startindex;
        Int dst_size = tocheck_len - DEFL_MIN_MATCH_LENGTH + 1;

        if (dst_size <= 0)
            continue;

        uint32_t *dst = s->hash_match;
        defl_bulk_hash4(tocheck, tocheck_len, dst);
        for (Int i = 0; i < dst_size; i++) {
            int32_t di = (int32_t)i + startindex;
            uint32_t new_h = dst[i] & DEFL_HASH_MASK;
            /* Get previous value with the same hash. Our chain should point
             * to the previous value. */
            s->hash_prev[di & DEFL_WINDOW_MASK] = s->hash_head[new_h];
            /* Set the head of the hash chain to us. */
            s->hash_head[new_h] = di + s->hash_offset;
        }
    }
    /* Update window information. */
    d->window_end += c;
    s->index = c;
}

/* Tries to find a match starting at pos whose length is greater than the
 * current best match, looking back along the hash chain from prev_head. The
 * match, if any, goes in *length and *offset. */
static bool defl_find_match(FlateWriter *d, int32_t pos, int32_t prev_head,
                            int32_t lookahead, int32_t *length_out,
                            int32_t *offset_out) {
    int32_t min_match_look =
        lookahead < DEFL_MAX_MATCH_LENGTH ? lookahead : DEFL_MAX_MATCH_LENGTH;

    const Byte *win = d->window;
    int32_t win_len = pos + min_match_look;

    /* We quit when we get a match that's at least nice long. */
    int32_t nice = d->lv.nice < win_len - pos ? d->lv.nice : win_len - pos;

    /* If we've got a match that's good enough, only look in 1/4 the chain. */
    int32_t tries = d->lv.chain;
    int32_t length = DEFL_MIN_MATCH_LENGTH - 1;
    int32_t offset = 0;
    bool ok = false;

    Byte w_end = win[pos + length];
    const Byte *w_pos = win + pos;
    int32_t min_index = pos - DEFL_WINDOW_SIZE > 0 ? pos - DEFL_WINDOW_SIZE : 0;

    Int c_gain = 4;

    /* Base is 4 bytes at with an additional cost. Matches must be better
     * than this. */
    enum { BASE_COST = 3 };

    for (int32_t i = prev_head; tries > 0; tries--) {
        if (w_end == win[i + length]) {
            int32_t n = (int32_t)defl_match_len(win + i, min_match_look, w_pos);
            if (n > length) {
                /* Calculate gain. Estimates the gains of the new match
                 * compared to emitting as literals. */
                if (d->lv.chain >= 100) {
                    Int new_gain =
                        defl_bit_length_raw(d->h, w_pos, n) -
                        (Int)defl_offset_extra_bits[defl_offset_code(
                            (uint32_t)(pos - i))] -
                        BASE_COST -
                        (Int)defl_length_extra_bits[defl_length_codes[(n - 3) & 255]];
                    if (new_gain <= c_gain)
                        goto next;
                    c_gain = new_gain;
                }
                length = n;
                offset = pos - i;
                ok = true;
                if (n >= nice)
                    /* The match is good enough that we don't try to find a
                     * better one. */
                    break;
                w_end = win[pos + n];
            }
        }
    next:
        if (i <= min_index)
            /* hash_prev[i & DEFL_WINDOW_MASK] has already been overwritten,
             * so stop now. */
            break;
        i = d->state->hash_prev[i & DEFL_WINDOW_MASK] - d->state->hash_offset;
        if (i < min_index)
            break;
    }
    *length_out = length;
    *offset_out = offset;
    return ok;
}

static Error defl_write_stored_block(FlateWriter *d, const Byte *buf, Int n) {
    defl_bw_write_stored_header(&d->w, n, false);
    if (BURROW_FAILED(d->w.err))
        return d->w.err;
    defl_bw_write_bytes(&d->w, buf, n);
    return d->w.err;
}

static void defl_init_deflate(FlateWriter *d) {
    d->byte_available = false;
    d->err = BURROW_NO_ERROR;
    if (d->state == NULL)
        return;
    DeflAdvanced *s = d->state;
    s->index = 0;
    s->hash_offset = 1;
    s->length = DEFL_MIN_MATCH_LENGTH - 1;
    s->offset = 0;
    s->chain_head = -1;
}

/* Adds the literal at window[s->index], and inserts it into the hash
 * chains, which the lazy matcher does in a few places. */
static void defl_insert_index(FlateWriter *d) {
    DeflAdvanced *s = d->state;
    uint32_t h = defl_hash4(d->window + s->index);
    int32_t ch = s->hash_head[h];
    s->chain_head = ch;
    s->hash_prev[s->index & DEFL_WINDOW_MASK] = ch;
    s->hash_head[h] = s->index + s->hash_offset;
}

/* Checks whether a match that starts a byte or two earlier and ends where
 * the previous one did is longer, and takes it when it is. */
static void defl_try_better_match_at_end(FlateWriter *d, int32_t *prev_length_io,
                                         int32_t *prev_offset_io, int32_t lookahead) {
    enum { CHECK_OFF = 2 };
    DeflAdvanced *s = d->state;
    int32_t prev_length = *prev_length_io;

    if (prev_length >= DEFL_MAX_MATCH_LENGTH - CHECK_OFF)
        return;
    int32_t prev_index = s->index - 1;
    if (prev_index + prev_length >= s->max_insert_index)
        return;

    int32_t end = (lookahead < DEFL_MAX_MATCH_LENGTH + CHECK_OFF
                       ? lookahead
                       : DEFL_MAX_MATCH_LENGTH + CHECK_OFF) +
                  prev_index;
    int32_t min_index =
        s->index - DEFL_WINDOW_SIZE > 0 ? s->index - DEFL_WINDOW_SIZE : 0;

    uint32_t h = defl_hash4(d->window + prev_index + prev_length);
    int32_t ch2 = s->hash_head[h] - s->hash_offset - prev_length;
    if (prev_index - ch2 == *prev_offset_io || ch2 <= min_index + CHECK_OFF)
        return;

    /* Check the end of the match. */
    int32_t length = (int32_t)defl_match_len(d->window + prev_index + CHECK_OFF,
                                             end - (prev_index + CHECK_OFF),
                                             d->window + ch2 + CHECK_OFF);
    if (length <= prev_length)
        return;

    prev_length = length;
    int32_t prev_offset = prev_index - ch2;
    *prev_length_io = prev_length;
    *prev_offset_io = prev_offset;

    /* Check the bytes before, CHECK_OFF of them. */
    for (int32_t i = CHECK_OFF - 1; i >= 0; i--) {
        if (prev_length >= DEFL_MAX_MATCH_LENGTH ||
            d->window[prev_index + i] != d->window[ch2 + i]) {
            /* Emit tokens for the bytes up to and including i. */
            for (int32_t j = 0; j < i + 1; j++) {
                defl_tokens_add_literal(d->tokens, d->window[prev_index + j]);
                if (d->tokens->n == DEFL_MAX_FLATE_BLOCK_TOKENS) {
                    /* The block includes the current character. */
                    d->err = defl_write_block(d, d->tokens, s->index, false);
                    if (BURROW_FAILED(d->err)) {
                        *prev_length_io = prev_length;
                        return;
                    }
                    defl_tokens_reset(d->tokens);
                }
                s->index++;
                if (s->index < s->max_insert_index)
                    defl_insert_index(d);
            }
            break;
        }
        prev_length++;
    }
    *prev_length_io = prev_length;
}

/* Skips ahead over literals when there have been a lot of them in a row,
 * emitting more than one at a time. False when writing a block failed. */
static bool defl_skip_literals(FlateWriter *d) {
    DeflAdvanced *s = d->state;
    int32_t n = (int32_t)s->literal_counter - d->lv.chain;
    if (n <= 0)
        return true;
    n = 1 + (n >> 6);
    for (int32_t k = 0; k < n; k++) {
        if (s->index >= d->window_end - 1)
            break;
        defl_tokens_add_literal(d->tokens, d->window[s->index - 1]);
        if (d->tokens->n == DEFL_MAX_FLATE_BLOCK_TOKENS) {
            d->err = defl_write_block(d, d->tokens, s->index, false);
            if (BURROW_FAILED(d->err))
                return false;
            defl_tokens_reset(d->tokens);
        }
        if (s->index < s->max_insert_index)
            defl_insert_index(d);
        s->index++;
    }
    /* Flush last byte. */
    defl_tokens_add_literal(d->tokens, d->window[s->index - 1]);
    d->byte_available = false;
    if (d->tokens->n == DEFL_MAX_FLATE_BLOCK_TOKENS) {
        d->err = defl_write_block(d, d->tokens, s->index, false);
        if (BURROW_FAILED(d->err))
            return false;
        defl_tokens_reset(d->tokens);
    }
    return true;
}

static void defl_deflate_lazy(FlateWriter *d) {
    DeflAdvanced *s = d->state;
    /* Sanity enables additional runtime tests. It's intended to be used
     * during development to supplement the currently ad-hoc unit tests. */

    if (d->window_end - s->index < DEFL_MIN_MATCH_LENGTH + DEFL_MAX_MATCH_LENGTH &&
        !d->sync)
        return;
    if (d->window_end != s->index && d->lv.chain > 100) {
        /* Get literal huffman coder. */
        uint16_t tmp[256];
        memset(tmp, 0, sizeof tmp);
        Int n = d->window_end - s->index;
        if (n > DEFL_MAX_FLATE_BLOCK_TOKENS)
            n = DEFL_MAX_FLATE_BLOCK_TOKENS;
        defl_histogram(d->window + s->index, n, tmp);
        defl_generate(d->h, tmp, 256, 15);
    }

    s->max_insert_index = d->window_end - (DEFL_MIN_MATCH_LENGTH - 1);

    for (;;) {
        int32_t lookahead = d->window_end - s->index;
        if (lookahead < DEFL_MIN_MATCH_LENGTH + DEFL_MAX_MATCH_LENGTH) {
            if (!d->sync)
                return;
            if (lookahead == 0) {
                /* Flush current output block if any. */
                if (d->byte_available) {
                    /* There is still one pending token that needs to be
                     * flushed. */
                    defl_tokens_add_literal(d->tokens, d->window[s->index - 1]);
                    d->byte_available = false;
                }
                if (d->tokens->n > 0) {
                    d->err = defl_write_block(d, d->tokens, s->index, false);
                    if (BURROW_FAILED(d->err))
                        return;
                    defl_tokens_reset(d->tokens);
                }
                return;
            }
        }
        if (s->index < s->max_insert_index)
            /* Update the hash. */
            defl_insert_index(d);
        int32_t prev_length = s->length;
        int32_t prev_offset = s->offset;
        s->length = DEFL_MIN_MATCH_LENGTH - 1;
        s->offset = 0;
        int32_t min_index =
            s->index - DEFL_WINDOW_SIZE > 0 ? s->index - DEFL_WINDOW_SIZE : 0;

        if (s->chain_head - s->hash_offset >= min_index && lookahead > prev_length &&
            prev_length < d->lv.lazy) {
            int32_t new_length, new_offset;
            if (defl_find_match(d, s->index, s->chain_head - s->hash_offset, lookahead,
                                &new_length, &new_offset)) {
                s->length = new_length;
                s->offset = new_offset;
            }
        }

        if (prev_length >= DEFL_MIN_MATCH_LENGTH && s->length <= prev_length) {
            /* No better match, but check for better match at end... Skip
             * forward a number of bytes. Offset of 2 seems to yield the best
             * results. 3 is sometimes better. */
            defl_try_better_match_at_end(d, &prev_length, &prev_offset, lookahead);
            if (BURROW_FAILED(d->err))
                return;

            /* There was a match at the previous step, and the current match
             * is not better. Output the previous match. */
            defl_tokens_add_match(d->tokens, (uint32_t)(prev_length - 3),
                                  (uint32_t)(prev_offset - DEFL_MIN_OFFSET_SIZE));

            /* Insert in the hash table all strings up to the end of the
             * match. index and index-1 are already inserted. If there is not
             * enough lookahead, the last two strings are not inserted into
             * the hash table. */
            int32_t new_index = s->index + prev_length - 1;
            /* Calculate missing hashes. */
            int32_t end =
                new_index < s->max_insert_index ? new_index : s->max_insert_index;
            end += DEFL_MIN_MATCH_LENGTH - 1;
            int32_t startindex =
                s->index + 1 < s->max_insert_index ? s->index + 1 : s->max_insert_index;
            const Byte *tocheck = d->window + startindex;
            Int tocheck_len = end - startindex;
            Int dst_size = tocheck_len - DEFL_MIN_MATCH_LENGTH + 1;
            if (dst_size > 0) {
                uint32_t *dst = s->hash_match;
                defl_bulk_hash4(tocheck, tocheck_len, dst);
                for (Int i = 0; i < dst_size; i++) {
                    int32_t di = (int32_t)i + startindex;
                    uint32_t new_h = dst[i] & DEFL_HASH_MASK;
                    /* Get previous value with the same hash. Our chain
                     * should point to the previous value. */
                    s->hash_prev[di & DEFL_WINDOW_MASK] = s->hash_head[new_h];
                    /* Set the head of the hash chain to us. */
                    s->hash_head[new_h] = di + s->hash_offset;
                }
            }

            s->index = new_index;
            d->byte_available = false;
            s->length = DEFL_MIN_MATCH_LENGTH - 1;
            if (d->tokens->n == DEFL_MAX_FLATE_BLOCK_TOKENS) {
                /* The block includes the current character. */
                d->err = defl_write_block(d, d->tokens, s->index, false);
                if (BURROW_FAILED(d->err))
                    return;
                defl_tokens_reset(d->tokens);
            }
            s->literal_counter = 0;
            continue;
        }
        /* Reset, if we got a match this run. */
        if (s->length >= DEFL_MIN_MATCH_LENGTH)
            s->literal_counter = 0;
        /* We have a byte waiting. Emit it. */
        if (d->byte_available) {
            s->literal_counter++;
            defl_tokens_add_literal(d->tokens, d->window[s->index - 1]);
            if (d->tokens->n == DEFL_MAX_FLATE_BLOCK_TOKENS) {
                d->err = defl_write_block(d, d->tokens, s->index, false);
                if (BURROW_FAILED(d->err))
                    return;
                defl_tokens_reset(d->tokens);
            }
            s->index++;

            /* If we have a long run of no matches, skip additional bytes.
             * Resets when s->literal_counter overflows. */
            if (!defl_skip_literals(d))
                return;
        } else {
            s->index++;
            d->byte_available = true;
        }
    }
}

static void defl_store(FlateWriter *d) {
    if (d->window_end > 0 && (d->window_end == DEFL_MAX_STORE_BLOCK_SIZE || d->sync)) {
        d->err = defl_write_stored_block(d, d->window, d->window_end);
        d->window_end = 0;
    }
}

/* Huffman codes the window without matching, for level -2. */
static void defl_deflate_huff(FlateWriter *d) {
    if (((Int)d->window_end < d->window_len && !d->sync) || d->window_end == 0)
        return;
    defl_bw_write_block_huff(&d->w, false, d->window, d->window_end, d->sync);
    d->err = d->w.err;
    d->window_end = 0;
}

/* Runs the fast encoder over the window, for levels 1 to 6. */
static void defl_deflate_fast(FlateWriter *d) {
    if ((Int)d->window_end < d->window_len) {
        if (!d->sync)
            return;
        /* Handle extremely small sizes. */
        if (d->window_end < 128) {
            if (d->window_end == 0)
                return;
            if (d->window_end <= 32) {
                d->err = defl_write_stored_block(d, d->window, d->window_end);
            } else {
                defl_bw_write_block_huff(&d->w, false, d->window, d->window_end, true);
                d->err = d->w.err;
            }
            defl_tokens_reset(d->tokens);
            d->window_end = 0;
            defl_fast_reset(d->fast);
            return;
        }
    }

    defl_fast_encode(d->fast, d->tokens, d->window, d->window_end);
    /* If we made zero matches, store the block as is. */
    if (d->tokens->n == 0) {
        d->err = defl_write_stored_block(d, d->window, d->window_end);
        /* If we removed less than 1/16th, huffman compress the block. */
    } else if ((int32_t)d->tokens->n > d->window_end - (d->window_end >> 4)) {
        defl_bw_write_block_huff(&d->w, false, d->window, d->window_end, d->sync);
        d->err = d->w.err;
    } else {
        defl_bw_write_block_dynamic(&d->w, d->tokens, false, d->window, d->window_end,
                                    d->sync);
        d->err = d->w.err;
    }
    defl_tokens_reset(d->tokens);
    d->window_end = 0;
}

static void defl_step(FlateWriter *d) {
    switch (d->step) {
    case DEFL_STEP_STORE:
        defl_store(d);
        break;
    case DEFL_STEP_HUFF:
        defl_deflate_huff(d);
        break;
    case DEFL_STEP_FAST:
        defl_deflate_fast(d);
        break;
    case DEFL_STEP_LAZY:
    default:
        defl_deflate_lazy(d);
        break;
    }
}

static Int defl_write(FlateWriter *d, const Byte *b, Int len, Error *err) {
    if (BURROW_FAILED(d->err)) {
        *err = d->err;
        return 0;
    }
    Int n = len;
    while (len > 0) {
        if ((Int)d->window_end == d->window_len || d->sync)
            defl_step(d);
        Int used;
        if (d->fill_deflate)
            defl_fill_deflate(d, b, len, &used);
        else
            defl_fill_block(d, b, len, &used);
        b += used;
        len -= used;
        if (BURROW_FAILED(d->err)) {
            *err = d->err;
            return 0;
        }
    }
    *err = d->err;
    return n;
}

static Error defl_sync_flush(FlateWriter *d) {
    if (BURROW_FAILED(d->err))
        return d->err;
    d->sync = true;
    defl_step(d);
    if (!BURROW_FAILED(d->err)) {
        defl_bw_write_stored_header(&d->w, 0, false);
        defl_bw_flush(&d->w);
        d->err = d->w.err;
    }
    d->sync = false;
    return d->err;
}

static void defl_reset(FlateWriter *d, IoWriter w) {
    defl_bw_reset(&d->w, w);
    d->sync = false;
    d->err = BURROW_NO_ERROR;
    d->window_end = 0;
    /* We only need to reset a few things for fast levels. */
    if (d->fast != NULL) {
        defl_fast_reset(d->fast);
        defl_tokens_reset(d->tokens);
        return;
    }
    if (d->lv.chain == 0)
        return;
    DeflAdvanced *s = d->state;
    s->chain_head = -1;
    memset(s->hash_head, 0, sizeof s->hash_head);
    memset(s->hash_prev, 0, sizeof s->hash_prev);
    s->hash_offset = 1;
    s->index = 0;
    d->block_start = 0;
    d->byte_available = false;
    defl_tokens_reset(d->tokens);
    s->length = DEFL_MIN_MATCH_LENGTH - 1;
    s->offset = 0;
    s->literal_counter = 0;
    s->max_insert_index = 0;
}

static Error defl_close(FlateWriter *d) {
    if (defl_same_error(d->err, defl_err_writer_closed))
        return BURROW_NO_ERROR;
    if (BURROW_FAILED(d->err))
        return d->err;
    d->sync = true;
    defl_step(d);
    if (BURROW_FAILED(d->err))
        return d->err;
    defl_bw_write_stored_header(&d->w, 0, true);
    if (BURROW_FAILED(d->w.err))
        return d->w.err;
    defl_bw_flush(&d->w);
    if (BURROW_FAILED(d->w.err))
        return d->w.err;
    d->err = defl_err_writer_closed;
    IoWriter none = {NULL, NULL};
    defl_bw_reset(&d->w, none);
    return BURROW_NO_ERROR;
}

/* ---------------------------------------------------------------- the API */

static const Type defl_writer_desc = {
    {(const Byte *)"Writer", 6},
    {(const Byte *)"compress/flate", 14},
    KIND_STRUCT,
    (uint32_t)sizeof(FlateWriter),
    (uint16_t)_Alignof(FlateWriter),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x666c7772U, /* "flwr" */
    NULL,
};

const Type *const TYPE_FLATE_WRITER = &defl_writer_desc;

static void defl_free_fast(Alloc *a, DeflFast *f) {
    if (f->hist != NULL)
        mem_free(a, f->hist, DEFL_ALLOC_HISTORY, 1);
    if (f->table != NULL)
        mem_free(a, f->table, (size_t)f->table_len * sizeof(int32_t),
                 _Alignof(int32_t));
    if (f->btable != NULL)
        mem_free(a, f->btable, (size_t)f->table_len * sizeof(int32_t),
                 _Alignof(int32_t));
    if (f->ptable != NULL)
        mem_free(a, f->ptable, (size_t)f->ptable_len * sizeof(DeflPrevEntry),
                 _Alignof(DeflPrevEntry));
}

static void defl_free(FlateWriter *d) {
    Alloc *a = d->a;
    if (d->fast != NULL)
        defl_free_fast(a, d->fast);
    if (d->state != NULL)
        mem_free(a, d->state, sizeof *d->state, _Alignof(DeflAdvanced));
    if (d->h != NULL)
        mem_free(a, d->h, sizeof *d->h, _Alignof(DeflHuffEnc));
    if (d->tokens != NULL)
        mem_free(a, d->tokens, sizeof *d->tokens, _Alignof(DeflTokens));
    if (d->window != NULL)
        mem_free(a, d->window, (size_t)d->window_len, 1);
    if (d->dict != NULL)
        mem_free(a, d->dict, (size_t)d->dict_len, 1);
    mem_free(a, d, sizeof *d, _Alignof(FlateWriter));
}

/* Go's error for a level out of range, built without fmt. */
static Error defl_level_error(Int level) {
    static const char head[] = "flate: invalid compression level ";
    static const char tail[] = ": want value in range [-2, 9]";
    Byte buf[sizeof head + sizeof tail + 24];
    Int n = 0;
    for (Int i = 0; i < (Int)sizeof head - 1; i++)
        buf[n++] = (Byte)head[i];
    Byte digits[20];
    Int nd = 0;
    uint64_t u = level < 0 ? 0U - (uint64_t)level : (uint64_t)level;
    do {
        digits[nd++] = (Byte)('0' + (int)(u % 10));
        u /= 10;
    } while (u != 0);
    if (level < 0)
        buf[n++] = '-';
    while (nd > 0)
        buf[n++] = digits[--nd];
    for (Int i = 0; i < (Int)sizeof tail - 1; i++)
        buf[n++] = (Byte)tail[i];
    return errors_new(error_allocator(), (Str){buf, n});
}

static int32_t *defl_alloc_table(Alloc *a, Int n) {
    return (int32_t *)mem_alloc(a, (size_t)n * sizeof(int32_t), _Alignof(int32_t));
}

/* Sets up the fast encoder of levels 1 to 6. False when a refuses. */
static bool defl_new_fast(FlateWriter *d, int level) {
    Alloc *a = d->a;
    DeflFast *f = &d->fast_store;
    d->fast = f;
    f->level = level;
    f->cur = DEFL_MAX_STORE_BLOCK_SIZE;
    f->hist = (Byte *)mem_alloc_nozero(a, DEFL_ALLOC_HISTORY, 1);
    if (f->hist == NULL)
        return false;
    switch (level) {
    case 1:
        f->table_len = DEFL_TABLE_SIZE;
        return (f->table = defl_alloc_table(a, f->table_len)) != NULL;
    case 2:
        f->table_len = DEFL_L2_TABLE_SIZE;
        return (f->table = defl_alloc_table(a, f->table_len)) != NULL;
    case 3:
        f->ptable_len = DEFL_L3_TABLE_SIZE;
        f->ptable = (DeflPrevEntry *)mem_alloc(
            a, (size_t)f->ptable_len * sizeof(DeflPrevEntry), _Alignof(DeflPrevEntry));
        return f->ptable != NULL;
    case 4:
        f->table_len = DEFL_TABLE_SIZE;
        f->table = defl_alloc_table(a, f->table_len);
        f->btable = defl_alloc_table(a, f->table_len);
        return f->table != NULL && f->btable != NULL;
    default:
        f->table_len = DEFL_TABLE_SIZE;
        f->ptable_len = DEFL_TABLE_SIZE;
        f->table = defl_alloc_table(a, f->table_len);
        f->ptable = (DeflPrevEntry *)mem_alloc(
            a, (size_t)f->ptable_len * sizeof(DeflPrevEntry), _Alignof(DeflPrevEntry));
        return f->table != NULL && f->ptable != NULL;
    }
}

/* Go's compressor.init, with everything the level needs allocated from a up
 * front. */
static Error defl_init(FlateWriter *d, IoWriter w, int level) {
    Alloc *a = d->a;
    defl_bw_init(&d->w, w);
    d->tokens = (DeflTokens *)mem_alloc(a, sizeof *d->tokens, _Alignof(DeflTokens));
    if (d->tokens == NULL)
        return burrow_err_out_of_memory;

    if (level == DEFL_NO_COMPRESSION) {
        d->window_len = DEFL_MAX_STORE_BLOCK_SIZE;
        d->step = DEFL_STEP_STORE;
    } else if (level == DEFL_HUFFMAN_ONLY) {
        d->w.log_new_table_penalty = 10;
        d->window_len = 32 << 10;
        d->step = DEFL_STEP_HUFF;
    } else if (level == DEFL_DEFAULT_COMPRESSION || (1 <= level && level <= 6)) {
        if (level == DEFL_DEFAULT_COMPRESSION)
            level = 6;
        d->w.log_new_table_penalty = 7;
        if (!defl_new_fast(d, level))
            return burrow_err_out_of_memory;
        d->window_len = DEFL_MAX_STORE_BLOCK_SIZE;
        d->step = DEFL_STEP_FAST;
    } else if (7 <= level && level <= 9) {
        d->w.log_new_table_penalty = 8;
        d->state =
            (DeflAdvanced *)mem_alloc(a, sizeof *d->state, _Alignof(DeflAdvanced));
        if (d->state == NULL)
            return burrow_err_out_of_memory;
        d->lv = defl_levels[level];
        if (d->lv.chain > 100) {
            d->h = (DeflHuffEnc *)mem_alloc(a, sizeof *d->h, _Alignof(DeflHuffEnc));
            if (d->h == NULL)
                return burrow_err_out_of_memory;
        }
        d->window_len = 2 * DEFL_WINDOW_SIZE;
        defl_init_deflate(d);
        d->fill_deflate = true;
        d->step = DEFL_STEP_LAZY;
    } else {
        return defl_level_error(level);
    }
    d->window = (Byte *)mem_alloc(a, (size_t)d->window_len, 1);
    if (d->window == NULL)
        return burrow_err_out_of_memory;
    d->level = level;
    return BURROW_NO_ERROR;
}

FlateWriter *flate_new_writer(Alloc *a, IoWriter w, Int level, Error *err) {
    if (level < DEFL_HUFFMAN_ONLY || level > 9) {
        *err = defl_level_error(level);
        return NULL;
    }
    defl_fixed();
    FlateWriter *d = (FlateWriter *)mem_alloc(a, sizeof *d, _Alignof(FlateWriter));
    if (d == NULL) {
        *err = burrow_err_out_of_memory;
        return NULL;
    }
    d->a = a;
    Error e = defl_init(d, w, (int)level);
    if (BURROW_FAILED(e)) {
        defl_free(d);
        *err = e;
        return NULL;
    }
    *err = BURROW_NO_ERROR;
    return d;
}

FlateWriter *flate_new_writer_dict(Alloc *a, IoWriter w, Int level, Slice dict,
                                   Error *err) {
    FlateWriter *d = flate_new_writer(a, w, level, err);
    if (d == NULL)
        return NULL;
    if (dict.len > 0) {
        d->dict = (Byte *)mem_alloc_nozero(a, (size_t)dict.len, 1);
        if (d->dict == NULL) {
            defl_free(d);
            *err = burrow_err_out_of_memory;
            return NULL;
        }
        memcpy(d->dict, dict.p, (size_t)dict.len);
        d->dict_len = dict.len;
    }
    defl_fill_window(d, d->dict, d->dict_len);
    return d;
}

Int flate_writer_write(FlateWriter *w, Slice data, Error *err) {
    return defl_write(w, (const Byte *)data.p, data.len, err);
}

Error flate_writer_flush(FlateWriter *w) {
    return defl_sync_flush(w);
}

Error flate_writer_close(FlateWriter *w) {
    return defl_close(w);
}

void flate_writer_reset(FlateWriter *w, IoWriter dst) {
    defl_reset(w, dst);
    defl_fill_window(w, w->dict, w->dict_len);
}

void flate_writer_free(FlateWriter *w) {
    if (w != NULL)
        defl_free(w);
}

static Int defl_vt_write(void *self, Slice p, Error *err) {
    return flate_writer_write((FlateWriter *)self, p, err);
}

static Error defl_vt_close(void *self) {
    return flate_writer_close((FlateWriter *)self);
}

static const IoWriteCloserVT defl_write_closer_vt = {
    {&defl_writer_desc, defl_vt_write},
    {&defl_writer_desc, defl_vt_close},
};

IoWriter flate_writer_as_io_writer(FlateWriter *w) {
    return (IoWriter){&defl_write_closer_vt.writer, w};
}

IoWriteCloser flate_writer_as_io_write_closer(FlateWriter *w) {
    return (IoWriteCloser){&defl_write_closer_vt, w};
}

/* ------------------------------------------------------------- test hooks */

Error burrow__flate_err_writer_closed(void) {
    return defl_err_writer_closed;
}

void burrow__flate_bulk_hash4(const Byte *b, Int n, uint32_t *dst) {
    defl_bulk_hash4(b, n, dst);
}

uint32_t burrow__flate_hash4(const Byte *b) {
    return defl_hash4(b);
}

uint16_t burrow__flate_reverse_bits(uint16_t x, uint8_t b) {
    return defl_reverse_bits(x, b);
}

struct BurrowFlateBitWriter {
    DeflBitWriter w;
    DeflTokens t;
};

BurrowFlateBitWriter *burrow__flate_bw_new(IoWriter w) {
    defl_fixed();
    BurrowFlateBitWriter *bw = (BurrowFlateBitWriter *)mem_alloc(
        heap_allocator(), sizeof *bw, _Alignof(BurrowFlateBitWriter));
    if (bw == NULL)
        return NULL;
    defl_bw_init(&bw->w, w);
    bw->w.log_new_table_penalty = 8;
    return bw;
}

void burrow__flate_bw_free(BurrowFlateBitWriter *bw) {
    if (bw != NULL)
        mem_free(heap_allocator(), bw, sizeof *bw, _Alignof(BurrowFlateBitWriter));
}

/* Go's indexTokens. */
static void defl_index_tokens(DeflTokens *t, const uint32_t *in, Int n) {
    t->n = 1;
    defl_tokens_reset(t);
    for (Int i = 0; i < n; i++) {
        uint32_t tok = in[i];
        if (tok < DEFL_MATCH_TYPE) {
            defl_tokens_add_literal(t, (Byte)tok);
            continue;
        }
        defl_tokens_add_match(t, (tok >> DEFL_LENGTH_SHIFT) & 0xff,
                              tok & DEFL_MATCH_OFFSET_ONLY_MASK);
    }
}

void burrow__flate_bw_write_block(BurrowFlateBitWriter *bw, const uint32_t *tokens,
                                  Int n, bool eof, const Byte *input, Int input_len) {
    defl_index_tokens(&bw->t, tokens, n);
    defl_bw_write_block(&bw->w, &bw->t, eof, input, input_len);
}

void burrow__flate_bw_write_block_dynamic(BurrowFlateBitWriter *bw,
                                          const uint32_t *tokens, Int n, bool eof,
                                          const Byte *input, Int input_len, bool sync) {
    defl_index_tokens(&bw->t, tokens, n);
    defl_bw_write_block_dynamic(&bw->w, &bw->t, eof, input, input_len, sync);
}

void burrow__flate_bw_write_block_huff(BurrowFlateBitWriter *bw, bool eof,
                                       const Byte *input, Int input_len, bool sync) {
    defl_bw_write_block_huff(&bw->w, eof, input, input_len, sync);
}

void burrow__flate_bw_flush(BurrowFlateBitWriter *bw) {
    defl_bw_flush(&bw->w);
}

Error burrow__flate_bw_err(BurrowFlateBitWriter *bw) {
    return bw->w.err;
}

struct BurrowFlateFast {
    FlateWriter d; /* for defl_new_fast and defl_free_fast */
    DeflTokens t;
};

BurrowFlateFast *burrow__flate_fast_new(int level) {
    Alloc *a = heap_allocator();
    BurrowFlateFast *f =
        (BurrowFlateFast *)mem_alloc(a, sizeof *f, _Alignof(BurrowFlateFast));
    if (f == NULL)
        return NULL;
    f->d.a = a;
    if (!defl_new_fast(&f->d, level)) {
        burrow__flate_fast_free(f);
        return NULL;
    }
    return f;
}

void burrow__flate_fast_free(BurrowFlateFast *f) {
    if (f == NULL)
        return;
    defl_free_fast(f->d.a, &f->d.fast_store);
    mem_free(f->d.a, f, sizeof *f, _Alignof(BurrowFlateFast));
}

Int burrow__flate_fast_encode(BurrowFlateFast *f, const Byte *src, Int n, uint32_t *out,
                              Int cap) {
    f->t.n = 1;
    defl_tokens_reset(&f->t);
    defl_fast_encode(&f->d.fast_store, &f->t, src, n);
    Int m = f->t.n;
    if (m > cap)
        m = cap;
    for (Int i = 0; i < m; i++)
        out[i] = f->t.tokens[i];
    return f->t.n;
}

int32_t burrow__flate_fast_add_block(BurrowFlateFast *f, const Byte *src, Int n) {
    return defl_fast_add_block(&f->d.fast_store, src, n);
}

int32_t burrow__flate_fast_match_len_limited(BurrowFlateFast *f, int32_t s, int32_t t) {
    DeflFast *e = &f->d.fast_store;
    return defl_match_len_limited(s, t, e->hist, e->hist_len);
}

int32_t burrow__flate_fast_cur(BurrowFlateFast *f) {
    return f->d.fast_store.cur;
}

void burrow__flate_fast_set_cur(BurrowFlateFast *f, int32_t cur) {
    f->d.fast_store.cur = cur;
}

Int burrow__flate_fast_hist_len(BurrowFlateFast *f) {
    return f->d.fast_store.hist_len;
}

void burrow__flate_fast_drop_hist(BurrowFlateFast *f) {
    f->d.fast_store.hist_len = 0;
}

int32_t burrow__flate_fast_buffer_reset(void) {
    return DEFL_BUFFER_RESET;
}

bool burrow__flate_writer_fast_reset(FlateWriter *w) {
    if (w->fast == NULL)
        return false;
    defl_fast_reset(w->fast);
    return true;
}

void burrow__flate_writer_reset_nil(FlateWriter *w) {
    IoWriter none = {NULL, NULL};
    defl_reset(w, none);
}

bool burrow__flate_writer_state_eq(FlateWriter *a, FlateWriter *b) {
    const DeflBitWriter *x = &a->w, *y = &b->w;
    if (x->bits != y->bits || x->nbits != y->nbits || x->nbytes != y->nbytes ||
        x->wrote_huffman != y->wrote_huffman || x->prev_header != y->prev_header ||
        x->log_new_table_penalty != y->log_new_table_penalty ||
        !defl_same_error(x->err, y->err))
        return false;
    if (a->level != b->level || a->sync != b->sync ||
        !defl_same_error(a->err, b->err) || a->window_end != b->window_end ||
        a->block_start != b->block_start || a->byte_available != b->byte_available ||
        a->tokens->n != b->tokens->n)
        return false;
    if ((a->state == NULL) != (b->state == NULL))
        return false;
    if (a->state != NULL) {
        const DeflAdvanced *s = a->state, *t = b->state;
        if (s->length != t->length || s->offset != t->offset || s->index != t->index ||
            s->hash_offset != t->hash_offset || s->chain_head != t->chain_head ||
            s->max_insert_index != t->max_insert_index ||
            s->literal_counter != t->literal_counter ||
            memcmp(s->hash_head, t->hash_head, sizeof s->hash_head) != 0 ||
            memcmp(s->hash_prev, t->hash_prev, sizeof s->hash_prev) != 0)
            return false;
    }
    return true;
}

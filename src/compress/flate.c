/* compress/flate: the decompressor, from inflate.go and dict_decoder.go.
 *
 * The shape is Go's. A decompressor is a small state machine that decodes
 * into a 32 KiB window, and Read hands out the part of the window that has
 * not been handed out yet. When the window fills, the step that filled it
 * records where it was and returns, and the next Read picks it up again.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/compress/flate.h"

#include "burrow/bufio.h"
#include "burrow/math/bits.h"
#include "burrow/mem/heap.h"
#include "burrow/sync.h"

#include "flate_internal.h"

#include <string.h>

enum {
    FLATE_MAX_CODE_LEN = 16, /* max length of Huffman code */
    FLATE_MAX_NUM_LIT = 286,
    FLATE_MAX_NUM_DIST = 30,
    FLATE_NUM_CODES = 19, /* number of codes in Huffman meta-code */
    FLATE_MAX_MATCH_OFFSET = 1 << 15,
    FLATE_END_BLOCK_MARKER = 256,
};

/* ------------------------------------------------------------------ errors */

/* The decimal digits of v at p, or only their count when p is NULL. */
static Int flate_put_int(Byte *p, int64_t v) {
    Byte digits[20];
    Int n = 0;
    uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    do {
        digits[n++] = (Byte)('0' + (int)(u % 10));
        u /= 10;
    } while (u != 0);
    Int len = (v < 0) + n;
    if (p != NULL) {
        if (v < 0)
            *p++ = '-';
        while (n > 0)
            *p++ = digits[--n];
    }
    return len;
}

static Byte *flate_put(Byte *p, const void *src, Int n) {
    if (n > 0)
        memcpy(p, src, (size_t)n);
    return p + n;
}

#define FLATE_LIT_LEN(s) ((Int)sizeof(s) - 1)

static bool flate_str_eq(Str a, Str b) {
    return a.len == b.len && (a.len == 0 || memcmp(a.p, b.p, (size_t)a.len) == 0);
}

/* CorruptInputError. The offset first, so errors_as hands back a pointer to
 * it. */
typedef struct FlateCorruptBox {
    FlateCorruptInputError off;
    Str message;
} FlateCorruptBox;

static const Type flate_corrupt_desc = {
    {(const Byte *)"CorruptInputError", 17},
    {(const Byte *)"compress/flate", 14},
    KIND_INT64,
    (uint32_t)sizeof(FlateCorruptInputError),
    (uint16_t)_Alignof(FlateCorruptInputError),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x666c6369U, /* "flci" */
    NULL,
};

const Type *const TYPE_FLATE_CORRUPT_INPUT_ERROR = &flate_corrupt_desc;

static const char flate_corrupt_prefix[] = "flate: corrupt input before offset ";

static Int flate_corrupt_message(Byte *p, FlateCorruptInputError e) {
    Int n = FLATE_LIT_LEN(flate_corrupt_prefix);
    if (p != NULL)
        p = flate_put(p, flate_corrupt_prefix, n);
    return n + flate_put_int(p, e);
}

static Str flate_box_message(const void *self) {
    return ((const FlateCorruptBox *)self)->message;
}

static bool flate_corrupt_is(const void *self, Error target);
static Error flate_corrupt_clone(const void *self, Alloc *a);

static const ErrorVT flate_corrupt_vt = {
    .self_type = &flate_corrupt_desc,
    .message = flate_box_message,
    .is = flate_corrupt_is,
    .clone = flate_corrupt_clone,
};

/* Go compares two of them with ==, which compares the offsets. */
static bool flate_corrupt_is(const void *self, Error target) {
    return target.vt == &flate_corrupt_vt && target.data != NULL &&
           ((const FlateCorruptBox *)target.data)->off ==
               ((const FlateCorruptBox *)self)->off;
}

Error flate_corrupt_input_error_as_error(FlateCorruptInputError e, Alloc *a) {
    Int mlen = flate_corrupt_message(NULL, e);
    FlateCorruptBox *b = (FlateCorruptBox *)mem_alloc_nozero(
        a, sizeof(FlateCorruptBox) + (size_t)mlen, _Alignof(FlateCorruptBox));
    if (b == NULL)
        return burrow_err_out_of_memory;
    Byte *p = (Byte *)(b + 1);
    flate_corrupt_message(p, e);
    b->off = e;
    b->message = str_from_bytes(p, mlen);
    return (Error){&flate_corrupt_vt, b};
}

static Error flate_corrupt_clone(const void *self, Alloc *a) {
    return flate_corrupt_input_error_as_error(((const FlateCorruptBox *)self)->off, a);
}

Str flate_corrupt_input_error_error(FlateCorruptInputError e, Alloc *a) {
    Int mlen = flate_corrupt_message(NULL, e);
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)mlen, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    flate_corrupt_message(p, e);
    return str_from_bytes(p, mlen);
}

/* InternalError, a string, boxed the same way. */
typedef struct FlateInternalBox {
    FlateInternalError s;
    Str message;
} FlateInternalBox;

static const Type flate_internal_desc = {
    {(const Byte *)"InternalError", 13},
    {(const Byte *)"compress/flate", 14},
    KIND_STRING,
    (uint32_t)sizeof(Str),
    (uint16_t)_Alignof(Str),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x666c6965U, /* "flie" */
    NULL,
};

const Type *const TYPE_FLATE_INTERNAL_ERROR = &flate_internal_desc;

static const char flate_internal_prefix[] = "flate: internal error: ";

static Str flate_internal_message(const void *self) {
    return ((const FlateInternalBox *)self)->message;
}

static bool flate_internal_is(const void *self, Error target);
static Error flate_internal_clone(const void *self, Alloc *a);

static const ErrorVT flate_internal_vt = {
    .self_type = &flate_internal_desc,
    .message = flate_internal_message,
    .is = flate_internal_is,
    .clone = flate_internal_clone,
};

static bool flate_internal_is(const void *self, Error target) {
    return target.vt == &flate_internal_vt && target.data != NULL &&
           flate_str_eq(((const FlateInternalBox *)target.data)->s,
                        ((const FlateInternalBox *)self)->s);
}

Error flate_internal_error_as_error(FlateInternalError e, Alloc *a) {
    Int plen = FLATE_LIT_LEN(flate_internal_prefix);
    Int mlen = plen + e.len;
    FlateInternalBox *b = (FlateInternalBox *)mem_alloc_nozero(
        a, sizeof(FlateInternalBox) + (size_t)mlen, _Alignof(FlateInternalBox));
    if (b == NULL)
        return burrow_err_out_of_memory;
    Byte *p = (Byte *)(b + 1);
    flate_put(flate_put(p, flate_internal_prefix, plen), e.p, e.len);
    b->message = str_from_bytes(p, mlen);
    b->s = str_from_bytes(p + plen, e.len);
    return (Error){&flate_internal_vt, b};
}

static Error flate_internal_clone(const void *self, Alloc *a) {
    return flate_internal_error_as_error(((const FlateInternalBox *)self)->s, a);
}

Str flate_internal_error_error(FlateInternalError e, Alloc *a) {
    Int plen = FLATE_LIT_LEN(flate_internal_prefix);
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)(plen + e.len), 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    flate_put(flate_put(p, flate_internal_prefix, plen), e.p, e.len);
    return str_from_bytes(p, plen + e.len);
}

/* ReadError and WriteError. They have the same layout and differ in the word
 * in the message, so one box and two vtables do both. */
typedef struct FlateIOBox {
    FlateReadError e;
    Str message;
} FlateIOBox;

static const Type flate_read_error_desc = {
    {(const Byte *)"ReadError", 9},
    {(const Byte *)"compress/flate", 14},
    KIND_STRUCT,
    (uint32_t)sizeof(FlateReadError),
    (uint16_t)_Alignof(FlateReadError),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x666c7265U, /* "flre" */
    NULL,
};

static const Type flate_write_error_desc = {
    {(const Byte *)"WriteError", 10},
    {(const Byte *)"compress/flate", 14},
    KIND_STRUCT,
    (uint32_t)sizeof(FlateWriteError),
    (uint16_t)_Alignof(FlateWriteError),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x666c7765U, /* "flwe" */
    NULL,
};

const Type *const TYPE_FLATE_READ_ERROR = &flate_read_error_desc;
const Type *const TYPE_FLATE_WRITE_ERROR = &flate_write_error_desc;

static const char flate_io_prefix_read[] = "flate: read error at offset ";
static const char flate_io_prefix_write[] = "flate: write error at offset ";

static Str flate_io_message(const void *self) {
    return ((const FlateIOBox *)self)->message;
}

static Error flate_read_error_clone(const void *self, Alloc *a);
static Error flate_write_error_clone(const void *self, Alloc *a);

static const ErrorVT flate_read_error_vt = {
    .self_type = &flate_read_error_desc,
    .message = flate_io_message,
    .clone = flate_read_error_clone,
};

static const ErrorVT flate_write_error_vt = {
    .self_type = &flate_write_error_desc,
    .message = flate_io_message,
    .clone = flate_write_error_clone,
};

/* prefix + Offset + ": " + Err.Error(), written to p or only counted. */
static Int flate_io_text(Byte *p, bool write, int64_t off, Error err) {
    const char *prefix = write ? flate_io_prefix_write : flate_io_prefix_read;
    Int plen = write ? FLATE_LIT_LEN(flate_io_prefix_write)
                     : FLATE_LIT_LEN(flate_io_prefix_read);
    Str text = error_text(err);
    Int n = plen + flate_put_int(NULL, off) + 2 + text.len;
    if (p != NULL) {
        p = flate_put(p, prefix, plen);
        p += flate_put_int(p, off);
        *p++ = ':';
        *p++ = ' ';
        flate_put(p, text.p, text.len);
    }
    return n;
}

static Error flate_io_build(Alloc *a, bool write, int64_t off, Error err) {
    Int mlen = flate_io_text(NULL, write, off, err);
    FlateIOBox *b = (FlateIOBox *)mem_alloc_nozero(a, sizeof(FlateIOBox) + (size_t)mlen,
                                                   _Alignof(FlateIOBox));
    if (b == NULL)
        return burrow_err_out_of_memory;
    Byte *p = (Byte *)(b + 1);
    flate_io_text(p, write, off, err);
    b->e.offset = off;
    b->e.err = err;
    b->message = str_from_bytes(p, mlen);
    return (Error){write ? &flate_write_error_vt : &flate_read_error_vt, b};
}

static Str flate_io_string(Alloc *a, bool write, int64_t off, Error err) {
    Int mlen = flate_io_text(NULL, write, off, err);
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)mlen, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    flate_io_text(p, write, off, err);
    return str_from_bytes(p, mlen);
}

static Error flate_read_error_clone(const void *self, Alloc *a) {
    const FlateReadError *e = (const FlateReadError *)self;
    return flate_io_build(a, false, e->offset, error_retain(a, e->err));
}

static Error flate_write_error_clone(const void *self, Alloc *a) {
    const FlateReadError *e = (const FlateReadError *)self;
    return flate_io_build(a, true, e->offset, error_retain(a, e->err));
}

Str flate_read_error_error(const FlateReadError *e, Alloc *a) {
    return flate_io_string(a, false, e->offset, e->err);
}

Error flate_read_error_as_error(const FlateReadError *e, Alloc *a) {
    return flate_io_build(a, false, e->offset, e->err);
}

Str flate_write_error_error(const FlateWriteError *e, Alloc *a) {
    return flate_io_string(a, true, e->offset, e->err);
}

Error flate_write_error_as_error(const FlateWriteError *e, Alloc *a) {
    return flate_io_build(a, true, e->offset, e->err);
}

/* ---------------------------------------------------------- Huffman decoder
 *
 * The data structure for decoding Huffman tables is based on that of zlib.
 * There is a lookup table of a fixed bit width (HUFF_CHUNK_BITS). For codes
 * smaller than the table width, there are multiple entries (each combination
 * of trailing bits has the same value). For codes larger than the table width,
 * the table contains a link to an overflow table. The width of each entry in
 * the link table is the maximum code size minus the chunk width.
 *
 * Note that you can do a lookup in the table even without all bits filled.
 * Since the extra bits are zero, and the DEFLATE Huffman codes have the
 * property that shorter codes come before longer ones, the bit length estimate
 * in the result is a lower bound on the actual number of bits.
 *
 * See https://github.com/madler/zlib/raw/master/doc/algorithm.txt
 *
 * chunk & 15 is the number of bits and chunk >> 4 is the value, including a
 * table link. Go keeps the overflow tables as a slice of slices, and here they
 * are one array of link_mask + 1 entries per table, which is always a power of
 * two, so table i starts at i << link_shift. */
enum {
    HUFF_CHUNK_BITS = 9,
    HUFF_NUM_CHUNKS = 1 << HUFF_CHUNK_BITS,
    HUFF_COUNT_MASK = 15,
    HUFF_VALUE_SHIFT = 4,
};

typedef struct FlateHuff {
    int min;             /* the minimum code length */
    uint32_t link_mask;  /* mask the width of the link table */
    unsigned link_shift; /* log2 of the width of a link table */
    uint32_t *links;     /* overflow links, from the decompressor's allocator */
    size_t links_cap;    /* how many entries links has room for */
    uint32_t chunks[HUFF_NUM_CHUNKS];
} FlateHuff;

enum { HUFF_OK, HUFF_BAD, HUFF_NO_MEMORY };

/* Initialize Huffman decoding tables from array of code lengths. Following
 * this function, h is guaranteed to be initialized into a complete tree (i.e.,
 * neither over-subscribed nor under-subscribed). The exception is a degenerate
 * case where the tree has only a single symbol with length 1. Empty trees are
 * permitted.
 *
 * Go makes the overflow tables fresh each time. Here the array is kept and
 * grown from a when a tree needs more of it, and a NULL a means the tree must
 * not need any, which is true of the fixed one. */
static int flate_huff_init(FlateHuff *h, Alloc *a, const int *lengths, int nlengths) {
    if (h->min != 0) {
        h->min = 0;
        h->link_mask = 0;
        h->link_shift = 0;
        memset(h->chunks, 0, sizeof h->chunks);
    }

    /* Count number of codes of each length, compute min and max length. */
    int count[FLATE_MAX_CODE_LEN] = {0};
    int min = 0, max = 0;
    for (int i = 0; i < nlengths; i++) {
        int n = lengths[i];
        if (n == 0)
            continue;
        if (min == 0 || n < min)
            min = n;
        if (n > max)
            max = n;
        count[n]++;
    }

    /* Empty tree. The decompressor.huffSym function will fail later if the
     * tree is used. Technically, an empty tree is only valid for the HDIST
     * tree and not the HCLEN and HLIT tree. However, a stream with an empty
     * HCLEN tree is guaranteed to fail since it will attempt to use the tree
     * to decode the codes for the HLIT and HDIST trees. Similarly, an empty
     * HLIT tree is guaranteed to fail later since the compressed data section
     * must be composed of at least one symbol (the end-of-block marker). */
    if (max == 0)
        return HUFF_OK;

    int code = 0;
    int nextcode[FLATE_MAX_CODE_LEN] = {0};
    for (int i = min; i <= max; i++) {
        code <<= 1;
        nextcode[i] = code;
        code += count[i];
    }

    /* Check that the coding is complete (i.e., that we've assigned all 2-to-
     * the-max possible bit sequences). Exception: To be compatible with zlib,
     * we also need to accept degenerate single-code codings. See also
     * TestDegenerateHuffmanCoding. */
    if (code != 1 << max && !(code == 1 && max == 1))
        return HUFF_BAD;

    h->min = min;
    if (max > HUFF_CHUNK_BITS) {
        unsigned shift = (unsigned)max - HUFF_CHUNK_BITS;
        size_t num_links = (size_t)1 << shift;
        int link = nextcode[HUFF_CHUNK_BITS + 1] >> 1;
        size_t need = (size_t)(HUFF_NUM_CHUNKS - link) << shift;
        if (h->links == NULL || h->links_cap < need) {
            uint32_t *p = a == NULL
                              ? NULL
                              : (uint32_t *)mem_alloc_nozero(a, need * sizeof(uint32_t),
                                                             _Alignof(uint32_t));
            if (p == NULL) {
                h->min = 0;
                return HUFF_NO_MEMORY;
            }
            if (h->links != NULL)
                mem_free(a, h->links, h->links_cap * sizeof(uint32_t),
                         _Alignof(uint32_t));
            h->links = p;
            h->links_cap = need;
        }
        memset(h->links, 0, need * sizeof(uint32_t));
        h->link_mask = (uint32_t)(num_links - 1);
        h->link_shift = shift;
        for (unsigned j = (unsigned)link; j < HUFF_NUM_CHUNKS; j++) {
            int reverse = (int)bits_reverse16((uint16_t)j);
            reverse >>= 16 - HUFF_CHUNK_BITS;
            unsigned off = j - (unsigned)link;
            h->chunks[reverse] =
                (uint32_t)(off << HUFF_VALUE_SHIFT | (HUFF_CHUNK_BITS + 1));
        }
    }

    for (int i = 0; i < nlengths; i++) {
        int n = lengths[i];
        if (n <= 0)
            continue;
        int c = nextcode[n];
        nextcode[n]++;
        uint32_t chunk = (uint32_t)i << HUFF_VALUE_SHIFT | (uint32_t)n;
        int reverse = (int)bits_reverse16((uint16_t)c);
        reverse >>= 16 - n;
        if (n <= HUFF_CHUNK_BITS) {
            for (int off = reverse; off < HUFF_NUM_CHUNKS; off += 1 << n)
                h->chunks[off] = chunk;
        } else {
            int j = reverse & (HUFF_NUM_CHUNKS - 1);
            uint32_t value = h->chunks[j] >> HUFF_VALUE_SHIFT;
            uint32_t *linktab = h->links + ((size_t)value << h->link_shift);
            int width = (int)h->link_mask + 1;
            reverse >>= HUFF_CHUNK_BITS;
            /* n is past HUFF_CHUNK_BITS on this side of the if. */
            /* links was sized for every link chunk before this loop. */
            /* NOLINTBEGIN(clang-analyzer-core.BitwiseShift,clang-analyzer-core.NullDereference) */
            for (int off = reverse; off < width; off += 1 << (n - HUFF_CHUNK_BITS))
                linktab[off] = chunk;
            /* NOLINTEND(clang-analyzer-core.BitwiseShift,clang-analyzer-core.NullDereference) */
        }
    }
    return HUFF_OK;
}

static void flate_huff_clear(FlateHuff *h) {
    h->min = 0;
    h->link_mask = 0;
    h->link_shift = 0;
    memset(h->chunks, 0, sizeof h->chunks);
}

/* Built once on first use, and only read after that. */
static FlateHuff flate_fixed;
static SyncOnce flate_fixed_once;

static void flate_fixed_init(void *env) {
    (void)env;
    int bits[288];
    for (int i = 0; i < 144; i++)
        bits[i] = 8;
    for (int i = 144; i < 256; i++)
        bits[i] = 9;
    for (int i = 256; i < 280; i++)
        bits[i] = 7;
    for (int i = 280; i < 288; i++)
        bits[i] = 8;
    flate_huff_init(&flate_fixed, NULL, bits, 288);
}

/* ---------------------------------------------------------- decompressor */

enum { STEP_NEXT_BLOCK, STEP_HUFFMAN_BLOCK, STEP_COPY_DATA };

typedef struct FlateDecompressor {
    Alloc *a;

    /* Input source. r is what data blocks read with io_read_full. Bytes come
     * from direct when it is a BufioReader, which skips the method lookup,
     * and from read_byte, the ReadByte in r's method set, otherwise. */
    IoReader r;
    BufioReader *direct;
    const Method *read_byte;
    BufioReader *rbuf; /* created if r has no ReadByte, and kept for reuse */
    int64_t roffset;

    /* Input bits, in top of b. */
    uint32_t b;
    unsigned nb;

    /* Huffman decoders for literal/length, distance. */
    FlateHuff h1, h2;

    /* Length arrays used to define Huffman codes. */
    int bits[FLATE_MAX_NUM_LIT + FLATE_MAX_NUM_DIST];
    int codebits[FLATE_NUM_CODES];

    /* Output history, buffer. */
    FlateDict dict;

    /* Temporary buffer (avoids repeated allocation). */
    Byte buf[4];

    /* Next step in the decompression, and decompression state. */
    int step;
    int step_state;
    bool final;
    Error err;
    Slice to_read;
    const FlateHuff *hlit, *hdist;
    Int copy_len;
    Int copy_dist;
} FlateDecompressor;

static const Type flate_decompressor_desc = {
    {(const Byte *)"decompressor", 12},
    {(const Byte *)"compress/flate", 14},
    KIND_STRUCT,
    (uint32_t)sizeof(FlateDecompressor),
    (uint16_t)_Alignof(FlateDecompressor),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x666c6463U, /* "fldc" */
    NULL,
};

static inline bool flate_same_error(Error a, Error b) {
    return a.vt == b.vt && a.data == b.data;
}

/* err, unless err is io_eof, in which case it is io_err_unexpected_eof. */
static inline Error flate_no_eof(Error e) {
    return flate_same_error(e, io_eof) ? io_err_unexpected_eof : e;
}

static Error flate_corrupt(const FlateDecompressor *f) {
    return flate_corrupt_input_error_as_error(f->roffset, error_allocator());
}

/* The next input byte into *c, or false and the error with io_eof made
 * io_err_unexpected_eof. */
static inline bool flate_read_byte(FlateDecompressor *f, Byte *c, Error *err) {
    Error e = BURROW_NO_ERROR;
    if (f->direct != NULL) {
        *c = bufio_reader_read_byte(f->direct, &e);
    } else {
        IoErrorArg ea = &e;
        void *args[1] = {(void *)&ea};
        void *rets[1] = {c};
        f->read_byte->thunk(f->r.data, args, rets);
    }
    if (BURROW_UNLIKELY(BURROW_FAILED(e))) {
        *err = flate_no_eof(e);
        return false;
    }
    return true;
}

static bool flate_more_bits(FlateDecompressor *f, Error *err) {
    Byte c;
    if (!flate_read_byte(f, &c, err))
        return false;
    f->roffset++;
    f->b |= (uint32_t)c << f->nb;
    f->nb += 8;
    return true;
}

/* Read the next Huffman-encoded symbol from f according to h. The symbol, or
 * -1 and the error. */
static inline int flate_huff_sym(FlateDecompressor *f, const FlateHuff *h, Error *err) {
    /* Since a huffmanDecoder can be empty or be composed of a degenerate tree
     * with single element, huff_sym must error on these two edge cases. In
     * both cases, the chunks slice will be 0 for the invalid sequence,
     * leading it satisfy the n == 0 check below. */
    unsigned n = (unsigned)h->min;
    /* Optimization. Compiler isn't smart enough to keep f->b, f->nb in
     * registers, but is smart enough to keep local variables in registers, so
     * use nb and b, inline call to more_bits and reassign b, nb back to f on
     * return. */
    unsigned nb = f->nb;
    uint32_t b = f->b;
    for (;;) {
        while (nb < n) {
            Byte c;
            if (!flate_read_byte(f, &c, err)) {
                f->b = b;
                f->nb = nb;
                return -1;
            }
            f->roffset++;
            b |= (uint32_t)c << (nb & 31);
            nb += 8;
        }
        uint32_t chunk = h->chunks[b & (HUFF_NUM_CHUNKS - 1)];
        n = chunk & HUFF_COUNT_MASK;
        if (n > HUFF_CHUNK_BITS) {
            chunk = h->links[(size_t)(chunk >> HUFF_VALUE_SHIFT) << h->link_shift |
                             ((b >> HUFF_CHUNK_BITS) & h->link_mask)];
            n = chunk & HUFF_COUNT_MASK;
        }
        if (n <= nb) {
            if (n == 0) {
                f->b = b;
                f->nb = nb;
                f->err = flate_corrupt(f);
                *err = f->err;
                return -1;
            }
            f->b = b >> (n & 31);
            f->nb = nb - n;
            return (int)(chunk >> HUFF_VALUE_SHIFT);
        }
    }
}

static const int flate_code_order[FLATE_NUM_CODES] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

static Error flate_huff_status(const FlateDecompressor *f, int status) {
    return status == HUFF_NO_MEMORY ? burrow_err_out_of_memory : flate_corrupt(f);
}

/* RFC 1951 section 3.2.7. Compression with dynamic Huffman codes. */
static Error flate_read_huffman(FlateDecompressor *f) {
    Error err = BURROW_NO_ERROR;
    /* HLIT[5], HDIST[5], HCLEN[4]. */
    while (f->nb < 5 + 5 + 4) {
        if (!flate_more_bits(f, &err))
            return err;
    }
    int nlit = (int)(f->b & 0x1F) + 257;
    if (nlit > FLATE_MAX_NUM_LIT)
        return flate_corrupt(f);
    f->b >>= 5;
    int ndist = (int)(f->b & 0x1F) + 1;
    if (ndist > FLATE_MAX_NUM_DIST)
        return flate_corrupt(f);
    f->b >>= 5;
    int nclen = (int)(f->b & 0xF) + 4;
    /* nclen is at most 19, which is the number of code order entries. */
    f->b >>= 4;
    f->nb -= 5 + 5 + 4;

    /* (HCLEN+4)*3 bits: code lengths in the magic code_order order. */
    for (int i = 0; i < nclen; i++) {
        while (f->nb < 3) {
            if (!flate_more_bits(f, &err))
                return err;
        }
        f->codebits[flate_code_order[i]] = (int)(f->b & 0x7);
        f->b >>= 3;
        f->nb -= 3;
    }
    for (int i = nclen; i < FLATE_NUM_CODES; i++)
        f->codebits[flate_code_order[i]] = 0;
    int st = flate_huff_init(&f->h1, f->a, f->codebits, FLATE_NUM_CODES);
    if (st != HUFF_OK)
        return flate_huff_status(f, st);

    /* HLIT + 257 code lengths, HDIST + 1 code lengths, using the code length
     * Huffman code. */
    for (int i = 0, n = nlit + ndist; i < n;) {
        int x = flate_huff_sym(f, &f->h1, &err);
        if (x < 0)
            return err;
        if (x < 16) {
            /* Actual length. */
            f->bits[i] = x;
            i++;
            continue;
        }
        /* Repeat previous length or zero. */
        int rep;
        unsigned nb;
        int b;
        switch (x) {
        case 16:
            rep = 3;
            nb = 2;
            if (i == 0)
                return flate_corrupt(f);
            b = f->bits[i - 1];
            break;
        case 17:
            rep = 3;
            nb = 3;
            b = 0;
            break;
        case 18:
            rep = 11;
            nb = 7;
            b = 0;
            break;
        default:
            return flate_internal_error_as_error(BURROW_S("unexpected length code"),
                                                 error_allocator());
        }
        while (f->nb < nb) {
            if (!flate_more_bits(f, &err))
                return err;
        }
        rep += (int)(f->b & (((uint32_t)1 << nb) - 1));
        f->b >>= nb;
        f->nb -= nb;
        if (i + rep > n)
            return flate_corrupt(f);
        for (int j = 0; j < rep; j++) {
            f->bits[i] = b;
            i++;
        }
    }

    st = flate_huff_init(&f->h1, f->a, f->bits, nlit);
    if (st == HUFF_OK)
        st = flate_huff_init(&f->h2, f->a, f->bits + nlit, ndist);
    if (st != HUFF_OK)
        return flate_huff_status(f, st);

    /* As an optimization, we can initialize the min bits to read at a time
     * for the HLIT tree to the length of the EOB marker since we know that
     * every block must terminate with one. This preserves the property that
     * we never read any extra bytes after the end of the DEFLATE stream. */
    if (f->h1.min < f->bits[FLATE_END_BLOCK_MARKER])
        f->h1.min = f->bits[FLATE_END_BLOCK_MARKER];

    return BURROW_NO_ERROR;
}

static void flate_finish_block(FlateDecompressor *f) {
    if (f->final) {
        if (flate_dict_avail_read(&f->dict) > 0)
            f->to_read = flate_dict_read_flush(&f->dict);
        f->err = io_eof;
    }
    f->step = STEP_NEXT_BLOCK;
}

/* Decode a single Huffman block from f. hlit and hdist are the Huffman states for
 * the lit/length values and the distance values, respectively. If hdist is NULL,
 * using the fixed distance encoding associated with fixed Huffman blocks. */
static void flate_huffman_block(FlateDecompressor *f) {
    enum { STATE_INIT, STATE_DICT };
    Error err = BURROW_NO_ERROR;

    if (f->step_state == STATE_DICT)
        goto copy_history;

read_literal:
    /* Read literal and/or (length, distance) according to RFC section
     * 3.2.3. */
    {
        int v = flate_huff_sym(f, f->hlit, &err);
        if (v < 0) {
            f->err = err;
            return;
        }
        unsigned n; /* number of bits extra */
        Int length;
        if (v < 256) {
            flate_dict_write_byte(&f->dict, (Byte)v);
            if (flate_dict_avail_write(&f->dict) == 0) {
                f->to_read = flate_dict_read_flush(&f->dict);
                f->step = STEP_HUFFMAN_BLOCK;
                f->step_state = STATE_INIT;
                return;
            }
            goto read_literal;
        } else if (v == 256) {
            flate_finish_block(f);
            return;
        } else if (v < 265) {
            /* otherwise, reference to older data */
            length = v - (257 - 3);
            n = 0;
        } else if (v < 269) {
            length = v * 2 - (265 * 2 - 11);
            n = 1;
        } else if (v < 273) {
            length = v * 4 - (269 * 4 - 19);
            n = 2;
        } else if (v < 277) {
            length = v * 8 - (273 * 8 - 35);
            n = 3;
        } else if (v < 281) {
            length = v * 16 - (277 * 16 - 67);
            n = 4;
        } else if (v < 285) {
            length = v * 32 - (281 * 32 - 131);
            n = 5;
        } else if (v < FLATE_MAX_NUM_LIT) {
            length = 258;
            n = 0;
        } else {
            f->err = flate_corrupt(f);
            return;
        }
        if (n > 0) {
            while (f->nb < n) {
                if (!flate_more_bits(f, &err)) {
                    f->err = err;
                    return;
                }
            }
            length += (Int)(f->b & (((uint32_t)1 << n) - 1));
            f->b >>= n;
            f->nb -= n;
        }

        int dist;
        if (f->hdist == NULL) {
            while (f->nb < 5) {
                if (!flate_more_bits(f, &err)) {
                    f->err = err;
                    return;
                }
            }
            dist = (int)bits_reverse8((uint8_t)((f->b & 0x1F) << 3));
            f->b >>= 5;
            f->nb -= 5;
        } else {
            dist = flate_huff_sym(f, f->hdist, &err);
            if (dist < 0) {
                f->err = err;
                return;
            }
        }

        if (dist < 4) {
            dist++;
        } else if (dist < FLATE_MAX_NUM_DIST) {
            unsigned nb = (unsigned)(dist - 2) >> 1;
            /* have 1 bit in bottom of dist, need nb more. */
            int extra = (dist & 1) << nb;
            while (f->nb < nb) {
                if (!flate_more_bits(f, &err)) {
                    f->err = err;
                    return;
                }
            }
            extra |= (int)(f->b & (((uint32_t)1 << nb) - 1));
            f->b >>= nb;
            f->nb -= nb;
            dist = (1 << (nb + 1)) + 1 + extra;
        } else {
            f->err = flate_corrupt(f);
            return;
        }

        /* No check on length; encoding can be prescient. */
        if (dist > flate_dict_hist_size(&f->dict)) {
            f->err = flate_corrupt(f);
            return;
        }

        f->copy_len = length;
        f->copy_dist = dist;
        goto copy_history;
    }

copy_history:
    /* Perform a backwards copy according to RFC section 3.2.3. */
    {
        Int cnt = flate_dict_try_write_copy(&f->dict, f->copy_dist, f->copy_len);
        if (cnt == 0)
            cnt = flate_dict_write_copy(&f->dict, f->copy_dist, f->copy_len);
        f->copy_len -= cnt;

        if (flate_dict_avail_write(&f->dict) == 0 || f->copy_len > 0) {
            f->to_read = flate_dict_read_flush(&f->dict);
            f->step = STEP_HUFFMAN_BLOCK; /* We need to continue this work */
            f->step_state = STATE_DICT;
            return;
        }
        goto read_literal;
    }
}

/* copy_data copies f->copy_len bytes from the underlying reader into the
 * history. It pauses for reads when the history is full. */
static void flate_copy_data(FlateDecompressor *f) {
    Int avail = flate_dict_avail_write(&f->dict);
    Int want = avail < f->copy_len ? avail : f->copy_len;
    Slice buf = slice_from(f->dict.hist + f->dict.wr_pos, want, want, TYPE_BYTE);

    Error err = BURROW_NO_ERROR;
    Int cnt = io_read_full(f->r, buf, &err);
    f->roffset += cnt;
    f->copy_len -= cnt;
    f->dict.wr_pos += cnt;
    if (BURROW_FAILED(err)) {
        f->err = flate_no_eof(err);
        return;
    }

    if (flate_dict_avail_write(&f->dict) == 0 || f->copy_len > 0) {
        f->to_read = flate_dict_read_flush(&f->dict);
        f->step = STEP_COPY_DATA;
        return;
    }
    flate_finish_block(f);
}

/* Copy a single uncompressed data block from input to output. */
static void flate_data_block(FlateDecompressor *f) {
    /* Uncompressed. Discard current half-byte. */
    f->nb = 0;
    f->b = 0;

    /* Length then ones-complement of length. */
    Slice head = slice_from(f->buf, 4, 4, TYPE_BYTE);
    Error err = BURROW_NO_ERROR;
    Int nr = io_read_full(f->r, head, &err);
    f->roffset += nr;
    if (BURROW_FAILED(err)) {
        f->err = flate_no_eof(err);
        return;
    }
    int n = (int)f->buf[0] | (int)f->buf[1] << 8;
    int nn = (int)f->buf[2] | (int)f->buf[3] << 8;
    if ((uint16_t)nn != (uint16_t)~n) {
        f->err = flate_corrupt(f);
        return;
    }

    if (n == 0) {
        f->to_read = flate_dict_read_flush(&f->dict);
        flate_finish_block(f);
        return;
    }

    f->copy_len = n;
    flate_copy_data(f);
}

static void flate_next_block(FlateDecompressor *f) {
    Error err = BURROW_NO_ERROR;
    while (f->nb < 1 + 2) {
        if (!flate_more_bits(f, &err)) {
            f->err = err;
            return;
        }
    }
    f->final = (f->b & 1) == 1;
    f->b >>= 1;
    uint32_t typ = f->b & 3;
    f->b >>= 2;
    f->nb -= 1 + 2;
    switch (typ) {
    case 0:
        flate_data_block(f);
        break;
    case 1:
        /* compressed, fixed Huffman tables */
        f->hlit = &flate_fixed;
        f->hdist = NULL;
        flate_huffman_block(f);
        break;
    case 2:
        /* compressed, dynamic Huffman tables */
        f->err = flate_read_huffman(f);
        if (BURROW_FAILED(f->err))
            break;
        f->hlit = &f->h1;
        f->hdist = &f->h2;
        flate_huffman_block(f);
        break;
    default:
        /* 3 is reserved. */
        f->err = flate_corrupt(f);
        break;
    }
}

static void flate_step(FlateDecompressor *f) {
    switch (f->step) {
    case STEP_NEXT_BLOCK:
        /* A new block starts its Huffman state from the top. */
        f->step_state = 0;
        flate_next_block(f);
        break;
    case STEP_HUFFMAN_BLOCK:
        flate_huffman_block(f);
        break;
    default:
        flate_copy_data(f);
        break;
    }
}

static Int flate_read(void *self, Slice b, Error *err) {
    FlateDecompressor *f = (FlateDecompressor *)self;
    for (;;) {
        if (f->to_read.len > 0) {
            Int n = b.len < f->to_read.len ? b.len : f->to_read.len;
            if (n > 0)
                memcpy(b.p, f->to_read.p, (size_t)n);
            f->to_read.p = (Byte *)f->to_read.p + n;
            f->to_read.len -= n;
            f->to_read.cap -= n;
            BURROW_OUT(err, f->to_read.len == 0 ? f->err : BURROW_NO_ERROR);
            return n;
        }
        if (BURROW_FAILED(f->err)) {
            BURROW_OUT(err, f->err);
            return 0;
        }
        flate_step(f);
        if (BURROW_FAILED(f->err) && f->to_read.len == 0)
            f->to_read = flate_dict_read_flush(&f->dict); /* Flush what's left in
                                                            case of error */
    }
}

static Error flate_close(void *self) {
    FlateDecompressor *f = (FlateDecompressor *)self;
    if (flate_same_error(f->err, io_eof))
        return BURROW_NO_ERROR;
    return f->err;
}

/* Points f at r: straight at it when it has ReadByte, and through a
 * BufioReader of f's own when it does not. False when that cannot be
 * allocated. */
static bool flate_make_reader(FlateDecompressor *f, IoReader r) {
    f->direct = NULL;
    f->read_byte = NULL;
    if (r.vt != NULL && r.vt->self_type == TYPE_BUFIO_READER) {
        f->r = r;
        f->direct = (BufioReader *)r.data;
        return true;
    }
    const Method *m = burrow__io_read_byte_method(r);
    if (m != NULL) {
        f->r = r;
        f->read_byte = m;
        return true;
    }
    if (f->rbuf != NULL) {
        bufio_reader_reset(f->rbuf, r);
    } else {
        f->rbuf = bufio_new_reader(f->a, r);
        if (f->rbuf == NULL)
            return false;
    }
    f->r = bufio_reader_as_io_reader(f->rbuf);
    f->direct = f->rbuf;
    return true;
}

/* Everything Go's Reset zeroes, which is all of it but the buffers. */
static void flate_clear_state(FlateDecompressor *f) {
    f->roffset = 0;
    f->b = 0;
    f->nb = 0;
    flate_huff_clear(&f->h1);
    flate_huff_clear(&f->h2);
    f->step = STEP_NEXT_BLOCK;
    f->step_state = 0;
    f->final = false;
    f->err = BURROW_NO_ERROR;
    f->to_read = (Slice){NULL, 0, 0, NULL};
    f->hlit = NULL;
    f->hdist = NULL;
    f->copy_len = 0;
    f->copy_dist = 0;
}

static Error flate_reset(void *self, IoReader r, Slice dict) {
    FlateDecompressor *f = (FlateDecompressor *)self;
    flate_clear_state(f);
    if (!flate_make_reader(f, r)) {
        f->err = burrow_err_out_of_memory;
        return burrow_err_out_of_memory;
    }
    flate_dict_init(&f->dict, dict);
    return BURROW_NO_ERROR;
}

static const IoReadCloserVT flate_read_closer_vt = {
    {&flate_decompressor_desc, flate_read},
    {&flate_decompressor_desc, flate_close},
};

static const FlateResetterVT flate_resetter_vt = {&flate_decompressor_desc,
                                                  flate_reset};

static void flate_free(FlateDecompressor *f) {
    Alloc *a = f->a;
    if (f->h1.links != NULL)
        mem_free(a, f->h1.links, f->h1.links_cap * sizeof(uint32_t),
                 _Alignof(uint32_t));
    if (f->h2.links != NULL)
        mem_free(a, f->h2.links, f->h2.links_cap * sizeof(uint32_t),
                 _Alignof(uint32_t));
    if (f->dict.hist != NULL)
        mem_free(a, f->dict.hist, (size_t)f->dict.size, 1);
    bufio_reader_free(f->rbuf);
    mem_free(a, f, sizeof *f, _Alignof(FlateDecompressor));
}

IoReadCloser flate_new_reader_dict(Alloc *a, IoReader r, Slice dict) {
    sync_once_do(&flate_fixed_once, BURROW_FN(Func, flate_fixed_init, NULL));

    IoReadCloser nil = {NULL, NULL};
    FlateDecompressor *f =
        (FlateDecompressor *)mem_alloc(a, sizeof *f, _Alignof(FlateDecompressor));
    if (f == NULL)
        return nil;
    f->a = a;
    f->dict.size = FLATE_MAX_MATCH_OFFSET;
    f->dict.hist = (Byte *)mem_alloc_nozero(a, (size_t)f->dict.size, 1);
    if (f->dict.hist == NULL || !flate_make_reader(f, r)) {
        flate_free(f);
        return nil;
    }
    flate_clear_state(f);
    flate_dict_init(&f->dict, dict);
    return (IoReadCloser){&flate_read_closer_vt, f};
}

IoReadCloser flate_new_reader(Alloc *a, IoReader r) {
    Slice none = {NULL, 0, 0, NULL};
    return flate_new_reader_dict(a, r, none);
}

static bool flate_is_ours(IoReadCloser rc) {
    return rc.vt != NULL && rc.data != NULL &&
           rc.vt->reader.self_type == &flate_decompressor_desc;
}

FlateResetter flate_reader_as_resetter(IoReadCloser rc) {
    FlateResetter rs = {NULL, NULL};
    if (flate_is_ours(rc)) {
        rs.vt = &flate_resetter_vt;
        rs.data = rc.data;
    }
    return rs;
}

void flate_reader_free(IoReadCloser rc) {
    if (flate_is_ours(rc))
        flate_free((FlateDecompressor *)rc.data);
}

/* ------------------------------------------------------------ test hooks */

bool burrow__flate_huff_init_ok(const int *lengths, int n) {
    FlateHuff h;
    memset(&h, 0, sizeof h);
    int st = flate_huff_init(&h, heap_allocator(), lengths, n);
    if (h.links != NULL)
        mem_free(heap_allocator(), h.links, h.links_cap * sizeof(uint32_t),
                 _Alignof(uint32_t));
    return st == HUFF_OK;
}

Error burrow__flate_huff_sym(const int *lengths, int n, IoReader r) {
    Alloc *a = heap_allocator();
    IoReadCloser rc = flate_new_reader(a, r);
    if (rc.vt == NULL)
        return burrow_err_out_of_memory;
    FlateDecompressor *f = (FlateDecompressor *)rc.data;
    Error err = BURROW_NO_ERROR;
    if (flate_huff_init(&f->h2, a, lengths, n) == HUFF_OK)
        flate_huff_sym(f, &f->h2, &err);
    else
        err = flate_corrupt(f);
    flate_reader_free(rc);
    return err;
}

Int burrow__flate_avail_write(IoReadCloser rc) {
    return flate_is_ours(rc)
               ? flate_dict_avail_write(&((FlateDecompressor *)rc.data)->dict)
               : 0;
}

IoReader burrow__flate_source(IoReadCloser rc) {
    IoReader none = {NULL, NULL};
    return flate_is_ours(rc) ? ((FlateDecompressor *)rc.data)->r : none;
}

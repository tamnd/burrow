/* compress/bzip2, from bzip2.go, bit_reader.go, huffman.go and
 * move_to_front.go.
 *
 * The decoder is Go's: a bit reader over a byte source, a Huffman tree walked
 * a bit at a time, move-to-front and the run lengths merged into the Huffman
 * loop, the inverse Burrows-Wheeler transform in one array, and the first
 * run-length stage undone as the bytes are handed out. Everything a block
 * needs has a fixed upper bound, so it lives in the reader instead of being
 * allocated per block the way Go does, except for the block itself, whose
 * size comes from the header.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/compress/bzip2.h"

#include "burrow/bufio.h"
#include "burrow/runtime.h"

#include <string.h>

/* ------------------------------------------------------------------ errors */

typedef struct Bzip2StructuralBox {
    Bzip2StructuralError s;
    Str message;
} Bzip2StructuralBox;

static const Type bzip2_structural_desc = {
    {(const Byte *)"StructuralError", 15},
    {(const Byte *)"compress/bzip2", 14},
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
    0x627a7365U, /* "bzse" */
    NULL,
};

const Type *const TYPE_BZIP2_STRUCTURAL_ERROR = &bzip2_structural_desc;

#define BZIP2_PREFIX "bzip2 data invalid: "

static Str bzip2_structural_message(const void *self) {
    return ((const Bzip2StructuralBox *)self)->message;
}

static bool bzip2_structural_is(const void *self, Error target);
static Error bzip2_structural_clone(const void *self, Alloc *a);

static const ErrorVT bzip2_structural_vt = {
    .self_type = &bzip2_structural_desc,
    .message = bzip2_structural_message,
    .is = bzip2_structural_is,
    .clone = bzip2_structural_clone,
};

static bool bzip2_structural_is(const void *self, Error target) {
    if (target.vt != &bzip2_structural_vt || target.data == NULL)
        return false;
    Str a = ((const Bzip2StructuralBox *)self)->s;
    Str b = ((const Bzip2StructuralBox *)target.data)->s;
    return a.len == b.len && (a.len == 0 || memcmp(a.p, b.p, (size_t)a.len) == 0);
}

Error bzip2_structural_error_as_error(Bzip2StructuralError e, Alloc *a) {
    Int plen = (Int)sizeof BZIP2_PREFIX - 1;
    Int mlen = plen + e.len;
    Bzip2StructuralBox *b = (Bzip2StructuralBox *)mem_alloc_nozero(
        a, sizeof(Bzip2StructuralBox) + (size_t)mlen, _Alignof(Bzip2StructuralBox));
    if (b == NULL)
        return burrow_err_out_of_memory;
    Byte *p = (Byte *)(b + 1);
    memcpy(p, BZIP2_PREFIX, (size_t)plen);
    if (e.len > 0)
        memcpy(p + plen, e.p, (size_t)e.len);
    b->message = str_from_bytes(p, mlen);
    b->s = str_from_bytes(p + plen, e.len);
    return (Error){&bzip2_structural_vt, b};
}

static Error bzip2_structural_clone(const void *self, Alloc *a) {
    return bzip2_structural_error_as_error(((const Bzip2StructuralBox *)self)->s, a);
}

Str bzip2_structural_error_error(Bzip2StructuralError e, Alloc *a) {
    Int plen = (Int)sizeof BZIP2_PREFIX - 1;
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)(plen + e.len), 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    memcpy(p, BZIP2_PREFIX, (size_t)plen);
    if (e.len > 0)
        memcpy(p + plen, e.p, (size_t)e.len);
    return str_from_bytes(p, plen + e.len);
}

/* The StructuralErrors the reader gives, in read only memory. */
#define BZIP2_ERROR(name, text)                                                        \
    static const Bzip2StructuralBox name##__box = {                                    \
        {(const Byte *)BZIP2_PREFIX text + sizeof BZIP2_PREFIX - 1, sizeof text - 1},  \
        {(const Byte *)BZIP2_PREFIX text, sizeof BZIP2_PREFIX text - 1}};              \
    static const Error name = {&bzip2_structural_vt, &name##__box}

BZIP2_ERROR(bzip2_err_bad_magic, "bad magic value");
BZIP2_ERROR(bzip2_err_non_huffman, "non-Huffman entropy encoding");
BZIP2_ERROR(bzip2_err_level, "invalid compression level");
BZIP2_ERROR(bzip2_err_block_crc, "block checksum mismatch");
BZIP2_ERROR(bzip2_err_bad_magic_found, "bad magic value found");
BZIP2_ERROR(bzip2_err_file_crc, "file checksum mismatch");
BZIP2_ERROR(bzip2_err_continuation, "bad magic value in continuation file");
BZIP2_ERROR(bzip2_err_randomized, "deprecated randomized files");
BZIP2_ERROR(bzip2_err_no_symbols, "no symbols in input");
BZIP2_ERROR(bzip2_err_num_trees, "invalid number of Huffman trees");
BZIP2_ERROR(bzip2_err_tree_index, "tree index too large");
BZIP2_ERROR(bzip2_err_length_range, "Huffman length out of range");
BZIP2_ERROR(bzip2_err_no_selectors, "no tree selectors given");
BZIP2_ERROR(bzip2_err_selector_range, "tree selector out of range");
BZIP2_ERROR(bzip2_err_few_selectors,
            "insufficient selector indices for number of symbols");
BZIP2_ERROR(bzip2_err_repeat, "repeat count too large");
BZIP2_ERROR(bzip2_err_repeat_end, "repeats past end of block");
BZIP2_ERROR(bzip2_err_block_size, "data exceeds block size");
BZIP2_ERROR(bzip2_err_orig_ptr, "origPtr out of bounds");
BZIP2_ERROR(bzip2_err_empty_tree, "empty Huffman tree");
BZIP2_ERROR(bzip2_err_equal_symbols, "equal symbols in Huffman tree");

static inline bool bzip2_same_error(Error a, Error b) {
    return a.vt == b.vt && a.data == b.data;
}

BURROW_NORETURN static void bzip2_nil_panic(void) {
    runtime_panic(
        BURROW_S("runtime error: invalid memory address or nil pointer dereference"));
}

/* ------------------------------------------------------------------- state */

enum {
    BZIP2_FILE_MAGIC = 0x425a, /* "BZ" */
    BZIP2_MAX_TREES = 6,
    BZIP2_MAX_SYMBOLS = 256 + 2,
    BZIP2_MAX_SELECTORS = 1 << 15,
    BZIP2_INVALID_NODE = 0xffff,
};

#define BZIP2_BLOCK_MAGIC UINT64_C(0x314159265359)
#define BZIP2_FINAL_MAGIC UINT64_C(0x177245385090)

/* A node of a Huffman tree. left and right index nodes, or are
 * BZIP2_INVALID_NODE when that side is a leaf whose symbol is in leftValue or
 * rightValue. */
typedef struct Bzip2Node {
    uint16_t left, right;
    uint16_t left_value, right_value;
} Bzip2Node;

/* A tree has one node fewer than it has symbols. */
typedef struct Bzip2Tree {
    Bzip2Node nodes[BZIP2_MAX_SYMBOLS];
    int next_node;
} Bzip2Tree;

typedef struct Bzip2Reader {
    Alloc *a;

    /* bitReader: where bytes come from, as in compress/lzw, and the bits
     * read from them that have not been used yet, in the low bits of n. */
    IoReader r;
    BufioReader *direct;
    const Method *read_byte;
    BufioReader *rbuf; /* made for a source with no ReadByte */
    uint64_t n;
    unsigned bits;
    Error err;

    uint32_t file_crc;
    uint32_t block_crc;
    uint32_t want_block_crc;
    bool setup_done; /* true once the bzip2 header has been parsed */
    bool eof;
    Int block_size; /* blockSize in bytes, i.e. 900 * 1000 */
    size_t c[256];  /* the "C" array for the inverse BWT */
    uint32_t *tt;   /* bzip2's "tt" array, with the P array in the top 24 bits */
    Int tt_len;     /* how many tt has room for */
    uint32_t t_pos; /* index of the next output byte in tt */

    Int pre_rle_len;       /* tt[:pre_rle_len] is the RLE data still to be processed */
    Int pre_rle_used;      /* how much of it is used */
    int last_byte;         /* the last byte value seen */
    unsigned byte_repeats; /* the number of repeats of last_byte seen */
    unsigned repeats;      /* the number of copies of last_byte to output */

    /* What readBlock makes fresh each block in Go. */
    uint8_t tree_indexes[BZIP2_MAX_SELECTORS];
    Bzip2Tree trees[BZIP2_MAX_TREES];
} Bzip2Reader;

static const Type bzip2_reader_desc = {
    {(const Byte *)"reader", 6},
    {(const Byte *)"compress/bzip2", 14},
    KIND_STRUCT,
    (uint32_t)sizeof(Bzip2Reader),
    (uint16_t)_Alignof(Bzip2Reader),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x627a7264U, /* "bzrd" */
    NULL,
};

/* -------------------------------------------------------------- bitReader */

static inline bool bzip2_read_byte(Bzip2Reader *z, Byte *c, Error *err) {
    Error e = BURROW_NO_ERROR;
    if (z->direct != NULL) {
        *c = bufio_reader_read_byte(z->direct, &e);
    } else if (z->read_byte != NULL) {
        IoErrorArg ea = &e;
        void *args[1] = {(void *)&ea};
        void *rets[1] = {c};
        z->read_byte->thunk(z->r.data, args, rets);
    } else {
        bzip2_nil_panic();
    }
    if (BURROW_UNLIKELY(BURROW_FAILED(e))) {
        *err = e;
        return false;
    }
    return true;
}

static uint64_t bzip2_read_bits64(Bzip2Reader *z, unsigned bits) {
    while (bits > z->bits) {
        Byte b;
        Error e;
        if (!bzip2_read_byte(z, &b, &e)) {
            if (bzip2_same_error(e, io_eof))
                e = io_err_unexpected_eof;
            z->err = e;
            return 0;
        }
        z->n <<= 8;
        z->n |= b;
        z->bits += 8;
    }
    uint64_t n = (z->n >> (z->bits - bits)) & ((UINT64_C(1) << bits) - 1);
    z->bits -= bits;
    return n;
}

static inline int bzip2_read_bits(Bzip2Reader *z, unsigned bits) {
    return (int)bzip2_read_bits64(z, bits);
}

static inline bool bzip2_read_bit(Bzip2Reader *z) {
    return bzip2_read_bits(z, 1) != 0;
}

/* ----------------------------------------------------------------- Huffman */

/* Decode: reads bits and walks the tree until a symbol is found. */
static inline uint16_t bzip2_tree_decode(const Bzip2Tree *t, Bzip2Reader *z) {
    uint16_t node_index = 0; /* node 0 is the root of the tree */
    for (;;) {
        const Bzip2Node *node = &t->nodes[node_index];
        unsigned bit;
        if (z->bits > 0) {
            /* Get next bit, fast path. */
            z->bits--;
            bit = (unsigned)(z->n >> (z->bits & 63)) & 1;
        } else {
            /* Get next bit, slow path. */
            bit = (unsigned)bzip2_read_bits(z, 1);
        }
        node_index = bit == 1 ? node->left : node->right;
        if (node_index == BZIP2_INVALID_NODE)
            /* We found a leaf. Use the value of bit to decide whether is a
             * left or a right value. */
            return bit == 1 ? node->left_value : node->right_value;
    }
}

typedef struct Bzip2Code {
    uint32_t code;
    uint8_t code_len;
    uint16_t value;
} Bzip2Code;

/* buildHuffmanNode takes sorted codes and builds a node of the tree at the
 * given level, returning the index of the node it made. */
static Error bzip2_build_node(Bzip2Tree *t, const Bzip2Code *codes, int ncodes,
                              uint32_t level, uint16_t *node_index) {
    uint32_t test = UINT32_C(1) << (31 - level);

    /* Search the list of codes to find the divide between the left and right
     * sides. */
    int first_right = ncodes;
    for (int i = 0; i < ncodes; i++) {
        if ((codes[i].code & test) != 0) {
            first_right = i;
            break;
        }
    }
    const Bzip2Code *left = codes;
    int nleft = first_right;
    const Bzip2Code *right = codes + first_right;
    int nright = ncodes - first_right;

    if (nleft == 0 || nright == 0) {
        /* There is a superfluous level in the Huffman tree indicating a bug in
         * the encoder. However, this bug has been observed in the wild so we
         * handle it.
         *
         * If this function was called recursively then ncodes >= 2, because
         * otherwise we would have hit the leaf case below and not recurred.
         * For the first call it's possible that ncodes is zero or one. Both
         * cases are invalid because a zero length tree cannot encode anything
         * and a length 1 tree can only encode EOF and so is superfluous. We
         * reject both. */
        if (ncodes < 2)
            return bzip2_err_empty_tree;
        /* In this case the recursion doesn't always reduce the length of codes
         * so we need to ensure termination via another mechanism. Since
         * ncodes >= 2 the only way that the values can match at all 32 bits
         * is if they are equal, which is invalid. This ensures that we never
         * enter infinite recursion. */
        if (level == 31)
            return bzip2_err_equal_symbols;
        if (nleft == 0)
            return bzip2_build_node(t, right, nright, level + 1, node_index);
        return bzip2_build_node(t, left, nleft, level + 1, node_index);
    }

    *node_index = (uint16_t)t->next_node;
    Bzip2Node *node = &t->nodes[t->next_node];
    t->next_node++;

    Error err = BURROW_NO_ERROR;
    if (nleft == 1) {
        /* leaf node */
        node->left = BZIP2_INVALID_NODE;
        node->left_value = left[0].value;
    } else {
        err = bzip2_build_node(t, left, nleft, level + 1, &node->left);
    }
    if (BURROW_FAILED(err))
        return err;

    if (nright == 1) {
        /* leaf node */
        node->right = BZIP2_INVALID_NODE;
        node->right_value = right[0].value;
    } else {
        err = bzip2_build_node(t, right, nright, level + 1, &node->right);
    }
    return err;
}

/* newHuffmanTree builds a Huffman tree from the code lengths of each symbol.
 * bzip2 uses a canonical tree so that it can be rebuilt from the lengths
 * alone. There are always at least three symbols. */
static Error bzip2_new_tree(Bzip2Tree *t, const uint8_t *lengths, int n) {
    /* First sort the symbols by ascending code length, using the symbol value
     * to break ties. Every key is different, so any sort gives Go's order. */
    uint16_t pairs[BZIP2_MAX_SYMBOLS];
    for (int i = 0; i < n; i++) {
        uint16_t v = (uint16_t)i;
        int j = i;
        while (j > 0 && lengths[pairs[j - 1]] > lengths[v]) {
            pairs[j] = pairs[j - 1];
            j--;
        }
        pairs[j] = v;
    }

    /* Now assign codes to the symbols, starting with the longest code. The
     * codes are packed into a uint32 at the most significant end, so branches
     * are taken from the MSB downwards, which makes them easy to sort. */
    Bzip2Code codes[BZIP2_MAX_SYMBOLS];
    uint32_t code = 0;
    uint8_t length = 32;
    for (int i = n - 1; i >= 0; i--) {
        uint8_t l = lengths[pairs[i]];
        if (length > l)
            length = l;
        codes[i].code = code;
        codes[i].code_len = length;
        codes[i].value = pairs[i];
        /* 'Increment' the code, treating it as a length bit number. */
        code += UINT32_C(1) << (32 - length);
    }

    /* Sort by code so that the left half of each branch is grouped together,
     * recursively. Equal codes make the tree fail to build whichever way
     * round they are, so an insertion sort's order is as good as Go's. */
    for (int i = 1; i < n; i++) {
        Bzip2Code x = codes[i];
        int j = i;
        while (j > 0 && codes[j - 1].code > x.code) {
            codes[j] = codes[j - 1];
            j--;
        }
        codes[j] = x;
    }

    t->next_node = 0;
    memset(t->nodes, 0, sizeof(Bzip2Node) * (size_t)n);
    uint16_t root;
    return bzip2_build_node(t, codes, n, 0, &root);
}

/* ---------------------------------------------------------------- reading */

/* setup parses the bzip2 header. */
static Error bzip2_setup(Bzip2Reader *z, bool need_magic) {
    if (need_magic) {
        int magic = bzip2_read_bits(z, 16);
        if (magic != BZIP2_FILE_MAGIC)
            return bzip2_err_bad_magic;
    }
    int t = bzip2_read_bits(z, 8);
    if (t != 'h')
        return bzip2_err_non_huffman;
    int level = bzip2_read_bits(z, 8);
    if (level < '1' || level > '9')
        return bzip2_err_level;

    z->file_crc = 0;
    z->block_size = 100 * 1000 * (Int)(level - '0');
    if (z->block_size > z->tt_len) {
        uint32_t *tt = (uint32_t *)mem_alloc_nozero(
            z->a, sizeof(uint32_t) * (size_t)z->block_size, _Alignof(uint32_t));
        if (tt == NULL)
            return burrow_err_out_of_memory;
        mem_free(z->a, z->tt, sizeof(uint32_t) * (size_t)z->tt_len, _Alignof(uint32_t));
        z->tt = tt;
        z->tt_len = z->block_size;
    }
    return BURROW_NO_ERROR;
}

static const uint32_t bzip2_crctab[256] = {
    0x00000000U, 0x04c11db7U, 0x09823b6eU, 0x0d4326d9U, 0x130476dcU, 0x17c56b6bU,
    0x1a864db2U, 0x1e475005U, 0x2608edb8U, 0x22c9f00fU, 0x2f8ad6d6U, 0x2b4bcb61U,
    0x350c9b64U, 0x31cd86d3U, 0x3c8ea00aU, 0x384fbdbdU, 0x4c11db70U, 0x48d0c6c7U,
    0x4593e01eU, 0x4152fda9U, 0x5f15adacU, 0x5bd4b01bU, 0x569796c2U, 0x52568b75U,
    0x6a1936c8U, 0x6ed82b7fU, 0x639b0da6U, 0x675a1011U, 0x791d4014U, 0x7ddc5da3U,
    0x709f7b7aU, 0x745e66cdU, 0x9823b6e0U, 0x9ce2ab57U, 0x91a18d8eU, 0x95609039U,
    0x8b27c03cU, 0x8fe6dd8bU, 0x82a5fb52U, 0x8664e6e5U, 0xbe2b5b58U, 0xbaea46efU,
    0xb7a96036U, 0xb3687d81U, 0xad2f2d84U, 0xa9ee3033U, 0xa4ad16eaU, 0xa06c0b5dU,
    0xd4326d90U, 0xd0f37027U, 0xddb056feU, 0xd9714b49U, 0xc7361b4cU, 0xc3f706fbU,
    0xceb42022U, 0xca753d95U, 0xf23a8028U, 0xf6fb9d9fU, 0xfbb8bb46U, 0xff79a6f1U,
    0xe13ef6f4U, 0xe5ffeb43U, 0xe8bccd9aU, 0xec7dd02dU, 0x34867077U, 0x30476dc0U,
    0x3d044b19U, 0x39c556aeU, 0x278206abU, 0x23431b1cU, 0x2e003dc5U, 0x2ac12072U,
    0x128e9dcfU, 0x164f8078U, 0x1b0ca6a1U, 0x1fcdbb16U, 0x018aeb13U, 0x054bf6a4U,
    0x0808d07dU, 0x0cc9cdcaU, 0x7897ab07U, 0x7c56b6b0U, 0x71159069U, 0x75d48ddeU,
    0x6b93dddbU, 0x6f52c06cU, 0x6211e6b5U, 0x66d0fb02U, 0x5e9f46bfU, 0x5a5e5b08U,
    0x571d7dd1U, 0x53dc6066U, 0x4d9b3063U, 0x495a2dd4U, 0x44190b0dU, 0x40d816baU,
    0xaca5c697U, 0xa864db20U, 0xa527fdf9U, 0xa1e6e04eU, 0xbfa1b04bU, 0xbb60adfcU,
    0xb6238b25U, 0xb2e29692U, 0x8aad2b2fU, 0x8e6c3698U, 0x832f1041U, 0x87ee0df6U,
    0x99a95df3U, 0x9d684044U, 0x902b669dU, 0x94ea7b2aU, 0xe0b41de7U, 0xe4750050U,
    0xe9362689U, 0xedf73b3eU, 0xf3b06b3bU, 0xf771768cU, 0xfa325055U, 0xfef34de2U,
    0xc6bcf05fU, 0xc27dede8U, 0xcf3ecb31U, 0xcbffd686U, 0xd5b88683U, 0xd1799b34U,
    0xdc3abdedU, 0xd8fba05aU, 0x690ce0eeU, 0x6dcdfd59U, 0x608edb80U, 0x644fc637U,
    0x7a089632U, 0x7ec98b85U, 0x738aad5cU, 0x774bb0ebU, 0x4f040d56U, 0x4bc510e1U,
    0x46863638U, 0x42472b8fU, 0x5c007b8aU, 0x58c1663dU, 0x558240e4U, 0x51435d53U,
    0x251d3b9eU, 0x21dc2629U, 0x2c9f00f0U, 0x285e1d47U, 0x36194d42U, 0x32d850f5U,
    0x3f9b762cU, 0x3b5a6b9bU, 0x0315d626U, 0x07d4cb91U, 0x0a97ed48U, 0x0e56f0ffU,
    0x1011a0faU, 0x14d0bd4dU, 0x19939b94U, 0x1d528623U, 0xf12f560eU, 0xf5ee4bb9U,
    0xf8ad6d60U, 0xfc6c70d7U, 0xe22b20d2U, 0xe6ea3d65U, 0xeba91bbcU, 0xef68060bU,
    0xd727bbb6U, 0xd3e6a601U, 0xdea580d8U, 0xda649d6fU, 0xc423cd6aU, 0xc0e2d0ddU,
    0xcda1f604U, 0xc960ebb3U, 0xbd3e8d7eU, 0xb9ff90c9U, 0xb4bcb610U, 0xb07daba7U,
    0xae3afba2U, 0xaafbe615U, 0xa7b8c0ccU, 0xa379dd7bU, 0x9b3660c6U, 0x9ff77d71U,
    0x92b45ba8U, 0x9675461fU, 0x8832161aU, 0x8cf30badU, 0x81b02d74U, 0x857130c3U,
    0x5d8a9099U, 0x594b8d2eU, 0x5408abf7U, 0x50c9b640U, 0x4e8ee645U, 0x4a4ffbf2U,
    0x470cdd2bU, 0x43cdc09cU, 0x7b827d21U, 0x7f436096U, 0x7200464fU, 0x76c15bf8U,
    0x68860bfdU, 0x6c47164aU, 0x61043093U, 0x65c52d24U, 0x119b4be9U, 0x155a565eU,
    0x18197087U, 0x1cd86d30U, 0x029f3d35U, 0x065e2082U, 0x0b1d065bU, 0x0fdc1becU,
    0x3793a651U, 0x3352bbe6U, 0x3e119d3fU, 0x3ad08088U, 0x2497d08dU, 0x2056cd3aU,
    0x2d15ebe3U, 0x29d4f654U, 0xc5a92679U, 0xc1683bceU, 0xcc2b1d17U, 0xc8ea00a0U,
    0xd6ad50a5U, 0xd26c4d12U, 0xdf2f6bcbU, 0xdbee767cU, 0xe3a1cbc1U, 0xe760d676U,
    0xea23f0afU, 0xeee2ed18U, 0xf0a5bd1dU, 0xf464a0aaU, 0xf9278673U, 0xfde69bc4U,
    0x89b8fd09U, 0x8d79e0beU, 0x803ac667U, 0x84fbdbd0U, 0x9abc8bd5U, 0x9e7d9662U,
    0x933eb0bbU, 0x97ffad0cU, 0xafb010b1U, 0xab710d06U, 0xa6322bdfU, 0xa2f33668U,
    0xbcb4666dU, 0xb8757bdaU, 0xb5365d03U, 0xb1f740b4U,
};

/* updateCRC: CRC-32 like hash/crc32's except that all the shifts are the
 * other way round, so the bits of the input are taken most significant first.
 * The initial value is 0. */
static uint32_t bzip2_update_crc(uint32_t val, const Byte *b, Int n) {
    uint32_t crc = ~val;
    for (Int i = 0; i < n; i++)
        crc = bzip2_crctab[(Byte)(crc >> 24) ^ b[i]] ^ (crc << 8);
    return ~crc;
}

/* The RLE data still to go: bzip2 is a block compressor, except that it has a
 * run-length step before the rest. Undoing that step as a whole would need a
 * buffer for its largest expansion, so it is undone as the bytes are asked
 * for. Any sequence of four equal bytes is followed by a length byte giving
 * how many more of that byte to put out, which can be zero. */
static Int bzip2_read_from_block(Bzip2Reader *z, Byte *buf, Int len) {
    Int n = 0;
    while ((z->repeats > 0 || z->pre_rle_used < z->pre_rle_len) && n < len) {
        if (z->repeats > 0) {
            buf[n] = (Byte)z->last_byte;
            n++;
            z->repeats--;
            if (z->repeats == 0)
                z->last_byte = -1;
            continue;
        }

        z->t_pos = z->tt[z->t_pos];
        Byte b = (Byte)z->t_pos;
        z->t_pos >>= 8;
        z->pre_rle_used++;

        if (z->byte_repeats == 3) {
            z->repeats = b;
            z->byte_repeats = 0;
            continue;
        }

        if (z->last_byte == (int)b)
            z->byte_repeats++;
        else
            z->byte_repeats = 0;
        z->last_byte = (int)b;

        buf[n] = b;
        n++;
    }
    return n;
}

/* inverseBWT: the inverse Burrows-Wheeler transform, as section 4.2 of
 * http://www.hpl.hp.com/techreports/Compaq-DEC/SRC-RR-124.pdf describes it.
 * orig_ptr is that paper's I, and c is its C array after the first pass,
 * which is merged into the Huffman decoding. This is also bzip2's single array
 * method, which leaves the output, still shuffled, in the bottom 8 bits of tt
 * with the index of the next byte in the top 24 bits. The index of the first
 * byte is returned. */
static uint32_t bzip2_inverse_bwt(uint32_t *tt, Int n, size_t orig_ptr, size_t *c) {
    size_t sum = 0;
    for (int i = 0; i < 256; i++) {
        sum += c[i];
        c[i] = sum - c[i];
    }
    for (Int i = 0; i < n; i++) {
        uint32_t b = tt[i] & 0xff;
        tt[c[b]] |= (uint32_t)i << 8;
        c[b]++;
    }
    return tt[orig_ptr] >> 8;
}

/* moveToFrontDecoder.Decode, with a plain copy, which Go found beats cleverer
 * ways because most moves are short and stay in one cache line. */
static inline Byte bzip2_mtf_decode(Byte *m, int n) {
    Byte b = m[n];
    memmove(m + 1, m, (size_t)n);
    m[0] = b;
    return b;
}

/* readBlock reads a bzip2 block. The magic number has already been read. */
static Error bzip2_read_block(Bzip2Reader *z) {
    z->want_block_crc = (uint32_t)bzip2_read_bits64(z, 32);
    z->block_crc = 0;
    z->file_crc = (z->file_crc << 1 | z->file_crc >> 31) ^ z->want_block_crc;
    int randomized = bzip2_read_bits(z, 1);
    if (randomized != 0)
        return bzip2_err_randomized;
    size_t orig_ptr = (size_t)bzip2_read_bits(z, 24);

    /* If not every byte value is used in the block (it's text, say) then the
     * symbol set is reduced. The symbols used are stored as a two level, 16x16
     * bitmap. */
    int symbol_range_used_bitmap = bzip2_read_bits(z, 16);
    bool symbol_present[256] = {false};
    int num_symbols = 0;
    for (unsigned sym_range = 0; sym_range < 16; sym_range++) {
        if ((symbol_range_used_bitmap & (1 << (15 - sym_range))) != 0) {
            int bits = bzip2_read_bits(z, 16);
            for (unsigned symbol = 0; symbol < 16; symbol++) {
                if ((bits & (1 << (15 - symbol))) != 0) {
                    symbol_present[16 * sym_range + symbol] = true;
                    num_symbols++;
                }
            }
        }
    }
    if (num_symbols == 0)
        /* There must be an EOF symbol. */
        return bzip2_err_no_symbols;

    /* A block uses between two and six different Huffman trees. */
    int num_trees = bzip2_read_bits(z, 3);
    if (num_trees < 2 || num_trees > 6)
        return bzip2_err_num_trees;

    /* The Huffman tree can switch every 50 symbols so there's a list of tree
     * indexes telling us which tree to use for each 50 symbol block. They are
     * move-to-front transformed and stored as unary numbers. */
    int num_selectors = bzip2_read_bits(z, 15);
    Byte mtf_trees[BZIP2_MAX_TREES];
    for (int i = 0; i < num_trees; i++)
        mtf_trees[i] = (Byte)i;
    for (int i = 0; i < num_selectors; i++) {
        int c = 0;
        for (;;) {
            int inc = bzip2_read_bits(z, 1);
            if (inc == 0)
                break;
            c++;
        }
        if (c >= num_trees)
            return bzip2_err_tree_index;
        z->tree_indexes[i] = bzip2_mtf_decode(mtf_trees, c);
    }

    /* The list of symbols for the move-to-front transform is taken from the
     * symbol bitmap. */
    Byte mtf[256];
    int next_symbol = 0;
    for (int i = 0; i < 256; i++) {
        if (symbol_present[i]) {
            mtf[next_symbol] = (Byte)i;
            next_symbol++;
        }
    }

    num_symbols += 2; /* to account for the RUNA and RUNB symbols */

    /* Now decode the code lengths for each tree. */
    uint8_t lengths[BZIP2_MAX_SYMBOLS];
    for (int i = 0; i < num_trees; i++) {
        /* The code lengths are delta encoded from a 5 bit base value. */
        int length = bzip2_read_bits(z, 5);
        for (int j = 0; j < num_symbols; j++) {
            for (;;) {
                if (length < 1 || length > 20)
                    return bzip2_err_length_range;
                if (!bzip2_read_bit(z))
                    break;
                if (bzip2_read_bit(z))
                    length--;
                else
                    length++;
            }
            lengths[j] = (uint8_t)length;
        }
        Error err = bzip2_new_tree(&z->trees[i], lengths, num_symbols);
        if (BURROW_FAILED(err))
            return err;
    }

    int selector_index = 1; /* the next tree index to use */
    if (num_selectors == 0)
        return bzip2_err_no_selectors;
    if ((int)z->tree_indexes[0] >= num_trees)
        return bzip2_err_selector_range;
    const Bzip2Tree *tree = &z->trees[z->tree_indexes[0]];
    Int buf_index = 0; /* indexes tt, the output buffer */
    /* The output of the move-to-front transform is run-length encoded and the
     * decoding is merged into the Huffman loop. These two accumulate the
     * repeat count. */
    Int repeat = 0;
    Int repeat_power = 0;

    /* The C array used by the inverse BWT needs to start at zero. */
    memset(z->c, 0, sizeof z->c);

    uint32_t *tt = z->tt;
    int decoded = 0; /* the number of symbols decoded by the current tree */
    for (;;) {
        if (decoded == 50) {
            if (selector_index >= num_selectors)
                return bzip2_err_few_selectors;
            if ((int)z->tree_indexes[selector_index] >= num_trees)
                return bzip2_err_selector_range;
            tree = &z->trees[z->tree_indexes[selector_index]];
            selector_index++;
            decoded = 0;
        }

        uint16_t v = bzip2_tree_decode(tree, z);
        decoded++;

        if (v < 2) {
            /* This is either the RUNA or the RUNB symbol. */
            if (repeat == 0)
                repeat_power = 1;
            repeat += repeat_power << v;
            repeat_power <<= 1;
            /* This limit of 2 million comes from the bzip2 source code. It
             * keeps repeat from overflowing. */
            if (repeat > 2 * 1024 * 1024)
                return bzip2_err_repeat;
            continue;
        }

        if (repeat > 0) {
            /* A complete run length has been decoded, so the last output
             * symbol is repeated. */
            if (repeat > z->block_size - buf_index)
                return bzip2_err_repeat_end;
            Byte b = mtf[0];
            for (Int i = 0; i < repeat; i++)
                tt[buf_index + i] = b;
            z->c[b] += (size_t)repeat;
            buf_index += repeat;
            repeat = 0;
        }

        if ((int)v == num_symbols - 1)
            /* This is the EOF symbol. Because it's always at the end of the
             * move-to-front list, and never gets moved to the front, it has
             * this unique value. */
            break;

        /* Since RUNA and RUNB have the values 0 and 1, one would expect v-2
         * to go to the MTF decoder. However, the front of the MTF list is
         * never referenced as 0, it's always referenced with a run length of
         * 1. Thus 0 doesn't need to be encoded and it is v-1 here. */
        Byte b = bzip2_mtf_decode(mtf, (int)v - 1);
        if (buf_index >= z->block_size)
            return bzip2_err_block_size;
        tt[buf_index] = b;
        z->c[b]++;
        buf_index++;
    }

    if (orig_ptr >= (size_t)buf_index)
        return bzip2_err_orig_ptr;

    /* The entropy decoding is done. Now do the inverse BWT and set up the RLE
     * buffer. */
    z->pre_rle_len = buf_index;
    z->pre_rle_used = 0;
    z->t_pos = bzip2_inverse_bwt(tt, buf_index, orig_ptr, z->c);
    z->last_byte = -1;
    z->byte_repeats = 0;
    z->repeats = 0;
    return BURROW_NO_ERROR;
}

static Int bzip2_read(Bzip2Reader *z, Byte *buf, Int len, Error *err) {
    for (;;) {
        Int n = bzip2_read_from_block(z, buf, len);
        if (n > 0 || len == 0) {
            z->block_crc = bzip2_update_crc(z->block_crc, buf, n);
            *err = BURROW_NO_ERROR;
            return n;
        }

        /* End of block. Check the CRC. */
        if (z->block_crc != z->want_block_crc) {
            z->err = bzip2_err_block_crc;
            *err = z->err;
            return 0;
        }

        /* Find the next block. */
        uint64_t magic = bzip2_read_bits64(z, 48);
        if (magic == BZIP2_BLOCK_MAGIC) {
            /* Start of a block. */
            Error e = bzip2_read_block(z);
            if (BURROW_FAILED(e)) {
                *err = e;
                return 0;
            }
        } else if (magic == BZIP2_FINAL_MAGIC) {
            /* Check the end of file CRC. */
            uint32_t want_file_crc = (uint32_t)bzip2_read_bits64(z, 32);
            if (BURROW_FAILED(z->err)) {
                *err = z->err;
                return 0;
            }
            if (z->file_crc != want_file_crc) {
                z->err = bzip2_err_file_crc;
                *err = z->err;
                return 0;
            }

            /* Skip ahead to a byte boundary. Is there a file concatenated to
             * this one? It would start with BZ. */
            if (z->bits % 8 != 0)
                bzip2_read_bits(z, z->bits % 8);
            Byte b, bz;
            Error e;
            if (!bzip2_read_byte(z, &b, &e)) {
                if (bzip2_same_error(e, io_eof))
                    z->eof = true;
                z->err = e;
                *err = e;
                return 0;
            }
            if (!bzip2_read_byte(z, &bz, &e)) {
                if (bzip2_same_error(e, io_eof))
                    e = io_err_unexpected_eof;
                z->err = e;
                *err = e;
                return 0;
            }
            if (b != 'B' || bz != 'Z') {
                *err = bzip2_err_continuation;
                return 0;
            }
            e = bzip2_setup(z, false);
            if (BURROW_FAILED(e)) {
                *err = e;
                return 0;
            }
        } else {
            *err = bzip2_err_bad_magic_found;
            return 0;
        }
    }
}

static Int bzip2_reader_read(Bzip2Reader *z, Slice p, Error *err) {
    if (z->eof) {
        BURROW_OUT(err, io_eof);
        return 0;
    }
    Error e;
    if (!z->setup_done) {
        e = bzip2_setup(z, true);
        if (BURROW_FAILED(z->err))
            e = z->err;
        if (BURROW_FAILED(e)) {
            BURROW_OUT(err, e);
            return 0;
        }
        z->setup_done = true;
    }
    Int n = bzip2_read(z, (Byte *)p.p, p.len, &e);
    if (BURROW_FAILED(z->err))
        e = z->err;
    BURROW_OUT(err, e);
    return n;
}

static Int bzip2_vt_read(void *self, Slice p, Error *err) {
    return bzip2_reader_read((Bzip2Reader *)self, p, err);
}

static const IoReaderVT bzip2_reader_vt = {&bzip2_reader_desc, bzip2_vt_read};

IoReader bzip2_new_reader(Alloc *a, IoReader r) {
    Bzip2Reader *z = (Bzip2Reader *)mem_alloc(a, sizeof *z, _Alignof(Bzip2Reader));
    if (z == NULL)
        return (IoReader){NULL, NULL};
    z->a = a;
    z->r = r;
    if (r.vt != NULL && r.vt->self_type == TYPE_BUFIO_READER) {
        z->direct = (BufioReader *)r.data;
    } else if ((z->read_byte = burrow__io_read_byte_method(r)) != NULL) {
        /* z->r is used through read_byte. */
    } else if (r.vt != NULL) {
        z->rbuf = bufio_new_reader(a, r);
        if (z->rbuf == NULL) {
            mem_free(a, z, sizeof *z, _Alignof(Bzip2Reader));
            return (IoReader){NULL, NULL};
        }
        z->direct = z->rbuf;
    }
    return (IoReader){&bzip2_reader_vt, z};
}

void bzip2_reader_free(IoReader r) {
    if (r.vt != &bzip2_reader_vt || r.data == NULL)
        return;
    Bzip2Reader *z = (Bzip2Reader *)r.data;
    bufio_reader_free(z->rbuf);
    mem_free(z->a, z->tt, sizeof(uint32_t) * (size_t)z->tt_len, _Alignof(uint32_t));
    mem_free(z->a, z, sizeof *z, _Alignof(Bzip2Reader));
}

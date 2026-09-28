/* compress/lzw, from reader.go and writer.go.
 *
 * The reader is Go's decoder line for line: a table of prefix codes and
 * suffix bytes, and an output buffer that each code's expansion is written
 * into backwards from the end and then moved down. The writer is Go's too, a
 * hash table from (code, byte) to the next code, open addressed, with the
 * whole table cleared when the codes run out.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/compress/lzw.h"

#include "burrow/bufio.h"
#include "burrow/runtime.h"

#include <string.h>

enum {
    LZW_MAX_WIDTH = 12,
    LZW_DECODER_INVALID_CODE = 0xffff,
    LZW_FLUSH_BUFFER = 1 << LZW_MAX_WIDTH,
};

static const Str lzw_err_closed__text = {(const Byte *)"lzw: reader/writer is closed",
                                         28};
static const Error lzw_err_closed = {&burrow_sentinel_error_vt, &lzw_err_closed__text};

static const Str lzw_err_invalid_code__text = {(const Byte *)"lzw: invalid code", 17};
static const Error lzw_err_invalid_code = {&burrow_sentinel_error_vt,
                                           &lzw_err_invalid_code__text};

static const Str lzw_err_unknown_order__text = {(const Byte *)"lzw: unknown order", 18};
static const Error lzw_err_unknown_order = {&burrow_sentinel_error_vt,
                                            &lzw_err_unknown_order__text};

static const Str lzw_err_too_large__text = {
    (const Byte *)"lzw: input byte too large for the litWidth", 42};
static const Error lzw_err_too_large = {&burrow_sentinel_error_vt,
                                        &lzw_err_too_large__text};

/* The writer's own signal that incHi cleared the table, never returned. */
static const Str lzw_err_out_of_codes__text = {(const Byte *)"lzw: out of codes", 17};
static const Error lzw_err_out_of_codes = {&burrow_sentinel_error_vt,
                                           &lzw_err_out_of_codes__text};

static inline bool lzw_same_error(Error a, Error b) {
    return a.vt == b.vt && a.data == b.data;
}

BURROW_NORETURN static void lzw_nil_panic(void) {
    runtime_panic(
        BURROW_S("runtime error: invalid memory address or nil pointer dereference"));
}

/* fmt.Errorf("lzw: litWidth %d out of range", litWidth). */
static Error lzw_width_error(Int lit_width) {
    static const char head[] = "lzw: litWidth ";
    static const char tail[] = " out of range";
    Byte buf[sizeof head + sizeof tail + 24];
    Int n = 0;
    for (Int i = 0; i < (Int)sizeof head - 1; i++)
        buf[n++] = (Byte)head[i];
    Byte digits[20];
    Int nd = 0;
    uint64_t u = lit_width < 0 ? 0U - (uint64_t)lit_width : (uint64_t)lit_width;
    do {
        digits[nd++] = (Byte)('0' + (int)(u % 10));
        u /= 10;
    } while (u != 0);
    if (lit_width < 0)
        buf[n++] = '-';
    while (nd > 0)
        buf[n++] = digits[--nd];
    for (Int i = 0; i < (Int)sizeof tail - 1; i++)
        buf[n++] = (Byte)tail[i];
    return errors_new(error_allocator(), (Str){buf, n});
}

/* ---------------------------------------------------------------- reading */

struct LzwReader {
    Alloc *a;
    /* Where bytes come from: direct when it is a bufio.Reader, the ReadByte
     * method of r when it has one, and neither when there is no source. */
    IoReader r;
    BufioReader *direct;
    const Method *read_byte;
    BufioReader *rbuf; /* made for a source with no ReadByte, and kept */
    uint32_t bits;
    uint32_t n_bits;
    uint32_t width;
    bool msb;
    int lit_width; /* width in bits of literal codes */
    Error err;

    /* The first 1<<lit_width codes are literal codes. The next two codes mean
     * clear and EOF. Other valid codes are in the range [lo, hi] where lo :=
     * clear + 2, with the upper bound incrementing on each code seen.
     *
     * overflow is the code at which hi overflows the code width. It always
     * equals 1 << width.
     *
     * last is the most recently seen code, or LZW_DECODER_INVALID_CODE.
     *
     * An invariant is that hi < overflow. */
    uint16_t clear, eof, hi, overflow, last;

    /* Each code c in [lo, hi] expands to two or more bytes. For c != hi:
     *   suffix[c] is the last of these bytes.
     *   prefix[c] is the code for all but the last byte.
     *   This code can either be a literal code or another code in [lo, c).
     * The c == hi case is a special case. */
    uint8_t suffix[1 << LZW_MAX_WIDTH];
    uint16_t prefix[1 << LZW_MAX_WIDTH];

    /* output is the temporary output buffer. Literal codes are accumulated
     * from the start of the buffer. Non-literal codes decode to a sequence of
     * suffixes that are first written right-to-left from the end of the
     * buffer before being copied to the start of the buffer. It is flushed
     * when it contains >= 1<<LZW_MAX_WIDTH bytes, so that there is always room
     * to decode an entire code. */
    Byte output[2 * (1 << LZW_MAX_WIDTH)];
    Int o;       /* write index into output */
    Int to_read; /* output[to_read:to_read_end] is what Read hands out next */
    Int to_read_end;
};

static const Type lzw_reader_desc = {
    {(const Byte *)"Reader", 6},
    {(const Byte *)"compress/lzw", 12},
    KIND_STRUCT,
    (uint32_t)sizeof(LzwReader),
    (uint16_t)_Alignof(LzwReader),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x6c7a7264U, /* "lzrd" */
    NULL,
};

const Type *const TYPE_LZW_READER = &lzw_reader_desc;

static inline bool lzw_read_byte(LzwReader *r, Byte *c, Error *err) {
    Error e = BURROW_NO_ERROR;
    if (r->direct != NULL) {
        *c = bufio_reader_read_byte(r->direct, &e);
    } else if (r->read_byte != NULL) {
        IoErrorArg ea = &e;
        void *args[1] = {(void *)&ea};
        void *rets[1] = {c};
        r->read_byte->thunk(r->r.data, args, rets);
    } else {
        lzw_nil_panic();
    }
    if (BURROW_UNLIKELY(BURROW_FAILED(e))) {
        *err = e;
        return false;
    }
    return true;
}

/* readLSB and readMSB: the next code, or false and the error from the
 * source. */
static inline bool lzw_read_code(LzwReader *r, uint16_t *code, Error *err) {
    while (r->n_bits < r->width) {
        Byte x;
        if (!lzw_read_byte(r, &x, err))
            return false;
        if (r->msb)
            r->bits |= (uint32_t)x << (24 - r->n_bits);
        else
            r->bits |= (uint32_t)x << r->n_bits;
        r->n_bits += 8;
    }
    if (r->msb) {
        *code = (uint16_t)(r->bits >> (32 - r->width));
        r->bits <<= r->width;
    } else {
        *code = (uint16_t)(r->bits & ((1U << r->width) - 1));
        r->bits >>= r->width;
    }
    r->n_bits -= r->width;
    return true;
}

/* decode decompresses bytes from r and leaves them in output[to_read:]. It
 * stops when it has enough to hand out, or at an error or the end. */
static void lzw_decode(LzwReader *r) {
    for (;;) {
        uint16_t code;
        Error e = BURROW_NO_ERROR;
        if (!lzw_read_code(r, &code, &e)) {
            if (lzw_same_error(e, io_eof))
                e = io_err_unexpected_eof;
            r->err = e;
            break;
        }
        if (code < r->clear) {
            /* We have a literal code. */
            r->output[r->o] = (Byte)code;
            r->o++;
            if (r->last != LZW_DECODER_INVALID_CODE) {
                /* Save what the hi code expands to. */
                r->suffix[r->hi] = (uint8_t)code;
                r->prefix[r->hi] = r->last;
            }
        } else if (code == r->clear) {
            r->width = 1 + (uint32_t)r->lit_width;
            r->hi = r->eof;
            r->overflow = (uint16_t)(1U << r->width);
            r->last = LZW_DECODER_INVALID_CODE;
            continue;
        } else if (code == r->eof) {
            r->err = io_eof;
            break;
        } else if (code <= r->hi) {
            uint16_t c = code;
            Int i = (Int)sizeof r->output - 1;
            if (code == r->hi && r->last != LZW_DECODER_INVALID_CODE) {
                /* code == hi is a special case which expands to the last
                 * expansion followed by the head of the last expansion. To
                 * find the head of the last expansion, we follow the prefix
                 * chain until we find a literal code. */
                c = r->last;
                while (c >= r->clear)
                    c = r->prefix[c];
                r->output[i] = (Byte)c;
                i--;
                c = r->last;
            }
            /* Copy the suffix chain into output and then write that to w. */
            while (c >= r->clear) {
                r->output[i] = r->suffix[c];
                i--;
                c = r->prefix[c];
            }
            r->output[i] = (Byte)c;
            Int n = (Int)sizeof r->output - i;
            memmove(r->output + r->o, r->output + i, (size_t)n);
            r->o += n;
            if (r->last != LZW_DECODER_INVALID_CODE) {
                /* Save what the hi code expands to. */
                r->suffix[r->hi] = (uint8_t)c;
                r->prefix[r->hi] = r->last;
            }
        } else {
            r->err = lzw_err_invalid_code;
            break;
        }
        r->last = code;
        r->hi++;
        if (r->hi >= r->overflow) {
            if (r->width == LZW_MAX_WIDTH) {
                r->last = LZW_DECODER_INVALID_CODE;
                /* Undo the hi++ a few lines above, so that (1) we maintain the
                 * invariant that hi < overflow, and (2) hi does not
                 * eventually overflow a uint16. */
                r->hi--;
            } else {
                r->width++;
                r->overflow = (uint16_t)(1U << r->width);
            }
        }
        if (r->o >= LZW_FLUSH_BUFFER)
            break;
    }
    /* Flush pending output. */
    r->to_read = 0;
    r->to_read_end = r->o;
    r->o = 0;
}

Int lzw_reader_read(LzwReader *r, Slice p, Error *err) {
    for (;;) {
        if (r->to_read < r->to_read_end) {
            Int n = r->to_read_end - r->to_read;
            if (n > p.len)
                n = p.len;
            if (n > 0)
                memcpy(p.p, r->output + r->to_read, (size_t)n);
            r->to_read += n;
            BURROW_OUT(err, BURROW_NO_ERROR);
            return n;
        }
        if (BURROW_FAILED(r->err)) {
            BURROW_OUT(err, r->err);
            return 0;
        }
        lzw_decode(r);
    }
}

Error lzw_reader_close(LzwReader *r) {
    r->err = lzw_err_closed; /* in case any Reads come along */
    return BURROW_NO_ERROR;
}

static void lzw_reader_init(LzwReader *r, IoReader src, LzwOrder order, Int lit_width) {
    switch (order) {
    case LZW_LSB:
        r->msb = false;
        break;
    case LZW_MSB:
        r->msb = true;
        break;
    default:
        r->err = lzw_err_unknown_order;
        return;
    }
    if (lit_width < 2 || 8 < lit_width) {
        r->err = lzw_width_error(lit_width);
        return;
    }

    r->r = src;
    if (src.vt != NULL && src.vt->self_type == TYPE_BUFIO_READER) {
        r->direct = (BufioReader *)src.data;
    } else if ((r->read_byte = burrow__io_read_byte_method(src)) != NULL) {
        /* r->r is used through read_byte. */
    } else if (src.vt != NULL) {
        if (r->rbuf != NULL) {
            bufio_reader_reset(r->rbuf, src);
        } else {
            r->rbuf = bufio_new_reader(r->a, src);
            if (r->rbuf == NULL) {
                r->err = burrow_err_out_of_memory;
                return;
            }
        }
        r->direct = r->rbuf;
    }
    r->lit_width = (int)lit_width;
    r->width = 1 + (uint32_t)lit_width;
    r->clear = (uint16_t)(1U << lit_width);
    r->eof = r->hi = (uint16_t)(r->clear + 1);
    r->overflow = (uint16_t)(1U << r->width);
    r->last = LZW_DECODER_INVALID_CODE;
}

void lzw_reader_reset(LzwReader *r, IoReader src, LzwOrder order, Int lit_width) {
    /* Everything but the allocator and the bufio.Reader kept for reuse. */
    Alloc *a = r->a;
    BufioReader *rbuf = r->rbuf;
    memset(r, 0, sizeof *r);
    r->a = a;
    r->rbuf = rbuf;
    lzw_reader_init(r, src, order, lit_width);
}

LzwReader *lzw_new_reader(Alloc *a, IoReader r, LzwOrder order, Int lit_width) {
    LzwReader *z = (LzwReader *)mem_alloc(a, sizeof *z, _Alignof(LzwReader));
    if (z == NULL)
        return NULL;
    z->a = a;
    lzw_reader_init(z, r, order, lit_width);
    if (lzw_same_error(z->err, burrow_err_out_of_memory)) {
        mem_free(a, z, sizeof *z, _Alignof(LzwReader));
        return NULL;
    }
    return z;
}

void lzw_reader_free(LzwReader *r) {
    if (r == NULL)
        return;
    bufio_reader_free(r->rbuf);
    mem_free(r->a, r, sizeof *r, _Alignof(LzwReader));
}

static Int lzw_reader_vt_read(void *self, Slice p, Error *err) {
    return lzw_reader_read((LzwReader *)self, p, err);
}

static Error lzw_reader_vt_close(void *self) {
    return lzw_reader_close((LzwReader *)self);
}

static const IoReadCloserVT lzw_read_closer_vt = {
    {&lzw_reader_desc, lzw_reader_vt_read},
    {&lzw_reader_desc, lzw_reader_vt_close},
};

IoReader lzw_reader_as_io_reader(LzwReader *r) {
    return (IoReader){&lzw_read_closer_vt.reader, r};
}

IoReadCloser lzw_reader_as_io_read_closer(LzwReader *r) {
    return (IoReadCloser){&lzw_read_closer_vt, r};
}

/* ---------------------------------------------------------------- writing */

enum {
    /* A code is a 12 bit value, stored as a uint32 when encoding to avoid type
     * conversions when shifting bits. */
    LZW_MAX_CODE = (1 << 12) - 1,
    /* There are 1<<12 possible codes, which is an upper bound on the number of
     * valid hash table entries at any given point in time. tableSize is 4x
     * that. */
    LZW_TABLE_SIZE = 4 * (1 << 12),
    LZW_TABLE_MASK = LZW_TABLE_SIZE - 1,
    /* A hash table entry is a uint32. Zero is an invalid entry since the
     * lower 12 bits of a valid entry must be a non-literal code. */
    LZW_INVALID_ENTRY = 0,
};

#define LZW_INVALID_CODE UINT32_C(0xffffffff)

struct LzwWriter {
    Alloc *a;
    /* w is the writer that compressed bytes are written to: dst when it is a
     * bufio.Writer, or wbuf in front of it. */
    BufioWriter *w;
    BufioWriter *wbuf; /* made for a destination that is not one, and kept */
    /* lit_width is the width in bits of literal codes. */
    uint32_t lit_width;
    LzwOrder order;
    bool msb;
    /* n_bits is the number of bits of pending bits; bits holds them. width
     * is the width in bits of the codes. */
    uint32_t n_bits;
    uint32_t width;
    uint32_t bits;
    /* hi is the code implied by the next code emission. overflow is the code
     * at which hi overflows the code width. */
    uint32_t hi, overflow;
    /* saved_code is the accumulated code at the end of the most recent Write
     * call. It is equal to LZW_INVALID_CODE if there was no such call. */
    uint32_t saved_code;
    /* err is the first error encountered during writing. Closing the writer
     * will make any future Write calls return lzw_err_closed. */
    Error err;
    /* table is the hash table from 20-bit keys to 12-bit values. Each table
     * entry contains key<<12|val and collisions resolve by linear probing.
     * The keys consist of a 12-bit code prefix and an 8-bit byte suffix. The
     * values are a 12-bit code. */
    uint32_t table[LZW_TABLE_SIZE];
};

static const Type lzw_writer_desc = {
    {(const Byte *)"Writer", 6},
    {(const Byte *)"compress/lzw", 12},
    KIND_STRUCT,
    (uint32_t)sizeof(LzwWriter),
    (uint16_t)_Alignof(LzwWriter),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x6c7a7772U, /* "lzwr" */
    NULL,
};

const Type *const TYPE_LZW_WRITER = &lzw_writer_desc;

static inline Error lzw_write_byte(LzwWriter *w, Byte c) {
    if (w->w == NULL)
        lzw_nil_panic();
    return bufio_writer_write_byte(w->w, c);
}

/* writeLSB and writeMSB: write the code c for the LSB and MSB orders. */
static Error lzw_write_code(LzwWriter *w, uint32_t c) {
    if (w->msb) {
        w->bits |= c << (32 - w->width - w->n_bits);
        w->n_bits += w->width;
        while (w->n_bits >= 8) {
            Error e = lzw_write_byte(w, (Byte)(w->bits >> 24));
            if (BURROW_FAILED(e))
                return e;
            w->bits <<= 8;
            w->n_bits -= 8;
        }
    } else {
        w->bits |= c << w->n_bits;
        w->n_bits += w->width;
        while (w->n_bits >= 8) {
            Error e = lzw_write_byte(w, (Byte)w->bits);
            if (BURROW_FAILED(e))
                return e;
            w->bits >>= 8;
            w->n_bits -= 8;
        }
    }
    return BURROW_NO_ERROR;
}

/* incHi increments w->hi and checks for both overflow and running out of
 * unused codes. In the latter case, incHi sends a clear code, resets the
 * writer state and returns lzw_err_out_of_codes. */
static Error lzw_inc_hi(LzwWriter *w) {
    w->hi++;
    if (w->hi == w->overflow) {
        w->width++;
        w->overflow <<= 1;
    }
    if (w->hi == LZW_MAX_CODE) {
        uint32_t clear = UINT32_C(1) << w->lit_width;
        Error e = lzw_write_code(w, clear);
        if (BURROW_FAILED(e))
            return e;
        w->width = w->lit_width + 1;
        w->hi = clear + 1;
        w->overflow = clear << 1;
        memset(w->table, 0, sizeof w->table);
        return lzw_err_out_of_codes;
    }
    return BURROW_NO_ERROR;
}

Int lzw_writer_write(LzwWriter *w, Slice p, Error *err) {
    if (BURROW_FAILED(w->err)) {
        BURROW_OUT(err, w->err);
        return 0;
    }
    if (p.len == 0) {
        BURROW_OUT(err, BURROW_NO_ERROR);
        return 0;
    }
    const Byte *in = (const Byte *)p.p;
    Int len = p.len;
    Byte max_lit = (Byte)((1U << w->lit_width) - 1);
    if (max_lit != 0xff) {
        for (Int i = 0; i < len; i++) {
            if (in[i] > max_lit) {
                w->err = lzw_err_too_large;
                BURROW_OUT(err, w->err);
                return 0;
            }
        }
    }
    Int n = len;
    uint32_t code = w->saved_code;
    if (code == LZW_INVALID_CODE) {
        /* This is the first write; send a clear code.
         * https://www.w3.org/Graphics/GIF/spec-gif89a.txt Appendix F
         * "Variable-Length-Code LZW Compression" says that "Encoders should
         * output a Clear code as the first code of each image data stream".
         *
         * LZW compression isn't only used by GIF, but it's cheap to follow
         * that directive unconditionally. */
        uint32_t clear = UINT32_C(1) << w->lit_width;
        Error e = lzw_write_code(w, clear);
        if (BURROW_FAILED(e)) {
            BURROW_OUT(err, e);
            return 0;
        }
        code = in[0];
        in++;
        len--;
    }
    for (Int i = 0; i < len; i++) {
        uint32_t literal = in[i];
        uint32_t key = code << 8 | literal;
        /* If there is a hash table hit for this key then we continue the
         * loop and do not emit a code yet. */
        uint32_t hash = (key >> 12 ^ key) & LZW_TABLE_MASK;
        bool hit = false;
        for (uint32_t h = hash, t = w->table[hash]; t != LZW_INVALID_ENTRY;) {
            if (key == t >> 12) {
                code = t & LZW_MAX_CODE;
                hit = true;
                break;
            }
            h = (h + 1) & LZW_TABLE_MASK;
            t = w->table[h];
        }
        if (hit)
            continue;
        /* Otherwise, write the current code, and literal becomes the start of
         * the next emitted code. */
        w->err = lzw_write_code(w, code);
        if (BURROW_FAILED(w->err)) {
            BURROW_OUT(err, w->err);
            return 0;
        }
        code = literal;
        /* Increment e.hi, the next implied code. If we run out of codes,
         * reset the writer state (including clearing the hash table) and
         * continue. */
        Error e1 = lzw_inc_hi(w);
        if (BURROW_FAILED(e1)) {
            if (lzw_same_error(e1, lzw_err_out_of_codes))
                continue;
            w->err = e1;
            BURROW_OUT(err, w->err);
            return 0;
        }
        /* Otherwise, insert key -> e.hi into the map that e.table represents. */
        for (;;) {
            if (w->table[hash] == LZW_INVALID_ENTRY) {
                w->table[hash] = (key << 12) | w->hi;
                break;
            }
            hash = (hash + 1) & LZW_TABLE_MASK;
        }
    }
    w->saved_code = code;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return n;
}

Error lzw_writer_close(LzwWriter *w) {
    if (BURROW_FAILED(w->err)) {
        if (lzw_same_error(w->err, lzw_err_closed))
            return BURROW_NO_ERROR;
        return w->err;
    }
    /* Make any future calls to Write return lzw_err_closed. */
    w->err = lzw_err_closed;
    /* Write the saved code, if valid. */
    if (w->saved_code != LZW_INVALID_CODE) {
        Error e = lzw_write_code(w, w->saved_code);
        if (BURROW_FAILED(e))
            return e;
        e = lzw_inc_hi(w);
        if (BURROW_FAILED(e) && !lzw_same_error(e, lzw_err_out_of_codes))
            return e;
    } else {
        /* Write the starting clear code, as w->write did not. */
        uint32_t clear = UINT32_C(1) << w->lit_width;
        Error e = lzw_write_code(w, clear);
        if (BURROW_FAILED(e))
            return e;
    }
    /* Write the eof code. */
    uint32_t eof = (UINT32_C(1) << w->lit_width) + 1;
    Error e = lzw_write_code(w, eof);
    if (BURROW_FAILED(e))
        return e;
    /* Write the final bits. */
    if (w->n_bits > 0) {
        if (w->msb)
            w->bits >>= 24;
        e = lzw_write_byte(w, (Byte)w->bits);
        if (BURROW_FAILED(e))
            return e;
    }
    return bufio_writer_flush(w->w);
}

static void lzw_writer_init(LzwWriter *w, IoWriter dst, LzwOrder order, Int lit_width) {
    switch (order) {
    case LZW_LSB:
        w->msb = false;
        break;
    case LZW_MSB:
        w->msb = true;
        break;
    default:
        w->err = lzw_err_unknown_order;
        return;
    }
    if (lit_width < 2 || 8 < lit_width) {
        w->err = lzw_width_error(lit_width);
        return;
    }
    if (dst.vt != NULL && dst.vt->self_type == TYPE_BUFIO_WRITER) {
        w->w = (BufioWriter *)dst.data;
    } else if (dst.vt != NULL && dst.vt->self_type == TYPE_BUFIO_READ_WRITER) {
        w->w = ((BufioReadWriter *)dst.data)->writer;
    } else if (dst.vt != NULL) {
        if (w->wbuf != NULL) {
            bufio_writer_reset(w->wbuf, dst);
        } else {
            w->wbuf = bufio_new_writer(w->a, dst);
            if (w->wbuf == NULL) {
                w->err = burrow_err_out_of_memory;
                return;
            }
        }
        w->w = w->wbuf;
    }
    uint32_t lw = (uint32_t)lit_width;
    w->order = order;
    w->width = 1 + lw;
    w->lit_width = lw;
    w->hi = (UINT32_C(1) << lw) + 1;
    w->overflow = UINT32_C(1) << (lw + 1);
    w->saved_code = LZW_INVALID_CODE;
}

void lzw_writer_reset(LzwWriter *w, IoWriter dst, LzwOrder order, Int lit_width) {
    /* Everything but the allocator and the bufio.Writer kept for reuse. */
    Alloc *a = w->a;
    BufioWriter *wbuf = w->wbuf;
    memset(w, 0, sizeof *w);
    w->a = a;
    w->wbuf = wbuf;
    lzw_writer_init(w, dst, order, lit_width);
}

LzwWriter *lzw_new_writer(Alloc *a, IoWriter w, LzwOrder order, Int lit_width) {
    LzwWriter *z = (LzwWriter *)mem_alloc(a, sizeof *z, _Alignof(LzwWriter));
    if (z == NULL)
        return NULL;
    z->a = a;
    lzw_writer_init(z, w, order, lit_width);
    if (lzw_same_error(z->err, burrow_err_out_of_memory)) {
        mem_free(a, z, sizeof *z, _Alignof(LzwWriter));
        return NULL;
    }
    return z;
}

void lzw_writer_free(LzwWriter *w) {
    if (w == NULL)
        return;
    bufio_writer_free(w->wbuf);
    mem_free(w->a, w, sizeof *w, _Alignof(LzwWriter));
}

static Int lzw_writer_vt_write(void *self, Slice p, Error *err) {
    return lzw_writer_write((LzwWriter *)self, p, err);
}

static Error lzw_writer_vt_close(void *self) {
    return lzw_writer_close((LzwWriter *)self);
}

static const IoWriteCloserVT lzw_write_closer_vt = {
    {&lzw_writer_desc, lzw_writer_vt_write},
    {&lzw_writer_desc, lzw_writer_vt_close},
};

IoWriter lzw_writer_as_io_writer(LzwWriter *w) {
    return (IoWriter){&lzw_write_closer_vt.writer, w};
}

IoWriteCloser lzw_writer_as_io_write_closer(LzwWriter *w) {
    return (IoWriteCloser){&lzw_write_closer_vt, w};
}

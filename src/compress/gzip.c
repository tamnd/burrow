/* compress/gzip, from gunzip.go and gzip.go.
 *
 * The header is ten fixed bytes and then whichever of the extra field, the
 * name, the comment and a CRC-16 of the header the flags say are there. The
 * trailer is the CRC-32 and the length of the data, both little endian. The
 * DEFLATE in between is compress/flate's.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/compress/gzip.h"

#include "burrow/hash/crc32.h"
#include "burrow/panic.h"
#include "burrow/utf8.h"

#include <string.h>

BURROW_SENTINEL_ERROR(gzip_err_checksum, "gzip: invalid checksum");
BURROW_SENTINEL_ERROR(gzip_err_header, "gzip: invalid header");

enum {
    GZIP_ID1 = 0x1f,
    GZIP_ID2 = 0x8b,
    GZIP_DEFLATE = 8,
    GZIP_FLAG_TEXT = 1 << 0,
    GZIP_FLAG_HDR_CRC = 1 << 1,
    GZIP_FLAG_EXTRA = 1 << 2,
    GZIP_FLAG_NAME = 1 << 3,
    GZIP_FLAG_COMMENT = 1 << 4,
};

/* What a header extra of no bytes points at, so that it is not nil. */
static const Byte gzip_no_bytes[1] = {0};

static inline bool gzip_same_error(Error a, Error b) {
    return a.vt == b.vt && a.data == b.data;
}

static inline uint16_t gzip_le16(const Byte *b) {
    return (uint16_t)(b[0] | b[1] << 8);
}

static inline uint32_t gzip_le32(const Byte *b) {
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 |
           (uint32_t)b[3] << 24;
}

static inline void gzip_put_le16(Byte *b, uint16_t v) {
    b[0] = (Byte)v;
    b[1] = (Byte)(v >> 8);
}

static inline void gzip_put_le32(Byte *b, uint32_t v) {
    b[0] = (Byte)v;
    b[1] = (Byte)(v >> 8);
    b[2] = (Byte)(v >> 16);
    b[3] = (Byte)(v >> 24);
}

static inline Slice gzip_bytes(Byte *p, Int n) {
    return slice_from(p, n, n, TYPE_BYTE);
}

static inline uint32_t gzip_crc(uint32_t crc, const Byte *p, Int n) {
    return crc32_update(crc, crc32_ieee_table,
                        slice_from((void *)(uintptr_t)p, n, n, TYPE_BYTE));
}

static Error gzip_no_eof(Error err) {
    return gzip_same_error(err, io_eof) ? io_err_unexpected_eof : err;
}

/* ---------------------------------------------------------------- reading */

static const Type gzip_reader_desc = {
    {(const Byte *)"Reader", 6},
    {(const Byte *)"compress/gzip", 13},
    KIND_STRUCT,
    (uint32_t)sizeof(GzipReader),
    (uint16_t)_Alignof(GzipReader),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x677a7264U, /* "gzrd" */
    NULL,
};

const Type *const TYPE_GZIP_READER = &gzip_reader_desc;

/* Gives what a header read put in hdr back to a, and empties it. */
static void gzip_header_free(Alloc *a, GzipHeader *hdr) {
    if (hdr->comment.len > 0)
        mem_free(a, (void *)(uintptr_t)hdr->comment.p, (size_t)hdr->comment.len, 1);
    if (hdr->name.len > 0)
        mem_free(a, (void *)(uintptr_t)hdr->name.p, (size_t)hdr->name.len, 1);
    if (hdr->extra.len > 0)
        mem_free(a, hdr->extra.p, (size_t)hdr->extra.len, 1);
    memset(hdr, 0, sizeof *hdr);
}

/* One byte from z->r, through the bufio.Reader directly or the ReadByte in
 * r's method set. */
static Byte gzip_read_byte(GzipReader *z, Error *err) {
    Byte c = 0;
    if (z->r.vt->self_type == TYPE_BUFIO_READER)
        return bufio_reader_read_byte((BufioReader *)z->r.data, err);
    IoErrorArg ea = err;
    void *args[1] = {(void *)&ea};
    void *rets[1] = {&c};
    burrow__io_read_byte_method(z->r)->thunk(z->r.data, args, rets);
    return c;
}

/* Reads a NUL-terminated string, which the header has in Latin-1, and gives
 * it back as UTF-8 from a. */
static Error gzip_read_string(GzipReader *z, Str *out) {
    bool need_conv = false;
    for (Int i = 0;; i++) {
        if (i >= (Int)sizeof z->buf)
            return gzip_err_header;
        Error err = BURROW_NO_ERROR;
        z->buf[i] = gzip_read_byte(z, &err);
        if (BURROW_FAILED(err))
            return err;
        if (z->buf[i] > 0x7f)
            need_conv = true;
        if (z->buf[i] != 0)
            continue;

        /* Digest covers the NUL terminator. */
        z->digest = gzip_crc(z->digest, z->buf, i + 1);

        /* Strings are ISO 8859-1, Latin-1 (RFC 1952, section 2.3.1). */
        Int n = i;
        if (need_conv)
            for (Int k = 0; k < i; k++)
                n += z->buf[k] > 0x7f;
        if (n == 0) {
            *out = (Str){NULL, 0};
            return BURROW_NO_ERROR;
        }
        Byte *p = (Byte *)mem_alloc_nozero(z->a, (size_t)n, 1);
        if (p == NULL)
            return burrow_err_out_of_memory;
        if (!need_conv) {
            memcpy(p, z->buf, (size_t)n);
        } else {
            Int o = 0;
            for (Int k = 0; k < i; k++) {
                Byte c = z->buf[k];
                if (c > 0x7f) {
                    p[o++] = (Byte)(0xc0 | c >> 6);
                    p[o++] = (Byte)(0x80 | (c & 0x3f));
                } else {
                    p[o++] = c;
                }
            }
        }
        *out = (Str){p, n};
        return BURROW_NO_ERROR;
    }
}

/* Reads the GZIP header according to section 2.3.1. This method does not set
 * z->err. */
static Error gzip_read_header(GzipReader *z, GzipHeader *hdr) {
    Error err = BURROW_NO_ERROR;
    io_read_full(z->r, gzip_bytes(z->buf, 10), &err);
    if (BURROW_FAILED(err)) {
        /* RFC 1952, section 2.2, says the following:
         *	A gzip file consists of a series of "members" (compressed data
         *	sets).
         *
         * Other than this, the specification does not clarify whether a
         * "series" is defined as "one or more" or "zero or more". To err on
         * the side of caution, Go interprets this to mean "zero or more".
         * Thus, it is okay to return io_eof here. */
        return err;
    }
    if (z->buf[0] != GZIP_ID1 || z->buf[1] != GZIP_ID2 || z->buf[2] != GZIP_DEFLATE)
        return gzip_err_header;
    Byte flg = z->buf[3];
    int64_t t = (int64_t)gzip_le32(z->buf + 4);
    if (t > 0) {
        /* Section 2.3.1, the zero value for MTIME means that the modified
         * time is not set. */
        hdr->mod_time = time_from_unix(t, 0);
    }
    /* z->buf[8] is XFL and is currently ignored. */
    hdr->os = z->buf[9];
    z->digest = gzip_crc(0, z->buf, 10);

    if ((flg & GZIP_FLAG_EXTRA) != 0) {
        io_read_full(z->r, gzip_bytes(z->buf, 2), &err);
        if (BURROW_FAILED(err))
            return gzip_no_eof(err);
        z->digest = gzip_crc(z->digest, z->buf, 2);
        Int n = gzip_le16(z->buf);
        Byte *data = (Byte *)(uintptr_t)gzip_no_bytes;
        if (n > 0) {
            data = (Byte *)mem_alloc_nozero(z->a, (size_t)n, 1);
            if (data == NULL)
                return burrow_err_out_of_memory;
        }
        hdr->extra = gzip_bytes(data, n);
        io_read_full(z->r, hdr->extra, &err);
        if (BURROW_FAILED(err)) {
            if (n > 0)
                mem_free(z->a, data, (size_t)n, 1);
            hdr->extra = (Slice){NULL, 0, 0, NULL};
            return gzip_no_eof(err);
        }
        z->digest = gzip_crc(z->digest, data, n);
    }

    if ((flg & GZIP_FLAG_NAME) != 0) {
        err = gzip_read_string(z, &hdr->name);
        if (BURROW_FAILED(err))
            return gzip_no_eof(err);
    }

    if ((flg & GZIP_FLAG_COMMENT) != 0) {
        err = gzip_read_string(z, &hdr->comment);
        if (BURROW_FAILED(err))
            return gzip_no_eof(err);
    }

    if ((flg & GZIP_FLAG_HDR_CRC) != 0) {
        io_read_full(z->r, gzip_bytes(z->buf, 2), &err);
        if (BURROW_FAILED(err))
            return gzip_no_eof(err);
        if (gzip_le16(z->buf) != (uint16_t)z->digest)
            return gzip_err_header;
    }

    z->digest = 0;
    if (z->decompressor.vt == NULL) {
        z->decompressor = flate_new_reader(z->a, z->r);
        if (z->decompressor.vt == NULL)
            return burrow_err_out_of_memory;
    } else {
        FlateResetter rs = flate_reader_as_resetter(z->decompressor);
        if (rs.vt == NULL) /* Go's type assertion, which cannot fail here */
            panic_str(BURROW_S("gzip: decompressor is not a flate.Resetter"));
        Slice none = {NULL, 0, 0, NULL};
        Error e = flate_resetter_reset(rs, z->r, none);
        if (BURROW_FAILED(e))
            return e;
    }
    return BURROW_NO_ERROR;
}

Error gzip_reader_reset(GzipReader *z, IoReader r) {
    gzip_header_free(z->a, &z->header);
    z->digest = 0;
    z->size = 0;
    z->err = BURROW_NO_ERROR;
    z->multistream = true;
    memset(z->buf, 0, sizeof z->buf);
    if ((r.vt != NULL && r.vt->self_type == TYPE_BUFIO_READER) ||
        burrow__io_read_byte_method(r) != NULL) {
        z->r = r;
    } else {
        if (z->rbuf != NULL) {
            bufio_reader_reset(z->rbuf, r);
        } else {
            z->rbuf = bufio_new_reader(z->a, r);
            if (z->rbuf == NULL) {
                z->err = burrow_err_out_of_memory;
                return z->err;
            }
        }
        z->r = bufio_reader_as_io_reader(z->rbuf);
    }
    z->err = gzip_read_header(z, &z->header);
    return z->err;
}

GzipReader *gzip_new_reader(Alloc *a, IoReader r, Error *err) {
    GzipReader *z = (GzipReader *)mem_alloc(a, sizeof *z, _Alignof(GzipReader));
    if (z == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    z->a = a;
    Error e = gzip_reader_reset(z, r);
    if (BURROW_FAILED(e)) {
        gzip_reader_free(z);
        BURROW_OUT(err, e);
        return NULL;
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return z;
}

void gzip_reader_multistream(GzipReader *z, bool ok) {
    z->multistream = ok;
}

Int gzip_reader_read(GzipReader *z, Slice p, Error *err) {
    if (BURROW_FAILED(z->err)) {
        BURROW_OUT(err, z->err);
        return 0;
    }

    Int n = 0;
    while (n == 0) {
        Error e = BURROW_NO_ERROR;
        n = z->decompressor.vt->reader.read(z->decompressor.data, p, &e);
        z->err = e;
        z->digest = gzip_crc(z->digest, (const Byte *)p.p, n);
        z->size += (uint32_t)n;
        if (!gzip_same_error(z->err, io_eof)) {
            /* In the normal case we return here. */
            BURROW_OUT(err, z->err);
            return n;
        }

        /* Finished file; check checksum and size. */
        Error re = BURROW_NO_ERROR;
        io_read_full(z->r, gzip_bytes(z->buf, 8), &re);
        if (BURROW_FAILED(re)) {
            z->err = gzip_no_eof(re);
            BURROW_OUT(err, z->err);
            return n;
        }
        uint32_t digest = gzip_le32(z->buf);
        uint32_t size = gzip_le32(z->buf + 4);
        if (digest != z->digest || size != z->size) {
            z->err = gzip_err_checksum;
            BURROW_OUT(err, z->err);
            return n;
        }
        z->digest = 0;
        z->size = 0;

        /* File is ok; check if there is another. */
        if (!z->multistream) {
            BURROW_OUT(err, io_eof);
            return n;
        }
        z->err = BURROW_NO_ERROR; /* Remove io_eof */

        GzipHeader next = {0};
        z->err = gzip_read_header(z, &next);
        gzip_header_free(z->a, &next);
        if (BURROW_FAILED(z->err)) {
            BURROW_OUT(err, z->err);
            return n;
        }
    }

    BURROW_OUT(err, BURROW_NO_ERROR);
    return n;
}

Error gzip_reader_close(GzipReader *z) {
    /* Go would dereference a nil decompressor here, after a reset whose
     * header could not be read on a reader that never had one. */
    if (z->decompressor.vt == NULL)
        return BURROW_NO_ERROR;
    return z->decompressor.vt->closer.close(z->decompressor.data);
}

static Int gzip_vt_read(void *self, Slice p, Error *err) {
    return gzip_reader_read((GzipReader *)self, p, err);
}

static Error gzip_vt_read_close(void *self) {
    return gzip_reader_close((GzipReader *)self);
}

static const IoReadCloserVT gzip_read_closer_vt = {
    {&gzip_reader_desc, gzip_vt_read},
    {&gzip_reader_desc, gzip_vt_read_close},
};

IoReader gzip_reader_as_io_reader(GzipReader *z) {
    return (IoReader){&gzip_read_closer_vt.reader, z};
}

IoReadCloser gzip_reader_as_io_read_closer(GzipReader *z) {
    return (IoReadCloser){&gzip_read_closer_vt, z};
}

void gzip_reader_free(GzipReader *z) {
    if (z == NULL)
        return;
    gzip_header_free(z->a, &z->header);
    flate_reader_free(z->decompressor);
    bufio_reader_free(z->rbuf);
    mem_free(z->a, z, sizeof *z, _Alignof(GzipReader));
}

/* ---------------------------------------------------------------- writing */

static const Type gzip_writer_desc = {
    {(const Byte *)"Writer", 6},
    {(const Byte *)"compress/gzip", 13},
    KIND_STRUCT,
    (uint32_t)sizeof(GzipWriter),
    (uint16_t)_Alignof(GzipWriter),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x677a7772U, /* "gzwr" */
    NULL,
};

const Type *const TYPE_GZIP_WRITER = &gzip_writer_desc;

/* fmt.Errorf("gzip: invalid compression level: %d", level). */
static Error gzip_level_error(Int level) {
    static const char head[] = "gzip: invalid compression level: ";
    Byte buf[sizeof head + 24];
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
    return errors_new(error_allocator(), (Str){buf, n});
}

static void gzip_writer_init(GzipWriter *z, IoWriter w, Int level) {
    FlateWriter *compressor = z->compressor;
    if (compressor != NULL)
        flate_writer_reset(compressor, w);
    Alloc *a = z->a;
    memset(z, 0, sizeof *z);
    z->header.os = 255; /* unknown */
    z->a = a;
    z->w = w;
    z->level = level;
    z->compressor = compressor;
}

GzipWriter *gzip_new_writer_level(Alloc *a, IoWriter w, Int level, Error *err) {
    if (level < GZIP_HUFFMAN_ONLY || level > GZIP_BEST_COMPRESSION) {
        BURROW_OUT(err, gzip_level_error(level));
        return NULL;
    }
    GzipWriter *z = (GzipWriter *)mem_alloc(a, sizeof *z, _Alignof(GzipWriter));
    if (z == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    z->a = a;
    gzip_writer_init(z, w, level);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return z;
}

GzipWriter *gzip_new_writer(Alloc *a, IoWriter w) {
    return gzip_new_writer_level(a, w, GZIP_DEFAULT_COMPRESSION, NULL);
}

void gzip_writer_reset(GzipWriter *z, IoWriter w) {
    gzip_writer_init(z, w, z->level);
}

static Error gzip_write_all(GzipWriter *z, Slice b) {
    Error err = BURROW_NO_ERROR;
    z->w.vt->write(z->w.data, b, &err);
    return err;
}

/* Writes a length-prefixed byte slice to z->w. */
static Error gzip_write_bytes(GzipWriter *z, Slice b) {
    if (b.len > 0xffff)
        return errors_new(error_allocator(),
                          BURROW_S("gzip.Write: Extra data is too large"));
    gzip_put_le16(z->buf, (uint16_t)b.len);
    Error err = gzip_write_all(z, gzip_bytes(z->buf, 2));
    if (BURROW_FAILED(err))
        return err;
    return gzip_write_all(z, b);
}

/* Writes a UTF-8 string s in GZIP's format to z->w. GZIP (RFC 1952)
 * specifies that strings are NUL-terminated ISO 8859-1 (Latin-1). */
static Error gzip_write_string(GzipWriter *z, Str s) {
    /* GZIP stores Latin-1 strings; error if non-Latin-1; convert if
     * non-ASCII. */
    bool need_conv = false;
    Int runes = 0;
    for (Int i = 0; i < s.len;) {
        Int size = 0;
        Rune v = utf8_decode_rune_in_string((Str){s.p + i, s.len - i}, &size);
        if (v == 0 || v > 0xff)
            return errors_new(error_allocator(),
                              BURROW_S("gzip.Write: non-Latin-1 header string"));
        if (v > 0x7f)
            need_conv = true;
        i += size;
        runes++;
    }
    Error err;
    if (need_conv) {
        Byte *b = (Byte *)mem_alloc_nozero(z->a, (size_t)runes, 1);
        if (b == NULL)
            return burrow_err_out_of_memory;
        Int n = 0;
        for (Int i = 0; i < s.len;) {
            Int size = 0;
            b[n++] = (Byte)utf8_decode_rune_in_string((Str){s.p + i, s.len - i}, &size);
            i += size;
        }
        err = gzip_write_all(z, gzip_bytes(b, n));
        mem_free(z->a, b, (size_t)runes, 1);
    } else {
        err = gzip_write_all(
            z, slice_from((void *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE));
    }
    if (BURROW_FAILED(err))
        return err;
    /* GZIP strings are NUL-terminated. */
    z->buf[0] = 0;
    return gzip_write_all(z, gzip_bytes(z->buf, 1));
}

/* Writes the header, the first time through Write. */
static Error gzip_write_header(GzipWriter *z) {
    z->wrote_header = true;
    memset(z->buf, 0, sizeof z->buf);
    z->buf[0] = GZIP_ID1;
    z->buf[1] = GZIP_ID2;
    z->buf[2] = GZIP_DEFLATE;
    if (z->header.extra.p != NULL)
        z->buf[3] |= 0x04;
    if (z->header.name.len > 0)
        z->buf[3] |= 0x08;
    if (z->header.comment.len > 0)
        z->buf[3] |= 0x10;
    if (time_after(z->header.mod_time, time_from_unix(0, 0))) {
        /* Section 2.3.1, the zero value for MTIME means that the modified
         * time is not set. */
        gzip_put_le32(z->buf + 4, (uint32_t)time_unix(z->header.mod_time));
    }
    if (z->level == GZIP_BEST_COMPRESSION)
        z->buf[8] = 2;
    else if (z->level == GZIP_BEST_SPEED)
        z->buf[8] = 4;
    z->buf[9] = z->header.os;
    Error err = gzip_write_all(z, gzip_bytes(z->buf, 10));
    if (BURROW_FAILED(err))
        return err;
    if (z->header.extra.p != NULL) {
        err = gzip_write_bytes(z, z->header.extra);
        if (BURROW_FAILED(err))
            return err;
    }
    if (z->header.name.len > 0) {
        err = gzip_write_string(z, z->header.name);
        if (BURROW_FAILED(err))
            return err;
    }
    if (z->header.comment.len > 0) {
        err = gzip_write_string(z, z->header.comment);
        if (BURROW_FAILED(err))
            return err;
    }
    if (z->compressor == NULL) {
        z->compressor = flate_new_writer(z->a, z->w, z->level, &err);
        if (z->compressor == NULL)
            return err;
    }
    return BURROW_NO_ERROR;
}

Int gzip_writer_write(GzipWriter *z, Slice p, Error *err) {
    if (BURROW_FAILED(z->err)) {
        BURROW_OUT(err, z->err);
        return 0;
    }
    /* Write the GZIP header lazily. */
    if (!z->wrote_header) {
        z->err = gzip_write_header(z);
        if (BURROW_FAILED(z->err)) {
            BURROW_OUT(err, z->err);
            return 0;
        }
    }
    z->size += (uint32_t)p.len;
    z->digest = gzip_crc(z->digest, (const Byte *)p.p, p.len);
    Error e = BURROW_NO_ERROR;
    Int n = flate_writer_write(z->compressor, p, &e);
    z->err = e;
    BURROW_OUT(err, e);
    return n;
}

Error gzip_writer_flush(GzipWriter *z) {
    if (BURROW_FAILED(z->err))
        return z->err;
    if (z->closed)
        return BURROW_NO_ERROR;
    if (!z->wrote_header) {
        Slice none = {NULL, 0, 0, NULL};
        (void)gzip_writer_write(z, none, NULL);
        if (BURROW_FAILED(z->err))
            return z->err;
    }
    z->err = flate_writer_flush(z->compressor);
    return z->err;
}

Error gzip_writer_close(GzipWriter *z) {
    if (BURROW_FAILED(z->err))
        return z->err;
    if (z->closed)
        return BURROW_NO_ERROR;
    z->closed = true;
    if (!z->wrote_header) {
        Slice none = {NULL, 0, 0, NULL};
        (void)gzip_writer_write(z, none, NULL);
        if (BURROW_FAILED(z->err))
            return z->err;
    }
    z->err = flate_writer_close(z->compressor);
    if (BURROW_FAILED(z->err))
        return z->err;
    gzip_put_le32(z->buf, z->digest);
    gzip_put_le32(z->buf + 4, z->size);
    z->err = gzip_write_all(z, gzip_bytes(z->buf, 8));
    return z->err;
}

void gzip_writer_free(GzipWriter *z) {
    if (z == NULL)
        return;
    flate_writer_free(z->compressor);
    mem_free(z->a, z, sizeof *z, _Alignof(GzipWriter));
}

static Int gzip_vt_write(void *self, Slice p, Error *err) {
    return gzip_writer_write((GzipWriter *)self, p, err);
}

static Error gzip_vt_write_close(void *self) {
    return gzip_writer_close((GzipWriter *)self);
}

static const IoWriteCloserVT gzip_write_closer_vt = {
    {&gzip_writer_desc, gzip_vt_write},
    {&gzip_writer_desc, gzip_vt_write_close},
};

IoWriter gzip_writer_as_io_writer(GzipWriter *z) {
    return (IoWriter){&gzip_write_closer_vt.writer, z};
}

IoWriteCloser gzip_writer_as_io_write_closer(GzipWriter *z) {
    return (IoWriteCloser){&gzip_write_closer_vt, z};
}

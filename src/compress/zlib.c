/* compress/zlib, from reader.go and writer.go.
 *
 * Both halves are thin: the header is two bytes, maybe followed by the Adler-32
 * of a preset dictionary, and the trailer is the Adler-32 of the data. The
 * DEFLATE in between is compress/flate's, and the running checksum is the
 * update function from hash/adler32 with no Hash32 around it.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/compress/zlib.h"

#include "burrow/bufio.h"
#include "burrow/compress/flate.h"

#include "../hash/internal.h"

#include <string.h>

BURROW_SENTINEL_ERROR(zlib_err_checksum, "zlib: invalid checksum");
BURROW_SENTINEL_ERROR(zlib_err_dictionary, "zlib: invalid dictionary");
BURROW_SENTINEL_ERROR(zlib_err_header, "zlib: invalid header");

enum {
    ZLIB_DEFLATE = 8,
    ZLIB_MAX_WINDOW = 7,
};

static inline bool zlib_same_error(Error a, Error b) {
    return a.vt == b.vt && a.data == b.data;
}

static inline uint32_t zlib_be32(const Byte *b) {
    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 |
           (uint32_t)b[3];
}

static inline void zlib_put_be32(Byte *b, uint32_t v) {
    b[0] = (Byte)(v >> 24);
    b[1] = (Byte)(v >> 16);
    b[2] = (Byte)(v >> 8);
    b[3] = (Byte)v;
}

static inline Slice zlib_bytes(Byte *p, Int n) {
    return slice_from(p, n, n, TYPE_BYTE);
}

/* ---------------------------------------------------------------- reading */

typedef struct ZlibReader {
    Alloc *a;
    /* The source, with ReadByte: r itself when it has one, else rbuf. */
    IoReader r;
    BufioReader *rbuf; /* created if r has no ReadByte, and kept for reuse */
    IoReadCloser decompressor;
    uint32_t digest;
    Error err;
    Byte scratch[4];
} ZlibReader;

static const Type zlib_reader_desc = {
    {(const Byte *)"reader", 6},
    {(const Byte *)"compress/zlib", 13},
    KIND_STRUCT,
    (uint32_t)sizeof(ZlibReader),
    (uint16_t)_Alignof(ZlibReader),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x7a6c7264U, /* "zlrd" */
    NULL,
};

static Int zlib_read(void *self, Slice p, Error *err) {
    ZlibReader *z = (ZlibReader *)self;
    if (BURROW_FAILED(z->err)) {
        BURROW_OUT(err, z->err);
        return 0;
    }

    Error e = BURROW_NO_ERROR;
    Int n = z->decompressor.vt->reader.read(z->decompressor.data, p, &e);
    z->err = e;
    z->digest = burrow__adler32_update(z->digest, (const Byte *)p.p, n);
    if (!zlib_same_error(e, io_eof)) {
        BURROW_OUT(err, e);
        return n;
    }

    /* Finished file; check checksum. */
    Error re = BURROW_NO_ERROR;
    io_read_full(z->r, zlib_bytes(z->scratch, 4), &re);
    if (BURROW_FAILED(re)) {
        if (zlib_same_error(re, io_eof))
            re = io_err_unexpected_eof;
        z->err = re;
        BURROW_OUT(err, re);
        return n;
    }
    if (zlib_be32(z->scratch) != z->digest) {
        z->err = zlib_err_checksum;
        BURROW_OUT(err, zlib_err_checksum);
        return n;
    }
    BURROW_OUT(err, io_eof);
    return n;
}

/* Calling Close does not close the wrapped io.Reader originally passed to
 * zlib_new_reader. In order for the ZLIB checksum to be verified, the reader
 * must be fully consumed until the io_eof. */
static Error zlib_close(void *self) {
    ZlibReader *z = (ZlibReader *)self;
    if (BURROW_FAILED(z->err) && !zlib_same_error(z->err, io_eof))
        return z->err;
    z->err = z->decompressor.vt->closer.close(z->decompressor.data);
    return z->err;
}

/* Reads n bytes of header into scratch, setting z->err on failure. */
static bool zlib_read_header(ZlibReader *z, Int n) {
    Error e = BURROW_NO_ERROR;
    io_read_full(z->r, zlib_bytes(z->scratch, n), &e);
    if (zlib_same_error(e, io_eof))
        e = io_err_unexpected_eof;
    z->err = e;
    return BURROW_OK(e);
}

static Error zlib_reset(void *self, IoReader r, Slice dict) {
    ZlibReader *z = (ZlibReader *)self;
    z->err = BURROW_NO_ERROR;
    z->digest = 1;
    memset(z->scratch, 0, sizeof z->scratch);
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

    /* Read the header (RFC 1950 section 2.2.). */
    if (!zlib_read_header(z, 2))
        return z->err;
    unsigned h = (unsigned)z->scratch[0] << 8 | z->scratch[1];
    if ((z->scratch[0] & 0x0f) != ZLIB_DEFLATE ||
        z->scratch[0] >> 4 > ZLIB_MAX_WINDOW || h % 31 != 0) {
        z->err = zlib_err_header;
        return z->err;
    }
    bool have_dict = (z->scratch[1] & 0x20) != 0;
    if (have_dict) {
        if (!zlib_read_header(z, 4))
            return z->err;
        if (zlib_be32(z->scratch) !=
            burrow__adler32_update(1, (const Byte *)dict.p, dict.len)) {
            z->err = zlib_err_dictionary;
            return z->err;
        }
    }

    if (z->decompressor.vt == NULL) {
        Slice none = {NULL, 0, 0, NULL};
        z->decompressor = flate_new_reader_dict(z->a, z->r, have_dict ? dict : none);
        if (z->decompressor.vt == NULL) {
            z->err = burrow_err_out_of_memory;
            return z->err;
        }
    } else {
        /* As in Go, a reused decompressor gets dict whether or not the header
         * asked for one. */
        Error e =
            flate_resetter_reset(flate_reader_as_resetter(z->decompressor), z->r, dict);
        if (BURROW_FAILED(e)) {
            z->err = e;
            return e;
        }
    }
    z->digest = 1;
    return BURROW_NO_ERROR;
}

static const IoReadCloserVT zlib_read_closer_vt = {
    {&zlib_reader_desc, zlib_read},
    {&zlib_reader_desc, zlib_close},
};

static const ZlibResetterVT zlib_resetter_vt = {&zlib_reader_desc, zlib_reset};

static void zlib_reader_destroy(ZlibReader *z) {
    flate_reader_free(z->decompressor);
    bufio_reader_free(z->rbuf);
    mem_free(z->a, z, sizeof *z, _Alignof(ZlibReader));
}

IoReadCloser zlib_new_reader_dict(Alloc *a, IoReader r, Slice dict, Error *err) {
    IoReadCloser nil = {NULL, NULL};
    ZlibReader *z = (ZlibReader *)mem_alloc(a, sizeof *z, _Alignof(ZlibReader));
    if (z == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return nil;
    }
    z->a = a;
    Error e = zlib_reset(z, r, dict);
    if (BURROW_FAILED(e)) {
        zlib_reader_destroy(z);
        BURROW_OUT(err, e);
        return nil;
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return (IoReadCloser){&zlib_read_closer_vt, z};
}

IoReadCloser zlib_new_reader(Alloc *a, IoReader r, Error *err) {
    Slice none = {NULL, 0, 0, NULL};
    return zlib_new_reader_dict(a, r, none, err);
}

static bool zlib_is_ours(IoReadCloser rc) {
    return rc.vt != NULL && rc.data != NULL &&
           rc.vt->reader.self_type == &zlib_reader_desc;
}

ZlibResetter zlib_reader_as_resetter(IoReadCloser rc) {
    ZlibResetter rs = {NULL, NULL};
    if (zlib_is_ours(rc)) {
        rs.vt = &zlib_resetter_vt;
        rs.data = rc.data;
    }
    return rs;
}

void zlib_reader_free(IoReadCloser rc) {
    if (zlib_is_ours(rc))
        zlib_reader_destroy((ZlibReader *)rc.data);
}

/* ---------------------------------------------------------------- writing */

struct ZlibWriter {
    Alloc *a;
    IoWriter w;
    Int level;
    Byte *dict; /* the copy, NULL when dict_len is 0 */
    Int dict_len;
    bool have_dict;
    FlateWriter *compressor;
    uint32_t digest;
    Error err;
    Byte scratch[4];
    bool wrote_header;
};

static const Type zlib_writer_desc = {
    {(const Byte *)"Writer", 6},
    {(const Byte *)"compress/zlib", 13},
    KIND_STRUCT,
    (uint32_t)sizeof(ZlibWriter),
    (uint16_t)_Alignof(ZlibWriter),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x7a6c7772U, /* "zlwr" */
    NULL,
};

const Type *const TYPE_ZLIB_WRITER = &zlib_writer_desc;

/* fmt.Errorf("zlib: invalid compression level: %d", level). */
static Error zlib_level_error(Int level) {
    static const char head[] = "zlib: invalid compression level: ";
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

ZlibWriter *zlib_new_writer_level_dict(Alloc *a, IoWriter w, Int level, Slice dict,
                                       Error *err) {
    if (level < ZLIB_HUFFMAN_ONLY || level > ZLIB_BEST_COMPRESSION) {
        BURROW_OUT(err, zlib_level_error(level));
        return NULL;
    }
    ZlibWriter *z = (ZlibWriter *)mem_alloc(a, sizeof *z, _Alignof(ZlibWriter));
    if (z == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    z->a = a;
    z->w = w;
    z->level = level;
    z->have_dict = dict.p != NULL;
    if (dict.len > 0) {
        z->dict = (Byte *)mem_alloc_nozero(a, (size_t)dict.len, 1);
        if (z->dict == NULL) {
            mem_free(a, z, sizeof *z, _Alignof(ZlibWriter));
            BURROW_OUT(err, burrow_err_out_of_memory);
            return NULL;
        }
        memcpy(z->dict, dict.p, (size_t)dict.len);
        z->dict_len = dict.len;
    }
    z->digest = 1;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return z;
}

ZlibWriter *zlib_new_writer_level(Alloc *a, IoWriter w, Int level, Error *err) {
    Slice none = {NULL, 0, 0, NULL};
    return zlib_new_writer_level_dict(a, w, level, none, err);
}

ZlibWriter *zlib_new_writer(Alloc *a, IoWriter w) {
    Slice none = {NULL, 0, 0, NULL};
    return zlib_new_writer_level_dict(a, w, ZLIB_DEFAULT_COMPRESSION, none, NULL);
}

void zlib_writer_reset(ZlibWriter *z, IoWriter w) {
    z->w = w;
    if (z->compressor != NULL)
        flate_writer_reset(z->compressor, w);
    z->digest = 1;
    z->err = BURROW_NO_ERROR;
    memset(z->scratch, 0, sizeof z->scratch);
    z->wrote_header = false;
}

static Slice zlib_dict(const ZlibWriter *z) {
    if (!z->have_dict) {
        Slice none = {NULL, 0, 0, NULL};
        return none;
    }
    return slice_from(z->dict, z->dict_len, z->dict_len, TYPE_BYTE);
}

/* Writes the zlib header. */
static Error zlib_write_header(ZlibWriter *z) {
    z->wrote_header = true;
    /* ZLIB has a two-byte header (as documented in RFC 1950). The first four
     * bits is the CINFO (compression info), which is 7 for the default
     * compression window size. The next four bits is the CM (compression
     * method), which is 8 for deflate. */
    z->scratch[0] = 0x78;
    /* The next two bits is the FLEVEL (compression level). The four values
     * are: 0=fastest, 1=fast, 2=default, 3=best. The next bit, FDICT, is set
     * if a dictionary is given. The final five FCHECK bits form a mod-31
     * checksum. */
    switch (z->level) {
    case -2:
    case 0:
    case 1:
        z->scratch[1] = 0 << 6;
        break;
    case 2:
    case 3:
    case 4:
    case 5:
        z->scratch[1] = 1 << 6;
        break;
    case 6:
    case -1:
        z->scratch[1] = 2 << 6;
        break;
    default: /* 7, 8 and 9, the constructors let nothing else in */
        z->scratch[1] = 3 << 6;
        break;
    }
    if (z->have_dict)
        z->scratch[1] |= 1 << 5;
    unsigned h = (unsigned)z->scratch[0] << 8 | z->scratch[1];
    z->scratch[1] = (Byte)(z->scratch[1] + (31 - h % 31));

    Error e = BURROW_NO_ERROR;
    z->w.vt->write(z->w.data, zlib_bytes(z->scratch, 2), &e);
    if (BURROW_FAILED(e))
        return e;
    if (z->have_dict) {
        /* The next four bytes are the Adler-32 checksum of the dictionary. */
        zlib_put_be32(z->scratch, burrow__adler32_update(1, z->dict, z->dict_len));
        z->w.vt->write(z->w.data, zlib_bytes(z->scratch, 4), &e);
        if (BURROW_FAILED(e))
            return e;
    }
    if (z->compressor == NULL) {
        /* Initialize deflater unless the Writer is being reused after a Reset
         * call. */
        z->compressor = flate_new_writer_dict(z->a, z->w, z->level, zlib_dict(z), &e);
        if (z->compressor == NULL)
            return e;
        z->digest = 1;
    }
    return BURROW_NO_ERROR;
}

Int zlib_writer_write(ZlibWriter *z, Slice data, Error *err) {
    if (!z->wrote_header)
        z->err = zlib_write_header(z);
    if (BURROW_FAILED(z->err)) {
        BURROW_OUT(err, z->err);
        return 0;
    }
    if (data.len == 0) {
        BURROW_OUT(err, BURROW_NO_ERROR);
        return 0;
    }
    Error e = BURROW_NO_ERROR;
    Int n = flate_writer_write(z->compressor, data, &e);
    if (BURROW_FAILED(e)) {
        z->err = e;
        BURROW_OUT(err, e);
        return n;
    }
    z->digest = burrow__adler32_update(z->digest, (const Byte *)data.p, data.len);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return n;
}

Error zlib_writer_flush(ZlibWriter *z) {
    if (!z->wrote_header)
        z->err = zlib_write_header(z);
    if (BURROW_FAILED(z->err))
        return z->err;
    z->err = flate_writer_flush(z->compressor);
    return z->err;
}

Error zlib_writer_close(ZlibWriter *z) {
    if (!z->wrote_header)
        z->err = zlib_write_header(z);
    if (BURROW_FAILED(z->err))
        return z->err;
    z->err = flate_writer_close(z->compressor);
    if (BURROW_FAILED(z->err))
        return z->err;
    zlib_put_be32(z->scratch, z->digest);
    Error e = BURROW_NO_ERROR;
    z->w.vt->write(z->w.data, zlib_bytes(z->scratch, 4), &e);
    z->err = e;
    return e;
}

void zlib_writer_free(ZlibWriter *z) {
    if (z == NULL)
        return;
    flate_writer_free(z->compressor);
    if (z->dict != NULL)
        mem_free(z->a, z->dict, (size_t)z->dict_len, 1);
    mem_free(z->a, z, sizeof *z, _Alignof(ZlibWriter));
}

static Int zlib_vt_write(void *self, Slice p, Error *err) {
    return zlib_writer_write((ZlibWriter *)self, p, err);
}

static Error zlib_vt_close(void *self) {
    return zlib_writer_close((ZlibWriter *)self);
}

static const IoWriteCloserVT zlib_write_closer_vt = {
    {&zlib_writer_desc, zlib_vt_write},
    {&zlib_writer_desc, zlib_vt_close},
};

IoWriter zlib_writer_as_io_writer(ZlibWriter *z) {
    return (IoWriter){&zlib_write_closer_vt.writer, z};
}

IoWriteCloser zlib_writer_as_io_write_closer(ZlibWriter *z) {
    return (IoWriteCloser){&zlib_write_closer_vt, z};
}

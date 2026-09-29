/* image/png, from reader.go, writer.go and paeth.go.
 *
 * The decoder is Go's: the chunks are read in order through a running CRC,
 * and the IDAT chunks, however many there are, are joined into one stream by
 * a reader that the zlib reader reads from. Each row is unfiltered against
 * the one before it and then unpacked into the image, and an interlaced file
 * is seven small images merged into a full sized one.
 *
 * The encoder is Go's as well, down to the heuristic that picks each row's
 * filter, so the bytes it writes are the bytes Go writes.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/image/png.h"

#include "burrow/bufio.h"
#include "burrow/compress/flate.h"
#include "burrow/compress/zlib.h"
#include "burrow/hash/crc32.h"
#include "burrow/runtime.h"
#include "burrow/sync.h"

#include <string.h>

/* ------------------------------------------------------------------ errors */

typedef struct PngErrorBox {
    Str s;
    Str message;
} PngErrorBox;

#define PNG_ERROR_TYPE(desc, name, tag)                                                \
    static const Type desc = {                                                         \
        {(const Byte *)name, sizeof name - 1},                                         \
        {(const Byte *)"image/png", 9},                                                \
        KIND_STRING,                                                                   \
        (uint32_t)sizeof(Str),                                                         \
        (uint16_t)_Alignof(Str),                                                       \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
        NULL,                                                                          \
        NULL,                                                                          \
        NULL,                                                                          \
        0,                                                                             \
        tag,                                                                           \
        NULL,                                                                          \
    }

PNG_ERROR_TYPE(png_format_desc, "FormatError", 0x706e6665U);           /* "pnfe" */
PNG_ERROR_TYPE(png_unsupported_desc, "UnsupportedError", 0x706e7565U); /* "pnue" */

const Type *const TYPE_PNG_FORMAT_ERROR = &png_format_desc;
const Type *const TYPE_PNG_UNSUPPORTED_ERROR = &png_unsupported_desc;

#define PNG_FORMAT_PREFIX "png: invalid format: "
#define PNG_UNSUPPORTED_PREFIX "png: unsupported feature: "

static Str png_error_message(const void *self) {
    return ((const PngErrorBox *)self)->message;
}

static bool png_format_is(const void *self, Error target);
static bool png_unsupported_is(const void *self, Error target);
static Error png_format_clone(const void *self, Alloc *a);
static Error png_unsupported_clone(const void *self, Alloc *a);

static const ErrorVT png_format_vt = {
    .self_type = &png_format_desc,
    .message = png_error_message,
    .is = png_format_is,
    .clone = png_format_clone,
};

static const ErrorVT png_unsupported_vt = {
    .self_type = &png_unsupported_desc,
    .message = png_error_message,
    .is = png_unsupported_is,
    .clone = png_unsupported_clone,
};

static bool png_same_text(const void *self, Error target) {
    Str a = ((const PngErrorBox *)self)->s;
    Str b = ((const PngErrorBox *)target.data)->s;
    return a.len == b.len && (a.len == 0 || memcmp(a.p, b.p, (size_t)a.len) == 0);
}

static bool png_format_is(const void *self, Error target) {
    return target.vt == &png_format_vt && target.data != NULL &&
           png_same_text(self, target);
}

static bool png_unsupported_is(const void *self, Error target) {
    return target.vt == &png_unsupported_vt && target.data != NULL &&
           png_same_text(self, target);
}

/* The prefix and e in one allocation, with the box in front of them. */
static Error png_error_box(const ErrorVT *vt, const char *prefix, Str e, Alloc *a) {
    Int plen = (Int)strlen(prefix);
    Int mlen = plen + e.len;
    PngErrorBox *b = (PngErrorBox *)mem_alloc_nozero(
        a, sizeof(PngErrorBox) + (size_t)mlen, _Alignof(PngErrorBox));
    if (b == NULL)
        return burrow_err_out_of_memory;
    Byte *p = (Byte *)(b + 1);
    memcpy(p, prefix, (size_t)plen);
    if (e.len > 0)
        memcpy(p + plen, e.p, (size_t)e.len);
    b->message = str_from_bytes(p, mlen);
    b->s = str_from_bytes(p + plen, e.len);
    return (Error){vt, b};
}

static Str png_error_text(const char *prefix, Str e, Alloc *a) {
    Int plen = (Int)strlen(prefix);
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)(plen + e.len), 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    memcpy(p, prefix, (size_t)plen);
    if (e.len > 0)
        memcpy(p + plen, e.p, (size_t)e.len);
    return str_from_bytes(p, plen + e.len);
}

Error png_format_error_as_error(PngFormatError e, Alloc *a) {
    return png_error_box(&png_format_vt, PNG_FORMAT_PREFIX, e, a);
}

Error png_unsupported_error_as_error(PngUnsupportedError e, Alloc *a) {
    return png_error_box(&png_unsupported_vt, PNG_UNSUPPORTED_PREFIX, e, a);
}

static Error png_format_clone(const void *self, Alloc *a) {
    return png_format_error_as_error(((const PngErrorBox *)self)->s, a);
}

static Error png_unsupported_clone(const void *self, Alloc *a) {
    return png_unsupported_error_as_error(((const PngErrorBox *)self)->s, a);
}

Str png_format_error_error(PngFormatError e, Alloc *a) {
    return png_error_text(PNG_FORMAT_PREFIX, e, a);
}

Str png_unsupported_error_error(PngUnsupportedError e, Alloc *a) {
    return png_error_text(PNG_UNSUPPORTED_PREFIX, e, a);
}

/* The errors with fixed text, in read only memory. text is pasted onto the
 * prefix, so it cannot have parentheses around it. */
/* NOLINTBEGIN(bugprone-macro-parentheses) */
#define PNG_ERROR(name, vt, prefix, text)                                              \
    static const PngErrorBox name##__box = {                                           \
        {(const Byte *)prefix text + sizeof prefix - 1, sizeof text - 1},              \
        {(const Byte *)prefix text, sizeof prefix text - 1}};                          \
    static const Error name = {&vt, &name##__box}
#define PNG_FORMAT(name, text) PNG_ERROR(name, png_format_vt, PNG_FORMAT_PREFIX, text)
#define PNG_UNSUPPORTED(name, text)                                                    \
    PNG_ERROR(name, png_unsupported_vt, PNG_UNSUPPORTED_PREFIX, text)
/* NOLINTEND(bugprone-macro-parentheses) */

PNG_FORMAT(png_err_chunk_order, "chunk out of order");
PNG_FORMAT(png_err_ihdr_length, "bad IHDR length");
PNG_FORMAT(png_err_interlace, "invalid interlace method");
PNG_FORMAT(png_err_dimension, "non-positive dimension");
PNG_FORMAT(png_err_plte_length, "bad PLTE length");
PNG_FORMAT(png_err_plte_type, "PLTE, color type mismatch");
PNG_FORMAT(png_err_trns_length, "bad tRNS length");
PNG_FORMAT(png_err_trns_type, "tRNS, color type mismatch");
PNG_FORMAT(png_err_not_enough, "not enough pixel data");
PNG_FORMAT(png_err_too_much, "too much pixel data");
PNG_FORMAT(png_err_filter, "bad filter type");
PNG_FORMAT(png_err_iend_length, "bad IEND length");
PNG_FORMAT(png_err_checksum, "invalid checksum");
PNG_FORMAT(png_err_not_png, "not a PNG file");
PNG_UNSUPPORTED(png_err_compression, "compression method");
PNG_UNSUPPORTED(png_err_filter_method, "filter method");
PNG_UNSUPPORTED(png_err_overflow, "dimension overflow");
#if BURROW_PTR_BITS != 64
PNG_UNSUPPORTED(png_err_idat_overflow, "IDAT chunk length overflow");
#endif

/* A FormatError or UnsupportedError whose text is built on the spot, in the
 * calling goroutine's error arena. */
static Error png_format_str(Str s) {
    return png_format_error_as_error(s, error_allocator());
}

static Error png_unsupported_str(Str s) {
    return png_unsupported_error_as_error(s, error_allocator());
}

/* Appends s to the buffer at *n, which has room for it. */
static void png_cat(Byte *buf, Int *n, const char *s) {
    size_t len = strlen(s);
    memcpy(buf + *n, s, len);
    *n += (Int)len;
}

static void png_cat_int(Byte *buf, Int *n, int64_t v) {
    Byte tmp[24];
    int i = (int)sizeof tmp;
    uint64_t u = v < 0 ? 0 - (uint64_t)v : (uint64_t)v;
    do {
        tmp[--i] = (Byte)('0' + u % 10);
        u /= 10;
    } while (u != 0);
    if (v < 0)
        tmp[--i] = '-';
    memcpy(buf + *n, tmp + i, sizeof tmp - (size_t)i);
    *n += (Int)(sizeof tmp - (size_t)i);
}

/* ------------------------------------------------------------- constants */

enum {
    PNG_CT_GRAYSCALE = 0,
    PNG_CT_TRUE_COLOR = 2,
    PNG_CT_PALETTED = 3,
    PNG_CT_GRAYSCALE_ALPHA = 4,
    PNG_CT_TRUE_COLOR_ALPHA = 6
};

/* A cb is a combination of color type and bit depth. */
enum {
    CB_INVALID,
    CB_G1,
    CB_G2,
    CB_G4,
    CB_G8,
    CB_GA8,
    CB_TC8,
    CB_P1,
    CB_P2,
    CB_P4,
    CB_P8,
    CB_TCA8,
    CB_G16,
    CB_GA16,
    CB_TC16,
    CB_TCA16
};

static bool png_cb_paletted(int cb) {
    return CB_P1 <= cb && cb <= CB_P8;
}

static bool png_cb_true_color(int cb) {
    return cb == CB_TC8 || cb == CB_TC16;
}

enum { FT_NONE, FT_SUB, FT_UP, FT_AVERAGE, FT_PAETH, N_FILTER };

enum { IT_NONE, IT_ADAM7 };

typedef struct PngInterlaceScan {
    int x_factor, y_factor, x_offset, y_offset;
} PngInterlaceScan;

static const PngInterlaceScan png_interlacing[7] = {
    {8, 8, 0, 0}, {8, 8, 4, 0}, {4, 8, 0, 4}, {4, 4, 2, 0},
    {2, 4, 0, 2}, {2, 2, 1, 0}, {1, 2, 0, 1},
};

/* The decoder's stage, which is how far through the chunks it has got. */
enum { DS_START, DS_SEEN_IHDR, DS_SEEN_PLTE, DS_SEEN_TRNS, DS_SEEN_IDAT, DS_SEEN_IEND };

#define PNG_HEADER "\x89PNG\r\n\x1a\n"
#define PNG_HEADER_LEN 8

static uint32_t png_be32(const Byte *b) {
    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 |
           (uint32_t)b[3];
}

static void png_put_be32(Byte *b, uint32_t v) {
    b[0] = (Byte)(v >> 24);
    b[1] = (Byte)(v >> 16);
    b[2] = (Byte)(v >> 8);
    b[3] = (Byte)v;
}

/* Go's err == io.EOF, which is identity rather than errors.Is. */
static bool png_is_eof(Error err) {
    return err.vt == io_eof.vt && err.data == io_eof.data;
}

static int png_abs(int x) {
    return x < 0 ? -x : x;
}

/* paeth: the Paeth predictor of a, b and c. */
static uint8_t png_paeth(uint8_t a, uint8_t b, uint8_t c) {
    int pc = c;
    int pa = (int)b - pc;
    int pb = (int)a - pc;
    pc = png_abs(pa + pb);
    pa = png_abs(pa);
    pb = png_abs(pb);
    if (pa <= pb && pa <= pc)
        return a;
    if (pb <= pc)
        return b;
    return c;
}

/* filterPaeth: undoes the Paeth filter on cdat, n bytes, against pdat. */
static void png_filter_paeth(Byte *cdat, const Byte *pdat, Int n, Int bpp) {
    for (Int i = 0; i < bpp; i++) {
        int a = 0, c = 0;
        for (Int j = i; j < n; j += bpp) {
            int b = pdat[j];
            int pa = b - c;
            int pb = a - c;
            int pc = png_abs(pa + pb);
            pa = png_abs(pa);
            pb = png_abs(pb);
            if (pa <= pb && pa <= pc) {
                /* a stays */
            } else if (pb <= pc) {
                a = b;
            } else {
                a = c;
            }
            a += cdat[j];
            a &= 0xff;
            cdat[j] = (Byte)a;
            c = b;
        }
    }
}

/* ---------------------------------------------------------------- decoder */

typedef struct PngDecoder {
    Alloc *a;
    IoReader r;
    Image img;
    uint32_t crc;
    Int width, height;
    int depth;
    /* The palette, whose array has room for 256 colors so that the decoder
     * can lengthen it the way Go reslices one, or a nil slice. */
    ColorPalette palette;
    int cb;
    int stage;
    uint32_t idat_length;
    Byte tmp[3 * 256];
    int interlace;
    bool use_transparent;
    Byte transparent[6];
} PngDecoder;

static void png_crc_write(PngDecoder *d, const Byte *p, Int n) {
    d->crc = crc32_update(d->crc, crc32_ieee_table,
                          slice_from((void *)(uintptr_t)p, n, n, TYPE_BYTE));
}

static Error png_read_full(PngDecoder *d, Byte *p, Int n) {
    Error err = BURROW_NO_ERROR;
    io_read_full(d->r, slice_from(p, n, n, TYPE_BYTE), &err);
    return err;
}

static Error png_verify_checksum(PngDecoder *d) {
    Error err = png_read_full(d, d->tmp, 4);
    if (BURROW_FAILED(err))
        return err;
    if (png_be32(d->tmp) != d->crc)
        return png_err_checksum;
    return BURROW_NO_ERROR;
}

static Error png_parse_ihdr(PngDecoder *d, uint32_t length) {
    if (length != 13)
        return png_err_ihdr_length;
    Error err = png_read_full(d, d->tmp, 13);
    if (BURROW_FAILED(err))
        return err;
    png_crc_write(d, d->tmp, 13);
    if (d->tmp[10] != 0)
        return png_err_compression;
    if (d->tmp[11] != 0)
        return png_err_filter_method;
    if (d->tmp[12] != IT_NONE && d->tmp[12] != IT_ADAM7)
        return png_err_interlace;
    d->interlace = d->tmp[12];

    int32_t w = (int32_t)png_be32(d->tmp);
    int32_t h = (int32_t)png_be32(d->tmp + 4);
    if (w <= 0 || h <= 0)
        return png_err_dimension;
    int64_t n_pixels64 = (int64_t)w * (int64_t)h;
    if (n_pixels64 > BURROW_INT_MAX / 8)
        return png_err_overflow;

    d->cb = CB_INVALID;
    d->depth = d->tmp[8];
    Byte ct = d->tmp[9];
    switch (d->depth) {
    case 1:
        d->cb = ct == PNG_CT_GRAYSCALE  ? CB_G1
                : ct == PNG_CT_PALETTED ? CB_P1
                                        : CB_INVALID;
        break;
    case 2:
        d->cb = ct == PNG_CT_GRAYSCALE  ? CB_G2
                : ct == PNG_CT_PALETTED ? CB_P2
                                        : CB_INVALID;
        break;
    case 4:
        d->cb = ct == PNG_CT_GRAYSCALE  ? CB_G4
                : ct == PNG_CT_PALETTED ? CB_P4
                                        : CB_INVALID;
        break;
    case 8:
        switch (ct) {
        case PNG_CT_GRAYSCALE:
            d->cb = CB_G8;
            break;
        case PNG_CT_TRUE_COLOR:
            d->cb = CB_TC8;
            break;
        case PNG_CT_PALETTED:
            d->cb = CB_P8;
            break;
        case PNG_CT_GRAYSCALE_ALPHA:
            d->cb = CB_GA8;
            break;
        case PNG_CT_TRUE_COLOR_ALPHA:
            d->cb = CB_TCA8;
            break;
        default:
            break;
        }
        break;
    case 16:
        switch (ct) {
        case PNG_CT_GRAYSCALE:
            d->cb = CB_G16;
            break;
        case PNG_CT_TRUE_COLOR:
            d->cb = CB_TC16;
            break;
        case PNG_CT_GRAYSCALE_ALPHA:
            d->cb = CB_GA16;
            break;
        case PNG_CT_TRUE_COLOR_ALPHA:
            d->cb = CB_TCA16;
            break;
        default:
            break;
        }
        break;
    default:
        break;
    }
    if (d->cb == CB_INVALID) {
        Byte buf[64];
        Int n = 0;
        png_cat(buf, &n, "bit depth ");
        png_cat_int(buf, &n, d->tmp[8]);
        png_cat(buf, &n, ", color type ");
        png_cat_int(buf, &n, d->tmp[9]);
        return png_unsupported_str(str_from_bytes(buf, n));
    }
    d->width = w;
    d->height = h;
    return png_verify_checksum(d);
}

static Color *png_palette_at(ColorPalette p, Int i) {
    return (Color *)p.p + i;
}

static Error png_parse_plte(PngDecoder *d, uint32_t length) {
    Int np = (Int)(length / 3);
    if (length % 3 != 0 || np <= 0 || np > 256 || np > (Int)1 << d->depth)
        return png_err_plte_length;
    Error err = png_read_full(d, d->tmp, 3 * np);
    if (BURROW_FAILED(err))
        return err;
    png_crc_write(d, d->tmp, 3 * np);
    switch (d->cb) {
    case CB_P1:
    case CB_P2:
    case CB_P4:
    case CB_P8: {
        Color *c = (Color *)mem_alloc(d->a, 256 * sizeof(Color), _Alignof(Color));
        if (c == NULL)
            return burrow_err_out_of_memory;
        for (Int i = 0; i < np; i++)
            c[i] = color_rgba_as_color((ColorRGBA){d->tmp[3 * i + 0], d->tmp[3 * i + 1],
                                                   d->tmp[3 * i + 2], 0xff});
        for (Int i = np; i < 256; i++)
            c[i] = color_rgba_as_color((ColorRGBA){0x00, 0x00, 0x00, 0xff});
        d->palette = slice_from(c, np, 256, TYPE_OF(Color));
        break;
    }
    case CB_TC8:
    case CB_TCA8:
    case CB_TC16:
    case CB_TCA16:
        /* A PLTE chunk is optional here, a suggested palette for a display
         * that cannot show truecolor, and is ignored. */
        break;
    default:
        return png_err_plte_type;
    }
    return png_verify_checksum(d);
}

static Error png_parse_trns(PngDecoder *d, uint32_t length) {
    Error err;
    switch (d->cb) {
    case CB_G1:
    case CB_G2:
    case CB_G4:
    case CB_G8:
    case CB_G16:
        if (length != 2)
            return png_err_trns_length;
        err = png_read_full(d, d->tmp, 2);
        if (BURROW_FAILED(err))
            return err;
        png_crc_write(d, d->tmp, 2);
        memcpy(d->transparent, d->tmp, 2);
        switch (d->cb) {
        case CB_G1:
            d->transparent[1] = (Byte)(d->transparent[1] * 0xff);
            break;
        case CB_G2:
            d->transparent[1] = (Byte)(d->transparent[1] * 0x55);
            break;
        case CB_G4:
            d->transparent[1] = (Byte)(d->transparent[1] * 0x11);
            break;
        default:
            break;
        }
        d->use_transparent = true;
        break;
    case CB_TC8:
    case CB_TC16:
        if (length != 6)
            return png_err_trns_length;
        err = png_read_full(d, d->tmp, 6);
        if (BURROW_FAILED(err))
            return err;
        png_crc_write(d, d->tmp, 6);
        memcpy(d->transparent, d->tmp, 6);
        d->use_transparent = true;
        break;
    case CB_P1:
    case CB_P2:
    case CB_P4:
    case CB_P8: {
        if (length > 256)
            return png_err_trns_length;
        Int n = (Int)length;
        err = png_read_full(d, d->tmp, n);
        if (BURROW_FAILED(err))
            return err;
        png_crc_write(d, d->tmp, n);
        if (d->palette.len < n)
            d->palette = slice_sub(d->palette, 0, n);
        for (Int i = 0; i < n; i++) {
            ColorRGBA c = png_palette_at(d->palette, i)->data.rgba;
            *png_palette_at(d->palette, i) =
                color_nrgba_as_color((ColorNRGBA){c.r, c.g, c.b, d->tmp[i]});
        }
        break;
    }
    default:
        return png_err_trns_type;
    }
    return png_verify_checksum(d);
}

/* Read: the pixel data, read out of as many IDAT chunks as it takes. The
 * decoder is the reader the zlib reader reads. */
static Int png_idat_read(void *self, Slice p, Error *err) {
    PngDecoder *d = (PngDecoder *)self;
    if (p.len == 0)
        return 0;
    while (d->idat_length == 0) {
        /* The end of one IDAT chunk: check it, and go on to the next. */
        Error e = png_verify_checksum(d);
        if (BURROW_OK(e))
            e = png_read_full(d, d->tmp, 8);
        if (BURROW_FAILED(e)) {
            BURROW_OUT(err, e);
            return 0;
        }
        d->idat_length = png_be32(d->tmp);
        if (memcmp(d->tmp + 4, "IDAT", 4) != 0) {
            BURROW_OUT(err, png_err_not_enough);
            return 0;
        }
        d->crc = 0;
        png_crc_write(d, d->tmp + 4, 4);
    }
#if BURROW_PTR_BITS != 64
    /* Go's int(d.idatLength) < 0, which only a 32-bit int can hit. */
    if (d->idat_length > (uint32_t)BURROW_INT_MAX) {
        BURROW_OUT(err, png_err_idat_overflow);
        return 0;
    }
#endif
    Int want = p.len < (Int)d->idat_length ? p.len : (Int)d->idat_length;
    Error e = BURROW_NO_ERROR;
    Int n = d->r.vt->read(d->r.data, slice_sub(p, 0, want), &e);
    png_crc_write(d, (const Byte *)p.p, n);
    d->idat_length -= (uint32_t)n;
    BURROW_OUT(err, e);
    return n;
}

static const IoReaderVT png_idat_reader_vt = {NULL, png_idat_read};

/* The pixels of a decoded image, whichever type it is, and its layout. */
typedef struct PngPix {
    Byte *pix;
    Int stride;
    Int bpp;
} PngPix;

static PngPix png_pix_of(Image m) {
    PngPix p = {NULL, 0, 0};
    const Type *t = m.vt->self_type;
#define PNG_PIX(T, n)                                                                  \
    if (t == TYPE_OF(T)) {                                                             \
        T *q = (T *)m.data;                                                            \
        p.pix = (Byte *)q->pix.p;                                                      \
        p.stride = q->stride;                                                          \
        p.bpp = (n);                                                                   \
        return p;                                                                      \
    }
    PNG_PIX(ImageGray, 1)
    PNG_PIX(ImageGray16, 2)
    PNG_PIX(ImageNRGBA, 4)
    PNG_PIX(ImageNRGBA64, 8)
    PNG_PIX(ImagePaletted, 1)
    PNG_PIX(ImageRGBA, 4)
    PNG_PIX(ImageRGBA64, 8)
#undef PNG_PIX
    return p;
}

/* Allocates the image for a pass, or for the whole image, of the decoder's
 * type. Null with nothing allocated when a says no. */
static Image png_new_image(PngDecoder *d, Int width, Int height) {
    ImageRectangle r = image_rect(0, 0, width, height);
    Image m = {NULL, NULL};
    switch (d->cb) {
    case CB_G1:
    case CB_G2:
    case CB_G4:
    case CB_G8:
        if (d->use_transparent) {
            ImageNRGBA *p = image_new_nrgba(d->a, r);
            if (p != NULL)
                m = image_nrgba_as_image(p);
        } else {
            ImageGray *p = image_new_gray(d->a, r);
            if (p != NULL)
                m = image_gray_as_image(p);
        }
        break;
    case CB_GA8:
    case CB_TCA8: {
        ImageNRGBA *p = image_new_nrgba(d->a, r);
        if (p != NULL)
            m = image_nrgba_as_image(p);
        break;
    }
    case CB_TC8:
        if (d->use_transparent) {
            ImageNRGBA *p = image_new_nrgba(d->a, r);
            if (p != NULL)
                m = image_nrgba_as_image(p);
        } else {
            ImageRGBA *p = image_new_rgba(d->a, r);
            if (p != NULL)
                m = image_rgba_as_image(p);
        }
        break;
    case CB_P1:
    case CB_P2:
    case CB_P4:
    case CB_P8: {
        ImagePaletted *p = image_new_paletted(d->a, r, d->palette);
        if (p != NULL)
            m = image_paletted_as_image(p);
        break;
    }
    case CB_G16:
        if (d->use_transparent) {
            ImageNRGBA64 *p = image_new_nrgba64(d->a, r);
            if (p != NULL)
                m = image_nrgba64_as_image(p);
        } else {
            ImageGray16 *p = image_new_gray16(d->a, r);
            if (p != NULL)
                m = image_gray16_as_image(p);
        }
        break;
    case CB_GA16:
    case CB_TCA16: {
        ImageNRGBA64 *p = image_new_nrgba64(d->a, r);
        if (p != NULL)
            m = image_nrgba64_as_image(p);
        break;
    }
    case CB_TC16:
        if (d->use_transparent) {
            ImageNRGBA64 *p = image_new_nrgba64(d->a, r);
            if (p != NULL)
                m = image_nrgba64_as_image(p);
        } else {
            ImageRGBA64 *p = image_new_rgba64(d->a, r);
            if (p != NULL)
                m = image_rgba64_as_image(p);
        }
        break;
    default:
        break;
    }
    return m;
}

/* Frees an image the decoder made, leaving the palette, which the decoder
 * frees itself. */
static void png_free_image(PngDecoder *d, Image m) {
    if (m.data == NULL)
        return;
    if (m.vt->self_type == TYPE_OF(ImagePaletted))
        image_paletted_free((ImagePaletted *)m.data, d->a);
    else
        image_decoded_free(m, d->a);
}

static void png_put16(Byte *p, uint16_t v) {
    p[0] = (Byte)(v >> 8);
    p[1] = (Byte)v;
}

/* Writes an NRGBA64 pixel, stored big endian, at p. */
static void png_put_nrgba64(Byte *p, uint16_t r, uint16_t g, uint16_t b, uint16_t a) {
    png_put16(p + 0, r);
    png_put16(p + 2, g);
    png_put16(p + 4, b);
    png_put16(p + 6, a);
}

static void png_put_nrgba(Byte *p, Byte r, Byte g, Byte b, Byte a) {
    p[0] = r;
    p[1] = g;
    p[2] = b;
    p[3] = a;
}

/* Unpacks one unfiltered row of packed gray samples of depth bits into the
 * image row at dst. */
static void png_row_gray_packed(PngDecoder *d, const Byte *cdat, Byte *dst, Int width,
                                int depth) {
    int per = 8 / depth;
    int shift = 8 - depth;
    Byte scale = depth == 1 ? 0xff : depth == 2 ? 0x55 : 0x11;
    Byte ty = d->transparent[1];
    for (Int x = 0; x < width; x += per) {
        Byte b = cdat[x / per];
        for (int x2 = 0; x2 < per && x + x2 < width; x2++) {
            Byte ycol = (Byte)((b >> shift) * scale);
            if (d->use_transparent)
                png_put_nrgba(dst + 4 * (x + x2), ycol, ycol, ycol,
                              ycol == ty ? 0x00 : 0xff);
            else
                dst[x + x2] = ycol;
            b = (Byte)(b << depth);
        }
    }
}

/* The same for packed palette indexes, lengthening the palette to cover each
 * index as Go does. */
static void png_row_paletted_packed(ImagePaletted *p, const Byte *cdat, Byte *dst,
                                    Int width, int depth) {
    int per = 8 / depth;
    int shift = 8 - depth;
    for (Int x = 0; x < width; x += per) {
        Byte b = cdat[x / per];
        for (int x2 = 0; x2 < per && x + x2 < width; x2++) {
            Byte idx = (Byte)(b >> shift);
            if (p->palette.len <= (Int)idx)
                p->palette = slice_sub(p->palette, 0, (Int)idx + 1);
            dst[x + x2] = idx;
            b = (Byte)(b << depth);
        }
    }
}

/* readImagePass: reads one pass of an interlaced image, or the whole of one
 * that is not, from r. With allocate_only it only makes the full sized image
 * for the passes to be merged into. *out is the nil Image for a pass with no
 * pixels in it. */
static Error png_read_image_pass(PngDecoder *d, IoReader r, int pass,
                                 bool allocate_only, Image *out) {
    Int bits_per_pixel = 0;
    Int width = d->width, height = d->height;
    *out = (Image){NULL, NULL};
    if (d->interlace == IT_ADAM7 && !allocate_only) {
        PngInterlaceScan p = png_interlacing[pass];
        width = (width - p.x_offset + p.x_factor - 1) / p.x_factor;
        height = (height - p.y_offset + p.y_factor - 1) / p.y_factor;
        /* A PNG image can't have zero width or height, but for an interlaced
         * image, an individual pass might have zero width or height. */
        if (width == 0 || height == 0)
            return BURROW_NO_ERROR;
    }
    switch (d->cb) {
    case CB_G1:
    case CB_G2:
    case CB_G4:
    case CB_G8:
    case CB_P1:
    case CB_P2:
    case CB_P4:
    case CB_P8:
        bits_per_pixel = d->depth;
        break;
    case CB_GA8:
    case CB_G16:
        bits_per_pixel = 16;
        break;
    case CB_TC8:
        bits_per_pixel = 24;
        break;
    case CB_TCA8:
    case CB_GA16:
        bits_per_pixel = 32;
        break;
    case CB_TC16:
        bits_per_pixel = 48;
        break;
    case CB_TCA16:
        bits_per_pixel = 64;
        break;
    default:
        break;
    }
    Image img = png_new_image(d, width, height);
    if (img.data == NULL)
        return burrow_err_out_of_memory;
    if (allocate_only) {
        *out = img;
        return BURROW_NO_ERROR;
    }
    PngPix px = png_pix_of(img);
    ImagePaletted *paletted = png_cb_paletted(d->cb) ? (ImagePaletted *)img.data : NULL;
    Int bytes_per_pixel = (bits_per_pixel + 7) / 8;

    /* The +1 is for the per-row filter type, which is at cr[0]. */
    int64_t row_size64 = 1 + ((int64_t)bits_per_pixel * (int64_t)width + 7) / 8;
    if (row_size64 > BURROW_INT_MAX / 2) {
        png_free_image(d, img);
        return png_err_overflow;
    }
    Int row_size = (Int)row_size64;
    /* The current and previous row, one allocation. */
    Byte *rows = (Byte *)mem_alloc(d->a, 2 * (size_t)row_size, 1);
    if (rows == NULL) {
        png_free_image(d, img);
        return burrow_err_out_of_memory;
    }
    Byte *cr = rows, *pr = rows + row_size;
    Error err = BURROW_NO_ERROR;

    for (Int y = 0; y < height; y++) {
        /* Read the decompressed bytes. */
        Error e = BURROW_NO_ERROR;
        io_read_full(r, slice_from(cr, row_size, row_size, TYPE_BYTE), &e);
        if (BURROW_FAILED(e)) {
            if (png_is_eof(e) || (e.vt == io_err_unexpected_eof.vt &&
                                  e.data == io_err_unexpected_eof.data))
                e = png_err_not_enough;
            err = e;
            break;
        }

        /* Apply the filter. */
        Byte *cdat = cr + 1;
        const Byte *pdat = pr + 1;
        Int n = row_size - 1;
        switch (cr[0]) {
        case FT_NONE:
            break;
        case FT_SUB:
            for (Int i = bytes_per_pixel; i < n; i++)
                cdat[i] = (Byte)(cdat[i] + cdat[i - bytes_per_pixel]);
            break;
        case FT_UP:
            for (Int i = 0; i < n; i++)
                cdat[i] = (Byte)(cdat[i] + pdat[i]);
            break;
        case FT_AVERAGE:
            /* The first column has no column to its left, so it is a
             * special case. We know that the first column exists because we
             * check above that width != 0, and so len(cdat) != 0. */
            for (Int i = 0; i < bytes_per_pixel; i++)
                cdat[i] = (Byte)(cdat[i] + pdat[i] / 2);
            for (Int i = bytes_per_pixel; i < n; i++)
                cdat[i] = (Byte)(cdat[i] +
                                 ((int)cdat[i - bytes_per_pixel] + (int)pdat[i]) / 2);
            break;
        case FT_PAETH:
            png_filter_paeth(cdat, pdat, n, bytes_per_pixel);
            break;
        default:
            err = png_err_filter;
            break;
        }
        if (BURROW_FAILED(err))
            break;

        /* Convert from bytes to colors. */
        Byte *dst = px.pix + y * px.stride;
        switch (d->cb) {
        case CB_G1:
            png_row_gray_packed(d, cdat, dst, width, 1);
            break;
        case CB_G2:
            png_row_gray_packed(d, cdat, dst, width, 2);
            break;
        case CB_G4:
            png_row_gray_packed(d, cdat, dst, width, 4);
            break;
        case CB_G8:
            if (d->use_transparent) {
                Byte ty = d->transparent[1];
                for (Int x = 0; x < width; x++) {
                    Byte ycol = cdat[x];
                    png_put_nrgba(dst + 4 * x, ycol, ycol, ycol,
                                  ycol == ty ? 0x00 : 0xff);
                }
            } else {
                memcpy(dst, cdat, (size_t)width);
            }
            break;
        case CB_GA8:
            for (Int x = 0; x < width; x++) {
                Byte ycol = cdat[2 * x + 0];
                png_put_nrgba(dst + 4 * x, ycol, ycol, ycol, cdat[2 * x + 1]);
            }
            break;
        case CB_TC8:
            if (d->use_transparent) {
                Byte tr = d->transparent[1], tg = d->transparent[3],
                     tb = d->transparent[5];
                for (Int x = 0; x < width; x++) {
                    Byte cr8 = cdat[3 * x + 0], cg = cdat[3 * x + 1],
                         cb = cdat[3 * x + 2];
                    Byte ca = cr8 == tr && cg == tg && cb == tb ? 0x00 : 0xff;
                    png_put_nrgba(dst + 4 * x, cr8, cg, cb, ca);
                }
            } else {
                for (Int x = 0; x < width; x++)
                    png_put_nrgba(dst + 4 * x, cdat[3 * x + 0], cdat[3 * x + 1],
                                  cdat[3 * x + 2], 0xff);
            }
            break;
        case CB_P1:
            png_row_paletted_packed(paletted, cdat, dst, width, 1);
            break;
        case CB_P2:
            png_row_paletted_packed(paletted, cdat, dst, width, 2);
            break;
        case CB_P4:
            png_row_paletted_packed(paletted, cdat, dst, width, 4);
            break;
        case CB_P8:
            if (paletted->palette.len != 256) {
                for (Int x = 0; x < width; x++)
                    if (paletted->palette.len <= (Int)cdat[x])
                        paletted->palette =
                            slice_sub(paletted->palette, 0, (Int)cdat[x] + 1);
            }
            memcpy(dst, cdat, (size_t)width);
            break;
        case CB_TCA8:
            memcpy(dst, cdat, (size_t)(4 * width));
            break;
        case CB_G16:
            if (d->use_transparent) {
                uint16_t ty =
                    (uint16_t)((uint16_t)d->transparent[0] << 8 | d->transparent[1]);
                for (Int x = 0; x < width; x++) {
                    uint16_t ycol =
                        (uint16_t)((uint16_t)cdat[2 * x] << 8 | cdat[2 * x + 1]);
                    png_put_nrgba64(dst + 8 * x, ycol, ycol, ycol,
                                    ycol == ty ? 0x0000 : 0xffff);
                }
            } else {
                memcpy(dst, cdat, (size_t)(2 * width));
            }
            break;
        case CB_GA16:
            for (Int x = 0; x < width; x++) {
                uint16_t ycol =
                    (uint16_t)((uint16_t)cdat[4 * x] << 8 | cdat[4 * x + 1]);
                uint16_t acol =
                    (uint16_t)((uint16_t)cdat[4 * x + 2] << 8 | cdat[4 * x + 3]);
                png_put_nrgba64(dst + 8 * x, ycol, ycol, ycol, acol);
            }
            break;
        case CB_TC16:
            if (d->use_transparent) {
                uint16_t tr =
                    (uint16_t)((uint16_t)d->transparent[0] << 8 | d->transparent[1]);
                uint16_t tg =
                    (uint16_t)((uint16_t)d->transparent[2] << 8 | d->transparent[3]);
                uint16_t tb =
                    (uint16_t)((uint16_t)d->transparent[4] << 8 | d->transparent[5]);
                for (Int x = 0; x < width; x++) {
                    const Byte *s = cdat + 6 * x;
                    uint16_t rcol = (uint16_t)((uint16_t)s[0] << 8 | s[1]);
                    uint16_t gcol = (uint16_t)((uint16_t)s[2] << 8 | s[3]);
                    uint16_t bcol = (uint16_t)((uint16_t)s[4] << 8 | s[5]);
                    uint16_t acol =
                        rcol == tr && gcol == tg && bcol == tb ? 0x0000 : 0xffff;
                    png_put_nrgba64(dst + 8 * x, rcol, gcol, bcol, acol);
                }
            } else {
                for (Int x = 0; x < width; x++) {
                    memcpy(dst + 8 * x, cdat + 6 * x, 6);
                    dst[8 * x + 6] = 0xff;
                    dst[8 * x + 7] = 0xff;
                }
            }
            break;
        case CB_TCA16:
            memcpy(dst, cdat, (size_t)(8 * width));
            break;
        default:
            break;
        }

        /* The current row for y is the previous row for y+1. */
        Byte *t = pr;
        pr = cr;
        cr = t;
    }
    mem_free(d->a, rows, 2 * (size_t)row_size, 1);
    if (BURROW_FAILED(err)) {
        png_free_image(d, img);
        return err;
    }
    *out = img;
    return BURROW_NO_ERROR;
}

/* mergePassInto: merges a single pass into a full sized image. */
static void png_merge_pass_into(Image dst, Image src, int pass) {
    PngInterlaceScan p = png_interlacing[pass];
    PngPix s = png_pix_of(src), t = png_pix_of(dst);
    if (dst.vt->self_type == TYPE_OF(ImagePaletted)) {
        ImagePaletted *target = (ImagePaletted *)dst.data;
        const ImagePaletted *source = (const ImagePaletted *)src.data;
        if (target->palette.len < source->palette.len)
            target->palette = source->palette;
    }
    ImageRectangle bounds = src.vt->bounds(src.data);
    Int bpp = t.bpp;
    Int si = 0;
    for (Int y = bounds.min.y; y < bounds.max.y; y++) {
        Int d_base = (y * p.y_factor + p.y_offset) * t.stride + p.x_offset * bpp;
        for (Int x = bounds.min.x; x < bounds.max.x; x++) {
            Int di = d_base + x * p.x_factor * bpp;
            memcpy(t.pix + di, s.pix + si, (size_t)bpp);
            si += bpp;
        }
    }
}

static Error png_decode_idat(PngDecoder *d, Image *out) {
    Error err = BURROW_NO_ERROR;
    IoReader self = {&png_idat_reader_vt, d};
    IoReadCloser zr = zlib_new_reader(d->a, self, &err);
    if (BURROW_FAILED(err))
        return err;
    IoReader r = {&zr.vt->reader, zr.data};
    Image img = {NULL, NULL};
    if (d->interlace == IT_NONE) {
        err = png_read_image_pass(d, r, 0, false, &img);
    } else if (d->interlace == IT_ADAM7) {
        /* Allocate a blank image of the full size. */
        err = png_read_image_pass(d, r, 0, true, &img);
        for (int pass = 0; pass < 7 && BURROW_OK(err); pass++) {
            Image image_pass;
            err = png_read_image_pass(d, r, pass, false, &image_pass);
            if (BURROW_OK(err) && image_pass.data != NULL) {
                png_merge_pass_into(img, image_pass, pass);
                png_free_image(d, image_pass);
            }
        }
    }

    /* Check for EOF, to verify the zlib checksum. */
    Int n = 0;
    Error rerr = BURROW_NO_ERROR;
    for (int i = 0; BURROW_OK(err) && n == 0 && BURROW_OK(rerr); i++) {
        if (i == 100) {
            err = io_err_no_progress;
            break;
        }
        n = r.vt->read(r.data, slice_from(d->tmp, 1, 1, TYPE_BYTE), &rerr);
    }
    if (BURROW_OK(err) && BURROW_FAILED(rerr) && !png_is_eof(rerr))
        err = png_format_str(error_text(rerr));
    if (BURROW_OK(err) && (n != 0 || d->idat_length != 0))
        err = png_err_too_much;

    zlib_reader_free(zr);
    if (BURROW_FAILED(err)) {
        png_free_image(d, img);
        return err;
    }
    *out = img;
    return BURROW_NO_ERROR;
}

static Error png_parse_idat(PngDecoder *d, uint32_t length) {
    d->idat_length = length;
    Error err = png_decode_idat(d, &d->img);
    if (BURROW_FAILED(err))
        return err;
    return png_verify_checksum(d);
}

static Error png_parse_iend(PngDecoder *d, uint32_t length) {
    if (length != 0)
        return png_err_iend_length;
    return png_verify_checksum(d);
}

static Error png_parse_chunk(PngDecoder *d, bool config_only) {
    /* Read the length and chunk type. */
    Error err = png_read_full(d, d->tmp, 8);
    if (BURROW_FAILED(err))
        return err;
    uint32_t length = png_be32(d->tmp);
    d->crc = 0;
    png_crc_write(d, d->tmp + 4, 4);

    /* Read the chunk data. */
    const Byte *t = d->tmp + 4;
    if (memcmp(t, "IHDR", 4) == 0) {
        if (d->stage != DS_START)
            return png_err_chunk_order;
        d->stage = DS_SEEN_IHDR;
        return png_parse_ihdr(d, length);
    } else if (memcmp(t, "PLTE", 4) == 0) {
        if (d->stage != DS_SEEN_IHDR)
            return png_err_chunk_order;
        d->stage = DS_SEEN_PLTE;
        return png_parse_plte(d, length);
    } else if (memcmp(t, "tRNS", 4) == 0) {
        if (png_cb_paletted(d->cb)) {
            if (d->stage != DS_SEEN_PLTE)
                return png_err_chunk_order;
        } else if (png_cb_true_color(d->cb)) {
            if (d->stage != DS_SEEN_IHDR && d->stage != DS_SEEN_PLTE)
                return png_err_chunk_order;
        } else if (d->stage != DS_SEEN_IHDR) {
            return png_err_chunk_order;
        }
        d->stage = DS_SEEN_TRNS;
        return png_parse_trns(d, length);
    } else if (memcmp(t, "IDAT", 4) == 0) {
        if (d->stage < DS_SEEN_IHDR || d->stage > DS_SEEN_IDAT ||
            (d->stage == DS_SEEN_IHDR && png_cb_paletted(d->cb)))
            return png_err_chunk_order;
        if (d->stage != DS_SEEN_IDAT) {
            d->stage = DS_SEEN_IDAT;
            if (config_only)
                return BURROW_NO_ERROR;
            return png_parse_idat(d, length);
        }
        /* An IDAT after the ones decode read, which is ignored like any
         * other chunk. */
    } else if (memcmp(t, "IEND", 4) == 0) {
        if (d->stage != DS_SEEN_IDAT)
            return png_err_chunk_order;
        d->stage = DS_SEEN_IEND;
        return png_parse_iend(d, length);
    }
    if (length > 0x7fffffff) {
        Byte buf[48];
        Int n = 0;
        png_cat(buf, &n, "Bad chunk length: ");
        png_cat_int(buf, &n, (int64_t)length);
        return png_format_str(str_from_bytes(buf, n));
    }
    /* Ignore this chunk (of a known length). */
    Byte ignored[4096];
    while (length > 0) {
        Int n = length < sizeof ignored ? (Int)length : (Int)sizeof ignored;
        err = png_read_full(d, ignored, n);
        if (BURROW_FAILED(err))
            return err;
        png_crc_write(d, ignored, n);
        length -= (uint32_t)n;
    }
    return png_verify_checksum(d);
}

static Error png_check_header(PngDecoder *d) {
    Error err = png_read_full(d, d->tmp, PNG_HEADER_LEN);
    if (BURROW_FAILED(err))
        return err;
    if (memcmp(d->tmp, PNG_HEADER, PNG_HEADER_LEN) != 0)
        return png_err_not_png;
    return BURROW_NO_ERROR;
}

static void png_decoder_init(PngDecoder *d, Alloc *a, IoReader r) {
    memset(d, 0, sizeof *d);
    d->a = a;
    d->r = r;
    d->palette = slice_nil(TYPE_OF(Color));
}

static void png_palette_free(PngDecoder *d) {
    if (d->palette.p != NULL)
        mem_free(d->a, d->palette.p, 256 * sizeof(Color), _Alignof(Color));
    d->palette = slice_nil(TYPE_OF(Color));
}

static Error png_eof_unexpected(Error err) {
    return png_is_eof(err) ? io_err_unexpected_eof : err;
}

Image png_decode(Alloc *a, IoReader r, Error *err) {
    PngDecoder d;
    png_decoder_init(&d, a, r);
    Error e = png_check_header(&d);
    while (BURROW_OK(e) && d.stage != DS_SEEN_IEND)
        e = png_parse_chunk(&d, false);
    if (BURROW_FAILED(e)) {
        png_free_image(&d, d.img);
        png_palette_free(&d);
        BURROW_OUT(err, png_eof_unexpected(e));
        return (Image){NULL, NULL};
    }
    /* The palette now belongs to the image, when there is one. */
    if (d.img.vt == NULL || d.img.vt->self_type != TYPE_OF(ImagePaletted))
        png_palette_free(&d);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return d.img;
}

ImageConfig png_decode_config(Alloc *a, IoReader r, Error *err) {
    ImageConfig c;
    memset(&c, 0, sizeof c);
    PngDecoder d;
    png_decoder_init(&d, a, r);
    Error e = png_check_header(&d);
    while (BURROW_OK(e)) {
        e = png_parse_chunk(&d, true);
        if (BURROW_FAILED(e))
            break;
        if (png_cb_paletted(d.cb)) {
            if (d.stage >= DS_SEEN_TRNS)
                break;
        } else if (d.stage >= DS_SEEN_IHDR) {
            break;
        }
    }
    if (BURROW_FAILED(e)) {
        png_palette_free(&d);
        BURROW_OUT(err, png_eof_unexpected(e));
        return c;
    }
    switch (d.cb) {
    case CB_G1:
    case CB_G2:
    case CB_G4:
    case CB_G8:
        c.color_model = color_gray_model;
        break;
    case CB_GA8:
    case CB_TCA8:
        c.color_model = color_nrgba_model;
        break;
    case CB_TC8:
        c.color_model = color_rgba_model;
        break;
    case CB_P1:
    case CB_P2:
    case CB_P4:
    case CB_P8: {
        ColorPalette *p =
            (ColorPalette *)mem_alloc(a, sizeof *p, _Alignof(ColorPalette));
        if (p == NULL) {
            png_palette_free(&d);
            BURROW_OUT(err, burrow_err_out_of_memory);
            return c;
        }
        *p = d.palette;
        c.color_model = color_palette_as_model(p);
        break;
    }
    case CB_G16:
        c.color_model = color_gray16_model;
        break;
    case CB_GA16:
    case CB_TCA16:
        c.color_model = color_nrgba64_model;
        break;
    case CB_TC16:
        c.color_model = color_rgba64_model;
        break;
    default:
        break;
    }
    c.width = d.width;
    c.height = d.height;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return c;
}

static Image png_decode_fn(void *env, Alloc *a, IoReader r, Error *err) {
    (void)env;
    return png_decode(a, r, err);
}

static ImageConfig png_decode_config_fn(void *env, Alloc *a, IoReader r, Error *err) {
    (void)env;
    return png_decode_config(a, r, err);
}

static void png_do_register(void *env) {
    (void)env;
    image_register_format(BURROW_S("png"), BURROW_S(PNG_HEADER),
                          BURROW_FN(ImageDecodeFunc, png_decode_fn, NULL),
                          BURROW_FN(ImageDecodeConfigFunc, png_decode_config_fn, NULL));
}

static SyncOnce png_registered;

void png_register(void) {
    sync_once_do(&png_registered, BURROW_FN(Func, png_do_register, NULL));
}

/* ---------------------------------------------------------------- encoder */

struct PngEncoderBuffer {
    Alloc *a;
    IoWriter w;
    Image m;
    int cb;
    Error err;
    Byte header[8];
    Byte footer[4];
    Byte tmp[4 * 256];
    /* The current row filtered each of the five ways, and the previous row,
     * each cap bytes. */
    Byte *cr[N_FILTER];
    Byte *pr;
    Int cap;
    ZlibWriter *zw;
    Int zw_level;
    BufioWriter *bw;
};

void png_encoder_buffer_free(PngEncoderBuffer *e) {
    if (e == NULL)
        return;
    Alloc *a = e->a;
    for (int i = 0; i < N_FILTER; i++)
        if (e->cr[i] != NULL)
            mem_free(a, e->cr[i], (size_t)e->cap, 1);
    if (e->pr != NULL)
        mem_free(a, e->pr, (size_t)e->cap, 1);
    zlib_writer_free(e->zw);
    bufio_writer_free(e->bw);
    mem_free(a, e, sizeof *e, _Alignof(PngEncoderBuffer));
}

/* opaque: whether m is fully opaque, asked of m when it can say and worked
 * out pixel by pixel when it cannot. */
static bool png_opaque(Image m) {
    if (m.vt->opaque != NULL)
        return m.vt->opaque(m.data);
    ImageRectangle b = m.vt->bounds(m.data);
    for (Int y = b.min.y; y < b.max.y; y++)
        for (Int x = b.min.x; x < b.max.x; x++)
            if (color_rgba(m.vt->at(m.data, x, y)).a != 0xffff)
                return false;
    return true;
}

/* The absolute value of a byte interpreted as a signed int8. */
static int png_abs8(uint8_t d) {
    return d < 128 ? (int)d : 256 - (int)d;
}

static void png_write_all(PngEncoderBuffer *e, const Byte *p, Int n) {
    Error err = BURROW_NO_ERROR;
    e->w.vt->write(e->w.data, slice_from((void *)(uintptr_t)p, n, n, TYPE_BYTE), &err);
    e->err = err;
}

static void png_write_chunk(PngEncoderBuffer *e, const Byte *b, Int len,
                            const char *name) {
    if (BURROW_FAILED(e->err))
        return;
    if ((Int)(uint32_t)len != len) {
        Byte buf[64];
        Int n = 0;
        png_cat(buf, &n, name);
        png_cat(buf, &n, " chunk is too large: ");
        png_cat_int(buf, &n, (int64_t)len);
        e->err = png_unsupported_str(str_from_bytes(buf, n));
        return;
    }
    png_put_be32(e->header, (uint32_t)len);
    memcpy(e->header + 4, name, 4);
    uint32_t crc =
        crc32_update(0, crc32_ieee_table, slice_from(e->header + 4, 4, 4, TYPE_BYTE));
    if (len > 0)
        crc = crc32_update(crc, crc32_ieee_table,
                           slice_from((void *)(uintptr_t)b, len, len, TYPE_BYTE));
    png_put_be32(e->footer, crc);

    png_write_all(e, e->header, 8);
    if (BURROW_FAILED(e->err))
        return;
    if (len > 0) {
        png_write_all(e, b, len);
        if (BURROW_FAILED(e->err))
            return;
    }
    png_write_all(e, e->footer, 4);
}

static void png_write_ihdr(PngEncoderBuffer *e) {
    ImageRectangle b = e->m.vt->bounds(e->m.data);
    png_put_be32(e->tmp, (uint32_t)image_rectangle_dx(b));
    png_put_be32(e->tmp + 4, (uint32_t)image_rectangle_dy(b));
    /* Set bit depth and color type. */
    switch (e->cb) {
    case CB_G8:
        e->tmp[8] = 8;
        e->tmp[9] = PNG_CT_GRAYSCALE;
        break;
    case CB_TC8:
        e->tmp[8] = 8;
        e->tmp[9] = PNG_CT_TRUE_COLOR;
        break;
    case CB_P8:
        e->tmp[8] = 8;
        e->tmp[9] = PNG_CT_PALETTED;
        break;
    case CB_P4:
        e->tmp[8] = 4;
        e->tmp[9] = PNG_CT_PALETTED;
        break;
    case CB_P2:
        e->tmp[8] = 2;
        e->tmp[9] = PNG_CT_PALETTED;
        break;
    case CB_P1:
        e->tmp[8] = 1;
        e->tmp[9] = PNG_CT_PALETTED;
        break;
    case CB_TCA8:
        e->tmp[8] = 8;
        e->tmp[9] = PNG_CT_TRUE_COLOR_ALPHA;
        break;
    case CB_G16:
        e->tmp[8] = 16;
        e->tmp[9] = PNG_CT_GRAYSCALE;
        break;
    case CB_TC16:
        e->tmp[8] = 16;
        e->tmp[9] = PNG_CT_TRUE_COLOR;
        break;
    case CB_TCA16:
        e->tmp[8] = 16;
        e->tmp[9] = PNG_CT_TRUE_COLOR_ALPHA;
        break;
    default:
        break;
    }
    e->tmp[10] = 0; /* default compression method */
    e->tmp[11] = 0; /* default filter method */
    e->tmp[12] = 0; /* non-interlaced */
    png_write_chunk(e, e->tmp, 13, "IHDR");
}

static void png_write_plte_and_trns(PngEncoderBuffer *e, ColorPalette p) {
    if (p.len < 1 || p.len > 256) {
        Byte buf[48];
        Int n = 0;
        png_cat(buf, &n, "bad palette length: ");
        png_cat_int(buf, &n, (int64_t)p.len);
        e->err = png_format_str(str_from_bytes(buf, n));
        return;
    }
    Int last = -1;
    for (Int i = 0; i < p.len; i++) {
        ColorNRGBA c1 =
            color_model_convert(color_nrgba_model, *png_palette_at(p, i)).data.nrgba;
        e->tmp[3 * i + 0] = c1.r;
        e->tmp[3 * i + 1] = c1.g;
        e->tmp[3 * i + 2] = c1.b;
        if (c1.a != 0xff)
            last = i;
        e->tmp[3 * 256 + i] = c1.a;
    }
    png_write_chunk(e, e->tmp, 3 * p.len, "PLTE");
    if (last != -1)
        png_write_chunk(e, e->tmp + 3 * 256, 1 + last, "tRNS");
}

/* Write: the IDAT writer, which puts each buffer it is given in a chunk of
 * its own. The bufio.Writer in front of it makes those 32 KB. */
static Int png_idat_write(void *self, Slice b, Error *err) {
    PngEncoderBuffer *e = (PngEncoderBuffer *)self;
    png_write_chunk(e, (const Byte *)b.p, b.len, "IDAT");
    if (BURROW_FAILED(e->err)) {
        BURROW_OUT(err, e->err);
        return 0;
    }
    return b.len;
}

static const IoWriterVT png_idat_writer_vt = {NULL, png_idat_write};

/* filter: chooses the filter to use for encoding the current row, and
 * applies it. The return value is the index of the filter and also of the
 * row in cr that has had it applied. */
static int png_filter(Byte *const *cr, const Byte *pr, Int n, Int bpp) {
    /* We try all five filter types, and pick the one that minimizes the sum
     * of absolute differences. This is the same heuristic that libpng uses,
     * although the filters are attempted in order of estimated most likely
     * to be minimal (ftUp, ftPaeth, ftNone, ftSub, ftAverage), rather than
     * in their enumeration order (ftNone, ftSub, ftUp, ftAverage,
     * ftPaeth). */
    const Byte *cdat0 = cr[0] + 1;
    Byte *cdat1 = cr[1] + 1;
    Byte *cdat2 = cr[2] + 1;
    Byte *cdat3 = cr[3] + 1;
    Byte *cdat4 = cr[4] + 1;
    const Byte *pdat = pr + 1;

    /* The up filter. */
    int sum = 0;
    for (Int i = 0; i < n; i++) {
        cdat2[i] = (Byte)(cdat0[i] - pdat[i]);
        sum += png_abs8(cdat2[i]);
    }
    int best = sum;
    int filter = FT_UP;

    /* The Paeth filter. */
    sum = 0;
    for (Int i = 0; i < bpp; i++) {
        cdat4[i] = (Byte)(cdat0[i] - pdat[i]);
        sum += png_abs8(cdat4[i]);
    }
    for (Int i = bpp; i < n; i++) {
        cdat4[i] = (Byte)(cdat0[i] - png_paeth(cdat0[i - bpp], pdat[i], pdat[i - bpp]));
        sum += png_abs8(cdat4[i]);
        if (sum >= best)
            break;
    }
    if (sum < best) {
        best = sum;
        filter = FT_PAETH;
    }

    /* The none filter. */
    sum = 0;
    for (Int i = 0; i < n; i++) {
        sum += png_abs8(cdat0[i]);
        if (sum >= best)
            break;
    }
    if (sum < best) {
        best = sum;
        filter = FT_NONE;
    }

    /* The sub filter. */
    sum = 0;
    for (Int i = 0; i < bpp; i++) {
        cdat1[i] = cdat0[i];
        sum += png_abs8(cdat1[i]);
    }
    for (Int i = bpp; i < n; i++) {
        cdat1[i] = (Byte)(cdat0[i] - cdat0[i - bpp]);
        sum += png_abs8(cdat1[i]);
        if (sum >= best)
            break;
    }
    if (sum < best) {
        best = sum;
        filter = FT_SUB;
    }

    /* The average filter. */
    sum = 0;
    for (Int i = 0; i < bpp; i++) {
        cdat3[i] = (Byte)(cdat0[i] - pdat[i] / 2);
        sum += png_abs8(cdat3[i]);
    }
    for (Int i = bpp; i < n; i++) {
        cdat3[i] = (Byte)(cdat0[i] - (Byte)(((int)cdat0[i - bpp] + (int)pdat[i]) / 2));
        sum += png_abs8(cdat3[i]);
        if (sum >= best)
            break;
    }
    if (sum < best)
        filter = FT_AVERAGE;

    return filter;
}

/* Makes sure each row buffer holds sz bytes. */
static bool png_rows(PngEncoderBuffer *e, Int sz) {
    if (e->cap >= sz)
        return true;
    Byte *fresh[N_FILTER + 1];
    for (int i = 0; i <= N_FILTER; i++) {
        Byte *b = (Byte *)mem_alloc(e->a, (size_t)sz, 1);
        if (b == NULL) {
            while (i-- > 0)
                mem_free(e->a, fresh[i], (size_t)sz, 1);
            return false;
        }
        fresh[i] = b;
    }
    for (int i = 0; i < N_FILTER; i++) {
        if (e->cr[i] != NULL)
            mem_free(e->a, e->cr[i], (size_t)e->cap, 1);
        e->cr[i] = fresh[i];
    }
    if (e->pr != NULL)
        mem_free(e->a, e->pr, (size_t)e->cap, 1);
    e->pr = fresh[N_FILTER];
    e->cap = sz;
    return true;
}

static Error png_write_image(PngEncoderBuffer *e, IoWriter w, Image m, int cb,
                             Int level) {
    if (e->zw == NULL || e->zw_level != level) {
        Error err = BURROW_NO_ERROR;
        ZlibWriter *zw = zlib_new_writer_level(e->a, w, level, &err);
        if (zw == NULL)
            return BURROW_FAILED(err) ? err : burrow_err_out_of_memory;
        zlib_writer_free(e->zw);
        e->zw = zw;
        e->zw_level = level;
    } else {
        zlib_writer_reset(e->zw, w);
    }

    Int bits_per_pixel = 0;
    switch (cb) {
    case CB_G8:
    case CB_P8:
        bits_per_pixel = 8;
        break;
    case CB_TC8:
        bits_per_pixel = 24;
        break;
    case CB_P4:
        bits_per_pixel = 4;
        break;
    case CB_P2:
        bits_per_pixel = 2;
        break;
    case CB_P1:
        bits_per_pixel = 1;
        break;
    case CB_TCA8:
        bits_per_pixel = 32;
        break;
    case CB_TC16:
        bits_per_pixel = 48;
        break;
    case CB_TCA16:
        bits_per_pixel = 64;
        break;
    case CB_G16:
        bits_per_pixel = 16;
        break;
    default:
        break;
    }

    /* cr[*] and pr are the bytes for the current and previous row. cr[0] is
     * unfiltered (or equivalently, filtered with the ftNone filter). cr[ft],
     * for non-zero filter types ft, are buffers for transforming cr[0] under
     * the other filters. pr is the unfiltered row of the previous line. The
     * +1 is for the per-row filter type, which is at cr[*][0]. */
    ImageRectangle b = m.vt->bounds(m.data);
    Int dx = image_rectangle_dx(b);
    Int sz = 1 + (bits_per_pixel * dx + 7) / 8;
    if (!png_rows(e, sz)) {
        (void)zlib_writer_close(e->zw);
        return burrow_err_out_of_memory;
    }
    for (int i = 0; i < N_FILTER; i++)
        e->cr[i][0] = (Byte)i;
    Byte *cr[N_FILTER];
    memcpy(cr, e->cr, sizeof cr);
    Byte *pr = e->pr;
    memset(pr, 0, (size_t)sz);

    const Type *t = m.vt->self_type;
    const ImageGray *gray = t == TYPE_OF(ImageGray) ? (const ImageGray *)m.data : NULL;
    const ImageRGBA *rgba = t == TYPE_OF(ImageRGBA) ? (const ImageRGBA *)m.data : NULL;
    const ImagePaletted *paletted =
        t == TYPE_OF(ImagePaletted) ? (const ImagePaletted *)m.data : NULL;
    const ImageNRGBA *nrgba =
        t == TYPE_OF(ImageNRGBA) ? (const ImageNRGBA *)m.data : NULL;

    Error err = BURROW_NO_ERROR;
    for (Int y = b.min.y; y < b.max.y; y++) {
        /* Convert from colors to bytes. */
        Int i = 1;
        Byte *cr0 = cr[0];
        switch (cb) {
        case CB_G8:
            if (gray != NULL) {
                Int offset = (y - b.min.y) * gray->stride;
                memcpy(cr0 + 1, (const Byte *)gray->pix.p + offset, (size_t)dx);
            } else {
                for (Int x = b.min.x; x < b.max.x; x++) {
                    Color c =
                        color_model_convert(color_gray_model, m.vt->at(m.data, x, y));
                    cr0[i++] = c.data.gray.y;
                }
            }
            break;
        case CB_TC8: {
            /* We have previously verified that the alpha value is fully
             * opaque. */
            Int stride = 0;
            const Byte *pix = NULL;
            if (rgba != NULL) {
                stride = rgba->stride;
                pix = (const Byte *)rgba->pix.p;
            } else if (nrgba != NULL) {
                stride = nrgba->stride;
                pix = (const Byte *)nrgba->pix.p;
            }
            if (stride != 0) {
                Int j0 = (y - b.min.y) * stride;
                Int j1 = j0 + dx * 4;
                for (Int j = j0; j < j1; j += 4) {
                    cr0[i + 0] = pix[j + 0];
                    cr0[i + 1] = pix[j + 1];
                    cr0[i + 2] = pix[j + 2];
                    i += 3;
                }
            } else {
                for (Int x = b.min.x; x < b.max.x; x++) {
                    ColorRGBAValue v = color_rgba(m.vt->at(m.data, x, y));
                    cr0[i + 0] = (Byte)(v.r >> 8);
                    cr0[i + 1] = (Byte)(v.g >> 8);
                    cr0[i + 2] = (Byte)(v.b >> 8);
                    i += 3;
                }
            }
            break;
        }
        case CB_P8:
            if (paletted != NULL) {
                Int offset = (y - b.min.y) * paletted->stride;
                memcpy(cr0 + 1, (const Byte *)paletted->pix.p + offset, (size_t)dx);
            } else {
                for (Int x = b.min.x; x < b.max.x; x++)
                    cr0[i++] = m.vt->color_index_at(m.data, x, y);
            }
            break;
        case CB_P4:
        case CB_P2:
        case CB_P1: {
            Byte a = 0;
            Int c = 0;
            Int pixels_per_byte = 8 / bits_per_pixel;
            for (Int x = b.min.x; x < b.max.x; x++) {
                a = (Byte)(a << bits_per_pixel | m.vt->color_index_at(m.data, x, y));
                c++;
                if (c == pixels_per_byte) {
                    cr0[i++] = a;
                    a = 0;
                    c = 0;
                }
            }
            if (c != 0) {
                while (c != pixels_per_byte) {
                    a = (Byte)(a << bits_per_pixel);
                    c++;
                }
                cr0[i] = a;
            }
            break;
        }
        case CB_TCA8:
            if (nrgba != NULL) {
                Int offset = (y - b.min.y) * nrgba->stride;
                memcpy(cr0 + 1, (const Byte *)nrgba->pix.p + offset, (size_t)(dx * 4));
            } else if (rgba != NULL) {
                Byte *d = cr0 + 1;
                const Byte *s =
                    (const Byte *)rgba->pix.p + image_rgba_pix_offset(rgba, b.min.x, y);
                for (Int x = 0; x < dx; x++, d += 4, s += 4) {
                    if (s[3] == 0x00) {
                        d[0] = 0;
                        d[1] = 0;
                        d[2] = 0;
                        d[3] = 0;
                    } else if (s[3] == 0xff) {
                        memcpy(d, s, 4);
                    } else {
                        /* This code does the same as
                         * color.NRGBAModel.Convert(rgba.At(x, y)).(color.NRGBA)
                         * but saves the cost of a type switch and calls. */
                        const uint32_t mm = 0x101 * 0xffff;
                        uint32_t a = (uint32_t)s[3] * 0x101;
                        d[0] = (Byte)(((uint32_t)s[0] * mm / a) >> 8);
                        d[1] = (Byte)(((uint32_t)s[1] * mm / a) >> 8);
                        d[2] = (Byte)(((uint32_t)s[2] * mm / a) >> 8);
                        d[3] = s[3];
                    }
                }
            } else {
                /* Convert from image.Image (which is alpha-premultiplied) to
                 * PNG's non-alpha-premultiplied. */
                for (Int x = b.min.x; x < b.max.x; x++) {
                    ColorNRGBA c =
                        color_model_convert(color_nrgba_model, m.vt->at(m.data, x, y))
                            .data.nrgba;
                    cr0[i + 0] = c.r;
                    cr0[i + 1] = c.g;
                    cr0[i + 2] = c.b;
                    cr0[i + 3] = c.a;
                    i += 4;
                }
            }
            break;
        case CB_G16:
            for (Int x = b.min.x; x < b.max.x; x++) {
                ColorGray16 c =
                    color_model_convert(color_gray16_model, m.vt->at(m.data, x, y))
                        .data.gray16;
                png_put16(cr0 + i, c.y);
                i += 2;
            }
            break;
        case CB_TC16:
            /* We have previously verified that the alpha value is fully
             * opaque. */
            for (Int x = b.min.x; x < b.max.x; x++) {
                ColorRGBAValue v = color_rgba(m.vt->at(m.data, x, y));
                png_put16(cr0 + i + 0, (uint16_t)v.r);
                png_put16(cr0 + i + 2, (uint16_t)v.g);
                png_put16(cr0 + i + 4, (uint16_t)v.b);
                i += 6;
            }
            break;
        case CB_TCA16:
            /* Convert from image.Image (which is alpha-premultiplied) to PNG's
             * non-alpha-premultiplied. */
            for (Int x = b.min.x; x < b.max.x; x++) {
                ColorNRGBA64 c =
                    color_model_convert(color_nrgba64_model, m.vt->at(m.data, x, y))
                        .data.nrgba64;
                png_put_nrgba64(cr0 + i, c.r, c.g, c.b, c.a);
                i += 8;
            }
            break;
        default:
            break;
        }

        /* Apply the filter. Skip filtering for NoCompression and paletted
         * images (cbP8) as described in
         * https://www.w3.org/TR/png/#12Filter-selection. */
        int f = FT_NONE;
        if (level != FLATE_NO_COMPRESSION && cb != CB_P8 && cb != CB_P4 &&
            cb != CB_P2 && cb != CB_P1) {
            /* Since we skip paletted images, we don't have to worry about
             * bitsPerPixel not being a multiple of 8. */
            Int bpp = bits_per_pixel / 8;
            f = png_filter(cr, pr, sz - 1, bpp);
        }

        /* Write the compressed bytes. */
        zlib_writer_write(e->zw, slice_from(cr[f], sz, sz, TYPE_BYTE), &err);
        if (BURROW_FAILED(err))
            break;

        /* The current row for y is the previous row for y+1. */
        Byte *t2 = pr;
        pr = cr[0];
        cr[0] = t2;
    }
    /* Go's cr and pr are the encoder's own slices, swapped as it goes, and
     * the next encode starts from wherever they ended up. */
    e->cr[0] = cr[0];
    e->pr = pr;
    Error cerr = zlib_writer_close(e->zw);
    (void)cerr;
    return err;
}

/* Write the actual image data to one or more IDAT chunks. */
static void png_write_idats(PngEncoderBuffer *e, Int level) {
    if (BURROW_FAILED(e->err))
        return;
    IoWriter idat = {&png_idat_writer_vt, e};
    if (e->bw == NULL) {
        e->bw = bufio_new_writer_size(e->a, idat, 1 << 15);
        if (e->bw == NULL) {
            e->err = burrow_err_out_of_memory;
            return;
        }
    } else {
        bufio_writer_reset(e->bw, idat);
    }
    e->err = png_write_image(e, bufio_writer_as_io_writer(e->bw), e->m, e->cb, level);
    if (BURROW_FAILED(e->err))
        return;
    e->err = bufio_writer_flush(e->bw);
}

static Int png_level_to_zlib(PngCompressionLevel l) {
    switch (l) {
    case PNG_NO_COMPRESSION:
        return FLATE_NO_COMPRESSION;
    case PNG_BEST_SPEED:
        return FLATE_BEST_SPEED;
    case PNG_BEST_COMPRESSION:
        return FLATE_BEST_COMPRESSION;
    default:
        return FLATE_DEFAULT_COMPRESSION;
    }
}

static bool png_model_eq(ColorModel a, ColorModel b) {
    return a.vt == b.vt && a.data == b.data;
}

Error png_encode(Alloc *a, IoWriter w, Image m) {
    PngEncoder e;
    memset(&e, 0, sizeof e);
    return png_encoder_encode(&e, a, w, m);
}

Error png_encoder_encode(const PngEncoder *enc, Alloc *a, IoWriter w, Image m) {
    /* Obviously, negative widths and heights are invalid. Furthermore, the
     * PNG spec section 11.2.2 says that zero is invalid. Excessively large
     * images are also rejected. */
    ImageRectangle bounds = m.vt->bounds(m.data);
    int64_t mw = image_rectangle_dx(bounds), mh = image_rectangle_dy(bounds);
    if (mw <= 0 || mh <= 0 || mw >= (int64_t)1 << 32 || mh >= (int64_t)1 << 32) {
        Byte buf[80];
        Int n = 0;
        png_cat(buf, &n, "invalid image size: ");
        png_cat_int(buf, &n, mw);
        png_cat(buf, &n, "x");
        png_cat_int(buf, &n, mh);
        return png_format_str(str_from_bytes(buf, n));
    }

    bool pooled = enc->buffer_pool.vt != NULL;
    PngEncoderBuffer *e = NULL;
    if (pooled)
        e = enc->buffer_pool.vt->get(enc->buffer_pool.data);
    if (e == NULL) {
        e = (PngEncoderBuffer *)mem_alloc(a, sizeof *e, _Alignof(PngEncoderBuffer));
        if (e == NULL)
            return burrow_err_out_of_memory;
        e->a = a;
    }

    e->w = w;
    e->m = m;
    e->err = BURROW_NO_ERROR;

    ColorPalette pal = slice_nil(TYPE_OF(Color));
    bool has_pal = false;
    if (m.vt->color_index_at != NULL) {
        const ColorPalette *p;
        if (color_model_as_palette(m.vt->color_model(m.data), &p) && p != NULL &&
            !slice_is_nil(*p)) {
            pal = *p;
            has_pal = true;
        }
    }
    if (has_pal) {
        if (pal.len <= 2)
            e->cb = CB_P1;
        else if (pal.len <= 4)
            e->cb = CB_P2;
        else if (pal.len <= 16)
            e->cb = CB_P4;
        else
            e->cb = CB_P8;
    } else {
        ColorModel cm = m.vt->color_model(m.data);
        if (png_model_eq(cm, color_gray_model))
            e->cb = CB_G8;
        else if (png_model_eq(cm, color_gray16_model))
            e->cb = CB_G16;
        else if (png_model_eq(cm, color_rgba_model) ||
                 png_model_eq(cm, color_nrgba_model) ||
                 png_model_eq(cm, color_alpha_model))
            e->cb = png_opaque(m) ? CB_TC8 : CB_TCA8;
        else
            e->cb = png_opaque(m) ? CB_TC16 : CB_TCA16;
    }

    png_write_all(e, (const Byte *)PNG_HEADER, PNG_HEADER_LEN);
    png_write_ihdr(e);
    if (has_pal)
        png_write_plte_and_trns(e, pal);
    png_write_idats(e, png_level_to_zlib(enc->compression_level));
    png_write_chunk(e, NULL, 0, "IEND");
    Error err = e->err;

    e->w = (IoWriter){NULL, NULL};
    e->m = (Image){NULL, NULL};
    e->err = BURROW_NO_ERROR;
    if (pooled)
        enc->buffer_pool.vt->put(enc->buffer_pool.data, e);
    else
        png_encoder_buffer_free(e);
    return err;
}

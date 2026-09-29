/* image/gif, from reader.go and writer.go.
 *
 * The decoder is Go's. Blocks are read one at a time from a byte reader, a
 * bufio.Reader when the source has no ReadByte, and a frame's image data goes
 * through a block reader that hides the sub-block lengths from the LZW
 * decoder, with Go's leniency about stray bytes after the end code.
 *
 * The encoder is Go's as well, so the bytes it writes are the bytes Go
 * writes, palettes, graphic control blocks and LZW codes included.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/image/gif.h"

#include "burrow/bufio.h"
#include "burrow/compress/lzw.h"
#include "burrow/declare.h"
#include "burrow/image/color/palette.h"
#include "burrow/strconv.h"
#include "burrow/sync.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ errors */

/* Go makes most of these with errors.New or fmt.Errorf where they happen, so
 * nobody can compare against them. The fixed ones live in read only memory. */
#define GIF_ERROR(name, text)                                                          \
    static const Str name##__text = {(const Byte *)(text), (Int)(sizeof(text) - 1)};   \
    static const Error name = {&burrow_sentinel_error_vt, &name##__text}

GIF_ERROR(gif_err_not_enough, "gif: not enough image data");
GIF_ERROR(gif_err_too_much, "gif: too much image data");
GIF_ERROR(gif_err_bad_pixel, "gif: invalid pixel value");
GIF_ERROR(gif_err_missing_image, "gif: missing image data");
GIF_ERROR(gif_err_no_color_table, "gif: no color table");
GIF_ERROR(gif_err_frame_bounds, "gif: frame bounds larger than image bounds");
GIF_ERROR(gif_err_table_too_big,
          "gif: cannot encode color table with more than 256 entries");
GIF_ERROR(gif_err_nil_entry, "gif: cannot encode color table with nil entries");
GIF_ERROR(gif_err_empty_palette, "gif: cannot encode image block with empty palette");
GIF_ERROR(gif_err_block_too_large, "gif: image block is too large to encode");
GIF_ERROR(gif_err_block_out_of_bounds, "gif: image block is out of bounds");
GIF_ERROR(gif_err_no_images, "gif: must provide at least one image");
GIF_ERROR(gif_err_mismatched_delay, "gif: mismatched image and delay lengths");
GIF_ERROR(gif_err_mismatched_disposal, "gif: mismatched image and disposal lengths");
GIF_ERROR(gif_err_model, "gif: GIF color model must be a color.Palette");
GIF_ERROR(gif_err_too_large, "gif: image is too large to encode");

#undef GIF_ERROR

/* prefix and then tail, as one error in the calling goroutine's error
 * arena. */
static Error gif_errorf(const char *prefix, Str tail) {
    Alloc *ea = error_allocator();
    Int plen = (Int)strlen(prefix);
    Byte *p = (Byte *)mem_alloc_nozero(ea, (size_t)(plen + tail.len) + 1, 1);
    if (p == NULL)
        return burrow_err_out_of_memory;
    memcpy(p, prefix, (size_t)plen);
    if (tail.len > 0)
        memcpy(p + plen, tail.p, (size_t)tail.len);
    return errors_new(ea, str_from_bytes(p, plen + tail.len));
}

/* fmt.Errorf(prefix + "%v", err). */
static Error gif_wrap(const char *prefix, Error err) {
    return gif_errorf(prefix, error_text(err));
}

/* fmt.Errorf(prefix + "%d", v), or prefix + "0x%.2x" when hex is set, which
 * are all the numbers the messages here have. */
static Error gif_errorf_num(const char *prefix, unsigned v, bool hex) {
    char buf[16];
    if (hex)
        snprintf(buf, sizeof buf, "0x%.2x", v);
    else
        snprintf(buf, sizeof buf, "%u", v);
    return gif_errorf(prefix, str_from_bytes(buf, (Int)strlen(buf)));
}

static bool gif_is(Error err, Error target) {
    return err.vt == target.vt && err.data == target.data;
}

/* ------------------------------------------------------------------ fields */

enum {
    /* Fields. */
    GIF_F_COLOR_TABLE = 1 << 7,
    GIF_F_INTERLACE = 1 << 6,
    GIF_F_COLOR_TABLE_BITS_MASK = 7,

    /* Graphic control flags. */
    GIF_GC_TRANSPARENT_COLOR_SET = 1 << 0,
    GIF_GC_DISPOSAL_METHOD_MASK = 7 << 2,

    /* Section indicators. */
    GIF_S_EXTENSION = 0x21,
    GIF_S_IMAGE_DESCRIPTOR = 0x2C,
    GIF_S_TRAILER = 0x3B,

    /* Extensions. */
    GIF_E_TEXT = 0x01,
    GIF_E_GRAPHIC_CONTROL = 0xF9,
    GIF_E_COMMENT = 0xFE,
    GIF_E_APPLICATION = 0xFF,

    /* Graphic control extension fields. */
    GIF_GC_LABEL = 0xF9,
    GIF_GC_BLOCK_SIZE = 0x04,
};

/* --------------------------------------------------------------- palettes */

static ColorPalette gif_palette_make(Alloc *a, Int n) {
    if (n == 0)
        return slice_nil(TYPE_OF(Color));
    Color *c = (Color *)mem_alloc(a, (size_t)n * sizeof(Color), _Alignof(Color));
    if (c == NULL)
        return slice_nil(TYPE_OF(Color));
    return slice_from(c, n, n, TYPE_OF(Color));
}

static void gif_palette_free(Alloc *a, ColorPalette p) {
    if (p.p != NULL && p.cap > 0)
        mem_free(a, p.p, (size_t)p.cap * sizeof(Color), _Alignof(Color));
}

/* A ColorPalette model over p, with the palette in a box of its own, the way
 * image_config_free expects. */
static bool gif_palette_model(Alloc *a, ColorPalette p, ColorModel *out) {
    ColorPalette *box =
        (ColorPalette *)mem_alloc(a, sizeof *box, _Alignof(ColorPalette));
    if (box == NULL)
        return false;
    *box = p;
    *out = color_palette_as_model(box);
    return true;
}

/* append(s, v), growing by doubling and giving the old array back. */
static bool gif_push(Alloc *a, Slice *s, const void *v) {
    size_t size = (size_t)s->elem->size;
    size_t align = (size_t)s->elem->align;
    if (s->len == s->cap) {
        Int cap = s->cap == 0 ? 4 : 2 * s->cap;
        void *p = mem_alloc(a, (size_t)cap * size, align);
        if (p == NULL)
            return false;
        if (s->len > 0)
            memcpy(p, s->p, (size_t)s->len * size);
        if (s->p != NULL)
            mem_free(a, s->p, (size_t)s->cap * size, align);
        s->p = p;
        s->cap = cap;
    }
    memcpy((Byte *)s->p + (size_t)s->len * size, v, size);
    s->len++;
    return true;
}

static void gif_slice_free(Alloc *a, Slice s) {
    if (s.p != NULL && s.cap > 0)
        mem_free(a, s.p, (size_t)s.cap * (size_t)s.elem->size, (size_t)s.elem->align);
}

/* ---------------------------------------------------------------- decoder */

typedef struct GifDecoder {
    Alloc *a;
    /* r is what reads come from. Bytes come through direct when it is a
     * bufio.Reader and through read_byte otherwise. own is the bufio.Reader
     * made here when the source has no ReadByte. */
    IoReader r;
    BufioReader *direct;
    BufioReader *own;
    const Method *read_byte;

    /* From header. */
    Int width;
    Int height;
    Int loop_count;
    Int delay_time;
    Byte background_index;
    Byte disposal_method;

    /* From image descriptor. */
    Byte image_fields;

    /* From graphics control. */
    Byte transparent_index;
    bool has_transparent_index;

    /* Computed. A nil palette when the file has none. */
    ColorPalette global_color_table;

    /* Used when decoding. */
    Slice delay;
    Slice disposal;
    Slice image;
    LzwReader *lzw;
    Byte tmp[1024]; /* must be at least 768 so we can read color table */
} GifDecoder;

static Error gif_decoder_init(GifDecoder *d, Alloc *a, IoReader r) {
    memset(d, 0, sizeof *d);
    d->a = a;
    d->global_color_table = slice_nil(TYPE_OF(Color));
    d->delay = slice_nil(TYPE_INT);
    d->disposal = slice_nil(TYPE_BYTE);
    d->image = slice_nil(TYPE_OF(ImagePalettedPtr));
    d->loop_count = -1;
    /* Add buffering if r does not provide ReadByte. */
    if (r.vt != NULL && r.vt->self_type == TYPE_BUFIO_READER) {
        d->direct = (BufioReader *)r.data;
        d->r = r;
        return BURROW_NO_ERROR;
    }
    d->read_byte = burrow__io_read_byte_method(r);
    if (d->read_byte != NULL) {
        d->r = r;
        return BURROW_NO_ERROR;
    }
    d->own = bufio_new_reader(a, r);
    if (d->own == NULL)
        return burrow_err_out_of_memory;
    d->direct = d->own;
    d->r = bufio_reader_as_io_reader(d->own);
    return BURROW_NO_ERROR;
}

/* A frame and its palette, unless the palette is the global one. */
static void gif_frame_free(Alloc *a, ImagePaletted *m, ColorPalette global) {
    if (m == NULL)
        return;
    if (m->palette.p != global.p)
        gif_palette_free(a, m->palette);
    image_paletted_free(m, a);
}

/* Everything but the frames, which the caller has either kept or freed. */
static void gif_decoder_release(GifDecoder *d) {
    gif_slice_free(d->a, d->image);
    gif_slice_free(d->a, d->delay);
    gif_slice_free(d->a, d->disposal);
    lzw_reader_free(d->lzw);
    bufio_reader_free(d->own);
    d->image = slice_nil(TYPE_OF(ImagePalettedPtr));
    d->delay = slice_nil(TYPE_INT);
    d->disposal = slice_nil(TYPE_BYTE);
    d->lzw = NULL;
    d->own = NULL;
}

/* After an error: the frames, the global palette and the rest. */
static void gif_decoder_free(GifDecoder *d) {
    ImagePaletted **f = (ImagePaletted **)d->image.p;
    for (Int i = 0; i < d->image.len; i++)
        gif_frame_free(d->a, f[i], d->global_color_table);
    gif_palette_free(d->a, d->global_color_table);
    d->global_color_table = slice_nil(TYPE_OF(Color));
    gif_decoder_release(d);
}

static bool gif_is_eof(Error err) {
    return gif_is(err, io_eof);
}

/* readByte: the next byte, with io.EOF turned into io.ErrUnexpectedEOF. */
static Error gif_read_byte(GifDecoder *d, Byte *c) {
    Error e = BURROW_NO_ERROR;
    if (d->direct != NULL) {
        *c = bufio_reader_read_byte(d->direct, &e);
    } else {
        IoErrorArg ea = &e;
        void *args[1] = {(void *)&ea};
        void *rets[1] = {c};
        d->read_byte->thunk(d->r.data, args, rets);
    }
    if (gif_is_eof(e))
        e = io_err_unexpected_eof;
    return e;
}

/* readFull: n bytes into p, with io.EOF turned into io.ErrUnexpectedEOF. */
static Error gif_read_full(IoReader r, Byte *p, Int n) {
    Error e = BURROW_NO_ERROR;
    io_read_full(r, slice_from(p, n, n, TYPE_BYTE), &e);
    if (gif_is_eof(e))
        e = io_err_unexpected_eof;
    return e;
}

/* blockReader parses the block structure of GIF image data, which comprises
 * (n, (n bytes)) blocks, with 1 <= n <= 255. It is the reader given to the
 * LZW decoder, which is thus immune to the blocking. After the LZW decoder
 * completes, there will be a 0-byte block remaining (0, ()), which is
 * consumed when checking that the blockReader is exhausted.
 *
 * It has a ReadByte method, which the LZW reader finds and reads through, so
 * nothing reads ahead of it. d->tmp[i:j] holds the buffered bytes. */
typedef struct GifBlockReader {
    GifDecoder *d;
    Byte i, j;
    Error err;
} GifBlockReader;

static void gif_block_fill(GifBlockReader *b) {
    if (BURROW_FAILED(b->err))
        return;
    b->err = gif_read_byte(b->d, &b->j);
    if (b->j == 0 && BURROW_OK(b->err))
        b->err = io_eof;
    if (BURROW_FAILED(b->err))
        return;

    b->i = 0;
    b->err = gif_read_full(b->d->r, b->d->tmp, b->j);
    if (BURROW_FAILED(b->err))
        b->j = 0;
}

static Byte gif_block_read_byte(GifBlockReader *b, Error *err) {
    if (b->i == b->j) {
        gif_block_fill(b);
        if (BURROW_FAILED(b->err)) {
            *err = b->err;
            return 0;
        }
    }

    Byte c = b->d->tmp[b->i];
    b->i++;
    *err = BURROW_NO_ERROR;
    return c;
}

/* blockReader must be an io.Reader, but its Read is not called in practice,
 * because the LZW reader only calls ReadByte. */
static Int gif_block_read(void *self, Slice p, Error *err) {
    GifBlockReader *b = (GifBlockReader *)self;
    if (p.len == 0 || BURROW_FAILED(b->err)) {
        *err = b->err;
        return 0;
    }
    if (b->i == b->j) {
        gif_block_fill(b);
        if (BURROW_FAILED(b->err)) {
            *err = b->err;
            return 0;
        }
    }

    Int n = b->j - b->i;
    if (n > p.len)
        n = p.len;
    memcpy(p.p, b->d->tmp + b->i, (size_t)n);
    b->i = (Byte)(b->i + n);
    *err = BURROW_NO_ERROR;
    return n;
}

#define GIF_BLOCK_READER_METHODS(M, T)                                                 \
    M(T, ReadByte, gif_block_read_byte, IO_SIG_READ_BYTE)
BURROW_METHODS_DEFINE(GifBlockReader, GIF_BLOCK_READER_METHODS);

static const Type gif_block_reader_type = {
    {(const Byte *)"blockReader", 11},
    {(const Byte *)"image/gif", 9},
    KIND_STRUCT,
    (uint32_t)sizeof(GifBlockReader),
    (uint16_t)_Alignof(GifBlockReader),
    0,
    (uint16_t)(sizeof burrow__methods_GifBlockReader /
               sizeof burrow__methods_GifBlockReader[0]),
    NULL,
    burrow__methods_GifBlockReader,
    NULL,
    NULL,
    0,
    0x67696272U, /* "gibr" */
    NULL,
};

static const IoReaderVT gif_block_reader_vt = {&gif_block_reader_type, gif_block_read};

/* close primarily detects whether or not a block terminator was encountered
 * after reading a sequence of data sub-blocks. It allows at most one trailing
 * sub-block worth of data. I.e., if some number of bytes exist in one
 * sub-block following the end of LZW data, the very next sub-block must be
 * the block terminator. If the very end of LZW data happened to fill one
 * sub-block, at most one more sub-block of length 1 may exist before the
 * block-terminator. These accommodations allow us to support GIFs created by
 * less strict encoders. See https://golang.org/issue/16146. */
static Error gif_block_close(GifBlockReader *b) {
    if (gif_is_eof(b->err)) {
        /* A clean block-sequence terminator was encountered while
         * reading. */
        return BURROW_NO_ERROR;
    } else if (BURROW_FAILED(b->err)) {
        /* Some other error was encountered while reading. */
        return b->err;
    }

    if (b->i == b->j) {
        /* We reached the end of a sub block reading LZW data. We'll allow at
         * most one more sub block of data with a length of 1 byte. */
        gif_block_fill(b);
        if (gif_is_eof(b->err))
            return BURROW_NO_ERROR;
        else if (BURROW_FAILED(b->err))
            return b->err;
        else if (b->j > 1)
            return gif_err_too_much;
    }

    /* Part of a sub-block remains buffered. We expect that the next attempt
     * to buffer a sub-block will reach the block terminator. */
    gif_block_fill(b);
    if (gif_is_eof(b->err))
        return BURROW_NO_ERROR;
    else if (BURROW_FAILED(b->err))
        return b->err;

    return gif_err_too_much;
}

static Error gif_read_color_table(GifDecoder *d, Byte fields, ColorPalette *out) {
    Int n = (Int)1 << (1 + (fields & GIF_F_COLOR_TABLE_BITS_MASK));
    Error err = gif_read_full(d->r, d->tmp, 3 * n);
    if (BURROW_FAILED(err))
        return gif_wrap("gif: reading color table: ", err);
    ColorPalette p = gif_palette_make(d->a, n);
    if (p.p == NULL)
        return burrow_err_out_of_memory;
    Color *c = (Color *)p.p;
    for (Int i = 0, j = 0; i < n; i++, j += 3)
        c[i] = color_rgba_as_color(
            (ColorRGBA){d->tmp[j + 0], d->tmp[j + 1], d->tmp[j + 2], 0xFF});
    *out = p;
    return BURROW_NO_ERROR;
}

static Error gif_read_header_and_screen_descriptor(GifDecoder *d) {
    Error err = gif_read_full(d->r, d->tmp, 13);
    if (BURROW_FAILED(err))
        return gif_wrap("gif: reading header: ", err);
    if (memcmp(d->tmp, "GIF87a", 6) != 0 && memcmp(d->tmp, "GIF89a", 6) != 0) {
        Alloc *ea = error_allocator();
        return gif_errorf("gif: can't recognize format ",
                          strconv_quote(ea, str_from_bytes(d->tmp, 6)));
    }
    d->width = (Int)d->tmp[6] + ((Int)d->tmp[7] << 8);
    d->height = (Int)d->tmp[8] + ((Int)d->tmp[9] << 8);
    Byte fields = d->tmp[10];
    if ((fields & GIF_F_COLOR_TABLE) != 0) {
        d->background_index = d->tmp[11];
        /* readColorTable overwrites the contents of d.tmp, but that's OK. */
        err = gif_read_color_table(d, fields, &d->global_color_table);
        if (BURROW_FAILED(err))
            return err;
    }
    /* d.tmp[12] is the Pixel Aspect Ratio, which is ignored. */
    return BURROW_NO_ERROR;
}

/* readBlock: the next sub-block into d->tmp, and its length, which is 0 at
 * the terminator. */
static Error gif_read_block(GifDecoder *d, Int *n) {
    Byte c = 0;
    *n = 0;
    Error err = gif_read_byte(d, &c);
    if (c == 0 || BURROW_FAILED(err))
        return err;
    err = gif_read_full(d->r, d->tmp, c);
    if (BURROW_FAILED(err))
        return err;
    *n = c;
    return BURROW_NO_ERROR;
}

static Error gif_read_graphic_control(GifDecoder *d) {
    Error err = gif_read_full(d->r, d->tmp, 6);
    if (BURROW_FAILED(err))
        return gif_wrap("gif: can't read graphic control: ", err);
    if (d->tmp[0] != 4)
        return gif_errorf_num(
            "gif: invalid graphic control extension block size: ", d->tmp[0], false);
    Byte flags = d->tmp[1];
    d->disposal_method = (Byte)((flags & GIF_GC_DISPOSAL_METHOD_MASK) >> 2);
    d->delay_time = (Int)d->tmp[2] | ((Int)d->tmp[3] << 8);
    if ((flags & GIF_GC_TRANSPARENT_COLOR_SET) != 0) {
        d->transparent_index = d->tmp[4];
        d->has_transparent_index = true;
    }
    if (d->tmp[5] != 0)
        return gif_errorf_num(
            "gif: invalid graphic control extension block terminator: ", d->tmp[5],
            false);
    return BURROW_NO_ERROR;
}

static Error gif_read_extension(GifDecoder *d) {
    Byte extension = 0;
    Error err = gif_read_byte(d, &extension);
    if (BURROW_FAILED(err))
        return gif_wrap("gif: reading extension: ", err);
    Int size = 0;
    switch (extension) {
    case GIF_E_TEXT:
        size = 13;
        break;
    case GIF_E_GRAPHIC_CONTROL:
        return gif_read_graphic_control(d);
    case GIF_E_COMMENT:
        /* nothing to do but read the data. */
        break;
    case GIF_E_APPLICATION: {
        Byte b = 0;
        err = gif_read_byte(d, &b);
        if (BURROW_FAILED(err))
            return gif_wrap("gif: reading extension: ", err);
        /* The spec requires size be 11, but Adobe sometimes uses 10. */
        size = b;
        break;
    }
    default:
        return gif_errorf_num("gif: unknown extension ", extension, true);
    }
    if (size > 0) {
        err = gif_read_full(d->r, d->tmp, size);
        if (BURROW_FAILED(err))
            return gif_wrap("gif: reading extension: ", err);
    }

    /* Application Extension with "NETSCAPE2.0" as string and 1 in data means
     * this extension defines a loop count. */
    if (extension == GIF_E_APPLICATION && size == 11 &&
        memcmp(d->tmp, "NETSCAPE2.0", 11) == 0) {
        Int n = 0;
        err = gif_read_block(d, &n);
        if (BURROW_FAILED(err))
            return gif_wrap("gif: reading extension: ", err);
        if (n == 0)
            return BURROW_NO_ERROR;
        if (n == 3 && d->tmp[0] == 1)
            d->loop_count = (Int)d->tmp[1] | ((Int)d->tmp[2] << 8);
    }
    for (;;) {
        Int n = 0;
        err = gif_read_block(d, &n);
        if (BURROW_FAILED(err))
            return gif_wrap("gif: reading extension: ", err);
        if (n == 0)
            return BURROW_NO_ERROR;
    }
}

static Error gif_new_image_from_descriptor(GifDecoder *d, ImagePaletted **out) {
    Error err = gif_read_full(d->r, d->tmp, 9);
    if (BURROW_FAILED(err))
        return gif_wrap("gif: can't read image descriptor: ", err);
    Int left = (Int)d->tmp[0] + ((Int)d->tmp[1] << 8);
    Int top = (Int)d->tmp[2] + ((Int)d->tmp[3] << 8);
    Int width = (Int)d->tmp[4] + ((Int)d->tmp[5] << 8);
    Int height = (Int)d->tmp[6] + ((Int)d->tmp[7] << 8);
    d->image_fields = d->tmp[8];

    /* The GIF89a spec, Section 20 (Image Descriptor) says: "Each image must
     * fit within the boundaries of the Logical Screen, as defined in the
     * Logical Screen Descriptor." Rectangle.In is true of any empty
     * rectangle, so this checks the far corner by hand, and left and top are
     * never negative. */
    if (left + width > d->width || top + height > d->height)
        return gif_err_frame_bounds;
    ImagePaletted *m =
        image_new_paletted(d->a, image_rect(left, top, left + width, top + height),
                           slice_nil(TYPE_OF(Color)));
    if (m == NULL)
        return burrow_err_out_of_memory;
    *out = m;
    return BURROW_NO_ERROR;
}

/* interlacing: the passes of an interlaced GIF, each a row step and the row
 * it starts at. */
static const struct {
    Int skip, start;
} gif_interlacing[4] = {
    {8, 0}, /* Group 1 : Every 8th. row, starting with row 0. */
    {8, 4}, /* Group 2 : Every 8th. row, starting with row 4. */
    {4, 2}, /* Group 3 : Every 4th. row, starting with row 2. */
    {2, 1}, /* Group 4 : Every 2nd. row, starting with row 1. */
};

/* uninterlace rearranges the pixels in m to account for interlaced input. */
static bool gif_uninterlace(Alloc *a, ImagePaletted *m) {
    Int dx = image_rectangle_dx(m->rect);
    Int dy = image_rectangle_dy(m->rect);
    Int n = dx * dy;
    if (n == 0)
        return true;
    Byte *npix = (Byte *)mem_alloc(a, (size_t)n, 1);
    if (npix == NULL)
        return false;
    const Byte *pix = (const Byte *)m->pix.p;
    Int offset = 0; /* steps through the input by sequential scan lines. */
    for (int p = 0; p < 4; p++) {
        /* steps through the output as defined by pass. */
        Int noffset = gif_interlacing[p].start * dx;
        for (Int y = gif_interlacing[p].start; y < dy; y += gif_interlacing[p].skip) {
            memcpy(npix + noffset, pix + offset, (size_t)dx);
            offset += dx;
            noffset += dx * gif_interlacing[p].skip;
        }
    }
    mem_free(a, m->pix.p, (size_t)m->pix.cap, 1);
    m->pix = slice_from(npix, n, n, TYPE_BYTE);
    return true;
}

/* The LZW part of readImageDescriptor: fills m's pixels from the image data
 * sub-blocks and checks what is left after them. */
static Error gif_read_image_data(GifDecoder *d, ImagePaletted *m, Int lit_width) {
    GifBlockReader br = {d, 0, 0, BURROW_NO_ERROR};
    IoReader bri = {&gif_block_reader_vt, &br};
    if (d->lzw == NULL) {
        d->lzw = lzw_new_reader(d->a, bri, LZW_LSB, lit_width);
        if (d->lzw == NULL)
            return burrow_err_out_of_memory;
    } else {
        lzw_reader_reset(d->lzw, bri, LZW_LSB, lit_width);
    }
    IoReader lzwr = lzw_reader_as_io_reader(d->lzw);
    Error err = gif_read_full(lzwr, (Byte *)m->pix.p, m->pix.len);
    if (BURROW_FAILED(err)) {
        if (!gif_is(err, io_err_unexpected_eof))
            return gif_wrap("gif: reading image data: ", err);
        return gif_err_not_enough;
    }
    /* In theory, both lzwr and br should be exhausted. Reading from them
     * should yield (0, io.EOF).
     *
     * The spec (Appendix F - Compression), says that "An End of
     * Information code... must be the last code output by the encoder for
     * an image". In practice, though, giflib (a widely used C library) does
     * not enforce this, so we also accept lzwr returning io.ErrUnexpectedEOF
     * (meaning that the encoded stream hit io.EOF before the LZW decoder saw
     * an explicit end code), provided that the io.ReadFull call above
     * successfully read len(m.Pix) bytes. See https://golang.org/issue/9856
     * for an example GIF. */
    Error e = BURROW_NO_ERROR;
    Int n = lzw_reader_read(d->lzw, slice_from(d->tmp + 256, 1, 1, TYPE_BYTE), &e);
    if (n != 0 || (!gif_is_eof(e) && !gif_is(e, io_err_unexpected_eof))) {
        if (BURROW_FAILED(e))
            return gif_wrap("gif: reading image data: ", e);
        return gif_err_too_much;
    }

    /* In practice, some GIFs have an extra byte in the data sub-block
     * stream, which we ignore. See https://golang.org/issue/16146. */
    err = gif_block_close(&br);
    if (gif_is(err, gif_err_too_much))
        return gif_err_too_much;
    if (BURROW_FAILED(err))
        return gif_wrap("gif: reading image data: ", err);
    return BURROW_NO_ERROR;
}

static Error gif_read_image_descriptor(GifDecoder *d, bool keep_all_frames) {
    ImagePaletted *m = NULL;
    Error err = gif_new_image_from_descriptor(d, &m);
    if (BURROW_FAILED(err))
        return err;
    ColorPalette global = d->global_color_table;
    bool use_local_color_table = (d->image_fields & GIF_F_COLOR_TABLE) != 0;
    if (use_local_color_table) {
        err = gif_read_color_table(d, d->image_fields, &m->palette);
        if (BURROW_FAILED(err))
            goto fail;
    } else {
        if (global.p == NULL) {
            err = gif_err_no_color_table;
            goto fail;
        }
        m->palette = global;
    }
    if (d->has_transparent_index) {
        if (!use_local_color_table) {
            /* Clone the global color table. */
            ColorPalette p = gif_palette_make(d->a, global.len);
            if (p.p == NULL) {
                m->palette = slice_nil(TYPE_OF(Color));
                err = burrow_err_out_of_memory;
                goto fail;
            }
            memcpy(p.p, global.p, (size_t)global.len * sizeof(Color));
            m->palette = p;
        }
        Int ti = d->transparent_index;
        if (ti < m->palette.len) {
            ((Color *)m->palette.p)[ti] = color_rgba_as_color((ColorRGBA){0, 0, 0, 0});
        } else {
            /* The transparentIndex is out of range, which is an error
             * according to the spec, but Firefox and Google Chrome seem OK
             * with this, so we enlarge the palette with transparent colors.
             * See golang.org/issue/15059. */
            ColorPalette p = gif_palette_make(d->a, ti + 1);
            if (p.p == NULL) {
                err = burrow_err_out_of_memory;
                goto fail;
            }
            memcpy(p.p, m->palette.p, (size_t)m->palette.len * sizeof(Color));
            for (Int i = m->palette.len; i < p.len; i++)
                ((Color *)p.p)[i] = color_rgba_as_color((ColorRGBA){0, 0, 0, 0});
            gif_palette_free(d->a, m->palette);
            m->palette = p;
        }
    }
    Byte lit_width = 0;
    err = gif_read_byte(d, &lit_width);
    if (BURROW_FAILED(err)) {
        err = gif_wrap("gif: reading image data: ", err);
        goto fail;
    }
    if (lit_width < 2 || lit_width > 8) {
        err = gif_errorf_num("gif: pixel size in decode out of range: ", lit_width,
                             false);
        goto fail;
    }
    err = gif_read_image_data(d, m, lit_width);
    if (BURROW_FAILED(err))
        goto fail;

    /* Check that the color indexes are inside the palette. */
    if (m->palette.len < 256) {
        const Byte *pix = (const Byte *)m->pix.p;
        for (Int i = 0; i < m->pix.len; i++) {
            if ((Int)pix[i] >= m->palette.len) {
                err = gif_err_bad_pixel;
                goto fail;
            }
        }
    }

    /* Undo the interlacing if necessary. */
    if ((d->image_fields & GIF_F_INTERLACE) != 0 && !gif_uninterlace(d->a, m)) {
        err = burrow_err_out_of_memory;
        goto fail;
    }

    if (keep_all_frames || d->image.len == 0) {
        Byte disposal = d->disposal_method;
        if (!gif_push(d->a, &d->image, &m) ||
            !gif_push(d->a, &d->delay, &d->delay_time) ||
            !gif_push(d->a, &d->disposal, &disposal)) {
            /* The frame may be in d->image already. */
            if (d->image.len > 0 &&
                ((ImagePaletted **)d->image.p)[d->image.len - 1] == m)
                d->image.len--;
            err = burrow_err_out_of_memory;
            goto fail;
        }
    } else {
        gif_frame_free(d->a, m, global);
    }
    /* The GIF89a spec, Section 23 (Graphic Control Extension) says: "The
     * scope of this extension is the first graphic rendering block to
     * follow." We therefore reset the GCE fields to zero. */
    d->delay_time = 0;
    d->has_transparent_index = false;
    return BURROW_NO_ERROR;

fail:
    gif_frame_free(d->a, m, global);
    return err;
}

/* decode reads a GIF image from r and stores the result in d. */
static Error gif_decoder_decode(GifDecoder *d, bool config_only, bool keep_all_frames) {
    Error err = gif_read_header_and_screen_descriptor(d);
    if (BURROW_FAILED(err))
        return err;
    if (config_only)
        return BURROW_NO_ERROR;

    for (;;) {
        Byte c = 0;
        err = gif_read_byte(d, &c);
        if (BURROW_FAILED(err))
            return gif_wrap("gif: reading frames: ", err);
        switch (c) {
        case GIF_S_EXTENSION:
            err = gif_read_extension(d);
            if (BURROW_FAILED(err))
                return err;
            break;

        case GIF_S_IMAGE_DESCRIPTOR:
            err = gif_read_image_descriptor(d, keep_all_frames);
            if (BURROW_FAILED(err))
                return err;
            if (!keep_all_frames && d->image.len == 1)
                return BURROW_NO_ERROR;
            break;

        case GIF_S_TRAILER:
            if (d->image.len == 0)
                return gif_err_missing_image;
            return BURROW_NO_ERROR;

        default:
            return gif_errorf_num("gif: unknown block type: ", c, true);
        }
    }
}

Image gif_decode(Alloc *a, IoReader r, Error *err) {
    GifDecoder d;
    Error e = gif_decoder_init(&d, a, r);
    if (BURROW_OK(e))
        e = gif_decoder_decode(&d, false, false);
    if (BURROW_FAILED(e)) {
        gif_decoder_free(&d);
        BURROW_OUT(err, e);
        return (Image){NULL, NULL};
    }
    ImagePaletted *m = ((ImagePaletted **)d.image.p)[0];
    /* The global palette now belongs to the frame, when it uses it. */
    if (m->palette.p != d.global_color_table.p)
        gif_palette_free(a, d.global_color_table);
    gif_decoder_release(&d);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return image_paletted_as_image(m);
}

Gif *gif_decode_all(Alloc *a, IoReader r, Error *err) {
    GifDecoder d;
    Error e = gif_decoder_init(&d, a, r);
    if (BURROW_OK(e))
        e = gif_decoder_decode(&d, false, true);
    Gif *g = NULL;
    if (BURROW_OK(e)) {
        g = (Gif *)mem_alloc(a, sizeof *g, _Alignof(Gif));
        if (g == NULL ||
            !gif_palette_model(a, d.global_color_table, &g->config.color_model)) {
            if (g != NULL)
                mem_free(a, g, sizeof *g, _Alignof(Gif));
            g = NULL;
            e = burrow_err_out_of_memory;
        }
    }
    if (BURROW_FAILED(e) || g == NULL) {
        gif_decoder_free(&d);
        BURROW_OUT(err, e);
        return NULL;
    }
    g->image = d.image;
    g->loop_count = d.loop_count;
    g->delay = d.delay;
    g->disposal = d.disposal;
    g->config.width = d.width;
    g->config.height = d.height;
    g->background_index = d.background_index;
    /* The slices now belong to g. */
    d.image = slice_nil(TYPE_OF(ImagePalettedPtr));
    d.delay = slice_nil(TYPE_INT);
    d.disposal = slice_nil(TYPE_BYTE);
    gif_decoder_release(&d);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return g;
}

void gif_free(Gif *g, Alloc *a) {
    if (g == NULL)
        return;
    const ColorPalette *gp = NULL;
    ColorPalette global = slice_nil(TYPE_OF(Color));
    if (color_model_as_palette(g->config.color_model, &gp) && gp != NULL)
        global = *gp;
    ImagePaletted **f = (ImagePaletted **)g->image.p;
    for (Int i = 0; i < g->image.len; i++)
        gif_frame_free(a, f[i], global);
    gif_slice_free(a, g->image);
    gif_slice_free(a, g->delay);
    gif_slice_free(a, g->disposal);
    image_config_free(g->config, a);
    mem_free(a, g, sizeof *g, _Alignof(Gif));
}

ImageConfig gif_decode_config(Alloc *a, IoReader r, Error *err) {
    ImageConfig c;
    memset(&c, 0, sizeof c);
    GifDecoder d;
    Error e = gif_decoder_init(&d, a, r);
    if (BURROW_OK(e))
        e = gif_decoder_decode(&d, true, false);
    if (BURROW_OK(e) && !gif_palette_model(a, d.global_color_table, &c.color_model))
        e = burrow_err_out_of_memory;
    if (BURROW_FAILED(e)) {
        gif_decoder_free(&d);
        BURROW_OUT(err, e);
        return c;
    }
    c.width = d.width;
    c.height = d.height;
    gif_decoder_release(&d);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return c;
}

static Image gif_decode_fn(void *env, Alloc *a, IoReader r, Error *err) {
    (void)env;
    return gif_decode(a, r, err);
}

static ImageConfig gif_decode_config_fn(void *env, Alloc *a, IoReader r, Error *err) {
    (void)env;
    return gif_decode_config(a, r, err);
}

static void gif_do_register(void *env) {
    (void)env;
    image_register_format(BURROW_S("gif"), BURROW_S("GIF8?a"),
                          BURROW_FN(ImageDecodeFunc, gif_decode_fn, NULL),
                          BURROW_FN(ImageDecodeConfigFunc, gif_decode_config_fn, NULL));
}

static SyncOnce gif_registered;

void gif_register(void) {
    sync_once_do(&gif_registered, BURROW_FN(Func, gif_do_register, NULL));
}

/* ---------------------------------------------------------------- encoder */

static Int gif_log2(Int x) {
    if (x < 2)
        return 0;
    /* bits.Len(uint(x-1)) - 1 */
    Uint v = (Uint)(x - 1);
    Int n = 0;
    while (v != 0) {
        n++;
        v >>= 1;
    }
    return n - 1;
}

/* encoder encodes an image to the GIF format. */
typedef struct GifEncoder {
    Alloc *a;
    /* w is the writer to write to. err is the first error encountered during
     * writing. All attempted writes after the first error become no-ops. own
     * is the bufio.Writer made here when w is not one. */
    BufioWriter *w;
    BufioWriter *own;
    Error err;
    /* g is a copy of the Gif being encoded, with its config filled in. */
    Gif g;
    /* The global palette, when the config's model is one. */
    const ColorPalette *global;
    /* globalCT is the size in bytes of the global color table. */
    Int global_ct;
    LzwWriter *lzw;
    /* buf is a scratch buffer. It must be at least 256 for the blockWriter. */
    Byte buf[256];
    Byte global_color_table[3 * 256];
    Byte local_color_table[3 * 256];
} GifEncoder;

static void gif_flush(GifEncoder *e) {
    if (BURROW_FAILED(e->err))
        return;
    e->err = bufio_writer_flush(e->w);
}

static void gif_write(GifEncoder *e, const Byte *p, Int n) {
    if (BURROW_FAILED(e->err))
        return;
    Byte *q;
    memcpy(&q, &p, sizeof q);
    bufio_writer_write(e->w, slice_from(q, n, n, TYPE_BYTE), &e->err);
}

static void gif_write_byte(GifEncoder *e, Byte b) {
    if (BURROW_FAILED(e->err))
        return;
    e->err = bufio_writer_write_byte(e->w, b);
}

static void gif_put_le16(Byte *b, Int v) {
    b[0] = (Byte)v;
    b[1] = (Byte)((Uint)v >> 8);
}

/* blockWriter writes the block structure of GIF image data, which comprises
 * (n, (n bytes)) blocks, with 1 <= n <= 255. It is the writer given to the
 * LZW encoder, which is thus immune to the blocking. */
static Error gif_block_write_byte(GifEncoder *e, Byte c) {
    if (BURROW_FAILED(e->err))
        return e->err;

    /* Append c to buffered sub-block. */
    e->buf[0]++;
    e->buf[e->buf[0]] = c;
    if (e->buf[0] < 255)
        return BURROW_NO_ERROR;

    /* Flush block */
    gif_write(e, e->buf, 256);
    e->buf[0] = 0;
    return e->err;
}

static Int gif_block_write(void *self, Slice p, Error *err) {
    GifEncoder *e = (GifEncoder *)self;
    const Byte *b = (const Byte *)p.p;
    for (Int i = 0; i < p.len; i++) {
        Error x = gif_block_write_byte(e, b[i]);
        if (BURROW_FAILED(x)) {
            *err = x;
            return i;
        }
    }
    *err = BURROW_NO_ERROR;
    return p.len;
}

static const IoWriterVT gif_block_writer_vt = {NULL, gif_block_write};

static void gif_block_close_writer(GifEncoder *e) {
    /* Write the block terminator (0x00), either by itself, or along with a
     * pending sub-block. */
    if (e->buf[0] == 0) {
        gif_write_byte(e, 0);
    } else {
        Int n = e->buf[0];
        e->buf[n + 1] = 0;
        gif_write(e, e->buf, n + 2);
    }
    gif_flush(e);
}

static Error gif_encode_color_table(Byte *dst, ColorPalette p, Int size, Int *n) {
    *n = 0;
    if ((Uint)size >= 8)
        return gif_err_table_too_big;
    const Color *c = (const Color *)p.p;
    for (Int i = 0; i < p.len; i++) {
        if (c[i].vt == NULL)
            return gif_err_nil_entry;
        ColorRGBAValue v = color_rgba(c[i]);
        dst[3 * i + 0] = (Byte)(v.r >> 8);
        dst[3 * i + 1] = (Byte)(v.g >> 8);
        dst[3 * i + 2] = (Byte)(v.b >> 8);
    }
    Int count = (Int)1 << (size + 1);
    if (count > p.len) {
        /* Pad with black. */
        memset(dst + 3 * p.len, 0, (size_t)(3 * (count - p.len)));
    }
    *n = 3 * count;
    return BURROW_NO_ERROR;
}

static void gif_write_header(GifEncoder *e) {
    if (BURROW_FAILED(e->err))
        return;
    gif_write(e, (const Byte *)"GIF89a", 6);
    if (BURROW_FAILED(e->err))
        return;

    /* Logical screen width and height. */
    gif_put_le16(e->buf + 0, e->g.config.width);
    gif_put_le16(e->buf + 2, e->g.config.height);
    gif_write(e, e->buf, 4);

    if (e->global != NULL && e->global->len > 0) {
        Int padded_size =
            gif_log2(e->global->len); /* Size of Global Color Table: 2^(1+n). */
        e->buf[0] = (Byte)(GIF_F_COLOR_TABLE | (Byte)padded_size);
        e->buf[1] = e->g.background_index;
        e->buf[2] = 0x00; /* Pixel Aspect Ratio. */
        gif_write(e, e->buf, 3);
        Error err = gif_encode_color_table(e->global_color_table, *e->global,
                                           padded_size, &e->global_ct);
        if (BURROW_FAILED(err) && BURROW_OK(e->err)) {
            e->err = err;
            return;
        }
        gif_write(e, e->global_color_table, e->global_ct);
    } else {
        /* All frames have a local color table, so a global color table is not
         * needed. */
        e->buf[0] = 0x00;
        e->buf[1] = 0x00; /* Background Color Index. */
        e->buf[2] = 0x00; /* Pixel Aspect Ratio. */
        gif_write(e, e->buf, 3);
    }

    /* Add animation info if necessary. */
    if (e->g.image.len > 1 && e->g.loop_count >= 0) {
        e->buf[0] = 0x21; /* Extension Introducer. */
        e->buf[1] = 0xff; /* Application Label. */
        e->buf[2] = 0x0b; /* Block Size. */
        gif_write(e, e->buf, 3);
        Error err = BURROW_NO_ERROR;
        bufio_writer_write(
            e->w, slice_from((void *)(uintptr_t)"NETSCAPE2.0", 11, 11, TYPE_BYTE),
            &err); /* Application Identifier. */
        if (BURROW_FAILED(err) && BURROW_OK(e->err)) {
            e->err = err;
            return;
        }
        e->buf[0] = 0x03; /* Block Size. */
        e->buf[1] = 0x01; /* Sub-block Index. */
        gif_put_le16(e->buf + 2, e->g.loop_count);
        e->buf[4] = 0x00; /* Block Terminator. */
        gif_write(e, e->buf, 5);
    }
}

static bool gif_color_tables_match(GifEncoder *e, Int local_len,
                                   Int transparent_index) {
    Int local_size = 3 * local_len;
    if (transparent_index >= 0) {
        Int tr_off = 3 * transparent_index;
        return memcmp(e->global_color_table, e->local_color_table, (size_t)tr_off) ==
                   0 &&
               memcmp(e->global_color_table + tr_off + 3,
                      e->local_color_table + tr_off + 3,
                      (size_t)(local_size - tr_off - 3)) == 0;
    }
    return memcmp(e->global_color_table, e->local_color_table, (size_t)local_size) == 0;
}

static void gif_write_image_block(GifEncoder *e, const ImagePaletted *pm, Int delay,
                                  Byte disposal) {
    if (BURROW_FAILED(e->err))
        return;

    if (pm->palette.len == 0) {
        e->err = gif_err_empty_palette;
        return;
    }

    ImageRectangle b = pm->rect;
    if (b.min.x < 0 || b.max.x >= 1 << 16 || b.min.y < 0 || b.max.y >= 1 << 16) {
        e->err = gif_err_block_too_large;
        return;
    }
    if (!image_rectangle_in(b,
                            image_rect(0, 0, e->g.config.width, e->g.config.height))) {
        e->err = gif_err_block_out_of_bounds;
        return;
    }

    Int transparent_index = -1;
    const Color *pal = (const Color *)pm->palette.p;
    for (Int i = 0; i < pm->palette.len; i++) {
        if (pal[i].vt == NULL) {
            e->err = gif_err_nil_entry;
            return;
        }
        if (color_rgba(pal[i]).a == 0) {
            transparent_index = i;
            break;
        }
    }

    if (delay > 0 || disposal != 0 || transparent_index != -1) {
        e->buf[0] = GIF_S_EXTENSION;   /* Extension Introducer. */
        e->buf[1] = GIF_GC_LABEL;      /* Graphic Control Label. */
        e->buf[2] = GIF_GC_BLOCK_SIZE; /* Block Size. */
        if (transparent_index != -1)
            e->buf[3] = (Byte)(0x01 | (Byte)(disposal << 2));
        else
            e->buf[3] = (Byte)(0x00 | (Byte)(disposal << 2));
        gif_put_le16(e->buf + 4, delay); /* Delay Time (1/100ths of a second) */

        /* Transparent color index. */
        if (transparent_index != -1)
            e->buf[6] = (Byte)transparent_index;
        else
            e->buf[6] = 0x00;
        e->buf[7] = 0x00; /* Block Terminator. */
        gif_write(e, e->buf, 8);
    }
    e->buf[0] = GIF_S_IMAGE_DESCRIPTOR;
    gif_put_le16(e->buf + 1, b.min.x);
    gif_put_le16(e->buf + 3, b.min.y);
    gif_put_le16(e->buf + 5, image_rectangle_dx(b));
    gif_put_le16(e->buf + 7, image_rectangle_dy(b));
    gif_write(e, e->buf, 9);

    /* To determine whether or not this frame's palette is the same as the
     * global palette, we can check a couple things. First, do they actually
     * point to the same []color.Color? If so, they are equal so long as the
     * frame's palette is not longer than the global palette... */
    Int padded_size =
        gif_log2(pm->palette.len); /* Size of Local Color Table: 2^(1+n). */
    if (e->global != NULL && pm->palette.len <= e->global->len &&
        e->global->p == pm->palette.p) {
        gif_write_byte(e, 0); /* Use the global color table. */
    } else {
        Int ct = 0;
        Error err =
            gif_encode_color_table(e->local_color_table, pm->palette, padded_size, &ct);
        if (BURROW_FAILED(err)) {
            if (BURROW_OK(e->err))
                e->err = err;
            return;
        }
        /* This frame's palette is not the very same slice as the global
         * palette, but it might be a copy, possibly with one value turned into
         * transparency by DecodeAll. */
        if (ct <= e->global_ct &&
            gif_color_tables_match(e, pm->palette.len, transparent_index)) {
            gif_write_byte(e, 0); /* Use the global color table. */
        } else {
            /* Use a local color table. */
            gif_write_byte(e, (Byte)(GIF_F_COLOR_TABLE | (Byte)padded_size));
            gif_write(e, e->local_color_table, ct);
        }
    }

    Int lit_width = padded_size + 1;
    if (lit_width < 2)
        lit_width = 2;
    gif_write_byte(e, (Byte)lit_width); /* LZW Minimum Code Size. */

    e->buf[0] = 0;
    IoWriter bw = {&gif_block_writer_vt, e};
    if (e->lzw == NULL) {
        e->lzw = lzw_new_writer(e->a, bw, LZW_LSB, lit_width);
        if (e->lzw == NULL) {
            e->err = burrow_err_out_of_memory;
            return;
        }
    } else {
        lzw_writer_reset(e->lzw, bw, LZW_LSB, lit_width);
    }
    Int dx = image_rectangle_dx(b);
    Byte *pix = (Byte *)pm->pix.p;
    if (dx == pm->stride) {
        lzw_writer_write(e->lzw,
                         slice_from(pix, dx * image_rectangle_dy(b),
                                    dx * image_rectangle_dy(b), TYPE_BYTE),
                         &e->err);
        if (BURROW_FAILED(e->err)) {
            (void)lzw_writer_close(e->lzw);
            return;
        }
    } else {
        for (Int i = 0, y = b.min.y; y < b.max.y; i += pm->stride, y++) {
            lzw_writer_write(e->lzw, slice_from(pix + i, dx, dx, TYPE_BYTE), &e->err);
            if (BURROW_FAILED(e->err)) {
                (void)lzw_writer_close(e->lzw);
                return;
            }
        }
    }
    (void)lzw_writer_close(e->lzw); /* flush to bw */
    gif_block_close_writer(e);      /* flush to e.w */
}

Error gif_encode_all(Alloc *a, IoWriter w, const Gif *g) {
    if (g->image.len == 0)
        return gif_err_no_images;

    if (g->image.len != g->delay.len)
        return gif_err_mismatched_delay;

    GifEncoder *e = (GifEncoder *)mem_alloc(a, sizeof *e, _Alignof(GifEncoder));
    if (e == NULL)
        return burrow_err_out_of_memory;
    e->a = a;
    e->g = *g;
    /* The GIF.Disposal, GIF.Config and GIF.BackgroundIndex fields were added
     * in Go 1.5. Valid Go 1.4 code, such as when the Disposal field is
     * omitted in a GIF struct literal, should still produce valid GIFs. */
    Error err = BURROW_NO_ERROR;
    if (!slice_is_nil(e->g.disposal) && e->g.image.len != e->g.disposal.len) {
        err = gif_err_mismatched_disposal;
        goto out;
    }
    ImagePaletted *const *frames = (ImagePaletted *const *)g->image.p;
    if (e->g.config.color_model.vt == NULL && e->g.config.width == 0 &&
        e->g.config.height == 0) {
        ImagePoint p = frames[0]->rect.max;
        e->g.config.width = p.x;
        e->g.config.height = p.y;
    } else if (e->g.config.color_model.vt != NULL) {
        if (!color_model_as_palette(e->g.config.color_model, &e->global)) {
            err = gif_err_model;
            goto out;
        }
    }

    if (w.vt != NULL && w.vt->self_type == TYPE_BUFIO_WRITER) {
        e->w = (BufioWriter *)w.data;
    } else {
        e->own = bufio_new_writer(a, w);
        if (e->own == NULL) {
            err = burrow_err_out_of_memory;
            goto out;
        }
        e->w = e->own;
    }

    gif_write_header(e);
    const Int *delays = (const Int *)g->delay.p;
    for (Int i = 0; i < g->image.len; i++) {
        Byte disposal = 0;
        if (!slice_is_nil(g->disposal))
            disposal = ((const Byte *)g->disposal.p)[i];
        gif_write_image_block(e, frames[i], delays[i], disposal);
    }
    gif_write_byte(e, GIF_S_TRAILER);
    gif_flush(e);
    err = e->err;

out:
    lzw_writer_free(e->lzw);
    bufio_writer_free(e->own);
    mem_free(a, e, sizeof *e, _Alignof(GifEncoder));
    return err;
}

Error gif_encode(Alloc *a, IoWriter w, Image m, const GifOptions *o) {
    /* Check for bounds and size restrictions. */
    ImageRectangle b = image_bounds(m);
    if (image_rectangle_dx(b) >= 1 << 16 || image_rectangle_dy(b) >= 1 << 16)
        return gif_err_too_large;

    GifOptions opts;
    memset(&opts, 0, sizeof opts);
    if (o != NULL)
        opts = *o;
    if (opts.num_colors < 1 || 256 < opts.num_colors)
        opts.num_colors = 256;
    if (opts.drawer.vt == NULL)
        opts.drawer = draw_floyd_steinberg;

    /* made is a paletted image allocated here, and quant the palette handed
     * to the quantizer. */
    ImagePaletted *made = NULL;
    ColorPalette quant = slice_nil(TYPE_OF(Color));
    ImagePaletted *pm = NULL;
    if (m.vt != NULL && m.vt->self_type == TYPE_OF(ImagePaletted))
        pm = (ImagePaletted *)m.data;
    if (pm == NULL) {
        const ColorPalette *cp = NULL;
        if (color_model_as_palette(image_color_model(m), &cp)) {
            made = image_new_paletted(a, b, *cp);
            if (made == NULL)
                return burrow_err_out_of_memory;
            for (Int y = b.min.y; y < b.max.y; y++)
                for (Int x = b.min.x; x < b.max.x; x++)
                    image_paletted_set(made, x, y,
                                       color_palette_convert(*cp, image_at(m, x, y)));
            pm = made;
        }
    }
    if (pm == NULL || pm->palette.len > opts.num_colors) {
        /* Set pm to be a palettedized copy of m, including its bounds, which
         * might not start at (0, 0).
         *
         * TODO: Pick a better sub-sample of the Plan 9 palette. */
        image_paletted_free(made, a);
        ColorPalette plan9 = palette_plan9;
        made = image_new_paletted(a, b, slice_sub(plan9, 0, opts.num_colors));
        if (made == NULL)
            return burrow_err_out_of_memory;
        if (opts.quantizer.vt != NULL) {
            quant = slice_make(a, TYPE_OF(Color), 0, opts.num_colors);
            if (quant.p == NULL) {
                image_paletted_free(made, a);
                return burrow_err_out_of_memory;
            }
            made->palette = draw_quantizer_quantize(opts.quantizer, quant, m);
        }
        pm = made;
        draw_drawer_draw(opts.drawer, image_paletted_as_image(made), b, m, b.min);
    }

    /* When calling Encode instead of EncodeAll, the single-frame image is
     * translated such that its top-left corner is (0, 0), so that the single
     * frame completely fills the overall GIF's bounds. */
    ImagePaletted dup = *pm;
    if (!image_point_eq(dup.rect.min, image_pt(0, 0)))
        dup.rect = image_rectangle_sub(dup.rect, dup.rect.min);

    ImagePalettedPtr frames[1] = {&dup};
    Int delays[1] = {0};
    Gif g;
    memset(&g, 0, sizeof g);
    g.image = slice_from(frames, 1, 1, TYPE_OF(ImagePalettedPtr));
    g.delay = slice_from(delays, 1, 1, TYPE_INT);
    g.disposal = slice_nil(TYPE_BYTE);
    g.config.color_model = color_palette_as_model(&dup.palette);
    g.config.width = image_rectangle_dx(b);
    g.config.height = image_rectangle_dy(b);
    Error err = gif_encode_all(a, w, &g);

    gif_palette_free(a, quant);
    image_paletted_free(made, a);
    return err;
}

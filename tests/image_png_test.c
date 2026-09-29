/* Derived from Go's src/image/png/reader_test.go and writer_test.go. Go
 * source: go1.27.1.
 *
 * tests/image_png_test_gen.h, from tools/gen-image-png-tests.sh, holds what
 * Go's image/png does in two tables. Each encode case is a seed for the
 * xorshift generator below, which builds an image of a random type, size and
 * content, sometimes a sub-image of it and sometimes behind a type with only
 * the Image methods. The case holds the length and a digest of the PNG Go
 * writes at each compression level, then what Go's Decode and DecodeConfig
 * make of the default one. The decode cases take the PNG files from Go's
 * testdata, PngSuite and the invalid ones among them, flip bytes or cut them
 * short, and hold what Decode and DecodeConfig say about each. Go's own tests
 * compare the suite against text dumps and round trip a few images, and all
 * of that is covered here byte for byte.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/hash/crc32.h"
#include "burrow/image/png.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"

#include <stdio.h>
#include <string.h>

typedef struct PngEncodeCase {
    uint64_t seed;
    const char *want;
} PngEncodeCase;

typedef struct PngFile {
    const char *name;
    const Byte *data;
    size_t len;
} PngFile;

typedef struct PngDecodeCase {
    int file;
    uint64_t seed;
    const char *want;
    const char *want_slow; /* through a one-byte reader, when it differs */
} PngDecodeCase;

#include "image_png_test_gen.h"

/* --------------------------------------------------------- the generator */

typedef struct Xs {
    uint64_t x;
} Xs;

static uint64_t xs_next(Xs *r) {
    r->x ^= r->x << 13;
    r->x ^= r->x >> 7;
    r->x ^= r->x << 17;
    return r->x;
}

static Int xs_intn(Xs *r, Int n) {
    return (Int)(xs_next(r) % (uint64_t)n);
}

#define U8(x) ((uint8_t)(x))
#define U16(x) ((uint16_t)(x))

static Color xcolor(Xs *r, Int k) {
    uint64_t v[4];
    for (int i = 0; i < 4; i++)
        v[i] = xs_next(r);
    switch (k) {
    case 0:
        return color_rgba_as_color((ColorRGBA){U8(v[0]), U8(v[1]), U8(v[2]), U8(v[3])});
    case 1:
        return color_rgba64_as_color(
            (ColorRGBA64){U16(v[0]), U16(v[1]), U16(v[2]), U16(v[3])});
    case 2:
        return color_nrgba_as_color(
            (ColorNRGBA){U8(v[0]), U8(v[1]), U8(v[2]), U8(v[3])});
    case 3:
        return color_nrgba64_as_color(
            (ColorNRGBA64){U16(v[0]), U16(v[1]), U16(v[2]), U16(v[3])});
    case 4:
        return color_alpha_as_color((ColorAlpha){U8(v[0])});
    case 5:
        return color_alpha16_as_color((ColorAlpha16){U16(v[0])});
    case 6:
        return color_gray_as_color((ColorGray){U8(v[0])});
    case 7:
        return color_gray16_as_color((ColorGray16){U16(v[0])});
    case 8:
        return color_y_cb_cr_as_color((ColorYCbCr){U8(v[0]), U8(v[1]), U8(v[2])});
    case 9:
        return color_ny_cb_cr_a_as_color(
            (ColorNYCbCrA){{U8(v[0]), U8(v[1]), U8(v[2])}, U8(v[3])});
    default:
        return color_cmyk_as_color((ColorCMYK){U8(v[0]), U8(v[1]), U8(v[2]), U8(v[3])});
    }
}

static void fill(Xs *r, Slice b) {
    for (Int i = 0; i < b.len; i++)
        ((Byte *)b.p)[i] = (Byte)xs_next(r);
}

/* Sets the n-byte alpha sample at off in every bpp-byte pixel to all ones. */
static void opaque_fill(Slice b, Int bpp, Int off, Int n) {
    for (Int i = 0; i + bpp <= b.len; i += bpp)
        memset((Byte *)b.p + i + off, 0xff, (size_t)n);
}

/* ------------------------------------------------------ plain and plainPal
 *
 * An image with only ColorModel, Bounds and At, and one that adds
 * ColorIndexAt, so that the encoder cannot see the type underneath. */

static const Type plain_type = {
    {(const Byte *)"plain", 5},
    {(const Byte *)"main", 4},
    KIND_STRUCT,
    (uint32_t)sizeof(Image),
    (uint16_t)_Alignof(Image),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x706c6e33U,
    NULL,
};

static const Type plain_pal_type = {
    {(const Byte *)"plainPal", 8},
    {(const Byte *)"main", 4},
    KIND_STRUCT,
    (uint32_t)sizeof(ImagePaletted *),
    (uint16_t)_Alignof(ImagePaletted *),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x706c6e34U,
    NULL,
};

static ColorModel plain_color_model(void *self) {
    Image *m = (Image *)self;
    return m->vt->color_model(m->data);
}
static ImageRectangle plain_bounds(void *self) {
    Image *m = (Image *)self;
    return m->vt->bounds(m->data);
}
static Color plain_at(void *self, Int x, Int y) {
    Image *m = (Image *)self;
    return m->vt->at(m->data, x, y);
}

static const ImageVT plain_vt = {
    &plain_type, plain_color_model, plain_bounds, plain_at, NULL, NULL, NULL, NULL,
    NULL,
};

static ColorModel plain_pal_color_model(void *self) {
    return image_paletted_color_model((const ImagePaletted *)self);
}
static ImageRectangle plain_pal_bounds(void *self) {
    return image_paletted_bounds((const ImagePaletted *)self);
}
static Color plain_pal_at(void *self, Int x, Int y) {
    return image_paletted_at((const ImagePaletted *)self, x, y);
}
static uint8_t plain_pal_color_index_at(void *self, Int x, Int y) {
    return image_paletted_color_index_at((const ImagePaletted *)self, x, y);
}

static const ImageVT plain_pal_vt = {
    &plain_pal_type,
    plain_pal_color_model,
    plain_pal_bounds,
    plain_pal_at,
    NULL,
    plain_pal_color_index_at,
    NULL,
    NULL,
    NULL,
};

/* ------------------------------------------------------------ the images */

enum {
    K_RGBA,
    K_RGBA64,
    K_NRGBA,
    K_NRGBA64,
    K_ALPHA,
    K_ALPHA16,
    K_GRAY,
    K_GRAY16,
    K_CMYK,
    K_PALETTED,
    K_Y_CB_CR,
    K_NY_CB_CR_A,
    K_PLAIN,
    K_PLAIN_PAL,
};

static ColorPalette xpalette(Alloc *a, Xs *r) {
    Int n;
    switch (xs_intn(r, 5)) {
    case 0:
        n = 1 + xs_intn(r, 2);
        break;
    case 1:
        n = 3 + xs_intn(r, 2);
        break;
    case 2:
        n = 5 + xs_intn(r, 12);
        break;
    case 3:
        n = 17 + xs_intn(r, 240);
        break;
    default:
        if (xs_intn(r, 3) == 0)
            n = 257 + xs_intn(r, 8);
        else
            n = 1 + xs_intn(r, 256);
    }
    Color *c = BURROW_NEW_N(a, Color, (size_t)n);
    for (Int i = 0; i < n; i++) {
        Int k = xs_intn(r, 11);
        c[i] = xcolor(r, k);
    }
    ColorPalette p = {c, n, n, &burrow_type_Color};
    return p;
}

/* Copies a sub-image into a and makes it an Image. */
#define SUB(T, pre, m, sr)                                                             \
    do {                                                                               \
        T *s_ = BURROW_NEW(a, T);                                                      \
        *s_ = image_##pre##_sub_image(m, sr);                                          \
        img = image_##pre##_as_image(s_);                                              \
    } while (0)

/* The generator's build, draw for draw. */
static Image build(Alloc *a, Xs *r) {
    Int k = xs_intn(r, 14);
    Int x0 = xs_intn(r, 9) - 4;
    Int y0 = xs_intn(r, 9) - 4;
    Int inner = k;
    if (k == K_PLAIN)
        inner = xs_intn(r, 9);
    if (inner == K_Y_CB_CR || inner == K_NY_CB_CR_A)
        x0 += 4, y0 += 4;
    Int w = xs_intn(r, 40);
    Int h = xs_intn(r, 40);
    ImageRectangle rect = image_rect(x0, y0, x0 + w, y0 + h);
    bool opaque = xs_intn(r, 2) == 0;

    Image img = {NULL, NULL};
    ImageRGBA *rgba = NULL;
    ImageRGBA64 *rgba64 = NULL;
    ImageNRGBA *nrgba = NULL;
    ImageNRGBA64 *nrgba64 = NULL;
    ImageAlpha *alpha = NULL;
    ImageAlpha16 *alpha16 = NULL;
    ImageGray *gray = NULL;
    ImageGray16 *gray16 = NULL;
    ImageCMYK *cmyk = NULL;
    ImagePaletted *pal = NULL;
    ImageYCbCr *ycc = NULL;
    ImageNYCbCrA *nycca = NULL;
    switch (inner) {
    case K_RGBA:
        rgba = image_new_rgba(a, rect);
        fill(r, rgba->pix);
        if (opaque)
            opaque_fill(rgba->pix, 4, 3, 1);
        img = image_rgba_as_image(rgba);
        break;
    case K_RGBA64:
        rgba64 = image_new_rgba64(a, rect);
        fill(r, rgba64->pix);
        if (opaque)
            opaque_fill(rgba64->pix, 8, 6, 2);
        img = image_rgba64_as_image(rgba64);
        break;
    case K_NRGBA:
        nrgba = image_new_nrgba(a, rect);
        fill(r, nrgba->pix);
        if (opaque)
            opaque_fill(nrgba->pix, 4, 3, 1);
        img = image_nrgba_as_image(nrgba);
        break;
    case K_NRGBA64:
        nrgba64 = image_new_nrgba64(a, rect);
        fill(r, nrgba64->pix);
        if (opaque)
            opaque_fill(nrgba64->pix, 8, 6, 2);
        img = image_nrgba64_as_image(nrgba64);
        break;
    case K_ALPHA:
        alpha = image_new_alpha(a, rect);
        fill(r, alpha->pix);
        if (opaque)
            opaque_fill(alpha->pix, 1, 0, 1);
        img = image_alpha_as_image(alpha);
        break;
    case K_ALPHA16:
        alpha16 = image_new_alpha16(a, rect);
        fill(r, alpha16->pix);
        if (opaque)
            opaque_fill(alpha16->pix, 2, 0, 2);
        img = image_alpha16_as_image(alpha16);
        break;
    case K_GRAY:
        gray = image_new_gray(a, rect);
        fill(r, gray->pix);
        img = image_gray_as_image(gray);
        break;
    case K_GRAY16:
        gray16 = image_new_gray16(a, rect);
        fill(r, gray16->pix);
        img = image_gray16_as_image(gray16);
        break;
    case K_CMYK:
        cmyk = image_new_cmyk(a, rect);
        fill(r, cmyk->pix);
        img = image_cmyk_as_image(cmyk);
        break;
    case K_PALETTED:
    case K_PLAIN_PAL:
        pal = image_new_paletted(a, rect, xpalette(a, r));
        fill(r, pal->pix);
        img = image_paletted_as_image(pal);
        break;
    case K_Y_CB_CR:
        ycc = image_new_y_cb_cr(a, rect, xs_intn(r, 6));
        fill(r, ycc->y);
        fill(r, ycc->cb);
        fill(r, ycc->cr);
        img = image_y_cb_cr_as_image(ycc);
        break;
    default:
        nycca = image_new_ny_cb_cr_a(a, rect, xs_intn(r, 6));
        fill(r, nycca->y_cb_cr.y);
        fill(r, nycca->y_cb_cr.cb);
        fill(r, nycca->y_cb_cr.cr);
        fill(r, nycca->a);
        if (opaque)
            opaque_fill(nycca->a, 1, 0, 1);
        img = image_ny_cb_cr_a_as_image(nycca);
        break;
    }
    if (xs_intn(r, 3) == 0 && !image_rectangle_empty(rect)) {
        Int sx0 = x0 + xs_intn(r, image_rectangle_dx(rect));
        Int sy0 = y0 + xs_intn(r, image_rectangle_dy(rect));
        Int sx1 = sx0 + xs_intn(r, rect.max.x - sx0 + 1);
        Int sy1 = sy0 + xs_intn(r, rect.max.y - sy0 + 1);
        ImageRectangle sr = image_rect(sx0, sy0, sx1, sy1);
        switch (inner) {
        case K_RGBA:
            SUB(ImageRGBA, rgba, rgba, sr);
            break;
        case K_RGBA64:
            SUB(ImageRGBA64, rgba64, rgba64, sr);
            break;
        case K_NRGBA:
            SUB(ImageNRGBA, nrgba, nrgba, sr);
            break;
        case K_NRGBA64:
            SUB(ImageNRGBA64, nrgba64, nrgba64, sr);
            break;
        case K_ALPHA:
            SUB(ImageAlpha, alpha, alpha, sr);
            break;
        case K_ALPHA16:
            SUB(ImageAlpha16, alpha16, alpha16, sr);
            break;
        case K_GRAY:
            SUB(ImageGray, gray, gray, sr);
            break;
        case K_GRAY16:
            SUB(ImageGray16, gray16, gray16, sr);
            break;
        case K_CMYK:
            SUB(ImageCMYK, cmyk, cmyk, sr);
            break;
        case K_PALETTED:
        case K_PLAIN_PAL: {
            ImagePaletted *s = BURROW_NEW(a, ImagePaletted);
            *s = image_paletted_sub_image(pal, sr);
            pal = s;
            img = image_paletted_as_image(s);
            break;
        }
        case K_Y_CB_CR:
            SUB(ImageYCbCr, y_cb_cr, ycc, sr);
            break;
        default:
            SUB(ImageNYCbCrA, ny_cb_cr_a, nycca, sr);
            break;
        }
    }
    if (k == K_PLAIN) {
        Image *p = BURROW_NEW(a, Image);
        *p = img;
        img = (Image){&plain_vt, p};
    } else if (k == K_PLAIN_PAL) {
        img = (Image){&plain_pal_vt, pal};
    }
    return img;
}

#undef SUB

/* ------------------------------------------------------------ summaries */

static uint64_t fnv(const Byte *p, Int n) {
    uint64_t h = UINT64_C(14695981039346656037);
    for (Int i = 0; i < n; i++) {
        h ^= p[i];
        h *= UINT64_C(1099511628211);
    }
    return h;
}

static uint64_t fnv_slice(Slice b) {
    return fnv((const Byte *)b.p, b.len);
}

/* A Slice over read only bytes, which the readers only read. */
static Slice cslice(const Byte *p, Int n) {
    Byte *q;
    memcpy(&q, &p, sizeof q);
    return slice_from(q, n, n, TYPE_BYTE);
}

/* Appends to a summary, which is always long enough. */
typedef struct Sum {
    char buf[1024];
    size_t n;
} Sum;

#define SUMF(s, ...)                                                                   \
    ((s)->n +=                                                                         \
     (size_t)snprintf((s)->buf + (s)->n, sizeof(s)->buf - (s)->n, __VA_ARGS__))

static void put_be32(Byte *b, uint32_t v) {
    b[0] = (Byte)(v >> 24);
    b[1] = (Byte)(v >> 16);
    b[2] = (Byte)(v >> 8);
    b[3] = (Byte)v;
}

static void sum_palette(Sum *s, const ColorPalette *p) {
    const Color *c = (const Color *)p->p;
    uint64_t h = UINT64_C(14695981039346656037);
    for (Int i = 0; i < p->len; i++) {
        Byte b[17];
        if (c[i].vt == &burrow__color_rgba_vt)
            b[0] = 'r';
        else if (c[i].vt == &burrow__color_nrgba_vt)
            b[0] = 'n';
        else
            b[0] = '?';
        ColorRGBAValue v = color_rgba(c[i]);
        put_be32(b + 1, v.r);
        put_be32(b + 5, v.g);
        put_be32(b + 9, v.b);
        put_be32(b + 13, v.a);
        for (int j = 0; j < 17; j++) {
            h ^= b[j];
            h *= UINT64_C(1099511628211);
        }
    }
    SUMF(s, "p%d:%016llx", (int)p->len, (unsigned long long)h);
}

static void sum_error(Sum *s, Error err) {
    Str t = error_text(err);
    SUMF(s, "E:%.*s", (int)t.len, (const char *)t.p);
}

static void sum_image(Sum *s, Image m) {
    ImageRectangle b = m.vt->bounds(m.data);
    const Type *t = m.vt->self_type;
    const char *name = "?";
    Slice pix = slice_nil(TYPE_BYTE);
    if (t == TYPE_OF(ImageGray))
        name = "Gray", pix = ((ImageGray *)m.data)->pix;
    else if (t == TYPE_OF(ImageGray16))
        name = "Gray16", pix = ((ImageGray16 *)m.data)->pix;
    else if (t == TYPE_OF(ImageRGBA))
        name = "RGBA", pix = ((ImageRGBA *)m.data)->pix;
    else if (t == TYPE_OF(ImageRGBA64))
        name = "RGBA64", pix = ((ImageRGBA64 *)m.data)->pix;
    else if (t == TYPE_OF(ImageNRGBA))
        name = "NRGBA", pix = ((ImageNRGBA *)m.data)->pix;
    else if (t == TYPE_OF(ImageNRGBA64))
        name = "NRGBA64", pix = ((ImageNRGBA64 *)m.data)->pix;
    else if (t == TYPE_OF(ImagePaletted))
        name = "Paletted", pix = ((ImagePaletted *)m.data)->pix;
    SUMF(s, "%s %dx%d %016llx", name, (int)image_rectangle_dx(b),
         (int)image_rectangle_dy(b), (unsigned long long)fnv_slice(pix));
    if (t == TYPE_OF(ImagePaletted)) {
        SUMF(s, " ");
        sum_palette(s, &((ImagePaletted *)m.data)->palette);
    }
}

static bool model_eq(ColorModel x, ColorModel y) {
    return x.vt == y.vt && x.data == y.data;
}

static void sum_config(Sum *s, ImageConfig c) {
    SUMF(s, "%dx%d ", (int)c.width, (int)c.height);
    const ColorPalette *p;
    if (model_eq(c.color_model, color_gray_model))
        SUMF(s, "gray");
    else if (model_eq(c.color_model, color_gray16_model))
        SUMF(s, "gray16");
    else if (model_eq(c.color_model, color_rgba_model))
        SUMF(s, "rgba");
    else if (model_eq(c.color_model, color_rgba64_model))
        SUMF(s, "rgba64");
    else if (model_eq(c.color_model, color_nrgba_model))
        SUMF(s, "nrgba");
    else if (model_eq(c.color_model, color_nrgba64_model))
        SUMF(s, "nrgba64");
    else if (color_model_as_palette(c.color_model, &p))
        sum_palette(s, p);
    else
        SUMF(s, "?");
}

/* A reader that gives at most one byte a call, like Go's iotest.OneByteReader,
 * so that every read in the decoder has to cope with short reads. */
typedef struct OneByte {
    IoReader r;
} OneByte;

static Int one_byte_read(void *self, Slice p, Error *err) {
    OneByte *o = (OneByte *)self;
    if (p.len == 0)
        return 0;
    return o->r.vt->read(o->r.data, slice_sub(p, 0, 1), err);
}

static const IoReaderVT one_byte_vt = {NULL, one_byte_read};

/* What Decode and then DecodeConfig make of data, the way the generator puts
 * it, read through a one-byte reader when slow is set. */
static void sum_decode(Sum *s, Alloc *a, const Byte *data, Int n, bool slow) {
    BytesReader br;
    bytes_reader_reset(&br, cslice(data, n));
    OneByte ob = {bytes_reader_as_io_reader(&br)};
    IoReader r = slow ? (IoReader){&one_byte_vt, &ob} : ob.r;
    Error err = BURROW_NO_ERROR;
    Image m = png_decode(a, r, &err);
    if (BURROW_FAILED(err)) {
        sum_error(s, err);
        if (m.vt != NULL)
            SUMF(s, " (and an image)");
    } else {
        sum_image(s, m);
        image_decoded_free(m, a);
    }
    SUMF(s, " | ");
    bytes_reader_reset(&br, cslice(data, n));
    err = BURROW_NO_ERROR;
    ImageConfig c = png_decode_config(a, r, &err);
    if (BURROW_FAILED(err)) {
        sum_error(s, err);
    } else {
        sum_config(s, c);
        image_config_free(c, a);
    }
}

/* ------------------------------------------------------------- the tests */

static const PngCompressionLevel levels[] = {PNG_DEFAULT_COMPRESSION,
                                             PNG_NO_COMPRESSION, PNG_BEST_SPEED,
                                             PNG_BEST_COMPRESSION, 7};

static void TestEncodeRandom(TestingT *t) {
    int bad = 0;
    size_t n = sizeof png_encode_cases / sizeof png_encode_cases[0];
    for (size_t i = 0; i < n && bad < 20; i++) {
        const PngEncodeCase *tc = &png_encode_cases[i];
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        Sum s;
        s.n = 0;
        s.buf[0] = 0;
        BytesBuffer first = BYTES_BUFFER(a);
        for (size_t l = 0; l < sizeof levels / sizeof levels[0]; l++) {
            Xs r = {tc->seed};
            Image m = build(a, &r);
            BytesBuffer out = BYTES_BUFFER(a);
            PngEncoder enc = {levels[l], {NULL, NULL}};
            if (l > 0)
                SUMF(&s, " ");
            Error err = png_encoder_encode(&enc, a, bytes_buffer_as_io_writer(&out), m);
            if (BURROW_FAILED(err)) {
                sum_error(&s, err);
                bytes_buffer_free(&out);
                continue;
            }
            Slice b = bytes_buffer_bytes(&out);
            SUMF(&s, "%d:%016llx", (int)b.len, (unsigned long long)fnv_slice(b));
            if (l == 0)
                first = out;
            else
                bytes_buffer_free(&out);
        }
        if (bytes_buffer_len(&first) > 0) {
            Slice b = bytes_buffer_bytes(&first);
            SUMF(&s, " | ");
            sum_decode(&s, a, (const Byte *)b.p, b.len, false);
        }
        bytes_buffer_free(&first);
        if (strcmp(s.buf, tc->want) != 0) {
            testing_t_errorf_v(t, "case %d (seed %#x):\n got %s\nwant %s", (int)i,
                               (unsigned long long)tc->seed, s.buf, tc->want);
            bad++;
        }
        arena_free(&ar);
    }
}

/* The generator's fixCRC. */
static void fix_crc(Byte *b, Int n) {
    Int off = 8;
    while (off + 12 <= n) {
        uint64_t l = ((uint64_t)b[off] << 24) | ((uint64_t)b[off + 1] << 16) |
                     ((uint64_t)b[off + 2] << 8) | b[off + 3];
        if (l > (uint64_t)(n - off - 12))
            break;
        Int end = off + 8 + (Int)l;
        uint32_t c = crc32_update(
            0, crc32_ieee_table,
            slice_from(b + off + 4, end - off - 4, end - off - 4, TYPE_BYTE));
        put_be32(b + end, c);
        off = end + 4;
    }
}

/* The generator's mutate, in place. The new length is returned. */
static Int mutate(Byte *b, Int n, uint64_t seed) {
    if (seed == 0)
        return n;
    Xs r = {seed};
    Int op = xs_intn(&r, 3);
    if (op == 2)
        return xs_intn(&r, n);
    Int k = 1 + xs_intn(&r, 3);
    for (Int i = 0; i < k; i++) {
        Int pos = 8 + xs_intn(&r, n - 8);
        Int x = 1 + xs_intn(&r, 255);
        b[pos] ^= (Byte)x;
    }
    if (op == 0)
        fix_crc(b, n);
    return n;
}

static void TestDecodeFiles(TestingT *t) {
    int bad = 0;
    size_t n = sizeof png_decode_cases / sizeof png_decode_cases[0];
    for (size_t i = 0; i < n && bad < 20; i++) {
        const PngDecodeCase *tc = &png_decode_cases[i];
        const PngFile *f = &png_files[tc->file];
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        Byte *b = BURROW_NEW_N(a, Byte, f->len);
        memcpy(b, f->data, f->len);
        Int len = mutate(b, (Int)f->len, tc->seed);
        for (int slow = 0; slow < 2; slow++) {
            Sum s;
            s.n = 0;
            s.buf[0] = 0;
            sum_decode(&s, a, b, len, slow != 0);
            const char *want = slow && tc->want_slow != NULL ? tc->want_slow : tc->want;
            if (strcmp(s.buf, want) != 0) {
                testing_t_errorf_v(t, "%s, seed %#x, slow %d:\n got %s\nwant %s",
                                   f->name, (unsigned long long)tc->seed, slow, s.buf,
                                   want);
                bad++;
            }
        }
        arena_free(&ar);
    }
}

/* Decoding with the heap allocator leaves nothing behind once the image and
 * config are given back, whether the file is good or not. The leak checker in
 * the sanitizer build is what catches a miss. */
static void TestDecodeFree(TestingT *t) {
    (void)t;
    Alloc *a = heap_allocator();
    for (size_t i = 0; i < sizeof png_decode_cases / sizeof png_decode_cases[0];
         i += 7) {
        const PngDecodeCase *tc = &png_decode_cases[i];
        const PngFile *f = &png_files[tc->file];
        Byte *b = BURROW_NEW_N(a, Byte, f->len);
        memcpy(b, f->data, f->len);
        Int len = mutate(b, (Int)f->len, tc->seed);
        Sum s;
        s.n = 0;
        sum_decode(&s, a, b, len, false);
        CHECK(strcmp(s.buf, tc->want) == 0);
        mem_free(a, b, f->len, 1);
    }
}

/* image_decode finds PNG once png_register has run, and errors come out as
 * the types Go has. */
static void TestRegisterAndErrors(TestingT *t) {
    (void)t;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    png_register();
    png_register();

    const PngFile *f = &png_files[0];
    BytesReader br;
    bytes_reader_reset(&br, cslice(f->data, (Int)f->len));
    Error err = BURROW_NO_ERROR;
    Str format = BURROW_STR_EMPTY;
    Image m = image_decode(a, bytes_reader_as_io_reader(&br), &format, &err);
    CHECK(BURROW_OK(err));
    CHECK(str_eq(format, BURROW_S("png")));
    CHECK(m.vt != NULL);
    image_decoded_free(m, a);

    /* A bad checksum is a FormatError, which errors_as finds. */
    Byte *b = BURROW_NEW_N(a, Byte, f->len);
    memcpy(b, f->data, f->len);
    b[f->len - 1] ^= 1;
    bytes_reader_reset(&br, slice_from(b, (Int)f->len, (Int)f->len, TYPE_BYTE));
    m = png_decode(a, bytes_reader_as_io_reader(&br), &err);
    CHECK(BURROW_FAILED(err));
    CHECK(str_eq(error_text(err), BURROW_S("png: invalid format: invalid checksum")));
    const PngFormatError *fe = errors_as(err, TYPE_PNG_FORMAT_ERROR);
    CHECK(fe != NULL && str_eq(*fe, BURROW_S("invalid checksum")));
    CHECK(errors_as(err, TYPE_PNG_UNSUPPORTED_ERROR) == NULL);
    CHECK(errors_is(err, png_format_error_as_error(BURROW_S("invalid checksum"), a)));
    CHECK(!errors_is(err, png_format_error_as_error(BURROW_S("not a PNG file"), a)));
    CHECK(str_eq(png_unsupported_error_error(BURROW_S("x"), a),
                 BURROW_S("png: unsupported feature: x")));

    /* Cut short in the header is io.ErrUnexpectedEOF, and so is empty. */
    bytes_reader_reset(&br, cslice(f->data, 4));
    m = png_decode(a, bytes_reader_as_io_reader(&br), &err);
    CHECK(errors_is(err, io_err_unexpected_eof));
    bytes_reader_reset(&br, cslice(f->data, 0));
    m = png_decode(a, bytes_reader_as_io_reader(&br), &err);
    CHECK(errors_is(err, io_err_unexpected_eof));
    arena_free(&ar);
}

/* A pool that keeps one buffer. */
typedef struct OnePool {
    PngEncoderBuffer *b;
    int gets, puts;
} OnePool;

static PngEncoderBuffer *one_pool_get(void *self) {
    OnePool *p = (OnePool *)self;
    PngEncoderBuffer *b = p->b;
    p->b = NULL;
    p->gets++;
    return b;
}

static void one_pool_put(void *self, PngEncoderBuffer *b) {
    OnePool *p = (OnePool *)self;
    png_encoder_buffer_free(p->b);
    p->b = b;
    p->puts++;
}

static const PngEncoderBufferPoolVT one_pool_vt = {NULL, one_pool_get, one_pool_put};

/* An encoder with a pool writes the same bytes as one without, with the buffer
 * going round the pool, and with a heap allocator nothing is left over. */
static void TestEncoderPool(TestingT *t) {
    (void)t;
    Alloc *a = heap_allocator();
    OnePool pool = {NULL, 0, 0};
    PngEncoder with = {PNG_BEST_SPEED, {&one_pool_vt, &pool}};
    PngEncoder without = {PNG_BEST_SPEED, {NULL, NULL}};
    int encoded = 0;
    for (size_t i = 0; i < 200; i++) {
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *ia = arena_allocator(&ar);
        Xs r = {png_encode_cases[i].seed};
        Image m = build(ia, &r);
        BytesBuffer x = BYTES_BUFFER(a), y = BYTES_BUFFER(a);
        Error e1 = png_encoder_encode(&with, a, bytes_buffer_as_io_writer(&x), m);
        Error e2 = png_encoder_encode(&without, a, bytes_buffer_as_io_writer(&y), m);
        CHECK(BURROW_OK(e1) == BURROW_OK(e2));
        if (BURROW_OK(e1)) {
            Slice bx = bytes_buffer_bytes(&x), by = bytes_buffer_bytes(&y);
            CHECK(bx.len == by.len && memcmp(bx.p, by.p, (size_t)bx.len) == 0);
            encoded++;
        }
        bytes_buffer_free(&x);
        bytes_buffer_free(&y);
        arena_free(&ar);
    }
    CHECK(encoded > 100);
    CHECK(pool.gets == pool.puts && pool.gets >= encoded);
    png_encoder_buffer_free(pool.b);
}

/* Go's TestWriterLevels and the zero-size cases from TestWriteErrors. */
static void TestEncodeErrors(TestingT *t) {
    (void)t;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer out = BYTES_BUFFER(a);
    ImageNRGBA *m = image_new_nrgba(a, image_rect(0, 0, 0, 10));
    Error err = png_encode(a, bytes_buffer_as_io_writer(&out), image_nrgba_as_image(m));
    CHECK(str_eq(error_text(err),
                 BURROW_S("png: invalid format: invalid image size: 0x10")));
    CHECK(errors_as(err, TYPE_PNG_FORMAT_ERROR) != NULL);

    Color c[257];
    for (int i = 0; i < 257; i++)
        c[i] = color_gray_as_color((ColorGray){(uint8_t)i});
    ColorPalette p = {c, 257, 257, &burrow_type_Color};
    ImagePaletted *pm = image_new_paletted(a, image_rect(0, 0, 1, 1), p);
    err = png_encode(a, bytes_buffer_as_io_writer(&out), image_paletted_as_image(pm));
    CHECK(str_eq(error_text(err),
                 BURROW_S("png: invalid format: bad palette length: 257")));
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestEncodeRandom)                                                                \
    X(TestDecodeFiles)                                                                 \
    X(TestDecodeFree)                                                                  \
    X(TestRegisterAndErrors)                                                           \
    X(TestEncoderPool)                                                                 \
    X(TestEncodeErrors)

TESTING_MAIN(TESTS)

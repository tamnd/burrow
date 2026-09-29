/* Derived from Go's src/image/gif/reader_test.go and writer_test.go. Go
 * source: go1.27.1.
 *
 * tests/image_gif_test_gen.h, from tools/gen-image-gif-tests.sh, holds what
 * Go's image/gif does in three tables. Each Encode case is a seed for the
 * xorshift generator below, which builds an image of a random type, size and
 * content, as the png tests do, and picks the number of colors, the drawer and
 * sometimes a quantizer. Each EncodeAll case is a seed for an animation of a
 * few frames with random palettes, bounds, delays, disposal methods, loop
 * count and global palette, some of them wrong on purpose. Both hold the
 * length and a digest of the GIF Go writes, then what Go's DecodeAll makes of
 * it. The decode cases take the GIF files from Go's testdata and two made by
 * the generator, flip bytes or cut them short, and hold what Decode, DecodeAll
 * and DecodeConfig say about each. Go's own tests check a few hand made files
 * and round trips, and all of that is covered here byte for byte.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/image/gif.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"

#include <stdio.h>
#include <string.h>

typedef struct GifEncodeCase {
    uint64_t seed;
    const char *want;
} GifEncodeCase;

typedef struct GifFile {
    const char *name;
    const Byte *data;
    size_t len;
} GifFile;

typedef struct GifDecodeCase {
    int file;
    uint64_t seed;
    const char *want;
    const char *want_slow; /* through a one-byte reader, when it differs */
} GifDecodeCase;

#include "image_gif_test_gen.h"

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
        /* At goes past the palette on an index outside it, so keep them
         * inside. */
        for (Int i = 0; i < pal->pix.len; i++)
            ((Byte *)pal->pix.p)[i] =
                (Byte)(((Byte *)pal->pix.p)[i] % pal->palette.len);
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
    char buf[8192];
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

static void sum_frame(Sum *s, const ImagePaletted *m) {
    ImageRectangle b = m->rect;
    SUMF(s, "%d,%d,%d,%d %016llx ", (int)b.min.x, (int)b.min.y, (int)b.max.x,
         (int)b.max.y, (unsigned long long)fnv_slice(m->pix));
    sum_palette(s, &m->palette);
}

static void sum_gif(Sum *s, const Gif *g) {
    const ColorPalette *p = NULL;
    SUMF(s, "%dx%d ", (int)g->config.width, (int)g->config.height);
    if (color_model_as_palette(g->config.color_model, &p))
        sum_palette(s, p);
    else
        SUMF(s, "?");
    SUMF(s, " loop %d bg %d", (int)g->loop_count, (int)g->background_index);
    ImagePaletted *const *f = (ImagePaletted *const *)g->image.p;
    for (Int i = 0; i < g->image.len; i++) {
        SUMF(s, " [");
        sum_frame(s, f[i]);
        SUMF(s, " d%d s%d]", (int)((const Int *)g->delay.p)[i],
             (int)((const Byte *)g->disposal.p)[i]);
    }
}

static IoReader open_reader(BytesReader *br, OneByte *ob, const Byte *data, Int n,
                            bool slow) {
    bytes_reader_reset(br, cslice(data, n));
    ob->r = bytes_reader_as_io_reader(br);
    return slow ? (IoReader){&one_byte_vt, ob} : ob->r;
}

static void sum_decode_all(Sum *s, Alloc *a, const Byte *data, Int n, bool slow) {
    BytesReader br;
    OneByte ob;
    Error err = BURROW_NO_ERROR;
    Gif *g = gif_decode_all(a, open_reader(&br, &ob, data, n, slow), &err);
    if (BURROW_FAILED(err)) {
        sum_error(s, err);
        if (g != NULL)
            SUMF(s, " (and a gif)");
        return;
    }
    sum_gif(s, g);
    gif_free(g, a);
}

/* What Decode, DecodeAll and DecodeConfig make of data, the way the generator
 * puts it, read through a one-byte reader when slow is set. */
static void sum_decode(Sum *s, Alloc *a, const Byte *data, Int n, bool slow) {
    BytesReader br;
    OneByte ob;
    Error err = BURROW_NO_ERROR;
    Image m = gif_decode(a, open_reader(&br, &ob, data, n, slow), &err);
    if (BURROW_FAILED(err)) {
        sum_error(s, err);
        if (m.vt != NULL)
            SUMF(s, " (and an image)");
    } else {

        sum_frame(s, (const ImagePaletted *)m.data);
        image_decoded_free(m, a);
    }
    SUMF(s, " | ");
    sum_decode_all(s, a, data, n, slow);
    SUMF(s, " | ");
    err = BURROW_NO_ERROR;
    ImageConfig c = gif_decode_config(a, open_reader(&br, &ob, data, n, slow), &err);
    if (BURROW_FAILED(err)) {
        sum_error(s, err);
    } else {
        const ColorPalette *p = NULL;
        SUMF(s, "%dx%d ", (int)c.width, (int)c.height);
        if (color_model_as_palette(c.color_model, &p))
            sum_palette(s, p);
        else
            SUMF(s, "?");
        image_config_free(c, a);
    }
}

/* What the generator's encoded says: the length and digest of the output
 * and what DecodeAll makes of it, or the error. */
static void sum_encoded(Sum *s, Alloc *a, BytesBuffer *out, Error err) {
    if (BURROW_FAILED(err)) {
        sum_error(s, err);
        return;
    }
    Slice b = bytes_buffer_bytes(out);
    SUMF(s, "%d:%016llx | ", (int)b.len, (unsigned long long)fnv_slice(b));
    sum_decode_all(s, a, (const Byte *)b.p, b.len, false);
}

/* ------------------------------------------------------------ encoding */

/* The generator's grayQuantizer: evenly spaced grays, as many as p has room
 * for. */
static ColorPalette gray_quantize(void *self, ColorPalette p, Image m) {
    (void)self;
    (void)m;
    Int n = p.cap;
    for (Int i = 0; i < n; i++) {
        Int v = n > 1 ? i * 255 / (n - 1) : 255;
        ((Color *)p.p)[p.len++] = color_gray_as_color((ColorGray){(uint8_t)v});
    }
    return p;
}

static const DrawQuantizerVT gray_quantizer_vt = {NULL, gray_quantize};

static const DrawOp op_src = DRAW_SRC;

static void TestEncodeRandom(TestingT *t) {
    int bad = 0;
    size_t n = sizeof gif_encode_cases / sizeof gif_encode_cases[0];
    for (size_t i = 0; i < n && bad < 20; i++) {
        const GifEncodeCase *tc = &gif_encode_cases[i];
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        Xs r = {tc->seed};
        Image m = build(a, &r);
        GifOptions o;
        memset(&o, 0, sizeof o);
        switch (xs_intn(&r, 4)) {
        case 1:
            o.num_colors = 1 + xs_intn(&r, 256);
            break;
        case 2:
            o.num_colors = 1 + xs_intn(&r, 16);
            break;
        case 3:
            o.num_colors = 300;
            break;
        default:
            break;
        }
        switch (xs_intn(&r, 3)) {
        case 1:
            o.drawer = draw_floyd_steinberg;
            break;
        case 2:
            o.drawer = draw_op_as_drawer(&op_src);
            break;
        default:
            break;
        }
        if (xs_intn(&r, 3) == 0)
            o.quantizer = (DrawQuantizer){&gray_quantizer_vt, NULL};
        BytesBuffer out = BYTES_BUFFER(a);
        Error err = gif_encode(a, bytes_buffer_as_io_writer(&out), m, &o);
        Sum s;
        s.n = 0;
        s.buf[0] = 0;
        sum_encoded(&s, a, &out, err);
        if (strcmp(s.buf, tc->want) != 0) {
            testing_t_errorf_v(t, "case %d (seed %#x):\n got %s\nwant %s", (int)i,
                               (unsigned long long)tc->seed, s.buf, tc->want);
            bad++;
        }
        arena_free(&ar);
    }
}

/* The generator's gifPalette. */
static ColorPalette gif_palette(Alloc *a, Xs *r) {
    Int n = 1 + xs_intn(r, 16);
    if (xs_intn(r, 2) == 0)
        n = 1 + xs_intn(r, 256);
    Color *c = BURROW_NEW_N(a, Color, (size_t)n);
    for (Int i = 0; i < n; i++) {
        Int k = xs_intn(r, 11);
        c[i] = xcolor(r, k);
    }
    if (xs_intn(r, 4) == 0)
        c[xs_intn(r, n)] = color_rgba_as_color((ColorRGBA){0, 0, 0, 0});
    ColorPalette p = {c, n, n, &burrow_type_Color};
    return p;
}

/* The generator's frame. */
static ImagePaletted *frame(Alloc *a, Xs *r, Int w, Int h, ColorPalette p) {
    Int x0 = xs_intn(r, w);
    Int y0 = xs_intn(r, h);
    Int x1 = x0 + xs_intn(r, w - x0 + 1);
    Int y1 = y0 + xs_intn(r, h - y0 + 1);
    if (xs_intn(r, 10) == 0)
        x1 += 1 + xs_intn(r, 3);
    ImagePaletted *m = image_new_paletted(a, image_rect(x0, y0, x1, y1), p);
    if (xs_intn(r, 10) == 0) {
        fill(r, m->pix);
    } else {
        for (Int i = 0; i < m->pix.len; i++)
            ((Byte *)m->pix.p)[i] = (Byte)xs_intn(r, p.len);
    }
    return m;
}

/* The generator's buildAll, with every slice in a. */
static Gif build_all(Alloc *a, Xs *r) {
    Int w = 1 + xs_intn(r, 40);
    Int h = 1 + xs_intn(r, 40);
    Int n = 1 + xs_intn(r, 4);
    Gif g;
    memset(&g, 0, sizeof g);
    g.loop_count = xs_intn(r, 5) - 1;
    g.background_index = (Byte)xs_intn(r, 4);
    Int mode = xs_intn(r, 6);
    ColorPalette *global = BURROW_NEW(a, ColorPalette);
    *global = slice_nil(TYPE_OF(Color));
    if (mode >= 1 && mode <= 3)
        *global = gif_palette(a, r);
    ImagePalettedPtr *frames = BURROW_NEW_N(a, ImagePalettedPtr, 4);
    Int *delays = BURROW_NEW_N(a, Int, 5);
    Byte *disposals = BURROW_NEW_N(a, Byte, 5);
    for (Int i = 0; i < n; i++) {
        ColorPalette p = *global;
        if (mode == 1 || mode == 3 || (mode == 2 && i > 0 && xs_intn(r, 2) == 0) ||
            global->p == NULL)
            p = gif_palette(a, r);
        frames[i] = frame(a, r, w, h, p);
        Int d = 0;
        if (xs_intn(r, 3) != 0)
            d = xs_intn(r, 500);
        delays[i] = d;
    }
    if (mode == 3) {
        ColorPalette p0 = frames[0]->palette;
        Color *c = BURROW_NEW_N(a, Color, (size_t)p0.len);
        memcpy(c, p0.p, (size_t)p0.len * sizeof(Color));
        *global = (ColorPalette){c, p0.len, p0.len, &burrow_type_Color};
    }
    g.image = slice_from(frames, n, 4, TYPE_OF(ImagePalettedPtr));
    g.delay = slice_from(delays, n, 5, TYPE_INT);
    g.disposal = slice_nil(TYPE_BYTE);
    if (xs_intn(r, 2) == 0) {
        for (Int i = 0; i < n; i++)
            disposals[i] = (Byte)xs_intn(r, 4);
        g.disposal = slice_from(disposals, n, 5, TYPE_BYTE);
    }
    switch (mode) {
    case 1:
    case 2:
    case 3:
        g.config = (ImageConfig){color_palette_as_model(global), w, h};
        break;
    case 4:
        g.config = (ImageConfig){{NULL, NULL}, w, h};
        break;
    case 5:
        if (xs_intn(r, 4) == 0)
            g.config = (ImageConfig){color_rgba_model, w, h};
        break;
    default:
        break;
    }
    switch (xs_intn(r, 20)) {
    case 0:
        delays[n] = 0;
        g.delay.len = n + 1;
        break;
    case 1:
        memset(disposals, 0, 5);
        g.disposal = slice_from(disposals, n + 1, 5, TYPE_BYTE);
        break;
    default:
        break;
    }
    return g;
}

static void TestEncodeAllRandom(TestingT *t) {
    int bad = 0;
    size_t n = sizeof gif_encode_all_cases / sizeof gif_encode_all_cases[0];
    for (size_t i = 0; i < n && bad < 20; i++) {
        const GifEncodeCase *tc = &gif_encode_all_cases[i];
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        Xs r = {tc->seed};
        Gif g = build_all(a, &r);
        BytesBuffer out = BYTES_BUFFER(a);
        Error err = gif_encode_all(a, bytes_buffer_as_io_writer(&out), &g);
        Sum s;
        s.n = 0;
        s.buf[0] = 0;
        sum_encoded(&s, a, &out, err);
        if (strcmp(s.buf, tc->want) != 0) {
            testing_t_errorf_v(t, "case %d (seed %#x):\n got %s\nwant %s", (int)i,
                               (unsigned long long)tc->seed, s.buf, tc->want);
            bad++;
        }
        arena_free(&ar);
    }
}

/* ------------------------------------------------------------ decoding */

/* The generator's mutate, in place. The new length is returned. */
static Int mutate(Byte *b, Int n, uint64_t seed) {
    if (seed == 0)
        return n;
    Xs r = {seed};
    if (xs_intn(&r, 3) == 2)
        return xs_intn(&r, n);
    Int k = 1 + xs_intn(&r, 3);
    for (Int i = 0; i < k; i++) {
        Int pos = 6 + xs_intn(&r, n - 6);
        Int x = 1 + xs_intn(&r, 255);
        b[pos] ^= (Byte)x;
    }
    return n;
}

static void TestDecodeFiles(TestingT *t) {
    int bad = 0;
    size_t n = sizeof gif_decode_cases / sizeof gif_decode_cases[0];
    for (size_t i = 0; i < n && bad < 20; i++) {
        const GifDecodeCase *tc = &gif_decode_cases[i];
        const GifFile *f = &gif_files[tc->file];
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

/* Decoding with the heap allocator leaves nothing behind once the image, the
 * Gif and the config are given back, whether the file is good or not. The
 * leak checker in the sanitizer build is what catches a miss. */
static void TestDecodeFree(TestingT *t) {
    (void)t;
    Alloc *a = heap_allocator();
    for (size_t i = 0; i < sizeof gif_decode_cases / sizeof gif_decode_cases[0];
         i += 3) {
        const GifDecodeCase *tc = &gif_decode_cases[i];
        const GifFile *f = &gif_files[tc->file];
        Byte *b = BURROW_NEW_N(a, Byte, f->len);
        memcpy(b, f->data, f->len);
        Int len = mutate(b, (Int)f->len, tc->seed);
        Sum s;
        s.n = 0;
        s.buf[0] = 0;
        sum_decode(&s, a, b, len, i % 2 == 1);
        CHECK(strcmp(s.buf, i % 2 == 1 && tc->want_slow != NULL ? tc->want_slow
                                                                : tc->want) == 0);
        mem_free(a, b, f->len, 1);
    }
}

/* Encoding with the heap allocator gives back all it takes, errors included,
 * and so does decoding what it wrote. */
static void TestEncodeFree(TestingT *t) {
    (void)t;
    Alloc *a = heap_allocator();
    for (size_t i = 0; i < 300; i++) {
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *ia = arena_allocator(&ar);
        Xs r = {gif_encode_all_cases[i].seed};
        Gif g = build_all(ia, &r);
        BytesBuffer out = BYTES_BUFFER(a);
        Error err = gif_encode_all(a, bytes_buffer_as_io_writer(&out), &g);
        Sum s;
        s.n = 0;
        s.buf[0] = 0;
        sum_encoded(&s, a, &out, err);
        CHECK(strcmp(s.buf, gif_encode_all_cases[i].want) == 0);
        bytes_buffer_free(&out);

        r = (Xs){gif_encode_cases[i].seed};
        Image m = build(ia, &r);
        GifOptions o;
        memset(&o, 0, sizeof o);
        o.num_colors = 1 + (Int)(i % 40);
        if (i % 3 == 0)
            o.quantizer = (DrawQuantizer){&gray_quantizer_vt, NULL};
        out = BYTES_BUFFER(a);
        err = gif_encode(a, bytes_buffer_as_io_writer(&out), m, &o);
        (void)err;
        bytes_buffer_free(&out);
        arena_free(&ar);
    }
}

/* image_decode finds GIF once gif_register has run, a writer that is already
 * a bufio.Writer is written to directly, and the encoder's argument checks
 * give Go's errors. */
static void TestRegisterAndErrors(TestingT *t) {
    (void)t;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    gif_register();
    gif_register();

    const GifFile *f = &gif_files[0];
    BytesReader br;
    bytes_reader_reset(&br, cslice(f->data, (Int)f->len));
    Error err = BURROW_NO_ERROR;
    Str format = BURROW_STR_EMPTY;
    Image m = image_decode(a, bytes_reader_as_io_reader(&br), &format, &err);
    CHECK(BURROW_OK(err));
    CHECK(str_eq(format, BURROW_S("gif")));
    CHECK(m.vt != NULL);
    bytes_reader_reset(&br, cslice(f->data, (Int)f->len));
    ImageConfig c =
        image_decode_config(a, bytes_reader_as_io_reader(&br), &format, &err);
    CHECK(BURROW_OK(err));
    CHECK(c.width == image_rectangle_dx(image_bounds(m)));

    /* Encode writes through a bufio.Writer it is handed without another one
     * in front, so nothing reaches the buffer until the flush at the end,
     * which EncodeAll does. */
    BytesBuffer x = BYTES_BUFFER(a), y = BYTES_BUFFER(a);
    BufioWriter *bw = bufio_new_writer(a, bytes_buffer_as_io_writer(&x));
    CHECK(BURROW_OK(gif_encode(a, bufio_writer_as_io_writer(bw), m, NULL)));
    CHECK(BURROW_OK(gif_encode(a, bytes_buffer_as_io_writer(&y), m, NULL)));
    CHECK(bufio_writer_buffered(bw) == 0);
    CHECK(bytes_equal(bytes_buffer_bytes(&x), bytes_buffer_bytes(&y)));

    /* Cut short in the header is io.ErrUnexpectedEOF, wrapped. */
    bytes_reader_reset(&br, cslice(f->data, 4));
    m = gif_decode(a, bytes_reader_as_io_reader(&br), &err);
    CHECK(m.vt == NULL);
    CHECK(str_eq(error_text(err), BURROW_S("gif: reading header: unexpected EOF")));
    bytes_reader_reset(&br, cslice((const Byte *)"GIF90a1234567", 13));
    m = gif_decode(a, bytes_reader_as_io_reader(&br), &err);
    CHECK(str_eq(error_text(err), BURROW_S("gif: can't recognize format \"GIF90a\"")));

    /* Go's TestEncodeZeroGIF and the size limit. */
    Gif g;
    memset(&g, 0, sizeof g);
    g.image = slice_nil(TYPE_OF(ImagePalettedPtr));
    g.delay = slice_nil(TYPE_INT);
    g.disposal = slice_nil(TYPE_BYTE);
    err = gif_encode_all(a, bytes_buffer_as_io_writer(&y), &g);
    CHECK(str_eq(error_text(err), BURROW_S("gif: must provide at least one image")));
    ImageGray *big = image_new_gray(a, image_rect(0, 0, 1 << 16, 1));
    err = gif_encode(a, bytes_buffer_as_io_writer(&y), image_gray_as_image(big), NULL);
    CHECK(str_eq(error_text(err), BURROW_S("gif: image is too large to encode")));

    /* Go's TestEncodePalettes: a nil entry and one of more than 256. */
    Color cs[257];
    for (int i = 0; i < 257; i++)
        cs[i] = color_gray_as_color((ColorGray){(uint8_t)i});
    ColorPalette p = {cs, 257, 257, &burrow_type_Color};
    ImagePaletted *pm = image_new_paletted(a, image_rect(0, 0, 1, 1), p);
    ImagePalettedPtr frames[1] = {pm};
    Int delays[1] = {0};
    g.image = slice_from(frames, 1, 1, TYPE_OF(ImagePalettedPtr));
    g.delay = slice_from(delays, 1, 1, TYPE_INT);
    err = gif_encode_all(a, bytes_buffer_as_io_writer(&y), &g);
    CHECK(
        str_eq(error_text(err),
               BURROW_S("gif: cannot encode color table with more than 256 entries")));
    memset(&cs[1], 0, sizeof cs[1]);
    pm->palette.len = 3;
    err = gif_encode_all(a, bytes_buffer_as_io_writer(&y), &g);
    CHECK(str_eq(error_text(err),
                 BURROW_S("gif: cannot encode color table with nil entries")));
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestEncodeRandom)                                                                \
    X(TestEncodeAllRandom)                                                             \
    X(TestDecodeFiles)                                                                 \
    X(TestDecodeFree)                                                                  \
    X(TestEncodeFree)                                                                  \
    X(TestRegisterAndErrors)

TESTING_MAIN(TESTS)

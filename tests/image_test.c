/* Derived from Go's src/image/image_test.go, geom_test.go, ycbcr_test.go and
 * format_test.go. Go source: go1.27.1.
 *
 * tests/image_test_gen.h, from tools/gen-image-tests.sh, holds what Go's image
 * package makes of random points and rectangles, and of every image type after
 * a run of random writes. The writes come from the xorshift generator below,
 * which the generator runs too, so each case only carries its seed and digests
 * of what Go ended up with. Go's own tests set and read a few pixels of each
 * type and check SubImage and Opaque, and all of that is covered by matching
 * Go on every pixel.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/image.h"
#include "burrow/mem/arena.h"

#include <string.h>

typedef struct ImPointCase {
    Int p[2], q[2], k, r[4], add[2], sub[2], mul[2], div[2];
    bool in, has_mod;
    Int mod[2];
    bool eq;
    const char *str;
} ImPointCase;

typedef struct ImRectCase {
    Int r[4], s[4], p[2], n;
    Int inter[4], uni[4], inset[4], canon[4], add[4], sub[4], size[2], dx, dy;
    bool empty, eq, overlaps, in, pin;
    uint32_t at_a;
    const char *str;
} ImRectCase;

typedef struct ImImageCase {
    int kind, rect, mode;
    uint64_t seed;
    Int pix_len, stride;
    uint64_t fresh, pix, reads;
    bool opaque;
    Int sr[4], sb[4], sub_len, sub_stride;
    uint64_t sub_reads;
    bool sub_opaque;
    uint64_t pix_after;
} ImImageCase;

typedef struct ImYCbCrCase {
    int ratio, rect, alpha;
    uint64_t seed;
    Int lens[4], caps[4], y_stride, c_stride, a_stride;
    uint64_t reads;
    const char *reads_panic;
    bool opaque;
    Int sr[4];
    const char *sub_panic;
    Int sb[4], sub_lens[4], sub_y_stride, sub_c_stride, sub_a_stride;
    uint64_t sub_reads;
    const char *sub_reads_panic;
    bool sub_opaque;
} ImYCbCrCase;

typedef struct ImFmtCase {
    int which;
    const char *v, *plus_v, *sharp_v, *d;
} ImFmtCase;

#include "image_test_gen.h"

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

static ImageRectangle rect4(const Int r[4]) {
    ImageRectangle x = {{r[0], r[1]}, {r[2], r[3]}};
    return x;
}

static bool rect_is(ImageRectangle r, const Int w[4]) {
    return r.min.x == w[0] && r.min.y == w[1] && r.max.x == w[2] && r.max.y == w[3];
}

static bool pt_is(ImagePoint p, const Int w[2]) {
    return p.x == w[0] && p.y == w[1];
}

/* --------------------------------------------------------------- geometry */

static void TestPoint(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < COUNT(im_point_cases); i++) {
        const ImPointCase *tc = &im_point_cases[i];
        ImagePoint p = image_pt(tc->p[0], tc->p[1]), q = image_pt(tc->q[0], tc->q[1]);
        ImageRectangle r = rect4(tc->r);
        bool ok = pt_is(image_point_add(p, q), tc->add) &&
                  pt_is(image_point_sub(p, q), tc->sub) &&
                  pt_is(image_point_mul(p, tc->k), tc->mul) &&
                  pt_is(image_point_div(p, tc->k), tc->div) &&
                  image_point_in(p, r) == tc->in && image_point_eq(p, q) == tc->eq &&
                  (!tc->has_mod || pt_is(image_point_mod(p, r), tc->mod)) &&
                  str_eq(image_point_string(p, a), str_from_cstr(tc->str));
        if (!ok)
            testing_t_errorf_v(t, "point case %d, %s: wrong result", (int)i, tc->str);
    }
    CHECK(image_zp.x == 0 && image_zp.y == 0);
    arena_free(&ar);
}

static void TestRectangle(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < COUNT(im_rect_cases); i++) {
        const ImRectCase *tc = &im_rect_cases[i];
        ImageRectangle r = rect4(tc->r), s = rect4(tc->s);
        ImagePoint p = image_pt(tc->p[0], tc->p[1]);
        ColorRGBA64 at64 = image_rectangle_rgba64_at(r, p.x, p.y);
        Color at = image_rectangle_at(r, p.x, p.y);
        bool ok =
            rect_is(image_rectangle_intersect(r, s), tc->inter) &&
            rect_is(image_rectangle_union(r, s), tc->uni) &&
            rect_is(image_rectangle_inset(r, tc->n), tc->inset) &&
            rect_is(image_rectangle_canon(r), tc->canon) &&
            rect_is(image_rectangle_add(r, p), tc->add) &&
            rect_is(image_rectangle_sub(r, p), tc->sub) &&
            pt_is(image_rectangle_size(r), tc->size) &&
            image_rectangle_dx(r) == tc->dx && image_rectangle_dy(r) == tc->dy &&
            image_rectangle_empty(r) == tc->empty &&
            image_rectangle_eq(r, s) == tc->eq &&
            image_rectangle_overlaps(r, s) == tc->overlaps &&
            image_rectangle_in(r, s) == tc->in && image_point_in(p, r) == tc->pin &&
            at.vt->self_type == TYPE_OF(ColorAlpha16) &&
            at.data.alpha16.a == tc->at_a && at64.r == tc->at_a && at64.a == tc->at_a &&
            str_eq(image_rectangle_string(r, a), str_from_cstr(tc->str));
        if (!ok)
            testing_t_errorf_v(t, "rectangle case %d, %s: wrong result", (int)i,
                               tc->str);
    }

    /* Rect puts the corners in order, and a Rectangle is an Image. */
    ImageRectangle r = image_rect(5, 4, 1, 2);
    CHECK(r.min.x == 1 && r.min.y == 2 && r.max.x == 5 && r.max.y == 4);
    Image m = image_rectangle_as_image(&r);
    CHECK(image_rectangle_eq(image_bounds(m), r));
    CHECK(image_at(m, 1, 2).data.alpha16.a == 0xffff);
    CHECK(image_at(m, 5, 2).data.alpha16.a == 0);
    CHECK(m.vt->opaque == NULL && m.vt->set == NULL);
    CHECK(image_color_model(m).vt == color_alpha16_model.vt);
    CHECK(image_rectangle_eq(image_zr, image_rect(3, 3, 3, 9)));
    arena_free(&ar);
}

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

static Color xcolor(Xs *r, int k) {
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

/* ------------------------------------------------------------------ digest */

typedef struct Fnv {
    uint64_t h;
} Fnv;

#define FNV_BASIS UINT64_C(14695981039346656037)

static void fnv_byte(Fnv *f, uint8_t b) {
    f->h ^= b;
    f->h *= UINT64_C(1099511628211);
}

static void fnv_u16(Fnv *f, uint32_t v) {
    fnv_byte(f, (uint8_t)v);
    fnv_byte(f, (uint8_t)(v >> 8));
}

static void fnv_u64(Fnv *f, uint64_t v) {
    for (int i = 0; i < 8; i++)
        fnv_byte(f, (uint8_t)(v >> (8 * i)));
}

static void fnv_slice(Fnv *f, Slice s) {
    for (Int i = 0; i < s.len; i++)
        fnv_byte(f, ((const Byte *)s.p)[i]);
}

static const Type *const im_color_types[] = {
    TYPE_OF(ColorRGBA),    TYPE_OF(ColorRGBA64), TYPE_OF(ColorNRGBA),
    TYPE_OF(ColorNRGBA64), TYPE_OF(ColorAlpha),  TYPE_OF(ColorAlpha16),
    TYPE_OF(ColorGray),    TYPE_OF(ColorGray16), TYPE_OF(ColorYCbCr),
    TYPE_OF(ColorNYCbCrA), TYPE_OF(ColorCMYK),
};

/* The same as the generator's: kind, the fields, then RGBA. */
static void fnv_color(Fnv *f, Color c) {
    if (color_is_nil(c)) {
        fnv_byte(f, 0xee);
        return;
    }
    int k = -1;
    for (int i = 0; i < (int)COUNT(im_color_types); i++)
        if (c.vt->self_type == im_color_types[i])
            k = i;
    uint32_t v[4] = {0, 0, 0, 0};
    const ColorData *d = &c.data;
    switch (k) {
    case 0:
        v[0] = d->rgba.r, v[1] = d->rgba.g, v[2] = d->rgba.b, v[3] = d->rgba.a;
        break;
    case 1:
        v[0] = d->rgba64.r, v[1] = d->rgba64.g, v[2] = d->rgba64.b, v[3] = d->rgba64.a;
        break;
    case 2:
        v[0] = d->nrgba.r, v[1] = d->nrgba.g, v[2] = d->nrgba.b, v[3] = d->nrgba.a;
        break;
    case 3:
        v[0] = d->nrgba64.r, v[1] = d->nrgba64.g, v[2] = d->nrgba64.b,
        v[3] = d->nrgba64.a;
        break;
    case 4:
        v[0] = d->alpha.a;
        break;
    case 5:
        v[0] = d->alpha16.a;
        break;
    case 6:
        v[0] = d->gray.y;
        break;
    case 7:
        v[0] = d->gray16.y;
        break;
    case 8:
        v[0] = d->y_cb_cr.y, v[1] = d->y_cb_cr.cb, v[2] = d->y_cb_cr.cr;
        break;
    case 9:
        v[0] = d->ny_cb_cr_a.y_cb_cr.y, v[1] = d->ny_cb_cr_a.y_cb_cr.cb;
        v[2] = d->ny_cb_cr_a.y_cb_cr.cr, v[3] = d->ny_cb_cr_a.a;
        break;
    default:
        v[0] = d->cmyk.c, v[1] = d->cmyk.m, v[2] = d->cmyk.y, v[3] = d->cmyk.k;
        break;
    }
    fnv_byte(f, (uint8_t)k);
    for (int i = 0; i < 4; i++)
        fnv_u16(f, v[i]);
    ColorRGBAValue x = color_rgba(c);
    fnv_u16(f, x.r);
    fnv_u16(f, x.g);
    fnv_u16(f, x.b);
    fnv_u16(f, x.a);
}

static void fnv_rgba64(Fnv *f, ColorRGBA64 c) {
    fnv_u16(f, c.r);
    fnv_u16(f, c.g);
    fnv_u16(f, c.b);
    fnv_u16(f, c.a);
}

/* ------------------------------------------------------ the Pix image types */

typedef union ImAny {
    ImageRGBA rgba;
    ImageRGBA64 rgba64;
    ImageNRGBA nrgba;
    ImageNRGBA64 nrgba64;
    ImageAlpha alpha;
    ImageAlpha16 alpha16;
    ImageGray gray;
    ImageGray16 gray16;
    ImageCMYK cmyk;
    ImagePaletted paletted;
} ImAny;

/* One of the ten, by kind, with its Image. */
typedef struct ImPix {
    int kind;
    ImAny *p;
    Image m;
} ImPix;

static Color im_palette_colors[6];
static ColorPalette im_palette;

static void palette_init(void) {
    im_palette_colors[0] = color_rgba_as_color((ColorRGBA){0, 0, 0, 0xff});
    im_palette_colors[1] = color_gray_as_color((ColorGray){100});
    im_palette_colors[2] = color_nrgba_as_color((ColorNRGBA){10, 20, 30, 128});
    im_palette_colors[3] = color_cmyk_as_color((ColorCMYK){0x10, 0x20, 0x30, 0x40});
    im_palette_colors[4] = color_alpha16_as_color((ColorAlpha16){0xffff});
    im_palette_colors[5] =
        color_rgba64_as_color((ColorRGBA64){0x1234, 0x2345, 0x3456, 0xffff});
    im_palette = slice_from(im_palette_colors, 6, 6, TYPE_OF(Color));
}

static Image as_image(int kind, ImAny *p) {
    switch (kind) {
    case 0:
        return image_rgba_as_image(&p->rgba);
    case 1:
        return image_rgba64_as_image(&p->rgba64);
    case 2:
        return image_nrgba_as_image(&p->nrgba);
    case 3:
        return image_nrgba64_as_image(&p->nrgba64);
    case 4:
        return image_alpha_as_image(&p->alpha);
    case 5:
        return image_alpha16_as_image(&p->alpha16);
    case 6:
        return image_gray_as_image(&p->gray);
    case 7:
        return image_gray16_as_image(&p->gray16);
    case 8:
        return image_cmyk_as_image(&p->cmyk);
    default:
        return image_paletted_as_image(&p->paletted);
    }
}

static ImPix new_pix(Alloc *a, int kind, ImageRectangle r) {
    ImPix x = {kind, NULL, {NULL, NULL}};
    switch (kind) {
    case 0:
        x.p = (ImAny *)image_new_rgba(a, r);
        break;
    case 1:
        x.p = (ImAny *)image_new_rgba64(a, r);
        break;
    case 2:
        x.p = (ImAny *)image_new_nrgba(a, r);
        break;
    case 3:
        x.p = (ImAny *)image_new_nrgba64(a, r);
        break;
    case 4:
        x.p = (ImAny *)image_new_alpha(a, r);
        break;
    case 5:
        x.p = (ImAny *)image_new_alpha16(a, r);
        break;
    case 6:
        x.p = (ImAny *)image_new_gray(a, r);
        break;
    case 7:
        x.p = (ImAny *)image_new_gray16(a, r);
        break;
    case 8:
        x.p = (ImAny *)image_new_cmyk(a, r);
        break;
    default:
        x.p = (ImAny *)image_new_paletted(a, r, im_palette);
        break;
    }
    x.m = as_image(kind, x.p);
    return x;
}

/* Every type starts with Pix and Stride, so these read them through any. */
static Slice pix_of(ImPix x) {
    return x.p->rgba.pix;
}

static Int stride_of(ImPix x) {
    return x.p->rgba.stride;
}

static Int pix_offset(ImPix x, Int px, Int py) {
    switch (x.kind) {
    case 0:
        return image_rgba_pix_offset(&x.p->rgba, px, py);
    case 1:
        return image_rgba64_pix_offset(&x.p->rgba64, px, py);
    case 2:
        return image_nrgba_pix_offset(&x.p->nrgba, px, py);
    case 3:
        return image_nrgba64_pix_offset(&x.p->nrgba64, px, py);
    case 4:
        return image_alpha_pix_offset(&x.p->alpha, px, py);
    case 5:
        return image_alpha16_pix_offset(&x.p->alpha16, px, py);
    case 6:
        return image_gray_pix_offset(&x.p->gray, px, py);
    case 7:
        return image_gray16_pix_offset(&x.p->gray16, px, py);
    case 8:
        return image_cmyk_pix_offset(&x.p->cmyk, px, py);
    default:
        return image_paletted_pix_offset(&x.p->paletted, px, py);
    }
}

static void typed_set(ImPix x, Xs *r, Int px, Int py) {
    switch (x.kind) {
    case 0:
        image_rgba_set_rgba(&x.p->rgba, px, py, xcolor(r, 0).data.rgba);
        break;
    case 1:
        image_rgba64_set_rgba64(&x.p->rgba64, px, py, xcolor(r, 1).data.rgba64);
        break;
    case 2:
        image_nrgba_set_nrgba(&x.p->nrgba, px, py, xcolor(r, 2).data.nrgba);
        break;
    case 3:
        image_nrgba64_set_nrgba64(&x.p->nrgba64, px, py, xcolor(r, 3).data.nrgba64);
        break;
    case 4:
        image_alpha_set_alpha(&x.p->alpha, px, py, xcolor(r, 4).data.alpha);
        break;
    case 5:
        image_alpha16_set_alpha16(&x.p->alpha16, px, py, xcolor(r, 5).data.alpha16);
        break;
    case 6:
        image_gray_set_gray(&x.p->gray, px, py, xcolor(r, 6).data.gray);
        break;
    case 7:
        image_gray16_set_gray16(&x.p->gray16, px, py, xcolor(r, 7).data.gray16);
        break;
    case 8:
        image_cmyk_set_cmyk(&x.p->cmyk, px, py, xcolor(r, 10).data.cmyk);
        break;
    default:
        image_paletted_set_color_index(&x.p->paletted, px, py,
                                       (uint8_t)xs_intn(r, im_palette.len));
        break;
    }
}

static void typed_at(Fnv *f, ImPix x, Int px, Int py) {
    switch (x.kind) {
    case 0:
        fnv_color(f, color_rgba_as_color(image_rgba_rgba_at(&x.p->rgba, px, py)));
        break;
    case 1:
        fnv_color(f,
                  color_rgba64_as_color(image_rgba64_rgba64_at(&x.p->rgba64, px, py)));
        break;
    case 2:
        fnv_color(f, color_nrgba_as_color(image_nrgba_nrgba_at(&x.p->nrgba, px, py)));
        break;
    case 3:
        fnv_color(
            f, color_nrgba64_as_color(image_nrgba64_nrgba64_at(&x.p->nrgba64, px, py)));
        break;
    case 4:
        fnv_color(f, color_alpha_as_color(image_alpha_alpha_at(&x.p->alpha, px, py)));
        break;
    case 5:
        fnv_color(
            f, color_alpha16_as_color(image_alpha16_alpha16_at(&x.p->alpha16, px, py)));
        break;
    case 6:
        fnv_color(f, color_gray_as_color(image_gray_gray_at(&x.p->gray, px, py)));
        break;
    case 7:
        fnv_color(f,
                  color_gray16_as_color(image_gray16_gray16_at(&x.p->gray16, px, py)));
        break;
    case 8:
        fnv_color(f, color_cmyk_as_color(image_cmyk_cmyk_at(&x.p->cmyk, px, py)));
        break;
    default:
        fnv_byte(f, image_paletted_color_index_at(&x.p->paletted, px, py));
        break;
    }
}

static ImAny sub_image(ImPix x, ImageRectangle r) {
    ImAny s;
    memset(&s, 0, sizeof s);
    switch (x.kind) {
    case 0:
        s.rgba = image_rgba_sub_image(&x.p->rgba, r);
        break;
    case 1:
        s.rgba64 = image_rgba64_sub_image(&x.p->rgba64, r);
        break;
    case 2:
        s.nrgba = image_nrgba_sub_image(&x.p->nrgba, r);
        break;
    case 3:
        s.nrgba64 = image_nrgba64_sub_image(&x.p->nrgba64, r);
        break;
    case 4:
        s.alpha = image_alpha_sub_image(&x.p->alpha, r);
        break;
    case 5:
        s.alpha16 = image_alpha16_sub_image(&x.p->alpha16, r);
        break;
    case 6:
        s.gray = image_gray_sub_image(&x.p->gray, r);
        break;
    case 7:
        s.gray16 = image_gray16_sub_image(&x.p->gray16, r);
        break;
    case 8:
        s.cmyk = image_cmyk_sub_image(&x.p->cmyk, r);
        break;
    default:
        s.paletted = image_paletted_sub_image(&x.p->paletted, r);
        break;
    }
    return s;
}

static uint64_t reads(ImPix x) {
    Fnv f = {FNV_BASIS};
    ImageRectangle b = image_bounds(x.m);
    for (Int y = b.min.y - 1; y < b.max.y + 1; y++) {
        for (Int px = b.min.x - 1; px < b.max.x + 1; px++) {
            fnv_color(&f, image_at(x.m, px, y));
            fnv_rgba64(&f, x.m.vt->rgba64_at(x.m.data, px, y));
            typed_at(&f, x, px, y);
            fnv_u64(&f, (uint64_t)pix_offset(x, px, y));
        }
    }
    return f.h;
}

static uint64_t pix_digest(ImPix x) {
    Fnv f = {FNV_BASIS};
    fnv_slice(&f, pix_of(x));
    return f.h;
}

static ImageRectangle sub_rect(Xs *r, ImageRectangle b) {
    Int dx = image_rectangle_dx(b), dy = image_rectangle_dy(b);
    Int x0 = b.min.x - 2 + xs_intn(r, dx + 4);
    Int y0 = b.min.y - 2 + xs_intn(r, dy + 4);
    Int x1 = x0 + xs_intn(r, dx + 3);
    Int y1 = y0 + xs_intn(r, dy + 3);
    ImageRectangle s = {{x0, y0}, {x1, y1}};
    return s;
}

static const Int im_rects[][4] = {
    {0, 0, 4, 3}, {-2, -3, 3, 1}, {5, 5, 5, 9}, {2, 1, 7, 6}, {-1, -1, 0, 0},
};

/* Go's TestImage and TestNewXxxBadRectangle's working half, over every type
 * and writes of every kind. */
static void TestImage(TestingT *t) {
    palette_init();
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < COUNT(im_image_cases); i++) {
        const ImImageCase *tc = &im_image_cases[i];
        ImageRectangle b = rect4(im_rects[tc->rect]);
        Xs r = {tc->seed};
        ImPix x = new_pix(a, tc->kind, b);
        if (x.p == NULL)
            testing_t_fatalf_v(t, "case %d: out of memory", (int)i);
        if (reads(x) != tc->fresh)
            testing_t_errorf_v(t, "case %d, kind %d: a new image reads wrong", (int)i,
                               tc->kind);
        if (tc->mode == 1) {
            Color c = color_rgba_as_color((ColorRGBA){0x40, 0x80, 0xc0, 0xff});
            for (Int y = b.min.y; y < b.max.y; y++)
                for (Int px = b.min.x; px < b.max.x; px++)
                    x.m.vt->set(x.m.data, px, y, c);
        } else {
            for (int n = 0; n < 60; n++) {
                Int op = xs_intn(&r, 3);
                Int px = b.min.x - 1 + xs_intn(&r, image_rectangle_dx(b) + 2);
                Int py = b.min.y - 1 + xs_intn(&r, image_rectangle_dy(b) + 2);
                if (op == 0) {
                    int k = (int)xs_intn(&r, 11);
                    x.m.vt->set(x.m.data, px, py, xcolor(&r, k));
                } else if (op == 1) {
                    x.m.vt->set_rgba64(x.m.data, px, py, xcolor(&r, 1).data.rgba64);
                } else {
                    typed_set(x, &r, px, py);
                }
            }
        }
        if (pix_of(x).len != tc->pix_len || stride_of(x) != tc->stride)
            testing_t_errorf_v(t, "case %d, kind %d: pix %d stride %d, want %d and %d",
                               (int)i, tc->kind, pix_of(x).len, stride_of(x),
                               tc->pix_len, tc->stride);
        if (pix_digest(x) != tc->pix)
            testing_t_errorf_v(t, "case %d, kind %d, mode %d: wrong pixels", (int)i,
                               tc->kind, tc->mode);
        if (reads(x) != tc->reads)
            testing_t_errorf_v(t, "case %d, kind %d, mode %d: wrong reads", (int)i,
                               tc->kind, tc->mode);
        if (x.m.vt->opaque(x.m.data) != tc->opaque)
            testing_t_errorf_v(t, "case %d, kind %d, mode %d: Opaque is %t", (int)i,
                               tc->kind, tc->mode, (bool)!tc->opaque);

        ImageRectangle sr = sub_rect(&r, b);
        if (!rect_is(sr, tc->sr))
            testing_t_fatalf_v(t, "case %d: the generator is out of step", (int)i);
        ImAny *sp = mem_alloc(a, sizeof(ImAny), _Alignof(ImAny));
        if (sp == NULL)
            testing_t_fatalf_v(t, "case %d: out of memory", (int)i);
        *sp = sub_image(x, sr);
        ImPix sub = {x.kind, sp, as_image(x.kind, sp)};
        ImageRectangle sb = image_bounds(sub.m);
        if (!rect_is(sb, tc->sb) || pix_of(sub).len != tc->sub_len ||
            stride_of(sub) != tc->sub_stride)
            testing_t_errorf_v(t, "case %d, kind %d: wrong SubImage", (int)i, tc->kind);
        if (x.kind == 9 && sub.p->paletted.palette.p != im_palette.p)
            testing_t_errorf_v(t, "case %d: SubImage lost the palette", (int)i);
        if (reads(sub) != tc->sub_reads)
            testing_t_errorf_v(t, "case %d, kind %d: wrong reads of SubImage", (int)i,
                               tc->kind);
        if (sub.m.vt->opaque(sub.m.data) != tc->sub_opaque)
            testing_t_errorf_v(t, "case %d, kind %d: SubImage Opaque is %t", (int)i,
                               tc->kind, (bool)!tc->sub_opaque);
        if (!image_rectangle_empty(sb))
            sub.m.vt->set(sub.m.data, sb.min.x, sb.min.y,
                          color_rgba_as_color((ColorRGBA){1, 2, 3, 4}));
        if (pix_digest(x) != tc->pix_after)
            testing_t_errorf_v(t, "case %d, kind %d: SubImage does not share pixels",
                               (int)i, tc->kind);
    }
    arena_free(&ar);
}

/* -------------------------------------------------------- YCbCr, NYCbCrA */

static char panic_buf[256];

static Str recovered(Func f) {
    volatile Int n = 0;
    BURROW_TRY {
        BURROW_CALLF0(f);
    }
    BURROW_CATCH(r) {
        Str m = panic_text(r);
        n = m.len < (Int)sizeof panic_buf ? m.len : (Int)sizeof panic_buf;
        memcpy(panic_buf, m.p, (size_t)n);
    }
    BURROW_TRY_END;
    return str_from_bytes((const Byte *)panic_buf, n);
}

/* What a run reads, kept outside the run's frame so that a panic partway
 * through leaves what was read so far. */
static Fnv y_digest;

typedef struct YRun {
    ImageYCbCr *y;
    ImageNYCbCrA *n;
} YRun;

static void run_y_reads(void *env) {
    YRun *run = env;
    const ImageYCbCr *y = run->y;
    const ImageNYCbCrA *n = run->n;
    Image m =
        n != NULL ? image_ny_cb_cr_a_as_image(run->n) : image_y_cb_cr_as_image(run->y);
    ImageRectangle b = image_bounds(m);
    for (Int py = b.min.y - 1; py < b.max.y + 1; py++) {
        for (Int px = b.min.x - 1; px < b.max.x + 1; px++) {
            fnv_color(&y_digest, image_at(m, px, py));
            fnv_rgba64(&y_digest, m.vt->rgba64_at(m.data, px, py));
            fnv_color(&y_digest,
                      color_y_cb_cr_as_color(image_y_cb_cr_y_cb_cr_at(y, px, py)));
            fnv_u64(&y_digest, (uint64_t)image_y_cb_cr_y_offset(y, px, py));
            fnv_u64(&y_digest, (uint64_t)image_y_cb_cr_c_offset(y, px, py));
            if (n != NULL) {
                fnv_color(&y_digest, color_ny_cb_cr_a_as_color(
                                         image_ny_cb_cr_a_ny_cb_cr_a_at(n, px, py)));
                fnv_u64(&y_digest, (uint64_t)image_ny_cb_cr_a_a_offset(n, px, py));
            }
        }
    }
}

static uint64_t y_reads(YRun run, Str *panicked) {
    y_digest.h = FNV_BASIS;
    *panicked = recovered(BURROW_FN(Func, run_y_reads, &run));
    return y_digest.h;
}

typedef struct YSub {
    YRun in, out;
    ImageRectangle r;
} YSub;

static void run_y_sub(void *env) {
    YSub *s = env;
    if (s->in.n != NULL)
        *s->out.n = image_ny_cb_cr_a_sub_image(s->in.n, s->r);
    else
        *s->out.y = image_y_cb_cr_sub_image(s->in.y, s->r);
}

static void fill(Slice s, int mul, int add) {
    Byte *b = s.p;
    for (Int i = 0; i < s.len; i++)
        b[i] = (Byte)(i * mul + add);
}

static const Int im_y_rects[][4] = {
    {0, 0, 8, 6}, {1, 1, 9, 7},     {-3, -1, 4, 5}, {0, 0, 1, 1},
    {3, 2, 3, 5}, {-5, -5, -1, -2}, {-7, 3, 0, 4},
};

static void TestYCbCr(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < COUNT(im_y_cb_cr_cases); i++) {
        const ImYCbCrCase *tc = &im_y_cb_cr_cases[i];
        ImageRectangle b = rect4(im_y_rects[tc->rect]);
        ImageYCbCrSubsampleRatio ratio = (ImageYCbCrSubsampleRatio)tc->ratio;
        Xs r = {tc->seed};
        YRun run = {NULL, NULL};
        Int lens[4] = {0, 0, 0, 0}, caps[4] = {0, 0, 0, 0}, a_stride = 0;
        if (tc->alpha == 0) {
            run.y = image_new_y_cb_cr(a, b, ratio);
        } else {
            run.n = image_new_ny_cb_cr_a(a, b, ratio);
            run.y = run.n != NULL ? &run.n->y_cb_cr : NULL;
        }
        if (run.y == NULL) {
            testing_t_fatalf_v(t, "case %d: out of memory", (int)i);
            return;
        }
        if (run.n != NULL) {
            if (tc->alpha == 1)
                fill(run.n->a, 17, 7);
            else if (run.n->a.len > 0)
                memset(run.n->a.p, 0xff, (size_t)run.n->a.len);
            lens[3] = run.n->a.len, caps[3] = run.n->a.cap, a_stride = run.n->a_stride;
        }
        fill(run.y->y, 7, 1);
        fill(run.y->cb, 11, 3);
        fill(run.y->cr, 13, 5);
        lens[0] = run.y->y.len, lens[1] = run.y->cb.len, lens[2] = run.y->cr.len;
        caps[0] = run.y->y.cap, caps[1] = run.y->cb.cap, caps[2] = run.y->cr.cap;
        bool shape = a_stride == tc->a_stride && run.y->y_stride == tc->y_stride &&
                     run.y->c_stride == tc->c_stride;
        for (int k = 0; k < 4; k++)
            shape = shape && lens[k] == tc->lens[k] && caps[k] == tc->caps[k];
        if (!shape)
            testing_t_errorf_v(t, "case %d, ratio %d, rect %d: wrong planes", (int)i,
                               tc->ratio, tc->rect);

        Str p;
        uint64_t h = y_reads(run, &p);
        if (h != tc->reads || !str_eq(p, str_from_cstr(tc->reads_panic)))
            testing_t_errorf_v(t, "case %d, ratio %d, rect %d: wrong reads, panic %q",
                               (int)i, tc->ratio, tc->rect, p);
        Image m = run.n != NULL ? image_ny_cb_cr_a_as_image(run.n)
                                : image_y_cb_cr_as_image(run.y);
        if (m.vt->opaque(m.data) != tc->opaque)
            testing_t_errorf_v(t, "case %d: Opaque is %t", (int)i, (bool)!tc->opaque);

        ImageRectangle sr = sub_rect(&r, b);
        if (!rect_is(sr, tc->sr))
            testing_t_fatalf_v(t, "case %d: the generator is out of step", (int)i);
        ImageYCbCr sy;
        ImageNYCbCrA sn;
        memset(&sy, 0, sizeof sy);
        memset(&sn, 0, sizeof sn);
        YSub s = {run, {&sy, run.n != NULL ? &sn : NULL}, sr};
        p = recovered(BURROW_FN(Func, run_y_sub, &s));
        if (!str_eq(p, str_from_cstr(tc->sub_panic)))
            testing_t_errorf_v(t, "case %d: SubImage panicked with %q", (int)i, p);
        if (tc->sub_panic[0] != '\0')
            continue;
        YRun sub = {run.n != NULL ? &sn.y_cb_cr : &sy, run.n != NULL ? &sn : NULL};
        Int slens[4] = {sub.y->y.len, sub.y->cb.len, sub.y->cr.len,
                        sub.n != NULL ? sub.n->a.len : 0};
        shape = rect_is(sub.y->rect, tc->sb) && sub.y->y_stride == tc->sub_y_stride &&
                sub.y->c_stride == tc->sub_c_stride &&
                (sub.n != NULL ? sub.n->a_stride : 0) == tc->sub_a_stride &&
                sub.y->subsample_ratio == ratio;
        for (int k = 0; k < 4; k++)
            shape = shape && slens[k] == tc->sub_lens[k];
        if (!shape)
            testing_t_errorf_v(t, "case %d, ratio %d, rect %d: wrong SubImage", (int)i,
                               tc->ratio, tc->rect);
        h = y_reads(sub, &p);
        if (h != tc->sub_reads || !str_eq(p, str_from_cstr(tc->sub_reads_panic)))
            testing_t_errorf_v(t, "case %d: wrong reads of SubImage, panic %q", (int)i,
                               p);
        Image sm = sub.n != NULL ? image_ny_cb_cr_a_as_image(sub.n)
                                 : image_y_cb_cr_as_image(sub.y);
        if (sm.vt->opaque(sm.data) != tc->sub_opaque)
            testing_t_errorf_v(t, "case %d: SubImage Opaque is %t", (int)i,
                               (bool)!tc->sub_opaque);
    }
    arena_free(&ar);
}

/* --------------------------------------------------------------- panics */

static ImageRectangle ctor_rect;
static int ctor_which;

static void run_ctor(void *env) {
    Alloc *a = env;
    ImageRectangle r = ctor_rect;
    switch (ctor_which) {
    case 0:
        image_new_rgba(a, r);
        break;
    case 1:
        image_new_rgba64(a, r);
        break;
    case 2:
        image_new_nrgba(a, r);
        break;
    case 3:
        image_new_nrgba64(a, r);
        break;
    case 4:
        image_new_alpha(a, r);
        break;
    case 5:
        image_new_alpha16(a, r);
        break;
    case 6:
        image_new_gray(a, r);
        break;
    case 7:
        image_new_gray16(a, r);
        break;
    case 8:
        image_new_cmyk(a, r);
        break;
    case 9:
        image_new_paletted(a, r, im_palette);
        break;
    case 10:
        image_new_y_cb_cr(a, r, IMAGE_Y_CB_CR_SUBSAMPLE_RATIO420);
        break;
    default:
        image_new_ny_cb_cr_a(a, r, IMAGE_Y_CB_CR_SUBSAMPLE_RATIO420);
        break;
    }
}

/* Go's TestNewXxxBadRectangle. */
static void TestBadRectangle(TestingT *t) {
    palette_init();
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    const ImageRectangle rs[2] = {{{0, 0}, {-1, 5}},
                                  {{0, 0}, {(Int)1 << 40, (Int)1 << 40}}};
    for (int w = 0; w < (int)COUNT(im_ctor_panics); w++) {
        for (int k = 0; k < 2; k++) {
            ctor_which = w;
            ctor_rect = rs[k];
            Str got = recovered(BURROW_FN(Func, run_ctor, a));
            if (!str_eq(got, str_from_cstr(im_ctor_panics[w][k])))
                testing_t_errorf_v(t, "constructor %d, rectangle %d: panic %q, want %q",
                                   w, k, got, str_from_cstr(im_ctor_panics[w][k]));
        }
    }
    arena_free(&ar);
}

/* ------------------------------------------------------------- Uniform */

static void TestUniform(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Color red = color_rgba_as_color((ColorRGBA){0xff, 0, 0, 0xff});
    ImageUniform *u = image_new_uniform(a, red);
    CHECK(u != NULL);
    Image m = image_uniform_as_image(u);
    ImageRectangle b = image_bounds(m);
    CHECK(b.min.x == -1000000000 && b.min.y == -1000000000 && b.max.x == 1000000000 &&
          b.max.y == 1000000000);
    CHECK(color_equal(image_at(m, -5, 1 << 20), red));
    ColorRGBA64 c = m.vt->rgba64_at(m.data, 3, 4);
    CHECK(c.r == 0xffff && c.g == 0 && c.b == 0 && c.a == 0xffff);
    CHECK(m.vt->opaque(m.data));
    CHECK(m.vt->set == NULL && m.vt->color_index_at == NULL);

    /* A Uniform is its own model, which turns everything into its color. */
    ColorModel cm = image_color_model(m);
    Color g = color_gray_as_color((ColorGray){7});
    CHECK(color_equal(color_model_convert(cm, g), red));
    CHECK(cm.data == u);

    /* And a Color, whose RGBA is its color's. */
    Color uc = image_uniform_as_color(u);
    ColorRGBAValue v = color_rgba(uc);
    CHECK(v.r == 0xffff && v.g == 0 && v.a == 0xffff);
    CHECK(uc.vt->self_type == TYPE_OF(ImageUniform));

    ImageUniform *h =
        image_new_uniform(a, color_nrgba_as_color((ColorNRGBA){1, 2, 3, 4}));
    CHECK(!image_uniform_opaque(h));

    CHECK(image_uniform_rgba(image_black).a == 0xffff &&
          image_uniform_rgba(image_black).r == 0);
    CHECK(image_uniform_rgba(image_white).r == 0xffff);
    CHECK(image_uniform_rgba(image_transparent).a == 0);
    CHECK(image_uniform_opaque(image_opaque) &&
          !image_uniform_opaque(image_transparent));
    CHECK(image_black->c.vt->self_type == TYPE_OF(ColorGray16));
    CHECK(image_opaque->c.vt->self_type == TYPE_OF(ColorAlpha16));
    image_uniform_free(u, a);
    arena_free(&ar);
}

/* ----------------------------------------------------------------- fmt */

static void TestFmt(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    ImagePoint p = {1, -2};
    ImageRectangle r = {{-3, 4}, {5, -6}};
    ImageYCbCrSubsampleRatio s420 = IMAGE_Y_CB_CR_SUBSAMPLE_RATIO420, s9 = 9;
    Any vals[] = {
        BURROW_ANY(TYPE_OF(ImagePoint), &p),
        BURROW_ANY(TYPE_OF(ImageRectangle), &r),
        BURROW_ANY(TYPE_OF(ImageYCbCrSubsampleRatio), &s420),
        BURROW_ANY(TYPE_OF(ImageYCbCrSubsampleRatio), &s9),
    };
    for (size_t i = 0; i < COUNT(im_fmt_cases); i++) {
        const ImFmtCase *tc = &im_fmt_cases[i];
        const char *formats[] = {"%v", "%+v", "%#v", "%d"};
        const char *wants[] = {tc->v, tc->plus_v, tc->sharp_v, tc->d};
        for (int f = 0; f < 4; f++) {
            Str got = fmt_sprintf(a, str_from_cstr(formats[f]),
                                  slice_from(&vals[tc->which], 1, 1, TYPE_ANY));
            Str want = str_from_cstr(wants[f]);
            if (!str_eq(got, want))
                testing_t_errorf_v(t, "%s of value %d: got %q, want %q", formats[f],
                                   tc->which, got, want);
        }
    }
    arena_free(&ar);
}

/* ------------------------------------------------------------- formats */

typedef struct FakeFormat {
    int decodes, configs;
} FakeFormat;

static FakeFormat fake;

static Image fake_decode(void *env, Alloc *a, IoReader r, Error *err) {
    (void)a;
    (void)r;
    (void)err;
    FakeFormat *f = env;
    f->decodes++;
    return image_rectangle_as_image(&image_zr);
}

static ImageConfig fake_config(void *env, Alloc *a, IoReader r, Error *err) {
    (void)a;
    FakeFormat *f = env;
    f->configs++;
    /* The reader is past nothing: sniffing only peeks. */
    Byte b[4];
    Int n = io_read_full(r, slice_from(b, 4, 4, TYPE_BYTE), err);
    ImageConfig c;
    memset(&c, 0, sizeof c);
    c.color_model = color_gray_model;
    c.width = n;
    c.height = b[1];
    return c;
}

static IoReader reader_of(BytesReader *br, const char *s) {
    bytes_reader_reset(br, slice_from((void *)(uintptr_t)s, (Int)strlen(s),
                                      (Int)strlen(s), TYPE_BYTE));
    return bytes_reader_as_io_reader(br);
}

/* Go's format_test.go registers nothing of its own and leaves that to the
 * decoders' tests. This registers two fake formats and checks the sniffing:
 * the order they were registered in, ? in the magic, input shorter than the
 * magic, and a bufio.Reader passed through as it is. */
static void TestFormats(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesReader br;
    bytes_reader_reset(&br, slice_nil(TYPE_BYTE));
    Str name;
    Error err = BURROW_NO_ERROR;

    image_decode_config(a, reader_of(&br, "FAKE"), &name, &err);
    CHECK(errors_is(err, image_err_format));
    CHECK(name.len == 0);

    image_register_format(BURROW_S("fake"), BURROW_S("F?KE"),
                          (ImageDecodeFunc){fake_decode, &fake},
                          (ImageDecodeConfigFunc){fake_config, &fake});
    image_register_format(BURROW_S("fake2"), BURROW_S("F"),
                          (ImageDecodeFunc){NULL, NULL},
                          (ImageDecodeConfigFunc){fake_config, &fake});

    err = BURROW_NO_ERROR;
    ImageConfig c = image_decode_config(a, reader_of(&br, "FXKE"), &name, &err);
    CHECK(BURROW_OK(err));
    CHECK(str_eq(name, BURROW_S("fake")));
    CHECK(c.width == 4 && c.height == 'X' && c.color_model.vt == color_gray_model.vt);
    CHECK(fake.configs == 1);

    /* F?KE does not match FAKX, but F does, and it comes second. */
    err = BURROW_NO_ERROR;
    c = image_decode_config(a, reader_of(&br, "FAKX"), &name, &err);
    CHECK(BURROW_OK(err) && str_eq(name, BURROW_S("fake2")));

    /* Shorter than the first magic, so only the second can match. */
    err = BURROW_NO_ERROR;
    image_decode_config(a, reader_of(&br, "FA"), &name, &err);
    CHECK(str_eq(name, BURROW_S("fake2")));

    /* fake2 has no decoder, which is ErrFormat for Decode. */
    err = BURROW_NO_ERROR;
    image_decode(a, reader_of(&br, "FAKX"), &name, &err);
    CHECK(errors_is(err, image_err_format));

    err = BURROW_NO_ERROR;
    Image m = image_decode(a, reader_of(&br, "F_KE"), &name, &err);
    CHECK(BURROW_OK(err) && str_eq(name, BURROW_S("fake")) && fake.decodes == 1);
    CHECK(image_rectangle_empty(image_bounds(m)));

    err = BURROW_NO_ERROR;
    image_decode(a, reader_of(&br, "nothing"), &name, &err);
    CHECK(errors_is(err, image_err_format) && name.len == 0);
    CHECK(str_eq(error_text(image_err_format), BURROW_S("image: unknown format")));

    /* A bufio.Reader is used as it is, so what the decoder reads comes out of
     * the caller's buffer. */
    BufioReader *b = bufio_new_reader(a, reader_of(&br, "FAKEFAKE"));
    CHECK(b != NULL);
    err = BURROW_NO_ERROR;
    image_decode_config(a, bufio_reader_as_io_reader(b), &name, &err);
    CHECK(BURROW_OK(err) && bufio_reader_buffered(b) == 4);
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestPoint)                                                                       \
    X(TestRectangle)                                                                   \
    X(TestImage)                                                                       \
    X(TestYCbCr)                                                                       \
    X(TestBadRectangle)                                                                \
    X(TestUniform)                                                                     \
    X(TestFmt)                                                                         \
    X(TestFormats)

TESTING_MAIN(TESTS)

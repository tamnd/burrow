/* Derived from Go's src/image/draw/draw_test.go and clip_test.go. Go source:
 * go1.27.1.
 *
 * tests/image_draw_test_gen.h, from tools/gen-image-draw-tests.sh, holds what
 * Go's image/draw does to random images. Each case is a seed for the xorshift
 * generator below, which picks a destination, a source and a mask of every
 * image type, fills their pixels, and picks a rectangle, two points, an op and
 * which of Draw, DrawMask, Op.Draw and FloydSteinberg to call. The case holds
 * a digest of the destination's pixels afterwards, or the panic Go ended in.
 * Go's own tests draw a handful of fixed images through each fast path and the
 * fallback and compare them with a slow reference, and every one of those
 * paths is taken many times over here and has to match Go byte for byte.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/image/draw.h"
#include "burrow/mem/arena.h"

#include <string.h>

typedef struct DrawCase {
    uint64_t seed;
    uint64_t digest;
    const char *panic;
} DrawCase;

#include "image_draw_test_gen.h"

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

/* ------------------------------------------------------ plain and plainMask
 *
 * An image with only ColorModel, Bounds, At and Set, and a mask with only the
 * first three, so that DrawMask has to take its slowest path for them. */

typedef struct Plain {
    ImageNRGBA *m;
} Plain;

typedef struct PlainMask {
    ImageAlpha *m;
} PlainMask;

static const Type plain_type = {
    {(const Byte *)"plain", 5},
    {(const Byte *)"main", 4},
    KIND_STRUCT,
    (uint32_t)sizeof(Plain),
    (uint16_t)_Alignof(Plain),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x706c6e31U,
    NULL,
};

static const Type plain_mask_type = {
    {(const Byte *)"plainMask", 9},
    {(const Byte *)"main", 4},
    KIND_STRUCT,
    (uint32_t)sizeof(PlainMask),
    (uint16_t)_Alignof(PlainMask),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x706c6e32U,
    NULL,
};

static ColorModel plain_color_model(void *self) {
    return image_nrgba_color_model(((Plain *)self)->m);
}
static ImageRectangle plain_bounds(void *self) {
    return image_nrgba_bounds(((Plain *)self)->m);
}
static Color plain_at(void *self, Int x, Int y) {
    return image_nrgba_at(((Plain *)self)->m, x, y);
}
static void plain_set(void *self, Int x, Int y, Color c) {
    image_nrgba_set(((Plain *)self)->m, x, y, c);
}

static const ImageVT plain_vt = {
    &plain_type, plain_color_model, plain_bounds, plain_at, NULL,
    NULL,        plain_set,         NULL,         NULL,
};

static ColorModel plain_mask_color_model(void *self) {
    return image_alpha_color_model(((PlainMask *)self)->m);
}
static ImageRectangle plain_mask_bounds(void *self) {
    return image_alpha_bounds(((PlainMask *)self)->m);
}
static Color plain_mask_at(void *self, Int x, Int y) {
    return image_alpha_at(((PlainMask *)self)->m, x, y);
}

static const ImageVT plain_mask_vt = {
    &plain_mask_type,
    plain_mask_color_model,
    plain_mask_bounds,
    plain_mask_at,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
};

/* ------------------------------------------------------------ the images */

/* The kinds. The ones before K_Y_CB_CR have Set and can be a destination. */
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
    K_PLAIN,
    K_Y_CB_CR,
    K_NY_CB_CR_A,
    K_UNIFORM,
    K_PLAIN_MASK,
    K_NIL,
};

typedef struct Built {
    Image img;
    Slice pix;
} Built;

static ImageRectangle xrect(Xs *r) {
    Int x0 = xs_intn(r, 9) - 4, y0 = xs_intn(r, 9) - 4;
    Int w = xs_intn(r, 12), h = xs_intn(r, 12);
    return image_rect(x0, y0, x0 + w, y0 + h);
}

static void fill(Xs *r, Slice b) {
    for (Int i = 0; i < b.len; i++)
        ((Byte *)b.p)[i] = (Byte)xs_next(r);
}

static ColorPalette xpalette(Alloc *a, Xs *r) {
    switch (xs_intn(r, 4)) {
    case 0:
        return palette_plan9;
    case 1:
        return palette_web_safe;
    default:
        break;
    }
    Int n = 1 + xs_intn(r, 16);
    Color *c = BURROW_NEW_N(a, Color, (size_t)n);
    for (Int i = 0; i < n; i++) {
        Int k = xs_intn(r, 11);
        c[i] = xcolor(r, k);
    }
    ColorPalette p = {c, n, n, &burrow_type_Color};
    return p;
}

static Built build(Alloc *a, Xs *r, int k) {
    ImageRectangle rect = xrect(r);
    Built b;
    memset(&b, 0, sizeof b);
    switch (k) {
#define PIXKIND(K, T, pre)                                                             \
    case K: {                                                                          \
        T *m = image_new_##pre(a, rect);                                               \
        fill(r, m->pix);                                                               \
        b.img = image_##pre##_as_image(m);                                             \
        b.pix = m->pix;                                                                \
        return b;                                                                      \
    }
        PIXKIND(K_RGBA, ImageRGBA, rgba)
        PIXKIND(K_RGBA64, ImageRGBA64, rgba64)
        PIXKIND(K_NRGBA, ImageNRGBA, nrgba)
        PIXKIND(K_NRGBA64, ImageNRGBA64, nrgba64)
        PIXKIND(K_ALPHA, ImageAlpha, alpha)
        PIXKIND(K_ALPHA16, ImageAlpha16, alpha16)
        PIXKIND(K_GRAY, ImageGray, gray)
        PIXKIND(K_GRAY16, ImageGray16, gray16)
        PIXKIND(K_CMYK, ImageCMYK, cmyk)
#undef PIXKIND
    case K_PALETTED: {
        ImagePaletted *m = image_new_paletted(a, rect, xpalette(a, r));
        for (Int i = 0; i < m->pix.len; i++)
            ((Byte *)m->pix.p)[i] = (Byte)(xs_next(r) % (uint64_t)m->palette.len);
        b.img = image_paletted_as_image(m);
        b.pix = m->pix;
        return b;
    }
    case K_PLAIN: {
        Plain *p = BURROW_NEW(a, Plain);
        p->m = image_new_nrgba(a, rect);
        fill(r, p->m->pix);
        b.img = (Image){&plain_vt, p};
        b.pix = p->m->pix;
        return b;
    }
    case K_Y_CB_CR: {
        ImageYCbCr *m = image_new_y_cb_cr(a, rect, xs_intn(r, 6));
        fill(r, m->y);
        fill(r, m->cb);
        fill(r, m->cr);
        b.img = image_y_cb_cr_as_image(m);
        return b;
    }
    case K_NY_CB_CR_A: {
        ImageNYCbCrA *m = image_new_ny_cb_cr_a(a, rect, xs_intn(r, 6));
        fill(r, m->y_cb_cr.y);
        fill(r, m->y_cb_cr.cb);
        fill(r, m->y_cb_cr.cr);
        fill(r, m->a);
        b.img = image_ny_cb_cr_a_as_image(m);
        return b;
    }
    case K_UNIFORM: {
        Int ck = xs_intn(r, 11);
        b.img = image_uniform_as_image(image_new_uniform(a, xcolor(r, ck)));
        return b;
    }
    case K_PLAIN_MASK: {
        PlainMask *p = BURROW_NEW(a, PlainMask);
        p->m = image_new_alpha(a, rect);
        fill(r, p->m->pix);
        b.img = (Image){&plain_mask_vt, p};
        return b;
    }
    default:
        break;
    }
    return b;
}

static const int mask_kinds[] = {K_NIL,  K_ALPHA, K_ALPHA16,   K_UNIFORM,
                                 K_RGBA, K_GRAY,  K_PLAIN_MASK};

static uint64_t digest(Slice b) {
    uint64_t h = UINT64_C(14695981039346656037);
    for (Int i = 0; i < b.len; i++) {
        h ^= ((const Byte *)b.p)[i];
        h *= UINT64_C(1099511628211);
    }
    return h;
}

/* One call, with everything it needs, so that it can run under a TRY. */
typedef struct DrawRun {
    Int how;
    DrawOp op;
    Image dst, src, mask;
    ImageRectangle r;
    ImagePoint sp, mp;
} DrawRun;

static void run_draw(void *env) {
    DrawRun *d = (DrawRun *)env;
    switch (d->how) {
    case 0:
        draw_drawer_draw(draw_floyd_steinberg, d->dst, d->r, d->src, d->sp);
        break;
    case 1:
        draw_op_draw(d->op, d->dst, d->r, d->src, d->sp);
        break;
    case 2:
        draw_draw(d->dst, d->r, d->src, d->sp, d->op);
        break;
    default:
        draw_draw_mask(d->dst, d->r, d->src, d->sp, d->mask, d->mp, d->op);
    }
}

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

static const char *const how_names[] = {"FloydSteinberg", "Op.Draw", "Draw",
                                        "DrawMask"};

static void TestDrawRandom(TestingT *t) {
    int bad = 0;
    for (size_t i = 0; i < sizeof draw_cases / sizeof draw_cases[0] && bad < 20; i++) {
        const DrawCase *tc = &draw_cases[i];
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);

        Xs r = {tc->seed};
        int dk = (int)xs_intn(&r, K_Y_CB_CR);
        if (xs_intn(&r, 2) == 0)
            dk = K_RGBA;
        int sk = (int)xs_intn(&r, K_PLAIN_MASK);
        if (xs_intn(&r, 3) == 0)
            sk = dk;
        int mk = mask_kinds[xs_intn(&r, 7)];
        if (xs_intn(&r, 3) == 0)
            mk = K_NIL;
        DrawRun run;
        memset(&run, 0, sizeof run);
        run.op = xs_intn(&r, 2);
        run.how = xs_intn(&r, 8);
        Built dst = build(a, &r, dk);
        Built src = dst;
        if (xs_intn(&r, 4) != 0)
            src = build(a, &r, sk);
        if (mk != K_NIL)
            run.mask = build(a, &r, mk).img;
        run.dst = dst.img;
        run.src = src.img;
        run.r = xrect(&r);
        Int x = xs_intn(&r, 20) - 8, y = xs_intn(&r, 20) - 8;
        run.sp = image_pt(x, y);
        x = xs_intn(&r, 20) - 8, y = xs_intn(&r, 20) - 8;
        run.mp = image_pt(x, y);

        Str p = recovered(BURROW_FN(Func, run_draw, &run));
        Str want = str_from_cstr(tc->panic != NULL ? tc->panic : "");
        bool ok =
            str_eq(p, want) && (tc->panic != NULL || digest(dst.pix) == tc->digest);
        if (!ok) {
            testing_t_errorf_v(
                t, "case %d (%s, dst %d, src %d, mask %d, op %d): panic %q, want %q",
                (int)i, how_names[run.how < 3 ? run.how : 3], dk, sk, mk, (int)run.op,
                p, want);
            bad++;
        }
        arena_free(&ar);
    }
}

/* Go's TestClip: a rectangle, points and a mask outside the images move in
 * step as the rectangle is clipped. */
static void TestClip(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    ImageRGBA *dst = image_new_rgba(a, image_rect(0, 0, 100, 100));
    ImageRGBA *src = image_new_rgba(a, image_rect(0, 0, 100, 100));
    for (Int i = 0; i < src->pix.len; i++)
        ((Byte *)src->pix.p)[i] = (Byte)(i * 7);
    /* Drawing from (-10, -10) clips the top left and lands src's (0, 0) at
     * dst's (10, 10). */
    draw_draw(image_rgba_as_image(dst), image_rect(0, 0, 50, 50),
              image_rgba_as_image(src), image_pt(-10, -10), DRAW_SRC);
    CHECK(image_rgba_rgba_at(dst, 10, 10).r == image_rgba_rgba_at(src, 0, 0).r);
    CHECK(image_rgba_rgba_at(dst, 49, 49).g == image_rgba_rgba_at(src, 39, 39).g);
    CHECK(image_rgba_rgba_at(dst, 9, 9).a == 0);
    arena_free(&ar);
}

/* Go's TestFill: a Uniform fills every pixel it covers, and Over with an
 * opaque color is the same as Src. */
static void TestFill(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    ImageRGBA *m = image_new_rgba(a, image_rect(0, 0, 40, 30));
    ImageUniform *blue =
        image_new_uniform(a, color_rgba_as_color((ColorRGBA){0, 0, 0xff, 0xff}));
    draw_draw(image_rgba_as_image(m), image_rect(5, 5, 35, 25),
              image_uniform_as_image(blue), image_pt(0, 0), DRAW_OVER);
    ColorRGBA in = image_rgba_rgba_at(m, 5, 5), out = image_rgba_rgba_at(m, 4, 5);
    CHECK(in.b == 0xff && in.a == 0xff && in.r == 0);
    CHECK(out.a == 0);
    CHECK(image_rgba_rgba_at(m, 34, 24).b == 0xff &&
          image_rgba_rgba_at(m, 35, 24).b == 0);
    arena_free(&ar);
}

/* An Op is a Drawer, and so is FloydSteinberg. */
static void TestDrawer(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    ImageRGBA *m = image_new_rgba(a, image_rect(0, 0, 4, 4));
    DrawOp op = DRAW_SRC;
    DrawDrawer d = draw_op_as_drawer(&op);
    CHECK(d.vt->self_type == &burrow_type_DrawOp);
    draw_drawer_draw(d, image_rgba_as_image(m), image_rgba_bounds(m),
                     image_uniform_as_image(image_white), image_pt(0, 0));
    CHECK(image_rgba_rgba_at(m, 3, 3).r == 0xff);

    /* Floyd-Steinberg onto black and white turns 50% gray into a pattern with
     * as many white pixels as black. */
    Color bw[2] = {color_gray16_as_color(color_black),
                   color_gray16_as_color(color_white)};
    ColorPalette p = {bw, 2, 2, &burrow_type_Color};
    ImagePaletted *pm = image_new_paletted(a, image_rect(0, 0, 16, 16), p);
    ImageUniform *gray = image_new_uniform(a, color_gray_as_color((ColorGray){0x80}));
    draw_drawer_draw(draw_floyd_steinberg, image_paletted_as_image(pm),
                     image_paletted_bounds(pm), image_uniform_as_image(gray),
                     image_pt(0, 0));
    Int white = 0;
    for (Int i = 0; i < pm->pix.len; i++)
        white += ((Byte *)pm->pix.p)[i];
    CHECK(white > 100 && white < 156);
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestDrawRandom)                                                                  \
    X(TestClip)                                                                        \
    X(TestFill)                                                                        \
    X(TestDrawer)

TESTING_MAIN(TESTS)

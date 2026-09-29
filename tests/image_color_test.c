/* Derived from Go's src/image/color/color_test.go, ycbcr_test.go and
 * src/image/color/palette/palette_test.go. Go source: go1.27.1.
 *
 * tests/image_color_test_gen.h, from tools/gen-image-color-tests.sh, holds
 * what Go's image/color makes of a few thousand colors: RGBA, every model and
 * both palettes on colors of every type, how fmt prints each type, and digests
 * of the conversion functions over all of their inputs, or a stride of them
 * where there are 2^32. Go's own tests check round trips and a few known
 * values, and all of those follow from matching Go on every input.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/image/color.h"
#include "burrow/image/color/palette.h"
#include "burrow/mem/arena.h"

#include <string.h>

typedef struct ClCol {
    int kind;
    uint32_t v[4];
} ClCol;

typedef struct ClRGBACase {
    ClCol in;
    ColorRGBAValue want;
} ClRGBACase;

typedef struct ClModelCase {
    int model;
    ClCol in, want;
} ClModelCase;

typedef struct ClPaletteCase {
    int palette;
    ClCol in;
    Int index;
    ClCol want;
} ClPaletteCase;

typedef struct ClFmtCase {
    ClCol in;
    const char *v, *plus_v, *sharp_v;
} ClFmtCase;

#include "image_color_test_gen.h"

static const Type *const cl_types[] = {
    TYPE_OF(ColorRGBA),    TYPE_OF(ColorRGBA64), TYPE_OF(ColorNRGBA),
    TYPE_OF(ColorNRGBA64), TYPE_OF(ColorAlpha),  TYPE_OF(ColorAlpha16),
    TYPE_OF(ColorGray),    TYPE_OF(ColorGray16), TYPE_OF(ColorYCbCr),
    TYPE_OF(ColorNYCbCrA), TYPE_OF(ColorCMYK),
};

static const ColorModel *const cl_models[] = {
    &color_rgba_model,       &color_rgba64_model, &color_nrgba_model,
    &color_nrgba64_model,    &color_alpha_model,  &color_alpha16_model,
    &color_gray_model,       &color_gray16_model, &color_y_cb_cr_model,
    &color_ny_cb_cr_a_model, &color_cmyk_model,
};

#define U8(x) ((uint8_t)(x))
#define U16(x) ((uint16_t)(x))

static Color make(ClCol c) {
    const uint32_t *v = c.v;
    switch (c.kind) {
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

/* Whether got is the color c describes, type and value, as Go's == would say
 * of two interface values. */
static bool is(Color got, ClCol c) {
    return got.vt != NULL && got.vt->self_type == cl_types[c.kind] &&
           color_equal(got, make(c));
}

static bool rgba_is(ColorRGBAValue got, ColorRGBAValue want) {
    return got.r == want.r && got.g == want.g && got.b == want.b && got.a == want.a;
}

static Str sprint(Alloc *a, const char *format, Color c) {
    Any arg = BURROW_ANY(TYPE_OF(Color), &c);
    return fmt_sprintf(a, str_from_cstr(format), slice_from(&arg, 1, 1, TYPE_ANY));
}

static void TestRGBA(TestingT *t) {
    size_t n = sizeof cl_rgba_cases / sizeof cl_rgba_cases[0];
    for (size_t i = 0; i < n; i++) {
        const ClRGBACase *tc = &cl_rgba_cases[i];
        ColorRGBAValue got = color_rgba(make(tc->in));
        if (!rgba_is(got, tc->want))
            testing_t_errorf_v(
                t, "case %d, kind %d: got {%d %d %d %d}, want {%d %d %d %d}", (int)i,
                tc->in.kind, got.r, got.g, got.b, got.a, tc->want.r, tc->want.g,
                tc->want.b, tc->want.a);
    }
}

static void TestModels(TestingT *t) {
    size_t n = sizeof cl_model_cases / sizeof cl_model_cases[0];
    for (size_t i = 0; i < n; i++) {
        const ClModelCase *tc = &cl_model_cases[i];
        Color in = make(tc->in), want = make(tc->want);
        Color got = color_model_convert(*cl_models[tc->model], in);
        if (!is(got, tc->want))
            testing_t_errorf_v(t, "model %d on %v: got %v, want %v", tc->model,
                               BURROW_ANY(TYPE_OF(Color), &in),
                               BURROW_ANY(TYPE_OF(Color), &got),
                               BURROW_ANY(TYPE_OF(Color), &want));
    }
}

static void TestPalettes(TestingT *t) {
    Color small_colors[] = {
        color_gray_as_color((ColorGray){0x80}),
        color_rgba_as_color((ColorRGBA){0x80, 0x80, 0x80, 0xff}),
        color_alpha16_as_color((ColorAlpha16){0}),
        color_cmyk_as_color((ColorCMYK){0, 0xff, 0xff, 0}),
        color_nrgba_as_color((ColorNRGBA){0xff, 0, 0, 0x80}),
    };
    ColorPalette small = slice_from(small_colors, 5, 5, TYPE_OF(Color));
    const ColorPalette *pals[] = {&palette_plan9, &palette_web_safe, &small};
    size_t n = sizeof cl_palette_cases / sizeof cl_palette_cases[0];
    for (size_t i = 0; i < n; i++) {
        const ClPaletteCase *tc = &cl_palette_cases[i];
        ColorPalette p = *pals[tc->palette];
        Color in = make(tc->in);
        Int index = color_palette_index(p, in);
        if (index != tc->index)
            testing_t_errorf_v(t, "palette %d, case %d: index %d, want %d", tc->palette,
                               (int)i, index, tc->index);
        if (!is(color_palette_convert(p, in), tc->want))
            testing_t_errorf_v(t, "palette %d, case %d: wrong Convert", tc->palette,
                               (int)i);
        ColorModel m = color_palette_as_model(pals[tc->palette]);
        if (!is(color_model_convert(m, in), tc->want))
            testing_t_errorf_v(t, "palette %d, case %d: wrong Convert as a model",
                               tc->palette, (int)i);
    }
    CHECK(palette_plan9.len == 256 && palette_web_safe.len == 216);
    CHECK(palette_plan9.elem == TYPE_OF(Color));

    /* An empty palette gives a nil color and index 0. */
    ColorPalette empty = slice_from(small_colors, 0, 0, TYPE_OF(Color));
    Color red = color_rgba_as_color((ColorRGBA){0xff, 0, 0, 0xff});
    CHECK(color_is_nil(color_palette_convert(empty, red)));
    CHECK(color_palette_index(empty, red) == 0);
}

static void TestFmt(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    size_t n = sizeof cl_fmt_cases / sizeof cl_fmt_cases[0];
    for (size_t i = 0; i < n; i++) {
        const ClFmtCase *tc = &cl_fmt_cases[i];
        Color c = make(tc->in);
        const char *formats[] = {"%v", "%+v", "%#v"};
        const char *wants[] = {tc->v, tc->plus_v, tc->sharp_v};
        for (int f = 0; f < 3; f++) {
            Str got = sprint(a, formats[f], c);
            Str want = str_from_cstr(wants[f]);
            if (!str_eq(got, want))
                testing_t_errorf_v(t, "%s of kind %d: got %q, want %q", formats[f],
                                   tc->in.kind, got, want);
        }
    }
    Color nil;
    memset(&nil, 0, sizeof nil);
    CHECK(str_eq(sprint(a, "%v", nil), BURROW_S("<nil>")));
    arena_free(&ar);
}

typedef struct ClFnv {
    uint64_t h;
} ClFnv;

static void fnv_byte(ClFnv *f, uint8_t b) {
    f->h ^= b;
    f->h *= UINT64_C(1099511628211);
}

static void fnv_u16(ClFnv *f, uint32_t v) {
    fnv_byte(f, (uint8_t)v);
    fnv_byte(f, (uint8_t)(v >> 8));
}

static void fnv_rgba(ClFnv *f, ColorRGBAValue v) {
    fnv_u16(f, v.r);
    fnv_u16(f, v.g);
    fnv_u16(f, v.b);
    fnv_u16(f, v.a);
}

/* Go's TestYCbCrRoundTrip, TestYCbCrToRGBConsistency, TestCMYKRoundTrip and
 * the rest check properties of these functions over a sample of inputs. This
 * checks every input against Go's answers instead. */
static void TestConversions(TestingT *t) {
    const uint64_t basis = UINT64_C(14695981039346656037);
    ClFnv yc = {basis}, rgb = {basis}, cmyk = {basis}, ycrgba = {basis};
    for (uint32_t i = 0; i < (1U << 24); i++) {
        uint8_t a = (uint8_t)(i >> 16), b = (uint8_t)(i >> 8), c = (uint8_t)i;
        ColorYCbCr y = color_rgb_to_y_cb_cr(a, b, c);
        fnv_byte(&yc, y.y);
        fnv_byte(&yc, y.cb);
        fnv_byte(&yc, y.cr);
        ColorRGBA r = color_y_cb_cr_to_rgb(a, b, c);
        fnv_byte(&rgb, r.r);
        fnv_byte(&rgb, r.g);
        fnv_byte(&rgb, r.b);
        ColorCMYK k = color_rgb_to_cmyk(a, b, c);
        fnv_byte(&cmyk, k.c);
        fnv_byte(&cmyk, k.m);
        fnv_byte(&cmyk, k.y);
        fnv_byte(&cmyk, k.k);
        fnv_rgba(&ycrgba, color_y_cb_cr_rgba((ColorYCbCr){a, b, c}));
    }
    ClFnv cmykrgb = {basis}, nyc = {basis};
    for (uint64_t i = 0; i < (UINT64_C(1) << 32); i += 257) {
        uint8_t a = (uint8_t)(i >> 24), b = (uint8_t)(i >> 16), c = (uint8_t)(i >> 8),
                d = (uint8_t)i;
        ColorRGBA r = color_cmyk_to_rgb(a, b, c, d);
        fnv_byte(&cmykrgb, r.r);
        fnv_byte(&cmykrgb, r.g);
        fnv_byte(&cmykrgb, r.b);
        fnv_rgba(&nyc, color_ny_cb_cr_a_rgba((ColorNYCbCrA){{a, b, c}, d}));
    }
    CHECK(yc.h == CL_DIGEST_RGB_TO_Y_CB_CR);
    CHECK(rgb.h == CL_DIGEST_Y_CB_CR_TO_RGB);
    CHECK(cmyk.h == CL_DIGEST_RGB_TO_CMYK);
    CHECK(ycrgba.h == CL_DIGEST_Y_CB_CR_RGBA);
    CHECK(cmykrgb.h == CL_DIGEST_CMYK_TO_RGB);
    CHECK(nyc.h == CL_DIGEST_NY_CB_CR_A_RGBA);
}

static Color swap_model(void *env, Color c) {
    int *calls = env;
    (*calls)++;
    ColorRGBAValue v = color_rgba(c);
    return color_rgba64_as_color(
        (ColorRGBA64){(uint16_t)v.b, (uint16_t)v.g, (uint16_t)v.r, (uint16_t)v.a});
}

/* ModelFunc, the package's variables and type assertions. */
static void TestModelFunc(TestingT *t) {
    int calls = 0;
    ColorModelFunc f = {swap_model, &calls};
    ColorModel m = color_model_func(&f);
    Color got =
        color_model_convert(m, color_rgba_as_color((ColorRGBA){0xff, 0, 0, 0xff}));
    CHECK(calls == 1);
    CHECK(got.vt->self_type == TYPE_OF(ColorRGBA64));
    CHECK(got.data.rgba64.r == 0 && got.data.rgba64.b == 0xffff);

    CHECK(rgba_is(color_gray16_rgba(color_black), (ColorRGBAValue){0, 0, 0, 0xffff}));
    CHECK(rgba_is(color_gray16_rgba(color_white),
                  (ColorRGBAValue){0xffff, 0xffff, 0xffff, 0xffff}));
    CHECK(rgba_is(color_alpha16_rgba(color_transparent), (ColorRGBAValue){0, 0, 0, 0}));
    CHECK(rgba_is(color_alpha16_rgba(color_opaque),
                  (ColorRGBAValue){0xffff, 0xffff, 0xffff, 0xffff}));

    /* The model hands back the same color when it is already its type. */
    Color g = color_gray_as_color((ColorGray){7});
    CHECK(color_equal(color_model_convert(color_gray_model, g), g));

    /* Equality is type and value. */
    CHECK(!color_equal(color_gray_as_color((ColorGray){0}),
                       color_alpha_as_color((ColorAlpha){0})));
    Color nil;
    memset(&nil, 0, sizeof nil);
    CHECK(color_equal(nil, nil));
    CHECK(!color_equal(nil, g));
}

#define TESTS(X)                                                                       \
    X(TestRGBA)                                                                        \
    X(TestModels)                                                                      \
    X(TestPalettes)                                                                    \
    X(TestFmt)                                                                         \
    X(TestConversions)                                                                 \
    X(TestModelFunc)

TESTING_MAIN(TESTS)

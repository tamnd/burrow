/* image/color, from color.go and ycbcr.go.
 *
 * Each color type has three things here besides its RGBA method: a descriptor
 * with Go's type and field names, so fmt prints it the way Go does, a vtable
 * whose self_type is that descriptor, and the function that puts one in a
 * Color. The models are Go's model functions behind one static ColorModelFunc
 * each.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/image/color.h"

#include <stddef.h>
#include <string.h>

/* ------------------------------------------------------------ descriptors */

#define CL_STR(s) {(const Byte *)(s), (Int)sizeof(s) - 1}
#define CL_FIELD(T, go, c, ft)                                                         \
    {CL_STR(go), {NULL, 0}, &burrow_type_##ft, (uint32_t)offsetof(T, c)}
#define CL_COUNT(arr) (uint16_t)(sizeof(arr) / sizeof((arr)[0]))

#define CL_TYPE(T, go, fields, tag)                                                    \
    const Type burrow_type_##T = {                                                     \
        CL_STR(go),                                                                    \
        CL_STR("image/color"),                                                         \
        KIND_STRUCT,                                                                   \
        (uint32_t)sizeof(T),                                                           \
        (uint16_t)_Alignof(T),                                                         \
        CL_COUNT(fields),                                                              \
        0,                                                                             \
        fields,                                                                        \
        NULL,                                                                          \
        NULL,                                                                          \
        NULL,                                                                          \
        0,                                                                             \
        tag,                                                                           \
        NULL,                                                                          \
    }

static const Field cl_rgba_fields[] = {
    CL_FIELD(ColorRGBA, "R", r, uint8_t),
    CL_FIELD(ColorRGBA, "G", g, uint8_t),
    CL_FIELD(ColorRGBA, "B", b, uint8_t),
    CL_FIELD(ColorRGBA, "A", a, uint8_t),
};
static const Field cl_rgba64_fields[] = {
    CL_FIELD(ColorRGBA64, "R", r, uint16_t),
    CL_FIELD(ColorRGBA64, "G", g, uint16_t),
    CL_FIELD(ColorRGBA64, "B", b, uint16_t),
    CL_FIELD(ColorRGBA64, "A", a, uint16_t),
};
static const Field cl_nrgba_fields[] = {
    CL_FIELD(ColorNRGBA, "R", r, uint8_t),
    CL_FIELD(ColorNRGBA, "G", g, uint8_t),
    CL_FIELD(ColorNRGBA, "B", b, uint8_t),
    CL_FIELD(ColorNRGBA, "A", a, uint8_t),
};
static const Field cl_nrgba64_fields[] = {
    CL_FIELD(ColorNRGBA64, "R", r, uint16_t),
    CL_FIELD(ColorNRGBA64, "G", g, uint16_t),
    CL_FIELD(ColorNRGBA64, "B", b, uint16_t),
    CL_FIELD(ColorNRGBA64, "A", a, uint16_t),
};
static const Field cl_alpha_fields[] = {CL_FIELD(ColorAlpha, "A", a, uint8_t)};
static const Field cl_alpha16_fields[] = {CL_FIELD(ColorAlpha16, "A", a, uint16_t)};
static const Field cl_gray_fields[] = {CL_FIELD(ColorGray, "Y", y, uint8_t)};
static const Field cl_gray16_fields[] = {CL_FIELD(ColorGray16, "Y", y, uint16_t)};
static const Field cl_y_cb_cr_fields[] = {
    CL_FIELD(ColorYCbCr, "Y", y, uint8_t),
    CL_FIELD(ColorYCbCr, "Cb", cb, uint8_t),
    CL_FIELD(ColorYCbCr, "Cr", cr, uint8_t),
};
static const Field cl_ny_cb_cr_a_fields[] = {
    CL_FIELD(ColorNYCbCrA, "YCbCr", y_cb_cr, ColorYCbCr),
    CL_FIELD(ColorNYCbCrA, "A", a, uint8_t),
};
static const Field cl_cmyk_fields[] = {
    CL_FIELD(ColorCMYK, "C", c, uint8_t),
    CL_FIELD(ColorCMYK, "M", m, uint8_t),
    CL_FIELD(ColorCMYK, "Y", y, uint8_t),
    CL_FIELD(ColorCMYK, "K", k, uint8_t),
};

CL_TYPE(ColorRGBA, "RGBA", cl_rgba_fields, 0x636c3031U);
CL_TYPE(ColorRGBA64, "RGBA64", cl_rgba64_fields, 0x636c3032U);
CL_TYPE(ColorNRGBA, "NRGBA", cl_nrgba_fields, 0x636c3033U);
CL_TYPE(ColorNRGBA64, "NRGBA64", cl_nrgba64_fields, 0x636c3034U);
CL_TYPE(ColorAlpha, "Alpha", cl_alpha_fields, 0x636c3035U);
CL_TYPE(ColorAlpha16, "Alpha16", cl_alpha16_fields, 0x636c3036U);
CL_TYPE(ColorGray, "Gray", cl_gray_fields, 0x636c3037U);
CL_TYPE(ColorGray16, "Gray16", cl_gray16_fields, 0x636c3038U);
CL_TYPE(ColorYCbCr, "YCbCr", cl_y_cb_cr_fields, 0x636c3039U);
CL_TYPE(ColorNYCbCrA, "NYCbCrA", cl_ny_cb_cr_a_fields, 0x636c3130U);
CL_TYPE(ColorCMYK, "CMYK", cl_cmyk_fields, 0x636c3131U);

/* An interface, and bigger than two words: fmt reads the value inline. */
const Type burrow_type_Color = {
    CL_STR("Color"),
    CL_STR("image/color"),
    KIND_INTERFACE,
    (uint32_t)sizeof(Color),
    (uint16_t)_Alignof(Color),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x636c6f72U, /* "clor" */
    NULL,
};

/* ------------------------------------------------------------ RGBA methods */

ColorRGBAValue color_rgba_rgba(ColorRGBA c) {
    uint32_t r = c.r, g = c.g, b = c.b, a = c.a;
    return (ColorRGBAValue){r | r << 8, g | g << 8, b | b << 8, a | a << 8};
}

ColorRGBAValue color_rgba64_rgba(ColorRGBA64 c) {
    return (ColorRGBAValue){c.r, c.g, c.b, c.a};
}

ColorRGBAValue color_nrgba_rgba(ColorNRGBA c) {
    uint32_t r = c.r, g = c.g, b = c.b, a = c.a;
    r = (r | r << 8) * a / 0xff;
    g = (g | g << 8) * a / 0xff;
    b = (b | b << 8) * a / 0xff;
    return (ColorRGBAValue){r, g, b, a | a << 8};
}

ColorRGBAValue color_nrgba64_rgba(ColorNRGBA64 c) {
    uint32_t a = c.a;
    return (ColorRGBAValue){(uint32_t)c.r * a / 0xffff, (uint32_t)c.g * a / 0xffff,
                            (uint32_t)c.b * a / 0xffff, a};
}

ColorRGBAValue color_alpha_rgba(ColorAlpha c) {
    uint32_t a = c.a;
    a |= a << 8;
    return (ColorRGBAValue){a, a, a, a};
}

ColorRGBAValue color_alpha16_rgba(ColorAlpha16 c) {
    uint32_t a = c.a;
    return (ColorRGBAValue){a, a, a, a};
}

ColorRGBAValue color_gray_rgba(ColorGray c) {
    uint32_t y = c.y;
    y |= y << 8;
    return (ColorRGBAValue){y, y, y, 0xffff};
}

ColorRGBAValue color_gray16_rgba(ColorGray16 c) {
    uint32_t y = c.y;
    return (ColorRGBAValue){y, y, y, 0xffff};
}

/* Go's bit twiddling for clamping v >> shift to [0, max]: when the top byte of
 * v is set, v is either negative, which clamps to 0, or too big, which clamps
 * to max. Go writes the second case as ^(v >> 31), which leans on an
 * arithmetic shift, so this spells it out. */
static inline uint32_t cl_clamp(int32_t v, int shift, uint32_t max) {
    if (((uint32_t)v & 0xff000000U) == 0)
        return (uint32_t)v >> shift;
    return v < 0 ? 0 : max;
}

ColorRGBAValue color_y_cb_cr_rgba(ColorYCbCr c) {
    /* This code is a copy of color_y_cb_cr_to_rgb, except that it returns
     * values in the range [0, 0xffff] instead of [0, 0xff]. Going through an
     * 8-bit ColorRGBA instead would lose some information. */
    int32_t yy1 = (int32_t)c.y * 0x10101;
    int32_t cb1 = (int32_t)c.cb - 128;
    int32_t cr1 = (int32_t)c.cr - 128;
    uint32_t r = cl_clamp(yy1 + 91881 * cr1, 8, 0xffff);
    uint32_t g = cl_clamp(yy1 - 22554 * cb1 - 46802 * cr1, 8, 0xffff);
    uint32_t b = cl_clamp(yy1 + 116130 * cb1, 8, 0xffff);
    return (ColorRGBAValue){r, g, b, 0xffff};
}

ColorRGBAValue color_ny_cb_cr_a_rgba(ColorNYCbCrA c) {
    /* The first part is the same as color_y_cb_cr_rgba, and the second part
     * applies the alpha. */
    ColorRGBAValue v = color_y_cb_cr_rgba(c.y_cb_cr);
    uint32_t a = (uint32_t)c.a * 0x101;
    return (ColorRGBAValue){v.r * a / 0xffff, v.g * a / 0xffff, v.b * a / 0xffff, a};
}

ColorRGBAValue color_cmyk_rgba(ColorCMYK c) {
    /* This code is a copy of color_cmyk_to_rgb, except that it returns values
     * in the range [0, 0xffff] instead of [0, 0xff]. */
    uint32_t w = 0xffff - (uint32_t)c.k * 0x101;
    uint32_t r = (0xffff - (uint32_t)c.c * 0x101) * w / 0xffff;
    uint32_t g = (0xffff - (uint32_t)c.m * 0x101) * w / 0xffff;
    uint32_t b = (0xffff - (uint32_t)c.y * 0x101) * w / 0xffff;
    return (ColorRGBAValue){r, g, b, 0xffff};
}

/* ------------------------------------------------------------ as a Color */

#define CL_IFACE(T, snake, member)                                                     \
    static ColorRGBAValue cl_vt_##snake(const ColorData *d) {                          \
        return color_##snake##_rgba(d->member);                                        \
    }                                                                                  \
    const ColorVT burrow__color_##snake##_vt = {&burrow_type_##T, cl_vt_##snake};      \
    Color color_##snake##_as_color(T c) {                                              \
        Color v;                                                                       \
        memset(&v, 0, sizeof v);                                                       \
        v.vt = &burrow__color_##snake##_vt;                                            \
        v.data.member = c;                                                             \
        return v;                                                                      \
    }

CL_IFACE(ColorRGBA, rgba, rgba)
CL_IFACE(ColorRGBA64, rgba64, rgba64)
CL_IFACE(ColorNRGBA, nrgba, nrgba)
CL_IFACE(ColorNRGBA64, nrgba64, nrgba64)
CL_IFACE(ColorAlpha, alpha, alpha)
CL_IFACE(ColorAlpha16, alpha16, alpha16)
CL_IFACE(ColorGray, gray, gray)
CL_IFACE(ColorGray16, gray16, gray16)
CL_IFACE(ColorYCbCr, y_cb_cr, y_cb_cr)
CL_IFACE(ColorNYCbCrA, ny_cb_cr_a, ny_cb_cr_a)
CL_IFACE(ColorCMYK, cmyk, cmyk)

static inline bool cl_is(Color c, const Type *t) {
    return c.vt != NULL && c.vt->self_type == t;
}

bool color_equal(Color a, Color b) {
    if (a.vt == NULL || b.vt == NULL)
        return a.vt == b.vt;
    const Type *t = a.vt->self_type;
    if (t != b.vt->self_type)
        return false;
    if (t == NULL)
        return a.vt == b.vt &&
               memcmp(a.data.bytes, b.data.bytes, sizeof a.data.bytes) == 0;
    if (t->ops != NULL && t->ops->equal != NULL)
        return t->ops->equal(&a.data, &b.data);
    size_t n = t->size < sizeof a.data ? t->size : sizeof a.data;
    return memcmp(a.data.bytes, b.data.bytes, n) == 0;
}

const ColorGray16 color_black = {0};
const ColorGray16 color_white = {0xffff};
const ColorAlpha16 color_transparent = {0};
const ColorAlpha16 color_opaque = {0xffff};

/* ------------------------------------------------------------ conversions */

ColorYCbCr color_rgb_to_y_cb_cr(uint8_t r, uint8_t g, uint8_t b) {
    /* The JFIF specification says:
     *	Y' =  0.2990*R + 0.5870*G + 0.1140*B
     *	Cb = -0.1687*R - 0.3313*G + 0.5000*B + 128
     *	Cr =  0.5000*R - 0.4187*G - 0.0813*B + 128
     * https://www.w3.org/Graphics/JPEG/jfif3.pdf says Y but means Y'. */
    int32_t r1 = r, g1 = g, b1 = b;
    /* yy is in range [0,0xff]. Note that 19595 + 38470 + 7471 equals 65536. */
    int32_t yy = (19595 * r1 + 38470 * g1 + 7471 * b1 + (1 << 15)) >> 16;
    /* Note that -11056 - 21712 + 32768 equals 0. */
    uint32_t cb =
        cl_clamp(-11056 * r1 - 21712 * g1 + 32768 * b1 + (257 << 15), 16, 0xff);
    /* Note that 32768 - 27440 - 5328 equals 0. */
    uint32_t cr = cl_clamp(32768 * r1 - 27440 * g1 - 5328 * b1 + (257 << 15), 16, 0xff);
    return (ColorYCbCr){(uint8_t)yy, (uint8_t)cb, (uint8_t)cr};
}

ColorRGBA color_y_cb_cr_to_rgb(uint8_t y, uint8_t cb, uint8_t cr) {
    /* The JFIF specification says:
     *	R = Y' + 1.40200*(Cr-128)
     *	G = Y' - 0.34414*(Cb-128) - 0.71414*(Cr-128)
     *	B = Y' + 1.77200*(Cb-128)
     * The factors are multiplied by 1<<16 and rounded, and the rounding
     * adjustment is 257*Y', so that YCbCr{y, 0x80, 0x80} comes out the same as
     * Gray{y}. Go's ycbcr.go has the whole derivation. */
    int32_t yy1 = (int32_t)y * 0x10101;
    int32_t cb1 = (int32_t)cb - 128;
    int32_t cr1 = (int32_t)cr - 128;
    uint32_t r = cl_clamp(yy1 + 91881 * cr1, 16, 0xff);
    uint32_t g = cl_clamp(yy1 - 22554 * cb1 - 46802 * cr1, 16, 0xff);
    uint32_t b = cl_clamp(yy1 + 116130 * cb1, 16, 0xff);
    return (ColorRGBA){(uint8_t)r, (uint8_t)g, (uint8_t)b, 0xff};
}

ColorCMYK color_rgb_to_cmyk(uint8_t r, uint8_t g, uint8_t b) {
    uint32_t rr = r, gg = g, bb = b;
    uint32_t w = rr;
    if (w < gg)
        w = gg;
    if (w < bb)
        w = bb;
    if (w == 0)
        return (ColorCMYK){0, 0, 0, 0xff};
    uint32_t c = (w - rr) * 0xff / w;
    uint32_t m = (w - gg) * 0xff / w;
    uint32_t y = (w - bb) * 0xff / w;
    return (ColorCMYK){(uint8_t)c, (uint8_t)m, (uint8_t)y, (uint8_t)(0xff - w)};
}

ColorRGBA color_cmyk_to_rgb(uint8_t c, uint8_t m, uint8_t y, uint8_t k) {
    uint32_t w = 0xffff - (uint32_t)k * 0x101;
    uint32_t r = (0xffff - (uint32_t)c * 0x101) * w / 0xffff;
    uint32_t g = (0xffff - (uint32_t)m * 0x101) * w / 0xffff;
    uint32_t b = (0xffff - (uint32_t)y * 0x101) * w / 0xffff;
    return (ColorRGBA){(uint8_t)(r >> 8), (uint8_t)(g >> 8), (uint8_t)(b >> 8), 0xff};
}

/* ----------------------------------------------------------------- models */

static Color cl_func_convert(void *self, Color c) {
    const ColorModelFunc *f = (const ColorModelFunc *)self;
    return f->f(f->env, c);
}

static const ColorModelVT cl_func_vt = {NULL, cl_func_convert};

ColorModel color_model_func(const ColorModelFunc *f) {
    return (ColorModel){&cl_func_vt, (void *)(uintptr_t)f};
}

static Color cl_rgba_model(void *env, Color c) {
    (void)env;
    if (cl_is(c, &burrow_type_ColorRGBA))
        return c;
    ColorRGBAValue v = color_rgba(c);
    return color_rgba_as_color((ColorRGBA){(uint8_t)(v.r >> 8), (uint8_t)(v.g >> 8),
                                           (uint8_t)(v.b >> 8), (uint8_t)(v.a >> 8)});
}

static Color cl_rgba64_model(void *env, Color c) {
    (void)env;
    if (cl_is(c, &burrow_type_ColorRGBA64))
        return c;
    ColorRGBAValue v = color_rgba(c);
    return color_rgba64_as_color(
        (ColorRGBA64){(uint16_t)v.r, (uint16_t)v.g, (uint16_t)v.b, (uint16_t)v.a});
}

static Color cl_nrgba_model(void *env, Color c) {
    (void)env;
    if (cl_is(c, &burrow_type_ColorNRGBA))
        return c;
    ColorRGBAValue v = color_rgba(c);
    if (v.a == 0xffff)
        return color_nrgba_as_color((ColorNRGBA){
            (uint8_t)(v.r >> 8), (uint8_t)(v.g >> 8), (uint8_t)(v.b >> 8), 0xff});
    if (v.a == 0)
        return color_nrgba_as_color((ColorNRGBA){0, 0, 0, 0});
    /* Since Color.RGBA returns an alpha-premultiplied color, we should have r
     * <= a && g <= a && b <= a. */
    uint32_t r = (v.r * 0xffff) / v.a;
    uint32_t g = (v.g * 0xffff) / v.a;
    uint32_t b = (v.b * 0xffff) / v.a;
    return color_nrgba_as_color((ColorNRGBA){(uint8_t)(r >> 8), (uint8_t)(g >> 8),
                                             (uint8_t)(b >> 8), (uint8_t)(v.a >> 8)});
}

static Color cl_nrgba64_model(void *env, Color c) {
    (void)env;
    if (cl_is(c, &burrow_type_ColorNRGBA64))
        return c;
    ColorRGBAValue v = color_rgba(c);
    if (v.a == 0xffff)
        return color_nrgba64_as_color(
            (ColorNRGBA64){(uint16_t)v.r, (uint16_t)v.g, (uint16_t)v.b, 0xffff});
    if (v.a == 0)
        return color_nrgba64_as_color((ColorNRGBA64){0, 0, 0, 0});
    /* Since Color.RGBA returns an alpha-premultiplied color, we should have r
     * <= a && g <= a && b <= a. */
    uint32_t r = (v.r * 0xffff) / v.a;
    uint32_t g = (v.g * 0xffff) / v.a;
    uint32_t b = (v.b * 0xffff) / v.a;
    return color_nrgba64_as_color(
        (ColorNRGBA64){(uint16_t)r, (uint16_t)g, (uint16_t)b, (uint16_t)v.a});
}

static Color cl_alpha_model(void *env, Color c) {
    (void)env;
    if (cl_is(c, &burrow_type_ColorAlpha))
        return c;
    return color_alpha_as_color((ColorAlpha){(uint8_t)(color_rgba(c).a >> 8)});
}

static Color cl_alpha16_model(void *env, Color c) {
    (void)env;
    if (cl_is(c, &burrow_type_ColorAlpha16))
        return c;
    return color_alpha16_as_color((ColorAlpha16){(uint16_t)color_rgba(c).a});
}

static Color cl_gray_model(void *env, Color c) {
    (void)env;
    if (cl_is(c, &burrow_type_ColorGray))
        return c;
    ColorRGBAValue v = color_rgba(c);
    /* These coefficients (the fractions 0.299, 0.587 and 0.114) are the same
     * as those given by the JFIF specification and used by
     * color_rgb_to_y_cb_cr. Note that 19595 + 38470 + 7471 equals 65536.
     *
     * The 24 is 16 + 8. The 16 is the same as used in color_rgb_to_y_cb_cr.
     * The 8 is because the return value is 8 bit color, not 16 bit color. */
    uint32_t y = (19595 * v.r + 38470 * v.g + 7471 * v.b + (1U << 15)) >> 24;
    return color_gray_as_color((ColorGray){(uint8_t)y});
}

static Color cl_gray16_model(void *env, Color c) {
    (void)env;
    if (cl_is(c, &burrow_type_ColorGray16))
        return c;
    ColorRGBAValue v = color_rgba(c);
    /* These coefficients (the fractions 0.299, 0.587 and 0.114) are the same
     * as those given by the JFIF specification and used by
     * color_rgb_to_y_cb_cr. Note that 19595 + 38470 + 7471 equals 65536. */
    uint32_t y = (19595 * v.r + 38470 * v.g + 7471 * v.b + (1U << 15)) >> 16;
    return color_gray16_as_color((ColorGray16){(uint16_t)y});
}

static Color cl_y_cb_cr_model(void *env, Color c) {
    (void)env;
    if (cl_is(c, &burrow_type_ColorYCbCr))
        return c;
    ColorRGBAValue v = color_rgba(c);
    return color_y_cb_cr_as_color(color_rgb_to_y_cb_cr(
        (uint8_t)(v.r >> 8), (uint8_t)(v.g >> 8), (uint8_t)(v.b >> 8)));
}

static Color cl_ny_cb_cr_a_model(void *env, Color c) {
    (void)env;
    if (cl_is(c, &burrow_type_ColorNYCbCrA))
        return c;
    if (cl_is(c, &burrow_type_ColorYCbCr))
        return color_ny_cb_cr_a_as_color((ColorNYCbCrA){c.data.y_cb_cr, 0xff});
    ColorRGBAValue v = color_rgba(c);
    /* Convert from alpha-premultiplied to non-alpha-premultiplied. */
    if (v.a != 0) {
        v.r = (v.r * 0xffff) / v.a;
        v.g = (v.g * 0xffff) / v.a;
        v.b = (v.b * 0xffff) / v.a;
    }
    ColorYCbCr y = color_rgb_to_y_cb_cr((uint8_t)(v.r >> 8), (uint8_t)(v.g >> 8),
                                        (uint8_t)(v.b >> 8));
    return color_ny_cb_cr_a_as_color((ColorNYCbCrA){y, (uint8_t)(v.a >> 8)});
}

static Color cl_cmyk_model(void *env, Color c) {
    (void)env;
    if (cl_is(c, &burrow_type_ColorCMYK))
        return c;
    ColorRGBAValue v = color_rgba(c);
    return color_cmyk_as_color(color_rgb_to_cmyk(
        (uint8_t)(v.r >> 8), (uint8_t)(v.g >> 8), (uint8_t)(v.b >> 8)));
}

#define CL_MODEL(snake)                                                                \
    static const ColorModelFunc cl_##snake##_model_fn = {cl_##snake##_model, NULL};    \
    const ColorModel color_##snake##_model = {                                         \
        &cl_func_vt, (void *)(uintptr_t)&cl_##snake##_model_fn};

CL_MODEL(rgba)
CL_MODEL(rgba64)
CL_MODEL(nrgba)
CL_MODEL(nrgba64)
CL_MODEL(alpha)
CL_MODEL(alpha16)
CL_MODEL(gray)
CL_MODEL(gray16)
CL_MODEL(y_cb_cr)
CL_MODEL(ny_cb_cr_a)
CL_MODEL(cmyk)

/* --------------------------------------------------------------- palettes */

/* sqDiff returns the squared-difference of x and y, shifted by 2 so that
 * adding four of those won't overflow a uint32.
 *
 * x and y are both assumed to be in the range [0, 0xffff]. */
static inline uint32_t cl_sq_diff(uint32_t x, uint32_t y) {
    /* The canonical code of this function looks as follows:
     *
     *	var d uint32
     *	if x > y {
     *		d = x - y
     *	} else {
     *		d = y - x
     *	}
     *	return (d * d) >> 2
     *
     * Language spec guarantees the following properties of unsigned integer
     * values operations with respect to overflow/wrap around:
     *
     * > For unsigned integer values, the operations +, -, *, and << are
     * > computed modulo 2n, where n is the bit width of the unsigned
     * > integer's type. Loosely speaking, these unsigned integer operations
     * > discard high bits upon overflow, and programs may rely on "wrap
     * > around".
     *
     * Considering these properties and the fact that this function is called
     * in the hot paths (x,y loops), it is reduced to the below code which is
     * slightly faster. See TestSqDiff for correctness check. C's unsigned
     * arithmetic wraps the same way. */
    uint32_t d = x - y;
    return (d * d) >> 2;
}

Int color_palette_index(ColorPalette p, Color c) {
    /* A batch version of this would be cool. */
    ColorRGBAValue cv = color_rgba(c);
    Int ret = 0;
    uint32_t best_sum = UINT32_MAX;
    const Color *pc = (const Color *)p.p;
    for (Int i = 0; i < p.len; i++) {
        ColorRGBAValue v = color_rgba(pc[i]);
        uint32_t sum = cl_sq_diff(cv.r, v.r) + cl_sq_diff(cv.g, v.g) +
                       cl_sq_diff(cv.b, v.b) + cl_sq_diff(cv.a, v.a);
        if (sum < best_sum) {
            if (sum == 0)
                return i;
            ret = i;
            best_sum = sum;
        }
    }
    return ret;
}

Color color_palette_convert(ColorPalette p, Color c) {
    if (p.len == 0) {
        Color nil;
        memset(&nil, 0, sizeof nil);
        return nil;
    }
    return ((const Color *)p.p)[color_palette_index(p, c)];
}

static Color cl_palette_convert(void *self, Color c) {
    return color_palette_convert(*(const ColorPalette *)self, c);
}

static const ColorModelVT cl_palette_vt = {NULL, cl_palette_convert};

ColorModel color_palette_as_model(const ColorPalette *p) {
    return (ColorModel){&cl_palette_vt, (void *)(uintptr_t)p};
}

bool color_model_as_palette(ColorModel m, const ColorPalette **p) {
    if (m.vt != &cl_palette_vt)
        return false;
    if (p != NULL)
        *p = (const ColorPalette *)m.data;
    return true;
}

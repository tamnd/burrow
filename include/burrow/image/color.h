/* image/color: colors, color models and palettes.
 *
 * A Color is anything that can give its red, green, blue and alpha as four
 * alpha-premultiplied values in [0, 0xffff]. The package has a struct for each
 * common layout, ColorRGBA, ColorGray16, ColorYCbCr and the rest, and a Color
 * holds any of them:
 *
 *     Color red = color_rgba_as_color((ColorRGBA){0xff, 0, 0, 0xff});
 *     ColorRGBAValue v = color_rgba(red);            // v.r == 0xffff
 *     Color g = color_model_convert(color_gray_model, red);
 *     // g holds a ColorGray: g.vt->self_type == TYPE_OF(ColorGray)
 *     // and g.data.gray.y == 76
 *
 * HOW A COLOR IS HELD
 *
 * Every other interface in burrow is a vtable pointer and a data pointer. Color
 * is a vtable pointer and the value itself, in a 16 byte union after it. Go
 * boxes a small value when it goes into an interface, and image.At returns one
 * for every pixel it is asked about, so a Color that pointed somewhere would
 * need an allocation per pixel or a caller-provided slot per call. All of the
 * package's colors fit in 8 bytes, so they are stored inline. A color type of
 * your own can store up to 16 bytes the same way, or a pointer in data.ptr.
 *
 * The rest is as for any interface. The vtable's first member is the concrete
 * type, a zeroed Color is nil, and a type assertion is a comparison:
 *
 *     if (c.vt != NULL && c.vt->self_type == TYPE_OF(ColorRGBA))
 *         use(c.data.rgba);
 *
 * fmt knows the layout from the descriptor, TYPE_OF(Color), which is bigger
 * than two words: printing a Color, or a ColorPalette, prints the colors in it
 * the way Go does.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package image/color */

#ifndef BURROW_IMAGE_COLOR_H
#define BURROW_IMAGE_COLOR_H

#include "burrow/core.h"
#include "burrow/func.h"
#include "burrow/own.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------------- colors */

/* color.RGBA: 8 bits for each of red, green, blue and alpha, with red, green
 * and blue already multiplied by alpha. */
typedef struct ColorRGBA {
    uint8_t r, g, b, a;
} ColorRGBA;

/* color.RGBA64: the same with 16 bits each. */
typedef struct ColorRGBA64 {
    uint16_t r, g, b, a;
} ColorRGBA64;

/* color.NRGBA: 8 bits each, not multiplied by alpha. */
typedef struct ColorNRGBA {
    uint8_t r, g, b, a;
} ColorNRGBA;

/* color.NRGBA64: 16 bits each, not multiplied by alpha. */
typedef struct ColorNRGBA64 {
    uint16_t r, g, b, a;
} ColorNRGBA64;

/* color.Alpha: 8 bits of alpha. */
typedef struct ColorAlpha {
    uint8_t a;
} ColorAlpha;

/* color.Alpha16: 16 bits of alpha. */
typedef struct ColorAlpha16 {
    uint16_t a;
} ColorAlpha16;

/* color.Gray: 8 bits of gray, fully opaque. */
typedef struct ColorGray {
    uint8_t y;
} ColorGray;

/* color.Gray16: 16 bits of gray, fully opaque. */
typedef struct ColorGray16 {
    uint16_t y;
} ColorGray16;

/* color.YCbCr: a fully opaque Y'CbCr color, 8 bits for luma and each of the
 * two chroma components, converted to and from RGB the way JFIF says. */
typedef struct ColorYCbCr {
    uint8_t y, cb, cr;
} ColorYCbCr;

/* color.NYCbCrA: a Y'CbCr color with 8 bits of alpha, not multiplied by it. Go
 * embeds YCbCr, and y_cb_cr is that field. */
typedef struct ColorNYCbCrA {
    ColorYCbCr y_cb_cr;
    uint8_t a;
} ColorNYCbCrA;

/* color.CMYK: a fully opaque color with 8 bits of each of cyan, magenta,
 * yellow and black, not tied to any color profile. */
typedef struct ColorCMYK {
    uint8_t c, m, y, k;
} ColorCMYK;

/* The four results of Color.RGBA: alpha-premultiplied red, green, blue and
 * alpha, each in [0, 0xffff] and held in 32 bits so that multiplying two of
 * them cannot overflow. */
typedef struct ColorRGBAValue {
    uint32_t r, g, b, a;
} ColorRGBAValue;

/* ------------------------------------------------------------------ Color */

/* Where a Color keeps its value: one of the package's colors, or up to 16
 * bytes or a pointer for a color type of your own. */
typedef union ColorData {
    ColorRGBA rgba;
    ColorRGBA64 rgba64;
    ColorNRGBA nrgba;
    ColorNRGBA64 nrgba64;
    ColorAlpha alpha;
    ColorAlpha16 alpha16;
    ColorGray gray;
    ColorGray16 gray16;
    ColorYCbCr y_cb_cr;
    ColorNYCbCrA ny_cb_cr_a;
    ColorCMYK cmyk;
    void *ptr;
    uint64_t words[2];
    Byte bytes[16];
} ColorData;

/* color.Color's method table. rgba gets the data of the Color it is called
 * on. */
typedef struct ColorVT {
    const Type *self_type;
    ColorRGBAValue (*rgba)(const ColorData *self);
} ColorVT;

/* color.Color. See the top of this header for why the value is inline. */
typedef struct Color {
    const ColorVT *vt;
    ColorData data;
} Color;

extern const Type burrow_type_Color;

/* Color.RGBA: calls c's method. */
static inline ColorRGBAValue color_rgba(Color c) {
    return c.vt->rgba(&c.data);
}

/* Whether c is nil. */
static inline bool color_is_nil(Color c) {
    return c.vt == NULL;
}

/* Whether a and b are the same type and the same value, Go's == on two
 * interface values. Two nil Colors are equal. A color type of your own is
 * compared by its descriptor's equal, or byte for byte over its size. */
bool color_equal(Color a, Color b);

/* The RGBA methods of the package's colors, and each color as a Color. Go's
 * RGBA has four results, and here they come back as one ColorRGBAValue. */
ColorRGBAValue color_rgba_rgba(ColorRGBA c);
ColorRGBAValue color_rgba64_rgba(ColorRGBA64 c);
ColorRGBAValue color_nrgba_rgba(ColorNRGBA c);
ColorRGBAValue color_nrgba64_rgba(ColorNRGBA64 c);
ColorRGBAValue color_alpha_rgba(ColorAlpha c);
ColorRGBAValue color_alpha16_rgba(ColorAlpha16 c);
ColorRGBAValue color_gray_rgba(ColorGray c);
ColorRGBAValue color_gray16_rgba(ColorGray16 c);
ColorRGBAValue color_y_cb_cr_rgba(ColorYCbCr c);
ColorRGBAValue color_ny_cb_cr_a_rgba(ColorNYCbCrA c);
ColorRGBAValue color_cmyk_rgba(ColorCMYK c);

Color color_rgba_as_color(ColorRGBA c);
Color color_rgba64_as_color(ColorRGBA64 c);
Color color_nrgba_as_color(ColorNRGBA c);
Color color_nrgba64_as_color(ColorNRGBA64 c);
Color color_alpha_as_color(ColorAlpha c);
Color color_alpha16_as_color(ColorAlpha16 c);
Color color_gray_as_color(ColorGray c);
Color color_gray16_as_color(ColorGray16 c);
Color color_y_cb_cr_as_color(ColorYCbCr c);
Color color_ny_cb_cr_a_as_color(ColorNYCbCrA c);
Color color_cmyk_as_color(ColorCMYK c);

extern const Type burrow_type_ColorRGBA;
extern const Type burrow_type_ColorRGBA64;
extern const Type burrow_type_ColorNRGBA;
extern const Type burrow_type_ColorNRGBA64;
extern const Type burrow_type_ColorAlpha;
extern const Type burrow_type_ColorAlpha16;
extern const Type burrow_type_ColorGray;
extern const Type burrow_type_ColorGray16;
extern const Type burrow_type_ColorYCbCr;
extern const Type burrow_type_ColorNYCbCrA;
extern const Type burrow_type_ColorCMYK;

/* The vtables the functions above put in a Color. A table of colors that has
 * to be a constant, such as image/color/palette's, can use them directly:
 * {&burrow__color_rgba_vt, {.rgba = {0x33, 0x66, 0x99, 0xff}}}. */
extern const ColorVT burrow__color_rgba_vt;
extern const ColorVT burrow__color_rgba64_vt;
extern const ColorVT burrow__color_nrgba_vt;
extern const ColorVT burrow__color_nrgba64_vt;
extern const ColorVT burrow__color_alpha_vt;
extern const ColorVT burrow__color_alpha16_vt;
extern const ColorVT burrow__color_gray_vt;
extern const ColorVT burrow__color_gray16_vt;
extern const ColorVT burrow__color_y_cb_cr_vt;
extern const ColorVT burrow__color_ny_cb_cr_a_vt;
extern const ColorVT burrow__color_cmyk_vt;

/* color.Black, White, Transparent and Opaque. */
extern const ColorGray16 color_black;
extern const ColorGray16 color_white;
extern const ColorAlpha16 color_transparent;
extern const ColorAlpha16 color_opaque;

/* ------------------------------------------------------------ conversions */

/* color.RGBToYCbCr, YCbCrToRGB, RGBToCMYK and CMYKToRGB, returning the
 * package's struct for the triple or quadruple. */
ColorYCbCr color_rgb_to_y_cb_cr(uint8_t r, uint8_t g, uint8_t b);
ColorRGBA color_y_cb_cr_to_rgb(uint8_t y, uint8_t cb, uint8_t cr);
ColorCMYK color_rgb_to_cmyk(uint8_t r, uint8_t g, uint8_t b);
ColorRGBA color_cmyk_to_rgb(uint8_t c, uint8_t m, uint8_t y, uint8_t k);

/* ----------------------------------------------------------------- models */

/* color.Model: converts any Color to one of its own. */
typedef struct ColorModelVT {
    const Type *self_type;
    Color (*convert)(void *self, Color c);
} ColorModelVT;

typedef struct ColorModel {
    const ColorModelVT *vt;
    void *data;
} ColorModel;

static inline Color color_model_convert(ColorModel m, Color c) {
    return m.vt->convert(m.data, c);
}

/* Go's func(Color) Color. */
BURROW_FUNC(ColorModelFunc, Color, Color c);

/* color.ModelFunc: a Model that calls f. The Model points at f, which has to
 * outlive it. */
BURROW_BORROWS(ret, f) ColorModel color_model_func(const ColorModelFunc *f);

/* The package's models. Each returns a color it is given unchanged when that
 * is already its type, and converts anything else. */
extern const ColorModel color_rgba_model;
extern const ColorModel color_rgba64_model;
extern const ColorModel color_nrgba_model;
extern const ColorModel color_nrgba64_model;
extern const ColorModel color_alpha_model;
extern const ColorModel color_alpha16_model;
extern const ColorModel color_gray_model;
extern const ColorModel color_gray16_model;
extern const ColorModel color_y_cb_cr_model;
extern const ColorModel color_ny_cb_cr_a_model;
extern const ColorModel color_cmyk_model;

/* --------------------------------------------------------------- palettes */

/* color.Palette: a slice of Color, with TYPE_OF(Color) as its element type. */
typedef Slice ColorPalette;

/* Palette.Convert: the color in p closest to c in Euclidean R,G,B,A space. A
 * nil Color for an empty palette. */
Color color_palette_convert(ColorPalette p, Color c);

/* Palette.Index: the index of that color, the first one on a tie, and 0 for
 * an empty palette. */
Int color_palette_index(ColorPalette p, Color c);

/* A palette as a Model. It points at p, which has to outlive it. */
BURROW_BORROWS(ret, p) ColorModel color_palette_as_model(const ColorPalette *p);

/* m.(color.Palette): whether m is a palette made a Model by
 * color_palette_as_model, and if it is, a pointer to the palette in *p. */
bool color_model_as_palette(ColorModel m, const ColorPalette **p);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_IMAGE_COLOR_H */

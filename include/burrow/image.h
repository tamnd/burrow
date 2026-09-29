/* image: points, rectangles, and images held in memory.
 *
 * An Image is a rectangle of colors. The package has a type for each common
 * pixel layout, ImageRGBA, ImageGray, ImageYCbCr, ImagePaletted and the rest,
 * each a slice of bytes with a stride and the rectangle it covers, and an Image
 * holds a pointer to any of them:
 *
 *     ImageRGBA *m = image_new_rgba(a, image_rect(0, 0, 640, 480));
 *     image_rgba_set_rgba(m, 10, 20, (ColorRGBA){0xff, 0, 0, 0xff});
 *     Image img = image_rgba_as_image(m);
 *     Color c = image_at(img, 10, 20);  // holds a ColorRGBA
 *     ...
 *     image_rgba_free(m, a);
 *
 * Every function Go has on a type is here with the type's prefix, and the
 * ones that read or write a single pixel work on the concrete struct without a
 * call through a vtable, which is what makes them worth calling in a loop.
 *
 * WHAT AN IMAGE CAN DO
 *
 * Go's image.Image has three methods, and other interfaces add one each:
 * RGBA64Image adds RGBA64At, PalettedImage adds ColorIndexAt, and image/draw's
 * Image and RGBA64Image add Set and SetRGBA64. Code that is handed an Image asks
 * with a type assertion whether it can do more, image/draw on every call. A C
 * vtable cannot be asked for a method it was not built with, so ImageVT has a
 * slot for every one of them, and a type that does not have a method leaves its
 * slot NULL. The package's own types fill in everything they have:
 *
 *     if (img.vt->rgba64_at != NULL)
 *         ColorRGBA64 c = img.vt->rgba64_at(img.data, x, y);
 *
 * ImageRGBA64Image and ImagePalettedImage are the same type as Image, named
 * for what the vtable promises.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package image */

#ifndef BURROW_IMAGE_H
#define BURROW_IMAGE_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/func.h"
#include "burrow/image/color.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/own.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ Point */

/* image.Point: an X, Y pair. The axes increase right and down. */
typedef struct ImagePoint {
    Int x, y;
} ImagePoint;

extern const Type burrow_type_ImagePoint;

/* image.Pt. */
static inline ImagePoint image_pt(Int x, Int y) {
    ImagePoint p = {x, y};
    return p;
}

/* image.ZP, the zero Point. Deprecated in Go in favour of a literal. */
extern const ImagePoint image_zp;

/* Point.String: "(x,y)". */
BURROW_OWNS(ret) Str image_point_string(ImagePoint p, Alloc *a);

ImagePoint image_point_add(ImagePoint p, ImagePoint q);
ImagePoint image_point_sub(ImagePoint p, ImagePoint q);
ImagePoint image_point_mul(ImagePoint p, Int k);

/* Point.Div. Dividing by zero panics, as in Go. */
ImagePoint image_point_div(ImagePoint p, Int k);

/* ------------------------------------------------------------- Rectangle */

/* image.Rectangle: the points with min.x <= x < max.x and min.y <= y < max.y.
 * It is well formed when min.x <= max.x and min.y <= max.y, and image_rect
 * always makes one that is. */
typedef struct ImageRectangle {
    ImagePoint min, max;
} ImageRectangle;

extern const Type burrow_type_ImageRectangle;

/* image.Rect: the rectangle with corners (x0, y0) and (x1, y1), swapped as
 * needed to be well formed. */
ImageRectangle image_rect(Int x0, Int y0, Int x1, Int y1);

/* image.ZR, the zero Rectangle. Deprecated in Go in favour of a literal. */
extern const ImageRectangle image_zr;

/* Point.In: whether p is inside r. */
bool image_point_in(ImagePoint p, ImageRectangle r);

/* Point.Mod: the point q in r such that p - q is a multiple of r's size. r
 * has to be non-empty, and an empty one divides by zero and panics. */
ImagePoint image_point_mod(ImagePoint p, ImageRectangle r);

/* Point.Eq. */
bool image_point_eq(ImagePoint p, ImagePoint q);

/* Rectangle.String: "(x0,y0)-(x1,y1)". */
BURROW_OWNS(ret) Str image_rectangle_string(ImageRectangle r, Alloc *a);

Int image_rectangle_dx(ImageRectangle r);
Int image_rectangle_dy(ImageRectangle r);
ImagePoint image_rectangle_size(ImageRectangle r);
ImageRectangle image_rectangle_add(ImageRectangle r, ImagePoint p);
ImageRectangle image_rectangle_sub(ImageRectangle r, ImagePoint p);

/* Rectangle.Inset: r moved in by n on every side, or out for a negative n. A
 * side too short to lose 2n collapses to its midpoint. */
ImageRectangle image_rectangle_inset(ImageRectangle r, Int n);

/* Rectangle.Intersect and Union. An empty intersection is the zero
 * rectangle, and an empty argument to a union is ignored. */
ImageRectangle image_rectangle_intersect(ImageRectangle r, ImageRectangle s);
ImageRectangle image_rectangle_union(ImageRectangle r, ImageRectangle s);

bool image_rectangle_empty(ImageRectangle r);

/* Rectangle.Eq: the same points, so every empty rectangle equals every
 * other. */
bool image_rectangle_eq(ImageRectangle r, ImageRectangle s);

bool image_rectangle_overlaps(ImageRectangle r, ImageRectangle s);

/* Rectangle.In: whether every point of r is in s. */
bool image_rectangle_in(ImageRectangle r, ImageRectangle s);

/* Rectangle.Canon: r with its corners swapped as needed to be well formed. */
ImageRectangle image_rectangle_canon(ImageRectangle r);

/* A Rectangle is also an Image, of an infinite opaque mask: opaque inside and
 * transparent outside, in color_alpha16_model. */
Color image_rectangle_at(ImageRectangle r, Int x, Int y);
ColorRGBA64 image_rectangle_rgba64_at(ImageRectangle r, Int x, Int y);
ImageRectangle image_rectangle_bounds(ImageRectangle r);
ColorModel image_rectangle_color_model(ImageRectangle r);

/* ------------------------------------------------------------------ Image */

/* image.Image's method table, with a slot for each optional method as well.
 * See the top of this header. The optional slots are NULL on a type that does
 * not have the method. */
typedef struct ImageVT {
    const Type *self_type;
    ColorModel (*color_model)(void *self);
    ImageRectangle (*bounds)(void *self);
    Color (*at)(void *self, Int x, Int y);
    /* RGBA64Image. */
    ColorRGBA64 (*rgba64_at)(void *self, Int x, Int y);
    /* PalettedImage. */
    uint8_t (*color_index_at)(void *self, Int x, Int y);
    /* image/draw's Image and RGBA64Image. */
    void (*set)(void *self, Int x, Int y, Color c);
    void (*set_rgba64)(void *self, Int x, Int y, ColorRGBA64 c);
    /* interface{ Opaque() bool }, which image/draw and the encoders ask
     * for. */
    bool (*opaque)(void *self);
} ImageVT;

/* image.Image. */
typedef struct Image {
    const ImageVT *vt;
    void *data;
} Image;

extern const Type burrow_type_Image;

static inline ColorModel image_color_model(Image m) {
    return m.vt->color_model(m.data);
}

static inline ImageRectangle image_bounds(Image m) {
    return m.vt->bounds(m.data);
}

static inline Color image_at(Image m, Int x, Int y) {
    return m.vt->at(m.data, x, y);
}

/* image.RGBA64Image: an Image whose rgba64_at is set. */
typedef ImageVT ImageRGBA64ImageVT;
typedef Image ImageRGBA64Image;

/* image.PalettedImage: an Image whose color_index_at is set. */
typedef ImageVT ImagePalettedImageVT;
typedef Image ImagePalettedImage;

/* image.Config: an image's color model and size, what DecodeConfig reads
 * without decoding the pixels. */
typedef struct ImageConfig {
    ColorModel color_model;
    Int width, height;
} ImageConfig;

extern const Type burrow_type_ImageConfig;

/* A Rectangle as an Image. It points at r, which has to outlive it. */
BURROW_BORROWS(ret, r) Image image_rectangle_as_image(const ImageRectangle *r);

/* --------------------------------------------------- the in-memory images
 *
 * Each of these is Go's struct with the same fields: pix holds the pixels,
 * row by row from rect.min, stride bytes apart, and the pixel at (x, y) starts
 * at pix_offset(x, y).
 *
 * image_new_x allocates one in a, with every pixel zero, and returns NULL when
 * a says no. A rectangle whose size is negative or does not fit in an Int
 * panics with Go's message. image_x_free gives back what image_new_x took, and
 * should not be called on a sub-image or on a struct you filled in yourself.
 *
 * Reading a pixel outside rect gives the zero color, and writing one does
 * nothing. A pix too short for rect panics the way indexing it does in Go.
 *
 * SubImage returns the concrete type by value rather than an Image, since the
 * result shares its pixels with the original and there is nothing new to
 * allocate. Take its address to make an Image of it. */

/* image.RGBA: 4 bytes a pixel, R, G, B, A, alpha-premultiplied. */
typedef struct ImageRGBA {
    Slice pix;
    Int stride;
    ImageRectangle rect;
} ImageRGBA;
extern const Type burrow_type_ImageRGBA;
BURROW_OWNS(ret) ImageRGBA *image_new_rgba(Alloc *a, ImageRectangle r);
void image_rgba_free(ImageRGBA *p, Alloc *a);
ColorModel image_rgba_color_model(const ImageRGBA *p);
ImageRectangle image_rgba_bounds(const ImageRGBA *p);
Color image_rgba_at(const ImageRGBA *p, Int x, Int y);
ColorRGBA64 image_rgba_rgba64_at(const ImageRGBA *p, Int x, Int y);
ColorRGBA image_rgba_rgba_at(const ImageRGBA *p, Int x, Int y);
Int image_rgba_pix_offset(const ImageRGBA *p, Int x, Int y);
void image_rgba_set(ImageRGBA *p, Int x, Int y, Color c);
void image_rgba_set_rgba64(ImageRGBA *p, Int x, Int y, ColorRGBA64 c);
void image_rgba_set_rgba(ImageRGBA *p, Int x, Int y, ColorRGBA c);
ImageRGBA image_rgba_sub_image(const ImageRGBA *p, ImageRectangle r);
bool image_rgba_opaque(const ImageRGBA *p);
BURROW_BORROWS(ret, p) Image image_rgba_as_image(ImageRGBA *p);

/* image.RGBA64: 8 bytes a pixel, big-endian R, G, B, A, alpha-premultiplied.
 * Its typed getter and setter are rgba64_at and set_rgba64. */
typedef struct ImageRGBA64 {
    Slice pix;
    Int stride;
    ImageRectangle rect;
} ImageRGBA64;
extern const Type burrow_type_ImageRGBA64;
BURROW_OWNS(ret) ImageRGBA64 *image_new_rgba64(Alloc *a, ImageRectangle r);
void image_rgba64_free(ImageRGBA64 *p, Alloc *a);
ColorModel image_rgba64_color_model(const ImageRGBA64 *p);
ImageRectangle image_rgba64_bounds(const ImageRGBA64 *p);
Color image_rgba64_at(const ImageRGBA64 *p, Int x, Int y);
ColorRGBA64 image_rgba64_rgba64_at(const ImageRGBA64 *p, Int x, Int y);
Int image_rgba64_pix_offset(const ImageRGBA64 *p, Int x, Int y);
void image_rgba64_set(ImageRGBA64 *p, Int x, Int y, Color c);
void image_rgba64_set_rgba64(ImageRGBA64 *p, Int x, Int y, ColorRGBA64 c);
ImageRGBA64 image_rgba64_sub_image(const ImageRGBA64 *p, ImageRectangle r);
bool image_rgba64_opaque(const ImageRGBA64 *p);
BURROW_BORROWS(ret, p) Image image_rgba64_as_image(ImageRGBA64 *p);

/* image.NRGBA: 4 bytes a pixel, R, G, B, A, not premultiplied. */
typedef struct ImageNRGBA {
    Slice pix;
    Int stride;
    ImageRectangle rect;
} ImageNRGBA;
extern const Type burrow_type_ImageNRGBA;
BURROW_OWNS(ret) ImageNRGBA *image_new_nrgba(Alloc *a, ImageRectangle r);
void image_nrgba_free(ImageNRGBA *p, Alloc *a);
ColorModel image_nrgba_color_model(const ImageNRGBA *p);
ImageRectangle image_nrgba_bounds(const ImageNRGBA *p);
Color image_nrgba_at(const ImageNRGBA *p, Int x, Int y);
ColorRGBA64 image_nrgba_rgba64_at(const ImageNRGBA *p, Int x, Int y);
ColorNRGBA image_nrgba_nrgba_at(const ImageNRGBA *p, Int x, Int y);
Int image_nrgba_pix_offset(const ImageNRGBA *p, Int x, Int y);
void image_nrgba_set(ImageNRGBA *p, Int x, Int y, Color c);
void image_nrgba_set_rgba64(ImageNRGBA *p, Int x, Int y, ColorRGBA64 c);
void image_nrgba_set_nrgba(ImageNRGBA *p, Int x, Int y, ColorNRGBA c);
ImageNRGBA image_nrgba_sub_image(const ImageNRGBA *p, ImageRectangle r);
bool image_nrgba_opaque(const ImageNRGBA *p);
BURROW_BORROWS(ret, p) Image image_nrgba_as_image(ImageNRGBA *p);

/* image.NRGBA64: 8 bytes a pixel, big-endian, not premultiplied. */
typedef struct ImageNRGBA64 {
    Slice pix;
    Int stride;
    ImageRectangle rect;
} ImageNRGBA64;
extern const Type burrow_type_ImageNRGBA64;
BURROW_OWNS(ret) ImageNRGBA64 *image_new_nrgba64(Alloc *a, ImageRectangle r);
void image_nrgba64_free(ImageNRGBA64 *p, Alloc *a);
ColorModel image_nrgba64_color_model(const ImageNRGBA64 *p);
ImageRectangle image_nrgba64_bounds(const ImageNRGBA64 *p);
Color image_nrgba64_at(const ImageNRGBA64 *p, Int x, Int y);
ColorRGBA64 image_nrgba64_rgba64_at(const ImageNRGBA64 *p, Int x, Int y);
ColorNRGBA64 image_nrgba64_nrgba64_at(const ImageNRGBA64 *p, Int x, Int y);
Int image_nrgba64_pix_offset(const ImageNRGBA64 *p, Int x, Int y);
void image_nrgba64_set(ImageNRGBA64 *p, Int x, Int y, Color c);
void image_nrgba64_set_rgba64(ImageNRGBA64 *p, Int x, Int y, ColorRGBA64 c);
void image_nrgba64_set_nrgba64(ImageNRGBA64 *p, Int x, Int y, ColorNRGBA64 c);
ImageNRGBA64 image_nrgba64_sub_image(const ImageNRGBA64 *p, ImageRectangle r);
bool image_nrgba64_opaque(const ImageNRGBA64 *p);
BURROW_BORROWS(ret, p) Image image_nrgba64_as_image(ImageNRGBA64 *p);

/* image.Alpha: 1 byte of alpha a pixel. */
typedef struct ImageAlpha {
    Slice pix;
    Int stride;
    ImageRectangle rect;
} ImageAlpha;
extern const Type burrow_type_ImageAlpha;
BURROW_OWNS(ret) ImageAlpha *image_new_alpha(Alloc *a, ImageRectangle r);
void image_alpha_free(ImageAlpha *p, Alloc *a);
ColorModel image_alpha_color_model(const ImageAlpha *p);
ImageRectangle image_alpha_bounds(const ImageAlpha *p);
Color image_alpha_at(const ImageAlpha *p, Int x, Int y);
ColorRGBA64 image_alpha_rgba64_at(const ImageAlpha *p, Int x, Int y);
ColorAlpha image_alpha_alpha_at(const ImageAlpha *p, Int x, Int y);
Int image_alpha_pix_offset(const ImageAlpha *p, Int x, Int y);
void image_alpha_set(ImageAlpha *p, Int x, Int y, Color c);
void image_alpha_set_rgba64(ImageAlpha *p, Int x, Int y, ColorRGBA64 c);
void image_alpha_set_alpha(ImageAlpha *p, Int x, Int y, ColorAlpha c);
ImageAlpha image_alpha_sub_image(const ImageAlpha *p, ImageRectangle r);
bool image_alpha_opaque(const ImageAlpha *p);
BURROW_BORROWS(ret, p) Image image_alpha_as_image(ImageAlpha *p);

/* image.Alpha16: 2 bytes of alpha a pixel, big-endian. */
typedef struct ImageAlpha16 {
    Slice pix;
    Int stride;
    ImageRectangle rect;
} ImageAlpha16;
extern const Type burrow_type_ImageAlpha16;
BURROW_OWNS(ret) ImageAlpha16 *image_new_alpha16(Alloc *a, ImageRectangle r);
void image_alpha16_free(ImageAlpha16 *p, Alloc *a);
ColorModel image_alpha16_color_model(const ImageAlpha16 *p);
ImageRectangle image_alpha16_bounds(const ImageAlpha16 *p);
Color image_alpha16_at(const ImageAlpha16 *p, Int x, Int y);
ColorRGBA64 image_alpha16_rgba64_at(const ImageAlpha16 *p, Int x, Int y);
ColorAlpha16 image_alpha16_alpha16_at(const ImageAlpha16 *p, Int x, Int y);
Int image_alpha16_pix_offset(const ImageAlpha16 *p, Int x, Int y);
void image_alpha16_set(ImageAlpha16 *p, Int x, Int y, Color c);
void image_alpha16_set_rgba64(ImageAlpha16 *p, Int x, Int y, ColorRGBA64 c);
void image_alpha16_set_alpha16(ImageAlpha16 *p, Int x, Int y, ColorAlpha16 c);
ImageAlpha16 image_alpha16_sub_image(const ImageAlpha16 *p, ImageRectangle r);
bool image_alpha16_opaque(const ImageAlpha16 *p);
BURROW_BORROWS(ret, p) Image image_alpha16_as_image(ImageAlpha16 *p);

/* image.Gray: 1 byte of gray a pixel. */
typedef struct ImageGray {
    Slice pix;
    Int stride;
    ImageRectangle rect;
} ImageGray;
extern const Type burrow_type_ImageGray;
BURROW_OWNS(ret) ImageGray *image_new_gray(Alloc *a, ImageRectangle r);
void image_gray_free(ImageGray *p, Alloc *a);
ColorModel image_gray_color_model(const ImageGray *p);
ImageRectangle image_gray_bounds(const ImageGray *p);
Color image_gray_at(const ImageGray *p, Int x, Int y);
ColorRGBA64 image_gray_rgba64_at(const ImageGray *p, Int x, Int y);
ColorGray image_gray_gray_at(const ImageGray *p, Int x, Int y);
Int image_gray_pix_offset(const ImageGray *p, Int x, Int y);
void image_gray_set(ImageGray *p, Int x, Int y, Color c);
void image_gray_set_rgba64(ImageGray *p, Int x, Int y, ColorRGBA64 c);
void image_gray_set_gray(ImageGray *p, Int x, Int y, ColorGray c);
ImageGray image_gray_sub_image(const ImageGray *p, ImageRectangle r);
bool image_gray_opaque(const ImageGray *p);
BURROW_BORROWS(ret, p) Image image_gray_as_image(ImageGray *p);

/* image.Gray16: 2 bytes of gray a pixel, big-endian. */
typedef struct ImageGray16 {
    Slice pix;
    Int stride;
    ImageRectangle rect;
} ImageGray16;
extern const Type burrow_type_ImageGray16;
BURROW_OWNS(ret) ImageGray16 *image_new_gray16(Alloc *a, ImageRectangle r);
void image_gray16_free(ImageGray16 *p, Alloc *a);
ColorModel image_gray16_color_model(const ImageGray16 *p);
ImageRectangle image_gray16_bounds(const ImageGray16 *p);
Color image_gray16_at(const ImageGray16 *p, Int x, Int y);
ColorRGBA64 image_gray16_rgba64_at(const ImageGray16 *p, Int x, Int y);
ColorGray16 image_gray16_gray16_at(const ImageGray16 *p, Int x, Int y);
Int image_gray16_pix_offset(const ImageGray16 *p, Int x, Int y);
void image_gray16_set(ImageGray16 *p, Int x, Int y, Color c);
void image_gray16_set_rgba64(ImageGray16 *p, Int x, Int y, ColorRGBA64 c);
void image_gray16_set_gray16(ImageGray16 *p, Int x, Int y, ColorGray16 c);
ImageGray16 image_gray16_sub_image(const ImageGray16 *p, ImageRectangle r);
bool image_gray16_opaque(const ImageGray16 *p);
BURROW_BORROWS(ret, p) Image image_gray16_as_image(ImageGray16 *p);

/* image.CMYK: 4 bytes a pixel, C, M, Y, K. */
typedef struct ImageCMYK {
    Slice pix;
    Int stride;
    ImageRectangle rect;
} ImageCMYK;
extern const Type burrow_type_ImageCMYK;
BURROW_OWNS(ret) ImageCMYK *image_new_cmyk(Alloc *a, ImageRectangle r);
void image_cmyk_free(ImageCMYK *p, Alloc *a);
ColorModel image_cmyk_color_model(const ImageCMYK *p);
ImageRectangle image_cmyk_bounds(const ImageCMYK *p);
Color image_cmyk_at(const ImageCMYK *p, Int x, Int y);
ColorRGBA64 image_cmyk_rgba64_at(const ImageCMYK *p, Int x, Int y);
ColorCMYK image_cmyk_cmyk_at(const ImageCMYK *p, Int x, Int y);
Int image_cmyk_pix_offset(const ImageCMYK *p, Int x, Int y);
void image_cmyk_set(ImageCMYK *p, Int x, Int y, Color c);
void image_cmyk_set_rgba64(ImageCMYK *p, Int x, Int y, ColorRGBA64 c);
void image_cmyk_set_cmyk(ImageCMYK *p, Int x, Int y, ColorCMYK c);
ImageCMYK image_cmyk_sub_image(const ImageCMYK *p, ImageRectangle r);
bool image_cmyk_opaque(const ImageCMYK *p);
BURROW_BORROWS(ret, p) Image image_cmyk_as_image(ImageCMYK *p);

/* image.Paletted: 1 byte a pixel, an index into palette. */
typedef struct ImagePaletted {
    Slice pix;
    Int stride;
    ImageRectangle rect;
    ColorPalette palette;
} ImagePaletted;
extern const Type burrow_type_ImagePaletted;

/* *image.Paletted, for a slice of them such as the frames of a GIF. */
typedef ImagePaletted *ImagePalettedPtr;
extern const Type burrow_type_ImagePalettedPtr;

/* image.NewPaletted. The image shares p, which has to outlive it. */
BURROW_OWNS(ret) ImagePaletted *image_new_paletted(Alloc *a, ImageRectangle r,
                                                   ColorPalette p);
void image_paletted_free(ImagePaletted *p, Alloc *a);

/* Paletted.ColorModel: the palette as a Model. It points at p->palette. */
BURROW_BORROWS(ret, p) ColorModel image_paletted_color_model(const ImagePaletted *p);
ImageRectangle image_paletted_bounds(const ImagePaletted *p);

/* Paletted.At: the palette entry at (x, y), entry 0 outside the image, and a
 * nil Color for an empty palette. An index past the end of the palette
 * panics. */
Color image_paletted_at(const ImagePaletted *p, Int x, Int y);
ColorRGBA64 image_paletted_rgba64_at(const ImagePaletted *p, Int x, Int y);
Int image_paletted_pix_offset(const ImagePaletted *p, Int x, Int y);

/* Paletted.Set: stores the index of the palette entry closest to c. */
void image_paletted_set(ImagePaletted *p, Int x, Int y, Color c);
void image_paletted_set_rgba64(ImagePaletted *p, Int x, Int y, ColorRGBA64 c);
uint8_t image_paletted_color_index_at(const ImagePaletted *p, Int x, Int y);
void image_paletted_set_color_index(ImagePaletted *p, Int x, Int y, uint8_t index);
ImagePaletted image_paletted_sub_image(const ImagePaletted *p, ImageRectangle r);

/* Paletted.Opaque: whether every palette entry the image uses is opaque. */
bool image_paletted_opaque(const ImagePaletted *p);
BURROW_BORROWS(ret, p) Image image_paletted_as_image(ImagePaletted *p);

/* ------------------------------------------------------------------ YCbCr */

/* image.YCbCrSubsampleRatio: how many luma samples share one chroma sample. */
typedef Int ImageYCbCrSubsampleRatio;

enum {
    IMAGE_Y_CB_CR_SUBSAMPLE_RATIO444 = 0,
    IMAGE_Y_CB_CR_SUBSAMPLE_RATIO422 = 1,
    IMAGE_Y_CB_CR_SUBSAMPLE_RATIO420 = 2,
    IMAGE_Y_CB_CR_SUBSAMPLE_RATIO440 = 3,
    IMAGE_Y_CB_CR_SUBSAMPLE_RATIO411 = 4,
    IMAGE_Y_CB_CR_SUBSAMPLE_RATIO410 = 5,
};

extern const Type burrow_type_ImageYCbCrSubsampleRatio;

/* YCbCrSubsampleRatio.String: the constant's Go name, such as
 * "YCbCrSubsampleRatio420", or "YCbCrSubsampleRatioUnknown". The result is a
 * string constant. */
BURROW_STATIC(ret) Str image_y_cb_cr_subsample_ratio_string(ImageYCbCrSubsampleRatio s);

/* image.YCbCr: planes of Y', Cb and Cr samples, one Y' per pixel and one of
 * each chroma per group of pixels that the ratio says. The pixel at (x, y) has
 * its Y' at y_offset(x, y) and its chroma at c_offset(x, y). */
typedef struct ImageYCbCr {
    Slice y, cb, cr;
    Int y_stride;
    Int c_stride;
    ImageYCbCrSubsampleRatio subsample_ratio;
    ImageRectangle rect;
} ImageYCbCr;
extern const Type burrow_type_ImageYCbCr;

/* image.NewYCbCr: the three planes are one allocation, which
 * image_y_cb_cr_free gives back. */
BURROW_OWNS(ret) ImageYCbCr *
image_new_y_cb_cr(Alloc *a, ImageRectangle r, ImageYCbCrSubsampleRatio subsample_ratio);
void image_y_cb_cr_free(ImageYCbCr *p, Alloc *a);
ColorModel image_y_cb_cr_color_model(const ImageYCbCr *p);
ImageRectangle image_y_cb_cr_bounds(const ImageYCbCr *p);
Color image_y_cb_cr_at(const ImageYCbCr *p, Int x, Int y);
ColorRGBA64 image_y_cb_cr_rgba64_at(const ImageYCbCr *p, Int x, Int y);
ColorYCbCr image_y_cb_cr_y_cb_cr_at(const ImageYCbCr *p, Int x, Int y);
Int image_y_cb_cr_y_offset(const ImageYCbCr *p, Int x, Int y);
Int image_y_cb_cr_c_offset(const ImageYCbCr *p, Int x, Int y);
ImageYCbCr image_y_cb_cr_sub_image(const ImageYCbCr *p, ImageRectangle r);
bool image_y_cb_cr_opaque(const ImageYCbCr *p);
BURROW_BORROWS(ret, p) Image image_y_cb_cr_as_image(ImageYCbCr *p);

/* image.NYCbCrA: a YCbCr with a plane of alpha, one sample per pixel, not
 * premultiplied. Go embeds YCbCr, and y_cb_cr is that field. */
typedef struct ImageNYCbCrA {
    ImageYCbCr y_cb_cr;
    Slice a;
    Int a_stride;
} ImageNYCbCrA;
extern const Type burrow_type_ImageNYCbCrA;

BURROW_OWNS(ret) ImageNYCbCrA *
image_new_ny_cb_cr_a(Alloc *a, ImageRectangle r,
                     ImageYCbCrSubsampleRatio subsample_ratio);
void image_ny_cb_cr_a_free(ImageNYCbCrA *p, Alloc *a);
ColorModel image_ny_cb_cr_a_color_model(const ImageNYCbCrA *p);
ImageRectangle image_ny_cb_cr_a_bounds(const ImageNYCbCrA *p);
Color image_ny_cb_cr_a_at(const ImageNYCbCrA *p, Int x, Int y);
ColorRGBA64 image_ny_cb_cr_a_rgba64_at(const ImageNYCbCrA *p, Int x, Int y);
ColorNYCbCrA image_ny_cb_cr_a_ny_cb_cr_a_at(const ImageNYCbCrA *p, Int x, Int y);

/* The methods NYCbCrA gets from the YCbCr it embeds. */
ColorYCbCr image_ny_cb_cr_a_y_cb_cr_at(const ImageNYCbCrA *p, Int x, Int y);
Int image_ny_cb_cr_a_y_offset(const ImageNYCbCrA *p, Int x, Int y);
Int image_ny_cb_cr_a_c_offset(const ImageNYCbCrA *p, Int x, Int y);

Int image_ny_cb_cr_a_a_offset(const ImageNYCbCrA *p, Int x, Int y);
ImageNYCbCrA image_ny_cb_cr_a_sub_image(const ImageNYCbCrA *p, ImageRectangle r);
bool image_ny_cb_cr_a_opaque(const ImageNYCbCrA *p);
BURROW_BORROWS(ret, p) Image image_ny_cb_cr_a_as_image(ImageNYCbCrA *p);

/* ---------------------------------------------------------------- Uniform */

/* image.Uniform: an infinite image of one color. It is a Color and a Model
 * too, and as a Model it turns every color into c. */
typedef struct ImageUniform {
    Color c;
} ImageUniform;
extern const Type burrow_type_ImageUniform;

/* image.NewUniform. */
BURROW_OWNS(ret) ImageUniform *image_new_uniform(Alloc *a, Color c);
void image_uniform_free(ImageUniform *u, Alloc *a);

ColorRGBAValue image_uniform_rgba(const ImageUniform *u);
BURROW_BORROWS(ret, u) ColorModel image_uniform_color_model(ImageUniform *u);
Color image_uniform_convert(const ImageUniform *u, Color c);
ImageRectangle image_uniform_bounds(const ImageUniform *u);
Color image_uniform_at(const ImageUniform *u, Int x, Int y);
ColorRGBA64 image_uniform_rgba64_at(const ImageUniform *u, Int x, Int y);
bool image_uniform_opaque(const ImageUniform *u);

/* The Uniform as each of the three interfaces it satisfies. All three point
 * at u. */
BURROW_BORROWS(ret, u) Image image_uniform_as_image(ImageUniform *u);
BURROW_BORROWS(ret, u) Color image_uniform_as_color(ImageUniform *u);

/* image.Black, White, Transparent and Opaque: Uniforms of color_black,
 * color_white, color_transparent and color_opaque. */
extern ImageUniform *const image_black;
extern ImageUniform *const image_white;
extern ImageUniform *const image_transparent;
extern ImageUniform *const image_opaque;

/* ---------------------------------------------------------------- formats */

/* image.ErrFormat: the input's first bytes match no registered format. */
extern const Error image_err_format;

/* The decoders a format registers. The image, and anything else a decoder
 * allocates for the caller, comes from a. */
BURROW_FUNC(ImageDecodeFunc, Image, Alloc *a, IoReader r, Error *err);
BURROW_FUNC(ImageDecodeConfigFunc, ImageConfig, Alloc *a, IoReader r, Error *err);

/* image.RegisterFormat: adds a format for image_decode and
 * image_decode_config to recognise. magic is the prefix that identifies it, in
 * which '?' matches any byte. name and magic are kept, not copied, so they
 * should be string constants. A codec package registers itself this way;
 * image/png's is png_register. Safe to call from any thread. */
void image_register_format(Str name, Str magic, ImageDecodeFunc decode,
                           ImageDecodeConfigFunc decode_config);

/* image.Decode: finds the first registered format whose magic the input
 * starts with and decodes with it. name, if not NULL, gets the format's name.
 *
 * The format is recognised by peeking at the input, so r is read through a
 * bufio.Reader, which is r itself when r already is one. */
Image image_decode(Alloc *a, IoReader r, Str *name, Error *err);

/* image.DecodeConfig: the same, reading only as far as the size and the color
 * model. */
ImageConfig image_decode_config(Alloc *a, IoReader r, Str *name, Error *err);

/* Gives back an image that image_decode or a codec's decode function made in
 * a: the struct, its pixels, and for an ImagePaletted the palette, which a
 * decoder allocates along with the image. Not for an image you made yourself,
 * whose palette the decoder did not allocate. A nil Image is fine. */
void image_decoded_free(Image m, Alloc *a);

/* Gives back what image_decode_config or a codec's decode_config function
 * made in a, which is the palette when the color model is one, and nothing
 * otherwise. */
void image_config_free(ImageConfig c, Alloc *a);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_IMAGE_H */

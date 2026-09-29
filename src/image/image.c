/* image, from geom.go, image.go, ycbcr.go, names.go and format.go.
 *
 * The pixel accessors index pix the way Go does, through slice_sub3 for a run
 * of bytes and im_byte for one, so a pix too short for its rectangle panics
 * with the message Go's bounds check gives instead of reading past the end.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/image.h"

#include "burrow/bufio.h"
#include "burrow/declare.h"
#include "burrow/mem/heap.h"
#include "burrow/runtime.h"
#include "burrow/sync.h"

#include <stddef.h>
#include <string.h>

/* ----------------------------------------------------------------- helpers */

static const Str im_no_name = {NULL, 0};

#define IM_INT_MAX ((Int)(((uint64_t)1 << (sizeof(Int) * 8 - 1)) - 1))

/* Go's p.Pix[i : i+n : i+n]. */
static inline Byte *im_run(Slice pix, Int i, Int n) {
    return (Byte *)slice_sub3(pix, i, i + n, i + n).p;
}

/* Go's p.Pix[i]. Out of range goes through slice_at for Go's panic. */
static inline Byte *im_byte(Slice pix, Int i) {
    if ((uint64_t)i >= (uint64_t)pix.len)
        return (Byte *)slice_at(pix, i);
    return (Byte *)pix.p + i;
}

static inline bool im_in(Int x, Int y, ImageRectangle r) {
    return r.min.x <= x && x < r.max.x && r.min.y <= y && y < r.max.y;
}

/* mul3NonNeg and add2NonNeg: the product or sum, or -1 when an argument is
 * negative or the result does not fit in an Int. */
static Int im_mul3(Int x, Int y, Int z) {
    if (x < 0 || y < 0 || z < 0)
        return -1;
    if (y != 0 && x > IM_INT_MAX / y)
        return -1;
    Int xy = x * y;
    if (z != 0 && xy > IM_INT_MAX / z)
        return -1;
    return xy * z;
}

static Int im_add2(Int x, Int y) {
    if (x < 0 || y < 0)
        return -1;
    if (x > IM_INT_MAX - y)
        return -1;
    return x + y;
}

static BURROW_NORETURN void im_huge(const char *msg) {
    runtime_panic(str_from_cstr(msg));
}

/* pixelBufferLength. */
static Int im_buffer_length(Int bpp, ImageRectangle r, const char *msg) {
    Int n = im_mul3(bpp, r.max.x - r.min.x, r.max.y - r.min.y);
    if (n < 0)
        im_huge(msg);
    return n;
}

/* A zeroed buffer of n bytes as a []uint8, or false when a says no. Zero bytes
 * is the nil slice, which Go's make would not give, and which nothing can tell
 * apart from an empty one without looking at the pointer. */
static bool im_make(Alloc *a, Int n, Slice *out) {
    if (n == 0) {
        *out = slice_nil(TYPE_BYTE);
        return true;
    }
    Byte *b = (Byte *)mem_alloc(a, (size_t)n, 1);
    if (b == NULL)
        return false;
    *out = slice_from(b, n, n, TYPE_BYTE);
    return true;
}

static void im_unmake(Alloc *a, Slice s) {
    if (s.p != NULL)
        mem_free(a, s.p, (size_t)s.cap, 1);
}

static ColorRGBA64 im_rgba64(ColorRGBAValue v) {
    ColorRGBA64 c = {(uint16_t)v.r, (uint16_t)v.g, (uint16_t)v.b, (uint16_t)v.a};
    return c;
}

/* Int to decimal, into the end of buf, returning where it starts. */
static char *im_itoa(char *end, Int v) {
    uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    char *p = end;
    do {
        *--p = (char)('0' + u % 10);
        u /= 10;
    } while (u != 0);
    if (v < 0)
        *--p = '-';
    return p;
}

/* "(x,y)" at buf + *n. buf has room for two Ints and the punctuation. */
static void im_put_point(char *buf, size_t *n, ImagePoint p) {
    char tmp[24];
    char *end = tmp + sizeof tmp;
    buf[(*n)++] = '(';
    char *s = im_itoa(end, p.x);
    memcpy(buf + *n, s, (size_t)(end - s));
    *n += (size_t)(end - s);
    buf[(*n)++] = ',';
    s = im_itoa(end, p.y);
    memcpy(buf + *n, s, (size_t)(end - s));
    *n += (size_t)(end - s);
    buf[(*n)++] = ')';
}

/* ------------------------------------------------------------------ Point */

const ImagePoint image_zp = {0, 0};
const ImageRectangle image_zr = {{0, 0}, {0, 0}};

Str image_point_string(ImagePoint p, Alloc *a) {
    char buf[64];
    size_t n = 0;
    im_put_point(buf, &n, p);
    Str s = {(const Byte *)buf, (Int)n};
    return str_clone(a, s);
}

ImagePoint image_point_add(ImagePoint p, ImagePoint q) {
    return image_pt(p.x + q.x, p.y + q.y);
}

ImagePoint image_point_sub(ImagePoint p, ImagePoint q) {
    return image_pt(p.x - q.x, p.y - q.y);
}

ImagePoint image_point_mul(ImagePoint p, Int k) {
    return image_pt(p.x * k, p.y * k);
}

ImagePoint image_point_div(ImagePoint p, Int k) {
    if (k == 0)
        runtime_panic(BURROW_S("runtime error: integer divide by zero"));
    /* The one quotient that overflows, which Go defines as wrapping. */
    if (k == -1)
        return image_pt((Int)((uint64_t)0 - (uint64_t)p.x), (Int)((uint64_t)0 - (uint64_t)p.y));
    return image_pt(p.x / k, p.y / k);
}

bool image_point_in(ImagePoint p, ImageRectangle r) {
    return im_in(p.x, p.y, r);
}

ImagePoint image_point_mod(ImagePoint p, ImageRectangle r) {
    Int w = r.max.x - r.min.x, h = r.max.y - r.min.y;
    if (w == 0 || h == 0)
        runtime_panic(BURROW_S("runtime error: integer divide by zero"));
    p = image_point_sub(p, r.min);
    p.x = w == -1 ? 0 : p.x % w;
    if (p.x < 0)
        p.x += w;
    p.y = h == -1 ? 0 : p.y % h;
    if (p.y < 0)
        p.y += h;
    return image_point_add(p, r.min);
}

bool image_point_eq(ImagePoint p, ImagePoint q) {
    return p.x == q.x && p.y == q.y;
}

/* -------------------------------------------------------------- Rectangle */

ImageRectangle image_rect(Int x0, Int y0, Int x1, Int y1) {
    if (x0 > x1) {
        Int t = x0;
        x0 = x1;
        x1 = t;
    }
    if (y0 > y1) {
        Int t = y0;
        y0 = y1;
        y1 = t;
    }
    ImageRectangle r = {{x0, y0}, {x1, y1}};
    return r;
}

Str image_rectangle_string(ImageRectangle r, Alloc *a) {
    char buf[128];
    size_t n = 0;
    im_put_point(buf, &n, r.min);
    buf[n++] = '-';
    im_put_point(buf, &n, r.max);
    Str s = {(const Byte *)buf, (Int)n};
    return str_clone(a, s);
}

Int image_rectangle_dx(ImageRectangle r) {
    return r.max.x - r.min.x;
}

Int image_rectangle_dy(ImageRectangle r) {
    return r.max.y - r.min.y;
}

ImagePoint image_rectangle_size(ImageRectangle r) {
    return image_pt(r.max.x - r.min.x, r.max.y - r.min.y);
}

ImageRectangle image_rectangle_add(ImageRectangle r, ImagePoint p) {
    ImageRectangle s = {{r.min.x + p.x, r.min.y + p.y}, {r.max.x + p.x, r.max.y + p.y}};
    return s;
}

ImageRectangle image_rectangle_sub(ImageRectangle r, ImagePoint p) {
    ImageRectangle s = {{r.min.x - p.x, r.min.y - p.y}, {r.max.x - p.x, r.max.y - p.y}};
    return s;
}

ImageRectangle image_rectangle_inset(ImageRectangle r, Int n) {
    if (image_rectangle_dx(r) < 2 * n) {
        r.min.x = (r.min.x + r.max.x) / 2;
        r.max.x = r.min.x;
    } else {
        r.min.x += n;
        r.max.x -= n;
    }
    if (image_rectangle_dy(r) < 2 * n) {
        r.min.y = (r.min.y + r.max.y) / 2;
        r.max.y = r.min.y;
    } else {
        r.min.y += n;
        r.max.y -= n;
    }
    return r;
}

bool image_rectangle_empty(ImageRectangle r) {
    return r.min.x >= r.max.x || r.min.y >= r.max.y;
}

ImageRectangle image_rectangle_intersect(ImageRectangle r, ImageRectangle s) {
    if (r.min.x < s.min.x)
        r.min.x = s.min.x;
    if (r.min.y < s.min.y)
        r.min.y = s.min.y;
    if (r.max.x > s.max.x)
        r.max.x = s.max.x;
    if (r.max.y > s.max.y)
        r.max.y = s.max.y;
    if (image_rectangle_empty(r))
        return image_zr;
    return r;
}

ImageRectangle image_rectangle_union(ImageRectangle r, ImageRectangle s) {
    if (image_rectangle_empty(r))
        return s;
    if (image_rectangle_empty(s))
        return r;
    if (r.min.x > s.min.x)
        r.min.x = s.min.x;
    if (r.min.y > s.min.y)
        r.min.y = s.min.y;
    if (r.max.x < s.max.x)
        r.max.x = s.max.x;
    if (r.max.y < s.max.y)
        r.max.y = s.max.y;
    return r;
}

bool image_rectangle_eq(ImageRectangle r, ImageRectangle s) {
    return (image_point_eq(r.min, s.min) && image_point_eq(r.max, s.max)) ||
           (image_rectangle_empty(r) && image_rectangle_empty(s));
}

bool image_rectangle_overlaps(ImageRectangle r, ImageRectangle s) {
    return !image_rectangle_empty(r) && !image_rectangle_empty(s) && r.min.x < s.max.x &&
           s.min.x < r.max.x && r.min.y < s.max.y && s.min.y < r.max.y;
}

bool image_rectangle_in(ImageRectangle r, ImageRectangle s) {
    if (image_rectangle_empty(r))
        return true;
    return s.min.x <= r.min.x && r.max.x <= s.max.x && s.min.y <= r.min.y &&
           r.max.y <= s.max.y;
}

ImageRectangle image_rectangle_canon(ImageRectangle r) {
    if (r.max.x < r.min.x) {
        Int t = r.min.x;
        r.min.x = r.max.x;
        r.max.x = t;
    }
    if (r.max.y < r.min.y) {
        Int t = r.min.y;
        r.min.y = r.max.y;
        r.max.y = t;
    }
    return r;
}

Color image_rectangle_at(ImageRectangle r, Int x, Int y) {
    return color_alpha16_as_color(im_in(x, y, r) ? color_opaque : color_transparent);
}

ColorRGBA64 image_rectangle_rgba64_at(ImageRectangle r, Int x, Int y) {
    ColorRGBA64 c = {0, 0, 0, 0};
    if (im_in(x, y, r)) {
        ColorRGBA64 o = {0xffff, 0xffff, 0xffff, 0xffff};
        c = o;
    }
    return c;
}

ImageRectangle image_rectangle_bounds(ImageRectangle r) {
    return r;
}

ColorModel image_rectangle_color_model(ImageRectangle r) {
    (void)r;
    return color_alpha16_model;
}

/* -------------------------------------------------------------- descriptors
 *
 * Go's names, so fmt prints these the way Go does: Point and Rectangle through
 * their String methods, and the images as structs of their fields. */

#define IM_STR(s) {(const Byte *)(s), (Int)sizeof(s) - 1}
#define IM_FIELD(T, go, c, ft)                                                         \
    {IM_STR(go), {NULL, 0}, &(ft), (uint32_t)offsetof(T, c)}
#define IM_COUNT(arr) (uint16_t)(sizeof(arr) / sizeof((arr)[0]))

#define IM_TYPE(T, go, kind, fields, nfields, methods, nmethods, tag)                  \
    const Type burrow_type_##T = {                                                     \
        IM_STR(go),                                                                    \
        IM_STR("image"),                                                               \
        kind,                                                                          \
        (uint32_t)sizeof(T),                                                           \
        (uint16_t)_Alignof(T),                                                         \
        nfields,                                                                       \
        nmethods,                                                                      \
        fields,                                                                        \
        methods,                                                                       \
        NULL,                                                                          \
        NULL,                                                                          \
        0,                                                                             \
        tag,                                                                           \
        NULL,                                                                          \
    }

#define IM_SIG_STRING(IN, OUT) OUT(Str)

static Str im_point_m_string(ImagePoint *self) {
    return image_point_string(*self, error_allocator());
}

static Str im_rectangle_m_string(ImageRectangle *self) {
    return image_rectangle_string(*self, error_allocator());
}

static Str im_ratio_m_string(ImageYCbCrSubsampleRatio *self) {
    return image_y_cb_cr_subsample_ratio_string(*self);
}

#define IM_POINT_METHODS(M, T) M(T, String, im_point_m_string, IM_SIG_STRING)
#define IM_RECTANGLE_METHODS(M, T) M(T, String, im_rectangle_m_string, IM_SIG_STRING)
#define IM_RATIO_METHODS(M, T) M(T, String, im_ratio_m_string, IM_SIG_STRING)

BURROW_METHODS_DEFINE(ImagePoint, IM_POINT_METHODS);
BURROW_METHODS_DEFINE(ImageRectangle, IM_RECTANGLE_METHODS);
BURROW_METHODS_DEFINE(ImageYCbCrSubsampleRatio, IM_RATIO_METHODS);

/* color.Model, which image/color has no need to describe itself either. */
static const Type im_color_model_type = {
    IM_STR("Model"),
    IM_STR("image/color"),
    KIND_INTERFACE,
    (uint32_t)sizeof(ColorModel),
    (uint16_t)_Alignof(ColorModel),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x696d636dU, /* "imcm" */
    NULL,
};

static const Field im_point_fields[] = {
    IM_FIELD(ImagePoint, "X", x, burrow_type_Int),
    IM_FIELD(ImagePoint, "Y", y, burrow_type_Int),
};
static const Field im_rectangle_fields[] = {
    IM_FIELD(ImageRectangle, "Min", min, burrow_type_ImagePoint),
    IM_FIELD(ImageRectangle, "Max", max, burrow_type_ImagePoint),
};
static const Field im_config_fields[] = {
    IM_FIELD(ImageConfig, "ColorModel", color_model, im_color_model_type),
    IM_FIELD(ImageConfig, "Width", width, burrow_type_Int),
    IM_FIELD(ImageConfig, "Height", height, burrow_type_Int),
};

#define IM_PIX_FIELDS(name, T)                                                         \
    static const Field name[] = {                                                      \
        IM_FIELD(T, "Pix", pix, burrow_type_Bytes),                                    \
        IM_FIELD(T, "Stride", stride, burrow_type_Int),                                \
        IM_FIELD(T, "Rect", rect, burrow_type_ImageRectangle),                         \
    }

IM_PIX_FIELDS(im_rgba_fields, ImageRGBA);
IM_PIX_FIELDS(im_rgba64_fields, ImageRGBA64);
IM_PIX_FIELDS(im_nrgba_fields, ImageNRGBA);
IM_PIX_FIELDS(im_nrgba64_fields, ImageNRGBA64);
IM_PIX_FIELDS(im_alpha_fields, ImageAlpha);
IM_PIX_FIELDS(im_alpha16_fields, ImageAlpha16);
IM_PIX_FIELDS(im_gray_fields, ImageGray);
IM_PIX_FIELDS(im_gray16_fields, ImageGray16);
IM_PIX_FIELDS(im_cmyk_fields, ImageCMYK);

/* color.Palette, which image/color has no need to describe itself. */
static const Type im_palette_type = {
    IM_STR("Palette"),
    IM_STR("image/color"),
    KIND_SLICE,
    (uint32_t)sizeof(Slice),
    (uint16_t)_Alignof(Slice),
    0,
    0,
    NULL,
    NULL,
    &burrow_type_Color,
    NULL,
    0,
    0x696d7070U, /* "impp" */
    NULL,
};

static const Field im_paletted_fields[] = {
    IM_FIELD(ImagePaletted, "Pix", pix, burrow_type_Bytes),
    IM_FIELD(ImagePaletted, "Stride", stride, burrow_type_Int),
    IM_FIELD(ImagePaletted, "Rect", rect, burrow_type_ImageRectangle),
    IM_FIELD(ImagePaletted, "Palette", palette, im_palette_type),
};
static const Field im_y_cb_cr_fields[] = {
    IM_FIELD(ImageYCbCr, "Y", y, burrow_type_Bytes),
    IM_FIELD(ImageYCbCr, "Cb", cb, burrow_type_Bytes),
    IM_FIELD(ImageYCbCr, "Cr", cr, burrow_type_Bytes),
    IM_FIELD(ImageYCbCr, "YStride", y_stride, burrow_type_Int),
    IM_FIELD(ImageYCbCr, "CStride", c_stride, burrow_type_Int),
    IM_FIELD(ImageYCbCr, "SubsampleRatio", subsample_ratio,
             burrow_type_ImageYCbCrSubsampleRatio),
    IM_FIELD(ImageYCbCr, "Rect", rect, burrow_type_ImageRectangle),
};
static const Field im_ny_cb_cr_a_fields[] = {
    IM_FIELD(ImageNYCbCrA, "YCbCr", y_cb_cr, burrow_type_ImageYCbCr),
    IM_FIELD(ImageNYCbCrA, "A", a, burrow_type_Bytes),
    IM_FIELD(ImageNYCbCrA, "AStride", a_stride, burrow_type_Int),
};
static const Field im_uniform_fields[] = {
    IM_FIELD(ImageUniform, "C", c, burrow_type_Color),
};

IM_TYPE(ImagePoint, "Point", KIND_STRUCT, im_point_fields, IM_COUNT(im_point_fields),
        burrow__methods_ImagePoint, IM_COUNT(burrow__methods_ImagePoint), 0x696d3031U);
IM_TYPE(ImageRectangle, "Rectangle", KIND_STRUCT, im_rectangle_fields,
        IM_COUNT(im_rectangle_fields), burrow__methods_ImageRectangle,
        IM_COUNT(burrow__methods_ImageRectangle), 0x696d3032U);
IM_TYPE(ImageYCbCrSubsampleRatio, "YCbCrSubsampleRatio", KIND_INT, NULL, 0,
        burrow__methods_ImageYCbCrSubsampleRatio,
        IM_COUNT(burrow__methods_ImageYCbCrSubsampleRatio), 0x696d3033U);
IM_TYPE(ImageConfig, "Config", KIND_STRUCT, im_config_fields, IM_COUNT(im_config_fields),
        NULL, 0, 0x696d3034U);
IM_TYPE(ImageRGBA, "RGBA", KIND_STRUCT, im_rgba_fields, IM_COUNT(im_rgba_fields), NULL, 0,
        0x696d3035U);
IM_TYPE(ImageRGBA64, "RGBA64", KIND_STRUCT, im_rgba64_fields, IM_COUNT(im_rgba64_fields),
        NULL, 0, 0x696d3036U);
IM_TYPE(ImageNRGBA, "NRGBA", KIND_STRUCT, im_nrgba_fields, IM_COUNT(im_nrgba_fields), NULL,
        0, 0x696d3037U);
IM_TYPE(ImageNRGBA64, "NRGBA64", KIND_STRUCT, im_nrgba64_fields,
        IM_COUNT(im_nrgba64_fields), NULL, 0, 0x696d3038U);
IM_TYPE(ImageAlpha, "Alpha", KIND_STRUCT, im_alpha_fields, IM_COUNT(im_alpha_fields), NULL,
        0, 0x696d3039U);
IM_TYPE(ImageAlpha16, "Alpha16", KIND_STRUCT, im_alpha16_fields,
        IM_COUNT(im_alpha16_fields), NULL, 0, 0x696d3130U);
IM_TYPE(ImageGray, "Gray", KIND_STRUCT, im_gray_fields, IM_COUNT(im_gray_fields), NULL, 0,
        0x696d3131U);
IM_TYPE(ImageGray16, "Gray16", KIND_STRUCT, im_gray16_fields, IM_COUNT(im_gray16_fields),
        NULL, 0, 0x696d3132U);
IM_TYPE(ImageCMYK, "CMYK", KIND_STRUCT, im_cmyk_fields, IM_COUNT(im_cmyk_fields), NULL, 0,
        0x696d3133U);
IM_TYPE(ImagePaletted, "Paletted", KIND_STRUCT, im_paletted_fields,
        IM_COUNT(im_paletted_fields), NULL, 0, 0x696d3134U);
IM_TYPE(ImageYCbCr, "YCbCr", KIND_STRUCT, im_y_cb_cr_fields, IM_COUNT(im_y_cb_cr_fields),
        NULL, 0, 0x696d3135U);
IM_TYPE(ImageNYCbCrA, "NYCbCrA", KIND_STRUCT, im_ny_cb_cr_a_fields,
        IM_COUNT(im_ny_cb_cr_a_fields), NULL, 0, 0x696d3136U);
IM_TYPE(ImageUniform, "Uniform", KIND_STRUCT, im_uniform_fields,
        IM_COUNT(im_uniform_fields), NULL, 0, 0x696d3137U);
IM_TYPE(Image, "Image", KIND_INTERFACE, NULL, 0, NULL, 0, 0x696d3138U);

/* --------------------------------------------------------- the image types
 *
 * What every one of the Pix, Stride, Rect types has in common, written once:
 * the constructor and free, Bounds, PixOffset, SubImage, and the vtable. Each
 * type then has its own reading and writing, which is where they differ. */

#define IM_COMMON(T, pre, bpp, goname, model)                                          \
    T *image_new_##pre(Alloc *a, ImageRectangle r) {                                   \
        Int n = im_buffer_length(bpp, r,                                               \
                                 "image: New" goname                                   \
                                 " Rectangle has huge or negative dimensions");        \
        T *p = (T *)mem_alloc(a, sizeof(T), _Alignof(T));                              \
        if (p == NULL)                                                                 \
            return NULL;                                                               \
        if (!im_make(a, n, &p->pix)) {                                                 \
            mem_free(a, p, sizeof(T), _Alignof(T));                                    \
            return NULL;                                                               \
        }                                                                              \
        p->stride = (bpp) * (r.max.x - r.min.x);                                       \
        p->rect = r;                                                                   \
        return p;                                                                      \
    }                                                                                  \
    void image_##pre##_free(T *p, Alloc *a) {                                          \
        if (p == NULL)                                                                 \
            return;                                                                    \
        im_unmake(a, p->pix);                                                          \
        mem_free(a, p, sizeof(T), _Alignof(T));                                        \
    }                                                                                  \
    ColorModel image_##pre##_color_model(const T *p) {                                 \
        (void)p;                                                                       \
        return model;                                                                  \
    }                                                                                  \
    ImageRectangle image_##pre##_bounds(const T *p) {                                  \
        return p->rect;                                                                \
    }                                                                                  \
    Int image_##pre##_pix_offset(const T *p, Int x, Int y) {                           \
        return (y - p->rect.min.y) * p->stride + (x - p->rect.min.x) * (bpp);          \
    }                                                                                  \
    T image_##pre##_sub_image(const T *p, ImageRectangle r) {                          \
        T s;                                                                           \
        memset(&s, 0, sizeof s);                                                       \
        r = image_rectangle_intersect(r, p->rect);                                     \
        if (image_rectangle_empty(r))                                                  \
            return s;                                                                  \
        Int i = image_##pre##_pix_offset(p, r.min.x, r.min.y);                         \
        s.pix = slice_sub(p->pix, i, p->pix.len);                                      \
        s.stride = p->stride;                                                          \
        s.rect = r;                                                                    \
        return s;                                                                      \
    }                                                                                  \
    IM_VTABLE(T, pre, NULL)

/* The vtable, through thunks that take void *. cidx is the ColorIndexAt slot,
 * which only Paletted fills in. */
#define IM_VTABLE(T, pre, cidx)                                                        \
    static ColorModel im_##pre##_vt_color_model(void *self) {                          \
        return image_##pre##_color_model((const T *)self);                             \
    }                                                                                  \
    static ImageRectangle im_##pre##_vt_bounds(void *self) {                           \
        return image_##pre##_bounds((const T *)self);                                  \
    }                                                                                  \
    static Color im_##pre##_vt_at(void *self, Int x, Int y) {                          \
        return image_##pre##_at((const T *)self, x, y);                                \
    }                                                                                  \
    static ColorRGBA64 im_##pre##_vt_rgba64_at(void *self, Int x, Int y) {             \
        return image_##pre##_rgba64_at((const T *)self, x, y);                         \
    }                                                                                  \
    static void im_##pre##_vt_set(void *self, Int x, Int y, Color c) {                 \
        image_##pre##_set((T *)self, x, y, c);                                         \
    }                                                                                  \
    static void im_##pre##_vt_set_rgba64(void *self, Int x, Int y, ColorRGBA64 c) {    \
        image_##pre##_set_rgba64((T *)self, x, y, c);                                  \
    }                                                                                  \
    static bool im_##pre##_vt_opaque(void *self) {                                     \
        return image_##pre##_opaque((const T *)self);                                  \
    }                                                                                  \
    static const ImageVT im_##pre##_vt = {                                             \
        &burrow_type_##T,         im_##pre##_vt_color_model,                           \
        im_##pre##_vt_bounds,     im_##pre##_vt_at,                                    \
        im_##pre##_vt_rgba64_at,  cidx,                                                \
        im_##pre##_vt_set,        im_##pre##_vt_set_rgba64,                            \
        im_##pre##_vt_opaque,                                                          \
    };                                                                                 \
    Image image_##pre##_as_image(T *p) {                                               \
        Image m = {&im_##pre##_vt, p};                                                 \
        return m;                                                                      \
    }

/* Opaque for the types that have alpha: every alpha sample, of n bytes at
 * offset off in each pixel of bpp bytes, is all ones. */
#define IM_OPAQUE_SCAN(p, bpp, off, n)                                                 \
    do {                                                                               \
        if (image_rectangle_empty((p)->rect))                                          \
            return true;                                                               \
        Int i0 = (off), i1 = image_rectangle_dx((p)->rect) * (bpp);                    \
        for (Int y = (p)->rect.min.y; y < (p)->rect.max.y; y++) {                      \
            for (Int i = i0; i < i1; i += (bpp)) {                                     \
                for (Int k = 0; k < (n); k++)                                          \
                    if (*im_byte((p)->pix, i + k) != 0xff)                             \
                        return false;                                                  \
            }                                                                          \
            i0 += (p)->stride;                                                         \
            i1 += (p)->stride;                                                         \
        }                                                                              \
        return true;                                                                   \
    } while (0)

static inline uint16_t im_be16(const Byte *s) {
    return (uint16_t)((uint16_t)s[0] << 8 | s[1]);
}

static inline void im_put_be16(Byte *s, uint32_t v) {
    s[0] = (Byte)(v >> 8);
    s[1] = (Byte)v;
}

/* ------------------------------------------------------------------- RGBA */

ColorRGBA image_rgba_rgba_at(const ImageRGBA *p, Int x, Int y) {
    ColorRGBA c = {0, 0, 0, 0};
    if (!im_in(x, y, p->rect))
        return c;
    const Byte *s = im_run(p->pix, image_rgba_pix_offset(p, x, y), 4);
    c.r = s[0];
    c.g = s[1];
    c.b = s[2];
    c.a = s[3];
    return c;
}

Color image_rgba_at(const ImageRGBA *p, Int x, Int y) {
    return color_rgba_as_color(image_rgba_rgba_at(p, x, y));
}

ColorRGBA64 image_rgba_rgba64_at(const ImageRGBA *p, Int x, Int y) {
    ColorRGBA64 c = {0, 0, 0, 0};
    if (!im_in(x, y, p->rect))
        return c;
    const Byte *s = im_run(p->pix, image_rgba_pix_offset(p, x, y), 4);
    c.r = (uint16_t)(s[0] << 8 | s[0]);
    c.g = (uint16_t)(s[1] << 8 | s[1]);
    c.b = (uint16_t)(s[2] << 8 | s[2]);
    c.a = (uint16_t)(s[3] << 8 | s[3]);
    return c;
}

void image_rgba_set_rgba(ImageRGBA *p, Int x, Int y, ColorRGBA c) {
    if (!im_in(x, y, p->rect))
        return;
    Byte *s = im_run(p->pix, image_rgba_pix_offset(p, x, y), 4);
    s[0] = c.r;
    s[1] = c.g;
    s[2] = c.b;
    s[3] = c.a;
}

void image_rgba_set(ImageRGBA *p, Int x, Int y, Color c) {
    if (!im_in(x, y, p->rect))
        return;
    Int i = image_rgba_pix_offset(p, x, y);
    ColorRGBA c1 = color_model_convert(color_rgba_model, c).data.rgba;
    Byte *s = im_run(p->pix, i, 4);
    s[0] = c1.r;
    s[1] = c1.g;
    s[2] = c1.b;
    s[3] = c1.a;
}

void image_rgba_set_rgba64(ImageRGBA *p, Int x, Int y, ColorRGBA64 c) {
    if (!im_in(x, y, p->rect))
        return;
    Byte *s = im_run(p->pix, image_rgba_pix_offset(p, x, y), 4);
    s[0] = (Byte)(c.r >> 8);
    s[1] = (Byte)(c.g >> 8);
    s[2] = (Byte)(c.b >> 8);
    s[3] = (Byte)(c.a >> 8);
}

bool image_rgba_opaque(const ImageRGBA *p) {
    IM_OPAQUE_SCAN(p, 4, 3, 1);
}

IM_COMMON(ImageRGBA, rgba, 4, "RGBA", color_rgba_model)

/* ----------------------------------------------------------------- RGBA64 */

ColorRGBA64 image_rgba64_rgba64_at(const ImageRGBA64 *p, Int x, Int y) {
    ColorRGBA64 c = {0, 0, 0, 0};
    if (!im_in(x, y, p->rect))
        return c;
    const Byte *s = im_run(p->pix, image_rgba64_pix_offset(p, x, y), 8);
    c.r = im_be16(s);
    c.g = im_be16(s + 2);
    c.b = im_be16(s + 4);
    c.a = im_be16(s + 6);
    return c;
}

Color image_rgba64_at(const ImageRGBA64 *p, Int x, Int y) {
    return color_rgba64_as_color(image_rgba64_rgba64_at(p, x, y));
}

void image_rgba64_set_rgba64(ImageRGBA64 *p, Int x, Int y, ColorRGBA64 c) {
    if (!im_in(x, y, p->rect))
        return;
    Byte *s = im_run(p->pix, image_rgba64_pix_offset(p, x, y), 8);
    im_put_be16(s, c.r);
    im_put_be16(s + 2, c.g);
    im_put_be16(s + 4, c.b);
    im_put_be16(s + 6, c.a);
}

void image_rgba64_set(ImageRGBA64 *p, Int x, Int y, Color c) {
    if (!im_in(x, y, p->rect))
        return;
    Int i = image_rgba64_pix_offset(p, x, y);
    ColorRGBA64 c1 = color_model_convert(color_rgba64_model, c).data.rgba64;
    Byte *s = im_run(p->pix, i, 8);
    im_put_be16(s, c1.r);
    im_put_be16(s + 2, c1.g);
    im_put_be16(s + 4, c1.b);
    im_put_be16(s + 6, c1.a);
}

bool image_rgba64_opaque(const ImageRGBA64 *p) {
    IM_OPAQUE_SCAN(p, 8, 6, 2);
}

IM_COMMON(ImageRGBA64, rgba64, 8, "RGBA64", color_rgba64_model)

/* ------------------------------------------------------------------ NRGBA */

ColorNRGBA image_nrgba_nrgba_at(const ImageNRGBA *p, Int x, Int y) {
    ColorNRGBA c = {0, 0, 0, 0};
    if (!im_in(x, y, p->rect))
        return c;
    const Byte *s = im_run(p->pix, image_nrgba_pix_offset(p, x, y), 4);
    c.r = s[0];
    c.g = s[1];
    c.b = s[2];
    c.a = s[3];
    return c;
}

Color image_nrgba_at(const ImageNRGBA *p, Int x, Int y) {
    return color_nrgba_as_color(image_nrgba_nrgba_at(p, x, y));
}

ColorRGBA64 image_nrgba_rgba64_at(const ImageNRGBA *p, Int x, Int y) {
    return im_rgba64(color_nrgba_rgba(image_nrgba_nrgba_at(p, x, y)));
}

void image_nrgba_set_nrgba(ImageNRGBA *p, Int x, Int y, ColorNRGBA c) {
    if (!im_in(x, y, p->rect))
        return;
    Byte *s = im_run(p->pix, image_nrgba_pix_offset(p, x, y), 4);
    s[0] = c.r;
    s[1] = c.g;
    s[2] = c.b;
    s[3] = c.a;
}

void image_nrgba_set(ImageNRGBA *p, Int x, Int y, Color c) {
    if (!im_in(x, y, p->rect))
        return;
    Int i = image_nrgba_pix_offset(p, x, y);
    ColorNRGBA c1 = color_model_convert(color_nrgba_model, c).data.nrgba;
    Byte *s = im_run(p->pix, i, 4);
    s[0] = c1.r;
    s[1] = c1.g;
    s[2] = c1.b;
    s[3] = c1.a;
}

/* Un-premultiply, as SetRGBA64 on the two NRGBA types does. */
static void im_unpremul(ColorRGBA64 c, uint32_t *r, uint32_t *g, uint32_t *b, uint32_t *a) {
    *r = c.r;
    *g = c.g;
    *b = c.b;
    *a = c.a;
    if (*a != 0 && *a != 0xffff) {
        *r = (*r * 0xffff) / *a;
        *g = (*g * 0xffff) / *a;
        *b = (*b * 0xffff) / *a;
    }
}

void image_nrgba_set_rgba64(ImageNRGBA *p, Int x, Int y, ColorRGBA64 c) {
    if (!im_in(x, y, p->rect))
        return;
    uint32_t r, g, b, a;
    im_unpremul(c, &r, &g, &b, &a);
    Byte *s = im_run(p->pix, image_nrgba_pix_offset(p, x, y), 4);
    s[0] = (Byte)(r >> 8);
    s[1] = (Byte)(g >> 8);
    s[2] = (Byte)(b >> 8);
    s[3] = (Byte)(a >> 8);
}

bool image_nrgba_opaque(const ImageNRGBA *p) {
    IM_OPAQUE_SCAN(p, 4, 3, 1);
}

IM_COMMON(ImageNRGBA, nrgba, 4, "NRGBA", color_nrgba_model)

/* ---------------------------------------------------------------- NRGBA64 */

ColorNRGBA64 image_nrgba64_nrgba64_at(const ImageNRGBA64 *p, Int x, Int y) {
    ColorNRGBA64 c = {0, 0, 0, 0};
    if (!im_in(x, y, p->rect))
        return c;
    const Byte *s = im_run(p->pix, image_nrgba64_pix_offset(p, x, y), 8);
    c.r = im_be16(s);
    c.g = im_be16(s + 2);
    c.b = im_be16(s + 4);
    c.a = im_be16(s + 6);
    return c;
}

Color image_nrgba64_at(const ImageNRGBA64 *p, Int x, Int y) {
    return color_nrgba64_as_color(image_nrgba64_nrgba64_at(p, x, y));
}

ColorRGBA64 image_nrgba64_rgba64_at(const ImageNRGBA64 *p, Int x, Int y) {
    return im_rgba64(color_nrgba64_rgba(image_nrgba64_nrgba64_at(p, x, y)));
}

void image_nrgba64_set_nrgba64(ImageNRGBA64 *p, Int x, Int y, ColorNRGBA64 c) {
    if (!im_in(x, y, p->rect))
        return;
    Byte *s = im_run(p->pix, image_nrgba64_pix_offset(p, x, y), 8);
    im_put_be16(s, c.r);
    im_put_be16(s + 2, c.g);
    im_put_be16(s + 4, c.b);
    im_put_be16(s + 6, c.a);
}

void image_nrgba64_set(ImageNRGBA64 *p, Int x, Int y, Color c) {
    if (!im_in(x, y, p->rect))
        return;
    Int i = image_nrgba64_pix_offset(p, x, y);
    ColorNRGBA64 c1 = color_model_convert(color_nrgba64_model, c).data.nrgba64;
    Byte *s = im_run(p->pix, i, 8);
    im_put_be16(s, c1.r);
    im_put_be16(s + 2, c1.g);
    im_put_be16(s + 4, c1.b);
    im_put_be16(s + 6, c1.a);
}

void image_nrgba64_set_rgba64(ImageNRGBA64 *p, Int x, Int y, ColorRGBA64 c) {
    if (!im_in(x, y, p->rect))
        return;
    uint32_t r, g, b, a;
    im_unpremul(c, &r, &g, &b, &a);
    Byte *s = im_run(p->pix, image_nrgba64_pix_offset(p, x, y), 8);
    im_put_be16(s, r);
    im_put_be16(s + 2, g);
    im_put_be16(s + 4, b);
    im_put_be16(s + 6, a);
}

bool image_nrgba64_opaque(const ImageNRGBA64 *p) {
    IM_OPAQUE_SCAN(p, 8, 6, 2);
}

IM_COMMON(ImageNRGBA64, nrgba64, 8, "NRGBA64", color_nrgba64_model)

/* ------------------------------------------------------------------ Alpha */

ColorAlpha image_alpha_alpha_at(const ImageAlpha *p, Int x, Int y) {
    ColorAlpha c = {0};
    if (!im_in(x, y, p->rect))
        return c;
    c.a = *im_byte(p->pix, image_alpha_pix_offset(p, x, y));
    return c;
}

Color image_alpha_at(const ImageAlpha *p, Int x, Int y) {
    return color_alpha_as_color(image_alpha_alpha_at(p, x, y));
}

ColorRGBA64 image_alpha_rgba64_at(const ImageAlpha *p, Int x, Int y) {
    uint16_t a = image_alpha_alpha_at(p, x, y).a;
    a = (uint16_t)(a | a << 8);
    ColorRGBA64 c = {a, a, a, a};
    return c;
}

void image_alpha_set_alpha(ImageAlpha *p, Int x, Int y, ColorAlpha c) {
    if (!im_in(x, y, p->rect))
        return;
    *im_byte(p->pix, image_alpha_pix_offset(p, x, y)) = c.a;
}

void image_alpha_set(ImageAlpha *p, Int x, Int y, Color c) {
    if (!im_in(x, y, p->rect))
        return;
    Int i = image_alpha_pix_offset(p, x, y);
    ColorAlpha c1 = color_model_convert(color_alpha_model, c).data.alpha;
    *im_byte(p->pix, i) = c1.a;
}

void image_alpha_set_rgba64(ImageAlpha *p, Int x, Int y, ColorRGBA64 c) {
    if (!im_in(x, y, p->rect))
        return;
    *im_byte(p->pix, image_alpha_pix_offset(p, x, y)) = (Byte)(c.a >> 8);
}

bool image_alpha_opaque(const ImageAlpha *p) {
    IM_OPAQUE_SCAN(p, 1, 0, 1);
}

IM_COMMON(ImageAlpha, alpha, 1, "Alpha", color_alpha_model)

/* ---------------------------------------------------------------- Alpha16 */

/* Go reads and writes the two bytes of a 16 bit sample one index at a time,
 * so each is checked on its own. */
static inline uint16_t im_get16(Slice pix, Int i) {
    uint16_t hi = *im_byte(pix, i);
    return (uint16_t)(hi << 8 | *im_byte(pix, i + 1));
}

static inline void im_set16(Slice pix, Int i, uint32_t v) {
    *im_byte(pix, i) = (Byte)(v >> 8);
    *im_byte(pix, i + 1) = (Byte)v;
}

ColorAlpha16 image_alpha16_alpha16_at(const ImageAlpha16 *p, Int x, Int y) {
    ColorAlpha16 c = {0};
    if (!im_in(x, y, p->rect))
        return c;
    c.a = im_get16(p->pix, image_alpha16_pix_offset(p, x, y));
    return c;
}

Color image_alpha16_at(const ImageAlpha16 *p, Int x, Int y) {
    return color_alpha16_as_color(image_alpha16_alpha16_at(p, x, y));
}

ColorRGBA64 image_alpha16_rgba64_at(const ImageAlpha16 *p, Int x, Int y) {
    uint16_t a = image_alpha16_alpha16_at(p, x, y).a;
    ColorRGBA64 c = {a, a, a, a};
    return c;
}

void image_alpha16_set_alpha16(ImageAlpha16 *p, Int x, Int y, ColorAlpha16 c) {
    if (!im_in(x, y, p->rect))
        return;
    im_set16(p->pix, image_alpha16_pix_offset(p, x, y), c.a);
}

void image_alpha16_set(ImageAlpha16 *p, Int x, Int y, Color c) {
    if (!im_in(x, y, p->rect))
        return;
    Int i = image_alpha16_pix_offset(p, x, y);
    ColorAlpha16 c1 = color_model_convert(color_alpha16_model, c).data.alpha16;
    im_set16(p->pix, i, c1.a);
}

void image_alpha16_set_rgba64(ImageAlpha16 *p, Int x, Int y, ColorRGBA64 c) {
    if (!im_in(x, y, p->rect))
        return;
    im_set16(p->pix, image_alpha16_pix_offset(p, x, y), c.a);
}

bool image_alpha16_opaque(const ImageAlpha16 *p) {
    IM_OPAQUE_SCAN(p, 2, 0, 2);
}

IM_COMMON(ImageAlpha16, alpha16, 2, "Alpha16", color_alpha16_model)

/* ------------------------------------------------------------------- Gray */

ColorGray image_gray_gray_at(const ImageGray *p, Int x, Int y) {
    ColorGray c = {0};
    if (!im_in(x, y, p->rect))
        return c;
    c.y = *im_byte(p->pix, image_gray_pix_offset(p, x, y));
    return c;
}

Color image_gray_at(const ImageGray *p, Int x, Int y) {
    return color_gray_as_color(image_gray_gray_at(p, x, y));
}

ColorRGBA64 image_gray_rgba64_at(const ImageGray *p, Int x, Int y) {
    uint16_t g = image_gray_gray_at(p, x, y).y;
    g = (uint16_t)(g | g << 8);
    ColorRGBA64 c = {g, g, g, 0xffff};
    return c;
}

void image_gray_set_gray(ImageGray *p, Int x, Int y, ColorGray c) {
    if (!im_in(x, y, p->rect))
        return;
    *im_byte(p->pix, image_gray_pix_offset(p, x, y)) = c.y;
}

void image_gray_set(ImageGray *p, Int x, Int y, Color c) {
    if (!im_in(x, y, p->rect))
        return;
    Int i = image_gray_pix_offset(p, x, y);
    ColorGray c1 = color_model_convert(color_gray_model, c).data.gray;
    *im_byte(p->pix, i) = c1.y;
}

/* The luma weights color.grayModel uses, before the final shift. */
static inline uint32_t im_luma(ColorRGBA64 c) {
    return 19595u * c.r + 38470u * c.g + 7471u * c.b + (1u << 15);
}

void image_gray_set_rgba64(ImageGray *p, Int x, Int y, ColorRGBA64 c) {
    if (!im_in(x, y, p->rect))
        return;
    uint32_t gray = im_luma(c) >> 24;
    *im_byte(p->pix, image_gray_pix_offset(p, x, y)) = (Byte)gray;
}

bool image_gray_opaque(const ImageGray *p) {
    (void)p;
    return true;
}

IM_COMMON(ImageGray, gray, 1, "Gray", color_gray_model)

/* ----------------------------------------------------------------- Gray16 */

ColorGray16 image_gray16_gray16_at(const ImageGray16 *p, Int x, Int y) {
    ColorGray16 c = {0};
    if (!im_in(x, y, p->rect))
        return c;
    c.y = im_get16(p->pix, image_gray16_pix_offset(p, x, y));
    return c;
}

Color image_gray16_at(const ImageGray16 *p, Int x, Int y) {
    return color_gray16_as_color(image_gray16_gray16_at(p, x, y));
}

ColorRGBA64 image_gray16_rgba64_at(const ImageGray16 *p, Int x, Int y) {
    uint16_t g = image_gray16_gray16_at(p, x, y).y;
    ColorRGBA64 c = {g, g, g, 0xffff};
    return c;
}

void image_gray16_set_gray16(ImageGray16 *p, Int x, Int y, ColorGray16 c) {
    if (!im_in(x, y, p->rect))
        return;
    im_set16(p->pix, image_gray16_pix_offset(p, x, y), c.y);
}

void image_gray16_set(ImageGray16 *p, Int x, Int y, Color c) {
    if (!im_in(x, y, p->rect))
        return;
    Int i = image_gray16_pix_offset(p, x, y);
    ColorGray16 c1 = color_model_convert(color_gray16_model, c).data.gray16;
    im_set16(p->pix, i, c1.y);
}

void image_gray16_set_rgba64(ImageGray16 *p, Int x, Int y, ColorRGBA64 c) {
    if (!im_in(x, y, p->rect))
        return;
    uint32_t gray = im_luma(c) >> 16;
    im_set16(p->pix, image_gray16_pix_offset(p, x, y), gray);
}

bool image_gray16_opaque(const ImageGray16 *p) {
    (void)p;
    return true;
}

IM_COMMON(ImageGray16, gray16, 2, "Gray16", color_gray16_model)

/* ------------------------------------------------------------------- CMYK */

ColorCMYK image_cmyk_cmyk_at(const ImageCMYK *p, Int x, Int y) {
    ColorCMYK c = {0, 0, 0, 0};
    if (!im_in(x, y, p->rect))
        return c;
    const Byte *s = im_run(p->pix, image_cmyk_pix_offset(p, x, y), 4);
    c.c = s[0];
    c.m = s[1];
    c.y = s[2];
    c.k = s[3];
    return c;
}

Color image_cmyk_at(const ImageCMYK *p, Int x, Int y) {
    return color_cmyk_as_color(image_cmyk_cmyk_at(p, x, y));
}

ColorRGBA64 image_cmyk_rgba64_at(const ImageCMYK *p, Int x, Int y) {
    return im_rgba64(color_cmyk_rgba(image_cmyk_cmyk_at(p, x, y)));
}

void image_cmyk_set_cmyk(ImageCMYK *p, Int x, Int y, ColorCMYK c) {
    if (!im_in(x, y, p->rect))
        return;
    Byte *s = im_run(p->pix, image_cmyk_pix_offset(p, x, y), 4);
    s[0] = c.c;
    s[1] = c.m;
    s[2] = c.y;
    s[3] = c.k;
}

void image_cmyk_set(ImageCMYK *p, Int x, Int y, Color c) {
    if (!im_in(x, y, p->rect))
        return;
    Int i = image_cmyk_pix_offset(p, x, y);
    ColorCMYK c1 = color_model_convert(color_cmyk_model, c).data.cmyk;
    Byte *s = im_run(p->pix, i, 4);
    s[0] = c1.c;
    s[1] = c1.m;
    s[2] = c1.y;
    s[3] = c1.k;
}

void image_cmyk_set_rgba64(ImageCMYK *p, Int x, Int y, ColorRGBA64 c) {
    if (!im_in(x, y, p->rect))
        return;
    ColorCMYK k = color_rgb_to_cmyk((uint8_t)(c.r >> 8), (uint8_t)(c.g >> 8),
                                    (uint8_t)(c.b >> 8));
    Byte *s = im_run(p->pix, image_cmyk_pix_offset(p, x, y), 4);
    s[0] = k.c;
    s[1] = k.m;
    s[2] = k.y;
    s[3] = k.k;
}

bool image_cmyk_opaque(const ImageCMYK *p) {
    (void)p;
    return true;
}

IM_COMMON(ImageCMYK, cmyk, 4, "CMYK", color_cmyk_model)

/* --------------------------------------------------------------- Paletted */

ImagePaletted *image_new_paletted(Alloc *a, ImageRectangle r, ColorPalette pal) {
    Int n = im_buffer_length(1, r, "image: NewPaletted Rectangle has huge or negative dimensions");
    ImagePaletted *p = (ImagePaletted *)mem_alloc(a, sizeof *p, _Alignof(ImagePaletted));
    if (p == NULL)
        return NULL;
    if (!im_make(a, n, &p->pix)) {
        mem_free(a, p, sizeof *p, _Alignof(ImagePaletted));
        return NULL;
    }
    p->stride = r.max.x - r.min.x;
    p->rect = r;
    p->palette = pal;
    return p;
}

void image_paletted_free(ImagePaletted *p, Alloc *a) {
    if (p == NULL)
        return;
    im_unmake(a, p->pix);
    mem_free(a, p, sizeof *p, _Alignof(ImagePaletted));
}

ColorModel image_paletted_color_model(const ImagePaletted *p) {
    return color_palette_as_model(&p->palette);
}

ImageRectangle image_paletted_bounds(const ImagePaletted *p) {
    return p->rect;
}

Int image_paletted_pix_offset(const ImagePaletted *p, Int x, Int y) {
    return (y - p->rect.min.y) * p->stride + (x - p->rect.min.x);
}

static Color im_palette_entry(ColorPalette pal, Int i) {
    return *(const Color *)slice_at(pal, i);
}

Color image_paletted_at(const ImagePaletted *p, Int x, Int y) {
    if (p->palette.len == 0) {
        Color nil;
        memset(&nil, 0, sizeof nil);
        return nil;
    }
    if (!im_in(x, y, p->rect))
        return im_palette_entry(p->palette, 0);
    return im_palette_entry(p->palette, *im_byte(p->pix, image_paletted_pix_offset(p, x, y)));
}

ColorRGBA64 image_paletted_rgba64_at(const ImagePaletted *p, Int x, Int y) {
    if (p->palette.len == 0) {
        ColorRGBA64 z = {0, 0, 0, 0};
        return z;
    }
    return im_rgba64(color_rgba(image_paletted_at(p, x, y)));
}

void image_paletted_set(ImagePaletted *p, Int x, Int y, Color c) {
    if (!im_in(x, y, p->rect))
        return;
    Int i = image_paletted_pix_offset(p, x, y);
    *im_byte(p->pix, i) = (Byte)color_palette_index(p->palette, c);
}

void image_paletted_set_rgba64(ImagePaletted *p, Int x, Int y, ColorRGBA64 c) {
    if (!im_in(x, y, p->rect))
        return;
    Int i = image_paletted_pix_offset(p, x, y);
    *im_byte(p->pix, i) = (Byte)color_palette_index(p->palette, color_rgba64_as_color(c));
}

uint8_t image_paletted_color_index_at(const ImagePaletted *p, Int x, Int y) {
    if (!im_in(x, y, p->rect))
        return 0;
    return *im_byte(p->pix, image_paletted_pix_offset(p, x, y));
}

void image_paletted_set_color_index(ImagePaletted *p, Int x, Int y, uint8_t index) {
    if (!im_in(x, y, p->rect))
        return;
    *im_byte(p->pix, image_paletted_pix_offset(p, x, y)) = index;
}

ImagePaletted image_paletted_sub_image(const ImagePaletted *p, ImageRectangle r) {
    ImagePaletted s;
    memset(&s, 0, sizeof s);
    s.palette = p->palette;
    r = image_rectangle_intersect(r, p->rect);
    if (image_rectangle_empty(r))
        return s;
    Int i = image_paletted_pix_offset(p, r.min.x, r.min.y);
    s.pix = slice_sub(p->pix, i, p->pix.len);
    s.stride = p->stride;
    s.rect = image_rectangle_intersect(p->rect, r);
    return s;
}

bool image_paletted_opaque(const ImagePaletted *p) {
    bool present[256] = {false};
    Int i0 = 0, i1 = image_rectangle_dx(p->rect);
    for (Int y = p->rect.min.y; y < p->rect.max.y; y++) {
        Slice row = slice_sub(p->pix, i0, i1);
        const Byte *b = (const Byte *)row.p;
        for (Int i = 0; i < row.len; i++)
            present[b[i]] = true;
        i0 += p->stride;
        i1 += p->stride;
    }
    /* Go indexes present with every palette index, so a palette of more than
     * 256 colours panics here the way it does there. */
    Slice seen = slice_from(present, 256, 256, TYPE_BYTE);
    const Color *pal = (const Color *)p->palette.p;
    for (Int i = 0; i < p->palette.len; i++) {
        if (!*(const bool *)slice_at(seen, i))
            continue;
        if (color_rgba(pal[i]).a != 0xffff)
            return false;
    }
    return true;
}

static uint8_t im_paletted_vt_color_index_at(void *self, Int x, Int y) {
    return image_paletted_color_index_at((const ImagePaletted *)self, x, y);
}

IM_VTABLE(ImagePaletted, paletted, im_paletted_vt_color_index_at)

/* ------------------------------------------------------------------ YCbCr */

Str image_y_cb_cr_subsample_ratio_string(ImageYCbCrSubsampleRatio s) {
    switch (s) {
    case IMAGE_Y_CB_CR_SUBSAMPLE_RATIO444:
        return BURROW_S("YCbCrSubsampleRatio444");
    case IMAGE_Y_CB_CR_SUBSAMPLE_RATIO422:
        return BURROW_S("YCbCrSubsampleRatio422");
    case IMAGE_Y_CB_CR_SUBSAMPLE_RATIO420:
        return BURROW_S("YCbCrSubsampleRatio420");
    case IMAGE_Y_CB_CR_SUBSAMPLE_RATIO440:
        return BURROW_S("YCbCrSubsampleRatio440");
    case IMAGE_Y_CB_CR_SUBSAMPLE_RATIO411:
        return BURROW_S("YCbCrSubsampleRatio411");
    case IMAGE_Y_CB_CR_SUBSAMPLE_RATIO410:
        return BURROW_S("YCbCrSubsampleRatio410");
    default:
        return BURROW_S("YCbCrSubsampleRatioUnknown");
    }
}

Int image_y_cb_cr_y_offset(const ImageYCbCr *p, Int x, Int y) {
    return (y - p->rect.min.y) * p->y_stride + (x - p->rect.min.x);
}

/* Go's / truncates toward zero, and so does C's, so these halve and quarter
 * negative coordinates the same way. */
Int image_y_cb_cr_c_offset(const ImageYCbCr *p, Int x, Int y) {
    ImagePoint m = p->rect.min;
    switch (p->subsample_ratio) {
    case IMAGE_Y_CB_CR_SUBSAMPLE_RATIO422:
        return (y - m.y) * p->c_stride + (x / 2 - m.x / 2);
    case IMAGE_Y_CB_CR_SUBSAMPLE_RATIO420:
        return (y / 2 - m.y / 2) * p->c_stride + (x / 2 - m.x / 2);
    case IMAGE_Y_CB_CR_SUBSAMPLE_RATIO440:
        return (y / 2 - m.y / 2) * p->c_stride + (x - m.x);
    case IMAGE_Y_CB_CR_SUBSAMPLE_RATIO411:
        return (y - m.y) * p->c_stride + (x / 4 - m.x / 4);
    case IMAGE_Y_CB_CR_SUBSAMPLE_RATIO410:
        return (y / 2 - m.y / 2) * p->c_stride + (x / 4 - m.x / 4);
    default:
        return (y - m.y) * p->c_stride + (x - m.x);
    }
}

ColorYCbCr image_y_cb_cr_y_cb_cr_at(const ImageYCbCr *p, Int x, Int y) {
    ColorYCbCr c = {0, 0, 0};
    if (!im_in(x, y, p->rect))
        return c;
    Int yi = image_y_cb_cr_y_offset(p, x, y);
    Int ci = image_y_cb_cr_c_offset(p, x, y);
    c.y = *im_byte(p->y, yi);
    c.cb = *im_byte(p->cb, ci);
    c.cr = *im_byte(p->cr, ci);
    return c;
}

ColorModel image_y_cb_cr_color_model(const ImageYCbCr *p) {
    (void)p;
    return color_y_cb_cr_model;
}

ImageRectangle image_y_cb_cr_bounds(const ImageYCbCr *p) {
    return p->rect;
}

Color image_y_cb_cr_at(const ImageYCbCr *p, Int x, Int y) {
    return color_y_cb_cr_as_color(image_y_cb_cr_y_cb_cr_at(p, x, y));
}

ColorRGBA64 image_y_cb_cr_rgba64_at(const ImageYCbCr *p, Int x, Int y) {
    return im_rgba64(color_y_cb_cr_rgba(image_y_cb_cr_y_cb_cr_at(p, x, y)));
}

ImageYCbCr image_y_cb_cr_sub_image(const ImageYCbCr *p, ImageRectangle r) {
    ImageYCbCr s;
    memset(&s, 0, sizeof s);
    s.subsample_ratio = p->subsample_ratio;
    r = image_rectangle_intersect(r, p->rect);
    if (image_rectangle_empty(r))
        return s;
    Int yi = image_y_cb_cr_y_offset(p, r.min.x, r.min.y);
    Int ci = image_y_cb_cr_c_offset(p, r.min.x, r.min.y);
    s.y = slice_sub(p->y, yi, p->y.len);
    s.cb = slice_sub(p->cb, ci, p->cb.len);
    s.cr = slice_sub(p->cr, ci, p->cr.len);
    s.y_stride = p->y_stride;
    s.c_stride = p->c_stride;
    s.rect = r;
    return s;
}

bool image_y_cb_cr_opaque(const ImageYCbCr *p) {
    (void)p;
    return true;
}

/* yCbCrSize: the size of the luma plane and of each chroma plane. */
static void im_y_cb_cr_size(ImageRectangle r, ImageYCbCrSubsampleRatio ratio, Int *w, Int *h,
                            Int *cw, Int *ch) {
    *w = r.max.x - r.min.x;
    *h = r.max.y - r.min.y;
    switch (ratio) {
    case IMAGE_Y_CB_CR_SUBSAMPLE_RATIO422:
        *cw = (r.max.x + 1) / 2 - r.min.x / 2;
        *ch = *h;
        break;
    case IMAGE_Y_CB_CR_SUBSAMPLE_RATIO420:
        *cw = (r.max.x + 1) / 2 - r.min.x / 2;
        *ch = (r.max.y + 1) / 2 - r.min.y / 2;
        break;
    case IMAGE_Y_CB_CR_SUBSAMPLE_RATIO440:
        *cw = *w;
        *ch = (r.max.y + 1) / 2 - r.min.y / 2;
        break;
    case IMAGE_Y_CB_CR_SUBSAMPLE_RATIO411:
        *cw = (r.max.x + 3) / 4 - r.min.x / 4;
        *ch = *h;
        break;
    case IMAGE_Y_CB_CR_SUBSAMPLE_RATIO410:
        *cw = (r.max.x + 3) / 4 - r.min.x / 4;
        *ch = (r.max.y + 1) / 2 - r.min.y / 2;
        break;
    default:
        *cw = *w;
        *ch = *h;
        break;
    }
}

/* One buffer of n bytes cut into the planes at the given offsets, each with
 * its capacity ending where the next begins, as Go's three index slices do. */
static void im_planes(Slice b, ImageYCbCr *p, Int i0, Int i1, Int i2) {
    p->y = slice_sub3(b, 0, i0, i0);
    p->cb = slice_sub3(b, i0, i1, i1);
    p->cr = slice_sub3(b, i1, i2, i2);
}

ImageYCbCr *image_new_y_cb_cr(Alloc *a, ImageRectangle r, ImageYCbCrSubsampleRatio ratio) {
    Int w, h, cw, ch;
    im_y_cb_cr_size(r, ratio, &w, &h, &cw, &ch);
    if (im_add2(im_mul3(1, w, h), im_mul3(2, cw, ch)) < 0)
        im_huge("image: NewYCbCr Rectangle has huge or negative dimensions");
    Int i0 = w * h, i1 = w * h + cw * ch, i2 = w * h + 2 * cw * ch;
    ImageYCbCr *p = (ImageYCbCr *)mem_alloc(a, sizeof *p, _Alignof(ImageYCbCr));
    if (p == NULL)
        return NULL;
    Slice b;
    if (!im_make(a, i2, &b)) {
        mem_free(a, p, sizeof *p, _Alignof(ImageYCbCr));
        return NULL;
    }
    im_planes(b, p, i0, i1, i2);
    p->subsample_ratio = ratio;
    p->y_stride = w;
    p->c_stride = cw;
    p->rect = r;
    return p;
}

/* The planes are one buffer, which y starts, and whose size is the three
 * capacities added up. */
void image_y_cb_cr_free(ImageYCbCr *p, Alloc *a) {
    if (p == NULL)
        return;
    if (p->y.p != NULL || p->cb.p != NULL)
        mem_free(a, p->y.p != NULL ? p->y.p : p->cb.p,
                 (size_t)(p->y.cap + p->cb.cap + p->cr.cap), 1);
    mem_free(a, p, sizeof *p, _Alignof(ImageYCbCr));
}

static ColorModel im_y_cb_cr_vt_color_model(void *self) {
    return image_y_cb_cr_color_model((const ImageYCbCr *)self);
}
static ImageRectangle im_y_cb_cr_vt_bounds(void *self) {
    return image_y_cb_cr_bounds((const ImageYCbCr *)self);
}
static Color im_y_cb_cr_vt_at(void *self, Int x, Int y) {
    return image_y_cb_cr_at((const ImageYCbCr *)self, x, y);
}
static ColorRGBA64 im_y_cb_cr_vt_rgba64_at(void *self, Int x, Int y) {
    return image_y_cb_cr_rgba64_at((const ImageYCbCr *)self, x, y);
}
static bool im_y_cb_cr_vt_opaque(void *self) {
    return image_y_cb_cr_opaque((const ImageYCbCr *)self);
}

static const ImageVT im_y_cb_cr_vt = {
    &burrow_type_ImageYCbCr, im_y_cb_cr_vt_color_model, im_y_cb_cr_vt_bounds,
    im_y_cb_cr_vt_at,        im_y_cb_cr_vt_rgba64_at,   NULL,
    NULL,                    NULL,                      im_y_cb_cr_vt_opaque,
};

Image image_y_cb_cr_as_image(ImageYCbCr *p) {
    Image m = {&im_y_cb_cr_vt, p};
    return m;
}

/* ---------------------------------------------------------------- NYCbCrA */

ColorModel image_ny_cb_cr_a_color_model(const ImageNYCbCrA *p) {
    (void)p;
    return color_ny_cb_cr_a_model;
}

ImageRectangle image_ny_cb_cr_a_bounds(const ImageNYCbCrA *p) {
    return p->y_cb_cr.rect;
}

ColorYCbCr image_ny_cb_cr_a_y_cb_cr_at(const ImageNYCbCrA *p, Int x, Int y) {
    return image_y_cb_cr_y_cb_cr_at(&p->y_cb_cr, x, y);
}

Int image_ny_cb_cr_a_y_offset(const ImageNYCbCrA *p, Int x, Int y) {
    return image_y_cb_cr_y_offset(&p->y_cb_cr, x, y);
}

Int image_ny_cb_cr_a_c_offset(const ImageNYCbCrA *p, Int x, Int y) {
    return image_y_cb_cr_c_offset(&p->y_cb_cr, x, y);
}

Int image_ny_cb_cr_a_a_offset(const ImageNYCbCrA *p, Int x, Int y) {
    return (y - p->y_cb_cr.rect.min.y) * p->a_stride + (x - p->y_cb_cr.rect.min.x);
}

ColorNYCbCrA image_ny_cb_cr_a_ny_cb_cr_a_at(const ImageNYCbCrA *p, Int x, Int y) {
    ColorNYCbCrA c;
    memset(&c, 0, sizeof c);
    if (!im_in(x, y, p->y_cb_cr.rect))
        return c;
    const ImageYCbCr *q = &p->y_cb_cr;
    Int yi = image_y_cb_cr_y_offset(q, x, y);
    Int ci = image_y_cb_cr_c_offset(q, x, y);
    Int ai = image_ny_cb_cr_a_a_offset(p, x, y);
    c.y_cb_cr.y = *im_byte(q->y, yi);
    c.y_cb_cr.cb = *im_byte(q->cb, ci);
    c.y_cb_cr.cr = *im_byte(q->cr, ci);
    c.a = *im_byte(p->a, ai);
    return c;
}

Color image_ny_cb_cr_a_at(const ImageNYCbCrA *p, Int x, Int y) {
    return color_ny_cb_cr_a_as_color(image_ny_cb_cr_a_ny_cb_cr_a_at(p, x, y));
}

ColorRGBA64 image_ny_cb_cr_a_rgba64_at(const ImageNYCbCrA *p, Int x, Int y) {
    return im_rgba64(color_ny_cb_cr_a_rgba(image_ny_cb_cr_a_ny_cb_cr_a_at(p, x, y)));
}

ImageNYCbCrA image_ny_cb_cr_a_sub_image(const ImageNYCbCrA *p, ImageRectangle r) {
    ImageNYCbCrA s;
    memset(&s, 0, sizeof s);
    s.y_cb_cr.subsample_ratio = p->y_cb_cr.subsample_ratio;
    r = image_rectangle_intersect(r, p->y_cb_cr.rect);
    if (image_rectangle_empty(r))
        return s;
    s.y_cb_cr = image_y_cb_cr_sub_image(&p->y_cb_cr, r);
    Int ai = image_ny_cb_cr_a_a_offset(p, r.min.x, r.min.y);
    s.a = slice_sub(p->a, ai, p->a.len);
    s.a_stride = p->a_stride;
    return s;
}

bool image_ny_cb_cr_a_opaque(const ImageNYCbCrA *p) {
    const ImageRectangle r = p->y_cb_cr.rect;
    if (image_rectangle_empty(r))
        return true;
    Int i0 = 0, i1 = image_rectangle_dx(r);
    for (Int y = r.min.y; y < r.max.y; y++) {
        Slice row = slice_sub(p->a, i0, i1);
        const Byte *b = (const Byte *)row.p;
        for (Int i = 0; i < row.len; i++)
            if (b[i] != 0xff)
                return false;
        i0 += p->a_stride;
        i1 += p->a_stride;
    }
    return true;
}

ImageNYCbCrA *image_new_ny_cb_cr_a(Alloc *a, ImageRectangle r, ImageYCbCrSubsampleRatio ratio) {
    Int w, h, cw, ch;
    im_y_cb_cr_size(r, ratio, &w, &h, &cw, &ch);
    if (im_add2(im_mul3(2, w, h), im_mul3(2, cw, ch)) < 0)
        im_huge("image: NewNYCbCrA Rectangle has huge or negative dimension");
    Int i0 = w * h, i1 = w * h + cw * ch, i2 = w * h + 2 * cw * ch, i3 = 2 * w * h + 2 * cw * ch;
    ImageNYCbCrA *p = (ImageNYCbCrA *)mem_alloc(a, sizeof *p, _Alignof(ImageNYCbCrA));
    if (p == NULL)
        return NULL;
    Slice b;
    if (!im_make(a, i3, &b)) {
        mem_free(a, p, sizeof *p, _Alignof(ImageNYCbCrA));
        return NULL;
    }
    im_planes(b, &p->y_cb_cr, i0, i1, i2);
    p->y_cb_cr.subsample_ratio = ratio;
    p->y_cb_cr.y_stride = w;
    p->y_cb_cr.c_stride = cw;
    p->y_cb_cr.rect = r;
    p->a = slice_sub(b, i2, b.len);
    p->a_stride = w;
    return p;
}

void image_ny_cb_cr_a_free(ImageNYCbCrA *p, Alloc *a) {
    if (p == NULL)
        return;
    const ImageYCbCr *q = &p->y_cb_cr;
    Int n = q->y.cap + q->cb.cap + q->cr.cap + p->a.cap;
    const void *base = q->y.p != NULL ? q->y.p : q->cb.p != NULL ? q->cb.p : p->a.p;
    if (base != NULL)
        mem_free(a, (void *)(uintptr_t)base, (size_t)n, 1);
    mem_free(a, p, sizeof *p, _Alignof(ImageNYCbCrA));
}

static ColorModel im_ny_cb_cr_a_vt_color_model(void *self) {
    return image_ny_cb_cr_a_color_model((const ImageNYCbCrA *)self);
}
static ImageRectangle im_ny_cb_cr_a_vt_bounds(void *self) {
    return image_ny_cb_cr_a_bounds((const ImageNYCbCrA *)self);
}
static Color im_ny_cb_cr_a_vt_at(void *self, Int x, Int y) {
    return image_ny_cb_cr_a_at((const ImageNYCbCrA *)self, x, y);
}
static ColorRGBA64 im_ny_cb_cr_a_vt_rgba64_at(void *self, Int x, Int y) {
    return image_ny_cb_cr_a_rgba64_at((const ImageNYCbCrA *)self, x, y);
}
static bool im_ny_cb_cr_a_vt_opaque(void *self) {
    return image_ny_cb_cr_a_opaque((const ImageNYCbCrA *)self);
}

static const ImageVT im_ny_cb_cr_a_vt = {
    &burrow_type_ImageNYCbCrA,  im_ny_cb_cr_a_vt_color_model,
    im_ny_cb_cr_a_vt_bounds,    im_ny_cb_cr_a_vt_at,
    im_ny_cb_cr_a_vt_rgba64_at, NULL,
    NULL,                       NULL,
    im_ny_cb_cr_a_vt_opaque,
};

Image image_ny_cb_cr_a_as_image(ImageNYCbCrA *p) {
    Image m = {&im_ny_cb_cr_a_vt, p};
    return m;
}

/* -------------------------------------------------------------- Rectangle */

static ColorModel im_rect_vt_color_model(void *self) {
    return image_rectangle_color_model(*(const ImageRectangle *)self);
}
static ImageRectangle im_rect_vt_bounds(void *self) {
    return *(const ImageRectangle *)self;
}
static Color im_rect_vt_at(void *self, Int x, Int y) {
    return image_rectangle_at(*(const ImageRectangle *)self, x, y);
}
static ColorRGBA64 im_rect_vt_rgba64_at(void *self, Int x, Int y) {
    return image_rectangle_rgba64_at(*(const ImageRectangle *)self, x, y);
}

static const ImageVT im_rect_vt = {
    &burrow_type_ImageRectangle,
    im_rect_vt_color_model,
    im_rect_vt_bounds,
    im_rect_vt_at,
    im_rect_vt_rgba64_at,
    NULL,
    NULL,
    NULL,
    NULL,
};

Image image_rectangle_as_image(const ImageRectangle *r) {
    Image m = {&im_rect_vt, (void *)(uintptr_t)r};
    return m;
}

/* ---------------------------------------------------------------- Uniform */

ImageUniform *image_new_uniform(Alloc *a, Color c) {
    ImageUniform *u = (ImageUniform *)mem_alloc(a, sizeof *u, _Alignof(ImageUniform));
    if (u == NULL)
        return NULL;
    u->c = c;
    return u;
}

void image_uniform_free(ImageUniform *u, Alloc *a) {
    if (u != NULL)
        mem_free(a, u, sizeof *u, _Alignof(ImageUniform));
}

ColorRGBAValue image_uniform_rgba(const ImageUniform *u) {
    return color_rgba(u->c);
}

Color image_uniform_convert(const ImageUniform *u, Color c) {
    (void)c;
    return u->c;
}

ImageRectangle image_uniform_bounds(const ImageUniform *u) {
    (void)u;
    ImageRectangle r = {{-1000000000, -1000000000}, {1000000000, 1000000000}};
    return r;
}

Color image_uniform_at(const ImageUniform *u, Int x, Int y) {
    (void)x;
    (void)y;
    return u->c;
}

ColorRGBA64 image_uniform_rgba64_at(const ImageUniform *u, Int x, Int y) {
    (void)x;
    (void)y;
    return im_rgba64(color_rgba(u->c));
}

bool image_uniform_opaque(const ImageUniform *u) {
    return color_rgba(u->c).a == 0xffff;
}

static Color im_uniform_model_convert(void *self, Color c) {
    return image_uniform_convert((const ImageUniform *)self, c);
}

static const ColorModelVT im_uniform_model_vt = {&burrow_type_ImageUniform,
                                                 im_uniform_model_convert};

ColorModel image_uniform_color_model(ImageUniform *u) {
    ColorModel m = {&im_uniform_model_vt, u};
    return m;
}

static ColorRGBAValue im_uniform_color_rgba(const ColorData *self) {
    return image_uniform_rgba((const ImageUniform *)self->ptr);
}

static const ColorVT im_uniform_color_vt = {&burrow_type_ImageUniform, im_uniform_color_rgba};

Color image_uniform_as_color(ImageUniform *u) {
    Color c;
    memset(&c, 0, sizeof c);
    c.vt = &im_uniform_color_vt;
    c.data.ptr = u;
    return c;
}

static ColorModel im_uniform_vt_color_model(void *self) {
    return image_uniform_color_model((ImageUniform *)self);
}
static ImageRectangle im_uniform_vt_bounds(void *self) {
    return image_uniform_bounds((const ImageUniform *)self);
}
static Color im_uniform_vt_at(void *self, Int x, Int y) {
    return image_uniform_at((const ImageUniform *)self, x, y);
}
static ColorRGBA64 im_uniform_vt_rgba64_at(void *self, Int x, Int y) {
    return image_uniform_rgba64_at((const ImageUniform *)self, x, y);
}
static bool im_uniform_vt_opaque(void *self) {
    return image_uniform_opaque((const ImageUniform *)self);
}

static const ImageVT im_uniform_vt = {
    &burrow_type_ImageUniform, im_uniform_vt_color_model, im_uniform_vt_bounds,
    im_uniform_vt_at,          im_uniform_vt_rgba64_at,   NULL,
    NULL,                      NULL,                      im_uniform_vt_opaque,
};

Image image_uniform_as_image(ImageUniform *u) {
    Image m = {&im_uniform_vt, u};
    return m;
}

/* Go's Black, White, Transparent and Opaque are package variables pointing at
 * Uniforms, and so are these. */
static ImageUniform im_black = {{&burrow__color_gray16_vt, {.gray16 = {0}}}};
static ImageUniform im_white = {{&burrow__color_gray16_vt, {.gray16 = {0xffff}}}};
static ImageUniform im_transparent = {{&burrow__color_alpha16_vt, {.alpha16 = {0}}}};
static ImageUniform im_opaque = {{&burrow__color_alpha16_vt, {.alpha16 = {0xffff}}}};

ImageUniform *const image_black = &im_black;
ImageUniform *const image_white = &im_white;
ImageUniform *const image_transparent = &im_transparent;
ImageUniform *const image_opaque = &im_opaque;

/* ---------------------------------------------------------------- formats
 *
 * Go keeps the formats in an atomic.Value holding a slice that
 * RegisterFormat replaces under a mutex. Here they are a list that only
 * grows, pushed at the front under a mutex and read without one: a reader
 * that loads the head sees a complete list, since a node is filled in before
 * it is published and never changes afterwards. The list is in reverse order
 * of registration, so sniffing walks it back to front to try the first
 * registered format first, as Go does. */

BURROW_SENTINEL_ERROR(image_err_format, "image: unknown format");

typedef struct ImFormat {
    Str name, magic;
    ImageDecodeFunc decode;
    ImageDecodeConfigFunc decode_config;
    struct ImFormat *next;
    Int index;
} ImFormat;

static SyncMutex im_formats_mu;
static ImFormat *im_formats;

void image_register_format(Str name, Str magic, ImageDecodeFunc decode,
                           ImageDecodeConfigFunc decode_config) {
    ImFormat *f = (ImFormat *)mem_alloc(heap_allocator(), sizeof *f, _Alignof(ImFormat));
    if (f == NULL)
        runtime_panic(BURROW_S("image: RegisterFormat: out of memory"));
    f->name = name;
    f->magic = magic;
    f->decode = decode;
    f->decode_config = decode_config;
    sync_mutex_lock(&im_formats_mu);
    ImFormat *head = __atomic_load_n(&im_formats, __ATOMIC_ACQUIRE);
    f->next = head;
    f->index = head == NULL ? 0 : head->index + 1;
    __atomic_store_n(&im_formats, f, __ATOMIC_RELEASE);
    sync_mutex_unlock(&im_formats_mu);
}

static bool im_match(Str magic, Slice b) {
    if (magic.len != b.len)
        return false;
    const Byte *p = (const Byte *)b.p;
    for (Int i = 0; i < b.len; i++)
        if (magic.p[i] != p[i] && magic.p[i] != '?')
            return false;
    return true;
}

/* sniff: the first registered format whose magic matches, or NULL. */
static const ImFormat *im_sniff(BufioReader *r) {
    const ImFormat *head = __atomic_load_n(&im_formats, __ATOMIC_ACQUIRE);
    if (head == NULL)
        return NULL;
    for (Int want = 0; want <= head->index; want++) {
        const ImFormat *f = head;
        while (f->index != want)
            f = f->next;
        Error err = BURROW_NO_ERROR;
        Slice b = bufio_reader_peek(r, f->magic.len, &err);
        if (BURROW_OK(err) && im_match(f->magic, b))
            return f;
    }
    return NULL;
}

/* asReader: r itself when it is a bufio.Reader, and a new one over it when
 * not, which *owned says to free. */
static BufioReader *im_as_reader(Alloc *a, IoReader r, bool *owned) {
    *owned = false;
    if (r.vt != NULL && r.vt->self_type == TYPE_BUFIO_READER)
        return (BufioReader *)r.data;
    BufioReader *br = bufio_new_reader(a, r);
    *owned = br != NULL;
    return br;
}

Image image_decode(Alloc *a, IoReader r, Str *name, Error *err) {
    Image m = {NULL, NULL};
    BURROW_OUT(name, im_no_name);
    bool owned;
    BufioReader *br = im_as_reader(a, r, &owned);
    if (br == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return m;
    }
    const ImFormat *f = im_sniff(br);
    if (f == NULL || f->decode.f == NULL) {
        BURROW_OUT(err, image_err_format);
    } else {
        Error e = BURROW_NO_ERROR;
        m = f->decode.f(f->decode.env, a, bufio_reader_as_io_reader(br), &e);
        BURROW_OUT(name, f->name);
        BURROW_OUT(err, e);
    }
    if (owned)
        bufio_reader_free(br);
    return m;
}

ImageConfig image_decode_config(Alloc *a, IoReader r, Str *name, Error *err) {
    ImageConfig c;
    memset(&c, 0, sizeof c);
    BURROW_OUT(name, im_no_name);
    bool owned;
    BufioReader *br = im_as_reader(a, r, &owned);
    if (br == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return c;
    }
    const ImFormat *f = im_sniff(br);
    if (f == NULL || f->decode_config.f == NULL) {
        BURROW_OUT(err, image_err_format);
    } else {
        Error e = BURROW_NO_ERROR;
        c = f->decode_config.f(f->decode_config.env, a, bufio_reader_as_io_reader(br), &e);
        BURROW_OUT(name, f->name);
        BURROW_OUT(err, e);
    }
    if (owned)
        bufio_reader_free(br);
    return c;
}

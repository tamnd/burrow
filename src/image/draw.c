/* image/draw, from draw.go and image/internal/imageutil's DrawYCbCr.
 *
 * Go's switch on the concrete types of dst, src and mask is a comparison of
 * each vtable's self_type here, and Go's assertion to RGBA64Image asks whether
 * the vtable has the slots that interface needs. The arithmetic is Go's, in
 * uint32_t where Go has uint32, so that it wraps where Go's does on colors
 * that are not valid alpha-premultiplied ones.
 *
 * The pixels are indexed the way Go indexes them, with a bounds check that
 * panics with Go's message, so an image whose pix is too short for its
 * rectangle fails the same way it does in Go.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/image/draw.h"

#include "burrow/mem/heap.h"
#include "burrow/runtime.h"

#include <stddef.h>
#include <string.h>

/* m, the largest value color.Color.RGBA returns. */
#define DR_M ((uint32_t)0xffff)

/* ----------------------------------------------------------------- helpers */

/* Go's s[i : i+n : i+n]. */
static inline Byte *dr_run(Slice s, Int i, Int n) {
    if (i < 0 || i > s.cap - n)
        (void)slice_sub3(s, i, i + n, i + n);
    return (Byte *)s.p + i;
}

/* Go's s[i : i+n : len(s)]. */
static inline Byte *dr_run_len(Slice s, Int i, Int n) {
    if (i < 0 || i > s.len - n)
        (void)slice_sub3(s, i, i + n, s.len);
    return (Byte *)s.p + i;
}

/* Go's s[i]. */
static inline Byte *dr_byte(Slice s, Int i) {
    if ((uint64_t)i >= (uint64_t)s.len)
        return (Byte *)slice_at(s, i);
    return (Byte *)s.p + i;
}

/* Go's s[lo:]. */
static inline Slice dr_from(Slice s, Int lo) {
    return slice_sub(s, lo, s.len);
}

/* Go's copy(dst[lo:hi], src[lo2:hi2]) where the caller has sliced both. */
static void dr_copy(Slice dst, Slice src) {
    Int n = dst.len < src.len ? dst.len : src.len;
    if (n > 0)
        memmove(dst.p, src.p, (size_t)n);
}

static inline bool dr_is(Image m, const Type *t) {
    return m.vt != NULL && m.vt->self_type == t;
}

/* Go's == on two interface values holding pointers. */
static inline bool dr_same(Image a, Image b) {
    return a.vt != NULL && b.vt != NULL && a.vt->self_type == b.vt->self_type &&
           a.data == b.data;
}

/* Whether m satisfies image.RGBA64Image. */
static inline bool dr_has_rgba64(Image m) {
    return m.vt != NULL && m.vt->rgba64_at != NULL;
}

/* Whether m satisfies draw.RGBA64Image. */
static inline bool dr_is_rgba64_image(Image m) {
    return dr_has_rgba64(m) && m.vt->set != NULL && m.vt->set_rgba64 != NULL;
}

static inline uint32_t dr_mask_alpha(Image mask, Int x, Int y) {
    return color_rgba(image_at(mask, x, y)).a;
}

static void *dr_scratch(size_t n, size_t size, void *stack, size_t stack_n) {
    if (n <= stack_n) {
        memset(stack, 0, n * size);
        return stack;
    }
    void *p = mem_alloc_array(heap_allocator(), n, size, _Alignof(int32_t));
    if (p == NULL)
        runtime_panic(BURROW_S("image/draw: out of memory"));
    return p;
}

static void dr_unscratch(void *p, size_t n, size_t size, void *stack) {
    if (p != stack)
        mem_free(heap_allocator(), p, n * size, _Alignof(int32_t));
}

/* ------------------------------------------------------------------- types */

#define DR_STR(s) {(const Byte *)(s), (Int)sizeof(s) - 1}

#define DR_TYPE(T, go, kind, tag)                                                      \
    const Type burrow_type_##T = {                                                     \
        DR_STR(go),                                                                    \
        DR_STR("image/draw"),                                                          \
        kind,                                                                          \
        (uint32_t)sizeof(T),                                                           \
        (uint16_t)_Alignof(T),                                                         \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
        NULL,                                                                          \
        NULL,                                                                          \
        NULL,                                                                          \
        0,                                                                             \
        tag,                                                                           \
        NULL,                                                                          \
    }

DR_TYPE(DrawOp, "Op", KIND_INT, 0x64723031U);
DR_TYPE(DrawQuantizer, "Quantizer", KIND_INTERFACE, 0x64723032U);
DR_TYPE(DrawDrawer, "Drawer", KIND_INTERFACE, 0x64723033U);

typedef struct DrFloydSteinberg {
    Byte unused;
} DrFloydSteinberg;

static const Type dr_floyd_steinberg_type = {
    DR_STR("floydSteinberg"),
    DR_STR("image/draw"),
    KIND_STRUCT,
    0,
    1,
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x64723034U,
    NULL,
};

/* -------------------------------------------------------------------- clip */

/* clip: clips r against each image's bounds, after moving them into dst's
 * coordinates, and moves sp and mp by as much as r.min moved. */
static void dr_clip(Image dst, ImageRectangle *r, Image src, ImagePoint *sp, Image mask,
                    ImagePoint *mp) {
    ImagePoint orig = r->min;
    *r = image_rectangle_intersect(*r, image_bounds(dst));
    *r = image_rectangle_intersect(
        *r, image_rectangle_add(image_bounds(src), image_point_sub(orig, *sp)));
    if (mask.vt != NULL)
        *r = image_rectangle_intersect(
            *r, image_rectangle_add(image_bounds(mask), image_point_sub(orig, *mp)));
    Int dx = r->min.x - orig.x;
    Int dy = r->min.y - orig.y;
    if (dx == 0 && dy == 0)
        return;
    sp->x += dx;
    sp->y += dy;
    if (mp != NULL) {
        mp->x += dx;
        mp->y += dy;
    }
}

static bool dr_backward(Image dst, ImageRectangle r, Image src, ImagePoint sp) {
    return dr_same(dst, src) &&
           image_rectangle_overlaps(
               r, image_rectangle_add(r, image_point_sub(sp, r.min))) &&
           (sp.y < r.min.y || (sp.y == r.min.y && sp.x < r.min.x));
}

/* --------------------------------------------------- RGBA destination paths */

static void dr_fill_over(ImageRGBA *dst, ImageRectangle r, uint32_t sr, uint32_t sg,
                         uint32_t sb, uint32_t sa) {
    uint32_t a = (DR_M - sa) * 0x101;
    Int i0 = image_rgba_pix_offset(dst, r.min.x, r.min.y);
    Int i1 = i0 + image_rectangle_dx(r) * 4;
    for (Int y = r.min.y; y != r.max.y; y++) {
        for (Int i = i0; i < i1; i += 4) {
            Byte *dr = dr_byte(dst->pix, i + 0);
            Byte *dg = dr_byte(dst->pix, i + 1);
            Byte *db = dr_byte(dst->pix, i + 2);
            Byte *da = dr_byte(dst->pix, i + 3);
            *dr = (Byte)(((uint32_t)*dr * a / DR_M + sr) >> 8);
            *dg = (Byte)(((uint32_t)*dg * a / DR_M + sg) >> 8);
            *db = (Byte)(((uint32_t)*db * a / DR_M + sb) >> 8);
            *da = (Byte)(((uint32_t)*da * a / DR_M + sa) >> 8);
        }
        i0 += dst->stride;
        i1 += dst->stride;
    }
}

static void dr_fill_src(ImageRGBA *dst, ImageRectangle r, uint32_t sr, uint32_t sg,
                        uint32_t sb, uint32_t sa) {
    Byte sr8 = (Byte)(sr >> 8), sg8 = (Byte)(sg >> 8), sb8 = (Byte)(sb >> 8),
         sa8 = (Byte)(sa >> 8);
    Int i0 = image_rgba_pix_offset(dst, r.min.x, r.min.y);
    Int i1 = i0 + image_rectangle_dx(r) * 4;
    for (Int i = i0; i < i1; i += 4) {
        *dr_byte(dst->pix, i + 0) = sr8;
        *dr_byte(dst->pix, i + 1) = sg8;
        *dr_byte(dst->pix, i + 2) = sb8;
        *dr_byte(dst->pix, i + 3) = sa8;
    }
    Slice first = slice_sub(dst->pix, i0, i1);
    for (Int y = r.min.y + 1; y < r.max.y; y++) {
        i0 += dst->stride;
        i1 += dst->stride;
        dr_copy(slice_sub(dst->pix, i0, i1), first);
    }
}

static void dr_copy_over(ImageRGBA *dst, ImageRectangle r, const ImageRGBA *src,
                         ImagePoint sp) {
    Int dx = image_rectangle_dx(r), dy = image_rectangle_dy(r);
    Int d0 = image_rgba_pix_offset(dst, r.min.x, r.min.y);
    Int s0 = image_rgba_pix_offset(src, sp.x, sp.y);
    Int ddelta, sdelta, i0, i1, idelta;
    if (r.min.y < sp.y || (r.min.y == sp.y && r.min.x <= sp.x)) {
        ddelta = dst->stride;
        sdelta = src->stride;
        i0 = 0, i1 = dx * 4, idelta = 4;
    } else {
        /* The source starts above the destination, or level with it and to
         * the left, so go right to left and bottom up. */
        d0 += (dy - 1) * dst->stride;
        s0 += (dy - 1) * src->stride;
        ddelta = -dst->stride;
        sdelta = -src->stride;
        i0 = (dx - 1) * 4, i1 = -4, idelta = -4;
    }
    for (; dy > 0; dy--) {
        Slice dpix = dr_from(dst->pix, d0);
        Slice spix = dr_from(src->pix, s0);
        for (Int i = i0; i != i1; i += idelta) {
            const Byte *s = dr_run(spix, i, 4);
            uint32_t sr = (uint32_t)s[0] * 0x101;
            uint32_t sg = (uint32_t)s[1] * 0x101;
            uint32_t sb = (uint32_t)s[2] * 0x101;
            uint32_t sa = (uint32_t)s[3] * 0x101;
            uint32_t a = (DR_M - sa) * 0x101;
            Byte *d = dr_run(dpix, i, 4);
            d[0] = (Byte)(((uint32_t)d[0] * a / DR_M + sr) >> 8);
            d[1] = (Byte)(((uint32_t)d[1] * a / DR_M + sg) >> 8);
            d[2] = (Byte)(((uint32_t)d[2] * a / DR_M + sb) >> 8);
            d[3] = (Byte)(((uint32_t)d[3] * a / DR_M + sa) >> 8);
        }
        d0 += ddelta;
        s0 += sdelta;
    }
}

/* drawCopySrc: dst_pix and src_pix start at r.min and sp. Rows go bottom up
 * when the source is above the destination, and memmove takes care of rows
 * that overlap. */
static void dr_copy_src(Slice dst_pix, Int dst_stride, ImageRectangle r, Slice src_pix,
                        Int src_stride, ImagePoint sp, Int bytes_per_row) {
    Int d0 = 0, s0 = 0, ddelta = dst_stride, sdelta = src_stride,
        dy = image_rectangle_dy(r);
    if (r.min.y > sp.y) {
        d0 = (dy - 1) * dst_stride;
        s0 = (dy - 1) * src_stride;
        ddelta = -dst_stride;
        sdelta = -src_stride;
    }
    for (; dy > 0; dy--) {
        dr_copy(slice_sub(dst_pix, d0, d0 + bytes_per_row),
                slice_sub(src_pix, s0, s0 + bytes_per_row));
        d0 += ddelta;
        s0 += sdelta;
    }
}

static void dr_nrgba_over(ImageRGBA *dst, ImageRectangle r, const ImageNRGBA *src,
                          ImagePoint sp) {
    Int i0 = (r.min.x - dst->rect.min.x) * 4;
    Int i1 = (r.max.x - dst->rect.min.x) * 4;
    Int si0 = (sp.x - src->rect.min.x) * 4;
    Int ymax = r.max.y - dst->rect.min.y;
    Int y = r.min.y - dst->rect.min.y;
    Int sy = sp.y - src->rect.min.y;
    for (; y != ymax; y++, sy++) {
        Slice dpix = dr_from(dst->pix, y * dst->stride);
        Slice spix = dr_from(src->pix, sy * src->stride);
        for (Int i = i0, si = si0; i < i1; i += 4, si += 4) {
            /* Non-premultiplied to premultiplied. */
            const Byte *s = dr_run(spix, si, 4);
            uint32_t sa = (uint32_t)s[3] * 0x101;
            uint32_t sr = (uint32_t)s[0] * sa / 0xff;
            uint32_t sg = (uint32_t)s[1] * sa / 0xff;
            uint32_t sb = (uint32_t)s[2] * sa / 0xff;
            Byte *d = dr_run(dpix, i, 4);
            uint32_t dr = d[0], dg = d[1], db = d[2], da = d[3];
            uint32_t a = (DR_M - sa) * 0x101;
            d[0] = (Byte)((dr * a / DR_M + sr) >> 8);
            d[1] = (Byte)((dg * a / DR_M + sg) >> 8);
            d[2] = (Byte)((db * a / DR_M + sb) >> 8);
            d[3] = (Byte)((da * a / DR_M + sa) >> 8);
        }
    }
}

static void dr_nrgba_src(ImageRGBA *dst, ImageRectangle r, const ImageNRGBA *src,
                         ImagePoint sp) {
    Int i0 = (r.min.x - dst->rect.min.x) * 4;
    Int i1 = (r.max.x - dst->rect.min.x) * 4;
    Int si0 = (sp.x - src->rect.min.x) * 4;
    Int ymax = r.max.y - dst->rect.min.y;
    Int y = r.min.y - dst->rect.min.y;
    Int sy = sp.y - src->rect.min.y;
    for (; y != ymax; y++, sy++) {
        Slice dpix = dr_from(dst->pix, y * dst->stride);
        Slice spix = dr_from(src->pix, sy * src->stride);
        for (Int i = i0, si = si0; i < i1; i += 4, si += 4) {
            const Byte *s = dr_run(spix, si, 4);
            uint32_t sa = (uint32_t)s[3] * 0x101;
            uint32_t sr = (uint32_t)s[0] * sa / 0xff;
            uint32_t sg = (uint32_t)s[1] * sa / 0xff;
            uint32_t sb = (uint32_t)s[2] * sa / 0xff;
            Byte *d = dr_run(dpix, i, 4);
            d[0] = (Byte)(sr >> 8);
            d[1] = (Byte)(sg >> 8);
            d[2] = (Byte)(sb >> 8);
            d[3] = (Byte)(sa >> 8);
        }
    }
}

static void dr_gray(ImageRGBA *dst, ImageRectangle r, const ImageGray *src,
                    ImagePoint sp) {
    Int i0 = (r.min.x - dst->rect.min.x) * 4;
    Int i1 = (r.max.x - dst->rect.min.x) * 4;
    Int si0 = sp.x - src->rect.min.x;
    Int ymax = r.max.y - dst->rect.min.y;
    Int y = r.min.y - dst->rect.min.y;
    Int sy = sp.y - src->rect.min.y;
    for (; y != ymax; y++, sy++) {
        Slice dpix = dr_from(dst->pix, y * dst->stride);
        Slice spix = dr_from(src->pix, sy * src->stride);
        for (Int i = i0, si = si0; i < i1; i += 4, si++) {
            Byte p = *dr_byte(spix, si);
            Byte *d = dr_run(dpix, i, 4);
            d[0] = p;
            d[1] = p;
            d[2] = p;
            d[3] = 255;
        }
    }
}

static void dr_cmyk(ImageRGBA *dst, ImageRectangle r, const ImageCMYK *src,
                    ImagePoint sp) {
    Int i0 = (r.min.x - dst->rect.min.x) * 4;
    Int i1 = (r.max.x - dst->rect.min.x) * 4;
    Int si0 = (sp.x - src->rect.min.x) * 4;
    Int ymax = r.max.y - dst->rect.min.y;
    Int y = r.min.y - dst->rect.min.y;
    Int sy = sp.y - src->rect.min.y;
    for (; y != ymax; y++, sy++) {
        Slice dpix = dr_from(dst->pix, y * dst->stride);
        Slice spix = dr_from(src->pix, sy * src->stride);
        for (Int i = i0, si = si0; i < i1; i += 4, si += 4) {
            const Byte *s = dr_run(spix, si, 4);
            Byte *d = dr_run(dpix, i, 4);
            ColorRGBA c = color_cmyk_to_rgb(s[0], s[1], s[2], s[3]);
            d[0] = c.r;
            d[1] = c.g;
            d[2] = c.b;
            d[3] = 255;
        }
    }
}

/* imageutil.DrawYCbCr. Reports false, having changed nothing, for the
 * subsample ratios it has no loop for. */
static bool dr_y_cb_cr(ImageRGBA *dst, ImageRectangle r, const ImageYCbCr *src,
                       ImagePoint sp) {
    Int x0 = (r.min.x - dst->rect.min.x) * 4;
    Int x1 = (r.max.x - dst->rect.min.x) * 4;
    Int y0 = r.min.y - dst->rect.min.y;
    Int y1 = r.max.y - dst->rect.min.y;
    ImageYCbCrSubsampleRatio ratio = src->subsample_ratio;
    if (ratio != IMAGE_Y_CB_CR_SUBSAMPLE_RATIO444 &&
        ratio != IMAGE_Y_CB_CR_SUBSAMPLE_RATIO422 &&
        ratio != IMAGE_Y_CB_CR_SUBSAMPLE_RATIO420 &&
        ratio != IMAGE_Y_CB_CR_SUBSAMPLE_RATIO440)
        return false;
    for (Int y = y0, sy = sp.y; y != y1; y++, sy++) {
        Slice dpix = dr_from(dst->pix, y * dst->stride);
        Int yi = (sy - src->rect.min.y) * src->y_stride + (sp.x - src->rect.min.x);
        /* The chroma row, and for 422 and 420 the chroma index is found from
         * sx on every pixel instead of being stepped. */
        Int crow;
        if (ratio == IMAGE_Y_CB_CR_SUBSAMPLE_RATIO444 ||
            ratio == IMAGE_Y_CB_CR_SUBSAMPLE_RATIO422)
            crow = (sy - src->rect.min.y) * src->c_stride;
        else
            crow = (sy / 2 - src->rect.min.y / 2) * src->c_stride;
        bool half = ratio == IMAGE_Y_CB_CR_SUBSAMPLE_RATIO422 ||
                    ratio == IMAGE_Y_CB_CR_SUBSAMPLE_RATIO420;
        Int ci = crow + (sp.x - src->rect.min.x);
        Int cbase = crow - src->rect.min.x / 2;
        for (Int x = x0, sx = sp.x; x != x1; x += 4, sx++, yi++, ci++) {
            Int c = half ? cbase + sx / 2 : ci;
            Byte yy = *dr_byte(src->y, yi);
            Byte cb = *dr_byte(src->cb, c);
            Byte cr = *dr_byte(src->cr, c);
            ColorRGBA v = color_y_cb_cr_to_rgb(yy, cb, cr);
            Byte *d = dr_run_len(dpix, x, 4);
            d[0] = v.r;
            d[1] = v.g;
            d[2] = v.b;
            d[3] = 255;
        }
    }
    return true;
}

static void dr_glyph_over(ImageRGBA *dst, ImageRectangle r, const ImageUniform *src,
                          const ImageAlpha *mask, ImagePoint mp) {
    Int i0 = image_rgba_pix_offset(dst, r.min.x, r.min.y);
    Int i1 = i0 + image_rectangle_dx(r) * 4;
    Int mi0 = image_alpha_pix_offset(mask, mp.x, mp.y);
    ColorRGBAValue s = image_uniform_rgba(src);
    for (Int y = r.min.y; y != r.max.y; y++) {
        for (Int i = i0, mi = mi0; i < i1; i += 4, mi++) {
            uint32_t ma = *dr_byte(mask->pix, mi);
            if (ma == 0)
                continue;
            ma |= ma << 8;
            uint32_t a = (DR_M - (s.a * ma / DR_M)) * 0x101;
            Byte *d = dr_run(dst->pix, i, 4);
            d[0] = (Byte)(((uint32_t)d[0] * a + s.r * ma) / DR_M >> 8);
            d[1] = (Byte)(((uint32_t)d[1] * a + s.g * ma) / DR_M >> 8);
            d[2] = (Byte)(((uint32_t)d[2] * a + s.b * ma) / DR_M >> 8);
            d[3] = (Byte)(((uint32_t)d[3] * a + s.a * ma) / DR_M >> 8);
        }
        i0 += dst->stride;
        i1 += dst->stride;
        mi0 += mask->stride;
    }
}

/* The start, end and step of each axis, walking backward when the source
 * overlaps the destination and starts above it or level and to the left.
 * backward says whether they could overlap at all. */
typedef struct DrWalk {
    Int x0, x1, dx, y0, y1, dy;
} DrWalk;

static DrWalk dr_walk(ImageRectangle r, ImagePoint sp, bool may_overlap) {
    DrWalk w = {r.min.x, r.max.x, 1, r.min.y, r.max.y, 1};
    if (may_overlap &&
        image_rectangle_overlaps(r,
                                 image_rectangle_add(r, image_point_sub(sp, r.min))) &&
        (sp.y < r.min.y || (sp.y == r.min.y && sp.x < r.min.x))) {
        w.x0 = r.max.x - 1, w.x1 = r.min.x - 1, w.dx = -1;
        w.y0 = r.max.y - 1, w.y1 = r.min.y - 1, w.dy = -1;
    }
    return w;
}

/* One pixel of dst composited Over, through a 16 bit mask alpha ma, with a
 * 16 bit source. */
static inline void dr_over_masked(Byte *d, uint32_t sr, uint32_t sg, uint32_t sb,
                                  uint32_t sa, uint32_t ma) {
    uint32_t dr = d[0], dg = d[1], db = d[2], da = d[3];
    uint32_t a = (DR_M - (sa * ma / DR_M)) * 0x101;
    d[0] = (Byte)((dr * a + sr * ma) / DR_M >> 8);
    d[1] = (Byte)((dg * a + sg * ma) / DR_M >> 8);
    d[2] = (Byte)((db * a + sb * ma) / DR_M >> 8);
    d[3] = (Byte)((da * a + sa * ma) / DR_M >> 8);
}

static void dr_gray_mask_over(ImageRGBA *dst, ImageRectangle r, const ImageGray *src,
                              ImagePoint sp, const ImageAlpha *mask, ImagePoint mp) {
    DrWalk w = dr_walk(r, sp, true);
    Int sy = sp.y + w.y0 - r.min.y;
    Int my = mp.y + w.y0 - r.min.y;
    Int sx0 = sp.x + w.x0 - r.min.x;
    Int mx0 = mp.x + w.x0 - r.min.x;
    Int sx1 = sx0 + (w.x1 - w.x0);
    Int i0 = image_rgba_pix_offset(dst, w.x0, w.y0);
    Int di = w.dx * 4;
    for (Int y = w.y0; y != w.y1; y += w.dy, sy += w.dy, my += w.dy) {
        for (Int i = i0, sx = sx0, mx = mx0; sx != sx1;
             i += di, sx += w.dx, mx += w.dx) {
            uint32_t ma = *dr_byte(mask->pix, image_alpha_pix_offset(mask, mx, my));
            ma |= ma << 8;
            uint32_t g = *dr_byte(src->pix, image_gray_pix_offset(src, sx, sy));
            g |= g << 8;
            dr_over_masked(dr_run(dst->pix, i, 4), g, g, g, 0xffff, ma);
        }
        i0 += w.dy * dst->stride;
    }
}

static void dr_rgba_mask_over(ImageRGBA *dst, ImageRectangle r, const ImageRGBA *src,
                              ImagePoint sp, const ImageAlpha *mask, ImagePoint mp) {
    DrWalk w = dr_walk(r, sp, dst == src);
    Int sy = sp.y + w.y0 - r.min.y;
    Int my = mp.y + w.y0 - r.min.y;
    Int sx0 = sp.x + w.x0 - r.min.x;
    Int mx0 = mp.x + w.x0 - r.min.x;
    Int sx1 = sx0 + (w.x1 - w.x0);
    Int i0 = image_rgba_pix_offset(dst, w.x0, w.y0);
    Int di = w.dx * 4;
    for (Int y = w.y0; y != w.y1; y += w.dy, sy += w.dy, my += w.dy) {
        for (Int i = i0, sx = sx0, mx = mx0; sx != sx1;
             i += di, sx += w.dx, mx += w.dx) {
            uint32_t ma = *dr_byte(mask->pix, image_alpha_pix_offset(mask, mx, my));
            ma |= ma << 8;
            Int si = image_rgba_pix_offset(src, sx, sy);
            uint32_t sr = *dr_byte(src->pix, si + 0);
            uint32_t sg = *dr_byte(src->pix, si + 1);
            uint32_t sb = *dr_byte(src->pix, si + 2);
            uint32_t sa = *dr_byte(src->pix, si + 3);
            sr |= sr << 8;
            sg |= sg << 8;
            sb |= sb << 8;
            sa |= sa << 8;
            dr_over_masked(dr_run(dst->pix, i, 4), sr, sg, sb, sa, ma);
        }
        i0 += w.dy * dst->stride;
    }
}

static void dr_rgba64_image_mask_over(ImageRGBA *dst, Image dsti, ImageRectangle r,
                                      Image src, ImagePoint sp, const ImageAlpha *mask,
                                      ImagePoint mp) {
    DrWalk w = dr_walk(r, sp, dr_same(dsti, src));
    Int sy = sp.y + w.y0 - r.min.y;
    Int my = mp.y + w.y0 - r.min.y;
    Int sx0 = sp.x + w.x0 - r.min.x;
    Int mx0 = mp.x + w.x0 - r.min.x;
    Int sx1 = sx0 + (w.x1 - w.x0);
    Int i0 = image_rgba_pix_offset(dst, w.x0, w.y0);
    Int di = w.dx * 4;
    for (Int y = w.y0; y != w.y1; y += w.dy, sy += w.dy, my += w.dy) {
        for (Int i = i0, sx = sx0, mx = mx0; sx != sx1;
             i += di, sx += w.dx, mx += w.dx) {
            uint32_t ma = *dr_byte(mask->pix, image_alpha_pix_offset(mask, mx, my));
            ma |= ma << 8;
            ColorRGBA64 s = src.vt->rgba64_at(src.data, sx, sy);
            dr_over_masked(dr_run(dst->pix, i, 4), s.r, s.g, s.b, s.a, ma);
        }
        i0 += w.dy * dst->stride;
    }
}

/* drawRGBA: any source and mask onto an RGBA, through RGBA64At when the
 * source and mask have it and At when they do not. */
static void dr_rgba(ImageRGBA *dst, Image dsti, ImageRectangle r, Image src,
                    ImagePoint sp, Image mask, ImagePoint mp, DrawOp op) {
    DrWalk w = dr_walk(r, sp, dr_same(dsti, src));
    Int sy = sp.y + w.y0 - r.min.y;
    Int my = mp.y + w.y0 - r.min.y;
    Int sx0 = sp.x + w.x0 - r.min.x;
    Int mx0 = mp.x + w.x0 - r.min.x;
    Int sx1 = sx0 + (w.x1 - w.x0);
    Int i0 = image_rgba_pix_offset(dst, w.x0, w.y0);
    Int di = w.dx * 4;

    if (dr_has_rgba64(src)) {
        if (mask.vt == NULL) {
            for (Int y = w.y0; y != w.y1; y += w.dy, sy += w.dy, my += w.dy) {
                for (Int i = i0, sx = sx0; sx != sx1; i += di, sx += w.dx) {
                    ColorRGBA64 s = src.vt->rgba64_at(src.data, sx, sy);
                    Byte *d = dr_run(dst->pix, i, 4);
                    if (op == DRAW_OVER) {
                        uint32_t dr = d[0], dg = d[1], db = d[2], da = d[3];
                        uint32_t a = (DR_M - s.a) * 0x101;
                        d[0] = (Byte)((dr * a / DR_M + s.r) >> 8);
                        d[1] = (Byte)((dg * a / DR_M + s.g) >> 8);
                        d[2] = (Byte)((db * a / DR_M + s.b) >> 8);
                        d[3] = (Byte)((da * a / DR_M + s.a) >> 8);
                    } else {
                        d[0] = (Byte)(s.r >> 8);
                        d[1] = (Byte)(s.g >> 8);
                        d[2] = (Byte)(s.b >> 8);
                        d[3] = (Byte)(s.a >> 8);
                    }
                }
                i0 += w.dy * dst->stride;
            }
            return;
        }
        if (dr_has_rgba64(mask)) {
            for (Int y = w.y0; y != w.y1; y += w.dy, sy += w.dy, my += w.dy) {
                for (Int i = i0, sx = sx0, mx = mx0; sx != sx1;
                     i += di, sx += w.dx, mx += w.dx) {
                    uint32_t ma = mask.vt->rgba64_at(mask.data, mx, my).a;
                    ColorRGBA64 s = src.vt->rgba64_at(src.data, sx, sy);
                    Byte *d = dr_run(dst->pix, i, 4);
                    if (op == DRAW_OVER) {
                        dr_over_masked(d, s.r, s.g, s.b, s.a, ma);
                    } else {
                        d[0] = (Byte)((uint32_t)s.r * ma / DR_M >> 8);
                        d[1] = (Byte)((uint32_t)s.g * ma / DR_M >> 8);
                        d[2] = (Byte)((uint32_t)s.b * ma / DR_M >> 8);
                        d[3] = (Byte)((uint32_t)s.a * ma / DR_M >> 8);
                    }
                }
                i0 += w.dy * dst->stride;
            }
            return;
        }
    }

    for (Int y = w.y0; y != w.y1; y += w.dy, sy += w.dy, my += w.dy) {
        for (Int i = i0, sx = sx0, mx = mx0; sx != sx1;
             i += di, sx += w.dx, mx += w.dx) {
            uint32_t ma = DR_M;
            if (mask.vt != NULL)
                ma = dr_mask_alpha(mask, mx, my);
            ColorRGBAValue s = color_rgba(image_at(src, sx, sy));
            Byte *d = dr_run(dst->pix, i, 4);
            if (op == DRAW_OVER) {
                dr_over_masked(d, s.r, s.g, s.b, s.a, ma);
            } else {
                d[0] = (Byte)(s.r * ma / DR_M >> 8);
                d[1] = (Byte)(s.g * ma / DR_M >> 8);
                d[2] = (Byte)(s.b * ma / DR_M >> 8);
                d[3] = (Byte)(s.a * ma / DR_M >> 8);
            }
        }
        i0 += w.dy * dst->stride;
    }
}

/* The fast paths for an RGBA destination. Reports whether one of them did
 * the whole draw; when it did not, drawRGBA does. */
static bool dr_rgba_fast(ImageRGBA *dst, ImageRectangle r, Image src, ImagePoint sp,
                         Image mask, ImagePoint mp, DrawOp op) {
    if (op == DRAW_OVER) {
        if (mask.vt == NULL) {
            if (dr_is(src, &burrow_type_ImageUniform)) {
                ColorRGBAValue s = image_uniform_rgba((const ImageUniform *)src.data);
                if (s.a == 0xffff)
                    dr_fill_src(dst, r, s.r, s.g, s.b, s.a);
                else
                    dr_fill_over(dst, r, s.r, s.g, s.b, s.a);
                return true;
            }
            if (dr_is(src, &burrow_type_ImageRGBA)) {
                dr_copy_over(dst, r, (const ImageRGBA *)src.data, sp);
                return true;
            }
            if (dr_is(src, &burrow_type_ImageNRGBA)) {
                dr_nrgba_over(dst, r, (const ImageNRGBA *)src.data, sp);
                return true;
            }
            /* A YCbCr, a Gray and a CMYK are opaque, so with no mask Over is
             * the same as Src. */
            if (dr_is(src, &burrow_type_ImageYCbCr))
                return dr_y_cb_cr(dst, r, (const ImageYCbCr *)src.data, sp);
            if (dr_is(src, &burrow_type_ImageGray)) {
                dr_gray(dst, r, (const ImageGray *)src.data, sp);
                return true;
            }
            if (dr_is(src, &burrow_type_ImageCMYK)) {
                dr_cmyk(dst, r, (const ImageCMYK *)src.data, sp);
                return true;
            }
        } else if (dr_is(mask, &burrow_type_ImageAlpha)) {
            const ImageAlpha *mask0 = (const ImageAlpha *)mask.data;
            if (dr_is(src, &burrow_type_ImageUniform)) {
                dr_glyph_over(dst, r, (const ImageUniform *)src.data, mask0, mp);
                return true;
            }
            if (dr_is(src, &burrow_type_ImageRGBA)) {
                dr_rgba_mask_over(dst, r, (const ImageRGBA *)src.data, sp, mask0, mp);
                return true;
            }
            if (dr_is(src, &burrow_type_ImageGray)) {
                dr_gray_mask_over(dst, r, (const ImageGray *)src.data, sp, mask0, mp);
                return true;
            }
            if (dr_has_rgba64(src)) {
                dr_rgba64_image_mask_over(dst, image_rgba_as_image(dst), r, src, sp,
                                          mask0, mp);
                return true;
            }
        }
        return false;
    }
    if (mask.vt != NULL)
        return false;
    if (dr_is(src, &burrow_type_ImageUniform)) {
        ColorRGBAValue s = image_uniform_rgba((const ImageUniform *)src.data);
        dr_fill_src(dst, r, s.r, s.g, s.b, s.a);
        return true;
    }
    if (dr_is(src, &burrow_type_ImageRGBA)) {
        const ImageRGBA *src0 = (const ImageRGBA *)src.data;
        Int d0 = image_rgba_pix_offset(dst, r.min.x, r.min.y);
        Int s0 = image_rgba_pix_offset(src0, sp.x, sp.y);
        dr_copy_src(dr_from(dst->pix, d0), dst->stride, r, dr_from(src0->pix, s0),
                    src0->stride, sp, 4 * image_rectangle_dx(r));
        return true;
    }
    if (dr_is(src, &burrow_type_ImageNRGBA)) {
        dr_nrgba_src(dst, r, (const ImageNRGBA *)src.data, sp);
        return true;
    }
    if (dr_is(src, &burrow_type_ImageYCbCr))
        return dr_y_cb_cr(dst, r, (const ImageYCbCr *)src.data, sp);
    if (dr_is(src, &burrow_type_ImageGray)) {
        dr_gray(dst, r, (const ImageGray *)src.data, sp);
        return true;
    }
    if (dr_is(src, &burrow_type_ImageCMYK)) {
        dr_cmyk(dst, r, (const ImageCMYK *)src.data, sp);
        return true;
    }
    return false;
}

/* ---------------------------------------------------------- drawPaletted */

static inline int32_t dr_clamp(int32_t i) {
    if (i < 0)
        return 0;
    if (i > 0xffff)
        return 0xffff;
    return i;
}

/* sqDiff: the squared difference of x and y shifted right by 2, so that four
 * of them add up without overflowing. */
static inline uint32_t dr_sq_diff(int32_t x, int32_t y) {
    uint32_t d = (uint32_t)x - (uint32_t)y;
    return (d * d) >> 2;
}

static inline ColorRGBAValue dr_px(Image src, Int x, Int y) {
    if (dr_is(src, &burrow_type_ImageRGBA))
        return color_rgba_rgba(image_rgba_rgba_at((const ImageRGBA *)src.data, x, y));
    if (dr_is(src, &burrow_type_ImageNRGBA))
        return color_nrgba_rgba(
            image_nrgba_nrgba_at((const ImageNRGBA *)src.data, x, y));
    if (dr_is(src, &burrow_type_ImageYCbCr))
        return color_y_cb_cr_rgba(
            image_y_cb_cr_y_cb_cr_at((const ImageYCbCr *)src.data, x, y));
    return color_rgba(image_at(src, x, y));
}

#define DR_STACK 256

typedef int32_t DrQuad[4];

static void dr_paletted(Image dst, ImageRectangle r, Image src, ImagePoint sp,
                        bool fs) {
    DrQuad pal_stack[DR_STACK], curr_stack[DR_STACK], next_stack[DR_STACK];
    DrQuad *palette = NULL;
    size_t npal = 0;
    Slice pix = {NULL, 0, 0, NULL};
    Slice pal_colors = {NULL, 0, 0, NULL};
    Int stride = 0;
    if (dr_is(dst, &burrow_type_ImagePaletted)) {
        const ImagePaletted *p = (const ImagePaletted *)dst.data;
        pal_colors = p->palette;
        npal = (size_t)p->palette.len;
        palette = (DrQuad *)dr_scratch(npal, sizeof(DrQuad), pal_stack, DR_STACK);
        for (size_t i = 0; i < npal; i++) {
            ColorRGBAValue c = color_rgba(((const Color *)p->palette.p)[i]);
            palette[i][0] = (int32_t)c.r;
            palette[i][1] = (int32_t)c.g;
            palette[i][2] = (int32_t)c.b;
            palette[i][3] = (int32_t)c.a;
        }
        pix = dr_from(p->pix, image_paletted_pix_offset(p, r.min.x, r.min.y));
        stride = p->stride;
    }

    /* The errors carried to the pixels of this row and the next. The two
     * extra entries save a test at each edge. */
    DrQuad *curr = NULL, *next = NULL;
    size_t nq = 0;
    if (fs) {
        nq = (size_t)image_rectangle_dx(r) + 2;
        curr = (DrQuad *)dr_scratch(nq, sizeof(DrQuad), curr_stack, DR_STACK);
        next = (DrQuad *)dr_scratch(nq, sizeof(DrQuad), next_stack, DR_STACK);
    }
    DrQuad *curr0 = curr, *next0 = next;

    Int w = image_rectangle_dx(r), h = image_rectangle_dy(r);
    for (Int y = 0; y != h; y++) {
        for (Int x = 0; x != w; x++) {
            ColorRGBAValue s = dr_px(src, sp.x + x, sp.y + y);
            int32_t er = (int32_t)s.r, eg = (int32_t)s.g, eb = (int32_t)s.b,
                    ea = (int32_t)s.a;
            if (fs) {
                er = dr_clamp(er + curr[x + 1][0] / 16);
                eg = dr_clamp(eg + curr[x + 1][1] / 16);
                eb = dr_clamp(eb + curr[x + 1][2] / 16);
                ea = dr_clamp(ea + curr[x + 1][3] / 16);
            }

            if (palette != NULL) {
                /* The nearest palette entry in R, G, B, A space. */
                size_t best = 0;
                uint32_t best_sum = UINT32_MAX;
                for (size_t i = 0; i < npal; i++) {
                    uint32_t sum =
                        dr_sq_diff(er, palette[i][0]) + dr_sq_diff(eg, palette[i][1]) +
                        dr_sq_diff(eb, palette[i][2]) + dr_sq_diff(ea, palette[i][3]);
                    if (sum < best_sum) {
                        best = i, best_sum = sum;
                        if (sum == 0)
                            break;
                    }
                }
                *dr_byte(pix, y * stride + x) = (Byte)best;
                if (!fs)
                    continue;
                if (best >= npal)
                    (void)slice_at(pal_colors, (Int)best); /* Go's index panic */
                er -= palette[best][0];
                eg -= palette[best][1];
                eb -= palette[best][2];
                ea -= palette[best][3];
            } else {
                ColorRGBA64 out = {(uint16_t)er, (uint16_t)eg, (uint16_t)eb,
                                   (uint16_t)ea};
                dst.vt->set(dst.data, r.min.x + x, r.min.y + y,
                            color_rgba64_as_color(out));
                if (!fs)
                    continue;
                ColorRGBAValue d = color_rgba(image_at(dst, r.min.x + x, r.min.y + y));
                er -= (int32_t)d.r;
                eg -= (int32_t)d.g;
                eb -= (int32_t)d.b;
                ea -= (int32_t)d.a;
            }

            /* Pass the error on: 3/16 below left, 5/16 below, 1/16 below
             * right and 7/16 to the right. */
            next[x + 0][0] += er * 3;
            next[x + 0][1] += eg * 3;
            next[x + 0][2] += eb * 3;
            next[x + 0][3] += ea * 3;
            next[x + 1][0] += er * 5;
            next[x + 1][1] += eg * 5;
            next[x + 1][2] += eb * 5;
            next[x + 1][3] += ea * 5;
            next[x + 2][0] += er;
            next[x + 2][1] += eg;
            next[x + 2][2] += eb;
            next[x + 2][3] += ea;
            curr[x + 2][0] += er * 7;
            curr[x + 2][1] += eg * 7;
            curr[x + 2][2] += eb * 7;
            curr[x + 2][3] += ea * 7;
        }
        if (fs) {
            DrQuad *t = curr;
            curr = next;
            next = t;
            memset(next, 0, nq * sizeof(DrQuad));
        }
    }

    if (fs) {
        dr_unscratch(curr0, nq, sizeof(DrQuad), curr_stack);
        dr_unscratch(next0, nq, sizeof(DrQuad), next_stack);
    }
    if (palette != NULL)
        dr_unscratch(palette, npal, sizeof(DrQuad), pal_stack);
}

/* ----------------------------------------------------------------- DrawMask */

/* FALLBACK1.17 and FALLBACK1.0: any dst, src and mask, pixel by pixel. */
static void dr_generic(Image dst, ImageRectangle r, Image src, ImagePoint sp,
                       Image mask, ImagePoint mp, DrawOp op) {
    Int x0 = r.min.x, x1 = r.max.x, dx = 1;
    Int y0 = r.min.y, y1 = r.max.y, dy = 1;
    if (dr_backward(dst, r, src, sp)) {
        x0 = r.max.x - 1, x1 = r.min.x - 1, dx = -1;
        y0 = r.max.y - 1, y1 = r.min.y - 1, dy = -1;
    }

    if (dr_is_rgba64_image(dst) && dr_has_rgba64(src) &&
        (mask.vt == NULL || dr_has_rgba64(mask))) {
        const ImageVT *dv = dst.vt, *sv = src.vt;
        Int sy = sp.y + y0 - r.min.y;
        Int my = mp.y + y0 - r.min.y;
        for (Int y = y0; y != y1; y += dy, sy += dy, my += dy) {
            Int sx = sp.x + x0 - r.min.x;
            Int mx = mp.x + x0 - r.min.x;
            for (Int x = x0; x != x1; x += dx, sx += dx, mx += dx) {
                if (mask.vt == NULL) {
                    ColorRGBA64 s = sv->rgba64_at(src.data, sx, sy);
                    if (op == DRAW_SRC) {
                        dv->set_rgba64(dst.data, x, y, s);
                    } else {
                        uint32_t a = DR_M - s.a;
                        ColorRGBA64 d = dv->rgba64_at(dst.data, x, y);
                        ColorRGBA64 o = {
                            (uint16_t)((uint16_t)((uint32_t)d.r * a / DR_M) + s.r),
                            (uint16_t)((uint16_t)((uint32_t)d.g * a / DR_M) + s.g),
                            (uint16_t)((uint16_t)((uint32_t)d.b * a / DR_M) + s.b),
                            (uint16_t)((uint16_t)((uint32_t)d.a * a / DR_M) + s.a),
                        };
                        dv->set_rgba64(dst.data, x, y, o);
                    }
                    continue;
                }
                uint32_t ma = mask.vt->rgba64_at(mask.data, mx, my).a;
                if (ma == 0) {
                    if (op != DRAW_OVER) {
                        ColorRGBA64 zero = {0, 0, 0, 0};
                        dv->set_rgba64(dst.data, x, y, zero);
                    }
                } else if (ma == DR_M && op == DRAW_SRC) {
                    dv->set_rgba64(dst.data, x, y, sv->rgba64_at(src.data, sx, sy));
                } else {
                    ColorRGBA64 s = sv->rgba64_at(src.data, sx, sy);
                    ColorRGBA64 o;
                    if (op == DRAW_OVER) {
                        ColorRGBA64 d = dv->rgba64_at(dst.data, x, y);
                        uint32_t a = DR_M - ((uint32_t)s.a * ma / DR_M);
                        o.r =
                            (uint16_t)(((uint32_t)d.r * a + (uint32_t)s.r * ma) / DR_M);
                        o.g =
                            (uint16_t)(((uint32_t)d.g * a + (uint32_t)s.g * ma) / DR_M);
                        o.b =
                            (uint16_t)(((uint32_t)d.b * a + (uint32_t)s.b * ma) / DR_M);
                        o.a =
                            (uint16_t)(((uint32_t)d.a * a + (uint32_t)s.a * ma) / DR_M);
                    } else {
                        o.r = (uint16_t)((uint32_t)s.r * ma / DR_M);
                        o.g = (uint16_t)((uint32_t)s.g * ma / DR_M);
                        o.b = (uint16_t)((uint32_t)s.b * ma / DR_M);
                        o.a = (uint16_t)((uint32_t)s.a * ma / DR_M);
                    }
                    dv->set_rgba64(dst.data, x, y, o);
                }
            }
        }
        return;
    }

    Int sy = sp.y + y0 - r.min.y;
    Int my = mp.y + y0 - r.min.y;
    for (Int y = y0; y != y1; y += dy, sy += dy, my += dy) {
        Int sx = sp.x + x0 - r.min.x;
        Int mx = mp.x + x0 - r.min.x;
        for (Int x = x0; x != x1; x += dx, sx += dx, mx += dx) {
            uint32_t ma = DR_M;
            if (mask.vt != NULL)
                ma = dr_mask_alpha(mask, mx, my);
            if (ma == 0) {
                if (op != DRAW_OVER)
                    dst.vt->set(dst.data, x, y,
                                color_alpha16_as_color(color_transparent));
            } else if (ma == DR_M && op == DRAW_SRC) {
                dst.vt->set(dst.data, x, y, image_at(src, sx, sy));
            } else {
                ColorRGBAValue s = color_rgba(image_at(src, sx, sy));
                ColorRGBA64 o;
                if (op == DRAW_OVER) {
                    ColorRGBAValue d = color_rgba(image_at(dst, x, y));
                    uint32_t a = DR_M - (s.a * ma / DR_M);
                    o.r = (uint16_t)((d.r * a + s.r * ma) / DR_M);
                    o.g = (uint16_t)((d.g * a + s.g * ma) / DR_M);
                    o.b = (uint16_t)((d.b * a + s.b * ma) / DR_M);
                    o.a = (uint16_t)((d.a * a + s.a * ma) / DR_M);
                } else {
                    o.r = (uint16_t)(s.r * ma / DR_M);
                    o.g = (uint16_t)(s.g * ma / DR_M);
                    o.b = (uint16_t)(s.b * ma / DR_M);
                    o.a = (uint16_t)(s.a * ma / DR_M);
                }
                dst.vt->set(dst.data, x, y, color_rgba64_as_color(o));
            }
        }
    }
}

void draw_draw_mask(DrawImage dst, ImageRectangle r, Image src, ImagePoint sp,
                    Image mask, ImagePoint mp, DrawOp op) {
    dr_clip(dst, &r, src, &sp, mask, &mp);
    if (image_rectangle_empty(r))
        return;

    /* Go's fast paths. For NRGBA and NRGBA64 they are more than fast: going
     * through premultiplied color would lose information. */
    if (dr_is(dst, &burrow_type_ImageRGBA)) {
        ImageRGBA *dst0 = (ImageRGBA *)dst.data;
        if (!dr_rgba_fast(dst0, r, src, sp, mask, mp, op))
            dr_rgba(dst0, dst, r, src, sp, mask, mp, op);
        return;
    }
    if (dr_is(dst, &burrow_type_ImagePaletted)) {
        if (op == DRAW_SRC && mask.vt == NULL) {
            if (dr_is(src, &burrow_type_ImageUniform)) {
                ImagePaletted *dst0 = (ImagePaletted *)dst.data;
                const ImageUniform *u = (const ImageUniform *)src.data;
                Byte index = (Byte)color_palette_index(dst0->palette, u->c);
                Int i0 = image_paletted_pix_offset(dst0, r.min.x, r.min.y);
                Int i1 = i0 + image_rectangle_dx(r);
                for (Int i = i0; i < i1; i++)
                    *dr_byte(dst0->pix, i) = index;
                Slice first = slice_sub(dst0->pix, i0, i1);
                for (Int y = r.min.y + 1; y < r.max.y; y++) {
                    i0 += dst0->stride;
                    i1 += dst0->stride;
                    dr_copy(slice_sub(dst0->pix, i0, i1), first);
                }
                return;
            }
            if (!dr_backward(dst, r, src, sp)) {
                dr_paletted(dst, r, src, sp, false);
                return;
            }
        }
    } else if (dr_is(dst, &burrow_type_ImageNRGBA)) {
        if (op == DRAW_SRC && mask.vt == NULL && dr_is(src, &burrow_type_ImageNRGBA)) {
            ImageNRGBA *dst0 = (ImageNRGBA *)dst.data;
            const ImageNRGBA *src0 = (const ImageNRGBA *)src.data;
            Int d0 = image_nrgba_pix_offset(dst0, r.min.x, r.min.y);
            Int s0 = image_nrgba_pix_offset(src0, sp.x, sp.y);
            dr_copy_src(dr_from(dst0->pix, d0), dst0->stride, r, dr_from(src0->pix, s0),
                        src0->stride, sp, 4 * image_rectangle_dx(r));
            return;
        }
    } else if (dr_is(dst, &burrow_type_ImageNRGBA64)) {
        if (op == DRAW_SRC && mask.vt == NULL &&
            dr_is(src, &burrow_type_ImageNRGBA64)) {
            ImageNRGBA64 *dst0 = (ImageNRGBA64 *)dst.data;
            const ImageNRGBA64 *src0 = (const ImageNRGBA64 *)src.data;
            Int d0 = image_nrgba64_pix_offset(dst0, r.min.x, r.min.y);
            Int s0 = image_nrgba64_pix_offset(src0, sp.x, sp.y);
            dr_copy_src(dr_from(dst0->pix, d0), dst0->stride, r, dr_from(src0->pix, s0),
                        src0->stride, sp, 8 * image_rectangle_dx(r));
            return;
        }
    }

    dr_generic(dst, r, src, sp, mask, mp, op);
}

void draw_draw(DrawImage dst, ImageRectangle r, Image src, ImagePoint sp, DrawOp op) {
    Image nil = {NULL, NULL};
    draw_draw_mask(dst, r, src, sp, nil, image_pt(0, 0), op);
}

void draw_op_draw(DrawOp op, DrawImage dst, ImageRectangle r, Image src,
                  ImagePoint sp) {
    draw_draw(dst, r, src, sp, op);
}

/* ------------------------------------------------------------------ Drawers */

static void dr_op_vt_draw(void *self, DrawImage dst, ImageRectangle r, Image src,
                          ImagePoint sp) {
    draw_op_draw(*(const DrawOp *)self, dst, r, src, sp);
}

static const DrawDrawerVT dr_op_drawer_vt = {&burrow_type_DrawOp, dr_op_vt_draw};

DrawDrawer draw_op_as_drawer(const DrawOp *op) {
    DrawDrawer d = {&dr_op_drawer_vt, (void *)(uintptr_t)op};
    return d;
}

static void dr_fs_vt_draw(void *self, DrawImage dst, ImageRectangle r, Image src,
                          ImagePoint sp) {
    (void)self;
    Image nil = {NULL, NULL};
    dr_clip(dst, &r, src, &sp, nil, NULL);
    if (image_rectangle_empty(r))
        return;
    dr_paletted(dst, r, src, sp, true);
}

static const DrawDrawerVT dr_fs_vt = {&dr_floyd_steinberg_type, dr_fs_vt_draw};

const DrawDrawer draw_floyd_steinberg = {&dr_fs_vt, NULL};

/* image/draw: compositing one image onto another.
 *
 *     ImageRGBA *dst = image_new_rgba(a, image_rect(0, 0, 64, 64));
 *     draw_draw(image_rgba_as_image(dst), image_rgba_bounds(dst),
 *               image_uniform_as_image(image_white), image_zp, DRAW_SRC);
 *
 * draw_draw_mask lines up r.min in dst with sp in src and mp in mask, and
 * replaces the rectangle r in dst with src drawn over it or in place of it,
 * through the mask. A nil Image, (Image){0}, is an opaque mask. The fast paths
 * are Go's, with the same arithmetic, so the pixels come out the same as Go's
 * on every image type.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package image/draw */

#ifndef BURROW_IMAGE_DRAW_H
#define BURROW_IMAGE_DRAW_H

#include "burrow/core.h"
#include "burrow/image.h"
#include "burrow/image/color.h"
#include "burrow/own.h"
#include "burrow/type.h"

#ifdef __cplusplus
extern "C" {
#endif

/* draw.Image: an Image whose set slot is filled in. Every in-memory image in
 * image has one. */
typedef ImageVT DrawImageVT;
typedef Image DrawImage;

/* draw.RGBA64Image: an Image whose rgba64_at, set and set_rgba64 slots are
 * filled in. */
typedef ImageVT DrawRGBA64ImageVT;
typedef Image DrawRGBA64Image;

/* draw.Op: a Porter-Duff compositing operator. */
typedef Int DrawOp;
enum {
    /* (src in mask) over dst. */
    DRAW_OVER = 0,
    /* src in mask. */
    DRAW_SRC = 1,
};

extern const Type burrow_type_DrawOp;

/* draw.Quantizer: makes a palette for an image. quantize appends up to
 * p.cap - p.len colors to p and returns the palette to convert m with. */
typedef struct DrawQuantizerVT {
    const Type *self_type;
    ColorPalette (*quantize)(void *self, ColorPalette p, Image m);
} DrawQuantizerVT;

typedef struct DrawQuantizer {
    const DrawQuantizerVT *vt;
    void *data;
} DrawQuantizer;

extern const Type burrow_type_DrawQuantizer;

static inline ColorPalette draw_quantizer_quantize(DrawQuantizer q, ColorPalette p,
                                                   Image m) {
    return q.vt->quantize(q.data, p, m);
}

/* draw.Drawer: something with a Draw method, which lines up r.min in dst with
 * sp in src and replaces the rectangle r in dst with the result of drawing src
 * on it. */
typedef struct DrawDrawerVT {
    const Type *self_type;
    void (*draw)(void *self, DrawImage dst, ImageRectangle r, Image src, ImagePoint sp);
} DrawDrawerVT;

typedef struct DrawDrawer {
    const DrawDrawerVT *vt;
    void *data;
} DrawDrawer;

extern const Type burrow_type_DrawDrawer;

static inline void draw_drawer_draw(DrawDrawer d, DrawImage dst, ImageRectangle r,
                                    Image src, ImagePoint sp) {
    d.vt->draw(d.data, dst, r, src, sp);
}

/* Op.Draw: draw_draw_mask with a nil mask and op. */
void draw_op_draw(DrawOp op, DrawImage dst, ImageRectangle r, Image src, ImagePoint sp);

/* An Op as a Drawer. It points at op, which has to outlive it. */
BURROW_BORROWS(ret, op) DrawDrawer draw_op_as_drawer(const DrawOp *op);

/* draw.FloydSteinberg: the Src op with Floyd-Steinberg error diffusion. When
 * dst is an ImagePaletted each pixel gets the index of the palette entry
 * nearest to it, and the difference is spread over the pixels not yet
 * drawn. */
extern const DrawDrawer draw_floyd_steinberg;

/* draw.Draw: draw_draw_mask with a nil mask. */
void draw_draw(DrawImage dst, ImageRectangle r, Image src, ImagePoint sp, DrawOp op);

/* draw.DrawMask. dst has to have a set slot. A mask with a NULL vt is
 * opaque. */
void draw_draw_mask(DrawImage dst, ImageRectangle r, Image src, ImagePoint sp,
                    Image mask, ImagePoint mp, DrawOp op);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_IMAGE_DRAW_H */

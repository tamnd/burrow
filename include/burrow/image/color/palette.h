/* image/color/palette: the Plan 9 and web-safe palettes.
 *
 *     Color c = color_palette_convert(palette_plan9,
 *                                     color_rgba_as_color((ColorRGBA){0x12, 0x34, 0x56, 0xff}));
 *
 * Both are slices of Color, each one a ColorRGBA with alpha 0xff, in Go's
 * order. Go has them as package variables, and so does burrow: the Slice is
 * const, the colors it points at are not, so a program that wants to change an
 * entry the way it could in Go can.
 *
 * Copyright 2013 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package image/color/palette */

#ifndef BURROW_IMAGE_COLOR_PALETTE_H
#define BURROW_IMAGE_COLOR_PALETTE_H

#include "burrow/image/color.h"

#ifdef __cplusplus
extern "C" {
#endif

/* palette.Plan9: the 256 colors of Plan 9's 8 bit color map. Go's comment
 * says that it is not the same as a 4x4x4 color cube and is better for
 * rendering photographs. */
extern const ColorPalette palette_plan9;

/* palette.WebSafe: the 216 web-safe colors, a 6x6x6 cube with steps of 0x33,
 * listed with blue changing fastest. */
extern const ColorPalette palette_web_safe;

#ifdef __cplusplus
}
#endif

#endif /* BURROW_IMAGE_COLOR_PALETTE_H */

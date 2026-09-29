/* image/gif: reads and writes GIF images, animated ones included.
 *
 * gif_decode gives the first frame of a file as an ImagePaletted, and
 * gif_decode_all gives every frame with its delay and disposal method, the
 * loop count and the global palette. gif_encode writes one image, turning it
 * into a paletted one first when it is not, and gif_encode_all writes a Gif:
 *
 *     Error err = BURROW_NO_ERROR;
 *     Gif *g = gif_decode_all(a, r, &err);
 *     ...
 *     err = gif_encode_all(a, w, g);
 *     gif_free(g, a);
 *
 * Go registers the format with image.RegisterFormat when the package is
 * imported. C has nothing that runs on import, so call gif_register once
 * before image_decode or image_decode_config is to recognise GIF. Calling
 * gif_decode directly needs no registration.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package image/gif */

#ifndef BURROW_IMAGE_GIF_H
#define BURROW_IMAGE_GIF_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/image.h"
#include "burrow/image/color.h"
#include "burrow/image/draw.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/own.h"
#include "burrow/slice.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The disposal methods, what a viewer does with a frame before drawing the
 * next one. 0 means none was given. */
enum {
    GIF_DISPOSAL_NONE = 0x01,
    GIF_DISPOSAL_BACKGROUND = 0x02,
    GIF_DISPOSAL_PREVIOUS = 0x03
};

/* gif.GIF: the frames of a GIF and how to show them.
 *
 * image is a slice of ImagePalettedPtr and delay a slice of Int, one per
 * frame, in hundredths of a second. loop_count is how many times to play the
 * animation again: 0 is forever, -1 is once, and n plays it n+1 times.
 * disposal is a slice of Byte, one per frame, and a nil one means 0 for every
 * frame. config is the global palette, as a ColorPalette model, and the size.
 * A config with no color model means every frame has its own palette, and a
 * zero config means the size is the first frame's bounds.max. background_index
 * is the entry in the global palette that GIF_DISPOSAL_BACKGROUND clears to. */
typedef struct Gif {
    Slice image;
    Slice delay;
    Int loop_count;
    Slice disposal;
    ImageConfig config;
    Byte background_index;
} Gif;

/* --------------------------------------------------------------- decoding */

/* gif.Decode: reads a GIF from r and returns its first frame, an
 * ImagePaletted allocated in a, whose bounds are where the frame sits on the
 * screen and need not start at (0, 0). Give it back with image_decoded_free.
 *
 * Errors have Go's text, such as "gif: can't recognize format \"GIF90a\"" or
 * "gif: reading image data: unexpected EOF". The image is nil on any error
 * and nothing is left allocated. When the input is untrusted, look at the
 * size gif_decode_config gives first, since a frame can ask for 4 GB. */
BURROW_OWNS(ret) Image gif_decode(Alloc *a, IoReader r, Error *err);

/* gif.DecodeAll: reads every frame. The Gif, its slices, the frames and the
 * palettes are all allocated in a, and gif_free gives them back. NULL on any
 * error, with nothing left allocated. */
BURROW_OWNS(ret) Gif *gif_decode_all(Alloc *a, IoReader r, Error *err);

/* Gives back a Gif that gif_decode_all made in a, everything in it included.
 * Not for a Gif you put together yourself. NULL is fine. */
void gif_free(Gif *g, Alloc *a);

/* gif.DecodeConfig: reads only the header and the global palette. The model
 * is always a ColorPalette, allocated in a, which is empty when the file has
 * no global palette. image_config_free gives it back. */
BURROW_OWNS(ret) ImageConfig gif_decode_config(Alloc *a, IoReader r, Error *err);

/* Registers "gif" with image_register_format, for image_decode and
 * image_decode_config. Only the first call does anything, and it is safe to
 * call from any thread. */
void gif_register(void);

/* --------------------------------------------------------------- encoding */

/* gif.Options. num_colors is the most colors to use, from 1 to 256, and any
 * other value means 256. A quantizer with a NULL vt means the first
 * num_colors entries of palette_plan9, and a drawer with a NULL vt means
 * draw_floyd_steinberg. A quantizer is handed an empty palette with room for
 * num_colors entries, allocated in the encode's allocator and given back when
 * the encode ends, and what it returns has to last that long. */
typedef struct GifOptions {
    Int num_colors;
    DrawQuantizer quantizer;
    DrawDrawer drawer;
} GifOptions;

/* gif.EncodeAll: writes the frames of g to w. Every frame needs a palette of
 * 1 to 256 colors and has to sit inside the config's size. A frame whose
 * palette is the global one, or matches it, is written without a palette of
 * its own. The first palette entry with an alpha of 0 is the transparent one.
 * A frame's indexes have to fit in its palette's bit width. Scratch memory,
 * about 70 KB, comes from a and goes back to it before this returns. */
BURROW_STATIC(ret) Error gif_encode_all(Alloc *a, IoWriter w, const Gif *g);

/* gif.Encode: writes m to w as a single frame GIF, moved so that it starts at
 * (0, 0). An ImagePaletted with no more than num_colors colors is written as
 * it is. Otherwise the image is drawn with o's drawer onto a palette from o's
 * quantizer, or onto one whose model is a ColorPalette is converted pixel by
 * pixel first. o may be NULL, which is the zero GifOptions. An image 65536 or
 * more pixels wide or tall gives "gif: image is too large to encode". */
BURROW_STATIC(ret) Error gif_encode(Alloc *a, IoWriter w, Image m, const GifOptions *o);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_IMAGE_GIF_H */

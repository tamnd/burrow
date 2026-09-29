/* image/png: reads and writes PNG images.
 *
 * png_decode reads any PNG the PNG specification allows except for the
 * ancillary chunks, which it skips, and gives an image of the package's type
 * that holds the pixels without loss: ImageGray for 8-bit grayscale,
 * ImageNRGBA64 for 16-bit color with alpha, ImagePaletted for a palette and so
 * on, the same type Go gives for the same file. png_encode writes any Image,
 * choosing the smallest color type that holds it:
 *
 *     png_register();
 *     Error err = BURROW_NO_ERROR;
 *     Image m = image_decode(a, r, NULL, &err);
 *     ...
 *     err = png_encode(a, w, m);
 *     image_decoded_free(m, a);
 *
 * Go registers the format with image.RegisterFormat when the package is
 * imported. C has nothing that runs on import, so call png_register once
 * before image_decode or image_decode_config is to recognise PNG. Calling
 * png_decode directly needs no registration.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package image/png */

#ifndef BURROW_IMAGE_PNG_H
#define BURROW_IMAGE_PNG_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/image.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/own.h"
#include "burrow/type.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------------- errors */

/* png.FormatError: the input is not a valid PNG. The message is
 * "png: invalid format: " and then the text, as in "png: invalid format:
 * invalid checksum". errors_is matches two of them with the same text, and
 * errors_as with TYPE_PNG_FORMAT_ERROR gives a pointer to the text. */
typedef Str PngFormatError;

extern const Type *const TYPE_PNG_FORMAT_ERROR;

/* Go's Error method, built in a. */
BURROW_OWNS(ret) Str png_format_error_error(PngFormatError e, Alloc *a);

/* An Error for e with its message, built in a. Out of memory gives
 * burrow_err_out_of_memory. */
BURROW_OWNS(ret) Error png_format_error_as_error(PngFormatError e, Alloc *a);

/* png.UnsupportedError: the input is a valid PNG that uses a feature this
 * decoder does not have, or an image the encoder cannot write. The message is
 * "png: unsupported feature: " and then the text. */
typedef Str PngUnsupportedError;

extern const Type *const TYPE_PNG_UNSUPPORTED_ERROR;

BURROW_OWNS(ret) Str png_unsupported_error_error(PngUnsupportedError e, Alloc *a);
BURROW_OWNS(ret) Error png_unsupported_error_as_error(PngUnsupportedError e, Alloc *a);

/* --------------------------------------------------------------- decoding */

/* png.Decode: reads a PNG from r and returns it as an Image, allocated in a.
 * The image is one of ImageGray, ImageGray16, ImageRGBA, ImageRGBA64,
 * ImageNRGBA, ImageNRGBA64 and ImagePaletted, and starts at (0, 0). Give it
 * back with image_decoded_free.
 *
 * Errors are a PngFormatError or PngUnsupportedError for a bad or unusual
 * file, io_err_unexpected_eof when r ends early, and whatever r or the zlib
 * reader gives otherwise. The image is nil on any error and nothing is left
 * allocated. */
BURROW_OWNS(ret) Image png_decode(Alloc *a, IoReader r, Error *err);

/* png.DecodeConfig: reads only as far as the size and the color model. For a
 * paletted file the model is the palette, allocated in a, and
 * image_config_free gives it back. */
BURROW_OWNS(ret) ImageConfig png_decode_config(Alloc *a, IoReader r, Error *err);

/* Registers "png" with image_register_format, for image_decode and
 * image_decode_config. Only the first call does anything, and it is safe to
 * call from any thread. */
void png_register(void);

/* --------------------------------------------------------------- encoding */

/* png.CompressionLevel. The four levels map to zlib's default, none, best
 * speed and best compression, and any other value is the default. */
typedef Int PngCompressionLevel;

enum {
    PNG_DEFAULT_COMPRESSION = 0,
    PNG_NO_COMPRESSION = -1,
    PNG_BEST_SPEED = -2,
    PNG_BEST_COMPRESSION = -3
};

/* png.EncoderBuffer: the buffers and compressor one encode needs, which a
 * PngEncoderBufferPool can keep between encodes so they are not allocated
 * every time. Opaque. */
typedef struct PngEncoderBuffer PngEncoderBuffer;

/* Gives a buffer back to the allocator it was made from. A pool calls this for
 * the buffers it drops. NULL is fine. */
void png_encoder_buffer_free(PngEncoderBuffer *e);

/* png.EncoderBufferPool: where an encoder gets its buffer and puts it back.
 * get may return NULL, and the encoder then makes a new one. After put the
 * pool owns the buffer. */
typedef struct PngEncoderBufferPoolVT {
    const Type *self_type;
    PngEncoderBuffer *(*get)(void *self);
    void (*put)(void *self, PngEncoderBuffer *b);
} PngEncoderBufferPoolVT;

typedef struct PngEncoderBufferPool {
    const PngEncoderBufferPoolVT *vt;
    void *data;
} PngEncoderBufferPool;

/* png.Encoder. The zero value encodes at the default level with no pool. */
typedef struct PngEncoder {
    PngCompressionLevel compression_level;
    PngEncoderBufferPool buffer_pool;
} PngEncoder;

/* png.Encode: writes m to w as a PNG at the default level. A new buffer comes
 * from a and goes back to it before this returns.
 *
 * An Image whose color model is a ColorPalette and that has color_index_at is
 * written with a palette. The rest are written as 8-bit gray, 16-bit gray, or
 * 8 or 16-bit truecolor, with alpha when the image is not opaque. An image
 * with no pixels, or wider or taller than a uint32 holds, gives a
 * PngFormatError. */
BURROW_STATIC(ret) Error png_encode(Alloc *a, IoWriter w, Image m);

/* Encoder.Encode: the same at enc's level, and with its pool when it has
 * one. A buffer made because the pool had none comes from a and is put in the
 * pool afterwards. */
BURROW_STATIC(ret) Error png_encoder_encode(const PngEncoder *enc, Alloc *a, IoWriter w,
                                            Image m);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_IMAGE_PNG_H */

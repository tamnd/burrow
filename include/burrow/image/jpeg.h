/* image/jpeg: reads and writes JPEG images.
 *
 * jpeg_decode reads baseline and progressive files, grayscale, YCbCr, RGB,
 * CMYK and YCbCrK, with any chroma subsampling whose factors divide the
 * largest one. jpeg_encode writes a baseline 4:2:0 file, or a grayscale one
 * for an ImageGray, at a quality from 1 to 100:
 *
 *     JpegOptions o = {.quality = 90};
 *     Error err = jpeg_encode(a, w, m, &o);
 *     ...
 *     Image back = jpeg_decode(a, r, &err);
 *     image_decoded_free(back, a);
 *
 * Go registers the format with image.RegisterFormat when the package is
 * imported. C has nothing that runs on import, so call jpeg_register once
 * before image_decode or image_decode_config is to recognise JPEG. Calling
 * jpeg_decode directly needs no registration.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package image/jpeg */

#ifndef BURROW_IMAGE_JPEG_H
#define BURROW_IMAGE_JPEG_H

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

/* jpeg.FormatError: the input is not a valid JPEG. The message is "invalid
 * JPEG format: " and then the text, as in "invalid JPEG format: missing SOI
 * marker". errors_is matches two of them with the same text, and errors_as
 * with TYPE_JPEG_FORMAT_ERROR gives a pointer to the text. */
typedef Str JpegFormatError;

extern const Type *const TYPE_JPEG_FORMAT_ERROR;

/* Go's Error method, built in a. */
BURROW_OWNS(ret) Str jpeg_format_error_error(JpegFormatError e, Alloc *a);

/* An Error for e with its message, built in a. Out of memory gives
 * burrow_err_out_of_memory. */
BURROW_OWNS(ret) Error jpeg_format_error_as_error(JpegFormatError e, Alloc *a);

/* jpeg.UnsupportedError: the input is a valid JPEG that uses a feature this
 * decoder does not have, such as 12-bit samples or lossless coding. The
 * message is "unsupported JPEG feature: " and then the text. */
typedef Str JpegUnsupportedError;

extern const Type *const TYPE_JPEG_UNSUPPORTED_ERROR;

BURROW_OWNS(ret) Str jpeg_unsupported_error_error(JpegUnsupportedError e, Alloc *a);
BURROW_OWNS(ret) Error jpeg_unsupported_error_as_error(JpegUnsupportedError e,
                                                       Alloc *a);

/* --------------------------------------------------------------- decoding */

/* jpeg.Reader, which Go keeps only for compatibility: an io.Reader that can
 * also hand out one byte at a time. Nothing here takes one. */
typedef struct JpegReaderVT {
    IoReaderVT reader;
    IoByteReaderVT byte_reader;
} JpegReaderVT;

typedef struct JpegReader {
    const JpegReaderVT *vt;
    void *data;
} JpegReader;

/* jpeg.Decode: reads a JPEG from r and returns it as an Image allocated in a,
 * starting at (0, 0). A grayscale file gives an ImageGray, a YCbCr one an
 * ImageYCbCr, an RGB one an ImageRGBA, and a CMYK or YCbCrK one an ImageCMYK.
 * Give it back with image_decoded_free.
 *
 * Errors are a JpegFormatError or JpegUnsupportedError for a bad or unusual
 * file, io_err_unexpected_eof when r ends early, and whatever r gives
 * otherwise. The image is nil on any error and nothing is left allocated.
 * When the input is untrusted, look at the size jpeg_decode_config gives
 * first, since a file of a few hundred bytes can ask for gigabytes. */
BURROW_OWNS(ret) Image jpeg_decode(Alloc *a, IoReader r, Error *err);

/* jpeg.DecodeConfig: reads as far as the frame header, and gives
 * color_gray_model, color_y_cb_cr_model, color_rgba_model or color_cmyk_model
 * with the size. Nothing is allocated. */
BURROW_STATIC(ret) ImageConfig jpeg_decode_config(Alloc *a, IoReader r, Error *err);

/* Registers "jpeg" with image_register_format, for image_decode and
 * image_decode_config. Only the first call does anything, and it is safe to
 * call from any thread. */
void jpeg_register(void);

/* --------------------------------------------------------------- encoding */

/* jpeg.DefaultQuality. */
enum { JPEG_DEFAULT_QUALITY = 75 };

/* jpeg.Options. quality runs from 1 to 100, higher is better, and a value
 * outside that is clipped to it. */
typedef struct JpegOptions {
    Int quality;
} JpegOptions;

/* jpeg.Encode: writes m to w as a baseline JPEG with 4:2:0 chroma
 * subsampling, or as a grayscale one when m is an ImageGray. o may be NULL,
 * for JPEG_DEFAULT_QUALITY. Alpha is dropped. An image 65536 or more pixels
 * wide or tall gives "jpeg: image is too large to encode". The encoder state,
 * about 4 KB with its buffer, comes from a and goes back before this
 * returns. */
BURROW_STATIC(ret) Error jpeg_encode(Alloc *a, IoWriter w, Image m,
                                     const JpegOptions *o);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_IMAGE_JPEG_H */

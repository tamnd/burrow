/* mime, media types and the encoded words of RFC 2047.
 *
 * A media type is the value of a Content-Type or Content-Disposition header, a
 * type such as text/html and a list of parameters after it. mime_parse_media_type
 * reads one and mime_format_media_type writes one:
 *
 *     Map *params;
 *     Str t = mime_parse_media_type(a, BURROW_S("text/html; charset=UTF-8"),
 *                                   &params, &err);
 *
 * An encoded word is how a mail header carries text that is not plain ASCII,
 * such as =?UTF-8?q?Caf=C3=A9?=. mime_word_encoder_encode makes one and
 * MimeWordDecoder reads them back.
 *
 * Like the strings functions, a function here that builds a string can hand
 * back its input when there is nothing to do. What it does build is one
 * allocation from the allocator it was given, len bytes long, and each key and
 * value in a params map is one too. An arena is the easy way to give them all
 * back at once. A failed allocation gives the empty string, and nothing is left
 * allocated behind it.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package mime */

#ifndef BURROW_MIME_H
#define BURROW_MIME_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/func.h"
#include "burrow/io.h"
#include "burrow/map.h"
#include "burrow/mem.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------ media types */

/* mime.ErrInvalidMediaParameter: the media type was fine and a parameter after
 * it was not. mime_parse_media_type hands back the media type with it. */
extern const Error mime_err_invalid_media_parameter;

/* mime.FormatMediaType. t and the parameters in param as a media type, with the
 * type and the parameter names in lower case and the parameters sorted by name.
 * param maps Str to Str, and NULL is fine for none. A value that is not plain
 * ASCII is written the RFC 2231 way, as name*=utf-8''%E2%82%AC. Anything that
 * cannot be written, such as a type or a name that is not a token, gives the
 * empty string, as in Go. */
BURROW_OWNS(ret) Str mime_format_media_type(Alloc *a, Str t, Map *param);

/* mime.ParseMediaType. Reads a media type and its parameters, per RFC 1521 and
 * RFC 2183. The media type comes back in lower case with the white space around
 * it gone. *params gets a new map from Str to Str, from the parameter names in
 * lower case to the values as they were, with the RFC 2231 continuations and
 * encodings put back together.
 *
 * With a bad media type the result is empty, *params is NULL and err says what
 * was wrong. With a bad parameter after a good media type the result is the
 * media type, *params is NULL and err is mime_err_invalid_media_parameter. A
 * parameter given twice with two different values is "mime: duplicate
 * parameter name". params may be NULL when you only want the type. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, v) Str mime_parse_media_type(Alloc *a, Str v,
                                                                  Map **params,
                                                                  Error *err);

/* ---------------------------------------------------------- encoded words */

/* mime.WordEncoder: which of the two encodings of RFC 2047 to use. */
typedef Byte MimeWordEncoder;

/* mime.BEncoding, base64, and mime.QEncoding, which is like quoted-printable
 * and leaves ASCII readable. */
#define MIME_B_ENCODING ((MimeWordEncoder)'b')
#define MIME_Q_ENCODING ((MimeWordEncoder)'q')

extern const Type *const TYPE_MIME_WORD_ENCODER;

/* WordEncoder.Encode. s as encoded words in charset, which is the IANA name of
 * the charset s is in, or s itself when it is ASCII with no control
 * characters. With UTF-8 the output is split into words of at most 75
 * characters, and never in the middle of a character. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, s) Str mime_word_encoder_encode(MimeWordEncoder e,
                                                                     Alloc *a,
                                                                     Str charset,
                                                                     Str s);

/* The type of WordDecoder.CharsetReader: a reader that turns input, which is
 * text in charset, into UTF-8. charset is in lower case. The decoder reads what
 * it gives to the end and is done with it before the call that used it
 * returns, so anything it allocated can be let go of then. */
BURROW_FUNC(MimeCharsetReader, IoReader, Str charset, IoReader input, Error *err);

/* mime.WordDecoder. UTF-8, ISO-8859-1 and US-ASCII are built in. For any other
 * charset the decoder calls charset_reader, and with charset_reader nil it is
 * an error. A zeroed MimeWordDecoder is ready to use. */
typedef struct MimeWordDecoder {
    MimeCharsetReader charset_reader;
} MimeWordDecoder;

extern const Type *const TYPE_MIME_WORD_DECODER;

/* WordDecoder.Decode. The text of one encoded word, such as =?UTF-8?q?hi?=, in
 * UTF-8. Anything else is "mime: invalid RFC 2047 encoded-word", and bad base64
 * or a bad escape inside a word is an error too. */
BURROW_OWNS(ret) Str mime_word_decoder_decode(const MimeWordDecoder *d, Alloc *a,
                                              Str word, Error *err);

/* WordDecoder.DecodeHeader. header with every encoded word in it decoded and
 * the white space between two encoded words taken out. A word that does not
 * decode is left as it is, so the only error is one from charset_reader, or an
 * unknown charset when there is none. header itself comes back when there is no
 * encoded word in it. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, header) Str mime_word_decoder_decode_header(
    const MimeWordDecoder *d, Alloc *a, Str header, Error *err);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_MIME_H */

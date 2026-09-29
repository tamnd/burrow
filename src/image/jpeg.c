/* image/jpeg, from reader.go, huffman.go, scan.go, dct.go and writer.go.
 *
 * The decoder is Go's. Segments are read in order through a buffer that can
 * give back the one or two bytes Huffman decoding reads past the end of a
 * scan. Sequential scans are turned into pixels a block at a time, and a
 * progressive file keeps every block's coefficients until the end of the
 * image and turns them into pixels then.
 *
 * The encoder is Go's as well, with the same quantization tables, Huffman
 * tables and forward DCT, so the bytes it writes are the bytes Go writes.
 *
 * Go's DCTs work in int32 and let garbage input wrap. C calls that undefined,
 * so the arithmetic here goes through uint32 where it can overflow, which
 * wraps the same way, and the results match Go's bit for bit.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/image/jpeg.h"

#include "burrow/bufio.h"
#include "burrow/image/color.h"
#include "burrow/image/draw.h"
#include "burrow/sync.h"

#include <string.h>

/* ------------------------------------------------------------------ errors */

typedef struct JpegErrorBox {
    Str s;
    Str message;
} JpegErrorBox;

/* NOLINTBEGIN(bugprone-macro-parentheses) */
#define JPEG_ERROR_TYPE(desc, name, tag)                                               \
    static const Type desc = {                                                         \
        {(const Byte *)name, sizeof name - 1},                                         \
        {(const Byte *)"image/jpeg", 10},                                              \
        KIND_STRING,                                                                   \
        (uint32_t)sizeof(Str),                                                         \
        (uint16_t)_Alignof(Str),                                                       \
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
/* NOLINTEND(bugprone-macro-parentheses) */

JPEG_ERROR_TYPE(jpeg_format_desc, "FormatError", 0x6a706665U);           /* "jpfe" */
JPEG_ERROR_TYPE(jpeg_unsupported_desc, "UnsupportedError", 0x6a707565U); /* "jpue" */

const Type *const TYPE_JPEG_FORMAT_ERROR = &jpeg_format_desc;
const Type *const TYPE_JPEG_UNSUPPORTED_ERROR = &jpeg_unsupported_desc;

#define JPEG_FORMAT_PREFIX "invalid JPEG format: "
#define JPEG_UNSUPPORTED_PREFIX "unsupported JPEG feature: "

static Str jpeg_error_message(const void *self) {
    return ((const JpegErrorBox *)self)->message;
}

static bool jpeg_format_is(const void *self, Error target);
static bool jpeg_unsupported_is(const void *self, Error target);
static Error jpeg_format_clone(const void *self, Alloc *a);
static Error jpeg_unsupported_clone(const void *self, Alloc *a);

static const ErrorVT jpeg_format_vt = {
    .self_type = &jpeg_format_desc,
    .message = jpeg_error_message,
    .is = jpeg_format_is,
    .clone = jpeg_format_clone,
};

static const ErrorVT jpeg_unsupported_vt = {
    .self_type = &jpeg_unsupported_desc,
    .message = jpeg_error_message,
    .is = jpeg_unsupported_is,
    .clone = jpeg_unsupported_clone,
};

static bool jpeg_same_text(const void *self, Error target) {
    Str a = ((const JpegErrorBox *)self)->s;
    Str b = ((const JpegErrorBox *)target.data)->s;
    return a.len == b.len && (a.len == 0 || memcmp(a.p, b.p, (size_t)a.len) == 0);
}

static bool jpeg_format_is(const void *self, Error target) {
    return target.vt == &jpeg_format_vt && target.data != NULL &&
           jpeg_same_text(self, target);
}

static bool jpeg_unsupported_is(const void *self, Error target) {
    return target.vt == &jpeg_unsupported_vt && target.data != NULL &&
           jpeg_same_text(self, target);
}

/* The prefix and e in one allocation, with the box in front of them. */
static Error jpeg_error_box(const ErrorVT *vt, const char *prefix, Str e, Alloc *a) {
    Int plen = (Int)strlen(prefix);
    Int mlen = plen + e.len;
    JpegErrorBox *b = (JpegErrorBox *)mem_alloc_nozero(
        a, sizeof(JpegErrorBox) + (size_t)mlen, _Alignof(JpegErrorBox));
    if (b == NULL)
        return burrow_err_out_of_memory;
    Byte *p = (Byte *)(b + 1);
    memcpy(p, prefix, (size_t)plen);
    if (e.len > 0)
        memcpy(p + plen, e.p, (size_t)e.len);
    b->message = str_from_bytes(p, mlen);
    b->s = str_from_bytes(p + plen, e.len);
    return (Error){vt, b};
}

static Str jpeg_error_text(const char *prefix, Str e, Alloc *a) {
    Int plen = (Int)strlen(prefix);
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)(plen + e.len), 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    memcpy(p, prefix, (size_t)plen);
    if (e.len > 0)
        memcpy(p + plen, e.p, (size_t)e.len);
    return str_from_bytes(p, plen + e.len);
}

Error jpeg_format_error_as_error(JpegFormatError e, Alloc *a) {
    return jpeg_error_box(&jpeg_format_vt, JPEG_FORMAT_PREFIX, e, a);
}

Error jpeg_unsupported_error_as_error(JpegUnsupportedError e, Alloc *a) {
    return jpeg_error_box(&jpeg_unsupported_vt, JPEG_UNSUPPORTED_PREFIX, e, a);
}

static Error jpeg_format_clone(const void *self, Alloc *a) {
    return jpeg_format_error_as_error(((const JpegErrorBox *)self)->s, a);
}

static Error jpeg_unsupported_clone(const void *self, Alloc *a) {
    return jpeg_unsupported_error_as_error(((const JpegErrorBox *)self)->s, a);
}

Str jpeg_format_error_error(JpegFormatError e, Alloc *a) {
    return jpeg_error_text(JPEG_FORMAT_PREFIX, e, a);
}

Str jpeg_unsupported_error_error(JpegUnsupportedError e, Alloc *a) {
    return jpeg_error_text(JPEG_UNSUPPORTED_PREFIX, e, a);
}

/* Every error the package makes has fixed text, so they all live in read
 * only memory. text is pasted onto the prefix, so it cannot have parentheses
 * around it. */
/* NOLINTBEGIN(bugprone-macro-parentheses) */
#define JPEG_ERROR(name, vt, prefix, text)                                             \
    static const JpegErrorBox name##__box = {                                          \
        {(const Byte *)prefix text + sizeof prefix - 1, sizeof text - 1},              \
        {(const Byte *)prefix text, sizeof prefix text - 1}};                          \
    static const Error name = {&vt, &name##__box}
#define JPEG_FORMAT(name, text)                                                        \
    JPEG_ERROR(name, jpeg_format_vt, JPEG_FORMAT_PREFIX, text)
#define JPEG_UNSUPPORTED(name, text)                                                   \
    JPEG_ERROR(name, jpeg_unsupported_vt, JPEG_UNSUPPORTED_PREFIX, text)
/* NOLINTEND(bugprone-macro-parentheses) */

JPEG_FORMAT(jpeg_err_short_huffman, "short Huffman data");
JPEG_FORMAT(jpeg_err_dht_length, "DHT has wrong length");
JPEG_FORMAT(jpeg_err_tc, "bad Tc value");
JPEG_FORMAT(jpeg_err_th, "bad Th value");
JPEG_FORMAT(jpeg_err_huffman_zero, "Huffman table has zero length");
JPEG_FORMAT(jpeg_err_huffman_excessive, "Huffman table has excessive length");
JPEG_FORMAT(jpeg_err_huffman_uninit, "uninitialized Huffman table");
JPEG_FORMAT(jpeg_err_huffman_code, "bad Huffman code");
JPEG_FORMAT(jpeg_err_missing_ff00, "missing 0xff00 sequence");
JPEG_FORMAT(jpeg_err_multiple_sof, "multiple SOF markers");
JPEG_FORMAT(jpeg_err_sof_length, "SOF has wrong length");
JPEG_FORMAT(jpeg_err_repeated_component, "repeated component identifier");
JPEG_FORMAT(jpeg_err_tq, "bad Tq value");
JPEG_FORMAT(jpeg_err_sampling_format, "luma/chroma subsampling ratio");
JPEG_FORMAT(jpeg_err_pq, "bad Pq value");
JPEG_FORMAT(jpeg_err_dqt_length, "DQT has wrong length");
JPEG_FORMAT(jpeg_err_dri_length, "DRI has wrong length");
JPEG_FORMAT(jpeg_err_missing_soi, "missing SOI marker");
JPEG_FORMAT(jpeg_err_short_segment, "short segment length");
JPEG_FORMAT(jpeg_err_unknown_marker_format, "unknown marker");
JPEG_FORMAT(jpeg_err_missing_sos, "missing SOS marker");
JPEG_FORMAT(jpeg_err_missing_sof, "missing SOF marker");
JPEG_FORMAT(jpeg_err_sos_length, "SOS has wrong length");
JPEG_FORMAT(jpeg_err_sos_inconsistent,
            "SOS length inconsistent with number of components");
JPEG_FORMAT(jpeg_err_unknown_selector, "unknown component selector");
JPEG_FORMAT(jpeg_err_repeated_selector, "repeated component selector");
JPEG_FORMAT(jpeg_err_td, "bad Td value");
JPEG_FORMAT(jpeg_err_ta, "bad Ta value");
JPEG_FORMAT(jpeg_err_total_sampling, "total sampling factors too large");
JPEG_FORMAT(jpeg_err_spectral, "bad spectral selection bounds");
JPEG_FORMAT(jpeg_err_progressive_ac,
            "progressive AC coefficients for more than one component");
JPEG_FORMAT(jpeg_err_successive, "bad successive approximation values");
JPEG_FORMAT(jpeg_err_unexpected_huffman, "unexpected Huffman code");
JPEG_FORMAT(jpeg_err_too_many_coefficients, "too many coefficients");
JPEG_FORMAT(jpeg_err_bad_rst, "bad RST marker");
JPEG_UNSUPPORTED(jpeg_err_sampling, "luma/chroma subsampling ratio");
JPEG_UNSUPPORTED(jpeg_err_components, "number of components");
JPEG_UNSUPPORTED(jpeg_err_precision, "precision");
JPEG_UNSUPPORTED(jpeg_err_unknown_marker, "unknown marker");
JPEG_UNSUPPORTED(jpeg_err_no_adobe, "unknown color model: 4-component JPEG doesn't "
                                    "have Adobe APP14 metadata");
JPEG_UNSUPPORTED(jpeg_err_excessive_dc, "excessive DC component");
JPEG_UNSUPPORTED(jpeg_err_too_many_components, "too many components");

static const Str jpeg_err_too_large__text = {
    (const Byte *)"jpeg: image is too large to encode",
    (Int)sizeof("jpeg: image is too large to encode") - 1};
static const Error jpeg_err_too_large = {&burrow_sentinel_error_vt,
                                         &jpeg_err_too_large__text};

static bool jpeg_is(Error err, Error target) {
    return err.vt == target.vt && err.data == target.data;
}

/* -------------------------------------------------------------- arithmetic */

/* int32 arithmetic that wraps as Go's does. Right shifts of negative values
 * are arithmetic on every compiler burrow supports. */
static inline int32_t jpeg_add(int32_t a, int32_t b) {
    return (int32_t)((uint32_t)a + (uint32_t)b);
}

static inline int32_t jpeg_sub(int32_t a, int32_t b) {
    return (int32_t)((uint32_t)a - (uint32_t)b);
}

static inline int32_t jpeg_mul(int32_t a, int32_t b) {
    return (int32_t)((uint32_t)a * (uint32_t)b);
}

static inline int32_t jpeg_shl(int32_t a, unsigned n) {
    return (int32_t)((uint32_t)a << n);
}

/* --------------------------------------------------------------------- dct */

enum { JPEG_BLOCK_SIZE = 8 * 8 };

/* A block is an 8x8 input to a 2D DCT (either the FDCT or IDCT). The input
 * is actually only 8x8 uint8 values, and the outputs are 8x8 int16, but it
 * is convenient to use int32s for intermediate storage. */
typedef struct JpegBlock {
    int32_t v[JPEG_BLOCK_SIZE];
} JpegBlock;

/* The DCT is Christoph Loeffler, Adriaan Lightenberg and George S. Mostchytz,
 * "Practical Fast 1-D DCT Algorithms with 11 Multiplications", ICASSP 1989,
 * as Go writes it. dct.go explains the algorithm and tracks the fixed point
 * precision of every step, and the order of the steps here is the same.
 *
 * The constants are 60-bit fixed point, and jpeg_c rounds one to bits
 * bits. */
#define JPEG_COS1 UINT64_C(1130768441178740757)         /* fix cos 1*pi/16 */
#define JPEG_SIN1 UINT64_C(224923827593068887)          /* fix sin 1*pi/16 */
#define JPEG_COS3 UINT64_C(958619196450722178)          /* fix cos 3*pi/16 */
#define JPEG_SIN3 UINT64_C(640528868967736374)          /* fix sin 3*pi/16 */
#define JPEG_SQRT2 UINT64_C(1630477228166597777)        /* fix sqrt 2 */
#define JPEG_SQRT2_COS6 UINT64_C(623956622067911264)    /* fix (sqrt 2)*cos 6*pi/16 */
#define JPEG_SQRT2_SIN6 UINT64_C(1506364539328854985)   /* fix (sqrt 2)*sin 6*pi/16 */
#define JPEG_SQRT2INV UINT64_C(815238614083298888)      /* fix 1/sqrt 2 */
#define JPEG_SQRT2INV_COS6 UINT64_C(311978311033955632) /* fix (1/sqrt 2)*cos 6*pi/16 */
#define JPEG_SQRT2INV_SIN6 UINT64_C(753182269664427492) /* fix (1/sqrt 2)*sin 6*pi/16 */

static inline int32_t jpeg_c(uint64_t x, unsigned bits) {
    return (int32_t)((x + (UINT64_C(1) << (59 - bits))) >> (60 - bits));
}

/* dctBox: a 3-multiply, 3-add rotation and scaling. Given x0, x1, k*cos θ and
 * k*sin θ it gives the rotated and scaled coordinates. */
static inline void jpeg_dct_box(int32_t x0, int32_t x1, int32_t kcos, int32_t ksin,
                                int32_t *y0, int32_t *y1) {
    int32_t ksum = jpeg_mul(kcos, jpeg_add(x0, x1));
    *y0 = jpeg_add(ksum, jpeg_mul(jpeg_sub(ksin, kcos), x1));
    *y1 = jpeg_sub(ksum, jpeg_mul(jpeg_add(kcos, ksin), x0));
}

/* A butterfly, x0, x1 = x0+x1, x0-x1. */
#define JPEG_FLY(x0, x1)                                                               \
    do {                                                                               \
        int32_t fly_t = (x0);                                                          \
        (x0) = jpeg_add(fly_t, (x1));                                                  \
        (x1) = jpeg_sub(fly_t, (x1));                                                  \
    } while (0)

/* fdctCols: the 1D DCT on the columns of b. Inputs are UQ8.0 in [0,255] but
 * interpreted as [-128,127]. Outputs are Q10.18. */
static void jpeg_fdct_cols(JpegBlock *blk) {
    int32_t *b = blk->v;
    for (int i = 0; i < 8; i++) {
        int32_t x0 = b[0 * 8 + i], x1 = b[1 * 8 + i], x2 = b[2 * 8 + i];
        int32_t x3 = b[3 * 8 + i], x4 = b[4 * 8 + i], x5 = b[5 * 8 + i];
        int32_t x6 = b[6 * 8 + i], x7 = b[7 * 8 + i];

        /* Stage 1: four butterflies. */
        JPEG_FLY(x0, x7);
        JPEG_FLY(x1, x6);
        JPEG_FLY(x2, x5);
        JPEG_FLY(x3, x4);

        /* Stage 2: two boxes and two butterflies. */
        jpeg_dct_box(x4, x7, jpeg_c(JPEG_COS3, 18), jpeg_c(JPEG_SIN3, 18), &x4, &x7);
        jpeg_dct_box(x5, x6, jpeg_c(JPEG_COS1, 18), jpeg_c(JPEG_SIN1, 18), &x5, &x6);
        JPEG_FLY(x0, x3);
        JPEG_FLY(x1, x2);

        /* Stage 3: one box and three butterflies. */
        jpeg_dct_box(x2, x3, jpeg_c(JPEG_SQRT2_COS6, 18), jpeg_c(JPEG_SQRT2_SIN6, 18),
                     &x2, &x3);
        JPEG_FLY(x0, x1);

        /* Store x0, x1, x2, x3 to their permuted targets. Subtracting 128*8
         * here is the same as subtracting 128 from every input first. */
        b[0 * 8 + i] = jpeg_shl(x0 - 128 * 8, 18);
        b[4 * 8 + i] = jpeg_shl(x1, 18);
        b[2 * 8 + i] = x2;
        b[6 * 8 + i] = x3;

        JPEG_FLY(x4, x6);
        JPEG_FLY(x7, x5);

        /* Stage 4: two √2 scalings and one butterfly. */
        x5 = jpeg_mul(x5 >> 12, jpeg_c(JPEG_SQRT2, 12));
        x6 = jpeg_mul(x6 >> 12, jpeg_c(JPEG_SQRT2, 12));
        JPEG_FLY(x7, x4);

        /* Store x4 x5 x6 x7 to their permuted targets. */
        b[1 * 8 + i] = x7;
        b[3 * 8 + i] = x5;
        b[5 * 8 + i] = x6;
        b[7 * 8 + i] = x4;
    }
}

/* fdctRows: the 1D DCT on the rows of b. Inputs are Q10.18, outputs Q13.0. */
static void jpeg_fdct_rows(JpegBlock *blk) {
    for (int i = 0; i < 8; i++) {
        int32_t *x = blk->v + (ptrdiff_t)8 * i;
        int32_t x0 = x[0], x1 = x[1], x2 = x[2], x3 = x[3];
        int32_t x4 = x[4], x5 = x[5], x6 = x[6], x7 = x[7];

        /* Stage 1: four butterflies. */
        JPEG_FLY(x0, x7);
        JPEG_FLY(x1, x6);
        JPEG_FLY(x2, x5);
        JPEG_FLY(x3, x4);

        /* Stage 2: two boxes and two butterflies. */
        jpeg_dct_box(x4 >> 14, x7 >> 14, jpeg_c(JPEG_COS3, 14), jpeg_c(JPEG_SIN3, 14),
                     &x4, &x7);
        jpeg_dct_box(x5 >> 14, x6 >> 14, jpeg_c(JPEG_COS1, 14), jpeg_c(JPEG_SIN1, 14),
                     &x5, &x6);
        JPEG_FLY(x0, x3);
        JPEG_FLY(x1, x2);

        /* Stage 3: one box and three butterflies. */
        jpeg_dct_box(x2 >> 14, x3 >> 14, jpeg_c(JPEG_SQRT2_COS6, 14),
                     jpeg_c(JPEG_SQRT2_SIN6, 14), &x2, &x3);
        JPEG_FLY(x0, x1);
        JPEG_FLY(x4, x6);
        JPEG_FLY(x7, x5);

        /* Stage 4: two √2 scalings and one butterfly. */
        x5 = jpeg_mul(x5 >> 14, jpeg_c(JPEG_SQRT2, 14));
        x6 = jpeg_mul(x6 >> 14, jpeg_c(JPEG_SQRT2, 14));
        JPEG_FLY(x7, x4);

        /* Cut from Q13.18 to Q13.0. */
        x[0] = jpeg_add(x0, 1 << 17) >> 18;
        x[1] = jpeg_add(x7, 1 << 17) >> 18;
        x[2] = jpeg_add(x2, 1 << 17) >> 18;
        x[3] = jpeg_add(x5, 1 << 17) >> 18;
        x[4] = jpeg_add(x1, 1 << 17) >> 18;
        x[5] = jpeg_add(x6, 1 << 17) >> 18;
        x[6] = jpeg_add(x3, 1 << 17) >> 18;
        x[7] = jpeg_add(x4, 1 << 17) >> 18;
    }
}

/* fdct: the forward DCT. Inputs are UQ8.0, outputs Q13.0. */
static void jpeg_fdct(JpegBlock *b) {
    jpeg_fdct_cols(b);
    jpeg_fdct_rows(b);
}

/* idctRows: the 1D IDCT on the rows of b, the FDCT run backward. Inputs are
 * UQ8.0, outputs Q9.20. */
static void jpeg_idct_rows(JpegBlock *blk) {
    for (int i = 0; i < 8; i++) {
        int32_t *x = blk->v + (ptrdiff_t)8 * i;
        int32_t x0 = x[0], x7 = x[1], x2 = x[2], x5 = x[3];
        int32_t x1 = x[4], x6 = x[5], x3 = x[6], x4 = x[7];

        /* Stages 4, 3, 2: x0, x1, x2, x3. */
        x0 = jpeg_shl(x0, 17);
        x1 = jpeg_shl(x1, 17);
        JPEG_FLY(x0, x1);
        jpeg_dct_box(x2, x3, jpeg_c(JPEG_SQRT2INV_COS6, 18),
                     -jpeg_c(JPEG_SQRT2INV_SIN6, 18), &x2, &x3);
        JPEG_FLY(x1, x2);
        JPEG_FLY(x0, x3);

        /* Stages 4, 3, 2: x4, x5, x6, x7. */
        x4 = jpeg_shl(x4, 7);
        x7 = jpeg_shl(x7, 7);
        JPEG_FLY(x7, x4);
        x6 = jpeg_mul(x6, jpeg_c(JPEG_SQRT2INV, 8));
        x5 = jpeg_mul(x5, jpeg_c(JPEG_SQRT2INV, 8));
        JPEG_FLY(x7, x5);
        JPEG_FLY(x4, x6);
        jpeg_dct_box(x4 >> 2, x7 >> 2, jpeg_c(JPEG_COS3, 12), -jpeg_c(JPEG_SIN3, 12),
                     &x4, &x7);
        jpeg_dct_box(x5 >> 2, x6 >> 2, jpeg_c(JPEG_COS1, 12), -jpeg_c(JPEG_SIN1, 12),
                     &x5, &x6);

        /* Stage 1. */
        JPEG_FLY(x0, x7);
        JPEG_FLY(x1, x6);
        JPEG_FLY(x2, x5);
        JPEG_FLY(x3, x4);

        x[0] = x0;
        x[1] = x1;
        x[2] = x2;
        x[3] = x3;
        x[4] = x4;
        x[5] = x5;
        x[6] = x6;
        x[7] = x7;
    }
}

/* idctCols: the 1D IDCT on the columns of b. Inputs are Q9.20, outputs
 * Q10.3, which is the IDCT*8. */
static void jpeg_idct_cols(JpegBlock *blk) {
    int32_t *b = blk->v;
    for (int i = 0; i < 8; i++) {
        int32_t x0 = b[0 * 8 + i], x7 = b[1 * 8 + i], x2 = b[2 * 8 + i];
        int32_t x5 = b[3 * 8 + i], x1 = b[4 * 8 + i], x6 = b[5 * 8 + i];
        int32_t x3 = b[6 * 8 + i], x4 = b[7 * 8 + i];

        /* Start by adding 0.5 to x0 (the incoming DC signal). The butterflies
         * add it to all the other values, and then the final shifts round
         * properly. */
        x0 = jpeg_add(x0, 1 << 19);

        /* Stages 4, 3, 2: x0, x1, x2, x3. */
        int32_t t = x0;
        x0 = jpeg_add(t, x1) >> 2;
        x1 = jpeg_sub(t, x1) >> 2;
        jpeg_dct_box(x2 >> 13, x3 >> 13, jpeg_c(JPEG_SQRT2INV_COS6, 12),
                     -jpeg_c(JPEG_SQRT2INV_SIN6, 12), &x2, &x3);
        JPEG_FLY(x1, x2);
        JPEG_FLY(x0, x3);

        /* Stages 4, 3, 2: x4, x5, x6, x7. */
        JPEG_FLY(x7, x4);
        x5 = jpeg_mul(x5 >> 13, jpeg_c(JPEG_SQRT2INV, 14));
        x6 = jpeg_mul(x6 >> 13, jpeg_c(JPEG_SQRT2INV, 14));
        JPEG_FLY(x7, x5);
        JPEG_FLY(x4, x6);
        jpeg_dct_box(x4 >> 14, x7 >> 14, jpeg_c(JPEG_COS3, 12), -jpeg_c(JPEG_SIN3, 12),
                     &x4, &x7);
        jpeg_dct_box(x5 >> 14, x6 >> 14, jpeg_c(JPEG_COS1, 12), -jpeg_c(JPEG_SIN1, 12),
                     &x5, &x6);

        JPEG_FLY(x0, x7);
        JPEG_FLY(x1, x6);
        JPEG_FLY(x2, x5);
        JPEG_FLY(x3, x4);

        b[0 * 8 + i] = x0 >> 18;
        b[1 * 8 + i] = x1 >> 18;
        b[2 * 8 + i] = x2 >> 18;
        b[3 * 8 + i] = x3 >> 18;
        b[4 * 8 + i] = x4 >> 18;
        b[5 * 8 + i] = x5 >> 18;
        b[6 * 8 + i] = x6 >> 18;
        b[7 * 8 + i] = x7 >> 18;
    }
}

/* idct: the inverse DCT, a 1D IDCT on rows followed by columns. */
static void jpeg_idct(JpegBlock *b) {
    jpeg_idct_rows(b);
    jpeg_idct_cols(b);
}

/* ----------------------------------------------------------------- decoder */

enum {
    JPEG_DC_TABLE = 0,
    JPEG_AC_TABLE = 1,
    JPEG_MAX_TC = 1,
    JPEG_MAX_TH = 3,
    JPEG_MAX_TQ = 3,
    JPEG_MAX_COMPONENTS = 4,

    JPEG_SOF0 = 0xc0, /* Start Of Frame (Baseline Sequential). */
    JPEG_SOF1 = 0xc1, /* Start Of Frame (Extended Sequential). */
    JPEG_SOF2 = 0xc2, /* Start Of Frame (Progressive). */
    JPEG_DHT = 0xc4,  /* Define Huffman Table. */
    JPEG_RST0 = 0xd0, /* ReSTart (0). */
    JPEG_RST7 = 0xd7, /* ReSTart (7). */
    JPEG_SOI = 0xd8,  /* Start Of Image. */
    JPEG_EOI = 0xd9,  /* End Of Image. */
    JPEG_SOS = 0xda,  /* Start Of Scan. */
    JPEG_DQT = 0xdb,  /* Define Quantization Table. */
    JPEG_DRI = 0xdd,  /* Define Restart Interval. */
    JPEG_COM = 0xfe,  /* COMment. */
    /* "APPlication specific" markers aren't part of the JPEG spec per se, but
     * in practice, their use is described at
     * https://www.sno.phy.queensu.ca/~phil/exiftool/TagNames/JPEG.html */
    JPEG_APP0 = 0xe0,
    JPEG_APP14 = 0xee,
    JPEG_APP15 = 0xef,

    JPEG_ADOBE_TRANSFORM_UNKNOWN = 0,
    JPEG_ADOBE_TRANSFORM_Y_CB_CR = 1,
    JPEG_ADOBE_TRANSFORM_Y_CB_CR_K = 2,

    JPEG_MAX_CODE_LENGTH = 16,
    JPEG_MAX_N_CODES = 256,
    JPEG_LUT_SIZE = 8,
};

/* unzig maps from the zig-zag ordering to the natural ordering. For example,
 * unzig[3] is the column and row of the fourth element in zig-zag order. The
 * value is 16, which means first column (16%8 == 0) and third row
 * (16/8 == 2). */
static const uint8_t jpeg_unzig[JPEG_BLOCK_SIZE] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

/* Component specification, specified in section B.2.2. */
typedef struct JpegComponent {
    Int h;        /* Horizontal sampling factor. */
    Int v;        /* Vertical sampling factor. */
    uint8_t c;    /* Component identifier. */
    uint8_t tq;   /* Quantization table destination selector. */
    Int expand_h; /* Horizontal expansion factor for non-standard subsampling. */
    Int expand_v; /* Vertical expansion factor for non-standard subsampling. */
} JpegComponent;

typedef struct JpegHuffman {
    /* length is the number of codes in the tree. */
    int32_t n_codes;
    /* lut is the look-up table for the next lutSize bits in the bit-stream.
     * The high 8 bits of the uint16 are the encoded value. The low 8 bits
     * are 1 plus the code length, or 0 if the value is too large to fit in
     * lutSize bits. */
    uint16_t lut[1 << JPEG_LUT_SIZE];
    /* vals are the decoded values, sorted by their encoding. */
    uint8_t vals[JPEG_MAX_N_CODES];
    /* minCodes[i] is the minimum code of length i, or -1 if there are no
     * codes of that length. */
    int32_t min_codes[JPEG_MAX_CODE_LENGTH];
    /* maxCodes[i] is the maximum code of length i, or -1 if there are no
     * codes of that length. */
    int32_t max_codes[JPEG_MAX_CODE_LENGTH];
    /* valsIndices[i] is the index into vals of minCodes[i]. */
    int32_t vals_indices[JPEG_MAX_CODE_LENGTH];
} JpegHuffman;

/* bits holds the unprocessed bits that have been taken from the byte-stream.
 * The n least significant bits of a form the unread bits, to be read in MSB
 * to LSB order. */
typedef struct JpegBits {
    uint32_t a; /* accumulator. */
    uint32_t m; /* mask. m==1<<(n-1) when n>0, with m==0 when n==0. */
    int32_t n;  /* the number of unread bits in a. */
} JpegBits;

typedef struct JpegDecoder {
    Alloc *a;
    IoReader r;
    JpegBits bits;
    /* bytes is a byte buffer, similar to a bufio.Reader, except that it has
     * to be able to unread more than 1 byte, due to byte stuffing. Byte
     * stuffing is specified in section F.1.2.3. */
    struct {
        /* buf[i:j] are the buffered bytes read from the underlying io.Reader
         * that haven't yet been passed further on. */
        Byte buf[4096];
        Int i, j;
        /* nUnreadable is the number of bytes to back up i after overshooting.
         * It can be 0, 1 or 2. */
        Int n_unreadable;
    } bytes;
    Int width, height;

    ImageGray *img1;
    ImageYCbCr *img3;
    Byte *black_pix;
    Int black_len;
    Int black_stride;

    /* For non-standard subsampling ratios (flex mode). True if using
     * non-standard subsampling that requires manual pixel expansion. */
    bool flex;
    /* Maximum horizontal and vertical sampling factors across all
     * components. */
    Int max_h, max_v;

    Int ri; /* Restart Interval. */
    Int n_comp;

    /* As per section 4.5, there are four modes of operation (selected by the
     * SOF? markers): sequential DCT, progressive DCT, lossless and
     * hierarchical, although this implementation does not support the latter
     * two non-DCT modes. Sequential DCT is further split into baseline and
     * extended, as per section 4.11. */
    bool baseline;
    bool progressive;

    bool jfif;
    bool adobe_transform_valid;
    uint8_t adobe_transform;
    uint16_t eob_run; /* End-of-Band run, specified in section G.1.2.2. */

    JpegComponent comp[JPEG_MAX_COMPONENTS];
    /* Saved state between progressive-mode scans, and each one's length. */
    JpegBlock *prog_coeffs[JPEG_MAX_COMPONENTS];
    Int prog_len[JPEG_MAX_COMPONENTS];
    JpegHuffman huff[JPEG_MAX_TC + 1][JPEG_MAX_TH + 1];
    JpegBlock quant[JPEG_MAX_TQ + 1]; /* Quantization tables, in zig-zag order. */
    Byte tmp[2 * JPEG_BLOCK_SIZE];
} JpegDecoder;

/* Everything the decoder allocated but the image it is about to return,
 * which keep says. */
static void jpeg_decoder_free(JpegDecoder *d, Image keep) {
    if (d->img1 != NULL && d->img1 != keep.data)
        image_gray_free(d->img1, d->a);
    if (d->img3 != NULL && d->img3 != keep.data)
        image_y_cb_cr_free(d->img3, d->a);
    if (d->black_pix != NULL)
        mem_free(d->a, d->black_pix, (size_t)d->black_len, 1);
    for (int i = 0; i < JPEG_MAX_COMPONENTS; i++)
        if (d->prog_coeffs[i] != NULL)
            mem_free(d->a, d->prog_coeffs[i],
                     (size_t)d->prog_len[i] * sizeof(JpegBlock), _Alignof(JpegBlock));
    mem_free(d->a, d, sizeof *d, _Alignof(JpegDecoder));
}

/* fill fills up the d.bytes.buf buffer from the underlying io.Reader. It
 * should only be called when there are no unread bytes in d.bytes. */
static Error jpeg_fill(JpegDecoder *d) {
    /* Move the last 2 bytes to the start of the buffer, in case we need to
     * call unreadByteStuffedByte. */
    if (d->bytes.j > 2) {
        d->bytes.buf[0] = d->bytes.buf[d->bytes.j - 2];
        d->bytes.buf[1] = d->bytes.buf[d->bytes.j - 1];
        d->bytes.i = 2;
        d->bytes.j = 2;
    }
    /* Fill in the rest of the buffer. */
    Int room = (Int)sizeof d->bytes.buf - d->bytes.j;
    Error err = BURROW_NO_ERROR;
    Int n = d->r.vt->read(
        d->r.data, slice_from(d->bytes.buf + d->bytes.j, room, room, TYPE_BYTE), &err);
    d->bytes.j += n;
    if (n > 0)
        return BURROW_NO_ERROR;
    if (jpeg_is(err, io_eof))
        err = io_err_unexpected_eof;
    return err;
}

/* unreadByteStuffedByte undoes the most recent readByteStuffedByte call,
 * giving a byte of data back from d.bits to d.bytes. The Huffman look-up
 * table requires at least 8 bits for look-up, which means that Huffman
 * decoding can sometimes overshoot and read one or two too many bytes.
 * Two-byte overshoot can happen when expecting to read a 0xff 0x00
 * byte-stuffed byte. */
static void jpeg_unread_byte_stuffed_byte(JpegDecoder *d) {
    d->bytes.i -= d->bytes.n_unreadable;
    d->bytes.n_unreadable = 0;
    if (d->bits.n >= 8) {
        d->bits.a >>= 8;
        d->bits.n -= 8;
        d->bits.m >>= 8;
    }
}

/* readByte returns the next byte, whether buffered or not buffered. It does
 * not care about byte stuffing. */
static Error jpeg_read_byte(JpegDecoder *d, Byte *x) {
    while (d->bytes.i == d->bytes.j) {
        Error err = jpeg_fill(d);
        if (BURROW_FAILED(err)) {
            *x = 0;
            return err;
        }
    }
    *x = d->bytes.buf[d->bytes.i];
    d->bytes.i++;
    d->bytes.n_unreadable = 0;
    return BURROW_NO_ERROR;
}

/* readByteStuffedByte is like readByte but is for byte-stuffed Huffman
 * data. */
static Error jpeg_read_byte_stuffed_byte(JpegDecoder *d, Byte *x) {
    /* Take the fast path if d.bytes.buf contains at least two bytes. */
    if (d->bytes.i + 2 <= d->bytes.j) {
        *x = d->bytes.buf[d->bytes.i];
        d->bytes.i++;
        d->bytes.n_unreadable = 1;
        if (*x != 0xff)
            return BURROW_NO_ERROR;
        if (d->bytes.buf[d->bytes.i] != 0x00) {
            *x = 0;
            return jpeg_err_missing_ff00;
        }
        d->bytes.i++;
        d->bytes.n_unreadable = 2;
        return BURROW_NO_ERROR;
    }

    d->bytes.n_unreadable = 0;

    Error err = jpeg_read_byte(d, x);
    if (BURROW_FAILED(err))
        return err;
    d->bytes.n_unreadable = 1;
    if (*x != 0xff)
        return BURROW_NO_ERROR;

    err = jpeg_read_byte(d, x);
    if (BURROW_FAILED(err))
        return err;
    d->bytes.n_unreadable = 2;
    if (*x != 0x00) {
        *x = 0;
        return jpeg_err_missing_ff00;
    }
    *x = 0xff;
    return BURROW_NO_ERROR;
}

/* Unread the overshot bytes, if any. */
static void jpeg_unread_overshoot(JpegDecoder *d) {
    if (d->bytes.n_unreadable != 0) {
        if (d->bits.n >= 8)
            jpeg_unread_byte_stuffed_byte(d);
        d->bytes.n_unreadable = 0;
    }
}

/* readFull reads exactly n bytes into p. It does not care about byte
 * stuffing. */
static Error jpeg_read_full(JpegDecoder *d, Byte *p, Int n) {
    jpeg_unread_overshoot(d);
    for (;;) {
        Int m = d->bytes.j - d->bytes.i;
        if (m > n)
            m = n;
        if (m > 0)
            memcpy(p, d->bytes.buf + d->bytes.i, (size_t)m);
        p += m;
        n -= m;
        d->bytes.i += m;
        if (n == 0)
            break;
        Error err = jpeg_fill(d);
        if (BURROW_FAILED(err))
            return err;
    }
    return BURROW_NO_ERROR;
}

/* ignore ignores the next n bytes. */
static Error jpeg_ignore(JpegDecoder *d, Int n) {
    jpeg_unread_overshoot(d);
    for (;;) {
        Int m = d->bytes.j - d->bytes.i;
        if (m > n)
            m = n;
        d->bytes.i += m;
        n -= m;
        if (n == 0)
            break;
        Error err = jpeg_fill(d);
        if (BURROW_FAILED(err))
            return err;
    }
    return BURROW_NO_ERROR;
}

/* ---------------------------------------------------------------- huffman */

/* ensureNBits reads bytes from the byte buffer to ensure that d.bits.n is at
 * least n. For best performance (avoiding function calls inside hot loops),
 * the caller is the one responsible for first checking that d.bits.n < n. */
static Error jpeg_ensure_n_bits(JpegDecoder *d, int32_t n) {
    for (;;) {
        Byte c;
        Error err = jpeg_read_byte_stuffed_byte(d, &c);
        if (BURROW_FAILED(err)) {
            if (jpeg_is(err, io_err_unexpected_eof))
                return jpeg_err_short_huffman;
            return err;
        }
        d->bits.a = d->bits.a << 8 | (uint32_t)c;
        d->bits.n += 8;
        if (d->bits.m == 0)
            d->bits.m = 1 << 7;
        else
            d->bits.m <<= 8;
        if (d->bits.n >= n)
            break;
    }
    return BURROW_NO_ERROR;
}

/* receiveExtend is the composition of RECEIVE and EXTEND, specified in
 * section F.2.2.1. */
static Error jpeg_receive_extend(JpegDecoder *d, uint8_t t, int32_t *out) {
    *out = 0;
    if (d->bits.n < (int32_t)t) {
        Error err = jpeg_ensure_n_bits(d, (int32_t)t);
        if (BURROW_FAILED(err))
            return err;
    }
    d->bits.n -= (int32_t)t;
    d->bits.m >>= t;
    /* With t == 0 Go's arithmetic comes to 0, through shifts C does not
     * allow. */
    if (t == 0)
        return BURROW_NO_ERROR;
    int32_t s = (int32_t)1 << t;
    int32_t x = (int32_t)(d->bits.a >> (uint8_t)d->bits.n) & (s - 1);
    int32_t sign = (x >> (t - 1)) - 1;
    x += sign & (jpeg_shl(-1, t) + 1);
    *out = x;
    return BURROW_NO_ERROR;
}

/* processDHT processes a Define Huffman Table marker, and initializes a
 * huffman struct from its contents. Specified in section B.2.4.2. */
static Error jpeg_process_dht(JpegDecoder *d, Int n) {
    while (n > 0) {
        if (n < 17)
            return jpeg_err_dht_length;
        Error err = jpeg_read_full(d, d->tmp, 17);
        if (BURROW_FAILED(err))
            return err;
        uint8_t tc = d->tmp[0] >> 4;
        if (tc > JPEG_MAX_TC)
            return jpeg_err_tc;
        uint8_t th = d->tmp[0] & 0x0f;
        /* The baseline th <= 1 restriction is specified in table B.5. */
        if (th > JPEG_MAX_TH || (d->baseline && th > 1))
            return jpeg_err_th;
        JpegHuffman *h = &d->huff[tc][th];

        /* Read nCodes and h.vals (and derive h.nCodes). nCodes[i] is the
         * number of codes with code length i. h.nCodes is the total number
         * of codes. */
        h->n_codes = 0;
        int32_t n_codes[JPEG_MAX_CODE_LENGTH];
        for (int i = 0; i < JPEG_MAX_CODE_LENGTH; i++) {
            n_codes[i] = (int32_t)d->tmp[i + 1];
            h->n_codes += n_codes[i];
        }
        if (h->n_codes == 0)
            return jpeg_err_huffman_zero;
        if (h->n_codes > JPEG_MAX_N_CODES)
            return jpeg_err_huffman_excessive;
        n -= (Int)h->n_codes + 17;
        if (n < 0)
            return jpeg_err_dht_length;
        err = jpeg_read_full(d, h->vals, h->n_codes);
        if (BURROW_FAILED(err))
            return err;

        /* Derive the look-up table. */
        memset(h->lut, 0, sizeof h->lut);
        uint32_t x = 0, code = 0;
        for (uint32_t i = 0; i < JPEG_LUT_SIZE; i++) {
            code <<= 1;
            for (int32_t j = 0; j < n_codes[i]; j++) {
                /* The codeLength is 1+i, so shift code by 8-(1+i) to
                 * calculate the high bits for every 8-bit sequence whose
                 * codeLength's high bits matches code. The high 8 bits of
                 * lutValue are the encoded value. The low 8 bits are 1 plus
                 * the codeLength. */
                uint8_t base = (uint8_t)(code << (7 - i));
                uint16_t lut_value = (uint16_t)((uint32_t)h->vals[x] << 8 | (2 + i));
                for (uint32_t k = 0; k < 1U << (7 - i); k++)
                    h->lut[base | k] = lut_value;
                code++;
                x++;
            }
        }

        /* Derive minCodes, maxCodes, and valsIndices. */
        int32_t c = 0, index = 0;
        for (int i = 0; i < JPEG_MAX_CODE_LENGTH; i++) {
            int32_t nc = n_codes[i];
            if (nc == 0) {
                h->min_codes[i] = -1;
                h->max_codes[i] = -1;
                h->vals_indices[i] = -1;
            } else {
                h->min_codes[i] = c;
                h->max_codes[i] = c + nc - 1;
                h->vals_indices[i] = index;
                c += nc;
                index += nc;
            }
            c <<= 1;
        }
    }
    return BURROW_NO_ERROR;
}

/* decodeHuffman returns the next Huffman-coded value from the bit-stream,
 * decoded according to h. */
static Error jpeg_decode_huffman(JpegDecoder *d, const JpegHuffman *h, uint8_t *out) {
    *out = 0;
    if (h->n_codes == 0)
        return jpeg_err_huffman_uninit;

    if (d->bits.n < 8) {
        Error err = jpeg_ensure_n_bits(d, 8);
        if (BURROW_FAILED(err)) {
            if (!jpeg_is(err, jpeg_err_missing_ff00) &&
                !jpeg_is(err, jpeg_err_short_huffman))
                return err;
            /* There are no more bytes of data in this segment, but we may
             * still be able to read the next symbol out of the previously
             * read bits. First, undo the readByte that the ensureNBits call
             * made. */
            if (d->bytes.n_unreadable != 0)
                jpeg_unread_byte_stuffed_byte(d);
            goto slow_path;
        }
    }
    {
        /* ensureNBits left at least 8 bits. */
        uint32_t shift = (uint32_t)(d->bits.n - JPEG_LUT_SIZE);
        /* NOLINTNEXTLINE(clang-analyzer-core.BitwiseShift) */
        uint16_t v = h->lut[(d->bits.a >> shift) & 0xff];
        if (v != 0) {
            uint16_t n = (uint16_t)((v & 0xff) - 1);
            d->bits.n -= (int32_t)n;
            d->bits.m >>= n;
            *out = (uint8_t)(v >> 8);
            return BURROW_NO_ERROR;
        }
    }

slow_path:
    for (int i = 0, code = 0; i < JPEG_MAX_CODE_LENGTH; i++) {
        if (d->bits.n == 0) {
            Error err = jpeg_ensure_n_bits(d, 1);
            if (BURROW_FAILED(err))
                return err;
        }
        if ((d->bits.a & d->bits.m) != 0)
            code |= 1;
        d->bits.n--;
        d->bits.m >>= 1;
        if (code <= h->max_codes[i]) {
            *out = h->vals[h->vals_indices[i] + code - h->min_codes[i]];
            return BURROW_NO_ERROR;
        }
        code <<= 1;
    }
    return jpeg_err_huffman_code;
}

static Error jpeg_decode_bit(JpegDecoder *d, bool *out) {
    *out = false;
    if (d->bits.n == 0) {
        Error err = jpeg_ensure_n_bits(d, 1);
        if (BURROW_FAILED(err))
            return err;
    }
    *out = (d->bits.a & d->bits.m) != 0;
    d->bits.n--;
    d->bits.m >>= 1;
    return BURROW_NO_ERROR;
}

static Error jpeg_decode_bits(JpegDecoder *d, int32_t n, uint32_t *out) {
    *out = 0;
    if (d->bits.n < n) {
        Error err = jpeg_ensure_n_bits(d, n);
        if (BURROW_FAILED(err))
            return err;
    }
    uint32_t ret = d->bits.a >> (uint32_t)(d->bits.n - n);
    ret &= ((uint32_t)1 << (uint32_t)n) - 1;
    d->bits.n -= n;
    d->bits.m >>= (uint32_t)n;
    *out = ret;
    return BURROW_NO_ERROR;
}

/* ---------------------------------------------------------------- markers */

/* Specified in section B.2.2. */
static Error jpeg_process_sof(JpegDecoder *d, Int n) {
    if (d->n_comp != 0)
        return jpeg_err_multiple_sof;
    switch (n) {
    case 6 + 3 * 1: /* Grayscale image. */
        d->n_comp = 1;
        break;
    case 6 + 3 * 3: /* YCbCr or RGB image. */
        d->n_comp = 3;
        break;
    case 6 + 3 * 4: /* YCbCrK or CMYK image. */
        d->n_comp = 4;
        break;
    default:
        return jpeg_err_components;
    }
    Error err = jpeg_read_full(d, d->tmp, n);
    if (BURROW_FAILED(err))
        return err;
    /* We only support 8-bit precision. */
    if (d->tmp[0] != 8)
        return jpeg_err_precision;
    d->height = (Int)d->tmp[1] << 8 | (Int)d->tmp[2];
    d->width = (Int)d->tmp[3] << 8 | (Int)d->tmp[4];
    if ((Int)d->tmp[5] != d->n_comp)
        return jpeg_err_sof_length;

    for (Int i = 0; i < d->n_comp; i++) {
        d->comp[i].c = d->tmp[6 + 3 * i];
        /* Section B.2.2 states that "the value of C_i shall be different from
         * the values of C_1 through C_(i-1)". */
        for (Int j = 0; j < i; j++)
            if (d->comp[i].c == d->comp[j].c)
                return jpeg_err_repeated_component;

        d->comp[i].tq = d->tmp[8 + 3 * i];
        if (d->comp[i].tq > JPEG_MAX_TQ)
            return jpeg_err_tq;

        uint8_t hv = d->tmp[7 + 3 * i];
        Int h = hv >> 4, v = hv & 0x0f;
        if (h < 1 || 4 < h || v < 1 || 4 < v)
            return jpeg_err_sampling_format;
        if (h == 3 || v == 3)
            return jpeg_err_sampling;
        switch (d->n_comp) {
        case 1:
            /* If a JPEG image has only one component, section A.2 says "this
             * data is non-interleaved by definition" and section A.2.2 says
             * "[in this case...] the order of data units within a scan shall
             * be left-to-right and top-to-bottom... regardless of the values
             * of H_1 and V_1". The component's (h, v) is effectively always
             * (1, 1): even if the nominal (h, v) is (2, 1), a 20x5 image is
             * encoded in three 8x8 MCUs, not two 16x8 MCUs. */
            h = 1;
            v = 1;
            break;
        case 3:
            /* For YCbCr images, both the standard subsampling ratios and
             * non-standard ones where components have different sampling
             * factors are supported. The only restriction is that each
             * component's sampling factors must evenly divide the maximum
             * factors (validated after the loop). */
            break;
        case 4:
            /* For 4-component images (either CMYK or YCbCrK), we only
             * support two hv vectors: [0x11 0x11 0x11 0x11] and
             * [0x22 0x11 0x11 0x22]. Those are the only combinations in use,
             * and they keep applyBlack simple. */
            switch (i) {
            case 0:
                if (hv != 0x11 && hv != 0x22)
                    return jpeg_err_sampling;
                break;
            case 1:
            case 2:
                if (hv != 0x11)
                    return jpeg_err_sampling;
                break;
            default:
                if (d->comp[0].h != h || d->comp[0].v != v)
                    return jpeg_err_sampling;
                break;
            }
            break;
        default:
            break;
        }

        if (h > d->max_h)
            d->max_h = h;
        if (v > d->max_v)
            d->max_v = v;
        d->comp[i].h = h;
        d->comp[i].v = v;
    }

    /* For 3-component images, validate that maxH and maxV are evenly
     * divisible by each component's sampling factors. */
    if (d->n_comp == 3)
        for (int i = 0; i < 3; i++)
            if (d->max_h % d->comp[i].h != 0 || d->max_v % d->comp[i].v != 0)
                return jpeg_err_sampling;

    /* Compute expansion factors for each component. */
    for (Int i = 0; i < d->n_comp; i++) {
        d->comp[i].expand_h = d->max_h / d->comp[i].h;
        d->comp[i].expand_v = d->max_v / d->comp[i].v;
    }
    return BURROW_NO_ERROR;
}

/* Specified in section B.2.4.1. */
static Error jpeg_process_dqt(JpegDecoder *d, Int n) {
    while (n > 0) {
        n--;
        Byte x;
        Error err = jpeg_read_byte(d, &x);
        if (BURROW_FAILED(err))
            return err;
        uint8_t tq = x & 0x0f;
        if (tq > JPEG_MAX_TQ)
            return jpeg_err_tq;
        switch (x >> 4) {
        case 0:
            if (n < JPEG_BLOCK_SIZE)
                goto done;
            n -= JPEG_BLOCK_SIZE;
            err = jpeg_read_full(d, d->tmp, JPEG_BLOCK_SIZE);
            if (BURROW_FAILED(err))
                return err;
            for (int i = 0; i < JPEG_BLOCK_SIZE; i++)
                d->quant[tq].v[i] = (int32_t)d->tmp[i];
            break;
        case 1:
            if (n < (Int)2 * JPEG_BLOCK_SIZE)
                goto done;
            n -= (Int)2 * JPEG_BLOCK_SIZE;
            err = jpeg_read_full(d, d->tmp, (Int)2 * JPEG_BLOCK_SIZE);
            if (BURROW_FAILED(err))
                return err;
            for (Int i = 0; i < JPEG_BLOCK_SIZE; i++)
                d->quant[tq].v[i] =
                    (int32_t)d->tmp[2 * i] << 8 | (int32_t)d->tmp[2 * i + 1];
            break;
        default:
            return jpeg_err_pq;
        }
    }
done:
    if (n != 0)
        return jpeg_err_dqt_length;
    return BURROW_NO_ERROR;
}

/* Specified in section B.2.4.4. */
static Error jpeg_process_dri(JpegDecoder *d, Int n) {
    if (n != 2)
        return jpeg_err_dri_length;
    Error err = jpeg_read_full(d, d->tmp, 2);
    if (BURROW_FAILED(err))
        return err;
    d->ri = (Int)d->tmp[0] << 8 | (Int)d->tmp[1];
    return BURROW_NO_ERROR;
}

static Error jpeg_process_app0(JpegDecoder *d, Int n) {
    if (n < 5)
        return jpeg_ignore(d, n);
    Error err = jpeg_read_full(d, d->tmp, 5);
    if (BURROW_FAILED(err))
        return err;
    n -= 5;
    d->jfif = memcmp(d->tmp, "JFIF\0", 5) == 0;
    if (n > 0)
        return jpeg_ignore(d, n);
    return BURROW_NO_ERROR;
}

static Error jpeg_process_app14(JpegDecoder *d, Int n) {
    if (n < 12)
        return jpeg_ignore(d, n);
    Error err = jpeg_read_full(d, d->tmp, 12);
    if (BURROW_FAILED(err))
        return err;
    n -= 12;
    if (memcmp(d->tmp, "Adobe", 5) == 0) {
        d->adobe_transform_valid = true;
        d->adobe_transform = d->tmp[11];
    }
    if (n > 0)
        return jpeg_ignore(d, n);
    return BURROW_NO_ERROR;
}

/* ------------------------------------------------------------------- scans */

/* a*b, or -1 when that overflows or either is negative. */
static Int jpeg_mul_size(Int a, Int b) {
    if (a < 0 || b < 0)
        return -1;
    if (a != 0 && b > BURROW_INT_MAX / a)
        return -1;
    return a * b;
}

/* makeImg allocates and initializes the destination image. */
static Error jpeg_make_img(JpegDecoder *d, Int mxx, Int myy) {
    if (d->n_comp == 1) {
        ImageGray *m = image_new_gray(d->a, image_rect(0, 0, 8 * mxx, 8 * myy));
        if (m == NULL)
            return burrow_err_out_of_memory;
        /* The sub image starts where m does, so it keeps the whole buffer and
         * image_gray_free gives it all back. An empty one keeps nothing,
         * like Go's, so the buffer goes back now. */
        ImageRectangle r = image_rect(0, 0, d->width, d->height);
        if (image_rectangle_empty(image_rectangle_intersect(r, m->rect))) {
            image_gray_free(m, d->a);
            m = image_new_gray(d->a, image_rect(0, 0, 0, 0));
            if (m == NULL)
                return burrow_err_out_of_memory;
        } else {
            *m = image_gray_sub_image(m, r);
        }
        d->img1 = m;
        return BURROW_NO_ERROR;
    }

    /* Determine if we need flex mode for non-standard subsampling. Flex mode
     * is needed when:
     *  - Cb and Cr have different sampling factors, or
     *  - The Y component doesn't have the maximum sampling factors, or
     *  - The ratio doesn't match any standard YCbCrSubsampleRatio. */
    ImageYCbCrSubsampleRatio ratio = IMAGE_Y_CB_CR_SUBSAMPLE_RATIO444;
    if (d->comp[1].h != d->comp[2].h || d->comp[1].v != d->comp[2].v ||
        d->max_h != d->comp[0].h || d->max_v != d->comp[0].v) {
        d->flex = true;
    } else {
        Int h_ratio = d->max_h / d->comp[1].h;
        Int v_ratio = d->max_v / d->comp[1].v;
        switch (h_ratio << 4 | v_ratio) {
        case 0x11:
            ratio = IMAGE_Y_CB_CR_SUBSAMPLE_RATIO444;
            break;
        case 0x12:
            ratio = IMAGE_Y_CB_CR_SUBSAMPLE_RATIO440;
            break;
        case 0x21:
            ratio = IMAGE_Y_CB_CR_SUBSAMPLE_RATIO422;
            break;
        case 0x22:
            ratio = IMAGE_Y_CB_CR_SUBSAMPLE_RATIO420;
            break;
        case 0x41:
            ratio = IMAGE_Y_CB_CR_SUBSAMPLE_RATIO411;
            break;
        case 0x42:
            ratio = IMAGE_Y_CB_CR_SUBSAMPLE_RATIO410;
            break;
        default:
            d->flex = true;
            break;
        }
    }

    ImageYCbCr *m = image_new_y_cb_cr(
        d->a, image_rect(0, 0, 8 * d->max_h * mxx, 8 * d->max_v * myy), ratio);
    if (m == NULL)
        return burrow_err_out_of_memory;
    ImageRectangle r = image_rect(0, 0, d->width, d->height);
    if (image_rectangle_empty(image_rectangle_intersect(r, m->rect))) {
        image_y_cb_cr_free(m, d->a);
        m = image_new_y_cb_cr(d->a, image_rect(0, 0, 0, 0), ratio);
        if (m == NULL)
            return burrow_err_out_of_memory;
    } else {
        *m = image_y_cb_cr_sub_image(m, r);
    }
    d->img3 = m;

    if (d->n_comp == 4) {
        Int h3 = d->comp[3].h, v3 = d->comp[3].v;
        Int n = jpeg_mul_size(8 * h3 * mxx, 8 * v3 * myy);
        if (n < 0)
            return burrow_err_out_of_memory;
        /* Go's make gives a non-nil slice even for 0 bytes, and decode
         * goes by whether it is nil, so this always takes at least one. */
        if (n == 0)
            n = 1;
        d->black_pix = (Byte *)mem_alloc(d->a, (size_t)n, 1);
        if (d->black_pix == NULL)
            return burrow_err_out_of_memory;
        d->black_len = n;
        d->black_stride = 8 * h3 * mxx;
    }
    return BURROW_NO_ERROR;
}

/* refineNonZeroes refines non-zero entries of b in zig-zag order. If
 * nz >= 0, the first nz zero entries are skipped over. */
static Error jpeg_refine_non_zeroes(JpegDecoder *d, JpegBlock *b, int32_t zig,
                                    int32_t zig_end, int32_t nz, int32_t delta,
                                    int32_t *out) {
    *out = 0;
    for (; zig <= zig_end; zig++) {
        int u = jpeg_unzig[zig];
        if (b->v[u] == 0) {
            if (nz == 0)
                break;
            nz--;
            continue;
        }
        bool bit;
        Error err = jpeg_decode_bit(d, &bit);
        if (BURROW_FAILED(err))
            return err;
        if (!bit)
            continue;
        if (b->v[u] >= 0)
            b->v[u] = jpeg_add(b->v[u], delta);
        else
            b->v[u] = jpeg_sub(b->v[u], delta);
    }
    *out = zig;
    return BURROW_NO_ERROR;
}

/* refine decodes a successive approximation refinement block, as specified
 * in section G.1.2. */
static Error jpeg_refine(JpegDecoder *d, JpegBlock *b, const JpegHuffman *h,
                         int32_t zig_start, int32_t zig_end, int32_t delta) {
    /* Refining a DC component is trivial. processSOS only lets zigStart be 0
     * when zigEnd is too. */
    if (zig_start == 0) {
        bool bit;
        Error err = jpeg_decode_bit(d, &bit);
        if (BURROW_FAILED(err))
            return err;
        if (bit)
            b->v[0] |= delta;
        return BURROW_NO_ERROR;
    }

    /* Refining AC components is more complicated; see sections G.1.2.2 and
     * G.1.2.3. */
    int32_t zig = zig_start;
    if (d->eob_run == 0) {
        for (; zig <= zig_end; zig++) {
            int32_t z = 0;
            uint8_t value;
            Error err = jpeg_decode_huffman(d, h, &value);
            if (BURROW_FAILED(err))
                return err;
            uint8_t val0 = value >> 4;
            uint8_t val1 = value & 0x0f;

            bool eob = false;
            switch (val1) {
            case 0:
                if (val0 != 0x0f) {
                    d->eob_run = (uint16_t)(1U << val0);
                    if (val0 != 0) {
                        uint32_t bits;
                        err = jpeg_decode_bits(d, (int32_t)val0, &bits);
                        if (BURROW_FAILED(err))
                            return err;
                        d->eob_run |= (uint16_t)bits;
                    }
                    eob = true;
                }
                break;
            case 1: {
                z = delta;
                bool bit;
                err = jpeg_decode_bit(d, &bit);
                if (BURROW_FAILED(err))
                    return err;
                if (!bit)
                    z = -z;
                break;
            }
            default:
                return jpeg_err_unexpected_huffman;
            }
            if (eob)
                break;

            err =
                jpeg_refine_non_zeroes(d, b, zig, zig_end, (int32_t)val0, delta, &zig);
            if (BURROW_FAILED(err))
                return err;
            if (zig > zig_end)
                return jpeg_err_too_many_coefficients;
            if (z != 0)
                b->v[jpeg_unzig[zig]] = z;
        }
    }
    if (d->eob_run > 0) {
        d->eob_run--;
        int32_t ignored;
        Error err = jpeg_refine_non_zeroes(d, b, zig, zig_end, -1, delta, &ignored);
        if (BURROW_FAILED(err))
            return err;
    }
    return BURROW_NO_ERROR;
}

static uint8_t jpeg_clamp(int32_t v) {
    v = jpeg_add(v, 128);
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

/* reconstructBlock dequantizes, performs the inverse DCT and stores the
 * block to the image. */
static Error jpeg_reconstruct_block(JpegDecoder *d, JpegBlock *b, Int bx, Int by,
                                    Int comp_index) {
    const JpegBlock *qt = &d->quant[d->comp[comp_index].tq];
    for (int zig = 0; zig < JPEG_BLOCK_SIZE; zig++)
        b->v[jpeg_unzig[zig]] = jpeg_mul(b->v[jpeg_unzig[zig]], qt->v[zig]);
    jpeg_idct(b);

    Int h = 0, v = 0;
    if (d->flex) {
        /* Flex mode: scale bx and by according to the component's sampling
         * factors. */
        h = d->comp[comp_index].expand_h;
        v = d->comp[comp_index].expand_v;
        bx *= h;
        by *= v;
    }

    Byte *dst;
    Int stride;
    if (d->n_comp == 1) {
        stride = d->img1->stride;
        dst = (Byte *)d->img1->pix.p + 8 * (by * stride + bx);
    } else {
        switch (comp_index) {
        case 0:
            stride = d->img3->y_stride;
            dst = (Byte *)d->img3->y.p + 8 * (by * stride + bx);
            break;
        case 1:
            stride = d->img3->c_stride;
            dst = (Byte *)d->img3->cb.p + 8 * (by * stride + bx);
            break;
        case 2:
            stride = d->img3->c_stride;
            dst = (Byte *)d->img3->cr.p + 8 * (by * stride + bx);
            break;
        case 3:
            stride = d->black_stride;
            dst = d->black_pix + 8 * (by * stride + bx);
            break;
        default:
            return jpeg_err_too_many_components;
        }
    }

    if (d->flex) {
        /* Flex mode: expand each source pixel to h×v destination pixels. */
        for (Int y = 0; y < 8; y++) {
            Int yv = y * v;
            for (Int x = 0; x < 8; x++) {
                Byte val = jpeg_clamp(b->v[y * 8 + x]);
                Int xh = x * h;
                for (Int yy = 0; yy < v; yy++)
                    for (Int xx = 0; xx < h; xx++)
                        dst[(yv + yy) * stride + xh + xx] = val;
            }
        }
        return BURROW_NO_ERROR;
    }

    /* Level shift by +128, clip to [0, 255], and write to dst. */
    for (Int y = 0; y < 8; y++)
        for (Int x = 0; x < 8; x++)
            dst[y * stride + x] = jpeg_clamp(b->v[y * 8 + x]);
    return BURROW_NO_ERROR;
}

/* findRST advances past the next RST restart marker that matches
 * expectedRST. Other than I/O errors, it is also an error if we encounter an
 * {0xFF, M} two-byte marker sequence where M is not 0x00, 0xFF or the
 * expectedRST. This is similar to libjpeg's jdmarker.c's next_marker.
 *
 * Precondition: d.tmp[:2] holds the next two bytes of JPEG-encoded input
 * (input in the d.readFull sense). */
static Error jpeg_find_rst(JpegDecoder *d, uint8_t expected_rst) {
    for (;;) {
        /* i is the index such that, at the bottom of the loop, we read 2-i
         * bytes into d.tmp[i:2], maintaining the invariant that d.tmp[:2]
         * holds the next two bytes of JPEG-encoded input. It is either 0 or
         * 1, so that each iteration advances by 1 or 2 bytes (or returns). */
        Int i = 0;
        if (d->tmp[0] == 0xff) {
            if (d->tmp[1] == expected_rst)
                return BURROW_NO_ERROR;
            if (d->tmp[1] == 0xff)
                i = 1;
            else if (d->tmp[1] != 0x00)
                /* libjpeg's jpeg_resync_to_restart does something fancy here.
                 * Any marker that's not 0x00, 0xff or expectedRST is a fatal
                 * FormatError. */
                return jpeg_err_bad_rst;
        } else if (d->tmp[1] == 0xff) {
            d->tmp[0] = 0xff;
            i = 1;
        }
        Error err = jpeg_read_full(d, d->tmp + i, 2 - i);
        if (BURROW_FAILED(err))
            return err;
    }
}

/* Specified in section B.2.3. */
static Error jpeg_process_sos(JpegDecoder *d, Int n) {
    if (d->n_comp == 0)
        return jpeg_err_missing_sof;
    if (n < 6 || 4 + 2 * d->n_comp < n || n % 2 != 0)
        return jpeg_err_sos_length;
    Error err = jpeg_read_full(d, d->tmp, n);
    if (BURROW_FAILED(err))
        return err;
    Int n_comp = d->tmp[0];
    if (n != 4 + 2 * n_comp)
        return jpeg_err_sos_inconsistent;
    struct {
        uint8_t comp_index;
        uint8_t td; /* DC table selector. */
        uint8_t ta; /* AC table selector. */
    } scan[JPEG_MAX_COMPONENTS];
    Int total_hv = 0;
    for (Int i = 0; i < n_comp; i++) {
        uint8_t cs = d->tmp[1 + 2 * i]; /* Component selector. */
        Int comp_index = -1;
        for (Int j = 0; j < d->n_comp; j++)
            if (cs == d->comp[j].c)
                comp_index = j;
        if (comp_index < 0)
            return jpeg_err_unknown_selector;
        scan[i].comp_index = (uint8_t)comp_index;
        /* Section B.2.3 states that "the value of Cs_j shall be different
         * from the values of Cs_1 through Cs_(j-1)". Since we have previously
         * verified that a frame's component identifiers (C_i values in
         * section B.2.2) are unique, it suffices to check that the implicit
         * indexes into d.comp are unique. */
        for (Int j = 0; j < i; j++)
            if (scan[i].comp_index == scan[j].comp_index)
                return jpeg_err_repeated_selector;
        total_hv += d->comp[comp_index].h * d->comp[comp_index].v;

        /* The baseline t <= 1 restriction is specified in table B.3. */
        scan[i].td = d->tmp[2 + 2 * i] >> 4;
        if (scan[i].td > JPEG_MAX_TH || (d->baseline && scan[i].td > 1))
            return jpeg_err_td;
        scan[i].ta = d->tmp[2 + 2 * i] & 0x0f;
        if (scan[i].ta > JPEG_MAX_TH || (d->baseline && scan[i].ta > 1))
            return jpeg_err_ta;
    }
    /* Section B.2.3 states that if there is more than one component then the
     * total H*V values in a scan must be <= 10. */
    if (d->n_comp > 1 && total_hv > 10)
        return jpeg_err_total_sampling;

    /* zigStart and zigEnd are the spectral selection bounds. ah and al are
     * the successive approximation high and low values. The spec calls these
     * values Ss, Se, Ah and Al. For sequential JPEGs, these parameters are
     * hard-coded to 0/63/0/0, as per table B.3. */
    int32_t zig_start = 0, zig_end = JPEG_BLOCK_SIZE - 1;
    uint32_t ah = 0, al = 0;
    if (d->progressive) {
        zig_start = (int32_t)d->tmp[1 + 2 * n_comp];
        zig_end = (int32_t)d->tmp[2 + 2 * n_comp];
        ah = (uint32_t)(d->tmp[3 + 2 * n_comp] >> 4);
        al = (uint32_t)(d->tmp[3 + 2 * n_comp] & 0x0f);
        if ((zig_start == 0 && zig_end != 0) || zig_start > zig_end ||
            JPEG_BLOCK_SIZE <= zig_end)
            return jpeg_err_spectral;
        if (zig_start != 0 && n_comp != 1)
            return jpeg_err_progressive_ac;
        if (ah != 0 && ah != al + 1)
            return jpeg_err_successive;
    }

    /* mxx and myy are the number of MCUs (Minimum Coded Units) in the image.
     * The MCU dimensions are based on the maximum sampling factors. */
    Int mxx = (d->width + 8 * d->max_h - 1) / (8 * d->max_h);
    Int myy = (d->height + 8 * d->max_v - 1) / (8 * d->max_v);
    if (d->img1 == NULL && d->img3 == NULL) {
        err = jpeg_make_img(d, mxx, myy);
        if (BURROW_FAILED(err))
            return err;
    }
    if (d->progressive) {
        for (Int i = 0; i < n_comp; i++) {
            Int ci = scan[i].comp_index;
            if (d->prog_coeffs[ci] == NULL) {
                Int len = jpeg_mul_size(mxx * myy, d->comp[ci].h * d->comp[ci].v);
                if (len < 0 || jpeg_mul_size(len, (Int)sizeof(JpegBlock)) < 0)
                    return burrow_err_out_of_memory;
                if (len == 0)
                    continue;
                JpegBlock *blocks = (JpegBlock *)mem_alloc(
                    d->a, (size_t)len * sizeof(JpegBlock), _Alignof(JpegBlock));
                if (blocks == NULL)
                    return burrow_err_out_of_memory;
                d->prog_coeffs[ci] = blocks;
                d->prog_len[ci] = len;
            }
        }
    }

    memset(&d->bits, 0, sizeof d->bits);
    Int mcu = 0;
    uint8_t expected_rst = JPEG_RST0;
    /* b is the decoded coefficients, in natural (not zig-zag) order. bx and
     * by are the location of the current block, in units of 8x8 blocks: the
     * third block in the first row has (bx, by) = (2, 0). */
    JpegBlock b;
    int32_t dc[JPEG_MAX_COMPONENTS] = {0, 0, 0, 0};
    Int bx = 0, by = 0, block_count = 0;
    for (Int my = 0; my < myy; my++) {
        for (Int mx = 0; mx < mxx; mx++) {
            for (Int i = 0; i < n_comp; i++) {
                Int ci = scan[i].comp_index;
                Int hi = d->comp[ci].h;
                Int vi = d->comp[ci].v;
                for (Int j = 0; j < hi * vi; j++) {
                    /* The blocks are traversed one MCU at a time. For
                     * progressive images, the interleaved scans (those with
                     * nComp > 1) are traversed MCU by MCU as well, but
                     * non-interleaved scans are traversed left to right, top
                     * to bottom, and there is no data for any blocks that
                     * are inside the image at the MCU level but outside the
                     * image at the pixel level. */
                    if (n_comp != 1) {
                        bx = hi * mx + j % hi;
                        by = vi * my + j / hi;
                    } else {
                        Int q = mxx * hi;
                        bx = block_count % q;
                        by = block_count / q;
                        block_count++;
                        if (bx * 8 >= d->width || by * 8 >= d->height)
                            continue;
                    }

                    /* Load the previous partially decoded coefficients, if
                     * applicable. */
                    if (d->progressive)
                        b = d->prog_coeffs[ci][by * mxx * hi + bx];
                    else
                        memset(&b, 0, sizeof b);

                    if (ah != 0) {
                        err = jpeg_refine(d, &b, &d->huff[JPEG_AC_TABLE][scan[i].ta],
                                          zig_start, zig_end, (int32_t)1 << al);
                        if (BURROW_FAILED(err))
                            return err;
                    } else {
                        int32_t zig = zig_start;
                        if (zig == 0) {
                            zig++;
                            /* Decode the DC coefficient, as specified in
                             * section F.2.2.1. */
                            uint8_t value;
                            err = jpeg_decode_huffman(
                                d, &d->huff[JPEG_DC_TABLE][scan[i].td], &value);
                            if (BURROW_FAILED(err))
                                return err;
                            if (value > 16)
                                return jpeg_err_excessive_dc;
                            int32_t dc_delta;
                            err = jpeg_receive_extend(d, value, &dc_delta);
                            if (BURROW_FAILED(err))
                                return err;
                            dc[ci] = jpeg_add(dc[ci], dc_delta);
                            b.v[0] = jpeg_shl(dc[ci], al);
                        }

                        if (zig <= zig_end && d->eob_run > 0) {
                            d->eob_run--;
                        } else {
                            /* Decode the AC coefficients, as specified in
                             * section F.2.2.2. */
                            const JpegHuffman *huff =
                                &d->huff[JPEG_AC_TABLE][scan[i].ta];
                            for (; zig <= zig_end; zig++) {
                                uint8_t value;
                                err = jpeg_decode_huffman(d, huff, &value);
                                if (BURROW_FAILED(err))
                                    return err;
                                uint8_t val0 = value >> 4;
                                uint8_t val1 = value & 0x0f;
                                if (val1 != 0) {
                                    zig += (int32_t)val0;
                                    if (zig > zig_end)
                                        break;
                                    int32_t ac;
                                    err = jpeg_receive_extend(d, val1, &ac);
                                    if (BURROW_FAILED(err))
                                        return err;
                                    b.v[jpeg_unzig[zig]] = jpeg_shl(ac, al);
                                } else {
                                    if (val0 != 0x0f) {
                                        d->eob_run = (uint16_t)(1U << val0);
                                        if (val0 != 0) {
                                            uint32_t bits;
                                            err = jpeg_decode_bits(d, (int32_t)val0,
                                                                   &bits);
                                            if (BURROW_FAILED(err))
                                                return err;
                                            d->eob_run |= (uint16_t)bits;
                                        }
                                        d->eob_run--;
                                        break;
                                    }
                                    zig += 0x0f;
                                }
                            }
                        }
                    }

                    if (d->progressive) {
                        /* Save the coefficients. Go's Decode does not return
                         * until the entire image is decoded, so the blocks
                         * are turned into pixels once, after all of the SOS
                         * markers are processed. */
                        d->prog_coeffs[ci][by * mxx * hi + bx] = b;
                        continue;
                    }
                    err = jpeg_reconstruct_block(d, &b, bx, by, ci);
                    if (BURROW_FAILED(err))
                        return err;
                }
            }
            mcu++;
            if (d->ri > 0 && mcu % d->ri == 0 && mcu < mxx * myy) {
                /* For well-formed input, the RST[0-7] restart marker follows
                 * immediately. For corrupt input, call findRST to try to
                 * resynchronize. */
                err = jpeg_read_full(d, d->tmp, 2);
                if (BURROW_FAILED(err))
                    return err;
                if (d->tmp[0] != 0xff || d->tmp[1] != expected_rst) {
                    err = jpeg_find_rst(d, expected_rst);
                    if (BURROW_FAILED(err))
                        return err;
                }
                expected_rst++;
                if (expected_rst == JPEG_RST7 + 1)
                    expected_rst = JPEG_RST0;
                /* Reset the Huffman decoder. */
                memset(&d->bits, 0, sizeof d->bits);
                /* Reset the DC components, as per section F.2.1.3.1. */
                memset(dc, 0, sizeof dc);
                /* Reset the progressive decoder state, as per section
                 * G.1.2.2. */
                d->eob_run = 0;
            }
        }
    }
    return BURROW_NO_ERROR;
}

static Error jpeg_reconstruct_progressive_image(JpegDecoder *d) {
    /* The mxx, by and bx variables have the same meaning as in the
     * processSOS method. */
    Int mxx = (d->width + 8 * d->max_h - 1) / (8 * d->max_h);
    for (Int i = 0; i < d->n_comp; i++) {
        if (d->prog_coeffs[i] == NULL)
            continue;
        Int v = 8 * d->max_v / d->comp[i].v;
        Int h = 8 * d->max_h / d->comp[i].h;
        Int stride = mxx * d->comp[i].h;
        for (Int by = 0; by * v < d->height; by++) {
            for (Int bx = 0; bx * h < d->width; bx++) {
                Error err = jpeg_reconstruct_block(
                    d, &d->prog_coeffs[i][by * stride + bx], bx, by, i);
                if (BURROW_FAILED(err))
                    return err;
            }
        }
    }
    return BURROW_NO_ERROR;
}

/* ------------------------------------------------------------------ decode */

/* applyBlack combines d.img3 and d.blackPix into a CMYK image. The formula
 * used depends on whether the JPEG image is stored as CMYK or YCbCrK,
 * indicated by the APP14 (Adobe) metadata.
 *
 * Adobe CMYK JPEG images are inverted, where 255 means no ink instead of
 * full ink, so we apply "v = 255 - v" at various points. Note that a double
 * inversion is a no-op, so inversions might be implicit in the code below. */
static Error jpeg_apply_black(JpegDecoder *d, Image *out) {
    if (!d->adobe_transform_valid)
        return jpeg_err_no_adobe;

    ImageRectangle bounds = d->img3->rect;
    ImageCMYK *img = image_new_cmyk(d->a, bounds);
    if (img == NULL)
        return burrow_err_out_of_memory;
    Byte *pix = (Byte *)img->pix.p;

    /* If the 4-component JPEG image isn't explicitly marked as "Unknown (RGB
     * or CMYK)" we assume that it is YCbCrK. This matches libjpeg's
     * jdapimin.c. */
    if (d->adobe_transform != JPEG_ADOBE_TRANSFORM_UNKNOWN) {
        /* Convert the YCbCr part of the YCbCrK to RGB, invert the RGB to get
         * CMY, and patch in the original K. The RGB to CMY inversion cancels
         * out the 'Adobe inversion' described in the applyBlack doc comment
         * above, so in practice, only the fourth channel (black) is
         * inverted. Go draws into an RGBA and then takes its pixels, which
         * have the same layout as these. */
        ImageRGBA rgba = {img->pix, img->stride, img->rect};
        draw_draw(image_rgba_as_image(&rgba), bounds, image_y_cb_cr_as_image(d->img3),
                  bounds.min, DRAW_SRC);
        for (Int i_base = 0, y = bounds.min.y; y < bounds.max.y;
             i_base += img->stride, y++)
            for (Int i = i_base + 3, x = bounds.min.x; x < bounds.max.x; i += 4, x++)
                pix[i] =
                    (Byte)(255 - d->black_pix[(y - bounds.min.y) * d->black_stride +
                                              (x - bounds.min.x)]);
        *out = image_cmyk_as_image(img);
        return BURROW_NO_ERROR;
    }

    /* The first three channels (cyan, magenta, yellow) of the CMYK were
     * decoded into d.img3, but each channel was decoded into a separate
     * []byte slice, and some channels may be subsampled. We interleave the
     * separate channels into an image.CMYK's single []byte slice containing
     * 4 contiguous bytes per pixel. */
    struct {
        const Byte *src;
        Int stride;
    } translations[4] = {
        {(const Byte *)d->img3->y.p, d->img3->y_stride},
        {(const Byte *)d->img3->cb.p, d->img3->c_stride},
        {(const Byte *)d->img3->cr.p, d->img3->c_stride},
        {d->black_pix, d->black_stride},
    };
    for (Int t = 0; t < 4; t++) {
        bool subsample = d->comp[t].h != d->comp[0].h || d->comp[t].v != d->comp[0].v;
        for (Int i_base = 0, y = bounds.min.y; y < bounds.max.y;
             i_base += img->stride, y++) {
            Int sy = y - bounds.min.y;
            if (subsample)
                sy /= 2;
            for (Int i = i_base + t, x = bounds.min.x; x < bounds.max.x; i += 4, x++) {
                Int sx = x - bounds.min.x;
                if (subsample)
                    sx /= 2;
                pix[i] =
                    (Byte)(255 - translations[t].src[sy * translations[t].stride + sx]);
            }
        }
    }
    *out = image_cmyk_as_image(img);
    return BURROW_NO_ERROR;
}

static bool jpeg_is_rgb(const JpegDecoder *d) {
    if (d->jfif)
        return false;
    if (d->adobe_transform_valid && d->adobe_transform == JPEG_ADOBE_TRANSFORM_UNKNOWN)
        /* https://www.sno.phy.queensu.ca/~phil/exiftool/TagNames/JPEG.html#Adobe
         * says that 0 means Unknown (and in practice RGB) and 1 means
         * YCbCr. */
        return true;
    return d->comp[0].c == 'R' && d->comp[1].c == 'G' && d->comp[2].c == 'B';
}

static Error jpeg_convert_to_rgb(JpegDecoder *d, Image *out) {
    /* convertToRGB supports the historical chroma subsampling ratios and not
     * the intersection of atypical subsampling and RGB-instead-of-YCbCr,
     * which is very rare. */
    Int h0 = d->comp[0].h, h1 = d->comp[1].h, h2 = d->comp[2].h;
    Int v0 = d->comp[0].v, v1 = d->comp[1].v, v2 = d->comp[2].v;
    if (h1 != h2 || h0 % h1 != 0 || v1 != v2 || v0 % v1 != 0)
        return jpeg_err_sampling;

    Int c_scale = h0 / h1;
    ImageRectangle bounds = d->img3->rect;
    ImageRGBA *img = image_new_rgba(d->a, bounds);
    if (img == NULL)
        return burrow_err_out_of_memory;
    Byte *pix = (Byte *)img->pix.p;
    const Byte *yp = (const Byte *)d->img3->y.p;
    const Byte *cb = (const Byte *)d->img3->cb.p;
    const Byte *cr = (const Byte *)d->img3->cr.p;
    for (Int y = bounds.min.y; y < bounds.max.y; y++) {
        Int po = image_rgba_pix_offset(img, bounds.min.x, y);
        Int yo = image_y_cb_cr_y_offset(d->img3, bounds.min.x, y);
        Int co = image_y_cb_cr_c_offset(d->img3, bounds.min.x, y);
        for (Int i = 0, i_max = bounds.max.x - bounds.min.x; i < i_max; i++) {
            pix[po + 4 * i + 0] = yp[yo + i];
            pix[po + 4 * i + 1] = cb[co + i / c_scale];
            pix[po + 4 * i + 2] = cr[co + i / c_scale];
            pix[po + 4 * i + 3] = 255;
        }
    }
    *out = image_rgba_as_image(img);
    return BURROW_NO_ERROR;
}

/* decode reads a JPEG image from r. With config_only it stops at the first
 * SOS marker, or at the SOF marker in a JFIF file, and makes no image. */
static Error jpeg_decoder_decode(JpegDecoder *d, IoReader r, bool config_only,
                                 Image *out) {
    d->r = r;

    /* Check for the Start Of Image marker. */
    Error err = jpeg_read_full(d, d->tmp, 2);
    if (BURROW_FAILED(err))
        return err;
    if (d->tmp[0] != 0xff || d->tmp[1] != JPEG_SOI)
        return jpeg_err_missing_soi;

    /* Process the remaining segments until the End Of Image marker. */
    for (;;) {
        err = jpeg_read_full(d, d->tmp, 2);
        if (BURROW_FAILED(err))
            return err;
        while (d->tmp[0] != 0xff) {
            /* Strictly speaking, this is a format error. However, libjpeg is
             * liberal in what it accepts. As of version 9, next_marker in
             * jdmarker.c treats this as a warning (JWRN_EXTRANEOUS_DATA) and
             * continues to decode the stream. We are therefore also liberal
             * in what we accept. Extraneous data is silently ignored.
             *
             * Note that extraneous 0xff bytes in e.g. SOS data are escaped as
             * "\xff\x00", and so are detected a little further down below. */
            d->tmp[0] = d->tmp[1];
            err = jpeg_read_byte(d, &d->tmp[1]);
            if (BURROW_FAILED(err))
                return err;
        }
        Byte marker = d->tmp[1];
        if (marker == 0)
            /* Treat "\xff\x00" as extraneous data. */
            continue;
        while (marker == 0xff) {
            /* Section B.1.1.2 says, "Any marker may optionally be preceded by
             * any number of fill bytes, which are bytes assigned code
             * X'FF'". */
            err = jpeg_read_byte(d, &marker);
            if (BURROW_FAILED(err))
                return err;
        }
        if (marker == JPEG_EOI) /* End Of Image. */
            break;
        if (JPEG_RST0 <= marker && marker <= JPEG_RST7)
            /* Figures B.2 and B.16 of the specification suggest that restart
             * markers should only occur between Entropy Coded Segments and
             * not after the final ECS. However, some encoders may generate
             * incorrect JPEGs with a final restart marker. That restart
             * marker will be seen here instead of inside the processSOS
             * method, and is ignored as a harmless error. Restart markers
             * have no extra data, so we check for this before we read the
             * 16-bit length of the segment. */
            continue;

        /* Read the 16-bit length of the segment. The value includes the 2
         * bytes for the length itself, so we subtract 2 to get the number of
         * remaining bytes. */
        err = jpeg_read_full(d, d->tmp, 2);
        if (BURROW_FAILED(err))
            return err;
        Int n = ((Int)d->tmp[0] << 8) + (Int)d->tmp[1] - 2;
        if (n < 0)
            return jpeg_err_short_segment;

        switch (marker) {
        case JPEG_SOF0:
        case JPEG_SOF1:
        case JPEG_SOF2:
            d->baseline = marker == JPEG_SOF0;
            d->progressive = marker == JPEG_SOF2;
            err = jpeg_process_sof(d, n);
            if (config_only && d->jfif)
                return err;
            break;
        case JPEG_DHT:
            err = config_only ? jpeg_ignore(d, n) : jpeg_process_dht(d, n);
            break;
        case JPEG_DQT:
            err = config_only ? jpeg_ignore(d, n) : jpeg_process_dqt(d, n);
            break;
        case JPEG_SOS:
            if (config_only)
                return BURROW_NO_ERROR;
            err = jpeg_process_sos(d, n);
            break;
        case JPEG_DRI:
            err = config_only ? jpeg_ignore(d, n) : jpeg_process_dri(d, n);
            break;
        case JPEG_APP0:
            err = jpeg_process_app0(d, n);
            break;
        case JPEG_APP14:
            err = jpeg_process_app14(d, n);
            break;
        default:
            if ((JPEG_APP0 <= marker && marker <= JPEG_APP15) || marker == JPEG_COM)
                err = jpeg_ignore(d, n);
            else if (marker < 0xc0) /* See Table B.1 "Marker code assignments". */
                err = jpeg_err_unknown_marker_format;
            else
                err = jpeg_err_unknown_marker;
            break;
        }
        if (BURROW_FAILED(err))
            return err;
    }

    if (d->progressive) {
        err = jpeg_reconstruct_progressive_image(d);
        if (BURROW_FAILED(err))
            return err;
    }
    if (d->img1 != NULL) {
        *out = image_gray_as_image(d->img1);
        return BURROW_NO_ERROR;
    }
    if (d->img3 != NULL) {
        if (d->black_pix != NULL)
            return jpeg_apply_black(d, out);
        if (jpeg_is_rgb(d))
            return jpeg_convert_to_rgb(d, out);
        *out = image_y_cb_cr_as_image(d->img3);
        return BURROW_NO_ERROR;
    }
    return jpeg_err_missing_sos;
}

static JpegDecoder *jpeg_decoder_new(Alloc *a) {
    JpegDecoder *d = (JpegDecoder *)mem_alloc(a, sizeof *d, _Alignof(JpegDecoder));
    if (d != NULL)
        d->a = a;
    return d;
}

Image jpeg_decode(Alloc *a, IoReader r, Error *err) {
    Image m = {NULL, NULL};
    JpegDecoder *d = jpeg_decoder_new(a);
    if (d == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return m;
    }
    Error e = jpeg_decoder_decode(d, r, false, &m);
    if (BURROW_FAILED(e))
        m = (Image){NULL, NULL};
    jpeg_decoder_free(d, m);
    BURROW_OUT(err, e);
    return m;
}

ImageConfig jpeg_decode_config(Alloc *a, IoReader r, Error *err) {
    ImageConfig c;
    memset(&c, 0, sizeof c);
    JpegDecoder *d = jpeg_decoder_new(a);
    if (d == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return c;
    }
    Image none = {NULL, NULL};
    Error e = jpeg_decoder_decode(d, r, true, &none);
    if (BURROW_OK(e)) {
        switch (d->n_comp) {
        case 1:
            c.color_model = color_gray_model;
            break;
        case 3:
            c.color_model = jpeg_is_rgb(d) ? color_rgba_model : color_y_cb_cr_model;
            break;
        case 4:
            c.color_model = color_cmyk_model;
            break;
        default:
            e = jpeg_err_missing_sof;
            break;
        }
        if (BURROW_OK(e)) {
            c.width = d->width;
            c.height = d->height;
        }
    }
    jpeg_decoder_free(d, none);
    BURROW_OUT(err, e);
    return c;
}

static Image jpeg_decode_fn(void *env, Alloc *a, IoReader r, Error *err) {
    (void)env;
    return jpeg_decode(a, r, err);
}

static ImageConfig jpeg_decode_config_fn(void *env, Alloc *a, IoReader r, Error *err) {
    (void)env;
    return jpeg_decode_config(a, r, err);
}

static void jpeg_do_register(void *env) {
    (void)env;
    image_register_format(
        BURROW_S("jpeg"), BURROW_S("\xff\xd8"),
        BURROW_FN(ImageDecodeFunc, jpeg_decode_fn, NULL),
        BURROW_FN(ImageDecodeConfigFunc, jpeg_decode_config_fn, NULL));
}

static SyncOnce jpeg_registered;

void jpeg_register(void) {
    sync_once_do(&jpeg_registered, BURROW_FN(Func, jpeg_do_register, NULL));
}

/* ----------------------------------------------------------------- encoder */

/* div returns a/b rounded to the nearest integer, instead of rounded to
 * zero. */
static int32_t jpeg_div(int32_t a, int32_t b) {
    if (a >= 0)
        return (a + (b >> 1)) / b;
    return -((-a + (b >> 1)) / b);
}

/* bitCount counts the number of bits needed to hold an integer. */
static const uint8_t jpeg_bit_count[256] = {
    0, 1, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
    5, 5, 5, 5, 5, 5, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
    6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 8, 8,
    8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
    8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
    8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
    8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
    8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
};

enum {
    JPEG_QUANT_LUMINANCE = 0,
    JPEG_QUANT_CHROMINANCE = 1,
    JPEG_N_QUANT = 2,

    JPEG_HUFF_LUMINANCE_DC = 0,
    JPEG_HUFF_LUMINANCE_AC = 1,
    JPEG_HUFF_CHROMINANCE_DC = 2,
    JPEG_HUFF_CHROMINANCE_AC = 3,
    JPEG_N_HUFF = 4,
};

/* unscaledQuant are the unscaled quantization tables in zig-zag order. Each
 * encoder copies and scales the tables according to its quality parameter.
 * The values are derived from section K.1 of the spec, after converting from
 * natural to zig-zag order. */
static const uint8_t jpeg_unscaled_quant[JPEG_N_QUANT][JPEG_BLOCK_SIZE] = {
    /* Luminance. */
    {
        16, 11, 12,  14,  12,  10, 16, 14,  13,  14,  18,  17,  16, 19,  24,  40,
        26, 24, 22,  22,  24,  49, 35, 37,  29,  40,  58,  51,  61, 60,  57,  51,
        56, 55, 64,  72,  92,  78, 64, 68,  87,  69,  55,  56,  80, 109, 81,  87,
        95, 98, 103, 104, 103, 62, 77, 113, 121, 112, 100, 120, 92, 101, 103, 99,
    },
    /* Chrominance. */
    {
        17, 18, 18, 24, 21, 24, 47, 26, 26, 47, 99, 66, 56, 66, 99, 99,
        99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
        99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
        99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
    },
};

/* huffmanSpec specifies a Huffman encoding. count[i] is the number of codes
 * of length i+1 bits, and value[i] is the decoded value of the i'th
 * codeword. */
typedef struct JpegHuffmanSpec {
    uint8_t count[16];
    const uint8_t *value;
    Int n_value;
} JpegHuffmanSpec;

static const uint8_t jpeg_dc_values[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};

static const uint8_t jpeg_luminance_ac_values[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51,
    0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1,
    0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18,
    0x19, 0x1a, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
    0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57,
    0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75,
    0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92,
    0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
    0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
    0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8,
    0xd9, 0xda, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2,
    0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa,
};

static const uint8_t jpeg_chrominance_ac_values[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07,
    0x61, 0x71, 0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09,
    0x23, 0x33, 0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25,
    0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38,
    0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56,
    0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74,
    0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5,
    0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba,
    0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6,
    0xd7, 0xd8, 0xd9, 0xda, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2,
    0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa,
};

/* theHuffmanSpec is the Huffman encoding specifications. This encoder uses
 * the same Huffman encoding for all images. It is also the same Huffman
 * encoding used by section K.3 of the spec.
 *
 * The DC tables have 12 decoded values, called categories. The AC tables
 * have 162 decoded values: bytes that pack a 4-bit Run and a 4-bit Size.
 * There are 16 valid Runs and 10 valid Sizes, plus two special R|S cases:
 * 0|0 (meaning EOB) and F|0 (meaning ZRL). */
static const JpegHuffmanSpec jpeg_huffman_spec[JPEG_N_HUFF] = {
    /* Luminance DC. */
    {{0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0}, jpeg_dc_values, 12},
    /* Luminance AC. */
    {{0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 125}, jpeg_luminance_ac_values, 162},
    /* Chrominance DC. */
    {{0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0}, jpeg_dc_values, 12},
    /* Chrominance AC. */
    {{0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 119},
     jpeg_chrominance_ac_values,
     162},
};

/* encoder encodes an image to the JPEG format. */
typedef struct JpegEncoder {
    /* w is the writer to write to. err is the first error encountered during
     * writing. All attempted writes after the first error become no-ops.
     * own is the bufio.Writer made here when w is not one. */
    BufioWriter *w;
    BufioWriter *own;
    Error err;
    /* buf is a scratch buffer. */
    Byte buf[16];
    /* bits and nBits are accumulated bits to write to w. */
    uint32_t bits, n_bits;
    /* quant is the scaled quantization tables, in zig-zag order. */
    Byte quant[JPEG_N_QUANT][JPEG_BLOCK_SIZE];
    /* huffmanLUT is a compiled look-up table representation of a
     * huffmanSpec. Each value maps to a uint32 of which the 8 most
     * significant bits hold the codeword size in bits and the 24 least
     * significant bits hold the codeword. The maximum codeword size is 16
     * bits. Go builds these once when the package starts, and they take
     * no time to build, so here every encode builds its own. */
    uint32_t lut[JPEG_N_HUFF][256];
} JpegEncoder;

static void jpeg_huffman_lut_init(uint32_t *h, const JpegHuffmanSpec *s) {
    uint32_t code = 0;
    Int k = 0;
    for (uint32_t i = 0; i < 16; i++) {
        uint32_t n_bits = (i + 1) << 24;
        for (uint8_t j = 0; j < s->count[i]; j++) {
            h[s->value[k]] = n_bits | code;
            code++;
            k++;
        }
        code <<= 1;
    }
}

static void jpeg_flush(JpegEncoder *e) {
    if (BURROW_FAILED(e->err))
        return;
    e->err = bufio_writer_flush(e->w);
}

static void jpeg_write(JpegEncoder *e, const Byte *p, Int n) {
    if (BURROW_FAILED(e->err))
        return;
    Byte *q;
    memcpy(&q, &p, sizeof q);
    bufio_writer_write(e->w, slice_from(q, n, n, TYPE_BYTE), &e->err);
}

static void jpeg_write_byte(JpegEncoder *e, Byte b) {
    if (BURROW_FAILED(e->err))
        return;
    e->err = bufio_writer_write_byte(e->w, b);
}

/* emit emits the least significant nBits bits of bits to the bit-stream. The
 * precondition is bits < 1<<nBits && nBits <= 16. */
static void jpeg_emit(JpegEncoder *e, uint32_t bits, uint32_t n_bits) {
    n_bits += e->n_bits;
    bits <<= 32 - n_bits;
    bits |= e->bits;
    while (n_bits >= 8) {
        uint8_t b = (uint8_t)(bits >> 24);
        jpeg_write_byte(e, b);
        if (b == 0xff)
            jpeg_write_byte(e, 0x00);
        bits <<= 8;
        n_bits -= 8;
    }
    e->bits = bits;
    e->n_bits = n_bits;
}

/* emitHuff emits the given value with the given Huffman encoder. */
static void jpeg_emit_huff(JpegEncoder *e, int h, int32_t value) {
    uint32_t x = e->lut[h][value];
    jpeg_emit(e, x & ((1U << 24) - 1), x >> 24);
}

/* emitHuffRLE emits a run of runLength copies of value encoded with the
 * given Huffman encoder. */
static void jpeg_emit_huff_rle(JpegEncoder *e, int h, int32_t run_length,
                               int32_t value) {
    int32_t a = value, b = value;
    if (a < 0) {
        a = -value;
        b = value - 1;
    }
    uint32_t n_bits;
    if (a < 0x100)
        n_bits = jpeg_bit_count[a];
    else
        n_bits = 8 + (uint32_t)jpeg_bit_count[a >> 8];
    jpeg_emit_huff(e, h, run_length << 4 | (int32_t)n_bits);
    if (n_bits > 0)
        jpeg_emit(e, (uint32_t)b & ((1U << n_bits) - 1), n_bits);
}

/* writeMarkerHeader writes the header for a marker with the given length. */
static void jpeg_write_marker_header(JpegEncoder *e, uint8_t marker, Int markerlen) {
    e->buf[0] = 0xff;
    e->buf[1] = marker;
    e->buf[2] = (uint8_t)(markerlen >> 8);
    e->buf[3] = (uint8_t)(markerlen & 0xff);
    jpeg_write(e, e->buf, 4);
}

/* writeDQT writes the Define Quantization Table marker. */
static void jpeg_write_dqt(JpegEncoder *e) {
    const Int markerlen = 2 + JPEG_N_QUANT * (1 + JPEG_BLOCK_SIZE);
    jpeg_write_marker_header(e, JPEG_DQT, markerlen);
    for (int i = 0; i < JPEG_N_QUANT; i++) {
        jpeg_write_byte(e, (uint8_t)i);
        jpeg_write(e, e->quant[i], JPEG_BLOCK_SIZE);
    }
}

/* writeSOF0 writes the Start Of Frame (Baseline Sequential) marker. */
static void jpeg_write_sof0(JpegEncoder *e, ImagePoint size, Int n_component) {
    Int markerlen = 8 + 3 * n_component;
    jpeg_write_marker_header(e, JPEG_SOF0, markerlen);
    e->buf[0] = 8; /* 8-bit color. */
    e->buf[1] = (uint8_t)(size.y >> 8);
    e->buf[2] = (uint8_t)(size.y & 0xff);
    e->buf[3] = (uint8_t)(size.x >> 8);
    e->buf[4] = (uint8_t)(size.x & 0xff);
    e->buf[5] = (uint8_t)n_component;
    if (n_component == 1) {
        e->buf[6] = 1;
        /* No subsampling for grayscale image. */
        e->buf[7] = 0x11;
        e->buf[8] = 0x00;
    } else {
        for (Int i = 0; i < n_component; i++) {
            e->buf[3 * i + 6] = (uint8_t)(i + 1);
            /* We use 4:2:0 chroma subsampling. */
            e->buf[3 * i + 7] = (uint8_t)"\x22\x11\x11"[i];
            e->buf[3 * i + 8] = (uint8_t)"\x00\x01\x01"[i];
        }
    }
    jpeg_write(e, e->buf, 3 * (n_component - 1) + 9);
}

/* writeDHT writes the Define Huffman Table marker. */
static void jpeg_write_dht(JpegEncoder *e, Int n_component) {
    Int markerlen = 2;
    /* Grayscale drops the Chrominance tables. */
    int n_specs = n_component == 1 ? 2 : JPEG_N_HUFF;
    for (int i = 0; i < n_specs; i++)
        markerlen += 1 + 16 + jpeg_huffman_spec[i].n_value;
    jpeg_write_marker_header(e, JPEG_DHT, markerlen);
    for (int i = 0; i < n_specs; i++) {
        const JpegHuffmanSpec *s = &jpeg_huffman_spec[i];
        jpeg_write_byte(e, (Byte) "\x00\x10\x01\x11"[i]);
        jpeg_write(e, s->count, 16);
        jpeg_write(e, s->value, s->n_value);
    }
}

/* writeBlock writes a block of pixel data using the given quantization
 * table, returning the post-quantized DC value of the DCT-transformed block.
 * b is in natural (not zig-zag) order. */
static int32_t jpeg_write_block(JpegEncoder *e, JpegBlock *b, int q, int32_t prev_dc) {
    jpeg_fdct(b);
    /* Emit the DC delta. */
    int32_t dc = jpeg_div(b->v[0], 8 * (int32_t)e->quant[q][0]);
    jpeg_emit_huff_rle(e, 2 * q + 0, 0, dc - prev_dc);
    /* Emit the AC components. */
    int h = 2 * q + 1;
    int32_t run_length = 0;
    for (int zig = 1; zig < JPEG_BLOCK_SIZE; zig++) {
        int32_t ac = jpeg_div(b->v[jpeg_unzig[zig]], 8 * (int32_t)e->quant[q][zig]);
        if (ac == 0) {
            run_length++;
        } else {
            while (run_length > 15) {
                jpeg_emit_huff(e, h, 0xf0);
                run_length -= 16;
            }
            jpeg_emit_huff_rle(e, h, run_length, ac);
            run_length = 0;
        }
    }
    if (run_length > 0)
        jpeg_emit_huff(e, h, 0x00);
    return dc;
}

static Int jpeg_min(Int a, Int b) {
    return a < b ? a : b;
}

/* toYCbCr converts the 8x8 region of m whose top-left corner is p to its
 * YCbCr values. */
static void jpeg_to_y_cb_cr(Image m, ImagePoint p, JpegBlock *y_block,
                            JpegBlock *cb_block, JpegBlock *cr_block) {
    ImageRectangle b = image_bounds(m);
    Int xmax = b.max.x - 1;
    Int ymax = b.max.y - 1;
    for (Int j = 0; j < 8; j++) {
        for (Int i = 0; i < 8; i++) {
            ColorRGBAValue c = color_rgba(
                image_at(m, jpeg_min(p.x + i, xmax), jpeg_min(p.y + j, ymax)));
            ColorYCbCr yc = color_rgb_to_y_cb_cr(
                (uint8_t)(c.r >> 8), (uint8_t)(c.g >> 8), (uint8_t)(c.b >> 8));
            y_block->v[8 * j + i] = (int32_t)yc.y;
            cb_block->v[8 * j + i] = (int32_t)yc.cb;
            cr_block->v[8 * j + i] = (int32_t)yc.cr;
        }
    }
}

/* grayToY stores the 8x8 region of m whose top-left corner is p in
 * yBlock. */
static void jpeg_gray_to_y(const ImageGray *m, ImagePoint p, JpegBlock *y_block) {
    ImageRectangle b = m->rect;
    Int xmax = b.max.x - 1;
    Int ymax = b.max.y - 1;
    const Byte *pix = (const Byte *)m->pix.p;
    for (Int j = 0; j < 8; j++) {
        for (Int i = 0; i < 8; i++) {
            Int idx = image_gray_pix_offset(m, jpeg_min(p.x + i, xmax),
                                            jpeg_min(p.y + j, ymax));
            y_block->v[8 * j + i] = (int32_t)pix[idx];
        }
    }
}

/* rgbaToYCbCr is a specialized version of toYCbCr for image.RGBA images. */
static void jpeg_rgba_to_y_cb_cr(const ImageRGBA *m, ImagePoint p, JpegBlock *y_block,
                                 JpegBlock *cb_block, JpegBlock *cr_block) {
    ImageRectangle b = m->rect;
    Int xmax = b.max.x - 1;
    Int ymax = b.max.y - 1;
    for (Int j = 0; j < 8; j++) {
        Int sj = p.y + j;
        if (sj > ymax)
            sj = ymax;
        Int offset = (sj - b.min.y) * m->stride - b.min.x * 4;
        for (Int i = 0; i < 8; i++) {
            Int sx = p.x + i;
            if (sx > xmax)
                sx = xmax;
            const Byte *pix = (const Byte *)m->pix.p + offset + sx * 4;
            ColorYCbCr yc = color_rgb_to_y_cb_cr(pix[0], pix[1], pix[2]);
            y_block->v[8 * j + i] = (int32_t)yc.y;
            cb_block->v[8 * j + i] = (int32_t)yc.cb;
            cr_block->v[8 * j + i] = (int32_t)yc.cr;
        }
    }
}

/* yCbCrToYCbCr is a specialized version of toYCbCr for image.YCbCr
 * images. */
static void jpeg_y_cb_cr_to_y_cb_cr(const ImageYCbCr *m, ImagePoint p,
                                    JpegBlock *y_block, JpegBlock *cb_block,
                                    JpegBlock *cr_block) {
    ImageRectangle b = m->rect;
    Int xmax = b.max.x - 1;
    Int ymax = b.max.y - 1;
    const Byte *yp = (const Byte *)m->y.p;
    const Byte *cb = (const Byte *)m->cb.p;
    const Byte *cr = (const Byte *)m->cr.p;
    for (Int j = 0; j < 8; j++) {
        Int sy = p.y + j;
        if (sy > ymax)
            sy = ymax;
        for (Int i = 0; i < 8; i++) {
            Int sx = p.x + i;
            if (sx > xmax)
                sx = xmax;
            Int yi = image_y_cb_cr_y_offset(m, sx, sy);
            Int ci = image_y_cb_cr_c_offset(m, sx, sy);
            y_block->v[8 * j + i] = (int32_t)yp[yi];
            cb_block->v[8 * j + i] = (int32_t)cb[ci];
            cr_block->v[8 * j + i] = (int32_t)cr[ci];
        }
    }
}

/* scale scales the 16x16 region represented by the 4 src blocks to the 8x8
 * dst block. */
static void jpeg_scale(JpegBlock *dst, const JpegBlock src[4]) {
    for (int i = 0; i < 4; i++) {
        int dst_off = (i & 2) << 4 | (i & 1) << 2;
        for (int y = 0; y < 4; y++) {
            for (int x = 0; x < 4; x++) {
                int j = 16 * y + 2 * x;
                int32_t sum =
                    src[i].v[j] + src[i].v[j + 1] + src[i].v[j + 8] + src[i].v[j + 9];
                dst->v[8 * y + x + dst_off] = (sum + 2) >> 2;
            }
        }
    }
}

/* sosHeaderY is the SOS marker "\xff\xda" followed by 8 bytes:
 *   - the marker length "\x00\x08",
 *   - the number of components "\x01",
 *   - component 1 uses DC table 0 and AC table 0 "\x01\x00",
 *   - the bytes "\x00\x3f\x00". Section B.2.3 of the spec says that for
 *     sequential DCTs, those bytes (8-bit Ss, 8-bit Se, 4-bit Ah, 4-bit Al)
 *     should be 0x00, 0x3f, 0x00<<4 | 0x00. */
static const Byte jpeg_sos_header_y[10] = {
    0xff, 0xda, 0x00, 0x08, 0x01, 0x01, 0x00, 0x00, 0x3f, 0x00,
};

/* sosHeaderYCbCr is the SOS marker "\xff\xda" followed by 12 bytes:
 *   - the marker length "\x00\x0c",
 *   - the number of components "\x03",
 *   - component 1 uses DC table 0 and AC table 0 "\x01\x00",
 *   - component 2 uses DC table 1 and AC table 1 "\x02\x11",
 *   - component 3 uses DC table 1 and AC table 1 "\x03\x11",
 *   - the bytes "\x00\x3f\x00", as for sosHeaderY. */
static const Byte jpeg_sos_header_y_cb_cr[14] = {
    0xff, 0xda, 0x00, 0x0c, 0x03, 0x01, 0x00, 0x02, 0x11, 0x03, 0x11, 0x00, 0x3f, 0x00,
};

/* writeSOS writes the StartOfScan marker. */
static void jpeg_write_sos(JpegEncoder *e, Image m) {
    const Type *t = m.vt->self_type;
    if (t == TYPE_OF(ImageGray))
        jpeg_write(e, jpeg_sos_header_y, (Int)sizeof jpeg_sos_header_y);
    else
        jpeg_write(e, jpeg_sos_header_y_cb_cr, (Int)sizeof jpeg_sos_header_y_cb_cr);
    /* Scratch buffers to hold the YCbCr values. The blocks are in natural
     * (not zig-zag) order. DC components are delta-encoded. */
    JpegBlock b;
    JpegBlock cb[4], cr[4];
    int32_t prev_dc_y = 0, prev_dc_cb = 0, prev_dc_cr = 0;
    ImageRectangle bounds = image_bounds(m);
    if (t == TYPE_OF(ImageGray)) {
        const ImageGray *g = (const ImageGray *)m.data;
        for (Int y = bounds.min.y; y < bounds.max.y; y += 8) {
            for (Int x = bounds.min.x; x < bounds.max.x; x += 8) {
                jpeg_gray_to_y(g, image_pt(x, y), &b);
                prev_dc_y = jpeg_write_block(e, &b, 0, prev_dc_y);
            }
        }
    } else {
        const ImageRGBA *rgba =
            t == TYPE_OF(ImageRGBA) ? (const ImageRGBA *)m.data : NULL;
        const ImageYCbCr *ycbcr =
            t == TYPE_OF(ImageYCbCr) ? (const ImageYCbCr *)m.data : NULL;
        for (Int y = bounds.min.y; y < bounds.max.y; y += 16) {
            for (Int x = bounds.min.x; x < bounds.max.x; x += 16) {
                for (Int i = 0; i < 4; i++) {
                    Int x_off = (i & 1) * 8;
                    Int y_off = (i & 2) * 4;
                    ImagePoint p = image_pt(x + x_off, y + y_off);
                    if (rgba != NULL)
                        jpeg_rgba_to_y_cb_cr(rgba, p, &b, &cb[i], &cr[i]);
                    else if (ycbcr != NULL)
                        jpeg_y_cb_cr_to_y_cb_cr(ycbcr, p, &b, &cb[i], &cr[i]);
                    else
                        jpeg_to_y_cb_cr(m, p, &b, &cb[i], &cr[i]);
                    prev_dc_y = jpeg_write_block(e, &b, 0, prev_dc_y);
                }
                jpeg_scale(&b, cb);
                prev_dc_cb = jpeg_write_block(e, &b, 1, prev_dc_cb);
                jpeg_scale(&b, cr);
                prev_dc_cr = jpeg_write_block(e, &b, 1, prev_dc_cr);
            }
        }
    }
    /* Pad the last byte with 1's. */
    jpeg_emit(e, 0x7f, 7);
}

Error jpeg_encode(Alloc *a, IoWriter w, Image m, const JpegOptions *o) {
    ImageRectangle b = image_bounds(m);
    if (image_rectangle_dx(b) >= 1 << 16 || image_rectangle_dy(b) >= 1 << 16)
        return jpeg_err_too_large;
    JpegEncoder *e = (JpegEncoder *)mem_alloc(a, sizeof *e, _Alignof(JpegEncoder));
    if (e == NULL)
        return burrow_err_out_of_memory;
    if (w.vt != NULL && w.vt->self_type == TYPE_BUFIO_WRITER) {
        e->w = (BufioWriter *)w.data;
    } else {
        e->own = bufio_new_writer(a, w);
        if (e->own == NULL) {
            mem_free(a, e, sizeof *e, _Alignof(JpegEncoder));
            return burrow_err_out_of_memory;
        }
        e->w = e->own;
    }
    for (int i = 0; i < JPEG_N_HUFF; i++)
        jpeg_huffman_lut_init(e->lut[i], &jpeg_huffman_spec[i]);

    /* Clip quality to [1, 100]. */
    Int quality = JPEG_DEFAULT_QUALITY;
    if (o != NULL) {
        quality = o->quality;
        if (quality < 1)
            quality = 1;
        else if (quality > 100)
            quality = 100;
    }
    /* Convert from a quality rating to a scaling factor. */
    Int scale = quality < 50 ? 5000 / quality : 200 - quality * 2;
    /* Initialize the quantization tables. */
    for (int i = 0; i < JPEG_N_QUANT; i++) {
        for (int j = 0; j < JPEG_BLOCK_SIZE; j++) {
            Int x = jpeg_unscaled_quant[i][j];
            x = (x * scale + 50) / 100;
            if (x < 1)
                x = 1;
            else if (x > 255)
                x = 255;
            e->quant[i][j] = (uint8_t)x;
        }
    }
    /* Compute number of components based on input image type. */
    Int n_component = m.vt->self_type == TYPE_OF(ImageGray) ? 1 : 3;
    /* Write the Start Of Image marker. */
    e->buf[0] = 0xff;
    e->buf[1] = 0xd8;
    jpeg_write(e, e->buf, 2);
    /* Write the quantization tables. */
    jpeg_write_dqt(e);
    /* Write the image dimensions. */
    jpeg_write_sof0(e, image_rectangle_size(b), n_component);
    /* Write the Huffman tables. */
    jpeg_write_dht(e, n_component);
    /* Write the image data. */
    jpeg_write_sos(e, m);
    /* Write the End Of Image marker. */
    e->buf[0] = 0xff;
    e->buf[1] = 0xd9;
    jpeg_write(e, e->buf, 2);
    jpeg_flush(e);

    Error err = e->err;
    bufio_writer_free(e->own);
    mem_free(a, e, sizeof *e, _Alignof(JpegEncoder));
    return err;
}

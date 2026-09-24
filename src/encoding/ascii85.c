/* Derived from Go's src/encoding/ascii85/ascii85.go.
 * Go source: go1.27.1.
 *
 * A straight port. Encode writes all five bytes of every group before it
 * knows how many it keeps, the way Go's does, so what lands in dst past the
 * result is the same too.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/encoding/ascii85.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/math/bits.h"
#include "burrow/mem.h"
#include "burrow/runtime.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <string.h>

/* ------------------------------------------------------------------ errors */

/* The offset first, so the data pointer of the Error is a pointer to an
 * Ascii85CorruptInputError and errors_as hands it straight back. */
typedef struct Ascii85CorruptBox {
    Ascii85CorruptInputError off;
    Str message;
} Ascii85CorruptBox;

static const Type ascii85_corrupt_desc = {
    {(const Byte *)"CorruptInputError", 17},
    {(const Byte *)"encoding/ascii85", 16},
    KIND_INT64,
    (uint32_t)sizeof(Ascii85CorruptInputError),
    (uint16_t)_Alignof(Ascii85CorruptInputError),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x61383563U, /* "a85c" */
    NULL,
};

const Type *const TYPE_ASCII85_CORRUPT_INPUT_ERROR = &ascii85_corrupt_desc;

static const char ascii85_corrupt_prefix[] = "illegal ascii85 data at input byte ";

#define ASCII85_PREFIX_LEN ((Int)sizeof(ascii85_corrupt_prefix) - 1)

/* Writes the message for e to p, or only counts it when p is NULL, and
 * returns its length. */
static Int ascii85_corrupt_message(Byte *p, Ascii85CorruptInputError e) {
    Byte digits[20];
    Int n = 0;
    uint64_t u = e < 0 ? (uint64_t)0 - (uint64_t)e : (uint64_t)e;
    do {
        digits[n++] = (Byte)('0' + (int)(u % 10));
        u /= 10;
    } while (u != 0);
    Int len = ASCII85_PREFIX_LEN + (e < 0) + n;
    if (p != NULL) {
        memcpy(p, ascii85_corrupt_prefix, (size_t)ASCII85_PREFIX_LEN);
        p += ASCII85_PREFIX_LEN;
        if (e < 0)
            *p++ = '-';
        while (n > 0)
            *p++ = digits[--n];
    }
    return len;
}

static Str ascii85_corrupt_text(const void *self) {
    return ((const Ascii85CorruptBox *)self)->message;
}

static Error ascii85_corrupt_clone(const void *self, Alloc *a) {
    return ascii85_corrupt_input_error_as_error(((const Ascii85CorruptBox *)self)->off,
                                                a);
}

static const ErrorVT ascii85_corrupt_vt = {
    .self_type = &ascii85_corrupt_desc,
    .message = ascii85_corrupt_text,
    .clone = ascii85_corrupt_clone,
};

Error ascii85_corrupt_input_error_as_error(Ascii85CorruptInputError e, Alloc *a) {
    Int mlen = ascii85_corrupt_message(NULL, e);
    Ascii85CorruptBox *b = (Ascii85CorruptBox *)mem_alloc_nozero(
        a, sizeof(Ascii85CorruptBox) + (size_t)mlen, _Alignof(Ascii85CorruptBox));
    if (b == NULL)
        return burrow_err_out_of_memory;
    Byte *p = (Byte *)(b + 1);
    ascii85_corrupt_message(p, e);
    b->off = e;
    b->message = str_from_bytes(p, mlen);
    return (Error){&ascii85_corrupt_vt, b};
}

Str ascii85_corrupt_input_error_error(Ascii85CorruptInputError e, Alloc *a) {
    Int mlen = ascii85_corrupt_message(NULL, e);
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)mlen, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    ascii85_corrupt_message(p, e);
    return str_from_bytes(p, mlen);
}

/* What decoding reports, in the error arena. */
static Error ascii85_corrupt(Int off) {
    return ascii85_corrupt_input_error_as_error((int64_t)off, error_allocator());
}

/* ---------------------------------------------------------------- encoding */

Int ascii85_max_encoded_len(Int n) {
    /* Go's int wraps on overflow and C's may not. */
    Int m = (Int)((uint64_t)n + 3) / 4;
    return (Int)((uint64_t)m * 5);
}

/* Encodes src into dst, which the caller has checked, one group at a time. */
static Int ascii85_encode_raw(Byte *d, Int dl, const Byte *s, Int sl) {
    Int n = 0;
    while (sl > 0) {
        /* Go clears dst[0] to dst[4] first, so a short dst fails at its end. */
        if (dl < 5)
            runtime_index_out_of_range(dl, dl);

        uint32_t v = 0;
        switch (sl) {
        default:
            v |= (uint32_t)s[3];
            /* fall through */
        case 3:
            v |= (uint32_t)s[2] << 8;
            /* fall through */
        case 2:
            v |= (uint32_t)s[1] << 16;
            /* fall through */
        case 1:
            v |= (uint32_t)s[0] << 24;
        }

        /* Four zero bytes are 'z', where Go leaves the other four cleared. */
        if (v == 0 && sl >= 4) {
            d[0] = 'z';
            d[1] = d[2] = d[3] = d[4] = 0;
            d++;
            dl--;
            s += 4;
            sl -= 4;
            n++;
            continue;
        }

        /* Split at 85 squared so the two halves divide down side by side
         * instead of in one chain of five. */
        uint32_t hi = v / 7225, lo = v % 7225;
        d[4] = (Byte)('!' + lo % 85);
        d[3] = (Byte)('!' + lo / 85);
        d[2] = (Byte)('!' + hi % 85);
        hi /= 85;
        d[1] = (Byte)('!' + hi % 85);
        d[0] = (Byte)('!' + hi / 85);

        /* A short group at the end keeps one character more than it has
         * bytes. */
        Int m = 5;
        if (sl < 4) {
            m -= 4 - sl;
            sl = 0;
        } else {
            s += 4;
            sl -= 4;
        }
        d += m;
        dl -= m;
        n += m;
    }
    return n;
}

Int ascii85_encode(Slice dst, Slice src) {
    if (src.len == 0)
        return 0;
    return ascii85_encode_raw((Byte *)dst.p, dst.len, (const Byte *)src.p, src.len);
}

#define ASCII85_OUT_SIZE 1024

typedef struct Ascii85Encoder {
    Error err;
    IoWriter w;
    Byte buf[4]; /* input waiting for a whole group */
    Int nbuf;
    Byte out[ASCII85_OUT_SIZE];
} Ascii85Encoder;

static Error ascii85_put(IoWriter w, Byte *p, Int n) {
    Error err = BURROW_NO_ERROR;
    w.vt->write(w.data, slice_from(p, n, n, TYPE_BYTE), &err);
    return err;
}

static Int ascii85_encoder_write(void *self, Slice p, Error *err) {
    Ascii85Encoder *e = (Ascii85Encoder *)self;
    if (BURROW_FAILED(e->err)) {
        BURROW_OUT(err, e->err);
        return 0;
    }
    const Byte *src = (const Byte *)p.p;
    Int left = p.len, n = 0;

    /* Top up a group started by the last write. */
    if (e->nbuf > 0) {
        Int i;
        for (i = 0; i < left && e->nbuf < 4; i++)
            e->buf[e->nbuf++] = src[i];
        n += i;
        src += i;
        left -= i;
        if (e->nbuf < 4) {
            BURROW_OUT(err, BURROW_NO_ERROR);
            return n;
        }
        Int nout = ascii85_encode_raw(e->out, ASCII85_OUT_SIZE, e->buf, 4);
        e->err = ascii85_put(e->w, e->out, nout);
        if (BURROW_FAILED(e->err)) {
            BURROW_OUT(err, e->err);
            return n;
        }
        e->nbuf = 0;
    }

    /* Whole groups, a buffer at a time. */
    while (left >= 4) {
        Int nn = (Int)ASCII85_OUT_SIZE / 5 * 4;
        if (nn > left)
            nn = left;
        nn -= nn % 4;
        if (nn > 0) {
            Int nout = ascii85_encode_raw(e->out, ASCII85_OUT_SIZE, src, nn);
            e->err = ascii85_put(e->w, e->out, nout);
            if (BURROW_FAILED(e->err)) {
                BURROW_OUT(err, e->err);
                return n;
            }
        }
        n += nn;
        src += nn;
        left -= nn;
    }

    /* Keep what is left for next time. */
    for (Int i = 0; i < left; i++)
        e->buf[i] = src[i];
    e->nbuf = left;
    n += left;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return n;
}

static Error ascii85_encoder_close(void *self) {
    Ascii85Encoder *e = (Ascii85Encoder *)self;
    if (BURROW_OK(e->err) && e->nbuf > 0) {
        Int nout = ascii85_encode_raw(e->out, ASCII85_OUT_SIZE, e->buf, e->nbuf);
        e->nbuf = 0;
        e->err = ascii85_put(e->w, e->out, nout);
    }
    return e->err;
}

static const IoWriteCloserVT ascii85_encoder_vt = {
    {NULL, ascii85_encoder_write},
    {NULL, ascii85_encoder_close},
};

IoWriteCloser ascii85_new_encoder(Alloc *a, IoWriter w) {
    Ascii85Encoder *e = BURROW_NEW(a, Ascii85Encoder);
    if (e == NULL)
        return (IoWriteCloser){NULL, NULL};
    e->w = w;
    return (IoWriteCloser){&ascii85_encoder_vt, e};
}

/* ---------------------------------------------------------------- decoding */

/* binary.BigEndian.PutUint32, as one store. */
static inline void ascii85_put_be32(Byte *p, uint32_t v) {
#if BURROW_LITTLE_ENDIAN
    v = bits_reverse_bytes32(v);
#endif
    memcpy(p, &v, 4);
}

Int ascii85_decode(Slice dst, Slice src, bool flush, Int *nsrc, Error *err) {
    Byte *d = (Byte *)dst.p;
    const Byte *s = (const Byte *)src.p;
    uint32_t v = 0;
    Int nb = 0, ndst = 0, ns = 0;
    Error e = BURROW_NO_ERROR;

    for (Int i = 0; i < src.len; i++) {
        if (dst.len - ndst < 4)
            goto out;

        /* Five digits in a row, which is most of any input, make a group in one
         * go. Anything else takes the loop below. */
        if (nb == 0 && src.len - i >= 5) {
            uint32_t c0 = (uint32_t)s[i] - '!', c1 = (uint32_t)s[i + 1] - '!',
                     c2 = (uint32_t)s[i + 2] - '!', c3 = (uint32_t)s[i + 3] - '!',
                     c4 = (uint32_t)s[i + 4] - '!';
            if (c0 < 85 && c1 < 85 && c2 < 85 && c3 < 85 && c4 < 85) {
                uint32_t w = (((c0 * 85 + c1) * 85 + c2) * 85 + c3) * 85 + c4;
                ascii85_put_be32(d + ndst, w);
                ndst += 4;
                i += 4;
                ns = i + 1;
                continue;
            }
        }

        Byte b = s[i];
        if (b <= ' ')
            continue;
        if (b == 'z' && nb == 0) {
            nb = 5;
            v = 0;
        } else if ('!' <= b && b <= 'u') {
            /* Five characters past "s8W-!" wrap, and so does Go's uint32. */
            v = v * 85 + (uint32_t)(b - '!');
            nb++;
        } else {
            e = ascii85_corrupt(i);
            ndst = ns = 0;
            goto out;
        }
        if (nb == 5) {
            ns = i + 1;
            ascii85_put_be32(d + ndst, v);
            ndst += 4;
            nb = 0;
            v = 0;
        }
    }

    if (flush) {
        ns = src.len;
        if (nb > 0) {
            /* A last group of n+1 characters is n bytes, so one alone is not
             * any. */
            if (nb == 1) {
                e = ascii85_corrupt(src.len);
                ndst = ns = 0;
                goto out;
            }
            for (Int i = nb; i < 5; i++)
                v = v * 85 + 84;
            for (Int i = 0; i < nb - 1; i++) {
                d[ndst++] = (Byte)(v >> 24);
                v <<= 8;
            }
        }
    }
out:
    if (nsrc != NULL)
        *nsrc = ns;
    BURROW_OUT(err, e);
    return ndst;
}

typedef struct Ascii85Decoder {
    Error err;
    Error read_err; /* what r last said */
    IoReader r;
    Byte buf[1024]; /* input not yet decoded */
    Int nbuf;
    Int out_off, out_len; /* decoded output not yet read, in outbuf */
    Byte outbuf[1024];
} Ascii85Decoder;

static Int ascii85_decoder_read(void *self, Slice p, Error *err) {
    Ascii85Decoder *d = (Ascii85Decoder *)self;
    if (p.len == 0) {
        BURROW_OUT(err, BURROW_NO_ERROR);
        return 0;
    }
    if (BURROW_FAILED(d->err)) {
        BURROW_OUT(err, d->err);
        return 0;
    }

    for (;;) {
        /* Output decoded last time and not yet read. */
        if (d->out_len > 0) {
            Int n = p.len < d->out_len ? p.len : d->out_len;
            memcpy(p.p, d->outbuf + d->out_off, (size_t)n);
            d->out_off += n;
            d->out_len -= n;
            BURROW_OUT(err, BURROW_NO_ERROR);
            return n;
        }

        /* Decode what there is, all of it once r has nothing more. */
        if (d->nbuf > 0) {
            Int nsrc;
            Int ndst = ascii85_decode(slice_from(d->outbuf, 1024, 1024, TYPE_BYTE),
                                      slice_from(d->buf, d->nbuf, d->nbuf, TYPE_BYTE),
                                      BURROW_FAILED(d->read_err), &nsrc, &d->err);
            if (ndst > 0) {
                d->out_off = 0;
                d->out_len = ndst;
                d->nbuf -= nsrc;
                memmove(d->buf, d->buf + nsrc, (size_t)d->nbuf);
                continue; /* copy out and return */
            }
            if (BURROW_OK(d->err)) {
                /* Nothing whole yet: drop the spaces so the buffer does not
                 * fill up with them. */
                Int off = 0;
                for (Int i = 0; i < d->nbuf; i++)
                    if (d->buf[i] > ' ')
                        d->buf[off++] = d->buf[i];
                d->nbuf = off;
            }
        }

        if (BURROW_FAILED(d->err)) {
            BURROW_OUT(err, d->err);
            return 0;
        }
        if (BURROW_FAILED(d->read_err)) {
            d->err = d->read_err;
            BURROW_OUT(err, d->err);
            return 0;
        }

        /* Read some more. */
        Int room = (Int)sizeof d->buf - d->nbuf;
        Error rerr = BURROW_NO_ERROR;
        Int nn = d->r.vt->read(
            d->r.data, slice_from(d->buf + d->nbuf, room, room, TYPE_BYTE), &rerr);
        d->read_err = rerr;
        d->nbuf += nn;
    }
}

static const IoReaderVT ascii85_decoder_vt = {NULL, ascii85_decoder_read};

IoReader ascii85_new_decoder(Alloc *a, IoReader r) {
    Ascii85Decoder *d = BURROW_NEW(a, Ascii85Decoder);
    if (d == NULL)
        return (IoReader){NULL, NULL};
    d->r = r;
    return (IoReader){&ascii85_decoder_vt, d};
}

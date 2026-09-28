/* mime: a port of Go's encodedword.go.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "internal.h"

#include "burrow/bytes.h"
#include "burrow/encoding/base64.h"
#include "burrow/fmt.h"
#include "burrow/mem/arena.h"
#include "burrow/strings.h"
#include "burrow/utf8.h"

static const Str mime_invalid_word__text = {
    (const Byte *)"mime: invalid RFC 2047 encoded-word", 35};
static const Error mime_invalid_word = {&burrow_sentinel_error_vt,
                                        &mime_invalid_word__text};

static const Type mime_word_encoder_desc = {
    {(const Byte *)"WordEncoder", 11},
    {(const Byte *)"mime", 4},
    KIND_UINT8,
    (uint32_t)sizeof(MimeWordEncoder),
    (uint16_t)_Alignof(MimeWordEncoder),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x6d77656eU, /* "mwen" */
    NULL,
};

const Type *const TYPE_MIME_WORD_ENCODER = &mime_word_encoder_desc;

static const Type mime_word_decoder_desc = {
    {(const Byte *)"WordDecoder", 11},
    {(const Byte *)"mime", 4},
    KIND_STRUCT,
    (uint32_t)sizeof(MimeWordDecoder),
    (uint16_t)_Alignof(MimeWordDecoder),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x6d776465U, /* "mwde" */
    NULL,
};

const Type *const TYPE_MIME_WORD_DECODER = &mime_word_decoder_desc;

/* A strings builder that remembers whether a write failed. */
typedef struct MimeWordBuf {
    StringsBuilder b;
    bool oom;
} MimeWordBuf;

static void mw_putc(MimeWordBuf *b, Byte c) {
    if (BURROW_FAILED(strings_builder_write_byte(&b->b, c)))
        b->oom = true;
}

static void mw_put(MimeWordBuf *b, Str s) {
    Error err;
    if (strings_builder_write_string(&b->b, s, &err) != s.len)
        b->oom = true;
}

static void mw_put_rune(MimeWordBuf *b, Rune r) {
    Error err;
    strings_builder_write_rune(&b->b, r, &err);
    if (BURROW_FAILED(err))
        b->oom = true;
}

static Str mw_sub(Str s, Int from, Int to) {
    Str r = {s.p + from, to - from};
    return r;
}

/* ---------------------------------------------------------------- encoding */

enum {
    /* The longest an encoded word may be, from RFC 2047 section 2. */
    MIME_MAX_ENCODED_WORD_LEN = 75,
    /* What is left for the text once =?UTF-8?q? and ?= are taken off. */
    MIME_MAX_CONTENT_LEN = MIME_MAX_ENCODED_WORD_LEN - 10 - 2,
    /* base64.StdEncoding.DecodedLen(MIME_MAX_CONTENT_LEN). */
    MIME_MAX_BASE64_LEN = MIME_MAX_CONTENT_LEN / 4 * 3,
};

static const char mw_upperhex[] = "0123456789ABCDEF";

static bool mw_is_utf8(Str charset) {
    return strings_equal_fold(charset, BURROW_S("UTF-8"));
}

static void mw_open_word(MimeWordBuf *b, MimeWordEncoder e, Str charset) {
    mw_put(b, BURROW_S("=?"));
    mw_put(b, charset);
    mw_putc(b, '?');
    mw_putc(b, e);
    mw_putc(b, '?');
}

static void mw_close_word(MimeWordBuf *b) {
    mw_put(b, BURROW_S("?="));
}

static void mw_split_word(MimeWordBuf *b, MimeWordEncoder e, Str charset) {
    mw_close_word(b);
    mw_putc(b, ' ');
    mw_open_word(b, e, charset);
}

/* One piece of the text as base64 with its padding, the way Go's encoder does
 * it between one Close and the next. */
static void mw_base64_piece(MimeWordBuf *b, Str s) {
    Byte chunk[4];
    for (Int i = 0; i < s.len; i += 3) {
        Int n = s.len - i < 3 ? s.len - i : 3;
        base64_encoding_encode(
            base64_std_encoding, slice_from(chunk, 4, 4, TYPE_BYTE),
            slice_from((void *)(uintptr_t)(s.p + i), n, n, TYPE_BYTE));
        mw_put(b, (Str){chunk, 4});
    }
}

static void mw_b_encode(MimeWordBuf *b, MimeWordEncoder e, Str charset, Str s) {
    /* Nothing is split unless the charset is UTF-8 and the text is too long
     * for one word. */
    if (!mw_is_utf8(charset) ||
        base64_encoding_encoded_len(base64_std_encoding, s.len) <=
            MIME_MAX_CONTENT_LEN) {
        mw_base64_piece(b, s);
        return;
    }

    Int current_len = 0, last = 0, rune_len;
    for (Int i = 0; i < s.len; i += rune_len) {
        /* A character must not be split across two words, RFC 2047 section
         * 5.3. */
        utf8_decode_rune_in_string(mw_sub(s, i, s.len), &rune_len);
        if (current_len + rune_len <= MIME_MAX_BASE64_LEN) {
            current_len += rune_len;
        } else {
            mw_base64_piece(b, mw_sub(s, last, i));
            mw_split_word(b, e, charset);
            last = i;
            current_len = rune_len;
        }
    }
    mw_base64_piece(b, mw_sub(s, last, s.len));
}

static void mw_write_q_string(MimeWordBuf *b, Str s) {
    for (Int i = 0; i < s.len; i++) {
        Byte c = s.p[i];
        if (c == ' ') {
            mw_putc(b, '_');
        } else if (c >= '!' && c <= '~' && c != '=' && c != '?' && c != '_') {
            mw_putc(b, c);
        } else {
            mw_putc(b, '=');
            mw_putc(b, (Byte)mw_upperhex[c >> 4]);
            mw_putc(b, (Byte)mw_upperhex[c & 0x0f]);
        }
    }
}

static void mw_q_encode(MimeWordBuf *b, MimeWordEncoder e, Str charset, Str s) {
    /* Words are only split when the charset is UTF-8. */
    if (!mw_is_utf8(charset)) {
        mw_write_q_string(b, s);
        return;
    }

    Int current_len = 0, rune_len;
    for (Int i = 0; i < s.len; i += rune_len) {
        Byte c = s.p[i];
        Int enc_len;
        if (c >= ' ' && c <= '~' && c != '=' && c != '?' && c != '_') {
            rune_len = 1;
            enc_len = 1;
        } else {
            utf8_decode_rune_in_string(mw_sub(s, i, s.len), &rune_len);
            enc_len = 3 * rune_len;
        }
        if (current_len + enc_len > MIME_MAX_CONTENT_LEN) {
            mw_split_word(b, e, charset);
            current_len = 0;
        }
        mw_write_q_string(b, mw_sub(s, i, i + rune_len));
        current_len += enc_len;
    }
}

Str mime_word_encoder_encode(MimeWordEncoder e, Alloc *a, Str charset, Str s) {
    if (!burrow__mime_needs_encoding(s))
        return s;
    Arena scratch;
    arena_init(&scratch, a, 0);
    MimeWordBuf b = {STRINGS_BUILDER(arena_allocator(&scratch)), false};
    strings_builder_grow(&b.b, 48);
    mw_open_word(&b, e, charset);
    if (e == MIME_B_ENCODING)
        mw_b_encode(&b, e, charset, s);
    else
        mw_q_encode(&b, e, charset, s);
    mw_close_word(&b);
    Str out = {NULL, 0};
    if (!b.oom)
        out = strings_clone(a, strings_builder_string(&b.b));
    arena_free(&scratch);
    return out;
}

/* ---------------------------------------------------------------- decoding */

static bool mw_from_hex(Byte c, Byte *v, Error *err) {
    if (c >= '0' && c <= '9') {
        *v = (Byte)(c - '0');
        return true;
    }
    if (c >= 'A' && c <= 'F') {
        *v = (Byte)(c - 'A' + 10);
        return true;
    }
    /* Accept badly encoded bytes. */
    if (c >= 'a' && c <= 'f') {
        *v = (Byte)(c - 'a' + 10);
        return true;
    }
    *err = fmt_errorf_v("mime: invalid hex byte %#02x", c);
    return false;
}

/* qDecode, into sa. */
static Slice mw_q_decode(Alloc *sa, Str s, Error *err) {
    Slice none = {0};
    Byte *dec = (Byte *)mem_alloc_nozero(sa, (size_t)(s.len > 0 ? s.len : 1), 1);
    if (dec == NULL) {
        *err = burrow__mime_err_no_memory;
        return none;
    }
    Int n = 0;
    for (Int i = 0; i < s.len; i++) {
        Byte c = s.p[i];
        if (c == '_') {
            dec[n] = ' ';
        } else if (c == '=') {
            if (i + 2 >= s.len) {
                *err = mime_invalid_word;
                return none;
            }
            Byte hb, lb;
            if (!mw_from_hex(s.p[i + 1], &hb, err) ||
                !mw_from_hex(s.p[i + 2], &lb, err))
                return none;
            dec[n] = (Byte)(hb << 4 | lb);
            i += 2;
        } else if ((c <= '~' && c >= ' ') || c == '\n' || c == '\r' || c == '\t') {
            dec[n] = c;
        } else {
            *err = mime_invalid_word;
            return none;
        }
        n++;
    }
    return slice_from(dec, n, n, TYPE_BYTE);
}

/* decode: the bytes of an encoded word's text, into sa. */
static Slice mw_decode(Alloc *sa, Byte encoding, Str text, Error *err) {
    Slice none = {0};
    *err = BURROW_NO_ERROR;
    switch (encoding) {
    case 'B':
    case 'b':
        return base64_encoding_decode_string(base64_std_encoding, sa, text, err);
    case 'Q':
    case 'q':
        return mw_q_decode(sa, text, err);
    default:
        *err = mime_invalid_word;
        return none;
    }
}

static Error mw_convert(const MimeWordDecoder *d, Alloc *sa, MimeWordBuf *b,
                        Str charset, Slice content) {
    const Byte *p = (const Byte *)content.p;
    if (strings_equal_fold(BURROW_S("utf-8"), charset)) {
        mw_put(b, (Str){p, content.len});
    } else if (strings_equal_fold(BURROW_S("iso-8859-1"), charset)) {
        for (Int i = 0; i < content.len; i++)
            mw_put_rune(b, (Rune)p[i]);
    } else if (strings_equal_fold(BURROW_S("us-ascii"), charset)) {
        for (Int i = 0; i < content.len; i++) {
            if (p[i] >= UTF8_RUNE_SELF)
                mw_put_rune(b, UTF8_RUNE_ERROR);
            else
                mw_putc(b, p[i]);
        }
    } else {
        if (d == NULL || BURROW_FUNC_IS_NIL(d->charset_reader))
            return fmt_errorf_v("mime: unhandled charset %q", charset);
        Str lower = strings_to_lower(sa, charset);
        if (lower.p == NULL && charset.len > 0)
            return burrow__mime_err_no_memory;
        BytesReader br;
        bytes_reader_reset(&br, content);
        Error err = BURROW_NO_ERROR;
        IoReader r = d->charset_reader.f(d->charset_reader.env, lower,
                                         bytes_reader_as_io_reader(&br), &err);
        if (BURROW_FAILED(err))
            return err;
        io_copy(sa, strings_builder_as_io_writer(&b->b), r, &err);
        if (BURROW_FAILED(err))
            return err;
    }
    if (b->oom)
        return burrow__mime_err_no_memory;
    return BURROW_NO_ERROR;
}

/* The result of b, copied into a, and the scratch arena let go of. */
static Str mw_finish(Arena *scratch, MimeWordBuf *b, Alloc *a, Error *err) {
    Str none = {NULL, 0};
    Str built = strings_builder_string(&b->b);
    Str out = strings_clone(a, built);
    arena_free(scratch);
    if (out.p == NULL && built.len > 0) {
        *err = burrow__mime_err_no_memory;
        return none;
    }
    return out;
}

Str mime_word_decoder_decode(const MimeWordDecoder *d, Alloc *a, Str word, Error *err) {
    Str none = {NULL, 0};
    *err = BURROW_NO_ERROR;
    /* RFC 2047 section 2. The decoder is permissive and takes empty text. */
    if (word.len < 8 || !strings_has_prefix(word, BURROW_S("=?")) ||
        !strings_has_suffix(word, BURROW_S("?=")) ||
        strings_count(word, BURROW_S("?")) != 4) {
        *err = mime_invalid_word;
        return none;
    }
    word = mw_sub(word, 2, word.len - 2);

    /* "UTF-8?q?text" into "UTF-8", 'q' and "text". */
    bool found;
    Str text;
    Str charset = strings_cut(word, BURROW_S("?"), &text, &found);
    if (charset.len == 0) {
        *err = mime_invalid_word;
        return none;
    }
    Str encoding = strings_cut(text, BURROW_S("?"), &text, &found);
    if (encoding.len != 1) {
        *err = mime_invalid_word;
        return none;
    }

    Arena scratch;
    arena_init(&scratch, a, 0);
    Alloc *sa = arena_allocator(&scratch);
    Slice content = mw_decode(sa, encoding.p[0], text, err);
    if (BURROW_FAILED(*err)) {
        arena_free(&scratch);
        return none;
    }
    MimeWordBuf b = {STRINGS_BUILDER(sa), false};
    Error e = mw_convert(d, sa, &b, charset, content);
    if (BURROW_FAILED(e)) {
        arena_free(&scratch);
        *err = e;
        return none;
    }
    return mw_finish(&scratch, &b, a, err);
}

/* Whether s, which is ASCII, has a byte that is not the white space allowed
 * between encoded words. A vertical tab is not. */
static bool mw_has_non_whitespace(Str s) {
    for (Int i = 0; i < s.len; i++) {
        Byte c = s.p[i];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
            return true;
    }
    return false;
}

Str mime_word_decoder_decode_header(const MimeWordDecoder *d, Alloc *a, Str header,
                                    Error *err) {
    Str none = {NULL, 0};
    *err = BURROW_NO_ERROR;
    /* No encoded word, so nothing to build. */
    Int i = strings_index(header, BURROW_S("=?"));
    if (i == -1)
        return header;

    Arena scratch;
    arena_init(&scratch, a, 0);
    Alloc *sa = arena_allocator(&scratch);
    MimeWordBuf b = {STRINGS_BUILDER(sa), false};
    mw_put(&b, mw_sub(header, 0, i));
    header = mw_sub(header, i, header.len);

    bool between_words = false;
    for (;;) {
        Int start = strings_index(header, BURROW_S("=?"));
        if (start == -1)
            break;
        Int cur = start + 2;

        Int j = strings_index(mw_sub(header, cur, header.len), BURROW_S("?"));
        if (j == -1)
            break;
        Str charset = mw_sub(header, cur, cur + j);
        cur += j + 1;

        if (header.len < cur + 4) /* "Q??=" */
            break;
        Byte encoding = header.p[cur];
        cur++;

        if (header.p[cur] != '?')
            break;
        cur++;

        j = strings_index(mw_sub(header, cur, header.len), BURROW_S("?="));
        if (j == -1)
            break;
        Str text = mw_sub(header, cur, cur + j);
        Int end = cur + j + 2;

        Error derr;
        Slice content = mw_decode(sa, encoding, text, &derr);
        if (BURROW_FAILED(derr)) {
            if (errors_is(derr, burrow__mime_err_no_memory)) {
                arena_free(&scratch);
                *err = derr;
                return none;
            }
            between_words = false;
            mw_put(&b, mw_sub(header, 0, end));
            header = mw_sub(header, end, header.len);
            continue;
        }

        /* What comes before the word is kept, unless it is only the white
         * space between two encoded words, which is taken out. */
        if (start > 0 &&
            (!between_words || mw_has_non_whitespace(mw_sub(header, 0, start))))
            mw_put(&b, mw_sub(header, 0, start));

        Error e = mw_convert(d, sa, &b, charset, content);
        if (BURROW_FAILED(e)) {
            arena_free(&scratch);
            *err = e;
            return none;
        }

        header = mw_sub(header, end, header.len);
        between_words = true;
    }

    if (header.len > 0)
        mw_put(&b, header);
    if (b.oom) {
        arena_free(&scratch);
        *err = burrow__mime_err_no_memory;
        return none;
    }
    return mw_finish(&scratch, &b, a, err);
}

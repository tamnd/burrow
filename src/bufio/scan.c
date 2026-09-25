/* Derived from Go's src/bufio/scan.go.
 * Go source: go1.27.1.
 *
 * Copyright 2013 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/bufio.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/runtime.h"
#include "burrow/slice.h"
#include "burrow/type.h"
#include "burrow/utf8.h"

#include <stdint.h>
#include <string.h>

enum {
    SCAN_MAX_CONSECUTIVE_EMPTY_READS = 100,
    /* Size of initial allocation for buffer. */
    SCAN_START_BUF_SIZE = 4096,
};

BURROW_SENTINEL_ERROR(bufio_err_too_long, "bufio.Scanner: token too long");
BURROW_SENTINEL_ERROR(bufio_err_negative_advance,
                      "bufio.Scanner: SplitFunc returns negative advance count");
BURROW_SENTINEL_ERROR(bufio_err_advance_too_far,
                      "bufio.Scanner: SplitFunc returns advance count beyond input");
BURROW_SENTINEL_ERROR(bufio_err_bad_read_count,
                      "bufio.Scanner: Read returned impossible count");
BURROW_SENTINEL_ERROR(bufio_err_final_token, "final token");

static bool scan_same_error(Error a, Error b) {
    return a.vt == b.vt && a.data == b.data;
}

static Slice scan_sub(Slice data, Int lo, Int hi) {
    return slice_from((Byte *)data.p + lo, hi - lo, hi - lo, TYPE_BYTE);
}

/* ---------------------------------------------------------- split functions */

Int bufio_scan_bytes(Slice data, bool at_eof, Slice *token, Error *err) {
    (void)err;
    if (at_eof && data.len == 0) {
        *token = slice_nil(TYPE_BYTE);
        return 0;
    }
    *token = scan_sub(data, 0, 1);
    return 1;
}

/* The encoding of utf8.RuneError, handed out for bytes that are not UTF-8. */
static const Byte scan_error_rune[3] = {0xEF, 0xBF, 0xBD};

Int bufio_scan_runes(Slice data, bool at_eof, Slice *token, Error *err) {
    (void)err;
    *token = slice_nil(TYPE_BYTE);
    if (at_eof && data.len == 0)
        return 0;

    /* Fast path 1: ASCII. */
    const Byte *p = (const Byte *)data.p;
    if (p[0] < UTF8_RUNE_SELF) {
        *token = scan_sub(data, 0, 1);
        return 1;
    }

    /* Fast path 2: Correct UTF-8 decode without error. */
    Int width = 0;
    (void)utf8_decode_rune(data, &width);
    if (width > 1) {
        /* It's a valid encoding. Width cannot be one for a correctly encoded
         * non-ASCII rune. */
        *token = scan_sub(data, 0, width);
        return width;
    }

    /* We know it's an error: we have width==1 and implicitly r==utf8.RuneError.
     * Is the error because there wasn't a full rune to be decoded? FullRune
     * distinguishes correctly between erroneous and incomplete encodings. */
    if (!at_eof && !utf8_full_rune(data)) {
        /* Incomplete; get more bytes. */
        return 0;
    }

    /* We have a real UTF-8 encoding error. Return a properly encoded error
     * rune but advance only one byte. This matches the behavior of a range
     * loop over an incorrectly encoded string. */
    *token = slice_from((void *)(uintptr_t)scan_error_rune, 3, 3, TYPE_BYTE);
    return 1;
}

/* Drops a terminal \r from the data. */
static Slice scan_drop_cr(Slice data) {
    if (data.len > 0 && ((const Byte *)data.p)[data.len - 1] == '\r')
        return scan_sub(data, 0, data.len - 1);
    return data;
}

Int bufio_scan_lines(Slice data, bool at_eof, Slice *token, Error *err) {
    (void)err;
    *token = slice_nil(TYPE_BYTE);
    if (at_eof && data.len == 0)
        return 0;
    const Byte *nl =
        data.len > 0 ? (const Byte *)memchr(data.p, '\n', (size_t)data.len) : NULL;
    if (nl != NULL) {
        /* We have a full newline-terminated line. */
        Int i = (Int)(nl - (const Byte *)data.p);
        *token = scan_drop_cr(scan_sub(data, 0, i));
        return i + 1;
    }
    /* If we're at EOF, we have a final, non-terminated line. Return it. */
    if (at_eof) {
        *token = scan_drop_cr(data);
        return data.len;
    }
    /* Request more data. */
    return 0;
}

/* bufio's isSpace, which reports whether the character is a Unicode white
 * space character. It avoids the tables of unicode.IsSpace, as Go does. */
bool burrow__bufio_is_space(Rune r) {
    if (r <= 0x00FF) {
        /* Obvious ASCII ones: \t through \r plus space. Plus two Latin-1 oddballs. */
        switch (r) {
        case ' ':
        case '\t':
        case '\n':
        case '\v':
        case '\f':
        case '\r':
        case 0x0085:
        case 0x00A0:
            return true;
        default:
            return false;
        }
    }
    /* High-valued ones. */
    if (0x2000 <= r && r <= 0x200a)
        return true;
    switch (r) {
    case 0x1680:
    case 0x2028:
    case 0x2029:
    case 0x202f:
    case 0x205f:
    case 0x3000:
        return true;
    default:
        return false;
    }
}

Int bufio_scan_words(Slice data, bool at_eof, Slice *token, Error *err) {
    (void)err;
    *token = slice_nil(TYPE_BYTE);
    /* Skip leading spaces. */
    Int start = 0;
    for (Int width = 0; start < data.len; start += width) {
        Rune r = utf8_decode_rune(scan_sub(data, start, data.len), &width);
        if (!burrow__bufio_is_space(r))
            break;
    }
    /* Scan until space, marking end of word. */
    for (Int width = 0, i = start; i < data.len; i += width) {
        Rune r = utf8_decode_rune(scan_sub(data, i, data.len), &width);
        if (burrow__bufio_is_space(r)) {
            *token = scan_sub(data, start, i);
            return i + width;
        }
    }
    /* If we're at EOF, we have a final, non-empty, non-terminated word. Return
     * it. */
    if (at_eof && data.len > start) {
        *token = scan_sub(data, start, data.len);
        return data.len;
    }
    /* Request more data. */
    return start;
}

static Int scan_split_bytes(void *env, Slice data, bool at_eof, Slice *token,
                            Error *err) {
    (void)env;
    return bufio_scan_bytes(data, at_eof, token, err);
}

static Int scan_split_runes(void *env, Slice data, bool at_eof, Slice *token,
                            Error *err) {
    (void)env;
    return bufio_scan_runes(data, at_eof, token, err);
}

static Int scan_split_lines(void *env, Slice data, bool at_eof, Slice *token,
                            Error *err) {
    (void)env;
    return bufio_scan_lines(data, at_eof, token, err);
}

static Int scan_split_words(void *env, Slice data, bool at_eof, Slice *token,
                            Error *err) {
    (void)env;
    return bufio_scan_words(data, at_eof, token, err);
}

const BufioSplitFunc BUFIO_SCAN_BYTES = {scan_split_bytes, NULL};
const BufioSplitFunc BUFIO_SCAN_RUNES = {scan_split_runes, NULL};
const BufioSplitFunc BUFIO_SCAN_LINES = {scan_split_lines, NULL};
const BufioSplitFunc BUFIO_SCAN_WORDS = {scan_split_words, NULL};

/* ------------------------------------------------------------------ Scanner */

BufioScanner *bufio_new_scanner(Alloc *a, IoReader r) {
    BufioScanner *s = (BufioScanner *)mem_alloc(a, sizeof *s, _Alignof(BufioScanner));
    if (s == NULL)
        return NULL;
    s->a = a;
    s->r = r;
    s->split = BUFIO_SCAN_LINES;
    s->max_token_size = BUFIO_MAX_SCAN_TOKEN_SIZE;
    s->token = slice_nil(TYPE_BYTE);
    s->err = BURROW_NO_ERROR;
    return s;
}

void bufio_scanner_free(BufioScanner *s) {
    if (s == NULL)
        return;
    if (s->own_buf && s->buf != NULL)
        mem_free(s->a, s->buf, (size_t)s->buf_size, 1);
    mem_free(s->a, s, sizeof *s, _Alignof(BufioScanner));
}

Error bufio_scanner_err(BufioScanner *s) {
    if (scan_same_error(s->err, io_eof))
        return BURROW_NO_ERROR;
    return s->err;
}

Slice bufio_scanner_bytes(BufioScanner *s) {
    return s->token;
}

Str bufio_scanner_text(BufioScanner *s) {
    return str_from_bytes((const Byte *)s->token.p, s->token.len);
}

/* Records the first non-EOF error. */
static void scan_set_err(BufioScanner *s, Error err) {
    if (BURROW_OK(s->err) || scan_same_error(s->err, io_eof))
        s->err = err;
}

/* Consumes n bytes of the buffer. It reports whether the advance was legal. */
static bool scan_advance(BufioScanner *s, Int n) {
    if (n < 0) {
        scan_set_err(s, bufio_err_negative_advance);
        return false;
    }
    if (n > s->end - s->start) {
        scan_set_err(s, bufio_err_advance_too_far);
        return false;
    }
    s->start += n;
    return true;
}

bool bufio_scanner_scan(BufioScanner *s) {
    if (s->done)
        return false;
    s->scan_called = true;
    /* Loop until we have a token. */
    for (;;) {
        /* See if we can get a token with what we already have. If we've run out
         * of data but have an error, give the split function a chance to
         * recover any remaining, possibly empty token. */
        if (s->end > s->start || BURROW_FAILED(s->err)) {
            Slice data = s->buf == NULL
                             ? slice_nil(TYPE_BYTE)
                             : slice_from(s->buf + s->start, s->end - s->start,
                                          s->end - s->start, TYPE_BYTE);
            Slice token = slice_nil(TYPE_BYTE);
            Error err = BURROW_NO_ERROR;
            Int adv = BURROW_CALLF(s->split, data, BURROW_FAILED(s->err), &token, &err);
            if (BURROW_FAILED(err)) {
                if (scan_same_error(err, bufio_err_final_token)) {
                    s->token = token;
                    s->done = true;
                    /* When token is not nil, it means the scanning stops with
                     * a trailing empty token in the final token. */
                    return token.p != NULL;
                }
                scan_set_err(s, err);
                return false;
            }
            if (!scan_advance(s, adv))
                return false;
            s->token = token;
            if (token.p != NULL) {
                if (BURROW_OK(s->err) || adv > 0) {
                    s->empties = 0;
                } else {
                    /* Returning tokens not advancing input at EOF. */
                    s->empties++;
                    if (s->empties > SCAN_MAX_CONSECUTIVE_EMPTY_READS)
                        runtime_panic(BURROW_S(
                            "bufio.Scan: too many empty tokens without progressing"));
                }
                return true;
            }
        }
        /* We cannot generate a token with what we are holding. If we've already
         * hit EOF or an I/O error, we are done. */
        if (BURROW_FAILED(s->err)) {
            /* Shut it down. */
            s->start = 0;
            s->end = 0;
            return false;
        }
        /* Must read more data. First, shift data to beginning of buffer if
         * there's lots of empty space or space is needed. */
        if (s->buf != NULL && s->start > 0 &&
            (s->end == s->buf_size || s->start > s->buf_size / 2)) {
            memmove(s->buf, s->buf + s->start, (size_t)(s->end - s->start));
            s->end -= s->start;
            s->start = 0;
        }
        /* Is the buffer full? If so, resize. */
        if (s->end == s->buf_size) {
            /* Guarantee no overflow in the multiplication below. */
            if (s->buf_size >= s->max_token_size || s->buf_size > BURROW_INT_MAX / 2) {
                scan_set_err(s, bufio_err_too_long);
                return false;
            }
            Int new_size = s->buf_size * 2;
            if (new_size == 0)
                new_size = SCAN_START_BUF_SIZE;
            if (new_size > s->max_token_size)
                new_size = s->max_token_size;
            Byte *new_buf = (Byte *)mem_alloc_nozero(s->a, (size_t)new_size, 1);
            if (new_buf == NULL) {
                scan_set_err(s, burrow_err_out_of_memory);
                return false;
            }
            if (s->buf != NULL && s->end > s->start)
                memcpy(new_buf, s->buf + s->start, (size_t)(s->end - s->start));
            if (s->own_buf && s->buf != NULL)
                mem_free(s->a, s->buf, (size_t)s->buf_size, 1);
            s->buf = new_buf;
            s->buf_size = new_size;
            s->own_buf = true;
            s->end -= s->start;
            s->start = 0;
        }
        /* Finally we can read some input. Make sure we don't get stuck with a
         * misbehaving Reader. Officially we don't need to do this, but let's
         * be extra careful: Scanner is for safe, simple jobs. */
        for (int loop = 0;;) {
            Error err = BURROW_NO_ERROR;
            Int n = BURROW_CALL(s->r, read,
                                slice_from(s->buf + s->end, s->buf_size - s->end,
                                           s->buf_size - s->end, TYPE_BYTE),
                                &err);
            if (n < 0 || s->buf_size - s->end < n) {
                scan_set_err(s, bufio_err_bad_read_count);
                break;
            }
            s->end += n;
            if (BURROW_FAILED(err)) {
                scan_set_err(s, err);
                break;
            }
            if (n > 0) {
                s->empties = 0;
                break;
            }
            loop++;
            if (loop > SCAN_MAX_CONSECUTIVE_EMPTY_READS) {
                scan_set_err(s, io_err_no_progress);
                break;
            }
        }
    }
}

void bufio_scanner_buffer(BufioScanner *s, Slice buf, Int max) {
    if (s->scan_called)
        runtime_panic(BURROW_S("Buffer called after Scan"));
    if (s->own_buf && s->buf != NULL)
        mem_free(s->a, s->buf, (size_t)s->buf_size, 1);
    s->buf = (Byte *)buf.p;
    s->buf_size = buf.cap;
    s->own_buf = false;
    s->max_token_size = max;
}

void bufio_scanner_split(BufioScanner *s, BufioSplitFunc split) {
    if (s->scan_called)
        runtime_panic(BURROW_S("Split called after Scan"));
    s->split = split;
}

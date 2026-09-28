/* mime/quotedprintable: a port of Go's reader.go and writer.go.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/mime/quotedprintable.h"
#include "burrow/bufio.h"
#include "burrow/fmt.h"

/* ---------------------------------------------------------------- reading */

struct QuotedprintableReader {
    Alloc *a;
    BufioReader *br;
    Error rerr; /* last read error */
    /* To be handed out before more of br. It points into br's buffer, the way
     * Go's slice does, and the CRLF put back after trimming goes there too. */
    Byte *line;
    Int line_len;
};

static const Type qp_reader_desc = {
    {(const Byte *)"Reader", 6},
    {(const Byte *)"mime/quotedprintable", 20},
    KIND_STRUCT,
    (uint32_t)sizeof(QuotedprintableReader),
    (uint16_t)_Alignof(QuotedprintableReader),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x71707264U, /* "qprd" */
    NULL,
};

const Type *const TYPE_QUOTEDPRINTABLE_READER = &qp_reader_desc;

QuotedprintableReader *quotedprintable_new_reader(Alloc *a, IoReader r) {
    QuotedprintableReader *z = (QuotedprintableReader *)mem_alloc(
        a, sizeof *z, _Alignof(QuotedprintableReader));
    if (z == NULL)
        return NULL;
    z->a = a;
    z->br = bufio_new_reader(a, r);
    if (z->br == NULL) {
        mem_free(a, z, sizeof *z, _Alignof(QuotedprintableReader));
        return NULL;
    }
    return z;
}

void quotedprintable_reader_free(QuotedprintableReader *r) {
    if (r == NULL)
        return;
    bufio_reader_free(r->br);
    mem_free(r->a, r, sizeof *r, _Alignof(QuotedprintableReader));
}

static bool qp_from_hex(Byte b, Byte *v, Error *err) {
    if (b >= '0' && b <= '9') {
        *v = (Byte)(b - '0');
        return true;
    }
    if (b >= 'A' && b <= 'F') {
        *v = (Byte)(b - 'A' + 10);
        return true;
    }
    if (b >= 'a' && b <= 'f') {
        *v = (Byte)(b - 'a' + 10);
        return true;
    }
    *err = fmt_errorf_v("quotedprintable: invalid hex byte 0x%02x", b);
    return false;
}

static bool qp_read_hex_byte(const Byte *v, Int n, Byte *b, Error *err) {
    if (n < 2) {
        *err = io_err_unexpected_eof;
        return false;
    }
    Byte hb, lb;
    if (!qp_from_hex(v[0], &hb, err) || !qp_from_hex(v[1], &lb, err))
        return false;
    *b = (Byte)(hb << 4 | lb);
    return true;
}

static bool qp_is_discard_whitespace(Byte b) {
    return b == '\n' || b == '\r' || b == ' ' || b == '\t';
}

/* Splits off the next line of br, trims it the way Go's Read does, and records
 * any error. */
static void qp_next_line(QuotedprintableReader *r) {
    Slice whole = bufio_reader_read_slice(r->br, '\n', &r->rerr);
    Byte *p = (Byte *)whole.p;
    Int n = whole.len;
    bool has_lf = n >= 1 && p[n - 1] == '\n';
    bool has_cr = n >= 2 && p[n - 2] == '\r' && p[n - 1] == '\n';
    Int len = n;
    while (len > 0 && qp_is_discard_whitespace(p[len - 1]))
        len--;
    r->line = p;
    if (len > 0 && p[len - 1] == '=') {
        /* A soft line break. What follows the = has to be a line break, or
         * nothing at all at the end of the input. */
        Int k = len;
        while (k < n && (p[k] == ' ' || p[k] == '\t'))
            k++;
        const Byte *rest = p + k;
        Int rest_len = n - k;
        len--;
        bool lf = rest_len >= 1 && rest[0] == '\n';
        bool crlf = rest_len >= 2 && rest[0] == '\r' && rest[1] == '\n';
        if (!lf && !crlf && !(rest_len == 0 && len > 0 && errors_is(r->rerr, io_eof))) {
            Str s = {rest, rest_len};
            r->rerr = fmt_errorf_v("quotedprintable: invalid bytes after =: %q", s);
        }
    } else if (has_lf) {
        /* The trimmed part held the line break, so there is room for it. */
        if (has_cr)
            p[len++] = '\r';
        p[len++] = '\n';
    }
    r->line_len = len;
}

Int quotedprintable_reader_read(QuotedprintableReader *r, Slice p, Error *err) {
    Byte *dst = (Byte *)p.p;
    Int n = 0;
    *err = BURROW_NO_ERROR;
    while (n < p.len) {
        if (r->line_len == 0) {
            if (BURROW_FAILED(r->rerr)) {
                *err = r->rerr;
                return n;
            }
            qp_next_line(r);
            continue;
        }
        Byte b = r->line[0];
        if (b == '=') {
            Error herr = BURROW_NO_ERROR;
            if (!qp_read_hex_byte(r->line + 1, r->line_len - 1, &b, &herr)) {
                if (r->line_len >= 2 && r->line[1] != '\r' && r->line[1] != '\n') {
                    /* Not an escape, so the = is passed through. */
                    b = '=';
                } else {
                    *err = herr;
                    return n;
                }
            } else {
                r->line += 2; /* 2 of the 3, the other 1 is done below */
                r->line_len -= 2;
            }
        } else if (b == '\t' || b == '\r' || b == '\n' || b >= 0x80) {
            /* passed through */
        } else if (b < ' ' || b > '~') {
            *err = fmt_errorf_v(
                "quotedprintable: invalid unescaped byte 0x%02x in body", b);
            return n;
        }
        dst[n++] = b;
        r->line++;
        r->line_len--;
    }
    return n;
}

static Int qp_reader_vt_read(void *self, Slice p, Error *err) {
    return quotedprintable_reader_read((QuotedprintableReader *)self, p, err);
}

static const IoReaderVT qp_reader_vt = {&qp_reader_desc, qp_reader_vt_read};

IoReader quotedprintable_reader_as_io_reader(QuotedprintableReader *r) {
    return (IoReader){&qp_reader_vt, r};
}

/* ---------------------------------------------------------------- writing */

enum { QP_LINE_MAX_LEN = 76 };

static const Type qp_writer_desc = {
    {(const Byte *)"Writer", 6},
    {(const Byte *)"mime/quotedprintable", 20},
    KIND_STRUCT,
    (uint32_t)sizeof(QuotedprintableWriter),
    (uint16_t)_Alignof(QuotedprintableWriter),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x71707772U, /* "qpwr" */
    NULL,
};

const Type *const TYPE_QUOTEDPRINTABLE_WRITER = &qp_writer_desc;

QuotedprintableWriter *quotedprintable_new_writer(Alloc *a, IoWriter w) {
    QuotedprintableWriter *z = (QuotedprintableWriter *)mem_alloc(
        a, sizeof *z, _Alignof(QuotedprintableWriter));
    if (z == NULL)
        return NULL;
    z->a = a;
    z->w = w;
    return z;
}

void quotedprintable_writer_free(QuotedprintableWriter *w) {
    if (w == NULL)
        return;
    mem_free(w->a, w, sizeof *w, _Alignof(QuotedprintableWriter));
}

static bool qp_is_whitespace(Byte b) {
    return b == ' ' || b == '\t';
}

static Error qp_flush(QuotedprintableWriter *w) {
    Error err = BURROW_NO_ERROR;
    w->w.vt->write(w->w.data, slice_from(w->line, w->i, w->i, TYPE_BYTE), &err);
    if (BURROW_FAILED(err))
        return err;
    w->i = 0;
    return BURROW_NO_ERROR;
}

static Error qp_insert_crlf(QuotedprintableWriter *w) {
    w->line[w->i] = '\r';
    w->line[w->i + 1] = '\n';
    w->i += 2;
    return qp_flush(w);
}

static Error qp_insert_soft_line_break(QuotedprintableWriter *w) {
    w->line[w->i] = '=';
    w->i++;
    return qp_insert_crlf(w);
}

static Error qp_encode(QuotedprintableWriter *w, Byte b) {
    static const char upperhex[] = "0123456789ABCDEF";
    if (QP_LINE_MAX_LEN - 1 - w->i < 3) {
        Error err = qp_insert_soft_line_break(w);
        if (BURROW_FAILED(err))
            return err;
    }
    w->line[w->i] = '=';
    w->line[w->i + 1] = (Byte)upperhex[b >> 4];
    w->line[w->i + 2] = (Byte)upperhex[b & 0x0f];
    w->i += 3;
    return BURROW_NO_ERROR;
}

/* A space or tab at the end of a line would be dropped by a reader, so it is
 * escaped. */
static Error qp_check_last_byte(QuotedprintableWriter *w) {
    if (w->i == 0)
        return BURROW_NO_ERROR;
    Byte b = w->line[w->i - 1];
    if (qp_is_whitespace(b)) {
        w->i--;
        return qp_encode(w, b);
    }
    return BURROW_NO_ERROR;
}

static Error qp_write_raw(QuotedprintableWriter *w, const Byte *p, Int n) {
    for (Int k = 0; k < n; k++) {
        Byte b = p[k];
        if (b == '\n' || b == '\r') {
            if (w->cr && b == '\n') {
                w->cr = false;
                continue;
            }
            if (b == '\r')
                w->cr = true;
            Error err = qp_check_last_byte(w);
            if (BURROW_FAILED(err))
                return err;
            err = qp_insert_crlf(w);
            if (BURROW_FAILED(err))
                return err;
            continue;
        }
        if (w->i == QP_LINE_MAX_LEN - 1) {
            Error err = qp_insert_soft_line_break(w);
            if (BURROW_FAILED(err))
                return err;
        }
        w->line[w->i] = b;
        w->i++;
        w->cr = false;
    }
    return BURROW_NO_ERROR;
}

Int quotedprintable_writer_write(QuotedprintableWriter *w, Slice p, Error *err) {
    const Byte *src = (const Byte *)p.p;
    Int n = 0;
    *err = BURROW_NO_ERROR;
    for (Int i = 0; i < p.len; i++) {
        Byte b = src[i];
        if (b >= '!' && b <= '~' && b != '=')
            continue;
        if (qp_is_whitespace(b) || (!w->binary && (b == '\n' || b == '\r')))
            continue;
        if (i > n) {
            Error e = qp_write_raw(w, src + n, i - n);
            if (BURROW_FAILED(e)) {
                *err = e;
                return n;
            }
            n = i;
        }
        Error e = qp_encode(w, b);
        if (BURROW_FAILED(e)) {
            *err = e;
            return n;
        }
        n++;
    }
    if (n == p.len)
        return n;
    Error e = qp_write_raw(w, src + n, p.len - n);
    if (BURROW_FAILED(e)) {
        *err = e;
        return n;
    }
    return p.len;
}

Error quotedprintable_writer_close(QuotedprintableWriter *w) {
    Error err = qp_check_last_byte(w);
    if (BURROW_FAILED(err))
        return err;
    return qp_flush(w);
}

static Int qp_writer_vt_write(void *self, Slice p, Error *err) {
    return quotedprintable_writer_write((QuotedprintableWriter *)self, p, err);
}

static Error qp_writer_vt_close(void *self) {
    return quotedprintable_writer_close((QuotedprintableWriter *)self);
}

static const IoWriteCloserVT qp_write_closer_vt = {
    {&qp_writer_desc, qp_writer_vt_write},
    {&qp_writer_desc, qp_writer_vt_close},
};

IoWriter quotedprintable_writer_as_io_writer(QuotedprintableWriter *w) {
    return (IoWriter){&qp_write_closer_vt.writer, w};
}

IoWriteCloser quotedprintable_writer_as_io_write_closer(QuotedprintableWriter *w) {
    return (IoWriteCloser){&qp_write_closer_vt, w};
}

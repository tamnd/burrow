/* Derived from Go's src/encoding/csv/reader.go and writer.go.
 * Go source: go1.27.1.
 *
 * The reader keeps Go's shape: bufio's ReadSlice hands out a line at a time,
 * the fields of a record are copied end to end into one buffer with the index
 * where each ends, and the record is cut out of that buffer at the end. Go
 * converts the buffer to a string once so every field shares one allocation,
 * and here the Str array and the text go into one block for the same reason.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/encoding/csv.h"

#include "burrow/panic.h"
#include "burrow/unicode.h"
#include "burrow/utf8.h"

#include <string.h>

/* ------------------------------------------------------------------- errors */

BURROW_SENTINEL_ERROR(csv_err_bare_quote, "bare \" in non-quoted-field");
BURROW_SENTINEL_ERROR(csv_err_quote, "extraneous or missing \" in quoted-field");
BURROW_SENTINEL_ERROR(csv_err_field_count, "wrong number of fields");
BURROW_SENTINEL_ERROR(csv_err_trailing_comma, "extra delimiter at end of line");
BURROW_SENTINEL_ERROR(csv_err_invalid_delim, "csv: invalid field or comment delimiter");

static bool csv_same_error(Error a, Error b) {
    return a.vt == b.vt && a.data == b.data;
}

/* The public struct first, so the data pointer of the Error is a pointer to a
 * CsvParseError and errors_as can hand it straight back. The message follows
 * because the message slot cannot allocate, so it is built once, up front. */
typedef struct CsvParseErrorBox {
    CsvParseError e;
    Str message;
} CsvParseErrorBox;

static Str csv_parse_error_message(const void *self) {
    return ((const CsvParseErrorBox *)self)->message;
}

static Error csv_parse_error_unwrap_slot(const void *self) {
    return ((const CsvParseError *)self)->err;
}

static const Type csv_parse_error_desc = {
    {(const Byte *)"ParseError", 10},
    {(const Byte *)"csv", 3},
    KIND_STRUCT,
    (uint32_t)sizeof(CsvParseError),
    (uint16_t)_Alignof(CsvParseError),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x63737065U, /* "cspe" */
    NULL,
};

const Type *const TYPE_CSV_PARSE_ERROR = &csv_parse_error_desc;

static Error csv_parse_error_clone(const void *self, Alloc *a);

static const ErrorVT csv_parse_error_vt = {
    &csv_parse_error_desc,
    csv_parse_error_message,
    csv_parse_error_unwrap_slot,
    NULL,
    NULL,
    NULL,
    csv_parse_error_clone,
};

/* The decimal digits of v into p, or only their count when p is NULL. */
static Int csv_put_int(Byte *p, Int v) {
    Byte digits[24];
    Int d = (Int)sizeof(digits);
    uint64_t u = v < 0 ? 0 - (uint64_t)v : (uint64_t)v;
    do {
        digits[--d] = (Byte)('0' + u % 10);
        u /= 10;
    } while (u != 0);
    if (v < 0)
        digits[--d] = '-';
    Int n = (Int)sizeof(digits) - d;
    if (p != NULL)
        memcpy(p, digits + d, (size_t)n);
    return n;
}

static Int csv_put_str(Byte *p, Str s) {
    if (p != NULL && s.len > 0)
        memcpy(p, s.p, (size_t)s.len);
    return s.len;
}

/* Go's ParseError.Error, written into p, or only measured when p is NULL. */
static Int csv_parse_error_write(Byte *p, const CsvParseError *e) {
    Str text = error_text(e->err);
    Int n = 0;
#define CSV_PUT_LIT(s) (n += csv_put_str(p != NULL ? p + n : NULL, BURROW_S(s)))
#define CSV_PUT_INT(v) (n += csv_put_int(p != NULL ? p + n : NULL, (v)))
    if (csv_same_error(e->err, csv_err_field_count)) {
        CSV_PUT_LIT("record on line ");
        CSV_PUT_INT(e->line);
        CSV_PUT_LIT(": ");
    } else {
        if (e->start_line != e->line) {
            CSV_PUT_LIT("record on line ");
            CSV_PUT_INT(e->start_line);
            CSV_PUT_LIT("; ");
        }
        CSV_PUT_LIT("parse error on line ");
        CSV_PUT_INT(e->line);
        CSV_PUT_LIT(", column ");
        CSV_PUT_INT(e->column);
        CSV_PUT_LIT(": ");
    }
#undef CSV_PUT_LIT
#undef CSV_PUT_INT
    n += csv_put_str(p != NULL ? p + n : NULL, text);
    return n;
}

/* One allocation: the box, then the message. A zero Error means the
 * allocation failed. */
static Error csv_parse_error_build(Alloc *a, const CsvParseError *e) {
    Int mlen = csv_parse_error_write(NULL, e);
    CsvParseErrorBox *b = (CsvParseErrorBox *)mem_alloc_nozero(
        a, sizeof(CsvParseErrorBox) + (size_t)mlen, _Alignof(CsvParseErrorBox));
    if (b == NULL)
        return BURROW_NO_ERROR;
    Byte *p = (Byte *)(b + 1);
    b->e = *e;
    csv_parse_error_write(p, e);
    b->message = str_from_bytes(p, mlen);
    return (Error){&csv_parse_error_vt, b};
}

Error csv_parse_error_as_error(Alloc *a, const CsvParseError *e) {
    Error made = csv_parse_error_build(a, e);
    return BURROW_FAILED(made) ? made : burrow_err_out_of_memory;
}

static Error csv_parse_error_clone(const void *self, Alloc *a) {
    CsvParseError copy = *(const CsvParseError *)self;
    copy.err = error_retain(a, copy.err);
    return csv_parse_error_as_error(a, &copy);
}

Str csv_parse_error_error(Alloc *a, const CsvParseError *e) {
    Int mlen = csv_parse_error_write(NULL, e);
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)mlen, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    csv_parse_error_write(p, e);
    return str_from_bytes(p, mlen);
}

Error csv_parse_error_unwrap(const CsvParseError *e) {
    return e->err;
}

/* What the reader returns, in the error arena. If that cannot be had the
 * reason alone still answers errors_is. */
static Error csv_parse_error_new(Int start_line, Int line, Int column, Error inner) {
    CsvParseError e = {start_line, line, column, inner};
    Error made = csv_parse_error_build(error_allocator(), &e);
    return BURROW_FAILED(made) ? made : inner;
}

/* ------------------------------------------------------------------ helpers */

static bool csv_valid_delim(Rune r) {
    return r != 0 && r != '"' && r != '\r' && r != '\n' && utf8_valid_rune(r) &&
           r != UTF8_RUNE_ERROR;
}

static Slice csv_bytes(Byte *p, Int n) {
    return (Slice){p, n, n, TYPE_BYTE};
}

static Rune csv_next_rune(const Byte *p, Int n) {
    Int size;
    return utf8_decode_rune(csv_bytes((Byte *)(uintptr_t)p, n), &size);
}

static Int csv_length_nl(const Byte *p, Int n) {
    return n > 0 && p[n - 1] == '\n' ? 1 : 0;
}

static Int csv_index_byte(const Byte *p, Int n, Byte c) {
    if (n <= 0)
        return -1;
    const Byte *q = (const Byte *)memchr(p, c, (size_t)n);
    return q != NULL ? (Int)(q - p) : -1;
}

/* bytes.IndexRune for a rune that is valid and not U+FFFD, which is the only
 * kind a delimiter can be, so it is the index of the rune's encoding. */
static Int csv_index_rune(const Byte *p, Int n, const Byte *enc, Int elen) {
    if (elen == 1)
        return csv_index_byte(p, n, enc[0]);
    for (Int i = 0; i + elen <= n;) {
        Int j = csv_index_byte(p + i, n - i - elen + 1, enc[0]);
        if (j < 0)
            return -1;
        i += j;
        if (memcmp(p + i, enc, (size_t)elen) == 0)
            return i;
        i++;
    }
    return -1;
}

/* Makes room for need elements in *pp, which holds *cap of them now. */
static bool csv_grow(Alloc *a, void **pp, Int *cap, Int need, size_t size,
                     size_t align) {
    if (need <= *cap)
        return true;
    Int ncap = *cap < 16 ? 16 : *cap;
    while (ncap < need)
        ncap *= 2;
    void *p = *pp == NULL ? mem_alloc_nozero(a, (size_t)ncap * size, align)
                          : mem_realloc(a, *pp, (size_t)*cap * size,
                                        (size_t)ncap * size, align);
    if (p == NULL)
        return false;
    *pp = p;
    *cap = ncap;
    return true;
}

/* ------------------------------------------------------------------- Reader */

static const Type csv_reader_desc = {
    {(const Byte *)"Reader", 6},
    {(const Byte *)"csv", 3},
    KIND_STRUCT,
    (uint32_t)sizeof(CsvReader),
    (uint16_t)_Alignof(CsvReader),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x63737264U, /* "csrd" */
    NULL,
};

const Type *const TYPE_CSV_READER = &csv_reader_desc;

/* [][]string, the element type of what csv_reader_read_all returns. */
static const Type csv_record_desc = {
    {NULL, 0},
    {NULL, 0},
    KIND_SLICE,
    (uint32_t)sizeof(Slice),
    (uint16_t)_Alignof(Slice),
    0,
    0,
    NULL,
    NULL,
    TYPE_STRING,
    NULL,
    0,
    0x63737273U, /* "csrs" */
    NULL,
};

CsvReader *csv_new_reader(Alloc *a, IoReader r) {
    CsvReader *cr = (CsvReader *)mem_alloc(a, sizeof *cr, _Alignof(CsvReader));
    if (cr == NULL)
        return NULL;
    cr->r = bufio_new_reader(a, r);
    if (cr->r == NULL) {
        mem_free(a, cr, sizeof *cr, _Alignof(CsvReader));
        return NULL;
    }
    cr->comma = ',';
    cr->a = a;
    return cr;
}

void csv_reader_free(CsvReader *r) {
    if (r == NULL)
        return;
    Alloc *a = r->a;
    bufio_reader_free(r->r);
    if (r->raw_buffer != NULL)
        mem_free(a, r->raw_buffer, (size_t)r->raw_cap, 1);
    if (r->record_buffer != NULL)
        mem_free(a, r->record_buffer, (size_t)r->record_cap, 1);
    if (r->field_indexes != NULL)
        mem_free(a, r->field_indexes, (size_t)r->indexes_cap * sizeof(Int),
                 _Alignof(Int));
    if (r->field_positions != NULL)
        mem_free(a, r->field_positions, (size_t)r->positions_cap * sizeof(CsvPosition),
                 _Alignof(CsvPosition));
    if (r->last_record != NULL)
        mem_free(a, r->last_record, r->last_size, _Alignof(Str));
    mem_free(a, r, sizeof *r, _Alignof(CsvReader));
}

/* Go's readLine. The line is a view of bufio's buffer or of raw_buffer, good
 * until the next call, and a \r\n at its end has become \n. */
static Slice csv_read_line(CsvReader *r, Error *err) {
    Error e = BURROW_NO_ERROR;
    Slice line = bufio_reader_read_slice(r->r, '\n', &e);
    if (csv_same_error(e, bufio_err_buffer_full)) {
        r->raw_len = 0;
        for (;;) {
            if (!csv_grow(r->a, (void **)&r->raw_buffer, &r->raw_cap,
                          r->raw_len + line.len, 1, 1)) {
                *err = burrow_err_out_of_memory;
                return csv_bytes(NULL, 0);
            }
            if (line.len > 0)
                memcpy(r->raw_buffer + r->raw_len, line.p, (size_t)line.len);
            r->raw_len += line.len;
            if (!csv_same_error(e, bufio_err_buffer_full))
                break;
            e = BURROW_NO_ERROR;
            line = bufio_reader_read_slice(r->r, '\n', &e);
        }
        line = csv_bytes(r->raw_buffer, r->raw_len);
    }
    Byte *p = (Byte *)line.p;
    Int read_size = line.len;
    Int n = line.len;
    if (read_size > 0 && csv_same_error(e, io_eof)) {
        e = BURROW_NO_ERROR;
        if (p[read_size - 1] == '\r')
            n--;
    }
    r->num_line++;
    r->offset += read_size;
    if (n >= 2 && p[n - 2] == '\r' && p[n - 1] == '\n') {
        p[n - 2] = '\n';
        n--;
    }
    *err = e;
    return csv_bytes(p, n);
}

static bool csv_append_record(CsvReader *r, const Byte *p, Int n) {
    if (n <= 0)
        return true;
    if (!csv_grow(r->a, (void **)&r->record_buffer, &r->record_cap, r->record_len + n,
                  1, 1))
        return false;
    memcpy(r->record_buffer + r->record_len, p, (size_t)n);
    r->record_len += n;
    return true;
}

static bool csv_append_field(CsvReader *r, CsvPosition pos) {
    Int need = r->fields_len + 1;
    if (!csv_grow(r->a, (void **)&r->field_indexes, &r->indexes_cap, need, sizeof(Int),
                  _Alignof(Int)) ||
        !csv_grow(r->a, (void **)&r->field_positions, &r->positions_cap, need,
                  sizeof(CsvPosition), _Alignof(CsvPosition)))
        return false;
    r->field_indexes[r->fields_len] = r->record_len;
    r->field_positions[r->fields_len] = pos;
    r->fields_len++;
    return true;
}

/* The size of a record block with n fields holding text bytes of text. */
static size_t csv_record_size(Int n, Int text) {
    return (size_t)n * sizeof(Str) + (size_t)text;
}

/* How much of record_buffer the finished fields use. A parse error in the
 * middle of a field leaves that field's text after it, which Go drops too. */
static Int csv_record_text(const CsvReader *r) {
    return r->fields_len > 0 ? r->field_indexes[r->fields_len - 1] : 0;
}

/* Cuts the record out of record_buffer into block, which has room for it. */
static Slice csv_cut_record(CsvReader *r, void *block) {
    Int n = r->fields_len;
    if (block == NULL) /* a parse error before the first field */
        return (Slice){NULL, 0, 0, TYPE_STRING};
    Str *fields = (Str *)block;
    Byte *text = (Byte *)(fields + n);
    Int text_len = csv_record_text(r);
    if (text_len > 0)
        memcpy(text, r->record_buffer, (size_t)text_len);
    Int pre = 0;
    for (Int i = 0; i < n; i++) {
        Int idx = r->field_indexes[i];
        fields[i] = str_from_bytes(text + pre, idx - pre);
        pre = idx;
    }
    return (Slice){fields, n, n, TYPE_STRING};
}

/* Go's readRecord. The fields end up in record_buffer, field_indexes and
 * field_positions, and the result says what went wrong, if anything. */
static Error csv_parse_record(CsvReader *r) {
    if (r->comma == r->comment || !csv_valid_delim(r->comma) ||
        (r->comment != 0 && !csv_valid_delim(r->comment)))
        return csv_err_invalid_delim;

    Slice ls = csv_bytes(NULL, 0);
    Error err_read = BURROW_NO_ERROR;
    while (BURROW_OK(err_read)) {
        ls = csv_read_line(r, &err_read);
        if (r->comment != 0 && csv_next_rune(ls.p, ls.len) == r->comment) {
            ls = csv_bytes(NULL, 0);
            continue; /* skip comment lines */
        }
        if (BURROW_OK(err_read) && ls.len == csv_length_nl(ls.p, ls.len)) {
            ls = csv_bytes(NULL, 0);
            continue; /* skip empty lines */
        }
        break;
    }
    if (csv_same_error(err_read, io_eof))
        return err_read;

    Byte *line = (Byte *)ls.p;
    Int len = ls.len;
    Error err = BURROW_NO_ERROR;
    enum { QUOTE_LEN = 1 };
    Byte comma[4];
    Int comma_len = utf8_encode_rune(csv_bytes(comma, 4), r->comma);
    Int rec_line = r->num_line;
    r->record_len = 0;
    r->fields_len = 0;
    CsvPosition pos = {r->num_line, 1};

#define CSV_ADVANCE(k) (line += (k), len -= (k))
#define CSV_OOM()                                                                      \
    do {                                                                               \
        err = burrow_err_out_of_memory;                                                \
        goto done;                                                                     \
    } while (0)

    for (;;) {
        if (r->trim_leading_space) {
            Int i = 0;
            while (i < len) {
                Int size;
                Rune rn = utf8_decode_rune(csv_bytes(line + i, len - i), &size);
                if (!unicode_is_space(rn))
                    break;
                i += size;
            }
            if (i >= len) {
                i = len;
                pos.col -= csv_length_nl(line, len);
            }
            CSV_ADVANCE(i);
            pos.col += i;
        }
        if (len == 0 || line[0] != '"') {
            /* A field without quotes. */
            Int i = csv_index_rune(line, len, comma, comma_len);
            Int flen = i >= 0 ? i : len - csv_length_nl(line, len);
            if (!r->lazy_quotes) {
                Int j = csv_index_byte(line, flen, '"');
                if (j >= 0) {
                    err = csv_parse_error_new(rec_line, r->num_line, pos.col + j,
                                              csv_err_bare_quote);
                    goto done;
                }
            }
            if (!csv_append_record(r, line, flen) || !csv_append_field(r, pos))
                CSV_OOM();
            if (i >= 0) {
                CSV_ADVANCE(i + comma_len);
                pos.col += i + comma_len;
                continue;
            }
            goto done;
        }

        /* A field in quotes. */
        CsvPosition field_pos = pos;
        CSV_ADVANCE(QUOTE_LEN);
        pos.col += QUOTE_LEN;
        for (;;) {
            Int i = csv_index_byte(line, len, '"');
            if (i >= 0) {
                /* Up to the next quote. */
                if (!csv_append_record(r, line, i))
                    CSV_OOM();
                CSV_ADVANCE(i + QUOTE_LEN);
                pos.col += i + QUOTE_LEN;
                Rune rn = csv_next_rune(line, len);
                if (rn == '"') {
                    /* "" is a quote. */
                    if (!csv_append_record(r, (const Byte *)"\"", 1))
                        CSV_OOM();
                    CSV_ADVANCE(QUOTE_LEN);
                    pos.col += QUOTE_LEN;
                } else if (rn == r->comma) {
                    /* ", ends the field. */
                    CSV_ADVANCE(comma_len);
                    pos.col += comma_len;
                    if (!csv_append_field(r, field_pos))
                        CSV_OOM();
                    break;
                } else if (csv_length_nl(line, len) == len) {
                    /* " at the end of the line ends the record. */
                    if (!csv_append_field(r, field_pos))
                        CSV_OOM();
                    goto done;
                } else if (r->lazy_quotes) {
                    /* A bare quote. */
                    if (!csv_append_record(r, (const Byte *)"\"", 1))
                        CSV_OOM();
                } else {
                    /* Something else after a quote. */
                    err = csv_parse_error_new(rec_line, r->num_line,
                                              pos.col - QUOTE_LEN, csv_err_quote);
                    goto done;
                }
            } else if (len > 0) {
                /* The field carries on onto the next line. */
                if (!csv_append_record(r, line, len))
                    CSV_OOM();
                if (BURROW_FAILED(err_read))
                    goto done;
                pos.col += len;
                ls = csv_read_line(r, &err_read);
                line = (Byte *)ls.p;
                len = ls.len;
                if (len > 0) {
                    pos.line++;
                    pos.col = 1;
                }
                if (csv_same_error(err_read, io_eof))
                    err_read = BURROW_NO_ERROR;
            } else {
                /* The input ended inside the quotes. */
                if (!r->lazy_quotes && BURROW_OK(err_read)) {
                    err =
                        csv_parse_error_new(rec_line, pos.line, pos.col, csv_err_quote);
                    goto done;
                }
                if (!csv_append_field(r, field_pos))
                    CSV_OOM();
                goto done;
            }
        }
    }

done:
#undef CSV_ADVANCE
#undef CSV_OOM
    if (BURROW_OK(err))
        err = err_read;

    if (r->fields_per_record > 0) {
        if (r->fields_len != r->fields_per_record && BURROW_OK(err))
            err = csv_parse_error_new(rec_line, rec_line, 1, csv_err_field_count);
    } else if (r->fields_per_record == 0) {
        r->fields_per_record = r->fields_len;
    }
    return err;
}

/* Whether the error from csv_parse_record still comes with a record. */
static bool csv_has_record(Error err) {
    return !csv_same_error(err, io_eof) &&
           !csv_same_error(err, csv_err_invalid_delim) &&
           !csv_same_error(err, burrow_err_out_of_memory);
}

Slice csv_reader_read(CsvReader *r, Alloc *a, Error *err) {
    Error e = csv_parse_record(r);
    if (!csv_has_record(e)) {
        BURROW_OUT(err, e);
        return slice_nil(TYPE_STRING);
    }
    size_t size = csv_record_size(r->fields_len, csv_record_text(r));
    void *block = NULL;
    if (size == 0) {
        /* A parse error before the first field, with nothing to hand back. */
    } else if (r->reuse_record) {
        if (r->last_size < size) {
            if (r->last_record != NULL)
                mem_free(r->a, r->last_record, r->last_size, _Alignof(Str));
            r->last_size = 0;
            r->last_record = mem_alloc_nozero(r->a, size, _Alignof(Str));
            if (r->last_record == NULL) {
                BURROW_OUT(err, burrow_err_out_of_memory);
                return slice_nil(TYPE_STRING);
            }
            r->last_size = size;
        }
        block = r->last_record;
    } else {
        block = mem_alloc_nozero(a, size, _Alignof(Str));
        if (block == NULL) {
            BURROW_OUT(err, burrow_err_out_of_memory);
            return slice_nil(TYPE_STRING);
        }
    }
    BURROW_OUT(err, e);
    return csv_cut_record(r, block);
}

Int csv_reader_field_pos(const CsvReader *r, Int field, Int *column) {
    if (field < 0 || field >= r->fields_len)
        panic_str(BURROW_S("out of range index passed to FieldPos"));
    const CsvPosition *p = &r->field_positions[field];
    if (column != NULL)
        *column = p->col;
    return p->line;
}

int64_t csv_reader_input_offset(const CsvReader *r) {
    return r->offset;
}

void csv_record_free(Alloc *a, Slice record) {
    if (record.p == NULL || record.len == 0)
        return;
    Int text = 0;
    const Str *fields = (const Str *)record.p;
    for (Int i = 0; i < record.len; i++)
        text += fields[i].len;
    mem_free(a, record.p, csv_record_size(record.len, text), _Alignof(Str));
}

void csv_records_free(Alloc *a, Slice records) {
    if (records.p == NULL)
        return;
    Slice *recs = (Slice *)records.p;
    for (Int i = 0; i < records.len; i++)
        csv_record_free(a, recs[i]);
    mem_free(a, records.p, (size_t)records.cap * sizeof(Slice), _Alignof(Slice));
}

Slice csv_reader_read_all(CsvReader *r, Alloc *a, Error *err) {
    Slice *recs = NULL;
    Int n = 0, cap = 0;
    bool reuse = r->reuse_record;
    /* Go's ReadAll calls readRecord(nil), so each record is its own whatever
     * reuse_record says. */
    r->reuse_record = false;
    for (;;) {
        Error e = BURROW_NO_ERROR;
        Slice rec = csv_reader_read(r, a, &e);
        if (csv_same_error(e, io_eof))
            break;
        if (BURROW_FAILED(e) ||
            (n == cap && !csv_grow(a, (void **)&recs, &cap, n + 1, sizeof(Slice),
                                   _Alignof(Slice)))) {
            csv_record_free(a, rec);
            csv_records_free(a, (Slice){recs, n, cap, &csv_record_desc});
            r->reuse_record = reuse;
            BURROW_OUT(err, BURROW_FAILED(e) ? e : burrow_err_out_of_memory);
            return slice_nil(&csv_record_desc);
        }
        recs[n++] = rec;
    }
    r->reuse_record = reuse;
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (n == 0)
        return slice_nil(&csv_record_desc);
    return (Slice){recs, n, cap, &csv_record_desc};
}

/* ------------------------------------------------------------------- Writer */

static const Type csv_writer_desc = {
    {(const Byte *)"Writer", 6},
    {(const Byte *)"csv", 3},
    KIND_STRUCT,
    (uint32_t)sizeof(CsvWriter),
    (uint16_t)_Alignof(CsvWriter),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x63737772U, /* "cswr" */
    NULL,
};

const Type *const TYPE_CSV_WRITER = &csv_writer_desc;

CsvWriter *csv_new_writer(Alloc *a, IoWriter w) {
    CsvWriter *cw = (CsvWriter *)mem_alloc(a, sizeof *cw, _Alignof(CsvWriter));
    if (cw == NULL)
        return NULL;
    cw->w = bufio_new_writer(a, w);
    if (cw->w == NULL) {
        mem_free(a, cw, sizeof *cw, _Alignof(CsvWriter));
        return NULL;
    }
    cw->comma = ',';
    cw->a = a;
    return cw;
}

void csv_writer_free(CsvWriter *w) {
    if (w == NULL)
        return;
    bufio_writer_free(w->w);
    mem_free(w->a, w, sizeof *w, _Alignof(CsvWriter));
}

/* Go's fieldNeedsQuotes. An empty field never does, since there is no way to
 * tell "" from nothing anyway. \. does, because Postgres reads it alone on a
 * line as the end of the data. Otherwise a field does when it holds the comma,
 * a quote or a line break, or starts with a space. */
static bool csv_field_needs_quotes(const CsvWriter *w, Str field) {
    if (field.len == 0)
        return false;
    if (field.len == 2 && field.p[0] == '\\' && field.p[1] == '.')
        return true;
    if (w->comma < UTF8_RUNE_SELF) {
        for (Int i = 0; i < field.len; i++) {
            Byte c = field.p[i];
            if (c == '\n' || c == '\r' || c == '"' || c == (Byte)w->comma)
                return true;
        }
    } else {
        Byte enc[4];
        Int elen = utf8_encode_rune(csv_bytes(enc, 4), w->comma);
        if (csv_index_rune(field.p, field.len, enc, elen) >= 0)
            return true;
        for (Int i = 0; i < field.len; i++) {
            Byte c = field.p[i];
            if (c == '\n' || c == '\r' || c == '"')
                return true;
        }
    }
    return unicode_is_space(csv_next_rune(field.p, field.len));
}

Error csv_writer_write(CsvWriter *w, Slice record) {
    if (!csv_valid_delim(w->comma))
        return csv_err_invalid_delim;

    BufioWriter *b = w->w;
    Error err = BURROW_NO_ERROR;
    const Str *fields = (const Str *)record.p;
    for (Int n = 0; n < record.len; n++) {
        if (n > 0) {
            bufio_writer_write_rune(b, w->comma, &err);
            if (BURROW_FAILED(err))
                return err;
        }

        Str field = fields[n];
        if (!csv_field_needs_quotes(w, field)) {
            bufio_writer_write_string(b, field, &err);
            if (BURROW_FAILED(err))
                return err;
            continue;
        }

        err = bufio_writer_write_byte(b, '"');
        if (BURROW_FAILED(err))
            return err;
        while (field.len > 0) {
            Int i = 0;
            while (i < field.len && field.p[i] != '"' && field.p[i] != '\r' &&
                   field.p[i] != '\n')
                i++;
            bufio_writer_write_string(b, str_from_bytes(field.p, i), &err);
            if (BURROW_FAILED(err))
                return err;
            field = str_from_bytes(field.p + i, field.len - i);

            if (field.len > 0) {
                switch (field.p[0]) {
                case '"':
                    bufio_writer_write_string(b, BURROW_S("\"\""), &err);
                    break;
                case '\r':
                    if (!w->use_crlf)
                        err = bufio_writer_write_byte(b, '\r');
                    break;
                case '\n':
                    if (w->use_crlf)
                        bufio_writer_write_string(b, BURROW_S("\r\n"), &err);
                    else
                        err = bufio_writer_write_byte(b, '\n');
                    break;
                default:
                    break;
                }
                field = str_from_bytes(field.p + 1, field.len - 1);
                if (BURROW_FAILED(err))
                    return err;
            }
        }
        err = bufio_writer_write_byte(b, '"');
        if (BURROW_FAILED(err))
            return err;
    }
    if (w->use_crlf)
        bufio_writer_write_string(b, BURROW_S("\r\n"), &err);
    else
        err = bufio_writer_write_byte(b, '\n');
    return err;
}

void csv_writer_flush(CsvWriter *w) {
    (void)bufio_writer_flush(w->w);
}

Error csv_writer_error(CsvWriter *w) {
    Error err = BURROW_NO_ERROR;
    bufio_writer_write(w->w, csv_bytes(NULL, 0), &err);
    return err;
}

Error csv_writer_write_all(CsvWriter *w, Slice records) {
    const Slice *recs = (const Slice *)records.p;
    for (Int i = 0; i < records.len; i++) {
        Error err = csv_writer_write(w, recs[i]);
        if (BURROW_FAILED(err))
            return err;
    }
    return bufio_writer_flush(w->w);
}

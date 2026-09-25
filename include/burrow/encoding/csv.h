/* encoding/csv, comma separated values as RFC 4180 describes them.
 *
 * A file is records, one per line, and a record is fields separated by commas.
 * A field can be quoted with ", and then it may hold commas, line breaks and
 * doubled "" for a quote of its own. Reading a file line by line:
 *
 *     CsvReader *r = csv_new_reader(a, strings_reader_as_io_reader(&sr));
 *     for (;;) {
 *         Error err = BURROW_NO_ERROR;
 *         Slice rec = csv_reader_read(r, a, &err);
 *         if (errors_is(err, io_eof))
 *             break;
 *         if (BURROW_FAILED(err))
 *             ...;
 *         Str first = BURROW_AT(Str, rec, 0);
 *     }
 *     csv_reader_free(r);
 *
 * and writing one:
 *
 *     CsvWriter *w = csv_new_writer(a, bytes_buffer_as_io_writer(&buf));
 *     csv_writer_write(w, record);
 *     csv_writer_flush(w);
 *     Error err = csv_writer_error(w);
 *     csv_writer_free(w);
 *
 * A record is a Slice of Str. The ones csv_reader_read hands out are a single
 * block from the allocator you pass, the Str array and the text behind it
 * together, so an arena needs nothing more and any other allocator takes
 * csv_record_free.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package encoding/csv */

#ifndef BURROW_ENCODING_CSV_H
#define BURROW_ENCODING_CSV_H

#include "burrow/bufio.h"
#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------- errors */

extern const Error csv_err_bare_quote;     /* bare " in non-quoted-field */
extern const Error csv_err_quote;          /* extraneous or missing " in quoted-field */
extern const Error csv_err_field_count;    /* wrong number of fields */
extern const Error csv_err_trailing_comma; /* no longer used, kept as Go does */

/* csv.ParseError, which is what a malformed record gives you. start_line is the
 * line the record began on, line and column say where the problem is, with the
 * column counted in bytes from 1, and err is one of the errors above.
 * errors_is sees through to err, so
 *
 *     if (errors_is(err, csv_err_field_count))
 *
 * works on what csv_reader_read gives you, and errors_as with
 * TYPE_CSV_PARSE_ERROR gets you the struct. The ones the reader makes live in
 * the calling goroutine's error arena. */
typedef struct CsvParseError {
    Int start_line;
    Int line;
    Int column;
    Error err;
} CsvParseError;

extern const Type *const TYPE_CSV_PARSE_ERROR;

/* The text Go's Error method gives, such as
 *
 *     parse error on line 1, column 3: bare " in non-quoted-field
 *
 * built in a. For an error that came out of the reader error_text is the same
 * text and needs no allocator. */
BURROW_OWNS(ret) Str csv_parse_error_error(Alloc *a, const CsvParseError *e);

/* e->err, which is what errors_unwrap gives for one inside an Error. */
BURROW_BORROWS(ret, e) Error csv_parse_error_unwrap(const CsvParseError *e);

/* An Error for a CsvParseError you filled in yourself, the way Go code writes
 * &csv.ParseError{...}. The message is built in a and err is kept as it is, so
 * it has to live as long as the result does. On an allocation failure you get
 * burrow_err_out_of_memory. */
BURROW_OWNS(ret) Error csv_parse_error_as_error(Alloc *a, const CsvParseError *e);

/* ------------------------------------------------------------------- Reader */

/* One field's place in the input, for csv_reader_field_pos. Not API. */
typedef struct CsvPosition {
    Int line;
    Int col;
} CsvPosition;

/* csv.Reader. The first seven fields are the options Go has, set them after
 * csv_new_reader and before the first read. The rest are the reader's own.
 *
 * comma is the field separator, ',' unless you change it. It cannot be '"',
 * '\r', '\n', zero or U+FFFD.
 *
 * comment, when not zero, makes a line that starts with it be skipped.
 * Everything else about it follows comma, and the two cannot be the same.
 *
 * fields_per_record set above zero is how many fields every record must have.
 * Zero, the default, makes the first record decide, and anything below zero
 * turns the check off. A record with the wrong count comes back together with
 * csv_err_field_count inside a CsvParseError.
 *
 * lazy_quotes lets a quote appear in an unquoted field and a lone quote appear
 * in a quoted one.
 *
 * trim_leading_space drops white space at the start of each field, even when
 * comma is itself a space.
 *
 * reuse_record makes csv_reader_read hand back the same block each time, owned
 * by the reader and good until the next read, instead of a fresh one from the
 * allocator you pass.
 *
 * trailing_comma does nothing, as in Go. */
typedef struct CsvReader {
    Rune comma;
    Rune comment;
    Int fields_per_record;
    bool lazy_quotes;
    bool trim_leading_space;
    bool reuse_record;
    bool trailing_comma;

    Alloc *a;
    BufioReader *r;
    Int num_line;
    int64_t offset;
    Byte *raw_buffer; /* a line longer than the bufio buffer, put together */
    Int raw_len, raw_cap;
    Byte *record_buffer; /* the fields of the record being read, end to end */
    Int record_len, record_cap;
    Int *field_indexes; /* where each field ends in record_buffer */
    CsvPosition *field_positions;
    Int fields_len, indexes_cap, positions_cap;
    void *last_record; /* reuse_record's block */
    size_t last_size;
} CsvReader;

extern const Type *const TYPE_CSV_READER;

/* csv.NewReader. A reader of r with comma set to ','. NULL when the allocator
 * refuses. */
BURROW_OWNS(ret) CsvReader *csv_new_reader(Alloc *a, IoReader r);

/* Gives the reader and what it holds back to its allocator. NULL is fine. */
void csv_reader_free(CsvReader *r);

/* csv.Reader.Read. The next record, a Slice of Str, from a. At the end of the
 * input err gets io_eof and the result is the nil slice.
 *
 * A record with the wrong number of fields is still returned, with err set,
 * the way Go returns both. For a field that cannot be parsed you get the
 * fields before it along with the error, and those need freeing too. An
 * invalid comma or comment gives the nil slice.
 *
 * With reuse_record set the record comes from the reader instead of a and is
 * only good until the next call. */
BURROW_OWNS(ret) Slice csv_reader_read(CsvReader *r, Alloc *a, Error *err);

/* csv.Reader.FieldPos. The line of the start of field number field in the
 * record csv_reader_read last returned, with its column in *column. Both
 * count from 1. An index out of range panics with the message Go panics
 * with. */
Int csv_reader_field_pos(const CsvReader *r, Int field, Int *column);

/* csv.Reader.InputOffset. How many bytes of the input the reader has used,
 * which is where the record it last read ended. */
int64_t csv_reader_input_offset(const CsvReader *r);

/* csv.Reader.ReadAll. Every record left, as a Slice of records, each of them a
 * Slice of Str, all from a. Reaching the end is not an error. On any error the
 * result is the nil slice and err says why. Free it with csv_records_free, or
 * use an arena. */
BURROW_OWNS(ret) Slice csv_reader_read_all(CsvReader *r, Alloc *a, Error *err);

/* Gives back a record csv_reader_read returned. The Str values in it must be
 * the ones it came with, since their lengths say how big the block was. */
void csv_record_free(Alloc *a, Slice record);

/* Gives back what csv_reader_read_all returned, records and all. */
void csv_records_free(Alloc *a, Slice records);

/* ------------------------------------------------------------------- Writer */

/* csv.Writer. comma is ',' unless you change it and use_crlf ends lines with
 * \r\n instead of \n. Output is buffered, so call csv_writer_flush when you
 * are done and csv_writer_error to see whether everything got through. */
typedef struct CsvWriter {
    Rune comma;
    bool use_crlf;

    Alloc *a;
    BufioWriter *w;
} CsvWriter;

extern const Type *const TYPE_CSV_WRITER;

/* csv.NewWriter. NULL when the allocator refuses. */
BURROW_OWNS(ret) CsvWriter *csv_new_writer(Alloc *a, IoWriter w);

/* Frees the writer without flushing it. NULL is fine. */
void csv_writer_free(CsvWriter *w);

/* csv.Writer.Write. Writes one record, a Slice of Str, quoting the fields
 * that need it. The error is for an invalid comma or a failed write. */
BURROW_STATIC(ret) Error csv_writer_write(CsvWriter *w, Slice record);

/* csv.Writer.Flush. Whatever went wrong shows up in csv_writer_error. */
void csv_writer_flush(CsvWriter *w);

/* csv.Writer.Error. The first error any write or flush ran into. */
BURROW_STATIC(ret) Error csv_writer_error(CsvWriter *w);

/* csv.Writer.WriteAll. Writes every record in records, a Slice of records,
 * and flushes. */
BURROW_STATIC(ret) Error csv_writer_write_all(CsvWriter *w, Slice records);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_ENCODING_CSV_H */

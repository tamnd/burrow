/* bytes: searching, splitting, trimming, case and replacement over []byte, and
 * Buffer and Reader.
 *
 * Go's bytes package, all of it. A []byte is a Slice of Byte, the Bytes type in
 * slice.h, and most of these are the strings functions again with Slice where
 * strings has Str:
 *
 *     Slice line = bytes_trim_space(in);
 *     Int i = bytes_index(line, BURROW_B("=")); // BURROW_B makes a Slice from a literal
 *     Slice parts = bytes_split(a, line, BURROW_B(","));
 *     Slice upper = bytes_to_upper(a, line);
 *
 * The rules are Go's, and they differ from strings in the ways Go's do. The
 * functions that cut a slice up return slices of the one you gave them, with
 * the capacity Go gives them, so appending to a field from bytes_split cannot
 * overwrite the next field. The ones that build a result always build a new
 * slice, even when nothing changed, because a caller may write to it. Where Go
 * returns nil, as bytes_trim does when nothing is left, so does this, and a
 * failed allocation gives the nil slice too.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package bytes */

#ifndef BURROW_BYTES_H
#define BURROW_BYTES_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/func.h"
#include "burrow/io.h"
#include "burrow/iter.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/unicode.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A []byte over a string literal, for arguments. It points into the literal, so
 * nothing may write through it. */
#define BURROW_B(lit)                                                                  \
    ((Slice){(void *)(uintptr_t)("" lit), (Int)(sizeof(lit) - 1),                      \
             (Int)(sizeof(lit) - 1), TYPE_BYTE})

/* ------------------------------------------------------------ comparing */

/* bytes.Equal. A nil slice and an empty one are equal. */
bool bytes_equal(Slice a, Slice b);

/* bytes.Compare: -1, 0 or 1 by byte order. */
Int bytes_compare(Slice a, Slice b);

/* bytes.EqualFold: equal under simple Unicode case folding. */
bool bytes_equal_fold(Slice s, Slice t);

bool bytes_has_prefix(Slice s, Slice prefix);
bool bytes_has_suffix(Slice s, Slice suffix);

/* ------------------------------------------------------------ searching
 *
 * An index is a byte offset, or -1 when there is nothing to find. */

bool bytes_contains(Slice b, Slice subslice);
bool bytes_contains_any(Slice b, Str chars);
bool bytes_contains_rune(Slice b, Rune r);
bool bytes_contains_func(Slice b, RuneFunc f);

/* The number of non-overlapping instances of sep, or one more than the number
 * of runes in s when sep is empty. */
Int bytes_count(Slice s, Slice sep);

Int bytes_index(Slice s, Slice sep);
Int bytes_index_any(Slice s, Str chars);
Int bytes_index_byte(Slice b, Byte c);
Int bytes_index_func(Slice s, RuneFunc f);

/* A rune that is not valid, such as UTF8_RUNE_ERROR, matches the first invalid
 * byte sequence. */
Int bytes_index_rune(Slice s, Rune r);

Int bytes_last_index(Slice s, Slice sep);
Int bytes_last_index_any(Slice s, Str chars);
Int bytes_last_index_byte(Slice s, Byte c);
Int bytes_last_index_func(Slice s, RuneFunc f);

/* ------------------------------------------------------------ cutting
 *
 * The results are slices of s. */

/* bytes.Cut: the bytes before and after the first sep. Without sep, before is
 * s, after is nil and found is false. after and found may be NULL. */
BURROW_BORROWS(ret, s) Slice bytes_cut(Slice s, Slice sep, Slice *after, bool *found);

/* bytes.CutLast, the same around the last sep. */
BURROW_BORROWS(ret, s) Slice bytes_cut_last(Slice s, Slice sep, Slice *after,
                                            bool *found);

BURROW_BORROWS(ret, s) Slice bytes_cut_prefix(Slice s, Slice prefix, bool *found);
BURROW_BORROWS(ret, s) Slice bytes_cut_suffix(Slice s, Slice suffix, bool *found);

/* ------------------------------------------------------------ trimming
 *
 * The results are slices of s, and nil when nothing is left of it, as in Go. */

BURROW_BORROWS(ret, s) Slice bytes_trim(Slice s, Str cutset);
BURROW_BORROWS(ret, s) Slice bytes_trim_left(Slice s, Str cutset);
BURROW_BORROWS(ret, s) Slice bytes_trim_right(Slice s, Str cutset);
BURROW_BORROWS(ret, s) Slice bytes_trim_func(Slice s, RuneFunc f);
BURROW_BORROWS(ret, s) Slice bytes_trim_left_func(Slice s, RuneFunc f);
BURROW_BORROWS(ret, s) Slice bytes_trim_right_func(Slice s, RuneFunc f);
BURROW_BORROWS(ret, s) Slice bytes_trim_space(Slice s);
BURROW_BORROWS(ret, s) Slice bytes_trim_prefix(Slice s, Slice prefix);
BURROW_BORROWS(ret, s) Slice bytes_trim_suffix(Slice s, Slice suffix);

/* ------------------------------------------------------------ splitting
 *
 * These return a Slice of Bytes from a, and each piece is a slice of s whose
 * capacity ends where the piece does. With an empty sep they split after each
 * UTF-8 sequence. n is Go's: less than zero for all of them, zero for nil, and
 * otherwise at most n pieces with the rest of s in the last one. */

BURROW_OWNS(ret) Slice bytes_split(Alloc *a, Slice s, Slice sep);
BURROW_OWNS(ret) Slice bytes_split_n(Alloc *a, Slice s, Slice sep, Int n);
BURROW_OWNS(ret) Slice bytes_split_after(Alloc *a, Slice s, Slice sep);
BURROW_OWNS(ret) Slice bytes_split_after_n(Alloc *a, Slice s, Slice sep, Int n);

/* Splits around runs of white space, as unicode_is_space defines it, and drops
 * the white space. */
BURROW_OWNS(ret) Slice bytes_fields(Alloc *a, Slice s);
BURROW_OWNS(ret) Slice bytes_fields_func(Alloc *a, Slice s, RuneFunc f);

/* ------------------------------------------------------------ sequences
 *
 * Go 1.24's iterator versions of the splits, yielding a Slice at a time. The
 * state behind a sequence comes from a and is single use, the way Go's is for
 * Lines and the splits. It is freed with a when a is an arena, or by
 * bytes_seq_free. */

/* The lines of s, each with its newline if it had one. */
BURROW_OWNS(ret) IterSeq bytes_lines(Alloc *a, Slice s);
BURROW_OWNS(ret) IterSeq bytes_split_seq(Alloc *a, Slice s, Slice sep);
BURROW_OWNS(ret) IterSeq bytes_split_after_seq(Alloc *a, Slice s, Slice sep);
BURROW_OWNS(ret) IterSeq bytes_fields_seq(Alloc *a, Slice s);
BURROW_OWNS(ret) IterSeq bytes_fields_func_seq(Alloc *a, Slice s, RuneFunc f);

/* Frees the state of a sequence from one of the functions above, which must
 * have been made with a. */
void bytes_seq_free(Alloc *a, IterSeq seq);

/* ------------------------------------------------------------ building
 *
 * Each returns a new slice from a, never s itself. */

/* A copy of b, or nil if b is nil. */
BURROW_OWNS(ret) Slice bytes_clone(Alloc *a, Slice b);

/* Joins a Slice of Bytes. Panics if the result would not fit in an Int. */
BURROW_OWNS(ret) Slice bytes_join(Alloc *a, Slice s, Slice sep);

/* Panics if count is negative or the result would not fit in an Int. */
BURROW_OWNS(ret) Slice bytes_repeat(Alloc *a, Slice b, Int count);

/* The first n instances of old replaced by repl, or all of them if n is
 * negative. An empty old matches before each rune and at the end. */
BURROW_OWNS(ret) Slice bytes_replace(Alloc *a, Slice s, Slice old, Slice repl, Int n);
BURROW_OWNS(ret) Slice bytes_replace_all(Alloc *a, Slice s, Slice old, Slice repl);

/* Each rune of s through mapping. A negative result drops the rune. */
BURROW_OWNS(ret) Slice bytes_map(Alloc *a, RuneMapFunc mapping, Slice s);

/* The runes of s as a Slice of Rune. */
BURROW_OWNS(ret) Slice bytes_runes(Alloc *a, Slice s);

BURROW_OWNS(ret) Slice bytes_to_upper(Alloc *a, Slice s);
BURROW_OWNS(ret) Slice bytes_to_lower(Alloc *a, Slice s);
BURROW_OWNS(ret) Slice bytes_to_title(Alloc *a, Slice s);
BURROW_OWNS(ret) Slice bytes_to_upper_special(Alloc *a, UnicodeSpecialCase c, Slice s);
BURROW_OWNS(ret) Slice bytes_to_lower_special(Alloc *a, UnicodeSpecialCase c, Slice s);
BURROW_OWNS(ret) Slice bytes_to_title_special(Alloc *a, UnicodeSpecialCase c, Slice s);

/* Each run of invalid UTF-8 in s replaced by replacement, which may be empty. */
BURROW_OWNS(ret) Slice bytes_to_valid_utf8(Alloc *a, Slice s, Slice replacement);

/* Deprecated in Go, because its idea of a word boundary does not handle
 * Unicode punctuation. Here because Go has it. */
BURROW_OWNS(ret) Slice bytes_title(Alloc *a, Slice s);

/* ------------------------------------------------------------ Buffer
 *
 * bytes.Buffer, a growable buffer to write into and read back out of:
 *
 *     BytesBuffer b = BYTES_BUFFER(a);
 *     bytes_buffer_write_string(&b, BURROW_S("hello "), NULL);
 *     fmt_fprintf_v(bytes_buffer_as_io_writer(&b), "%d", 42);
 *     Str out = bytes_buffer_string(&b, a);
 *     bytes_buffer_free(&b);
 *
 * A zeroed buffer is an empty one with no allocator, which reads fine and
 * fails every write that needs room with burrow_err_out_of_memory. Go panics
 * with ErrTooLarge when it cannot grow a buffer. This returns the error
 * instead, from the write that needed the room, and leaves the buffer as it
 * was.
 *
 * As in Go, bytes_buffer_bytes and bytes_buffer_next hand out a slice of the
 * buffer that is good until the next write, read, reset or truncate. */
typedef struct BytesBuffer {
    Alloc *a;
    Slice buf;             /* the contents are buf.p[off:buf.len] */
    Int off;               /* read at buf.p[off], write at buf.p[buf.len] */
    signed char last_read; /* what the last read did, for the unreads */
    bool owned;            /* buf came from a, so growing can give it back */
    bool heap;             /* the buffer itself came from bytes_new_buffer */
} BytesBuffer;

#define BYTES_BUFFER(alloc) ((BytesBuffer){.a = (alloc)})

/* bytes.MinRead, the smallest read bytes_buffer_read_from asks for. */
#define BYTES_MIN_READ 512

/* bytes.ErrTooLarge, which Go panics with and this never does. It is here for
 * code that compares against it. */
extern const Error bytes_err_too_large;

/* bytes.NewBuffer, which takes buf as the initial contents and keeps using it,
 * with a for growing. The buffer never frees buf, which may come from anywhere,
 * only the memory it allocated itself. */
BURROW_OWNS(ret) BytesBuffer *bytes_new_buffer(Alloc *a, Slice buf);
BURROW_OWNS(ret) BytesBuffer *bytes_new_buffer_string(Alloc *a, Str s);

/* Gives back the memory the buffer allocated, and the buffer itself when it
 * came from bytes_new_buffer. Go's collector does this. */
void bytes_buffer_free(BytesBuffer *b);

/* The unread bytes. Good until the buffer is next changed. */
BURROW_BORROWS(ret, b) Slice bytes_buffer_bytes(BytesBuffer *b);

/* The unread bytes as a new Str from a. A nil buffer gives "<nil>", as Go's
 * does. */
BURROW_OWNS(ret) Str bytes_buffer_string(BytesBuffer *b, Alloc *a);

Int bytes_buffer_len(BytesBuffer *b);
Int bytes_buffer_cap(BytesBuffer *b);
Int bytes_buffer_available(BytesBuffer *b);

/* An empty slice with the buffer's free space as its capacity, to append to and
 * pass to bytes_buffer_write. */
BURROW_BORROWS(ret, b) Slice bytes_buffer_available_buffer(BytesBuffer *b);

/* Makes room for n more bytes. Panics if n is negative. Returns false if the
 * room could not be allocated. */
bool bytes_buffer_grow(BytesBuffer *b, Int n);

/* Keeps the first n unread bytes. Panics if n is out of range. */
void bytes_buffer_truncate(BytesBuffer *b, Int n);
void bytes_buffer_reset(BytesBuffer *b);

Int bytes_buffer_write(BytesBuffer *b, Slice p, Error *err);
Int bytes_buffer_write_string(BytesBuffer *b, Str s, Error *err);
BURROW_STATIC(ret) Error bytes_buffer_write_byte(BytesBuffer *b, Byte c);
Int bytes_buffer_write_rune(BytesBuffer *b, Rune r, Error *err);

/* Reads from r until io_eof, which is not an error here. */
int64_t bytes_buffer_read_from(BytesBuffer *b, IoReader r, Error *err);

/* Writes the unread bytes to w. */
int64_t bytes_buffer_write_to(BytesBuffer *b, IoWriter w, Error *err);

/* Reads up to p.len bytes. io_eof when the buffer is empty and p is not. */
Int bytes_buffer_read(BytesBuffer *b, Slice p, Error *err);

/* The next n unread bytes without reading them, or all there are and io_eof
 * if there are fewer than n. */
BURROW_BORROWS(ret, b) Slice bytes_buffer_peek(BytesBuffer *b, Int n, Error *err);

/* The next n unread bytes, or all of them if there are fewer, as a slice of the
 * buffer. */
BURROW_BORROWS(ret, b) Slice bytes_buffer_next(BytesBuffer *b, Int n);

Byte bytes_buffer_read_byte(BytesBuffer *b, Error *err);
Rune bytes_buffer_read_rune(BytesBuffer *b, Int *size, Error *err);
BURROW_STATIC(ret) Error bytes_buffer_unread_byte(BytesBuffer *b);
BURROW_STATIC(ret) Error bytes_buffer_unread_rune(BytesBuffer *b);

/* Reads up to and including delim, as a new slice from a. io_eof if delim never
 * turned up, with what there was. */
BURROW_OWNS(ret) Slice bytes_buffer_read_bytes(BytesBuffer *b, Alloc *a, Byte delim,
                                               Error *err);
BURROW_OWNS(ret) Str bytes_buffer_read_string(BytesBuffer *b, Alloc *a, Byte delim,
                                              Error *err);

IoReader bytes_buffer_as_io_reader(BytesBuffer *b);
IoWriter bytes_buffer_as_io_writer(BytesBuffer *b);
IoByteReader bytes_buffer_as_io_byte_reader(BytesBuffer *r);

extern const Type *const TYPE_BYTES_BUFFER;

/* ------------------------------------------------------------ Reader
 *
 * bytes.Reader, which reads from a Slice through io.Reader, io.Seeker and the
 * rest. It holds the slice and an offset and nothing else, so one on the stack
 * is fine:
 *
 *     BytesReader r;
 *     bytes_reader_reset(&r, data);
 *
 * bytes_new_reader is Go's NewReader, for when it has to outlive the frame. */
typedef struct BytesReader {
    Slice s;
    int64_t i;     /* the read offset */
    Int prev_rune; /* where the last ReadRune started, or -1 */
} BytesReader;

BURROW_OWNS(ret) BytesReader *bytes_new_reader(Alloc *a, Slice b);
void bytes_reader_reset(BytesReader *r, Slice b);

/* The bytes not yet read, and the length of the whole slice. */
Int bytes_reader_len(BytesReader *r);
int64_t bytes_reader_size(BytesReader *r);

Int bytes_reader_read(BytesReader *r, Slice b, Error *err);
Int bytes_reader_read_at(BytesReader *r, Slice b, int64_t off, Error *err);
Byte bytes_reader_read_byte(BytesReader *r, Error *err);
BURROW_STATIC(ret) Error bytes_reader_unread_byte(BytesReader *r);
Rune bytes_reader_read_rune(BytesReader *r, Int *size, Error *err);
BURROW_STATIC(ret) Error bytes_reader_unread_rune(BytesReader *r);
int64_t bytes_reader_seek(BytesReader *r, int64_t offset, Int whence, Error *err);
int64_t bytes_reader_write_to(BytesReader *r, IoWriter w, Error *err);

IoReader bytes_reader_as_io_reader(BytesReader *r);
IoSeeker bytes_reader_as_io_seeker(BytesReader *r);
IoByteReader bytes_reader_as_io_byte_reader(BytesReader *r);
IoReaderAt bytes_reader_as_io_reader_at(BytesReader *r);

extern const Type *const TYPE_BYTES_READER;

#ifdef __cplusplus
}
#endif

#endif /* BURROW_BYTES_H */

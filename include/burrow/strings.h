/* strings: searching, splitting, trimming, case and replacement over Str.
 *
 * Go's strings package, all of it. The functions that find something take and
 * return plain values and never allocate. The ones that build a new string take
 * an allocator first, and the ones that cut a string up return views into the
 * string you gave them, so the pieces live exactly as long as the original:
 *
 *     Str host = strings_trim_space(line);
 *     Str before, after;
 *     bool found;
 *     before = strings_cut(host, BURROW_S(":"), &after, &found);
 *     Slice parts = strings_split(a, BURROW_S("a,b,c"), BURROW_S(","));
 *     Str upper = strings_to_upper(a, BURROW_S("gopher"));
 *
 * A function that builds a string can hand back its input unchanged when there
 * is nothing to do, the way Go does, so strings_to_upper of a string that is
 * already upper case allocates nothing. Treat the result as borrowed from both
 * the allocator and the input. A failed allocation gives the empty string or
 * the nil slice, the same as everywhere else in burrow.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package strings */

#ifndef BURROW_STRINGS_H
#define BURROW_STRINGS_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/func.h"
#include "burrow/io.h"
#include "burrow/iter.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/sync.h"
#include "burrow/unicode.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------ comparing */

/* strings.Compare: -1, 0 or 1 by byte order. The == of two Str is str_eq. */
Int strings_compare(Str a, Str b);

/* strings.EqualFold: equal under simple Unicode case folding, so "Go" and "GO"
 * match, and so do "σ" and "Σ". */
bool strings_equal_fold(Str s, Str t);

bool strings_has_prefix(Str s, Str prefix);
bool strings_has_suffix(Str s, Str suffix);

/* ------------------------------------------------------------ searching
 *
 * The index functions return a byte offset into s, or -1. An empty substr is
 * found at 0 by strings_index and at len(s) by strings_last_index, as in Go. */

bool strings_contains(Str s, Str substr);
bool strings_contains_any(Str s, Str chars);
bool strings_contains_rune(Str s, Rune r);
bool strings_contains_func(Str s, RuneFunc f);

/* Non-overlapping instances of substr. An empty substr counts one more than the
 * runes in s. */
Int strings_count(Str s, Str substr);

Int strings_index(Str s, Str substr);
Int strings_index_any(Str s, Str chars);
Int strings_index_byte(Str s, Byte c);
Int strings_index_func(Str s, RuneFunc f);

/* A rune of utf8.RuneError finds the first invalid byte sequence as well as
 * the first U+FFFD. */
Int strings_index_rune(Str s, Rune r);

Int strings_last_index(Str s, Str substr);
Int strings_last_index_any(Str s, Str chars);
Int strings_last_index_byte(Str s, Byte c);
Int strings_last_index_func(Str s, RuneFunc f);

/* ------------------------------------------------------------ cutting
 *
 * Go's multiple results come back as pointers on the end, and any of them may
 * be NULL if you do not want it. Every Str returned points into s. */

/* strings.Cut: the text before and after the first sep. Without sep, before is
 * s, after is empty and found is false. */
BURROW_BORROWS(ret, s) Str strings_cut(Str s, Str sep, Str *after, bool *found);

/* strings.CutLast, the same around the last sep. */
BURROW_BORROWS(ret, s) Str strings_cut_last(Str s, Str sep, Str *after, bool *found);

BURROW_BORROWS(ret, s) Str strings_cut_prefix(Str s, Str prefix, bool *found);
BURROW_BORROWS(ret, s) Str strings_cut_suffix(Str s, Str suffix, bool *found);

/* ------------------------------------------------------------ trimming
 *
 * All of these return a piece of s and never allocate. Trim, TrimLeft and
 * TrimRight take a set of runes to strip, not a prefix or suffix, which is the
 * classic mistake with them in Go too: strings_trim_left(s, "ab") strips any
 * run of a and b. strings_trim_prefix is the one that strips a prefix. */

BURROW_BORROWS(ret, s) Str strings_trim(Str s, Str cutset);
BURROW_BORROWS(ret, s) Str strings_trim_left(Str s, Str cutset);
BURROW_BORROWS(ret, s) Str strings_trim_right(Str s, Str cutset);
BURROW_BORROWS(ret, s) Str strings_trim_func(Str s, RuneFunc f);
BURROW_BORROWS(ret, s) Str strings_trim_left_func(Str s, RuneFunc f);
BURROW_BORROWS(ret, s) Str strings_trim_right_func(Str s, RuneFunc f);
BURROW_BORROWS(ret, s) Str strings_trim_space(Str s);
BURROW_BORROWS(ret, s) Str strings_trim_prefix(Str s, Str prefix);
BURROW_BORROWS(ret, s) Str strings_trim_suffix(Str s, Str suffix);

/* ------------------------------------------------------------ splitting
 *
 * The results are a Slice of Str, allocated from a, whose elements point into
 * s. n works as in Go: more than zero gives at most n pieces with the rest of
 * s in the last, zero gives the nil slice, and less than zero gives them all.
 * An empty sep splits after each UTF-8 sequence. */

BURROW_OWNS(ret) Slice strings_split(Alloc *a, Str s, Str sep);
BURROW_OWNS(ret) Slice strings_split_n(Alloc *a, Str s, Str sep, Int n);
BURROW_OWNS(ret) Slice strings_split_after(Alloc *a, Str s, Str sep);
BURROW_OWNS(ret) Slice strings_split_after_n(Alloc *a, Str s, Str sep, Int n);

/* Splits around runs of white space, as unicode_is_space defines it, and drops
 * the empty pieces at either end. */
BURROW_OWNS(ret) Slice strings_fields(Alloc *a, Str s);
BURROW_OWNS(ret) Slice strings_fields_func(Alloc *a, Str s, RuneFunc f);

/* ------------------------------------------------------------ sequences
 *
 * The same splits as sequences, which yield a pointer to each piece as a Str
 * without building a slice. See burrow/iter.h for how to consume one:
 *
 *     BURROW_RANGE(Str, line, strings_lines(a, text)) {
 *         ...
 *     }
 *
 * The sequence's state, a few words, comes from a, and a nil IterSeq means that
 * allocation failed. With an arena that is the end of it, and with the heap
 * strings_seq_free gives it back once you are done ranging.
 *
 * Lines and the split sequences are single use, as in Go, where the closure
 * cuts down the string it captured as it goes: run one to the end and a second
 * run yields nothing. The fields sequences can be run again. */

/* The lines of s, each with its newline if it had one. */
BURROW_OWNS(ret) IterSeq strings_lines(Alloc *a, Str s);
BURROW_OWNS(ret) IterSeq strings_split_seq(Alloc *a, Str s, Str sep);
BURROW_OWNS(ret) IterSeq strings_split_after_seq(Alloc *a, Str s, Str sep);
BURROW_OWNS(ret) IterSeq strings_fields_seq(Alloc *a, Str s);
BURROW_OWNS(ret) IterSeq strings_fields_func_seq(Alloc *a, Str s, RuneFunc f);

/* Frees the state of a sequence from one of the functions above, which must
 * have come from a. A nil seq is fine. */
void strings_seq_free(Alloc *a, IterSeq seq);

/* ------------------------------------------------------------ building
 *
 * These return a new string from a, or s itself when nothing changed. */

BURROW_OWNS(ret) Str strings_clone(Alloc *a, Str s);

/* Joins a Slice of Str. Panics if the result would not fit in an Int. */
BURROW_OWNS(ret) Str strings_join(Alloc *a, Slice elems, Str sep);

/* Panics if count is negative or the result would not fit in an Int. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, s) Str strings_repeat(Alloc *a, Str s, Int count);

/* The first n instances of old replaced by repl, or all of them if n is
 * negative. An empty old matches at the start and after each UTF-8 sequence. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, s) Str strings_replace(Alloc *a, Str s, Str old,
                                                            Str repl, Int n);
BURROW_OWNS(ret) BURROW_BORROWS(ret, s) Str strings_replace_all(Alloc *a, Str s,
                                                                Str old, Str repl);

/* Each rune of s through mapping. A negative result drops the rune. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, s) Str strings_map(Alloc *a, RuneMapFunc mapping,
                                                        Str s);

BURROW_OWNS(ret) BURROW_BORROWS(ret, s) Str strings_to_upper(Alloc *a, Str s);
BURROW_OWNS(ret) BURROW_BORROWS(ret, s) Str strings_to_lower(Alloc *a, Str s);
BURROW_OWNS(ret) BURROW_BORROWS(ret, s) Str strings_to_title(Alloc *a, Str s);
BURROW_OWNS(ret) BURROW_BORROWS(ret, s) Str
strings_to_upper_special(Alloc *a, UnicodeSpecialCase c, Str s);
BURROW_OWNS(ret) BURROW_BORROWS(ret, s) Str
strings_to_lower_special(Alloc *a, UnicodeSpecialCase c, Str s);
BURROW_OWNS(ret) BURROW_BORROWS(ret, s) Str
strings_to_title_special(Alloc *a, UnicodeSpecialCase c, Str s);

/* Each run of invalid UTF-8 in s replaced by replacement, which may be empty. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, s) Str strings_to_valid_utf8(Alloc *a, Str s,
                                                                  Str replacement);

/* Deprecated in Go, because its idea of a word boundary does not handle
 * Unicode punctuation. Here for completeness. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, s) Str strings_title(Alloc *a, Str s);

/* ------------------------------------------------------------ Builder
 *
 * strings.Builder, a buffer you append to and then read as one Str.
 *
 *     StringsBuilder b = STRINGS_BUILDER(a);
 *     strings_builder_write_string(&b, BURROW_S("hello, "), NULL);
 *     strings_builder_write_rune(&b, 0x4E16, NULL);
 *     Str s = strings_builder_string(&b);
 *
 * The builder keeps the allocator it was made with, as a Map does, so the
 * writes do not take one. A zeroed builder has no allocator and every write to
 * it fails with burrow_err_out_of_memory, which is the one place this differs
 * from Go's ready to use zero value.
 *
 * Go's writes never fail. Here a write that could not grow the buffer returns
 * burrow_err_out_of_memory through err, writes nothing, and leaves the builder
 * as it was.
 *
 * The Str from strings_builder_string points into the buffer, and stays good
 * after later writes and after a reset, because a builder never writes over
 * bytes it has handed out and never frees a buffer it has lent out that way.
 * A buffer nobody has seen is freed when the builder outgrows it. With an
 * arena none of this costs anything. With the heap allocator, the strings you
 * took are yours to free once you are done with them.
 *
 * A builder must not be copied once it has been written to, and Go's check for
 * that is here too: a write through a copy panics. */
typedef struct StringsBuilder {
    struct StringsBuilder *addr; /* the builder's own address, to catch copies */
    Alloc *a;
    Byte *buf;
    Int len;
    Int cap;
    bool lent; /* a Str into buf has been handed out, so buf must stay */
} StringsBuilder;

#define STRINGS_BUILDER(alloc) ((StringsBuilder){.a = (alloc)})

BURROW_BORROWS(ret, b) Str strings_builder_string(StringsBuilder *b);
Int strings_builder_len(StringsBuilder *b);
Int strings_builder_cap(StringsBuilder *b);

/* Empties the builder. It keeps its allocator, and a Str it handed out stays
 * good, because the buffer is dropped rather than freed or reused. */
void strings_builder_reset(StringsBuilder *b);

/* Makes room for n more bytes. Panics if n is negative, and returns false if
 * the room could not be allocated. */
bool strings_builder_grow(StringsBuilder *b, Int n);

Int strings_builder_write(StringsBuilder *b, Slice p, Error *err);
BURROW_STATIC(ret) Error strings_builder_write_byte(StringsBuilder *b, Byte c);
Int strings_builder_write_rune(StringsBuilder *b, Rune r, Error *err);
Int strings_builder_write_string(StringsBuilder *b, Str s, Error *err);

/* The builder as an io.Writer, for fmt_fprintf and io_copy. */
IoWriter strings_builder_as_io_writer(StringsBuilder *b);

extern const Type *const TYPE_STRINGS_BUILDER;

/* ------------------------------------------------------------ Reader
 *
 * strings.Reader, which reads from a Str through io.Reader, io.Seeker and
 * the rest. It holds the Str and an offset and nothing else, so one on the
 * stack is fine:
 *
 *     StringsReader r;
 *     strings_reader_reset(&r, BURROW_S("some input"));
 *     io_copy(a, w, strings_reader_as_io_reader(&r), &err);
 *
 * strings_new_reader is Go's NewReader, for when it has to outlive the frame. */
typedef struct StringsReader {
    Str s;
    int64_t i;     /* the read offset */
    Int prev_rune; /* where the last ReadRune started, or -1 */
} StringsReader;

BURROW_OWNS(ret) StringsReader *strings_new_reader(Alloc *a, Str s);
void strings_reader_reset(StringsReader *r, Str s);

/* The bytes not yet read, and the length of the whole string. */
Int strings_reader_len(StringsReader *r);
int64_t strings_reader_size(StringsReader *r);

Int strings_reader_read(StringsReader *r, Slice b, Error *err);
Int strings_reader_read_at(StringsReader *r, Slice b, int64_t off, Error *err);
Byte strings_reader_read_byte(StringsReader *r, Error *err);
BURROW_STATIC(ret) Error strings_reader_unread_byte(StringsReader *r);
Rune strings_reader_read_rune(StringsReader *r, Int *size, Error *err);
BURROW_STATIC(ret) Error strings_reader_unread_rune(StringsReader *r);
int64_t strings_reader_seek(StringsReader *r, int64_t offset, Int whence, Error *err);
int64_t strings_reader_write_to(StringsReader *r, IoWriter w, Error *err);

IoReader strings_reader_as_io_reader(StringsReader *r);
IoSeeker strings_reader_as_io_seeker(StringsReader *r);

extern const Type *const TYPE_STRINGS_READER;

/* ------------------------------------------------------------ Replacer
 *
 * strings.Replacer, which replaces a list of strings with others in one pass.
 * The pairs are tried in the order given, and matches do not overlap:
 *
 *     Str pairs[] = {BURROW_S("<"), BURROW_S("&lt;"), BURROW_S(">"), BURROW_S("&gt;")};
 *     StringsReplacer *r = strings_new_replacer(a, slice_from(pairs, 4, 4, TYPE_STRING));
 *     Str safe = strings_replacer_replace(r, a, text);
 *
 * The replacer copies the list but not the strings in it, so they have to live
 * as long as it does, which literals do. It works out how to do the job on
 * first use, choosing between a byte table, a single pattern search and a trie
 * the way Go does, with its memory from the allocator it was made with. It is
 * safe to use from many goroutines at once. If the build runs out of memory,
 * replace gives the empty string and write_string gives burrow_err_out_of_memory
 * from then on. */
typedef struct StringsReplacer StringsReplacer;

/* Panics on an odd number of strings. NULL if a could not allocate. */
BURROW_OWNS(ret) StringsReplacer *strings_new_replacer(Alloc *a, Slice oldnew);

/* s with the replacements made, from a, or s itself if there were none. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, s) Str strings_replacer_replace(StringsReplacer *r,
                                                                     Alloc *a, Str s);

/* The same result written to w. */
Int strings_replacer_write_string(StringsReplacer *r, IoWriter w, Str s, Error *err);

/* Gives back everything the replacer allocated. Go's collector does this, so
 * there is no Go name for it. NULL is fine. */
void strings_replacer_free(StringsReplacer *r);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_STRINGS_H */

/* regexp, Go's regular expressions: RE2 syntax, matching in time linear in the
 * input.
 *
 *     Error err;
 *     Regexp *re = regexp_compile(a, BURROW_S("(\\w+)@(\\w+)\\.com"), &err);
 *     if (re == NULL)
 *         ...error_text(err) says what is wrong with the pattern...
 *     bool ok = regexp_match_string(re, BURROW_S("gopher@example.com"));
 *     Slice m = regexp_find_string_submatch(re, a, BURROW_S("mail gopher@example.com"));
 *     ...((Str *)m.p)[1] is "gopher"...
 *     regexp_free(re);
 *
 * The syntax is the one in burrow/regexp/syntax.h and docs/guides/regexp.md.
 * The matching is Go's, which picks one of three engines for each search: a
 * one pass matcher for anchored patterns that never need to look back, a
 * backtracker for small programs on short inputs, and a Pike VM for the rest.
 * All three give the same answers, the ones Go gives.
 *
 * Names. The methods are Go's with regexp_ in front, so FindAllString is
 * regexp_find_all_string. The package functions Match, MatchString and
 * MatchReader, which share those names in Go, end in _pattern here.
 *
 * Results. Functions that give back a match borrow from the input: a Str or a
 * []byte result points into the text you passed. Functions that build
 * something, the index slices, the slices of matches, the replaced text, take
 * an allocator and put what they build there. Go keeps nil and empty apart,
 * and so does this: no match is the nil slice, and FindSubmatch gives a nil
 * slice for a group that did not take part. When the allocator runs out, the
 * functions that build give the nil slice or the empty string.
 *
 * Concurrency. A Regexp is safe to use from many threads at once, as in Go,
 * except for regexp_longest and regexp_unmarshal_text, which change it. Each
 * search borrows some working memory the Regexp keeps for the purpose, from
 * the heap and not from the allocator it was compiled with, so that an arena
 * is never touched from two threads. A search that cannot get that memory
 * panics, which is what Go does when it runs out.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package regexp */

#ifndef BURROW_REGEXP_H
#define BURROW_REGEXP_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/func.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/slice.h"

#ifdef __cplusplus
extern "C" {
#endif

/* regexp.Regexp: a compiled regular expression. */
typedef struct Regexp Regexp;

/* Go's func(string) string and func([]byte) []byte, which is what the
 * ReplaceAll Func forms take. What they return is copied into the result, so
 * it can live anywhere, including in memory the function reuses each call. */
BURROW_FUNC(StrFunc, Str, Str s);
BURROW_FUNC(BytesFunc, Slice, Slice b);

/* ---------------------------------------------------------------- compiling */

/* regexp.Compile: parses expr and gets it ready to match, with leftmost first
 * semantics, the ones Perl and Python have. NULL and the error, a SyntaxError
 * that errors_as finds, when expr does not parse. Everything the Regexp holds
 * comes from a, and regexp_free gives it back. err may be NULL. */
BURROW_OWNS(ret) Regexp *regexp_compile(Alloc *a, Str expr, Error *err);

/* regexp.CompilePOSIX: the POSIX egrep syntax, and leftmost longest matches:
 * of the matches that start first, the longest wins. */
BURROW_OWNS(ret) Regexp *regexp_compile_posix(Alloc *a, Str expr, Error *err);

/* regexp.MustCompile and MustCompilePOSIX: the same, but a pattern that does
 * not parse panics, with Go's message, which names the pattern. For patterns
 * written into the program. They still give NULL when a runs out. */
BURROW_OWNS(ret) Regexp *regexp_must_compile(Alloc *a, Str str);
BURROW_OWNS(ret) Regexp *regexp_must_compile_posix(Alloc *a, Str str);

/* Gives back everything re holds. NULL is fine. */
void regexp_free(Regexp *re);

/* Regexp.Copy: a second Regexp for the same pattern, from a. Go deprecates it,
 * since one Regexp is safe to share, and it is here because a program that
 * calls regexp_longest on one copy needs the other not to change. NULL when a
 * runs out. */
BURROW_OWNS(ret) Regexp *regexp_copy(const Regexp *re, Alloc *a);

/* Regexp.Longest: from now on prefer the leftmost longest match. Not safe
 * while another thread is using re. */
void regexp_longest(Regexp *re);

/* Regexp.String: the pattern re was compiled from. */
BURROW_BORROWS(ret, re) Str regexp_string(const Regexp *re);

/* Regexp.NumSubexp: how many groups the pattern has. */
Int regexp_num_subexp(const Regexp *re);

/* Regexp.SubexpNames: the names of the groups, a Slice of Str with the empty
 * string for the whole match, element 0, and for each group with no name. */
BURROW_BORROWS(ret, re) Slice regexp_subexp_names(const Regexp *re);

/* Regexp.SubexpIndex: the number of the first group called name, or -1 when
 * there is none or name is empty. */
Int regexp_subexp_index(const Regexp *re, Str name);

/* Regexp.LiteralPrefix: the literal text every match has to start with, and
 * in *complete whether that text is the whole pattern. complete may be NULL. */
BURROW_BORROWS(ret, re) Str regexp_literal_prefix(const Regexp *re, bool *complete);

/* regexp.QuoteMeta: s with a backslash in front of every metacharacter, so
 * that it matches itself. s itself when it has none. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, s) Str regexp_quote_meta(Alloc *a, Str s);

/* ----------------------------------------------------------------- matching */

/* Regexp.Match, MatchString and MatchReader: whether re matches anywhere in
 * the input. The reader is read up to where the answer is known, which can be
 * past the match, and no further. */
bool regexp_match(const Regexp *re, Slice b);
bool regexp_match_string(const Regexp *re, Str s);
bool regexp_match_reader(const Regexp *re, IoRuneReader r);

/* regexp.Match, MatchString and MatchReader, the package functions: compile
 * pattern and match. a holds the compiled pattern while it runs and the error
 * when pattern does not parse. */
bool regexp_match_pattern(Alloc *a, Str pattern, Slice b, Error *err);
bool regexp_match_string_pattern(Alloc *a, Str pattern, Str s, Error *err);
bool regexp_match_reader_pattern(Alloc *a, Str pattern, IoRuneReader r, Error *err);

/* ------------------------------------------------------------------ finding
 *
 * Find gives the leftmost match. The Index forms give where it is as a Slice
 * of Int, start and end. The Submatch forms give the groups too, with pair i
 * or element i for group i, and -1, the nil slice or the empty string for a
 * group that did not take part. */

/* Regexp.Find and FindString: the text of the first match, pointing into b or
 * s. The nil slice when there is none. FindString gives the empty string for
 * no match, as in Go, which cannot tell it from an empty match. */
BURROW_BORROWS(ret, b) Slice regexp_find(const Regexp *re, Slice b);
BURROW_BORROWS(ret, s) Str regexp_find_string(const Regexp *re, Str s);

BURROW_OWNS(ret) Slice regexp_find_index(const Regexp *re, Alloc *a, Slice b);
BURROW_OWNS(ret) Slice regexp_find_string_index(const Regexp *re, Alloc *a, Str s);
BURROW_OWNS(ret) Slice regexp_find_reader_index(const Regexp *re, Alloc *a,
                                                IoRuneReader r);

/* Regexp.FindSubmatch: a Slice of []byte slices into b. */
BURROW_OWNS(ret) Slice regexp_find_submatch(const Regexp *re, Alloc *a, Slice b);

/* Regexp.FindStringSubmatch: a Slice of Str into s. */
BURROW_OWNS(ret) Slice regexp_find_string_submatch(const Regexp *re, Alloc *a, Str s);

BURROW_OWNS(ret) Slice regexp_find_submatch_index(const Regexp *re, Alloc *a, Slice b);
BURROW_OWNS(ret) Slice regexp_find_string_submatch_index(const Regexp *re, Alloc *a,
                                                         Str s);
BURROW_OWNS(ret) Slice regexp_find_reader_submatch_index(const Regexp *re, Alloc *a,
                                                         IoRuneReader r);

/* The FindAll forms: every match that does not overlap the one before, as a
 * Slice of what the Find form gives for one. n limits how many, and less than
 * zero means all of them. An empty match right after another match is left
 * out, which is Go's rule and what makes a* on "baaab" give "", "aaa", "". */
BURROW_OWNS(ret) Slice regexp_find_all(const Regexp *re, Alloc *a, Slice b, Int n);
BURROW_OWNS(ret) Slice regexp_find_all_string(const Regexp *re, Alloc *a, Str s, Int n);
BURROW_OWNS(ret) Slice regexp_find_all_index(const Regexp *re, Alloc *a, Slice b,
                                             Int n);
BURROW_OWNS(ret) Slice regexp_find_all_string_index(const Regexp *re, Alloc *a, Str s,
                                                    Int n);
BURROW_OWNS(ret) Slice regexp_find_all_submatch(const Regexp *re, Alloc *a, Slice b,
                                                Int n);
BURROW_OWNS(ret) Slice regexp_find_all_string_submatch(const Regexp *re, Alloc *a,
                                                       Str s, Int n);
BURROW_OWNS(ret) Slice regexp_find_all_submatch_index(const Regexp *re, Alloc *a,
                                                      Slice b, Int n);
BURROW_OWNS(ret) Slice regexp_find_all_string_submatch_index(const Regexp *re, Alloc *a,
                                                             Str s, Int n);

/* Regexp.Split: s cut around the matches, at most n pieces with the rest in
 * the last when n is more than zero, none when it is zero, all of them when
 * it is less. A Slice of Str into s. */
BURROW_OWNS(ret) Slice regexp_split(const Regexp *re, Alloc *a, Str s, Int n);

/* -------------------------------------------------------------- replacing
 *
 * Each match replaced, in a new string or slice from a. In repl, $1 or ${1}
 * is group 1 and $name or ${name} is the group called that, the longest run
 * of letters, digits and underscores being the name, so $1x means the group
 * called 1x and ${1}x is group 1 then x. A group that is not there, or did not
 * take part, is the empty string. $$ is a dollar sign. The Literal forms take
 * repl as it is, and the Func forms call repl with the text of each match. */

BURROW_OWNS(ret) Slice regexp_replace_all(const Regexp *re, Alloc *a, Slice src,
                                          Slice repl);
BURROW_OWNS(ret) Str regexp_replace_all_string(const Regexp *re, Alloc *a, Str src,
                                               Str repl);
BURROW_OWNS(ret) Slice regexp_replace_all_literal(const Regexp *re, Alloc *a, Slice src,
                                                  Slice repl);
BURROW_OWNS(ret) Str regexp_replace_all_literal_string(const Regexp *re, Alloc *a,
                                                       Str src, Str repl);
BURROW_OWNS(ret) Slice regexp_replace_all_func(const Regexp *re, Alloc *a, Slice src,
                                               BytesFunc repl);
BURROW_OWNS(ret) Str regexp_replace_all_string_func(const Regexp *re, Alloc *a, Str src,
                                                    StrFunc repl);

/* Regexp.Expand and ExpandString: template with its $ references filled in
 * from match, an index slice from one of the SubmatchIndex functions over
 * src, appended to dst the way Go's append does it. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, dst) Slice regexp_expand(const Regexp *re,
                                                              Alloc *a, Slice dst,
                                                              Slice template_,
                                                              Slice src, Slice match);
BURROW_OWNS(ret) BURROW_BORROWS(ret, dst) Slice regexp_expand_string(
    const Regexp *re, Alloc *a, Slice dst, Str template_, Str src, Slice match);

/* ------------------------------------------------------------------ text form */

/* Regexp.AppendText and MarshalText: the pattern, appended to b or on its
 * own. The error is only ever the allocator running out. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, b) Slice regexp_append_text(const Regexp *re,
                                                                 Alloc *a, Slice b,
                                                                 Error *err);
BURROW_OWNS(ret) Slice regexp_marshal_text(const Regexp *re, Alloc *a, Error *err);

/* Regexp.UnmarshalText: compiles text, with regexp_compile, into re, which
 * gives back what it held before. On an error re is left as it was. What re
 * holds now comes from a. Not safe while another thread is using re. */
BURROW_OWNS(ret) Error regexp_unmarshal_text(Regexp *re, Alloc *a, Slice text);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_REGEXP_H */

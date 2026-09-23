/* strconv, which turns numbers, booleans and quoted strings into text and back.
 *
 * It is a port of Go's package, and the answers are Go's answers to the byte:
 * a string quoted here is the literal Go would print, and a literal unquoted
 * here is the string Go would read. Nothing in it depends on the C locale,
 * which is the first thing that goes wrong with printf and strtod.
 *
 * The functions that can fail return the value and report the error through a
 * trailing Error pointer, which may be NULL if you only want the value. Their
 * errors live in the calling goroutine's error arena, so parsing never needs an
 * allocator for the error, only for a result that has to be built.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package strconv */

#ifndef BURROW_STRCONV_H
#define BURROW_STRCONV_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/mem.h"
#include "burrow/slice.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------- errors */

/* strconv.ErrRange, for a value that is well formed but does not fit. */
extern const Error strconv_err_range;

/* strconv.ErrSyntax, for input that is not the thing it was meant to be. */
extern const Error strconv_err_syntax;

/* strconv.NumError, which is what every Parse function and Atoi fail with.
 *
 * func is the Go name of the function that failed, such as "ParseInt", num is
 * the input, and err is the reason: strconv_err_range, strconv_err_syntax, or
 * an error saying the base or the bit size was not one it accepts. errors_is
 * sees through to err, so
 *
 *     if (errors_is(err, strconv_err_range))
 *
 * works on what strconv_parse_int gives you, and errors_as with
 * TYPE_STRCONV_NUM_ERROR gets you the struct.
 *
 * The errors the Parse functions return live in the calling goroutine's error
 * arena, num included, which is a copy of the input and not a view of it. Keep
 * one past the end of the goroutine with error_retain. */
typedef struct StrconvNumError {
    Str func;
    Str num;
    Error err;
} StrconvNumError;

extern const Type *const TYPE_STRCONV_NUM_ERROR;

/* The text Go's Error method gives, such as
 *
 *     strconv.ParseInt: parsing "12a": invalid syntax
 *
 * built in a. For an error that came out of a Parse function error_text is the
 * same text and needs no allocator. This is for a StrconvNumError you filled in
 * yourself. */
BURROW_OWNS(ret) Str strconv_num_error_error(Alloc *a, const StrconvNumError *e);

/* e->err, which is what errors_unwrap gives for one inside an Error. */
BURROW_BORROWS(ret, e) Error strconv_num_error_unwrap(const StrconvNumError *e);

/* An Error for a StrconvNumError you filled in yourself, the way Go code writes
 * &strconv.NumError{...}. func, num and the message are copied into a. err is
 * kept as it is, so it has to live as long as the result does. On an
 * allocation failure you get burrow_err_out_of_memory. */
BURROW_OWNS(ret) Error strconv_num_error_as_error(Alloc *a, const StrconvNumError *e);

/* ----------------------------------------------------------------- integers */

/* strconv.IntSize, the width of Int in bits. */
#define STRCONV_INT_SIZE BURROW_PTR_BITS

/* strconv.ParseInt. s may start with + or -. base is 2 to 36, or 0 to take it
 * from a prefix the way Go source does: 0b, 0o or a bare 0, and 0x, and in that
 * case only underscores between digits are allowed too. bit_size is 0 for Int
 * and otherwise 8, 16, 32 or 64, or anything up to 64.
 *
 * Bad input gives strconv_err_syntax and 0. A number that does not fit gives
 * strconv_err_range and the closest value that does, the largest or the
 * smallest of the size asked for. Both come wrapped in a StrconvNumError. */
int64_t strconv_parse_int(Str s, Int base, Int bit_size, Error *err);

/* strconv.ParseUint, which is ParseInt without the sign. */
uint64_t strconv_parse_uint(Str s, Int base, Int bit_size, Error *err);

/* strconv.Atoi, which is strconv_parse_int(s, 10, 0, err) and faster for the
 * short numbers that most input is. */
Int strconv_atoi(Str s, Error *err);

/* strconv.FormatInt and FormatUint: i in base 2 to 36, with a to z for the
 * digits past 9. Any other base is a panic, as it is in Go, because it is a bug
 * in the caller rather than something the input did. On an allocation failure
 * the result is empty. */
BURROW_OWNS(ret) Str strconv_format_int(Alloc *a, int64_t i, Int base);
BURROW_OWNS(ret) Str strconv_format_uint(Alloc *a, uint64_t i, Int base);

/* strconv.Itoa, which is strconv_format_int(a, i, 10). */
BURROW_OWNS(ret) Str strconv_itoa(Alloc *a, Int i);

/* strconv.AppendInt and AppendUint, the Format forms added to dst the way the
 * quoting Append forms add to it. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, dst) Slice strconv_append_int(Alloc *a, Slice dst,
                                                                   int64_t i, Int base);
BURROW_OWNS(ret) BURROW_BORROWS(ret, dst) Slice strconv_append_uint(Alloc *a, Slice dst,
                                                                    uint64_t i,
                                                                    Int base);

/* ----------------------------------------------------------------- booleans */

/* strconv.ParseBool, which takes 1, t, T, TRUE, true and True, and their
 * opposites 0, f, F, FALSE, false and False. Anything else is
 * strconv_err_syntax in a StrconvNumError, and false. */
bool strconv_parse_bool(Str s, Error *err);

/* strconv.FormatBool: "true" or "false". They are literals, so there is
 * nothing to allocate and nothing to free. */
BURROW_STATIC(ret) Str strconv_format_bool(bool b);

/* strconv.AppendBool. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, dst) Slice strconv_append_bool(Alloc *a, Slice dst,
                                                                    bool b);

/* ------------------------------------------------------------------- quoting
 *
 * Quote gives you a double quoted Go string literal. Printable runes, as
 * strconv_is_print defines them, go through as they are, and everything else
 * becomes an escape: \n and friends where Go has a short one, \x for a byte
 * that is not valid UTF-8, and \u or \U for the rest. The ToASCII forms escape
 * every rune outside ASCII as well, and the ToGraphic forms let through the
 * spaces that Unicode calls graphic and IsPrint does not, such as U+00A0.
 *
 * The rune forms give a single quoted character literal. A rune that is not a
 * valid code point is quoted as U+FFFD, which is what Go does.
 *
 * The Append forms add the literal to the end of dst the way append does, which
 * means in place when dst has room and in a new array from a when it does not.
 * A zero Slice is fine as dst and means a new []byte. On an allocation failure
 * the Quote forms return an empty Str and the Append forms a nil Slice. */

BURROW_OWNS(ret) Str strconv_quote(Alloc *a, Str s);
BURROW_OWNS(ret) Str strconv_quote_to_ascii(Alloc *a, Str s);
BURROW_OWNS(ret) Str strconv_quote_to_graphic(Alloc *a, Str s);
BURROW_OWNS(ret) Str strconv_quote_rune(Alloc *a, Rune r);
BURROW_OWNS(ret) Str strconv_quote_rune_to_ascii(Alloc *a, Rune r);
BURROW_OWNS(ret) Str strconv_quote_rune_to_graphic(Alloc *a, Rune r);

BURROW_OWNS(ret) BURROW_BORROWS(ret, dst) Slice strconv_append_quote(Alloc *a,
                                                                     Slice dst, Str s);
BURROW_OWNS(ret) BURROW_BORROWS(ret, dst) Slice strconv_append_quote_to_ascii(Alloc *a,
                                                                              Slice dst,
                                                                              Str s);
BURROW_OWNS(ret) BURROW_BORROWS(ret, dst) Slice
strconv_append_quote_to_graphic(Alloc *a, Slice dst, Str s);
BURROW_OWNS(ret) BURROW_BORROWS(ret, dst) Slice strconv_append_quote_rune(Alloc *a,
                                                                          Slice dst,
                                                                          Rune r);
BURROW_OWNS(ret) BURROW_BORROWS(ret, dst) Slice
strconv_append_quote_rune_to_ascii(Alloc *a, Slice dst, Rune r);
BURROW_OWNS(ret) BURROW_BORROWS(ret, dst) Slice
strconv_append_quote_rune_to_graphic(Alloc *a, Slice dst, Rune r);

/* Whether s can be written between backquotes as it is, on one line, with no
 * control characters other than tab. A byte order mark is a no as well, since
 * it would be invisible in the literal. */
bool strconv_can_backquote(Str s);

/* ----------------------------------------------------------------- unquoting */

/* strconv.Unquote. s is a whole Go literal: double quoted, single quoted or
 * backquoted. A single quoted one gives back its one character as a string.
 *
 * When the literal has nothing to undo the result is the inside of s and no
 * memory is used, which is why it can borrow as well as own. A backquoted
 * literal drops any carriage returns, as the Go spec says it does. Anything
 * malformed, including trailing bytes after the closing quote, is
 * strconv_err_syntax and an empty result. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, s) Str strconv_unquote(Alloc *a, Str s,
                                                            Error *err);

/* strconv.QuotedPrefix: the quoted literal at the start of s, quotes and all
 * and still escaped, or strconv_err_syntax when s does not start with one. */
BURROW_BORROWS(ret, s) Str strconv_quoted_prefix(Str s, Error *err);

/* strconv.UnquoteChar: the first character of the escaped text in s, which is
 * the inside of a literal and not the literal itself.
 *
 * The rune is returned. multibyte says whether it needs more than one byte of
 * UTF-8, which is false for \x and octal escapes because those stand for a
 * byte, not a rune. tail is what is left of s after it. quote is the kind of
 * literal being read: a single quote allows \' and rejects a bare ', a double
 * quote does the same for ", and zero allows both bare and neither escaped. */
Rune strconv_unquote_char(Str s, Byte quote, bool *multibyte, Str *tail, Error *err);

/* ---------------------------------------------------------------- printable */

/* Whether Go calls r printable: letters, marks, numbers, punctuation, symbols
 * and the ASCII space, the same set as unicode.IsPrint. It is its own table so
 * that quoting does not need all of Unicode's. */
bool strconv_is_print(Rune r);

/* Whether Unicode calls r graphic, which is IsPrint plus the other spaces. */
bool strconv_is_graphic(Rune r);

/* Not API. Writes strconv_quote(s) to dst, which must have room, and returns
 * its length. With dst NULL it only counts. It is how a NumError's message is
 * built at exactly its size. */
Int burrow__strconv_quote_into(Byte *dst, Str s);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_STRCONV_H */

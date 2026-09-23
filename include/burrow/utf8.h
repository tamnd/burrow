/* unicode/utf8, the package that turns bytes into runes and back.
 *
 * Go's strings are UTF-8 by convention rather than by enforcement, and burrow's
 * Str is the same: a pointer and a length, holding whatever bytes you put in
 * it. This header is what reads those bytes as text, and it is a line for line
 * port of Go's package, including the parts of it that look surprising.
 *
 * The surprising part, and the one worth knowing before you use any of this, is
 * what happens to bytes that are not valid UTF-8. Nothing here fails. A decode
 * of an invalid sequence gives you UTF8_RUNE_ERROR and a width of one byte, so
 * a loop over a corrupt string still terminates and still makes progress, one
 * replacement character at a time. That is Go's answer and it is the reason a
 * Go program handed a truncated file prints mojibake instead of crashing.
 *
 * The one ambiguity that creates is worth naming. A decode that returns
 * UTF8_RUNE_ERROR with a size of 3 read a real U+FFFD that was in the input.
 * The same rune with a size of 1 is an error. Go has the same pair and the same
 * way of telling them apart.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package unicode/utf8 */

#ifndef BURROW_UTF8_H
#define BURROW_UTF8_H

#include "burrow/core.h"
#include "burrow/mem.h"
#include "burrow/slice.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The four numbers the encoding is built out of.
 *
 * UTF8_RUNE_ERROR is U+FFFD, the replacement character, and it is what every
 * function here gives you for input it cannot read. UTF8_RUNE_SELF is the point
 * where a byte stops standing for itself, so anything below it is ASCII and is
 * its own encoding. UTF8_MAX_RUNE is the largest code point Unicode has, and
 * UTF8_UTF_MAX is the most bytes one of them takes.
 *
 * Three of them also have a BURROW_ spelling in burrow/core.h, because runes
 * turn up in headers that have no business including this one. Same numbers,
 * and these are defined in terms of those so they cannot drift apart. */
#define UTF8_RUNE_ERROR BURROW_RUNE_ERROR
#define UTF8_RUNE_SELF ((Rune)0x80)
#define UTF8_MAX_RUNE BURROW_RUNE_MAX
#define UTF8_UTF_MAX BURROW_UTF8_MAX

/* ------------------------------------------------------------------ decoding
 *
 * Every decode returns the rune and writes the width in bytes through size,
 * which may be NULL if you do not want it. An empty input gives
 * UTF8_RUNE_ERROR with a size of zero, and an invalid encoding gives
 * UTF8_RUNE_ERROR with a size of one. Neither pair can come out of correct
 * non-empty UTF-8, which is what makes them usable as a signal.
 *
 * An encoding is invalid if it is malformed, if it encodes a rune that is out
 * of range, if it is a surrogate half, or if it is longer than it needed to be.
 * That last one matters for security: the overlong encoding of a slash is not a
 * slash here, and a filter that only looked for the short one is a filter
 * somebody will walk straight past. */

/* Go's utf8.DecodeRuneInString. This is the one you want for a Str. */
Rune utf8_decode_rune_in_string(Str s, Int *size);

/* Go's utf8.DecodeRune, over a slice of bytes. Same function, different input,
 * and both exist because both exist in Go. */
Rune utf8_decode_rune(Slice p, Int *size);

/* The same from the other end, which is what you need to walk a string
 * backwards. Both are O(1), because a rune is at most four bytes and the search
 * for the start of the last one gives up after that. */
Rune utf8_decode_last_rune_in_string(Str s, Int *size);
Rune utf8_decode_last_rune(Slice p, Int *size);

/* Whether the input begins with a complete encoding. A truncated sequence at
 * the end of a buffer is the case this exists for, which is every program
 * reading a stream in fixed size pieces. An invalid sequence counts as full,
 * since it decodes as a one byte error rune and waiting for more input would
 * not change that. */
bool utf8_full_rune_in_string(Str s);
bool utf8_full_rune(Slice p);

/* ------------------------------------------------------------------ encoding */

/* How many bytes r takes, or -1 if r cannot be encoded at all, which means it
 * is negative, above UTF8_MAX_RUNE, or one half of a surrogate pair. */
Int utf8_rune_len(Rune r);

/* Writes the encoding of r into p and returns how many bytes that took. A rune
 * that cannot be encoded is written as UTF8_RUNE_ERROR, which takes three, so
 * the result is never zero and never negative.
 *
 * p has to have room. Go's version indexes without checking and so does this
 * one, because the caller always knows: either the destination is at least
 * UTF8_UTF_MAX bytes, or it was sized with utf8_rune_len first. */
Int utf8_encode_rune(Slice p, Rune r);

/* The same encoding appended to a byte slice, which is the one to reach for
 * when you are building a string rather than filling a buffer. Takes an
 * allocator because appending can grow. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, p) Slice utf8_append_rune(Alloc *a, Slice p,
                                                               Rune r);

/* ------------------------------------------------------------------ counting */

/* How many runes are in the input. An invalid or truncated encoding counts as
 * one rune of one byte, the same way decoding treats it, so this never
 * disagrees with a loop that decodes. */
Int utf8_rune_count_in_string(Str s);
Int utf8_rune_count(Slice p);

/* Whether b could be the first byte of an encoding. Continuation bytes always
 * have their top two bits set to 10 and nothing else does, so this is one mask
 * and one compare, and it is how you find a rune boundary in a buffer you
 * arrived in the middle of. */
bool utf8_rune_start(Byte b);

/* Whether the whole input is valid UTF-8, with no error runes and nothing
 * truncated. This is the check to run on bytes that came from outside your
 * program, once, at the boundary, rather than on every decode afterwards. */
bool utf8_valid_string(Str s);
bool utf8_valid(Slice p);

/* Whether r can be encoded at all. Out of range or a surrogate half is a no,
 * and everything else is a yes, including runes that are not assigned to any
 * character yet. */
bool utf8_valid_rune(Rune r);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_UTF8_H */

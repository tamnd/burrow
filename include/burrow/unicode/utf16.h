/* unicode/utf16, runes to UTF-16 and back.
 *
 * UTF-16 is what Windows, Java and JavaScript use for text, so this is the
 * package you need at the edge of any of them: a wide string from a Win32 call,
 * a JSON \u escape, a Java class file. A rune below U+10000 is one 16 bit unit.
 * Anything above takes two, a surrogate pair, where the first is in
 * 0xD800-0xDBFF and the second is in 0xDC00-0xDFFF.
 *
 * Like unicode/utf8, nothing here fails. A rune that cannot be encoded comes
 * out as U+FFFD, and so does a surrogate half with no partner on the way back.
 *
 * The slices are Slice with an element type, the way Go's are typed: Encode
 * takes runes (TYPE_RUNE) and gives back uint16_t units (TYPE_UINT16), and
 * Decode goes the other way. The functions that allocate take an Alloc, and
 * the result belongs to whoever called.
 *
 *     Rune in[] = {'a', 0x1F600};
 *     Slice units = utf16_encode(a, slice_from(in, 2, 2, TYPE_RUNE));
 *     // units holds 0x0061 0xD83D 0xDE00
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package unicode/utf16 */

#ifndef BURROW_UNICODE_UTF16_H
#define BURROW_UNICODE_UTF16_H

#include "burrow/core.h"
#include "burrow/mem.h"
#include "burrow/slice.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Whether r is one half of a surrogate pair, 0xD800 through 0xDFFF. A rune in
 * that range is not a character and cannot be written as UTF-8. */
bool utf16_is_surrogate(Rune r);

/* The rune that the surrogate pair r1, r2 stands for. If they are not a pair,
 * because r1 is not a high half or r2 is not a low half, the result is U+FFFD. */
Rune utf16_decode_rune(Rune r1, Rune r2);

/* The surrogate pair for r, with the first half returned and the second
 * written through r2, which may be NULL. A rune that needs no pair, or that is
 * not a valid rune at all, gives U+FFFD for both. */
Rune utf16_encode_rune(Rune r, Rune *r2);

/* How many 16 bit units r takes: 1 or 2, or -1 if r is a surrogate half, is
 * negative, or is above U+10FFFF. */
Int utf16_rune_len(Rune r);

/* The UTF-16 encoding of the runes in s, as a new slice of uint16_t. */
BURROW_OWNS(ret) Slice utf16_encode(Alloc *a, Slice s);

/* Appends the encoding of r to p, a slice of uint16_t, and returns it with
 * append's usual rules about sharing. A Slice with no element type is taken as
 * a nil []uint16, the same way utf8_append_rune treats a zeroed Slice. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, p) Slice utf16_append_rune(Alloc *a, Slice p,
                                                                Rune r);

/* The runes that the uint16_t units in s encode, as a new slice of Rune. The
 * result is never nil, even for empty input, which Go's tests check. */
BURROW_OWNS(ret) Slice utf16_decode(Alloc *a, Slice s);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_UNICODE_UTF16_H */

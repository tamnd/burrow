/* Slices, the type most of this library returns.
 *
 * A pointer, a length, a capacity, and the element's type descriptor.
 *
 * Go's header is three words and this one is four. The fourth is what buys the
 * whole thing: with the element descriptor in hand, one implementation of
 * append works for every element type, and reflect, fmt and encoding/json can
 * see into a slice nobody told them about. C has no templates, so the choice is
 * between carrying a descriptor pointer and generating a slice type per element
 * with a macro, and the macro version cannot be passed to a function that does
 * not already know the element type. It is one word, it points at a static
 * object, and it costs nothing to initialise.
 *
 * The fields are public because Go's len and cap are not function calls either
 * and because half the loops in the library are `for (Int i = 0; i < s.len;
 * i++)`. Read them freely. Write them only if you are certain, since nothing
 * checks that len is within cap after you have done it.
 *
 * This is its own header rather than part of burrow/core.h because a Slice
 * points at a Type and a Type contains a Str, so the three have to be declared
 * in that order and each one needs the one before it. Include burrow/burrow.h
 * and the order is somebody else's problem.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SLICE_H
#define BURROW_SLICE_H

#include "burrow/core.h"
#include "burrow/mem.h"
#include "burrow/type.h"

#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Slice {
    void *p;
    Int len;
    Int cap;
    const Type *elem;
} Slice;

/* make([]T, len, cap), zeroed, because Go's zero value rule is the language
 * and not a convention.
 *
 * Passing cap < 0, len < 0 or len > cap is a fatal error carrying the text Go
 * panics with, since none of those is a condition a caller can sensibly handle
 * and all of them mean the arithmetic that produced them was wrong.
 *
 * A failed allocation gives you the nil slice for elem, the same way str_clone
 * gives you the empty string, because a Slice has no spare value to signal with
 * and the alternative is a second out parameter on the most common call in the
 * library. Check `p == NULL && cap > 0` if you need to tell that apart. */
BURROW_OWNS(ret) Slice slice_make(Alloc *a, const Type *elem, Int len, Int cap);

/* The nil slice of a given element type.
 *
 * Go keeps nil and empty distinct and the difference is observable: a nil slice
 * marshals to null and an empty one to [], and that shows up in the output of
 * every program that encodes JSON. So a zeroed Slice with an element type is
 * nil, and a zero length slice with a real pointer is empty, exactly as in Go. */
BURROW_STATIC(ret) Slice slice_nil(const Type *elem);
bool slice_is_nil(Slice s);

/* A slice over memory you already have, which does not copy and does not take
 * ownership. The result lives exactly as long as p does. This is the bridge
 * from a C array, and it is also how you hand burrow a stack buffer. */
BURROW_BORROWS(ret, p) Slice slice_from(void *p, Int len, Int cap, const Type *elem);

/* Go's s[i], bounds checked against len and not against cap, returning a
 * pointer to the element rather than the element, because C cannot return a
 * value whose type is only known at runtime. Use BURROW_AT to get the value.
 *
 * Out of range stops the program with the message Go prints. That is not
 * optional and not behind a build flag, for the reasons written out over
 * str_at. */
BURROW_BORROWS(ret, s) void *slice_at(Slice s, Int i);

/* s[lo:hi] and s[lo:hi:max].
 *
 * The bounds are Go's: 0 <= lo <= hi <= cap for the two index form, and
 * 0 <= lo <= hi <= max <= cap for the three index one. Note that both are
 * checked against cap rather than len, which surprises people the first time
 * they reslice past the length on purpose and is exactly what Go does.
 *
 * Neither copies. The result points into the same backing array, so writing
 * through it is visible through the original, which is the entire reason
 * slicing is cheap. */
BURROW_BORROWS(ret, s) Slice slice_sub(Slice s, Int lo, Int hi);
BURROW_BORROWS(ret, s) Slice slice_sub3(Slice s, Int lo, Int hi, Int max);

/* append(s, elems...), with Go's semantics including the part people trip on.
 *
 * When cap is big enough the elements are written into the existing backing
 * array and the returned header shares it with s. Anything else holding a
 * slice of that array sees the new elements. When cap is not big enough a new
 * array is allocated and the old one is left alone, so the same two slices now
 * disagree. Go behaves this way, Go's tests depend on it, and code ported from
 * Go would break if we quietly always copied.
 *
 * elems points at n contiguous elements of s.elem's type. It may point into s
 * itself, which is append(s, s...) and which works here for the same reason it
 * works in Go: the copy happens after the allocation.
 *
 * n <= 0 returns s unchanged, which is append(s) with nothing to add. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, s) Slice slice_append(Alloc *a, Slice s,
                                                           const void *elems, Int n);

/* append(dst, src...). The element sizes have to match. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, dst) Slice slice_append_slice(Alloc *a, Slice dst,
                                                                   Slice src);

/* copy(dst, src), returning the number of elements copied, which is the
 * smaller of the two lengths. Overlapping is fine and is what Go's copy
 * promises, so this is a memmove and not a memcpy. */
Int slice_copy(Slice dst, Slice src);

/* copy(dst, src) where src is a string, which Go allows for a []byte
 * destination and which turns up in every buffer implementation there is. */
Int slice_copy_str(Slice dst, Str src);

/* []byte(s) and string(b), both of which copy in Go and both of which copy
 * here. The result of the first has TYPE_BYTE as its element type.
 *
 * Neither borrows. If you want the cheap version, s.p and s.len are right
 * there and you already know whether the lifetime works out. */
BURROW_OWNS(ret) Slice slice_from_str(Alloc *a, Str s);
BURROW_OWNS(ret) Str str_from_slice(Alloc *a, Slice s);

/* The same two operations again, told the element size by the caller.
 *
 * Go compiles s[i] and append(s, v) into a bounds check and a move, with a call
 * only when append has to grow. burrow cannot do that from a .c file, because
 * the element size comes out of a descriptor at runtime, so the copy is a call
 * into memcpy with a length nothing can see, and the four word header goes out
 * to the stack on the way in and comes back through the stack on the way out.
 * On a plain x86-64 build that costs about eighteen nanoseconds an append, and
 * a profile puts most of it in the caller shuffling thirty two bytes each way
 * rather than in the append itself.
 *
 * Passing the size in fixes both halves at once. The size is sizeof(T) at the
 * call site, so the copy becomes a single store, and the whole thing inlines,
 * so the header stays in registers and never touches memory.
 *
 * Getting the size wrong is a performance mistake and not a correctness one.
 * The size is checked against the descriptor and anything that does not match
 * falls through to slice_at or slice_append, which use the descriptor's size
 * the way they always did. So does a nil pointer, a full slice, or an index out
 * of range, which means the failure messages and the growth behaviour are in
 * one place still.
 *
 * Use BURROW_AT and BURROW_APPEND rather than calling these. They exist to be
 * what the macros expand to. */
BURROW_BORROWS(ret, s) static inline void *slice_at_fast(Slice s, Int i,
                                                         size_t elem_size) {
    if (s.elem != NULL && s.p != NULL && (Uint)i < (Uint)s.len &&
        (size_t)s.elem->size == elem_size)
        return (Byte *)s.p + (size_t)i * elem_size;
    return slice_at(s, i);
}

BURROW_OWNS(ret) BURROW_BORROWS(ret, s) static inline Slice
slice_append_fast(Alloc *a, Slice s, const void *elem, size_t elem_size) {
    if (s.elem != NULL && s.p != NULL && s.len < s.cap &&
        (size_t)s.elem->size == elem_size) {
        memcpy((Byte *)s.p + (size_t)s.len * elem_size, elem, elem_size);
        s.len++;
        return s;
    }
    return slice_append(a, s, elem, 1);
}

/* Typed access, which is where the static typing C does have comes back.
 *
 *     Slice parts = strings_split(a, line, BURROW_S(","));
 *     for (Int i = 0; i < parts.len; i++) {
 *         Str f = BURROW_AT(Str, parts, i);
 *     }
 *
 * BURROW_AT gives an lvalue, so assigning through it is s[i] = v:
 *
 *     BURROW_AT(Int, xs, 0) = 42;
 *
 * The T you pass is not checked against the slice's element descriptor at
 * compile time, because there is nothing at compile time to check it against.
 * Its size is checked at runtime, and a T of the wrong size gets you the same
 * answer slice_at would have given, which is the element the descriptor says is
 * there read as the type you asked for. Getting it wrong is the same mistake as
 * getting a printf format wrong and it has the same flavour of consequence, so
 * pass the type the slice actually holds. */
#define BURROW_AT(T, s, i) (*(T *)slice_at_fast((s), (i), sizeof(T)))

/* append(s, v) for a single value, without the temporary:
 *
 *     xs = BURROW_APPEND(Int, a, xs, 42);
 *
 * The compound literal lives until the end of the enclosing block, which
 * outlasts the call, so there is nothing dangling here. */
#define BURROW_APPEND(T, a, s, v)                                                      \
    slice_append_fast((a), (s), (const T[]){(v)}, sizeof(T))

#if defined(BURROW_SHORT) && BURROW_SHORT
#define AT(T, s, i) BURROW_AT(T, s, i)
#define APPEND(T, a, s, v) BURROW_APPEND(T, a, s, v)
#endif

#ifdef __cplusplus
}
#endif

#endif /* BURROW_SLICE_H */

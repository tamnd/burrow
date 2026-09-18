/* The types Go has built in, spelled in C.
 *
 * Go's builtins are not a package, so there is nothing to name them after, and
 * they get the short names they deserve: Str, Slice, Map, Error, Int. The
 * operations on them are str_, slice_, map_ and so on, which is the same rule
 * the ported packages follow with the package name in front.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_CORE_H
#define BURROW_CORE_H

#include "burrow/mem.h"
#include "burrow/platform.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------- numbers
 *
 * Go's fixed width types are C's fixed width types, so uint8 is uint8_t and
 * there is nothing to say about them. These are the four that need a name.
 *
 * Int is the one decision worth explaining. Go's int is 64 bits on a 64 bit
 * platform and 32 on a 32 bit one, and a fixed int64_t would have been simpler
 * and wrong. len() returns int, strings_index returns int, and the overflow
 * behaviour of a 32 bit int is visible in Go's own tests, in strconv, in
 * bytes.Repeat's overflow check and in slices.Grow's capacity arithmetic. A
 * port that is wider than the original passes tests the original fails. */

typedef uint8_t Byte;
typedef int32_t Rune;

#if BURROW_PTR_BITS == 64
typedef int64_t Int;
typedef uint64_t Uint;
#define BURROW_INT_MAX INT64_MAX
#define BURROW_INT_MIN INT64_MIN
#else
typedef int32_t Int;
typedef uint32_t Uint;
#define BURROW_INT_MAX INT32_MAX
#define BURROW_INT_MIN INT32_MIN
#endif

typedef uintptr_t Uintptr;

/* Structs rather than C99 _Complex, because MSVC has no _Complex in C mode and
 * because Go pins the behaviour of complex arithmetic on infinities and NaNs in
 * math/cmplx's tests. Those tests are the specification, so the arithmetic is
 * ported over these rather than handed to <complex.h>. */
typedef struct Complex64 {
    float re, im;
} Complex64;

typedef struct Complex128 {
    double re, im;
} Complex128;

/* The rune returned when a decode fails, which is U+FFFD. Go returns it rather
 * than an error from every rune loop in the library, so it turns up a lot. */
#define BURROW_RUNE_ERROR ((Rune)0xFFFD)
#define BURROW_RUNE_MAX ((Rune)0x10FFFF)
#define BURROW_UTF8_MAX 4

/* ------------------------------------------------------------- strings
 *
 * A pointer and a length, by value, never NUL terminated.
 *
 * This is the most consequential decision in the whole API and the temptation
 * to use char * has to be refused every single time. A Go string can contain a
 * NUL byte. strings.Split("a\x00b", "\x00") is a meaningful call with a
 * meaningful answer. os.ReadFile on a JPEG hands back bytes that are a perfectly
 * legal Go string. Every place a char * gets in, a truncation bug follows it,
 * and the bug is always in the one input somebody sent on purpose.
 *
 * Immutability is a contract rather than a guarantee, because C cannot give the
 * guarantee. p is const, no function in burrow writes through it, and a function
 * that returns a Str says on its declaration whether the result points into an
 * input or is fresh memory from the allocator you passed. */

typedef struct Str {
    const Byte *p;
    Int len;
} Str;

/* The workhorse. Compiles to a constant with no call and no strlen, so it costs
 * nothing at all to write it in the middle of a condition:
 *
 *     if (strings_has_prefix(path, BURROW_S("/api/")))
 *
 * Only ever hand it a string literal. Handing it a char * variable gives you
 * sizeof a pointer, which is a bug the compiler cannot see. */
#define BURROW_S(lit) ((Str){(const Byte *)("" lit), (Int)(sizeof(lit) - 1)})

/* Printing one with printf, which you will want on your first afternoon:
 *
 *     printf("path is " BURROW_STR_FMT "\n", BURROW_STR_ARG(path));
 *
 * The cast to int is what %.*s needs and it is safe for any string that came
 * from this library, since Str lengths are bounded by Int. */
#define BURROW_STR_FMT "%.*s"
#define BURROW_STR_ARG(s) (int)(s).len, (const char *)(s).p

/* The empty string, which is also what a zeroed Str is, which is why nothing in
 * burrow has to check for a NULL pointer before reading a length. */
#define BURROW_STR_EMPTY ((Str){NULL, 0})

/* O(n) in the length of s and it does not allocate. The result points into s,
 * so it lives exactly as long as s does. NULL gives the empty string rather
 * than a crash, because the callers of this are at the boundary with other
 * people's code and other people's code returns NULL. */
BURROW_BORROWS(ret, s) Str str_from_cstr(const char *s);

/* Also borrows. A negative n gives the empty string. */
BURROW_BORROWS(ret, p) Str str_from_bytes(const void *p, Int n);

/* Allocates, copies, and appends the NUL. Returns NULL if the allocation fails.
 *
 * The result is a real C string only if s has no NUL in it. Ask str_has_nul
 * first when the bytes came from outside your program, because that is exactly
 * the case somebody is going to attack. */
BURROW_OWNS(ret) char *str_to_cstr(Alloc *a, Str s);

bool str_has_nul(Str s);

/* Byte order, which is what Go's < and > on strings do and what sort.Strings
 * uses. Returns a negative number, zero, or a positive number. */
int str_cmp(Str a, Str b);
bool str_eq(Str a, Str b);

/* Fresh memory holding the same bytes, so that the copy outlives whatever the
 * original pointed into. This is strings.Clone, and the reason it exists in Go
 * is the reason it exists here: a small Str cut out of a large buffer keeps the
 * whole buffer alive.
 *
 * A Str has no spare value to mean failure the way a pointer has NULL, so a
 * failed allocation gives you the empty string. Compare lengths if you need to
 * tell that apart from cloning something that was empty already. */
BURROW_OWNS(ret) Str str_clone(Alloc *a, Str s);

/* Here so that nobody writes s.p == NULL, which is the wrong question. A Str
 * with a NULL pointer and a zero length is the empty string and so is a Str
 * pointing at a real buffer with nothing in it. */
bool str_is_empty(Str s);

/* Go's s[i], including what Go does when i is out of range, which is stop.
 *
 * The check is not optional and it is not behind a build flag. Go's tests
 * depend on the failure, code written against Go relies on never reading past
 * the end, and a version of this that trusted the caller would be a different
 * language with the same spelling. The cost is a compare and a branch the
 * processor predicts perfectly.
 *
 * It does not panic yet, because panic needs defer and defer needs the
 * scheduler. Today it is a fatal error carrying the message Go's panic carries.
 * See burrow/runtime.h.
 *
 * When you are walking a string you already bounds checked, index s.p directly
 * and let the loop condition be the check. That is what the library does
 * internally and it is not cheating. */
Byte str_at(Str s, Int i);

/* -------------------------------------------------------------- slices
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
 * checks that len is within cap after you have done it. */

typedef struct Type Type;

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
Slice slice_nil(const Type *elem);
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
BURROW_OWNS(ret) Slice slice_append(Alloc *a, Slice s, const void *elems, Int n);

/* append(dst, src...). The element sizes have to match. */
BURROW_OWNS(ret) Slice slice_append_slice(Alloc *a, Slice dst, Slice src);

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
 * The T you pass is not checked against the slice's element descriptor, because
 * there is nothing at compile time to check it against. Getting it wrong is the
 * same mistake as getting a printf format wrong and it has the same flavour of
 * consequence, so pass the type the slice actually holds. */
#define BURROW_AT(T, s, i) (*(T *)slice_at((s), (i)))

/* append(s, v) for a single value, without the temporary:
 *
 *     xs = BURROW_APPEND(Int, a, xs, 42);
 *
 * The compound literal lives until the end of the enclosing block, which
 * outlasts the call, so there is nothing dangling here. */
#define BURROW_APPEND(T, a, s, v) slice_append((a), (s), (const T[]){(v)}, 1)

#if defined(BURROW_SHORT) && BURROW_SHORT
#define S(lit) BURROW_S(lit)
#define STR_FMT BURROW_STR_FMT
#define STR_ARG(s) BURROW_STR_ARG(s)
#define AT(T, s, i) BURROW_AT(T, s, i)
#define APPEND(T, a, s, v) BURROW_APPEND(T, a, s, v)
#endif

#ifdef __cplusplus
}
#endif

#endif /* BURROW_CORE_H */

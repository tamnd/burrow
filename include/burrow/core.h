/* The types Go has built in, spelled in C.
 *
 * Go's builtins are not a package, so there is nothing to name them after, and
 * they get the short names they deserve: Str, Slice, Map, Error, Int. The
 * operations on them are str_, slice_, map_ and so on, which is the same rule
 * the ported packages follow with the package name in front.
 *
 * Two of Go's rules live here as well, the zero value and the shape of a
 * function with more than one result, because they apply to every type in the
 * library rather than to one of them. Arithmetic that C and Go disagree about
 * is in burrow/num.h.
 *
 * Slice is not here, it is in burrow/slice.h. It carries a pointer to a type
 * descriptor and the descriptor carries a Str, so the order has to be Str then
 * Type then Slice, and a header cannot come before the one it depends on.
 * Include burrow/burrow.h and stop thinking about it.
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

/* ------------------------------------------------------- zero values and outs
 *
 * Two of Go's rules turn up in every header in the tree, so they get their
 * spelling here rather than being described nine times.
 *
 * The first is that every type has a useful zero value. var buf bytes.Buffer is
 * a working empty buffer in Go, a nil map reads as empty, a nil slice appends,
 * and a Mutex is unlocked. C gives the same bit pattern for free with = {0}, so
 * the rule transfers on one condition: no type in burrow may need non zero
 * initialisation. That is a constraint on the design of every struct here and
 * it is why SyncMutex will be an atomic word rather than a pthread_mutex_t,
 * whose initialiser is not portably all zero.
 *
 * BURROW_ZERO(T) is the spelling for a value, which is what you want inside an
 * expression:
 *
 *     Str empty = BURROW_ZERO(Str);
 *     return BURROW_ZERO(Slice);
 *
 * It is a compound literal, so it cannot initialise something with static
 * storage. Write = {0} by hand there, which is the same bits and is a constant
 * expression.
 *
 * The rule is checked rather than asserted. C cannot evaluate str_is_empty at
 * compile time, so a static assertion is not available, and what stands in for
 * it is a test per type in tests/core_test.c that takes the zero value and
 * exercises it. */
#define BURROW_ZERO(T) ((T){0})

/* The second rule is how a Go function with more than one result is spelled.
 * The first result comes back, everything after it is an out parameter at the
 * end of the parameter list in Go's order, error is always last, and any out
 * parameter may be NULL to throw that result away:
 *
 *     func Atoi(s string) (int, error)
 *     Int strconv_atoi(Str s, Error *err);
 *
 *     n := strconv.Atoi(s)         Int n = strconv_atoi(s, NULL);
 *     n, err := strconv.Atoi(s)    Int n = strconv_atoi(s, &err);
 *
 * NULL being allowed is the part that has to hold everywhere, because a caller
 * who does not care about the second result should not have to declare a
 * variable for it. BURROW_OUT is what that looks like on the writing side:
 *
 *     BURROW_OUT(err, io_err_short_buffer);
 *     return 0;
 *
 * p is written twice by the macro, so hand it a pointer variable rather than a
 * call. Both arguments are evaluated at most once for any pointer expression
 * without a side effect, which covers every out parameter in the library. */
#define BURROW_OUT(p, v)                                                               \
    do {                                                                               \
        if ((p) != NULL)                                                               \
            *(p) = (v);                                                                \
    } while (0)

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
 * sizeof a pointer, which is a bug the compiler cannot see.
 *
 * It is a compound literal, so it cannot initialise anything with static
 * storage. C11 wants a constant expression there and a compound literal is not
 * one. Write the braces out by hand for a static, the way
 * BURROW_SENTINEL_ERROR does. */
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
 * Out of range panics, carrying the message Go's panic carries, and the value
 * is a RuntimeError so a catch block can tell it from a panic somebody wrote by
 * hand. See burrow/runtime.h.
 *
 * When you are walking a string you already bounds checked, index s.p directly
 * and let the loop condition be the check. That is what the library does
 * internally and it is not cheating. */
Byte str_at(Str s, Int i);

/* The iterator behind the rune loop below. Keep one on the stack.
 *
 * The fields are here because C has no other way to let you declare one, and
 * not because they are yours to read. */
typedef struct StrIter {
    Str s;
    Int i;
} StrIter;

/* for i, r := range s.
 *
 *     Int i;
 *     Rune r;
 *     for (StrIter it = str_runes(s); str_next_rune(&it, &i, &r); )
 *         printf("%lld: %lx\n", (long long)i, (unsigned long)r);
 *
 * Either pointer may be NULL if you only want the other one, and the index is
 * the byte offset the rune started at rather than a count of runes, which is
 * what Go's loop gives you and what you need to slice with.
 *
 * Go's range over a string decodes UTF-8, and so does this, including what
 * happens to bytes that are not valid UTF-8: you get U+FFFD and the loop
 * advances one byte. That means a loop over a corrupt string still terminates.
 * See burrow/utf8.h for the whole story and for the functions underneath this.
 *
 * Walking the bytes instead is s.p[i] with your own loop, or str_at for the
 * bounds checked version, and for ASCII data that is what you want.
 *
 * Both are inline, which is not a detail. Go's range loop is generated by the
 * compiler with no call in it, and the first version of this was a real call
 * per rune. Summing the runes of a line of accented text measured 270
 * nanoseconds that way against Go's 117 for the same loop, and 80 once it was
 * inline. Almost none of that was the decoding. It was the iterator: a call the
 * compiler cannot see into has to leave StrIter in memory, so every iteration
 * stored the offset and loaded it back before it could do anything else, and
 * the loop ran at the speed of that round trip. Inline, the iterator stays in
 * registers and the loop is the decode and nothing else.
 *
 * So the ASCII path is here in the header, where it can be inlined into your
 * loop, and anything wider goes out to a real call. That split is the same one
 * Go's compiler makes, and it is the right one: a byte below 0x80 is its own
 * encoding, which is the majority of the characters in almost any input,
 * including text that is not English. */
Rune burrow__str_next_rune_slow(Str s, Int i, Int *size);

static inline StrIter str_runes(Str s) {
    StrIter it;
    it.s = s;
    it.i = 0;
    return it;
}

static inline bool str_next_rune(StrIter *it, Int *index, Rune *r) {
    Int start = it->i;
    Byte b;

    if (start >= it->s.len)
        return false;

    b = it->s.p[start];
    if (b < 0x80) {
        it->i = start + 1;
        BURROW_OUT(index, start);
        BURROW_OUT(r, (Rune)b);
        return true;
    }

    {
        Int size;
        Rune got = burrow__str_next_rune_slow(it->s, start, &size);
        it->i = start + size;
        BURROW_OUT(index, start);
        BURROW_OUT(r, got);
        return true;
    }
}

#if defined(BURROW_SHORT) && BURROW_SHORT
#define S(lit) BURROW_S(lit)
#define STR_FMT BURROW_STR_FMT
#define STR_ARG(s) BURROW_STR_ARG(s)
#endif

#ifdef __cplusplus
}
#endif

#endif /* BURROW_CORE_H */

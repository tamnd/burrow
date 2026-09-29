/* math/big: what the files of the package share.
 *
 * The layers are Go's. The word vector loops at the bottom (arith.go), the
 * natural numbers over them (nat.go and the files next to it), and Int, Rat
 * and Float on top. The functions keep Go's names with nat_ or big_ in front,
 * so nat_div is (nat).div and big_add_vv is addVV, and they keep Go's calling
 * shape: a nat function takes z by value, maybe reuses its memory, and returns
 * the result, exactly as Go's z = z.op(x, y) does.
 *
 * Memory. Go gets its temporaries from the garbage collector and forgets them.
 * Here every word vector the nat layer makes comes from a per thread scratch
 * arena, and every public function brackets its work with big_enter and
 * big_leave. On the way out the result is copied into memory the receiver's
 * allocator owns, unless it is already there, and the scratch arena goes back
 * to empty. Go's stack of temporaries (the stk argument) is a second arena on
 * the same thread with marks in the same places Go saves and restores.
 *
 * The rule that makes this sound: nothing in the package calls a public
 * function. Internal code calls the bi_ functions, which work on the same
 * structs and never commit or reset.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_MATH_BIG_INTERNAL_H
#define BURROW_MATH_BIG_INTERNAL_H

#include "burrow/math/big.h"
#include "burrow/math/bits.h"
#include "burrow/mem/arena.h"

#include <string.h>

typedef burrow__BigNat Nat;

#define BIG_W ((Uint)BITS_UINT_SIZE) /* word size in bits */
#define BIG_S (BITS_UINT_SIZE / 8)   /* word size in bytes */
#define BIG_M (~(BigWord)0)          /* digit mask */

/* ---------------------------------------------------------------- scratch */

/* Where the nat layer's memory comes from. It panics when the heap refuses,
 * which is what Go does. */
BigWord *big_alloc(Int n);

/* stk.nat: n words from the stack arena, with no room past them. */
Nat big_stk_nat(Int n);

/* stk.save and stk.restore. */
ArenaMark big_stk_save(void);
void big_stk_restore(ArenaMark m);

/* The bracket around every public function, and the step that moves a result
 * into the receiver's own memory. orig is what the receiver held on the way in,
 * and a result anywhere else is copied into it, grown from a when it is too
 * small. */
void big_enter(void);
void big_leave(void);
void big_commit(Nat *abs, Alloc *a, Nat orig);

/* Makes sure the receiver's own buffer can hold n words, so that the nat code
 * after it finds the room already there and writes the result in place. Keeps
 * what the buffer held. */
void big_reserve(Nat *abs, Alloc *a, Int n);

BURROW_NORETURN void big_oom(void);

/* The scratch arena as an allocator, for the odd object that is not words,
 * such as the random source Miller-Rabin draws its bases from. */
Alloc *big_scratch_alloc(void);

/* ------------------------------------------------------------- word slices */

static inline BigWord *big_at(BigWord *p, Int i) {
    return p == NULL ? p : p + i;
}

/* x[i:j], keeping the capacity past j the way a Go slice does, which is what
 * alias compares. */
static inline Nat nat_slice(Nat x, Int i, Int j) {
    Nat r = {big_at(x.p, i), j - i, x.cap - i};
    return r;
}

/* x[i:], and x[:n]. */
static inline Nat nat_from(Nat x, Int i) {
    return nat_slice(x, i, x.len);
}

static inline Nat nat_to(Nat x, Int n) {
    Nat r = {x.p, n, x.cap};
    return r;
}

static inline Nat nat_view(const BigWord *p, Int n) {
    Nat r = {(BigWord *)(uintptr_t)p, n, n};
    return r;
}

/* A Nat with a length has memory behind it, which the analyser cannot see
 * through every caller, so nat_clear and nat_norm say so. */
static inline void nat_clear(Nat z) {
    if (z.len > 0)
        /* NOLINTNEXTLINE(clang-analyzer-core.NonNullParamChecker) */
        memset(z.p, 0, (size_t)z.len * sizeof(BigWord));
}

static inline void nat_copy(Nat z, Nat x) {
    Int n = z.len < x.len ? z.len : x.len;
    if (n > 0 && z.p != x.p)
        memmove(z.p, x.p, (size_t)n * sizeof(BigWord));
}

static inline Nat nat_norm(Nat z) {
    Int i = z.len;
    /* NOLINTNEXTLINE(clang-analyzer-core.NullDereference) */
    while (i > 0 && z.p[i - 1] == 0)
        i--;
    z.len = i;
    return z;
}

static inline bool nat_same(Nat x, Nat y) {
    return x.len == y.len && x.len > 0 && x.p == y.p;
}

/* alias: whether x and y share their last word of capacity. */
static inline bool nat_alias(Nat x, Nat y) {
    return x.cap > 0 && y.cap > 0 && x.p + x.cap == y.p + y.cap;
}

extern const BigWord big_nat_one_w[1];
extern const BigWord big_nat_two_w[1];
extern const BigWord big_nat_five_w[1];
extern const BigWord big_nat_ten_w[1];
#define NAT_ONE nat_view(big_nat_one_w, 1)
#define NAT_TWO nat_view(big_nat_two_w, 1)
#define NAT_FIVE nat_view(big_nat_five_w, 1)
#define NAT_TEN nat_view(big_nat_ten_w, 1)
#define NAT_NIL ((Nat){NULL, 0, 0})

/* ----------------------------------------------------------- word arithmetic */

static inline BigWord big_mul_ww(BigWord x, BigWord y, BigWord *lo) {
    /* Through a local, because bits_mul tests lo for NULL, and passing it
     * &z.p[i] straight in lets gcc conclude z.p may be NULL. */
    BigWord l;
    BigWord h = bits_mul(x, y, &l);
    *lo = l;
    return h;
}

static inline Uint big_nlz(BigWord x) {
    return (Uint)bits_leading_zeros(x);
}

BigWord big_add_vv(Nat z, Nat x, Nat y);
BigWord big_sub_vv(Nat z, Nat x, Nat y);
BigWord big_add_vw(Nat z, Nat x, BigWord y);
BigWord big_sub_vw(Nat z, Nat x, BigWord y);
BigWord big_lsh_vu(Nat z, Nat x, Uint s);
BigWord big_rsh_vu(Nat z, Nat x, Uint s);
BigWord big_mul_add_vww(Nat z, Nat x, BigWord y, BigWord r);
BigWord big_add_mul_vvww(Nat z, Nat x, Nat y, BigWord m, BigWord a);
BigWord big_div_ww(BigWord x1, BigWord x0, BigWord y, BigWord m, BigWord *r);
BigWord big_reciprocal_word(BigWord d1);

/* ------------------------------------------------------------------- nats */

Nat nat_make(Nat z, Int n);
Nat nat_set_word(Nat z, BigWord x);
Nat nat_set_uint64(Nat z, uint64_t x);
Nat nat_set(Nat z, Nat x);
Nat nat_add(Nat z, Nat x, Nat y);
Nat nat_sub(Nat z, Nat x, Nat y);
int nat_cmp(Nat x, Nat y);
Nat nat_montgomery(Nat z, Nat x, Nat y, Nat m, BigWord k, Int n);
void nat_add_to(Nat z, Nat x);
Nat nat_mul_range(Nat z, uint64_t a, uint64_t b);
Int nat_bit_len(Nat x);
Uint nat_trailing_zero_bits(Nat x);
bool nat_is_pow2(Nat x, Uint *n);
Nat nat_lsh(Nat z, Nat x, Uint s);
Nat nat_rsh(Nat z, Nat x, Uint s);
Nat nat_set_bit(Nat z, Nat x, Uint i, Uint b);
Uint nat_bit(Nat x, Uint i);
Uint nat_sticky(Nat x, Uint i);
Nat nat_and(Nat z, Nat x, Nat y);
Nat nat_trunc(Nat z, Nat x, Uint n);
Nat nat_and_not(Nat z, Nat x, Nat y);
Nat nat_or(Nat z, Nat x, Nat y);
Nat nat_xor(Nat z, Nat x, Nat y);
Nat nat_random(Nat z, MathRandRand *rnd, Nat limit, Int n);
Nat nat_exp_nn(Nat z, Nat x, Nat y, Nat m, bool slow);
Int nat_bytes(Nat z, Slice buf);
Nat nat_set_bytes(Nat z, Slice buf);
Nat nat_sqrt(Nat z, Nat x);
Nat nat_sub_mod_2n(Nat z, Nat x, Nat y, Uint n);
Nat nat_mod_inverse(Nat z, Nat g, Nat n);

/* natmul.go */
Nat nat_mul(Nat z, Nat x, Nat y);
Nat nat_sqr(Nat z, Nat x);
Nat nat_mul_add_ww(Nat z, Nat x, BigWord y, BigWord r);

/* natdiv.go. nat_div returns the quotient and puts the remainder in *r; z2
 * is where the remainder may go. */
Nat nat_div(Nat z, Nat z2, Nat u, Nat v, Nat *r);
Nat nat_rem(Nat z, Nat u, Nat v);
Nat nat_div_w(Nat z, Nat x, BigWord y, BigWord *r);
BigWord nat_mod_w(Nat x, BigWord d);

/* natconv.go. A byte source for scan: a string, or a fmt.ScanState. */
typedef struct BigScanner {
    const Byte *p;
    Int len;
    Int pos;
    FmtScanState *st; /* when not NULL, the bytes come from here */
} BigScanner;

void big_scanner_init(BigScanner *r, Str s);
void big_scanner_init_state(BigScanner *r, FmtScanState *st);
/* ReadByte and UnreadByte. io_eof at the end. */
Error big_scanner_read(BigScanner *r, Byte *c);
void big_scanner_unread(BigScanner *r);

extern const Error big_err_no_digits;
extern const Error big_err_inval_sep;

/* scan: the digits of a number in base (0 for a prefix), with a fraction if
 * frac_ok. *res_base is the base found, *res_count the digits, or the
 * negative of the digits after the point when there is one. */
Nat nat_scan(Nat z, BigScanner *r, int base, bool frac_ok, int *res_base,
             Int *res_count, Error *res_err);

/* itoa: the digits, in scratch memory, with '-' in front when neg. */
Str nat_itoa(Nat x, bool neg, int base);
Str nat_utoa(Nat x, int base);
Nat nat_exp_ww(Nat z, BigWord x, BigWord y);

/* ------------------------------------------------------------------- ints */

/* The Int methods as Go writes them, with the memory rules above: z's words
 * may end up in scratch memory, and only the public wrapper puts them back. */
void bi_set_int64(BigInt *z, int64_t x);
void bi_set_uint64(BigInt *z, uint64_t x);
void bi_set(BigInt *z, const BigInt *x);
void bi_abs(BigInt *z, const BigInt *x);
void bi_neg(BigInt *z, const BigInt *x);
void bi_add(BigInt *z, const BigInt *x, const BigInt *y);
void bi_sub(BigInt *z, const BigInt *x, const BigInt *y);
void bi_mul(BigInt *z, const BigInt *x, const BigInt *y);
void bi_quo(BigInt *z, const BigInt *x, const BigInt *y);
void bi_rem(BigInt *z, const BigInt *x, const BigInt *y);
void bi_quo_rem(BigInt *z, const BigInt *x, const BigInt *y, BigInt *r);
void bi_div(BigInt *z, const BigInt *x, const BigInt *y);
void bi_mod(BigInt *z, const BigInt *x, const BigInt *y);
int bi_cmp(const BigInt *x, const BigInt *y);
void bi_lsh(BigInt *z, const BigInt *x, Uint n);
void bi_rsh(BigInt *z, const BigInt *x, Uint n);
bool bi_exp(BigInt *z, const BigInt *x, const BigInt *y, const BigInt *m, bool slow);
void bi_gcd(BigInt *z, BigInt *x, BigInt *y, const BigInt *a, const BigInt *b);
bool bi_mod_inverse(BigInt *z, const BigInt *g, const BigInt *n);
int bi_jacobi(const BigInt *x, const BigInt *y);
void bi_set_bit(BigInt *z, const BigInt *x, Int i, Uint b);
void bi_scan(BigInt *z, BigScanner *r, int base, int *b, Error *err);
Error bi_scan_sign(BigScanner *r, bool *neg);
double bi_float64(const BigInt *x, BigAccuracy *acc);

/* setFromScanner: z from all of r, false when r holds anything more than a
 * number. */
bool bi_set_from_scanner(BigInt *z, BigScanner *r, int base);

/* Rat's norm, which Float.Rat needs as well. */
void br_norm(BigRat *z);

/* append(buf, s...) for a byte slice that may still be the zero Slice. */
Slice big_append_str(Alloc *a, Slice buf, Str s);

/* writeMultiple: text count times, into s. */
void big_write_multiple(FmtState s, Str text, Int count);

/* scanExponent: an exponent, e or E for a decimal one and p or P for a binary
 * one when base2ok, with separators when sep_ok. No exponent is 0 in base 10.
 * *base is 10 or 2. */
Error big_scan_exponent(BigScanner *r, bool base2ok, bool sep_ok, int64_t *exp,
                        int *base);

#endif /* BURROW_MATH_BIG_INTERNAL_H */

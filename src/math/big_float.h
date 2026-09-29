/* math/big: what big_float.c and big_ftoa.c share.
 *
 * The bf_ functions are Go's Float methods without the commit around them, so
 * they follow the same rule as the bi_ ones: internal code may call them, and
 * they never call a public function.
 *
 * Copyright 2014 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_MATH_BIG_FLOAT_H
#define BURROW_MATH_BIG_FLOAT_H

#include "big_internal.h"

/* Go's form: the order matters, zero < finite < inf. */
#define BF_ZERO 0
#define BF_FINITE 1
#define BF_INF 2

BURROW_NORETURN void bf_nan(const BigErrNaN *e);

void bf_set_prec(BigFloat *z, Uint prec);
Uint bf_min_prec(const BigFloat *x);
Int bf_sign(const BigFloat *x);
int64_t bf_mant_exp(const BigFloat *x, BigFloat *mant);
void bf_set_mant_exp(BigFloat *z, const BigFloat *mant, int64_t exp);
void bf_set_exp_and_round(BigFloat *z, int64_t exp, Uint sbit);
bool bf_is_int(const BigFloat *x);
Str bf_validate0(const BigFloat *x);
void bf_round(BigFloat *z, Uint sbit);
int64_t bf_fnorm(Nat m);

void bf_set_uint64(BigFloat *z, uint64_t x);
void bf_set_int64(BigFloat *z, int64_t x);
void bf_set_float64(BigFloat *z, double x);
void bf_set_int(BigFloat *z, const BigInt *x);
void bf_set_inf(BigFloat *z, bool signbit);
void bf_set(BigFloat *z, const BigFloat *x);
void bf_copy(BigFloat *z, const BigFloat *x);
double bf_float64(const BigFloat *x, BigAccuracy *acc);

void bf_neg(BigFloat *z, const BigFloat *x);
void bf_add(BigFloat *z, const BigFloat *x, const BigFloat *y);
void bf_sub(BigFloat *z, const BigFloat *x, const BigFloat *y);
void bf_mul(BigFloat *z, const BigFloat *x, const BigFloat *y);
void bf_quo(BigFloat *z, const BigFloat *x, const BigFloat *y);

/* big_ftoa.c. Text in scratch memory, and Append onto buf in a. */
Str bf_text(const BigFloat *x, Alloc *a, Byte format, Int prec);
Slice bf_append(const BigFloat *x, Alloc *a, Slice buf, Byte fmt, Int prec);

#endif

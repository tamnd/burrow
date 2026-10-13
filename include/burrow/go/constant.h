/* go/constant, exact values for Go constants.
 *
 * Go's go/constant. A ConstantValue is the exact value of an untyped Go
 * constant: a boolean, a string, an integer of any size, a fraction or float
 * of very high precision, or a complex number made of two of those. Values
 * come from literals or from the constructors, and the operations make new
 * ones, the way a compiler folds constant expressions:
 *
 *     ConstantValue x = constant_make_from_literal(a, BURROW_S("1e100"), TOKEN_FLOAT, 0);
 *     ConstantValue y = constant_make_int64(3);
 *     ConstantValue q = constant_binary_op(a, x, TOKEN_QUO, y);
 *     Str s = constant_value_string(q, a);   // "3.33333e+99"
 *
 * Memory. A ConstantValue is a small struct passed by value, and {0} is the
 * unknown value. Anything bigger than an int64 lives in the allocator given to
 * the function that made it, and values share that memory freely, since none
 * of them ever changes. Nothing frees a single value, so make them in an arena
 * and free the arena when you are done with all of them.
 *
 * Values can be read from any number of threads at once, as in Go, as long as
 * the allocator they were made in is thread safe. A string built with + is
 * only joined up the first time its bytes are needed, and that allocates.
 *
 * Copyright 2013 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package go/constant */

#ifndef BURROW_GO_CONSTANT_H
#define BURROW_GO_CONSTANT_H

#include "burrow/core.h"
#include "burrow/go/token.h"
#include "burrow/iface.h"
#include "burrow/mem.h"
#include "burrow/own.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* constant.Kind: what sort of value a ConstantValue holds. */
typedef Int ConstantKind;

/* unknown values */
#define CONSTANT_UNKNOWN ((ConstantKind)0)
/* non-numeric values */
#define CONSTANT_BOOL ((ConstantKind)1)
#define CONSTANT_STRING ((ConstantKind)2)
/* numeric values */
#define CONSTANT_INT ((ConstantKind)3)
#define CONSTANT_FLOAT ((ConstantKind)4)
#define CONSTANT_COMPLEX ((ConstantKind)5)

/* Kind.String: "Unknown", "Bool" and so on, and "Kind(n)" for a value that is
 * none of them. */
BURROW_OWNS(ret) Str constant_kind_string(ConstantKind i, Alloc *a);

/* constant.Value. Read it through the functions below and leave the fields
 * alone: rep says how the value is held, which is finer than its kind, and u
 * holds it. */
typedef struct ConstantValue {
    uint8_t rep;
    union {
        bool b;
        int64_t i;
        const void *p;
    } u;
} ConstantValue;

/* Value.Kind: the kind of x. */
ConstantKind constant_value_kind(ConstantValue x);

/* Value.String: a short, quoted, human readable form of x. A string is cut
 * to 72 runes with "..." on the end, and a number that is not an integer is
 * rounded to 6 digits. */
BURROW_OWNS(ret) Str constant_value_string(ConstantValue x, Alloc *a);

/* Value.ExactString: x in full, as a fraction for a Float that is one and in
 * hexadecimal mantissa and binary exponent form for one that is not. */
BURROW_OWNS(ret) Str constant_value_exact_string(ConstantValue x, Alloc *a);

/* The type descriptor for Value, with its String method, so that a value can
 * go to fmt as BURROW_ANY(TYPE_CONSTANT_VALUE, &v). */
extern const Type burrow_type_ConstantValue;
extern const Type *const TYPE_CONSTANT_VALUE;

/* ------------------------------------------------------------ constructors */

/* MakeUnknown, MakeBool and MakeInt64. None of them allocates. */
ConstantValue constant_make_unknown(void);
ConstantValue constant_make_bool(bool b);
ConstantValue constant_make_int64(int64_t x);

/* MakeString: the String value for s, which is copied into a. */
ConstantValue constant_make_string(Alloc *a, Str s);

/* MakeUint64: the Int value for x. */
ConstantValue constant_make_uint64(Alloc *a, uint64_t x);

/* MakeFloat64: the Float value for x, with -0 made +0. An infinity or a NaN
 * gives the unknown value. */
ConstantValue constant_make_float64(Alloc *a, double x);

/* MakeFromLiteral: the value of a Go literal of kind tok, which is one of
 * TOKEN_INT, TOKEN_FLOAT, TOKEN_IMAG, TOKEN_CHAR and TOKEN_STRING. A literal
 * that is not valid gives the unknown value. Any other token panics, and so
 * does a zero other than 0, which is only there to match Go. */
ConstantValue constant_make_from_literal(Alloc *a, Str lit, Token tok, Uint zero);

/* Make: the value for x, which is a bool, Str, int64_t, BigInt, BigRat or
 * BigFloat, by its type descriptor. Anything else gives the unknown value.
 * Where Go keeps the *big.Int it is given, this copies it into a, so x may
 * change or go away afterwards. */
ConstantValue constant_make(Alloc *a, Any x);

/* MakeImag: the Complex value x*i for an Int or Float x. Unknown stays
 * unknown, and anything else panics. */
ConstantValue constant_make_imag(Alloc *a, ConstantValue x);

/* MakeFromBytes: the Int value of the little-endian unsigned bytes. */
ConstantValue constant_make_from_bytes(Alloc *a, Slice bytes);

/* --------------------------------------------------------------- accessors */

/* BoolVal: the Go bool of x, which must be a Bool or unknown, and false for
 * unknown. Anything else panics. */
bool constant_bool_val(ConstantValue x);

/* StringVal: the Go string of x, which must be a String or unknown, and ""
 * for unknown. The bytes belong to x. */
BURROW_BORROWS(ret, x) Str constant_string_val(ConstantValue x);

/* Int64Val and Uint64Val: x as a Go int64 or uint64, and in *exact whether
 * that is x itself. x must be an Int or unknown. exact may be NULL. */
int64_t constant_int64_val(ConstantValue x, bool *exact);
uint64_t constant_uint64_val(ConstantValue x, bool *exact);

/* Float32Val and Float64Val: the nearest float32 or float64 to x, and in
 * *exact whether that is x itself. x must be an Int, a Float or unknown, and
 * a value too big for the type is an infinity. exact may be NULL. */
float constant_float32_val(ConstantValue x, bool *exact);
double constant_float64_val(ConstantValue x, bool *exact);

/* Val: x as a Go value, which is a bool, Str or int64_t, or the BigInt,
 * BigRat or BigFloat x holds, by its type descriptor. Unknown and Complex
 * values give a nil Any. The big ones are x's own, and are read only. */
BURROW_OWNS(ret) Any constant_val(Alloc *a, ConstantValue x);

/* StringLen: the length in bytes of a String x, and 0 for unknown. It is
 * worked out without building the string. */
int64_t constant_string_len(ConstantValue x);

/* BitLen: how many bits it takes to hold the absolute value of an Int x. */
Int constant_bit_len(ConstantValue x);

/* Sign: -1, 0 or +1 as x is negative, zero or positive, for a numeric x. A
 * Complex value is 0 only when both its parts are. Unknown gives 1. */
Int constant_sign(ConstantValue x);

/* Bytes: the absolute value of an Int x as little-endian bytes, with no
 * zero bytes at the most significant end. */
BURROW_OWNS(ret) Slice constant_bytes(Alloc *a, ConstantValue x);

/* Num and Denom: the numerator and denominator of an Int or Float x in
 * lowest terms, where the denominator is always positive. A Float too big or
 * too small to hold as a fraction gives the unknown value. */
ConstantValue constant_num(Alloc *a, ConstantValue x);
ConstantValue constant_denom(Alloc *a, ConstantValue x);

/* Real and Imag: the real and imaginary parts of a numeric x. A value that is
 * not Complex is its own real part, with an imaginary part of 0. */
ConstantValue constant_real(ConstantValue x);
ConstantValue constant_imag(ConstantValue x);

/* ToInt, ToFloat and ToComplex: x as an Int, Float or Complex value when it
 * can be one, and the unknown value when it cannot. ToInt allows a Float that
 * is a few bits away from an integer, the way the compiler does. */
ConstantValue constant_to_int(Alloc *a, ConstantValue x);
ConstantValue constant_to_float(Alloc *a, ConstantValue x);
ConstantValue constant_to_complex(Alloc *a, ConstantValue x);

/* -------------------------------------------------------------- operations */

/* UnaryOp: op y, for TOKEN_ADD, TOKEN_SUB, TOKEN_XOR and TOKEN_NOT. A prec
 * above 0 makes ^ work on an unsigned value of that many bits. A bad operand
 * or operator panics. */
ConstantValue constant_unary_op(Alloc *a, Token op, ConstantValue y, Uint prec);

/* BinaryOp: x op y, for the arithmetic, bitwise and logical operators. An
 * Int and a Float give a Float, and so on up to Complex. TOKEN_QUO on two
 * Ints gives the exact quotient, and TOKEN_QUO_ASSIGN the truncated integer
 * one. An unknown operand gives unknown. A bad operand or operator panics,
 * and so does dividing by an integer zero. */
ConstantValue constant_binary_op(Alloc *a, ConstantValue x, Token op, ConstantValue y);

/* Shift: x << s or x >> s for an Int x and op TOKEN_SHL or TOKEN_SHR. */
ConstantValue constant_shift(Alloc *a, ConstantValue x, Token op, Uint s);

/* Compare: x op y for the comparison operators. Unknown compares false with
 * everything, itself included. */
bool constant_compare(ConstantValue x, Token op, ConstantValue y);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_GO_CONSTANT_H */

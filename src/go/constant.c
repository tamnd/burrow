/* go/constant: exact values for Go constants.
 *
 * Go's Value is an interface with eight implementations. Here it is a tag and
 * a union: rep is the implementation, and u holds a bool, an int64 or a
 * pointer to the rest. Every value is immutable once made, which is what lets
 * values share their numbers and string pieces without copying them.
 *
 * Strings are Go's lazy concatenation trees. A + of two strings makes a node
 * pointing at both, and the first time the bytes are wanted the node is
 * flattened in place under its mutex, with Go's hand over hand locking. The
 * empty string is a NULL node.
 *
 * Derived from Go's src/go/constant/value.go and kind_string.go.
 * Go source: go1.27.1.
 *
 * Copyright 2013 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/go/constant.h"

#include "burrow/core.h"
#include "burrow/declare.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/go/token.h"
#include "burrow/iface.h"
#include "burrow/math.h"
#include "burrow/math/big.h"
#include "burrow/math/bits.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/num.h"
#include "burrow/panic.h"
#include "burrow/runtime.h"
#include "burrow/slice.h"
#include "burrow/strconv.h"
#include "burrow/strings.h"
#include "burrow/sync.h"
#include "burrow/type.h"
#include "burrow/utf8.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* The implementations of Go's Value, in the order ord ranks them. */
enum {
    CV_UNKNOWN,
    CV_BOOL,
    CV_STRING,  /* u.p is a CvString, NULL for "" */
    CV_INT64,   /* an Int that fits in an int64 */
    CV_INT,     /* u.p is a BigInt that does not */
    CV_RAT,     /* u.p is a BigRat, a Float that is a small enough fraction */
    CV_FLOAT,   /* u.p is a BigFloat, a Float that is not */
    CV_COMPLEX, /* u.p is a CvComplex */
};

/* Maximum mantissa precision of a Float value, in bits. */
#define CV_PREC 512

/* The largest exponent, in bits, that a numerator, denominator or float may
 * have and still be held as a fraction. */
#define CV_MAX_EXP (4 << 10)

typedef struct CvString {
    SyncMutex mu;
    Str s;
    struct CvString *l, *r; /* both NULL once flattened, or for a leaf */
    Alloc *a;               /* where the flattened bytes go */
} CvString;

typedef struct CvComplex {
    ConstantValue re, im;
} CvComplex;

/* ----------------------------------------------------------------- helpers */

static void *cv_alloc(Alloc *a, size_t size, size_t align) {
    void *p = mem_alloc(a, size, align);
    if (p == NULL)
        panic_str(BURROW_S("go/constant: out of memory"));
    return p;
}

static ConstantValue cv_unknown(void) {
    ConstantValue v = {CV_UNKNOWN, {.i = 0}};
    return v;
}

static ConstantValue cv_bool(bool b) {
    ConstantValue v = {CV_BOOL, {.b = b}};
    return v;
}

static ConstantValue cv_int64(int64_t x) {
    ConstantValue v = {CV_INT64, {.i = x}};
    return v;
}

static ConstantValue cv_ptr(uint8_t rep, const void *p) {
    ConstantValue v = {rep, {.p = p}};
    return v;
}

static const BigInt *cv_int_of(ConstantValue x) {
    return (const BigInt *)x.u.p;
}

static const BigRat *cv_rat_of(ConstantValue x) {
    return (const BigRat *)x.u.p;
}

static const BigFloat *cv_float_of(ConstantValue x) {
    return (const BigFloat *)x.u.p;
}

static const CvComplex *cv_complex_of(ConstantValue x) {
    return (const CvComplex *)x.u.p;
}

static CvString *cv_string_of(ConstantValue x) {
    return (CvString *)(uintptr_t)x.u.p;
}

static BigInt *cv_new_int(Alloc *a) {
    return big_new_int(a, 0);
}

static BigRat *cv_new_rat(Alloc *a) {
    return big_new_rat(a, 0, 1);
}

/* An empty Float struct in a, with no precision yet. */
static BigFloat *cv_alloc_float(Alloc *a) {
    BigFloat *z = cv_alloc(a, sizeof(BigFloat), _Alignof(BigFloat));
    *z = BIG_FLOAT(a);
    return z;
}

static BigFloat *cv_new_float(Alloc *a) {
    return big_float_set_prec(cv_alloc_float(a), CV_PREC);
}

/* Go's panic(fmt.Sprintf("%v not ...", x)), with the text in the
 * goroutine's error arena. */
BURROW_NORETURN static void cv_panic_not(ConstantValue x, const char *what) {
    Alloc *ea = error_allocator();
    panic_str(fmt_sprintf_v(ea, "%s %s", constant_value_string(x, ea), what));
}

/* The name in Go of the dynamic type behind rep, for a failed type
 * assertion. */
static const char *cv_go_type(uint8_t rep) {
    static const char *const names[] = {
        "constant.unknownVal", "constant.boolVal",    "*constant.stringVal",
        "constant.int64Val",   "constant.intVal",     "constant.ratVal",
        "constant.floatVal",   "constant.complexVal",
    };
    return names[rep];
}

/* Go's y.(T) where y is not a T, which only happens for a bool and a string,
 * the two values that ord ranks the same. */
BURROW_NORETURN static void cv_assert_failed(ConstantValue y, uint8_t want) {
    runtime_panic(fmt_sprintf_v(error_allocator(),
                                "interface conversion: constant.Value is %s, not %s",
                                cv_go_type(y.rep), cv_go_type(want)));
}

/* ------------------------------------------------------------------ strings */

static ConstantValue cv_make_string(Alloc *a, Str s) {
    CvString *n = cv_alloc(a, sizeof(CvString), _Alignof(CvString));
    n->s = str_clone(a, s);
    n->a = a;
    return cv_ptr(CV_STRING, n);
}

static int64_t cv_len_locked(CvString *x);

/* Go's stringVal.len. */
static int64_t cv_len(CvString *x) {
    sync_mutex_lock(&x->mu);
    int64_t n = cv_len_locked(x);
    sync_mutex_unlock(&x->mu);
    return n;
}

/* The length of x, which the caller has locked. Go recurses down both sides,
 * which a goroutine's growable stack takes in its stride. This walks the left
 * spine in a loop, with the same hand over hand locking as appendReverse, so
 * a long chain of + made one string at a time does not recurse at all. */
static int64_t cv_len_locked(CvString *x) {
    int64_t n = 0;
    CvString *y = x;
    while (y->r != NULL) {
        n += cv_len(y->r);
        CvString *l = y->l;
        if (y != x)
            sync_mutex_unlock(&y->mu);
        sync_mutex_lock(&l->mu);
        y = l;
    }
    n += y->s.len;
    if (y != x)
        sync_mutex_unlock(&y->mu);
    return n;
}

/* Go's appendReverse, writing each piece just before *end instead of
 * appending it to a list that is then reversed, since the pieces come out
 * last first. x is locked by the caller. */
static void cv_fill_reverse(CvString *x, Byte *buf, int64_t *end) {
    CvString *y = x;
    while (y->r != NULL) {
        sync_mutex_lock(&y->r->mu);
        cv_fill_reverse(y->r, buf, end);
        sync_mutex_unlock(&y->r->mu);

        CvString *l = y->l;
        if (y != x)
            sync_mutex_unlock(&y->mu);
        sync_mutex_lock(&l->mu);
        y = l;
    }
    Str s = y->s;
    if (y != x)
        sync_mutex_unlock(&y->mu);
    *end -= s.len;
    if (s.len > 0)
        memcpy(buf + *end, s.p, (size_t)s.len);
}

/* Go's stringVal.string: the bytes of x, flattening it the first time. */
static Str cv_string(ConstantValue x) {
    CvString *n = cv_string_of(x);
    if (n == NULL)
        return BURROW_STR_EMPTY;
    sync_mutex_lock(&n->mu);
    if (n->l != NULL) {
        int64_t len = cv_len_locked(n);
        Byte *buf = cv_alloc(n->a, len > 0 ? (size_t)len : 1, 1);
        int64_t end = len;
        cv_fill_reverse(n, buf, &end);
        n->s = str_from_bytes(buf, (Int)len);
        n->l = NULL;
        n->r = NULL;
    }
    Str s = n->s;
    sync_mutex_unlock(&n->mu);
    return s;
}

static ConstantValue cv_concat(Alloc *a, ConstantValue x, ConstantValue y) {
    if (x.u.p == NULL)
        return y;
    if (y.u.p == NULL)
        return x;
    CvString *n = cv_alloc(a, sizeof(CvString), _Alignof(CvString));
    n->l = cv_string_of(x);
    n->r = cv_string_of(y);
    n->a = a;
    return cv_ptr(CV_STRING, n);
}

/* ------------------------------------------------------------------- kinds */

Str constant_kind_string(ConstantKind i, Alloc *a) {
    static const char names[] = "UnknownBoolStringIntFloatComplex";
    static const uint8_t index[] = {0, 7, 11, 17, 20, 25, 32};
    if (i < 0 || i >= (ConstantKind)(sizeof index - 1))
        return fmt_sprintf_v(a, "Kind(%d)", (int64_t)i);
    return str_from_bytes(names + index[i], index[i + 1] - index[i]);
}

ConstantKind constant_value_kind(ConstantValue x) {
    static const ConstantKind kinds[] = {
        CONSTANT_UNKNOWN, CONSTANT_BOOL,  CONSTANT_STRING, CONSTANT_INT,
        CONSTANT_INT,     CONSTANT_FLOAT, CONSTANT_FLOAT,  CONSTANT_COMPLEX,
    };
    return kinds[x.rep];
}

/* --------------------------------------------------------------- printing */

/* floatVal.String. Use exact fmt formatting if in float64 range (common
 * case): proceed if f doesn't underflow to 0 or overflow to inf. */
static Str cv_float_string(const BigFloat *f, Alloc *a) {
    /* Don't try to convert infinities (will not terminate). */
    if (big_float_is_inf(f))
        return big_float_string(f, a);

    BigAccuracy acc = BIG_EXACT;
    double x = big_float_float64(f, &acc);
    if ((big_float_sign(f) == 0) == (x == 0) && !math_is_inf(x, 0)) {
        Str s = fmt_sprintf_v(a, "%.6g", x);
        if (!big_float_is_int(f) && strings_index_byte(s, '.') < 0) {
            /* f is not an integer, but its string representation doesn't
             * reflect that. Use more digits. See issue #56220. */
            s = fmt_sprintf_v(a, "%g", x);
        }
        return s;
    }

    /* Out of float64 range. Do approximate manual to decimal conversion to
     * avoid precise but possibly slow Float formatting. f = mant * 2**exp */
    BigFloat mant = BIG_FLOAT(a);
    Int exp = big_float_mant_exp(f, &mant); /* 0.5 <= |mant| < 1.0 */

    /* approximate float64 mantissa m and decimal exponent d
     * f ~ m * 10**d */
    double m = big_float_float64(&mant, &acc); /* 0.5 <= |m| < 1.0 */
    /* log_10(2), the value Go's math.Ln2 / math.Ln10 has as a constant. */
    double d = (double)exp * 0.30102999566398119521;

    /* adjust m for truncated (integer) decimal exponent e */
    int64_t e = (int64_t)d;
    m *= math_pow(10, d - (double)e);

    /* ensure 1 <= |m| < 10 */
    double am = math_abs(m);
    if (am < 1 - 0.5e-6) {
        /* The %.6g format below rounds m to 5 digits after the decimal
         * point. Make sure that m*10 < 10 even after rounding up:
         * m*10 + 0.5e-5 < 10 => m < 1 - 0.5e6. */
        m *= 10;
        e--;
    } else if (am >= 10) {
        m /= 10;
        e++;
    }
    return fmt_sprintf_v(a, "%.6ge%+d", m, e);
}

/* stringVal.String: the quoted string, cut short at maxLen runes. */
static Str cv_quoted_short(Str s, Alloc *a) {
    enum { MAX_LEN = 72 }; /* a reasonable length */
    Str q = strconv_quote(a, s);
    if (utf8_rune_count_in_string(q) <= MAX_LEN)
        return q;
    /* The string without the enclosing quotes is greater than maxLen-2
     * runes long. Remove the last 3 runes (including the closing '"') by
     * keeping only the first maxLen-3 runes, then add "...". */
    Int i = 0;
    for (Int n = 0; n < MAX_LEN - 3; n++) {
        Int size = 0;
        utf8_decode_rune_in_string(str_from_bytes(q.p + i, q.len - i), &size);
        i += size;
    }
    Byte *b = cv_alloc(a, (size_t)i + 3, 1);
    memcpy(b, q.p, (size_t)i);
    memcpy(b + i, "...", 3);
    return str_from_bytes(b, i + 3);
}

Str constant_value_string(ConstantValue x, Alloc *a) {
    switch (x.rep) {
    case CV_BOOL:
        return strconv_format_bool(x.u.b);
    case CV_STRING:
        return cv_quoted_short(cv_string(x), a);
    case CV_INT64:
        return strconv_format_int(a, x.u.i, 10);
    case CV_INT:
        return big_int_string(cv_int_of(x), a);
    case CV_RAT: {
        BigFloat *f = big_float_set_rat(cv_new_float(a), cv_rat_of(x));
        return cv_float_string(f, a);
    }
    case CV_FLOAT:
        return cv_float_string(cv_float_of(x), a);
    case CV_COMPLEX: {
        const CvComplex *c = cv_complex_of(x);
        return fmt_sprintf_v(a, "(%s + %si)", constant_value_string(c->re, a),
                             constant_value_string(c->im, a));
    }
    default:
        return BURROW_S("unknown");
    }
}

Str constant_value_exact_string(ConstantValue x, Alloc *a) {
    switch (x.rep) {
    case CV_STRING:
        return strconv_quote(a, cv_string(x));
    case CV_RAT:
        /* RatString is the numerator alone for an integer, as Go's
         * ExactString writes it. */
        return big_rat_rat_string(cv_rat_of(x), a);
    case CV_FLOAT:
        return big_float_text(cv_float_of(x), a, 'p', 0);
    case CV_COMPLEX: {
        const CvComplex *c = cv_complex_of(x);
        return fmt_sprintf_v(a, "(%s + %si)", constant_value_exact_string(c->re, a),
                             constant_value_exact_string(c->im, a));
    }
    default:
        return constant_value_string(x, a);
    }
}

static Str cv_m_string(ConstantValue *self) {
    return constant_value_string(*self, error_allocator());
}

#define CV_SIG_STRING(IN, OUT) OUT(Str)

#define CV_METHODS(M, T) M(T, String, cv_m_string, CV_SIG_STRING)

BURROW_METHODS_DEFINE(ConstantValue, CV_METHODS);

const Type burrow_type_ConstantValue = {
    {(const Byte *)"Value", 5},
    {(const Byte *)"go/constant", 11},
    KIND_STRUCT,
    (uint32_t)sizeof(ConstantValue),
    (uint16_t)_Alignof(ConstantValue),
    0,
    (uint16_t)(sizeof burrow__methods_ConstantValue /
               sizeof burrow__methods_ConstantValue[0]),
    NULL,
    burrow__methods_ConstantValue,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

const Type *const TYPE_CONSTANT_VALUE = &burrow_type_ConstantValue;

/* ------------------------------------------------------------- conversions */

static ConstantValue cv_make_int(const BigInt *x) {
    if (big_int_is_int64(x))
        return cv_int64(big_int_int64(x));
    return cv_ptr(CV_INT, x);
}

static bool cv_small_int(const BigInt *x) {
    return big_int_bit_len(x) < CV_MAX_EXP;
}

static ConstantValue cv_make_rat(Alloc *a, BigRat *x) {
    /* A whole number has a denominator of 1, or none at all yet, and
     * Denom would store the 1. */
    if (cv_small_int(big_rat_num(x)) &&
        (big_rat_is_int(x) || cv_small_int(big_rat_denom(x))))
        return cv_ptr(CV_RAT, x);
    /* components too large => switch to float */
    return cv_ptr(CV_FLOAT, big_float_set_rat(cv_new_float(a), x));
}

static ConstantValue cv_make_float(Alloc *a, const BigFloat *x) {
    /* convert -0 to 0 */
    if (big_float_sign(x) == 0)
        return cv_ptr(CV_FLOAT, cv_new_float(a));
    if (big_float_is_inf(x))
        return cv_unknown();
    /* No attempt is made to "normalize" the float, unlike rat and int
     * values, which happens to make no difference to anything here. */
    return cv_ptr(CV_FLOAT, x);
}

static ConstantValue cv_make_complex(Alloc *a, ConstantValue re, ConstantValue im) {
    if (re.rep == CV_UNKNOWN || im.rep == CV_UNKNOWN)
        return cv_unknown();
    CvComplex *c = cv_alloc(a, sizeof(CvComplex), _Alignof(CvComplex));
    c->re = re;
    c->im = im;
    return cv_ptr(CV_COMPLEX, c);
}

static bool cv_small_float64(double x) {
    if (math_is_inf(x, 0))
        return false;
    Int e = 0;
    math_frexp(x, &e);
    return -CV_MAX_EXP < e && e < CV_MAX_EXP;
}

static bool cv_small_float(const BigFloat *x) {
    if (big_float_is_inf(x))
        return false;
    Int e = big_float_mant_exp(x, NULL);
    return -CV_MAX_EXP < e && e < CV_MAX_EXP;
}

/* makeFloatFromLiteral. false where Go returns nil. */
static bool cv_float_from_literal(Alloc *a, Str lit, ConstantValue *out) {
    BigFloat *f = cv_new_float(a);
    bool ok = false;
    big_float_set_string(f, lit, &ok);
    if (!ok)
        return false;
    if (cv_small_float(f)) {
        /* ok to use rationals */
        if (big_float_sign(f) == 0) {
            /* Issue 20228: If the float underflowed to zero, parse just
             * "0". Otherwise, lit might contain a value with a large
             * negative exponent, such as -6e-1886451601. As a float,
             * that will underflow to 0, but it'll take forever to parse
             * as a Rat. */
            lit = BURROW_S("0");
        }
        BigRat *r = cv_new_rat(a);
        big_rat_set_string(r, lit, &ok);
        if (ok) {
            *out = cv_ptr(CV_RAT, r);
            return true;
        }
    }
    /* otherwise use floats */
    *out = cv_make_float(a, f);
    return true;
}

static ConstantValue cv_i64toi(Alloc *a, int64_t x) {
    return cv_ptr(CV_INT, big_new_int(a, x));
}

static ConstantValue cv_i64tor(Alloc *a, int64_t x) {
    return cv_ptr(CV_RAT, big_new_rat(a, x, 1));
}

static ConstantValue cv_i64tof(Alloc *a, int64_t x) {
    return cv_ptr(CV_FLOAT, big_float_set_int64(cv_new_float(a), x));
}

static ConstantValue cv_itor(Alloc *a, ConstantValue x) {
    return cv_ptr(CV_RAT, big_rat_set_int(cv_new_rat(a), cv_int_of(x)));
}

static ConstantValue cv_itof(Alloc *a, ConstantValue x) {
    return cv_ptr(CV_FLOAT, big_float_set_int(cv_new_float(a), cv_int_of(x)));
}

static ConstantValue cv_rtof(Alloc *a, ConstantValue x) {
    return cv_ptr(CV_FLOAT, big_float_set_rat(cv_new_float(a), cv_rat_of(x)));
}

static ConstantValue cv_vtoc(Alloc *a, ConstantValue x) {
    CvComplex *c = cv_alloc(a, sizeof(CvComplex), _Alignof(CvComplex));
    c->re = x;
    c->im = cv_int64(0);
    return cv_ptr(CV_COMPLEX, c);
}

static bool cv_is_numeric(ConstantValue x) {
    return x.rep == CV_INT64 || x.rep == CV_INT || x.rep == CV_RAT || x.rep == CV_FLOAT;
}

/* ------------------------------------------------------------ constructors */

ConstantValue constant_make_unknown(void) {
    return cv_unknown();
}

ConstantValue constant_make_bool(bool b) {
    return cv_bool(b);
}

ConstantValue constant_make_string(Alloc *a, Str s) {
    if (s.len == 0)
        return cv_ptr(CV_STRING, NULL); /* common case */
    return cv_make_string(a, s);
}

ConstantValue constant_make_int64(int64_t x) {
    return cv_int64(x);
}

ConstantValue constant_make_uint64(Alloc *a, uint64_t x) {
    if (x < (uint64_t)1 << 63)
        return cv_int64((int64_t)x);
    return cv_ptr(CV_INT, big_int_set_uint64(cv_new_int(a), x));
}

ConstantValue constant_make_float64(Alloc *a, double x) {
    if (math_is_inf(x, 0) || math_is_nan(x))
        return cv_unknown();
    /* convert -0 to 0, which Go does with x + 0 */
    if (x == 0)
        x = 0;
    if (cv_small_float64(x))
        return cv_ptr(CV_RAT, big_rat_set_float64(cv_new_rat(a), x));
    return cv_ptr(CV_FLOAT, big_float_set_float64(cv_new_float(a), x));
}

ConstantValue constant_make_from_literal(Alloc *a, Str lit, Token tok, Uint zero) {
    if (zero != 0)
        panic_str(BURROW_S("MakeFromLiteral called with non-zero last argument"));

    switch (tok) {
    case TOKEN_INT: {
        Error err = BURROW_NO_ERROR;
        int64_t x = strconv_parse_int(lit, 0, 64, &err);
        if (BURROW_OK(err))
            return cv_int64(x);
        bool ok = false;
        BigInt *z = big_int_set_string(cv_new_int(a), lit, 0, &ok);
        if (ok)
            return cv_ptr(CV_INT, z);
        break;
    }
    case TOKEN_FLOAT: {
        ConstantValue x;
        if (cv_float_from_literal(a, lit, &x))
            return x;
        break;
    }
    case TOKEN_IMAG: {
        Int n = lit.len;
        ConstantValue im;
        if (n > 0 && lit.p[n - 1] == 'i' &&
            cv_float_from_literal(a, str_from_bytes(lit.p, n - 1), &im))
            return cv_make_complex(a, cv_int64(0), im);
        break;
    }
    case TOKEN_CHAR: {
        Int n = lit.len;
        if (n >= 2) {
            Error err = BURROW_NO_ERROR;
            bool multibyte = false;
            Str tail = BURROW_STR_EMPTY;
            Rune code = strconv_unquote_char(str_from_bytes(lit.p + 1, n - 2), '\'',
                                             &multibyte, &tail, &err);
            if (BURROW_OK(err))
                return cv_int64((int64_t)code);
        }
        break;
    }
    case TOKEN_STRING: {
        Error err = BURROW_NO_ERROR;
        Str s = strconv_unquote(a, lit, &err);
        if (BURROW_OK(err))
            return constant_make_string(a, s);
        break;
    }
    default: {
        Alloc *ea = error_allocator();
        panic_str(fmt_sprintf_v(ea, "%s is not a valid token", token_string(tok, ea)));
    }
    }
    return cv_unknown();
}

ConstantValue constant_make(Alloc *a, Any x) {
    if (x.t == TYPE_BOOL)
        return cv_bool(*(const bool *)x.data);
    if (x.t == TYPE_STRING)
        return cv_make_string(a, *(const Str *)x.data);
    if (x.t == TYPE_INT64)
        return cv_int64(*(const int64_t *)x.data);
    if (x.t == TYPE_BIG_INT)
        return cv_make_int(big_int_set(cv_new_int(a), (const BigInt *)x.data));
    if (x.t == TYPE_BIG_RAT)
        return cv_make_rat(a, big_rat_set(cv_new_rat(a), (const BigRat *)x.data));
    if (x.t == TYPE_BIG_FLOAT)
        return cv_make_float(
            a, big_float_copy(cv_alloc_float(a), (const BigFloat *)x.data));
    return cv_unknown();
}

ConstantValue constant_make_imag(Alloc *a, ConstantValue x) {
    if (x.rep == CV_UNKNOWN)
        return x;
    if (cv_is_numeric(x))
        return cv_make_complex(a, cv_int64(0), x);
    cv_panic_not(x, "not Int or Float");
}

ConstantValue constant_make_from_bytes(Alloc *a, Slice bytes) {
    enum { WORD_SIZE = sizeof(BigWord) };
    Int n = (bytes.len + (WORD_SIZE - 1)) / WORD_SIZE;
    BigWord *words =
        cv_alloc(a, (size_t)(n > 0 ? n : 1) * WORD_SIZE, _Alignof(BigWord));

    const Byte *p = bytes.p;
    Int i = 0;
    BigWord w = 0;
    unsigned s = 0;
    for (Int k = 0; k < bytes.len; k++) {
        w |= (BigWord)p[k] << s;
        s += 8;
        if (s == WORD_SIZE * 8) {
            words[i] = w;
            i++;
            w = 0;
            s = 0;
        }
    }
    /* store last word */
    if (i < n) {
        words[i] = w;
        i++;
    }
    /* normalize */
    while (i > 0 && words[i - 1] == 0)
        i--;

    Slice abs = {.p = words, .len = i, .cap = n, .elem = TYPE_UINT};
    return cv_make_int(big_int_set_bits(cv_new_int(a), abs));
}

/* --------------------------------------------------------------- accessors */

bool constant_bool_val(ConstantValue x) {
    switch (x.rep) {
    case CV_BOOL:
        return x.u.b;
    case CV_UNKNOWN:
        return false;
    default:
        cv_panic_not(x, "not a Bool");
    }
}

Str constant_string_val(ConstantValue x) {
    switch (x.rep) {
    case CV_STRING:
        return cv_string(x);
    case CV_UNKNOWN:
        return BURROW_STR_EMPTY;
    default:
        cv_panic_not(x, "not a String");
    }
}

int64_t constant_int64_val(ConstantValue x, bool *exact) {
    bool e = false;
    int64_t v = 0;
    switch (x.rep) {
    case CV_INT64:
        v = x.u.i;
        e = true;
        break;
    case CV_INT:
        v = big_int_int64(cv_int_of(x)); /* not an int64Val and thus not exact */
        break;
    case CV_UNKNOWN:
        break;
    default:
        cv_panic_not(x, "not an Int");
    }
    if (exact != NULL)
        *exact = e;
    return v;
}

uint64_t constant_uint64_val(ConstantValue x, bool *exact) {
    bool e = false;
    uint64_t v = 0;
    switch (x.rep) {
    case CV_INT64:
        v = (uint64_t)x.u.i;
        e = x.u.i >= 0;
        break;
    case CV_INT:
        v = big_int_uint64(cv_int_of(x));
        e = big_int_is_uint64(cv_int_of(x));
        break;
    case CV_UNKNOWN:
        break;
    default:
        cv_panic_not(x, "not an Int");
    }
    if (exact != NULL)
        *exact = e;
    return v;
}

/* Go's int64Val(f) == x for a float that came from x. A float of 2**63,
 * which an int64 near the top rounds up to, is out of range, and Go's
 * conversion of it on amd64 gives the most negative int64, which is not x. */
static bool cv_float_is_int64(double f, int64_t x) {
    return f < 9223372036854775808.0 && (int64_t)f == x;
}

float constant_float32_val(ConstantValue x, bool *exact) {
    bool e = false;
    float v = 0;
    BigAccuracy acc = BIG_EXACT;
    switch (x.rep) {
    case CV_INT64:
        v = (float)x.u.i;
        e = cv_float_is_int64((double)v, x.u.i);
        break;
    case CV_INT: {
        BigFloat t = BIG_FLOAT(NULL);
        big_float_set_int(big_float_set_prec(&t, CV_PREC), cv_int_of(x));
        v = big_float_float32(&t, &acc);
        e = acc == BIG_EXACT;
        big_float_free(&t);
        break;
    }
    case CV_RAT:
        v = big_rat_float32(cv_rat_of(x), &e);
        break;
    case CV_FLOAT:
        v = big_float_float32(cv_float_of(x), &acc);
        e = acc == BIG_EXACT;
        break;
    case CV_UNKNOWN:
        break;
    default:
        cv_panic_not(x, "not a Float");
    }
    if (exact != NULL)
        *exact = e;
    return v;
}

double constant_float64_val(ConstantValue x, bool *exact) {
    bool e = false;
    double v = 0;
    BigAccuracy acc = BIG_EXACT;
    switch (x.rep) {
    case CV_INT64:
        v = (double)x.u.i;
        e = cv_float_is_int64(v, x.u.i);
        break;
    case CV_INT: {
        BigFloat t = BIG_FLOAT(NULL);
        big_float_set_int(big_float_set_prec(&t, CV_PREC), cv_int_of(x));
        v = big_float_float64(&t, &acc);
        e = acc == BIG_EXACT;
        big_float_free(&t);
        break;
    }
    case CV_RAT:
        v = big_rat_float64(cv_rat_of(x), &e);
        break;
    case CV_FLOAT:
        v = big_float_float64(cv_float_of(x), &acc);
        e = acc == BIG_EXACT;
        break;
    case CV_UNKNOWN:
        break;
    default:
        cv_panic_not(x, "not a Float");
    }
    if (exact != NULL)
        *exact = e;
    return v;
}

Any constant_val(Alloc *a, ConstantValue x) {
    switch (x.rep) {
    case CV_BOOL:
        return any_box(a, BURROW_ANY_VAL(TYPE_BOOL, bool, x.u.b));
    case CV_STRING:
        return any_box(a, BURROW_ANY_VAL(TYPE_STRING, Str, cv_string(x)));
    case CV_INT64:
        return any_box(a, BURROW_ANY_VAL(TYPE_INT64, int64_t, x.u.i));
    case CV_INT:
        return BURROW_ANY(TYPE_BIG_INT, (void *)(uintptr_t)x.u.p);
    case CV_RAT:
        return BURROW_ANY(TYPE_BIG_RAT, (void *)(uintptr_t)x.u.p);
    case CV_FLOAT:
        return BURROW_ANY(TYPE_BIG_FLOAT, (void *)(uintptr_t)x.u.p);
    default: {
        Any nil = {NULL, NULL};
        return nil;
    }
    }
}

int64_t constant_string_len(ConstantValue x) {
    switch (x.rep) {
    case CV_STRING:
        return x.u.p == NULL ? 0 : cv_len(cv_string_of(x));
    case CV_UNKNOWN:
        return 0;
    default:
        cv_panic_not(x, "not a String");
    }
}

Int constant_bit_len(ConstantValue x) {
    switch (x.rep) {
    case CV_INT64: {
        uint64_t u = (uint64_t)x.u.i;
        if (x.u.i < 0)
            u = (uint64_t)0 - u;
        return 64 - bits_leading_zeros64(u);
    }
    case CV_INT:
        return big_int_bit_len(cv_int_of(x));
    case CV_UNKNOWN:
        return 0;
    default:
        cv_panic_not(x, "not an Int");
    }
}

Int constant_sign(ConstantValue x) {
    switch (x.rep) {
    case CV_INT64:
        if (x.u.i < 0)
            return -1;
        if (x.u.i > 0)
            return 1;
        return 0;
    case CV_INT:
        return big_int_sign(cv_int_of(x));
    case CV_RAT:
        return big_rat_sign(cv_rat_of(x));
    case CV_FLOAT:
        return big_float_sign(cv_float_of(x));
    case CV_COMPLEX:
        return constant_sign(cv_complex_of(x)->re) |
               constant_sign(cv_complex_of(x)->im);
    case CV_UNKNOWN:
        return 1; /* avoid spurious division by zero errors */
    default:
        cv_panic_not(x, "not numeric");
    }
}

Slice constant_bytes(Alloc *a, ConstantValue x) {
    enum { WORD_SIZE = sizeof(BigWord) };
    const BigInt *t = NULL;
    switch (x.rep) {
    case CV_INT64:
        t = big_new_int(a, x.u.i);
        break;
    case CV_INT:
        t = cv_int_of(x);
        break;
    default:
        cv_panic_not(x, "not an Int");
    }

    Slice words = big_int_bits(t);
    Int n = words.len * WORD_SIZE;
    Slice out = slice_make(a, TYPE_BYTE, n, n);
    Byte *bytes = out.p;
    const BigWord *ws = words.p;
    Int i = 0;
    for (Int k = 0; k < words.len; k++) {
        BigWord w = ws[k];
        for (Int j = 0; j < WORD_SIZE; j++) {
            bytes[i] = (Byte)w;
            w >>= 8;
            i++;
        }
    }
    /* remove leading 0's */
    while (i > 0 && bytes[i - 1] == 0)
        i--;
    out.len = i;
    return out;
}

/* The numerator of a Rat value, which the value owns and nothing writes. */
static const BigInt *cv_rat_num(const BigRat *r) {
    return big_rat_num((BigRat *)(uintptr_t)r);
}

/* The denominator of a Rat value. Go's Denom stores a 1 in a Rat that has no
 * denominator yet, so a whole number gets its 1 here instead, and only a Rat
 * whose denominator is already there is asked for it. */
static ConstantValue cv_rat_denom(const BigRat *r) {
    if (big_rat_is_int(r))
        return cv_int64(1);
    return cv_make_int(big_rat_denom((BigRat *)(uintptr_t)r));
}

ConstantValue constant_num(Alloc *a, ConstantValue x) {
    switch (x.rep) {
    case CV_INT64:
    case CV_INT:
        return x;
    case CV_RAT:
        return cv_make_int(cv_rat_num(cv_rat_of(x)));
    case CV_FLOAT:
        if (cv_small_float(cv_float_of(x))) {
            BigAccuracy acc = BIG_EXACT;
            BigRat *r = big_float_rat(cv_float_of(x), cv_new_rat(a), &acc);
            return cv_make_int(cv_rat_num(r));
        }
        break;
    case CV_UNKNOWN:
        break;
    default:
        cv_panic_not(x, "not Int or Float");
    }
    return cv_unknown();
}

ConstantValue constant_denom(Alloc *a, ConstantValue x) {
    switch (x.rep) {
    case CV_INT64:
    case CV_INT:
        return cv_int64(1);
    case CV_RAT:
        return cv_rat_denom(cv_rat_of(x));
    case CV_FLOAT:
        if (cv_small_float(cv_float_of(x))) {
            BigAccuracy acc = BIG_EXACT;
            BigRat *r = big_float_rat(cv_float_of(x), cv_new_rat(a), &acc);
            return cv_rat_denom(r);
        }
        break;
    case CV_UNKNOWN:
        break;
    default:
        cv_panic_not(x, "not Int or Float");
    }
    return cv_unknown();
}

ConstantValue constant_real(ConstantValue x) {
    if (x.rep == CV_UNKNOWN || cv_is_numeric(x))
        return x;
    if (x.rep == CV_COMPLEX)
        return cv_complex_of(x)->re;
    cv_panic_not(x, "not numeric");
}

ConstantValue constant_imag(ConstantValue x) {
    if (x.rep == CV_UNKNOWN)
        return x;
    if (cv_is_numeric(x))
        return cv_int64(0);
    if (x.rep == CV_COMPLEX)
        return cv_complex_of(x)->im;
    cv_panic_not(x, "not numeric");
}

ConstantValue constant_to_int(Alloc *a, ConstantValue x) {
    switch (x.rep) {
    case CV_INT64:
    case CV_INT:
        return x;

    case CV_RAT:
        if (big_rat_is_int(cv_rat_of(x)))
            return cv_make_int(cv_rat_num(cv_rat_of(x)));
        break;

    case CV_FLOAT: {
        /* avoid creation of huge integers
         * (Existing tests require permitting exponents of at least 1024;
         * allow any value that would also be permissible as a fraction.) */
        const BigFloat *f = cv_float_of(x);
        if (!cv_small_float(f))
            break;
        BigInt *i = cv_new_int(a);
        BigAccuracy acc = BIG_EXACT;
        big_float_int(f, i, &acc);
        if (acc == BIG_EXACT)
            return cv_make_int(i);

        /* If we can get an integer by rounding up or down, assume x is not
         * an integer, but just a value that is not quite exact. Try to
         * round toward zero and away from it, in a few bits less precision
         * than x holds. */
        enum { DELTA = 4 }; /* a small number of bits > 0 */
        BigFloat t = BIG_FLOAT(a);
        big_float_set_prec(&t, CV_PREC - DELTA);

        /* try rounding down a little */
        big_float_set_mode(&t, BIG_TO_ZERO);
        big_float_set(&t, f);
        big_float_int(&t, i, &acc);
        if (acc == BIG_EXACT)
            return cv_make_int(i);

        /* try rounding up a little */
        big_float_set_mode(&t, BIG_AWAY_FROM_ZERO);
        big_float_set(&t, f);
        big_float_int(&t, i, &acc);
        if (acc == BIG_EXACT)
            return cv_make_int(i);
        break;
    }

    case CV_COMPLEX: {
        ConstantValue re = constant_to_float(a, x);
        if (re.rep == CV_RAT || re.rep == CV_FLOAT)
            return constant_to_int(a, re);
        break;
    }

    default:
        break;
    }
    return cv_unknown();
}

ConstantValue constant_to_float(Alloc *a, ConstantValue x) {
    switch (x.rep) {
    case CV_INT64:
        return cv_i64tor(a, x.u.i); /* x is always a small int */
    case CV_INT:
        if (cv_small_int(cv_int_of(x)))
            return cv_itor(a, x);
        return cv_itof(a, x);
    case CV_RAT:
    case CV_FLOAT:
        return x;
    case CV_COMPLEX:
        if (constant_sign(cv_complex_of(x)->im) == 0)
            return constant_to_float(a, cv_complex_of(x)->re);
        break;
    default:
        break;
    }
    return cv_unknown();
}

ConstantValue constant_to_complex(Alloc *a, ConstantValue x) {
    if (cv_is_numeric(x))
        return cv_vtoc(a, x);
    if (x.rep == CV_COMPLEX)
        return x;
    return cv_unknown();
}

/* -------------------------------------------------------------- operations */

static bool cv_is32bit(int64_t x) {
    return INT64_C(-2147483648) <= x && x <= INT64_C(2147483647);
}

static bool cv_is63bit(int64_t x) {
    return -(INT64_C(1) << 62) <= x && x <= (INT64_C(1) << 62) - 1;
}

ConstantValue constant_unary_op(Alloc *a, Token op, ConstantValue y, Uint prec) {
    switch (op) {
    case TOKEN_ADD:
        if (y.rep != CV_BOOL && y.rep != CV_STRING)
            return y;
        break;

    case TOKEN_SUB:
        switch (y.rep) {
        case CV_UNKNOWN:
            return y;
        case CV_INT64:
            if (y.u.i != 0 && y.u.i != INT64_MIN)
                return cv_int64(-y.u.i); /* no overflow */
            return cv_make_int(big_int_neg(cv_new_int(a), big_new_int(a, y.u.i)));
        case CV_INT:
            return cv_make_int(big_int_neg(cv_new_int(a), cv_int_of(y)));
        case CV_RAT:
            return cv_make_rat(a, big_rat_neg(cv_new_rat(a), cv_rat_of(y)));
        case CV_FLOAT:
            return cv_make_float(a, big_float_neg(cv_new_float(a), cv_float_of(y)));
        case CV_COMPLEX: {
            ConstantValue re = constant_unary_op(a, TOKEN_SUB, cv_complex_of(y)->re, 0);
            ConstantValue im = constant_unary_op(a, TOKEN_SUB, cv_complex_of(y)->im, 0);
            return cv_make_complex(a, re, im);
        }
        default:
            break;
        }
        break;

    case TOKEN_XOR: {
        BigInt *z = cv_new_int(a);
        switch (y.rep) {
        case CV_UNKNOWN:
            return y;
        case CV_INT64:
            big_int_not(z, big_new_int(a, y.u.i));
            break;
        case CV_INT:
            big_int_not(z, cv_int_of(y));
            break;
        default:
            goto error;
        }
        /* For unsigned types, the result will be negative and thus "too
         * large": We must limit the result precision to the type's
         * precision. */
        if (prec > 0)
            big_int_and_not(z, z, big_int_lsh(cv_new_int(a), big_new_int(a, -1), prec));
        return cv_make_int(z);
    }

    case TOKEN_NOT:
        if (y.rep == CV_UNKNOWN)
            return y;
        if (y.rep == CV_BOOL)
            return cv_bool(!y.u.b);
        break;

    default:
        break;
    }

error: {
    Alloc *ea = error_allocator();
    panic_str(fmt_sprintf_v(ea, "invalid unary operation %s%s", token_string(op, ea),
                            constant_value_string(y, ea)));
}
}

static int cv_ord(ConstantValue x) {
    static const int8_t ords[] = {0, 1, 1, 2, 3, 4, 5, 6};
    return ords[x.rep];
}

/* match0 must only be called by match. Invariant: ord(x) < ord(y). On
 * success x is converted to y's representation, and on failure y becomes x,
 * as Go's x, x return does. */
static void cv_match0(Alloc *a, ConstantValue *x, ConstantValue *y) {
    /* Prefer to return the original x and y arguments when possible, to
     * avoid unnecessary heap allocations. */
    switch (y->rep) {
    case CV_INT:
        if (x->rep == CV_INT64) {
            *x = cv_i64toi(a, x->u.i);
            return;
        }
        break;
    case CV_RAT:
        if (x->rep == CV_INT64) {
            *x = cv_i64tor(a, x->u.i);
            return;
        }
        if (x->rep == CV_INT) {
            *x = cv_itor(a, *x);
            return;
        }
        break;
    case CV_FLOAT:
        if (x->rep == CV_INT64) {
            *x = cv_i64tof(a, x->u.i);
            return;
        }
        if (x->rep == CV_INT) {
            *x = cv_itof(a, *x);
            return;
        }
        if (x->rep == CV_RAT) {
            *x = cv_rtof(a, *x);
            return;
        }
        break;
    case CV_COMPLEX:
        if (cv_is_numeric(*x)) {
            *x = cv_vtoc(a, *x);
            return;
        }
        break;
    default:
        break;
    }
    /* force unknown and invalid values into "x position" in callers of
     * match (don't care about y since we don't use it) */
    *y = *x;
}

/* match returns the matching representation (same type) with the smallest
 * complexity for two values x and y. If one of them is numeric, both of them
 * must be numeric. If one of them is unknown or invalid (say, nil) both
 * results are that value. */
static void cv_match(Alloc *a, ConstantValue *x, ConstantValue *y) {
    int ox = cv_ord(*x), oy = cv_ord(*y);
    if (ox < oy)
        cv_match0(a, x, y);
    else if (ox > oy)
        cv_match0(a, y, x);
}

BURROW_NORETURN static void cv_binary_failed(ConstantValue x, Token op,
                                             ConstantValue y) {
    Alloc *ea = error_allocator();
    panic_str(fmt_sprintf_v(ea, "invalid binary operation %s %s %s",
                            constant_value_string(x, ea), token_string(op, ea),
                            constant_value_string(y, ea)));
}

static ConstantValue cv_binary_int64(Alloc *a, int64_t x, Token op, int64_t y,
                                     bool *ok) {
    int64_t c = 0;
    switch (op) {
    case TOKEN_ADD:
        if (!cv_is63bit(x) || !cv_is63bit(y))
            return cv_make_int(
                big_int_add(cv_new_int(a), big_new_int(a, x), big_new_int(a, y)));
        c = x + y;
        break;
    case TOKEN_SUB:
        if (!cv_is63bit(x) || !cv_is63bit(y))
            return cv_make_int(
                big_int_sub(cv_new_int(a), big_new_int(a, x), big_new_int(a, y)));
        c = x - y;
        break;
    case TOKEN_MUL:
        if (!cv_is32bit(x) || !cv_is32bit(y))
            return cv_make_int(
                big_int_mul(cv_new_int(a), big_new_int(a, x), big_new_int(a, y)));
        c = x * y;
        break;
    case TOKEN_QUO:
        return cv_make_rat(a, big_new_rat(a, x, y));
    case TOKEN_QUO_ASSIGN: /* force integer division */
        c = int64_div(x, y);
        break;
    case TOKEN_REM:
        c = int64_mod(x, y);
        break;
    case TOKEN_AND:
        c = x & y;
        break;
    case TOKEN_OR:
        c = x | y;
        break;
    case TOKEN_XOR:
        c = x ^ y;
        break;
    case TOKEN_AND_NOT:
        c = x & ~y;
        break;
    default:
        *ok = false;
        return cv_unknown();
    }
    return cv_int64(c);
}

static ConstantValue cv_binary_int(Alloc *a, const BigInt *x, Token op, const BigInt *y,
                                   bool *ok) {
    BigInt *c = cv_new_int(a);
    switch (op) {
    case TOKEN_ADD:
        big_int_add(c, x, y);
        break;
    case TOKEN_SUB:
        big_int_sub(c, x, y);
        break;
    case TOKEN_MUL:
        big_int_mul(c, x, y);
        break;
    case TOKEN_QUO:
        return cv_make_rat(a, big_rat_set_frac(cv_new_rat(a), x, y));
    case TOKEN_QUO_ASSIGN: /* force integer division */
        big_int_quo(c, x, y);
        break;
    case TOKEN_REM:
        big_int_rem(c, x, y);
        break;
    case TOKEN_AND:
        big_int_and(c, x, y);
        break;
    case TOKEN_OR:
        big_int_or(c, x, y);
        break;
    case TOKEN_XOR:
        big_int_xor(c, x, y);
        break;
    case TOKEN_AND_NOT:
        big_int_and_not(c, x, y);
        break;
    default:
        *ok = false;
        return cv_unknown();
    }
    return cv_make_int(c);
}

static ConstantValue cv_binary_complex(Alloc *a, const CvComplex *x, Token op,
                                       const CvComplex *y, bool *ok) {
    ConstantValue re, im;
    ConstantValue p = x->re, q = x->im; /* Go's a and b */
    ConstantValue r = y->re, s = y->im; /* Go's c and d */
    switch (op) {
    case TOKEN_ADD:
        /* (a+c) + i(b+d) */
        re = constant_binary_op(a, p, TOKEN_ADD, r);
        im = constant_binary_op(a, q, TOKEN_ADD, s);
        break;
    case TOKEN_SUB:
        /* (a-c) + i(b-d) */
        re = constant_binary_op(a, p, TOKEN_SUB, r);
        im = constant_binary_op(a, q, TOKEN_SUB, s);
        break;
    case TOKEN_MUL: {
        /* (ac-bd) + i(bc+ad) */
        ConstantValue ac = constant_binary_op(a, p, TOKEN_MUL, r);
        ConstantValue bd = constant_binary_op(a, q, TOKEN_MUL, s);
        ConstantValue bc = constant_binary_op(a, q, TOKEN_MUL, r);
        ConstantValue ad = constant_binary_op(a, p, TOKEN_MUL, s);
        re = constant_binary_op(a, ac, TOKEN_SUB, bd);
        im = constant_binary_op(a, bc, TOKEN_ADD, ad);
        break;
    }
    case TOKEN_QUO: {
        /* (ac+bd)/s + i(bc-ad)/s, with s = cc + dd */
        ConstantValue ac = constant_binary_op(a, p, TOKEN_MUL, r);
        ConstantValue bd = constant_binary_op(a, q, TOKEN_MUL, s);
        ConstantValue bc = constant_binary_op(a, q, TOKEN_MUL, r);
        ConstantValue ad = constant_binary_op(a, p, TOKEN_MUL, s);
        ConstantValue cc = constant_binary_op(a, r, TOKEN_MUL, r);
        ConstantValue dd = constant_binary_op(a, s, TOKEN_MUL, s);
        ConstantValue ss = constant_binary_op(a, cc, TOKEN_ADD, dd);
        re = constant_binary_op(a, ac, TOKEN_ADD, bd);
        re = constant_binary_op(a, re, TOKEN_QUO, ss);
        im = constant_binary_op(a, bc, TOKEN_SUB, ad);
        im = constant_binary_op(a, im, TOKEN_QUO, ss);
        break;
    }
    default:
        *ok = false;
        return cv_unknown();
    }
    return cv_make_complex(a, re, im);
}

ConstantValue constant_binary_op(Alloc *a, ConstantValue x_, Token op,
                                 ConstantValue y_) {
    ConstantValue x = x_, y = y_;
    cv_match(a, &x, &y);
    bool ok = true;
    ConstantValue z;

    switch (x.rep) {
    case CV_UNKNOWN:
        return x;

    case CV_BOOL:
        if (y.rep != CV_BOOL)
            cv_assert_failed(y, CV_BOOL);
        if (op == TOKEN_LAND)
            return cv_bool(x.u.b && y.u.b);
        if (op == TOKEN_LOR)
            return cv_bool(x.u.b || y.u.b);
        break;

    case CV_INT64:
        z = cv_binary_int64(a, x.u.i, op, y.u.i, &ok);
        if (ok)
            return z;
        break;

    case CV_INT:
        z = cv_binary_int(a, cv_int_of(x), op, cv_int_of(y), &ok);
        if (ok)
            return z;
        break;

    case CV_RAT: {
        BigRat *c = cv_new_rat(a);
        switch (op) {
        case TOKEN_ADD:
            big_rat_add(c, cv_rat_of(x), cv_rat_of(y));
            break;
        case TOKEN_SUB:
            big_rat_sub(c, cv_rat_of(x), cv_rat_of(y));
            break;
        case TOKEN_MUL:
            big_rat_mul(c, cv_rat_of(x), cv_rat_of(y));
            break;
        case TOKEN_QUO:
            big_rat_quo(c, cv_rat_of(x), cv_rat_of(y));
            break;
        default:
            ok = false;
            break;
        }
        if (ok)
            return cv_make_rat(a, c);
        break;
    }

    case CV_FLOAT: {
        BigFloat *c = cv_new_float(a);
        switch (op) {
        case TOKEN_ADD:
            big_float_add(c, cv_float_of(x), cv_float_of(y));
            break;
        case TOKEN_SUB:
            big_float_sub(c, cv_float_of(x), cv_float_of(y));
            break;
        case TOKEN_MUL:
            big_float_mul(c, cv_float_of(x), cv_float_of(y));
            break;
        case TOKEN_QUO:
            big_float_quo(c, cv_float_of(x), cv_float_of(y));
            break;
        default:
            ok = false;
            break;
        }
        if (ok)
            return cv_make_float(a, c);
        break;
    }

    case CV_COMPLEX:
        z = cv_binary_complex(a, cv_complex_of(x), op, cv_complex_of(y), &ok);
        if (ok)
            return z;
        break;

    case CV_STRING:
        if (op == TOKEN_ADD) {
            if (y.rep != CV_STRING)
                cv_assert_failed(y, CV_STRING);
            return cv_concat(a, x, y);
        }
        break;

    default:
        break;
    }

    cv_binary_failed(x_, op, y_);
}

ConstantValue constant_shift(Alloc *a, ConstantValue x, Token op, Uint s) {
    switch (x.rep) {
    case CV_UNKNOWN:
        return x;

    case CV_INT64:
        if (s == 0)
            return x;
        if (op == TOKEN_SHL) {
            BigInt *z = big_new_int(a, x.u.i);
            return cv_make_int(big_int_lsh(z, z, s));
        }
        if (op == TOKEN_SHR)
            return cv_int64(s >= 64 ? (x.u.i < 0 ? -1 : 0) : int64_shr(x.u.i, (Int)s));
        break;

    case CV_INT:
        if (s == 0)
            return x;
        if (op == TOKEN_SHL)
            return cv_make_int(big_int_lsh(cv_new_int(a), cv_int_of(x), s));
        if (op == TOKEN_SHR)
            return cv_make_int(big_int_rsh(cv_new_int(a), cv_int_of(x), s));
        break;

    default:
        break;
    }

    Alloc *ea = error_allocator();
    panic_str(fmt_sprintf_v(ea, "invalid shift %s %s %d", constant_value_string(x, ea),
                            token_string(op, ea), s));
}

static bool cv_cmp_zero(Int x, Token op) {
    switch (op) {
    case TOKEN_EQL:
        return x == 0;
    case TOKEN_NEQ:
        return x != 0;
    case TOKEN_LSS:
        return x < 0;
    case TOKEN_LEQ:
        return x <= 0;
    case TOKEN_GTR:
        return x > 0;
    case TOKEN_GEQ:
        return x >= 0;
    default:
        break;
    }
    Alloc *ea = error_allocator();
    panic_str(fmt_sprintf_v(ea, "invalid comparison %d %s 0", x, token_string(op, ea)));
}

static bool cv_is_comparison(Token op) {
    switch (op) {
    case TOKEN_EQL:
    case TOKEN_NEQ:
    case TOKEN_LSS:
    case TOKEN_LEQ:
    case TOKEN_GTR:
    case TOKEN_GEQ:
        return true;
    default:
        return false;
    }
}

/* Compare of two values that match has made the same, with the int64 and
 * big cases boiled down to a three way result in *c. Returns 1 or 0 for a
 * result, 2 when *c is to be compared with zero, and -1 when the operation
 * is invalid. */
static int cv_compare_matched(ConstantValue x, Token op, ConstantValue y, Int *c) {
    switch (x.rep) {
    case CV_UNKNOWN:
        return 0;

    case CV_BOOL:
        if (y.rep != CV_BOOL)
            return -2;
        if (op == TOKEN_EQL)
            return x.u.b == y.u.b;
        if (op == TOKEN_NEQ)
            return x.u.b != y.u.b;
        return -1;

    case CV_INT64:
        if (!cv_is_comparison(op))
            return -1;
        *c = x.u.i < y.u.i ? -1 : x.u.i > y.u.i ? 1 : 0;
        return 2;

    case CV_INT:
        *c = big_int_cmp(cv_int_of(x), cv_int_of(y));
        return 2;

    case CV_RAT:
        *c = big_rat_cmp(cv_rat_of(x), cv_rat_of(y));
        return 2;

    case CV_FLOAT:
        *c = big_float_cmp(cv_float_of(x), cv_float_of(y));
        return 2;

    case CV_COMPLEX: {
        const CvComplex *p = cv_complex_of(x), *q = cv_complex_of(y);
        bool re = constant_compare(p->re, TOKEN_EQL, q->re);
        bool im = constant_compare(p->im, TOKEN_EQL, q->im);
        if (op == TOKEN_EQL)
            return re && im;
        if (op == TOKEN_NEQ)
            return !re || !im;
        return -1;
    }

    case CV_STRING:
        if (y.rep != CV_STRING)
            return -2;
        if (!cv_is_comparison(op))
            return -1;
        *c = str_cmp(cv_string(x), cv_string(y));
        return 2;

    default:
        return -1;
    }
}

bool constant_compare(ConstantValue x_, Token op, ConstantValue y_) {
    /* match makes Go's temporaries, which only live until the comparison
     * is made. */
    Arena ar;
    arena_init(&ar, NULL, 0);
    ConstantValue x = x_, y = y_;
    cv_match(arena_allocator(&ar), &x, &y);
    Int c = 0;
    int r = cv_compare_matched(x, op, y, &c);
    arena_free(&ar);

    if (r == 0 || r == 1)
        return r == 1;
    if (r == 2)
        return cv_cmp_zero(c, op);
    if (r == -2)
        cv_assert_failed(y, x.rep);
    Alloc *ea = error_allocator();
    panic_str(fmt_sprintf_v(ea, "invalid comparison %s %s %s",
                            constant_value_string(x_, ea), token_string(op, ea),
                            constant_value_string(y_, ea)));
}

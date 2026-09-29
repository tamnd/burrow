/* Derived from Go's src/math/big/float_test.go, floatconv_test.go,
 * floatmarsh_test.go and sqrt_test.go.
 * Go source: go1.27.1.
 *
 * Most of what Go checks is in tests/big_float_test_gen.h, from
 * tools/gen-math-big-float-tests.sh: every string in Go's own Float tests
 * through Parse, random Floats at many precisions and in every rounding mode
 * through the arithmetic, the conversions and the text formats, and values at
 * the ends of the exponent range. TestGoCases runs them all. Every operation
 * that sets a receiver runs again on an arena, and again with the receiver
 * aliasing an operand, where it has to agree with a fresh receiver of the
 * operand's precision and mode. The tests after it are about burrow's API.
 *
 * Copyright 2014 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/math/big.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/thread.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverlength-strings"
#endif
#include "big_float_test_gen.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

static Arena ar;
static Alloc *a;

static Str S(const char *s) {
    return str_from_cstr(s);
}

static const char *cs(Str s) {
    char *b = mem_alloc(a, (size_t)s.len + 1, 1);
    if (s.len > 0)
        memcpy(b, s.p, (size_t)s.len);
    b[s.len] = '\0';
    return b;
}

static char cfbuf[1 << 16];

/* snprintf into memory on the arena. */
#define cf(...) (snprintf(cfbuf, sizeof cfbuf, __VA_ARGS__), cs(S(cfbuf)))

/* A receiver, "prec/mode". */
static BigFloat recv(Alloc *al, const char *c) {
    char *end;
    unsigned long prec = strtoul(c, &end, 10);
    int mode = (int)strtol(end + 1, NULL, 10);
    BigFloat z = BIG_FLOAT(al);
    big_float_set_mode(&z, (BigRoundingMode)mode);
    if (prec > 0)
        big_float_set_prec(&z, (Uint)prec);
    return z;
}

/* An operand, "prec/mode/value" with the value in the 'p' format. */
static BigFloat parse(const char *s) {
    char *end;
    unsigned long prec = strtoul(s, &end, 10);
    int mode = (int)strtol(end + 1, &end, 10);
    BigFloat z = BIG_FLOAT(a);
    big_float_set_mode(&z, (BigRoundingMode)mode);
    if (prec > 0)
        big_float_set_prec(&z, (Uint)prec);
    Error err;
    if (big_float_parse(&z, S(end + 1), 0, NULL, &err) == NULL) {
        fprintf(stderr, "bad operand %s\n", s);
        abort();
    }
    if (prec == 0)
        big_float_set_prec(&z, 0);
    return z;
}

/* A fresh receiver of x's precision and mode, which is what a receiver that
 * aliases x is. */
static BigFloat like(const BigFloat *x) {
    BigFloat z = BIG_FLOAT(NULL);
    big_float_set_mode(&z, big_float_mode(x));
    if (big_float_prec(x) > 0)
        big_float_set_prec(&z, big_float_prec(x));
    return z;
}

/* A result, "prec/mode/acc/value". */
static const char *res(const BigFloat *z) {
    return cs(fmt_sprintf_v(a, "%d/%d/%d/%s", (int64_t)big_float_prec(z),
                            (int)big_float_mode(z), (int)big_float_acc(z),
                            big_float_text(z, a, 'p', 0)));
}

static const char *acc(BigAccuracy x) {
    return cf(" %d", (int)x);
}

static uint64_t fnv64a(const char *s) {
    uint64_t h = 14695981039346656037ull;
    for (; *s; s++) {
        h ^= (Byte)*s;
        h *= 1099511628211ull;
    }
    return h;
}

/* Whether got is want, or hashes to it when want is a hash. */
static bool same(const char *got, const char *want) {
    if (want[0] == '#') {
        char buf[20];
        snprintf(buf, sizeof buf, "#%016llx", (unsigned long long)fnv64a(got));
        return strcmp(buf, want) == 0;
    }
    return strcmp(got, want) == 0;
}

static const char *hexbytes(Slice b) {
    char *s = mem_alloc(a, (size_t)b.len * 2 + 1, 1);
    for (Int i = 0; i < b.len; i++)
        snprintf(s + 2 * i, 3, "%02x", ((const Byte *)b.p)[i]);
    s[b.len * 2] = '\0';
    return s;
}

static int nibble(char c) {
    return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10;
}

static Slice unhex(const char *s) {
    Int n = (Int)strlen(s) / 2;
    Slice b = slice_make(a, TYPE_BYTE, n, n);
    for (Int i = 0; i < n; i++)
        ((Byte *)b.p)[i] = (Byte)(nibble(s[2 * i]) << 4 | nibble(s[2 * i + 1]));
    return b;
}

static Slice bytes_of(const char *s) {
    Int n = (Int)strlen(s);
    return slice_from((void *)(uintptr_t)s, n, n, TYPE_BYTE);
}

static const char *err_or(Error err, const char *ok) {
    return BURROW_FAILED(err) ? cs(fmt_sprintf_v(a, "err:%s", error_text(err))) : ok;
}

typedef BigFloat *(*BinOp)(BigFloat *z, const BigFloat *x, const BigFloat *y);
typedef BigFloat *(*UnOp)(BigFloat *z, const BigFloat *x);

/* One call to a binary or unary operation, run under a recover so that a NaN
 * comes back as "nan:" and its message. */
typedef struct Call {
    BinOp bin;
    UnOp un;
    BigFloat *z;
    const BigFloat *x, *y;
} Call;

static const char *run(const Call *c) {
    const char *volatile out = NULL;
    BURROW_TRY {
        if (c->bin != NULL)
            out = res(c->bin(c->z, c->x, c->y));
        else
            out = res(c->un(c->z, c->x));
    }
    BURROW_CATCH(r) {
        out = cs(fmt_sprintf_v(a, "nan:%s", panic_text(r)));
    }
    BURROW_TRY_END;
    return out;
}

/* A receiver on the arena that already holds a long mantissa. */
static BigFloat busy(const char *c) {
    BigFloat w = BIG_FLOAT(a);
    big_float_set_prec(&w, 3000);
    big_float_set_int64(&w, 3);
    big_float_sqrt(&w, &w);
    BigFloat r = recv(NULL, c);
    big_float_set_prec(&w, big_float_prec(&r));
    big_float_set_mode(&w, big_float_mode(&r));
    return w;
}

static void want(TestingT *t, const FloatCase *c, const char *how, const char *got) {
    if (!same(got, c->want))
        testing_t_errorf_v(t, "%s(%s, %s, %s) %s= %s, want %s", c->op, c->a, c->b, c->c,
                           how, got, c->want);
}

static void agree(TestingT *t, const FloatCase *c, const char *how, const char *got,
                  const char *fresh) {
    if (strcmp(got, fresh) != 0)
        testing_t_errorf_v(t, "%s(%s, %s, %s) %s= %s, but a fresh receiver gives %s",
                           c->op, c->a, c->b, c->c, how, got, fresh);
}

static void check_binop(TestingT *t, const FloatCase *c, BinOp op) {
    BigFloat x = parse(c->a), y = parse(c->b);
    BigFloat z = recv(NULL, c->c);
    want(t, c, "", run(&(Call){op, NULL, &z, &x, &y}));
    big_float_free(&z);

    BigFloat w = busy(c->c);
    want(t, c, "on an arena ", run(&(Call){op, NULL, &w, &x, &y}));

    /* into x, which has x's precision and mode */
    BigFloat f = like(&x);
    const char *fresh = run(&(Call){op, NULL, &f, &x, &y});
    big_float_free(&f);
    BigFloat x2 = parse(c->a);
    agree(t, c, "into x ", run(&(Call){op, NULL, &x2, &x2, &y}), fresh);

    /* into y */
    f = like(&y);
    fresh = run(&(Call){op, NULL, &f, &x, &y});
    big_float_free(&f);
    BigFloat y2 = parse(c->b);
    agree(t, c, "into y ", run(&(Call){op, NULL, &y2, &x, &y2}), fresh);

    /* x op x with one Float, when the case is one */
    if (strcmp(c->a, c->b) == 0) {
        f = like(&x);
        fresh = run(&(Call){op, NULL, &f, &x, &x});
        big_float_free(&f);
        x2 = parse(c->a);
        agree(t, c, "into x with x twice ", run(&(Call){op, NULL, &x2, &x2, &x2}),
              fresh);
    }
}

static void check_unop(TestingT *t, const FloatCase *c, UnOp op) {
    BigFloat x = parse(c->a);
    BigFloat z = recv(NULL, c->c);
    want(t, c, "", run(&(Call){NULL, op, &z, &x, NULL}));
    big_float_free(&z);

    BigFloat w = busy(c->c);
    want(t, c, "on an arena ", run(&(Call){NULL, op, &w, &x, NULL}));

    BigFloat f = like(&x);
    const char *fresh = run(&(Call){NULL, op, &f, &x, NULL});
    big_float_free(&f);
    BigFloat x2 = parse(c->a);
    agree(t, c, "into x ", run(&(Call){NULL, op, &x2, &x2, NULL}), fresh);
}

static void parse_int(BigInt *z, const char *hex, Int n) {
    bool ok;
    big_int_set_string(z, (Str){(const Byte *)hex, n}, 16, &ok);
    if (!ok) {
        fprintf(stderr, "bad operand %s\n", hex);
        abort();
    }
}

static const char *set_float64(BigFloat *z, double f) {
    const char *volatile out = NULL;
    BURROW_TRY {
        out = res(big_float_set_float64(z, f));
    }
    BURROW_CATCH(r) {
        out = cs(fmt_sprintf_v(a, "nan:%s", panic_text(r)));
    }
    BURROW_TRY_END;
    return out;
}

static void check_case(TestingT *t, const FloatCase *c) {
    const char *op = c->op;
    const char *got = NULL;

    if (strcmp(op, "add") == 0) {
        check_binop(t, c, big_float_add);
        return;
    } else if (strcmp(op, "sub") == 0) {
        check_binop(t, c, big_float_sub);
        return;
    } else if (strcmp(op, "mul") == 0) {
        check_binop(t, c, big_float_mul);
        return;
    } else if (strcmp(op, "quo") == 0) {
        check_binop(t, c, big_float_quo);
        return;
    } else if (strcmp(op, "sqrt") == 0) {
        check_unop(t, c, big_float_sqrt);
        return;
    } else if (strcmp(op, "neg") == 0) {
        check_unop(t, c, big_float_neg);
        return;
    } else if (strcmp(op, "abs") == 0) {
        check_unop(t, c, big_float_abs);
        return;
    } else if (strcmp(op, "set") == 0) {
        check_unop(t, c, big_float_set);
        return;
    } else if (strcmp(op, "copy") == 0) {
        check_unop(t, c, big_float_copy);
        return;
    }

    BigFloat z = BIG_FLOAT(NULL);
    if (c->c[0] != '\0' && strchr(c->c, '/') != NULL)
        z = recv(NULL, c->c);

    if (strcmp(op, "cmp") == 0) {
        BigFloat x = parse(c->a), y = parse(c->b);
        got = cf("%d", (int)big_float_cmp(&x, &y));
    } else if (strcmp(op, "parse") == 0) {
        Int b;
        Error err;
        BigFloat *f =
            big_float_parse(&z, S(c->a), (Int)strtol(c->b, NULL, 10), &b, &err);
        if ((f == NULL) != BURROW_FAILED(err))
            testing_t_errorf_v(t, "parse(%q): the result and the error disagree", c->a);
        got = err_or(err, cf("%s %d", res(&z), (int)b));
        BigFloat w = busy(c->c);
        big_float_parse(&w, S(c->a), (Int)strtol(c->b, NULL, 10), &b, &err);
        want(t, c, "on an arena ", err_or(err, cf("%s %d", res(&w), (int)b)));
    } else if (strcmp(op, "uint64") == 0) {
        BigFloat x = parse(c->a);
        BigAccuracy e;
        uint64_t u = big_float_uint64(&x, &e);
        got = cf("%llu%s", (unsigned long long)u, acc(e));
    } else if (strcmp(op, "int64") == 0) {
        BigFloat x = parse(c->a);
        BigAccuracy e;
        int64_t i = big_float_int64(&x, &e);
        got = cf("%lld%s", (long long)i, acc(e));
    } else if (strcmp(op, "float32") == 0) {
        BigFloat x = parse(c->a);
        BigAccuracy e;
        float f = big_float_float32(&x, &e);
        uint32_t bits;
        memcpy(&bits, &f, sizeof bits);
        got = cf("%08lx%s", (unsigned long)bits, acc(e));
    } else if (strcmp(op, "float64") == 0) {
        BigFloat x = parse(c->a);
        BigAccuracy e;
        double f = big_float_float64(&x, &e);
        uint64_t bits;
        memcpy(&bits, &f, sizeof bits);
        got = cf("%016llx%s", (unsigned long long)bits, acc(e));
    } else if (strcmp(op, "int") == 0) {
        BigFloat x = parse(c->a);
        BigAccuracy e;
        BigInt n = {0};
        BigInt *r = big_float_int(&x, &n, &e);
        got = cf("%s%s", r == NULL ? "nil" : cs(big_int_text(r, a, 16)), acc(e));
        big_int_free(&n);
    } else if (strcmp(op, "rat") == 0) {
        BigFloat x = parse(c->a);
        BigAccuracy e;
        BigRat q = {0};
        BigRat *r = big_float_rat(&x, &q, &e);
        got = cf("%s%s", r == NULL ? "nil" : cs(big_rat_string(r, a)), acc(e));
        big_rat_free(&q);
    } else if (strcmp(op, "mantexp") == 0) {
        BigFloat x = parse(c->a);
        BigFloat m = BIG_FLOAT(NULL);
        Int e = big_float_mant_exp(&x, &m);
        got = cf("%lld %s", (long long)e, res(&m));
        CHECK_INT_EQ(big_float_mant_exp(&x, NULL), e);
        big_float_free(&m);
        /* into x */
        big_float_mant_exp(&x, &x);
        want(t, c, "into x ", cf("%lld %s", (long long)e, res(&x)));
    } else if (strcmp(op, "minprec") == 0) {
        BigFloat x = parse(c->a);
        got = cf("%llu", (unsigned long long)big_float_min_prec(&x));
    } else if (strcmp(op, "sign") == 0) {
        BigFloat x = parse(c->a);
        got = cf("%d", (int)big_float_sign(&x));
    } else if (strcmp(op, "isint") == 0) {
        BigFloat x = parse(c->a);
        got = big_float_is_int(&x) ? "true" : "false";
    } else if (strcmp(op, "setprec") == 0) {
        BigFloat x = parse(c->a);
        big_float_copy(&z, &x);
        got = res(big_float_set_prec(&z, (Uint)strtoul(c->b, NULL, 10)));
    } else if (strcmp(op, "setmode") == 0) {
        BigFloat x = parse(c->a);
        big_float_copy(&z, &x);
        got = res(big_float_set_mode(&z, (BigRoundingMode)strtol(c->b, NULL, 10)));
    } else if (strcmp(op, "setmantexp") == 0) {
        BigFloat x = parse(c->a);
        Int e = (Int)strtoll(c->b, NULL, 10);
        got = res(big_float_set_mant_exp(&z, &x, e));
        BigFloat w = busy(c->c);
        want(t, c, "on an arena ", res(big_float_set_mant_exp(&w, &x, e)));
    } else if (strcmp(op, "setint64") == 0) {
        got = res(big_float_set_int64(&z, strtoll(c->a, NULL, 10)));
    } else if (strcmp(op, "setuint64") == 0) {
        got = res(big_float_set_uint64(&z, strtoull(c->a, NULL, 10)));
    } else if (strcmp(op, "setfloat64") == 0) {
        uint64_t bits = strtoull(c->a, NULL, 16);
        double f;
        memcpy(&f, &bits, sizeof f);
        got = set_float64(&z, f);
    } else if (strcmp(op, "setint") == 0) {
        BigInt n = BIG_INT(a);
        parse_int(&n, c->a, (Int)strlen(c->a));
        got = res(big_float_set_int(&z, &n));
        BigFloat w = busy(c->c);
        want(t, c, "on an arena ", res(big_float_set_int(&w, &n)));
    } else if (strcmp(op, "setrat") == 0) {
        const char *slash = strchr(c->a, '/');
        BigInt n = BIG_INT(a), d = BIG_INT(a);
        parse_int(&n, c->a, (Int)(slash - c->a));
        parse_int(&d, slash + 1, (Int)strlen(slash + 1));
        BigRat q = BIG_RAT(a);
        big_rat_set_frac(&q, &n, &d);
        got = res(big_float_set_rat(&z, &q));
        BigFloat w = busy(c->c);
        want(t, c, "on an arena ", res(big_float_set_rat(&w, &q)));
    } else if (strcmp(op, "text") == 0) {
        BigFloat x = parse(c->a);
        got = cs(big_float_text(&x, a, (Byte)c->b[0], (Int)strtol(c->c, NULL, 10)));
        /* Append onto something already there */
        Slice b = big_float_append(&x, a, bytes_of("<"), (Byte)c->b[0],
                                   (Int)strtol(c->c, NULL, 10));
        want(t, c, "appended ", cs((Str){(const Byte *)b.p + 1, b.len - 1}));
    } else if (strcmp(op, "string") == 0) {
        BigFloat x = parse(c->a);
        got = cs(big_float_string(&x, a));
    } else if (strcmp(op, "sprintf") == 0) {
        BigFloat x = parse(c->a);
        Any args[] = {BURROW_ANY(TYPE_BIG_FLOAT, &x)};
        got = cs(fmt_sprintf(a, S(c->b), slice_from(args, 1, 1, TYPE_ANY)));
    } else if (strcmp(op, "marshaltext") == 0) {
        BigFloat x = parse(c->a);
        Error err;
        Slice b = big_float_marshal_text(&x, a, &err);
        CHECK(BURROW_OK(err));
        got = cs((Str){b.p, b.len});
    } else if (strcmp(op, "gobencode") == 0) {
        BigFloat x = parse(c->a);
        Error err;
        Slice b = big_float_gob_encode(&x, a, &err);
        CHECK(BURROW_OK(err));
        got = hexbytes(b);
        /* and back */
        BigFloat y = BIG_FLOAT(a);
        CHECK(BURROW_OK(big_float_gob_decode(&y, b)));
        if (big_float_cmp(&x, &y) != 0 || big_float_prec(&x) != big_float_prec(&y) ||
            big_float_signbit(&x) != big_float_signbit(&y))
            testing_t_errorf_v(t, "gob round trip of %s gives %s", c->a, res(&y));
    } else if (strcmp(op, "gobdecode") == 0) {
        big_float_set_int64(&z, 5);
        Error err = big_float_gob_decode(&z, unhex(c->a));
        got = err_or(err, res(&z));
    } else if (strcmp(op, "unmarshaltext") == 0) {
        Error err = big_float_unmarshal_text(&z, bytes_of(c->a));
        got = err_or(err, res(&z));
    } else if (strcmp(op, "scan") == 0) {
        big_float_set_float64(&z, 5);
        Any args[] = {BURROW_ANY(TYPE_BIG_FLOAT, &z)};
        Error err;
        fmt_sscanf(a, S(c->a), S(c->b), slice_from(args, 1, 1, TYPE_ANY), &err);
        got = err_or(err, res(&z));
    } else {
        testing_t_errorf_v(t, "unknown case %s", op);
        return;
    }
    want(t, c, "", got);
    big_float_free(&z);
}

static void TestGoCases(TestingT *t) {
    Int n = (Int)(sizeof float_cases / sizeof float_cases[0]);
    for (Int i = 0; i < n; i++) {
        check_case(t, &float_cases[i]);
        arena_reset(&ar);
    }
}

/* ------------------------------------------------------- burrow's own API */

/* Go's TestFloatZeroValue: a zero Float is 0 with precision 0 and takes the
 * precision of what is stored in it. */
static void TestZeroValue(TestingT *t) {
    BigFloat x = {0};
    CHECK_STR_EQ(cs(big_float_text(&x, a, 'f', 1)), "0.0");
    CHECK_INT_EQ(big_float_prec(&x), 0);
    CHECK_INT_EQ(big_float_sign(&x), 0);
    CHECK(!big_float_signbit(&x) && big_float_is_int(&x) && !big_float_is_inf(&x));

    big_float_set_int64(&x, 42);
    CHECK_INT_EQ(big_float_prec(&x), 64);
    /* free keeps the precision */
    big_float_free(&x);
    CHECK_INT_EQ(big_float_prec(&x), 64);
    CHECK_INT_EQ(big_float_sign(&x), 0);
    x = (BigFloat){0};
    big_float_set_float64(&x, 0.5);
    CHECK_INT_EQ(big_float_prec(&x), 53);
    big_float_free(&x);
    x = (BigFloat){0};

    BigFloat y = {0}, z = {0};
    big_float_set_prec(&y, 100);
    big_float_add(&z, &x, &y);
    CHECK_INT_EQ(big_float_prec(&z), 100);
    big_float_free(&y);
    big_float_free(&z);
    big_float_free(NULL);
}

static void TestNewFloat(TestingT *t) {
    BigFloat *x = big_new_float(a, 1.5);
    CHECK(x->a == a);
    CHECK_INT_EQ(big_float_prec(x), 53);
    CHECK_STR_EQ(cs(big_float_string(x, a)), "1.5");

    Int b;
    Error err;
    x = big_parse_float(a, S("0x1.8p3"), 0, 10, BIG_TO_ZERO, &b, &err);
    CHECK(x != NULL && BURROW_OK(err));
    CHECK_INT_EQ(b, 16);
    CHECK_INT_EQ(big_float_prec(x), 10);
    CHECK(big_float_mode(x) == BIG_TO_ZERO);
    CHECK_STR_EQ(cs(big_float_text(x, a, 'g', -1)), "12");
    x = big_parse_float(a, S("1.2.3"), 0, 10, BIG_TO_ZERO, &b, &err);
    CHECK(x == NULL);
    CHECK_STR_EQ(cs(error_text(err)), "expected end of string, found '.'");
}

/* Go's example for Float.Shift and TestFloatSetMantExp by hand. */
static void TestMantExp(TestingT *t) {
    BigFloat x = BIG_FLOAT(a), m = BIG_FLOAT(a);
    big_float_set_float64(&x, 12);
    CHECK_INT_EQ(big_float_mant_exp(&x, &m), 4);
    CHECK_STR_EQ(cs(big_float_string(&m, a)), "0.75");
    big_float_set_mant_exp(&x, &m, -2);
    CHECK_STR_EQ(cs(big_float_string(&x, a)), "0.1875");
}

static const char *panic_of(void (*f)(void)) {
    const char *volatile msg = NULL;
    BURROW_TRY {
        f();
    }
    BURROW_CATCH(r) {
        msg = cs(panic_text(r));
    }
    BURROW_TRY_END;
    return msg;
}

static void new_nan(void) {
    big_new_float(a, (double)NAN);
}

static void sub_inf(void) {
    BigFloat x = BIG_FLOAT(a);
    big_float_set_inf(&x, false);
    big_float_sub(&x, &x, &x);
}

/* A NaN panics with an ErrNaN, which errors_as gets back out. */
static void TestErrNaN(TestingT *t) {
    CHECK_STR_EQ(panic_of(new_nan), "NewFloat(NaN)");
    CHECK_STR_EQ(panic_of(sub_inf), "subtraction of infinities with equal signs");

    bool caught = false;
    BURROW_TRY {
        sub_inf();
    }
    BURROW_CATCH(r) {
        CHECK(r.t == TYPE_ERROR);
        Error err = *(const Error *)r.data;
        const BigErrNaN *e = errors_as(err, TYPE_BIG_ERR_NAN);
        CHECK(e != NULL);
        if (e != NULL)
            CHECK_STR_EQ(cs(big_err_nan_error(*e)),
                         "subtraction of infinities with equal signs");
        caught = true;
    }
    BURROW_TRY_END;
    CHECK(caught);
}

static void TestFormatWidths(TestingT *t) {
    BigFloat x = BIG_FLOAT(a);
    big_float_set_float64(&x, -1.25);
    Any args[] = {BURROW_ANY(TYPE_BIG_FLOAT, &x), BURROW_ANY(TYPE_BIG_FLOAT, &x)};
    CHECK_STR_EQ(
        cs(fmt_sprintf(a, S("[%8.3f|%-8.1e]"), slice_from(args, 2, 2, TYPE_ANY))),
        "[  -1.250|-1.2e+00]");
    big_float_set_inf(&x, false);
    CHECK_STR_EQ(cs(fmt_sprintf(a, S("[%08v|% v]"), slice_from(args, 2, 2, TYPE_ANY))),
                 "[    +Inf| Inf]");
}

/* Go's TestFloatGobEncoding, a round trip in every mode and a few
 * precisions. */
static void TestGobRoundTrip(TestingT *t) {
    static const char *const values[] = {
        "0",        "1",   "-1",   "123456789012345678901234567890",
        "1.5e-300", "Inf", "-Inf", "3.14159265358979323846264338"};
    static const Uint precs[] = {0, 1, 2, 10, 42, 53, 64, 100, 1000};
    for (size_t i = 0; i < sizeof values / sizeof values[0]; i++) {
        for (int m = 0; m <= 5; m++) {
            for (size_t j = 0; j < sizeof precs / sizeof precs[0]; j++) {
                BigFloat x = BIG_FLOAT(a), y = BIG_FLOAT(a);
                big_float_set_prec(&x, 1000);
                big_float_set_mode(&x, (BigRoundingMode)m);
                CHECK(big_float_set_string(&x, S(values[i]), NULL) != NULL);
                big_float_set_prec(&x, precs[j]);
                Error err;
                Slice b = big_float_gob_encode(&x, a, &err);
                CHECK(BURROW_OK(err));
                CHECK(BURROW_OK(big_float_gob_decode(&y, b)));
                CHECK_INT_EQ(big_float_cmp(&x, &y), 0);
                CHECK_INT_EQ(big_float_prec(&x), big_float_prec(&y));
                CHECK(big_float_mode(&x) == big_float_mode(&y));
                CHECK(big_float_acc(&x) == big_float_acc(&y));
            }
        }
    }
}

static void worker(void *arg) {
    Str *out = arg;
    BigFloat x = {0};
    big_float_set_prec(&x, 2000);
    big_float_set_int64(&x, 2);
    big_float_sqrt(&x, &x);
    *out = big_float_text(&x, heap_allocator(), 'g', 500);
    big_float_free(&x);
}

/* Each thread has its own scratch memory: every thread takes the same square
 * root at once. */
static void TestThreads(TestingT *t) {
    enum { N = 4 };
    burrow__Thread th[N];
    Str out[N];
    for (int i = 0; i < N; i++)
        CHECK(burrow__thread_start(&th[i], worker, &out[i], 0));
    for (int i = 0; i < N; i++)
        CHECK(burrow__thread_join(&th[i]));
    for (int i = 1; i < N; i++)
        CHECK(str_eq(out[i], out[0]));
    CHECK_STR_EQ(cs((Str){out[0].p, 12}), "1.4142135623");
    for (int i = 0; i < N; i++)
        mem_free(heap_allocator(), (void *)(uintptr_t)out[i].p, (size_t)out[i].len, 1);
}

static void setup(void) {
    arena_init(&ar, NULL, 1 << 16);
    a = arena_allocator(&ar);
}

#define TESTS(X)                                                                       \
    X(TestGoCases)                                                                     \
    X(TestZeroValue)                                                                   \
    X(TestNewFloat)                                                                    \
    X(TestMantExp)                                                                     \
    X(TestErrNaN)                                                                      \
    X(TestFormatWidths)                                                                \
    X(TestGobRoundTrip)                                                                \
    X(TestThreads)

static int float_main(TestingM *m) {
    setup();
    int r = testing_m_run(m);
    arena_free(&ar);
    return r;
}

TESTING_MAIN_WITH(float_main, TESTS)

/* Derived from Go's src/math/big/int_test.go, intconv_test.go,
 * intmarsh_test.go and prime_test.go.
 * Go source: go1.27.1.
 *
 * Most of what Go checks is in tests/big_test_gen.h, from
 * tools/gen-math-big-tests.sh: a few thousand operations on random numbers of
 * every size the package treats differently, and Go's fixed tables, each with
 * the result Go's math/big gives. TestGoCases runs them all, and runs every
 * operation that sets a receiver three more times, with the receiver aliasing
 * each operand and on an arena, because where the result lands is the part of
 * this port that Go does not have. The tests after it are the ones that are
 * about burrow's API rather than about the arithmetic.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/math/big.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/thread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The long operands are longer than the 4095 bytes C99 promises a string literal
 * can hold. Every compiler burrow builds with takes them. */
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverlength-strings"
#endif
#include "big_test_gen.h"
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

/* The number in hex, with the heap as its allocator. */
static void parse(BigInt *z, const char *hex) {
    bool ok;
    big_int_set_string(z, S(hex), 16, &ok);
    if (!ok) {
        fprintf(stderr, "bad operand %s\n", hex);
        abort();
    }
}

/* A decimal operand. */
static Int dec(const char *s) {
    return (Int)strtoll(s, NULL, 10);
}

static const char *hx(const BigInt *x) {
    if (x == NULL)
        return "nil";
    return cs(big_int_text(x, a, 16));
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

/* By hand, since cosmopolitan's sscanf does not count a leading 0 toward
 * the width of %2x. */
static unsigned nibble(char c) {
    return c <= '9' ? (unsigned)(c - '0') : (unsigned)((c | 0x20) - 'a' + 10);
}

static Slice unhex(const char *s) {
    Int n = (Int)strlen(s) / 2;
    Slice b = slice_make(a, TYPE_BYTE, n, n);
    for (Int i = 0; i < n; i++)
        ((Byte *)b.p)[i] = (Byte)(nibble(s[2 * i]) << 4 | nibble(s[2 * i + 1]));
    return b;
}

static const char *join2(const char *x, const char *y) {
    size_t n = strlen(x) + strlen(y) + 2;
    char *s = mem_alloc(a, n, 1);
    snprintf(s, n, "%s %s", x, y);
    return s;
}

static const char *join3(const char *x, const char *y, const char *z) {
    return join2(join2(x, y), z);
}

typedef BigInt *(*BinOp)(BigInt *z, const BigInt *x, const BigInt *y);
typedef BigInt *(*UnOp)(BigInt *z, const BigInt *x);

/* Runs op four ways: into a fresh Int, into x, into y, and into an Int on an
 * arena that already holds a long number, and checks each result. */
static void check_binop(TestingT *t, const BigCase *c, BinOp op) {
    BigInt x = {0}, y = {0}, z = {0};
    parse(&x, c->a);
    parse(&y, c->b);
    op(&z, &x, &y);
    const char *got = hx(&z);
    if (!same(got, c->want))
        testing_t_errorf_v(t, "%s(%s, %s) = %s, want %s", c->op, c->a, c->b, got,
                           c->want);

    BigInt x2 = {0};
    parse(&x2, c->a);
    op(&x2, &x2, &y);
    got = hx(&x2);
    if (!same(got, c->want))
        testing_t_errorf_v(t, "%s(%s, %s) into x = %s, want %s", c->op, c->a, c->b, got,
                           c->want);

    BigInt y2 = {0};
    parse(&y2, c->b);
    op(&y2, &x, &y2);
    got = hx(&y2);
    if (!same(got, c->want))
        testing_t_errorf_v(t, "%s(%s, %s) into y = %s, want %s", c->op, c->a, c->b, got,
                           c->want);

    BigInt w = BIG_INT(a);
    big_int_lsh(&w, big_int_set_int64(&w, 12345), 2000);
    op(&w, &x, &y);
    got = hx(&w);
    if (!same(got, c->want))
        testing_t_errorf_v(t, "%s(%s, %s) on an arena = %s, want %s", c->op, c->a, c->b,
                           got, c->want);

    big_int_free(&x);
    big_int_free(&y);
    big_int_free(&z);
    big_int_free(&x2);
    big_int_free(&y2);
}

static void check_unop(TestingT *t, const BigCase *c, UnOp op) {
    BigInt x = {0}, z = {0};
    parse(&x, c->a);
    op(&z, &x);
    const char *got = hx(&z);
    if (!same(got, c->want))
        testing_t_errorf_v(t, "%s(%s) = %s, want %s", c->op, c->a, got, c->want);
    op(&x, &x);
    got = hx(&x);
    if (!same(got, c->want))
        testing_t_errorf_v(t, "%s(%s) into x = %s, want %s", c->op, c->a, got, c->want);
    big_int_free(&x);
    big_int_free(&z);
}

static BigInt *sqr_op(BigInt *z, const BigInt *x) {
    return big_int_mul(z, x, x);
}

static BigInt *sqrt_op(BigInt *z, const BigInt *x) {
    return big_int_sqrt(z, x);
}

static void check_case(TestingT *t, const BigCase *c) {
    const char *op = c->op;
    const char *got = NULL;
    BigInt x = {0}, y = {0}, z = {0}, m = {0};

    if (strcmp(op, "add") == 0) {
        check_binop(t, c, big_int_add);
    } else if (strcmp(op, "sub") == 0) {
        check_binop(t, c, big_int_sub);
    } else if (strcmp(op, "mul") == 0) {
        check_binop(t, c, big_int_mul);
    } else if (strcmp(op, "quo") == 0) {
        check_binop(t, c, big_int_quo);
    } else if (strcmp(op, "rem") == 0) {
        check_binop(t, c, big_int_rem);
    } else if (strcmp(op, "div") == 0) {
        check_binop(t, c, big_int_div);
    } else if (strcmp(op, "mod") == 0) {
        check_binop(t, c, big_int_mod);
    } else if (strcmp(op, "and") == 0) {
        check_binop(t, c, big_int_and);
    } else if (strcmp(op, "andnot") == 0) {
        check_binop(t, c, big_int_and_not);
    } else if (strcmp(op, "or") == 0) {
        check_binop(t, c, big_int_or);
    } else if (strcmp(op, "xor") == 0) {
        check_binop(t, c, big_int_xor);
    } else if (strcmp(op, "not") == 0) {
        check_unop(t, c, big_int_not);
    } else if (strcmp(op, "neg") == 0) {
        check_unop(t, c, big_int_neg);
    } else if (strcmp(op, "abs") == 0) {
        check_unop(t, c, big_int_abs);
    } else if (strcmp(op, "sqr") == 0) {
        check_unop(t, c, sqr_op);
    } else if (strcmp(op, "sqrt") == 0) {
        check_unop(t, c, sqrt_op);
    } else if (strcmp(op, "quorem") == 0 || strcmp(op, "divmod") == 0 ||
               strcmp(op, "divide") == 0) {
        parse(&x, c->a);
        parse(&y, c->b);
        /* into fresh Ints, and then with the quotient in x and the
         * remainder in y */
        for (int k = 0; k < 2; k++) {
            BigInt *q = k == 0 ? &z : &x;
            BigInt *r = k == 0 ? &m : &y;
            BigInt xs = {0}, ys = {0};
            big_int_set(&xs, &x);
            big_int_set(&ys, &y);
            if (op[3] == 'r')
                big_int_quo_rem(q, &xs, &ys, r);
            else if (op[3] == 'm')
                big_int_div_mod(q, &xs, &ys, r);
            else
                big_int_divide(q, &xs, &ys, r,
                               c->c[0] == 'T'   ? BIG_TRUNC
                               : c->c[0] == 'F' ? BIG_FLOOR
                               : c->c[0] == 'R' ? BIG_ROUND
                                                : BIG_CEIL);
            if (k == 1) {
                big_int_set(q, &xs);
                big_int_set(r, &ys);
            }
            got = join2(hx(k == 0 ? q : &xs), hx(k == 0 ? r : &ys));
            if (k == 1) {
                /* aliasing run: q was x and r was y */
                BigInt qq = {0}, rr = {0};
                parse(&qq, c->a);
                parse(&rr, c->b);
                if (op[3] == 'r')
                    big_int_quo_rem(&qq, &qq, &rr, &rr);
                else if (op[3] == 'm')
                    big_int_div_mod(&qq, &qq, &rr, &rr);
                else
                    big_int_divide(&qq, &qq, &rr, &rr,
                                   c->c[0] == 'T'   ? BIG_TRUNC
                                   : c->c[0] == 'F' ? BIG_FLOOR
                                   : c->c[0] == 'R' ? BIG_ROUND
                                                    : BIG_CEIL);
                got = join2(hx(&qq), hx(&rr));
                big_int_free(&qq);
                big_int_free(&rr);
                if (op[3] == 'm')
                    got = NULL; /* DivMod with m aliasing y is not Go's either */
            }
            if (got != NULL && !same(got, c->want))
                testing_t_errorf_v(t, "%s%s(%s, %s) run %d = %s, want %s", op, c->c,
                                   c->a, c->b, k, got, c->want);
            big_int_free(&xs);
            big_int_free(&ys);
        }
        got = NULL;
    } else if (strcmp(op, "cmp") == 0 || strcmp(op, "cmpabs") == 0) {
        parse(&x, c->a);
        parse(&y, c->b);
        Int r = op[3] == 'a' ? big_int_cmp_abs(&x, &y) : big_int_cmp(&x, &y);
        got = r < 0 ? "-1" : r > 0 ? "1" : "0";
    } else if (strcmp(op, "lsh") == 0 || strcmp(op, "rsh") == 0) {
        parse(&x, c->a);
        Uint s = (Uint)strtoul(c->b, NULL, 10);
        if (op[0] == 'l')
            big_int_lsh(&z, &x, s);
        else
            big_int_rsh(&z, &x, s);
        got = hx(&z);
        if (op[0] == 'l')
            big_int_lsh(&x, &x, s);
        else
            big_int_rsh(&x, &x, s);
        if (!same(hx(&x), c->want))
            testing_t_errorf_v(t, "%s(%s, %s) into x = %s, want %s", op, c->a, c->b,
                               hx(&x), c->want);
    } else if (strcmp(op, "bitlen") == 0) {
        parse(&x, c->a);
        got = cs(fmt_sprintf_v(a, "%d", big_int_bit_len(&x)));
    } else if (strcmp(op, "tzb") == 0) {
        parse(&x, c->a);
        got = cs(fmt_sprintf_v(a, "%d", big_int_trailing_zero_bits(&x)));
    } else if (strcmp(op, "bit") == 0) {
        parse(&x, c->a);
        got = cs(fmt_sprintf_v(a, "%d", big_int_bit(&x, (Int)strtol(c->b, NULL, 10))));
    } else if (strcmp(op, "setbit") == 0) {
        parse(&x, c->a);
        Int i = (Int)strtol(c->b, NULL, 10);
        Uint b = (Uint)strtoul(c->c, NULL, 10);
        big_int_set_bit(&z, &x, i, b);
        got = hx(&z);
        big_int_set_bit(&x, &x, i, b);
        if (!same(hx(&x), c->want))
            testing_t_errorf_v(t, "setbit(%s) into x = %s, want %s", c->a, hx(&x),
                               c->want);
    } else if (strcmp(op, "exp") == 0) {
        parse(&x, c->a);
        parse(&y, c->b);
        const BigInt *mp = NULL;
        if (c->c[0] != '\0') {
            parse(&m, c->c);
            mp = &m;
        }
        got = hx(big_int_exp(&z, &x, &y, mp));
        if (mp != NULL) {
            /* the result into the modulus */
            BigInt m2 = {0};
            parse(&m2, c->c);
            BigInt *r = big_int_exp(&m2, &x, &y, &m2);
            if (!same(hx(r), c->want))
                testing_t_errorf_v(t, "exp(%s, %s, %s) into m = %s, want %s", c->a,
                                   c->b, c->c, hx(r), c->want);
            big_int_free(&m2);
        }
    } else if (strcmp(op, "gcd") == 0) {
        parse(&x, c->a);
        parse(&y, c->b);
        BigInt gx = {0}, gy = {0};
        big_int_gcd(&z, &gx, &gy, &x, &y);
        got = join3(hx(&z), hx(&gx), hx(&gy));
        /* and with x and y in the operands' places */
        BigInt ga = {0}, gb = {0};
        parse(&ga, c->a);
        parse(&gb, c->b);
        big_int_gcd(&m, &ga, &gb, &ga, &gb);
        const char *got2 = join3(hx(&m), hx(&ga), hx(&gb));
        if (!same(got2, c->want))
            testing_t_errorf_v(t, "gcd(%s, %s) aliased = %s, want %s", c->a, c->b, got2,
                               c->want);
        big_int_free(&gx);
        big_int_free(&gy);
        big_int_free(&ga);
        big_int_free(&gb);
    } else if (strcmp(op, "gcdz") == 0) {
        parse(&x, c->a);
        parse(&y, c->b);
        got = hx(big_int_gcd(&z, NULL, NULL, &x, &y));
    } else if (strcmp(op, "modinv") == 0) {
        parse(&x, c->a);
        parse(&y, c->b);
        got = hx(big_int_mod_inverse(&z, &x, &y));
    } else if (strcmp(op, "jacobi") == 0) {
        parse(&x, c->a);
        parse(&y, c->b);
        got = cs(fmt_sprintf_v(a, "%d", big_jacobi(&x, &y)));
    } else if (strcmp(op, "modsqrt") == 0) {
        parse(&x, c->a);
        parse(&y, c->b);
        got = hx(big_int_mod_sqrt(&z, &x, &y));
    } else if (strcmp(op, "prime") == 0) {
        parse(&x, c->a);
        got = big_int_probably_prime(&x, dec(c->b)) ? "true" : "false";
    } else if (strcmp(op, "text") == 0) {
        parse(&x, c->a);
        Int base = dec(c->b);
        got = cs(big_int_text(&x, a, base));
        Slice buf = slice_from_str(a, S("<>"));
        buf = big_int_append(&x, a, buf, base);
        const char *app = cs((Str){buf.p, buf.len});
        if (strncmp(app, "<>", 2) != 0 || !same(app + 2, c->want))
            testing_t_errorf_v(t, "append(%s, %d) = %s, want <>%s", c->a, base, app,
                               c->want);
    } else if (strcmp(op, "setstring") == 0) {
        bool ok;
        BigInt *r = big_int_set_string(&z, S(c->a), dec(c->b), &ok);
        got = ok ? hx(r) : "nil";
        if (ok != (r != NULL))
            testing_t_errorf_v(t, "setstring(%q): ok and the result disagree", c->a);
    } else if (strcmp(op, "format") == 0) {
        parse(&x, c->a);
        Any args[] = {BURROW_ANY(TYPE_BIG_INT, &x)};
        got = cs(fmt_sprintf(a, S(c->b), slice_from(args, 1, 1, TYPE_ANY)));
    } else if (strcmp(op, "scan") == 0) {
        Any args[] = {BURROW_ANY(TYPE_BIG_INT, &z)};
        Error err;
        fmt_sscanf(a, S(c->a), S(c->b), slice_from(args, 1, 1, TYPE_ANY), &err);
        if (BURROW_FAILED(err))
            got = cs(fmt_sprintf_v(a, "err:%s", error_text(err)));
        else
            got = hx(&z);
    } else if (strcmp(op, "float64") == 0) {
        parse(&x, c->a);
        BigAccuracy acc;
        double f = big_int_float64(&x, &acc);
        uint64_t bits;
        memcpy(&bits, &f, sizeof bits);
        char buf[64];
        snprintf(buf, sizeof buf, "%016llx %s", (unsigned long long)bits,
                 cs(big_accuracy_string(acc, a)));
        got = cs(S(buf));
    } else if (strcmp(op, "int64") == 0) {
        parse(&x, c->a);
        char buf[96];
        snprintf(buf, sizeof buf, "%lld %llu %s %s", (long long)big_int_int64(&x),
                 (unsigned long long)big_int_uint64(&x),
                 big_int_is_int64(&x) ? "true" : "false",
                 big_int_is_uint64(&x) ? "true" : "false");
        got = cs(S(buf));
    } else if (strcmp(op, "bytes") == 0) {
        parse(&x, c->a);
        got = hexbytes(big_int_bytes(&x, a));
    } else if (strcmp(op, "gob") == 0) {
        parse(&x, c->a);
        got = hexbytes(big_int_gob_encode(&x, a, NULL));
        /* and back */
        big_int_gob_decode(&z, big_int_gob_encode(&x, a, NULL));
        if (big_int_cmp(&z, &x) != 0)
            testing_t_errorf_v(t, "gob round trip of %s gave %s", c->a, hx(&z));
    } else if (strcmp(op, "fillbytes") == 0) {
        parse(&x, c->a);
        Int n = dec(c->b);
        got = hexbytes(big_int_fill_bytes(&x, slice_make(a, TYPE_BYTE, n, n)));
    } else if (strcmp(op, "setbytes") == 0) {
        got = hx(big_int_set_bytes(&z, unhex(c->a)));
    } else if (strcmp(op, "json") == 0) {
        parse(&x, c->a);
        Slice j = big_int_marshal_json(&x, a, NULL);
        got = cs((Str){j.p, j.len});
    } else if (strcmp(op, "unmarshaltext") == 0 || strcmp(op, "unmarshaljson") == 0) {
        Slice text = slice_from_str(a, S(c->a));
        Error err;
        if (op[9] == 't') {
            err = big_int_unmarshal_text(&z, text);
        } else {
            big_int_set_int64(&z, 77);
            err = big_int_unmarshal_json(&z, text);
        }
        got = BURROW_FAILED(err) ? cs(fmt_sprintf_v(a, "err:%s", error_text(err)))
                                 : hx(&z);
    } else if (strcmp(op, "gobdecode") == 0) {
        big_int_set_int64(&z, 5);
        Error err = big_int_gob_decode(&z, unhex(c->a));
        got = BURROW_FAILED(err) ? cs(fmt_sprintf_v(a, "err:%s", error_text(err)))
                                 : hx(&z);
    } else if (strcmp(op, "binomial") == 0) {
        got =
            hx(big_int_binomial(&z, strtoll(c->a, NULL, 10), strtoll(c->b, NULL, 10)));
    } else if (strcmp(op, "mulrange") == 0) {
        got =
            hx(big_int_mul_range(&z, strtoll(c->a, NULL, 10), strtoll(c->b, NULL, 10)));
    } else if (strcmp(op, "rand") == 0) {
        parse(&x, c->a);
        MathRandRand *r =
            math_rand_new(a, math_rand_new_source(a, strtoll(c->b, NULL, 10)));
        got = hx(big_int_rand(&z, r, &x));
    } else {
        testing_t_errorf_v(t, "unknown op %s", op);
    }
    if (got != NULL && !same(got, c->want))
        testing_t_errorf_v(t, "%s(%q, %q, %q) = %q, want %q", op, c->a, c->b, c->c, got,
                           c->want);
    big_int_free(&x);
    big_int_free(&y);
    big_int_free(&z);
    big_int_free(&m);
}

static void TestGoCases(TestingT *t) {
    Int n = (Int)(sizeof big_cases / sizeof big_cases[0]);
    for (Int i = 0; i < n; i++) {
        check_case(t, &big_cases[i]);
        arena_reset(&ar);
    }
}

/* ------------------------------------------------------- burrow's own API */

static void TestZeroValues(TestingT *t) {
    BigInt x = {0};
    CHECK(big_int_sign(&x) == 0);
    CHECK_STR_EQ(cs(big_int_string(&x, a)), "0");
    BigInt y = BIG_INT(a);
    big_int_add(&y, &x, &x);
    CHECK_STR_EQ(cs(big_int_string(&y, a)), "0");
    CHECK_STR_EQ(cs(big_int_text(NULL, a, 10)), "<nil>");
    big_int_free(&x);
    big_int_free(NULL);
}

static void TestDocExample(TestingT *t) {
    BigInt x = {0}, y = {0};
    big_int_set_string(&x, BURROW_S("123456789012345678901234567890"), 10, NULL);
    big_int_mul(&y, &x, &x);
    big_int_exp(&y, &y, big_new_int(a, 3), NULL);
    CHECK_STR_EQ(
        cs(big_int_string(&y, a)),
        "3540705970274021332875685499624548306450060730882215041340919342277215"
        "7756535080066728356702754804887056732422499817077065394570337395298570"
        "49458574415437768675412790761000000");
    big_int_free(&x);
    big_int_free(&y);
}

/* Go's TestFibo-like walk: a result kept in the receiver's own memory, so it
 * survives the scratch memory being reused by the next call. */
static void TestResultsOutliveScratch(TestingT *t) {
    BigInt f[3] = {{0}, {0}, {0}};
    big_int_set_int64(&f[0], 0);
    big_int_set_int64(&f[1], 1);
    for (int i = 2; i <= 1000; i++)
        big_int_add(&f[i % 3], &f[(i - 1) % 3], &f[(i - 2) % 3]);
    Str s = big_int_string(&f[1000 % 3], a);
    CHECK_INT_EQ(s.len, 209);
    CHECK_STR_EQ(cs((Str){s.p, 12}), "434665576869");
    for (int i = 0; i < 3; i++)
        big_int_free(&f[i]);
}

static void TestBits(TestingT *t) {
    BigInt x = {0};
    BigWord w[] = {3, 0, 0};
    big_int_set_bits(&x, slice_from(w, 3, 3, TYPE_UINT));
    CHECK_INT_EQ(big_int_bits(&x).len, 1);
    CHECK_STR_EQ(cs(big_int_string(&x, a)), "3");
    w[0] = 5; /* SetBits copies */
    CHECK_STR_EQ(cs(big_int_string(&x, a)), "3");
    big_int_free(&x);
}

static void TestStrings(TestingT *t) {
    CHECK_STR_EQ(cs(big_accuracy_string(BIG_BELOW, a)), "Below");
    CHECK_STR_EQ(cs(big_accuracy_string(BIG_EXACT, a)), "Exact");
    CHECK_STR_EQ(cs(big_accuracy_string(BIG_ABOVE, a)), "Above");
    CHECK_STR_EQ(cs(big_accuracy_string(7, a)), "Accuracy(7)");
    CHECK_STR_EQ(cs(big_rounding_mode_string(BIG_TO_NEAREST_EVEN, a)), "ToNearestEven");
    CHECK_STR_EQ(cs(big_rounding_mode_string(BIG_TO_POSITIVE_INF, a)), "ToPositiveInf");
    CHECK_STR_EQ(cs(big_rounding_mode_string(9, a)), "RoundingMode(9)");
}

/* The Ints in these are on the arena, because a panic leaves no chance to free
 * them. */
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

static void div_zero(void) {
    BigInt x = BIG_INT(a), y = BIG_INT(a), z = BIG_INT(a);
    big_int_set_int64(&x, 1);
    big_int_quo(&z, &x, &y);
}

static void sqrt_neg(void) {
    BigInt x = BIG_INT(a), z = BIG_INT(a);
    big_int_set_int64(&x, -1);
    big_int_sqrt(&z, &x);
}

static void jacobi_even(void) {
    BigInt x = BIG_INT(a), y = BIG_INT(a);
    big_int_set_int64(&y, 10);
    big_jacobi(&x, &y);
}

static void bit_neg(void) {
    BigInt x = BIG_INT(a);
    big_int_bit(&x, -1);
}

static void fill_small(void) {
    BigInt x = BIG_INT(a);
    Byte b[1];
    big_int_set_int64(&x, 1000);
    big_int_fill_bytes(&x, slice_from(b, 1, 1, TYPE_BYTE));
}

static void TestPanics(TestingT *t) {
    CHECK_STR_EQ(panic_of(div_zero), "division by zero");
    CHECK_STR_EQ(panic_of(sqrt_neg), "square root of negative number");
    CHECK_STR_EQ(
        panic_of(jacobi_even),
        "big: invalid 2nd argument to Int.Jacobi: need odd integer but got 10");
    CHECK_STR_EQ(panic_of(bit_neg), "negative bit index");
    CHECK_STR_EQ(panic_of(fill_small), "math/big: buffer too small to fit value");
    /* and the thread's scratch memory is fine afterwards */
    BigInt x = {0};
    big_int_mul_range(&x, 1, 30);
    CHECK_STR_EQ(cs(big_int_string(&x, a)), "265252859812191058636308480000000");
    big_int_free(&x);
}

/* The scratch memory is per thread: every thread computes the same
 * factorial at once. */
static void factorial_worker(void *arg) {
    Str *out = arg;
    BigInt x = {0};
    big_int_mul_range(&x, 1, 2000);
    BigInt y = {0};
    big_int_sqrt(&y, &x);
    *out = big_int_string(&y, heap_allocator());
    big_int_free(&x);
    big_int_free(&y);
}

static void TestThreads(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);

    enum { N = 4 };
    burrow__Thread th[N];
    Str out[N];
    for (int i = 0; i < N; i++)
        CHECK(burrow__thread_start(&th[i], factorial_worker, &out[i], 0));
    for (int i = 0; i < N; i++)
        CHECK(burrow__thread_join(&th[i]));
    for (int i = 1; i < N; i++)
        CHECK(str_eq(out[i], out[0]));
    CHECK(out[0].len > 2000);
    for (int i = 0; i < N; i++)
        mem_free(heap_allocator(), (void *)(uintptr_t)out[i].p, (size_t)out[i].len, 1);
}

static void setup(void) {
    arena_init(&ar, NULL, 1 << 16);
    a = arena_allocator(&ar);
}

#define TESTS(X)                                                                       \
    X(TestGoCases)                                                                     \
    X(TestZeroValues)                                                                  \
    X(TestDocExample)                                                                  \
    X(TestResultsOutliveScratch)                                                       \
    X(TestBits)                                                                        \
    X(TestStrings)                                                                     \
    X(TestPanics)                                                                      \
    X(TestThreads)

static int big_main(TestingM *m) {
    setup();
    int r = testing_m_run(m);
    arena_free(&ar);
    return r;
}

TESTING_MAIN_WITH(big_main, TESTS)

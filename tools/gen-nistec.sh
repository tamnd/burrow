#!/bin/sh
# Regenerates src/crypto/nistec_p224.c, nistec_p384.c and nistec_p521.c, and
# the two headers nistec_p256.c includes, nistec_p256_point.h and
# nistec_p256_table.h.
#
# Go generates its p224.go, p384.go and p521.go from one template in
# crypto/internal/fips140/nistec/generate.go, and its purego p256.go starts
# with the same code before it goes its own way. This does the same thing in
# C. The template is below, written once from Go's, and what differs between
# the curves is read out of Go's tree: the generator and b from each pNNN.go,
# the addition chains for inversion from fiat/pNNN_invert.go and for square
# roots from pNNN.go and p224_sqrt.go, and P-256's table of multiples of the
# generator from p256_table.go. The chains are Go statements of three shapes,
# x.Square(y), x.Mul(y, z) and a counted loop, and are translated line by
# line.
#
# Go's field elements are fiat-crypto's Montgomery form for all four curves.
# P-521's here is fiat-crypto's unsaturated Solinas form instead, as
# tools/gen-fiat-nistec.sh explains, so its element functions differ from the
# others'. Everything above them is the same.
#
# The Go on PATH should be the release the port follows. Only its source tree
# is read; nothing is built. The output is formatted with clang-format, or
# with $CLANG_FORMAT, which should be the release CI pins.
#
# Copyright 2026 The burrow Authors. All rights reserved.
# Use of this source code is governed by a BSD-style licence that can be found
# in the LICENSE file.
set -eu

cd "$(dirname "$0")/.."

GOROOT=${GOROOT:-$(go env GOROOT)}
GOVERSION=${GOVERSION:-$(go env GOVERSION)}
export GOROOT GOVERSION

exec python3 - <<'PY'
import os
import re
import shutil
import subprocess
import sys
import textwrap

src = os.path.join(os.environ["GOROOT"], "src/crypto/internal/fips140/nistec")
version = os.environ["GOVERSION"]


def read(name):
    with open(os.path.join(src, name)) as f:
        return f.read()


def go_bytes(text):
    """The bytes of a []byte{...} literal."""
    return [int(v, 0) for v in text.split(",") if v.strip()]


def c_bytes(bs, indent):
    """bs as the body of a C array, twelve to a line."""
    lines = []
    for i in range(0, len(bs), 12):
        lines.append(indent + ", ".join("0x%02x" % b for b in bs[i : i + 12]) + ",")
    return "\n".join(lines)


def chain(body, n, square_n=False):
    """Translates the Go statements of an addition chain into C. Comments come
    across as one block comment, and the variables Go declares with new become
    elements on the stack with pointers of the same names."""
    out = []
    comment = []
    decls = []
    depth = 1
    for raw in body.splitlines():
        line = raw.strip()
        if not line:
            continue
        m = re.match(r"//\s?(.*)$", line)
        if m:
            comment.append(m.group(1).replace("\t", "    ").rstrip())
            continue
        if comment:
            while comment and not comment[-1]:
                comment.pop()
            text = "\n".join(
                ("    " * depth + " * " + c).rstrip() if i else "    " * depth + "/* " + c
                for i, c in enumerate(comment)
            )
            out.append(text + " */")
            comment = []
        ind = "    " * depth
        m = re.match(r"var (\w+) = new\(\w+(?:\.\w+)?\)(?:\.Set\(e\))?$", line)
        if m:
            decls.append(m.group(1))
            continue
        m = re.match(r"(\w+), (\w+) := new\(\w+\.\w+\), new\(\w+\.\w+\)$", line)
        if m:
            decls.extend([m.group(1), m.group(2)])
            continue
        m = re.match(r"(\w+)\.Square\((\w+)\)$", line)
        if m:
            out.append(ind + "p%s_square(%s, %s);" % (n, m.group(1), m.group(2)))
            continue
        m = re.match(r"(\w+)\.Mul\((\w+), (\w+)\)$", line)
        if m:
            out.append(ind + "p%s_mul(%s, %s, %s);" % (n, m.group(1), m.group(2), m.group(3)))
            continue
        m = re.match(r"p256Square\((\w+), (\w+), (\d+)\)$", line)
        if m and square_n:
            out.append(ind + "p256_square_n(%s, %s, %s);" % m.groups())
            continue
        m = re.match(r"for s := (\d+); s < (\d+); s\+\+ \{$", line)
        if m:
            out.append(ind + "for (int s = %s; s < %s; s++)" % m.groups())
            depth += 1
            continue
        if line == "}":
            depth -= 1
            continue
        m = re.match(r"return e\.Set\(z\)$", line)
        if m:
            continue
        sys.exit("gen-nistec: cannot translate %r" % line)
    if depth != 1:
        sys.exit("gen-nistec: unbalanced braces in a chain")
    head = []
    if decls:
        head.append("    NistecP%sElement %s;" % (n, ", ".join(d + "_" for d in decls)))
        head.append(
            "    NistecP%sElement %s;" % (n, ", ".join("*%s = &%s_" % (d, d) for d in decls))
        )
    return "\n".join(head + out)


def between(text, start, end):
    i = text.index(start) + len(start)
    j = text.index(end, i)
    return text[i:j]


def curve(n):
    go = read("p%s.go" % n)
    m = re.search(r"const p%sElementLength = (\d+)" % n, go)
    elen = int(m.group(1))
    gen = re.findall(r"p\.[xy]\.SetBytes\(\[\]byte\{([^}]*)\}\)", go)
    b = re.search(r"SetBytes\(\[\]byte\{([^}]*)\}\)\n\t\}\)\n\treturn _p%sB" % n, go)
    inv = read("fiat/p%s_invert.go" % n)
    inv = between(inv, "*P%sElement {\n" % n, "\n}")
    c = {
        "n": n,
        "elen": elen,
        "gx": go_bytes(gen[0]),
        "gy": go_bytes(gen[1]),
        "b": go_bytes(b.group(1)),
        "invert": chain(inv, n),
    }
    if n == "224":
        sq = read("p224_sqrt.go")
        c["sqrt"] = chain(between(sq, "\t// Compute x^(2^127-1) first.\n\t//\n", "\n\t// v = x^"), n)
        c["gg"] = go_bytes(between(sq, "SetBytes([]byte{", "})"))
    elif n == "256":
        c["sqrt"] = chain(
            between(go, "func p256Sqrt(e, x *fiat.P256Element) (isSquare bool) {\n",
                    "\n\n\t// Check if the candidate"),
            n,
            square_n=True,
        )
    else:
        c["sqrt"] = chain(
            between(go, "func p%sSqrtCandidate(z, x *fiat.P%sElement) {\n" % (n, n), "\n}"),
            n,
        )
    for k in ("gx", "gy", "b"):
        if len(c[k]) != elen:
            sys.exit("gen-nistec: P-%s %s is %d bytes" % (n, k, len(c[k])))
    return c


HEAD = """\
/* Derived from Go's src/crypto/internal/fips140/nistec/p{n}.go.
 * Go source: {version}.
 *
 * {what}
 *
 * {generated}
 *
 * Copyright 2022 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */
"""

INCLUDES = """\
#include "nistec.h"

#include "fiat_p{n}_32.h"
#include "fiat_p{n}_64.h"

#include "burrow/core.h"
#include "burrow/crypto/subtle.h"
#include "burrow/error.h"
#include "burrow/func.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/sync.h"

#include <stdint.h>
#include <string.h>

_Static_assert(FIAT_P{n}_LIMBS == NISTEC_P{n}_LIMBS,
               "nistec.h has the wrong number of words for a P-{n} element");
_Static_assert(sizeof(fiat_p{n}_limb) == sizeof(NistecLimb),
               "nistec.h has the wrong word for a P-{n} element");

enum {{ p{n}_element_length = {elen} }};

static void p{n}_reverse(uint8_t *v) {{
    for (int i = 0; i < p{n}_element_length / 2; i++) {{
        uint8_t t = v[i];
        v[i] = v[p{n}_element_length - 1 - i];
        v[p{n}_element_length - 1 - i] = t;
    }}
}}
"""

# fiat/pNNN.go, for the three Montgomery fields.
ELEMENT_MONTGOMERY = """
/* ------------------------------------------------------------- element */

static NistecP{n}Element *p{n}_one(NistecP{n}Element *e) {{
    fiat_p{n}_set_one(e->l);
    return e;
}}

static NistecP{n}Element *p{n}_add(NistecP{n}Element *e, const NistecP{n}Element *t1,
                                 const NistecP{n}Element *t2) {{
    fiat_p{n}_add(e->l, t1->l, t2->l);
    return e;
}}

static NistecP{n}Element *p{n}_sub(NistecP{n}Element *e, const NistecP{n}Element *t1,
                                 const NistecP{n}Element *t2) {{
    fiat_p{n}_sub(e->l, t1->l, t2->l);
    return e;
}}

static NistecP{n}Element *p{n}_mul(NistecP{n}Element *e, const NistecP{n}Element *t1,
                                 const NistecP{n}Element *t2) {{
    fiat_p{n}_mul(e->l, t1->l, t2->l);
    return e;
}}

static NistecP{n}Element *p{n}_square(NistecP{n}Element *e, const NistecP{n}Element *t) {{
    fiat_p{n}_square(e->l, t->l);
    return e;
}}

/* Writes the {elen} byte big endian encoding of e to out. */
static void p{n}_bytes(const NistecP{n}Element *e, uint8_t *out) {{
    fiat_p{n}_non_montgomery_domain_field_element tmp;
    fiat_p{n}_from_montgomery(tmp, e->l);
    fiat_p{n}_to_bytes(out, tmp);
    p{n}_reverse(out);
}}

static void p{n}_from_canonical_bytes(NistecP{n}Element *e, const uint8_t *in) {{
    fiat_p{n}_non_montgomery_domain_field_element tmp;
    fiat_p{n}_from_bytes(tmp, in);
    fiat_p{n}_to_montgomery(e->l, tmp);
}}
"""

# fiat-crypto's unsaturated Solinas P-521. Sums and differences come out of
# fiat_p521_add and fiat_p521_sub loose, and get carried back to tight, the
# form everything else takes and every element is kept in. A tight element is
# a loose one too, so the multiplications take them as they are.
ELEMENT_SOLINAS = """
/* ------------------------------------------------------------- element */

static NistecP{n}Element *p{n}_one(NistecP{n}Element *e) {{
    memset(e, 0, sizeof *e);
    e->l[0] = 1;
    return e;
}}

static NistecP{n}Element *p{n}_add(NistecP{n}Element *e, const NistecP{n}Element *t1,
                                 const NistecP{n}Element *t2) {{
    fiat_p{n}_loose_field_element loose;
    fiat_p{n}_add(loose, t1->l, t2->l);
    fiat_p{n}_carry(e->l, loose);
    return e;
}}

static NistecP{n}Element *p{n}_sub(NistecP{n}Element *e, const NistecP{n}Element *t1,
                                 const NistecP{n}Element *t2) {{
    fiat_p{n}_loose_field_element loose;
    fiat_p{n}_sub(loose, t1->l, t2->l);
    fiat_p{n}_carry(e->l, loose);
    return e;
}}

static NistecP{n}Element *p{n}_mul(NistecP{n}Element *e, const NistecP{n}Element *t1,
                                 const NistecP{n}Element *t2) {{
    fiat_p{n}_carry_mul(e->l, t1->l, t2->l);
    return e;
}}

static NistecP{n}Element *p{n}_square(NistecP{n}Element *e, const NistecP{n}Element *t) {{
    fiat_p{n}_carry_square(e->l, t->l);
    return e;
}}

/* Writes the {elen} byte big endian encoding of e to out. */
static void p{n}_bytes(const NistecP{n}Element *e, uint8_t *out) {{
    fiat_p{n}_to_bytes(out, e->l);
    p{n}_reverse(out);
}}

static void p{n}_from_canonical_bytes(NistecP{n}Element *e, const uint8_t *in) {{
    fiat_p{n}_from_bytes(e->l, in);
}}
"""

ELEMENT_COMMON = """
static int p{n}_equal(const NistecP{n}Element *e, const NistecP{n}Element *t) {{
    uint8_t eb[p{n}_element_length], tb[p{n}_element_length];
    p{n}_bytes(e, eb);
    p{n}_bytes(t, tb);
    return burrow__nistec_equal_bytes(eb, tb, p{n}_element_length);
}}

static int p{n}_is_zero(const NistecP{n}Element *e) {{
    uint8_t eb[p{n}_element_length], zero[p{n}_element_length];
    memset(zero, 0, sizeof zero);
    p{n}_bytes(e, eb);
    return burrow__nistec_equal_bytes(eb, zero, p{n}_element_length);
}}

/* Sets e to the value of the {elen} big endian bytes at v. Go's SetBytes also
 * takes a slice of the wrong length, which nothing here passes it. */
static NistecP{n}Element *p{n}_set_bytes(NistecP{n}Element *e, const uint8_t *v,
                                       Error *err) {{
    /* Check for non-canonical encodings (p + k, 2p + k, etc.) by comparing to
     * the encoding of -1 mod p, so p - 1, the highest canonical encoding. */
    NistecP{n}Element zero, one, minus_one;
    uint8_t minus_one_encoding[p{n}_element_length];
    memset(&zero, 0, sizeof zero);
    p{n}_bytes(p{n}_sub(&minus_one, &zero, p{n}_one(&one)), minus_one_encoding);
    if (burrow__nistec_less_or_eq_bytes(v, minus_one_encoding, p{n}_element_length) ==
        0) {{
        BURROW_OUT(err,
                   errors_new(error_allocator(), BURROW_S("invalid P{n}Element encoding")));
        return NULL;
    }}
    uint8_t in[p{n}_element_length];
    memcpy(in, v, sizeof in);
    p{n}_reverse(in);
    p{n}_from_canonical_bytes(e, in);
    return e;
}}

/* Sets v to a if cond is 1 and to b if cond is 0. */
static NistecP{n}Element *p{n}_select(NistecP{n}Element *v, const NistecP{n}Element *a,
                                    const NistecP{n}Element *b, int cond) {{
    fiat_p{n}_selectznz(v->l, (fiat_p{n}_uint1)cond, b->l, a->l);
    return v;
}}

/* Sets e to 1/x and returns e. If x is 0, the result is 0. Inversion is
 * exponentiation by p - 2. */
static NistecP{n}Element *p{n}_invert(NistecP{n}Element *e, const NistecP{n}Element *x) {{
{invert}

    *e = *z;
    return e;
}}
"""

SQRT_CANDIDATE = """
/* Sets z to a square root candidate for x. z and x must not overlap. */
static void p{n}_sqrt_candidate(NistecP{n}Element *z, const NistecP{n}Element *x) {{
{sqrt}
}}

/* Sets e to a square root of x. If x is not a square, returns 0 and leaves e
 * unchanged. e and x can overlap. */
static int p{n}_sqrt(NistecP{n}Element *e, const NistecP{n}Element *x) {{
    NistecP{n}Element candidate, square;
    p{n}_sqrt_candidate(&candidate, x);
    p{n}_square(&square, &candidate);
    if (p{n}_equal(&square, x) != 1)
        return 0;
    *e = candidate;
    return 1;
}}
"""

# p224_sqrt.go. p = 1 mod 4, so no exponentiation gives a square root.
SQRT_224 = """
/* GG[j] = g^(2^j) for j from 0 to 95, where g = 11^q, built the first time a
 * square root needs it. */
static NistecP224Element p224_gg[96];
static SyncOnce p224_gg_once;

static void p224_gg_init(void *env) {{
    (void)env;
    static const uint8_t g[p224_element_length] = {{
{gg}
    }};
    p224_set_bytes(&p224_gg[0], g, NULL);
    for (int i = 1; i < 96; i++)
        p224_square(&p224_gg[i], &p224_gg[i - 1]);
}}

/* Sets r to a square root candidate for x. r and x must not overlap. */
static void p224_sqrt_candidate(NistecP224Element *r, const NistecP224Element *x) {{
    /* Since p = 1 mod 4, we can't use the exponentiation by (p + 1) / 4 like
     * for the other primes. Instead, implement a variation of Tonelli-Shanks.
     * The constant-time implementation is adapted from Thomas Pornin's ecGFp5.
     *
     * https://github.com/pornin/ecgfp5/blob/82325b965/rust/src/field.rs#L337-L385
     *
     * p = q*2^n + 1 with q odd -> q = 2^128 - 1 and n = 96
     * g^(2^n) = 1 -> g = 11 ^ q (where 11 is the smallest non-square)
     * GG[j] = g^(2^j) for j = 0 to n-1 */
    sync_once_do(&p224_gg_once, BURROW_FN(Func, p224_gg_init, NULL));

    /* r <- x^((q+1)/2) = x^(2^127)
     * v <- x^q = x^(2^128-1)
     *
     * Compute x^(2^127-1) first. */
{sqrt}

    /* v = x^(2^127-1)^2 * x */
    NistecP224Element v;
    p224_square(&v, r);
    p224_mul(&v, &v, x);

    /* r = x^(2^127-1) * x */
    p224_mul(r, r, x);

    /* for i = n-1 down to 1:
     *     w = v^(2^(i-1))
     *     if w == -1 then:
     *         v <- v*GG[n-i]
     *         r <- r*GG[n-i-1] */
    NistecP224Element zero, one, minus_one;
    memset(&zero, 0, sizeof zero);
    p224_sub(&minus_one, &zero, p224_one(&one));

    for (int i = 96 - 1; i >= 1; i--) {{
        NistecP224Element w = v;
        for (int j = 0; j < i - 1; j++)
            p224_square(&w, &w);
        int cond = p224_equal(&w, &minus_one);
        p224_select(&v, p224_mul(t0, &v, &p224_gg[96 - i]), &v, cond);
        p224_select(r, p224_mul(t0, r, &p224_gg[96 - i - 1]), r, cond);
    }}
}}

/* Sets e to a square root of x. If x is not a square, returns 0 and leaves e
 * unchanged. e and x can overlap. */
static int p224_sqrt(NistecP224Element *e, const NistecP224Element *x) {{
    NistecP224Element candidate, square;
    p224_sqrt_candidate(&candidate, x);
    p224_square(&square, &candidate);
    if (p224_equal(&square, x) != 1)
        return 0;
    *e = candidate;
    return 1;
}}
"""

# p256.go's p256Sqrt, which is written out rather than generated.
SQRT_256 = """
/* Sets e to the square of x, n times over, for n of at least 1. */
static void p256_square_n(NistecP256Element *e, const NistecP256Element *x, int n) {{
    p256_square(e, x);
    for (int i = 1; i < n; i++)
        p256_square(e, e);
}}

/* Sets e to a square root of x. If x is not a square, returns 0 and leaves e
 * unchanged. e and x can overlap. */
static int p256_sqrt(NistecP256Element *e, const NistecP256Element *x) {{
{sqrt}

    /* Check if the candidate t0 is indeed a square root of x. */
    p256_square(t1, t0);
    if (p256_equal(t1, x) != 1)
        return 0;
    *e = *t0;
    return 1;
}}
"""

POINT = """
/* --------------------------------------------------------------- point */

/* b, the constant of the curve, set the first time something needs it. */
static NistecP{n}Element p{n}_b_value;
static SyncOnce p{n}_b_once;

static void p{n}_b_init(void *env) {{
    (void)env;
    static const uint8_t b[p{n}_element_length] = {{
{b}
    }};
    p{n}_set_bytes(&p{n}_b_value, b, NULL);
}}

static const NistecP{n}Element *p{n}_b(void) {{
    sync_once_do(&p{n}_b_once, BURROW_FN(Func, p{n}_b_init, NULL));
    return &p{n}_b_value;
}}

/* Sets p to the point at infinity. */
static NistecP{n}Point *p{n}_point_new(NistecP{n}Point *p) {{
    memset(&p->x, 0, sizeof p->x);
    p{n}_one(&p->y);
    memset(&p->z, 0, sizeof p->z);
    return p;
}}

static NistecP{n}Point *p{n}_point_set_generator(NistecP{n}Point *p) {{
    static const uint8_t x[p{n}_element_length] = {{
{gx}
    }};
    static const uint8_t y[p{n}_element_length] = {{
{gy}
    }};
    p{n}_set_bytes(&p->x, x, NULL);
    p{n}_set_bytes(&p->y, y, NULL);
    p{n}_one(&p->z);
    return p;
}}

/* Sets y2 to x^3 - 3x + b, and returns y2. */
static NistecP{n}Element *p{n}_polynomial(NistecP{n}Element *y2,
                                        const NistecP{n}Element *x) {{
    p{n}_square(y2, x);
    p{n}_mul(y2, y2, x);

    NistecP{n}Element three_x;
    p{n}_add(&three_x, x, x);
    p{n}_add(&three_x, &three_x, x);
    p{n}_sub(y2, y2, &three_x);

    return p{n}_add(y2, y2, p{n}_b());
}}

static int p{n}_check_on_curve(const NistecP{n}Element *x, const NistecP{n}Element *y,
                              Error *err) {{
    /* y^2 = x^3 - 3x + b */
    NistecP{n}Element rhs, lhs;
    p{n}_polynomial(&rhs, x);
    p{n}_square(&lhs, y);
    if (p{n}_equal(&rhs, &lhs) != 1) {{
        BURROW_OUT(err, errors_new(error_allocator(), BURROW_S("P{n} point not on curve")));
        return 0;
    }}
    return 1;
}}

/* Sets p to the compressed, uncompressed, or infinity value encoded in b, as
 * specified in SEC 1, Version 2.0, Section 2.3.4. If the point is not on the
 * curve, it returns NULL and an error, and the receiver is unchanged.
 * Otherwise, it returns p. */
static NistecP{n}Point *p{n}_point_set_bytes(NistecP{n}Point *p, Slice b, Error *err) {{
    const uint8_t *bb = b.p;
    NistecP{n}Element x, y;

    /* Point at infinity. */
    if (b.len == 1 && bb[0] == 0)
        return p{n}_point_new(p);

    /* Uncompressed form. */
    if (b.len == 1 + 2 * p{n}_element_length && bb[0] == 4) {{
        if (p{n}_set_bytes(&x, bb + 1, err) == NULL)
            return NULL;
        if (p{n}_set_bytes(&y, bb + 1 + p{n}_element_length, err) == NULL)
            return NULL;
        if (!p{n}_check_on_curve(&x, &y, err))
            return NULL;
        p->x = x;
        p->y = y;
        p{n}_one(&p->z);
        return p;
    }}

    /* Compressed form. */
    if (b.len == 1 + p{n}_element_length && (bb[0] == 2 || bb[0] == 3)) {{
        if (p{n}_set_bytes(&x, bb + 1, err) == NULL)
            return NULL;

        /* y^2 = x^3 - 3x + b */
        p{n}_polynomial(&y, &x);
        if (!p{n}_sqrt(&y, &y)) {{
            BURROW_OUT(err, errors_new(error_allocator(),
                                       BURROW_S("invalid P{n} compressed point encoding")));
            return NULL;
        }}

        /* Select the positive or negative root, as indicated by the least
         * significant bit, based on the encoding type byte. */
        NistecP{n}Element other_root;
        memset(&other_root, 0, sizeof other_root);
        p{n}_sub(&other_root, &other_root, &y);
        uint8_t yb[p{n}_element_length];
        p{n}_bytes(&y, yb);
        int cond = (yb[p{n}_element_length - 1] & 1) ^ (bb[0] & 1);
        p{n}_select(&y, &other_root, &y, cond);

        p->x = x;
        p->y = y;
        p{n}_one(&p->z);
        return p;
    }}

    BURROW_OUT(err, errors_new(error_allocator(), BURROW_S("invalid P{n} point encoding")));
    return NULL;
}}

/* Writes the uncompressed or infinity encoding of p, as specified in SEC 1,
 * Version 2.0, Section 2.3.3, and returns its length. Note that the encoding
 * of the point at infinity is shorter than all other encodings. */
static Int p{n}_point_bytes(const NistecP{n}Point *p, uint8_t *out) {{
    /* The SEC 1 representation of the point at infinity is a single zero byte,
     * and only infinity has z = 0. */
    if (p{n}_is_zero(&p->z) == 1) {{
        out[0] = 0;
        return 1;
    }}

    NistecP{n}Element zinv, x, y;
    p{n}_invert(&zinv, &p->z);
    p{n}_mul(&x, &p->x, &zinv);
    p{n}_mul(&y, &p->y, &zinv);

    out[0] = 4;
    p{n}_bytes(&x, out + 1);
    p{n}_bytes(&y, out + 1 + p{n}_element_length);
    return 1 + 2 * p{n}_element_length;
}}

/* Writes the encoding of the x-coordinate of p, as specified in SEC 1, Version
 * 2.0, Section 2.3.5, or returns NULL and an error if p is the point at
 * infinity. */
static uint8_t *p{n}_point_bytes_x(const NistecP{n}Point *p, uint8_t *out, Error *err) {{
    if (p{n}_is_zero(&p->z) == 1) {{
        BURROW_OUT(err, errors_new(error_allocator(),
                                   BURROW_S("P{n} point is the point at infinity")));
        return NULL;
    }}

    NistecP{n}Element zinv, x;
    p{n}_invert(&zinv, &p->z);
    p{n}_mul(&x, &p->x, &zinv);

    p{n}_bytes(&x, out);
    return out;
}}

/* Writes the compressed or infinity encoding of p, as specified in SEC 1,
 * Version 2.0, Section 2.3.3, and returns its length. Note that the encoding
 * of the point at infinity is shorter than all other encodings. */
static Int p{n}_point_bytes_compressed(const NistecP{n}Point *p, uint8_t *out) {{
    if (p{n}_is_zero(&p->z) == 1) {{
        out[0] = 0;
        return 1;
    }}

    NistecP{n}Element zinv, x, y;
    p{n}_invert(&zinv, &p->z);
    p{n}_mul(&x, &p->x, &zinv);
    p{n}_mul(&y, &p->y, &zinv);

    /* Encode the sign of the y coordinate (indicated by the least significant
     * bit) as the encoding type (2 or 3). */
    uint8_t yb[p{n}_element_length];
    p{n}_bytes(&y, yb);
    out[0] = (uint8_t)(2 | (yb[p{n}_element_length - 1] & 1));
    p{n}_bytes(&x, out + 1);
    return 1 + p{n}_element_length;
}}

/* Sets q = p1 + p2, and returns q. The points may overlap. */
static NistecP{n}Point *p{n}_point_add(NistecP{n}Point *q, const NistecP{n}Point *p1,
                                     const NistecP{n}Point *p2) {{
    /* Complete addition formula for a = -3 from "Complete addition formulas for
     * prime order elliptic curves" (https://eprint.iacr.org/2015/1060), A.2. */

    NistecP{n}Element t0_, t1_, t2_, t3_, t4_, x3_, y3_, z3_;
    NistecP{n}Element *t0 = &t0_, *t1 = &t1_, *t2 = &t2_, *t3 = &t3_, *t4 = &t4_;
    NistecP{n}Element *x3 = &x3_, *y3 = &y3_, *z3 = &z3_;
    const NistecP{n}Element *b = p{n}_b();

    p{n}_mul(t0, &p1->x, &p2->x); /* t0 := X1 * X2 */
    p{n}_mul(t1, &p1->y, &p2->y); /* t1 := Y1 * Y2 */
    p{n}_mul(t2, &p1->z, &p2->z); /* t2 := Z1 * Z2 */
    p{n}_add(t3, &p1->x, &p1->y); /* t3 := X1 + Y1 */
    p{n}_add(t4, &p2->x, &p2->y); /* t4 := X2 + Y2 */
    p{n}_mul(t3, t3, t4);         /* t3 := t3 * t4 */
    p{n}_add(t4, t0, t1);         /* t4 := t0 + t1 */
    p{n}_sub(t3, t3, t4);         /* t3 := t3 - t4 */
    p{n}_add(t4, &p1->y, &p1->z); /* t4 := Y1 + Z1 */
    p{n}_add(x3, &p2->y, &p2->z); /* X3 := Y2 + Z2 */
    p{n}_mul(t4, t4, x3);         /* t4 := t4 * X3 */
    p{n}_add(x3, t1, t2);         /* X3 := t1 + t2 */
    p{n}_sub(t4, t4, x3);         /* t4 := t4 - X3 */
    p{n}_add(x3, &p1->x, &p1->z); /* X3 := X1 + Z1 */
    p{n}_add(y3, &p2->x, &p2->z); /* Y3 := X2 + Z2 */
    p{n}_mul(x3, x3, y3);         /* X3 := X3 * Y3 */
    p{n}_add(y3, t0, t2);         /* Y3 := t0 + t2 */
    p{n}_sub(y3, x3, y3);         /* Y3 := X3 - Y3 */
    p{n}_mul(z3, b, t2);          /* Z3 := b * t2 */
    p{n}_sub(x3, y3, z3);         /* X3 := Y3 - Z3 */
    p{n}_add(z3, x3, x3);         /* Z3 := X3 + X3 */
    p{n}_add(x3, x3, z3);         /* X3 := X3 + Z3 */
    p{n}_sub(z3, t1, x3);         /* Z3 := t1 - X3 */
    p{n}_add(x3, t1, x3);         /* X3 := t1 + X3 */
    p{n}_mul(y3, b, y3);          /* Y3 := b * Y3 */
    p{n}_add(t1, t2, t2);         /* t1 := t2 + t2 */
    p{n}_add(t2, t1, t2);         /* t2 := t1 + t2 */
    p{n}_sub(y3, y3, t2);         /* Y3 := Y3 - t2 */
    p{n}_sub(y3, y3, t0);         /* Y3 := Y3 - t0 */
    p{n}_add(t1, y3, y3);         /* t1 := Y3 + Y3 */
    p{n}_add(y3, t1, y3);         /* Y3 := t1 + Y3 */
    p{n}_add(t1, t0, t0);         /* t1 := t0 + t0 */
    p{n}_add(t0, t1, t0);         /* t0 := t1 + t0 */
    p{n}_sub(t0, t0, t2);         /* t0 := t0 - t2 */
    p{n}_mul(t1, t4, y3);         /* t1 := t4 * Y3 */
    p{n}_mul(t2, t0, y3);         /* t2 := t0 * Y3 */
    p{n}_mul(y3, x3, z3);         /* Y3 := X3 * Z3 */
    p{n}_add(y3, y3, t2);         /* Y3 := Y3 + t2 */
    p{n}_mul(x3, t3, x3);         /* X3 := t3 * X3 */
    p{n}_sub(x3, x3, t1);         /* X3 := X3 - t1 */
    p{n}_mul(z3, t4, z3);         /* Z3 := t4 * Z3 */
    p{n}_mul(t1, t3, t0);         /* t1 := t3 * t0 */
    p{n}_add(z3, z3, t1);         /* Z3 := Z3 + t1 */

    q->x = *x3;
    q->y = *y3;
    q->z = *z3;
    return q;
}}

/* Sets q = p + p, and returns q. The points may overlap. */
static NistecP{n}Point *p{n}_point_double(NistecP{n}Point *q, const NistecP{n}Point *p) {{
    /* Complete addition formula for a = -3 from "Complete addition formulas for
     * prime order elliptic curves" (https://eprint.iacr.org/2015/1060), A.2. */

    NistecP{n}Element t0_, t1_, t2_, t3_, x3_, y3_, z3_;
    NistecP{n}Element *t0 = &t0_, *t1 = &t1_, *t2 = &t2_, *t3 = &t3_;
    NistecP{n}Element *x3 = &x3_, *y3 = &y3_, *z3 = &z3_;
    const NistecP{n}Element *b = p{n}_b();

    p{n}_square(t0, &p->x);     /* t0 := X ^ 2 */
    p{n}_square(t1, &p->y);     /* t1 := Y ^ 2 */
    p{n}_square(t2, &p->z);     /* t2 := Z ^ 2 */
    p{n}_mul(t3, &p->x, &p->y); /* t3 := X * Y */
    p{n}_add(t3, t3, t3);       /* t3 := t3 + t3 */
    p{n}_mul(z3, &p->x, &p->z); /* Z3 := X * Z */
    p{n}_add(z3, z3, z3);       /* Z3 := Z3 + Z3 */
    p{n}_mul(y3, b, t2);        /* Y3 := b * t2 */
    p{n}_sub(y3, y3, z3);       /* Y3 := Y3 - Z3 */
    p{n}_add(x3, y3, y3);       /* X3 := Y3 + Y3 */
    p{n}_add(y3, x3, y3);       /* Y3 := X3 + Y3 */
    p{n}_sub(x3, t1, y3);       /* X3 := t1 - Y3 */
    p{n}_add(y3, t1, y3);       /* Y3 := t1 + Y3 */
    p{n}_mul(y3, x3, y3);       /* Y3 := X3 * Y3 */
    p{n}_mul(x3, x3, t3);       /* X3 := X3 * t3 */
    p{n}_add(t3, t2, t2);       /* t3 := t2 + t2 */
    p{n}_add(t2, t2, t3);       /* t2 := t2 + t3 */
    p{n}_mul(z3, b, z3);        /* Z3 := b * Z3 */
    p{n}_sub(z3, z3, t2);       /* Z3 := Z3 - t2 */
    p{n}_sub(z3, z3, t0);       /* Z3 := Z3 - t0 */
    p{n}_add(t3, z3, z3);       /* t3 := Z3 + Z3 */
    p{n}_add(z3, z3, t3);       /* Z3 := Z3 + t3 */
    p{n}_add(t3, t0, t0);       /* t3 := t0 + t0 */
    p{n}_add(t0, t3, t0);       /* t0 := t3 + t0 */
    p{n}_sub(t0, t0, t2);       /* t0 := t0 - t2 */
    p{n}_mul(t0, t0, z3);       /* t0 := t0 * Z3 */
    p{n}_add(y3, y3, t0);       /* Y3 := Y3 + t0 */
    p{n}_mul(t0, &p->y, &p->z); /* t0 := Y * Z */
    p{n}_add(t0, t0, t0);       /* t0 := t0 + t0 */
    p{n}_mul(z3, t0, z3);       /* Z3 := t0 * Z3 */
    p{n}_sub(x3, x3, z3);       /* X3 := X3 - Z3 */
    p{n}_mul(z3, t0, t1);       /* Z3 := t0 * t1 */
    p{n}_add(z3, z3, z3);       /* Z3 := Z3 + Z3 */
    p{n}_add(z3, z3, z3);       /* Z3 := Z3 + Z3 */

    q->x = *x3;
    q->y = *y3;
    q->z = *z3;
    return q;
}}

/* Sets q to p1 if cond == 1, and to p2 if cond == 0. */
static NistecP{n}Point *p{n}_point_select(NistecP{n}Point *q, const NistecP{n}Point *p1,
                                        const NistecP{n}Point *p2, int cond) {{
    p{n}_select(&q->x, &p1->x, &p2->x, cond);
    p{n}_select(&q->y, &p1->y, &p2->y, cond);
    p{n}_select(&q->z, &p1->z, &p2->z, cond);
    return q;
}}
"""

MULT = """
/* ------------------------------------------------- scalar multiplication */

/* The first 15 multiples of a point at offset -1, so [1]P is at table[0],
 * [15]P is at table[14], and [0]P is implicitly the identity point. */
typedef struct P{n}Table {{
    NistecP{n}Point p[15];
}} P{n}Table;

/* Selects the n-th multiple of the table base point into p. It works in
 * constant time by iterating over every entry of the table. n must be in
 * [0, 15]. */
static void p{n}_table_select(const P{n}Table *table, NistecP{n}Point *p, uint8_t n) {{
    if (n >= 16)
        panic_str(BURROW_S(
            "nistec: internal error: p{n}Table called with out-of-bounds value"));
    p{n}_point_new(p);
    for (uint8_t i = 1; i < 16; i++) {{
        int cond = (int)subtle_constant_time_byte_eq(i, n);
        p{n}_point_select(p, &table->p[i - 1], p, cond);
    }}
}}

/* Sets p = scalar * q, and returns p. */
static NistecP{n}Point *p{n}_point_scalar_mult(NistecP{n}Point *p, const NistecP{n}Point *q,
                                             Slice scalar, Error *err) {{
    (void)err;
    const uint8_t *s = scalar.p;

    /* Compute a table for the base point q. */
    P{n}Table table;
    table.p[0] = *q;
    for (int i = 1; i < 15; i += 2) {{
        p{n}_point_double(&table.p[i], &table.p[i / 2]);
        p{n}_point_add(&table.p[i + 1], &table.p[i], q);
    }}

    /* Instead of doing the classic double-and-add chain, we do it with a
     * four-bit window: we double four times, and then add [0-15]P. */
    NistecP{n}Point t;
    p{n}_point_new(&t);
    p{n}_point_new(p);
    for (Int i = 0; i < scalar.len; i++) {{
        /* No need to double on the first iteration, as p is the identity at
         * this point, and [N]inf = inf. */
        if (i != 0) {{
            p{n}_point_double(p, p);
            p{n}_point_double(p, p);
            p{n}_point_double(p, p);
            p{n}_point_double(p, p);
        }}

        uint8_t window_value = (uint8_t)(s[i] >> 4);
        p{n}_table_select(&table, &t, window_value);
        p{n}_point_add(p, p, &t);

        p{n}_point_double(p, p);
        p{n}_point_double(p, p);
        p{n}_point_double(p, p);
        p{n}_point_double(p, p);

        window_value = s[i] & 0xf;
        p{n}_table_select(&table, &t, window_value);
        p{n}_point_add(p, p, &t);
    }}

    return p;
}}

/* A sequence of tables, built the first time a base multiplication needs it.
 * The first table contains multiples of G. Each successive table is the
 * previous table doubled four times. */
static P{n}Table p{n}_generator_table[p{n}_element_length * 2];
static SyncOnce p{n}_generator_table_once;

static void p{n}_generator_table_init(void *env) {{
    (void)env;
    NistecP{n}Point base;
    p{n}_point_set_generator(&base);
    for (int i = 0; i < p{n}_element_length * 2; i++) {{
        p{n}_generator_table[i].p[0] = base;
        for (int j = 1; j < 15; j++)
            p{n}_point_add(&p{n}_generator_table[i].p[j],
                          &p{n}_generator_table[i].p[j - 1], &base);
        p{n}_point_double(&base, &base);
        p{n}_point_double(&base, &base);
        p{n}_point_double(&base, &base);
        p{n}_point_double(&base, &base);
    }}
}}

/* Sets p = scalar * B, where B is the canonical generator, and returns p. */
static NistecP{n}Point *p{n}_point_scalar_base_mult(NistecP{n}Point *p, Slice scalar,
                                                  Error *err) {{
    if (scalar.len != p{n}_element_length) {{
        BURROW_OUT(err, errors_new(error_allocator(), BURROW_S("invalid scalar length")));
        return NULL;
    }}
    sync_once_do(&p{n}_generator_table_once,
                 BURROW_FN(Func, p{n}_generator_table_init, NULL));
    const uint8_t *s = scalar.p;

    /* This is also a scalar multiplication with a four-bit window like in
     * ScalarMult, but in this case the doublings are precomputed. The value
     * [windowValue]G added at iteration k would normally get doubled
     * (totIterations-k)x4 times, but with a larger precomputation we can
     * instead add [2^((totIterations-k)x4)][windowValue]G and avoid the
     * doublings between iterations. */
    NistecP{n}Point t;
    p{n}_point_new(&t);
    p{n}_point_new(p);
    int table_index = p{n}_element_length * 2 - 1;
    for (Int i = 0; i < scalar.len; i++) {{
        uint8_t window_value = (uint8_t)(s[i] >> 4);
        p{n}_table_select(&p{n}_generator_table[table_index], &t, window_value);
        p{n}_point_add(p, p, &t);
        table_index--;

        window_value = s[i] & 0xf;
        p{n}_table_select(&p{n}_generator_table[table_index], &t, window_value);
        p{n}_point_add(p, p, &t);
        table_index--;
    }}

    return p;
}}
"""

VTABLE = """
/* ---------------------------------------------------------------- curve */

static NistecPoint *p{n}_v_point_new(NistecPoint *p) {{
    p{n}_point_new(&p->p{n});
    return p;
}}

static NistecPoint *p{n}_v_set_generator(NistecPoint *p) {{
    p{n}_point_set_generator(&p->p{n});
    return p;
}}

static NistecPoint *p{n}_v_set(NistecPoint *p, const NistecPoint *q) {{
    p->p{n} = q->p{n};
    return p;
}}

static NistecPoint *p{n}_v_set_bytes(NistecPoint *p, Slice b, Error *err) {{
    return p{n}_point_set_bytes(&p->p{n}, b, err) == NULL ? NULL : p;
}}

static Int p{n}_v_bytes(const NistecPoint *p, uint8_t *out) {{
    return p{n}_point_bytes(&p->p{n}, out);
}}

static uint8_t *p{n}_v_bytes_x(const NistecPoint *p, uint8_t *out, Error *err) {{
    return p{n}_point_bytes_x(&p->p{n}, out, err);
}}

static Int p{n}_v_bytes_compressed(const NistecPoint *p, uint8_t *out) {{
    return p{n}_point_bytes_compressed(&p->p{n}, out);
}}

static NistecPoint *p{n}_v_add(NistecPoint *q, const NistecPoint *p1,
                              const NistecPoint *p2) {{
    p{n}_point_add(&q->p{n}, &p1->p{n}, &p2->p{n});
    return q;
}}

static NistecPoint *p{n}_v_double(NistecPoint *q, const NistecPoint *p) {{
    p{n}_point_double(&q->p{n}, &p->p{n});
    return q;
}}

static NistecPoint *p{n}_v_scalar_mult(NistecPoint *p, const NistecPoint *q, Slice scalar,
                                      Error *err) {{
    return p{n}_point_scalar_mult(&p->p{n}, &q->p{n}, scalar, err) == NULL ? NULL : p;
}}

static NistecPoint *p{n}_v_scalar_base_mult(NistecPoint *p, Slice scalar, Error *err) {{
    return p{n}_point_scalar_base_mult(&p->p{n}, scalar, err) == NULL ? NULL : p;
}}

const NistecCurve burrow__nistec_p{n} = {{
    "P-{n}",
    p{n}_element_length,
    p{n}_v_point_new,
    p{n}_v_set_generator,
    p{n}_v_set,
    p{n}_v_set_bytes,
    p{n}_v_bytes,
    p{n}_v_bytes_x,
    p{n}_v_bytes_compressed,
    p{n}_v_add,
    p{n}_v_double,
    p{n}_v_scalar_mult,
    p{n}_v_scalar_base_mult,
}};
"""


def body(c):
    n = c["n"]
    fmt = dict(c, b=c_bytes(c["b"], "        "), gx=c_bytes(c["gx"], "        "),
               gy=c_bytes(c["gy"], "        "))
    out = INCLUDES.format(**fmt)
    out += (ELEMENT_SOLINAS if n == "521" else ELEMENT_MONTGOMERY).format(**fmt)
    out += ELEMENT_COMMON.format(**fmt)
    if n == "224":
        out += SQRT_224.format(sqrt=c["sqrt"], gg=c_bytes(c["gg"], "        "))
    elif n == "256":
        out += SQRT_256.format(sqrt=c["sqrt"])
    else:
        out += SQRT_CANDIDATE.format(**fmt)
    out += POINT.format(**fmt)
    return out


def wrap(text):
    """text as the lines of a comment, wrapped at 80 columns."""
    return "\n * ".join(textwrap.wrap(text, 77, break_on_hyphens=False))


def head(n, what, sources):
    generated = "Generated by tools/gen-nistec.sh from Go's p%s.go, %s; do not edit." % (
        n, sources)
    return HEAD.format(n=n, version=version, what=wrap(what), generated=wrap(generated))


def write(path, text):
    with open(path + ".tmp", "w") as f:
        f.write(text)
    os.replace(path + ".tmp", path)


for n in ("224", "384", "521"):
    c = curve(n)
    sources = "fiat/p%s.go and fiat/p%s_invert.go" % (n, n)
    if n == "224":
        sources = "p224_sqrt.go, " + sources
    what = (
        "The P-%s group: its field elements, on fiat-crypto's arithmetic in "
        "fiat_p%s_64.h or fiat_p%s_32.h, and its points, with the complete "
        "addition formulas and a four bit window for scalar multiplication."
        % (n, n, n)
    )
    text = head(n, what, sources)
    text += "\n" + body(c) + MULT.format(n=n) + VTABLE.format(n=n)
    write("src/crypto/nistec_p%s.c" % n, text)

c = curve("256")
what = (
    "The part of the P-256 group that is the same as the other three curves': its "
    "field elements, on fiat-crypto's arithmetic in fiat_p256_64.h or "
    "fiat_p256_32.h, and its points, with the complete addition formulas. "
    "nistec_p256.c includes it and has the rest, which is P-256's own."
)
text = head("256", what, "fiat/p256.go and fiat/p256_invert.go")
text += "\n#ifndef BURROW_CRYPTO_NISTEC_P256_POINT_H\n#define BURROW_CRYPTO_NISTEC_P256_POINT_H\n\n"
text += body(c) + "\n#endif\n"
write("src/crypto/nistec_p256_point.h", text)

# p256_table.go: 43 tables of 32 affine points, each two elements of four
# little endian 64 bit words in the Montgomery domain.
table = go_bytes(between(read("p256_table.go"), "p256PrecomputedEmbed = [...]byte{", "}"))
if len(table) != 43 * 32 * 2 * 4 * 8:
    sys.exit("gen-nistec: p256_table.go has %d bytes" % len(table))
words = [int.from_bytes(bytes(table[i : i + 8]), "little") for i in range(0, len(table), 8)]
lines = []
for t in range(43):
    lines.append("    {")
    for j in range(32):
        w = words[(t * 32 + j) * 8 : (t * 32 + j + 1) * 8]
        lines.append(
            "        {{%s},\n         {%s}},"
            % (", ".join("0x%016x" % v for v in w[0:4]), ", ".join("0x%016x" % v for v in w[4:8]))
        )
    lines.append("    },")
text = """\
/* Derived from Go's src/crypto/internal/fips140/nistec/p256_table.go.
 * Go source: {version}.
 *
 * Multiples of the P-256 generator for nistec_p256.c, the only file that
 * includes this. Table i holds [1]G to [32]G, where G is the generator doubled
 * 6i times, and each point is its affine x and y in the Montgomery domain with
 * R = 2^256, as four 64 bit words with the least significant first. That is
 * the same number whichever of fiat-crypto's two P-256 fields is in use, and
 * on 32 bit machines each word splits into two.
 *
 * Generated by tools/gen-nistec.sh from Go's p256_table.go; do not edit.
 *
 * Copyright 2024 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_CRYPTO_NISTEC_P256_TABLE_H
#define BURROW_CRYPTO_NISTEC_P256_TABLE_H

#include "nistec.h"

#include <stdint.h>

static const uint64_t p256_generator_tables[43][32][2][4] = {{
{rows}
}};

#endif
""".format(version=version, rows="\n".join(lines))
write("src/crypto/nistec_p256_table.h", text)

# The C goes through clang-format, so that it passes the same format check as
# the rest of the tree. The table does not, and .clang-format-ignore keeps the
# check off it.
fmt = os.environ.get("CLANG_FORMAT", "clang-format")
if shutil.which(fmt) is None:
    sys.exit("gen-nistec: no %s; set CLANG_FORMAT to the one CI uses" % fmt)
subprocess.run(
    [fmt, "-i"]
    + ["src/crypto/nistec_p%s.c" % n for n in ("224", "384", "521")]
    + ["src/crypto/nistec_p256_point.h"],
    check=True,
)
PY

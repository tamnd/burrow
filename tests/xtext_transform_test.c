/* Derived from golang.org/x/text v0.37.0's transform/transform_test.go and
 * transform/examples_test.go, the version Go 1.27.1 vendors.
 *
 * Go's tests reach inside the package to shrink the Reader's and Writer's
 * buffers and the buffers between a chain's links, and these do the same
 * through the dst_len and src_len fields and burrow__transform_chain_set_buf.
 *
 * The test transformers that keep state, replaceWithConstant and the two
 * tricklers, live in the arena of the test that uses them. The allocation
 * counts in TestString are left out, as they measure Go's allocator.
 *
 * Copyright 2013 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/xtext/norm.h"
#include "../src/xtext/transform.h"

#include "burrow/burrow.h"
#include "burrow/mem/arena.h"

#include <stdint.h>
#include <string.h>

#define LEN(x) ((Int)(sizeof(x) / sizeof((x)[0])))

enum { INITIAL_BUF_SIZE = 128 };

BURROW_SENTINEL_ERROR(err_you_mentioned_x, "you mentioned X");
BURROW_SENTINEL_ERROR(err_at_end, "error after all text");
BURROW_SENTINEL_ERROR(err_rle_bad_input, "rleDecode: bad input");

static const Error *const err_short_dst = &burrow__transform_err_short_dst;
static const Error *const err_short_src = &burrow__transform_err_short_src;
static const Error *const err_short_internal = &burrow__transform_err_short_internal;

/* ------------------------------------------------------------------ helpers */

static Slice bytes_of(Byte *p, Int len, Int cap) {
    Slice s = {p, len, cap, TYPE_BYTE};
    return s;
}

static Slice str_bytes(Str s) {
    return bytes_of((Byte *)(uintptr_t)s.p, s.len, s.len);
}

static Byte *alloc_bytes(Alloc *a, Int n) {
    return mem_alloc(a, n > 0 ? (size_t)n : 1, 1);
}

static Str repeat(Alloc *a, char c, Int n) {
    Byte *p = alloc_bytes(a, n);
    memset(p, c, (size_t)n);
    return str_from_bytes(p, n);
}

static Str cat(Alloc *a, Str x, Str y) {
    Byte *p = alloc_bytes(a, x.len + y.len);
    if (x.len > 0)
        memcpy(p, x.p, (size_t)x.len);
    if (y.len > 0)
        memcpy(p + x.len, y.p, (size_t)y.len);
    return str_from_bytes(p, x.len + y.len);
}

static Str prefix(Str s, Int n) {
    return str_from_bytes(s.p, n);
}

/* strings.Replace(s, old, "", -1). */
static Str remove_all(Alloc *a, Str s, Str old) {
    Byte *p = alloc_bytes(a, s.len);
    Int n = 0;
    for (Int i = 0; i < s.len;) {
        if (i + old.len <= s.len && memcmp(s.p + i, old.p, (size_t)old.len) == 0) {
            i += old.len;
            continue;
        }
        p[n++] = s.p[i++];
    }
    return str_from_bytes(p, n);
}

static Str to_lower(Alloc *a, Str s) {
    Byte *p = alloc_bytes(a, s.len);
    for (Int i = 0; i < s.len; i++)
        p[i] = s.p[i] >= 'A' && s.p[i] <= 'Z' ? (Byte)(s.p[i] + 'a' - 'A') : s.p[i];
    return str_from_bytes(p, s.len);
}

static Int itoa(Byte *buf, Int n) {
    Byte tmp[24];
    Int i = 0;
    do {
        tmp[i++] = (Byte)('0' + n % 10);
        n /= 10;
    } while (n > 0);
    for (Int j = 0; j < i; j++)
        buf[j] = tmp[i - 1 - j];
    return i;
}

static Int copy_to(Slice dst, const Byte *src, Int n) {
    if (dst.len < n)
        n = dst.len;
    if (n > 0)
        memcpy(dst.p, src, (size_t)n);
    return n;
}

/* A byte slice that grows from an arena. */
typedef struct Buf {
    Alloc *a;
    Byte *p;
    Int len, cap;
} Buf;

static void buf_write(Buf *b, const void *p, Int n) {
    if (n <= 0)
        return;
    if (b->len + n > b->cap) {
        Int c = b->cap * 2 + n + 64;
        Byte *q = mem_alloc_nozero(b->a, (size_t)c, 1);
        if (b->len > 0)
            memcpy(q, b->p, (size_t)b->len);
        b->p = q;
        b->cap = c;
    }
    memcpy(b->p + b->len, p, (size_t)n);
    b->len += n;
}

static Str buf_str(const Buf *b) {
    return str_from_bytes(b->p, b->len);
}

static bool err_is(Error e, const Error *want) {
    if (want == NULL)
        return !BURROW_FAILED(e);
    return burrow__transform_err_eq(e, *want);
}

static Str want_text(const Error *want) {
    return want == NULL ? BURROW_S("<nil>") : error_text(*want);
}

static bool tt_eq(TransformTransformer x, TransformTransformer y) {
    return x.vt == y.vt && x.data == y.data;
}

static const Type test_desc = {
    {(const Byte *)"testTransformer", 15},
    {(const Byte *)"golang.org/x/text/transform", 27},
    KIND_STRUCT,
    1,
    1,
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

static void nop_reset(void *self) {
    (void)self;
}

/* -------------------------------------------------------- test transformers */

static Int lower_transform(void *self, Slice dst, Slice src, bool at_eof, Int *n_src,
                           Error *err) {
    (void)self;
    (void)at_eof;
    Int n = src.len;
    *err = BURROW_NO_ERROR;
    if (n > dst.len) {
        n = dst.len;
        *err = burrow__transform_err_short_dst;
    }
    const Byte *s = src.p;
    Byte *d = dst.p;
    for (Int i = 0; i < n; i++) {
        Byte c = s[i];
        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
        d[i] = c;
    }
    *n_src = n;
    return n;
}

static const TransformTransformerVT lower_vt = {&test_desc, lower_transform, nop_reset};

static TransformTransformer lower_case_ascii(void) {
    TransformTransformer t = {&lower_vt, NULL};
    return t;
}

/* lowerCaseASCIILookahead lowercases the string and reports ErrShortSrc as long
 * as the input is not atEOF. */
static Int lower_lookahead_transform(void *self, Slice dst, Slice src, bool at_eof,
                                     Int *n_src, Error *err) {
    Int n = lower_transform(self, dst, src, at_eof, n_src, err);
    if (!at_eof)
        *err = burrow__transform_err_short_src;
    return n;
}

static const TransformTransformerVT lower_lookahead_vt = {
    &test_desc, lower_lookahead_transform, nop_reset};

static TransformTransformer lower_case_ascii_lookahead(void) {
    TransformTransformer t = {&lower_lookahead_vt, NULL};
    return t;
}

static Int dont_mention_x_transform(void *self, Slice dst, Slice src, bool at_eof,
                                    Int *n_src, Error *err) {
    (void)self;
    (void)at_eof;
    Int n = src.len;
    *err = BURROW_NO_ERROR;
    if (n > dst.len) {
        n = dst.len;
        *err = burrow__transform_err_short_dst;
    }
    const Byte *s = src.p;
    Byte *d = dst.p;
    for (Int i = 0; i < n; i++) {
        if (s[i] == 'X') {
            *n_src = i;
            *err = err_you_mentioned_x;
            return i;
        }
        d[i] = s[i];
    }
    *n_src = n;
    return n;
}

static const TransformTransformerVT dont_mention_x_vt = {
    &test_desc, dont_mention_x_transform, nop_reset};

static TransformTransformer dont_mention_x(void) {
    TransformTransformer t = {&dont_mention_x_vt, NULL};
    return t;
}

static Int error_at_end_transform(void *self, Slice dst, Slice src, bool at_eof,
                                  Int *n_src, Error *err) {
    (void)self;
    Int n = copy_to(dst, src.p, src.len);
    *n_src = n;
    *err = BURROW_NO_ERROR;
    if (n < src.len)
        *err = burrow__transform_err_short_dst;
    else if (at_eof)
        *err = err_at_end;
    return n;
}

static const TransformTransformerVT error_at_end_vt = {
    &test_desc, error_at_end_transform, nop_reset};

static TransformTransformer error_at_end(void) {
    TransformTransformer t = {&error_at_end_vt, NULL};
    return t;
}

typedef struct ReplaceWithConstant {
    Str replacement;
    Int written;
} ReplaceWithConstant;

static void replace_reset(void *self) {
    ((ReplaceWithConstant *)self)->written = 0;
}

static Int replace_transform(void *self, Slice dst, Slice src, bool at_eof, Int *n_src,
                             Error *err) {
    ReplaceWithConstant *t = self;
    Int n_dst = 0;
    *err = BURROW_NO_ERROR;
    if (at_eof) {
        n_dst = copy_to(dst, t->replacement.p + t->written,
                        t->replacement.len - t->written);
        t->written += n_dst;
        if (t->written < t->replacement.len)
            *err = burrow__transform_err_short_dst;
    }
    *n_src = src.len;
    return n_dst;
}

static const TransformTransformerVT replace_vt = {&test_desc, replace_transform,
                                                  replace_reset};

static TransformTransformer replace_with_constant(Alloc *a, const char *s) {
    ReplaceWithConstant *r = mem_alloc(a, sizeof *r, _Alignof(ReplaceWithConstant));
    r->replacement = str_from_cstr(s);
    TransformTransformer t = {&replace_vt, r};
    return t;
}

static Int add_an_x_transform(void *self, Slice dst, Slice src, bool at_eof, Int *n_src,
                              Error *err) {
    (void)self;
    Int n = copy_to(dst, src.p, src.len);
    *n_src = n;
    *err = BURROW_NO_ERROR;
    if (n < src.len) {
        *err = burrow__transform_err_short_dst;
        return n;
    }
    if (!at_eof)
        return n;
    if (dst.len == n) {
        *err = burrow__transform_err_short_dst;
        return n;
    }
    ((Byte *)dst.p)[n] = 'X';
    return n + 1;
}

static const TransformTransformerVT add_an_x_vt = {&test_desc, add_an_x_transform,
                                                   nop_reset};

static TransformTransformer add_an_x_at_the_end(void) {
    TransformTransformer t = {&add_an_x_vt, NULL};
    return t;
}

/* doublerAtEOF is a strange Transformer that transforms "this" to "tthhiiss",
 * but only if atEOF is true. */
static Int doubler_transform(void *self, Slice dst, Slice src, bool at_eof, Int *n_src,
                             Error *err) {
    (void)self;
    *err = BURROW_NO_ERROR;
    if (!at_eof) {
        *n_src = 0;
        *err = burrow__transform_err_short_src;
        return 0;
    }
    const Byte *s = src.p;
    Byte *d = dst.p;
    for (Int i = 0; i < src.len; i++) {
        if (2 * i + 2 >= dst.len) {
            *n_src = i;
            *err = burrow__transform_err_short_dst;
            return 2 * i;
        }
        d[2 * i + 0] = s[i];
        d[2 * i + 1] = s[i];
    }
    *n_src = src.len;
    return 2 * src.len;
}

static const TransformTransformerVT doubler_vt = {&test_desc, doubler_transform,
                                                  nop_reset};

static TransformTransformer doubler_at_eof(void) {
    TransformTransformer t = {&doubler_vt, NULL};
    return t;
}

/* rleDecode and rleEncode implement a toy run-length encoding: "aabbbbbbbbbb"
 * is encoded as "2a10b". The decoding is assumed to not contain any numbers. */
static Int rle_decode_transform(void *self, Slice dst, Slice src, bool at_eof,
                                Int *n_src, Error *err) {
    (void)self;
    Byte *d = dst.p;
    Int dlen = dst.len;
    const Byte *s = src.p;
    Int slen = src.len;
    Int n_dst = 0, ns = 0;
    *err = BURROW_NO_ERROR;
    while (slen > 0) {
        Int n = 0;
        bool found = false;
        for (Int i = 0; i < slen; i++) {
            Byte c = s[i];
            if (c >= '0' && c <= '9') {
                n = 10 * n + (c - '0');
                continue;
            }
            if (i == 0) {
                *err = err_rle_bad_input;
                *n_src = ns;
                return n_dst;
            }
            if (n > dlen) {
                *err = burrow__transform_err_short_dst;
                *n_src = ns;
                return n_dst;
            }
            for (Int j = 0; j < n; j++)
                d[j] = c;
            d += n;
            dlen -= n;
            s += i + 1;
            slen -= i + 1;
            n_dst += n;
            ns += i + 1;
            found = true;
            break;
        }
        if (found)
            continue;
        *err = at_eof ? err_rle_bad_input : burrow__transform_err_short_src;
        *n_src = ns;
        return n_dst;
    }
    *n_src = ns;
    return n_dst;
}

static const TransformTransformerVT rle_decode_vt = {&test_desc, rle_decode_transform,
                                                     nop_reset};

static TransformTransformer rle_decode(void) {
    TransformTransformer t = {&rle_decode_vt, NULL};
    return t;
}

/* allowStutter means that "xxxxxxxx" can be encoded as "5x3x" instead of
 * always as "8x". rleEncode{} is the NULL data and rleEncode{allowStutter:
 * true} points at stutter. */
static const bool stutter = true;

static Int rle_encode_transform(void *self, Slice dst, Slice src, bool at_eof,
                                Int *n_src, Error *err) {
    bool allow_stutter = self != NULL;
    Byte *d = dst.p;
    Int dlen = dst.len;
    const Byte *s = src.p;
    Int slen = src.len;
    Int n_dst = 0, ns = 0;
    *err = BURROW_NO_ERROR;
    while (slen > 0) {
        Int n = slen;
        Byte c0 = s[0];
        for (Int i = 1; i < slen; i++) {
            if (s[i] != c0) {
                n = i;
                break;
            }
        }
        if (n == slen && !at_eof && !allow_stutter) {
            *err = burrow__transform_err_short_src;
            *n_src = ns;
            return n_dst;
        }
        Byte num[24];
        Int k = itoa(num, n);
        if (k >= dlen) {
            *err = burrow__transform_err_short_dst;
            *n_src = ns;
            return n_dst;
        }
        memcpy(d, num, (size_t)k);
        d[k] = c0;
        d += k + 1;
        dlen -= k + 1;
        s += n;
        slen -= n;
        n_dst += k + 1;
        ns += n;
    }
    *n_src = ns;
    return n_dst;
}

static const TransformTransformerVT rle_encode_vt = {&test_desc, rle_encode_transform,
                                                     nop_reset};

static TransformTransformer rle_encode(bool allow_stutter) {
    TransformTransformer t = {&rle_encode_vt,
                              allow_stutter ? (void *)(uintptr_t)&stutter : NULL};
    return t;
}

/* trickler consumes all input bytes, but writes a single byte at a time to
 * dst. The bytes it holds are buf[off:off+len]. */
typedef struct Trickler {
    Byte buf[8192];
    Int off, len;
} Trickler;

static void trickler_reset(void *self) {
    Trickler *t = self;
    t->off = 0;
    t->len = 0;
}

static void trickler_append(Trickler *t, Slice src) {
    if (t->off + t->len + src.len > (Int)sizeof t->buf) {
        memmove(t->buf, t->buf + t->off, (size_t)t->len);
        t->off = 0;
    }
    if (t->len + src.len > (Int)sizeof t->buf)
        runtime_panic(BURROW_S("trickler: too much input"));
    if (src.len > 0)
        memcpy(t->buf + t->off + t->len, src.p, (size_t)src.len);
    t->len += src.len;
}

static Int trickler_transform(void *self, Slice dst, Slice src, bool at_eof, Int *n_src,
                              Error *err) {
    (void)at_eof;
    Trickler *t = self;
    trickler_append(t, src);
    *err = BURROW_NO_ERROR;
    if (t->len == 0) {
        *n_src = 0;
        return 0;
    }
    *n_src = src.len;
    if (dst.len == 0) {
        *err = burrow__transform_err_short_dst;
        return 0;
    }
    ((Byte *)dst.p)[0] = t->buf[t->off];
    t->off++;
    t->len--;
    if (t->len > 0)
        *err = burrow__transform_err_short_dst;
    return 1;
}

static const TransformTransformerVT trickler_vt = {&test_desc, trickler_transform,
                                                   trickler_reset};

/* delayedTrickler is like trickler, but delays writing output to dst. This is
 * highly unlikely to be relevant in practice, but it seems like a good idea to
 * have some tolerance as long as progress can be detected. */
static Int delayed_trickler_transform(void *self, Slice dst, Slice src, bool at_eof,
                                      Int *n_src, Error *err) {
    (void)at_eof;
    Trickler *t = self;
    Int n_dst = 0;
    if (t->len > 0 && dst.len > 0) {
        ((Byte *)dst.p)[0] = t->buf[t->off];
        t->off++;
        t->len--;
        n_dst = 1;
    }
    trickler_append(t, src);
    *err = BURROW_NO_ERROR;
    if (t->len > 0)
        *err = burrow__transform_err_short_dst;
    *n_src = src.len;
    return n_dst;
}

static const TransformTransformerVT delayed_trickler_vt = {
    &test_desc, delayed_trickler_transform, trickler_reset};

static TransformTransformer trickler(Alloc *a, bool delayed) {
    Trickler *tr = mem_alloc(a, sizeof *tr, _Alignof(Trickler));
    TransformTransformer t = {delayed ? &delayed_trickler_vt : &trickler_vt, tr};
    return t;
}

static TransformTransformer nop(void) {
    return burrow__transform_of(burrow__transform_nop());
}

/* ---------------------------------------------------------------- testCases */

enum { TT_OTHER, TT_RLE_ENCODE, TT_RLE_DECODE };

typedef struct TestCase {
    const char *desc;
    TransformTransformer t;
    Str src;
    Int dst_size;
    Int src_size;
    Int io_size;
    Str want_str;
    const Error *want_err;
    Int want_iter; /* number of iterations taken; 0 means we don't care */
    int kind;
} TestCase;

typedef struct Cases {
    TestCase *c;
    Int n, cap;
} Cases;

static void add(Alloc *a, Cases *cs, TestCase tc) {
    if (cs->n == cs->cap) {
        Int c = cs->cap * 2 + 64;
        TestCase *p = mem_alloc(a, (size_t)c * sizeof *p, _Alignof(TestCase));
        if (cs->n > 0)
            memcpy(p, cs->c, (size_t)cs->n * sizeof *p);
        cs->c = p;
        cs->cap = c;
    }
    cs->c[cs->n++] = tc;
}

#define S(x) BURROW_S(x)

static Str aaa, AAA;

static void init_strings(Alloc *a) {
    aaa = repeat(a, 'a', 4096);
    AAA = repeat(a, 'A', 4096);
}

static Str rle_long(Alloc *a) {
    Str s = repeat(a, 'a', 12);
    s = cat(a, s, repeat(a, 'b', 23));
    s = cat(a, s, repeat(a, 'c', 34));
    s = cat(a, s, repeat(a, 'd', 45));
    s = cat(a, s, repeat(a, 'e', 56));
    return cat(a, s, repeat(a, 'z', 99));
}

static Cases test_cases(Alloc *a) {
    Cases cs = {NULL, 0, 0};
    Str rule = S("The First Rule of Transform Club: don't mention Mister X, ever.");
    Str rule_want = S("The First Rule of Transform Club: don't mention Mister ");
    Str hello = S("Hello WORLD."), hello_want = S("hello world.");
    Str all_goes = S("All goes well until it doesn't.");
    Str rle_dec = S("1a2b3c10d11e0f1g"), rle_enc = S("abbcccddddddddddeeeeeeeeeeeg");

    add(a, &cs,
        (TestCase){"empty", lower_case_ascii(), S(""), 100, 100, 0, S(""), NULL, 0, 0});
    add(a, &cs,
        (TestCase){"basic", lower_case_ascii(), hello, 100, 100, 0, hello_want, NULL, 0,
                   0});
    add(a, &cs,
        (TestCase){"small dst", lower_case_ascii(), hello, 3, 100, 0, hello_want, NULL,
                   0, 0});
    add(a, &cs,
        (TestCase){"small src", lower_case_ascii(), hello, 100, 4, 0, hello_want, NULL,
                   0, 0});
    add(a, &cs,
        (TestCase){"small buffers", lower_case_ascii(), hello, 3, 4, 0, hello_want,
                   NULL, 0, 0});
    add(a, &cs,
        (TestCase){"very small buffers", lower_case_ascii(), hello, 1, 1, 0, hello_want,
                   NULL, 0, 0});
    add(a, &cs,
        (TestCase){"small dst with lookahead", lower_case_ascii_lookahead(), hello, 3,
                   100, 0, hello_want, NULL, 0, 0});
    add(a, &cs,
        (TestCase){"small src with lookahead", lower_case_ascii_lookahead(), hello, 100,
                   4, 0, hello_want, NULL, 0, 0});
    add(a, &cs,
        (TestCase){"small buffers with lookahead", lower_case_ascii_lookahead(), hello,
                   3, 4, 0, hello_want, NULL, 0, 0});
    add(a, &cs,
        (TestCase){"very small buffers with lookahead", lower_case_ascii_lookahead(),
                   hello, 1, 2, 0, hello_want, NULL, 0, 0});
    add(a, &cs,
        (TestCase){"user error", dont_mention_x(), rule, 100, 100, 0, rule_want,
                   &err_you_mentioned_x, 0, 0});
    add(a, &cs,
        (TestCase){"user error at end", error_at_end(), all_goes, 100, 100, 0, all_goes,
                   &err_at_end, 0, 0});
    add(a, &cs,
        (TestCase){"user error at end, incremental", error_at_end(), all_goes, 10, 10,
                   0, all_goes, &err_at_end, 0, 0});
    add(a, &cs,
        (TestCase){"replace entire non-empty string with one byte",
                   replace_with_constant(a, "X"), S("none of this will be copied"), 1,
                   10, 0, S("X"), NULL, 0, 0});
    add(a, &cs,
        (TestCase){"replace entire empty string with one byte",
                   replace_with_constant(a, "X"), S(""), 1, 10, 0, S("X"), NULL, 0, 0});
    add(a, &cs,
        (TestCase){"replace entire empty string with seven bytes",
                   replace_with_constant(a, "ABCDEFG"), S(""), 3, 10, 0, S("ABCDEFG"),
                   NULL, 0, 0});
    add(a, &cs,
        (TestCase){"add an X (initialBufSize-1)", add_an_x_at_the_end(),
                   prefix(aaa, INITIAL_BUF_SIZE - 1), 10, 10, 0,
                   cat(a, prefix(aaa, INITIAL_BUF_SIZE - 1), S("X")), NULL, 0, 0});
    add(a, &cs,
        (TestCase){"add an X (initialBufSize+0)", add_an_x_at_the_end(),
                   prefix(aaa, INITIAL_BUF_SIZE + 0), 10, 10, 0,
                   cat(a, prefix(aaa, INITIAL_BUF_SIZE + 0), S("X")), NULL, 0, 0});
    add(a, &cs,
        (TestCase){"add an X (initialBufSize+1)", add_an_x_at_the_end(),
                   prefix(aaa, INITIAL_BUF_SIZE + 1), 10, 10, 0,
                   cat(a, prefix(aaa, INITIAL_BUF_SIZE + 1), S("X")), NULL, 0, 0});
    add(a, &cs,
        (TestCase){"small buffers", dont_mention_x(), rule, 10, 10, 0, rule_want,
                   &err_you_mentioned_x, 0, 0});
    add(a, &cs,
        (TestCase){"very small buffers", dont_mention_x(), rule, 1, 1, 0, rule_want,
                   &err_you_mentioned_x, 0, 0});
    add(a, &cs,
        (TestCase){"only transform at EOF", doubler_at_eof(), S("this"), 100, 100, 0,
                   S("tthhiiss"), NULL, 0, 0});
    add(a, &cs,
        (TestCase){"basic", rle_decode(), rle_dec, 100, 100, 0, rle_enc, NULL, 0,
                   TT_RLE_DECODE});
    add(a, &cs,
        (TestCase){"long", rle_decode(), S("12a23b34c45d56e99z"), 100, 100, 0,
                   rle_long(a), NULL, 0, TT_RLE_DECODE});
    add(a, &cs,
        (TestCase){"tight buffers", rle_decode(), rle_dec, 11, 3, 0, rle_enc, NULL, 0,
                   TT_RLE_DECODE});
    add(a, &cs,
        (TestCase){"short dst", rle_decode(), rle_dec, 10, 3, 0, S("abbcccdddddddddd"),
                   err_short_dst, 0, TT_RLE_DECODE});
    add(a, &cs,
        (TestCase){"short src", rle_decode(), rle_dec, 11, 2, 2, S("abbccc"),
                   err_short_src, 0, TT_RLE_DECODE});
    add(a, &cs,
        (TestCase){"basic", rle_encode(false), rle_enc, 100, 100, 0,
                   S("1a2b3c10d11e1g"), NULL, 0, TT_RLE_ENCODE});
    add(a, &cs,
        (TestCase){"long", rle_encode(false), rle_long(a), 100, 100, 0,
                   S("12a23b34c45d56e99z"), NULL, 0, TT_RLE_ENCODE});
    add(a, &cs,
        (TestCase){"tight buffers", rle_encode(false), rle_enc, 3, 12, 0,
                   S("1a2b3c10d11e1g"), NULL, 0, TT_RLE_ENCODE});
    add(a, &cs,
        (TestCase){"short dst", rle_encode(false), rle_enc, 2, 12, 0, S("1a2b3c"),
                   err_short_dst, 0, TT_RLE_ENCODE});
    add(a, &cs,
        (TestCase){"short src", rle_encode(false), rle_enc, 3, 11, 11, S("1a2b3c10d"),
                   err_short_src, 0, TT_RLE_ENCODE});
    add(a, &cs,
        (TestCase){"allowStutter = false", rle_encode(false),
                   S("aaaabbbbbbbbccccddddd"), 10, 10, 0, S("4a8b4c5d"), NULL, 0,
                   TT_RLE_ENCODE});
    add(a, &cs,
        (TestCase){"allowStutter = true", rle_encode(true), S("aaaabbbbbbbbccccddddd"),
                   10, 10, 10, S("4a6b2b4c4d1d"), NULL, 0, TT_RLE_ENCODE});
    add(a, &cs,
        (TestCase){"trickler", trickler(a, false), S("abcdefghijklm"), 3, 15, 0,
                   S("abcdefghijklm"), NULL, 0, 0});
    add(a, &cs,
        (TestCase){"delayedTrickler", trickler(a, true), S("abcdefghijklm"), 3, 15, 0,
                   S("abcdefghijklm"), NULL, 0, 0});
    return cs;
}

/* --------------------------------------------------------------- chainTests */

/* mkChain creates a Chain transformer of n transformers, with sizes[j-1] the
 * size of the buffer between t[j-1] and t[j]. */
static TransformTransformer mk_chain(Alloc *a, Int n, const TransformTransformer *t,
                                     const Int *sizes) {
    TransformTransformer c = burrow__transform_chain(a, t, n);
    for (Int j = 1; j < n; j++)
        burrow__transform_chain_set_buf(
            c, j, bytes_of(alloc_bytes(a, sizes[j - 1]), sizes[j - 1], sizes[j - 1]));
    return c;
}

#define T(...) ((const TransformTransformer[]){__VA_ARGS__})
#define Z(...) ((const Int[]){__VA_ARGS__})
#define CHAIN(n, ts, zs) mk_chain(a, n, ts, zs)

static Cases chain_tests(Alloc *a) {
    Cases cs = {NULL, 0, 0};
    Str rle_dec = S("1a2b3c10d11e0f1g");
    add(a, &cs,
        (TestCase){"nil error",
                   CHAIN(2, T(rle_encode(false), lower_case_ascii()), Z(100)), S("ABB"),
                   100, 100, 0, S("1a2b"), NULL, 1, 0});
    add(a, &cs,
        (TestCase){"short dst buffer",
                   CHAIN(2, T(lower_case_ascii(), rle_decode()), Z(3)), rle_dec, 10, 3,
                   0, S("abbcccdddddddddd"), err_short_dst, 0, 0});
    add(a, &cs,
        (TestCase){"short internal dst buffer",
                   CHAIN(3, T(lower_case_ascii(), rle_decode(), nop()), Z(3, 10)),
                   rle_dec, 100, 3, 0, S("abbcccdddddddddd"), err_short_internal, 0,
                   0});
    add(a, &cs,
        (TestCase){"short internal dst buffer from input",
                   CHAIN(2, T(rle_decode(), nop()), Z(10)), rle_dec, 100, 3, 0,
                   S("abbcccdddddddddd"), err_short_internal, 0, 0});
    add(a, &cs,
        (TestCase){"empty short internal dst buffer",
                   CHAIN(3, T(lower_case_ascii(), rle_decode(), nop()), Z(3, 10)),
                   S("4a7b11e0f1g"), 100, 3, 0, S("aaaabbbbbbb"), err_short_internal, 0,
                   0});
    add(a, &cs,
        (TestCase){"empty short internal dst buffer from input",
                   CHAIN(2, T(rle_decode(), nop()), Z(10)), S("4a7b11e0f1g"), 100, 3, 0,
                   S("aaaabbbbbbb"), err_short_internal, 0, 0});
    add(a, &cs,
        (TestCase){"short internal src buffer after full dst buffer",
                   CHAIN(3, T(nop(), rle_encode(false), nop()), Z(5, 10)),
                   S("cccccddddd"), 100, 100, 0, S(""), err_short_internal, 1, 0});
    add(a, &cs,
        (TestCase){"short internal src buffer after short dst buffer; test lastFull",
                   CHAIN(3, T(rle_decode(), rle_encode(false), nop()), Z(5, 4)),
                   S("2a1b4c6d"), 100, 100, 0, S("2a1b"), err_short_internal, 0, 0});
    add(a, &cs,
        (TestCase){"short internal src buffer after successful complete fill",
                   CHAIN(2, T(nop(), rle_decode()), Z(3)), S("123a4b"), 4, 3, 0, S(""),
                   err_short_internal, 1, 0});
    add(a, &cs,
        (TestCase){"short internal src buffer after short dst buffer; test lastFull",
                   CHAIN(2, T(rle_decode(), rle_encode(false)), Z(5)), S("2a1b4c6d"), 4,
                   100, 0, S("2a1b"), err_short_internal, 0, 0});
    add(a, &cs,
        (TestCase){"short src buffer", CHAIN(2, T(rle_encode(false), nop()), Z(5)),
                   S("abbcccddddeeeee"), 4, 4, 4, S("1a2b3c"), err_short_src, 0, 0});
    add(a, &cs,
        (TestCase){"process all in one go", CHAIN(2, T(rle_encode(false), nop()), Z(5)),
                   S("abbcccddddeeeeeffffff"), 100, 100, 0, S("1a2b3c4d5e6f"), NULL, 1,
                   0});
    add(a, &cs,
        (TestCase){"complete processing downstream after error",
                   CHAIN(3, T(dont_mention_x(), rle_decode(), nop()), Z(2, 5)),
                   S("3a4b5eX"), 100, 100, 100, S("aaabbbbeeeee"), &err_you_mentioned_x,
                   0, 0});
    add(a, &cs,
        (TestCase){"return downstream fatal errors first (followed by short dst)",
                   CHAIN(3, T(dont_mention_x(), rle_decode(), nop()), Z(8, 4)),
                   S("3a4b5eX"), 100, 100, 100, S("aaabbbb"), err_short_internal, 0,
                   0});
    add(a, &cs,
        (TestCase){"return downstream fatal errors first (followed by short src)",
                   CHAIN(3, T(dont_mention_x(), nop(), rle_decode()), Z(5, 1)),
                   S("1a5bX"), 100, 100, 100, S(""), err_short_internal, 0, 0});
    add(a, &cs,
        (TestCase){"short internal",
                   CHAIN(3, T(nop(), rle_encode(false), nop()), Z(11, 3)),
                   S("abbcccddddddddddeeeeeeeeeeeg"), 3, 100, 0, S("1a2b3c10d"),
                   err_short_internal, 0, 0});
    return cs;
}

static Cases all_cases(Alloc *a) {
    Cases cs = test_cases(a);
    Cases ct = chain_tests(a);
    for (Int i = 0; i < ct.n; i++)
        add(a, &cs, ct.c[i]);
    return cs;
}

/* -------------------------------------------------------------- doTransform */

static Str do_transform(Alloc *a, const TestCase *tc, Int *iter, Error *err) {
    if (tc->t.vt != NULL)
        tc->t.vt->reset(tc->t.data);
    Byte *dst = alloc_bytes(a, tc->dst_size);
    Buf out = {a, NULL, 0, 0};
    Slice in = str_bytes(tc->src);
    *iter = 0;
    for (;;) {
        (*iter)++;
        Slice src = in;
        bool at_eof = true;
        if (src.len > tc->src_size) {
            src.len = tc->src_size;
            at_eof = false;
        }
        Int n_src = 0;
        Error e = BURROW_NO_ERROR;
        Int n_dst = burrow__transform_call(
            tc->t, bytes_of(dst, tc->dst_size, tc->dst_size), src, at_eof, &n_src, &e);
        buf_write(&out, dst, n_dst);
        if (n_src > 0)
            in = bytes_of((Byte *)in.p + n_src, in.len - n_src, in.cap - n_src);
        if (!BURROW_FAILED(e) && in.len != 0)
            continue;
        if (burrow__transform_err_eq(e, *err_short_src) && n_src > 0)
            continue;
        if (burrow__transform_err_eq(e, *err_short_dst) && (n_dst > 0 || n_src > 0))
            continue;
        *err = e;
        return buf_str(&out);
    }
}

/* ------------------------------------------------------------ Reader, Writer */

/* strings.Reader and bytes.Buffer, near enough. */
typedef struct StrReader {
    Str s;
} StrReader;

static Int str_read(void *self, Slice p, Error *err) {
    StrReader *r = self;
    if (r->s.len == 0) {
        *err = io_eof;
        return 0;
    }
    Int n = copy_to(p, r->s.p, r->s.len);
    r->s = str_from_bytes(r->s.p + n, r->s.len - n);
    *err = BURROW_NO_ERROR;
    return n;
}

static const IoReaderVT str_reader_vt = {&test_desc, str_read};

static Int buf_io_write(void *self, Slice p, Error *err) {
    buf_write(self, p.p, p.len);
    *err = BURROW_NO_ERROR;
    return p.len;
}

static const IoWriterVT buf_writer_vt = {&test_desc, buf_io_write};

/* io.ReadAll. */
static Str read_all(Alloc *a, TransformReader *r, Error *err) {
    Buf out = {a, NULL, 0, 0};
    Byte chunk[512];
    for (;;) {
        Error e = BURROW_NO_ERROR;
        Int n = burrow__transform_reader_read(r, bytes_of(chunk, 512, 512), &e);
        buf_write(&out, chunk, n);
        if (BURROW_FAILED(e)) {
            *err = burrow__transform_err_eq(e, io_eof) ? BURROW_NO_ERROR : e;
            return buf_str(&out);
        }
    }
}

static void TestReader(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    init_strings(a);
    Cases cs = test_cases(a);
    for (Int i = 0; i < cs.n; i++) {
        const TestCase *tc = &cs.c[i];
        StrReader sr = {tc->src};
        IoReader src = {&str_reader_vt, &sr};
        TransformReader *r = burrow__transform_new_reader(a, src, tc->t);
        /* Differently sized dst and src buffers are not part of the exported
         * API. We override them manually. */
        r->dst = alloc_bytes(a, tc->dst_size);
        r->dst_len = tc->dst_size;
        r->src = alloc_bytes(a, tc->src_size);
        r->src_len = tc->src_size;
        Error err = BURROW_NO_ERROR;
        Str got = read_all(a, r, &err);
        if (!str_eq(got, tc->want_str) || !err_is(err, tc->want_err))
            testing_t_errorf_v(t, "%s:\ngot  %q, %v\nwant %q, %s", tc->desc, got, err,
                               tc->want_str, want_text(tc->want_err));
    }
    arena_free(&ar);
}

static void TestWriter(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    init_strings(a);
    Cases cs = all_cases(a);
    static const Int all_sizes[] = {1, 2, 3, 4, 5, 10, 100, 1000};
    for (Int i = 0; i < cs.n; i++) {
        const TestCase *tc = &cs.c[i];
        const Int *sizes = all_sizes;
        Int nsizes = LEN(all_sizes);
        if (tc->io_size > 0) {
            sizes = &tc->io_size;
            nsizes = 1;
        }
        for (Int k = 0; k < nsizes; k++) {
            Int sz = sizes[k];
            Buf bb = {a, NULL, 0, 0};
            IoWriter dst = {&buf_writer_vt, &bb};
            TransformWriter *w = burrow__transform_new_writer(a, dst, tc->t);
            /* Differently sized dst and src buffers are not part of the
             * exported API. We override them manually. */
            w->dst = alloc_bytes(a, tc->dst_size);
            w->dst_len = tc->dst_size;
            w->src = alloc_bytes(a, tc->src_size);
            w->src_len = tc->src_size;
            Byte *src = alloc_bytes(a, sz);
            Error err = BURROW_NO_ERROR;
            for (Str b = tc->src; b.len > 0 && !BURROW_FAILED(err);) {
                Int n = b.len < sz ? b.len : sz;
                memcpy(src, b.p, (size_t)n);
                b = str_from_bytes(b.p + n, b.len - n);
                Int m = burrow__transform_writer_write(w, bytes_of(src, n, sz), &err);
                if (m != n && !BURROW_FAILED(err))
                    testing_t_errorf_v(t, "%s/%d: did not consume all bytes %d < %d",
                                       tc->desc, sz, m, n);
            }
            if (!BURROW_FAILED(err))
                err = burrow__transform_writer_close(w);
            Str got = buf_str(&bb);
            if (!str_eq(got, tc->want_str) || !err_is(err, tc->want_err))
                testing_t_errorf_v(t, "%s/%d:\ngot  %q, %v\nwant %q, %s", tc->desc, sz,
                                   got, err, tc->want_str, want_text(tc->want_err));
        }
    }
    arena_free(&ar);
}

/* ----------------------------------------------------------------- Nop etc. */

static void TestNop(TestingT *t) {
    static const struct {
        const char *str;
        Int dst_size;
        const Error *err;
    } cases[] = {
        {"", 0, NULL},  {"", 10, NULL},  {"a", 0, &burrow__transform_err_short_dst},
        {"a", 1, NULL}, {"a", 10, NULL},
    };
    for (Int i = 0; i < LEN(cases); i++) {
        Byte dst[16];
        Str s = str_from_cstr(cases[i].str);
        Int n_src = 0;
        Error err = BURROW_NO_ERROR;
        Int n_dst = burrow__transform_call(nop(), bytes_of(dst, cases[i].dst_size, 16),
                                           str_bytes(s), true, &n_src, &err);
        Str want = s;
        if (cases[i].dst_size < want.len)
            want = prefix(want, cases[i].dst_size);
        Str got = str_from_bytes(dst, n_dst);
        if (!str_eq(got, want) || !err_is(err, cases[i].err) || n_src != n_dst)
            testing_t_errorf_v(t, "%d:\ngot %q, %d, %v\nwant %q, %d, %s", i, got, n_src,
                               err, want, n_dst, want_text(cases[i].err));
    }
}

static void TestDiscard(TestingT *t) {
    static const struct {
        const char *str;
        Int dst_size;
    } cases[] = {{"", 0}, {"", 10}, {"a", 0}, {"ab", 10}};
    for (Int i = 0; i < LEN(cases); i++) {
        Byte dst[16];
        Str s = str_from_cstr(cases[i].str);
        Int n_src = 0;
        Error err = BURROW_NO_ERROR;
        Int n_dst = burrow__transform_call(burrow__transform_discard(),
                                           bytes_of(dst, cases[i].dst_size, 16),
                                           str_bytes(s), true, &n_src, &err);
        if (n_dst != 0 || n_src != s.len || BURROW_FAILED(err))
            testing_t_errorf_v(t, "%d:\ngot %d, %d, %v\nwant 0, %d, nil", i, n_dst,
                               n_src, err, s.len);
    }
}

/* -------------------------------------------------------------------- Chain */

static void add_test(Alloc *a, Cases *tests, TestCase tc, TransformTransformer c,
                     TransformTransformer first, TransformTransformer last) {
    if (!tt_eq(first, tc.t) && tc.want_err == err_short_src)
        tc.want_err = err_short_internal;
    if (!tt_eq(last, tc.t) && tc.want_err == err_short_dst)
        tc.want_err = err_short_internal;
    tc.t = c;
    add(a, tests, tc);
}

#define ADD(tt, n, ts, zs)                                                             \
    do {                                                                               \
        const TransformTransformer *ts_ = (ts);                                        \
        add_test(a, &tests, (tt), mk_chain(a, (n), ts_, (zs)), ts_[0], ts_[(n) - 1]);  \
    } while (0)

static void TestChain(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    init_strings(a);

    TransformTransformer empty = burrow__transform_chain(a, NULL, 0);
    if (empty.vt != nop().vt)
        testing_t_errorf_v(t, "empty chain: not Nop");

    /* Test Chain for a single Transformer. */
    Cases cs = test_cases(a);
    for (Int i = 0; i < cs.n; i++) {
        TestCase tc = cs.c[i];
        tc.t = burrow__transform_chain(a, &cs.c[i].t, 1);
        Int iter = 0;
        Error err = BURROW_NO_ERROR;
        Str str = do_transform(a, &tc, &iter, &err);
        if (!str_eq(str, tc.want_str) || !err_is(err, tc.want_err))
            testing_t_errorf_v(t, "Chain; %s:\ngot  %q, %v\nwant %q, %s", tc.desc, str,
                               err, tc.want_str, want_text(tc.want_err));
    }

    Cases tests = chain_tests(a);
    static const Int sizes[] = {1, 2, 3, 4, 5, 7, 10, 100, 1000};
    for (Int i = 0; i < cs.n; i++) {
        TestCase tc = cs.c[i];
        for (Int k = 0; k < LEN(sizes); k++) {
            TestCase tt = tc;
            tt.dst_size = sizes[k];
            ADD(tt, 2, T(tc.t, nop()), Z(tc.dst_size));
            ADD(tt, 3, T(tc.t, nop(), nop()), Z(tc.dst_size, 2));
            ADD(tt, 3, T(nop(), tc.t, nop()), Z(tc.src_size, tc.dst_size));
            if (sizes[k] >= tc.dst_size &&
                (tc.want_err != err_short_dst || sizes[k] == tc.dst_size)) {
                ADD(tt, 2, T(nop(), tc.t), Z(tc.src_size));
                ADD(tt, 3, T(nop(), nop(), tc.t), Z(100, tc.src_size));
            }
        }
    }
    for (Int i = 0; i < cs.n; i++) {
        TestCase tc = cs.c[i];
        TestCase tt = tc;
        tt.dst_size = 1;
        tt.want_str = S("");
        TransformTransformer d = burrow__transform_discard();
        ADD(tt, 2, T(tc.t, d), Z(tc.dst_size));
        ADD(tt, 3, T(nop(), tc.t, d), Z(tc.src_size, tc.dst_size));
        ADD(tt, 4, T(nop(), tc.t, nop(), d), Z(tc.src_size, tc.dst_size, tc.dst_size));
    }
    for (Int i = 0; i < cs.n; i++) {
        TestCase tc = cs.c[i];
        TestCase tt = tc;
        tt.dst_size = 100;
        tt.want_str = remove_all(a, tc.src, S("0f"));
        /* Chain encoders and decoders. */
        if (tc.kind == TT_RLE_ENCODE && tc.want_err == NULL) {
            ADD(tt, 3, T(tc.t, nop(), rle_decode()), Z(tc.dst_size, 1000));
            ADD(tt, 3, T(tc.t, nop(), rle_decode()), Z(tc.dst_size, tc.dst_size));
            ADD(tt, 4, T(nop(), tc.t, nop(), rle_decode()),
                Z(tc.src_size, tc.dst_size, 100));
            /* decoding needs larger destinations */
            ADD(tt, 4, T(nop(), tc.t, rle_decode(), nop()),
                Z(tc.src_size, tc.dst_size, 100));
            ADD(tt, 5, T(nop(), tc.t, nop(), rle_decode(), nop()),
                Z(tc.src_size, tc.dst_size, 100, 100));
        } else if (tc.kind == TT_RLE_DECODE && tc.want_err == NULL) {
            /* The internal buffer size may need to be the sum of the maximum
             * segment size of the two encoders! */
            ADD(tt, 2, T(tc.t, rle_encode(false)), Z(2 * tc.dst_size));
            ADD(tt, 3, T(tc.t, nop(), rle_encode(false)), Z(tc.dst_size, 101));
            ADD(tt, 4, T(nop(), tc.t, nop(), rle_encode(false)),
                Z(tc.src_size, tc.dst_size, 100));
            ADD(tt, 5, T(nop(), tc.t, nop(), rle_encode(false), nop()),
                Z(tc.src_size, tc.dst_size, 200, 100));
        }
    }
    /* Go's loop ends in a break, so only the first of these runs there, and
     * so only the first runs here. With the break taken out, Go's own test
     * does not finish: it ran into the ten minute test timeout when tried
     * against x/text v0.37.0. */
    for (Int i = 0; i < tests.n; i++) {
        const TestCase *tc = &tests.c[i];
        Int iter = 0;
        Error err = BURROW_NO_ERROR;
        Str str = do_transform(a, tc, &iter, &err);
        bool mi = tc->want_iter != 0 && tc->want_iter != iter;
        if (!str_eq(str, tc->want_str) || !err_is(err, tc->want_err) || mi)
            testing_t_errorf_v(t, "%s:\ngot  iter:%d, %q, %v\nwant iter:%d, %q, %s",
                               tc->desc, iter, str, err, tc->want_iter, tc->want_str,
                               want_text(tc->want_err));
        break;
    }
    arena_free(&ar);
}

/* --------------------------------------------------------------- RemoveFunc */

static bool in_filter(void *env, Rune r) {
    (void)env;
    return r == 'a' || r == 'b' || r == 0x300 || r == 0x1234 || r == ',';
}

static bool is_rune_error(void *env, Rune r) {
    (void)env;
    return r == UTF8_RUNE_ERROR;
}

static void TestRemoveFunc(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TransformTransformer filter =
        burrow__transform_remove_func(a, BURROW_FN(RuneFunc, in_filter, NULL));
    TransformTransformer no_error =
        burrow__transform_remove_func(a, BURROW_FN(RuneFunc, is_rune_error, NULL));
    TransformTransformer none = {NULL, NULL};
    const TestCase tests[] = {
        {NULL, none, S(","), 0, 0, 0, S(""), NULL, 0, 0},
        {NULL, none, S("c"), 0, 0, 0, S("c"), NULL, 0, 0},
        {NULL, none, S("\xE2\x8D\x85"), 0, 0, 0, S("\xE2\x8D\x85"), NULL, 0, 0},
        {NULL, none, S("tsch\xC3\xBC\xC3\x9F"), 0, 0, 0, S("tsch\xC3\xBC\xC3\x9F"),
         NULL, 0, 0},
        {NULL, none,
         S(",\xD0\xB4\xD0\xBE,"
           "\xD1\x81\xD0\xB2\xD0\xB8\xD0\xB4\xD0\xB0\xD0\xBD\xD0\xB8\xD1\x8F,"),
         0, 0, 0,
         S("\xD0\xB4\xD0\xBE\xD1\x81\xD0\xB2\xD0\xB8\xD0\xB4\xD0\xB0\xD0\xBD\xD0\xB8"
           "\xD1\x8F"),
         NULL, 0, 0},
        {NULL, none, S("a\xbd\xb2=\xbc \xE2\x8C\x98"), 0, 0, 0,
         S("\xEF\xBF\xBD\xEF\xBF\xBD=\xEF\xBF\xBD \xE2\x8C\x98"), NULL, 0, 0},
        /* If we didn't replace illegal bytes with RuneError, the result would
         * be \u0300 or the code would need to be more complex. */
        {NULL, none, S("\xcc\xCC\x80\x80"), 0, 0, 0, S("\xEF\xBF\xBD\xEF\xBF\xBD"),
         NULL, 0, 0},
        {NULL, none, S("\xcc\xCC\x80\x80"), 3, 0, 0, S("\xEF\xBF\xBD\xEF\xBF\xBD"),
         NULL, 2, 0},
        /* Test a long buffer greater than the internal buffer size */
        {NULL, none, S("hello\xcc\xcc\xccworld"), 0, 13, 0,
         S("hello\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBDworld"), NULL, 1, 0},
        {NULL, none, S("\xE2\x8D\x85"), 2, 0, 0, S(""), err_short_dst, 0, 0},
        {NULL, none, S("\xcc"), 2, 0, 0, S(""), err_short_dst, 0, 0},
        {NULL, none, S("\xCC\x80"), 2, 1, 0, S(""), err_short_src, 0, 0},
        {NULL, no_error, S("\xcc\xCC\x80\x80"), 0, 0, 0, S("\xCC\x80"), NULL, 0, 0},
    };
    for (Int i = 0; i < LEN(tests); i++) {
        TestCase tc = tests[i];
        if (tc.t.vt == NULL)
            tc.t = filter;
        if (tc.dst_size == 0)
            tc.dst_size = 100;
        if (tc.src_size == 0)
            tc.src_size = 100;
        Int iter = 0;
        Error err = BURROW_NO_ERROR;
        Str str = do_transform(a, &tc, &iter, &err);
        bool mi = tc.want_iter != 0 && tc.want_iter != iter;
        if (!str_eq(str, tc.want_str) || !err_is(err, tc.want_err) || mi)
            testing_t_errorf_v(t, "%+q:\ngot  iter:%d, %+q, %v\nwant iter:%d, %+q, %s",
                               tc.src, iter, str, err, tc.want_iter, tc.want_str,
                               want_text(tc.want_err));

        tc.src = str;
        Str idem = do_transform(a, &tc, &iter, &err);
        if (!str_eq(str, idem))
            testing_t_errorf_v(t, "%+q: found %+q; want %+q", tc.src, idem, str);
    }
    arena_free(&ar);
}

/* --------------------------------------------------- String, Bytes, Append */

typedef Str (*StringFunc)(Alloc *a, TransformTransformer z, Str s, Int *n, Error *err,
                          void *env);

static void test_string(TestingT *t, const char *name, StringFunc f, void *env) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    init_strings(a);
    Cases cs = all_cases(a);
    for (Int i = 0; i < cs.n; i++) {
        const TestCase *tt = &cs.c[i];
        if (strcmp(tt->desc, "allowStutter = true") == 0) {
            /* We don't have control over the buffer size, so we eliminate
             * tests that depend on a specific buffer size being set. */
            continue;
        }
        if (tt->want_err == err_short_dst || tt->want_err == err_short_src) {
            /* The result string will be different. */
            continue;
        }
        Int n = 0;
        Error err = BURROW_NO_ERROR;
        Str got = f(a, tt->t, tt->src, &n, &err, env);
        if (!err_is(err, tt->want_err))
            testing_t_errorf_v(t, "%s/%s: error: got %v; want %s", name, tt->desc, err,
                               want_text(tt->want_err));
        /* Check that err == nil implies that n == len(tt.src). Note that vice
         * versa isn't necessarily true. */
        if (!BURROW_FAILED(err) && n != tt->src.len)
            testing_t_errorf_v(t, "%s/%s: err == nil: got %d bytes, want %d", name,
                               tt->desc, n, tt->src.len);
        if (!str_eq(got, tt->want_str))
            testing_t_errorf_v(t, "%s/%s: string: got %q; want %q", name, tt->desc, got,
                               tt->want_str);
    }
    arena_free(&ar);
}

static Str bytes_f(Alloc *a, TransformTransformer z, Str s, Int *n, Error *err,
                   void *env) {
    (void)env;
    Byte *p = alloc_bytes(a, s.len);
    if (s.len > 0)
        memcpy(p, s.p, (size_t)s.len);
    Slice b = burrow__transform_bytes(a, z, bytes_of(p, s.len, s.len), n, err);
    return str_from_bytes(b.p, b.len);
}

static void TestBytes(TestingT *t) {
    test_string(t, "Bytes", bytes_f, NULL);
}

typedef struct AppendDst {
    bool nil;
    Int len, cap;
} AppendDst;

static Str append_f(Alloc *a, TransformTransformer z, Str s, Int *n, Error *err,
                    void *env) {
    const AppendDst *d = env;
    Slice dst = {NULL, 0, 0, TYPE_BYTE};
    if (!d->nil)
        dst = bytes_of(alloc_bytes(a, d->cap), d->len, d->cap);
    Slice b = burrow__transform_append(a, z, dst, str_bytes(s), n, err);
    if (b.len == d->len)
        return BURROW_S("");
    return str_from_bytes((const Byte *)b.p + d->len, b.len - d->len);
}

static void TestAppend(TestingT *t) {
    /* Create a bunch of subtests for different buffer sizes. */
    static const AppendDst cases[] = {
        {true, 0, 0},  {false, 0, 0},     {false, 0, 1},     {false, 1, 1},
        {false, 1, 5}, {false, 100, 100}, {false, 100, 200},
    };
    for (Int i = 0; i < LEN(cases); i++)
        test_string(t, "Append", append_f, (void *)(uintptr_t)&cases[i]);
}

static Str string_f(Alloc *a, TransformTransformer z, Str s, Int *n, Error *err,
                    void *env) {
    (void)env;
    return burrow__transform_string(a, z, s, n, err);
}

static void TestString(TestingT *t) {
    test_string(t, "String", string_f, NULL);

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    init_strings(a);

    /* Overrun the internal destination buffer. */
    const Str dst_tests[] = {
        prefix(aaa, 1 * INITIAL_BUF_SIZE - 1),
        prefix(aaa, 1 * INITIAL_BUF_SIZE + 0),
        prefix(aaa, 1 * INITIAL_BUF_SIZE + 1),
        prefix(AAA, 1 * INITIAL_BUF_SIZE - 1),
        prefix(AAA, 1 * INITIAL_BUF_SIZE + 0),
        prefix(AAA, 1 * INITIAL_BUF_SIZE + 1),
        prefix(AAA, 2 * INITIAL_BUF_SIZE - 1),
        prefix(AAA, 2 * INITIAL_BUF_SIZE + 0),
        prefix(AAA, 2 * INITIAL_BUF_SIZE + 1),
        cat(a, prefix(aaa, 1 * INITIAL_BUF_SIZE - 2), S("A")),
        cat(a, prefix(aaa, 1 * INITIAL_BUF_SIZE - 1), S("A")),
        cat(a, prefix(aaa, 1 * INITIAL_BUF_SIZE + 0), S("A")),
        cat(a, prefix(aaa, 1 * INITIAL_BUF_SIZE + 1), S("A")),
    };
    for (Int i = 0; i < LEN(dst_tests); i++) {
        Str s = dst_tests[i];
        Int n = 0;
        Error err = BURROW_NO_ERROR;
        Str got = burrow__transform_string(a, lower_case_ascii(), s, &n, &err);
        Str want = to_lower(a, s);
        if (!str_eq(got, want))
            testing_t_errorf_v(
                t, "dst buffer test using lower/%d: got %s (%d); want %s (%d)", i, got,
                got.len, want, want.len);
    }

    /* Overrun the internal source buffer. */
    const Str src_tests[] = {
        prefix(aaa, 1 * INITIAL_BUF_SIZE - 1), prefix(aaa, 1 * INITIAL_BUF_SIZE + 0),
        prefix(aaa, 1 * INITIAL_BUF_SIZE + 1), prefix(aaa, 2 * INITIAL_BUF_SIZE + 1),
        prefix(aaa, 2 * INITIAL_BUF_SIZE + 0), prefix(aaa, 2 * INITIAL_BUF_SIZE + 1),
    };
    for (Int i = 0; i < LEN(src_tests); i++) {
        Str s = src_tests[i];
        Int n = 0;
        Error err = BURROW_NO_ERROR;
        Str got = burrow__transform_string(a, rle_encode(false), s, &n, &err);
        Byte w[32];
        Int k = itoa(w, s.len);
        w[k++] = 'a';
        Str want = str_from_bytes(w, k);
        if (!str_eq(got, want))
            testing_t_errorf_v(
                t, "src buffer test using rleEncode/%d: got %s (%d); want %s (%d)", i,
                got, got.len, want, want.len);
    }
    arena_free(&ar);
}

/* TestBytesAllocation and TestStringAllocation test that buffer growth stays
 * limited with the trickler transformer, which behaves oddly but within spec.
 * In case buffer growth is not correctly handled, the test will either panic
 * with a failed allocation or thrash. Go runs them with a timeout of three
 * seconds and the test binary's own timeout does that job here. */
static void TestBytesAllocation(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    init_strings(a);
    Int n = 0;
    Error err = BURROW_NO_ERROR;
    Slice b = burrow__transform_bytes(a, trickler(a, false),
                                      str_bytes(prefix(aaa, 1000)), &n, &err);
    CHECK_INT_EQ(b.len, 1000);
    arena_free(&ar);
}

static void TestStringAllocation(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    init_strings(a);
    Int n = 0;
    Error err = BURROW_NO_ERROR;
    Str s =
        burrow__transform_string(a, trickler(a, false), prefix(aaa, 1000), &n, &err);
    CHECK_INT_EQ(s.len, 1000);
    arena_free(&ar);
}

static Int bad_transform(void *self, Slice dst, Slice src, bool at_eof, Int *n_src,
                         Error *err) {
    (void)self;
    (void)dst;
    (void)src;
    (void)at_eof;
    *n_src = 0;
    *err = burrow__transform_err_short_src;
    return 0;
}

static const TransformTransformerVT bad_vt = {&test_desc, bad_transform, nop_reset};

static void TestBadTransformer(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TransformTransformer bt = {&bad_vt, NULL};
    Int n = 0;
    Error err = BURROW_NO_ERROR;
    burrow__transform_string(a, bt, S("aaa"), &n, &err);
    if (!burrow__transform_err_eq(err, *err_short_src))
        testing_t_errorf_v(t, "String expected ErrShortSrc, got %v", err);
    Byte aaa3[3] = {'a', 'a', 'a'};
    burrow__transform_bytes(a, bt, bytes_of(aaa3, 3, 3), &n, &err);
    if (!burrow__transform_err_eq(err, *err_short_src))
        testing_t_errorf_v(t, "Bytes expected ErrShortSrc, got %v", err);
    StrReader sr = {S("aaa")};
    IoReader src = {&str_reader_vt, &sr};
    TransformReader *r = burrow__transform_new_reader(a, src, bt);
    burrow__transform_reader_read(r, bytes_of(NULL, 0, 0), &err);
    if (!burrow__transform_err_eq(err, *err_short_src))
        testing_t_errorf_v(t, "NewReader Read expected ErrShortSrc, got %v", err);
    arena_free(&ar);
}

static bool is_space(void *env, Rune r) {
    (void)env;
    return unicode_is_space(r);
}

static bool not_latin(void *env, Rune r) {
    (void)env;
    return !unicode_is(unicode_latin, r);
}

static void TestExampleRemoveFunc(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str in = S("tsch\xC3\xBC\xC3\x9F; \xD0\xB4\xD0\xBE "
               "\xD1\x81\xD0\xB2\xD0\xB8\xD0\xB4\xD0\xB0\xD0\xBD"
               "\xD0\xB8\xD1\x8F");
    Slice input = str_bytes(in);
    Byte *b = alloc_bytes(a, in.len);
    Slice dst = bytes_of(b, in.len, in.len);
    Buf out = {a, NULL, 0, 0};
    Int n_src = 0;
    Error err = BURROW_NO_ERROR;

    TransformTransformer tr =
        burrow__transform_remove_func(a, BURROW_FN(RuneFunc, is_space, NULL));
    Int n = burrow__transform_call(tr, dst, input, true, &n_src, &err);
    buf_write(&out, b, n);
    buf_write(&out, "\n", 1);

    tr = burrow__transform_remove_func(a, BURROW_FN(RuneFunc, not_latin, NULL));
    n = burrow__transform_call(tr, dst, input, true, &n_src, &err);
    buf_write(&out, b, n);
    buf_write(&out, "\n", 1);

    n = burrow__transform_call(tr, dst, burrow__norm_bytes(a, NORM_NFD, input), true,
                               &n_src, &err);
    buf_write(&out, b, n);
    buf_write(&out, "\n", 1);

    Str want = S("tsch\xC3\xBC\xC3\x9F;\xD0\xB4\xD0\xBE\xD1\x81\xD0\xB2\xD0\xB8\xD0\xB4"
                 "\xD0\xB0\xD0\xBD\xD0\xB8\xD1\x8F\n"
                 "tsch\xC3\xBC\xC3\x9F\n"
                 "tschu\xC3\x9F\n");
    if (!str_eq(buf_str(&out), want))
        testing_t_errorf_v(t, "got\n%s\nwant\n%s", buf_str(&out), want);
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestReader)                                                                      \
    X(TestWriter)                                                                      \
    X(TestNop)                                                                         \
    X(TestDiscard)                                                                     \
    X(TestChain)                                                                       \
    X(TestRemoveFunc)                                                                  \
    X(TestBytes)                                                                       \
    X(TestAppend)                                                                      \
    X(TestString)                                                                      \
    X(TestBytesAllocation)                                                             \
    X(TestStringAllocation)                                                            \
    X(TestBadTransformer)                                                              \
    X(TestExampleRemoveFunc)

TESTING_MAIN(TESTS)

/* Derived from golang.org/x/text v0.37.0's secure/bidirule tests,
 * bidirule_test.go. That is the version Go 1.27.1 vendors.
 *
 * Go's test cases come from tests/xtext_bidirule_test_gen.h, which
 * tools/gen-xtext-bidi.sh writes from the Go package itself, along with hashes
 * of what Go says about a corpus of labels and about every rune.
 * TestDifferential and TestSweep work out the same records here and compare.
 * describe and sweep_record below have to stay in step with the generator. Set
 * BURROW_XTEXT_SHOW to see the first record that differs.
 *
 * Copyright 2016 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/xtext/bidi.h"
#include "../src/xtext/bidirule.h"
#include "../src/xtext/transform.h"

#include "burrow/burrow.h"
#include "burrow/mem/heap.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct BidiruleTest {
    Int rule;
    const char *in;
    Int len;
    BidiDirection dir;
    Int n;
    Int err;
    Int p_src;
    Int sz_dst;
    Int n_src;
    Int err0;
} BidiruleTest;

typedef struct BidiruleCorpus {
    const char *s;
    Int len;
    uint64_t hash;
} BidiruleCorpus;

#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Woverlength-strings"
#endif
#include "xtext_bidirule_test_gen.h"

#define LEN(x) ((Int)(sizeof(x) / sizeof((x)[0])))

static Slice bytes_of(const void *p, Int len) {
    Slice s = {(void *)(uintptr_t)p, len, len, TYPE_BYTE};
    return s;
}

/* The numbers the generator gives the errors. */
static Int err_code(Error err) {
    if (!BURROW_FAILED(err))
        return 0;
    if (errors_is(err, burrow__bidirule_err_invalid))
        return 1;
    if (burrow__transform_err_eq(err, burrow__transform_err_short_dst))
        return 2;
    if (burrow__transform_err_eq(err, burrow__transform_err_short_src))
        return 3;
    return -1;
}

/* ----------------------------------------------------------- bidirule_test.go */

static Str name_of(const BidiruleTest *tc) {
    return str_from_bytes(tc->in, tc->len);
}

static void TestDirection(TestingT *t) {
    for (Int i = 0; i < LEN(bidirule_tests); i++) {
        const BidiruleTest *tc = &bidirule_tests[i];
        BidiDirection dir = burrow__bidirule_direction(bytes_of(tc->in, tc->len));
        if (dir != tc->dir)
            testing_t_errorf_v(t, "%d/%d:%+q: dir was %d; want %d", tc->rule, i,
                               name_of(tc), dir, tc->dir);
    }
}

static void TestDirectionString(TestingT *t) {
    for (Int i = 0; i < LEN(bidirule_tests); i++) {
        const BidiruleTest *tc = &bidirule_tests[i];
        BidiDirection dir = burrow__bidirule_direction_string(name_of(tc));
        if (dir != tc->dir)
            testing_t_errorf_v(t, "%d/%d:%+q: dir was %d; want %d", tc->rule, i,
                               name_of(tc), dir, tc->dir);
    }
}

static void TestValid(TestingT *t) {
    for (Int i = 0; i < LEN(bidirule_tests); i++) {
        const BidiruleTest *tc = &bidirule_tests[i];
        bool want = tc->err == 0;
        bool got = burrow__bidirule_valid(bytes_of(tc->in, tc->len));
        if (got != want)
            testing_t_errorf_v(t, "%d/%d:%+q: Valid: got %t; want %t", tc->rule, i,
                               name_of(tc), got, want);
        got = burrow__bidirule_valid_string(name_of(tc));
        if (got != want)
            testing_t_errorf_v(t, "%d/%d:%+q: ValidString: got %t; want %t", tc->rule,
                               i, name_of(tc), got, want);
    }
}

static void TestSpan(TestingT *t) {
    for (Int i = 0; i < LEN(bidirule_tests); i++) {
        const BidiruleTest *tc = &bidirule_tests[i];
        /* Skip tests that test for limited destination buffer size. */
        if (tc->sz_dst > 0)
            continue;
        BidiruleTransformer r = burrow__bidirule_new();
        Error err;
        Int n = burrow__bidirule_span(&r, bytes_of(tc->in, tc->p_src),
                                      tc->p_src == tc->len, &err);
        if (err_code(err) != tc->err0)
            testing_t_errorf_v(t, "%d/%d:%+q: err0 was %v; want %d", tc->rule, i,
                               name_of(tc), err, tc->err0);
        if (n != tc->n_src) {
            testing_t_errorf_v(t, "%d/%d:%+q: nSrc was %d; want %d", tc->rule, i,
                               name_of(tc), n, tc->n_src);
            continue;
        }
        n = burrow__bidirule_span(&r, bytes_of(tc->in + n, tc->len - n), true, &err);
        if (err_code(err) != tc->err)
            testing_t_errorf_v(t, "%d/%d:%+q: error was %v; want %d", tc->rule, i,
                               name_of(tc), err, tc->err);
        if (n + tc->n_src != tc->n)
            testing_t_errorf_v(t, "%d/%d:%+q: n was %d; want %d", tc->rule, i,
                               name_of(tc), n + tc->n_src, tc->n);
    }
}

static void TestTransform(TestingT *t) {
    for (Int i = 0; i < LEN(bidirule_tests); i++) {
        const BidiruleTest *tc = &bidirule_tests[i];
        BidiruleTransformer r = burrow__bidirule_new();
        Int ndst_buf = tc->sz_dst > 0 ? tc->sz_dst : tc->len;
        /* From the heap, so that the sanitisers see a write past the end. */
        Alloc *a = heap_allocator();
        size_t ndst = (size_t)(ndst_buf > 0 ? ndst_buf : 1);
        size_t ndst1 = (size_t)(tc->len > 0 ? tc->len : 1);
        Byte *dst = mem_alloc_nozero(a, ndst, 1);
        Byte *dst1 = mem_alloc_nozero(a, ndst1, 1);

        /* First transform operates on a zero-length string for most tests. */
        Error err;
        Int n_src = 0;
        Int n_dst = burrow__bidirule_transform(&r, bytes_of(dst, ndst_buf),
                                               bytes_of(tc->in, tc->p_src),
                                               tc->p_src == tc->len, &n_src, &err);
        if (err_code(err) != tc->err0)
            testing_t_errorf_v(t, "%d/%d:%+q: err0 was %v; want %d", tc->rule, i,
                               name_of(tc), err, tc->err0);
        if (n_dst != n_src || n_src != tc->n_src) {
            testing_t_errorf_v(t, "%d/%d:%+q: nDst %d, nSrc %d; want both %d", tc->rule,
                               i, name_of(tc), n_dst, n_src, tc->n_src);
            goto next;
        }
        if (n_dst > 0)
            memcpy(dst1, dst, (size_t)n_dst);

        Int n_src0 = n_src;
        n_dst = burrow__bidirule_transform(&r, bytes_of(dst1 + n_dst, tc->len - n_dst),
                                           bytes_of(tc->in + n_src, tc->len - n_src),
                                           true, &n_src, &err);
        if (err_code(err) != tc->err)
            testing_t_errorf_v(t, "%d/%d:%+q: error was %v; want %d", tc->rule, i,
                               name_of(tc), err, tc->err);
        if (n_dst != n_src) {
            testing_t_errorf_v(t, "%d/%d:%+q: nDst (%d) and nSrc (%d) should match",
                               tc->rule, i, name_of(tc), n_dst, n_src);
            goto next;
        }
        Int n = n_src + n_src0;
        if (n != tc->n) {
            testing_t_errorf_v(t, "%d/%d:%+q: n was %d; want %d", tc->rule, i,
                               name_of(tc), n, tc->n);
            goto next;
        }
        if (n > 0 && memcmp(dst1, tc->in, (size_t)n) != 0)
            testing_t_errorf_v(t, "%d/%d:%+q: got %+q; want %+q", tc->rule, i,
                               name_of(tc), str_from_bytes(dst1, n),
                               str_from_bytes(tc->in, n));
    next:
        mem_free(a, dst, ndst, 1);
        mem_free(a, dst1, ndst1, 1);
    }
}

/* The transformer through the interface the transform package uses. */
static void TestTransformer(TestingT *t) {
    BidiruleTransformer r = burrow__bidirule_new();
    TransformSpanningTransformer st = burrow__bidirule_transformer(&r);
    Byte dst[8];
    Error err;
    Int n_src = 0;
    Int n = st.vt->transformer.transform(st.data, bytes_of(dst, 8),
                                         bytes_of("\xD7\x90"
                                                  "a",
                                                  3),
                                         true, &n_src, &err);
    if (n != 2 || n_src != 2 || err_code(err) != 1)
        testing_t_errorf_v(t, "Transform = %d, %d, %v; want 2, 2, ErrInvalid", n, n_src,
                           err);
    st.vt->transformer.reset(st.data);
    n = st.vt->span(st.data, bytes_of("abc", 3), true, &err);
    if (n != 3 || BURROW_FAILED(err))
        testing_t_errorf_v(t, "Span = %d, %v; want 3, nil", n, err);
}

/* ------------------------------------------------------------- differential */

typedef struct Rec {
    uint64_t h;
    Byte show[4096];
    Int show_len;
} Rec;

static void rec_init(Rec *r) {
    r->h = 14695981039346656037ULL;
    r->show_len = 0;
}

static void rec_write(Rec *r, const void *p, Int n) {
    const Byte *b = p;
    for (Int i = 0; i < n; i++) {
        r->h ^= b[i];
        r->h *= 1099511628211ULL;
    }
    Int room = (Int)sizeof r->show - r->show_len;
    Int k = n < room ? n : room;
    if (k > 0) {
        memcpy(r->show + r->show_len, b, (size_t)k);
        r->show_len += k;
    }
}

static void rec_cstr(Rec *r, const char *s) {
    rec_write(r, s, (Int)strlen(s));
}

static void rec_int(Rec *r, Int n) {
    char tmp[24];
    int i = (int)sizeof tmp;
    uint64_t u = n < 0 ? (uint64_t)0 - (uint64_t)n : (uint64_t)n;
    do {
        tmp[--i] = (char)('0' + u % 10);
        u /= 10;
    } while (u != 0);
    if (n < 0)
        tmp[--i] = '-';
    rec_write(r, tmp + i, (Int)sizeof tmp - i);
}

static void rec_n(Rec *r, const char *tag, Int n) {
    rec_cstr(r, tag);
    rec_int(r, n);
    rec_cstr(r, ";");
}

static void rec_bool(Rec *r, const char *tag, bool b) {
    rec_n(r, tag, b ? 1 : 0);
}

static void rec_s(Rec *r, const char *tag, const void *p, Int n) {
    rec_cstr(r, tag);
    rec_int(r, n);
    rec_cstr(r, ":");
    rec_write(r, p, n);
}

static void rec_e(Rec *r, const char *tag, Error err) {
    rec_cstr(r, tag);
    if (!BURROW_FAILED(err)) {
        rec_cstr(r, "-;");
        return;
    }
    Str s = error_text(err);
    rec_write(r, s.p, s.len);
    rec_cstr(r, ";");
}

static const Int dst_sizes[] = {0, 1, 2, 3, 5, 8, 64};

static void describe(Rec *r, Str s) {
    const Byte *b = s.p;
    rec_n(r, "D", burrow__bidirule_direction(bytes_of(b, s.len)));
    rec_n(r, "DS", burrow__bidirule_direction_string(s));
    rec_bool(r, "V", burrow__bidirule_valid(bytes_of(b, s.len)));
    rec_bool(r, "VS", burrow__bidirule_valid_string(s));

    Error err;
    for (int eof = 0; eof < 2; eof++) {
        BidiruleTransformer t = burrow__bidirule_new();
        Int n = burrow__bidirule_span(&t, bytes_of(b, s.len), eof, &err);
        rec_n(r, "SP", n);
        rec_e(r, "", err);
        /* Again, without a reset, which carries the state over. */
        n = burrow__bidirule_span(&t, bytes_of(b, s.len), eof, &err);
        rec_n(r, "SP2", n);
        rec_e(r, "", err);
    }

    /* Split in two, the way Go's TestSpan and TestTransform do it. */
    for (Int i = 0; i <= s.len; i++) {
        BidiruleTransformer t = burrow__bidirule_new();
        Int n = burrow__bidirule_span(&t, bytes_of(b, i), i == s.len, &err);
        rec_n(r, "S", n);
        rec_e(r, "", err);
        Int n2 = burrow__bidirule_span(
            &t, bytes_of(b == NULL ? NULL : b + n, s.len - n), true, &err);
        rec_n(r, "", n2);
        rec_e(r, "", err);
    }

    for (Int k = 0; k < LEN(dst_sizes); k++) {
        Int sz = dst_sizes[k];
        size_t ndst = (size_t)(sz > 0 ? sz : 1);
        Byte *dst = mem_alloc_nozero(heap_allocator(), ndst, 1);
        for (int eof = 0; eof < 2; eof++) {
            BidiruleTransformer t = burrow__bidirule_new();
            Int n_src = 0;
            Int n_dst = burrow__bidirule_transform(
                &t, bytes_of(dst, sz), bytes_of(b, s.len), eof, &n_src, &err);
            rec_n(r, "T", n_dst);
            rec_n(r, "", n_src);
            rec_e(r, "", err);
            rec_s(r, "", dst, n_dst);
            burrow__bidirule_reset(&t);
            n_dst = burrow__bidirule_transform(&t, bytes_of(dst, sz),
                                               bytes_of(b, s.len), eof, &n_src, &err);
            rec_n(r, "TR", n_dst);
            rec_n(r, "", n_src);
            rec_e(r, "", err);
        }
        mem_free(heap_allocator(), dst, ndst, 1);
    }
}

static void show(TestingT *t, const Rec *r) {
    if (getenv("BURROW_XTEXT_SHOW") != NULL)
        testing_t_logf_v(t, "record starts %q", str_from_bytes(r->show, r->show_len));
}

static void TestDifferential(TestingT *t) {
    int shown = 0;
    Rec *r = mem_alloc(heap_allocator(), sizeof *r, _Alignof(Rec));
    for (Int i = 0; i < LEN(bidirule_corpus); i++) {
        Str s = str_from_bytes(bidirule_corpus[i].s, bidirule_corpus[i].len);
        rec_init(r);
        describe(r, s);
        if (r->h != bidirule_corpus[i].hash) {
            testing_t_errorf_v(t, "corpus %d %+q: differs from Go", i, s);
            if (shown++ == 0)
                show(t, r);
        }
    }
    mem_free(heap_allocator(), r, sizeof *r, _Alignof(Rec));
}

/* -------------------------------------------------------------------- sweep */

/* The labels the sweep checks each rune in: alone, after an L, after an R,
 * before a European number, and between an R and an Arabic number. */
static void sweep_record(Rec *rec, Rune r) {
    Byte e[4];
    Int n = utf8_encode_rune(bytes_of(e, 4), r);
    Byte s[16];
    const struct {
        const char *pre;
        Int npre;
        const char *post;
        Int npost;
    } forms[] = {
        {"", 0, "", 0},
        {"a", 1, "", 0},
        {"\xD7\x90", 2, "", 0},
        {"", 0, "1", 1},
        {"\xD7\x90", 2, "\xD9\xA0", 2},
    };
    for (Int i = 0; i < LEN(forms); i++) {
        Int len = 0;
        memcpy(s, forms[i].pre, (size_t)forms[i].npre);
        len += forms[i].npre;
        memcpy(s + len, e, (size_t)n);
        len += n;
        memcpy(s + len, forms[i].post, (size_t)forms[i].npost);
        len += forms[i].npost;
        Str str = str_from_bytes(s, len);
        rec_n(rec, "D", burrow__bidirule_direction_string(str));
        rec_bool(rec, "", burrow__bidirule_valid_string(str));
    }
}

static void TestSweep(TestingT *t) {
    Rec rec;
    for (Rune blk = 0; blk < 0x110; blk++) {
        rec_init(&rec);
        for (Rune r = blk << 12; r < (blk + 1) << 12; r++) {
            if (r >= 0xd800 && r < 0xe000)
                continue;
            sweep_record(&rec, r);
            rec.show_len = 0;
        }
        if (rec.h != bidirule_sweep_hash[blk])
            testing_t_errorf_v(t, "block %X: differs from Go", blk);
    }
}

#define TESTS(X)                                                                       \
    X(TestDirection)                                                                   \
    X(TestDirectionString)                                                             \
    X(TestValid)                                                                       \
    X(TestSpan)                                                                        \
    X(TestTransform)                                                                   \
    X(TestTransformer)                                                                 \
    X(TestDifferential)                                                                \
    X(TestSweep)

TESTING_MAIN(TESTS)

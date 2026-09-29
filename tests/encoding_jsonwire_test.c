/* Derived from Go's src/encoding/json/internal/jsonwire/decode_test.go,
 * encode_test.go and wire_test.go. Go source: go1.27.1.
 *
 * tests/encoding_jsonwire_test_gen.h, from tools/gen-jsonwire-tests.sh, holds
 * what Go's jsonwire does with Go's own tables: how far each consumer got, the
 * flags it saw, every error text, and what the quoting and number formatting
 * wrote. The cases here make the same calls and have to match byte for byte.
 *
 * Copyright 2022 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/encoding/json_internal.h"

#include "burrow/burrow.h"
#include "burrow/crypto/sha256.h"
#include "burrow/hash.h"
#include "burrow/math.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"

#include "encoding_jsonwire_test_gen.h"

#include <string.h>

#define LEN(x) (sizeof(x) / sizeof((x)[0]))

static Str qstr(QStr q) {
    return (Str){(const Byte *)q.p, (Int)q.n};
}

static const Byte *qp(QStr q) {
    return (const Byte *)q.p;
}

/* The whole of q, with any repeated unit written out into the arena. */
static Str expand(QStr q, Arena *ar) {
    if (q.unit == NULL)
        return qstr(q);
    size_t nu = strlen(q.unit);
    Int n = (Int)q.n + (Int)nu * (Int)q.reps;
    Byte *p = mem_alloc(arena_allocator(ar), (size_t)n, 1);
    if (p == NULL)
        return (Str){NULL, 0};
    memcpy(p, q.p, (size_t)q.n);
    for (long long k = 0; k < q.reps; k++)
        memcpy(p + q.n + k * (long long)nu, q.unit, nu);
    return (Str){p, n};
}

static bool text_is(Str s, const char *want) {
    size_t n = strlen(want);
    return s.len == (Int)n && (n == 0 || memcmp(s.p, want, n) == 0);
}

static bool buf_is(const JsonBuf *b, QStr want) {
    return !b->failed && b->len == (Int)want.n &&
           (want.n == 0 || memcmp(b->p, want.p, (size_t)want.n) == 0);
}

/* An empty want means no error. */
static bool err_is(Error err, const char *want) {
    if (!BURROW_FAILED(err))
        return want[0] == 0;
    return text_is(error_text(err), want);
}

static Str buf_str(const JsonBuf *b) {
    return (Str){b->p, b->len};
}

static JsonBuf new_buf(Arena *ar) {
    JsonBuf b = {NULL, 0, 0, arena_allocator(ar), true, false};
    return b;
}

static void TestConsumeWhitespace(TestingT *t) {
    for (size_t i = 0; i < LEN(jw_whitespace_cases); i++) {
        const JwWhitespaceCase *c = &jw_whitespace_cases[i];
        Int got = jsonwire_consume_whitespace(qp(c->in), (Int)c->in.n);
        if (got != c->got)
            testing_t_errorf_v(t, "ConsumeWhitespace(%q) = %d, want %d", qstr(c->in),
                               got, c->got);
    }
}

static void TestConsumeLiteral(TestingT *t) {
    for (size_t i = 0; i < LEN(jw_literal_cases); i++) {
        const JwLiteralCase *c = &jw_literal_cases[i];
        const Byte *b = qp(c->in);
        Int n = (Int)c->in.n, simple = 0;
        switch (c->literal.p[0]) {
        case 'n':
            simple = jsonwire_consume_null(b, n);
            break;
        case 'f':
            simple = jsonwire_consume_false(b, n);
            break;
        default:
            simple = jsonwire_consume_true(b, n);
        }
        if (simple != c->simple)
            testing_t_errorf_v(t, "Consume%s(%q) = %d, want %d", qstr(c->literal),
                               qstr(c->in), simple, c->simple);
        Error err = BURROW_NO_ERROR;
        Int got = burrow__jsonwire_consume_literal(b, n, qstr(c->literal), &err);
        if (got != c->got || !err_is(err, c->err))
            testing_t_errorf_v(t, "ConsumeLiteral(%q, %q) = (%d, %v), want (%d, %s)",
                               qstr(c->in), qstr(c->literal), got, err, c->got, c->err);
    }
}

static void TestConsumeString(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    for (size_t i = 0; i < LEN(jw_string_cases); i++) {
        const JwStringCase *c = &jw_string_cases[i];
        const Byte *b = qp(c->in);
        Int n = (Int)c->in.n;
        Int simple = jsonwire_consume_simple_string(b, n);
        if (simple != c->simple)
            testing_t_errorf_v(t, "ConsumeSimpleString(%q) = %d, want %d", qstr(c->in),
                               simple, c->simple);
        for (int validate = 0; validate <= 1; validate++) {
            unsigned flags = 0, want_flags = validate ? c->flags8 : c->flags;
            Int want = validate ? c->got8 : c->got;
            const char *want_err = validate ? c->err8 : c->err;
            Error err = BURROW_NO_ERROR;
            Int got = burrow__jsonwire_consume_string_resumable(&flags, b, n, 0,
                                                                validate != 0, &err);
            if (got != want || flags != want_flags || !err_is(err, want_err))
                testing_t_errorf_v(t,
                                   "ConsumeString(%q, %v) = (%d, %v) with flags %d, "
                                   "want (%d, %s) with flags %d",
                                   qstr(c->in), validate != 0, got, err, (int)flags,
                                   want, want_err, (int)want_flags);
        }
        JsonBuf dst = new_buf(&ar);
        Error err = burrow__jsonwire_append_unquote(&dst, b, n);
        if (!buf_is(&dst, c->unquoted) || !err_is(err, c->unquote_err))
            testing_t_errorf_v(t, "AppendUnquote(nil, %q) = (%q, %v), want (%q, %s)",
                               qstr(c->in), buf_str(&dst), err, qstr(c->unquoted),
                               c->unquote_err);
        arena_reset(&ar);
    }
    arena_free(&ar);
}

static void TestConsumeNumber(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    for (size_t i = 0; i < LEN(jw_number_cases); i++) {
        const JwNumberCase *c = &jw_number_cases[i];
        Str in = expand(c->in, &ar);
        const Byte *b = in.p;
        Int n = in.len;
        Int simple = jsonwire_consume_simple_number(b, n);
        if (simple != c->simple)
            testing_t_errorf_v(t, "ConsumeSimpleNumber(%q) = %d, want %d", qstr(c->in),
                               simple, c->simple);
        Error err = BURROW_NO_ERROR;
        Int got = burrow__jsonwire_consume_number(b, n, &err);
        if (got != c->got || !err_is(err, c->err))
            testing_t_errorf_v(t, "ConsumeNumber(%q) = (%d, %v), want (%d, %s)",
                               qstr(c->in), got, err, c->got, c->err);
    }
    arena_free(&ar);
}

/* ConsumeNumberResumable stops at the end of what it has been given, either
 * with io.ErrUnexpectedEOF or, inside a run of digits, with no error at all,
 * and picks up from there when more turns up. Feeding every case one byte at
 * a time has to land in the same place as feeding it whole. */
static void TestConsumeNumberResumable(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    for (size_t i = 0; i < LEN(jw_number_cases); i++) {
        const JwNumberCase *c = &jw_number_cases[i];
        Str in = expand(c->in, &ar);
        const Byte *b = in.p;
        Int n = in.len, got = 0;
        int state = JSONWIRE_NUMBER_INIT;
        Error err = BURROW_NO_ERROR;
        for (Int k = 0; k <= n; k++) {
            err = BURROW_NO_ERROR;
            got = burrow__jsonwire_consume_number_resumable(b, k, got, &state, &err);
            bool more = errors_is(err, io_err_unexpected_eof) ||
                        (!BURROW_FAILED(err) && got == k);
            if (!more)
                break;
        }
        if (got != c->got || !err_is(err, c->err))
            testing_t_errorf_v(t,
                               "resumable ConsumeNumber(%q) = (%d, %v), want (%d, %s)",
                               qstr(c->in), got, err, c->got, c->err);
    }
    arena_free(&ar);
}

static void TestParseHexUint16(TestingT *t) {
    for (size_t i = 0; i < LEN(jw_hex_cases); i++) {
        const JwParseCase *c = &jw_hex_cases[i];
        uint16_t v = 0;
        bool ok = burrow__jsonwire_parse_hex_uint16(qp(c->in), (Int)c->in.n, &v);
        if (v != c->got || ok != (c->ok != 0))
            testing_t_errorf_v(t, "parseHexUint16(%q) = (%d, %v), want (%d, %v)",
                               qstr(c->in), (int)v, ok, (int)c->got, c->ok != 0);
    }
}

static void TestParseUint(TestingT *t) {
    for (size_t i = 0; i < LEN(jw_uint_cases); i++) {
        const JwParseCase *c = &jw_uint_cases[i];
        uint64_t v = 0;
        bool ok = burrow__jsonwire_parse_uint(qp(c->in), (Int)c->in.n, &v);
        if (v != c->got || ok != (c->ok != 0))
            testing_t_errorf_v(t, "ParseUint(%q) = (%d, %v), want (%d, %v)",
                               qstr(c->in), v, ok, (uint64_t)c->got, c->ok != 0);
    }
}

static void TestAppendQuote(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    for (size_t i = 0; i < LEN(jw_quote_cases); i++) {
        const JwQuoteCase *c = &jw_quote_cases[i];
        JsontextOptions flags;
        memset(&flags, 0, sizeof(flags));
        jsonflags_set(&flags, c->flags | 1);
        jsonflags_set(&flags, JSONFLAG_ALLOW_INVALID_UTF8 | 1);
        JsonBuf dst = new_buf(&ar);
        Error err =
            burrow__jsonwire_append_quote(&dst, qp(c->in), (Int)c->in.n, &flags);
        if (!buf_is(&dst, c->got) || !err_is(err, c->err))
            testing_t_errorf_v(t, "AppendQuote(nil, %q) = (%q, %v), want (%q, %s)",
                               qstr(c->in), buf_str(&dst), err, qstr(c->got), c->err);
        jsonflags_set(&flags, JSONFLAG_ALLOW_INVALID_UTF8 | 0);
        dst = new_buf(&ar);
        err = burrow__jsonwire_append_quote(&dst, qp(c->in), (Int)c->in.n, &flags);
        if (!buf_is(&dst, c->got8) || !err_is(err, c->err8))
            testing_t_errorf_v(
                t, "AppendQuote(nil, %q) with UTF-8 checks = (%q, %v), want (%q, %s)",
                qstr(c->in), buf_str(&dst), err, qstr(c->got8), c->err8);
        arena_reset(&ar);
    }
    arena_free(&ar);
}

static void TestAppendNumber(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    for (size_t i = 0; i < LEN(jw_float_cases); i++) {
        const JwFloatCase *c = &jw_float_cases[i];
        double f = math_float64frombits(c->bits);
        JsonBuf dst = new_buf(&ar);
        burrow__jsonwire_append_float(&dst, f, 32);
        if (!buf_is(&dst, c->got32))
            testing_t_errorf_v(t, "AppendFloat(%v, 32) = %q, want %q", f, buf_str(&dst),
                               qstr(c->got32));
        dst = new_buf(&ar);
        burrow__jsonwire_append_float(&dst, f, 64);
        if (!buf_is(&dst, c->got64))
            testing_t_errorf_v(t, "AppendFloat(%v, 64) = %q, want %q", f, buf_str(&dst),
                               qstr(c->got64));
        arena_reset(&ar);
    }
    arena_free(&ar);
}

/* The float64 bits TestCanonicalNumber formats next: the fixed list, then 2000
 * subnormal neighbours, then 8 byte little-endian chunks of a SHA-256 chain,
 * skipping zero, NaN and the infinities. */
typedef struct CanonicalGen {
    size_t idx;
    Byte block[SHA256_SIZE];
    Int left;
} CanonicalGen;

static double canonical_next(CanonicalGen *g) {
    enum { NUM_SERIAL = 2000 };
    size_t nstatic = LEN(jw_canonical_static);
    double f = 0;
    if (g->idx < nstatic) {
        f = math_float64frombits(jw_canonical_static[g->idx]);
    } else if (g->idx < nstatic + NUM_SERIAL) {
        f = math_float64frombits(0x0010000000000000ULL + (uint64_t)(g->idx - nstatic));
    } else {
        while (f == 0 || math_is_nan(f) || math_is_inf(f, 0)) {
            if (g->left == 0) {
                Sha256Sum256Ret s = sha256_sum256(
                    (Slice){g->block, SHA256_SIZE, SHA256_SIZE, TYPE_BYTE});
                memcpy(g->block, s.a, SHA256_SIZE);
                g->left = SHA256_SIZE;
            }
            const Byte *p = g->block + (SHA256_SIZE - g->left);
            uint64_t bits = 0;
            for (int k = 7; k >= 0; k--)
                bits = bits << 8 | p[k];
            f = math_float64frombits(bits);
            g->left -= 8;
        }
    }
    g->idx++;
    return f;
}

/* RFC 8785 number formatting, checked the way Go checks it offline: format
 * the first 1e4 numbers of the reference test file as "bits,text\n" lines and
 * compare the SHA-256 of the lot with the hash Go keeps for that many lines. */
static void TestCanonicalNumber(TestingT *t) {
    static const char want[] =
        "b9f7a8e75ef22a835685a52ccba7f7d6bdc99e34b010992cbc5864cd12be6892";
    enum { NUM_LINES = 10000 };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Hash h = sha256_new(arena_allocator(&ar));
    CanonicalGen g;
    memset(&g, 0, sizeof(g));
    JsonBuf line = new_buf(&ar);
    for (int n = 1; n <= NUM_LINES; n++) {
        double f = canonical_next(&g);
        line.len = 0;
        uint64_t bits = math_float64bits(f);
        Byte hex[16];
        int nh = 0;
        do {
            hex[nh++] = (Byte) "0123456789abcdef"[bits & 15];
            bits >>= 4;
        } while (bits != 0);
        while (nh > 0)
            jsonbuf_byte(&line, hex[--nh]);
        jsonbuf_byte(&line, ',');
        if (math_signbit(f) && f == 0)
            jsonbuf_byte(&line, '0');
        else
            burrow__jsonwire_append_float(&line, f, 64);
        jsonbuf_byte(&line, '\n');
        CHECK(!line.failed);
        hash_write(h, (Slice){line.p, line.len, line.cap, TYPE_BYTE}, NULL);
    }
    Slice sum = hash_sum(arena_allocator(&ar), h, (Slice){NULL, 0, 0, TYPE_BYTE});
    char got[2 * SHA256_SIZE + 1];
    for (size_t i = 0; i < (size_t)sum.len; i++) {
        Byte c = ((const Byte *)sum.p)[i];
        got[2 * i] = "0123456789abcdef"[c >> 4];
        got[2 * i + 1] = "0123456789abcdef"[c & 15];
    }
    got[sizeof(got) - 1] = 0;
    if (strcmp(got, want) != 0)
        testing_t_errorf_v(t, "canonical number hash over %d lines = %s, want %s",
                           NUM_LINES, got, want);
    arena_free(&ar);
}

static void TestQuoteRune(TestingT *t) {
    for (size_t i = 0; i < LEN(jw_quote_rune_cases); i++) {
        const JwPairCase *c = &jw_quote_rune_cases[i];
        Byte out[16];
        Str got = burrow__jsonwire_quote_rune(qp(c->in), (Int)c->in.n, out);
        if (!str_eq(got, qstr(c->got)))
            testing_t_errorf_v(t, "quoteRune(%q) = %s, want %s", qstr(c->in), got,
                               qstr(c->got));
    }
}

/* Go's list is in UTF-16 order, so every pair has to compare the way the
 * indexes do. */
static void TestCompareUTF16(TestingT *t) {
    size_t n = LEN(jw_compare_data);
    for (size_t i = 0; i < n; i++) {
        for (size_t j = 0; j < n; j++) {
            QStr si = jw_compare_data[i], sj = jw_compare_data[j];
            int got =
                burrow__jsonwire_compare_utf16(qp(si), (Int)si.n, qp(sj), (Int)sj.n);
            int want = i < j ? -1 : i > j ? 1 : 0;
            if (got != want)
                testing_t_errorf_v(t, "CompareUTF16(%q, %q) = %d, want %d", qstr(si),
                                   qstr(sj), got, want);
        }
    }
    for (size_t i = 0; i < LEN(jw_compare_cases); i++) {
        const JwCompareCase *c = &jw_compare_cases[i];
        int got = burrow__jsonwire_compare_utf16(qp(c->x), (Int)c->x.n, qp(c->y),
                                                 (Int)c->y.n);
        if (got != c->got)
            testing_t_errorf_v(t, "CompareUTF16(%q, %q) = %d, want %d", qstr(c->x),
                               qstr(c->y), got, c->got);
    }
}

static void TestTruncatePointer(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    for (size_t i = 0; i < LEN(jw_truncate_cases); i++) {
        const JwPairCase *c = &jw_truncate_cases[i];
        JsonBuf dst = new_buf(&ar);
        burrow__jsonwire_truncate_pointer(&dst, qstr(c->in), 10);
        if (!buf_is(&dst, c->got))
            testing_t_errorf_v(t, "TruncatePointer(%s) = %s, want %s", qstr(c->in),
                               buf_str(&dst), qstr(c->got));
        arena_reset(&ar);
    }
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestConsumeWhitespace)                                                           \
    X(TestConsumeLiteral)                                                              \
    X(TestConsumeString)                                                               \
    X(TestConsumeNumber)                                                               \
    X(TestConsumeNumberResumable)                                                      \
    X(TestParseHexUint16)                                                              \
    X(TestParseUint)                                                                   \
    X(TestAppendQuote)                                                                 \
    X(TestAppendNumber)                                                                \
    X(TestCanonicalNumber)                                                             \
    X(TestQuoteRune)                                                                   \
    X(TestCompareUTF16)                                                                \
    X(TestTruncatePointer)

TESTING_MAIN(TESTS)

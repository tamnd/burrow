/* Derived from Go's src/mime/mediatype_test.go and encodedword_test.go. Go
 * source: go1.27.1.
 *
 * tests/mime_test_gen.h, from tools/gen-mime-tests.sh, holds what Go's mime
 * package makes of Go's own test tables, so every result here is checked
 * against Go's byte for byte, errors included. Past the tables, 20,000 random
 * media types, 20,000 random headers and 5,000 random strings to encode are
 * built from pieces here the same way the generator builds them, and a digest
 * of every result has to match the digest of Go's.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/mime.h"

#include "mime_test_gen.h"

#include <string.h>

static Str qstr(QStr q) {
    return (Str){(const Byte *)q.p, (Int)q.n};
}

static bool text_is(Str s, const char *want) {
    size_t n = strlen(want);
    return s.len == (Int)n && (n == 0 || memcmp(s.p, want, n) == 0);
}

static Str err_text(Error err) {
    return BURROW_FAILED(err) ? error_text(err) : (Str){NULL, 0};
}

/* The params map in name order, as pairs. */
static Slice sorted_pairs(Alloc *a, Map *m) {
    Int n = map_len(m);
    Slice keys = slice_make(a, TYPE_STRING, n, n);
    Str *kp = (Str *)keys.p;
    Int i = 0;
    const void *k;
    for (MapIter it = map_iter(m); map_next(&it, &k, NULL);)
        kp[i++] = *(const Str *)k;
    sort_strings(keys);
    Slice pairs = slice_make(a, TYPE_STRING, 2 * n, 2 * n);
    Str *pp = (Str *)pairs.p;
    for (i = 0; i < n; i++) {
        pp[2 * i] = kp[i];
        pp[2 * i + 1] = *(const Str *)map_get(m, &kp[i]);
    }
    return pairs;
}

/* A map of Str to Str from pairs. */
static Map *map_of(Alloc *a, const QStr *pairs, int n) {
    Map *m = map_make(a, TYPE_STRING, TYPE_STRING, n);
    for (int i = 0; i < n; i++) {
        Str k = qstr(pairs[2 * i]), v = qstr(pairs[2 * i + 1]);
        map_set(m, &k, &v);
    }
    return m;
}

static void TestParseMediaType(TestingT *t) {
    for (size_t i = 0; i < sizeof mime_parse_cases / sizeof mime_parse_cases[0]; i++) {
        const MimeParseCase *c = &mime_parse_cases[i];
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        Map *params = NULL;
        Error err;
        Str mt = mime_parse_media_type(a, qstr(c->in), &params, &err);
        if (!str_eq(mt, qstr(c->mediatype)) || !text_is(err_text(err), c->err))
            testing_t_errorf_v(t, "ParseMediaType(%q) = %q, %v, want %q, %s",
                               qstr(c->in), mt, err, qstr(c->mediatype),
                               str_from_cstr(c->err));
        if (c->err[0] != '\0') {
            CHECK(params == NULL);
        } else if (params == NULL) {
            testing_t_errorf_v(t, "ParseMediaType(%q): params is NULL", qstr(c->in));
        } else {
            Slice pairs = sorted_pairs(a, params);
            Str *pp = (Str *)pairs.p;
            if (pairs.len != 2 * c->nparams) {
                testing_t_errorf_v(t, "ParseMediaType(%q): %d params, want %d",
                                   qstr(c->in), (int)(pairs.len / 2), c->nparams);
            } else {
                for (Int j = 0; j < pairs.len; j++)
                    if (!str_eq(pp[j], qstr(c->params[j])))
                        testing_t_errorf_v(t, "ParseMediaType(%q): got %q, want %q",
                                           qstr(c->in), pp[j], qstr(c->params[j]));
            }
        }
        /* No params wanted is fine too. */
        Str mt2 = mime_parse_media_type(a, qstr(c->in), NULL, &err);
        CHECK(str_eq(mt2, mt) && text_is(err_text(err), c->err));
        arena_free(&ar);
    }
    Arena ar;
    arena_init(&ar, NULL, 0);
    Error err;
    Map *params;
    mime_parse_media_type(arena_allocator(&ar), BURROW_S("text/html; x"), &params,
                          &err);
    CHECK(errors_is(err, mime_err_invalid_media_parameter));
    arena_free(&ar);
}

static void TestFormatMediaType(TestingT *t) {
    for (size_t i = 0; i < sizeof mime_format_cases / sizeof mime_format_cases[0];
         i++) {
        const MimeFormatCase *c = &mime_format_cases[i];
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        Map *m = c->nparams == 0 ? NULL : map_of(a, c->params, c->nparams);
        Str got = mime_format_media_type(a, qstr(c->typ), m);
        if (!str_eq(got, qstr(c->want)))
            testing_t_errorf_v(t, "FormatMediaType(%q) = %q, want %q", qstr(c->typ),
                               got, qstr(c->want));
        arena_free(&ar);
    }
}

static void TestEncodeWord(TestingT *t) {
    for (size_t i = 0; i < sizeof mime_encode_cases / sizeof mime_encode_cases[0];
         i++) {
        const MimeEncodeCase *c = &mime_encode_cases[i];
        Arena ar;
        arena_init(&ar, NULL, 0);
        Str got =
            mime_word_encoder_encode((MimeWordEncoder)c->enc, arena_allocator(&ar),
                                     qstr(c->charset), qstr(c->src));
        if (!str_eq(got, qstr(c->want)))
            testing_t_errorf_v(t, "Encode(%q) = %q, want %q", qstr(c->src), got,
                               qstr(c->want));
        /* No word longer than 75 characters, as Go's TestEncodedWordLength
         * checks. Only UTF-8 is split. */
        if (!strings_equal_fold(qstr(c->charset), BURROW_S("utf-8"))) {
            arena_free(&ar);
            continue;
        }
        Int word = 0;
        for (Int j = 0; j < got.len; j++) {
            word = got.p[j] == ' ' ? 0 : word + 1;
            if (word > 75) {
                testing_t_errorf_v(t, "Encode(%q) has a word over 75: %q", qstr(c->src),
                                   got);
                break;
            }
        }
        arena_free(&ar);
    }
}

static void TestDecodeWord(TestingT *t) {
    MimeWordDecoder dec = {0};
    for (size_t i = 0; i < sizeof mime_decode_cases / sizeof mime_decode_cases[0];
         i++) {
        const MimeDecodeCase *c = &mime_decode_cases[i];
        Arena ar;
        arena_init(&ar, NULL, 0);
        Error err;
        Str got =
            mime_word_decoder_decode(&dec, arena_allocator(&ar), qstr(c->in), &err);
        if (!str_eq(got, qstr(c->want)) || !text_is(err_text(err), c->err))
            testing_t_errorf_v(t, "Decode(%q) = %q, %v, want %q, %s", qstr(c->in), got,
                               err, qstr(c->want), str_from_cstr(c->err));
        arena_free(&ar);
    }
}

static void TestDecodeHeader(TestingT *t) {
    MimeWordDecoder dec = {0};
    for (size_t i = 0;
         i < sizeof mime_decode_header_cases / sizeof mime_decode_header_cases[0];
         i++) {
        const MimeDecodeCase *c = &mime_decode_header_cases[i];
        Arena ar;
        arena_init(&ar, NULL, 0);
        Error err;
        Str got = mime_word_decoder_decode_header(&dec, arena_allocator(&ar),
                                                  qstr(c->in), &err);
        if (!str_eq(got, qstr(c->want)) || !text_is(err_text(err), c->err))
            testing_t_errorf_v(t, "DecodeHeader(%q) = %q, %v, want %q, %s", qstr(c->in),
                               got, err, qstr(c->want), str_from_cstr(c->err));
        arena_free(&ar);
    }
}

/* Go's TestCharsetDecoder: a charset reader that checks what it is given and
 * hands the same bytes back. */
typedef struct CharsetCheck {
    TestingT *t;
    int calls;
    const char *const *charsets;
    const char *const *content;
    StringsReader out;
    Alloc *a;
} CharsetCheck;

static IoReader check_charset(void *env, Str charset, IoReader input, Error *err) {
    CharsetCheck *c = (CharsetCheck *)env;
    Slice got = io_read_all(c->a, input, err);
    Str s = {(const Byte *)got.p, got.len};
    if (!text_is(charset, c->charsets[c->calls]))
        testing_t_errorf_v(c->t, "charset %q, want %s", charset,
                           str_from_cstr(c->charsets[c->calls]));
    if (!text_is(s, c->content[c->calls]))
        testing_t_errorf_v(c->t, "content %q, want %q", s,
                           str_from_cstr(c->content[c->calls]));
    c->calls++;
    strings_reader_reset(&c->out, s);
    return strings_reader_as_io_reader(&c->out);
}

static const Str charset_failed__text = {(const Byte *)"Test error", 10};
static const Error charset_failed = {&burrow_sentinel_error_vt, &charset_failed__text};

static IoReader fail_charset(void *env, Str charset, IoReader input, Error *err) {
    (void)env;
    (void)charset;
    (void)input;
    *err = charset_failed;
    return (IoReader){0};
}

static void TestCharsetDecoder(TestingT *t) {
    static const char *const charsets[] = {"iso-8859-15", "windows-1252"};
    static const char *const content[] = {"f\xf5\xf6", "b\xe0r"};
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    CharsetCheck c = {t, 0, charsets, content, {0}, a};
    MimeWordDecoder dec = {BURROW_FN(MimeCharsetReader, check_charset, &c)};
    Error err;
    Str got = mime_word_decoder_decode_header(
        &dec, a, BURROW_S("=?ISO-8859-15?Q?f=F5=F6?=  =?windows-1252?Q?b=E0r?="), &err);
    CHECK(BURROW_OK(err) && text_is(got, "f\xf5\xf6"
                                         "b\xe0r"));
    CHECK(c.calls == 2);

    /* Go's TestCharsetDecoderError, and an unknown charset with no reader. */
    MimeWordDecoder failing = {BURROW_FN(MimeCharsetReader, fail_charset, NULL)};
    got = mime_word_decoder_decode_header(&failing, a, BURROW_S("=?charset?Q?foo?="),
                                          &err);
    CHECK(errors_is(err, charset_failed) && got.len == 0);
    got = mime_word_decoder_decode(&failing, a, BURROW_S("=?charset?Q?foo?="), &err);
    CHECK(errors_is(err, charset_failed) && got.len == 0);
    MimeWordDecoder plain = {0};
    got = mime_word_decoder_decode(&plain, a, BURROW_S("=?charset?Q?foo?="), &err);
    CHECK(text_is(err_text(err), "mime: unhandled charset \"charset\""));
    arena_free(&ar);
}

/* The generator's LCG and piece picker. */
static uint64_t rnd_next(uint64_t *x) {
    *x = *x * 6364136223846793005ULL + 1442695040888963407ULL;
    return *x >> 33;
}

static Str build(Alloc *a, uint64_t seed, const QStr *pieces, uint64_t npieces,
                 uint64_t max) {
    uint64_t x = seed;
    uint64_t n = rnd_next(&x) % (max + 1);
    StringsBuilder b = STRINGS_BUILDER(a);
    Error err;
    for (uint64_t i = 0; i < n; i++)
        strings_builder_write_string(&b, qstr(pieces[rnd_next(&x) % npieces]), &err);
    return strings_builder_string(&b);
}

typedef uint64_t Fnv;

static void fnv_len(Fnv *h, Int n) {
    for (int i = 0; i < 8; i++) {
        *h ^= (Byte)((uint64_t)n >> (8 * i));
        *h *= 1099511628211ULL;
    }
}

static void fnv_add(Fnv *h, Str s) {
    fnv_len(h, s.len);
    for (Int i = 0; i < s.len; i++) {
        *h ^= s.p[i];
        *h *= 1099511628211ULL;
    }
}

#define NPIECES(x) ((uint64_t)(sizeof(x) / sizeof((x)[0])))

static void TestRandomMediaTypes(TestingT *t) {
    Fnv h = 14695981039346656037ULL;
    for (uint64_t i = 1; i <= MIME_MT_COUNT; i++) {
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        Str in = build(a, i, mime_mt_pieces, NPIECES(mime_mt_pieces), 14);
        Map *params;
        Error err;
        Str mt = mime_parse_media_type(a, in, &params, &err);
        fnv_add(&h, mt);
        fnv_add(&h, err_text(err));
        if (params == NULL) {
            fnv_len(&h, 0);
        } else {
            Slice pairs = sorted_pairs(a, params);
            fnv_len(&h, pairs.len);
            for (Int j = 0; j < pairs.len; j++)
                fnv_add(&h, ((Str *)pairs.p)[j]);
        }
        if (BURROW_OK(err))
            fnv_add(&h, mime_format_media_type(a, mt, params));
        arena_free(&ar);
    }
    if (h != MIME_MT_FNV)
        testing_t_errorf_v(t, "digest %#x, want %#x", h, (uint64_t)MIME_MT_FNV);
}

static void TestRandomHeaders(TestingT *t) {
    MimeWordDecoder dec = {0};
    Fnv h = 14695981039346656037ULL;
    for (uint64_t i = 1; i <= MIME_HDR_COUNT; i++) {
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        Str in = build(a, i, mime_hdr_pieces, NPIECES(mime_hdr_pieces), 12);
        Error err;
        fnv_add(&h, mime_word_decoder_decode_header(&dec, a, in, &err));
        fnv_add(&h, err_text(err));
        fnv_add(&h, mime_word_decoder_decode(&dec, a, in, &err));
        fnv_add(&h, err_text(err));
        arena_free(&ar);
    }
    if (h != MIME_HDR_FNV)
        testing_t_errorf_v(t, "digest %#x, want %#x", h, (uint64_t)MIME_HDR_FNV);
}

static void TestRandomEncode(TestingT *t) {
    MimeWordDecoder dec = {0};
    Fnv h = 14695981039346656037ULL;
    for (uint64_t i = 1; i <= MIME_ENC_COUNT; i++) {
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        Str in = build(a, i, mime_enc_pieces, NPIECES(mime_enc_pieces), 40);
        uint64_t x = i * 7919;
        MimeWordEncoder e = rnd_next(&x) % 2 == 1 ? MIME_Q_ENCODING : MIME_B_ENCODING;
        Str cs = qstr(mime_charsets[rnd_next(&x) % NPIECES(mime_charsets)]);
        Str s = mime_word_encoder_encode(e, a, cs, in);
        fnv_add(&h, s);
        Error err;
        fnv_add(&h, mime_word_decoder_decode_header(&dec, a, s, &err));
        fnv_add(&h, err_text(err));
        arena_free(&ar);
    }
    if (h != MIME_ENC_FNV)
        testing_t_errorf_v(t, "digest %#x, want %#x", h, (uint64_t)MIME_ENC_FNV);
}

/* Inputs given back unchanged are the inputs themselves, and nothing is
 * allocated for them. */
static void TestBorrowed(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str s = BURROW_S("plain ascii");
    CHECK(mime_word_encoder_encode(MIME_Q_ENCODING, a, BURROW_S("utf-8"), s).p == s.p);
    MimeWordDecoder dec = {0};
    Error err;
    CHECK(mime_word_decoder_decode_header(&dec, a, s, &err).p == s.p && BURROW_OK(err));
    CHECK(mime_word_decoder_decode_header(NULL, a, s, &err).p == s.p);
    arena_free(&ar);
}

typedef struct Budget {
    int left;
    long long live;
} Budget;

static void *budget_alloc(void *self, size_t size, size_t align) {
    Budget *b = (Budget *)self;
    if (b->left <= 0)
        return NULL;
    b->left--;
    void *p = mem_alloc(heap_allocator(), size, align);
    if (p != NULL)
        b->live += (long long)size;
    return p;
}

static void budget_free(void *self, void *p, size_t size, size_t align) {
    Budget *b = (Budget *)self;
    if (p == NULL)
        return;
    b->live -= (long long)size;
    mem_free(heap_allocator(), p, size, align);
}

static const AllocVT budget_vt = {budget_alloc, NULL, NULL, budget_free, NULL, NULL};

static void free_str(Alloc *a, Str s) {
    if (s.p != NULL && s.len > 0)
        mem_free(a, (void *)(uintptr_t)s.p, (size_t)s.len, 1);
}

/* Every allocation that can fail, failing: the result is empty, or right, and
 * nothing is left behind once what came back is given back. */
static void TestNoMemory(TestingT *t) {
    Str in = BURROW_S("Text/HTML; Charset=\"utf-8\"; title*0*=us-ascii'en'a%20b; "
                      "title*1=\"c d\"");
    bool whole = false;
    for (int budget = 0; budget < 200 && !whole; budget++) {
        Budget b = {budget, 0};
        Alloc al = {&budget_vt, &b, NULL, NULL};
        Map *params = NULL;
        Error err;
        Str mt = mime_parse_media_type(&al, in, &params, &err);
        if (BURROW_OK(err)) {
            CHECK(text_is(mt, "text/html") && map_len(params) == 2);
            Str title = BURROW_S("title");
            const Str *v = (const Str *)map_get(params, &title);
            CHECK(v != NULL && text_is(*v, "a bc d"));
            const void *k;
            void *val;
            for (MapIter it = map_iter(params); map_next(&it, &k, &val);) {
                free_str(&al, *(const Str *)k);
                free_str(&al, *(const Str *)val);
            }
            map_free(params);
            free_str(&al, mt);
            whole = true;
        } else {
            CHECK(mt.len == 0 && params == NULL);
        }
        if (b.live != 0)
            testing_t_errorf_v(t, "parse, budget %d: %d bytes left", budget,
                               (int)b.live);
    }
    CHECK(whole);

    whole = false;
    for (int budget = 0; budget < 200 && !whole; budget++) {
        Budget b = {budget, 0};
        Alloc al = {&budget_vt, &b, NULL, NULL};
        MimeWordDecoder dec = {0};
        Error err;
        Str got = mime_word_decoder_decode_header(
            &dec, &al, BURROW_S("a =?utf-8?b?Q2Fmw6k=?= =?iso-8859-1?q?caf=E9?= b"),
            &err);
        if (BURROW_OK(err)) {
            CHECK(text_is(got, "a Caf\xc3\xa9"
                               "caf\xc3\xa9 b"));
            free_str(&al, got);
            whole = true;
        } else {
            CHECK(got.len == 0);
        }
        if (b.live != 0)
            testing_t_errorf_v(t, "decode, budget %d: %d bytes left", budget,
                               (int)b.live);
    }
    CHECK(whole);

    whole = false;
    for (int budget = 0; budget < 200 && !whole; budget++) {
        Budget b = {budget, 0};
        Alloc al = {&budget_vt, &b, NULL, NULL};
        Str got = mime_word_encoder_encode(MIME_Q_ENCODING, &al, BURROW_S("utf-8"),
                                           BURROW_S("Caf\xc3\xa9 au lait"));
        if (got.len > 0) {
            CHECK(text_is(got, "=?utf-8?q?Caf=C3=A9_au_lait?="));
            free_str(&al, got);
            whole = true;
        }
        if (b.live != 0)
            testing_t_errorf_v(t, "encode, budget %d: %d bytes left", budget,
                               (int)b.live);
    }
    CHECK(whole);

    whole = false;
    for (int budget = 0; budget < 200 && !whole; budget++) {
        Budget b = {budget, 0};
        Alloc al = {&budget_vt, &b, NULL, NULL};
        Map *m = map_make(heap_allocator(), TYPE_STRING, TYPE_STRING, 0);
        Str k = BURROW_S("Name"), v = BURROW_S("x y");
        map_set(m, &k, &v);
        Str got = mime_format_media_type(&al, BURROW_S("Text/Plain"), m);
        map_free(m);
        if (got.len > 0) {
            CHECK(text_is(got, "text/plain; name=\"x y\""));
            free_str(&al, got);
            whole = true;
        }
        if (b.live != 0)
            testing_t_errorf_v(t, "format, budget %d: %d bytes left", budget,
                               (int)b.live);
    }
    CHECK(whole);
}

#define TESTS(X)                                                                       \
    X(TestParseMediaType)                                                              \
    X(TestFormatMediaType)                                                             \
    X(TestEncodeWord)                                                                  \
    X(TestDecodeWord)                                                                  \
    X(TestDecodeHeader)                                                                \
    X(TestCharsetDecoder)                                                              \
    X(TestRandomMediaTypes)                                                            \
    X(TestRandomHeaders)                                                               \
    X(TestRandomEncode)                                                                \
    X(TestBorrowed)                                                                    \
    X(TestNoMemory)

TESTING_MAIN(TESTS)

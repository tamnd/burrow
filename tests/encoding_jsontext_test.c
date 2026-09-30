/* Derived from Go's src/encoding/json/jsontext/coder_test.go, encode_test.go,
 * decode_test.go, value_test.go, token_test.go and state_test.go. Go source:
 * go1.27.1.
 *
 * tests/encoding_jsontext_test_gen.h, from tools/gen-jsontext-tests.sh, holds
 * what Go's jsontext does with Go's own tables: the tokens it reads, what it
 * writes, and every error text, pointer, depth and offset along the way. The
 * cases here replay the same calls and have to match byte for byte.
 *
 * Copyright 2020 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"
#include "fatal.h"

#include "../src/encoding/json_internal.h"

#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/encoding/json/jsontext.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"

#include "encoding_jsontext_test_gen.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define LEN(x) (sizeof(x) / sizeof((x)[0]))

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

static JsontextOptions opts_of(const JtOpts *o) {
    JsontextOptions x;
    memset(&x, 0, sizeof(x));
    x.presence = o->presence;
    x.values = o->values;
    x.indent = qstr(o->indent);
    x.indent_prefix = qstr(o->prefix);
    x.byte_limit = o->byte_limit;
    x.depth_limit = (Int)o->depth_limit;
    return x;
}

static Slice opts_slice(JsontextOptions *o, Int n) {
    return (Slice){o, n, n, TYPE_JSONTEXT_OPTIONS};
}

static Slice str_slice(Str s) {
    return (Slice){(void *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE};
}

static IoReader string_reader(Alloc *a, Str s) {
    return bytes_buffer_as_io_reader(bytes_new_buffer_string(a, s));
}

/* A token the way the generator describes it, with raw ones read from
 * their text and cloned into a. */
static JsontextToken make_token(const JtTok *k, Alloc *a) {
    JsontextToken zero = {NULL, {NULL, 0}, 0};
    double d;
    float f;
    uint32_t b32;
    switch (k->how) {
    case 's':
        return jsontext_string(qstr(k->text));
    case 'f':
        memcpy(&d, &k->num, sizeof d);
        return jsontext_float(d);
    case 'F':
        b32 = (uint32_t)k->num;
        memcpy(&f, &b32, sizeof f);
        return jsontext_float32(f);
    case 'i':
        return jsontext_int((int64_t)k->num);
    case 'u':
        return jsontext_uint((uint64_t)k->num);
    case 'r':
        break;
    default:
        return zero;
    }
    Str s = qstr(k->text);
    if (text_is(s, "{"))
        return jsontext_begin_object;
    if (text_is(s, "}"))
        return jsontext_end_object;
    if (text_is(s, "["))
        return jsontext_begin_array;
    if (text_is(s, "]"))
        return jsontext_end_array;
    JsontextDecoder *dec = jsontext_new_decoder_v(a, string_reader(a, s), 0);
    Error err;
    JsontextToken t = jsontext_decoder_read_token(dec, &err);
    JsontextToken c = jsontext_token_clone(t, a);
    jsontext_decoder_free(dec);
    return c;
}

/* What the encoder has written so far, flushed or not. */
static Str encoder_output(Alloc *a, BytesBuffer *dst, JsontextEncoder *e) {
    Slice b = bytes_buffer_bytes(dst);
    Int n = b.len + e->buf.len;
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)(n > 0 ? n : 1), 1);
    if (p == NULL)
        return (Str){NULL, 0};
    if (b.len > 0)
        memcpy(p, b.p, (size_t)b.len);
    if (e->buf.len > 0)
        memcpy(p + b.len, e->buf.p, (size_t)e->buf.len);
    return str_from_bytes(p, n);
}

static Str trim_space(Str s) {
    Int i = 0, j = s.len;
    while (i < j &&
           (s.p[i] == ' ' || s.p[i] == '\n' || s.p[i] == '\r' || s.p[i] == '\t'))
        i++;
    while (j > i && (s.p[j - 1] == ' ' || s.p[j - 1] == '\n' || s.p[j - 1] == '\r' ||
                     s.p[j - 1] == '\t'))
        j--;
    return str_from_bytes(s.p + i, j - i);
}

/* ------------------------------------------------------------- the coder */

static const char *const enc_formats[] = {"Compact", "Indented"};
static const char *const enc_types[] = {"Token", "Value", "TokenDelims"};

static void encode_case(TestingT *t, const JtCoderCase *c, int format, int type) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    JsontextOptions o[4];
    memset(&o[0], 0, sizeof(o[0]));
    o[0].presence = o[0].values = JSONFLAG_OMIT_TOP_LEVEL_NEWLINE;
    Int no = 1;
    Str want = qstr(c->compacted);
    if (format == 1) {
        o[no++] = jsontext_multiline(true);
        o[no++] = jsontext_with_indent_prefix(BURROW_S("\t"));
        o[no++] = jsontext_with_indent(BURROW_S("    "));
        want = qstr(c->indented);
    }
    BytesBuffer dst = BYTES_BUFFER(a);
    JsontextEncoder *e =
        jsontext_new_encoder(a, bytes_buffer_as_io_writer(&dst), opts_slice(o, no));
    Error err = BURROW_NO_ERROR;
    for (int i = 0; type != 1 && i < c->ntok; i++) {
        JsontextToken tok = make_token(&jt_coder_tokens[c->tok_start + i], a);
        JsontextKind k = jsontext_token_kind(tok);
        if (type == 0 || k == '{' || k == '}' || k == '[' || k == ']') {
            err = jsontext_encoder_write_token(e, tok);
        } else {
            Str s = jsontext_token_string(tok, a);
            Slice v = str_slice(s);
            if (k == '"')
                v = jsontext_append_quote(a, (Slice){NULL, 0, 0, TYPE_BYTE}, s, &err);
            err = jsontext_encoder_write_value(e, v);
        }
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%s/%s/%s: %d: write error: %v", c->name,
                               enc_types[type], enc_formats[format], i, err);
            break;
        }
        if (type == 0 && c->nptr > 0) {
            Str p = jsontext_encoder_stack_pointer(e, a);
            if (!str_eq(p, qstr(jt_coder_pointers[c->ptr_start + i])))
                testing_t_errorf_v(t, "%s: %d: StackPointer = %q, want %q", c->name, i,
                                   p, qstr(jt_coder_pointers[c->ptr_start + i]));
        }
    }
    if (type == 1) {
        err = jsontext_encoder_write_value(e, str_slice(qstr(c->in)));
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "%s/Value/%s: WriteValue error: %v", c->name,
                               enc_formats[format], err);
    }
    Slice got = bytes_buffer_bytes(&dst);
    Str gs = str_from_bytes((const Byte *)got.p, got.len);
    if (!str_eq(gs, want))
        testing_t_errorf_v(t, "%s/%s/%s: output = %q, want %q", c->name,
                           enc_types[type], enc_formats[format], gs, want);
    jsontext_encoder_free(e);
    arena_free(&ar);
}

static void decode_case(TestingT *t, const JtCoderCase *c, int type) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    JsontextDecoder *d = jsontext_new_decoder_v(a, string_reader(a, qstr(c->in)), 0);
    Error err = BURROW_NO_ERROR;
    if (type == 1) {
        JsontextValue v = jsontext_decoder_read_value(d, &err);
        Str got = str_from_bytes((const Byte *)v.p, v.len);
        if (BURROW_FAILED(err) || !str_eq(got, trim_space(qstr(c->in))))
            testing_t_errorf_v(t, "%s: ReadValue = %q, %v", c->name, got, err);
    } else {
        int n = 0;
        for (;;) {
            JsontextKind k = jsontext_decoder_peek_kind(d);
            Str text = {NULL, 0};
            if (type == 0 || k == '{' || k == '}' || k == '[' || k == ']' || k == 0) {
                JsontextToken tok = jsontext_decoder_read_token(d, &err);
                if (BURROW_OK(err)) {
                    k = jsontext_token_kind(tok);
                    text = jsontext_token_string(tok, a);
                }
            } else {
                JsontextValue v = jsontext_decoder_read_value(d, &err);
                if (BURROW_OK(err)) {
                    k = jsontext_value_kind(v);
                    text = str_from_bytes((const Byte *)v.p, v.len);
                    if (k == '"') {
                        Slice u = jsontext_append_unquote(
                            a, (Slice){NULL, 0, 0, TYPE_BYTE}, text, &err);
                        text = str_from_bytes((const Byte *)u.p, u.len);
                    }
                }
            }
            if (errors_is(err, io_eof))
                break;
            if (BURROW_FAILED(err)) {
                testing_t_errorf_v(t, "%s/%s: %d: read error: %v", c->name,
                                   enc_types[type], n, err);
                break;
            }
            if (n >= c->ntok) {
                testing_t_errorf_v(t, "%s/%s: more than %d tokens", c->name,
                                   enc_types[type], c->ntok);
                break;
            }
            const JtRead *w = &jt_coder_decoded[c->dec_start + n];
            if (k != (JsontextKind)w->kind || !str_eq(text, qstr(w->text)))
                testing_t_errorf_v(t, "%s/%s: %d: token %c %q, want %c %q", c->name,
                                   enc_types[type], n, k, text, w->kind, qstr(w->text));
            if (type == 0 && c->nptr > 0) {
                Str p = jsontext_decoder_stack_pointer(d, a);
                if (!str_eq(p, qstr(jt_coder_pointers[c->ptr_start + n])))
                    testing_t_errorf_v(t, "%s: %d: StackPointer = %q, want %q", c->name,
                                       n, p, qstr(jt_coder_pointers[c->ptr_start + n]));
            }
            n++;
        }
        if (n != c->ntok)
            testing_t_errorf_v(t, "%s/%s: read %d tokens, want %d", c->name,
                               enc_types[type], n, c->ntok);
    }
    jsontext_decoder_free(d);
    arena_free(&ar);
}

static void TestEncoder(TestingT *t) {
    for (size_t i = 0; i < LEN(jt_coder_cases); i++)
        for (int f = 0; f < 2; f++)
            for (int ty = 0; ty < 3; ty++)
                encode_case(t, &jt_coder_cases[i], f, ty);
}

static void TestDecoder(TestingT *t) {
    for (size_t i = 0; i < LEN(jt_coder_cases); i++)
        for (int ty = 0; ty < 3; ty++)
            decode_case(t, &jt_coder_cases[i], ty);
}

/* --------------------------------------------------------- faulty I/O */

/* FaultyBuffer: reads and writes a random amount up to max_bytes, and now
 * and then fails with may_error without losing anything. */
typedef struct Faulty {
    Byte *b;
    Int len, cap, off;
    Int max_bytes;
    Error may_error;
    uint64_t rand;
    Alloc *a;
} Faulty;

static Int faulty_rand(Faulty *p, Int n) {
    p->rand ^= p->rand << 13;
    p->rand ^= p->rand >> 7;
    p->rand ^= p->rand << 17;
    return (Int)(p->rand % (uint64_t)n);
}

static Int faulty_truncate(Faulty *p, Int n) {
    if (p->max_bytes > 0) {
        if (n > p->max_bytes)
            n = p->max_bytes;
        return faulty_rand(p, n + 1);
    }
    return n;
}

static Error faulty_may_error(Faulty *p) {
    if (BURROW_FAILED(p->may_error) && faulty_rand(p, 2) == 0)
        return p->may_error;
    return BURROW_NO_ERROR;
}

static Int faulty_read(void *self, Slice b, Error *err) {
    Faulty *p = (Faulty *)self;
    Int n = faulty_truncate(p, b.len);
    if (n > p->len - p->off)
        n = p->len - p->off;
    if (n > 0)
        memcpy(b.p, p->b + p->off, (size_t)n);
    p->off += n;
    if (p->off == p->len && (n == 0 || faulty_rand(p, 2) == 0)) {
        *err = io_eof;
        return n;
    }
    *err = faulty_may_error(p);
    return n;
}

static Int faulty_write(void *self, Slice b, Error *err) {
    Faulty *p = (Faulty *)self;
    Int n = faulty_truncate(p, b.len);
    if (p->len + n > p->cap) {
        Int ncap = (p->len + n) * 2 + 16;
        Byte *q = (Byte *)mem_alloc_nozero(p->a, (size_t)ncap, 1);
        if (q == NULL) {
            *err = burrow_err_out_of_memory;
            return 0;
        }
        if (p->len > 0)
            memcpy(q, p->b, (size_t)p->len);
        p->b = q;
        p->cap = ncap;
    }
    if (n > 0)
        memcpy(p->b + p->len, b.p, (size_t)n);
    p->len += n;
    if (n < b.len) {
        *err = io_err_short_write;
        return n;
    }
    *err = faulty_may_error(p);
    return n;
}

static const IoReaderVT faulty_reader_vt = {NULL, faulty_read};
static const IoWriterVT faulty_writer_vt = {NULL, faulty_write};

static void TestFaultyEncoder(TestingT *t) {
    for (size_t i = 0; i < LEN(jt_coder_cases); i++) {
        const JtCoderCase *c = &jt_coder_cases[i];
        for (int ty = 0; ty < 2; ty++) {
            Arena ar;
            arena_init(&ar, NULL, 0);
            Alloc *a = arena_allocator(&ar);
            Faulty f = {NULL, 0, 0, 0, 1, io_err_short_write, 0x9e3779b97f4a7c15ULL + i,
                        a};
            IoWriter w = {&faulty_writer_vt, &f};
            JsontextEncoder *e = jsontext_new_encoder_v(a, w, 0);
            if (ty == 0) {
                for (int k = 0; k < c->ntok; k++) {
                    Error err = jsontext_encoder_write_token(
                        e, make_token(&jt_coder_tokens[c->tok_start + k], a));
                    if (BURROW_FAILED(err) && !errors_is(err, io_err_short_write))
                        testing_t_errorf_v(t, "%s: %d: WriteToken error: %v", c->name,
                                           k, err);
                }
            } else {
                Error err = jsontext_encoder_write_value(e, str_slice(qstr(c->in)));
                if (BURROW_FAILED(err) && !errors_is(err, io_err_short_write))
                    testing_t_errorf_v(t, "%s: WriteValue error: %v", c->name, err);
            }
            Str got = str_from_bytes(f.b, f.len);
            Int n = got.len + e->buf.len;
            Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)n + 1, 1);
            if (p != NULL) {
                if (f.len > 0)
                    memcpy(p, f.b, (size_t)f.len);
                if (e->buf.len > 0)
                    memcpy(p + f.len, e->buf.p, (size_t)e->buf.len);
                Str all = str_from_bytes(p, n);
                Str want = qstr(c->compacted);
                if (all.len != want.len + 1 ||
                    memcmp(all.p, want.p, (size_t)want.len) != 0 ||
                    all.p[want.len] != '\n')
                    testing_t_errorf_v(t, "%s: output = %q, want %q+newline", c->name,
                                       all, want);
            }
            jsontext_encoder_free(e);
            arena_free(&ar);
        }
    }
}

static void TestFaultyDecoder(TestingT *t) {
    for (size_t i = 0; i < LEN(jt_coder_cases); i++) {
        const JtCoderCase *c = &jt_coder_cases[i];
        for (int ty = 0; ty < 2; ty++) {
            Arena ar;
            arena_init(&ar, NULL, 0);
            Alloc *a = arena_allocator(&ar);
            Str in = qstr(c->in);
            Faulty f = {
                (Byte *)(uintptr_t)in.p,   in.len, in.len, 0, 1, io_err_no_progress,
                0x2545f4914f6cdd1dULL + i, a};
            IoReader r = {&faulty_reader_vt, &f};
            JsontextDecoder *d = jsontext_new_decoder_v(a, r, 0);
            int n = 0;
            for (int guard = 0; guard < 100000; guard++) {
                Error err = BURROW_NO_ERROR;
                Str text = {NULL, 0};
                JsontextKind k = 0;
                if (ty == 0) {
                    JsontextToken tok = jsontext_decoder_read_token(d, &err);
                    if (BURROW_OK(err)) {
                        k = jsontext_token_kind(tok);
                        text = jsontext_token_string(tok, a);
                    }
                } else {
                    JsontextValue v = jsontext_decoder_read_value(d, &err);
                    text = str_from_bytes((const Byte *)v.p, v.len);
                }
                if (errors_is(err, io_eof))
                    break;
                if (BURROW_FAILED(err)) {
                    if (!errors_is(err, io_err_no_progress)) {
                        testing_t_errorf_v(t, "%s: %d: read error: %v", c->name, n,
                                           err);
                        break;
                    }
                    continue;
                }
                if (ty == 1) {
                    if (!str_eq(text, trim_space(in)))
                        testing_t_errorf_v(t, "%s: ReadValue = %q", c->name, text);
                } else if (n < c->ntok) {
                    const JtRead *w = &jt_coder_decoded[c->dec_start + n];
                    if (k != (JsontextKind)w->kind || !str_eq(text, qstr(w->text)))
                        testing_t_errorf_v(t, "%s: %d: token %q, want %q", c->name, n,
                                           text, qstr(w->text));
                }
                n++;
            }
            if (ty == 0 && n != c->ntok)
                testing_t_errorf_v(t, "%s: read %d tokens, want %d", c->name, n,
                                   c->ntok);
            jsontext_decoder_free(d);
            arena_free(&ar);
        }
    }
}

/* ---------------------------------------------------------- the errors */

static void TestEncoderErrors(TestingT *t) {
    for (size_t i = 0; i < LEN(jt_enc_cases); i++) {
        const JtEncCase *c = &jt_enc_cases[i];
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        JsontextOptions o = opts_of(&c->opts);
        BytesBuffer dst = BYTES_BUFFER(a);
        JsontextEncoder *e =
            jsontext_new_encoder(a, bytes_buffer_as_io_writer(&dst), opts_slice(&o, 1));
        for (int k = 0; k < c->ncall; k++) {
            const JtEncCall *call = &jt_enc_calls[c->call_start + k];
            Error err;
            if (call->is_value)
                err = jsontext_encoder_write_value(e, str_slice(qstr(call->value)));
            else
                err = jsontext_encoder_write_token(e, make_token(&call->tok, a));
            if (!text_is(err_text(err), call->err))
                testing_t_errorf_v(t, "%s: %d: error = %v, want %s", c->name, k, err,
                                   call->err);
            Str p = jsontext_encoder_stack_pointer(e, a);
            if (!str_eq(p, qstr(call->pointer)))
                testing_t_errorf_v(t, "%s: %d: StackPointer = %q, want %q", c->name, k,
                                   p, qstr(call->pointer));
            if (jsontext_encoder_stack_depth(e) != call->depth)
                testing_t_errorf_v(t, "%s: %d: StackDepth = %d, want %d", c->name, k,
                                   jsontext_encoder_stack_depth(e), call->depth);
        }
        Str got = encoder_output(a, &dst, e);
        if (!str_eq(got, qstr(c->out)))
            testing_t_errorf_v(t, "%s: output = %q, want %q", c->name, got,
                               qstr(c->out));
        if (jsontext_encoder_output_offset(e) != c->offset)
            testing_t_errorf_v(t, "%s: OutputOffset = %d, want %d", c->name,
                               jsontext_encoder_output_offset(e), c->offset);
        jsontext_encoder_free(e);
        arena_free(&ar);
    }
}

static void TestDecoderErrors(TestingT *t) {
    for (size_t i = 0; i < LEN(jt_dec_cases); i++) {
        const JtDecCase *c = &jt_dec_cases[i];
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        JsontextOptions o = opts_of(&c->opts);
        JsontextDecoder *d =
            jsontext_new_decoder(a, string_reader(a, qstr(c->in)), opts_slice(&o, 1));
        for (int k = 0; k < c->ncall; k++) {
            const JtDecCall *call = &jt_dec_calls[c->call_start + k];
            JsontextKind kind = jsontext_decoder_peek_kind(d);
            if (kind != (JsontextKind)call->kind)
                testing_t_errorf_v(t, "%s: %d: PeekKind = %c, want %c", c->name, k,
                                   kind, call->kind);
            Error err;
            Str out;
            if (call->is_value) {
                JsontextValue v = jsontext_decoder_read_value(d, &err);
                out = str_from_bytes((const Byte *)v.p, v.len);
            } else {
                JsontextToken tok = jsontext_decoder_read_token(d, &err);
                out = jsontext_token_string(tok, a);
            }
            if (!str_eq(out, qstr(call->out)))
                testing_t_errorf_v(t, "%s: %d: read = %q, want %q", c->name, k, out,
                                   qstr(call->out));
            if (!text_is(err_text(err), call->err))
                testing_t_errorf_v(t, "%s: %d: error = %v, want %s", c->name, k, err,
                                   call->err);
            Str p = jsontext_decoder_stack_pointer(d, a);
            if (!str_eq(p, qstr(call->pointer)))
                testing_t_errorf_v(t, "%s: %d: StackPointer = %q, want %q", c->name, k,
                                   p, qstr(call->pointer));
            if (jsontext_decoder_stack_depth(d) != call->depth)
                testing_t_errorf_v(t, "%s: %d: StackDepth = %d, want %d", c->name, k,
                                   jsontext_decoder_stack_depth(d), call->depth);
        }
        if (jsontext_decoder_input_offset(d) != c->offset)
            testing_t_errorf_v(t, "%s: InputOffset = %d, want %d", c->name,
                               jsontext_decoder_input_offset(d), c->offset);
        Slice un = jsontext_decoder_unread_buffer(d);
        Str unread = str_from_bytes((const Byte *)un.p, un.len);
        if (!str_eq(unread, qstr(c->unread)))
            testing_t_errorf_v(t, "%s: UnreadBuffer = %q, want %q", c->name, unread,
                               qstr(c->unread));
        jsontext_decoder_free(d);
        arena_free(&ar);
    }
}

/* ------------------------------------------------------------- values */

static JsontextValue value_copy(Alloc *a, QStr q) {
    Str s = qstr(q);
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)s.len + 1, 1);
    if (p != NULL && s.len > 0)
        memcpy(p, s.p, (size_t)s.len);
    return (JsontextValue){p, s.len, s.len, TYPE_BYTE};
}

static void TestValueMethods(TestingT *t) {
    for (size_t i = 0; i < LEN(jt_value_cases); i++) {
        const JtValueCase *c = &jt_value_cases[i];
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        JsontextValue v = value_copy(a, c->in);
        if (jsontext_value_is_valid_v(v, 0) != (c->valid != 0))
            testing_t_errorf_v(t, "%s: IsValid = %t", c->name, c->valid == 0);

        JsontextValue cv = value_copy(a, c->in);
        Error err = jsontext_value_compact_v(&cv, a, 0);
        Str got = str_from_bytes((const Byte *)cv.p, cv.len);
        if (!str_eq(got, qstr(c->compacted)) || !text_is(err_text(err), c->compact_err))
            testing_t_errorf_v(t, "%s: Compact = %q, %v, want %q, %s", c->name, got,
                               err, qstr(c->compacted), c->compact_err);

        JsontextValue iv = value_copy(a, c->in);
        err = jsontext_value_indent_v(&iv, a, 2,
                                      jsontext_with_indent_prefix(BURROW_S("\t")),
                                      jsontext_with_indent(BURROW_S("    ")));
        got = str_from_bytes((const Byte *)iv.p, iv.len);
        if (!str_eq(got, qstr(c->indented)) || !text_is(err_text(err), c->indent_err))
            testing_t_errorf_v(t, "%s: Indent = %q, %v, want %q, %s", c->name, got, err,
                               qstr(c->indented), c->indent_err);

        JsontextValue kv = value_copy(a, c->in);
        err = jsontext_value_canonicalize_v(&kv, a, 0);
        got = str_from_bytes((const Byte *)kv.p, kv.len);
        if (!str_eq(got, qstr(c->canonicalized)) ||
            !text_is(err_text(err), c->canonicalize_err))
            testing_t_errorf_v(t, "%s: Canonicalize = %q, %v, want %q, %s", c->name,
                               got, err, qstr(c->canonicalized), c->canonicalize_err);
        arena_free(&ar);
    }
}

static const char *const interleave_modes[] = {"TokenFirst", "ValueFirst",
                                               "TokenDelims"};

static void interleave_case(TestingT *t, const JtCoderCase *c, int mode) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer dst = BYTES_BUFFER(a);
    JsontextDecoder *d = jsontext_new_decoder_v(a, string_reader(a, qstr(c->in)), 0);
    JsontextEncoder *e = jsontext_new_encoder_v(a, bytes_buffer_as_io_writer(&dst), 0);
    bool tick = mode == 0;
    for (;;) {
        if (mode == 2) {
            JsontextKind k = jsontext_decoder_peek_kind(d);
            tick = k == '{' || k == '}' || k == '[' || k == ']';
        }
        Error err;
        if (tick) {
            JsontextToken tok = jsontext_decoder_read_token(d, &err);
            if (errors_is(err, io_eof))
                break;
            if (BURROW_FAILED(err)) {
                testing_t_errorf_v(t, "%s/%s: ReadToken error: %v", c->name,
                                   interleave_modes[mode], err);
                break;
            }
            err = jsontext_encoder_write_token(e, tok);
        } else {
            JsontextValue v = jsontext_decoder_read_value(d, &err);
            if (BURROW_FAILED(err)) {
                /* ReadValue at the end of an object or array is a syntactic
                 * error, so go round again as a ReadToken. */
                JsontextKind k = jsontext_decoder_peek_kind(d);
                if (k == '}' || k == ']') {
                    if (errors_as(err, TYPE_JSONTEXT_SYNTACTIC_ERROR) == NULL)
                        testing_t_errorf_v(t,
                                           "%s/%s: ReadValue error %v is not syntactic",
                                           c->name, interleave_modes[mode], err);
                    tick = !tick;
                    continue;
                }
                if (errors_is(err, io_eof))
                    break;
                testing_t_errorf_v(t, "%s/%s: ReadValue error: %v", c->name,
                                   interleave_modes[mode], err);
                break;
            }
            err = jsontext_encoder_write_value(e, v);
        }
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%s/%s: write error: %v", c->name,
                               interleave_modes[mode], err);
            break;
        }
        tick = !tick;
    }
    Slice got = bytes_buffer_bytes(&dst);
    Str gs = str_from_bytes((const Byte *)got.p, got.len);
    Str want = qstr(c->compacted);
    if (gs.len != want.len + 1 || memcmp(gs.p, want.p, (size_t)want.len) != 0 ||
        gs.p[want.len] != '\n')
        testing_t_errorf_v(t, "%s/%s: output = %q, want %q plus a newline", c->name,
                           interleave_modes[mode], gs, want);
    jsontext_encoder_free(e);
    jsontext_decoder_free(d);
    arena_free(&ar);
}

static void TestCoderInterleaved(TestingT *t) {
    for (size_t i = 0; i < LEN(jt_coder_cases); i++)
        for (int m = 0; m < 3; m++)
            interleave_case(t, &jt_coder_cases[i], m);
}

typedef struct PointerStep {
    char how; /* n null, t true, s string, { } [ ] delimiters */
    const char *text;
    const char *want;
} PointerStep;

static const PointerStep pointer_steps[] = {
    {'n', "", ""},
    {'[', "", ""},
    {']', "", ""},
    {'[', "", ""},
    {'t', "", "/0"},
    {']', "", ""},
    {'[', "", ""},
    {'s', "hello", "/0"},
    {'s', "goodbye", "/1"},
    {']', "", ""},
    {'{', "", ""},
    {'}', "", ""},
    {'{', "", ""},
    {'s', "hello", "/hello"},
    {'s', "goodbye", "/hello"},
    {'}', "", ""},
    {'{', "", ""},
    {'s', "", "/"},
    {'n', "", "/"},
    {'s', "0", "/0"},
    {'n', "", "/0"},
    {'s', "~", "/~0"},
    {'n', "", "/~0"},
    {'s', "/", "/~1"},
    {'n', "", "/~1"},
    {'s', "a//b~/c/~d~~e", "/a~1~1b~0~1c~1~0d~0~0e"},
    {'n', "", "/a~1~1b~0~1c~1~0d~0~0e"},
    {'s', " \r\n\t", "/ \r\n\t"},
    {'n', "", "/ \r\n\t"},
    {'}', "", ""},
    {'[', "", ""},
    {'{', "", "/0"},
    {'s', "", "/0/"},
    {'[', "", "/0/"},
    {'{', "", "/0//0"},
    {'s', "#", "/0//0/#"},
    {'n', "", "/0//0/#"},
    {'}', "", "/0//0"},
    {']', "", "/0/"},
    {'}', "", "/0"},
    {']', "", ""},
};

static JsontextToken step_token(const PointerStep *s) {
    switch (s->how) {
    case 'n':
        return jsontext_null;
    case 't':
        return jsontext_true;
    case '{':
        return jsontext_begin_object;
    case '}':
        return jsontext_end_object;
    case '[':
        return jsontext_begin_array;
    case ']':
        return jsontext_end_array;
    default:
        return jsontext_string(str_from_cstr(s->text));
    }
}

static void TestCoderStackPointer(TestingT *t) {
    for (int dupes = 0; dupes < 2; dupes++) {
        const char *name = dupes ? "AllowDuplicateNames" : "RejectDuplicateNames";
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        BytesBuffer bb = BYTES_BUFFER(a);
        JsontextEncoder *e =
            jsontext_new_encoder_v(a, bytes_buffer_as_io_writer(&bb), 1,
                                   jsontext_allow_duplicate_names(dupes != 0));
        for (size_t i = 0; i < LEN(pointer_steps); i++) {
            Error err = jsontext_encoder_write_token(e, step_token(&pointer_steps[i]));
            if (BURROW_FAILED(err)) {
                testing_t_errorf_v(t, "%s: %d: WriteToken error: %v", name, (int)i,
                                   err);
                break;
            }
            Str got = jsontext_encoder_stack_pointer(e, a);
            if (!text_is(got, pointer_steps[i].want))
                testing_t_errorf_v(t, "%s: %d: Encoder.StackPointer = %q, want %q",
                                   name, (int)i, got,
                                   str_from_cstr(pointer_steps[i].want));
        }
        JsontextDecoder *d =
            jsontext_new_decoder_v(a, bytes_buffer_as_io_reader(&bb), 1,
                                   jsontext_allow_duplicate_names(dupes != 0));
        for (size_t i = 0; i < LEN(pointer_steps); i++) {
            Error err;
            jsontext_decoder_read_token(d, &err);
            if (BURROW_FAILED(err)) {
                testing_t_errorf_v(t, "%s: %d: ReadToken error: %v", name, (int)i, err);
                break;
            }
            Str got = jsontext_decoder_stack_pointer(d, a);
            if (!text_is(got, pointer_steps[i].want))
                testing_t_errorf_v(t, "%s: %d: Decoder.StackPointer = %q, want %q",
                                   name, (int)i, got,
                                   str_from_cstr(pointer_steps[i].want));
        }
        jsontext_decoder_free(d);
        jsontext_encoder_free(e);
        arena_free(&ar);
    }
}

/* ---------------------------------------------------------- max nesting */

#define MAX_DEPTH ((Int)10000)

/* Go's maxArrays and maxObjects, one level deeper than the limit allows. */
static Str max_arrays(Alloc *a) {
    Int n = MAX_DEPTH + 1;
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)(2 * n), 1);
    if (p == NULL)
        return (Str){NULL, 0};
    memset(p, '[', (size_t)n);
    memset(p + n, ']', (size_t)n);
    return str_from_bytes(p, 2 * n);
}

static Str max_objects(Alloc *a) {
    Int n = MAX_DEPTH + 1;
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)(5 * n + 2), 1);
    if (p == NULL)
        return (Str){NULL, 0};
    for (Int i = 0; i < n; i++)
        memcpy(p + 4 * i, "{\"\":", 4);
    memcpy(p + 4 * n, "\"\"", 2);
    memset(p + 4 * n + 2, '}', (size_t)n);
    return str_from_bytes(p, 5 * n + 2);
}

static Str trim_array(Str s) {
    return str_from_bytes(s.p + 1, s.len - 2);
}

static Str trim_object(Str s) {
    return str_from_bytes(s.p + 4, s.len - 5);
}

/* The error Go wants at the limit: the offset, a pointer of n copies of
 * elem, and errMaxDepth underneath. */
static bool is_max_depth(Error err, int64_t offset, const char *elem) {
    const JsontextSyntacticError *se = errors_as(err, TYPE_JSONTEXT_SYNTACTIC_ERROR);
    if (se == NULL || se->byte_offset != offset ||
        !text_is(err_text(se->err), "exceeded max depth"))
        return false;
    size_t k = strlen(elem);
    if (se->json_pointer.len != (Int)k * MAX_DEPTH)
        return false;
    for (Int i = 0; i < MAX_DEPTH; i++)
        if (memcmp(se->json_pointer.p + (size_t)i * k, elem, k) != 0)
            return false;
    return true;
}

typedef struct DepthRun {
    TestingT *t;
    const char *name;
    Alloc *a;
    JsontextDecoder *d;
    JsontextEncoder *e;
    BytesBuffer *bb;
    bool failed;
} DepthRun;

static void run_reset_decoder(DepthRun *r, Str in) {
    jsontext_decoder_reset_v(r->d, string_reader(r->a, in), 0);
    r->failed = false;
}

/* want_off < 0 means no error. */
static void read_token_is(DepthRun *r, JsontextKind kind, int64_t want_off,
                          const char *elem) {
    if (r->failed)
        return;
    Error err;
    JsontextToken tok = jsontext_decoder_read_token(r->d, &err);
    bool ok = want_off < 0 ? BURROW_OK(err) : is_max_depth(err, want_off, elem);
    if (jsontext_token_kind(tok) != kind || !ok) {
        testing_t_errorf_v(r->t, "%s: ReadToken = %c, %v; want %c", r->name,
                           jsontext_token_kind(tok), err, kind);
        r->failed = true;
    }
}
static void read_value_is(DepthRun *r, Int len, int64_t want_off, const char *elem) {
    if (r->failed)
        return;
    Error err;
    JsontextValue v = jsontext_decoder_read_value(r->d, &err);
    bool ok = want_off < 0 ? BURROW_OK(err) : is_max_depth(err, want_off, elem);
    if (v.len != len || !ok) {
        testing_t_errorf_v(r->t, "%s: ReadValue = %d bytes, %v; want %d", r->name,
                           (int)v.len, err, (int)len);
        r->failed = true;
    }
}

static void run_reset_encoder(DepthRun *r) {
    bytes_buffer_reset(r->bb);
    jsontext_encoder_reset_v(r->e, bytes_buffer_as_io_writer(r->bb), 0);
    r->failed = false;
}

static void write_token_is(DepthRun *r, JsontextToken tok, int64_t want_off,
                           const char *elem) {
    if (r->failed)
        return;
    Error err = jsontext_encoder_write_token(r->e, tok);
    if (want_off < 0 ? BURROW_FAILED(err) : !is_max_depth(err, want_off, elem)) {
        testing_t_errorf_v(r->t, "%s: WriteToken = %v", r->name, err);
        r->failed = true;
    }
}

static void write_value_is(DepthRun *r, Str v, int64_t want_off, const char *elem) {
    if (r->failed)
        return;
    Error err = jsontext_encoder_write_value(r->e, str_slice(v));
    if (want_off < 0 ? BURROW_FAILED(err) : !is_max_depth(err, want_off, elem)) {
        testing_t_errorf_v(r->t, "%s: WriteValue = %v", r->name, err);
        r->failed = true;
    }
}

static void TestCoderMaxDepth(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str arrays = max_arrays(a), objects = max_objects(a);
    BytesBuffer bb = BYTES_BUFFER(a);
    DepthRun r = {t, "", a, NULL, NULL, &bb, false};
    r.d = jsontext_new_decoder_v(a, string_reader(a, arrays), 0);
    r.e = jsontext_new_encoder_v(a, bytes_buffer_as_io_writer(&bb), 0);
    const int64_t aoff = MAX_DEPTH, ooff = MAX_DEPTH * 4;

    r.name = "Decoder/ArraysValid/SingleValue";
    run_reset_decoder(&r, trim_array(arrays));
    read_value_is(&r, MAX_DEPTH * 2, -1, NULL);
    r.name = "Decoder/ArraysValid/TokenThenValue";
    run_reset_decoder(&r, trim_array(arrays));
    read_token_is(&r, '[', -1, NULL);
    read_value_is(&r, (MAX_DEPTH - 1) * 2, -1, NULL);
    read_token_is(&r, ']', -1, NULL);
    r.name = "Decoder/ArraysValid/AllTokens";
    run_reset_decoder(&r, trim_array(arrays));
    for (int i = 0; i < MAX_DEPTH; i++)
        read_token_is(&r, '[', -1, NULL);
    for (int i = 0; i < MAX_DEPTH; i++)
        read_token_is(&r, ']', -1, NULL);

    r.name = "Decoder/ArraysInvalid/SingleValue";
    run_reset_decoder(&r, arrays);
    read_value_is(&r, 0, aoff, "/0");
    r.name = "Decoder/ArraysInvalid/TokenThenValue";
    run_reset_decoder(&r, arrays);
    read_token_is(&r, '[', -1, NULL);
    read_value_is(&r, 0, aoff, "/0");
    r.name = "Decoder/ArraysInvalid/AllTokens";
    run_reset_decoder(&r, arrays);
    for (int i = 0; i < MAX_DEPTH; i++)
        read_token_is(&r, '[', -1, NULL);
    read_value_is(&r, 0, aoff, "/0");

    r.name = "Decoder/ObjectsValid/SingleValue";
    run_reset_decoder(&r, trim_object(objects));
    read_value_is(&r, MAX_DEPTH * 5 + 2, -1, NULL);
    r.name = "Decoder/ObjectsValid/TokenThenValue";
    run_reset_decoder(&r, trim_object(objects));
    read_token_is(&r, '{', -1, NULL);
    read_token_is(&r, '"', -1, NULL);
    read_value_is(&r, (MAX_DEPTH - 1) * 5 + 2, -1, NULL);
    read_token_is(&r, '}', -1, NULL);
    r.name = "Decoder/ObjectsValid/AllTokens";
    run_reset_decoder(&r, trim_object(objects));
    for (int i = 0; i < MAX_DEPTH; i++) {
        read_token_is(&r, '{', -1, NULL);
        read_token_is(&r, '"', -1, NULL);
    }
    read_token_is(&r, '"', -1, NULL);
    for (int i = 0; i < MAX_DEPTH; i++)
        read_token_is(&r, '}', -1, NULL);

    r.name = "Decoder/ObjectsInvalid/SingleValue";
    run_reset_decoder(&r, objects);
    read_value_is(&r, 0, ooff, "/");
    r.name = "Decoder/ObjectsInvalid/TokenThenValue";
    run_reset_decoder(&r, objects);
    read_token_is(&r, '{', -1, NULL);
    read_token_is(&r, '"', -1, NULL);
    read_value_is(&r, 0, ooff, "/");
    r.name = "Decoder/ObjectsInvalid/AllTokens";
    run_reset_decoder(&r, objects);
    for (int i = 0; i < MAX_DEPTH; i++) {
        read_token_is(&r, '{', -1, NULL);
        read_token_is(&r, '"', -1, NULL);
    }
    read_token_is(&r, 0, ooff, "/");

    JsontextToken empty = jsontext_string(BURROW_S(""));
    r.name = "Encoder/Arrays/SingleValue";
    run_reset_encoder(&r);
    write_value_is(&r, arrays, aoff, "/0");
    write_value_is(&r, trim_array(arrays), -1, NULL);
    r.name = "Encoder/Arrays/TokenThenValue";
    run_reset_encoder(&r);
    write_token_is(&r, jsontext_begin_array, -1, NULL);
    write_value_is(&r, trim_array(arrays), aoff, "/0");
    write_value_is(&r, trim_array(trim_array(arrays)), -1, NULL);
    write_token_is(&r, jsontext_end_array, -1, NULL);
    r.name = "Encoder/Arrays/AllTokens";
    run_reset_encoder(&r);
    for (int i = 0; i < MAX_DEPTH; i++)
        write_token_is(&r, jsontext_begin_array, -1, NULL);
    write_token_is(&r, jsontext_begin_array, aoff, "/0");
    for (int i = 0; i < MAX_DEPTH; i++)
        write_token_is(&r, jsontext_end_array, -1, NULL);

    r.name = "Encoder/Objects/SingleValue";
    run_reset_encoder(&r);
    write_value_is(&r, objects, ooff, "/");
    write_value_is(&r, trim_object(objects), -1, NULL);
    r.name = "Encoder/Objects/TokenThenValue";
    run_reset_encoder(&r);
    write_token_is(&r, jsontext_begin_object, -1, NULL);
    write_token_is(&r, empty, -1, NULL);
    write_value_is(&r, trim_object(objects), ooff, "/");
    write_value_is(&r, trim_object(trim_object(objects)), -1, NULL);
    write_token_is(&r, jsontext_end_object, -1, NULL);
    r.name = "Encoder/Objects/AllTokens";
    run_reset_encoder(&r);
    for (int i = 0; i < MAX_DEPTH; i++) {
        write_token_is(&r, jsontext_begin_object, -1, NULL);
        write_token_is(&r, empty, -1, NULL);
    }
    write_token_is(&r, jsontext_begin_object, ooff, "/");
    write_token_is(&r, empty, -1, NULL);
    for (int i = 0; i < MAX_DEPTH; i++)
        write_token_is(&r, jsontext_end_object, -1, NULL);

    jsontext_encoder_free(r.e);
    jsontext_decoder_free(r.d);
    arena_free(&ar);
}

/* -------------------------------------------------------- reset and I/O */

/* A reader or writer over a BytesBuffer that hides the type, which is what
 * Go's struct{ io.Writer }{bb} does. */
static Int masked_read(void *self, Slice p, Error *err) {
    return bytes_buffer_read((BytesBuffer *)self, p, err);
}

static Int masked_write(void *self, Slice p, Error *err) {
    return bytes_buffer_write((BytesBuffer *)self, p, err);
}

static const IoReaderVT masked_reader_vt = {NULL, masked_read};
static const IoWriterVT masked_writer_vt = {NULL, masked_write};

static Str str_concat(Alloc *a, Str x, Str y) {
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)(x.len + y.len + 1), 1);
    if (p == NULL)
        return (Str){NULL, 0};
    if (x.len > 0)
        memcpy(p, x.p, (size_t)x.len);
    if (y.len > 0)
        memcpy(p + x.len, y.p, (size_t)y.len);
    return str_from_bytes(p, x.len + y.len);
}

static Str buffer_text(BytesBuffer *bb) {
    Slice b = bytes_buffer_bytes(bb);
    return str_from_bytes((const Byte *)b.p, b.len);
}

static const char large_json[] =
    "{\"key1\":\"value1\",\"key2\":\"value2\",\"key3\":\"value3\","
    "\"key4\":\"value4\",\"key5\":\"value5\"}";

static void TestEncoderReset(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer bb = BYTES_BUFFER(a);
    Str want = str_from_cstr(large_json);
    Str wantnl = str_concat(a, want, BURROW_S("\n"));
    JsontextEncoder *e =
        jsontext_new_encoder_v(a, (IoWriter){&masked_writer_vt, &bb}, 0);

    /* The first value grows the buffers. */
    Slice v = jsontext_encoder_available_buffer(e);
    if (v.cap >= wantnl.len) {
        memcpy(v.p, wantnl.p, (size_t)wantnl.len);
        v.len = wantnl.len;
    } else {
        v = str_slice(wantnl);
    }
    Error err = jsontext_encoder_write_value(e, v);
    if (BURROW_FAILED(err) || !str_eq(buffer_text(&bb), wantnl))
        testing_t_errorf_v(t, "first WriteValue = %q, %v", buffer_text(&bb), err);
    Int cap1 = e->buf.cap, acap1 = e->avail.cap;
    if (cap1 == 0 || acap1 == 0)
        testing_t_errorf_v(t, "capacities after first use: %d, %d", (int)cap1,
                           (int)acap1);

    /* Reset keeps them. */
    bytes_buffer_reset(&bb);
    jsontext_encoder_reset_v(e, (IoWriter){&masked_writer_vt, &bb}, 0);
    if (e->buf.cap < cap1 || e->avail.cap < acap1)
        testing_t_errorf_v(t,
                           "capacity reduced after Reset: %d, %d; want at least %d, %d",
                           (int)e->buf.cap, (int)e->avail.cap, (int)cap1, (int)acap1);
    err = jsontext_encoder_write_value(e, str_slice(wantnl));
    if (BURROW_FAILED(err) || !str_eq(buffer_text(&bb), wantnl))
        testing_t_errorf_v(t, "second WriteValue = %q, %v", buffer_text(&bb), err);

    /* A plain BytesBuffer and then a masked one again. */
    for (int k = 0; k < 2; k++) {
        bytes_buffer_reset(&bb);
        IoWriter w = k == 0 ? bytes_buffer_as_io_writer(&bb)
                            : (IoWriter){&masked_writer_vt, &bb};
        jsontext_encoder_reset_v(e, w, 0);
        err = jsontext_encoder_write_value(e, str_slice(want));
        if (BURROW_FAILED(err) || !str_eq(buffer_text(&bb), wantnl))
            testing_t_errorf_v(t, "%d: WriteValue = %q, %v", k, buffer_text(&bb), err);
    }
    jsontext_encoder_free(e);
    arena_free(&ar);
}

static void TestDecoderReset(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str want = str_from_cstr(large_json);
    JsontextDecoder *d = jsontext_new_decoder_v(a, string_reader(a, want), 0);
    Error err;
    JsontextValue v = jsontext_decoder_read_value(d, &err);
    if (BURROW_FAILED(err) || !str_eq(jsontext_value_string(v), want))
        testing_t_errorf_v(t, "first ReadValue = %q, %v", jsontext_value_string(v),
                           err);
    /* string_reader is a BytesBuffer, which the decoder reads in place, so
     * go through a masked one to make it grow its own buffer. */
    BytesBuffer *src = bytes_new_buffer_string(a, want);
    jsontext_decoder_reset_v(d, (IoReader){&masked_reader_vt, src}, 0);
    v = jsontext_decoder_read_value(d, &err);
    if (BURROW_FAILED(err) || !str_eq(jsontext_value_string(v), want))
        testing_t_errorf_v(t, "second ReadValue = %q, %v", jsontext_value_string(v),
                           err);
    Int cap1 = d->db.cap;
    if (cap1 == 0 || !d->owns_buf)
        testing_t_errorf_v(t, "no buffer of its own after first use");
    src = bytes_new_buffer_string(a, want);
    jsontext_decoder_reset_v(d, (IoReader){&masked_reader_vt, src}, 0);
    if (d->db.cap < cap1)
        testing_t_errorf_v(t, "capacity reduced after Reset: %d, want at least %d",
                           (int)d->db.cap, (int)cap1);
    v = jsontext_decoder_read_value(d, &err);
    if (BURROW_FAILED(err) || !str_eq(jsontext_value_string(v), want))
        testing_t_errorf_v(t, "third ReadValue = %q, %v", jsontext_value_string(v),
                           err);

    /* A BytesBuffer is read in place. */
    BytesBuffer *bb = bytes_new_buffer_string(a, want);
    const Byte *bbp = (const Byte *)bytes_buffer_bytes(bb).p;
    jsontext_decoder_reset_v(d, bytes_buffer_as_io_reader(bb), 0);
    v = jsontext_decoder_read_value(d, &err);
    if (BURROW_FAILED(err) || !str_eq(jsontext_value_string(v), want))
        testing_t_errorf_v(t, "fourth ReadValue = %q, %v", jsontext_value_string(v),
                           err);
    if (d->db.len == 0 || d->db.buf != bbp)
        testing_t_errorf_v(t, "decoder buffer does not alias the BytesBuffer");

    /* And after the next Reset it no longer is. */
    src = bytes_new_buffer_string(a, want);
    jsontext_decoder_reset_v(d, (IoReader){&masked_reader_vt, src}, 0);
    v = jsontext_decoder_read_value(d, &err);
    if (BURROW_FAILED(err) || !str_eq(jsontext_value_string(v), want))
        testing_t_errorf_v(t, "fifth ReadValue = %q, %v", jsontext_value_string(v),
                           err);
    if (d->db.buf == bbp)
        testing_t_errorf_v(t, "decoder buffer still aliases the BytesBuffer");
    jsontext_decoder_free(d);
    arena_free(&ar);
}

static void TestBufferDecoder(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer *bb = bytes_new_buffer_string(a, BURROW_S("[null, false, true]"));
    JsontextDecoder *d = jsontext_new_decoder_v(a, bytes_buffer_as_io_reader(bb), 0);
    Error err;
    for (int guard = 0; guard < 100; guard++) {
        jsontext_decoder_read_token(d, &err);
        if (BURROW_FAILED(err))
            break;
        /* Not allowed while the decoder holds on to what Next gave it. */
        bytes_buffer_write_byte(bb, ' ');
    }
    if (!text_is(err_text(err),
                 "jsontext: read error: invalid bytes.Buffer.Write call after calling "
                 "bytes.Buffer.Next"))
        testing_t_errorf_v(t, "error = %v", err);
    jsontext_decoder_free(d);
    arena_free(&ar);
}

/* iotest.OneByteReader. */
static Int one_byte_read(void *self, Slice p, Error *err) {
    if (p.len == 0) {
        *err = BURROW_NO_ERROR;
        return 0;
    }
    p.len = 1;
    return bytes_buffer_read((BytesBuffer *)self, p, err);
}

static const IoReaderVT one_byte_reader_vt = {NULL, one_byte_read};

static const char *const resumable_cases[] = {
    "0",
    "123456789",
    "0.0",
    "0.123456789",
    "0e0",
    "0e+0",
    "0e123456789",
    "0e+123456789",
    "123456789.123456789e+123456789",
    "-0",
    "-123456789",
    "-0.0",
    "-0.123456789",
    "-0e0",
    "-0e-0",
    "-0e123456789",
    "-0e-123456789",
    "-123456789.123456789e-123456789",
    "\"\"",
    "\"a\"",
    "\"ab\"",
    "\"abc\"",
    "\"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz\"",
    "\"\\\"\\\\\\/\\b\\f\\n\\r\\t\"",
    "\"\\u0022\\u005c\\u002f\\u0008\\u000c\\u000a\\u000d\\u0009\"",
    "\"\\ud800\\udead\"",
    ("\"\xc2\x80\xc3\xb6\xe2\x82\xac\xed\x9e\x99\xee\x80\x80\xef\xac\xb3\xef\xbf\xbd"
     "\xf0\x9f\x98"
     "\x82\""),
    "\"\\u0080\\u00f6\\u20ac\\ud799\\ue000\\ufb33\\ufffd\\ud83d\\ude02\"",
};

static void TestResumableDecoder(TestingT *t) {
    for (size_t i = 0; i < LEN(resumable_cases); i++) {
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        Str want = str_from_cstr(resumable_cases[i]);
        BytesBuffer *src = bytes_new_buffer_string(a, want);
        JsontextDecoder *d =
            jsontext_new_decoder_v(a, (IoReader){&one_byte_reader_vt, src}, 0);
        Error err;
        JsontextValue v = jsontext_decoder_read_value(d, &err);
        if (BURROW_FAILED(err) || !str_eq(jsontext_value_string(v), want))
            testing_t_errorf_v(t, "%d: ReadValue = %q, %v; want %q", (int)i,
                               jsontext_value_string(v), err, want);
        jsontext_decoder_free(d);
        arena_free(&ar);
    }
}

/* The reader side of a pipe with nothing else running: a read with nothing
 * written is the deadlock Go's test would hang on, so it fails instead. */
typedef struct Pipe {
    BytesBuffer bb;
    int stalls;
} Pipe;

static Int pipe_read(void *self, Slice p, Error *err) {
    Pipe *pp = (Pipe *)self;
    if (bytes_buffer_len(&pp->bb) == 0) {
        pp->stalls++;
        *err = io_err_no_progress;
        return 0;
    }
    return bytes_buffer_read(&pp->bb, p, err);
}

static Int pipe_write(void *self, Slice p, Error *err) {
    return bytes_buffer_write(&((Pipe *)self)->bb, p, err);
}

static const IoReaderVT pipe_reader_vt = {NULL, pipe_read};
static const IoWriterVT pipe_writer_vt = {NULL, pipe_write};

static void TestBlockingDecoder(TestingT *t) {
    static const char *const values[] = {"null", "false", "true", "\"\"", "{}", "[]"};
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Pipe pp = {BYTES_BUFFER(a), 0};
    JsontextOptions o;
    memset(&o, 0, sizeof(o));
    o.presence = o.values = JSONFLAG_OMIT_TOP_LEVEL_NEWLINE;
    JsontextEncoder *e =
        jsontext_new_encoder(a, (IoWriter){&pipe_writer_vt, &pp}, opts_slice(&o, 1));
    JsontextDecoder *d = jsontext_new_decoder_v(a, (IoReader){&pipe_reader_vt, &pp}, 0);
    for (int pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < LEN(values); i++) {
            Str want = str_from_cstr(values[i]);
            Error err = jsontext_encoder_write_value(e, str_slice(want));
            if (BURROW_FAILED(err)) {
                testing_t_errorf_v(t, "WriteValue error: %v", err);
                continue;
            }
            Str got;
            if (pass == 0) {
                JsontextToken tok = jsontext_decoder_read_token(d, &err);
                got = jsontext_token_string(tok, a);
                JsontextKind k = jsontext_token_kind(tok);
                if (k == '"') {
                    got = str_concat(a, BURROW_S("\""), got);
                    got = str_concat(a, got, BURROW_S("\""));
                } else if (BURROW_OK(err) && (k == '{' || k == '[')) {
                    tok = jsontext_decoder_read_token(d, &err);
                    got = str_concat(a, got, jsontext_token_string(tok, a));
                }
            } else {
                got = jsontext_value_string(jsontext_decoder_read_value(d, &err));
            }
            if (BURROW_FAILED(err) || !str_eq(got, want))
                testing_t_errorf_v(t, "%d: read %q, %v; want %q", pass, got, err, want);
        }
    }
    if (pp.stalls != 0)
        testing_t_errorf_v(t, "decoder read an empty pipe %d times", pp.stalls);
    jsontext_encoder_free(e);
    jsontext_decoder_free(d);
    arena_free(&ar);
}

/* A SyntacticError with the error text inner under it. */
static bool syntax_error_under(Error err, const char *inner) {
    const JsontextSyntacticError *se = errors_as(err, TYPE_JSONTEXT_SYNTACTIC_ERROR);
    return se != NULL && text_is(err_text(se->err), inner);
}

/* Go's E(err).withPos(prefix, pointer): a SyntacticError at len(prefix). */
static bool syntax_error_is(Error err, int64_t offset, const char *pointer,
                            const char *inner) {
    const JsontextSyntacticError *se = errors_as(err, TYPE_JSONTEXT_SYNTACTIC_ERROR);
    return se != NULL && se->byte_offset == offset &&
           text_is(se->json_pointer, pointer) && text_is(err_text(se->err), inner);
}

typedef struct PeekOp {
    char op; /* p PeekKind, t ReadToken, v ReadValue, w write */
    JsontextKind kind;
    const char *text; /* what to write, or the prefix before the error */
    const char *pointer;
    const char *err; /* NULL for none, "EOF" for a bare io.EOF */
} PeekOp;

static void TestPeekableDecoder(TestingT *t) {
    static const PeekOp ops[] = {
        {'p', 0, NULL, NULL, NULL},
        {'w', 0, "[ ", NULL, NULL},
        {'t', 0, NULL, NULL, "EOF"},
        {'t', '[', NULL, NULL, NULL},

        {'p', 0, NULL, NULL, NULL},
        {'w', 0, "] ", NULL, NULL},
        {'v', 0, "[ ", "", "unexpected EOF"},
        {'v', 0, "[ ", "/0", "invalid character ']' at start of value"},
        {'t', ']', NULL, NULL, NULL},

        {'w', 0, "[ ", NULL, NULL},
        {'t', '[', NULL, NULL, NULL},

        {'w', 0, " null ", NULL, NULL},
        {'p', 'n', NULL, NULL, NULL},
        {'p', 'n', NULL, NULL, NULL},
        {'t', 'n', NULL, NULL, NULL},

        {'w', 0, ", ", NULL, NULL},
        {'p', 0, NULL, NULL, NULL},
        {'w', 0, "fal", NULL, NULL},
        {'p', 'f', NULL, NULL, NULL},
        {'v', 0, "[ ] [  null , fal", "/1", "unexpected EOF"},
        {'w', 0, "se ", NULL, NULL},
        {'v', 'f', NULL, NULL, NULL},

        {'p', 0, NULL, NULL, NULL},
        {'w', 0, " , ", NULL, NULL},
        {'p', 0, NULL, NULL, NULL},
        {'w', 0, " \"\" ", NULL, NULL},
        {'v', 0, "[ ] [  null , false  , ", "", "unexpected EOF"},
        {'v', '"', NULL, NULL, NULL},

        {'w', 0, " , 0", NULL, NULL},
        {'p', '0', NULL, NULL, NULL},
        {'t', '0', NULL, NULL, NULL},

        {'w', 0, " , {} , []", NULL, NULL},
        {'p', '{', NULL, NULL, NULL},
        {'v', '{', NULL, NULL, NULL},
        {'v', '[', NULL, NULL, NULL},

        {'w', 0, "]", NULL, NULL},
        {'t', ']', NULL, NULL, NULL},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer bb = BYTES_BUFFER(a);
    JsontextDecoder *d =
        jsontext_new_decoder_v(a, (IoReader){&masked_reader_vt, &bb}, 0);
    for (size_t i = 0; i < LEN(ops); i++) {
        const PeekOp *op = &ops[i];
        Error err = BURROW_NO_ERROR;
        JsontextKind got = 0;
        switch (op->op) {
        case 'p':
            got = jsontext_decoder_peek_kind(d);
            break;
        case 't':
            got = jsontext_token_kind(jsontext_decoder_read_token(d, &err));
            break;
        case 'v':
            got = jsontext_value_kind(jsontext_decoder_read_value(d, &err));
            break;
        default:
            bytes_buffer_write_string(&bb, str_from_cstr(op->text), NULL);
            continue;
        }
        bool ok;
        if (op->err == NULL)
            ok = BURROW_OK(err);
        else if (strcmp(op->err, "EOF") == 0)
            ok = errors_is(err, io_eof) &&
                 errors_as(err, TYPE_JSONTEXT_SYNTACTIC_ERROR) == NULL;
        else
            ok = syntax_error_is(err, (int64_t)strlen(op->text), op->pointer, op->err);
        if (got != op->kind || !ok)
            testing_t_errorf_v(t, "%d: %c = (%c, %v), want %c", (int)i, op->op, got,
                               err, op->kind);
    }
    jsontext_decoder_free(d);
    arena_free(&ar);
}

/* ---------------------------------------------------------------- tokens */

/* Whether the last EXPECT_PANIC caught want, "" meaning it caught nothing. */
static bool panicked_with(const char *want) {
    if (*want == '\0')
        return !fatal_did_catch;
    return fatal_did_catch && strcmp(fatal_caught, want) == 0;
}

static bool err_is_text(Error err, const char *want) {
    return text_is(err_text(err), want);
}

static void TestTokenAccessors(TestingT *t) {
    for (size_t i = 0; i < LEN(jt_token_cases); i++) {
        const JtTokCase *c = &jt_token_cases[i];
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        JsontextToken tok = make_token(&c->tok, a);
        Str name = jsontext_token_string(tok, a);
        if (!str_eq(name, qstr(c->string)))
            testing_t_errorf_v(t, "%d: String = %q, want %q", (int)i, name,
                               qstr(c->string));
        if (jsontext_token_kind(tok) != (JsontextKind)c->kind)
            testing_t_errorf_v(t, "%q: Kind = %c, want %c", name,
                               jsontext_token_kind(tok), c->kind);

        volatile bool b = false;
        EXPECT_PANIC(b = jsontext_token_bool(tok));
        if (!panicked_with(c->bool_panic) ||
            (!fatal_did_catch && b != (c->bool_value != 0)))
            testing_t_errorf_v(t, "%q: Bool = %t, panic %s; want %t, panic %s", name, b,
                               fatal_caught, c->bool_value != 0, c->bool_panic);

        Error err = BURROW_NO_ERROR;
        volatile float f32 = 0;
        EXPECT_PANIC(f32 = jsontext_token_float32(tok, &err));
        uint32_t f32b;
        float f32v = f32;
        memcpy(&f32b, &f32v, sizeof f32b);
        if (!panicked_with(c->f32_panic) ||
            (!fatal_did_catch && (f32b != c->f32 || !err_is_text(err, c->f32_err))))
            testing_t_errorf_v(
                t, "%q: Float32 = %#x, %v, panic %s; want %#x, %s, panic %s", name,
                f32b, err, fatal_caught, c->f32, c->f32_err, c->f32_panic);

        err = BURROW_NO_ERROR;
        volatile double f64 = 0;
        EXPECT_PANIC(f64 = jsontext_token_float(tok, &err));
        uint64_t f64b;
        double f64v = f64;
        memcpy(&f64b, &f64v, sizeof f64b);
        if (!panicked_with(c->f64_panic) ||
            (!fatal_did_catch && (f64b != c->f64 || !err_is_text(err, c->f64_err))))
            testing_t_errorf_v(
                t, "%q: Float = %#x, %v, panic %s; want %#x, %s, panic %s", name, f64b,
                err, fatal_caught, c->f64, c->f64_err, c->f64_panic);

        err = BURROW_NO_ERROR;
        volatile int64_t i64 = 0;
        EXPECT_PANIC(i64 = jsontext_token_int(tok, &err));
        if (!panicked_with(c->i64_panic) ||
            (!fatal_did_catch &&
             ((uint64_t)i64 != c->i64 || !err_is_text(err, c->i64_err))))
            testing_t_errorf_v(t, "%q: Int = %v, %v, panic %s; want %v, %s, panic %s",
                               name, i64, err, fatal_caught, (int64_t)c->i64,
                               c->i64_err, c->i64_panic);

        err = BURROW_NO_ERROR;
        volatile uint64_t u64 = 0;
        EXPECT_PANIC(u64 = jsontext_token_uint(tok, &err));
        if (!panicked_with(c->u64_panic) ||
            (!fatal_did_catch && (u64 != c->u64 || !err_is_text(err, c->u64_err))))
            testing_t_errorf_v(t, "%q: Uint = %v, %v, panic %s; want %v, %s, panic %s",
                               name, u64, err, fatal_caught, c->u64, c->u64_err,
                               c->u64_panic);
        arena_free(&ar);
    }
}

static void TestTokenClone(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    JsontextToken zero = {NULL, {NULL, 0}, 0};
    struct {
        JsontextToken in;
        bool exact_raw;
    } tests[] = {
        {zero, true},
        {jsontext_null, true},
        {jsontext_false, true},
        {jsontext_true, true},
        {jsontext_begin_object, true},
        {jsontext_end_object, true},
        {jsontext_begin_array, true},
        {jsontext_end_array, true},
        {jsontext_string(BURROW_S("hello, world!")), true},
        {make_token(&(JtTok){'r', QS("\"hello, world!\""), 0}, a), false},
        {jsontext_float(3.14159), true},
        {make_token(&(JtTok){'r', QS("3.14159"), 0}, a), false},
    };
    for (size_t i = 0; i < LEN(tests); i++) {
        JsontextToken in = tests[i].in;
        JsontextToken got = jsontext_token_clone(in, a);
        Str ws = jsontext_token_string(in, a), gs = jsontext_token_string(got, a);
        if (!str_eq(gs, ws) || jsontext_token_kind(got) != jsontext_token_kind(in) ||
            got.num != in.num)
            testing_t_errorf_v(t, "Token(%s).Clone() = %s, not equal", ws, gs);
        if ((got.raw == in.raw) != tests[i].exact_raw)
            testing_t_errorf_v(t, "Token(%s).Clone().raw == raw = %t, want %t", ws,
                               got.raw == in.raw, tests[i].exact_raw);
    }
    arena_free(&ar);
}

/* ------------------------------------------------------ pointers and state */

typedef struct TokenList {
    Alloc *a;
    Str items[8];
    int n;
} TokenList;

static bool collect_token(void *env, const void *v) {
    TokenList *l = (TokenList *)env;
    const Str *s = (const Str *)v;
    if (l->n < 8) {
        Byte *p = (Byte *)mem_alloc_nozero(l->a, (size_t)s->len + 1, 1);
        if (p != NULL && s->len > 0)
            memcpy(p, s->p, (size_t)s->len);
        l->items[l->n] = str_from_bytes(p, p != NULL ? s->len : 0);
    }
    l->n++;
    return true;
}

static void TestPointer(TestingT *t) {
    static const struct {
        const char *in, *parent, *last, *roundtrip;
        const char *tokens[3];
        int ntok;
        bool valid;
    } tests[] = {
        {"", "", "", "", {NULL}, 0, true},
        {"a", "", "a", NULL, {"a"}, 1, false},
        {"~", "", "~", NULL, {"~"}, 1, false},
        {"/a", "", "a", "/a", {"a"}, 1, true},
        {"/foo/bar", "/foo", "bar", "/foo/bar", {"foo", "bar"}, 2, true},
        {"///", "//", "", "///", {"", "", ""}, 3, true},
        {"/~0~1", "", "~/", "/~0~1", {"~/"}, 1, true},
        /* Appending turns the bad bytes into U+FFFD, as Go's []rune does. */
        {"/\xde\xad\xbe\xef",
         "",
         "\xde\xad\xbe\xef",
         "/\xde\xad\xef\xbf\xbd\xef\xbf\xbd",
         {"\xde\xad\xbe\xef"},
         1,
         false},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < LEN(tests); i++) {
        Str in = str_from_cstr(tests[i].in);
        Str parent = jsontext_pointer_parent(in);
        if (!text_is(parent, tests[i].parent))
            testing_t_errorf_v(t, "Pointer(%q).Parent = %q, want %s", in, parent,
                               tests[i].parent);
        Str last = jsontext_pointer_last_token(in, a);
        if (!text_is(last, tests[i].last))
            testing_t_errorf_v(t, "Pointer(%q).LastToken = %q", in, last);
        if (tests[i].roundtrip != NULL && tests[i].roundtrip[0] == '/') {
            Str got = jsontext_pointer_append_token(parent, a, last);
            if (!text_is(got, tests[i].roundtrip))
                testing_t_errorf_v(
                    t, "Pointer(%q).Parent().AppendToken(LastToken()) = %q", in, got);
            Str p = in;
            for (;;) {
                Str px = str_concat(a, p, BURROW_S("x"));
                if (jsontext_pointer_contains(px, in))
                    testing_t_errorf_v(t, "Pointer(%q).Contains(%q) = true", px, in);
                if (!jsontext_pointer_contains(p, in))
                    testing_t_errorf_v(t, "Pointer(%q).Contains(%q) = false", p, in);
                Str pp = jsontext_pointer_parent(p);
                if (str_eq(pp, p))
                    break;
                p = pp;
            }
        }
        TokenList l = {a, {{NULL, 0}}, 0};
        IterSeq seq = jsontext_pointer_tokens(in, a);
        BURROW_CALLF(seq, BURROW_FN(IterYield, collect_token, &l));
        bool same = l.n == tests[i].ntok;
        for (int k = 0; same && k < l.n; k++)
            same = text_is(l.items[k], tests[i].tokens[k]);
        if (!same)
            testing_t_errorf_v(t, "Pointer(%q).Tokens gave %d tokens, want %d", in, l.n,
                               tests[i].ntok);
        if (jsontext_pointer_is_valid(in) != tests[i].valid)
            testing_t_errorf_v(t, "Pointer(%q).IsValid = %t", in, !tests[i].valid);
    }
    arena_free(&ar);
}

static JsontextToken kind_token(char k) {
    switch (k) {
    case 'n':
        return jsontext_null;
    case 'f':
        return jsontext_false;
    case 't':
        return jsontext_true;
    case '"':
        return jsontext_string(BURROW_S("x"));
    case '0':
        return jsontext_int(0);
    case '{':
        return jsontext_begin_object;
    case '}':
        return jsontext_end_object;
    case '[':
        return jsontext_begin_array;
    default:
        return jsontext_end_array;
    }
}

/* Go's TestStateMachine drives stateMachine directly. The encoder is a thin
 * layer over it, so this sends the same kinds through WriteToken and reads the
 * lengths back with StackIndex. Ops are a string: a kind appends a token that
 * must work, '!' then a kind and an error letter appends one that must fail
 * (s ErrNonStringName, v errMissingValue, d errMismatchDelim), and '=' then
 * digits separated by ',' and ended by ';' checks the stack lengths. Where the
 * commas and colons go, Go's needDelim cases, is what the coder tests see. */
static void TestStateMachine(TestingT *t) {
    static const struct {
        const char *label, *ops;
    } tests[] = {
        {"TopLevelValues", "=0;nft=3;\"0[]{}=7;"},
        {"ArrayValues", "=0;[=1,0;nft=1,3;\"0[]{}=1,7;]=1;"},
        {"ObjectValues", "=0;{=1,0;\"=1,1;n=1,2;\"f\"t=1,6;\"\"\"0\"[]\"{}=1,14;}=1;"},
        {"ObjectCardinality", "{!ns!fs!ts!0s!{s![s\"!}v\"}"},
        {"MismatchingDelims", "!}d[[{!]d}]!}d]!]d"},
    };
    for (size_t i = 0; i < LEN(tests); i++) {
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        BytesBuffer bb = BYTES_BUFFER(a);
        JsontextEncoder *e = jsontext_new_encoder_v(
            a, bytes_buffer_as_io_writer(&bb), 1, jsontext_allow_duplicate_names(true));
        const char *op = tests[i].ops;
        while (*op != '\0') {
            if (*op == '=') {
                op++;
                Int depth = jsontext_encoder_stack_depth(e);
                Int k = 0;
                bool ok = true;
                for (;;) {
                    long want = 0;
                    while (*op >= '0' && *op <= '9')
                        want = want * 10 + (*op++ - '0');
                    int64_t got = -1;
                    if (k > depth)
                        ok = false;
                    else
                        jsontext_encoder_stack_index(e, k, &got);
                    if (got != want)
                        ok = false;
                    k++;
                    if (*op++ == ';')
                        break;
                }
                if (!ok || k != depth + 1)
                    testing_t_errorf_v(t, "%s: stack lengths differ before %s",
                                       tests[i].label, op);
                continue;
            }
            if (*op == '!') {
                Error err = jsontext_encoder_write_token(e, kind_token(op[1]));
                bool ok = false;
                if (op[2] == 's')
                    ok = errors_is(err, jsontext_err_non_string_name);
                else if (op[2] == 'v')
                    ok = syntax_error_under(err, "missing value after object name");
                else
                    ok = syntax_error_under(
                        err, "mismatching structural token for object or array");
                if (!ok)
                    testing_t_errorf_v(t, "%s: append('%c') = %v, want error %c",
                                       tests[i].label, op[1], err, op[2]);
                op += 3;
                continue;
            }
            Error err = jsontext_encoder_write_token(e, kind_token(*op));
            if (BURROW_FAILED(err))
                testing_t_errorf_v(t, "%s: append('%c') = %v", tests[i].label, *op,
                                   err);
            op++;
        }
        jsontext_encoder_free(e);
        arena_free(&ar);
    }
}

/* Go's TestObjectNamespace works on objectNamespace itself. Here each insert
 * writes the quoted name into one open object, and removeLast is the unwrite
 * that encoding/json/v2 uses, which drops a member whose value is empty and
 * takes its name out of the namespace. */
static void TestObjectNamespace(TestingT *t) {
    static const struct {
        const char *name; /* NULL for removeLast */
        bool inserted;
    } ops[] = {
        {"\"\"", true},
        {NULL, false},
        {"\"\"", true},
        {"\"\"", false},

        {"\"alpha\"", true},
        {"\"ALPHA\"", true},
        {"\"alpha\"", false},
        {"\"\\u0061\\u006c\\u0070\\u0068\\u0061\"", false},
        {NULL, false},
        {"\"alpha\"", false},
        {NULL, false},
        {"\"alpha\"", true},
        {NULL, false},

        {"\"alpha\"", true},
        {"\"bravo\"", true},
        {"\"charlie\"", true},
        {"\"delta\"", true},
        {"\"echo\"", true},
        {"\"foxtrot\"", true},
        {"\"golf\"", true},
        {"\"hotel\"", true},
        {"\"india\"", true},
        {"\"juliet\"", true},
        {"\"kilo\"", true},
        {"\"lima\"", true},
        {"\"mike\"", true},
        {"\"november\"", true},
        {"\"oscar\"", true},
        {"\"papa\"", true},
        {"\"quebec\"", true},
        {"\"romeo\"", true},
        {"\"sierra\"", true},
        {"\"tango\"", true},
        {"\"uniform\"", true},
        {"\"victor\"", true},
        {"\"whiskey\"", true},
        {"\"xray\"", true},
        {"\"yankee\"", true},
        {"\"zulu\"", true},

        {"\"\xef\xbf\xbd\"", true},
        {"\"\xef\xbf\xbd\"", false},
        {"\"\\ufffd\"", false},
        {"\"\\uFFFD\"", false},
        {"\"\xff\"", false},
        {NULL, false},
        {"\"\xef\xbf\xbd\"", true},

        {"\"\xe2\x98\xba\xe2\x98\xbb\xe2\x98\xb9\"", true},
        {"\"\xe2\x98\xba\xe2\x98\xbb\xe2\x98\xb9\"", false},
        {NULL, false},
        {"\"\xe2\x98\xba\xe2\x98\xbb\xe2\x98\xb9\"", true},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer bb = BYTES_BUFFER(a);
    JsontextEncoder *e = jsontext_new_encoder_v(a, bytes_buffer_as_io_writer(&bb), 1,
                                                jsontext_allow_invalid_utf8(true));
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1) {
            bytes_buffer_reset(&bb);
            jsontext_encoder_reset_v(e, bytes_buffer_as_io_writer(&bb), 1,
                                     jsontext_allow_invalid_utf8(true));
        }
        /* The names that should be in the object, in order. */
        const char *want[64];
        int nwant = 0;
        CHECK(BURROW_OK(jsontext_encoder_write_token(e, jsontext_begin_object)));
        for (size_t i = 0; i < LEN(ops); i++) {
            if (ops[i].name == NULL) {
                if (!burrow__jsontext_unwrite_empty_object_member(e, NULL))
                    testing_t_errorf_v(t, "%d: unwrite failed", (int)i);
                nwant--;
                continue;
            }
            Str name = str_from_cstr(ops[i].name);
            Error err = jsontext_encoder_write_value(e, str_slice(name));
            bool inserted = BURROW_OK(err);
            if (inserted != ops[i].inserted) {
                testing_t_errorf_v(t, "%d: %d: insert(%s) = %t, %v", pass, (int)i,
                                   ops[i].name, inserted, err);
                continue;
            }
            if (!inserted) {
                if (!errors_is(err, jsontext_err_duplicate_name))
                    testing_t_errorf_v(t, "%d: insert(%s) error %v", (int)i,
                                       ops[i].name, err);
                continue;
            }
            want[nwant++] = ops[i].name;
            CHECK(BURROW_OK(jsontext_encoder_write_token(e, jsontext_null)));
        }
        /* Past 64 names Go moves to a map, which has to agree. */
        char buf[64][16];
        for (int i = 0; i < 64; i++) {
            snprintf(buf[i], sizeof buf[i], "name%d", i);
            CHECK(BURROW_OK(jsontext_encoder_write_token(
                e, jsontext_string(str_from_cstr(buf[i])))));
            CHECK(BURROW_OK(jsontext_encoder_write_token(e, jsontext_null)));
        }
        CHECK(errors_is(
            jsontext_encoder_write_token(e, jsontext_string(BURROW_S("name7"))),
            jsontext_err_duplicate_name));
        CHECK(
            errors_is(jsontext_encoder_write_value(e, str_slice(BURROW_S("\"zulu\""))),
                      jsontext_err_duplicate_name));
        CHECK(BURROW_OK(jsontext_encoder_write_token(e, jsontext_end_object)));

        /* What was written is the names that stayed, each with its null. */
        Str wantout = BURROW_S("{");
        for (int i = 0; i < nwant; i++) {
            if (i > 0)
                wantout = str_concat(a, wantout, BURROW_S(","));
            wantout = str_concat(a, wantout, str_from_cstr(want[i]));
            wantout = str_concat(a, wantout, BURROW_S(":null"));
        }
        for (int i = 0; i < 64; i++) {
            wantout = str_concat(a, wantout, BURROW_S(",\""));
            wantout = str_concat(a, wantout, str_from_cstr(buf[i]));
            wantout = str_concat(a, wantout, BURROW_S("\":null"));
        }
        wantout = str_concat(a, wantout, BURROW_S("}\n"));
        if (!str_eq(buffer_text(&bb), wantout))
            testing_t_errorf_v(t, "%d: output = %q, want %q", pass, buffer_text(&bb),
                               wantout);
    }
    jsontext_encoder_free(e);
    arena_free(&ar);
}

/* ------------------------------------------------------------ no memory */

/* An allocator that gives out a set number of blocks and then refuses, and
 * keeps count of what is live, so that nothing leaks when it runs out. */
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

static void *budget_realloc(void *self, void *p, size_t old, size_t nsz, size_t align) {
    Budget *b = (Budget *)self;
    if (b->left <= 0)
        return NULL;
    b->left--;
    void *q = mem_realloc(heap_allocator(), p, old, nsz, align);
    if (q != NULL)
        b->live += (long long)nsz - (long long)old;
    return q;
}

static void budget_free(void *self, void *p, size_t size, size_t align) {
    Budget *b = (Budget *)self;
    if (p == NULL)
        return;
    b->live -= (long long)size;
    mem_free(heap_allocator(), p, size, align);
}

static const AllocVT budget_vt = {budget_alloc, NULL, budget_realloc,
                                  budget_free,  NULL, NULL};

static bool ok_or_oom(Error err) {
    return BURROW_OK(err) || errors_is(err, burrow_err_out_of_memory);
}

/* Every coder case through a decoder, an encoder and the Value methods with
 * the allocator giving out fewer and fewer blocks: each call works or says
 * out of memory, and freeing gives everything back. With enough blocks the
 * output is the usual one. */
static void TestNoMemory(TestingT *t) {
    int finished = 0;
    for (size_t i = 0; i < LEN(jt_coder_cases); i++) {
        const JtCoderCase *c = &jt_coder_cases[i];
        Str in = qstr(c->in);
        for (int budget = 0; budget < 64; budget++) {
            Budget b = {budget, 0};
            Alloc al = {&budget_vt, &b, NULL, NULL};
            BytesBuffer src = BYTES_BUFFER(heap_allocator());
            bytes_buffer_write_string(&src, in, NULL);
            BytesBuffer dst = BYTES_BUFFER(heap_allocator());
            JsontextDecoder *d =
                jsontext_new_decoder_v(&al, (IoReader){&masked_reader_vt, &src}, 0);
            JsontextEncoder *e = jsontext_new_encoder_v(
                &al, bytes_buffer_as_io_writer(&dst), 3, jsontext_multiline(true),
                jsontext_with_indent_prefix(BURROW_S("\t")),
                jsontext_with_indent(BURROW_S("    ")));
            bool done = false;
            if (d != NULL && e != NULL) {
                for (int guard = 0; guard < 1000; guard++) {
                    Error err;
                    JsontextToken tok = jsontext_decoder_read_token(d, &err);
                    if (errors_is(err, io_eof)) {
                        done = true;
                        break;
                    }
                    if (!ok_or_oom(err))
                        testing_t_errorf_v(t, "%s, budget %d: ReadToken %v", c->name,
                                           budget, err);
                    if (BURROW_FAILED(err))
                        break;
                    err = jsontext_encoder_write_token(e, tok);
                    if (!ok_or_oom(err))
                        testing_t_errorf_v(t, "%s, budget %d: WriteToken %v", c->name,
                                           budget, err);
                    if (BURROW_FAILED(err))
                        break;
                }
            }
            if (done) {
                Str out = trim_space(buffer_text(&dst));
                if (!str_eq(out, qstr(c->indented)))
                    testing_t_errorf_v(t, "%s, budget %d: output %q", c->name, budget,
                                       out);
            }
            if (e != NULL)
                jsontext_encoder_free(e);
            if (d != NULL)
                jsontext_decoder_free(d);

            JsontextValue v = {NULL, 0, 0, TYPE_BYTE};
            Error err = jsontext_value_unmarshal_json(&v, &al, str_slice(in));
            if (!ok_or_oom(err))
                testing_t_errorf_v(t, "%s, budget %d: UnmarshalJSON %v", c->name,
                                   budget, err);
            bool vdone = false;
            if (BURROW_OK(err)) {
                err = jsontext_value_canonicalize_v(&v, &al, 0);
                if (!ok_or_oom(err))
                    testing_t_errorf_v(t, "%s, budget %d: Canonicalize %v", c->name,
                                       budget, err);
                if (BURROW_OK(err)) {
                    vdone = true;
                    if (!str_eq(jsontext_value_string(v), qstr(c->canonicalized)))
                        testing_t_errorf_v(t, "%s, budget %d: Canonicalize = %q",
                                           c->name, budget, jsontext_value_string(v));
                }
                if (v.p != NULL)
                    mem_free(&al, v.p, (size_t)v.cap, 1);
            }
            bytes_buffer_free(&src);
            bytes_buffer_free(&dst);
            if (b.live != 0)
                testing_t_errorf_v(t, "%s, budget %d: %d bytes leaked", c->name, budget,
                                   (int)b.live);
            if (done && vdone) {
                finished++;
                break;
            }
        }
    }
    if (finished != (int)LEN(jt_coder_cases))
        testing_t_errorf_v(t, "only %d of %d cases finished within the budgets",
                           finished, (int)LEN(jt_coder_cases));
}

#define TESTS(X)                                                                       \
    X(TestEncoder)                                                                     \
    X(TestDecoder)                                                                     \
    X(TestFaultyEncoder)                                                               \
    X(TestFaultyDecoder)                                                               \
    X(TestEncoderErrors)                                                               \
    X(TestDecoderErrors)                                                               \
    X(TestValueMethods)                                                                \
    X(TestCoderInterleaved)                                                            \
    X(TestCoderStackPointer)                                                           \
    X(TestCoderMaxDepth)                                                               \
    X(TestEncoderReset)                                                                \
    X(TestDecoderReset)                                                                \
    X(TestBufferDecoder)                                                               \
    X(TestResumableDecoder)                                                            \
    X(TestBlockingDecoder)                                                             \
    X(TestPeekableDecoder)                                                             \
    X(TestTokenAccessors)                                                              \
    X(TestTokenClone)                                                                  \
    X(TestPointer)                                                                     \
    X(TestStateMachine)                                                                \
    X(TestObjectNamespace)                                                             \
    X(TestNoMemory)

TESTING_MAIN(TESTS)

/* encoding/json/v2: the fields of a Go struct as JSON sees them, and the case
 * folding that matches names loosely.
 *
 * Derived from Go's src/encoding/json/v2/fields.go and fold.go.
 * Go source: go1.27.1.
 *
 * Copyright 2021 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/encoding/json/v2.h"

#include "burrow/lock.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/strconv.h"
#include "burrow/strings.h"
#include "burrow/unicode.h"
#include "burrow/utf8.h"

#include "jsonv2_internal.h"

#include <stdarg.h>
#include <string.h>

/* ------------------------------------------------------------------ errorf */

static void jv_put_rune_quoted(JsonBuf *b, Rune r) {
    Byte enc[8];
    Slice s = {enc, 0, (Int)sizeof(enc), TYPE_BYTE};
    s.len = (Int)sizeof(enc);
    Int n = utf8_encode_rune(s, r);
    Byte out[16];
    Str q = burrow__jsonwire_quote_rune(enc, n, out);
    jsonbuf_str(b, q);
}

static void jv_put_int64(JsonBuf *b, int64_t v) {
    Byte tmp[24];
    Int n = 0;
    uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    do {
        tmp[n++] = (Byte)('0' + u % 10);
        u /= 10;
    } while (u > 0);
    if (v < 0)
        jsonbuf_byte(b, '-');
    while (n > 0)
        jsonbuf_byte(b, tmp[--n]);
}

/* A small fmt.Errorf. %s takes a Str, %q a Str to quote the way Go does, %r a
 * Rune to quote, %d an int64_t, %T a const Type * and %e an Error's text. */
Error burrow__jsonv2_errorf_in(Alloc *a, const char *fmt, ...) {
    JsonBuf b = {NULL, 0, 0, heap_allocator(), true, false};
    va_list ap;
    va_start(ap, fmt);
    for (const char *c = fmt; *c != '\0'; c++) {
        if (*c != '%' || c[1] == '\0') {
            jsonbuf_byte(&b, (Byte)*c);
            continue;
        }
        c++;
        switch (*c) {
        case 's':
            jsonbuf_str(&b, va_arg(ap, Str));
            break;
        case 'q':
            burrow__jsontext_put_go_quote(&b, va_arg(ap, Str));
            break;
        case 'r':
            jv_put_rune_quoted(&b, va_arg(ap, Rune));
            break;
        case 'd':
            jv_put_int64(&b, va_arg(ap, int64_t));
            break;
        case 'T':
            burrow__jsonv2_put_type(&b, va_arg(ap, const Type *));
            break;
        case 'e':
            jsonbuf_str(&b, error_text(va_arg(ap, Error)));
            break;
        default:
            jsonbuf_byte(&b, (Byte)*c);
            break;
        }
    }
    va_end(ap);
    Error err =
        b.failed ? burrow_err_out_of_memory : errors_new(a, str_from_bytes(b.p, b.len));
    burrow__jsonbuf_free(&b);
    return err;
}

/* -------------------------------------------------------------------- fold */

static Rune jv_fold_rune(Rune r) {
    for (;;) {
        Rune r2 = unicode_simple_fold(r);
        if (r2 <= r)
            return r2;
        r = r2;
    }
}

/* appendFoldedName: upper case ASCII, no '_' or '-', and every other rune the
 * smallest of its fold set. */
void burrow__jsonv2_append_folded(JsonBuf *out, Str in) {
    for (Int i = 0; i < in.len;) {
        Byte c = in.p[i];
        if (c < UTF8_RUNE_SELF) {
            if (c != '_' && c != '-') {
                if ('a' <= c && c <= 'z')
                    c = (Byte)(c - ('a' - 'A'));
                jsonbuf_byte(out, c);
            }
            i++;
            continue;
        }
        Int n = 0;
        Rune r = utf8_decode_rune_in_string(str_from_bytes(in.p + i, in.len - i), &n);
        Byte enc[8];
        Slice s = {enc, (Int)sizeof(enc), (Int)sizeof(enc), TYPE_BYTE};
        Int w = utf8_encode_rune(s, jv_fold_rune(r));
        jsonbuf_put(out, enc, w);
        i += n;
    }
}

/* ------------------------------------------------------------------ fields */

static bool jv_str_has_prefix(Str s, const char *p) {
    size_t n = strlen(p);
    return (size_t)s.len >= n && memcmp(s.p, p, n) == 0;
}

static Str jv_sub(Str s, Int from, Int to) {
    return str_from_bytes(s.p + from, to - from);
}

static bool jv_is_letter_or_digit(Rune r) {
    return r == '_' || unicode_is_letter(r) || unicode_is_number(r);
}

/* How many bytes at the front of s are letters, digits and underscores. */
static Int jv_letters_len(Str s) {
    Int i = 0;
    while (i < s.len) {
        Int n = 0;
        Rune r = utf8_decode_rune_in_string(jv_sub(s, i, s.len), &n);
        if (!jv_is_letter_or_digit(r))
            break;
        i += n;
    }
    return i;
}

static bool jv_all_letters(Str s) {
    return jv_letters_len(s) == s.len;
}

/* Persistent errors: what the cache keeps lives as long as the process. */
/* Where everything a struct's fields need comes from while they are worked
 * out: an arena of the struct's own, which the result keeps and nothing ever
 * frees, since the result is cached for good. The scratch along the way goes
 * in it too, which costs a little memory once per type and saves freeing
 * every path out of the build by hand. Only set under jv_cache_lock. */
static Alloc *jv_build_alloc;
#define JV_HEAP jv_build_alloc

/* consumeTagOption. *out and *n are the option and how much of in it took;
 * the error, if any, is built in the heap. */
static Error jv_consume_tag_option(Str in, bool allow_quoted, Str *out, Int *n) {
    Int i = 0;
    while (i < in.len && in.p[i] != ',')
        i++;
    Int rn = 0;
    Rune r = utf8_decode_rune_in_string(in, &rn);
    if (in.len > 0 && (r == '_' || unicode_is_letter(r))) {
        Int k = jv_letters_len(in);
        *out = jv_sub(in, 0, k);
        *n = k;
        return BURROW_NO_ERROR;
    }
    *out = jv_sub(in, 0, i);
    *n = i;
    if (in.len > 0 && r == '\'') {
        if (!allow_quoted)
            return burrow__jsonv2_errorf_in(
                JV_HEAP,
                "invalid character %r at start of option (expecting Unicode letter)",
                r);
        bool in_escape = false;
        JsonBuf b = {NULL, 0, 0, JV_HEAP, true, false};
        jsonbuf_byte(&b, '"');
        Int k = 1;
        while (in.len > k) {
            Int w = 0;
            Rune c = utf8_decode_rune_in_string(jv_sub(in, k, in.len), &w);
            if (in_escape) {
                if (c == '\'')
                    b.len--;
                in_escape = false;
            } else if (c == '\\') {
                in_escape = true;
            } else if (c == '"') {
                jsonbuf_byte(&b, '\\');
            } else if (c == '\'') {
                jsonbuf_byte(&b, '"');
                k++;
                if (b.failed) {
                    burrow__jsonbuf_free(&b);
                    return burrow_err_out_of_memory;
                }
                Error uerr = BURROW_NO_ERROR;
                Str u = strconv_unquote(JV_HEAP, str_from_bytes(b.p, b.len), &uerr);
                burrow__jsonbuf_free(&b);
                if (BURROW_FAILED(uerr))
                    return burrow__jsonv2_errorf_in(
                        JV_HEAP, "invalid single-quoted string: %s", jv_sub(in, 0, k));
                *out = u;
                *n = k;
                return BURROW_NO_ERROR;
            }
            jsonbuf_put(&b, in.p + k, w);
            k += w;
        }
        burrow__jsonbuf_free(&b);
        if (k > 10)
            k = 10;
        return burrow__jsonv2_errorf_in(
            JV_HEAP, "single-quoted string not terminated: %s...", jv_sub(in, 0, k));
    }
    if (in.len == 0)
        return io_err_unexpected_eof;
    if (!allow_quoted)
        return burrow__jsonv2_errorf_in(
            JV_HEAP,
            "invalid character %r at start of option (expecting Unicode letter)", r);
    return burrow__jsonv2_errorf_in(
        JV_HEAP,
        "invalid character %r at start of option (expecting "
        "Unicode letter or single quote)",
        r);
}

/* string([]rune(s)): every invalid byte becomes U+FFFD. */
static Str jv_valid_utf8(Str s) {
    JsonBuf b = {NULL, 0, 0, JV_HEAP, true, false};
    for (Int i = 0; i < s.len;) {
        Int n = 0;
        Rune r = utf8_decode_rune_in_string(jv_sub(s, i, s.len), &n);
        Byte enc[8];
        Slice e = {enc, (Int)sizeof(enc), (Int)sizeof(enc), TYPE_BYTE};
        jsonbuf_put(&b, enc, utf8_encode_rune(e, r));
        i += n;
    }
    return str_from_bytes(b.p, b.len);
}

static bool jv_first_err(Error *dst, Error err) {
    if (BURROW_FAILED(err) && BURROW_OK(*dst))
        *dst = err;
    return BURROW_FAILED(err);
}

/* strings.ReplaceAll(strings.ToLower(opt), "_", "") is one of the known
 * options. */
static Str jv_norm_known_option(Str opt) {
    static const char *const known[] = {"case",      "embed",  "omitzero",
                                        "omitempty", "string", "format"};
    JsonBuf b = {NULL, 0, 0, JV_HEAP, true, false};
    for (Int i = 0; i < opt.len;) {
        Int n = 0;
        Rune r = utf8_decode_rune_in_string(jv_sub(opt, i, opt.len), &n);
        i += n;
        if (r == '_')
            continue;
        Byte enc[8];
        Slice e = {enc, (Int)sizeof(enc), (Int)sizeof(enc), TYPE_BYTE};
        jsonbuf_put(&b, enc, utf8_encode_rune(e, unicode_to_lower(r)));
    }
    Str got = str_from_bytes(b.p, b.len);
    for (size_t k = 0; k < sizeof(known) / sizeof(known[0]); k++) {
        if ((size_t)got.len == strlen(known[k]) &&
            memcmp(got.p, known[k], (size_t)got.len) == 0) {
            burrow__jsonbuf_free(&b);
            return str_from_bytes(known[k], (Int)strlen(known[k]));
        }
    }
    burrow__jsonbuf_free(&b);
    return BURROW_STR_EMPTY;
}

typedef struct JvOptSeen {
    Str *v;
    Int len;
    Int cap;
} JvOptSeen;

static bool jv_opt_seen(JvOptSeen *s, Str opt) {
    for (Int i = 0; i < s->len; i++)
        if (str_eq(s->v[i], opt))
            return true;
    if (s->len == s->cap) {
        Int nc = s->cap == 0 ? 8 : s->cap * 2;
        Str *nv = (Str *)mem_realloc(JV_HEAP, s->v, (size_t)s->cap * sizeof(Str),
                                     (size_t)nc * sizeof(Str), _Alignof(Str));
        if (nv == NULL)
            return false;
        s->v = nv;
        s->cap = nc;
    }
    s->v[s->len++] = opt;
    return false;
}

/* parseFieldOptions. */
static bool jv_parse_field_options(const Field *sf, bool anonymous, JvField *out,
                                   Error *errp) {
    Error err = BURROW_NO_ERROR;
    Str tag = BURROW_STR_EMPTY;
    bool has_tag = tag_lookup(JV_HEAP, sf->tag, JV_LIT("json"), &tag);
    memset(out, 0, sizeof(*out));
    if (str_eq(tag, JV_LIT("-"))) {
        *errp = err;
        return true;
    }
    if (!field_is_exported(sf) && !anonymous) {
        if (has_tag)
            err = burrow__jsonv2_errorf_in(
                JV_HEAP,
                "unexported Go struct field %s cannot have non-ignored `json:%q` tag",
                sf->name, tag);
        *errp = err;
        return true;
    }
    out->name = sf->name;
    if (tag.len > 0 && tag.p[0] != ',') {
        Int n = 0;
        while (n < tag.len && memchr(",\\'\"`", tag.p[n], 5) == NULL)
            n++;
        Str name = jv_sub(tag, 0, n);
        Error err2 = BURROW_NO_ERROR;
        if (!(n < tag.len && tag.p[n] == ',') && name.len != tag.len) {
            err2 = jv_consume_tag_option(tag, false, &name, &n);
            if (BURROW_FAILED(err2))
                jv_first_err(&err,
                             burrow__jsonv2_errorf_in(
                                 JV_HEAP,
                                 "Go struct field %s has malformed `json` tag: %e",
                                 sf->name, err2));
        }
        if (!utf8_valid_string(name)) {
            jv_first_err(
                &err,
                burrow__jsonv2_errorf_in(
                    JV_HEAP,
                    "Go struct field %s has JSON object name %q with invalid UTF-8",
                    sf->name, name));
            name = jv_valid_utf8(name);
        }
        if (BURROW_OK(err2)) {
            out->has_name = true;
            out->name = name;
        }
        tag = jv_sub(tag, n, tag.len);
    }
    {
        JsonBuf q = {NULL, 0, 0, JV_HEAP, true, false};
        JsontextOptions none;
        memset(&none, 0, sizeof(none));
        (void)burrow__jsonwire_append_quote(&q, out->name.p, out->name.len, &none);
        out->quoted_name = str_from_bytes(q.p, q.len);
        out->name_need_escape =
            burrow__jsonwire_need_escape(out->name.p, out->name.len);
    }
    bool was_format = false;
    JvOptSeen seen = {NULL, 0, 0};
    while (tag.len > 0) {
        if (tag.p[0] != ',') {
            jv_first_err(&err,
                         burrow__jsonv2_errorf_in(
                             JV_HEAP,
                             "Go struct field %s has malformed `json` tag: invalid "
                             "character %r before next option (expecting ',')",
                             sf->name, (Rune)tag.p[0]));
        } else {
            tag = jv_sub(tag, 1, tag.len);
            if (tag.len == 0) {
                jv_first_err(&err,
                             burrow__jsonv2_errorf_in(
                                 JV_HEAP,
                                 "Go struct field %s has malformed `json` tag: invalid "
                                 "trailing ',' character",
                                 sf->name));
                break;
            }
        }
        Str opt;
        Int n = 0;
        Error err2 = jv_consume_tag_option(tag, false, &opt, &n);
        if (BURROW_FAILED(err2))
            jv_first_err(&err,
                         burrow__jsonv2_errorf_in(
                             JV_HEAP, "Go struct field %s has malformed `json` tag: %e",
                             sf->name, err2));
        Str raw = jv_sub(tag, 0, n);
        tag = jv_sub(tag, n, tag.len);
        if (was_format) {
            jv_first_err(&err,
                         burrow__jsonv2_errorf_in(
                             JV_HEAP,
                             "Go struct field %s has `format` tag option that was not "
                             "specified last",
                             sf->name));
        } else if (raw.len > 0 && raw.p[0] == '\'' && jv_all_letters(opt)) {
            jv_first_err(
                &err, burrow__jsonv2_errorf_in(
                          JV_HEAP,
                          "Go struct field %s has unnecessarily quoted appearance of "
                          "`%s` tag option; specify `%s` instead",
                          sf->name, raw, opt));
        }
        if (str_eq(opt, JV_LIT("case"))) {
            if (!jv_str_has_prefix(tag, ":")) {
                jv_first_err(
                    &err, burrow__jsonv2_errorf_in(
                              JV_HEAP,
                              "Go struct field %s is missing value for `case` tag "
                              "option; specify `case:ignore` or `case:strict` instead",
                              sf->name));
                goto next;
            }
            tag = jv_sub(tag, 1, tag.len);
            Str copt;
            Int cn = 0;
            Error err3 = jv_consume_tag_option(tag, false, &copt, &cn);
            if (BURROW_FAILED(err3)) {
                jv_first_err(
                    &err, burrow__jsonv2_errorf_in(
                              JV_HEAP,
                              "Go struct field %s has malformed value for `case` tag "
                              "option: %e",
                              sf->name, err3));
                goto next;
            }
            Str craw = jv_sub(tag, 0, cn);
            tag = jv_sub(tag, cn, tag.len);
            if (craw.len > 0 && craw.p[0] == '\'')
                jv_first_err(
                    &err, burrow__jsonv2_errorf_in(
                              JV_HEAP,
                              "Go struct field %s has unnecessarily quoted appearance "
                              "of `case:%s` tag option; specify `case:%s` instead",
                              sf->name, craw, copt));
            if (str_eq(copt, JV_LIT("ignore")))
                out->casing |= JV_CASE_IGNORE;
            else if (str_eq(copt, JV_LIT("strict")))
                out->casing |= JV_CASE_STRICT;
            else
                jv_first_err(&err,
                             burrow__jsonv2_errorf_in(
                                 JV_HEAP,
                                 "Go struct field %s has unknown `case:%s` tag value",
                                 sf->name, craw));
        } else if (str_eq(opt, JV_LIT("embed"))) {
            out->embed = true;
        } else if (str_eq(opt, JV_LIT("omitzero"))) {
            out->omitzero = true;
        } else if (str_eq(opt, JV_LIT("omitempty"))) {
            out->omitempty = true;
        } else if (str_eq(opt, JV_LIT("string"))) {
            out->string_ = true;
        } else if (str_eq(opt, JV_LIT("format"))) {
            if (!jv_str_has_prefix(tag, ":")) {
                jv_first_err(&err,
                             burrow__jsonv2_errorf_in(
                                 JV_HEAP,
                                 "Go struct field %s is missing value for `format` tag "
                                 "option",
                                 sf->name));
                goto next;
            }
            tag = jv_sub(tag, 1, tag.len);
            Str fopt;
            Int fn = 0;
            Error err3 = jv_consume_tag_option(tag, true, &fopt, &fn);
            if (BURROW_FAILED(err3)) {
                jv_first_err(
                    &err, burrow__jsonv2_errorf_in(
                              JV_HEAP,
                              "Go struct field %s has malformed value for `format` tag "
                              "option: %e",
                              sf->name, err3));
                goto next;
            } else if (fopt.len == 0) {
                jv_first_err(
                    &err, burrow__jsonv2_errorf_in(
                              JV_HEAP,
                              "Go struct field %s cannot have empty value for `format` "
                              "tag option",
                              sf->name));
                goto next;
            }
            tag = jv_sub(tag, fn, tag.len);
            out->format = fopt;
            was_format = true;
        } else {
            Str norm = jv_norm_known_option(opt);
            if (norm.len > 0)
                jv_first_err(
                    &err, burrow__jsonv2_errorf_in(
                              JV_HEAP,
                              "Go struct field %s has invalid appearance of `%s` tag "
                              "option; specify `%s` instead",
                              sf->name, opt, norm));
        }
    next:;
        bool dup = jv_opt_seen(&seen, opt);
        if (out->casing == (JV_CASE_IGNORE | JV_CASE_STRICT))
            jv_first_err(&err,
                         burrow__jsonv2_errorf_in(
                             JV_HEAP,
                             "Go struct field %s cannot have both `case:ignore` and "
                             "`case:strict` tag options",
                             sf->name));
        else if (dup)
            jv_first_err(&err,
                         burrow__jsonv2_errorf_in(
                             JV_HEAP,
                             "Go struct field %s has duplicate appearance of `%s` tag "
                             "option",
                             sf->name, raw));
    }
    mem_free(JV_HEAP, seen.v, (size_t)seen.cap * sizeof(Str), _Alignof(Str));
    *errp = err;
    return false;
}

/* Go's Anonymous. field_is_embedded compares the field's name with its type's,
 * which an embedded *T does not match, since the pointer type has no name. */
bool burrow__jsonv2_field_anonymous(const Field *f) {
    if (field_is_embedded(f))
        return true;
    const Type *t = f->type;
    return t != NULL && t->kind == KIND_POINTER && t->name.len == 0 &&
           t->elem != NULL && str_eq(t->elem->name, f->name);
}

/* indirectType. */
static const Type *jv_indirect_type(const Type *t) {
    if (t->kind == KIND_POINTER && t->name.len == 0)
        return t->elem;
    return t;
}

/* Whether t has any of the methods json v2 would call. */
bool burrow__jsonv2_implements_any(const Type *t) {
    return burrow__jsonv2_implements(t, JV_MASK_ARSHALERS);
}

typedef struct JvQueue {
    const Type *typ;
    const Int *index;
    Int nindex;
    bool visit_children;
} JvQueue;

typedef struct JvBuild {
    JvQueue *queue;
    Int qlen, qcap;
    const Type **seen;
    Int nseen, seen_cap;
    JvField *all;
    Int nall, all_cap;
    JvField *fallbacks;
    Int nfall, fall_cap;
    bool oom;
} JvBuild;

/* Makes room for one more in the array at *arr holding n of size bytes each,
 * doubling *cap when it is full. Out of memory marks the build and leaves the
 * array as it was. */
static void jv_grow(JvBuild *bld, void *arr, Int n, Int *cap, size_t size,
                    size_t align) {
    if (n != *cap)
        return;
    Int nc = *cap == 0 ? 8 : *cap * 2;
    void *old;
    memcpy(&old, arr, sizeof(old));
    void *nv = mem_realloc(JV_HEAP, old, (size_t)*cap * size, (size_t)nc * size, align);
    if (nv == NULL) {
        bld->oom = true;
        return;
    }
    memcpy(arr, &nv, sizeof(nv));
    *cap = nc;
}

#define JV_GROW(bld, arr, n, cap, T)                                                   \
    jv_grow((bld), (void *)&(arr), (n), &(cap), sizeof(T), _Alignof(T))

static bool jv_seen_has(JvBuild *b, const Type *t) {
    for (Int i = 0; i < b->nseen; i++)
        if (b->seen[i] == t)
            return true;
    return false;
}

static void jv_seen_add(JvBuild *b, const Type *t) {
    if (jv_seen_has(b, t))
        return;
    JV_GROW(b, b->seen, b->nseen, b->seen_cap, const Type *);
    if (!b->oom)
        b->seen[b->nseen++] = t;
}

static void jv_queue_add(JvBuild *b, const Type *t, const Int *index, Int n,
                         bool visit) {
    JV_GROW(b, b->queue, b->qlen, b->qcap, JvQueue);
    if (b->oom)
        return;
    JvQueue *q = &b->queue[b->qlen++];
    q->typ = t;
    q->index = index;
    q->nindex = n;
    q->visit_children = visit;
}

static void jv_or_err(Error *serr, const Type **serr_t, const Type *t, Error err) {
    if (BURROW_OK(*serr) && BURROW_FAILED(err)) {
        *serr = err;
        *serr_t = t;
    }
}

static int jv_index_cmp(const JvField *x, const JvField *y) {
    Int n = x->nindex < y->nindex ? x->nindex : y->nindex;
    for (Int i = 0; i < n; i++) {
        if (x->index[i] != y->index[i])
            return x->index[i] < y->index[i] ? -1 : 1;
    }
    return x->nindex < y->nindex ? -1 : x->nindex > y->nindex ? 1 : 0;
}

static int jv_name_cmp(Str a, Str b) {
    Int n = a.len < b.len ? a.len : b.len;
    int c = n > 0 ? memcmp(a.p, b.p, (size_t)n) : 0;
    if (c != 0)
        return c < 0 ? -1 : 1;
    return a.len < b.len ? -1 : a.len > b.len ? 1 : 0;
}

static int jv_dominance_cmp(const JvField *x, const JvField *y) {
    int c = jv_name_cmp(x->name, y->name);
    if (c != 0)
        return c;
    if (x->nindex != y->nindex)
        return x->nindex < y->nindex ? -1 : 1;
    bool xn = !x->has_name, yn = !y->has_name;
    if (!xn && yn)
        return -1;
    if (xn && !yn)
        return 1;
    return 0;
}

static int jv_id_cmp(const JvField *x, const JvField *y) {
    return x->id < y->id ? -1 : x->id > y->id ? 1 : 0;
}

/* A stable merge sort, since the lists are short and stability matters. */
static void jv_sort_fields(JvField *v, Int n,
                           int (*cmp)(const JvField *, const JvField *), JvField *tmp) {
    if (n < 2)
        return;
    Int mid = n / 2;
    jv_sort_fields(v, mid, cmp, tmp);
    jv_sort_fields(v + mid, n - mid, cmp, tmp);
    Int i = 0, j = mid, k = 0;
    while (i < mid && j < n) {
        if (cmp(&v[j], &v[i]) < 0)
            tmp[k++] = v[j++];
        else
            tmp[k++] = v[i++];
    }
    while (i < mid)
        tmp[k++] = v[i++];
    while (j < n)
        tmp[k++] = v[j++];
    memcpy(v, tmp, (size_t)n * sizeof(JvField));
}

static uint64_t jv_hash(Str s) {
    uint64_t h = 1469598103934665603ULL;
    for (Int i = 0; i < s.len; i++) {
        h ^= s.p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static const JvFields *jv_fields_build(const Type *root) {
    JvFields *fs = BURROW_NEW(JV_HEAP, JvFields);
    if (fs == NULL)
        return NULL;
    JvBuild b;
    memset(&b, 0, sizeof(b));
    Error serr = BURROW_NO_ERROR;
    const Type *serr_t = NULL;
    Error fmt_err = BURROW_NO_ERROR;
    const Type *fmt_t = NULL;

    jv_queue_add(&b, root, NULL, 0, true);
    jv_seen_add(&b, root);
    for (Int qi = 0; qi < b.qlen && !b.oom; qi++) {
        JvQueue qe = b.queue[qi];
        const Type *t = qe.typ;
        Int fallback_index = -1;
        bool any_tag = false, any_field = false;
        /* namesIndex, as parallel arrays of the names in this struct. */
        Str *names = NULL;
        Int *name_idx = NULL;
        Int nnames = 0;
        if (t->nfield > 0) {
            names = (Str *)mem_alloc(JV_HEAP, (size_t)t->nfield * sizeof(Str),
                                     _Alignof(Str));
            name_idx = (Int *)mem_alloc(JV_HEAP, (size_t)t->nfield * sizeof(Int),
                                        _Alignof(Int));
            if (names == NULL || name_idx == NULL) {
                b.oom = true;
                break;
            }
        }
        for (Int i = 0; i < (Int)t->nfield && !b.oom; i++) {
            const Field *sf = &t->fields[i];
            Str tagv;
            bool has_tag = tag_lookup(JV_HEAP, sf->tag, JV_LIT("json"), &tagv);
            any_tag = any_tag || has_tag;
            bool anonymous = burrow__jsonv2_field_anonymous(sf);
            JvField f;
            Error perr = BURROW_NO_ERROR;
            bool ignored = jv_parse_field_options(sf, anonymous, &f, &perr);
            if (BURROW_FAILED(perr))
                jv_or_err(&serr, &serr_t, t, perr);
            if (ignored)
                continue;
            any_field = true;
            Int *index = (Int *)mem_alloc(
                JV_HEAP, (size_t)(qe.nindex + 1) * sizeof(Int), _Alignof(Int));
            if (index == NULL) {
                b.oom = true;
                break;
            }
            if (qe.nindex > 0)
                memcpy(index, qe.index, (size_t)qe.nindex * sizeof(Int));
            index[qe.nindex] = i;
            f.index = index;
            f.nindex = qe.nindex + 1;
            f.typ = sf->type;
            if (anonymous && !f.has_name) {
                if (jv_indirect_type(f.typ)->kind != KIND_STRUCT)
                    jv_or_err(
                        &serr, &serr_t, t,
                        burrow__jsonv2_errorf_in(
                            JV_HEAP,
                            "embedded Go struct field %s of non-struct type must be "
                            "explicitly given a JSON name",
                            sf->name));
                else
                    f.embed = true;
            }
            bool as_field = !f.embed;
            if (f.embed) {
                bool only_embed = !f.has_name && !f.name_need_escape && f.casing == 0 &&
                                  !f.omitzero && !f.omitempty && !f.string_ &&
                                  f.format.len == 0;
                bool handled = false;
                if (!only_embed) {
                    jv_or_err(
                        &serr, &serr_t, t,
                        burrow__jsonv2_errorf_in(
                            JV_HEAP,
                            "Go struct field %s cannot have any options other than "
                            "`embed` specified",
                            sf->name));
                    if (f.has_name) {
                        as_field = true;
                        handled = true;
                    } else {
                        f.casing = 0;
                        f.name_need_escape = false;
                        f.omitzero = f.omitempty = f.string_ = false;
                        f.format = BURROW_STR_EMPTY;
                    }
                }
                if (!handled) {
                    const Type *tf = jv_indirect_type(f.typ);
                    if (burrow__jsonv2_implements_any(tf) &&
                        tf != &burrow_type_JsontextValue)
                        jv_or_err(&serr, &serr_t, t,
                                  burrow__jsonv2_errorf_in(
                                      JV_HEAP,
                                      "embedded Go struct field %s of type %T must not "
                                      "implement marshal or unmarshal methods",
                                      sf->name, tf));
                    if (tf->kind == KIND_STRUCT) {
                        if (qe.visit_children)
                            jv_queue_add(&b, tf, f.index, f.nindex,
                                         !jv_seen_has(&b, tf));
                        jv_seen_add(&b, tf);
                        continue;
                    }
                    if (!field_is_exported(sf)) {
                        jv_or_err(&serr, &serr_t, t,
                                  burrow__jsonv2_errorf_in(
                                      JV_HEAP,
                                      "embedded Go struct field %s is not exported",
                                      sf->name));
                        continue;
                    }
                    if (tf == &burrow_type_JsontextValue) {
                        /* Handled by the fallback code in jsonv2_arshal.c. */
                    } else if (tf->kind == KIND_MAP && tf->key != NULL &&
                               tf->key->kind == KIND_STRING) {
                        if (burrow__jsonv2_implements_any(tf->key)) {
                            jv_or_err(
                                &serr, &serr_t, t,
                                burrow__jsonv2_errorf_in(
                                    JV_HEAP,
                                    "embedded map field %s of type %T must have a "
                                    "string key that does not implement marshal or "
                                    "unmarshal methods",
                                    sf->name, tf));
                            as_field = true;
                        }
                    } else {
                        jv_or_err(
                            &serr, &serr_t, t,
                            burrow__jsonv2_errorf_in(
                                JV_HEAP,
                                "embedded Go struct field %s of type %T must be a Go "
                                "struct, Go map of string key, or jsontext.Value",
                                sf->name, tf));
                        as_field = true;
                    }
                    if (!as_field) {
                        if (fallback_index >= 0)
                            jv_or_err(&serr, &serr_t, t,
                                      burrow__jsonv2_errorf_in(
                                          JV_HEAP,
                                          "embedded Go struct fields %s and %s cannot "
                                          "both be "
                                          "a Go map or jsontext.Value",
                                          t->fields[fallback_index].name, sf->name));
                        fallback_index = i;
                        JV_GROW(&b, b.fallbacks, b.nfall, b.fall_cap, JvField);
                        if (!b.oom)
                            b.fallbacks[b.nfall++] = f;
                        continue;
                    }
                }
            }
            if (as_field) {
                if (!field_is_exported(sf)) {
                    const Type *tf = jv_indirect_type(f.typ);
                    if (!(anonymous && tf->kind == KIND_STRUCT)) {
                        jv_or_err(&serr, &serr_t, t,
                                  burrow__jsonv2_errorf_in(
                                      JV_HEAP, "Go struct field %s is not exported",
                                      sf->name));
                        continue;
                    }
                    if (burrow__jsonv2_implements_any(tf) ||
                        (f.omitzero &&
                         burrow__jsonv2_implements(tf, 1U << JV_M_IS_ZERO))) {
                        jv_or_err(
                            &serr, &serr_t, t,
                            burrow__jsonv2_errorf_in(
                                JV_HEAP,
                                "Go struct field %s is not exported for method calls",
                                sf->name));
                        continue;
                    }
                }
                switch ((int)f.typ->kind) {
                case KIND_STRING:
                case KIND_MAP:
                case KIND_ARRAY:
                case KIND_SLICE:
                case KIND_POINTER:
                case KIND_INTERFACE:
                    f.can_empty = true;
                    break;
                default:
                    break;
                }
                for (Int j = 0; j < nnames; j++) {
                    if (str_eq(names[j], f.name)) {
                        jv_or_err(
                            &serr, &serr_t, t,
                            burrow__jsonv2_errorf_in(
                                JV_HEAP,
                                "Go struct fields %s and %s conflict over JSON object "
                                "name %q",
                                t->fields[name_idx[j]].name, sf->name, f.name));
                        break;
                    }
                }
                names[nnames] = f.name;
                name_idx[nnames] = i;
                nnames++;
                f.id = b.nall;
                JV_GROW(&b, b.all, b.nall, b.all_cap, JvField);
                if (b.oom)
                    break;
                b.all[b.nall++] = f;
                if (f.format.len > 0 && BURROW_OK(fmt_err)) {
                    fmt_err = burrow__jsonv2_errorf_in(
                        JV_HEAP,
                        "Go struct field %s has unsupported `format` tag option",
                        sf->name);
                    fmt_t = t;
                }
            }
        }
        mem_free(JV_HEAP, names, (size_t)t->nfield * sizeof(Str), _Alignof(Str));
        mem_free(JV_HEAP, name_idx, (size_t)t->nfield * sizeof(Int), _Alignof(Int));
        if (t->nfield != 0 && !any_tag && !any_field)
            jv_or_err(&serr, &serr_t, t, burrow__jsonv2_err_no_exported_fields);
    }
    if (b.oom)
        return NULL;

    /* Keep only the dominant field of each name. */
    JvField *tmp = NULL;
    if (b.nall > 0) {
        tmp = (JvField *)mem_alloc(JV_HEAP, (size_t)b.nall * sizeof(JvField),
                                   _Alignof(JvField));
        if (tmp == NULL)
            return NULL;
    }
    jv_sort_fields(b.all, b.nall, jv_dominance_cmp, tmp);
    Int nflat = 0;
    for (Int i = 0; i < b.nall;) {
        Int n = 1;
        while (i + n < b.nall && str_eq(b.all[i + n - 1].name, b.all[i + n].name))
            n++;
        if (n == 1 || b.all[i].nindex != b.all[i + 1].nindex ||
            b.all[i].has_name != b.all[i + 1].has_name)
            b.all[nflat++] = b.all[i];
        i += n;
    }
    jv_sort_fields(b.all, nflat, jv_id_cmp, tmp);
    for (Int i = 0; i < nflat; i++)
        b.all[i].id = i;
    jv_sort_fields(b.all, nflat, jv_index_cmp, tmp);
    mem_free(JV_HEAP, tmp, (size_t)b.nall * sizeof(JvField), _Alignof(JvField));

    fs->flat = b.all;
    fs->nflat = nflat;
    /* byActualName, open addressed on the name. */
    Int cap = 8;
    while (cap < nflat * 2)
        cap *= 2;
    fs->by_name = (Int *)mem_alloc(JV_HEAP, (size_t)cap * sizeof(Int), _Alignof(Int));
    fs->by_name_cap = cap;
    fs->by_folded = (Int *)mem_alloc(
        JV_HEAP, (size_t)(nflat > 0 ? nflat : 1) * sizeof(Int), _Alignof(Int));
    if (fs->by_name == NULL || fs->by_folded == NULL)
        return NULL;
    for (Int i = 0; i < nflat; i++) {
        JvField *f = &fs->flat[i];
        uint64_t h = jv_hash(f->name) & (uint64_t)(cap - 1);
        while (fs->by_name[h] != 0)
            h = (h + 1) & (uint64_t)(cap - 1);
        fs->by_name[h] = i + 1;
        JsonBuf fb = {NULL, 0, 0, JV_HEAP, true, false};
        burrow__jsonv2_append_folded(&fb, f->name);
        if (fb.failed)
            return NULL;
        f->folded = str_from_bytes(fb.p, fb.len);
        /* Insertion into by_folded, ordered by folded name and then id. */
        Int j = i;
        while (j > 0) {
            const JvField *g = &fs->flat[fs->by_folded[j - 1]];
            int c = jv_name_cmp(g->folded, f->folded);
            if (c < 0 || (c == 0 && g->id < f->id))
                break;
            fs->by_folded[j] = fs->by_folded[j - 1];
            j--;
        }
        fs->by_folded[j] = i;
    }
    if (b.nfall == 1 ||
        (b.nfall > 1 && b.fallbacks[0].nindex != b.fallbacks[1].nindex)) {
        fs->has_fallback = true;
        fs->fallback = b.fallbacks[0];
    }
    if (BURROW_FAILED(serr)) {
        fs->has_err_init = true;
        fs->err_init = serr;
        fs->err_init_type = serr_t;
    }
    if (BURROW_FAILED(fmt_err)) {
        fs->has_err_fmt = true;
        fs->err_fmt = fmt_err;
        fs->err_fmt_type = fmt_t;
    }
    return fs;
}

/* The cache: every struct type's fields, built once and kept. */
typedef struct JvCacheEntry {
    const Type *t;
    const JvFields *fs;
} JvCacheEntry;

static burrow__Lock jv_cache_lock;
static JvCacheEntry *jv_cache;
static Int jv_cache_cap;
static Int jv_cache_len;

static uint64_t jv_ptr_hash(const void *p) {
    uint64_t h = (uint64_t)(uintptr_t)p;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    return h;
}

static const JvFields *jv_cache_find(const Type *t) {
    if (jv_cache_cap == 0)
        return NULL;
    uint64_t m = (uint64_t)(jv_cache_cap - 1);
    for (uint64_t h = jv_ptr_hash(t) & m;; h = (h + 1) & m) {
        if (jv_cache[h].t == t)
            return jv_cache[h].fs;
        if (jv_cache[h].t == NULL)
            return NULL;
    }
}

static bool jv_cache_put(const Type *t, const JvFields *fs) {
    if ((jv_cache_len + 1) * 2 > jv_cache_cap) {
        Int nc = jv_cache_cap == 0 ? 64 : jv_cache_cap * 2;
        JvCacheEntry *nv = (JvCacheEntry *)mem_alloc(heap_allocator(),
                                                     (size_t)nc * sizeof(JvCacheEntry),
                                                     _Alignof(JvCacheEntry));
        if (nv == NULL)
            return false;
        for (Int i = 0; i < jv_cache_cap; i++) {
            if (jv_cache[i].t == NULL)
                continue;
            uint64_t h = jv_ptr_hash(jv_cache[i].t) & (uint64_t)(nc - 1);
            while (nv[h].t != NULL)
                h = (h + 1) & (uint64_t)(nc - 1);
            nv[h] = jv_cache[i];
        }
        mem_free(heap_allocator(), jv_cache,
                 (size_t)jv_cache_cap * sizeof(JvCacheEntry), _Alignof(JvCacheEntry));
        jv_cache = nv;
        jv_cache_cap = nc;
    }
    uint64_t m = (uint64_t)(jv_cache_cap - 1);
    uint64_t h = jv_ptr_hash(t) & m;
    while (jv_cache[h].t != NULL)
        h = (h + 1) & m;
    jv_cache[h].t = t;
    jv_cache[h].fs = fs;
    jv_cache_len++;
    return true;
}

const JvFields *burrow__jsonv2_fields(const Type *t) {
    burrow__lock(&jv_cache_lock);
    const JvFields *fs = jv_cache_find(t);
    if (fs == NULL) {
        Arena *ar = BURROW_NEW(heap_allocator(), Arena);
        if (ar != NULL) {
            arena_init(ar, NULL, 0);
            jv_build_alloc = arena_allocator(ar);
            JvFields *built = (JvFields *)(uintptr_t)jv_fields_build(t);
            jv_build_alloc = NULL;
            if (built != NULL && jv_cache_put(t, built)) {
                built->arena = ar;
                fs = built;
            } else {
                arena_free(ar);
                mem_free(heap_allocator(), ar, sizeof(Arena), _Alignof(Arena));
            }
        }
    }
    burrow__unlock(&jv_cache_lock);
    return fs;
}

const JvField *burrow__jsonv2_fields_by_name(const JvFields *fs, Str name) {
    uint64_t m = (uint64_t)(fs->by_name_cap - 1);
    for (uint64_t h = jv_hash(name) & m;; h = (h + 1) & m) {
        Int i = fs->by_name[h];
        if (i == 0)
            return NULL;
        if (str_eq(fs->flat[i - 1].name, name))
            return &fs->flat[i - 1];
    }
}

Int burrow__jsonv2_fields_by_folded(const JvFields *fs, Str name, Int *start) {
    Byte stack[64];
    JsonBuf fb = {stack, 0, (Int)sizeof(stack), heap_allocator(), false, false};
    burrow__jsonv2_append_folded(&fb, name);
    Str folded = str_from_bytes(fb.p, fb.len);
    Int lo = 0, hi = fs->nflat;
    while (lo < hi) {
        Int mid = lo + (hi - lo) / 2;
        if (jv_name_cmp(fs->flat[fs->by_folded[mid]].folded, folded) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    Int n = 0;
    while (lo + n < fs->nflat && str_eq(fs->flat[fs->by_folded[lo + n]].folded, folded))
        n++;
    if (fb.p != stack)
        burrow__jsonbuf_free(&fb);
    *start = lo;
    return n;
}

bool burrow__jsonv2_match_folded(const JvField *f, Str name, const JsontextOptions *o) {
    if (f->casing == JV_CASE_IGNORE ||
        (jsonflags_get(o, JSONFLAG_MATCH_CASE_INSENSITIVE_NAMES) &&
         f->casing != JV_CASE_STRICT)) {
        if (!jsonflags_get(o, JSONFLAG_MATCH_CASE_SENSITIVE_DELIMITER) ||
            strings_equal_fold(name, f->name))
            return true;
    }
    return false;
}

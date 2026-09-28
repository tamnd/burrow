/* mime: a port of Go's mediatype.go and grammar.go.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "internal.h"

#include "burrow/mem/arena.h"
#include "burrow/sort.h"
#include "burrow/strings.h"
#include "burrow/unicode.h"
#include "burrow/utf8.h"

static const Str mime_no_media_type__text = {(const Byte *)"mime: no media type", 19};
static const Error mime_no_media_type = {&burrow_sentinel_error_vt,
                                         &mime_no_media_type__text};
static const Str mime_no_slash__text = {
    (const Byte *)"mime: expected slash after first token", 38};
static const Error mime_no_slash = {&burrow_sentinel_error_vt, &mime_no_slash__text};
static const Str mime_no_token_after_slash__text = {
    (const Byte *)"mime: expected token after slash", 32};
static const Error mime_no_token_after_slash = {&burrow_sentinel_error_vt,
                                                &mime_no_token_after_slash__text};
static const Str mime_after_subtype__text = {
    (const Byte *)"mime: unexpected content after media subtype", 44};
static const Error mime_after_subtype = {&burrow_sentinel_error_vt,
                                         &mime_after_subtype__text};
static const Str mime_duplicate_param__text = {
    (const Byte *)"mime: duplicate parameter name", 30};
static const Error mime_duplicate_param = {&burrow_sentinel_error_vt,
                                           &mime_duplicate_param__text};
static const Str mime_invalid_media_parameter__text = {
    (const Byte *)"mime: invalid media parameter", 29};
const Error mime_err_invalid_media_parameter = {&burrow_sentinel_error_vt,
                                                &mime_invalid_media_parameter__text};
static const Str mime_no_memory__text = {(const Byte *)"mime: out of memory", 19};
const Error burrow__mime_err_no_memory = {&burrow_sentinel_error_vt,
                                          &mime_no_memory__text};

/* ---------------------------------------------------------------- grammar */

/* tspecials of RFC 1521 and RFC 2045. */
static bool mime_is_tspecial(Byte c) {
    switch (c) {
    case '(':
    case ')':
    case '<':
    case '>':
    case '@':
    case ',':
    case ';':
    case ':':
    case '\\':
    case '"':
    case '/':
    case '[':
    case ']':
    case '?':
    case '=':
        return true;
    default:
        return false;
    }
}

/* A token character: US-ASCII other than space, the controls and tspecials. */
static bool mime_is_token_char(Byte c) {
    return c > ' ' && c < 0x7f && !mime_is_tspecial(c);
}

static bool mime_is_token(Str s) {
    if (s.len == 0)
        return false;
    for (Int i = 0; i < s.len; i++)
        if (!mime_is_token_char(s.p[i]))
            return false;
    return true;
}

bool burrow__mime_needs_encoding(Str s) {
    /* Go ranges over runes, and every rune that is not ASCII, the replacement
     * for bad UTF-8 included, is past '~', so bytes give the same answer. */
    for (Int i = 0; i < s.len; i++) {
        Byte b = s.p[i];
        if ((b < ' ' || b > '~') && b != '\t')
            return true;
    }
    return false;
}

static Str mime_sub(Str s, Int from, Int to) {
    /* s.p is NULL for the empty string, and NULL + 0 is undefined. */
    Str r = {s.p == NULL ? s.p : s.p + from, to - from};
    return r;
}

static Str mime_trim_left_space(Str s) {
    Int i = 0;
    while (i < s.len) {
        Int size;
        Rune r = utf8_decode_rune_in_string(mime_sub(s, i, s.len), &size);
        if (!unicode_is_space(r))
            break;
        i += size;
    }
    return mime_sub(s, i, s.len);
}

/* ----------------------------------------------------------- formatting */

static const char mime_upperhex[] = "0123456789ABCDEF";

/* A strings builder that remembers whether a write failed, so the result is
 * not handed out with a piece missing. */
typedef struct MimeBuf {
    StringsBuilder b;
    bool oom;
} MimeBuf;

static void mime_putc(MimeBuf *b, Byte c) {
    if (BURROW_FAILED(strings_builder_write_byte(&b->b, c)))
        b->oom = true;
}

static void mime_put(MimeBuf *b, Str s) {
    Error err;
    if (strings_builder_write_string(&b->b, s, &err) != s.len)
        b->oom = true;
}

static void mime_put_lower(MimeBuf *b, Str s) {
    for (Int i = 0; i < s.len; i++) {
        Byte c = s.p[i];
        if (c >= 'A' && c <= 'Z')
            c = (Byte)(c + ('a' - 'A'));
        mime_putc(b, c);
    }
}

/* One parameter, "; " and all. False when the name is not a token. */
static bool mime_format_param(MimeBuf *b, Str attribute, Str value) {
    mime_put(b, BURROW_S("; "));
    if (!mime_is_token(attribute))
        return false;
    mime_put_lower(b, attribute);

    bool need_enc = burrow__mime_needs_encoding(value);
    if (need_enc) {
        /* RFC 2231 section 4 */
        mime_putc(b, '*');
    }
    mime_putc(b, '=');

    if (need_enc) {
        mime_put(b, BURROW_S("utf-8''"));
        Int offset = 0;
        for (Int index = 0; index < value.len; index++) {
            Byte ch = value.p[index];
            /* attribute-char of RFC 2231 section 7: US-ASCII other than space,
             * the controls, "*", "'", "%" and tspecials. */
            if (ch <= ' ' || ch >= 0x7f || ch == '*' || ch == '\'' || ch == '%' ||
                mime_is_tspecial(ch)) {
                mime_put(b, mime_sub(value, offset, index));
                offset = index + 1;
                mime_putc(b, '%');
                mime_putc(b, (Byte)mime_upperhex[ch >> 4]);
                mime_putc(b, (Byte)mime_upperhex[ch & 0x0f]);
            }
        }
        mime_put(b, mime_sub(value, offset, value.len));
        return true;
    }

    if (mime_is_token(value)) {
        mime_put(b, value);
        return true;
    }

    mime_putc(b, '"');
    Int offset = 0;
    for (Int index = 0; index < value.len; index++) {
        Byte c = value.p[index];
        if (c == '"' || c == '\\') {
            mime_put(b, mime_sub(value, offset, index));
            offset = index;
            mime_putc(b, '\\');
        }
    }
    mime_put(b, mime_sub(value, offset, value.len));
    mime_putc(b, '"');
    return true;
}

Str mime_format_media_type(Alloc *a, Str t, Map *param) {
    Str none = {NULL, 0};
    Arena scratch;
    arena_init(&scratch, a, 0);
    Alloc *sa = arena_allocator(&scratch);
    MimeBuf b = {STRINGS_BUILDER(sa), false};
    Str result = none;

    bool found;
    Str sub;
    Str major = strings_cut(t, BURROW_S("/"), &sub, &found);
    if (!found) {
        if (!mime_is_token(t))
            goto done;
        mime_put_lower(&b, t);
    } else {
        if (!mime_is_token(major) || !mime_is_token(sub))
            goto done;
        mime_put_lower(&b, major);
        mime_putc(&b, '/');
        mime_put_lower(&b, sub);
    }

    Int n = param == NULL ? 0 : map_len(param);
    if (n > 0) {
        Slice keys = slice_make(sa, TYPE_STRING, n, n);
        if (keys.p == NULL)
            goto done;
        Str *kp = (Str *)keys.p;
        Int k = 0;
        const void *key;
        for (MapIter it = map_iter(param); map_next(&it, &key, NULL);)
            kp[k++] = *(const Str *)key;
        sort_strings(keys);
        for (Int i = 0; i < n; i++) {
            const Str *value = (const Str *)map_get(param, &kp[i]);
            if (!mime_format_param(&b, kp[i], *value))
                goto done;
        }
    }

    if (!b.oom)
        result = strings_clone(a, strings_builder_string(&b.b));
done:
    arena_free(&scratch);
    return result;
}

/* -------------------------------------------------------------- parsing */

static Error mime_check_media_type_disposition(Str s);

/* consumeToken: the token at the start of v and what follows it, or an empty
 * token and v. */
static Str mime_consume_token(Str v, Str *rest) {
    for (Int i = 0; i < v.len; i++) {
        if (!mime_is_token_char(v.p[i])) {
            *rest = mime_sub(v, i, v.len);
            return mime_sub(v, 0, i);
        }
    }
    *rest = mime_sub(v, v.len, v.len);
    return v;
}

/* consumeValue: a token, or a quoted string with the quotes and escapes taken
 * out, which is built in sa. An empty value and v itself on failure. */
static Str mime_consume_value(Alloc *sa, Str v, Str *rest, bool *oom) {
    Str none = {NULL, 0};
    *rest = v;
    if (v.len == 0)
        return none;
    if (v.p[0] != '"')
        return mime_consume_token(v, rest);

    MimeBuf b = {STRINGS_BUILDER(sa), false};
    for (Int i = 1; i < v.len; i++) {
        Byte r = v.p[i];
        if (r == '"') {
            *rest = mime_sub(v, i + 1, v.len);
            if (b.oom)
                *oom = true;
            return strings_builder_string(&b.b);
        }
        /* MSIE sends a full file path without escaping the backslashes, so an
         * escape of anything but a tspecial is taken as a literal backslash,
         * which is how Go reads it too. */
        if (r == '\\' && i + 1 < v.len && mime_is_tspecial(v.p[i + 1])) {
            mime_putc(&b, v.p[i + 1]);
            i++;
            continue;
        }
        if (r == '\r' || r == '\n')
            return none;
        mime_putc(&b, r);
    }
    /* No closing quote. */
    return none;
}

/* consumeMediaParam: one "; name=value", with name in lower case. An empty
 * name and v itself as the rest when there is none. */
static Str mime_consume_media_param(Alloc *sa, Str v, Str *value, Str *rest,
                                    bool *oom) {
    Str none = {NULL, 0};
    *value = none;
    *rest = v;
    bool ok;
    Str r = strings_cut_prefix(mime_trim_left_space(v), BURROW_S(";"), &ok);
    if (!ok)
        return none;
    r = mime_trim_left_space(r);
    Str param = mime_consume_token(r, &r);
    if (param.len == 0)
        return none;
    param = strings_to_lower(sa, param);
    if (param.p == NULL) {
        *oom = true;
        return none;
    }
    r = strings_cut_prefix(mime_trim_left_space(r), BURROW_S("="), &ok);
    if (!ok)
        return none;
    r = mime_trim_left_space(r);
    Str r2;
    Str val = mime_consume_value(sa, r, &r2, oom);
    if (val.len == 0 && r2.p == r.p && r2.len == r.len)
        return none;
    *value = val;
    *rest = r2;
    return param;
}

static bool mime_ishex(Byte c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static Byte mime_unhex(Byte c) {
    if (c >= '0' && c <= '9')
        return (Byte)(c - '0');
    if (c >= 'a' && c <= 'f')
        return (Byte)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F')
        return (Byte)(c - 'A' + 10);
    return 0;
}

/* percentHexUnescape, into sa. False when a % is not followed by two hex
 * digits. */
static bool mime_percent_hex_unescape(Alloc *sa, Str s, Str *out, bool *oom) {
    Int percents = 0;
    for (Int i = 0; i < s.len;) {
        if (s.p[i] != '%') {
            i++;
            continue;
        }
        percents++;
        if (i + 2 >= s.len || !mime_ishex(s.p[i + 1]) || !mime_ishex(s.p[i + 2]))
            return false;
        i += 3;
    }
    if (percents == 0) {
        *out = s;
        return true;
    }
    Int n = s.len - 2 * percents;
    Byte *t = (Byte *)mem_alloc_nozero(sa, (size_t)n, 1);
    if (t == NULL) {
        *oom = true;
        return false;
    }
    Int j = 0;
    for (Int i = 0; i < s.len;) {
        if (s.p[i] == '%') {
            t[j++] = (Byte)(mime_unhex(s.p[i + 1]) << 4 | mime_unhex(s.p[i + 2]));
            i += 3;
        } else {
            t[j++] = s.p[i++];
        }
    }
    out->p = t;
    out->len = n;
    return true;
}

/* decode2231Enc: charset'language'text, with only US-ASCII and UTF-8 taken.
 * The language is ignored, as Go does. */
static bool mime_decode_2231_enc(Alloc *sa, Str v, Str *out, bool *oom) {
    bool ok;
    Str rest;
    Str charset = strings_cut(v, BURROW_S("'"), &rest, &ok);
    if (!ok)
        return false;
    Str ext_other_vals;
    strings_cut(rest, BURROW_S("'"), &ext_other_vals, &ok);
    if (!ok)
        return false;
    charset = strings_to_lower(sa, charset);
    if (charset.p == NULL && charset.len > 0) {
        *oom = true;
        return false;
    }
    if (!str_eq(charset, BURROW_S("us-ascii")) && !str_eq(charset, BURROW_S("utf-8")))
        return false;
    return mime_percent_hex_unescape(sa, ext_other_vals, out, oom);
}

static Error mime_check_media_type_disposition(Str s) {
    Str rest;
    Str typ = mime_consume_token(s, &rest);
    if (typ.len == 0)
        return mime_no_media_type;
    if (rest.len == 0)
        return BURROW_NO_ERROR;
    bool ok;
    rest = strings_cut_prefix(rest, BURROW_S("/"), &ok);
    if (!ok)
        return mime_no_slash;
    Str subtype = mime_consume_token(rest, &rest);
    if (subtype.len == 0)
        return mime_no_token_after_slash;
    if (rest.len != 0)
        return mime_after_subtype;
    return BURROW_NO_ERROR;
}

/* "key*n" in sa, for the continuation lookups. */
static Str mime_part_name(Alloc *sa, Str key, Int n, bool star) {
    char digits[24];
    Int d = 0;
    do {
        digits[d++] = (char)('0' + n % 10);
        n /= 10;
    } while (n > 0);
    Int len = key.len + 1 + d + (star ? 1 : 0);
    Byte *p = (Byte *)mem_alloc_nozero(sa, (size_t)len, 1);
    Str none = {NULL, 0};
    if (p == NULL)
        return none;
    Int j = 0;
    for (Int i = 0; i < key.len; i++)
        p[j++] = key.p[i];
    p[j++] = '*';
    while (d > 0)
        p[j++] = (Byte)digits[--d];
    if (star)
        p[j++] = '*';
    Str s = {p, len};
    return s;
}

/* Gives back a string cloned into a, on the way out after a failure. */
static void mime_free_str(Alloc *a, Str s) {
    if (s.p != NULL && s.len > 0)
        mem_free(a, (void *)(uintptr_t)s.p, (size_t)s.len, 1);
}

/* Gives back a map of cloned strings and the strings in it. */
static void mime_free_params(Alloc *a, Map *m) {
    const void *kp;
    void *vp;
    for (MapIter it = map_iter(m); map_next(&it, &kp, &vp);) {
        mime_free_str(a, *(const Str *)kp);
        mime_free_str(a, *(const Str *)vp);
    }
    map_free(m);
}

static Str mime_fail(Map **params, Error *err, Error e, Str result) {
    if (params != NULL)
        *params = NULL;
    *err = e;
    return result;
}

Str mime_parse_media_type(Alloc *a, Str v, Map **params, Error *err) {
    Str none = {NULL, 0};
    Arena scratch;
    arena_init(&scratch, a, 0);
    Alloc *sa = arena_allocator(&scratch);
    bool oom = false;
    Str result = none;
    Map *out = NULL;
    *err = BURROW_NO_ERROR;

    bool found;
    Str after;
    Str base = strings_cut(v, BURROW_S(";"), &after, &found);
    Str lower = strings_to_lower(sa, base);
    if (lower.p == NULL && base.len > 0)
        goto no_memory;
    Str mediatype = strings_trim_space(lower);

    Error e = mime_check_media_type_disposition(mediatype);
    if (BURROW_FAILED(e)) {
        arena_free(&scratch);
        return mime_fail(params, err, e, none);
    }
    /* The media type is all token characters now, so it is ASCII. */
    result = strings_clone(a, mediatype);
    if (result.p == NULL)
        goto no_memory;

    /* Parameters without a star go straight into out. The rest, "name*",
     * "name*0" and "name*1*", wait in cont under their full name, with the base
     * names they belong to in bases, until they can be put back together. */
    out = map_make(sa, TYPE_STRING, TYPE_STRING, 0);
    Map *cont = map_make(sa, TYPE_STRING, TYPE_STRING, 0);
    Map *bases = map_make(sa, TYPE_STRING, TYPE_BOOL, 0);
    if (out == NULL || cont == NULL || bases == NULL)
        goto no_memory;

    v = mime_sub(v, base.len, v.len);
    while (v.len > 0) {
        v = mime_trim_left_space(v);
        if (v.len == 0)
            break;
        Str value, rest;
        Str key = mime_consume_media_param(sa, v, &value, &rest, &oom);
        if (oom)
            goto no_memory;
        if (key.len == 0) {
            if (str_eq(strings_trim_space(rest), BURROW_S(";"))) {
                /* A semicolon at the end is not an error. */
                break;
            }
            arena_free(&scratch);
            return mime_fail(params, err, mime_err_invalid_media_parameter, result);
        }

        Map *pmap = out;
        Str base_name = strings_cut(key, BURROW_S("*"), &after, &found);
        if (found) {
            pmap = cont;
            bool yes = true;
            if (!map_set(bases, &base_name, &yes))
                goto no_memory;
        }
        const Str *existing = (const Str *)map_get(pmap, &key);
        if (existing != NULL && !str_eq(*existing, value)) {
            /* The same name twice is only wrong with two different values. */
            arena_free(&scratch);
            return mime_fail(params, err, mime_duplicate_param, none);
        }
        if (!map_set(pmap, &key, &value))
            goto no_memory;
        v = rest;
    }

    /* Put the RFC 2231 pieces together: "name*" is one encoded value, and
     * "name*0", "name*1*" and so on are parts, which only the first of may say
     * what charset it is in. */
    const void *kp;
    for (MapIter it = map_iter(bases); map_next(&it, &kp, NULL);) {
        Str key = *(const Str *)kp;
        Str single = mime_part_name(sa, key, 0, false);
        if (single.p == NULL)
            goto no_memory;
        single.len = key.len + 1; /* "key*", the same bytes without the 0 */
        const Str *sv = (const Str *)map_get(cont, &single);
        if (sv != NULL) {
            Str decv;
            if (mime_decode_2231_enc(sa, *sv, &decv, &oom) &&
                !map_set(out, &key, &decv))
                goto no_memory;
            if (oom)
                goto no_memory;
            continue;
        }

        MimeBuf buf = {STRINGS_BUILDER(sa), false};
        bool valid = false;
        for (Int n = 0;; n++) {
            Str simple = mime_part_name(sa, key, n, false);
            Str encoded = mime_part_name(sa, key, n, true);
            if (simple.p == NULL || encoded.p == NULL)
                goto no_memory;
            const Str *pv = (const Str *)map_get(cont, &simple);
            if (pv != NULL) {
                valid = true;
                mime_put(&buf, *pv);
                continue;
            }
            pv = (const Str *)map_get(cont, &encoded);
            if (pv == NULL)
                break;
            valid = true;
            Str decv;
            if (n == 0) {
                if (mime_decode_2231_enc(sa, *pv, &decv, &oom))
                    mime_put(&buf, decv);
            } else if (mime_percent_hex_unescape(sa, *pv, &decv, &oom)) {
                mime_put(&buf, decv);
            }
            if (oom)
                goto no_memory;
        }
        if (buf.oom)
            goto no_memory;
        if (valid) {
            Str joined = strings_builder_string(&buf.b);
            if (!map_set(out, &key, &joined))
                goto no_memory;
        }
    }

    if (params != NULL) {
        /* Everything so far is in the scratch arena. Copy what is kept into a. */
        Map *m = map_make(a, TYPE_STRING, TYPE_STRING, map_len(out));
        if (m == NULL)
            goto no_memory;
        const void *okp;
        void *ovp;
        for (MapIter it = map_iter(out); map_next(&it, &okp, &ovp);) {
            Str k = *(const Str *)okp, val = *(const Str *)ovp;
            Str kc = strings_clone(a, k), vc = strings_clone(a, val);
            if ((kc.p == NULL && k.len > 0) || (vc.p == NULL && val.len > 0) ||
                !map_set(m, &kc, &vc)) {
                mime_free_str(a, kc);
                mime_free_str(a, vc);
                mime_free_params(a, m);
                goto no_memory;
            }
        }
        *params = m;
    }
    arena_free(&scratch);
    return result;

no_memory:
    arena_free(&scratch);
    mime_free_str(a, result);
    return mime_fail(params, err, burrow__mime_err_no_memory, none);
}

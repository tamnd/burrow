/* Derived from Go's src/net/mail/message.go.
 * Go source: go1.27.1.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/net/mail.h"

#include "burrow/declare.h"
#include "burrow/fmt.h"
#include "burrow/map.h"
#include "burrow/net.h"
#include "burrow/strings.h"
#include "burrow/utf8.h"

#include <stdint.h>
#include <string.h>

BURROW_SENTINEL_ERROR(mail_err_header_not_present, "mail: header not in message");

BURROW_SENTINEL_ERROR(ml_err_comment, "mail: misformatted parenthetical comment");
BURROW_SENTINEL_ERROR(ml_err_comma, "mail: expected comma");
BURROW_SENTINEL_ERROR(ml_err_empty_group, "mail: empty group");
BURROW_SENTINEL_ERROR(ml_err_group_many, "mail: group with multiple addresses");
BURROW_SENTINEL_ERROR(ml_err_no_address, "mail: no address");
BURROW_SENTINEL_ERROR(ml_err_missing_at_or_angle, "mail: missing '@' or angle-addr");
BURROW_SENTINEL_ERROR(ml_err_no_angle, "mail: no angle-addr");
BURROW_SENTINEL_ERROR(ml_err_unclosed_angle, "mail: unclosed angle-addr");
BURROW_SENTINEL_ERROR(ml_err_no_addr_spec, "mail: no addr-spec");
BURROW_SENTINEL_ERROR(ml_err_empty_quoted, "mail: empty quoted string in addr-spec");
BURROW_SENTINEL_ERROR(ml_err_missing_at, "mail: missing @ in addr-spec");
BURROW_SENTINEL_ERROR(ml_err_no_domain, "mail: no domain in addr-spec");
BURROW_SENTINEL_ERROR(ml_err_unclosed_quoted, "mail: unclosed quoted-string");
BURROW_SENTINEL_ERROR(ml_err_invalid_string, "mail: invalid string");
BURROW_SENTINEL_ERROR(ml_err_leading_dot, "mail: leading dot in atom");
BURROW_SENTINEL_ERROR(ml_err_double_dot, "mail: double dot in atom");
BURROW_SENTINEL_ERROR(ml_err_trailing_dot, "mail: trailing dot in atom");
BURROW_SENTINEL_ERROR(ml_err_missing_bracket, "mail: missing \"[\" in domain-literal");
BURROW_SENTINEL_ERROR(ml_err_unclosed_literal, "mail: unclosed domain-literal");
BURROW_SENTINEL_ERROR(ml_err_comment_start, "mail: comment does not start with (");
BURROW_SENTINEL_ERROR(ml_err_cr, "mail: header has a CR without LF");
BURROW_SENTINEL_ERROR(ml_err_bad_date, "mail: header could not be parsed");

/* ------------------------------------------------------------- characters */

static bool ml_is_multibyte(Rune r) {
    return r >= UTF8_RUNE_SELF;
}

static bool ml_is_vchar(Rune r) {
    return ('!' <= r && r <= '~') || ml_is_multibyte(r);
}

static bool ml_is_wsp(Rune r) {
    return r == ' ' || r == '\t';
}

static bool ml_is_atext(Rune r, bool dot) {
    switch (r) {
    case '.':
        return dot;
    case '(':
    case ')':
    case '<':
    case '>':
    case '[':
    case ']':
    case ':':
    case ';':
    case '@':
    case '\\':
    case ',':
    case '"':
        return false;
    default:
        return ml_is_vchar(r);
    }
}

static bool ml_is_qtext(Rune r) {
    if (r == '\\' || r == '"')
        return false;
    return ml_is_vchar(r);
}

static bool ml_is_dtext(Rune r) {
    if (r == '[' || r == ']' || r == '\\')
        return false;
    return ml_is_vchar(r);
}

/* The rune at the start of s, with a bad byte as UTF8_RUNE_ERROR of size 1, as
 * range over a string gives it in Go. */
static Rune ml_rune(Str s, Int *size) {
    return utf8_decode_rune_in_string(s, size);
}

/* --------------------------------------------------------- scratch strings */

/* A growing array of strings in the scratch arena. */
typedef struct MlStrs {
    Str *p;
    Int len, cap;
} MlStrs;

static bool ml_strs_push(Alloc *sa, MlStrs *v, Str s) {
    if (v->len == v->cap) {
        Int cap = v->cap == 0 ? 4 : v->cap * 2;
        Str *p = (Str *)mem_alloc_array(sa, (size_t)cap, sizeof(Str), _Alignof(Str));
        if (p == NULL)
            return false;
        if (v->len > 0)
            memcpy(p, v->p, (size_t)v->len * sizeof(Str));
        v->p = p;
        v->cap = cap;
    }
    v->p[v->len++] = s;
    return true;
}

/* The words of v joined by sep, which is never empty. False when out of
 * memory, which an empty result cannot say by itself: one word can be "". */
static bool ml_join(Alloc *sa, MlStrs *v, Str sep, Str *out) {
    if (v->len <= 1) {
        *out = v->len == 1 ? v->p[0] : (Str){NULL, 0};
        return true;
    }
    *out = strings_join(sa, slice_from(v->p, v->len, v->cap, TYPE_STRING), sep);
    return out->p != NULL;
}

/* The name and address of one parsed address, still in the scratch arena. */
typedef struct MlAddr {
    Str name;
    Str address;
} MlAddr;

typedef struct MlAddrs {
    MlAddr *p;
    Int len, cap;
} MlAddrs;

static bool ml_addrs_push(Alloc *sa, MlAddrs *v, Str name, Str address) {
    if (v->len == v->cap) {
        Int cap = v->cap == 0 ? 4 : v->cap * 2;
        MlAddr *p = (MlAddr *)mem_alloc_array(sa, (size_t)cap, sizeof(MlAddr),
                                              _Alignof(MlAddr));
        if (p == NULL)
            return false;
        if (v->len > 0)
            memcpy(p, v->p, (size_t)v->len * sizeof(MlAddr));
        v->p = p;
        v->cap = cap;
    }
    v->p[v->len].name = name;
    v->p[v->len].address = address;
    v->len++;
    return true;
}

/* A strings builder that remembers whether a write failed. */
typedef struct MlBuf {
    StringsBuilder b;
    bool oom;
} MlBuf;

static void ml_putc(MlBuf *b, Byte c) {
    if (BURROW_FAILED(strings_builder_write_byte(&b->b, c)))
        b->oom = true;
}

static void ml_put(MlBuf *b, Str s) {
    Error err;
    if (strings_builder_write_string(&b->b, s, &err) != s.len)
        b->oom = true;
}

static void ml_put_rune(MlBuf *b, Rune r) {
    Error err = BURROW_NO_ERROR;
    strings_builder_write_rune(&b->b, r, &err);
    if (BURROW_FAILED(err))
        b->oom = true;
}

/* ----------------------------------------------------------------- parser */

typedef struct MlParser {
    Str s;
    const MimeWordDecoder *dec; /* may be NULL */
    Alloc *sa;                  /* the scratch arena */
} MlParser;

static bool ml_empty(const MlParser *p) {
    return p->s.len == 0;
}

static Byte ml_peek(const MlParser *p) {
    return p->s.p[0];
}

static void ml_advance(MlParser *p, Int n) {
    p->s.p += n;
    p->s.len -= n;
}

static bool ml_consume(MlParser *p, Byte c) {
    if (ml_empty(p) || ml_peek(p) != c)
        return false;
    ml_advance(p, 1);
    return true;
}

static void ml_skip_space(MlParser *p) {
    p->s = strings_trim_left(p->s, BURROW_S(" \t"));
}

/* After a '(' has been taken, the rest of the comment, with nested comments
 * and escapes. When b is not NULL the text goes there. False when the comment
 * is not closed. */
static bool ml_consume_comment(MlParser *p, MlBuf *b) {
    int depth = 1;
    while (!ml_empty(p) && depth != 0) {
        if (ml_peek(p) == '\\' && p->s.len > 1)
            ml_advance(p, 1);
        else if (ml_peek(p) == '(')
            depth++;
        else if (ml_peek(p) == ')')
            depth--;
        if (depth > 0 && b != NULL)
            ml_putc(b, ml_peek(p));
        ml_advance(p, 1);
    }
    return depth == 0;
}

static bool ml_skip_cfws(MlParser *p) {
    ml_skip_space(p);
    while (ml_consume(p, '(')) {
        if (!ml_consume_comment(p, NULL))
            return false;
        ml_skip_space(p);
    }
    return true;
}

static Error ml_consume_quoted_string(MlParser *p, Str *out) {
    *out = (Str){NULL, 0};
    Int i = 1; /* the '"' */
    MlBuf b = {STRINGS_BUILDER(p->sa), false};
    bool escaped = false;
    for (;;) {
        Int size;
        Str rest = {p->s.p + i, p->s.len - i};
        Rune r = ml_rune(rest, &size);
        if (size == 0)
            return ml_err_unclosed_quoted;
        if (size == 1 && r == UTF8_RUNE_ERROR)
            return fmt_errorf_v("mail: invalid utf-8 in quoted-string: %q", p->s);
        if (escaped) {
            /* quoted-pair = ("\" (VCHAR / WSP)) */
            if (!ml_is_vchar(r) && !ml_is_wsp(r))
                return fmt_errorf_v("mail: bad character in quoted-string: %q", r);
            ml_put_rune(&b, r);
            escaped = false;
        } else if (ml_is_qtext(r) || ml_is_wsp(r)) {
            /* qtext, or FWS without the CRLF */
            ml_put_rune(&b, r);
        } else if (r == '"') {
            break;
        } else if (r == '\\') {
            escaped = true;
        } else {
            return fmt_errorf_v("mail: bad character in quoted-string: %q", r);
        }
        i += size;
    }
    if (b.oom)
        return burrow_err_out_of_memory;
    ml_advance(p, i + 1);
    *out = strings_builder_string(&b.b);
    return BURROW_NO_ERROR;
}

/* An atom, or a dot-atom when dot is set. permissive lets leading, trailing
 * and double dots through, as Go does for names (golang.org/issue/4938). */
static Error ml_consume_atom(MlParser *p, bool dot, bool permissive, Str *out) {
    *out = (Str){NULL, 0};
    Int i = 0;
    for (;;) {
        Int size;
        Rune r = ml_rune((Str){p->s.p + i, p->s.len - i}, &size);
        if (size == 1 && r == UTF8_RUNE_ERROR)
            return fmt_errorf_v("mail: invalid utf-8 in address: %q", p->s);
        if (size == 0 || !ml_is_atext(r, dot))
            break;
        i += size;
    }
    if (i == 0)
        return ml_err_invalid_string;
    Str atom = {p->s.p, i};
    ml_advance(p, i);
    if (!permissive) {
        if (strings_has_prefix(atom, BURROW_S(".")))
            return ml_err_leading_dot;
        if (strings_contains(atom, BURROW_S("..")))
            return ml_err_double_dot;
        if (strings_has_suffix(atom, BURROW_S(".")))
            return ml_err_trailing_dot;
    }
    *out = atom;
    return BURROW_NO_ERROR;
}

static Error ml_consume_domain_literal(MlParser *p, Str *out) {
    *out = (Str){NULL, 0};
    const Byte *start = p->s.p;
    if (!ml_consume(p, '['))
        return ml_err_missing_bracket;
    Str dtext = p->s;
    Int dtext_len = 0;
    for (;;) {
        if (ml_empty(p))
            return ml_err_unclosed_literal;
        if (ml_peek(p) == ']')
            break;
        Int size;
        Rune r = ml_rune(p->s, &size);
        if (size == 1 && r == UTF8_RUNE_ERROR)
            return fmt_errorf_v("mail: invalid utf-8 in domain-literal: %q", p->s);
        if (!ml_is_dtext(r))
            return fmt_errorf_v("mail: bad character in domain-literal: %q", r);
        dtext_len += size;
        ml_advance(p, size);
    }
    dtext.len = dtext_len;
    if (!ml_consume(p, ']'))
        return ml_err_unclosed_literal;

    bool v6 = false;
    Str addr = strings_cut_prefix(dtext, BURROW_S("IPv6:"), &v6);
    if (v6) {
        if (net_parse_ip(p->sa, addr).len != NET_IPV6_LEN)
            return fmt_errorf_v("mail: invalid IPv6 address in domain-literal: %q",
                                dtext);
    } else if (net_ip_to4(net_parse_ip(p->sa, dtext)).p == NULL) {
        return fmt_errorf_v("mail: invalid IP address in domain-literal: %q", dtext);
    }
    /* "[" + dtext + "]" is the text it came from. */
    *out = (Str){start, (Int)(p->s.p - start)};
    return BURROW_NO_ERROR;
}

/* The charset reader the decoder is given, which notes whether it was the
 * charset that failed. */
typedef struct MlCharset {
    const MimeWordDecoder *dec;
    bool failed;
} MlCharset;

static IoReader ml_charset_reader(void *env, Str charset, IoReader input, Error *err) {
    MlCharset *c = (MlCharset *)env;
    if (c->dec == NULL || BURROW_FUNC_IS_NIL(c->dec->charset_reader)) {
        c->failed = true;
        *err = fmt_errorf_v("charset not supported: %q", charset);
        return (IoReader){0};
    }
    IoReader r =
        c->dec->charset_reader.f(c->dec->charset_reader.env, charset, input, err);
    if (BURROW_FAILED(*err))
        c->failed = true;
    return r;
}

/* s decoded when it is an RFC 2047 encoded word. A word that does not decode
 * is left as it is with no error, but a charset that cannot be read gives s
 * back marked as encoded, with the error. */
static Error ml_decode_word(MlParser *p, Str s, Str *word, bool *is_encoded) {
    MlCharset c = {p->dec, false};
    MimeWordDecoder adec = {BURROW_FN(MimeCharsetReader, ml_charset_reader, &c)};
    ArenaMark m = error_mark();
    Error err = BURROW_NO_ERROR;
    Str w = mime_word_decoder_decode(&adec, p->sa, s, &err);
    if (BURROW_OK(err)) {
        *word = w;
        *is_encoded = true;
        return BURROW_NO_ERROR;
    }
    *word = s;
    if (c.failed) {
        *is_encoded = true;
        return err;
    }
    if (errors_is(err, burrow_err_out_of_memory)) {
        *is_encoded = false;
        return err;
    }
    error_release(m);
    *is_encoded = false;
    return BURROW_NO_ERROR;
}

static Error ml_consume_display_name_comment(MlParser *p, Str *out) {
    *out = (Str){NULL, 0};
    if (!ml_consume(p, '('))
        return ml_err_comment_start;
    MlBuf b = {STRINGS_BUILDER(p->sa), false};
    if (!ml_consume_comment(p, &b))
        return ml_err_comment;
    if (b.oom)
        return burrow_err_out_of_memory;
    Str comment = strings_builder_string(&b.b);

    /* The words of the comment, split on spaces and tabs, each decoded when it
     * is an encoded word. */
    MlStrs words = {0};
    Int i = 0;
    while (i < comment.len) {
        while (i < comment.len && (comment.p[i] == ' ' || comment.p[i] == '\t'))
            i++;
        Int start = i;
        while (i < comment.len && comment.p[i] != ' ' && comment.p[i] != '\t')
            i++;
        if (i == start)
            break;
        Str word = {comment.p + start, i - start};
        Str decoded;
        bool encoded;
        Error err = ml_decode_word(p, word, &decoded, &encoded);
        if (BURROW_FAILED(err))
            return err;
        if (!ml_strs_push(p->sa, &words, encoded ? decoded : word))
            return burrow_err_out_of_memory;
    }
    Str joined;
    if (!ml_join(p->sa, &words, BURROW_S(" "), &joined))
        return burrow_err_out_of_memory;
    *out = joined;
    return BURROW_NO_ERROR;
}

static Error ml_consume_phrase(MlParser *p, Str *out) {
    *out = (Str){NULL, 0};
    /* phrase = 1*word */
    MlStrs words = {0};
    MlBuf sb = {STRINGS_BUILDER(p->sa), false};
    ArenaMark m = error_mark();
    Error err = BURROW_NO_ERROR;
    for (;;) {
        /* obs-phrase allows CFWS after one word */
        if (words.len > 0 && !ml_skip_cfws(p))
            return ml_err_comment;
        /* word = atom / quoted-string */
        ml_skip_space(p);
        if (ml_empty(p))
            break;
        bool is_encoded = false;
        Str word;
        if (ml_peek(p) == '"') {
            err = ml_consume_quoted_string(p, &word);
        } else {
            /* A dot-atom, which is more than RFC 5322 allows here. */
            err = ml_consume_atom(p, true, true, &word);
            if (BURROW_OK(err))
                err = ml_decode_word(p, word, &word, &is_encoded);
        }
        if (BURROW_FAILED(err))
            break;
        if (is_encoded) {
            ml_put(&sb, word);
        } else if (strings_builder_len(&sb.b) > 0) {
            Str joined = strings_clone(p->sa, strings_builder_string(&sb.b));
            if (joined.p == NULL || !ml_strs_push(p->sa, &words, joined))
                return burrow_err_out_of_memory;
            strings_builder_reset(&sb.b);
            sb.b = STRINGS_BUILDER(p->sa);
            if (!ml_strs_push(p->sa, &words, word))
                return burrow_err_out_of_memory;
        } else if (!ml_strs_push(p->sa, &words, word)) {
            return burrow_err_out_of_memory;
        }
        if (sb.oom)
            return burrow_err_out_of_memory;
    }
    if (strings_builder_len(&sb.b) > 0 &&
        !ml_strs_push(p->sa, &words, strings_builder_string(&sb.b)))
        return burrow_err_out_of_memory;

    if (BURROW_FAILED(err)) {
        if (errors_is(err, burrow_err_out_of_memory))
            return err;
        /* Any error is let go once there is a word. */
        if (words.len == 0)
            return fmt_errorf_v("mail: missing word in phrase: %v", err);
        error_release(m);
    }
    Str phrase;
    if (!ml_join(p->sa, &words, BURROW_S(" "), &phrase))
        return burrow_err_out_of_memory;
    *out = phrase;
    return BURROW_NO_ERROR;
}

/* An addr-spec. p is left as it was when there is an error. */
static Error ml_consume_addr_spec(MlParser *p, Str *out) {
    *out = (Str){NULL, 0};
    MlParser orig = *p;
    Error err;
    Str local;

    /* local-part = dot-atom / quoted-string */
    ml_skip_space(p);
    if (ml_empty(p)) {
        err = ml_err_no_addr_spec;
        goto fail;
    }
    if (ml_peek(p) == '"') {
        err = ml_consume_quoted_string(p, &local);
        if (local.len == 0 && !errors_is(err, burrow_err_out_of_memory))
            err = ml_err_empty_quoted;
    } else {
        err = ml_consume_atom(p, true, false, &local);
    }
    if (BURROW_FAILED(err))
        goto fail;
    if (!ml_consume(p, '@')) {
        err = ml_err_missing_at;
        goto fail;
    }

    /* domain = dot-atom / domain-literal */
    Str domain;
    ml_skip_space(p);
    if (ml_empty(p)) {
        err = ml_err_no_domain;
        goto fail;
    }
    if (ml_peek(p) == '[')
        err = ml_consume_domain_literal(p, &domain);
    else
        err = ml_consume_atom(p, true, false, &domain);
    if (BURROW_FAILED(err))
        goto fail;

    MlBuf b = {STRINGS_BUILDER(p->sa), false};
    ml_put(&b, local);
    ml_putc(&b, '@');
    ml_put(&b, domain);
    if (b.oom) {
        err = burrow_err_out_of_memory;
        goto fail;
    }
    *out = strings_builder_string(&b.b);
    return BURROW_NO_ERROR;

fail:
    *p = orig;
    return err;
}

static Error ml_parse_address(MlParser *p, bool handle_group, MlAddrs *list);

static Error ml_consume_group_list(MlParser *p, MlAddrs *list) {
    ml_skip_space(p);
    if (ml_consume(p, ';')) {
        if (!ml_skip_cfws(p))
            return ml_err_comment;
        return BURROW_NO_ERROR;
    }
    for (;;) {
        ml_skip_space(p);
        /* No groups inside a group. */
        Error err = ml_parse_address(p, false, list);
        if (BURROW_FAILED(err))
            return err;
        if (!ml_skip_cfws(p))
            return ml_err_comment;
        if (ml_consume(p, ';')) {
            if (!ml_skip_cfws(p))
                return ml_err_comment;
            return BURROW_NO_ERROR;
        }
        if (!ml_consume(p, ','))
            return ml_err_comma;
    }
}

/* One address, or the addresses of a group when handle_group is set, onto
 * list. */
static Error ml_parse_address(MlParser *p, bool handle_group, MlAddrs *list) {
    ml_skip_space(p);
    if (ml_empty(p))
        return ml_err_no_address;

    /* address = mailbox / group
     * mailbox = name-addr / addr-spec
     * group = display-name ":" [group-list] ";" [CFWS]
     *
     * An addr-spec is the stricter of the two, so it goes first, and a
     * name-addr is tried when it does not fit. */
    ArenaMark m = error_mark();
    Str spec;
    Error err = ml_consume_addr_spec(p, &spec);
    if (BURROW_OK(err)) {
        Str display = {NULL, 0};
        ml_skip_space(p);
        if (!ml_empty(p) && ml_peek(p) == '(') {
            err = ml_consume_display_name_comment(p, &display);
            if (BURROW_FAILED(err))
                return err;
        }
        if (!ml_addrs_push(p->sa, list, display, spec))
            return burrow_err_out_of_memory;
        return BURROW_NO_ERROR;
    }
    if (errors_is(err, burrow_err_out_of_memory))
        return err;
    error_release(m);

    /* display-name */
    Str display = {NULL, 0};
    if (ml_peek(p) != '<') {
        err = ml_consume_phrase(p, &display);
        if (BURROW_FAILED(err))
            return err;
    }

    ml_skip_space(p);
    if (handle_group && ml_consume(p, ':'))
        return ml_consume_group_list(p, list);

    /* angle-addr = "<" addr-spec ">" */
    if (!ml_consume(p, '<')) {
        bool atext = true;
        for (Int i = 0; i < display.len;) {
            Int size;
            Rune r = ml_rune((Str){display.p + i, display.len - i}, &size);
            if (!ml_is_atext(r, true)) {
                atext = false;
                break;
            }
            i += size;
        }
        /* "foo.bar" may have meant "foo.bar@domain" or "foo.bar <...>", but
         * "Full Name" can only have meant "Full Name <...>". */
        return atext ? ml_err_missing_at_or_angle : ml_err_no_angle;
    }
    err = ml_consume_addr_spec(p, &spec);
    if (BURROW_FAILED(err))
        return err;
    if (!ml_consume(p, '>'))
        return ml_err_unclosed_angle;
    if (!ml_addrs_push(p->sa, list, display, spec))
        return burrow_err_out_of_memory;
    return BURROW_NO_ERROR;
}

static Error ml_parse_address_list(MlParser *p, MlAddrs *list) {
    for (;;) {
        ml_skip_space(p);
        /* Empty entries are skipped, as obs-addr-list allows. */
        if (ml_consume(p, ','))
            continue;
        Error err = ml_parse_address(p, true, list);
        if (BURROW_FAILED(err))
            return err;
        if (!ml_skip_cfws(p))
            return ml_err_comment;
        if (ml_empty(p))
            return BURROW_NO_ERROR;
        if (ml_peek(p) != ',')
            return ml_err_comma;
        while (ml_consume(p, ','))
            ml_skip_space(p);
        if (ml_empty(p))
            return BURROW_NO_ERROR;
    }
}

static Error ml_parse_single_address(MlParser *p, MlAddrs *list) {
    Error err = ml_parse_address(p, true, list);
    if (BURROW_FAILED(err))
        return err;
    if (!ml_skip_cfws(p))
        return ml_err_comment;
    if (!ml_empty(p))
        return fmt_errorf_v("mail: expected single address, got %q", p->s);
    if (list->len == 0)
        return ml_err_empty_group;
    if (list->len > 1)
        return ml_err_group_many;
    return BURROW_NO_ERROR;
}

/* --------------------------------------------------------------- addresses */

/* A MailAddress in one block from a, with the two strings after the struct. */
static MailAddress *ml_new_address(Alloc *a, Str name, Str address) {
    size_t size = sizeof(MailAddress) + (size_t)name.len + (size_t)address.len;
    MailAddress *addr = (MailAddress *)mem_alloc(a, size, _Alignof(MailAddress));
    if (addr == NULL)
        return NULL;
    Byte *q = (Byte *)(addr + 1);
    if (name.len > 0)
        memcpy(q, name.p, (size_t)name.len);
    addr->name = (Str){q, name.len};
    q += name.len;
    if (address.len > 0)
        memcpy(q, address.p, (size_t)address.len);
    addr->address = (Str){q, address.len};
    addr->mem = addr;
    addr->size = size;
    return addr;
}

void mail_address_free(Alloc *a, MailAddress *addr) {
    if (addr == NULL || addr->mem == NULL)
        return;
    mem_free(a, addr->mem, addr->size, _Alignof(MailAddress));
}

void mail_address_list_free(Alloc *a, Slice list) {
    MailAddress **v = (MailAddress **)list.p;
    for (Int i = 0; i < list.len; i++)
        mail_address_free(a, v[i]);
    if (list.p != NULL)
        mem_free(a, list.p, (size_t)list.cap * sizeof(MailAddress *),
                 _Alignof(MailAddress *));
}

MailAddress *mail_address_parser_parse(const MailAddressParser *p, Alloc *a,
                                       Str address, Error *err) {
    Arena scratch;
    arena_init(&scratch, NULL, 0);
    MlParser ps = {address, p != NULL ? p->word_decoder : NULL,
                   arena_allocator(&scratch)};
    MlAddrs list = {0};
    MailAddress *out = NULL;
    *err = ml_parse_single_address(&ps, &list);
    if (BURROW_OK(*err)) {
        out = ml_new_address(a, list.p[0].name, list.p[0].address);
        if (out == NULL)
            *err = burrow_err_out_of_memory;
    }
    arena_free(&scratch);
    return out;
}

Slice mail_address_parser_parse_list(const MailAddressParser *p, Alloc *a, Str list,
                                     Error *err) {
    Slice none = slice_from(NULL, 0, 0, TYPE_UNSAFE_POINTER);
    Arena scratch;
    arena_init(&scratch, NULL, 0);
    MlParser ps = {list, p != NULL ? p->word_decoder : NULL, arena_allocator(&scratch)};
    MlAddrs addrs = {0};
    *err = ml_parse_address_list(&ps, &addrs);
    if (BURROW_FAILED(*err) || addrs.len == 0) {
        arena_free(&scratch);
        return none;
    }
    MailAddress **v = (MailAddress **)mem_alloc_array(
        a, (size_t)addrs.len, sizeof(MailAddress *), _Alignof(MailAddress *));
    if (v == NULL) {
        arena_free(&scratch);
        *err = burrow_err_out_of_memory;
        return none;
    }
    Slice out = slice_from(v, addrs.len, addrs.len, TYPE_UNSAFE_POINTER);
    for (Int i = 0; i < addrs.len; i++) {
        v[i] = ml_new_address(a, addrs.p[i].name, addrs.p[i].address);
        if (v[i] == NULL) {
            out.len = i;
            mail_address_list_free(a, out);
            arena_free(&scratch);
            *err = burrow_err_out_of_memory;
            return none;
        }
    }
    arena_free(&scratch);
    return out;
}

MailAddress *mail_parse_address(Alloc *a, Str address, Error *err) {
    return mail_address_parser_parse(NULL, a, address, err);
}

Slice mail_parse_address_list(Alloc *a, Str list, Error *err) {
    return mail_address_parser_parse_list(NULL, a, list, err);
}

/* s as an RFC 5322 quoted-string. Control characters are dropped. */
static void ml_quote_string(MlBuf *b, Str s) {
    ml_putc(b, '"');
    for (Int i = 0; i < s.len;) {
        Int size;
        Rune r = ml_rune((Str){s.p + i, s.len - i}, &size);
        if (ml_is_qtext(r) || ml_is_wsp(r)) {
            ml_put_rune(b, r);
        } else if (ml_is_vchar(r)) {
            ml_putc(b, '\\');
            ml_put_rune(b, r);
        }
        i += size;
    }
    ml_putc(b, '"');
}

Str mail_address_string(const MailAddress *addr, Alloc *a) {
    Str none = {NULL, 0};
    Arena scratch;
    arena_init(&scratch, NULL, 0);
    Alloc *sa = arena_allocator(&scratch);
    MlBuf b = {STRINGS_BUILDER(sa), false};

    /* local@domain. With no @, which an addr-spec has to have, the whole
     * address is the local part. */
    Str local = addr->address, domain = {NULL, 0};
    Int at = strings_last_index(addr->address, BURROW_S("@"));
    if (at >= 0) {
        local = (Str){addr->address.p, at};
        domain = (Str){addr->address.p + at + 1, addr->address.len - at - 1};
    }

    /* Quotes when the local part needs them. A dot is fine between two atext
     * characters, so it is enough to check that the one before it is not a
     * dot and that it is not the last. */
    bool quote_local = false;
    for (Int i = 0; i < local.len;) {
        Int size;
        Rune r = ml_rune((Str){local.p + i, local.len - i}, &size);
        if (!ml_is_atext(r, false) &&
            !(r == '.' && i > 0 && local.p[i - 1] != '.' && i < local.len - 1)) {
            quote_local = true;
            break;
        }
        i += size;
    }

    if (addr->name.len > 0) {
        /* Printable ASCII is quoted. Anything else is an encoded word. */
        bool all_printable = true;
        for (Int i = 0; i < addr->name.len;) {
            Int size;
            Rune r = ml_rune((Str){addr->name.p + i, addr->name.len - i}, &size);
            /* This should be FWS rather than WSP, but folding is not done. */
            if ((!ml_is_vchar(r) && !ml_is_wsp(r)) || ml_is_multibyte(r)) {
                all_printable = false;
                break;
            }
            i += size;
        }
        if (all_printable) {
            ml_quote_string(&b, addr->name);
        } else {
            /* An encoded word in a display name cannot have quotes,
             * parentheses and the like in it (RFC 2047 section 5.3), and
             * base64 keeps them out. */
            MimeWordEncoder e = MIME_Q_ENCODING;
            if (strings_contains_any(addr->name, BURROW_S("\"#$%&'(),.:;<>@[]^`{|}~")))
                e = MIME_B_ENCODING;
            Str enc = mime_word_encoder_encode(e, sa, BURROW_S("utf-8"), addr->name);
            if (enc.p == NULL)
                b.oom = true;
            ml_put(&b, enc);
        }
        ml_putc(&b, ' ');
    }

    ml_putc(&b, '<');
    if (quote_local)
        ml_quote_string(&b, local);
    else
        ml_put(&b, local);
    ml_putc(&b, '@');
    ml_put(&b, domain);
    ml_putc(&b, '>');

    Str out = none;
    if (!b.oom)
        out = strings_clone(a, strings_builder_string(&b.b));
    arena_free(&scratch);
    return out;
}

/* ------------------------------------------------------------------- dates */

/* The layouts tried in order, from RFC 5322 section 3.3: an optional day of
 * the week, a day of one or two digits, a year of four or two, optional
 * seconds, and a zone as an offset, a name, or "UT". "-0700 (MST)" is not in
 * the RFC but is common, and the comment is taken off before these are
 * tried. */
#define ML_L(s) {(const Byte *)(s), (Int)sizeof(s) - 1}
#define ML_ZONES(pre) ML_L(pre " -0700"), ML_L(pre " MST"), ML_L(pre " UT")
#define ML_SECS(pre) ML_ZONES(pre ":05"), ML_ZONES(pre)
#define ML_YEARS(pre) ML_SECS(pre " 2006 15:04"), ML_SECS(pre " 06 15:04")
#define ML_DAYS(pre) ML_YEARS(pre "2 Jan"), ML_YEARS(pre "02 Jan")

static const Str ml_date_layouts[] = {ML_DAYS(""), ML_DAYS("Mon, ")};

Time mail_parse_date(Alloc *a, Str date, Error *err) {
    Time zero = {0};
    Arena scratch;
    arena_init(&scratch, NULL, 0);
    Alloc *sa = arena_allocator(&scratch);

    /* CR and LF have to come together, and are let through anywhere. */
    if (strings_contains(date, BURROW_S("\r\n"))) {
        date = strings_replace_all(sa, date, BURROW_S("\r\n"), BURROW_S(""));
        if (date.p == NULL) {
            arena_free(&scratch);
            *err = burrow_err_out_of_memory;
            return zero;
        }
    }
    if (strings_contains(date, BURROW_S("\r"))) {
        arena_free(&scratch);
        *err = ml_err_cr;
        return zero;
    }

    /* The parser's skipping, which lets through obsolete text such as
     * characters that do not print. */
    MlParser p = {date, NULL, sa};
    ml_skip_space(&p);

    /* zone = (FWS ( "+" / "-" ) 4DIGIT) / obs-zone, which is five characters
     * unless it is obsolete. */
    Int ind = strings_index_any(p.s, BURROW_S("+-"));
    if (ind != -1 && p.s.len >= ind + 5) {
        date = (Str){p.s.p, ind + 5};
        ml_advance(&p, ind + 5);
    } else {
        ind = strings_index(p.s, BURROW_S("T"));
        if (ind == 0) {
            /* "Thu, 20 Nov 1997 09:55:06 MDT", maybe with a comment after it,
             * where the zone's T is the second one. */
            ind = strings_index((Str){p.s.p + 1, p.s.len - 1}, BURROW_S("T"));
            if (ind != -1)
                ind++;
        }
        /* With no offset, the last letter of an obsolete zone is a T. When the
         * T is somewhere else the date is not going to parse anyway. */
        if (ind != -1 && p.s.len >= ind + 5) {
            date = (Str){p.s.p, ind + 1};
            ml_advance(&p, ind + 1);
        }
    }
    if (!ml_skip_cfws(&p)) {
        arena_free(&scratch);
        *err = ml_err_comment;
        return zero;
    }
    size_t n = sizeof ml_date_layouts / sizeof ml_date_layouts[0];
    for (size_t i = 0; i < n; i++) {
        ArenaMark m = error_mark();
        Error e = BURROW_NO_ERROR;
        Time t = time_parse(a, ml_date_layouts[i], date, &e);
        if (BURROW_OK(e)) {
            arena_free(&scratch);
            *err = BURROW_NO_ERROR;
            return t;
        }
        error_release(m);
    }
    arena_free(&scratch);
    *err = ml_err_bad_date;
    return zero;
}

/* ------------------------------------------------------------------ header */

Str mail_header_get(MailHeader h, Str key) {
    return textproto_mime_header_get(h, key);
}

Time mail_header_date(MailHeader h, Alloc *a, Error *err) {
    Str hdr = mail_header_get(h, BURROW_S("Date"));
    if (hdr.len == 0) {
        *err = mail_err_header_not_present;
        return (Time){0};
    }
    return mail_parse_date(a, hdr, err);
}

Slice mail_header_address_list(MailHeader h, Alloc *a, Str key, Error *err) {
    Str hdr = mail_header_get(h, key);
    if (hdr.len == 0) {
        *err = mail_err_header_not_present;
        return slice_from(NULL, 0, 0, TYPE_UNSAFE_POINTER);
    }
    return mail_parse_address_list(a, hdr, err);
}

/* ----------------------------------------------------------------- message */

/* textproto's ReadMIMEHeader without the checks RFC 7230 asks for, which RFC
 * 5322 does not, as Go copies it. The header and its strings come from ha. */
static Error ml_read_header(TextprotoReader *tp, BufioReader *br, Alloc *ha,
                            MailHeader h) {
    /* The first line cannot start with a space. */
    ArenaMark m = error_mark();
    Error err = BURROW_NO_ERROR;
    Slice buf = bufio_reader_peek(br, 1, &err);
    if (BURROW_OK(err)) {
        Byte c = ((const Byte *)buf.p)[0];
        if (c == ' ' || c == '\t') {
            Str line = textproto_reader_read_line(tp, ha, &err);
            if (BURROW_FAILED(err))
                return err;
            return fmt_errorf_v("malformed initial line: %q", line);
        }
    } else {
        error_release(m);
    }

    for (;;) {
        err = BURROW_NO_ERROR;
        Str kv = textproto_reader_read_continued_line(tp, ha, &err);
        if (kv.len == 0)
            return err;

        /* The key ends at the first colon. */
        Str v;
        bool found;
        Str k = strings_cut(kv, BURROW_S(":"), &v, &found);
        if (!found)
            return fmt_errorf_v("malformed header line: %q", kv);
        Str key = textproto_canonical_mime_header_key(ha, k);
        if (key.p == NULL && k.len > 0)
            return burrow_err_out_of_memory;

        /* An empty key is let through, as it always has been. */
        if (key.len == 0)
            continue;

        Str value = strings_trim_left(v, BURROW_S(" \t"));
        if (!textproto_mime_header_add(h, key, value))
            return burrow_err_out_of_memory;
        if (BURROW_FAILED(err))
            return err;
    }
}

MailMessage *mail_read_message(Alloc *a, IoReader r, Error *err) {
    *err = BURROW_NO_ERROR;
    MailMessage *m =
        (MailMessage *)mem_alloc(a, sizeof(MailMessage), _Alignof(MailMessage));
    if (m == NULL) {
        *err = burrow_err_out_of_memory;
        return NULL;
    }
    m->a = a;
    arena_init(&m->arena, a, 0);
    Alloc *ha = arena_allocator(&m->arena);
    m->br = bufio_new_reader(a, r);
    m->tp = m->br != NULL ? textproto_new_reader(a, m->br) : NULL;
    m->header = m->tp != NULL ? textproto_mime_header_make(ha) : NULL;
    if (m->header == NULL) {
        mail_message_free(m);
        *err = burrow_err_out_of_memory;
        return NULL;
    }
    Error e = ml_read_header(m->tp, m->br, ha, m->header);
    if (BURROW_FAILED(e) && (!errors_is(e, io_eof) || map_len(m->header) == 0)) {
        mail_message_free(m);
        *err = e;
        return NULL;
    }
    m->body = bufio_reader_as_io_reader(m->br);
    return m;
}

void mail_message_free(MailMessage *m) {
    if (m == NULL)
        return;
    textproto_reader_free(m->tp);
    bufio_reader_free(m->br);
    arena_free(&m->arena);
    mem_free(m->a, m, sizeof(MailMessage), _Alignof(MailMessage));
}

/* ------------------------------------------------------------------- types */

static Str ml_m_string(MailAddress *self) {
    return mail_address_string(self, error_allocator());
}

#define ML_SIG_STRING(IN, OUT) OUT(Str)

#define ML_ADDRESS_METHODS(M, T) M(T, String, ml_m_string, ML_SIG_STRING)

BURROW_METHODS_DEFINE(MailAddress, ML_ADDRESS_METHODS);

static const Type ml_address_desc = {
    {(const Byte *)"Address", 7},
    {(const Byte *)"net/mail", 8},
    KIND_STRUCT,
    (uint32_t)sizeof(MailAddress),
    (uint16_t)_Alignof(MailAddress),
    0,
    (uint16_t)(sizeof burrow__methods_MailAddress /
               sizeof burrow__methods_MailAddress[0]),
    NULL,
    burrow__methods_MailAddress,
    NULL,
    NULL,
    0,
    0x6d6c6164U, /* "mlad" */
    NULL,
};

const Type *const TYPE_MAIL_ADDRESS = &ml_address_desc;

static const Type ml_message_desc = {
    {(const Byte *)"Message", 7},
    {(const Byte *)"net/mail", 8},
    KIND_STRUCT,
    (uint32_t)sizeof(MailMessage),
    (uint16_t)_Alignof(MailMessage),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x6d6c6d73U, /* "mlms" */
    NULL,
};

const Type *const TYPE_MAIL_MESSAGE = &ml_message_desc;

static const Type ml_address_parser_desc = {
    {(const Byte *)"AddressParser", 13},
    {(const Byte *)"net/mail", 8},
    KIND_STRUCT,
    (uint32_t)sizeof(MailAddressParser),
    (uint16_t)_Alignof(MailAddressParser),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x6d6c6170U, /* "mlap" */
    NULL,
};

const Type *const TYPE_MAIL_ADDRESS_PARSER = &ml_address_parser_desc;

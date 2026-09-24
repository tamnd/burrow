/* Which tests to run: Go's src/testing/match.go, and a small regular
 * expression matcher to stand in for regexp until regexp is ported.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "match.h"

#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/strconv.h"
#include "burrow/sync.h"
#include "burrow/type.h"
#include "burrow/utf8.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ the regexp
 *
 * A backtracking matcher over a parsed tree, which is plenty for patterns
 * typed on a command line and matched against test names. It reads Go's
 * syntax and reports Go's errors for the parts it understands, and anything
 * else is an error rather than a guess, so a run is never quietly wider or
 * narrower than the pattern asked for.
 *
 * Supported: literals, the dot, bracketed classes with ranges, negation and
 * the ASCII names like [:alpha:], the escapes \d \D \w \W \s \S \b \B \A \z,
 * \Q...\E, escaped punctuation and the usual control escapes, ^ and $, groups
 * of every kind Go accepts, alternation, the repetitions * + ? {n} {n,}
 * {n,m} and their lazy forms, and the flags i, m, s and U. Case folding is
 * ASCII only until the unicode tables are ported. Unicode classes such as
 * \pL are not supported. */

enum {
    RE_EMPTY,
    RE_LIT,
    RE_ANY,    /* any rune but newline */
    RE_ANY_NL, /* any rune at all */
    RE_CLASS,
    RE_BOL,      /* ^ without m: start of text */
    RE_BOL_LINE, /* ^ with m */
    RE_EOL,      /* $ without m: end of text */
    RE_EOL_LINE, /* $ with m */
    RE_WORD_B,
    RE_NWORD_B,
    RE_CAT,
    RE_ALT,
    RE_REP
};

/* An allocation the runner cannot go on without. Most of the calls that need
 * memory have no Error to hand it back through, and Go's runtime ends a test
 * binary that runs out of memory, so this panics. */
static void *match_must_alloc(Alloc *a, size_t size, size_t align) {
    void *p = mem_alloc(a, size, align);
    if (p == NULL)
        panic_str(BURROW_S("testing: out of memory"));
    return p;
}

static void *match_must_realloc(Alloc *a, void *old, size_t old_size, size_t size,
                                size_t align) {
    void *p = mem_realloc(a, old, old_size, size, align);
    if (p == NULL)
        panic_str(BURROW_S("testing: out of memory"));
    return p;
}

typedef struct Re Re;
struct Re {
    int op;
    bool fold;
    bool neg;
    bool greedy;
    Rune r;
    int min;
    int max;      /* -1 for no limit */
    Rune *ranges; /* pairs, lo then hi */
    Int nranges;
    Re **sub;
    Int nsub;
};

enum {
    FLAG_FOLD = 1,
    FLAG_DOT_NL = 2,
    FLAG_MULTI = 4,
    FLAG_UNGREEDY = 8,
};

typedef struct Parser {
    Alloc *a;
    Str src;
    Int pos;
    int flags;
    int depth;
    const char *code; /* the error, when there is one */
    Str expr;
} Parser;

/* A growable array of whatever, in the parser's arena. */
typedef struct Vec {
    void *p;
    Int len;
    Int cap;
} Vec;

static void vec_push(Alloc *a, Vec *v, const void *elem, size_t size) {
    if (v->len == v->cap) {
        Int ncap = v->cap == 0 ? 4 : v->cap * 2;
        v->p =
            match_must_realloc(a, v->p, (size_t)v->cap * size, (size_t)ncap * size, 8);
        v->cap = ncap;
    }
    memcpy((Byte *)v->p + (size_t)v->len * size, elem, size);
    v->len++;
}

static Re *re_new(Parser *p, int op) {
    Re *re = (Re *)match_must_alloc(p->a, sizeof(Re), _Alignof(Re));
    re->op = op;
    return re;
}

static bool fail(Parser *p, const char *code, Str expr) {
    if (p->code == NULL) {
        p->code = code;
        p->expr = expr;
    }
    return false;
}

static Str tail(Parser *p, Int from) {
    return str_from_bytes(p->src.p + from, p->src.len - from);
}

static Str span(Parser *p, Int from, Int to) {
    return str_from_bytes(p->src.p + from, to - from);
}

static bool at_end(Parser *p) {
    return p->pos >= p->src.len;
}

static Byte peek(Parser *p) {
    return p->src.p[p->pos];
}

static bool next_rune(Parser *p, Rune *r) {
    Int size = 0;
    *r = utf8_decode_rune_in_string(tail(p, p->pos), &size);
    if (*r == UTF8_RUNE_ERROR && size == 1)
        return fail(p, "invalid UTF-8", tail(p, p->pos));
    p->pos += size;
    return true;
}

static void add_range(Parser *p, Vec *v, Rune lo, Rune hi) {
    Rune pair[2] = {lo, hi};
    vec_push(p->a, v, pair, sizeof pair);
}

/* The Perl classes, \d \s \w, as ranges. */
static void add_perl(Parser *p, Vec *v, Byte c) {
    switch (c) {
    case 'd':
        add_range(p, v, '0', '9');
        break;
    case 's':
        add_range(p, v, '\t', '\n');
        add_range(p, v, '\f', '\r');
        add_range(p, v, ' ', ' ');
        break;
    default: /* w */
        add_range(p, v, '0', '9');
        add_range(p, v, 'A', 'Z');
        add_range(p, v, '_', '_');
        add_range(p, v, 'a', 'z');
        break;
    }
}

/* The complement of a sorted set of ranges, for \D inside brackets and the
 * negated ASCII names. */
static void add_negated(Parser *p, Vec *v, const Rune *r, Int n) {
    Rune next = 0;
    for (Int i = 0; i < n; i++) {
        if (r[2 * i] > next)
            add_range(p, v, next, r[2 * i] - 1);
        next = r[2 * i + 1] + 1;
    }
    if (next <= 0x10FFFF)
        add_range(p, v, next, 0x10FFFF);
}

static void add_perl_class(Parser *p, Vec *v, Byte c) {
    if (c >= 'a') {
        add_perl(p, v, c);
        return;
    }
    Vec tmp = {0};
    add_perl(p, &tmp, (Byte)(c + ('a' - 'A')));
    add_negated(p, v, (const Rune *)tmp.p, tmp.len);
}

static const struct {
    const char *name;
    const char *ranges; /* pairs of bytes */
} ascii_classes[] = {
    {"alnum", "09AZaz"},
    {"alpha", "AZaz"},
    {"ascii", "\x01\x7f"},
    {"blank", "\t\t  "},
    {"cntrl", "\x01\x1f\x7f\x7f"},
    {"digit", "09"},
    {"graph", "!~"},
    {"lower", "az"},
    {"print", " ~"},
    {"punct", "!/:@[`{~"},
    {"space", "\t\r  "},
    {"upper", "AZ"},
    {"word", "09AZ__az"},
    {"xdigit", "09AFaf"},
};

/* [:alpha:] and its friends, with the bracket already seen. */
static bool parse_ascii_class(Parser *p, Vec *v) {
    Int start = p->pos;
    Int end = -1;
    for (Int i = start + 2; i + 1 < p->src.len; i++) {
        if (p->src.p[i] == ':' && p->src.p[i + 1] == ']') {
            end = i + 2;
            break;
        }
    }
    if (end < 0)
        return false;
    Str name = span(p, start + 2, end - 2);
    bool neg = false;
    if (name.len > 0 && name.p[0] == '^') {
        neg = true;
        name = str_from_bytes(name.p + 1, name.len - 1);
    }
    for (size_t i = 0; i < sizeof ascii_classes / sizeof ascii_classes[0]; i++) {
        if (!str_eq(name, str_from_cstr(ascii_classes[i].name)))
            continue;
        const char *r = ascii_classes[i].ranges;
        size_t n = strlen(r) / 2;
        if (strcmp(ascii_classes[i].name, "ascii") == 0 ||
            strcmp(ascii_classes[i].name, "cntrl") == 0) {
            /* These two start at NUL, which a C string cannot hold. */
            Rune pairs[4] = {0, 0x7f, 0, 0};
            n = 1;
            if (strcmp(ascii_classes[i].name, "cntrl") == 0) {
                pairs[1] = 0x1f;
                pairs[2] = 0x7f;
                pairs[3] = 0x7f;
                n = 2;
            }
            if (neg)
                add_negated(p, v, pairs, (Int)n);
            else
                for (size_t k = 0; k < n; k++)
                    add_range(p, v, pairs[2 * k], pairs[2 * k + 1]);
        } else {
            Rune pairs[16] = {0};
            for (size_t k = 0; k < 2 * n; k++)
                pairs[k] = (Rune)(Byte)r[k];
            if (neg)
                add_negated(p, v, pairs, (Int)n);
            else
                for (size_t k = 0; k < n; k++)
                    add_range(p, v, pairs[2 * k], pairs[2 * k + 1]);
        }
        p->pos = end;
        return true;
    }
    p->pos = end;
    return fail(p, "invalid character class range", span(p, start, end));
}

static int hex_digit(Byte c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* An escape that stands for one rune, with the backslash at p->pos. The
 * classes and assertions are the caller's business. */
static bool parse_escape_rune(Parser *p, Rune *out) {
    Int start = p->pos;
    p->pos++;
    if (at_end(p))
        return fail(p, "trailing backslash at end of expression", BURROW_S(""));
    Rune c;
    if (!next_rune(p, &c))
        return false;
    switch (c) {
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
        /* Go reads \1 as a backreference it does not support unless two more
         * octal digits follow. */
        if (p->pos + 1 >= p->src.len || p->src.p[p->pos] < '0' ||
            p->src.p[p->pos] > '7')
            break;
        /* fallthrough */
    case '0': {
        Rune r = c - '0';
        for (int i = 1; i < 3; i++) {
            if (at_end(p) || peek(p) < '0' || peek(p) > '7')
                break;
            r = r * 8 + (peek(p) - '0');
            p->pos++;
        }
        *out = r;
        return true;
    }
    case 'x': {
        if (at_end(p))
            break;
        if (peek(p) == '{') {
            p->pos++;
            Rune r = 0;
            int nd = 0;
            while (!at_end(p) && hex_digit(peek(p)) >= 0) {
                r = r * 16 + hex_digit(peek(p));
                p->pos++;
                nd++;
                if (r > 0x10FFFF)
                    break;
            }
            if (at_end(p) || peek(p) != '}' || nd == 0 || r > 0x10FFFF)
                break;
            p->pos++;
            *out = r;
            return true;
        }
        if (p->pos + 1 >= p->src.len + 0 || hex_digit(p->src.p[p->pos]) < 0 ||
            hex_digit(p->src.p[p->pos + 1]) < 0)
            break;
        *out = hex_digit(p->src.p[p->pos]) * 16 + hex_digit(p->src.p[p->pos + 1]);
        p->pos += 2;
        return true;
    }
    case 'a':
        *out = '\a';
        return true;
    case 'f':
        *out = '\f';
        return true;
    case 'n':
        *out = '\n';
        return true;
    case 'r':
        *out = '\r';
        return true;
    case 't':
        *out = '\t';
        return true;
    case 'v':
        *out = '\v';
        return true;
    default:
        if (c < 0x80 && !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                          (c >= 'A' && c <= 'Z'))) {
            *out = c;
            return true;
        }
        break;
    }
    if (c == 'p' || c == 'P')
        return fail(p, "unsupported by the built in matcher", span(p, start, p->pos));
    return fail(p, "invalid escape sequence", span(p, start, p->pos));
}

static bool is_perl_class(Parser *p) {
    if (p->pos + 1 >= p->src.len)
        return false;
    switch (p->src.p[p->pos + 1]) {
    case 'd':
    case 'D':
    case 's':
    case 'S':
    case 'w':
    case 'W':
        return true;
    default:
        return false;
    }
}

static Re *parse_class(Parser *p) {
    Int start = p->pos;
    p->pos++;
    Re *re = re_new(p, RE_CLASS);
    re->fold = (p->flags & FLAG_FOLD) != 0;
    if (!at_end(p) && peek(p) == '^') {
        re->neg = true;
        p->pos++;
    }
    Vec v = {0};
    bool first = true;
    for (;;) {
        if (at_end(p)) {
            fail(p, "missing closing ]", tail(p, start));
            return NULL;
        }
        if (peek(p) == ']' && !first)
            break;
        /* Go rejects a - that could start a range nobody finished, except
         * first and last, and so does this. */
        if (peek(p) == '-' && !first && p->pos + 1 < p->src.len &&
            p->src.p[p->pos + 1] != ']') {
            Int dash = p->pos;
            p->pos++;
            Rune ignored;
            if (!next_rune(p, &ignored))
                return NULL;
            fail(p, "invalid character class range", span(p, dash, p->pos));
            return NULL;
        }
        first = false;
        if (peek(p) == '[' && p->pos + 1 < p->src.len && p->src.p[p->pos + 1] == ':') {
            Int before = p->pos;
            if (parse_ascii_class(p, &v))
                continue;
            if (p->code != NULL)
                return NULL;
            p->pos = before;
        }
        if (peek(p) == '\\' && is_perl_class(p)) {
            add_perl_class(p, &v, p->src.p[p->pos + 1]);
            p->pos += 2;
            continue;
        }
        Int item = p->pos;
        Rune lo;
        if (peek(p) == '\\') {
            if (!parse_escape_rune(p, &lo))
                return NULL;
        } else if (!next_rune(p, &lo)) {
            return NULL;
        }
        Rune hi = lo;
        if (p->pos + 1 < p->src.len && peek(p) == '-' && p->src.p[p->pos + 1] != ']') {
            p->pos++;
            if (peek(p) == '\\') {
                if (!parse_escape_rune(p, &hi))
                    return NULL;
            } else if (!next_rune(p, &hi)) {
                return NULL;
            }
            if (hi < lo) {
                fail(p, "invalid character class range", span(p, item, p->pos));
                return NULL;
            }
        }
        add_range(p, &v, lo, hi);
    }
    p->pos++;
    re->ranges = (Rune *)v.p;
    re->nranges = v.len;
    return re;
}

static Re *lit(Parser *p, Rune r) {
    Re *re = re_new(p, RE_LIT);
    re->r = r;
    re->fold = (p->flags & FLAG_FOLD) != 0;
    return re;
}

static Re *perl_class(Parser *p, Byte c) {
    Re *re = re_new(p, RE_CLASS);
    Vec v = {0};
    add_perl(p, &v, (Byte)(c | 0x20));
    re->ranges = (Rune *)v.p;
    re->nranges = v.len;
    re->neg = c < 'a';
    return re;
}

static Re *parse_alt(Parser *p);

/* Reads the flags of (?flags) or (?flags:re), with p->pos just past the
 * question mark. Answers 1 for a bare flag group, 2 for one with a colon. */
static int parse_flags(Parser *p, Int start) {
    int flags = p->flags;
    bool neg = false;
    bool any = false;
    while (!at_end(p)) {
        Byte c = peek(p);
        p->pos++;
        int bit = 0;
        switch (c) {
        case 'i':
            bit = FLAG_FOLD;
            break;
        case 'm':
            bit = FLAG_MULTI;
            break;
        case 's':
            bit = FLAG_DOT_NL;
            break;
        case 'U':
            bit = FLAG_UNGREEDY;
            break;
        case '-':
            if (neg)
                goto bad;
            neg = true;
            any = false;
            continue;
        case ':':
        case ')':
            if (neg && !any)
                goto bad;
            p->flags = flags;
            return c == ':' ? 2 : 1;
        default:
            goto bad;
        }
        any = true;
        if (neg)
            flags &= ~bit;
        else
            flags |= bit;
    }
bad:
    fail(p, "invalid or unsupported Perl syntax", span(p, start, p->pos));
    return 0;
}

static Re *parse_atom(Parser *p) {
    Int start = p->pos;
    Byte c = peek(p);
    switch (c) {
    case '(': {
        p->pos++;
        int saved = p->flags;
        if (!at_end(p) && peek(p) == '?') {
            p->pos++;
            if (!at_end(p) && (peek(p) == 'P' || peek(p) == '<')) {
                /* A named group, which matches the same as a plain one. */
                if (peek(p) == 'P')
                    p->pos++;
                if (at_end(p) || peek(p) != '<') {
                    fail(p, "invalid or unsupported Perl syntax",
                         span(p, start, p->pos));
                    return NULL;
                }
                Int name = p->pos;
                while (!at_end(p) && peek(p) != '>')
                    p->pos++;
                if (at_end(p) || p->pos == name + 1) {
                    fail(p, "invalid named capture", tail(p, start));
                    return NULL;
                }
                p->pos++;
            } else {
                int kind = parse_flags(p, start);
                if (kind == 0)
                    return NULL;
                if (kind == 1)
                    /* (?i) changes the flags for the rest of the enclosing
                     * group and matches nothing itself. */
                    return re_new(p, RE_EMPTY);
            }
        }
        p->depth++;
        if (p->depth > 1000) {
            fail(p, "expression nests too deeply", p->src);
            return NULL;
        }
        Re *re = parse_alt(p);
        if (re == NULL)
            return NULL;
        if (at_end(p) || peek(p) != ')') {
            fail(p, "missing closing )", p->src);
            return NULL;
        }
        p->pos++;
        p->depth--;
        p->flags = saved;
        return re;
    }
    case '[':
        return parse_class(p);
    case '.': {
        p->pos++;
        return re_new(p, (p->flags & FLAG_DOT_NL) ? RE_ANY_NL : RE_ANY);
    }
    case '^':
        p->pos++;
        return re_new(p, (p->flags & FLAG_MULTI) ? RE_BOL_LINE : RE_BOL);
    case '$':
        p->pos++;
        return re_new(p, (p->flags & FLAG_MULTI) ? RE_EOL_LINE : RE_EOL);
    case '\\': {
        if (p->pos + 1 < p->src.len) {
            Byte e = p->src.p[p->pos + 1];
            switch (e) {
            case 'd':
            case 'D':
            case 's':
            case 'S':
            case 'w':
            case 'W':
                p->pos += 2;
                return perl_class(p, e);
            case 'A':
                p->pos += 2;
                return re_new(p, RE_BOL);
            case 'z':
                p->pos += 2;
                return re_new(p, RE_EOL);
            case 'b':
                p->pos += 2;
                return re_new(p, RE_WORD_B);
            case 'B':
                p->pos += 2;
                return re_new(p, RE_NWORD_B);
            case 'Q': {
                /* Literal text up to \E or the end. */
                p->pos += 2;
                Re *cat = re_new(p, RE_CAT);
                Vec v = {0};
                while (!at_end(p)) {
                    if (peek(p) == '\\' && p->pos + 1 < p->src.len &&
                        p->src.p[p->pos + 1] == 'E') {
                        p->pos += 2;
                        break;
                    }
                    Rune r = 0;
                    if (!next_rune(p, &r))
                        return NULL;
                    Re *l = lit(p, r);
                    vec_push(p->a, &v, (const void *)&l, sizeof(Re *));
                }
                cat->sub = (Re **)v.p;
                cat->nsub = v.len;
                return cat;
            }
            default:
                break;
            }
        }
        Rune r = 0;
        if (!parse_escape_rune(p, &r))
            return NULL;
        return lit(p, r);
    }
    default: {
        Rune r = 0;
        if (!next_rune(p, &r))
            return NULL;
        return lit(p, r);
    }
    }
}

/* A {n}, {n,} or {n,m} at p->pos. Answers false, and leaves pos alone, when
 * what is there is not one, which makes the brace a literal the way Go does. */
static bool parse_count(Parser *p, int *min, int *max) {
    Int i = p->pos + 1;
    Int n = p->src.len;
    const Byte *s = p->src.p;
    int64_t lo = 0;
    int64_t hi;
    Int digits = 0;
    while (i < n && s[i] >= '0' && s[i] <= '9') {
        if (lo < 100000)
            lo = lo * 10 + (s[i] - '0');
        i++;
        digits++;
    }
    if (digits == 0 || i >= n)
        return false;
    if (s[i] == '}') {
        hi = lo;
    } else if (s[i] == ',') {
        i++;
        if (i >= n)
            return false;
        if (s[i] == '}') {
            hi = -1;
        } else {
            hi = 0;
            digits = 0;
            while (i < n && s[i] >= '0' && s[i] <= '9') {
                if (hi < 100000)
                    hi = hi * 10 + (s[i] - '0');
                i++;
                digits++;
            }
            if (digits == 0 || i >= n || s[i] != '}')
                return false;
        }
    } else {
        return false;
    }
    Int start = p->pos;
    p->pos = i + 1;
    if (lo > 1000 || hi > 1000 || (hi >= 0 && hi < lo))
        return fail(p, "invalid repeat count", span(p, start, p->pos));
    *min = (int)lo;
    *max = (int)hi;
    return true;
}

static bool is_repeat(Parser *p) {
    if (at_end(p))
        return false;
    Byte c = peek(p);
    if (c == '*' || c == '+' || c == '?')
        return true;
    if (c != '{')
        return false;
    Int saved = p->pos;
    int mn;
    int mx;
    bool ok = parse_count(p, &mn, &mx);
    p->pos = saved;
    return ok || p->code != NULL;
}

/* Reads one repetition operator at p->pos, lazy mark included. */
static bool parse_repeat(Parser *p, int *min, int *max, bool *greedy) {
    Byte c = peek(p);
    if (c == '{') {
        if (!parse_count(p, min, max))
            return false;
    } else {
        p->pos++;
        *min = c == '+' ? 1 : 0;
        *max = c == '?' ? 1 : -1;
    }
    *greedy = true;
    if (!at_end(p) && peek(p) == '?') {
        p->pos++;
        *greedy = false;
    }
    if (p->flags & FLAG_UNGREEDY)
        *greedy = !*greedy;
    return true;
}

static Re *parse_cat(Parser *p) {
    Vec v = {0};
    while (!at_end(p) && peek(p) != '|' && peek(p) != ')') {
        Int op_start = p->pos;
        if (is_repeat(p)) {
            if (p->code != NULL)
                return NULL;
            int mn;
            int mx;
            bool greedy;
            parse_repeat(p, &mn, &mx, &greedy);
            if (p->code != NULL)
                return NULL;
            fail(p, "missing argument to repetition operator",
                 span(p, op_start, p->pos));
            return NULL;
        }
        Re *atom = parse_atom(p);
        if (atom == NULL)
            return NULL;
        Int last_op = -1;
        while (is_repeat(p)) {
            if (p->code != NULL)
                return NULL;
            Int this_op = p->pos;
            int mn;
            int mx;
            bool greedy;
            if (!parse_repeat(p, &mn, &mx, &greedy))
                return NULL;
            if (last_op >= 0) {
                fail(p, "invalid nested repetition operator", span(p, last_op, p->pos));
                return NULL;
            }
            last_op = this_op;
            Re *rep = re_new(p, RE_REP);
            rep->min = mn;
            rep->max = mx;
            rep->greedy = greedy;
            rep->sub = (Re **)match_must_alloc(p->a, sizeof(Re *), _Alignof(Re *));
            rep->sub[0] = atom;
            rep->nsub = 1;
            atom = rep;
        }
        vec_push(p->a, &v, (const void *)&atom, sizeof(Re *));
    }
    if (v.len == 1)
        return ((Re **)v.p)[0];
    Re *cat = re_new(p, v.len == 0 ? RE_EMPTY : RE_CAT);
    cat->sub = (Re **)v.p;
    cat->nsub = v.len;
    return cat;
}

static Re *parse_alt(Parser *p) {
    Vec v = {0};
    for (;;) {
        Re *re = parse_cat(p);
        if (re == NULL)
            return NULL;
        vec_push(p->a, &v, (const void *)&re, sizeof(Re *));
        if (at_end(p) || peek(p) != '|')
            break;
        p->pos++;
    }
    if (v.len == 1)
        return ((Re **)v.p)[0];
    Re *alt = re_new(p, RE_ALT);
    alt->sub = (Re **)v.p;
    alt->nsub = v.len;
    return alt;
}

static Re *parse(Parser *p) {
    Re *re = parse_alt(p);
    if (re == NULL)
        return NULL;
    if (!at_end(p)) {
        /* parse_alt only stops early at a ) nobody opened. */
        fail(p, "unexpected )", p->src);
        return NULL;
    }
    return re;
}

/* ------------------------------------------------------------ matching */

typedef struct Matching {
    Str s;
} Matching;

enum { K_CAT, K_REP };

/* What to match after the node being matched now: the rest of a
 * concatenation, or another go round a repetition. */
typedef struct Kont {
    int kind;
    const Re *re;
    Int index; /* K_CAT: the next child. K_REP: iterations so far */
    Int start; /* K_REP: where the iteration that just ended began */
    const struct Kont *next;
} Kont;

static bool re_run(const Matching *m, const Re *re, Int pos, const Kont *k);

static bool rep(const Matching *m, const Re *re, Int count, Int last, Int pos,
                const Kont *k);

static bool cont(const Matching *m, Int pos, const Kont *k) {
    if (k == NULL)
        return true;
    if (k->kind == K_CAT) {
        if (k->index == k->re->nsub)
            return cont(m, pos, k->next);
        Kont nk = {K_CAT, k->re, k->index + 1, 0, k->next};
        return re_run(m, k->re->sub[k->index], pos, &nk);
    }
    return rep(m, k->re, k->index, k->start, pos, k->next);
}

static bool rep(const Matching *m, const Re *re, Int count, Int last, Int pos,
                const Kont *k) {
    bool can_stop = count >= re->min;
    /* An iteration that matched nothing will match nothing again, so going
     * round once more cannot help and would never end. */
    if (count > 0 && pos == last && can_stop)
        return cont(m, pos, k);
    bool can_more = re->max < 0 || count < re->max;
    Kont more = {K_REP, re, count + 1, pos, k};
    if (re->greedy) {
        if (can_more && re_run(m, re->sub[0], pos, &more))
            return true;
        return can_stop && cont(m, pos, k);
    }
    if (can_stop && cont(m, pos, k))
        return true;
    return can_more && re_run(m, re->sub[0], pos, &more);
}

static Rune fold_ascii(Rune r) {
    if (r >= 'A' && r <= 'Z')
        return r + ('a' - 'A');
    return r;
}

static bool in_class(const Re *re, Rune r) {
    for (Int i = 0; i < re->nranges; i++) {
        if (r >= re->ranges[2 * i] && r <= re->ranges[2 * i + 1])
            return true;
    }
    if (re->fold) {
        Rune other = r;
        if (r >= 'a' && r <= 'z')
            other = r - ('a' - 'A');
        else if (r >= 'A' && r <= 'Z')
            other = r + ('a' - 'A');
        if (other != r) {
            for (Int i = 0; i < re->nranges; i++) {
                if (other >= re->ranges[2 * i] && other <= re->ranges[2 * i + 1])
                    return true;
            }
        }
    }
    return false;
}

static bool is_word(Byte c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           c == '_';
}

static bool at_word_boundary(const Matching *m, Int pos) {
    bool before = pos > 0 && is_word(m->s.p[pos - 1]);
    bool after = pos < m->s.len && is_word(m->s.p[pos]);
    return before != after;
}

static bool re_run(const Matching *m, const Re *re, Int pos, const Kont *k) {
    Str s = m->s;
    switch (re->op) {
    case RE_EMPTY:
        return cont(m, pos, k);
    case RE_LIT:
    case RE_ANY:
    case RE_ANY_NL:
    case RE_CLASS: {
        if (pos >= s.len)
            return false;
        Int size = 0;
        Rune r =
            utf8_decode_rune_in_string(str_from_bytes(s.p + pos, s.len - pos), &size);
        bool ok;
        if (re->op == RE_LIT)
            ok = re->fold ? fold_ascii(r) == fold_ascii(re->r) : r == re->r;
        else if (re->op == RE_ANY)
            ok = r != '\n';
        else if (re->op == RE_ANY_NL)
            ok = true;
        else
            ok = in_class(re, r) != re->neg;
        return ok && cont(m, pos + size, k);
    }
    case RE_BOL:
        return pos == 0 && cont(m, pos, k);
    case RE_BOL_LINE:
        return (pos == 0 || s.p[pos - 1] == '\n') && cont(m, pos, k);
    case RE_EOL:
        return pos == s.len && cont(m, pos, k);
    case RE_EOL_LINE:
        return (pos == s.len || s.p[pos] == '\n') && cont(m, pos, k);
    case RE_WORD_B:
        return at_word_boundary(m, pos) && cont(m, pos, k);
    case RE_NWORD_B:
        return !at_word_boundary(m, pos) && cont(m, pos, k);
    case RE_CAT: {
        Kont nk = {K_CAT, re, 0, 0, k};
        return cont(m, pos, &nk);
    }
    case RE_ALT:
        for (Int i = 0; i < re->nsub; i++) {
            if (re_run(m, re->sub[i], pos, k))
                return true;
        }
        return false;
    default: /* RE_REP */
        return rep(m, re, 0, -1, pos, k);
    }
}

bool testing_match_string(Str pat, Str str, Error *err) {
    if (err != NULL)
        *err = (Error){0};
    Arena ar;
    arena_init(&ar, NULL, 0);
    Parser p = {0};
    p.a = arena_allocator(&ar);
    p.src = pat;
    Re *re = parse(&p);
    if (re == NULL) {
        if (err != NULL) {
            Alloc *ea = error_allocator();
            Str msg =
                fmt_sprintf_v(ea, "error parsing regexp: %s: `%s`", p.code, p.expr);
            *err = errors_new(ea, msg);
        }
        arena_free(&ar);
        return false;
    }
    Matching m = {str};
    bool found = false;
    for (Int i = 0; i <= str.len && !found;) {
        found = re_run(&m, re, i, NULL);
        if (i == str.len)
            break;
        Int size = 0;
        utf8_decode_rune_in_string(str_from_bytes(str.p + i, str.len - i), &size);
        i += size;
    }
    arena_free(&ar);
    return found;
}

bool burrow__testing_call_match(TestingMatchString match, Str pat, Str str,
                                Error *err) {
    if (BURROW_FUNC_IS_NIL(match))
        return testing_match_string(pat, str, err);
    return BURROW_CALLF(match, pat, str, err);
}

/* ------------------------------------------------------------ the matcher */

static bool is_space(Rune r) {
    if (r < 0x2000) {
        switch (r) {
        case '\t':
        case '\n':
        case '\v':
        case '\f':
        case '\r':
        case ' ':
        case 0x85:
        case 0xA0:
        case 0x1680:
            return true;
        default:
            return false;
        }
    }
    if (r <= 0x200a)
        return true;
    switch (r) {
    case 0x2028:
    case 0x2029:
    case 0x202f:
    case 0x205f:
    case 0x3000:
        return true;
    default:
        return false;
    }
}

/* Go's rewrite: spaces become underscores and what cannot be printed is
 * written as its escape, so that a name can be typed back on a command line. */
static Str rewrite(Alloc *a, Str s) {
    Slice b = slice_nil(TYPE_BYTE);
    for (Int i = 0; i < s.len;) {
        Int size = 0;
        Rune r = utf8_decode_rune_in_string(str_from_bytes(s.p + i, s.len - i), &size);
        if (is_space(r)) {
            Byte u = '_';
            b = slice_append(a, b, &u, 1);
        } else if (!strconv_is_print(r)) {
            Str q = strconv_quote_rune(a, r);
            b = slice_append(a, b, q.p + 1, q.len - 2);
        } else {
            /* The bytes as they were, which for an invalid sequence is not
             * what Go writes: Go appends string(r), which is U+FFFD. */
            if (r == UTF8_RUNE_ERROR && size == 1) {
                static const Byte fffd[] = {0xEF, 0xBF, 0xBD};
                b = slice_append(a, b, fffd, 3);
            } else {
                b = slice_append(a, b, s.p + i, size);
            }
        }
        i += size;
    }
    return str_from_bytes(b.p, b.len);
}

typedef struct StrVec {
    Str *p;
    Int len;
    Int cap;
} StrVec;

static void strvec_push(Alloc *a, StrVec *v, Str s) {
    vec_push(a, (Vec *)v, &s, sizeof s);
}

/* Go's splitRegexp. */
static burrow__TestingFilter split_regexp(Alloc *a, Str s) {
    StrVec cur = {0};
    Vec alts = {0};
    int cs = 0;
    int cp = 0;
    for (Int i = 0; i < s.len;) {
        switch (s.p[i]) {
        case '[':
            cs++;
            break;
        case ']':
            if (--cs < 0)
                cs = 0;
            break;
        case '(':
            if (cs == 0)
                cp++;
            break;
        case ')':
            if (cs == 0)
                cp--;
            break;
        case '\\':
            i++;
            break;
        case '/':
            if (cs == 0 && cp == 0) {
                strvec_push(a, &cur, str_from_bytes(s.p, i));
                s = str_from_bytes(s.p + i + 1, s.len - i - 1);
                i = 0;
                continue;
            }
            break;
        case '|':
            if (cs == 0 && cp == 0) {
                strvec_push(a, &cur, str_from_bytes(s.p, i));
                s = str_from_bytes(s.p + i + 1, s.len - i - 1);
                i = 0;
                burrow__TestingSimple sm = {cur.p, cur.len};
                vec_push(a, &alts, &sm, sizeof sm);
                cur = (StrVec){0};
                continue;
            }
            break;
        default:
            break;
        }
        i++;
    }
    strvec_push(a, &cur, s);
    burrow__TestingSimple last = {cur.p, cur.len};
    if (alts.len == 0) {
        burrow__TestingSimple *one = (burrow__TestingSimple *)match_must_alloc(
            a, sizeof *one, _Alignof(burrow__TestingSimple));
        *one = last;
        return (burrow__TestingFilter){false, one, 1};
    }
    vec_push(a, &alts, &last, sizeof last);
    return (burrow__TestingFilter){true, (burrow__TestingSimple *)alts.p, alts.len};
}

/* Go's verify, which rewrites each element the way names are rewritten and
 * then compiles it. Answers the message after "testing: invalid regexp for ",
 * or an empty string. */
static Str verify(Alloc *a, burrow__TestingFilter *f, Str name,
                  TestingMatchString match) {
    for (Int k = 0; k < f->n; k++) {
        burrow__TestingSimple *sm = &f->alt[k];
        for (Int i = 0; i < sm->n; i++)
            sm->elem[i] = rewrite(a, sm->elem[i]);
        for (Int i = 0; i < sm->n; i++) {
            Error err = {0};
            burrow__testing_call_match(match, sm->elem[i], BURROW_S("non-empty"), &err);
            if (err.vt != NULL) {
                Str msg = fmt_sprintf_v(a, "element %d of %s (%q): %s", i, name,
                                        sm->elem[i], error_text(err));
                if (f->alternation)
                    msg = fmt_sprintf_v(a, "alternation %d of %s", k, msg);
                return msg;
            }
        }
    }
    return BURROW_S("");
}

burrow__TestingMatcher *burrow__testing_matcher_new(TestingMatchString match,
                                                    Str patterns, Str name, Str skips) {
    Alloc *heap = heap_allocator();
    burrow__TestingMatcher *m = (burrow__TestingMatcher *)match_must_alloc(
        heap, sizeof *m, _Alignof(burrow__TestingMatcher));
    arena_init(&m->arena, NULL, 0);
    Alloc *a = arena_allocator(&m->arena);
    m->match = match;

    if (patterns.len == 0) {
        m->filter.alt = (burrow__TestingSimple *)match_must_alloc(
            a, sizeof(burrow__TestingSimple), _Alignof(burrow__TestingSimple));
        m->filter.n = 1;
    } else {
        m->filter = split_regexp(a, patterns);
        Str msg = verify(a, &m->filter, name, match);
        if (msg.len > 0) {
            fprintf(stderr, "testing: invalid regexp for %.*s\n", (int)msg.len,
                    (const char *)msg.p);
            exit(1);
        }
    }
    if (skips.len == 0) {
        m->skip.alternation = true;
    } else {
        m->skip = split_regexp(a, skips);
        Str msg = verify(a, &m->skip, BURROW_S("-test.skip"), match);
        if (msg.len > 0) {
            fprintf(stderr, "testing: invalid regexp for %.*s\n", (int)msg.len,
                    (const char *)msg.p);
            exit(1);
        }
    }
    arena_init(&m->names, NULL, 0);
    m->sub_names = map_make(arena_allocator(&m->names), TYPE_STRING, TYPE_INT32, 0);
    return m;
}

void burrow__testing_clear_sub_names(burrow__TestingMatcher *m) {
    sync_mutex_lock(&m->mu);
    map_free(m->sub_names);
    arena_free(&m->names);
    arena_init(&m->names, NULL, 0);
    m->sub_names = map_make(arena_allocator(&m->names), TYPE_STRING, TYPE_INT32, 0);
    sync_mutex_unlock(&m->mu);
}

void burrow__testing_matcher_free(burrow__TestingMatcher *m) {
    if (m == NULL)
        return;
    map_free(m->sub_names);
    arena_free(&m->names);
    arena_free(&m->arena);
    mem_free(heap_allocator(), m, sizeof *m, _Alignof(burrow__TestingMatcher));
}

static void simple_matches(const burrow__TestingSimple *sm, const Str *name, Int n,
                           TestingMatchString match, bool *ok, bool *partial) {
    for (Int i = 0; i < n; i++) {
        if (i >= sm->n)
            break;
        Error err = {0};
        if (!burrow__testing_call_match(match, sm->elem[i], name[i], &err)) {
            *ok = false;
            *partial = false;
            return;
        }
    }
    *ok = true;
    *partial = n < sm->n;
}

static void filter_matches(const burrow__TestingFilter *f, const Str *name, Int n,
                           TestingMatchString match, bool *ok, bool *partial) {
    if (!f->alternation) {
        simple_matches(&f->alt[0], name, n, match, ok, partial);
        return;
    }
    for (Int k = 0; k < f->n; k++) {
        simple_matches(&f->alt[k], name, n, match, ok, partial);
        if (*ok)
            return;
    }
    *ok = false;
    *partial = false;
}

static int32_t sub_count(burrow__TestingMatcher *m, Str key) {
    const int32_t *v = (const int32_t *)map_get(m->sub_names, &key);
    return v == NULL ? 0 : *v;
}

/* Go's parseSubtestNumber. */
static Str parse_subtest_number(Str s, int32_t *nn) {
    *nn = 0;
    Int i = s.len - 1;
    while (i >= 0 && s.p[i] != '#')
        i--;
    if (i < 0)
        return s;
    Str prefix = str_from_bytes(s.p, i);
    Str suffix = str_from_bytes(s.p + i + 1, s.len - i - 1);
    if (suffix.len < 2 || (suffix.len > 2 && suffix.p[0] == '0'))
        return s;
    if (str_eq(suffix, BURROW_S("00"))) {
        if (prefix.len == 0 || prefix.p[prefix.len - 1] != '/')
            return s;
    }
    Error err = {0};
    int64_t n = strconv_parse_int(suffix, 10, 32, &err);
    if (err.vt != NULL || n < 0)
        return s;
    *nn = (int32_t)n;
    return prefix;
}

/* Go's unique, which holds m->mu. */
static Str unique(burrow__TestingMatcher *m, Str parent, Str subname) {
    Alloc *a = arena_allocator(&m->names);
    Str base = fmt_sprintf_v(a, "%s/%s", parent, subname);
    for (;;) {
        int32_t n = sub_count(m, base);
        if (n < 0)
            panic_str(BURROW_S("subtest count overflow"));
        int32_t next = n + 1;
        map_set(m->sub_names, &base, &next);

        if (n == 0 && subname.len > 0) {
            int32_t nn;
            Str prefix = parse_subtest_number(base, &nn);
            if (prefix.len < base.len && nn < sub_count(m, prefix))
                continue;
            return base;
        }

        Str name = fmt_sprintf_v(a, "%s#%02d", base, n);
        if (sub_count(m, name) != 0)
            continue;
        return name;
    }
}

Str burrow__testing_full_name(burrow__TestingMatcher *m, const Str *parent, Str subname,
                              bool *ok, bool *partial) {
    Alloc *heap = heap_allocator();
    Str name;

    sync_mutex_lock(&m->mu);
    if (parent != NULL) {
        Arena scratch;
        arena_init(&scratch, NULL, 0);
        Str rewritten = rewrite(arena_allocator(&scratch), subname);
        name = str_clone(heap, unique(m, *parent, rewritten));
        arena_free(&scratch);
    } else {
        name = str_clone(heap, subname);
    }

    sync_mutex_lock(&m->match_mu);

    /* The whole path each time, since a pattern may itself contain a slash. */
    Arena scratch;
    arena_init(&scratch, NULL, 0);
    StrVec elem = {0};
    Int from = 0;
    for (Int i = 0; i <= name.len; i++) {
        if (i == name.len || name.p[i] == '/') {
            strvec_push(arena_allocator(&scratch), &elem,
                        str_from_bytes(name.p + from, i - from));
            from = i + 1;
        }
    }

    filter_matches(&m->filter, elem.p, elem.len, m->match, ok, partial);
    if (*ok) {
        bool skip;
        bool partial_skip;
        filter_matches(&m->skip, elem.p, elem.len, m->match, &skip, &partial_skip);
        if (skip && !partial_skip) {
            *ok = false;
            *partial = false;
        }
    } else {
        *partial = false;
    }
    arena_free(&scratch);

    sync_mutex_unlock(&m->match_mu);
    sync_mutex_unlock(&m->mu);
    return name;
}

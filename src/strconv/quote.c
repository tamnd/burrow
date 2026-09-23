/* Derived from Go's src/strconv/quote.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/strconv.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/type.h"
#include "burrow/utf8.h"

#include <string.h>

/* The port follows Go function by function. What changes is where the bytes go.
 * Go appends to a []byte and lets the collector sort out the size. Here a Str
 * result is exactly as long as its allocation, so that it can be handed back to
 * a heap allocator with its own length. Each quote and unquote writes into a
 * buffer on the stack while the bytes fit and counts them either way. A result
 * that fit is copied out, and a longer one is written a second time straight
 * into memory of the size the first run measured. Both runs are the same code,
 * so they cannot disagree. */

BURROW_SENTINEL_ERROR(strconv_err_range, "value out of range");
BURROW_SENTINEL_ERROR(strconv_err_syntax, "invalid syntax");

static const char lowerhex[] = "0123456789abcdef";

/* Where output goes. Bytes are stored while they fit in cap and counted either
 * way, so cap 0 only counts. */
typedef struct QuoteOut {
    Byte *p;
    Int n;
    Int cap;
} QuoteOut;

/* The size of the buffer on the stack, which holds most results whole. */
#define QUOTE_STACK 256

static void put(QuoteOut *o, const void *b, Int k) {
    if (k > 0 && k <= o->cap - o->n)
        memcpy(o->p + o->n, b, (size_t)k);
    o->n += k;
}

static void put_byte(QuoteOut *o, Byte b) {
    if (o->n < o->cap)
        o->p[o->n] = b;
    o->n++;
}

/* Two bytes, which is every escape but the long ones. */
static void put2(QuoteOut *o, Byte b0, Byte b1) {
    if (o->cap - o->n >= 2) {
        o->p[o->n] = b0;
        o->p[o->n + 1] = b1;
    }
    o->n += 2;
}

static void put_rune(QuoteOut *o, Rune r) {
    if (r >= 0 && r < UTF8_RUNE_SELF) {
        put_byte(o, (Byte)r);
        return;
    }
    Byte buf[UTF8_UTF_MAX];
    Int k = utf8_encode_rune(
        slice_from(buf, (Int)sizeof buf, (Int)sizeof buf, TYPE_BYTE), r);

    put(o, buf, k);
}

static Int index_byte(Str s, Byte c) {
    const void *p = s.len > 0 ? memchr(s.p, c, (size_t)s.len) : NULL;

    return p == NULL ? -1 : (Int)((const Byte *)p - s.p);
}

static bool contains_byte(Str s, Byte c) {
    return index_byte(s, c) >= 0;
}

/* -------------------------------------------------------------------- quoting */

static void escaped_rune(QuoteOut *o, Rune r, Byte quote, bool ascii_only,
                         bool graphic_only) {
    if (r == (Rune)quote || r == '\\') { /* always backslashed */
        put_byte(o, '\\');
        put_byte(o, (Byte)r);
        return;
    }
    if (ascii_only) {
        if (r < UTF8_RUNE_SELF && strconv_is_print(r)) {
            put_byte(o, (Byte)r);
            return;
        }
    } else if (graphic_only ? strconv_is_graphic(r) : strconv_is_print(r)) {
        put_rune(o, r);
        return;
    }

    switch (r) {
    case '\a':
        put2(o, '\\', 'a');
        return;
    case '\b':
        put2(o, '\\', 'b');
        return;
    case '\f':
        put2(o, '\\', 'f');
        return;
    case '\n':
        put2(o, '\\', 'n');
        return;
    case '\r':
        put2(o, '\\', 'r');
        return;
    case '\t':
        put2(o, '\\', 't');
        return;
    case '\v':
        put2(o, '\\', 'v');
        return;
    default:
        break;
    }

    if (r < ' ' || r == 0x7f) {
        put2(o, '\\', 'x');
        put_byte(o, (Byte)lowerhex[(Byte)r >> 4]);
        put_byte(o, (Byte)lowerhex[(Byte)r & 0xF]);
        return;
    }
    if (!utf8_valid_rune(r))
        r = 0xFFFD;
    if (r < 0x10000) {
        put2(o, '\\', 'u');
        for (int s = 12; s >= 0; s -= 4)
            put_byte(o, (Byte)lowerhex[(r >> s) & 0xF]);
        return;
    }
    put2(o, '\\', 'U');
    for (int s = 28; s >= 0; s -= 4)
        put_byte(o, (Byte)lowerhex[(r >> s) & 0xF]);
}

static void quoted(QuoteOut *o, Str s, Byte quote, bool ascii_only, bool graphic_only) {
    put_byte(o, quote);
    for (Int i = 0; i < s.len;) {
        Int width = 1;
        Rune r = s.p[i];
        if (r >= UTF8_RUNE_SELF)
            r = utf8_decode_rune_in_string(str_from_bytes(s.p + i, s.len - i), &width);
        if (width == 1 && r == UTF8_RUNE_ERROR) {
            /* A byte that is not part of any rune. It goes out as the byte it
             * was, so that unquoting the result gives back the same string. */
            put2(o, '\\', 'x');
            put_byte(o, (Byte)lowerhex[s.p[i] >> 4]);
            put_byte(o, (Byte)lowerhex[s.p[i] & 0xF]);
            i++;
            continue;
        }
        escaped_rune(o, r, quote, ascii_only, graphic_only);
        i += width;
    }
    put_byte(o, quote);
}

static void quoted_rune(QuoteOut *o, Rune r, Byte quote, bool ascii_only,
                        bool graphic_only) {
    put_byte(o, quote);
    if (!utf8_valid_rune(r))
        r = UTF8_RUNE_ERROR;
    escaped_rune(o, r, quote, ascii_only, graphic_only);
    put_byte(o, quote);
}

/* One quoting call, so that the counting run and the writing run are the same
 * call made twice. */
typedef struct QuoteJob {
    Str s;
    Rune r;
    bool is_rune;
    Byte quote;
    bool ascii_only;
    bool graphic_only;
} QuoteJob;

static void run(QuoteOut *o, const QuoteJob *j) {
    if (j->is_rune)
        quoted_rune(o, j->r, j->quote, j->ascii_only, j->graphic_only);
    else
        quoted(o, j->s, j->quote, j->ascii_only, j->graphic_only);
}

static Str quote_str(Alloc *a, QuoteJob j) {
    Byte tmp[QUOTE_STACK];
    QuoteOut o = {tmp, 0, QUOTE_STACK};
    run(&o, &j);

    /* Never empty, since the quotes are always there. */
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)o.n, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;

    if (o.n <= QUOTE_STACK) {
        memcpy(p, tmp, (size_t)o.n);
    } else {
        QuoteOut w = {p, 0, o.n};
        run(&w, &j);
    }
    return str_from_bytes(p, o.n);
}

static Slice quote_append(Alloc *a, Slice dst, QuoteJob j) {
    /* A zero Slice has no element type, and appending to it would do nothing.
     * Go's nil []byte is what the caller meant. */
    if (dst.elem == NULL)
        dst = slice_nil(TYPE_BYTE);

    /* Straight into the room dst already has when the quotes and the text
     * alone would fit there, which they usually do, and into the stack when
     * they would not. Go writes into that room too, before it knows whether
     * the escapes will overflow it. */
    Int spare = dst.cap - dst.len;
    Int least = j.is_rune ? 3 : j.s.len + 2;
    Byte tmp[QUOTE_STACK];
    Byte *room = (Byte *)dst.p + dst.len;
    bool in_place = spare >= least;
    QuoteOut o = {in_place ? room : tmp, 0, in_place ? spare : QUOTE_STACK};
    run(&o, &j);

    if (o.n <= o.cap) {
        if (!in_place)
            return slice_append(a, dst, tmp, o.n);
        Slice out = {dst.p, dst.len + o.n, dst.cap, dst.elem};
        return out;
    }

    /* Too long for either. Grow by the whole length in one append and write
     * into the space, which slice_append with no elements leaves for us. */
    Int old = dst.len;
    Slice out = slice_append(a, dst, NULL, o.n);
    if (out.p == NULL)
        return out;

    QuoteOut w = {(Byte *)out.p + old, 0, o.n};
    run(&w, &j);
    return out;
}

static QuoteJob str_job(Str s, bool ascii_only, bool graphic_only) {
    QuoteJob j = {s, 0, false, '"', ascii_only, graphic_only};
    return j;
}

static QuoteJob rune_job(Rune r, bool ascii_only, bool graphic_only) {
    QuoteJob j = {BURROW_STR_EMPTY, r, true, '\'', ascii_only, graphic_only};
    return j;
}

Int burrow__strconv_quote_into(Byte *dst, Str s) {
    QuoteJob j = str_job(s, false, false);
    /* NULL only counts. */
    QuoteOut o = {dst, 0, dst == NULL ? 0 : BURROW_INT_MAX};
    run(&o, &j);
    return o.n;
}

Str strconv_quote(Alloc *a, Str s) {
    return quote_str(a, str_job(s, false, false));
}

Str strconv_quote_to_ascii(Alloc *a, Str s) {
    return quote_str(a, str_job(s, true, false));
}

Str strconv_quote_to_graphic(Alloc *a, Str s) {
    return quote_str(a, str_job(s, false, true));
}

Str strconv_quote_rune(Alloc *a, Rune r) {
    return quote_str(a, rune_job(r, false, false));
}

Str strconv_quote_rune_to_ascii(Alloc *a, Rune r) {
    return quote_str(a, rune_job(r, true, false));
}

Str strconv_quote_rune_to_graphic(Alloc *a, Rune r) {
    return quote_str(a, rune_job(r, false, true));
}

Slice strconv_append_quote(Alloc *a, Slice dst, Str s) {
    return quote_append(a, dst, str_job(s, false, false));
}

Slice strconv_append_quote_to_ascii(Alloc *a, Slice dst, Str s) {
    return quote_append(a, dst, str_job(s, true, false));
}

Slice strconv_append_quote_to_graphic(Alloc *a, Slice dst, Str s) {
    return quote_append(a, dst, str_job(s, false, true));
}

Slice strconv_append_quote_rune(Alloc *a, Slice dst, Rune r) {
    return quote_append(a, dst, rune_job(r, false, false));
}

Slice strconv_append_quote_rune_to_ascii(Alloc *a, Slice dst, Rune r) {
    return quote_append(a, dst, rune_job(r, true, false));
}

Slice strconv_append_quote_rune_to_graphic(Alloc *a, Slice dst, Rune r) {
    return quote_append(a, dst, rune_job(r, false, true));
}

bool strconv_can_backquote(Str s) {
    for (Int i = 0; i < s.len;) {
        Int width = 0;
        Rune r = utf8_decode_rune_in_string(str_from_bytes(s.p + i, s.len - i), &width);
        i += width;
        if (width > 1) {
            if (r == 0xFEFF)
                return false; /* byte order marks are invisible */
            continue;         /* every other multibyte rune is fine */
        }
        if (r == UTF8_RUNE_ERROR)
            return false;
        if ((r < ' ' && r != '\t') || r == '`' || r == 0x7F)
            return false;
    }
    return true;
}

/* ------------------------------------------------------------------ unquoting */

static bool unhex(Byte b, Rune *v) {
    if ('0' <= b && b <= '9')
        *v = b - '0';
    else if ('a' <= b && b <= 'f')
        *v = b - 'a' + 10;
    else if ('A' <= b && b <= 'F')
        *v = b - 'A' + 10;
    else
        return false;
    return true;
}

/* UnquoteChar with its results in a struct, which is what the public function
 * and the unquote loop both want. On failure everything is left zero, which
 * is what Go's named results come back as. */
typedef struct CharResult {
    Rune value;
    bool multibyte;
    Str tail;
} CharResult;

static bool unquote_char(Str s, Byte quote, CharResult *out) {
    *out = (CharResult){0, false, BURROW_STR_EMPTY};

    if (s.len == 0)
        return false;

    Byte c = s.p[0];
    if (c == quote && (quote == '\'' || quote == '"'))
        return false;
    if (c >= UTF8_RUNE_SELF) {
        Int size = 0;
        Rune r = utf8_decode_rune_in_string(s, &size);
        *out = (CharResult){r, true, str_from_bytes(s.p + size, s.len - size)};
        return true;
    }
    if (c != '\\') {
        *out = (CharResult){c, false, str_from_bytes(s.p + 1, s.len - 1)};
        return true;
    }

    /* The hard case: a backslash. */
    if (s.len <= 1)
        return false;
    c = s.p[1];
    s = str_from_bytes(s.p + 2, s.len - 2);

    Rune value = 0;
    bool multibyte = false;

    switch (c) {
    case 'a':
        value = '\a';
        break;
    case 'b':
        value = '\b';
        break;
    case 'f':
        value = '\f';
        break;
    case 'n':
        value = '\n';
        break;
    case 'r':
        value = '\r';
        break;
    case 't':
        value = '\t';
        break;
    case 'v':
        value = '\v';
        break;
    case 'x':
    case 'u':
    case 'U': {
        Int n = c == 'x' ? 2 : c == 'u' ? 4 : 8;
        Rune v = 0;
        if (s.len < n)
            return false;
        for (Int j = 0; j < n; j++) {
            Rune x = 0;
            if (!unhex(s.p[j], &x))
                return false;
            v = v << 4 | x;
        }
        s = str_from_bytes(s.p + n, s.len - n);
        if (c == 'x') {
            /* One byte, which need not be UTF-8 on its own. */
            value = v;
            break;
        }
        if (!utf8_valid_rune(v))
            return false;
        value = v;
        multibyte = true;
        break;
    }
    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7': {
        Rune v = c - '0';
        if (s.len < 2)
            return false;
        for (Int j = 0; j < 2; j++) { /* one digit already, two more */
            Rune x = (Rune)s.p[j] - '0';
            if (x < 0 || x > 7)
                return false;
            v = (v << 3) | x;
        }
        s = str_from_bytes(s.p + 2, s.len - 2);
        if (v > 255)
            return false;
        value = v;
        break;
    }
    case '\\':
        value = '\\';
        break;
    case '\'':
    case '"':
        if (c != quote)
            return false;
        value = c;
        break;
    default:
        return false;
    }

    *out = (CharResult){value, multibyte, s};
    return true;
}

Rune strconv_unquote_char(Str s, Byte quote, bool *multibyte, Str *tail, Error *err) {
    CharResult r;
    bool ok = unquote_char(s, quote, &r);

    BURROW_OUT(multibyte, r.multibyte);
    BURROW_OUT(tail, r.tail);
    BURROW_OUT(err, ok ? BURROW_NO_ERROR : strconv_err_syntax);
    return r.value;
}

/* What the literal at the start of a string turned out to be. */
typedef enum LiteralKind {
    LITERAL_BAD,     /* not a literal */
    LITERAL_PLAIN,   /* its inside is its value, nothing to undo */
    LITERAL_RAW_CR,  /* backquoted with carriage returns to drop */
    LITERAL_ESCAPED, /* quoted with escapes to undo */
} LiteralKind;

/* The escape loop of Go's unquote, from just after the opening quote. It writes
 * the value to o and sets *end to just past the closing quote. */
static bool unescape(Str in, Byte quote, QuoteOut *o, Int *end) {
    Str rest = str_from_bytes(in.p + 1, in.len - 1);

    while (rest.len > 0 && rest.p[0] != quote) {
        /* An unescaped newline is not allowed in either kind of literal. */
        if (rest.p[0] == '\n')
            return false;
        CharResult c;
        if (!unquote_char(rest, quote, &c))
            return false;
        rest = c.tail;

        if (c.value < UTF8_RUNE_SELF || !c.multibyte)
            put_byte(o, (Byte)c.value);
        else
            put_rune(o, c.value);

        /* A single quoted literal holds exactly one character. */
        if (quote == '\'')
            break;
    }

    if (!(rest.len > 0 && rest.p[0] == quote))
        return false;
    *end = in.len - rest.len + 1;
    return true;
}

/* Finds the literal at the start of in, how far it runs and how long its value
 * is. The value of an escaped literal goes to o as far as it fits, and nothing
 * is written for the other kinds. */
static LiteralKind scan(Str in, Int *end, Int *n, QuoteOut *o) {
    if (in.len < 2)
        return LITERAL_BAD;

    Byte quote = in.p[0];
    Int e = index_byte(str_from_bytes(in.p + 1, in.len - 1), quote);
    if (e < 0)
        return LITERAL_BAD;
    e += 2; /* past the first matching quote, which escapes may yet move */

    Str lit = str_from_bytes(in.p, e);
    Str inside = str_from_bytes(in.p + 1, e - 2);

    switch (quote) {
    case '`':
        /* Raw strings are not checked for valid UTF-8. Go does not check
         * either, and says so. */
        *end = e;
        if (!contains_byte(lit, '\r')) {
            *n = inside.len;
            return LITERAL_PLAIN;
        }
        *n = 0;
        for (Int i = 0; i < inside.len; i++)
            if (inside.p[i] != '\r')
                (*n)++;
        return LITERAL_RAW_CR;

    case '"':
    case '\'':
        if (!contains_byte(lit, '\\') && !contains_byte(lit, '\n')) {
            bool valid;
            if (quote == '"') {
                valid = utf8_valid_string(inside);
            } else {
                Int size = 0;
                Rune r = utf8_decode_rune_in_string(inside, &size);
                valid = 1 + size + 1 == e && (r != UTF8_RUNE_ERROR || size != 1);
            }
            if (valid) {
                *end = e;
                *n = inside.len;
                return LITERAL_PLAIN;
            }
        }
        if (!unescape(in, quote, o, end))
            return LITERAL_BAD;
        *n = o->n;
        return LITERAL_ESCAPED;

    default:
        return LITERAL_BAD;
    }
}

Str strconv_quoted_prefix(Str s, Error *err) {
    Int end = 0, n = 0;
    QuoteOut count = {NULL, 0, 0};

    if (scan(s, &end, &n, &count) == LITERAL_BAD) {
        BURROW_OUT(err, strconv_err_syntax);
        return BURROW_STR_EMPTY;
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return str_from_bytes(s.p, end);
}

Str strconv_unquote(Alloc *a, Str s, Error *err) {
    Int end = 0, n = 0;
    Byte tmp[QUOTE_STACK];
    QuoteOut first = {tmp, 0, QUOTE_STACK};
    LiteralKind kind = scan(s, &end, &n, &first);

    if (kind == LITERAL_BAD || end != s.len) {
        BURROW_OUT(err, strconv_err_syntax);
        return BURROW_STR_EMPTY;
    }
    BURROW_OUT(err, BURROW_NO_ERROR);

    if (kind == LITERAL_PLAIN)
        return str_from_bytes(s.p + 1, s.len - 2);
    if (n == 0)
        return BURROW_STR_EMPTY;

    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)n, 1);
    if (p == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return BURROW_STR_EMPTY;
    }

    QuoteOut o = {p, 0, n};
    if (kind == LITERAL_RAW_CR) {
        for (Int i = 1; i < s.len - 1; i++)
            if (s.p[i] != '\r')
                put_byte(&o, s.p[i]);
    } else if (n <= QUOTE_STACK) {
        memcpy(p, tmp, (size_t)n);
        o.n = n;
    } else {
        (void)unescape(s, s.p[0], &o, &end);
    }
    return str_from_bytes(p, o.n);
}

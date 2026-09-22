/* Struct tags: getting json:"name,omitempty" back out of a field.
 *
 * A tag is one string on the field and it holds several things at once, one per
 * package that cares. Go's convention, which this follows exactly, is a
 * space separated list of key:"value" pairs:
 *
 *     json:"id,omitempty" xml:"id,attr" db:"user_id"
 *
 * encoding/json reads the json one, encoding/xml reads the xml one, and neither
 * of them has to know the other is there. Nothing validates the format, on
 * purpose and the same way Go does not: a tag that does not parse is a tag
 * nobody finds a key in, rather than an error at a point where there is nothing
 * useful to do with one.
 *
 * WHY THE VALUE IS COPIED
 *
 * Go returns a slice of the tag itself when the value has no escapes in it, and
 * it can, because a Go string is immutable and the tag lives as long as the
 * program. burrow copies, always, and takes an allocator to do it.
 *
 * The reason is that the alternative is a function that sometimes owns what it
 * returns and sometimes borrows it, decided by whether the tag happened to
 * contain a backslash. There is no way to annotate that, no way for a caller to
 * know which it got, and the whole ownership scheme in this library rests on
 * that question having one answer per function. A copy of a short string, on an
 * arena, in code that caches its answer per type anyway, is not worth the hole
 * that would put in it.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/type.h"

#include "burrow/core.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/utf8.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------- unquoting
 *
 * The value in a tag is a Go string literal, quotes and all, so getting the
 * string out of it means undoing the escapes. This is strconv.Unquote for the
 * double quoted case, which is the only case a tag can be in, and it is here
 * rather than in strconv because strconv is a later milestone and reflect needs
 * it now. It moves when strconv lands.
 *
 * Everything runs twice: once with out NULL to find out how long the answer is,
 * and once to write it. That is cheaper than it sounds on strings this short,
 * and it is what lets the caller be given an allocation of exactly the right
 * size. Sizing it by a bound instead would mean handing back a Str whose length
 * and whose allocation disagree, and then the caller could not free it. */

static bool hex_digit(Byte b, uint32_t *v) {
    if (b >= '0' && b <= '9')
        *v = (uint32_t)(b - '0');
    else if (b >= 'a' && b <= 'f')
        *v = (uint32_t)(b - 'a') + 10;
    else if (b >= 'A' && b <= 'F')
        *v = (uint32_t)(b - 'A') + 10;
    else
        return false;

    return true;
}

static void emit(Byte *out, Int *n, const void *p, Int k) {
    if (out != NULL)
        memcpy(out + *n, p, (size_t)k);

    *n += k;
}

static void emit_rune(Byte *out, Int *n, Rune r) {
    Byte buf[4];
    Slice dst = slice_from(buf, (Int)sizeof buf, (Int)sizeof buf, TYPE_OF(Byte));
    Int k = utf8_encode_rune(dst, r);

    emit(out, n, buf, k);
}

/* One escape sequence, starting at the backslash. Returns how many bytes of the
 * source it used, or zero for a sequence that is not one. */
static Int unquote_escape(Str q, Int i, Byte *out, Int *n) {
    if (i + 1 >= q.len)
        return 0;

    Byte c = (Byte)q.p[i + 1];
    Byte simple = 0;

    switch (c) {
    case 'a':
        simple = 0x07;
        break;
    case 'b':
        simple = 0x08;
        break;
    case 'f':
        simple = 0x0c;
        break;
    case 'n':
        simple = '\n';
        break;
    case 'r':
        simple = '\r';
        break;
    case 't':
        simple = '\t';
        break;
    case 'v':
        simple = 0x0b;
        break;
    case '\\':
        simple = '\\';
        break;
    case '"':
        simple = '"';
        break;
    default:
        break;
    }

    if (simple != 0) {
        emit(out, n, &simple, 1);
        return 2;
    }

    /* \xNN is a byte and not a rune, so it can put a byte in the string that no
     * rune would have produced. Go allows that and so does this. */
    if (c == 'x') {
        uint32_t hi = 0;
        uint32_t lo = 0;

        if (i + 3 >= q.len || !hex_digit((Byte)q.p[i + 2], &hi) ||
            !hex_digit((Byte)q.p[i + 3], &lo))
            return 0;

        Byte b = (Byte)(hi * 16 + lo);
        emit(out, n, &b, 1);
        return 4;
    }

    /* Three octal digits, also a byte, and it has to fit in one. */
    if (c >= '0' && c <= '7') {
        uint32_t v = 0;

        if (i + 3 >= q.len)
            return 0;

        for (Int k = 1; k <= 3; k++) {
            Byte d = (Byte)q.p[i + k];
            if (d < '0' || d > '7')
                return 0;
            v = v * 8 + (uint32_t)(d - '0');
        }
        if (v > 255)
            return 0;

        Byte b = (Byte)v;
        emit(out, n, &b, 1);
        return 4;
    }

    if (c == 'u' || c == 'U') {
        Int digits = c == 'u' ? 4 : 8;
        uint32_t v = 0;

        if (i + 1 + digits >= q.len)
            return 0;

        for (Int k = 0; k < digits; k++) {
            uint32_t d = 0;
            if (!hex_digit((Byte)q.p[i + 2 + k], &d))
                return 0;
            v = v * 16 + d;
        }

        if (!utf8_valid_rune((Rune)v))
            return 0;

        emit_rune(out, n, (Rune)v);
        return 2 + digits;
    }

    return 0;
}

/* q includes its quotes. Writes to out when out is not NULL, and sets *n to the
 * length either way. False means the literal is malformed, which is the same
 * answer as a key that is not there. */
static bool unquote(Str q, Byte *out, Int *n) {
    *n = 0;

    if (q.len < 2 || q.p[0] != '"' || q.p[q.len - 1] != '"')
        return false;

    Int i = 1;
    Int end = q.len - 1;

    while (i < end) {
        Byte c = (Byte)q.p[i];

        /* A quote here would have ended the literal, and a raw newline is not
         * allowed in one. Either means the scan that found this was wrong. */
        if (c == '"' || c == '\n')
            return false;

        if (c == '\\') {
            Int used = unquote_escape(q, i, out, n);
            if (used == 0 || i + used > end)
                return false;
            i += used;
            continue;
        }

        /* A plain rune. Go decodes and re-encodes here, which passes valid
         * UTF-8 through unchanged and turns a byte that is not part of any
         * rune into U+FFFD rather than smuggling it out. */
        Str rest = str_from_bytes(q.p + i, end - i);
        Int size = 0;
        Rune r = utf8_decode_rune_in_string(rest, &size);

        if (r == 0xFFFD && size == 1)
            emit_rune(out, n, 0xFFFD);
        else
            emit(out, n, q.p + i, size);

        i += size;
    }

    return true;
}

/* ------------------------------------------------------------- the lookup */

bool tag_lookup(Alloc *a, Str tag, Str key, Str *value) {
    if (value != NULL)
        *value = BURROW_STR_EMPTY;

    Int i = 0;

    while (i < tag.len) {
        /* Leading spaces, and only spaces. Go is strict about this and a tab
         * between two pairs ends the scan. */
        while (i < tag.len && tag.p[i] == ' ')
            i++;
        if (i >= tag.len)
            break;

        /* The key, up to the colon. A space, a quote or a control character in
         * here means the tag is not in the format, so stop rather than guess. */
        Int name_start = i;
        while (i < tag.len && (Byte)tag.p[i] > ' ' && tag.p[i] != ':' &&
               tag.p[i] != '"' && (Byte)tag.p[i] != 0x7f)
            i++;

        if (i == name_start || i + 1 >= tag.len || tag.p[i] != ':' ||
            tag.p[i + 1] != '"')
            break;

        Str name = str_from_bytes(tag.p + name_start, i - name_start);
        i++; /* now on the opening quote */

        /* The quoted value, skipping whatever a backslash escapes so that a
         * quote inside the literal does not end it. */
        Int quote_start = i;
        Int j = i + 1;
        while (j < tag.len && tag.p[j] != '"') {
            if (tag.p[j] == '\\')
                j++;
            j++;
        }
        if (j >= tag.len)
            break;

        Str quoted = str_from_bytes(tag.p + quote_start, j + 1 - quote_start);
        i = j + 1;

        if (!str_eq(name, key))
            continue;

        Int n = 0;
        if (!unquote(quoted, NULL, &n))
            break;

        if (n == 0) {
            /* Present and empty, which is not the same as absent, so this is
             * still true. Nothing is allocated for it. */
            return true;
        }

        Byte *buf = BURROW_NEW_N(a, Byte, (size_t)n);
        if (buf == NULL)
            return false;

        Int written = 0;
        (void)unquote(quoted, buf, &written);

        if (value != NULL)
            *value = str_from_bytes(buf, written);

        return true;
    }

    return false;
}

Str tag_get(Alloc *a, Str tag, Str key) {
    Str value = BURROW_STR_EMPTY;

    (void)tag_lookup(a, tag, key, &value);
    return value;
}

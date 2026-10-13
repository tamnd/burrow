/* package html, EscapeString and UnescapeString. Go's html/escape.go.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "internal.h"

#include <string.h>

#include "burrow/strings.h"
#include "burrow/utf8.h"

/* replacementTable: what the numeric references from 0x80 to 0x9F stand for,
 * which is their Windows-1252 meaning. 0x00 becomes U+FFFD in the code, and
 * 0x0D needs nothing. */
static const Rune html_replacement_table[] = {
    0x20AC, /* First entry is what 0x80 should be replaced with. */
    0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6,
    0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F, 0x0090,
    0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC,
    0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178, /* Last entry is 0x9F. */
};

/* The entity called name in the sorted table t, or NULL. */
static const burrow__HtmlEntity *html_lookup(const burrow__HtmlEntity *t, Int n,
                                             const Byte *name, Int len) {
    Int lo = 0;
    Int hi = n;
    while (lo < hi) {
        Int mid = lo + (hi - lo) / 2;
        Str k = t[mid].name;
        Int m = k.len < len ? k.len : len;
        int c = m > 0 ? memcmp(k.p, name, (size_t)m) : 0;
        if (c == 0)
            c = k.len < len ? -1 : (k.len > len ? 1 : 0);
        if (c == 0)
            return &t[mid];
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}

/* utf8.EncodeRune(b[dst:], r). */
static Int html_put_rune(Byte *b, Int len, Int dst, Rune r) {
    return utf8_encode_rune((Slice){b + dst, len - dst, len - dst, TYPE_BYTE}, r);
}

/* unescapeEntity: reads an entity like "&lt;" from b[src:] and writes the
 * corresponding "<" to b[dst:], returning the incremented dst and src cursors.
 * Precondition: b[src] == '&' && dst <= src. */
static void html_unescape_entity(Byte *b, Int len, Int *dstp, Int *srcp) {
    Int dst = *dstp;
    Int src = *srcp;
    /* http://www.whatwg.org/specs/web-apps/current-work/multipage/tokenization.html#consume-a-character-reference
     *
     * i starts at 1 because we already know that s[0] == '&'. */
    Int i = 1;
    const Byte *s = b + src;
    Int slen = len - src;

    if (slen <= 1) {
        b[dst] = b[src];
        *dstp = dst + 1;
        *srcp = src + 1;
        return;
    }

    if (s[i] == '#') {
        if (slen <= 3) { /* We need to have at least "&#.". */
            b[dst] = b[src];
            *dstp = dst + 1;
            *srcp = src + 1;
            return;
        }
        i++;
        Byte c = s[i];
        bool hex = false;
        if (c == 'x' || c == 'X') {
            hex = true;
            i++;
        }

        /* Go adds into a rune and lets it wrap. Doing that in uint32_t keeps
         * the wrapping defined, and the cast back gives Go's value. */
        uint32_t x = 0;
        while (i < slen) {
            c = s[i];
            i++;
            if (hex) {
                if ('0' <= c && c <= '9') {
                    x = 16 * x + (uint32_t)(c - '0');
                    continue;
                }
                if ('a' <= c && c <= 'f') {
                    x = 16 * x + (uint32_t)(c - 'a') + 10;
                    continue;
                }
                if ('A' <= c && c <= 'F') {
                    x = 16 * x + (uint32_t)(c - 'A') + 10;
                    continue;
                }
            } else if ('0' <= c && c <= '9') {
                x = 10 * x + (uint32_t)(c - '0');
                continue;
            }
            if (c != ';')
                i--;
            break;
        }

        if (i <= 3) { /* No characters matched. */
            b[dst] = b[src];
            *dstp = dst + 1;
            *srcp = src + 1;
            return;
        }

        Rune r = (Rune)x;
        if (0x80 <= r && r <= 0x9F) {
            /* Replace characters from Windows-1252 with UTF-8 equivalents. */
            r = html_replacement_table[r - 0x80];
        } else if (r == 0 || (0xD800 <= r && r <= 0xDFFF) || r > 0x10FFFF) {
            /* Replace invalid characters with the replacement character. */
            r = 0xFFFD;
        }

        *dstp = dst + html_put_rune(b, len, dst, r);
        *srcp = src + i;
        return;
    }

    /* Consume the maximum number of characters possible, with the consumed
     * characters matching one of the named references. */
    while (i < slen) {
        Byte c = s[i];
        i++;
        /* Lower-cased characters are more common in entities, so we check for
         * them first. */
        if (('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z') || ('0' <= c && c <= '9'))
            continue;
        if (c != ';')
            i--;
        break;
    }

    const Byte *entity_name = s + 1;
    Int name_len = i - 1;
    if (name_len > 0) {
        const burrow__HtmlEntity *e = html_lookup(
            burrow__html_entity, burrow__html_entity_len, entity_name, name_len);
        if (e != NULL) {
            *dstp = dst + html_put_rune(b, len, dst, e->r[0]);
            *srcp = src + i;
            return;
        }
        e = html_lookup(burrow__html_entity2, burrow__html_entity2_len, entity_name,
                        name_len);
        if (e != NULL) {
            Int dst1 = dst + html_put_rune(b, len, dst, e->r[0]);
            *dstp = dst1 + html_put_rune(b, len, dst1, e->r[1]);
            *srcp = src + i;
            return;
        }
        Int max_len = name_len - 1;
        if (max_len > BURROW__HTML_LONGEST_ENTITY_WITHOUT_SEMICOLON)
            max_len = BURROW__HTML_LONGEST_ENTITY_WITHOUT_SEMICOLON;
        for (Int j = max_len; j > 1; j--) {
            e = html_lookup(burrow__html_entity, burrow__html_entity_len, entity_name,
                            j);
            if (e != NULL) {
                *dstp = dst + html_put_rune(b, len, dst, e->r[0]);
                *srcp = src + j + 1;
                return;
            }
        }
    }

    memmove(b + dst, b + src, (size_t)i);
    *dstp = dst + i;
    *srcp = src + i;
}

/* htmlEscaper, which Go builds with strings.NewReplacer. */
static Str html_escape_of(Byte c) {
    switch (c) {
    case '&':
        return BURROW_S("&amp;");
    case '\'':
        return BURROW_S("&#39;"); /* "&#39;" is shorter than "&apos;" and apos was
                                     not in HTML until HTML5. */
    case '<':
        return BURROW_S("&lt;");
    case '>':
        return BURROW_S("&gt;");
    case '"':
        return BURROW_S("&#34;"); /* "&#34;" is shorter than "&quot;". */
    default:
        return BURROW_STR_EMPTY;
    }
}

Str html_escape_string(Alloc *a, Str s) {
    Int n = s.len;
    for (Int i = 0; i < s.len; i++) {
        Str r = html_escape_of(s.p[i]);
        if (r.len > 0)
            n += r.len - 1;
    }
    if (n == s.len)
        return s;
    Byte *p = BURROW_NEW_N(a, Byte, (size_t)n);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    Int w = 0;
    for (Int i = 0; i < s.len; i++) {
        Str r = html_escape_of(s.p[i]);
        if (r.len == 0) {
            p[w++] = s.p[i];
        } else {
            memcpy(p + w, r.p, (size_t)r.len);
            w += r.len;
        }
    }
    return str_from_bytes(p, w);
}

Str html_unescape_string(Alloc *a, Str s) {
    Int i = strings_index_byte(s, '&');

    if (i < 0)
        return s;

    /* An entity never takes more bytes unescaped than escaped, which the tests
     * check, so the result fits where s was. */
    Byte *b = BURROW_NEW_N(a, Byte, (size_t)s.len);
    if (b == NULL)
        return BURROW_STR_EMPTY;
    memcpy(b, s.p, (size_t)s.len);
    Int dst = i;
    Int src = i;
    html_unescape_entity(b, s.len, &dst, &src);
    while (s.len - src > 0) {
        if (s.p[src] == '&')
            i = 0;
        else
            i = strings_index_byte(str_from_bytes(s.p + src, s.len - src), '&');
        if (i < 0) {
            memmove(b + dst, s.p + src, (size_t)(s.len - src));
            dst += s.len - src;
            break;
        }

        if (i > 0)
            memmove(b + dst, s.p + src, (size_t)i);
        dst += i;
        src += i;
        html_unescape_entity(b, s.len, &dst, &src);
    }
    return str_from_bytes(b, dst);
}

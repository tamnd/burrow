/* Derived from Go's src/strings/strings.go, src/strings/compare.go,
 * src/strings/clone.go and src/internal/stringslite/strings.go.
 * Go source: go1.27.1.
 *
 * Line for line where it can be, including the fast paths and the constants
 * that decide when to leave them, because Go's tests size their inputs around
 * those. What changes is where the memory comes from: every function that
 * builds a string takes the allocator and sizes the result exactly when Go's
 * does, and one that finds nothing to change hands back its input, as Go's
 * does.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/strings.h"

#include "burrow/core.h"
#include "burrow/func.h"
#include "burrow/mem.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/type.h"
#include "burrow/unicode.h"
#include "burrow/utf8.h"

#include "bytealg.h"
#include "internal.h"

#include <stdint.h>
#include <string.h>

/* The empty Str may have a NULL p, and NULL + 0 is undefined in C, so an
 * offset of zero leaves p alone. */
static Str sub(Str s, Int lo, Int hi) {
    Str r = {lo == 0 ? s.p : s.p + lo, hi - lo};
    return r;
}

static Str tail_at(Str s, Int lo) {
    Str r = {lo == 0 ? s.p : s.p + lo, s.len - lo};
    return r;
}

static Rune decode_rune(Str s, Int *size) {
    return utf8_decode_rune_in_string(s, size);
}

/* A fresh buffer of n bytes from a, or NULL. */
static Byte *alloc_bytes(Alloc *a, Int n) {
    return (Byte *)mem_alloc_nozero(a, (size_t)n, 1);
}

static Str mkstr(const Byte *p, Int n) {
    Str s = {p, n};
    return s;
}

/* A Slice of n Str from a, for the splitting functions. */
static Slice str_slice(Alloc *a, Int n) {
    return slice_make(a, TYPE_STRING, n, n);
}

static void *unconst_str(const void *p) {
    return (void *)(uintptr_t)p;
}

/* ------------------------------------------------------------ comparing */

Int strings_compare(Str a, Str b) {
    /* str_cmp gives the sign and nothing more, and Go gives -1, 0 or +1. */
    int c = str_cmp(a, b);
    return (c > 0) - (c < 0);
}

bool strings_has_prefix(Str s, Str prefix) {
    return s.len >= prefix.len && burrow__bytealg_equal(sub(s, 0, prefix.len), prefix);
}

bool strings_has_suffix(Str s, Str suffix) {
    return s.len >= suffix.len &&
           burrow__bytealg_equal(sub(s, s.len - suffix.len, s.len), suffix);
}

bool strings_equal_fold(Str s, Str t) {
    /* ASCII fast path */
    Int i = 0;
    Int n = s.len < t.len ? s.len : t.len;
    for (; i < n; i++) {
        Byte sr = s.p[i];
        Byte tr = t.p[i];
        if ((sr | tr) >= UTF8_RUNE_SELF)
            goto has_unicode;
        if (tr == sr)
            continue;
        if (tr < sr) {
            Byte x = tr;
            tr = sr;
            sr = x;
        }
        /* ASCII only, sr/tr must be upper/lower case */
        if ('A' <= sr && sr <= 'Z' && tr == sr + 'a' - 'A')
            continue;
        return false;
    }
    return s.len == t.len;

has_unicode:
    s = tail_at(s, i);
    t = tail_at(t, i);
    {
        Rune sr;
        for (StrIter it = str_runes(s); str_next_rune(&it, NULL, &sr);) {
            if (t.len == 0)
                return false;
            Int size;
            Rune tr = decode_rune(t, &size);
            t = tail_at(t, size);
            if (tr == sr)
                continue;
            if (tr < sr) {
                Rune x = tr;
                tr = sr;
                sr = x;
            }
            if (tr < UTF8_RUNE_SELF) {
                if ('A' <= sr && sr <= 'Z' && tr == sr + 'a' - 'A')
                    continue;
                return false;
            }
            /* General case. SimpleFold(x) returns the next equivalent rune > x
             * or wraps around to smaller values. */
            Rune r = unicode_simple_fold(sr);
            while (r != sr && r < tr)
                r = unicode_simple_fold(r);
            if (r == tr)
                continue;
            return false;
        }
    }
    return t.len == 0;
}

/* ------------------------------------------------------------ searching */

Int strings_index_byte(Str s, Byte c) {
    return burrow__bytealg_index_byte(s, c);
}

Int strings_last_index_byte(Str s, Byte c) {
    return burrow__bytealg_last_index_byte(s, c);
}

Int strings_index(Str s, Str substr) {
    Int n = substr.len;
    if (n == 0 || str_eq(substr, s))
        return 0;
    if (n == 1)
        return strings_index_byte(s, substr.p[0]);
    if (n >= s.len)
        return -1;
    if (n <= BURROW__BYTEALG_MAX_LEN && s.len <= BURROW__BYTEALG_MAX_BRUTE_FORCE)
        /* Use brute force when s and substr both are small */
        return burrow__bytealg_index_string(s, substr);
    Byte c0 = substr.p[0];
    Byte c1 = substr.p[1];
    Int i = 0;
    Int t = s.len - n + 1;
    Int fails = 0;
    while (i < t) {
        if (s.p[i] != c0) {
            /* IndexByte is faster than IndexString, so use it as long as we
             * are not getting lots of false positives. */
            Int o = strings_index_byte(sub(s, i + 1, t), c0);
            if (o < 0)
                return -1;
            i += o + 1;
        }
        if (s.p[i + 1] == c1 && memcmp(s.p + i, substr.p, (size_t)n) == 0)
            return i;
        i++;
        fails++;
        if (n <= BURROW__BYTEALG_MAX_LEN && fails > burrow__bytealg_cutover(i)) {
            /* Switch to IndexString when IndexByte produces too many false
             * positives. */
            Int r = burrow__bytealg_index_string(tail_at(s, i), substr);
            return r >= 0 ? r + i : -1;
        } else if (n > BURROW__BYTEALG_MAX_LEN && fails >= 4 + (i >> 4) && i < t) {
            Int j = burrow__bytealg_index_rabin_karp(tail_at(s, i), substr);
            return j < 0 ? -1 : i + j;
        }
    }
    return -1;
}

Int strings_last_index(Str s, Str substr) {
    Int n = substr.len;
    if (n == 0)
        return s.len;
    if (n == 1)
        return burrow__bytealg_last_index_byte(s, substr.p[0]);
    if (n == s.len)
        return str_eq(substr, s) ? 0 : -1;
    if (n > s.len)
        return -1;
    return burrow__bytealg_last_index_rabin_karp(s, substr);
}

Int strings_index_rune(Str s, Rune r) {
    if (0 <= r && r < UTF8_RUNE_SELF)
        return strings_index_byte(s, (Byte)r);
    if (r == UTF8_RUNE_ERROR) {
        Int i;
        Rune c;
        for (StrIter it = str_runes(s); str_next_rune(&it, &i, &c);) {
            if (c == UTF8_RUNE_ERROR)
                return i;
        }
        return -1;
    }
    if (!utf8_valid_rune(r))
        return -1;

    /* Search for rune r using the last byte of its UTF-8 encoded form. The
     * distribution of the last byte is more uniform compared to the first byte
     * which has a 78% chance of being [240, 243, 244]. */
    Byte rb[UTF8_UTF_MAX];
    Int rlen =
        utf8_encode_rune(slice_from(rb, UTF8_UTF_MAX, UTF8_UTF_MAX, TYPE_BYTE), r);
    Str rs = {rb, rlen};
    Int last = rlen - 1;
    Int i = last;
    Int fails = 0;
    while (i < s.len) {
        if (s.p[i] != rb[last]) {
            Int o = strings_index_byte(tail_at(s, i + 1), rb[last]);
            if (o < 0)
                return -1;
            i += o + 1;
        }
        /* Step backwards comparing bytes. */
        bool match = true;
        for (Int j = 1; j < rlen; j++) {
            if (s.p[i - j] != rb[last - j]) {
                match = false;
                break;
            }
        }
        if (match)
            return i - last;
        fails++;
        i++;
        if (fails > burrow__bytealg_cutover(i) && i < s.len) {
            Int j = burrow__bytealg_index_string(tail_at(s, i - last), rs);
            return j >= 0 ? i + j - last : -1;
        }
    }
    return -1;
}

/* Go's asciiSet: a table of the bytes in chars, and whether they all were
 * ASCII. */
typedef struct AsciiSet {
    bool in[256];
} AsciiSet;

static bool make_ascii_set(AsciiSet *as, Str chars) {
    memset(as, 0, sizeof *as);
    for (Int i = 0; i < chars.len; i++) {
        Byte c = chars.p[i];
        if (c >= UTF8_RUNE_SELF)
            return false;
        as->in[c] = true;
    }
    return true;
}

/* The threshold of 8 bytes balances initialization cost against per-byte
 * search cost. */
static bool should_use_ascii_set(Int buf_len) {
    return buf_len > 8;
}

Int strings_index_any(Str s, Str chars) {
    if (chars.len == 0)
        return -1;
    if (chars.len == 1) {
        Rune r = chars.p[0];
        if (r >= UTF8_RUNE_SELF)
            r = UTF8_RUNE_ERROR;
        return strings_index_rune(s, r);
    }
    if (should_use_ascii_set(s.len)) {
        AsciiSet as;
        if (make_ascii_set(&as, chars)) {
            for (Int i = 0; i < s.len; i++) {
                if (as.in[s.p[i]])
                    return i;
            }
            return -1;
        }
    }
    Int i;
    Rune c;
    for (StrIter it = str_runes(s); str_next_rune(&it, &i, &c);) {
        if (strings_index_rune(chars, c) >= 0)
            return i;
    }
    return -1;
}

Int strings_last_index_any(Str s, Str chars) {
    if (chars.len == 0)
        return -1;
    if (s.len == 1) {
        Rune rc = s.p[0];
        if (rc >= UTF8_RUNE_SELF)
            rc = UTF8_RUNE_ERROR;
        return strings_index_rune(chars, rc) >= 0 ? 0 : -1;
    }
    if (should_use_ascii_set(s.len)) {
        AsciiSet as;
        if (make_ascii_set(&as, chars)) {
            for (Int i = s.len - 1; i >= 0; i--) {
                if (as.in[s.p[i]])
                    return i;
            }
            return -1;
        }
    }
    if (chars.len == 1) {
        Rune rc = chars.p[0];
        if (rc >= UTF8_RUNE_SELF)
            rc = UTF8_RUNE_ERROR;
        for (Int i = s.len; i > 0;) {
            Int size;
            Rune r = utf8_decode_last_rune_in_string(sub(s, 0, i), &size);
            i -= size;
            if (rc == r)
                return i;
        }
        return -1;
    }
    for (Int i = s.len; i > 0;) {
        Int size;
        Rune r = utf8_decode_last_rune_in_string(sub(s, 0, i), &size);
        i -= size;
        if (strings_index_rune(chars, r) >= 0)
            return i;
    }
    return -1;
}

/* indexFunc and lastIndexFunc, which with truth false look for the first rune
 * f says no to. */
static Int index_func(Str s, RuneFunc f, bool truth) {
    Int i;
    Rune r;
    for (StrIter it = str_runes(s); str_next_rune(&it, &i, &r);) {
        if (BURROW_CALLF(f, r) == truth)
            return i;
    }
    return -1;
}

static Int last_index_func(Str s, RuneFunc f, bool truth) {
    for (Int i = s.len; i > 0;) {
        Int size;
        Rune r = utf8_decode_last_rune_in_string(sub(s, 0, i), &size);
        i -= size;
        if (BURROW_CALLF(f, r) == truth)
            return i;
    }
    return -1;
}

Int strings_index_func(Str s, RuneFunc f) {
    return index_func(s, f, true);
}

Int strings_last_index_func(Str s, RuneFunc f) {
    return last_index_func(s, f, true);
}

bool strings_contains(Str s, Str substr) {
    return strings_index(s, substr) >= 0;
}

bool strings_contains_any(Str s, Str chars) {
    return strings_index_any(s, chars) >= 0;
}

bool strings_contains_rune(Str s, Rune r) {
    return strings_index_rune(s, r) >= 0;
}

bool strings_contains_func(Str s, RuneFunc f) {
    return strings_index_func(s, f) >= 0;
}

Int strings_count(Str s, Str substr) {
    if (substr.len == 0)
        return utf8_rune_count_in_string(s) + 1;
    if (substr.len == 1)
        return burrow__bytealg_count(s, substr.p[0]);
    Int n = 0;
    for (;;) {
        Int i = strings_index(s, substr);
        if (i == -1)
            return n;
        n++;
        s = tail_at(s, i + substr.len);
    }
}

/* ------------------------------------------------------------ cutting */

Str strings_cut(Str s, Str sep, Str *after, bool *found) {
    Int i = strings_index(s, sep);
    if (i >= 0) {
        BURROW_OUT(after, tail_at(s, i + sep.len));
        BURROW_OUT(found, true);
        return sub(s, 0, i);
    }
    BURROW_OUT(after, BURROW_STR_EMPTY);
    BURROW_OUT(found, false);
    return s;
}

Str strings_cut_last(Str s, Str sep, Str *after, bool *found) {
    Int i = strings_last_index(s, sep);
    if (i >= 0) {
        BURROW_OUT(after, tail_at(s, i + sep.len));
        BURROW_OUT(found, true);
        return sub(s, 0, i);
    }
    BURROW_OUT(after, BURROW_STR_EMPTY);
    BURROW_OUT(found, false);
    return s;
}

Str strings_cut_prefix(Str s, Str prefix, bool *found) {
    if (!strings_has_prefix(s, prefix)) {
        BURROW_OUT(found, false);
        return s;
    }
    BURROW_OUT(found, true);
    return tail_at(s, prefix.len);
}

Str strings_cut_suffix(Str s, Str suffix, bool *found) {
    if (!strings_has_suffix(s, suffix)) {
        BURROW_OUT(found, false);
        return s;
    }
    BURROW_OUT(found, true);
    return sub(s, 0, s.len - suffix.len);
}

/* ------------------------------------------------------------ trimming */

static const Byte ascii_space[256] = {
    ['\t'] = 1, ['\n'] = 1, ['\v'] = 1, ['\f'] = 1, ['\r'] = 1, [' '] = 1};

Str strings_trim_left_func(Str s, RuneFunc f) {
    Int i = index_func(s, f, false);
    if (i == -1)
        return BURROW_STR_EMPTY;
    return tail_at(s, i);
}

Str strings_trim_right_func(Str s, RuneFunc f) {
    Int i = last_index_func(s, f, false);
    if (i >= 0) {
        Int wid;
        decode_rune(tail_at(s, i), &wid);
        i += wid;
    } else {
        i++;
    }
    return sub(s, 0, i);
}

Str strings_trim_func(Str s, RuneFunc f) {
    return strings_trim_right_func(strings_trim_left_func(s, f), f);
}

static Str trim_left_byte(Str s, Byte c) {
    while (s.len > 0 && s.p[0] == c)
        s = tail_at(s, 1);
    return s;
}

static Str trim_left_ascii(Str s, const AsciiSet *as) {
    while (s.len > 0 && as->in[s.p[0]])
        s = tail_at(s, 1);
    return s;
}

static Str trim_left_unicode(Str s, Str cutset) {
    while (s.len > 0) {
        Int n;
        Rune r = decode_rune(s, &n);
        if (!strings_contains_rune(cutset, r))
            break;
        s = tail_at(s, n);
    }
    return s;
}

static Str trim_right_byte(Str s, Byte c) {
    while (s.len > 0 && s.p[s.len - 1] == c)
        s.len--;
    return s;
}

static Str trim_right_ascii(Str s, const AsciiSet *as) {
    while (s.len > 0 && as->in[s.p[s.len - 1]])
        s.len--;
    return s;
}

static Str trim_right_unicode(Str s, Str cutset) {
    while (s.len > 0) {
        Rune r = s.p[s.len - 1];
        Int n = 1;
        if (r >= UTF8_RUNE_SELF)
            r = utf8_decode_last_rune_in_string(s, &n);
        if (!strings_contains_rune(cutset, r))
            break;
        s.len -= n;
    }
    return s;
}

Str strings_trim(Str s, Str cutset) {
    if (s.len == 0 || cutset.len == 0)
        return s;
    if (cutset.len == 1 && cutset.p[0] < UTF8_RUNE_SELF)
        return trim_left_byte(trim_right_byte(s, cutset.p[0]), cutset.p[0]);
    AsciiSet as;
    if (make_ascii_set(&as, cutset))
        return trim_left_ascii(trim_right_ascii(s, &as), &as);
    return trim_left_unicode(trim_right_unicode(s, cutset), cutset);
}

Str strings_trim_left(Str s, Str cutset) {
    if (s.len == 0 || cutset.len == 0)
        return s;
    if (cutset.len == 1 && cutset.p[0] < UTF8_RUNE_SELF)
        return trim_left_byte(s, cutset.p[0]);
    AsciiSet as;
    if (make_ascii_set(&as, cutset))
        return trim_left_ascii(s, &as);
    return trim_left_unicode(s, cutset);
}

Str strings_trim_right(Str s, Str cutset) {
    if (s.len == 0 || cutset.len == 0)
        return s;
    if (cutset.len == 1 && cutset.p[0] < UTF8_RUNE_SELF)
        return trim_right_byte(s, cutset.p[0]);
    AsciiSet as;
    if (make_ascii_set(&as, cutset))
        return trim_right_ascii(s, &as);
    return trim_right_unicode(s, cutset);
}

static bool space_rune(void *env, Rune r) {
    (void)env;
    return unicode_is_space(r);
}

Str strings_trim_space(Str s) {
    /* Fast path for ASCII: look for the first ASCII non-space byte. */
    for (Int lo = 0; lo < s.len; lo++) {
        Byte c = s.p[lo];
        if (c >= UTF8_RUNE_SELF)
            /* If we run into a non-ASCII byte, fall back to the slower
             * unicode-aware method on the remaining bytes. */
            return strings_trim_func(tail_at(s, lo),
                                     BURROW_FN(RuneFunc, space_rune, NULL));
        if (ascii_space[c] != 0)
            continue;
        s = tail_at(s, lo);
        /* Now look for the first ASCII non-space byte from the end. */
        for (Int hi = s.len - 1; hi >= 0; hi--) {
            c = s.p[hi];
            if (c >= UTF8_RUNE_SELF)
                return strings_trim_right_func(sub(s, 0, hi + 1),
                                               BURROW_FN(RuneFunc, space_rune, NULL));
            if (ascii_space[c] == 0)
                return sub(s, 0, hi + 1);
        }
    }
    return BURROW_STR_EMPTY;
}

Str strings_trim_prefix(Str s, Str prefix) {
    return strings_has_prefix(s, prefix) ? tail_at(s, prefix.len) : s;
}

Str strings_trim_suffix(Str s, Str suffix) {
    return strings_has_suffix(s, suffix) ? sub(s, 0, s.len - suffix.len) : s;
}

/* ------------------------------------------------------------ splitting */

/* explode splits s into a slice of UTF-8 strings, one string per Unicode
 * character up to a maximum of n (n < 0 means no limit). Invalid UTF-8 bytes
 * are sliced individually. */
static Slice explode(Alloc *a, Str s, Int n) {
    Int l = utf8_rune_count_in_string(s);
    if (n < 0 || n > l)
        n = l;
    Slice out = str_slice(a, n);
    if (out.len != n)
        return slice_nil(TYPE_STRING);
    Str *arr = (Str *)out.p;
    for (Int i = 0; i < n - 1; i++) {
        Int size;
        decode_rune(s, &size);
        arr[i] = sub(s, 0, size);
        s = tail_at(s, size);
    }
    if (n > 0)
        arr[n - 1] = s;
    return out;
}

/* Generic split: splits after each instance of sep, including sep_save bytes
 * of sep in the subarrays. */
static Slice gen_split(Alloc *a, Str s, Str sep, Int sep_save, Int n) {
    if (n == 0)
        return slice_nil(TYPE_STRING);
    if (sep.len == 0)
        return explode(a, s, n);
    if (n < 0)
        n = strings_count(s, sep) + 1;
    if (n > s.len + 1)
        n = s.len + 1;
    Slice out = str_slice(a, n);
    if (out.len != n)
        return slice_nil(TYPE_STRING);
    Str *arr = (Str *)out.p;
    n--;
    Int i = 0;
    while (i < n) {
        Int m = strings_index(s, sep);
        if (m < 0)
            break;
        arr[i] = sub(s, 0, m + sep_save);
        s = tail_at(s, m + sep.len);
        i++;
    }
    arr[i] = s;
    out.len = i + 1;
    return out;
}

Slice strings_split_n(Alloc *a, Str s, Str sep, Int n) {
    return gen_split(a, s, sep, 0, n);
}

Slice strings_split_after_n(Alloc *a, Str s, Str sep, Int n) {
    return gen_split(a, s, sep, sep.len, n);
}

Slice strings_split(Alloc *a, Str s, Str sep) {
    return gen_split(a, s, sep, 0, -1);
}

Slice strings_split_after(Alloc *a, Str s, Str sep) {
    return gen_split(a, s, sep, sep.len, -1);
}

Slice strings_fields_func(Alloc *a, Str s, RuneFunc f) {
    /* Go records spans in a growing slice and then slices s. Counting first
     * and filling in on a second pass gives the same result with one
     * allocation, and f is documented as giving the same answer every time. */
    Int n = 0;
    bool in_field = false;
    Rune r;
    for (StrIter it = str_runes(s); str_next_rune(&it, NULL, &r);) {
        bool sep = BURROW_CALLF(f, r);
        if (!sep && !in_field)
            n++;
        in_field = !sep;
    }
    Slice out = str_slice(a, n);
    if (out.len != n)
        return slice_nil(TYPE_STRING);
    Str *arr = (Str *)out.p;
    Int na = 0;
    Int start = -1; /* valid span start if >= 0 */
    Int end;
    for (StrIter it = str_runes(s); str_next_rune(&it, &end, &r);) {
        if (BURROW_CALLF(f, r)) {
            if (start >= 0) {
                arr[na++] = sub(s, start, end);
                start = -1;
            }
        } else if (start < 0) {
            start = end;
        }
    }
    /* Last field might end at EOF. */
    if (start >= 0)
        arr[na++] = tail_at(s, start);
    out.len = na;
    return out;
}

Slice strings_fields(Alloc *a, Str s) {
    /* First count the fields. This is an exact count if s is ASCII, otherwise
     * it is an approximation. */
    Int n = 0;
    int was_space = 1;
    /* set_bits is used to track which bits are set in the bytes of s. */
    Byte set_bits = 0;
    for (Int i = 0; i < s.len; i++) {
        Byte r = s.p[i];
        set_bits |= r;
        int is_sp = ascii_space[r];
        n += was_space & ~is_sp;
        was_space = is_sp;
    }

    if (set_bits >= UTF8_RUNE_SELF)
        /* Some runes in the input string are not ASCII. */
        return strings_fields_func(a, s, BURROW_FN(RuneFunc, space_rune, NULL));

    /* ASCII fast path */
    Slice out = str_slice(a, n);
    if (out.len != n)
        return slice_nil(TYPE_STRING);
    Str *arr = (Str *)out.p;
    Int na = 0;
    Int field_start;
    Int i = 0;
    /* Skip spaces in the front of the input. */
    while (i < s.len && ascii_space[s.p[i]] != 0)
        i++;
    field_start = i;
    while (i < s.len) {
        if (ascii_space[s.p[i]] == 0) {
            i++;
            continue;
        }
        arr[na++] = sub(s, field_start, i);
        i++;
        /* Skip spaces in between fields. */
        while (i < s.len && ascii_space[s.p[i]] != 0)
            i++;
        field_start = i;
    }
    if (field_start < s.len) /* Last field might end at EOF. */
        arr[na] = tail_at(s, field_start);
    return out;
}

/* ------------------------------------------------------------ building */

Str strings_clone(Alloc *a, Str s) {
    return str_clone(a, s);
}

Str strings_join(Alloc *a, Slice elems, Str sep) {
    const Str *e = (const Str *)elems.p;
    switch (elems.len) {
    case 0:
        return BURROW_STR_EMPTY;
    case 1:
        return e[0];
    default:
        break;
    }
    Int n = 0;
    if (sep.len > 0) {
        if (sep.len >= BURROW_INT_MAX / (elems.len - 1))
            panic_str(BURROW_S("strings: Join output length overflow"));
        n += sep.len * (elems.len - 1);
    }
    for (Int i = 0; i < elems.len; i++) {
        if (e[i].len > BURROW_INT_MAX - n)
            panic_str(BURROW_S("strings: Join output length overflow"));
        n += e[i].len;
    }
    if (n == 0)
        return BURROW_STR_EMPTY;
    Byte *p = alloc_bytes(a, n);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    Int j = 0;
    for (Int i = 0; i < elems.len; i++) {
        if (i > 0 && sep.len > 0) {
            memcpy(p + j, sep.p, (size_t)sep.len);
            j += sep.len;
        }
        if (e[i].len > 0) {
            memcpy(p + j, e[i].p, (size_t)e[i].len);
            j += e[i].len;
        }
    }
    return mkstr(p, n);
}

/* According to static analysis, spaces, dashes, zeros, equals, and tabs are
 * the most commonly repeated string literal, often used for display on
 * fixed-width terminal windows. Pre-declare constants for these for O(1)
 * repetition in the common-case. */
#define REPEATED(c)                                                                    \
    c c c c c c c c c c c c c c c c c c c c c c c c c c c c c c c c c c c c c c c c c  \
        c c c c c c c c c c c c c c c c c c c c c c c
static const char repeated_spaces[] = REPEATED("  ");
static const char repeated_dashes[] = REPEATED("--");
static const char repeated_zeroes[] = REPEATED("0");
static const char repeated_equals[] = REPEATED("==");
static const char repeated_tabs[] = REPEATED("\t");
#undef REPEATED

static bool repeated(Str s, Int n, const char *lit, size_t lit_len, Str *out) {
    Str r = {(const Byte *)lit, (Int)lit_len};
    if (n <= r.len && strings_has_prefix(r, s)) {
        *out = sub(r, 0, n);
        return true;
    }
    return false;
}

Str strings_repeat(Alloc *a, Str s, Int count) {
    switch (count) {
    case 0:
        return BURROW_STR_EMPTY;
    case 1:
        return s;
    default:
        break;
    }
    /* Since we cannot return an error on overflow, we should panic if the
     * repeat will generate an overflow. See golang.org/issue/16237. */
    if (count < 0)
        panic_str(BURROW_S("strings: negative Repeat count"));
    if (s.len > 0 && count > BURROW_INT_MAX / s.len)
        panic_str(BURROW_S("strings: Repeat output length overflow"));
    Int n = s.len * count;
    if (s.len == 0)
        return BURROW_STR_EMPTY;

    /* Optimize for commonly repeated strings of relatively short length. */
    Str out;
    switch (s.p[0]) {
    case ' ':
    case '-':
    case '0':
    case '=':
    case '\t':
        if (repeated(s, n, repeated_spaces, sizeof repeated_spaces - 1, &out) ||
            repeated(s, n, repeated_dashes, sizeof repeated_dashes - 1, &out) ||
            repeated(s, n, repeated_zeroes, sizeof repeated_zeroes - 1, &out) ||
            repeated(s, n, repeated_equals, sizeof repeated_equals - 1, &out) ||
            repeated(s, n, repeated_tabs, sizeof repeated_tabs - 1, &out))
            return out;
        break;
    default:
        break;
    }

    /* Past a certain chunk size it is counterproductive to use larger chunks
     * as the source of the write, as when the source is too large we are
     * basically just thrashing the CPU D-cache. */
    const Int chunk_limit = 8 * 1024;
    Int chunk_max = n;
    if (n > chunk_limit) {
        chunk_max = chunk_limit / s.len * s.len;
        if (chunk_max == 0)
            chunk_max = s.len;
    }
    Byte *p = alloc_bytes(a, n);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    memcpy(p, s.p, (size_t)s.len);
    Int len = s.len;
    while (len < n) {
        Int chunk = n - len;
        if (chunk > len)
            chunk = len;
        if (chunk > chunk_max)
            chunk = chunk_max;
        memcpy(p + len, p, (size_t)chunk);
        len += chunk;
    }
    return mkstr(p, n);
}

Str strings_replace(Alloc *a, Str s, Str old, Str repl, Int n) {
    if (str_eq(old, repl) || n == 0)
        return s; /* avoid allocation */

    /* Compute number of replacements. */
    Int m = strings_count(s, old);
    if (m == 0)
        return s; /* avoid allocation */
    if (n < 0 || m < n)
        n = m;

    /* Apply replacements to buffer. */
    Int size = s.len + n * (repl.len - old.len);
    if (size == 0)
        return BURROW_STR_EMPTY;
    Byte *p = alloc_bytes(a, size);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    Int w = 0;
    Int start = 0;
#define REPLACE_PUT(src, len_)                                                         \
    do {                                                                               \
        if ((len_) > 0)                                                                \
            memcpy(p + w, (src), (size_t)(len_));                                      \
        w += (len_);                                                                   \
    } while (0)
    if (old.len > 0) {
        for (Int k = 0; k < n; k++) {
            Int j = start + strings_index(tail_at(s, start), old);
            REPLACE_PUT(s.p + start, j - start);
            REPLACE_PUT(repl.p, repl.len);
            start = j + old.len;
        }
    } else { /* old.len == 0 */
        REPLACE_PUT(repl.p, repl.len);
        for (Int k = 0; k < n - 1; k++) {
            Int wid;
            decode_rune(tail_at(s, start), &wid);
            Int j = start + wid;
            REPLACE_PUT(s.p + start, j - start);
            REPLACE_PUT(repl.p, repl.len);
            start = j;
        }
    }
    REPLACE_PUT(s.p + start, s.len - start);
#undef REPLACE_PUT
    return mkstr(p, size);
}

Str strings_replace_all(Alloc *a, Str s, Str old, Str repl) {
    return strings_replace(a, s, old, repl, -1);
}

/* A write to a builder that remembers a failure, so the loops below can run to
 * the end and check once. */
static void put_str(StringsBuilder *b, Str s, bool *ok) {
    Error err;
    strings_builder_write_string(b, s, &err);
    if (BURROW_FAILED(err))
        *ok = false;
}

static void map_put_rune(StringsBuilder *b, Rune r, bool *ok) {
    Error err;
    strings_builder_write_rune(b, r, &err);
    if (BURROW_FAILED(err))
        *ok = false;
}

static void map_put_byte(StringsBuilder *b, Byte c, bool *ok) {
    if (BURROW_FAILED(strings_builder_write_byte(b, c)))
        *ok = false;
}

static Str built(StringsBuilder *b, bool ok) {
    return ok ? strings_builder_string(b) : BURROW_STR_EMPTY;
}

Str strings_map(Alloc *a, RuneMapFunc mapping, Str s) {
    /* In the worst case, the string can grow when mapped, making things
     * unpleasant. But it's so rare we barge in assuming it's fine. It could
     * also shrink but that falls out naturally.
     *
     * The output buffer b is initialized on demand, the first time a
     * character differs. */
    StringsBuilder b = STRINGS_BUILDER(a);
    bool ok = true;

    Int i;
    Rune c;
    for (StrIter it = str_runes(s); str_next_rune(&it, &i, &c);) {
        Rune r = BURROW_CALLF(mapping, c);
        if (r == c && c != UTF8_RUNE_ERROR)
            continue;

        Int width;
        if (c == UTF8_RUNE_ERROR) {
            c = decode_rune(tail_at(s, i), &width);
            if (width != 1 && r == c)
                continue;
        } else {
            width = utf8_rune_len(c);
        }

        if (!strings_builder_grow(&b, s.len + UTF8_UTF_MAX))
            return BURROW_STR_EMPTY;
        put_str(&b, sub(s, 0, i), &ok);
        if (r >= 0)
            map_put_rune(&b, r, &ok);

        s = tail_at(s, i + width);
        break;
    }

    /* Fast path for unchanged input */
    if (strings_builder_cap(&b) == 0) /* didn't call b.Grow above */
        return s;

    for (StrIter it = str_runes(s); str_next_rune(&it, NULL, &c);) {
        Rune r = BURROW_CALLF(mapping, c);
        if (r >= 0) {
            if (r < UTF8_RUNE_SELF)
                map_put_byte(&b, (Byte)r, &ok);
            else
                map_put_rune(&b, r, &ok);
        }
    }
    return built(&b, ok);
}

/* The ASCII half of ToUpper and ToLower: s with the bytes from lo to hi moved
 * by delta, or s itself if there are none. */
static Str ascii_case(Alloc *a, Str s, Byte lo, Byte hi, int delta, bool any) {
    if (!any)
        return s;
    Byte *p = alloc_bytes(a, s.len);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    for (Int i = 0; i < s.len; i++) {
        Byte c = s.p[i];
        p[i] = lo <= c && c <= hi ? (Byte)(c + delta) : c;
    }
    return mkstr(p, s.len);
}

static Rune map_upper(void *env, Rune r) {
    (void)env;
    return unicode_to_upper(r);
}

static Rune map_lower(void *env, Rune r) {
    (void)env;
    return unicode_to_lower(r);
}

static Rune map_title(void *env, Rune r) {
    (void)env;
    return unicode_to_title(r);
}

Str strings_to_upper(Alloc *a, Str s) {
    bool has_lower = false;
    for (Int i = 0; i < s.len; i++) {
        Byte c = s.p[i];
        if (c >= UTF8_RUNE_SELF)
            return strings_map(a, BURROW_FN(RuneMapFunc, map_upper, NULL), s);
        has_lower = has_lower || ('a' <= c && c <= 'z');
    }
    return ascii_case(a, s, 'a', 'z', 'A' - 'a', has_lower);
}

Str strings_to_lower(Alloc *a, Str s) {
    bool has_upper = false;
    for (Int i = 0; i < s.len; i++) {
        Byte c = s.p[i];
        if (c >= UTF8_RUNE_SELF)
            return strings_map(a, BURROW_FN(RuneMapFunc, map_lower, NULL), s);
        has_upper = has_upper || ('A' <= c && c <= 'Z');
    }
    return ascii_case(a, s, 'A', 'Z', 'a' - 'A', has_upper);
}

Str strings_to_title(Alloc *a, Str s) {
    return strings_map(a, BURROW_FN(RuneMapFunc, map_title, NULL), s);
}

static Rune map_special_upper(void *env, Rune r) {
    return unicode_special_case_to_upper(*(const UnicodeSpecialCase *)env, r);
}

static Rune map_special_lower(void *env, Rune r) {
    return unicode_special_case_to_lower(*(const UnicodeSpecialCase *)env, r);
}

static Rune map_special_title(void *env, Rune r) {
    return unicode_special_case_to_title(*(const UnicodeSpecialCase *)env, r);
}

Str strings_to_upper_special(Alloc *a, UnicodeSpecialCase c, Str s) {
    return strings_map(a, BURROW_FN(RuneMapFunc, map_special_upper, &c), s);
}

Str strings_to_lower_special(Alloc *a, UnicodeSpecialCase c, Str s) {
    return strings_map(a, BURROW_FN(RuneMapFunc, map_special_lower, &c), s);
}

Str strings_to_title_special(Alloc *a, UnicodeSpecialCase c, Str s) {
    return strings_map(a, BURROW_FN(RuneMapFunc, map_special_title, &c), s);
}

Str strings_to_valid_utf8(Alloc *a, Str s, Str replacement) {
    StringsBuilder b = STRINGS_BUILDER(a);
    bool ok = true;

    Int i;
    Rune c;
    for (StrIter it = str_runes(s); str_next_rune(&it, &i, &c);) {
        if (c != UTF8_RUNE_ERROR)
            continue;
        Int wid;
        decode_rune(tail_at(s, i), &wid);
        if (wid == 1) {
            if (!strings_builder_grow(&b, s.len + replacement.len))
                return BURROW_STR_EMPTY;
            put_str(&b, sub(s, 0, i), &ok);
            s = tail_at(s, i);
            break;
        }
    }

    /* Fast path for unchanged input */
    if (strings_builder_cap(&b) == 0) /* didn't call b.Grow above */
        return s;

    bool invalid = false; /* previous byte was from an invalid UTF-8 sequence */
    for (Int j = 0; j < s.len;) {
        Byte ch = s.p[j];
        if (ch < UTF8_RUNE_SELF) {
            j++;
            invalid = false;
            map_put_byte(&b, ch, &ok);
            continue;
        }
        Int wid;
        decode_rune(tail_at(s, j), &wid);
        if (wid == 1) {
            j++;
            if (!invalid) {
                invalid = true;
                put_str(&b, replacement, &ok);
            }
            continue;
        }
        invalid = false;
        put_str(&b, sub(s, j, j + wid), &ok);
        j += wid;
    }
    return built(&b, ok);
}

/* isSeparator reports whether the rune could mark a word boundary. */
static bool is_separator(Rune r) {
    /* ASCII alphanumerics and underscore are not separators */
    if (r <= 0x7F) {
        if ('0' <= r && r <= '9')
            return false;
        if ('a' <= r && r <= 'z')
            return false;
        if ('A' <= r && r <= 'Z')
            return false;
        if (r == '_')
            return false;
        return true;
    }
    /* Letters and digits are not separators */
    if (unicode_is_letter(r) || unicode_is_digit(r))
        return false;
    /* Otherwise, all we can do for now is treat spaces as separators. */
    return unicode_is_space(r);
}

/* Title's closure, which remembers the rune before this one. It depends on Map
 * scanning in order and calling it once per rune, as Go's does. */
static Rune map_title_word(void *env, Rune r) {
    Rune *prev = (Rune *)env;
    if (is_separator(*prev)) {
        *prev = r;
        return unicode_to_title(r);
    }
    *prev = r;
    return r;
}

Str strings_title(Alloc *a, Str s) {
    Rune prev = ' ';
    return strings_map(a, BURROW_FN(RuneMapFunc, map_title_word, &prev), s);
}

/* The Str to Slice view that Replacer and Reader need to hand a string to an
 * io.Writer without a copy. The writer does not write through it. */
Slice burrow__strings_bytes(Str s) {
    return slice_from(unconst_str(s.p), s.len, s.len, TYPE_BYTE);
}

/* Derived from Go's src/bytes/bytes.go.
 * Go source: go1.27.1.
 *
 * Searching a []byte is searching a string, and Go's two packages carry the
 * same algorithms twice. Here the search functions look at the Slice as a Str
 * and call strings, so there is one copy of Index and its cutovers. What bytes
 * does differently is what it hands back: slices of the input with Go's
 * capacities, nil where Go returns nil, and a fresh slice from every function
 * that builds one. That part is ported line by line.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/bytes.h"

#include "burrow/core.h"
#include "burrow/func.h"
#include "burrow/mem.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/strings.h"
#include "burrow/type.h"
#include "burrow/unicode.h"
#include "burrow/utf8.h"

#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------ helpers */

/* s as a Str, to search it. */
static Str bview(Slice s) {
    Str v = {(const Byte *)s.p, s.len};
    return v;
}

static Byte *bptr(Slice s) {
    return (Byte *)s.p;
}

/* Go's s[lo:hi]. The nil slice stays nil when lo is zero, and NULL + 0 is
 * undefined in C, so an offset of zero leaves p alone. */
static Slice bsub(Slice s, Int lo, Int hi) {
    Slice r = {lo == 0 ? s.p : bptr(s) + lo, hi - lo, s.cap - lo, TYPE_BYTE};
    return r;
}

/* Go's s[lo:hi:max]. */
static Slice bsub3(Slice s, Int lo, Int hi, Int max) {
    Slice r = {lo == 0 ? s.p : bptr(s) + lo, hi - lo, max - lo, TYPE_BYTE};
    return r;
}

static Slice bnil(void) {
    return slice_nil(TYPE_BYTE);
}

/* A new []byte of length n, not nil even when n is zero, or nil when a cannot
 * give it. */
static Slice bmake(Alloc *a, Int n) {
    if (n == 0)
        return slice_make(a, TYPE_BYTE, 0, 0);
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)n, 1);
    if (p == NULL)
        return bnil();
    return slice_from(p, n, n, TYPE_BYTE);
}

/* Go's append([]byte{}, s...) when empty is false and append([]byte(nil), s...)
 * when it is true: the two differ only when s is empty. */
static Slice bdup(Alloc *a, Slice s, bool nil_when_empty) {
    if (s.len == 0)
        return nil_when_empty ? bnil() : bmake(a, 0);
    Slice out = bmake(a, s.len);
    if (out.p != NULL)
        memcpy(out.p, s.p, (size_t)s.len);
    return out;
}

static Rune bdecode(Slice s, Int i, Int *size) {
    return utf8_decode_rune_in_string(bview(bsub(s, i, s.len)), size);
}

/* A []byte that grows, for the functions whose result length is not known up
 * front. ok goes false on the first failed allocation and stays there. */
typedef struct BytesOut {
    Alloc *a;
    Byte *p;
    Int len;
    Int cap;
    bool ok;
} BytesOut;

static void bout_init(BytesOut *o, Alloc *a, Int cap) {
    o->a = a;
    o->p = NULL;
    o->len = 0;
    o->cap = 0;
    o->ok = true;
    if (cap > 0) {
        o->p = (Byte *)mem_alloc_nozero(a, (size_t)cap, 1);
        if (o->p == NULL)
            o->ok = false;
        else
            o->cap = cap;
    }
}

static bool bout_room(BytesOut *o, Int n) {
    if (!o->ok)
        return false;
    if (o->cap - o->len >= n)
        return true;
    Int ncap = o->cap * 2;
    if (ncap < o->len + n)
        ncap = o->len + n;
    if (ncap < 8)
        ncap = 8;
    Byte *np = o->p == NULL
                   ? (Byte *)mem_alloc_nozero(o->a, (size_t)ncap, 1)
                   : (Byte *)mem_realloc(o->a, o->p, (size_t)o->cap, (size_t)ncap, 1);
    if (np == NULL) {
        o->ok = false;
        return false;
    }
    o->p = np;
    o->cap = ncap;
    return true;
}

static void bout_bytes(BytesOut *o, const void *p, Int n) {
    if (n > 0 && bout_room(o, n)) {
        memcpy(o->p + o->len, p, (size_t)n);
        o->len += n;
    }
}

static void bout_rune(BytesOut *o, Rune r) {
    if (bout_room(o, UTF8_UTF_MAX)) {
        Slice room = slice_from(o->p + o->len, UTF8_UTF_MAX, UTF8_UTF_MAX, TYPE_BYTE);
        o->len += utf8_encode_rune(room, r);
    }
}

/* The result, which is never nil when it worked, as Go's make([]byte, 0, n)
 * is not. */
static Slice bout_done(BytesOut *o) {
    if (!o->ok) {
        if (o->p != NULL)
            mem_free(o->a, o->p, (size_t)o->cap, 1);
        return bnil();
    }
    if (o->p == NULL)
        return slice_make(o->a, TYPE_BYTE, 0, 0);
    return slice_from(o->p, o->len, o->cap, TYPE_BYTE);
}

/* ------------------------------------------------------------ comparing */

bool bytes_equal(Slice a, Slice b) {
    return str_eq(bview(a), bview(b));
}

Int bytes_compare(Slice a, Slice b) {
    return strings_compare(bview(a), bview(b));
}

bool bytes_equal_fold(Slice s, Slice t) {
    return strings_equal_fold(bview(s), bview(t));
}

bool bytes_has_prefix(Slice s, Slice prefix) {
    return strings_has_prefix(bview(s), bview(prefix));
}

bool bytes_has_suffix(Slice s, Slice suffix) {
    return strings_has_suffix(bview(s), bview(suffix));
}

/* ------------------------------------------------------------ searching */

bool bytes_contains(Slice b, Slice subslice) {
    return strings_contains(bview(b), bview(subslice));
}

bool bytes_contains_any(Slice b, Str chars) {
    return strings_contains_any(bview(b), chars);
}

bool bytes_contains_rune(Slice b, Rune r) {
    return strings_contains_rune(bview(b), r);
}

bool bytes_contains_func(Slice b, RuneFunc f) {
    return strings_contains_func(bview(b), f);
}

Int bytes_count(Slice s, Slice sep) {
    return strings_count(bview(s), bview(sep));
}

Int bytes_index(Slice s, Slice sep) {
    return strings_index(bview(s), bview(sep));
}

Int bytes_index_any(Slice s, Str chars) {
    return strings_index_any(bview(s), chars);
}

Int bytes_index_byte(Slice b, Byte c) {
    return strings_index_byte(bview(b), c);
}

Int bytes_index_func(Slice s, RuneFunc f) {
    return strings_index_func(bview(s), f);
}

Int bytes_index_rune(Slice s, Rune r) {
    return strings_index_rune(bview(s), r);
}

Int bytes_last_index(Slice s, Slice sep) {
    return strings_last_index(bview(s), bview(sep));
}

Int bytes_last_index_any(Slice s, Str chars) {
    return strings_last_index_any(bview(s), chars);
}

Int bytes_last_index_byte(Slice s, Byte c) {
    return strings_last_index_byte(bview(s), c);
}

Int bytes_last_index_func(Slice s, RuneFunc f) {
    return strings_last_index_func(bview(s), f);
}

/* ------------------------------------------------------------ cutting */

Slice bytes_cut(Slice s, Slice sep, Slice *after, bool *found) {
    Int i = bytes_index(s, sep);
    if (i >= 0) {
        BURROW_OUT(after, bsub(s, i + sep.len, s.len));
        BURROW_OUT(found, true);
        return bsub(s, 0, i);
    }
    BURROW_OUT(after, bnil());
    BURROW_OUT(found, false);
    return s;
}

Slice bytes_cut_last(Slice s, Slice sep, Slice *after, bool *found) {
    Int i = bytes_last_index(s, sep);
    if (i >= 0) {
        BURROW_OUT(after, bsub(s, i + sep.len, s.len));
        BURROW_OUT(found, true);
        return bsub(s, 0, i);
    }
    BURROW_OUT(after, bnil());
    BURROW_OUT(found, false);
    return s;
}

Slice bytes_cut_prefix(Slice s, Slice prefix, bool *found) {
    if (!bytes_has_prefix(s, prefix)) {
        BURROW_OUT(found, false);
        return s;
    }
    BURROW_OUT(found, true);
    return bsub(s, prefix.len, s.len);
}

Slice bytes_cut_suffix(Slice s, Slice suffix, bool *found) {
    if (!bytes_has_suffix(s, suffix)) {
        BURROW_OUT(found, false);
        return s;
    }
    BURROW_OUT(found, true);
    return bsub(s, 0, s.len - suffix.len);
}

/* ------------------------------------------------------------ trimming */

static Int bytes_index_func_truth(Slice s, RuneFunc f, bool truth) {
    Int start = 0;
    while (start < s.len) {
        Int wid;
        Rune r = bdecode(s, start, &wid);
        if (BURROW_CALLF(f, r) == truth)
            return start;
        start += wid;
    }
    return -1;
}

static Int bytes_last_index_func_truth(Slice s, RuneFunc f, bool truth) {
    for (Int i = s.len; i > 0;) {
        Rune r = bptr(s)[i - 1];
        Int size = 1;
        if (r >= UTF8_RUNE_SELF)
            r = utf8_decode_last_rune_in_string(bview(bsub(s, 0, i)), &size);
        i -= size;
        if (BURROW_CALLF(f, r) == truth)
            return i;
    }
    return -1;
}

Slice bytes_trim_left_func(Slice s, RuneFunc f) {
    Int i = bytes_index_func_truth(s, f, false);
    if (i == -1)
        return bnil();
    return bsub(s, i, s.len);
}

Slice bytes_trim_right_func(Slice s, RuneFunc f) {
    Int i = bytes_last_index_func_truth(s, f, false);
    if (i >= 0 && bptr(s)[i] >= UTF8_RUNE_SELF) {
        Int wid;
        bdecode(s, i, &wid);
        i += wid;
    } else {
        i++;
    }
    return bsub(s, 0, i);
}

Slice bytes_trim_func(Slice s, RuneFunc f) {
    return bytes_trim_right_func(bytes_trim_left_func(s, f), f);
}

Slice bytes_trim_prefix(Slice s, Slice prefix) {
    if (bytes_has_prefix(s, prefix))
        return bsub(s, prefix.len, s.len);
    return s;
}

Slice bytes_trim_suffix(Slice s, Slice suffix) {
    if (bytes_has_suffix(s, suffix))
        return bsub(s, 0, s.len - suffix.len);
    return s;
}

typedef struct BytesAsciiSet {
    bool has[256];
} BytesAsciiSet;

static bool bytes_make_ascii_set(BytesAsciiSet *as, Str chars) {
    memset(as, 0, sizeof *as);
    for (Int i = 0; i < chars.len; i++) {
        Byte c = chars.p[i];
        if (c >= UTF8_RUNE_SELF)
            return false;
        as->has[c] = true;
    }
    return true;
}

static bool bytes_cutset_has(Str cutset, Rune r) {
    for (Int i = 0; i < cutset.len;) {
        Int size;
        Str rest = {cutset.p + i, cutset.len - i};
        if (utf8_decode_rune_in_string(rest, &size) == r)
            return true;
        i += size;
    }
    return false;
}

static Slice bnil_if_empty(Slice s) {
    return s.len == 0 ? bnil() : s;
}

static Slice btrim_left_byte(Slice s, Byte c) {
    Int i = 0;
    while (i < s.len && bptr(s)[i] == c)
        i++;
    return bnil_if_empty(bsub(s, i, s.len));
}

static Slice btrim_left_ascii(Slice s, const BytesAsciiSet *as) {
    Int i = 0;
    while (i < s.len && as->has[bptr(s)[i]])
        i++;
    return bnil_if_empty(bsub(s, i, s.len));
}

static Slice btrim_left_unicode(Slice s, Str cutset) {
    Int i = 0;
    while (i < s.len) {
        Int n;
        Rune r = bdecode(s, i, &n);
        if (!bytes_cutset_has(cutset, r))
            break;
        i += n;
    }
    return bnil_if_empty(bsub(s, i, s.len));
}

static Slice btrim_right_byte(Slice s, Byte c) {
    Int n = s.len;
    while (n > 0 && bptr(s)[n - 1] == c)
        n--;
    return bsub(s, 0, n);
}

static Slice btrim_right_ascii(Slice s, const BytesAsciiSet *as) {
    Int n = s.len;
    while (n > 0 && as->has[bptr(s)[n - 1]])
        n--;
    return bsub(s, 0, n);
}

static Slice btrim_right_unicode(Slice s, Str cutset) {
    Int n = s.len;
    while (n > 0) {
        Rune r = bptr(s)[n - 1];
        Int size = 1;
        if (r >= UTF8_RUNE_SELF)
            r = utf8_decode_last_rune_in_string(bview(bsub(s, 0, n)), &size);
        if (!bytes_cutset_has(cutset, r))
            break;
        n -= size;
    }
    return bsub(s, 0, n);
}

Slice bytes_trim(Slice s, Str cutset) {
    if (s.len == 0)
        return bnil();
    if (cutset.len == 0)
        return s;
    if (cutset.len == 1 && cutset.p[0] < UTF8_RUNE_SELF)
        return btrim_left_byte(btrim_right_byte(s, cutset.p[0]), cutset.p[0]);
    BytesAsciiSet as;
    if (bytes_make_ascii_set(&as, cutset))
        return btrim_left_ascii(btrim_right_ascii(s, &as), &as);
    return btrim_left_unicode(btrim_right_unicode(s, cutset), cutset);
}

Slice bytes_trim_left(Slice s, Str cutset) {
    if (s.len == 0)
        return bnil();
    if (cutset.len == 0)
        return s;
    if (cutset.len == 1 && cutset.p[0] < UTF8_RUNE_SELF)
        return btrim_left_byte(s, cutset.p[0]);
    BytesAsciiSet as;
    if (bytes_make_ascii_set(&as, cutset))
        return btrim_left_ascii(s, &as);
    return btrim_left_unicode(s, cutset);
}

Slice bytes_trim_right(Slice s, Str cutset) {
    if (s.len == 0 || cutset.len == 0)
        return s;
    if (cutset.len == 1 && cutset.p[0] < UTF8_RUNE_SELF)
        return btrim_right_byte(s, cutset.p[0]);
    BytesAsciiSet as;
    if (bytes_make_ascii_set(&as, cutset))
        return btrim_right_ascii(s, &as);
    return btrim_right_unicode(s, cutset);
}

static const Byte bytes_ascii_space[256] = {
    ['\t'] = 1, ['\n'] = 1, ['\v'] = 1, ['\f'] = 1, ['\r'] = 1, [' '] = 1};

static bool bytes_is_space(void *env, Rune r) {
    (void)env;
    return unicode_is_space(r);
}

Slice bytes_trim_space(Slice s) {
    RuneFunc space = BURROW_FN(RuneFunc, bytes_is_space, NULL);
    for (Int lo = 0; lo < s.len; lo++) {
        Byte c = bptr(s)[lo];
        if (c >= UTF8_RUNE_SELF)
            return bytes_trim_func(bsub(s, lo, s.len), space);
        if (bytes_ascii_space[c] != 0)
            continue;
        Slice t = bsub(s, lo, s.len);
        for (Int hi = t.len - 1; hi >= 0; hi--) {
            Byte d = bptr(t)[hi];
            if (d >= UTF8_RUNE_SELF)
                return bytes_trim_func(bsub(t, 0, hi + 1), space);
            if (bytes_ascii_space[d] == 0)
                return bsub(t, 0, hi + 1);
        }
    }
    return bnil();
}

/* ------------------------------------------------------------ splitting */

static Slice bpieces(Alloc *a, Int n) {
    return slice_make(a, TYPE_BYTES, n, n);
}

static Slice *bpiece(Slice out, Int i) {
    return (Slice *)out.p + i;
}

/* Splits s into its UTF-8 sequences, at most n of them, the last holding the
 * rest. */
static Slice bexplode(Alloc *a, Slice s, Int n) {
    if (n <= 0 || n > s.len)
        n = s.len;
    Slice out = bpieces(a, n);
    if (out.p == NULL)
        return out;
    Int na = 0;
    while (s.len > 0) {
        if (na + 1 >= n) {
            *bpiece(out, na++) = s;
            break;
        }
        Int size;
        bdecode(s, 0, &size);
        *bpiece(out, na++) = bsub3(s, 0, size, size);
        s = bsub(s, size, s.len);
    }
    out.len = na;
    return out;
}

static Slice bgen_split(Alloc *a, Slice s, Slice sep, Int sep_save, Int n) {
    if (n == 0)
        return slice_nil(TYPE_BYTES);
    if (sep.len == 0)
        return bexplode(a, s, n);
    if (n < 0)
        n = bytes_count(s, sep) + 1;
    if (n > s.len + 1)
        n = s.len + 1;
    Slice out = bpieces(a, n);
    if (out.p == NULL)
        return out;
    n--;
    Int i = 0;
    while (i < n) {
        Int m = bytes_index(s, sep);
        if (m < 0)
            break;
        *bpiece(out, i) = bsub3(s, 0, m + sep_save, m + sep_save);
        s = bsub(s, m + sep.len, s.len);
        i++;
    }
    *bpiece(out, i) = s;
    out.len = i + 1;
    return out;
}

Slice bytes_split_n(Alloc *a, Slice s, Slice sep, Int n) {
    return bgen_split(a, s, sep, 0, n);
}

Slice bytes_split_after_n(Alloc *a, Slice s, Slice sep, Int n) {
    return bgen_split(a, s, sep, sep.len, n);
}

Slice bytes_split(Alloc *a, Slice s, Slice sep) {
    return bgen_split(a, s, sep, 0, -1);
}

Slice bytes_split_after(Alloc *a, Slice s, Slice sep) {
    return bgen_split(a, s, sep, sep.len, -1);
}

Slice bytes_fields_func(Alloc *a, Slice s, RuneFunc f) {
    /* Go records the spans first and then makes the slices. Counting them
     * first does the same with one allocation. */
    Int n = 0;
    Int start = -1;
    for (int pass = 0; pass < 2; pass++) {
        Slice out = {0};
        if (pass == 1) {
            out = bpieces(a, n);
            if (out.p == NULL)
                return out;
        }
        Int na = 0;
        start = -1;
        for (Int i = 0; i < s.len;) {
            Int size;
            Rune r = bdecode(s, i, &size);
            if (BURROW_CALLF(f, r)) {
                if (start >= 0) {
                    if (pass == 1)
                        *bpiece(out, na) = bsub3(s, start, i, i);
                    na++;
                    start = -1;
                }
            } else if (start < 0) {
                start = i;
            }
            i += size;
        }
        if (start >= 0) {
            if (pass == 1)
                *bpiece(out, na) = bsub3(s, start, s.len, s.len);
            na++;
        }
        if (pass == 0)
            n = na;
        else
            return out;
    }
    return slice_nil(TYPE_BYTES); /* not reached */
}

Slice bytes_fields(Alloc *a, Slice s) {
    /* First count the fields. This is an exact count if s is ASCII, otherwise
     * it is an approximation. */
    Int n = 0;
    int was_space = 1;
    Byte set_bits = 0;
    for (Int i = 0; i < s.len; i++) {
        Byte r = bptr(s)[i];
        set_bits |= r;
        int is_space = bytes_ascii_space[r];
        n += was_space & ~is_space;
        was_space = is_space;
    }
    if (set_bits >= UTF8_RUNE_SELF)
        return bytes_fields_func(a, s, BURROW_FN(RuneFunc, bytes_is_space, NULL));

    Slice out = bpieces(a, n);
    if (out.p == NULL)
        return out;
    Int na = 0;
    Int i = 0;
    while (i < s.len && bytes_ascii_space[bptr(s)[i]] != 0)
        i++;
    Int field_start = i;
    while (i < s.len) {
        if (bytes_ascii_space[bptr(s)[i]] == 0) {
            i++;
            continue;
        }
        *bpiece(out, na++) = bsub3(s, field_start, i, i);
        i++;
        while (i < s.len && bytes_ascii_space[bptr(s)[i]] != 0)
            i++;
        field_start = i;
    }
    if (field_start < s.len)
        *bpiece(out, na) = bsub3(s, field_start, s.len, s.len);
    return out;
}

/* ------------------------------------------------------------ building */

Slice bytes_clone(Alloc *a, Slice b) {
    if (slice_is_nil(b))
        return bnil();
    return bdup(a, b, false);
}

Slice bytes_join(Alloc *a, Slice s, Slice sep) {
    if (s.len == 0)
        return bmake(a, 0);
    const Slice *e = (const Slice *)s.p;
    if (s.len == 1)
        return bdup(a, e[0], true);
    Int n = 0;
    if (sep.len > 0) {
        if (sep.len >= BURROW_INT_MAX / (s.len - 1))
            panic_str(BURROW_S("bytes: Join output length overflow"));
        n += sep.len * (s.len - 1);
    }
    for (Int i = 0; i < s.len; i++) {
        if (e[i].len > BURROW_INT_MAX - n)
            panic_str(BURROW_S("bytes: Join output length overflow"));
        n += e[i].len;
    }
    Slice out = bmake(a, n);
    if (out.p == NULL)
        return out;
    Byte *p = bptr(out);
    Int bp = 0;
    if (e[0].len > 0) {
        memcpy(p, e[0].p, (size_t)e[0].len);
        bp = e[0].len;
    }
    for (Int i = 1; i < s.len; i++) {
        if (sep.len > 0) {
            memcpy(p + bp, sep.p, (size_t)sep.len);
            bp += sep.len;
        }
        if (e[i].len > 0) {
            memcpy(p + bp, e[i].p, (size_t)e[i].len);
            bp += e[i].len;
        }
    }
    return out;
}

Slice bytes_repeat(Alloc *a, Slice b, Int count) {
    if (count == 0)
        return bmake(a, 0);
    /* Since we cannot return an error on overflow, we should panic if the
     * repeat will generate an overflow. See golang.org/issue/16237. */
    if (count < 0)
        panic_str(BURROW_S("bytes: negative Repeat count"));
    if (b.len > 0 && count > BURROW_INT_MAX / b.len)
        panic_str(BURROW_S("bytes: Repeat output length overflow"));
    Int n = b.len * count;
    if (b.len == 0)
        return bmake(a, 0);

    /* Past a certain chunk size it is counterproductive to use larger chunks
     * as the source of the write, as when the source is too large we are
     * basically just thrashing the CPU D-cache. */
    const Int chunk_limit = 8 * 1024;
    Int chunk_max = n;
    if (chunk_max > chunk_limit) {
        chunk_max = chunk_limit / b.len * b.len;
        if (chunk_max == 0)
            chunk_max = b.len;
    }
    Slice out = bmake(a, n);
    if (out.p == NULL)
        return out;
    Byte *p = bptr(out);
    memcpy(p, b.p, (size_t)b.len);
    Int bp = b.len;
    while (bp < n) {
        Int chunk = bp < chunk_max ? bp : chunk_max;
        if (chunk > n - bp)
            chunk = n - bp;
        memcpy(p + bp, p, (size_t)chunk);
        bp += chunk;
    }
    return out;
}

Slice bytes_replace(Alloc *a, Slice s, Slice old, Slice repl, Int n) {
    Int m = 0;
    if (n != 0) {
        /* Compute number of replacements. */
        m = bytes_count(s, old);
    }
    if (m == 0) {
        /* Just return a copy. */
        return bdup(a, s, true);
    }
    if (n < 0 || m < n)
        n = m;

    /* Apply replacements to buffer. */
    Slice t = bmake(a, s.len + n * (repl.len - old.len));
    if (t.p == NULL)
        return t;
    Byte *p = bptr(t);
    Int w = 0;
    Int start = 0;
#define BYTES_PUT(src, len_)                                                           \
    do {                                                                               \
        if ((len_) > 0)                                                                \
            memcpy(p + w, (src), (size_t)(len_));                                      \
        w += (len_);                                                                   \
    } while (0)
    if (old.len > 0) {
        for (Int k = 0; k < n; k++) {
            Int j = start + bytes_index(bsub(s, start, s.len), old);
            BYTES_PUT(bptr(s) + start, j - start);
            BYTES_PUT(repl.p, repl.len);
            start = j + old.len;
        }
    } else { /* old.len == 0 */
        BYTES_PUT(repl.p, repl.len);
        for (Int k = 0; k < n - 1; k++) {
            Int wid;
            bdecode(s, start, &wid);
            Int j = start + wid;
            BYTES_PUT(bptr(s) + start, j - start);
            BYTES_PUT(repl.p, repl.len);
            start = j;
        }
    }
    BYTES_PUT(bptr(s) + start, s.len - start);
#undef BYTES_PUT
    t.len = w;
    return t;
}

Slice bytes_replace_all(Alloc *a, Slice s, Slice old, Slice repl) {
    return bytes_replace(a, s, old, repl, -1);
}

Slice bytes_map(Alloc *a, RuneMapFunc mapping, Slice s) {
    /* In the worst case, the slice can grow when mapped, making things
     * unpleasant. But it's so rare we barge in assuming it's fine. It could
     * also shrink but that falls out naturally. */
    BytesOut o;
    bout_init(&o, a, s.len);
    for (Int i = 0; i < s.len;) {
        Int wid;
        Rune r = bdecode(s, i, &wid);
        r = BURROW_CALLF(mapping, r);
        if (r >= 0)
            bout_rune(&o, r);
        i += wid;
    }
    return bout_done(&o);
}

Slice bytes_runes(Alloc *a, Slice s) {
    Int n = utf8_rune_count(s);
    Slice t = slice_make(a, TYPE_RUNE, n, n);
    if (t.p == NULL)
        return t;
    Rune *rp = (Rune *)t.p;
    Int i = 0;
    for (Int k = 0; k < s.len;) {
        Int l;
        rp[i++] = bdecode(s, k, &l);
        k += l;
    }
    return t;
}

static Slice bascii_case(Alloc *a, Slice s, Byte lo, Byte hi, int delta, bool any) {
    if (!any)
        return bdup(a, s, false);
    Slice b = bmake(a, s.len);
    if (b.p == NULL)
        return b;
    for (Int i = 0; i < s.len; i++) {
        Byte c = bptr(s)[i];
        if (lo <= c && c <= hi)
            c = (Byte)(c + delta);
        bptr(b)[i] = c;
    }
    return b;
}

static Rune bytes_map_upper(void *env, Rune r) {
    (void)env;
    return unicode_to_upper(r);
}

static Rune bytes_map_lower(void *env, Rune r) {
    (void)env;
    return unicode_to_lower(r);
}

static Rune bytes_map_title(void *env, Rune r) {
    (void)env;
    return unicode_to_title(r);
}

Slice bytes_to_upper(Alloc *a, Slice s) {
    bool has_lower = false;
    for (Int i = 0; i < s.len; i++) {
        Byte c = bptr(s)[i];
        if (c >= UTF8_RUNE_SELF)
            return bytes_map(a, BURROW_FN(RuneMapFunc, bytes_map_upper, NULL), s);
        has_lower = has_lower || ('a' <= c && c <= 'z');
    }
    return bascii_case(a, s, 'a', 'z', 'A' - 'a', has_lower);
}

Slice bytes_to_lower(Alloc *a, Slice s) {
    bool has_upper = false;
    for (Int i = 0; i < s.len; i++) {
        Byte c = bptr(s)[i];
        if (c >= UTF8_RUNE_SELF)
            return bytes_map(a, BURROW_FN(RuneMapFunc, bytes_map_lower, NULL), s);
        has_upper = has_upper || ('A' <= c && c <= 'Z');
    }
    return bascii_case(a, s, 'A', 'Z', 'a' - 'A', has_upper);
}

Slice bytes_to_title(Alloc *a, Slice s) {
    return bytes_map(a, BURROW_FN(RuneMapFunc, bytes_map_title, NULL), s);
}

static Rune bytes_special_upper(void *env, Rune r) {
    return unicode_special_case_to_upper(*(const UnicodeSpecialCase *)env, r);
}

static Rune bytes_special_lower(void *env, Rune r) {
    return unicode_special_case_to_lower(*(const UnicodeSpecialCase *)env, r);
}

static Rune bytes_special_title(void *env, Rune r) {
    return unicode_special_case_to_title(*(const UnicodeSpecialCase *)env, r);
}

Slice bytes_to_upper_special(Alloc *a, UnicodeSpecialCase c, Slice s) {
    return bytes_map(a, BURROW_FN(RuneMapFunc, bytes_special_upper, &c), s);
}

Slice bytes_to_lower_special(Alloc *a, UnicodeSpecialCase c, Slice s) {
    return bytes_map(a, BURROW_FN(RuneMapFunc, bytes_special_lower, &c), s);
}

Slice bytes_to_title_special(Alloc *a, UnicodeSpecialCase c, Slice s) {
    return bytes_map(a, BURROW_FN(RuneMapFunc, bytes_special_title, &c), s);
}

Slice bytes_to_valid_utf8(Alloc *a, Slice s, Slice replacement) {
    BytesOut o;
    bout_init(&o, a, s.len + replacement.len);
    bool invalid = false; /* previous byte was from an invalid UTF-8 sequence */
    for (Int i = 0; i < s.len;) {
        Byte c = bptr(s)[i];
        if (c < UTF8_RUNE_SELF) {
            i++;
            invalid = false;
            bout_bytes(&o, &c, 1);
            continue;
        }
        Int wid;
        bdecode(s, i, &wid);
        if (wid == 1) {
            i++;
            if (!invalid) {
                invalid = true;
                bout_bytes(&o, replacement.p, replacement.len);
            }
            continue;
        }
        invalid = false;
        bout_bytes(&o, bptr(s) + i, wid);
        i += wid;
    }
    return bout_done(&o);
}

/* isSeparator reports whether the rune could mark a word boundary. */
static bool bytes_is_separator(Rune r) {
    /* ASCII alphanumerics and underscore are not separators. */
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
    /* Letters and digits are not separators. */
    if (unicode_is_letter(r) || unicode_is_digit(r))
        return false;
    /* Otherwise, all we can do for now is treat spaces as separators. */
    return unicode_is_space(r);
}

static Rune bytes_map_title_word(void *env, Rune r) {
    Rune *prev = (Rune *)env;
    if (bytes_is_separator(*prev)) {
        *prev = r;
        return unicode_to_title(r);
    }
    *prev = r;
    return r;
}

Slice bytes_title(Alloc *a, Slice s) {
    /* Use a closure here to remember state. Hackish but effective. Relies on
     * Map scanning in order and calling the closure once per rune. */
    Rune prev = ' ';
    return bytes_map(a, BURROW_FN(RuneMapFunc, bytes_map_title_word, &prev), s);
}

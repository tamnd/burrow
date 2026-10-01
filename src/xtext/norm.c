/* Derived from Go's src/vendor/golang.org/x/text/unicode/norm/normalize.go.
 * Go source: go1.27.1, golang.org/x/text v0.37.0.
 *
 * The rest of the package is here too: composition.go, forminfo.go, input.go,
 * iter.go, readwriter.go, transform.go and the lookup half of trie.go and the
 * tables file. The order below follows the order of those files.
 *
 * Two things Go gets from its runtime are done by hand. One is append. The
 * output of Append can depend on how much room out has, because patchTail
 * keeps reading a slice of out while it appends to it, so out grows here the
 * way Go's growslice grows a byte slice, size classes and all, and an array
 * that is still being read is kept until the reading is done. The other is the
 * bounds checks, where Go's code panics with an index out of range this does
 * too, with the same text.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "norm.h"
#include "../runtime/growslice.h"
#include "norm_tables.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/panic.h"
#include "burrow/runtime.h"
#include "burrow/slice.h"
#include "burrow/type.h"
#include "burrow/utf8.h"

#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------------ helpers */

static Slice norm_bytes_of(Byte *p, Int len, Int cap) {
    Slice s = {p, len, cap, TYPE_BYTE};
    return s;
}

static Slice static_bytes(const Byte *p, Int len, Int cap) {
    return norm_bytes_of((Byte *)(uintptr_t)p, len, cap);
}

static void check_index(Int i, Int len) {
    if ((uint64_t)i >= (uint64_t)len)
        runtime_index_out_of_range(i, len);
}

static void check_slice(Int lo, Int hi, Int cap) {
    if (lo < 0 || hi < lo || hi > cap)
        runtime_slice_bounds_out_of_range(lo, hi, cap);
}

/* b[lo:], for a byte slice that may be all zeroes. */
static Slice btail(Slice b, Int lo) {
    check_slice(lo, b.len, b.len);
    if (b.p == NULL)
        return b;
    return norm_bytes_of((Byte *)b.p + lo, b.len - lo, b.cap - lo);
}

static Int min_int(Int a, Int b) {
    return a < b ? a : b;
}

/* copy(dst[:dlen], src[:n]), which may overlap. */
static Int norm_copy_bytes(Byte *dst, Int dlen, const Byte *src, Int n) {
    Int m = min_int(dlen, n);
    if (m > 0)
        memmove(dst, src, (size_t)m);
    return m;
}

static void nil_func_call(void) {
    runtime_panic(
        BURROW_S("runtime error: invalid memory address or nil pointer dereference"));
}

/* utf8.EncodeRune into p[:len], which touches the last byte it needs first and
 * so panics on a short p with that byte's index. */
static Int encode_rune(Byte *p, Int len, Rune r) {
    uint32_t u = (uint32_t)r;
    Int need = 4;
    if (u <= 0x7F)
        need = 1;
    else if (u <= 0x7FF)
        need = 2;
    else if (u <= 0xFFFF || u > 0x10FFFF)
        need = 3;
    if (len < need)
        runtime_index_out_of_range(need - 1, len);
    return utf8_encode_rune(norm_bytes_of(p, len, len), r);
}

static Rune norm_decode_rune(const Byte *p, Int len, Int *size) {
    return utf8_decode_rune(static_bytes(p, len, len), size);
}

/* ------------------------------------------------------------------- append */

/* Gives back the array rb.out had before it grew, unless the caller owns it
 * or patchTail is still reading it, in which case it is kept until then. */
static void release_out(NormReorderBuffer *rb, Byte *p, Int cap) {
    if (!rb->own_out || p == NULL)
        return;
    if (p == rb->pin) {
        rb->retired = norm_bytes_of(p, 0, cap);
        return;
    }
    mem_free(rb->a, p, (size_t)cap, 1);
}

/* Makes room in rb.out for new_len bytes the way growslice does. The old
 * array is the caller's to release. */
static bool grow_out(NormReorderBuffer *rb, Int new_len, Byte **old, Int *old_cap) {
    Slice *o = &rb->out;
    Int newcap = growslice_roundupsize(growslice_nextslicecap(new_len, o->cap));
    Byte *p = mem_alloc_nozero(rb->a, (size_t)newcap, 1);
    if (p == NULL) {
        rb->oom = true;
        return false;
    }
    if (o->len > 0)
        memcpy(p, o->p, (size_t)o->len);
    *old = (Byte *)o->p;
    *old_cap = o->cap;
    *o = norm_bytes_of(p, o->len, newcap);
    return true;
}

/* rb.out = append(rb.out, src[:n]...) */
static void out_append(NormReorderBuffer *rb, const Byte *src, Int n) {
    if (n == 0 || rb->oom)
        return;
    Slice *o = &rb->out;
    Int new_len = o->len + n;
    if (new_len <= o->cap) {
        memmove((Byte *)o->p + o->len, src, (size_t)n);
        o->len = new_len;
        return;
    }
    bool was_owned = rb->own_out;
    Byte *old = NULL;
    Int old_cap = 0;
    if (!grow_out(rb, new_len, &old, &old_cap))
        return;
    memcpy((Byte *)o->p + o->len, src, (size_t)n);
    o->len = new_len;
    rb->own_out = was_owned;
    release_out(rb, old, old_cap);
    rb->own_out = true;
}

/* The same thing one byte at a time, which is how Go appends from a string
 * here, and which grows by a different sequence of sizes. */
static void out_append_bytewise(NormReorderBuffer *rb, const Byte *src, Int n) {
    while (n > 0 && !rb->oom) {
        Slice *o = &rb->out;
        if (o->len == o->cap) {
            bool was_owned = rb->own_out;
            Byte *old = NULL;
            Int old_cap = 0;
            if (!grow_out(rb, o->len + 1, &old, &old_cap))
                return;
            rb->own_out = was_owned;
            release_out(rb, old, old_cap);
            rb->own_out = true;
        }
        Int k = min_int(n, o->cap - o->len);
        memmove((Byte *)o->p + o->len, src, (size_t)k);
        o->len += k;
        src += k;
        n -= k;
    }
}

static void rb_set_out(NormReorderBuffer *rb, Slice out, bool own) {
    rb->out = out;
    rb->own_out = own && out.p != NULL;
}

void burrow__norm_rb_free_out(NormReorderBuffer *rb) {
    if (rb->own_out && rb->out.p != NULL)
        mem_free(rb->a, rb->out.p, (size_t)rb->out.cap, 1);
    if (rb->retired.p != NULL)
        mem_free(rb->a, rb->retired.p, (size_t)rb->retired.cap, 1);
    rb->retired = slice_nil(TYPE_BYTE);
    rb->pin = NULL;
    rb->out = slice_nil(TYPE_BYTE);
    rb->own_out = false;
}

/* -------------------------------------------------------------------- input */

NormInput burrow__norm_input_bytes(Slice b) {
    NormInput in = {(const Byte *)b.p, b.len, false};
    return in;
}

NormInput burrow__norm_input_string(Str s) {
    NormInput in = {s.p, s.len, true};
    return in;
}

static NormInput input_raw(const Byte *p, Int len) {
    NormInput in = {p, len, false};
    return in;
}

/* Go tells the two apart by whether bytes is nil, so a nil byte slice is a
 * string. */
static bool in_is_str(NormInput in) {
    return in.is_str || in.p == NULL;
}

/* &in[p:], after Go's check of p. */
static const Byte *in_at(NormInput in, Int p) {
    check_slice(p, in.len, in.len);
    return in.p == NULL ? NULL : in.p + p;
}

static Byte in_byte(NormInput in, Int p) {
    check_index(p, in.len);
    return in.p[p];
}

static Int skip_ascii(NormInput in, Int p, Int max) {
    for (; p < max && in_byte(in, p) < 0x80; p++) {
    }
    return p;
}

static Int skip_continuation_bytes(NormInput in, Int p) {
    for (; p < in.len && !utf8_rune_start(in.p[p]); p++) {
    }
    return p;
}

/* buf = in.appendSlice(buf, b, e), where buf is always rb.out. */
static void out_append_input(NormReorderBuffer *rb, NormInput in, Int b, Int e) {
    check_slice(b, e, in.len);
    if (in_is_str(in))
        out_append_bytewise(rb, in.p + b, e - b);
    else
        out_append(rb, in.p + b, e - b);
}

static Int copy_slice(Byte *buf, Int buflen, NormInput in, Int b, Int e) {
    check_slice(b, e, in.len);
    if (e == b)
        return 0;
    return norm_copy_bytes(buf, buflen, in.p + b, e - b);
}

/* --------------------------------------------------------------------- trie */

typedef struct Trie {
    const uint16_t *values;
    const uint8_t *index8;
    const uint16_t *index16;
    uint32_t cutoff;
    const NormValueRange *sparse_values;
    const uint16_t *sparse_offset;
} Trie;

static const Trie nfc_trie = {
    burrow__norm_nfc_values,
    burrow__norm_nfc_index,
    NULL,
    NORM_NFC_CUTOFF,
    burrow__norm_nfc_sparse_values,
    burrow__norm_nfc_sparse_offset,
};

static const Trie nfkc_trie = {
    burrow__norm_nfkc_values,        NULL,
    burrow__norm_nfkc_index,         NORM_NFKC_CUTOFF,
    burrow__norm_nfkc_sparse_values, burrow__norm_nfkc_sparse_offset,
};

static uint32_t trie_index(const Trie *t, uint32_t i) {
    /* One of the two is always set. */
    /* NOLINTNEXTLINE(clang-analyzer-core.NullDereference) */
    return t->index8 != NULL ? t->index8[i] : t->index16[i];
}

/* sparseBlocks.lookup */
static uint16_t sparse_lookup(const Trie *t, uint32_t n, Byte b) {
    uint16_t offset = t->sparse_offset[n];
    NormValueRange header = t->sparse_values[offset];
    uint16_t lo = (uint16_t)(offset + 1);
    uint16_t hi = (uint16_t)(lo + header.lo);
    while (lo < hi) {
        uint16_t m = (uint16_t)(lo + (hi - lo) / 2);
        NormValueRange r = t->sparse_values[m];
        if (r.lo <= b && b <= r.hi)
            return (uint16_t)(r.value + (uint16_t)(b - r.lo) * header.value);
        if (b < r.lo)
            hi = m;
        else
            lo = (uint16_t)(m + 1);
    }
    return 0;
}

static uint16_t lookup_value(const Trie *t, uint32_t n, Byte b) {
    if (n < t->cutoff)
        return t->values[(n << 6) + b];
    return sparse_lookup(t, n - t->cutoff, b);
}

/* lookup and lookupString, which differ only in the type of s: the trie value
 * of the first rune in s and the length of its encoding, which is 0 when s
 * stops short of the end of it. */
static uint16_t trie_lookup(const Trie *t, const Byte *s, Int len, Int *sz) {
    check_index(0, len);
    Byte c0 = s[0];
    if (c0 < 0x80) {
        *sz = 1;
        return t->values[c0];
    }
    if (c0 < 0xC2) {
        *sz = 1;
        return 0;
    }
    Int want = c0 < 0xE0 ? 2 : c0 < 0xF0 ? 3 : c0 < 0xF8 ? 4 : 0;
    if (want == 0) {
        *sz = 1;
        return 0;
    }
    if (len < want) {
        *sz = 0;
        return 0;
    }
    uint32_t i = trie_index(t, c0);
    for (Int k = 1; k < want; k++) {
        Byte c = s[k];
        if (c < 0x80 || 0xC0 <= c) {
            *sz = k;
            return 0;
        }
        if (k == want - 1) {
            *sz = want;
            return lookup_value(t, i, c);
        }
        i = trie_index(t, (i << 6) + c);
    }
    *sz = 1;
    return 0;
}

/* ----------------------------------------------------------------- forminfo */

enum {
    QC_INFO_MASK = 0x3F,
    HEADER_LEN_MASK = 0x1F,
    HEADER_FLAGS_MASK = 0xE0,
};

struct NormFormInfo {
    NormForm form;
    bool composing, compatibility;
    NormProperties (*info)(NormInput b, Int i);
    NormIterFunc next_main;
};

static bool is_yes_c(NormProperties p) {
    return (p.flags & 0x10) == 0;
}

static bool is_yes_d(NormProperties p) {
    return (p.flags & 0x4) == 0;
}

static bool combines_backward(NormProperties p) {
    return (p.flags & 0x8) != 0;
}

static bool has_decomposition(NormProperties p) {
    return (p.flags & 0x4) != 0;
}

static bool is_inert(NormProperties p) {
    return (p.flags & QC_INFO_MASK) == 0 && p.ccc == 0;
}

static bool multi_segment(NormProperties p) {
    return p.index >= NORM_FIRST_MULTI && p.index < NORM_END_MULTI;
}

static uint8_t n_trailing_non_starters(NormProperties p) {
    return (uint8_t)(p.flags & 0x03);
}

bool burrow__norm_boundary_before(NormProperties p) {
    return p.ccc == 0 && !combines_backward(p);
}

bool burrow__norm_boundary_after(NormProperties p) {
    return is_inert(p);
}

Slice burrow__norm_decomposition(NormProperties p) {
    if (p.index == 0)
        return slice_nil(TYPE_BYTE);
    uint16_t i = p.index;
    Int n = burrow__norm_decomps[i] & HEADER_LEN_MASK;
    if (n == 31)
        n = 33;
    i++;
    Int cap = (Int)sizeof burrow__norm_decomps - i;
    return static_bytes(burrow__norm_decomps + i, n, cap);
}

Int burrow__norm_size(NormProperties p) {
    return p.size;
}

static uint8_t ccc_of(uint8_t c) {
    check_index(c, (Int)sizeof burrow__norm_ccc_table);
    return burrow__norm_ccc_table[c];
}

uint8_t burrow__norm_ccc(NormProperties p) {
    if (p.index >= NORM_FIRST_CCC_ZERO_EXCEPT)
        return 0;
    return ccc_of(p.ccc);
}

uint8_t burrow__norm_lead_ccc(NormProperties p) {
    return ccc_of(p.ccc);
}

uint8_t burrow__norm_trail_ccc(NormProperties p) {
    return ccc_of(p.tccc);
}

/* combine: the composition of a and b, or 0. The keys keep 16 bits of each,
 * which Go's tables are made to be unambiguous with. */
Rune burrow__norm_combine(Rune a, Rune b) {
    uint32_t key = ((uint32_t)(uint16_t)a << 16) + (uint32_t)(uint16_t)b;
    size_t lo = 0, hi = NORM_RECOMP_LEN;
    while (lo < hi) {
        size_t m = lo + (hi - lo) / 2;
        uint32_t k = burrow__norm_recomp[m][0];
        if (k == key)
            return (Rune)burrow__norm_recomp[m][1];
        if (k < key)
            lo = m + 1;
        else
            hi = m;
    }
    return 0;
}

/* compInfo, from the trie value v of a rune whose encoding is sz bytes. */
static NormProperties comp_info(uint16_t v, Int sz) {
    NormProperties p = {0};
    p.size = (uint8_t)sz;
    if (v == 0)
        return p;
    if (v >= 0x8000) {
        p.ccc = (uint8_t)v;
        p.tccc = (uint8_t)v;
        p.flags = (uint8_t)(v >> 8);
        if (p.ccc > 0 || combines_backward(p))
            p.n_lead = (uint8_t)(p.flags & 0x3);
        return p;
    }
    /* has decomposition */
    Byte h = burrow__norm_decomps[v];
    p.flags = (uint8_t)(((h & HEADER_FLAGS_MASK) >> 2) | 0x4);
    p.index = v;
    if (v >= NORM_FIRST_CCC) {
        uint16_t n = (uint16_t)(h & HEADER_LEN_MASK);
        if (n == 31)
            n = 33;
        v = (uint16_t)(v + n + 1);
        Byte c = burrow__norm_decomps[v];
        p.tccc = (uint8_t)(c >> 2);
        p.flags |= (uint8_t)(c & 0x3);
        if (v >= NORM_FIRST_LEADING_CCC) {
            p.n_lead = (uint8_t)(c & 0x3);
            if (v >= NORM_FIRST_STARTER_WITH_N_LEAD) {
                /* We were tricked. Remove the decomposition. */
                p.flags &= 0x03;
                p.index = 0;
                return p;
            }
            p.ccc = burrow__norm_decomps[v + 1];
        }
    }
    return p;
}

static NormProperties lookup_info(const Trie *t, NormInput b, Int i) {
    const Byte *s = in_at(b, i);
    Int sz = 0;
    uint16_t v = trie_lookup(t, s, b.len - i, &sz);
    return comp_info(v, sz);
}

static NormProperties lookup_info_nfc(NormInput b, Int i) {
    return lookup_info(&nfc_trie, b, i);
}

static NormProperties lookup_info_nfkc(NormInput b, Int i) {
    return lookup_info(&nfkc_trie, b, i);
}

static Slice next_composed(NormIter *i);
static Slice next_cgj_decompose(NormIter *i);
static Slice next_decomposed(NormIter *i);

static const NormFormInfo form_table[4] = {
    {NORM_NFC, true, false, lookup_info_nfc, next_composed},
    {NORM_NFD, false, false, lookup_info_nfc, next_decomposed},
    {NORM_NFKC, true, true, lookup_info_nfkc, next_composed},
    {NORM_NFKD, false, true, lookup_info_nfkc, next_decomposed},
};

static const NormFormInfo *form_info(NormForm f) {
    check_index((Int)f, 4);
    return &form_table[f];
}

static const Trie *form_trie(NormForm f) {
    return f == NORM_NFC || f == NORM_NFD ? &nfc_trie : &nfkc_trie;
}

NormProperties burrow__norm_properties(NormForm f, Slice s) {
    Int sz = 0;
    uint16_t v = trie_lookup(form_trie(f), (const Byte *)s.p, s.len, &sz);
    return comp_info(v, sz);
}

NormProperties burrow__norm_properties_string(NormForm f, Str s) {
    Int sz = 0;
    uint16_t v = trie_lookup(form_trie(f), s.p, s.len, &sz);
    return comp_info(v, sz);
}

/* -------------------------------------------------------------- composition */

enum {
    SS_SUCCESS,
    SS_STARTER,
    SS_OVERFLOW,
};

enum {
    I_SUCCESS = 0,
    I_SHORT_DST = -1,
    I_SHORT_SRC = -2,
};

static void ss_first(uint8_t *ss, NormProperties p) {
    *ss = n_trailing_non_starters(p);
}

static int ss_next(uint8_t *ss, NormProperties p) {
    if (*ss > NORM_MAX_NON_STARTERS)
        panic_str(BURROW_S("streamSafe was not reset"));
    uint8_t n = p.n_lead;
    *ss = (uint8_t)(*ss + n);
    if (*ss > NORM_MAX_NON_STARTERS) {
        *ss = 0;
        return SS_OVERFLOW;
    }
    /* Any rune with a non-zero nLead counts as a non-starter, because some
     * starters, like Jamo V and T, combine with what is before them. */
    if (n == 0) {
        *ss = n_trailing_non_starters(p);
        return SS_STARTER;
    }
    return SS_SUCCESS;
}

static int ss_backwards(uint8_t *ss, NormProperties p) {
    if (*ss > NORM_MAX_NON_STARTERS)
        panic_str(BURROW_S("streamSafe was not reset"));
    uint8_t c = (uint8_t)(*ss + n_trailing_non_starters(p));
    if (c > NORM_MAX_NON_STARTERS)
        return SS_OVERFLOW;
    *ss = c;
    if (p.n_lead == 0)
        return SS_STARTER;
    return SS_SUCCESS;
}

static void rb_init(NormReorderBuffer *rb, const NormFormInfo *f, NormInput src) {
    rb->f = f;
    rb->src = src;
    rb->nsrc = src.len;
    rb->ss = 0;
}

static void rb_reset(NormReorderBuffer *rb) {
    rb->nrune = 0;
    rb->nbyte = 0;
}

static void rb_compose(NormReorderBuffer *rb);

static bool rb_do_flush(NormReorderBuffer *rb) {
    if (rb->f->composing)
        rb_compose(rb);
    if (rb->flush_f == NULL)
        nil_func_call();
    bool res = rb->flush_f(rb);
    rb_reset(rb);
    return res;
}

/* appendFlush appends the normalised segment to rb.out. */
static bool append_flush(NormReorderBuffer *rb) {
    for (Int i = 0; i < rb->nrune; i++) {
        uint8_t start = rb->rune[i].pos;
        out_append(rb, rb->byte + start, rb->rune[i].size);
    }
    return true;
}

static Int rb_flush_copy(NormReorderBuffer *rb, Byte *buf, Int len) {
    Int p = 0;
    for (Int i = 0; i < rb->nrune; i++) {
        NormProperties r = rb->rune[i];
        p += norm_copy_bytes(buf + p, len - p, rb->byte + r.pos, r.size);
    }
    rb_reset(rb);
    return p;
}

/* insertOrdered inserts a rune in the buffer, ordered by canonical combining
 * class. Past the end of the buffer it panics the way Go's does. */
static void insert_ordered(NormReorderBuffer *rb, NormProperties info) {
    Int n = rb->nrune;
    NormProperties *b = rb->rune;
    uint8_t cc = info.ccc;
    if (cc > 0) {
        /* Find insertion position + move elements to make room. */
        for (; n > 0; n--) {
            check_index(n - 1, NORM_MAX_BUFFER_SIZE);
            if (b[n - 1].ccc <= cc)
                break;
            check_index(n, NORM_MAX_BUFFER_SIZE);
            b[n] = b[n - 1];
        }
    }
    rb->nrune += 1;
    uint8_t pos = rb->nbyte;
    rb->nbyte = (uint8_t)(rb->nbyte + UTF8_UTF_MAX);
    info.pos = pos;
    check_index(n, NORM_MAX_BUFFER_SIZE);
    b[n] = info;
}

/* rb.byte[rb.nbyte:] */
static Byte *rb_free_bytes(NormReorderBuffer *rb, Int *len) {
    check_slice(rb->nbyte, (Int)sizeof rb->byte, (Int)sizeof rb->byte);
    *len = (Int)sizeof rb->byte - rb->nbyte;
    return rb->byte + rb->nbyte;
}

static void insert_single(NormReorderBuffer *rb, NormInput src, Int i,
                          NormProperties info) {
    Int len = 0;
    Byte *dst = rb_free_bytes(rb, &len);
    copy_slice(dst, len, src, i, i + info.size);
    insert_ordered(rb, info);
}

static void insert_cgj(NormReorderBuffer *rb) {
    NormProperties p = {0};
    p.size = 2;
    insert_single(rb, burrow__norm_input_string(BURROW_S(NORM_GRAPHEME_JOINER)), 0, p);
}

static int insert_decomposed(NormReorderBuffer *rb, Slice dcomp) {
    rb->tmp_bytes = burrow__norm_input_bytes(dcomp);
    /* The streamSafe accounting already counts the modifiers, so next is not
     * needed, but the buffer has to be flushed at each segment start. */
    for (Int i = 0; i < dcomp.len;) {
        NormProperties info = rb->f->info(rb->tmp_bytes, i);
        if (burrow__norm_boundary_before(info) && rb->nrune > 0 && !rb_do_flush(rb))
            return I_SHORT_DST;
        check_slice(i, i + info.size, dcomp.cap);
        Int len = 0;
        Byte *dst = rb_free_bytes(rb, &len);
        i += norm_copy_bytes(dst, len, (const Byte *)dcomp.p + i, info.size);
        insert_ordered(rb, info);
    }
    return I_SUCCESS;
}

/* ------------------------------------------------------------------- hangul */

enum {
    HANGUL_BASE = 0xAC00,
    HANGUL_BASE0 = 0xEA,
    HANGUL_BASE1 = 0xB0,
    HANGUL_END = HANGUL_BASE + 19 * 21 * 28,
    HANGUL_END0 = 0xED,
    HANGUL_END1 = 0x9E,
    HANGUL_END2 = 0xA4,
    JAMO_L_BASE = 0x1100,
    JAMO_L_BASE0 = 0xE1,
    JAMO_L_BASE1 = 0x84,
    JAMO_L_END = 0x1113,
    JAMO_V_BASE = 0x1161,
    JAMO_V_END = 0x1176,
    JAMO_T_BASE = 0x11A7,
    JAMO_T_END = 0x11C3,
    JAMO_T_COUNT = 28,
    JAMO_V_COUNT = 21,
    JAMO_VT_COUNT = 21 * 28,
    HANGUL_UTF8_SIZE = 3,
};

static bool is_hangul(const Byte *b, Int len) {
    if (len < HANGUL_UTF8_SIZE)
        return false;
    Byte b0 = b[0];
    if (b0 < HANGUL_BASE0)
        return false;
    Byte b1 = b[1];
    if (b0 == HANGUL_BASE0)
        return b1 >= HANGUL_BASE1;
    if (b0 < HANGUL_END0)
        return true;
    if (b0 > HANGUL_END0)
        return false;
    if (b1 < HANGUL_END1)
        return true;
    return b1 == HANGUL_END1 && b[2] < HANGUL_END2;
}

bool burrow__norm_is_hangul(Slice b) {
    return is_hangul((const Byte *)b.p, b.len);
}

static bool is_jamo_vt(Slice b) {
    const Byte *p = (const Byte *)b.p;
    check_index(0, b.len);
    if (p[0] != JAMO_L_BASE0)
        return false;
    check_index(1, b.len);
    return (p[1] & 0xFC) == JAMO_L_BASE1;
}

/* in.hangul(p): the Hangul syllable at p, or 0. */
static Rune in_hangul(NormInput in, Int p) {
    const Byte *s = in_at(in, p);
    Int n = in.len - p;
    if (!is_hangul(s, n))
        return 0;
    Int size = 0;
    Rune r = norm_decode_rune(s, n, &size);
    if (size != HANGUL_UTF8_SIZE)
        return 0;
    return r;
}

/* decomposeHangul writes the decomposition of r to buf, which has room for 9
 * bytes, and returns its length. */
Int burrow__norm_decompose_hangul(Byte *buf, Rune r) {
    r -= HANGUL_BASE;
    Rune x = r % JAMO_T_COUNT;
    r /= JAMO_T_COUNT;
    utf8_encode_rune(norm_bytes_of(buf, 3, 3), JAMO_L_BASE + r / JAMO_V_COUNT);
    utf8_encode_rune(norm_bytes_of(buf + 3, 3, 3), JAMO_V_BASE + r % JAMO_V_COUNT);
    if (x != 0) {
        utf8_encode_rune(norm_bytes_of(buf + 6, 3, 3), JAMO_T_BASE + x);
        return 9;
    }
    return 6;
}

/* appendRune inserts a rune at the end of the buffer. It is used for Hangul. */
static void rb_append_rune(NormReorderBuffer *rb, Rune r) {
    uint8_t bn = rb->nbyte;
    Int len = 0;
    Byte *dst = rb_free_bytes(rb, &len);
    Int sz = encode_rune(dst, len, r);
    rb->nbyte = (uint8_t)(rb->nbyte + UTF8_UTF_MAX);
    check_index(rb->nrune, NORM_MAX_BUFFER_SIZE);
    NormProperties p = {0};
    p.pos = bn;
    p.size = (uint8_t)sz;
    rb->rune[rb->nrune] = p;
    rb->nrune++;
}

/* assignRune sets the rune at position pos. */
static void rb_assign_rune(NormReorderBuffer *rb, Int pos, Rune r) {
    check_index(pos, NORM_MAX_BUFFER_SIZE);
    uint8_t bn = rb->rune[pos].pos;
    Int sz = encode_rune(rb->byte + bn, (Int)sizeof rb->byte - bn, r);
    NormProperties p = {0};
    p.pos = bn;
    p.size = (uint8_t)sz;
    rb->rune[pos] = p;
}

Rune burrow__norm_rb_rune_at(const NormReorderBuffer *rb, Int n) {
    check_index(n, NORM_MAX_BUFFER_SIZE);
    NormProperties inf = rb->rune[n];
    Int size = 0;
    return norm_decode_rune(rb->byte + inf.pos, inf.size, &size);
}

static Slice rb_bytes_at(NormReorderBuffer *rb, Int n) {
    check_index(n, NORM_MAX_BUFFER_SIZE);
    NormProperties inf = rb->rune[n];
    return norm_bytes_of(rb->byte + inf.pos, inf.size, (Int)sizeof rb->byte - inf.pos);
}

static void rb_decompose_hangul(NormReorderBuffer *rb, Rune r) {
    r -= HANGUL_BASE;
    Rune x = r % JAMO_T_COUNT;
    r /= JAMO_T_COUNT;
    rb_append_rune(rb, JAMO_L_BASE + r / JAMO_V_COUNT);
    rb_append_rune(rb, JAMO_V_BASE + r % JAMO_V_COUNT);
    if (x != 0)
        rb_append_rune(rb, JAMO_T_BASE + x);
}

/* combineHangul combines Jamo into Hangul syllables. */
static void rb_combine_hangul(NormReorderBuffer *rb, Int s, Int i, Int k) {
    NormProperties *b = rb->rune;
    Int bn = rb->nrune;
    for (; i < bn; i++) {
        check_index(k - 1, NORM_MAX_BUFFER_SIZE);
        check_index(i, NORM_MAX_BUFFER_SIZE);
        uint8_t ccc_b = b[k - 1].ccc;
        uint8_t ccc_c = b[i].ccc;
        if (ccc_b == 0)
            s = k - 1;
        if (s != k - 1 && ccc_b >= ccc_c) {
            /* b[i] is blocked by greater-equal cccX below it */
            b[k] = b[i];
            k++;
        } else {
            Rune l = burrow__norm_rb_rune_at(rb, s);
            Rune v = burrow__norm_rb_rune_at(rb, i);
            if (JAMO_L_BASE <= l && l < JAMO_L_END && JAMO_V_BASE <= v &&
                v < JAMO_V_END) {
                /* 11xx plus 116x to LV */
                rb_assign_rune(rb, s,
                               HANGUL_BASE + (l - JAMO_L_BASE) * JAMO_VT_COUNT +
                                   (v - JAMO_V_BASE) * JAMO_T_COUNT);
            } else if (HANGUL_BASE <= l && l < HANGUL_END && JAMO_T_BASE < v &&
                       v < JAMO_T_END && ((l - HANGUL_BASE) % JAMO_T_COUNT) == 0) {
                /* ACxx plus 11Ax to LVT */
                rb_assign_rune(rb, s, l + v - JAMO_T_BASE);
            } else {
                b[k] = b[i];
                k++;
            }
        }
    }
    rb->nrune = k;
}

/* compose recombines the runes in the buffer, which hold one segment.
 *
 * UAX #15, section X5, including Corrigendum #5: "In any character sequence
 * beginning with starter S, a character C is blocked from S if and only if
 * there is some character B between S and C, and either B is a starter or it
 * has the same or higher combining class as C." */
static void rb_compose(NormReorderBuffer *rb) {
    Int bn = rb->nrune;
    if (bn == 0)
        return;
    Int k = 1;
    NormProperties *b = rb->rune;
    for (Int s = 0, i = 1; i < bn; i++) {
        if (is_jamo_vt(rb_bytes_at(rb, i))) {
            /* Redo from start in Hangul mode. Necessary to support
             * U+320E..U+321E in NFKC mode. */
            rb_combine_hangul(rb, s, i, k);
            return;
        }
        NormProperties ii = b[i];
        /* combinesBackward is safe to filter on, where combinesForward would
         * need the properties of the combined rune later. */
        if (combines_backward(ii)) {
            uint8_t ccc_b = b[k - 1].ccc;
            uint8_t ccc_c = ii.ccc;
            bool blocked = false;
            if (ccc_b == 0)
                s = k - 1;
            else
                blocked = s != k - 1 && ccc_b >= ccc_c;
            if (!blocked) {
                Rune combined = burrow__norm_combine(burrow__norm_rb_rune_at(rb, s),
                                                     burrow__norm_rb_rune_at(rb, i));
                if (combined != 0) {
                    rb_assign_rune(rb, s, combined);
                    continue;
                }
            }
        }
        b[k] = b[i];
        k++;
    }
    rb->nrune = k;
}

/* insertFlush inserts the rune at src[i:] in the buffer, ordered by CCC, and
 * flushes the leading segments of a decomposition that has more than one. */
static int insert_flush(NormReorderBuffer *rb, NormInput src, Int i,
                        NormProperties info) {
    Rune r = in_hangul(src, i);
    if (r != 0) {
        rb_decompose_hangul(rb, r);
        return I_SUCCESS;
    }
    if (has_decomposition(info))
        return insert_decomposed(rb, burrow__norm_decomposition(info));
    insert_single(rb, src, i, info);
    return I_SUCCESS;
}

/* insertUnsafe is insertFlush for when the caller knows there is room. */
static void insert_unsafe(NormReorderBuffer *rb, NormInput src, Int i,
                          NormProperties info) {
    Rune r = in_hangul(src, i);
    if (r != 0)
        rb_decompose_hangul(rb, r);
    if (has_decomposition(info))
        insert_decomposed(rb, burrow__norm_decomposition(info));
    else
        insert_single(rb, src, i, info);
}

/* ---------------------------------------------------------------- normalize */

static Int quick_span(const NormFormInfo *f, NormInput src, Int i, Int end, bool at_eof,
                      bool *ok);
static Int decompose_segment(NormReorderBuffer *rb, Int sp, bool at_eof);
static void decompose_to_last_boundary(NormReorderBuffer *rb);
static Slice do_append_inner(NormReorderBuffer *rb, Int p);

static void rb_start(NormReorderBuffer *rb, Alloc *a, const NormFormInfo *f,
                     NormInput src) {
    memset(rb, 0, sizeof *rb);
    rb->out = slice_nil(TYPE_BYTE);
    rb->retired = slice_nil(TYPE_BYTE);
    rb->a = a;
    rb_init(rb, f, src);
}

/* What a function that grows rb.out returns: the output, or nil and nothing
 * held when an allocation failed along the way. */
static Slice finish_out(NormReorderBuffer *rb) {
    if (rb->oom) {
        burrow__norm_rb_free_out(rb);
        return slice_nil(TYPE_BYTE);
    }
    return rb->out;
}

Slice burrow__norm_bytes(Alloc *a, NormForm f, Slice b) {
    NormInput src = burrow__norm_input_bytes(b);
    const NormFormInfo *ft = form_info(f);
    bool ok = false;
    Int n = quick_span(ft, src, 0, b.len, true, &ok);
    if (ok)
        return b;
    Byte *p = mem_alloc_nozero(a, (size_t)b.len, 1);
    if (p == NULL)
        return slice_nil(TYPE_BYTE);
    memcpy(p, b.p, (size_t)n);
    NormReorderBuffer rb;
    rb_start(&rb, a, ft, src);
    rb_set_out(&rb, norm_bytes_of(p, n, b.len), true);
    rb.flush_f = append_flush;
    do_append_inner(&rb, n);
    return finish_out(&rb);
}

Str burrow__norm_string(Alloc *a, NormForm f, Str s) {
    NormInput src = burrow__norm_input_string(s);
    const NormFormInfo *ft = form_info(f);
    bool ok = false;
    Int n = quick_span(ft, src, 0, s.len, true, &ok);
    if (ok)
        return s;
    Byte *p = mem_alloc_nozero(a, (size_t)s.len, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    memcpy(p, s.p, (size_t)n);
    NormReorderBuffer rb;
    rb_start(&rb, a, ft, src);
    rb_set_out(&rb, norm_bytes_of(p, n, s.len), true);
    rb.flush_f = append_flush;
    do_append_inner(&rb, n);
    Slice out = finish_out(&rb);
    if (out.len == 0) {
        burrow__norm_rb_free_out(&rb);
        return BURROW_STR_EMPTY;
    }
    if (out.len == out.cap) {
        Str r = {(const Byte *)out.p, out.len};
        return r;
    }
    Byte *q = mem_alloc_nozero(a, (size_t)out.len, 1);
    if (q != NULL)
        memcpy(q, out.p, (size_t)out.len);
    Int len = out.len;
    burrow__norm_rb_free_out(&rb);
    if (q == NULL)
        return BURROW_STR_EMPTY;
    Str r = {q, len};
    return r;
}

/* cmpNormalBytes, IsNormal's flusher. It compares from the start of rb.out
 * every time, which is what Go's does. */
static bool cmp_normal_bytes(NormReorderBuffer *rb) {
    const Byte *b = (const Byte *)rb->out.p;
    Int blen = rb->out.len;
    for (Int i = 0; i < rb->nrune; i++) {
        NormProperties info = rb->rune[i];
        if ((Int)info.size > blen)
            return false;
        for (uint8_t p = info.pos, pe = (uint8_t)(info.pos + info.size); p < pe; p++) {
            if (b[0] != rb->byte[p])
                return false;
            b++;
            blen--;
        }
    }
    return true;
}

bool burrow__norm_is_normal(NormForm f, Slice b) {
    NormInput src = burrow__norm_input_bytes(b);
    const NormFormInfo *ft = form_info(f);
    bool ok = false;
    Int bp = quick_span(ft, src, 0, b.len, true, &ok);
    if (ok)
        return true;
    NormReorderBuffer rb;
    rb_start(&rb, NULL, ft, src);
    rb.flush_f = cmp_normal_bytes;
    while (bp < b.len) {
        rb_set_out(&rb, btail(b, bp), false);
        bp = decompose_segment(&rb, bp, true);
        if (bp < 0)
            return false;
        bp = quick_span(rb.f, rb.src, bp, b.len, true, &ok);
    }
    return true;
}

/* IsNormalString's flusher, which is a closure over s and bp in Go. bp is the
 * loop's variable as well, so it lives in rb. */
static bool cmp_normal_string(NormReorderBuffer *rb) {
    Str s = rb->cmp_s;
    for (Int i = 0; i < rb->nrune; i++) {
        NormProperties info = rb->rune[i];
        if (rb->cmp_bp + (Int)info.size > s.len)
            return false;
        for (uint8_t p = info.pos, pe = (uint8_t)(info.pos + info.size); p < pe; p++) {
            if (s.p[rb->cmp_bp] != rb->byte[p])
                return false;
            rb->cmp_bp++;
        }
    }
    return true;
}

bool burrow__norm_is_normal_string(NormForm f, Str s) {
    NormInput src = burrow__norm_input_string(s);
    const NormFormInfo *ft = form_info(f);
    bool ok = false;
    Int bp = quick_span(ft, src, 0, s.len, true, &ok);
    if (ok)
        return true;
    NormReorderBuffer rb;
    rb_start(&rb, NULL, ft, src);
    rb.cmp_s = s;
    rb.cmp_bp = bp;
    rb.flush_f = cmp_normal_string;
    while (rb.cmp_bp < s.len) {
        Int next = decompose_segment(&rb, rb.cmp_bp, true);
        rb.cmp_bp = next;
        if (next < 0)
            return false;
        rb.cmp_bp = quick_span(rb.f, rb.src, rb.cmp_bp, s.len, true, &ok);
    }
    return true;
}

/* lastRuneStart: the properties and position of the last rune in buf, or the
 * zero properties and -1 when there is none. */
static NormProperties last_rune_start(const NormFormInfo *fd, const Byte *buf, Int len,
                                      Int *pos) {
    Int p = len - 1;
    for (; p >= 0 && !utf8_rune_start(buf[p]); p--) {
    }
    *pos = p;
    if (p < 0) {
        NormProperties z = {0};
        return z;
    }
    return fd->info(input_raw(buf, len), p);
}

/* patchTail fixes a case where a rune may be incorrectly normalised if it is
 * followed by illegal continuation bytes. It reports whether the
 * decomposition is still in progress. */
static bool patch_tail(NormReorderBuffer *rb) {
    Int p = 0;
    NormProperties info =
        last_rune_start(rb->f, (const Byte *)rb->out.p, rb->out.len, &p);
    if (p == -1 || info.size == 0)
        return true;
    Int end = p + info.size;
    Int extra = rb->out.len - end;
    if (extra > 0) {
        /* Only reached with ill-formed UTF-8. */
        Byte *x = mem_alloc_nozero(rb->a, (size_t)extra, 1);
        if (x == NULL) {
            rb->oom = true;
            return false;
        }
        memcpy(x, (Byte *)rb->out.p + end, (size_t)extra);
        rb->out.len = end;
        decompose_to_last_boundary(rb);
        rb_do_flush(rb);
        out_append(rb, x, extra);
        mem_free(rb->a, x, (size_t)extra, 1);
        return false;
    }
    /* buf := rb.out[p:]. The flushes below append to rb.out, which may write
     * over buf or move rb.out somewhere else, and buf is read after them
     * either way, so its array is pinned until then. */
    const Byte *buf = (const Byte *)rb->out.p + p;
    Int buf_len = rb->out.len - p;
    rb->out.len = p;
    rb->pin = rb->out.p;
    decompose_to_last_boundary(rb);
    int s = ss_next(&rb->ss, info);
    if (s == SS_STARTER) {
        rb_do_flush(rb);
        ss_first(&rb->ss, info);
    } else if (s == SS_OVERFLOW) {
        rb_do_flush(rb);
        insert_cgj(rb);
        rb->ss = 0;
    }
    insert_unsafe(rb, input_raw(buf, buf_len), 0, info);
    rb->pin = NULL;
    if (rb->retired.p != NULL) {
        mem_free(rb->a, rb->retired.p, (size_t)rb->retired.cap, 1);
        rb->retired = slice_nil(TYPE_BYTE);
    }
    return true;
}

static Int append_quick(NormReorderBuffer *rb, Int i) {
    if (rb->nsrc == i)
        return i;
    bool ok = false;
    Int end = quick_span(rb->f, rb->src, i, rb->nsrc, true, &ok);
    out_append_input(rb, rb->src, i, end);
    return end;
}

/* doAppend, for an rb whose src is set, appending to out, which rb may free
 * when it grows if own says out came from rb.a. */
static Slice norm_do_append(NormReorderBuffer *rb, Slice out, bool own, Int p) {
    rb_set_out(rb, out, own);
    rb->flush_f = append_flush;
    NormInput src = rb->src;
    Int n = rb->nsrc;
    bool do_merge = out.len > 0;
    Int q = skip_continuation_bytes(src, p);
    if (q > p) {
        /* Move leading non-starters to destination. */
        out_append_input(rb, src, p, q);
        p = q;
        do_merge = patch_tail(rb);
    }
    if (do_merge) {
        NormProperties info = {0};
        if (p < n) {
            info = rb->f->info(src, p);
            if (!burrow__norm_boundary_before(info) || info.n_lead > 0) {
                if (p == 0)
                    decompose_to_last_boundary(rb);
                p = decompose_segment(rb, p, true);
            }
        }
        if (info.size == 0) {
            rb_do_flush(rb);
            /* Append incomplete UTF-8 encoding. */
            out_append_input(rb, src, p, n);
            return rb->out;
        }
        if (rb->nrune > 0)
            return do_append_inner(rb, p);
    }
    p = append_quick(rb, p);
    return do_append_inner(rb, p);
}

static Slice do_append_inner(NormReorderBuffer *rb, Int p) {
    for (Int n = rb->nsrc; p < n;) {
        p = decompose_segment(rb, p, true);
        p = append_quick(rb, p);
    }
    return rb->out;
}

/* Form.doAppend */
static Slice form_do_append(Alloc *a, NormForm f, Slice out, NormInput src, Int n) {
    if (n == 0)
        return out;
    const NormFormInfo *ft = form_info(f);
    NormReorderBuffer rb;
    rb_start(&rb, a, ft, src);
    if (out.len == 0) {
        /* A quickSpan first, which saves setting up the reorder buffer. */
        bool ok = false;
        Int p = quick_span(ft, src, 0, n, true, &ok);
        rb_set_out(&rb, out, false);
        out_append_input(&rb, src, 0, p);
        if (p == n)
            return finish_out(&rb);
        rb.flush_f = append_flush;
        do_append_inner(&rb, p);
        return finish_out(&rb);
    }
    norm_do_append(&rb, out, false, 0);
    return finish_out(&rb);
}

Slice burrow__norm_append(Alloc *a, NormForm f, Slice out, Slice src) {
    return form_do_append(a, f, out, burrow__norm_input_bytes(src), src.len);
}

Slice burrow__norm_append_string(Alloc *a, NormForm f, Slice out, Str src) {
    return form_do_append(a, f, out, burrow__norm_input_string(src), src.len);
}

Int burrow__norm_quick_span(NormForm f, Slice b) {
    bool ok = false;
    return quick_span(form_info(f), burrow__norm_input_bytes(b), 0, b.len, true, &ok);
}

Int burrow__norm_quick_span_string(NormForm f, Str s) {
    bool ok = false;
    return quick_span(form_info(f), burrow__norm_input_string(s), 0, s.len, true, &ok);
}

static Int norm_span(NormForm f, NormInput in, bool at_eof, Error *err) {
    bool ok = false;
    Int n = quick_span(form_info(f), in, 0, in.len, at_eof, &ok);
    Error e = BURROW_NO_ERROR;
    if (n < in.len)
        e = ok ? burrow__transform_err_short_src : burrow__transform_err_end_of_span;
    if (err != NULL)
        *err = e;
    return n;
}

Int burrow__norm_span(NormForm f, Slice b, bool at_eof, Error *err) {
    return norm_span(f, burrow__norm_input_bytes(b), at_eof, err);
}

Int burrow__norm_span_string(NormForm f, Str s, bool at_eof, Error *err) {
    return norm_span(f, burrow__norm_input_string(s), at_eof, err);
}

/* quickSpan returns a boundary n such that src[0:n] == f(src[0:n]) and
 * whether any non-normalised parts were found. If at_eof is false, n will not
 * point past the last segment if this segment might become non-normalised by
 * appending other runes. */
static Int quick_span(const NormFormInfo *f, NormInput src, Int i, Int end, bool at_eof,
                      bool *ok) {
    uint8_t last_cc = 0;
    uint8_t ss = 0;
    Int last_seg_start = i;
    Int n;
    for (n = end; i < n;) {
        Int j = skip_ascii(src, i, n);
        if (i != j) {
            i = j;
            last_seg_start = i - 1;
            last_cc = 0;
            ss = 0;
            continue;
        }
        NormProperties info = f->info(src, i);
        if (info.size == 0) {
            *ok = true;
            /* include incomplete runes */
            return at_eof ? n : last_seg_start;
        }
        /* This has to come before the check below, because a starter can
         * overflow too (U+FF9E, for one). */
        int s = ss_next(&ss, info);
        if (s == SS_STARTER) {
            last_seg_start = i;
        } else if (s == SS_OVERFLOW || last_cc > info.ccc) {
            *ok = false;
            return last_seg_start;
        }
        if (f->composing ? !is_yes_c(info) : !is_yes_d(info))
            break;
        last_cc = info.ccc;
        i += info.size;
    }
    if (i == n) {
        if (!at_eof)
            n = last_seg_start;
        *ok = true;
        return n;
    }
    *ok = false;
    return last_seg_start;
}

Int burrow__norm_quick_span_input(NormForm f, NormInput src, Int i, Int end,
                                  bool at_eof, bool *ok) {
    bool k = false;
    Int n = quick_span(form_info(f), src, i, end, at_eof, &k);
    if (ok != NULL)
        *ok = k;
    return n;
}

static Int first_boundary(NormForm f, NormInput src, Int nsrc) {
    Int i = skip_continuation_bytes(src, 0);
    if (i >= nsrc)
        return -1;
    const NormFormInfo *fd = form_info(f);
    uint8_t ss = 0;
    /* ss.first should be called here, but the first rune has been skipped
     * already, so CGJ insertion points are not quite right. They do not need
     * to be. */
    for (;;) {
        NormProperties info = fd->info(src, i);
        if (info.size == 0)
            return -1;
        if (ss_next(&ss, info) != SS_SUCCESS)
            return i;
        i += info.size;
        if (i >= nsrc) {
            if (!burrow__norm_boundary_after(info) && ss != NORM_MAX_NON_STARTERS)
                return -1;
            return nsrc;
        }
    }
}

Int burrow__norm_first_boundary(NormForm f, Slice b) {
    return first_boundary(f, burrow__norm_input_bytes(b), b.len);
}

Int burrow__norm_first_boundary_in_string(NormForm f, Str s) {
    return first_boundary(f, burrow__norm_input_string(s), s.len);
}

static Int next_boundary(NormForm f, NormInput src, Int nsrc, bool at_eof) {
    if (nsrc == 0)
        return at_eof ? 0 : -1;
    const NormFormInfo *fd = form_info(f);
    NormProperties info = fd->info(src, 0);
    if (info.size == 0)
        return at_eof ? 1 : -1;
    uint8_t ss = 0;
    ss_first(&ss, info);
    for (Int i = info.size; i < nsrc; i += info.size) {
        info = fd->info(src, i);
        if (info.size == 0)
            return at_eof ? i : -1;
        if (ss_next(&ss, info) != SS_SUCCESS)
            return i;
    }
    if (!at_eof && !burrow__norm_boundary_after(info) && ss != NORM_MAX_NON_STARTERS)
        return -1;
    return nsrc;
}

Int burrow__norm_next_boundary(NormForm f, Slice b, bool at_eof) {
    return next_boundary(f, burrow__norm_input_bytes(b), b.len, at_eof);
}

Int burrow__norm_next_boundary_in_string(NormForm f, Str s, bool at_eof) {
    return next_boundary(f, burrow__norm_input_string(s), s.len, at_eof);
}

static Int last_boundary(const NormFormInfo *fd, const Byte *b, Int len) {
    Int i = len;
    Int p = 0;
    NormProperties info = last_rune_start(fd, b, len, &p);
    if (p == -1)
        return -1;
    if (info.size == 0) { /* ends with incomplete rune */
        if (p == 0)       /* starts with incomplete rune */
            return -1;
        i = p;
        info = last_rune_start(fd, b, i, &p);
        if (p ==
            -1) /* incomplete UTF-8 encoding or non-starter bytes without a starter */
            return i;
    }
    if (p + info.size != i) /* trailing non-starter bytes: illegal UTF-8 */
        return i;
    if (burrow__norm_boundary_after(info))
        return i;
    uint8_t ss = 0;
    int v = ss_backwards(&ss, info);
    for (i = p; i >= 0 && v != SS_STARTER; i = p) {
        info = last_rune_start(fd, b, i, &p);
        v = ss_backwards(&ss, info);
        if (v == SS_OVERFLOW)
            break;
        if (p + info.size != i) {
            if (p == -1) /* no boundary found */
                return -1;
            return i; /* boundary after an illegal UTF-8 encoding */
        }
    }
    return i;
}

Int burrow__norm_last_boundary(NormForm f, Slice b) {
    return last_boundary(form_info(f), (const Byte *)b.p, b.len);
}

/* decomposeSegment scans the first segment in src into rb. It inserts a CGJ
 * when it meets more than 30 non-starters in a row, and returns the number of
 * bytes consumed from src, or I_SHORT_DST or I_SHORT_SRC. */
static Int decompose_segment(NormReorderBuffer *rb, Int sp, bool at_eof) {
    /* Force one character to be consumed. */
    NormProperties info = rb->f->info(rb->src, sp);
    if (info.size == 0)
        return 0;
    int s = ss_next(&rb->ss, info);
    if (s == SS_STARTER) {
        if (rb->nrune > 0)
            goto end;
    } else if (s == SS_OVERFLOW) {
        insert_cgj(rb);
        goto end;
    }
    int e = insert_flush(rb, rb->src, sp, info);
    if (e != I_SUCCESS)
        return e;
    for (;;) {
        sp += info.size;
        if (sp >= rb->nsrc) {
            if (!at_eof && !burrow__norm_boundary_after(info))
                return I_SHORT_SRC;
            break;
        }
        info = rb->f->info(rb->src, sp);
        if (info.size == 0) {
            if (!at_eof)
                return I_SHORT_SRC;
            break;
        }
        s = ss_next(&rb->ss, info);
        if (s == SS_STARTER)
            break;
        if (s == SS_OVERFLOW) {
            insert_cgj(rb);
            break;
        }
        e = insert_flush(rb, rb->src, sp, info);
        if (e != I_SUCCESS)
            return e;
    }
end:
    if (!rb_do_flush(rb))
        return I_SHORT_DST;
    return sp;
}

/* decomposeToLastBoundary finds an open segment at the end of rb.out and
 * scans it into rb, leaving rb.out without it. */
static void decompose_to_last_boundary(NormReorderBuffer *rb) {
    const NormFormInfo *fd = rb->f;
    const Byte *out = (const Byte *)rb->out.p;
    Int i = 0;
    NormProperties info = last_rune_start(fd, out, rb->out.len, &i);
    if ((Int)info.size != rb->out.len - i) /* illegal trailing continuation bytes */
        return;
    if (burrow__norm_boundary_after(info))
        return;
    NormProperties
        add[NORM_MAX_NON_STARTERS + 1]; /* stores runeInfo in reverse order */
    Int padd = 0;
    uint8_t ss = 0;
    Int p = rb->out.len;
    for (;;) {
        check_index(padd, NORM_MAX_NON_STARTERS + 1);
        add[padd] = info;
        int v = ss_backwards(&ss, info);
        /* An overflow means the text being appended to is not normalised, and
         * then what happens is undefined. */
        if (v == SS_OVERFLOW)
            break;
        padd++;
        p -= info.size;
        if (v == SS_STARTER || p < 0)
            break;
        info = last_rune_start(fd, out, p, &i);
        if ((Int)info.size != p - i)
            break;
    }
    rb->ss = ss;
    /* Copy bytes for insertion as we may need to overwrite rb.out. */
    Byte buf[NORM_MAX_BUFFER_SIZE * UTF8_UTF_MAX];
    check_slice(p, rb->out.len, rb->out.cap);
    Int cp_len = norm_copy_bytes(buf, (Int)sizeof buf, out + p, rb->out.len - p);
    Int cp = 0;
    rb->out.len = p;
    for (padd--; padd >= 0; padd--) {
        info = add[padd];
        insert_unsafe(rb, input_raw(buf + cp, cp_len - cp), 0, info);
        check_slice(info.size, cp_len - cp, (Int)sizeof buf - cp);
        cp += info.size;
    }
}

/* --------------------------------------------------------------------- iter */

static Slice iter_buf(NormIter *i, Int n) {
    check_slice(0, n, NORM_MAX_SEGMENT_SIZE);
    return norm_bytes_of(i->buf, n, NORM_MAX_SEGMENT_SIZE);
}

static Slice next_ascii_bytes(NormIter *i);
static Slice next_ascii_string(NormIter *i);

static Slice next_done(NormIter *i) {
    (void)i;
    return slice_nil(TYPE_BYTE);
}

static void iter_set_done(NormIter *i) {
    i->next = next_done;
    i->p = i->rb.nsrc;
}

static void iter_init(NormIter *i, NormForm f, NormInput src, NormIterFunc ascii_f) {
    i->p = 0;
    if (src.len == 0) {
        iter_set_done(i);
        i->rb.nsrc = 0;
        return;
    }
    i->multi_seg = slice_nil(TYPE_BYTE);
    rb_init(&i->rb, form_info(f), src);
    i->next = i->rb.f->next_main;
    i->ascii_f = ascii_f;
    i->info = i->rb.f->info(i->rb.src, i->p);
    ss_first(&i->rb.ss, i->info);
}

void burrow__norm_iter_init(NormIter *i, NormForm f, Slice src) {
    iter_init(i, f, burrow__norm_input_bytes(src), next_ascii_bytes);
}

void burrow__norm_iter_init_string(NormIter *i, NormForm f, Str src) {
    iter_init(i, f, burrow__norm_input_string(src), next_ascii_string);
}

int64_t burrow__norm_iter_seek(NormIter *i, int64_t offset, int whence, Error *err) {
    int64_t abs;
    switch (whence) {
    case 0:
        abs = offset;
        break;
    case 1:
        abs = (int64_t)i->p + offset;
        break;
    case 2:
        abs = (int64_t)i->rb.nsrc + offset;
        break;
    default:
        *err = errors_new(error_allocator(), BURROW_S("norm: invalid whence"));
        return 0;
    }
    if (abs < 0) {
        *err = errors_new(error_allocator(), BURROW_S("norm: negative position"));
        return 0;
    }
    *err = BURROW_NO_ERROR;
    if ((Int)abs >= i->rb.nsrc) {
        iter_set_done(i);
        return (int64_t)i->p;
    }
    if (i->rb.f == NULL)
        nil_func_call();
    i->p = (Int)abs;
    i->multi_seg = slice_nil(TYPE_BYTE);
    i->next = i->rb.f->next_main;
    i->info = i->rb.f->info(i->rb.src, i->p);
    ss_first(&i->rb.ss, i->info);
    return abs;
}

/* returnSlice: src[a:b], which for a string means a copy in i.buf. */
static Slice iter_return_slice(NormIter *i, Int a, Int b) {
    NormInput src = i->rb.src;
    check_slice(a, b, src.len);
    if (in_is_str(src))
        return iter_buf(i,
                        norm_copy_bytes(i->buf, (Int)sizeof i->buf, src.p + a, b - a));
    return static_bytes(src.p + a, b - a, src.len - a);
}

Int burrow__norm_iter_pos(const NormIter *i) {
    return i->p;
}

bool burrow__norm_iter_done(const NormIter *i) {
    return i->p >= i->rb.nsrc;
}

Slice burrow__norm_iter_next(NormIter *i) {
    if (i->next == NULL)
        nil_func_call();
    return i->next(i);
}

static Slice next_ascii_bytes(NormIter *i) {
    Int p = i->p + 1;
    if (p >= i->rb.nsrc) {
        Int p0 = i->p;
        iter_set_done(i);
        return iter_return_slice(i, p0, p);
    }
    if (in_byte(i->rb.src, p) < 0x80) {
        Int p0 = i->p;
        i->p = p;
        return iter_return_slice(i, p0, p);
    }
    i->info = i->rb.f->info(i->rb.src, i->p);
    i->next = i->rb.f->next_main;
    return i->next(i);
}

static Slice next_ascii_string(NormIter *i) {
    Int p = i->p + 1;
    if (p >= i->rb.nsrc) {
        i->buf[0] = in_byte(i->rb.src, i->p);
        iter_set_done(i);
        return iter_buf(i, 1);
    }
    if (in_byte(i->rb.src, p) < 0x80) {
        i->buf[0] = in_byte(i->rb.src, i->p);
        i->p = p;
        return iter_buf(i, 1);
    }
    i->info = i->rb.f->info(i->rb.src, i->p);
    i->next = i->rb.f->next_main;
    return i->next(i);
}

static Slice next_hangul(NormIter *i) {
    Int p = i->p;
    Int next = p + HANGUL_UTF8_SIZE;
    if (next >= i->rb.nsrc) {
        iter_set_done(i);
    } else if (in_hangul(i->rb.src, next) == 0) {
        ss_next(&i->rb.ss, i->info);
        i->info = i->rb.f->info(i->rb.src, i->p);
        i->next = i->rb.f->next_main;
        return i->next(i);
    }
    i->p = next;
    return iter_buf(i, burrow__norm_decompose_hangul(i->buf, in_hangul(i->rb.src, p)));
}

/* nextMulti iterates over a decomposition of more than one segment, for the
 * decomposing forms. */
static Slice next_multi(NormIter *i) {
    Int j = 0;
    Slice d = i->multi_seg;
    const Byte *dp = (const Byte *)d.p;
    /* skip first rune */
    for (j = 1; j < d.len && !utf8_rune_start(dp[j]); j++) {
    }
    while (j < d.len) {
        NormProperties info = i->rb.f->info(burrow__norm_input_bytes(d), j);
        if (burrow__norm_boundary_before(info)) {
            i->multi_seg = btail(d, j);
            return norm_bytes_of(d.p, j, d.cap);
        }
        j += info.size;
    }
    /* treat last segment as normal decomposition */
    i->next = i->rb.f->next_main;
    return i->next(i);
}

static Slice do_norm_composed(NormIter *i);

/* nextMultiNorm is the same for the composing forms. */
static Slice next_multi_norm(NormIter *i) {
    Int j = 0;
    Slice d = i->multi_seg;
    NormInput din = burrow__norm_input_bytes(d);
    while (j < d.len) {
        NormProperties info = i->rb.f->info(din, j);
        if (burrow__norm_boundary_before(info)) {
            rb_compose(&i->rb);
            Slice seg = iter_buf(i, rb_flush_copy(&i->rb, i->buf, (Int)sizeof i->buf));
            insert_unsafe(&i->rb, din, j, info);
            i->multi_seg = btail(d, j + info.size);
            return seg;
        }
        insert_unsafe(&i->rb, din, j, info);
        j += info.size;
    }
    i->multi_seg = slice_nil(TYPE_BYTE);
    i->next = next_composed;
    return do_norm_composed(i);
}

static Slice do_norm_decomposed(NormIter *i) {
    for (;;) {
        insert_unsafe(&i->rb, i->rb.src, i->p, i->info);
        i->p += i->info.size;
        if (i->p >= i->rb.nsrc) {
            iter_set_done(i);
            break;
        }
        i->info = i->rb.f->info(i->rb.src, i->p);
        if (i->info.ccc == 0)
            break;
        if (ss_next(&i->rb.ss, i->info) == SS_OVERFLOW) {
            i->next = next_cgj_decompose;
            break;
        }
    }
    /* new segment or too many combining characters: exit normalization */
    return iter_buf(i, rb_flush_copy(&i->rb, i->buf, (Int)sizeof i->buf));
}

static Slice next_cgj_decompose(NormIter *i) {
    i->rb.ss = 0;
    insert_cgj(&i->rb);
    i->next = next_decomposed;
    ss_first(&i->rb.ss, i->info);
    return do_norm_decomposed(i);
}

/* nextDecomposed is Next for NFD and NFKD. */
static Slice next_decomposed(NormIter *i) {
    Int outp = 0;
    Int in_copy_start = i->p, out_copy_start = 0;
    for (;;) {
        Int sz = i->info.size;
        Slice d = slice_nil(TYPE_BYTE);
        Rune r = 0;
        if (sz > 1) {
            d = burrow__norm_decomposition(i->info);
            if (d.p == NULL)
                r = in_hangul(i->rb.src, i->p);
        }
        if (sz <= 1) {
            i->rb.ss = 0;
            Int p = i->p;
            i->p++; /* ASCII or illegal byte. Either way, advance by 1. */
            if (i->p >= i->rb.nsrc) {
                iter_set_done(i);
                return iter_return_slice(i, p, i->p);
            }
            if (in_byte(i->rb.src, i->p) < 0x80) {
                i->next = i->ascii_f;
                return iter_return_slice(i, p, i->p);
            }
            outp++;
        } else if (d.p != NULL) {
            /* If the leading CCC is not 0, then len(d) == 2 and the last is
             * not 0 either. Case 1: there is a leftover to copy, so the
             * decomposition starts with a modifier and should always be
             * appended. Case 2: no leftover, so just return d if a rune with
             * ccc 0 follows. */
            Int p = outp + d.len;
            if (outp > 0) {
                check_slice(out_copy_start, (Int)sizeof i->buf, (Int)sizeof i->buf);
                copy_slice(i->buf + out_copy_start, (Int)sizeof i->buf - out_copy_start,
                           i->rb.src, in_copy_start, i->p);
                /* Go's code says this should not be possible and checks for
                 * it anyway. */
                if (p > (Int)sizeof i->buf)
                    return iter_buf(i, outp);
            } else if (multi_segment(i->info)) {
                /* outp is 0 here, as multi-segment decompositions always start
                 * a new segment. */
                if (i->multi_seg.p == NULL) {
                    i->multi_seg = d;
                    i->next = next_multi;
                    return next_multi(i);
                }
                /* We are in the last segment. Treat as normal decomposition. */
                d = i->multi_seg;
                i->multi_seg = slice_nil(TYPE_BYTE);
                p = d.len;
            }
            uint8_t prev_cc = i->info.tccc;
            i->p += sz;
            if (i->p >= i->rb.nsrc) {
                iter_set_done(i);
                NormProperties z = {0}; /* Force BoundaryBefore to succeed. */
                i->info = z;
            } else {
                i->info = i->rb.f->info(i->rb.src, i->p);
            }
            int s = ss_next(&i->rb.ss, i->info);
            if (s == SS_OVERFLOW)
                i->next = next_cgj_decompose;
            if (s == SS_OVERFLOW || s == SS_STARTER) {
                if (outp > 0) {
                    norm_copy_bytes(i->buf + outp, (Int)sizeof i->buf - outp,
                                    (const Byte *)d.p, d.len);
                    return iter_buf(i, p);
                }
                return d;
            }
            norm_copy_bytes(i->buf + outp, (Int)sizeof i->buf - outp, (const Byte *)d.p,
                            d.len);
            outp = p;
            in_copy_start = i->p;
            out_copy_start = outp;
            if (i->info.ccc < prev_cc)
                goto do_norm;
            continue;
        } else if (r != 0) {
            outp = burrow__norm_decompose_hangul(i->buf, r);
            i->p += HANGUL_UTF8_SIZE;
            in_copy_start = i->p;
            out_copy_start = outp;
            if (i->p >= i->rb.nsrc) {
                iter_set_done(i);
                break;
            }
            if (in_hangul(i->rb.src, i->p) != 0) {
                i->next = next_hangul;
                return iter_buf(i, outp);
            }
        } else {
            Int p = outp + sz;
            if (p > (Int)sizeof i->buf)
                break;
            outp = p;
            i->p += sz;
        }
        if (i->p >= i->rb.nsrc) {
            iter_set_done(i);
            break;
        }
        uint8_t prev_cc = i->info.tccc;
        i->info = i->rb.f->info(i->rb.src, i->p);
        int v = ss_next(&i->rb.ss, i->info);
        if (v == SS_STARTER)
            break;
        if (v == SS_OVERFLOW) {
            i->next = next_cgj_decompose;
            break;
        }
        if (i->info.ccc < prev_cc)
            goto do_norm;
    }
    if (out_copy_start == 0)
        return iter_return_slice(i, in_copy_start, i->p);
    if (in_copy_start < i->p) {
        check_slice(out_copy_start, (Int)sizeof i->buf, (Int)sizeof i->buf);
        copy_slice(i->buf + out_copy_start, (Int)sizeof i->buf - out_copy_start,
                   i->rb.src, in_copy_start, i->p);
    }
    return iter_buf(i, outp);
do_norm:
    /* Insert what has been decomposed so far into the reorder buffer. It is
     * only reordered, so there is always room. */
    check_slice(out_copy_start, (Int)sizeof i->buf, (Int)sizeof i->buf);
    copy_slice(i->buf + out_copy_start, (Int)sizeof i->buf - out_copy_start, i->rb.src,
               in_copy_start, i->p);
    insert_decomposed(&i->rb, iter_buf(i, outp));
    return do_norm_decomposed(i);
}

static Slice next_cgj_compose(NormIter *i);

/* nextComposed is Next for NFC and NFKC. */
static Slice next_composed(NormIter *i) {
    Int outp = 0, startp = i->p;
    uint8_t prev_cc = 0;
    for (;;) {
        if (!is_yes_c(i->info))
            goto do_norm;
        prev_cc = i->info.tccc;
        Int sz = i->info.size;
        if (sz == 0)
            sz = 1; /* illegal rune: copy byte-by-byte */
        Int p = outp + sz;
        if (p > (Int)sizeof i->buf)
            break;
        outp = p;
        i->p += sz;
        if (i->p >= i->rb.nsrc) {
            iter_set_done(i);
            break;
        }
        if (in_byte(i->rb.src, i->p) < 0x80) {
            i->rb.ss = 0;
            i->next = i->ascii_f;
            break;
        }
        i->info = i->rb.f->info(i->rb.src, i->p);
        int v = ss_next(&i->rb.ss, i->info);
        if (v == SS_STARTER)
            break;
        if (v == SS_OVERFLOW) {
            i->next = next_cgj_compose;
            break;
        }
        if (i->info.ccc < prev_cc)
            goto do_norm;
    }
    return iter_return_slice(i, startp, i->p);
do_norm:
    /* reset to start position */
    i->p = startp;
    i->info = i->rb.f->info(i->rb.src, i->p);
    ss_first(&i->rb.ss, i->info);
    if (multi_segment(i->info)) {
        Slice d = burrow__norm_decomposition(i->info);
        NormInput din = burrow__norm_input_bytes(d);
        NormProperties info = i->rb.f->info(din, 0);
        insert_unsafe(&i->rb, din, 0, info);
        i->multi_seg = btail(d, info.size);
        i->next = next_multi_norm;
        return next_multi_norm(i);
    }
    ss_first(&i->rb.ss, i->info);
    insert_unsafe(&i->rb, i->rb.src, i->p, i->info);
    return do_norm_composed(i);
}

static Slice do_norm_composed(NormIter *i) {
    /* First rune should already be inserted. */
    for (;;) {
        i->p += i->info.size;
        if (i->p >= i->rb.nsrc) {
            iter_set_done(i);
            break;
        }
        i->info = i->rb.f->info(i->rb.src, i->p);
        int s = ss_next(&i->rb.ss, i->info);
        if (s == SS_STARTER)
            break;
        if (s == SS_OVERFLOW) {
            i->next = next_cgj_compose;
            break;
        }
        insert_unsafe(&i->rb, i->rb.src, i->p, i->info);
    }
    rb_compose(&i->rb);
    return iter_buf(i, rb_flush_copy(&i->rb, i->buf, (Int)sizeof i->buf));
}

static Slice next_cgj_compose(NormIter *i) {
    i->rb.ss = 0; /* instead of first */
    insert_cgj(&i->rb);
    i->next = next_composed;
    /* Any rune with nLeadingNonStarters > 0 counts as a non-starter here,
     * even when it is not one, which is dubious for U+FF9E and U+FF9A. */
    ss_first(&i->rb.ss, i->info);
    insert_unsafe(&i->rb, i->rb.src, i->p, i->info);
    return do_norm_composed(i);
}

/* ---------------------------------------------------------------- transform */

/* flushTransform writes the segment out, all of it or nothing. */
static bool flush_transform(NormReorderBuffer *rb) {
    if (rb->out.len < rb->nrune * UTF8_UTF_MAX)
        return false;
    Int n = rb_flush_copy(rb, (Byte *)rb->out.p, rb->out.len);
    rb->out = btail(rb->out, n);
    return true;
}

/* transform is the slow path of Transform, for when quickSpan does not pass. */
static Int form_transform(NormForm f, Slice dst, Slice src, bool at_eof, Int *n_src,
                          Error *err) {
    Int n_dst = 0, ns = 0;
    Error e = BURROW_NO_ERROR;
    NormReorderBuffer rb;
    rb_start(&rb, NULL, form_info(f), burrow__norm_input_bytes(src));
    for (;;) {
        /* Load segment into reorder buffer. */
        rb_set_out(&rb, btail(dst, n_dst), false);
        rb.flush_f = flush_transform;
        Int end = decompose_segment(&rb, ns, at_eof);
        if (end < 0) {
            *n_src = ns;
            *err = end == I_SHORT_DST ? burrow__transform_err_short_dst
                                      : burrow__transform_err_short_src;
            return n_dst;
        }
        n_dst = dst.len - rb.out.len;
        ns = end;

        /* Next quickSpan. */
        end = rb.nsrc;
        bool eof = at_eof;
        Int n = ns + dst.len - n_dst;
        if (n < end) {
            e = burrow__transform_err_short_dst;
            end = n;
            eof = false;
        }
        bool ok = false;
        end = quick_span(rb.f, rb.src, ns, end, eof, &ok);
        check_slice(ns, end, src.len);
        n = end == ns ? 0
                      : norm_copy_bytes((Byte *)dst.p + n_dst, dst.len - n_dst,
                                        (const Byte *)src.p + ns, end - ns);
        ns += n;
        n_dst += n;
        if (ok) {
            if (!BURROW_FAILED(e) && n < rb.nsrc && !at_eof)
                e = burrow__transform_err_short_src;
            *n_src = ns;
            *err = e;
            return n_dst;
        }
    }
}

Int burrow__norm_transform(NormForm f, Slice dst, Slice src, bool at_eof, Int *n_src,
                           Error *err) {
    Int ns_dummy = 0;
    Error err_dummy = BURROW_NO_ERROR;
    if (n_src == NULL)
        n_src = &ns_dummy;
    if (err == NULL)
        err = &err_dummy;

    /* Cap the maximum number of src bytes to check. */
    Slice b = src;
    bool eof = at_eof;
    Error e = BURROW_NO_ERROR;
    if (dst.len < b.len) {
        e = burrow__transform_err_short_dst;
        eof = false;
        b.len = dst.len;
    }
    bool ok = false;
    Int i = quick_span(form_info(f), burrow__norm_input_bytes(b), 0, b.len, eof, &ok);
    Int n = i == 0 ? 0 : norm_copy_bytes((Byte *)dst.p, dst.len, (const Byte *)b.p, i);
    if (!ok) {
        Int ns = 0;
        Int nd = form_transform(f, btail(dst, n), btail(src, n), at_eof, &ns, err);
        *n_src = ns + n;
        return nd + n;
    }
    if (!BURROW_FAILED(e) && n < src.len && !at_eof)
        e = burrow__transform_err_short_src;
    *n_src = n;
    *err = e;
    return n;
}

static const Type form_desc = {
    {(const Byte *)"Form", 4},
    {(const Byte *)"golang.org/x/text/unicode/norm", 30},
    KIND_INT,
    (uint32_t)sizeof(NormForm),
    (uint16_t)_Alignof(NormForm),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

static const NormForm forms[4] = {NORM_NFC, NORM_NFD, NORM_NFKC, NORM_NFKD};

static Int form_transform_method(void *self, Slice dst, Slice src, bool at_eof,
                                 Int *n_src, Error *err) {
    return burrow__norm_transform(*(const NormForm *)self, dst, src, at_eof, n_src,
                                  err);
}

static void form_reset(void *self) {
    (void)self;
}

static Int form_span(void *self, Slice src, bool at_eof, Error *err) {
    return burrow__norm_span(*(const NormForm *)self, src, at_eof, err);
}

static const TransformSpanningTransformerVT form_vt = {
    {&form_desc, form_transform_method, form_reset},
    form_span,
};

TransformSpanningTransformer burrow__norm_transformer(NormForm f) {
    check_index((Int)f, 4);
    TransformSpanningTransformer t = {&form_vt, (void *)(uintptr_t)&forms[f]};
    return t;
}

/* ------------------------------------------------------------ Reader, Writer */

enum { NORM_IO_CHUNK = 4000 };

static const Type norm_writer_desc = {
    {(const Byte *)"normWriter", 10},
    {(const Byte *)"golang.org/x/text/unicode/norm", 30},
    KIND_STRUCT,
    (uint32_t)sizeof(NormWriter),
    (uint16_t)_Alignof(NormWriter),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

static const Type norm_reader_desc = {
    {(const Byte *)"normReader", 10},
    {(const Byte *)"golang.org/x/text/unicode/norm", 30},
    KIND_STRUCT,
    (uint32_t)sizeof(NormReader),
    (uint16_t)_Alignof(NormReader),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

NormWriter *burrow__norm_new_writer(Alloc *a, NormForm f, IoWriter w) {
    const NormFormInfo *fi = form_info(f);
    NormWriter *wr = mem_alloc(a, sizeof *wr, _Alignof(NormWriter));
    if (wr == NULL)
        return NULL;
    rb_start(&wr->rb, a, fi, burrow__norm_input_bytes(slice_nil(TYPE_BYTE)));
    wr->w = w;
    wr->buf = slice_nil(TYPE_BYTE);
    wr->a = a;
    return wr;
}

void burrow__norm_writer_free(NormWriter *w) {
    if (w == NULL)
        return;
    Alloc *a = w->a;
    burrow__norm_rb_free_out(&w->rb);
    if (w->buf.p != NULL)
        mem_free(a, w->buf.p, (size_t)w->buf.cap, 1);
    mem_free(a, w, sizeof *w, _Alignof(NormWriter));
}

/* Write normalises data and writes out what is up to the last boundary. The
 * rest is kept for the next Write or for Close. */
Int burrow__norm_writer_write(NormWriter *w, Slice data, Error *err) {
    Int n = 0;
    Error e = BURROW_NO_ERROR;
    if (w->rb.oom) {
        *err = burrow_err_out_of_memory;
        return 0;
    }
    /* Process data in pieces to keep w.buf size bounded. */
    while (data.len > 0) {
        /* Normalize into w.buf. */
        Int m = min_int(data.len, NORM_IO_CHUNK);
        w->rb.src = input_raw((const Byte *)data.p, m);
        w->rb.nsrc = m;
        w->buf = norm_do_append(&w->rb, w->buf, true, 0);
        w->rb.own_out = false;
        if (w->rb.oom) {
            *err = burrow_err_out_of_memory;
            return n;
        }
        data = btail(data, m);
        n += m;

        /* Write out complete prefix, save remainder. lastBoundary looks back
         * at most 31 runes. */
        Int i = last_boundary(w->rb.f, (const Byte *)w->buf.p, w->buf.len);
        if (i == -1)
            i = 0;
        if (i > 0) {
            Error we = BURROW_NO_ERROR;
            BURROW_CALL(w->w, write, norm_bytes_of(w->buf.p, i, w->buf.cap), &we);
            if (BURROW_FAILED(we)) {
                e = we;
                break;
            }
            Int bn = norm_copy_bytes((Byte *)w->buf.p, w->buf.len,
                                     (const Byte *)w->buf.p + i, w->buf.len - i);
            w->buf.len = bn;
        }
    }
    *err = e;
    return n;
}

/* Close writes out what is still held. */
Error burrow__norm_writer_close(NormWriter *w) {
    if (w->buf.len > 0) {
        Error e = BURROW_NO_ERROR;
        BURROW_CALL(w->w, write, w->buf, &e);
        if (BURROW_FAILED(e))
            return e;
    }
    return BURROW_NO_ERROR;
}

static Int norm_writer_io_write(void *self, Slice p, Error *err) {
    return burrow__norm_writer_write((NormWriter *)self, p, err);
}

static Error norm_writer_io_close(void *self) {
    return burrow__norm_writer_close((NormWriter *)self);
}

static const IoWriteCloserVT norm_writer_vt = {
    {&norm_writer_desc, norm_writer_io_write},
    {&norm_writer_desc, norm_writer_io_close}};

IoWriteCloser burrow__norm_writer_as_io_write_closer(NormWriter *w) {
    IoWriteCloser out = {&norm_writer_vt, w};
    return out;
}

NormReader *burrow__norm_new_reader(Alloc *a, NormForm f, IoReader r) {
    const NormFormInfo *fi = form_info(f);
    NormReader *rr = mem_alloc(a, sizeof *rr, _Alignof(NormReader));
    if (rr == NULL)
        return NULL;
    Byte *buf = mem_alloc(a, NORM_IO_CHUNK, 1);
    if (buf == NULL) {
        mem_free(a, rr, sizeof *rr, _Alignof(NormReader));
        return NULL;
    }
    rr->inbuf = norm_bytes_of(buf, NORM_IO_CHUNK, NORM_IO_CHUNK);
    rb_start(&rr->rb, a, fi, burrow__norm_input_bytes(rr->inbuf));
    rr->r = r;
    rr->outbuf = slice_nil(TYPE_BYTE);
    rr->err = BURROW_NO_ERROR;
    rr->a = a;
    return rr;
}

void burrow__norm_reader_free(NormReader *r) {
    if (r == NULL)
        return;
    Alloc *a = r->a;
    burrow__norm_rb_free_out(&r->rb);
    if (r->outbuf.p != NULL)
        mem_free(a, r->outbuf.p, (size_t)r->outbuf.cap, 1);
    mem_free(a, r->inbuf.p, NORM_IO_CHUNK, 1);
    mem_free(a, r, sizeof *r, _Alignof(NormReader));
}

Int burrow__norm_reader_read(NormReader *r, Slice p, Error *err) {
    for (;;) {
        if (r->last_boundary - r->buf_start > 0) {
            Int n = norm_copy_bytes((Byte *)p.p, p.len,
                                    (const Byte *)r->outbuf.p + r->buf_start,
                                    r->last_boundary - r->buf_start);
            r->buf_start += n;
            if (r->last_boundary - r->buf_start > 0) {
                *err = BURROW_NO_ERROR;
                return n;
            }
            *err = r->err;
            return n;
        }
        if (BURROW_FAILED(r->err)) {
            *err = r->err;
            return 0;
        }
        Int outn = r->outbuf.len - r->last_boundary;
        if (outn > 0)
            memmove(r->outbuf.p, (const Byte *)r->outbuf.p + r->last_boundary,
                    (size_t)outn);
        r->outbuf.len = outn;
        r->buf_start = 0;

        Error e = BURROW_NO_ERROR;
        Int n = BURROW_CALL(r->r, read, r->inbuf, &e);
        r->rb.src = input_raw((const Byte *)r->inbuf.p, n);
        r->rb.nsrc = n;
        r->err = e;
        if (n > 0) {
            r->outbuf = norm_do_append(&r->rb, r->outbuf, true, 0);
            r->rb.own_out = false;
            if (r->rb.oom) {
                r->err = burrow_err_out_of_memory;
                r->last_boundary = 0;
                *err = r->err;
                return 0;
            }
        }
        if (burrow__transform_err_eq(e, io_eof)) {
            r->last_boundary = r->outbuf.len;
        } else {
            r->last_boundary =
                last_boundary(r->rb.f, (const Byte *)r->outbuf.p, r->outbuf.len);
            if (r->last_boundary == -1)
                r->last_boundary = 0;
        }
    }
}

static Int norm_reader_io_read(void *self, Slice p, Error *err) {
    return burrow__norm_reader_read((NormReader *)self, p, err);
}

static const IoReaderVT norm_reader_vt = {&norm_reader_desc, norm_reader_io_read};

IoReader burrow__norm_reader_as_io_reader(NormReader *r) {
    IoReader out = {&norm_reader_vt, r};
    return out;
}

/* ---------------------------------------------------------------- internals */

void burrow__norm_rb_init(NormReorderBuffer *rb, NormForm f, Slice src) {
    rb_init(rb, form_info(f), burrow__norm_input_bytes(src));
}

void burrow__norm_rb_init_string(NormReorderBuffer *rb, NormForm f, Str src) {
    rb_init(rb, form_info(f), burrow__norm_input_string(src));
}

void burrow__norm_rb_set_src(NormReorderBuffer *rb, NormInput src) {
    rb->src = src;
    rb->nsrc = src.len;
}

void burrow__norm_rb_set_flusher(NormReorderBuffer *rb, Alloc *a, Slice out,
                                 bool (*f)(NormReorderBuffer *rb)) {
    rb->a = a;
    rb_set_out(rb, out, false);
    rb->flush_f = f;
}

void burrow__norm_rb_reset(NormReorderBuffer *rb) {
    rb_reset(rb);
}

bool burrow__norm_rb_append_flush(NormReorderBuffer *rb) {
    return append_flush(rb);
}

bool burrow__norm_rb_do_flush(NormReorderBuffer *rb) {
    return rb_do_flush(rb);
}

Int burrow__norm_rb_flush_copy(NormReorderBuffer *rb, Slice buf) {
    return rb_flush_copy(rb, (Byte *)buf.p, buf.len);
}

/* flush appends the segment to out, growing it from a, and resets rb. */
Slice burrow__norm_rb_flush(NormReorderBuffer *rb, Alloc *a, Slice out) {
    Slice save = rb->out;
    bool save_own = rb->own_out;
    Alloc *save_a = rb->a;
    bool save_oom = rb->oom;
    rb->a = a;
    rb->oom = false;
    rb_set_out(rb, out, false);
    append_flush(rb);
    rb_reset(rb);
    Slice r = rb->oom ? slice_nil(TYPE_BYTE) : rb->out;
    rb->out = save;
    rb->own_out = save_own;
    rb->a = save_a;
    rb->oom = save_oom;
    return r;
}

void burrow__norm_rb_ss_first(NormReorderBuffer *rb, NormProperties p) {
    ss_first(&rb->ss, p);
}

int burrow__norm_rb_ss_next(NormReorderBuffer *rb, NormProperties p) {
    return ss_next(&rb->ss, p);
}

int burrow__norm_rb_insert_flush(NormReorderBuffer *rb, NormInput src, Int i,
                                 NormProperties info) {
    return insert_flush(rb, src, i, info);
}

void burrow__norm_rb_insert_unsafe(NormReorderBuffer *rb, NormInput src, Int i,
                                   NormProperties info) {
    insert_unsafe(rb, src, i, info);
}

int burrow__norm_rb_insert_decomposed(NormReorderBuffer *rb, Slice dcomp) {
    return insert_decomposed(rb, dcomp);
}

void burrow__norm_rb_compose(NormReorderBuffer *rb) {
    rb_compose(rb);
}

NormProperties burrow__norm_rb_info(const NormReorderBuffer *rb, NormInput src, Int i) {
    return rb->f->info(src, i);
}

Int burrow__norm_decompose_segment(NormReorderBuffer *rb, Int sp, bool at_eof) {
    return decompose_segment(rb, sp, at_eof);
}

void burrow__norm_decompose_to_last_boundary(NormReorderBuffer *rb) {
    decompose_to_last_boundary(rb);
}

/* HPACK, from Go's vendored golang.org/x/net/http2/hpack.
 *
 * Derived from Go's src/vendor/golang.org/x/net/http2/hpack/hpack.go,
 * encode.go, huffman.go and tables.go.
 * Go source: go1.27.1.
 *
 * Copyright 2014 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "hpack.h"

#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/io.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/mem/heap.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stdint.h>
#include <string.h>

enum { HP_INITIAL_TABLE_SIZE = 4096 };

/* --------------------------------------------------------------- buffers */

static bool hp_reserve(Alloc *a, burrow__HpackBuf *b, Int more) {
    if (b->failed)
        return false;
    if (more <= b->cap - b->len)
        return true;
    Int want = b->len + more;
    Int cap = b->cap * 2;
    if (cap < want)
        cap = want;
    if (cap < 64)
        cap = 64;
    Byte *p = b->p == NULL
                  ? (Byte *)mem_alloc_nozero(a, (size_t)cap, 1)
                  : (Byte *)mem_realloc(a, b->p, (size_t)b->cap, (size_t)cap, 1);
    if (p == NULL) {
        b->failed = true;
        return false;
    }
    b->p = p;
    b->cap = cap;
    return true;
}

static void hp_put(Alloc *a, burrow__HpackBuf *b, const void *p, Int n) {
    if (n <= 0 || !hp_reserve(a, b, n))
        return;
    memcpy(b->p + b->len, p, (size_t)n);
    b->len += n;
}

static void hp_put_byte(Alloc *a, burrow__HpackBuf *b, Byte c) {
    hp_put(a, b, &c, 1);
}

static void hp_buf_free(Alloc *a, burrow__HpackBuf *b) {
    if (b->p != NULL)
        mem_free(a, b->p, (size_t)b->cap, 1);
    memset(b, 0, sizeof *b);
}

/* ---------------------------------------------------------------- errors */

BURROW_SENTINEL_ERROR(burrow__hpack_err_string_length, "hpack: string too long");
BURROW_SENTINEL_ERROR(burrow__hpack_err_invalid_huffman,
                      "hpack: invalid Huffman-encoded data");
BURROW_SENTINEL_ERROR(burrow__hpack_err_need_more, "need more data");

/* DecodingError. The ones with a fixed text are constants, and fixed says so,
 * so that a clone of one is the same error. */
typedef struct HpDecoding {
    Error err;
    Str message;
    bool fixed;
} HpDecoding;

static Str hp_decoding_message(const void *self) {
    return ((const HpDecoding *)self)->message;
}

static Error hp_decoding_clone(const void *self, Alloc *a);

static const ErrorVT hp_decoding_vt = {
    .message = hp_decoding_message,
    .clone = hp_decoding_clone,
};

#define HP_FIXED(name, text)                                                           \
    static const Str name##_text = BURROW_S_INIT(text);                                \
    static const HpDecoding name = {{&burrow_sentinel_error_vt, &name##_text},         \
                                    BURROW_S_INIT("decoding error: " text),            \
                                    true}

HP_FIXED(hp_dec_varint, "varint integer overflow");
HP_FIXED(hp_dec_truncated, "truncated headers");
HP_FIXED(hp_dec_invalid, "invalid encoding");
HP_FIXED(hp_dec_update_late,
         "dynamic table size update MUST occur at the beginning of a header block");
HP_FIXED(hp_dec_update_large, "dynamic table size update too large");

const Error burrow__hpack_err_varint_overflow = {&hp_decoding_vt, &hp_dec_varint};

static Error hp_fixed(const HpDecoding *d) {
    return (Error){&hp_decoding_vt, d};
}

/* InvalidIndexError. */
typedef struct HpInvalidIndex {
    Int index;
    Str message;
} HpInvalidIndex;

static Str hp_invalid_index_message(const void *self) {
    return ((const HpInvalidIndex *)self)->message;
}

static Error hp_invalid_index_clone(const void *self, Alloc *a);

static const ErrorVT hp_invalid_index_vt = {
    .message = hp_invalid_index_message,
    .clone = hp_invalid_index_clone,
};

static Int hp_itoa(Byte buf[20], Int v) {
    uint64_t u = v < 0 ? 0 - (uint64_t)v : (uint64_t)v;
    Int i = 20;
    do {
        buf[--i] = (Byte)('0' + u % 10U);
        u /= 10U;
    } while (u != 0);
    if (v < 0)
        buf[--i] = '-';
    return i;
}

/* DecodingError{InvalidIndexError(index)}, both in one block from a. */
static Error hp_invalid_index_in(Alloc *a, Int index) {
    static const char inner_text[] = "invalid indexed representation index ";
    static const char outer_text[] = "decoding error: ";
    Byte digits[20];
    Int at = hp_itoa(digits, index);
    Int nd = 20 - at;
    Int ni = (Int)sizeof inner_text - 1 + nd;
    Int no = (Int)sizeof outer_text - 1 + ni;
    typedef struct HpBoth {
        HpDecoding outer;
        HpInvalidIndex inner;
    } HpBoth;
    HpBoth *e = (HpBoth *)mem_alloc_nozero(a, sizeof(HpBoth) + (size_t)(ni + no),
                                           _Alignof(HpBoth));
    if (e == NULL)
        return burrow_err_out_of_memory;
    Byte *p = (Byte *)(e + 1);
    memcpy(p, inner_text, sizeof inner_text - 1);
    memcpy(p + sizeof inner_text - 1, digits + at, (size_t)nd);
    Byte *q = p + ni;
    memcpy(q, outer_text, sizeof outer_text - 1);
    memcpy(q + sizeof outer_text - 1, p, (size_t)ni);
    e->inner.index = index;
    e->inner.message = str_from_bytes(p, ni);
    e->outer.err = (Error){&hp_invalid_index_vt, &e->inner};
    e->outer.message = str_from_bytes(q, no);
    e->outer.fixed = false;
    return (Error){&hp_decoding_vt, &e->outer};
}

static Error hp_invalid_index(uint64_t index) {
    return hp_invalid_index_in(error_allocator(), (Int)index);
}

static Error hp_decoding_clone(const void *self, Alloc *a) {
    const HpDecoding *d = (const HpDecoding *)self;
    if (d->fixed)
        return (Error){&hp_decoding_vt, d};
    /* The only DecodingError that is not fixed holds an InvalidIndexError. */
    const HpInvalidIndex *inner = (const HpInvalidIndex *)d->err.data;
    return hp_invalid_index_in(a, inner->index);
}

static Error hp_invalid_index_clone(const void *self, Alloc *a) {
    const HpInvalidIndex *e = (const HpInvalidIndex *)self;
    HpInvalidIndex *c = (HpInvalidIndex *)mem_alloc_nozero(
        a, sizeof(HpInvalidIndex) + (size_t)e->message.len, _Alignof(HpInvalidIndex));
    if (c == NULL)
        return burrow_err_out_of_memory;
    Byte *p = (Byte *)(c + 1);
    memcpy(p, e->message.p, (size_t)e->message.len);
    c->index = e->index;
    c->message = str_from_bytes(p, e->message.len);
    return (Error){&hp_invalid_index_vt, c};
}

bool burrow__hpack_error_decoding(Error err, Error *inner) {
    if (err.vt != &hp_decoding_vt)
        return false;
    if (inner != NULL)
        *inner = ((const HpDecoding *)err.data)->err;
    return true;
}

bool burrow__hpack_error_invalid_index(Error err, Int *index) {
    if (err.vt != &hp_invalid_index_vt)
        return false;
    if (index != NULL)
        *index = ((const HpInvalidIndex *)err.data)->index;
    return true;
}

static bool hp_is(Error a, Error b) {
    return a.vt == b.vt && a.data == b.data;
}

/* ---------------------------------------------------------- header fields */

bool burrow__hpack_header_field_is_pseudo(HpackHeaderField f) {
    return f.name.len != 0 && f.name.p[0] == ':';
}

Str burrow__hpack_header_field_string(Alloc *a, HpackHeaderField f) {
    const char *suffix = f.sensitive ? " (sensitive)" : "";
    return fmt_sprintf_v(a, "header field %q = %q%s", f.name, f.value, suffix);
}

uint32_t burrow__hpack_header_field_size(HpackHeaderField f) {
    return (uint32_t)(f.name.len + f.value.len + 32);
}

void burrow__hpack_header_fields_free(Alloc *a, HpackHeaderFields *fs) {
    for (Int i = 0; i < fs->len; i++) {
        HpackHeaderField *f = &fs->p[i];
        if (f->name.len > 0)
            mem_free(a, (void *)(uintptr_t)f->name.p, (size_t)f->name.len, 1);
        if (f->value.len > 0)
            mem_free(a, (void *)(uintptr_t)f->value.p, (size_t)f->value.len, 1);
    }
    if (fs->p != NULL)
        mem_free(a, fs->p, (size_t)fs->cap * sizeof(HpackHeaderField),
                 _Alignof(HpackHeaderField));
    memset(fs, 0, sizeof *fs);
}

/* ----------------------------------------------------------------- tables */

/* An entry's name and value live in one block: eight bytes of the name's
 * length, the name, then the value. The whole block is the entry's key in
 * by_name_value, which no other name and value can have. */
enum { HP_KEY_HEAD = 8 };

static void hp_key_head(Byte *p, Int n) {
    uint64_t u = (uint64_t)n;
    for (int i = 0; i < HP_KEY_HEAD; i++)
        p[i] = (Byte)(u >> (8 * i));
}

static Str hp_entry_key(const HpackHeaderField *f) {
    return str_from_bytes(f->name.p - HP_KEY_HEAD,
                          HP_KEY_HEAD + f->name.len + f->value.len);
}

static void hp_entry_free(Alloc *a, const HpackHeaderField *f) {
    mem_free(a, (void *)(uintptr_t)(f->name.p - HP_KEY_HEAD),
             (size_t)(HP_KEY_HEAD + f->name.len + f->value.len), 1);
}

/* The key for f, built in t->key. False when there is no room for it. */
static bool hp_query_key(HpackTable *t, HpackHeaderField f, Str *key) {
    Int n = HP_KEY_HEAD + f.name.len + f.value.len;
    if (n > t->key_cap) {
        Byte *p = (Byte *)mem_alloc_nozero(t->a, (size_t)n, 1);
        if (p == NULL)
            return false;
        if (t->key != NULL)
            mem_free(t->a, t->key, (size_t)t->key_cap, 1);
        t->key = p;
        t->key_cap = n;
    }
    hp_key_head(t->key, f.name.len);
    if (f.name.len > 0)
        memcpy(t->key + HP_KEY_HEAD, f.name.p, (size_t)f.name.len);
    if (f.value.len > 0)
        memcpy(t->key + HP_KEY_HEAD + f.name.len, f.value.p, (size_t)f.value.len);
    *key = str_from_bytes(t->key, n);
    return true;
}

bool burrow__hpack_table_init(HpackTable *t, Alloc *a) {
    memset(t, 0, sizeof *t);
    t->a = a;
    t->by_name = map_make(a, TYPE_STRING, TYPE_UINT64, 0);
    t->by_name_value = map_make(a, TYPE_STRING, TYPE_UINT64, 0);
    if (t->by_name == NULL || t->by_name_value == NULL) {
        burrow__hpack_table_free(t);
        return false;
    }
    return true;
}

void burrow__hpack_table_free(HpackTable *t) {
    for (Int k = 0; k < t->len; k++)
        hp_entry_free(t->a, burrow__hpack_table_at(t, k));
    if (t->ents != NULL)
        mem_free(t->a, t->ents, (size_t)t->cap * sizeof(HpackHeaderField),
                 _Alignof(HpackHeaderField));
    if (t->key != NULL)
        mem_free(t->a, t->key, (size_t)t->key_cap, 1);
    map_free(t->by_name);
    map_free(t->by_name_value);
    Alloc *a = t->a;
    memset(t, 0, sizeof *t);
    t->a = a;
}

const HpackHeaderField *burrow__hpack_table_at(const HpackTable *t, Int k) {
    return &t->ents[(t->head + k) % t->cap];
}

static bool hp_table_grow(HpackTable *t) {
    Int cap = t->cap == 0 ? 8 : t->cap * 2;
    HpackHeaderField *ents = (HpackHeaderField *)mem_alloc_nozero(
        t->a, (size_t)cap * sizeof(HpackHeaderField), _Alignof(HpackHeaderField));
    if (ents == NULL)
        return false;
    for (Int k = 0; k < t->len; k++)
        ents[k] = *burrow__hpack_table_at(t, k);
    if (t->ents != NULL)
        mem_free(t->a, t->ents, (size_t)t->cap * sizeof(HpackHeaderField),
                 _Alignof(HpackHeaderField));
    t->ents = ents;
    t->cap = cap;
    t->head = 0;
    return true;
}

bool burrow__hpack_table_add(HpackTable *t, HpackHeaderField f) {
    if (t->len == t->cap && !hp_table_grow(t))
        return false;
    Int n = HP_KEY_HEAD + f.name.len + f.value.len;
    Byte *p = (Byte *)mem_alloc_nozero(t->a, (size_t)n, 1);
    if (p == NULL)
        return false;
    hp_key_head(p, f.name.len);
    if (f.name.len > 0)
        memcpy(p + HP_KEY_HEAD, f.name.p, (size_t)f.name.len);
    if (f.value.len > 0)
        memcpy(p + HP_KEY_HEAD + f.name.len, f.value.p, (size_t)f.value.len);
    /* Built by hand and not with str_from_bytes, which gives an empty string a
     * NULL pointer: an empty name still has to point just past the length, or
     * hp_entry_key and hp_entry_free cannot find the block from it. */
    HpackHeaderField e;
    e.name.p = p + HP_KEY_HEAD;
    e.name.len = f.name.len;
    e.value.p = p + HP_KEY_HEAD + f.name.len;
    e.value.len = f.value.len;
    e.sensitive = f.sensitive;
    Str key = hp_entry_key(&e);

    /* The maps hold the key Strs, not the bytes, so the key an entry is found
     * by has to be this block and not an older one that is evicted first. A
     * set of a key already there keeps the key it had, so each one comes out
     * and goes back in. The maps only make search faster: a set that runs out
     * of memory leaves the key out, and search then finds an older entry or
     * none, which the encoder still writes correctly, only longer. */
    uint64_t id = (uint64_t)t->len + t->evict_count + 1;
    map_del(t->by_name, &e.name);
    (void)map_set(t->by_name, &e.name, &id);
    map_del(t->by_name_value, &key);
    (void)map_set(t->by_name_value, &key, &id);
    t->ents[(t->head + t->len) % t->cap] = e;
    t->len++;
    return true;
}

void burrow__hpack_table_evict_oldest(HpackTable *t, Int n) {
    if (n > t->len)
        panic_str(fmt_sprintf_v(heap_allocator(),
                                "evictOldest(%d) on table with %d entries", n, t->len));
    for (Int k = 0; k < n; k++) {
        const HpackHeaderField *f = burrow__hpack_table_at(t, k);
        uint64_t id = t->evict_count + (uint64_t)k + 1;
        const uint64_t *got = (const uint64_t *)map_get(t->by_name, &f->name);
        if (got != NULL && *got == id)
            map_del(t->by_name, &f->name);
        Str key = hp_entry_key(f);
        got = (const uint64_t *)map_get(t->by_name_value, &key);
        if (got != NULL && *got == id)
            map_del(t->by_name_value, &key);
        hp_entry_free(t->a, f);
    }
    if (n > 0) {
        t->head = (t->head + n) % t->cap;
        t->len -= n;
    }
    if (t->evict_count + (uint64_t)n < t->evict_count)
        panic_str(BURROW_S("evictCount overflow"));
    t->evict_count += (uint64_t)n;
}

static uint64_t hp_id_to_index(const HpackTable *t, uint64_t id) {
    if (id <= t->evict_count)
        panic_str(fmt_sprintf_v(heap_allocator(), "id (%d) <= evictCount (%d)", id,
                                t->evict_count));
    uint64_t k = id - t->evict_count - 1;
    if (!t->is_static)
        return (uint64_t)t->len - k;
    return k + 1;
}

uint64_t burrow__hpack_table_search(HpackTable *t, HpackHeaderField f,
                                    bool *name_value_match) {
    *name_value_match = false;
    Str key;
    if (!f.sensitive && hp_query_key(t, f, &key)) {
        const uint64_t *id = (const uint64_t *)map_get(t->by_name_value, &key);
        if (id != NULL && *id != 0) {
            *name_value_match = true;
            return hp_id_to_index(t, *id);
        }
    }
    const uint64_t *id = (const uint64_t *)map_get(t->by_name, &f.name);
    if (id != NULL && *id != 0)
        return hp_id_to_index(t, *id);
    return 0;
}

static int hp_compare(Str a, Str b) {
    Int n = a.len < b.len ? a.len : b.len;
    int c = n > 0 ? memcmp(a.p, b.p, (size_t)n) : 0;
    if (c != 0)
        return c;
    return a.len < b.len ? -1 : a.len > b.len ? 1 : 0;
}

uint64_t burrow__hpack_static_search(HpackHeaderField f, bool *name_value_match) {
    *name_value_match = false;
    Int lo = 0;
    Int hi = BURROW__HPACK_STATIC_NAMES;
    while (lo < hi) {
        Int mid = lo + (hi - lo) / 2;
        const burrow__HpackStaticName *n = &burrow__hpack_static_names[mid];
        int c = hp_compare(f.name, n->name);
        if (c == 0) {
            if (!f.sensitive)
                for (Int i = n->first; i <= n->last; i++)
                    if (str_eq(burrow__hpack_static[i - 1].name, f.name) &&
                        str_eq(burrow__hpack_static[i - 1].value, f.value)) {
                        *name_value_match = true;
                        return (uint64_t)i;
                    }
            return n->last;
        }
        if (c < 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    return 0;
}

/* ---------------------------------------------------------- dynamic table */

static void hp_evict(HpackDynamicTable *dt) {
    Int n = 0;
    while (dt->size > dt->max_size && n < dt->table.len) {
        dt->size -=
            burrow__hpack_header_field_size(*burrow__hpack_table_at(&dt->table, n));
        n++;
    }
    burrow__hpack_table_evict_oldest(&dt->table, n);
}

void burrow__hpack_dynamic_table_set_max_size(HpackDynamicTable *dt, uint32_t v) {
    dt->max_size = v;
    hp_evict(dt);
}

bool burrow__hpack_dynamic_table_add(HpackDynamicTable *dt, HpackHeaderField f) {
    if (!burrow__hpack_table_add(&dt->table, f))
        return false;
    dt->size += burrow__hpack_header_field_size(f);
    hp_evict(dt);
    return true;
}

/* ---------------------------------------------------------------- Huffman */

Error burrow__hpack_huffman_decode_into(Byte *out, Int cap, Int *n, Int max_len,
                                        const Byte *v, Int len) {
    unsigned node = 0;
    /* cur is the bits not yet fed to the trie, cbits how many of them there
     * are, and sbits how many bits of the symbol being decoded have been
     * read. */
    uint64_t cur = 0;
    unsigned cbits = 0;
    unsigned sbits = 0;
    Int w = 0;
    *n = 0;
    for (Int i = 0; i < len; i++) {
        cur = cur << 8 | v[i];
        cbits += 8;
        sbits += 8;
        while (cbits >= 8) {
            uint16_t e = burrow__hpack_trie[node][(Byte)(cur >> (cbits - 8))];
            if (e == 0)
                return burrow__hpack_err_invalid_huffman;
            if ((e & BURROW__HPACK_TRIE_NODE) != 0) {
                node = e & 0xFFU;
                cbits -= 8;
                continue;
            }
            if ((max_len != 0 && w == max_len) || w == cap) {
                *n = w;
                return burrow__hpack_err_string_length;
            }
            out[w++] = (Byte)e;
            cbits -= (unsigned)(e >> 8);
            node = 0;
            sbits = cbits;
        }
    }
    while (cbits > 0) {
        uint16_t e = burrow__hpack_trie[node][(Byte)(cur << (8 - cbits))];
        if (e == 0)
            return burrow__hpack_err_invalid_huffman;
        if ((e & BURROW__HPACK_TRIE_NODE) != 0 || (unsigned)(e >> 8) > cbits)
            break;
        if ((max_len != 0 && w == max_len) || w == cap) {
            *n = w;
            return burrow__hpack_err_string_length;
        }
        out[w++] = (Byte)e;
        cbits -= (unsigned)(e >> 8);
        node = 0;
        sbits = cbits;
    }
    *n = w;
    if (sbits > 7)
        /* Either a symbol was left unfinished or the padding was too long,
         * both decoding errors per RFC 7541 section 5.2. */
        return burrow__hpack_err_invalid_huffman;
    uint64_t mask = ((uint64_t)1 << cbits) - 1;
    if ((cur & mask) != mask)
        /* The padding has to be the start of EOS, which is all ones. */
        return burrow__hpack_err_invalid_huffman;
    return BURROW_NO_ERROR;
}

/* The most bytes len bytes of Huffman code can decode to: no code is shorter
 * than 5 bits. */
static Int hp_huffman_max(Int len) {
    return len / 5 * 8 + (len % 5) * 8 / 5 + 1;
}

Int burrow__hpack_huffman_decode(IoWriter w, Slice v, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    Alloc *a = heap_allocator();
    Int cap = hp_huffman_max(v.len);
    Byte *out = (Byte *)mem_alloc_nozero(a, (size_t)cap, 1);
    if (out == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return 0;
    }
    Int n = 0;
    Error e =
        burrow__hpack_huffman_decode_into(out, cap, &n, 0, (const Byte *)v.p, v.len);
    Int written = 0;
    if (BURROW_FAILED(e))
        BURROW_OUT(err, e);
    else
        written = w.vt->write(w.data, slice_from(out, n, n, TYPE_BYTE), err);
    mem_free(a, out, (size_t)cap, 1);
    return written;
}

Str burrow__hpack_huffman_decode_to_string(Alloc *a, Slice v, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    Int cap = hp_huffman_max(v.len);
    Byte *out = (Byte *)mem_alloc_nozero(a, (size_t)cap, 1);
    if (out == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return BURROW_STR_EMPTY;
    }
    Int n = 0;
    Error e =
        burrow__hpack_huffman_decode_into(out, cap, &n, 0, (const Byte *)v.p, v.len);
    if (BURROW_FAILED(e)) {
        mem_free(a, out, (size_t)cap, 1);
        BURROW_OUT(err, e);
        return BURROW_STR_EMPTY;
    }
    return str_from_bytes(out, n);
}

uint64_t burrow__hpack_huffman_encode_length(Str s) {
    uint64_t n = 0;
    for (Int i = 0; i < s.len; i++)
        n += burrow__hpack_huffman_code_len[s.p[i]];
    return (n + 7) / 8;
}

static void hp_put_huffman(Alloc *a, burrow__HpackBuf *b, Str s) {
    /* The longest code is 30 bits, so with fewer than 32 bits waiting in x
     * there is always room for one more. */
    uint64_t x = 0;
    unsigned n = 0;
    Byte out[4];
    if (!hp_reserve(a, b, (Int)burrow__hpack_huffman_encode_length(s)))
        return;
    for (Int i = 0; i < s.len; i++) {
        Byte c = s.p[i];
        n += burrow__hpack_huffman_code_len[c];
        x <<= burrow__hpack_huffman_code_len[c] % 64U;
        x |= burrow__hpack_huffman_codes[c];
        if (n >= 32) {
            n %= 32;
            uint32_t y = (uint32_t)(x >> n);
            out[0] = (Byte)(y >> 24);
            out[1] = (Byte)(y >> 16);
            out[2] = (Byte)(y >> 8);
            out[3] = (Byte)y;
            hp_put(a, b, out, 4);
        }
    }
    /* Pad with the start of EOS, which is all ones. */
    unsigned over = n % 8;
    if (over > 0) {
        unsigned pad = 8 - over;
        x = (x << pad) | (0xFFU >> over);
        n += pad;
    }
    switch (n / 8) {
    case 0:
        return;
    case 1:
        hp_put_byte(a, b, (Byte)x);
        return;
    case 2:
        out[0] = (Byte)(x >> 8);
        out[1] = (Byte)x;
        hp_put(a, b, out, 2);
        return;
    case 3:
        out[0] = (Byte)(x >> 16);
        out[1] = (Byte)(x >> 8);
        out[2] = (Byte)x;
        hp_put(a, b, out, 3);
        return;
    default:
        break;
    }
    out[0] = (Byte)(x >> 24);
    out[1] = (Byte)(x >> 16);
    out[2] = (Byte)(x >> 8);
    out[3] = (Byte)x;
    hp_put(a, b, out, 4);
}

/* -------------------------------------------------------- encoding pieces */

static void hp_put_var_int(Alloc *a, burrow__HpackBuf *b, Byte n, uint64_t i) {
    uint64_t k = ((uint64_t)1 << n) - 1;
    if (i < k) {
        hp_put_byte(a, b, (Byte)i);
        return;
    }
    hp_put_byte(a, b, (Byte)k);
    i -= k;
    for (; i >= 128; i >>= 7)
        hp_put_byte(a, b, (Byte)(0x80U | (i & 0x7FU)));
    hp_put_byte(a, b, (Byte)i);
}

/* Huffman only when that is strictly shorter. */
static void hp_put_string(Alloc *a, burrow__HpackBuf *b, Str s) {
    uint64_t huffman_length = burrow__hpack_huffman_encode_length(s);
    if (huffman_length < (uint64_t)s.len) {
        Int first = b->len;
        hp_put_var_int(a, b, 7, huffman_length);
        hp_put_huffman(a, b, s);
        if (!b->failed)
            b->p[first] |= 0x80;
    } else {
        hp_put_var_int(a, b, 7, (uint64_t)s.len);
        hp_put(a, b, s.p, s.len);
    }
}

Byte burrow__hpack_encode_type_byte(bool indexing, bool sensitive) {
    if (sensitive)
        return 0x10;
    if (indexing)
        return 0x40;
    return 0;
}

static void hp_put_indexed(Alloc *a, burrow__HpackBuf *b, uint64_t i) {
    Int first = b->len;
    hp_put_var_int(a, b, 7, i);
    if (!b->failed)
        b->p[first] |= 0x80;
}

static void hp_put_new_name(Alloc *a, burrow__HpackBuf *b, HpackHeaderField f,
                            bool indexing) {
    hp_put_byte(a, b, burrow__hpack_encode_type_byte(indexing, f.sensitive));
    hp_put_string(a, b, f.name);
    hp_put_string(a, b, f.value);
}

static void hp_put_indexed_name(Alloc *a, burrow__HpackBuf *b, HpackHeaderField f,
                                uint64_t i, bool indexing) {
    Int first = b->len;
    hp_put_var_int(a, b, indexing ? 6 : 4, i);
    if (!b->failed)
        b->p[first] |= burrow__hpack_encode_type_byte(indexing, f.sensitive);
    hp_put_string(a, b, f.value);
}

static void hp_put_table_size(Alloc *a, burrow__HpackBuf *b, uint32_t v) {
    Int first = b->len;
    hp_put_var_int(a, b, 5, v);
    if (!b->failed)
        b->p[first] |= 0x20;
}

/* What the append functions share: write into a buffer of a's, then append
 * that to dst. */
typedef struct HpAppend {
    Alloc *a;
    burrow__HpackBuf b;
} HpAppend;

static Slice hp_append_done(HpAppend *h, Slice dst) {
    if (dst.elem == NULL)
        dst = slice_nil(TYPE_BYTE);
    if (!h->b.failed)
        dst = slice_append(h->a, dst, h->b.p, h->b.len);
    hp_buf_free(h->a, &h->b);
    return dst;
}

Slice burrow__hpack_append_var_int(Alloc *a, Slice dst, Byte n, uint64_t i) {
    HpAppend h = {a, {0}};
    hp_put_var_int(a, &h.b, n, i);
    return hp_append_done(&h, dst);
}

Slice burrow__hpack_append_hpack_string(Alloc *a, Slice dst, Str s) {
    HpAppend h = {a, {0}};
    hp_put_string(a, &h.b, s);
    return hp_append_done(&h, dst);
}

Slice burrow__hpack_append_indexed(Alloc *a, Slice dst, uint64_t i) {
    HpAppend h = {a, {0}};
    hp_put_indexed(a, &h.b, i);
    return hp_append_done(&h, dst);
}

Slice burrow__hpack_append_new_name(Alloc *a, Slice dst, HpackHeaderField f,
                                    bool indexing) {
    HpAppend h = {a, {0}};
    hp_put_new_name(a, &h.b, f, indexing);
    return hp_append_done(&h, dst);
}

Slice burrow__hpack_append_indexed_name(Alloc *a, Slice dst, HpackHeaderField f,
                                        uint64_t i, bool indexing) {
    HpAppend h = {a, {0}};
    hp_put_indexed_name(a, &h.b, f, i, indexing);
    return hp_append_done(&h, dst);
}

Slice burrow__hpack_append_table_size(Alloc *a, Slice dst, uint32_t v) {
    HpAppend h = {a, {0}};
    hp_put_table_size(a, &h.b, v);
    return hp_append_done(&h, dst);
}

Slice burrow__hpack_append_huffman_string(Alloc *a, Slice dst, Str s) {
    HpAppend h = {a, {0}};
    hp_put_huffman(a, &h.b, s);
    return hp_append_done(&h, dst);
}

/* ---------------------------------------------------------------- encoder */

HpackEncoder *burrow__hpack_new_encoder(Alloc *a, IoWriter w) {
    HpackEncoder *e = (HpackEncoder *)mem_alloc(a, sizeof *e, _Alignof(HpackEncoder));
    if (e == NULL)
        return NULL;
    if (!burrow__hpack_table_init(&e->dyn_tab.table, a)) {
        mem_free(a, e, sizeof *e, _Alignof(HpackEncoder));
        return NULL;
    }
    e->a = a;
    e->w = w;
    e->min_size = UINT32_MAX;
    e->max_size_limit = HP_INITIAL_TABLE_SIZE;
    e->table_size_update = false;
    burrow__hpack_dynamic_table_set_max_size(&e->dyn_tab, HP_INITIAL_TABLE_SIZE);
    return e;
}

void burrow__hpack_encoder_free(HpackEncoder *e) {
    if (e == NULL)
        return;
    Alloc *a = e->a;
    burrow__hpack_table_free(&e->dyn_tab.table);
    hp_buf_free(a, &e->buf);
    mem_free(a, e, sizeof *e, _Alignof(HpackEncoder));
}

uint64_t burrow__hpack_encoder_search_table(HpackEncoder *e, HpackHeaderField f,
                                            bool *name_value_match) {
    uint64_t i = burrow__hpack_static_search(f, name_value_match);
    if (*name_value_match)
        return i;
    uint64_t j = burrow__hpack_table_search(&e->dyn_tab.table, f, name_value_match);
    if (*name_value_match || (i == 0 && j != 0))
        return j + BURROW__HPACK_STATIC_LEN;
    return i;
}

Error burrow__hpack_encoder_write_field(HpackEncoder *e, HpackHeaderField f) {
    Alloc *a = e->a;
    e->buf.len = 0;
    e->buf.failed = false;
    /* Room for all of it up front: two size updates, a type byte and an index
     * of up to ten bytes each, and two strings no longer than they are with
     * ten bytes of length each. Then nothing below can fail after the table
     * has changed, which would leave it out of step with the peer's. */
    if (!hp_reserve(a, &e->buf, 64 + f.name.len + f.value.len))
        return burrow_err_out_of_memory;

    if (e->table_size_update) {
        e->table_size_update = false;
        if (e->min_size < e->dyn_tab.max_size)
            hp_put_table_size(a, &e->buf, e->min_size);
        e->min_size = UINT32_MAX;
        hp_put_table_size(a, &e->buf, e->dyn_tab.max_size);
    }

    bool name_value_match = false;
    uint64_t idx = burrow__hpack_encoder_search_table(e, f, &name_value_match);
    if (name_value_match) {
        hp_put_indexed(a, &e->buf, idx);
    } else {
        bool indexing =
            !f.sensitive && burrow__hpack_header_field_size(f) <= e->dyn_tab.max_size;
        if (indexing && !burrow__hpack_dynamic_table_add(&e->dyn_tab, f))
            return burrow_err_out_of_memory;
        if (idx == 0)
            hp_put_new_name(a, &e->buf, f, indexing);
        else
            hp_put_indexed_name(a, &e->buf, f, idx, indexing);
    }
    if (e->buf.failed)
        return burrow_err_out_of_memory;
    if (e->w.vt == NULL)
        panic_str(BURROW_S("hpack: WriteField on an Encoder with no writer"));
    Error err = BURROW_NO_ERROR;
    Int n = e->w.vt->write(
        e->w.data, slice_from(e->buf.p, e->buf.len, e->buf.len, TYPE_BYTE), &err);
    if (BURROW_OK(err) && n != e->buf.len)
        err = io_err_short_write;
    return err;
}

void burrow__hpack_encoder_set_max_dynamic_table_size(HpackEncoder *e, uint32_t v) {
    if (v > e->max_size_limit)
        v = e->max_size_limit;
    if (v < e->min_size)
        e->min_size = v;
    e->table_size_update = true;
    burrow__hpack_dynamic_table_set_max_size(&e->dyn_tab, v);
}

uint32_t burrow__hpack_encoder_max_dynamic_table_size(const HpackEncoder *e) {
    return e->dyn_tab.max_size;
}

void burrow__hpack_encoder_set_max_dynamic_table_size_limit(HpackEncoder *e,
                                                            uint32_t v) {
    e->max_size_limit = v;
    if (e->dyn_tab.max_size > v) {
        e->table_size_update = true;
        burrow__hpack_dynamic_table_set_max_size(&e->dyn_tab, v);
    }
}

/* ---------------------------------------------------------------- decoder */

HpackDecoder *burrow__hpack_new_decoder(Alloc *a, uint32_t max_dynamic_table_size,
                                        HpackEmitFunc emit, void *ctx) {
    HpackDecoder *d = (HpackDecoder *)mem_alloc(a, sizeof *d, _Alignof(HpackDecoder));
    if (d == NULL)
        return NULL;
    if (!burrow__hpack_table_init(&d->dyn_tab.table, a)) {
        mem_free(a, d, sizeof *d, _Alignof(HpackDecoder));
        return NULL;
    }
    d->a = a;
    d->emit = emit;
    d->emit_ctx = ctx;
    d->emit_enabled = true;
    d->first_field = true;
    d->dyn_tab.allowed_max_size = max_dynamic_table_size;
    burrow__hpack_dynamic_table_set_max_size(&d->dyn_tab, max_dynamic_table_size);
    return d;
}

void burrow__hpack_decoder_free(HpackDecoder *d) {
    if (d == NULL)
        return;
    Alloc *a = d->a;
    burrow__hpack_table_free(&d->dyn_tab.table);
    hp_buf_free(a, &d->save);
    for (int i = 0; i < 3; i++)
        hp_buf_free(a, &d->str[i]);
    mem_free(a, d, sizeof *d, _Alignof(HpackDecoder));
}

void burrow__hpack_decoder_set_max_string_length(HpackDecoder *d, Int n) {
    d->max_str_len = n;
}

void burrow__hpack_decoder_set_emit_func(HpackDecoder *d, HpackEmitFunc emit,
                                         void *ctx) {
    d->emit = emit;
    d->emit_ctx = ctx;
}

void burrow__hpack_decoder_set_emit_enabled(HpackDecoder *d, bool v) {
    d->emit_enabled = v;
}

bool burrow__hpack_decoder_emit_enabled(const HpackDecoder *d) {
    return d->emit_enabled;
}

void burrow__hpack_decoder_set_max_dynamic_table_size(HpackDecoder *d, uint32_t v) {
    burrow__hpack_dynamic_table_set_max_size(&d->dyn_tab, v);
}

void burrow__hpack_decoder_set_allowed_max_dynamic_table_size(HpackDecoder *d,
                                                              uint32_t v) {
    d->dyn_tab.allowed_max_size = v;
}

bool burrow__hpack_decoder_at(const HpackDecoder *d, uint64_t i, HpackHeaderField *hf) {
    /* See section 2.3.3. */
    if (i == 0)
        return false;
    if (i <= BURROW__HPACK_STATIC_LEN) {
        *hf = burrow__hpack_static[i - 1];
        return true;
    }
    const HpackTable *dt = &d->dyn_tab.table;
    if (i > (uint64_t)(dt->len + BURROW__HPACK_STATIC_LEN))
        return false;
    /* Newer entries have lower indexes, and the table is oldest first. */
    *hf = *burrow__hpack_table_at(dt, dt->len - ((Int)i - BURROW__HPACK_STATIC_LEN));
    return true;
}

uint64_t burrow__hpack_read_var_int(Byte n, const Byte *p, Int len, Int *used,
                                    Error *err) {
    if (n < 1 || n > 8)
        panic_str(BURROW_S("bad n"));
    *used = 0;
    *err = BURROW_NO_ERROR;
    if (len == 0) {
        *err = burrow__hpack_err_need_more;
        return 0;
    }
    uint64_t i = p[0];
    if (n < 8)
        i &= ((uint64_t)1 << n) - 1;
    if (i < ((uint64_t)1 << n) - 1) {
        *used = 1;
        return i;
    }
    unsigned m = 0;
    for (Int k = 1; k < len; k++) {
        Byte b = p[k];
        i += (uint64_t)(b & 127) << m;
        if ((b & 128) == 0) {
            *used = k + 1;
            return i;
        }
        m += 7;
        if (m >= 63) {
            *err = burrow__hpack_err_varint_overflow;
            return 0;
        }
    }
    *err = burrow__hpack_err_need_more;
    return 0;
}

typedef struct HpUndecoded {
    const Byte *b;
    Int len;
    bool is_huff;
} HpUndecoded;

/* readString: the string at the start of p, left encoded until the whole
 * field is known to be there. */
static Error hp_read_string(const HpackDecoder *d, const Byte *p, Int len,
                            HpUndecoded *u, Int *used) {
    *used = 0;
    if (len == 0)
        return burrow__hpack_err_need_more;
    bool is_huff = (p[0] & 128) != 0;
    Int n = 0;
    Error err = BURROW_NO_ERROR;
    uint64_t str_len = burrow__hpack_read_var_int(7, p, len, &n, &err);
    if (BURROW_FAILED(err))
        return err;
    if (d->max_str_len != 0 && str_len > (uint64_t)d->max_str_len)
        /* Huffman errors in a string past the limit go unseen, and since the
         * string is not indexed the table is still right. */
        return burrow__hpack_err_string_length;
    if ((uint64_t)(len - n) < str_len)
        return burrow__hpack_err_need_more;
    u->is_huff = is_huff;
    u->b = p + n;
    u->len = (Int)str_len;
    *used = n + (Int)str_len;
    return BURROW_NO_ERROR;
}

static Error hp_decode_string(HpackDecoder *d, HpUndecoded u, int slot, Str *s) {
    if (!u.is_huff) {
        *s = str_from_bytes(u.b, u.len);
        return BURROW_NO_ERROR;
    }
    burrow__HpackBuf *b = &d->str[slot];
    b->len = 0;
    b->failed = false;
    Int cap = hp_huffman_max(u.len);
    if (!hp_reserve(d->a, b, cap))
        return burrow_err_out_of_memory;
    Int n = 0;
    Error err =
        burrow__hpack_huffman_decode_into(b->p, b->cap, &n, d->max_str_len, u.b, u.len);
    if (BURROW_FAILED(err))
        return err;
    *s = str_from_bytes(b->p, n);
    return BURROW_NO_ERROR;
}

static Error hp_call_emit(HpackDecoder *d, HpackHeaderField hf) {
    if (d->max_str_len != 0 &&
        (hf.name.len > d->max_str_len || hf.value.len > d->max_str_len))
        return burrow__hpack_err_string_length;
    if (d->emit_enabled && d->emit != NULL)
        d->emit(d->emit_ctx, hf);
    return BURROW_NO_ERROR;
}

static Error hp_parse_field_indexed(HpackDecoder *d) {
    Int n = 0;
    Error err = BURROW_NO_ERROR;
    uint64_t idx = burrow__hpack_read_var_int(7, d->buf, d->buf_len, &n, &err);
    if (BURROW_FAILED(err))
        return err;
    HpackHeaderField hf;
    if (!burrow__hpack_decoder_at(d, idx, &hf))
        return hp_invalid_index(idx);
    d->buf += n;
    d->buf_len -= n;
    return hp_call_emit(d, (HpackHeaderField){hf.name, hf.value, false});
}

enum { HP_INDEXED_TRUE, HP_INDEXED_FALSE, HP_INDEXED_NEVER };

static Error hp_parse_field_literal(HpackDecoder *d, Byte n, int it) {
    const Byte *p = d->buf;
    Int len = d->buf_len;
    Int used = 0;
    Error err = BURROW_NO_ERROR;
    uint64_t name_idx = burrow__hpack_read_var_int(n, p, len, &used, &err);
    if (BURROW_FAILED(err))
        return err;
    p += used;
    len -= used;

    HpackHeaderField hf = {BURROW_STR_EMPTY, BURROW_STR_EMPTY, false};
    bool indexed = it == HP_INDEXED_TRUE;
    bool want_str = d->emit_enabled || indexed;
    bool name_from_table = false;
    HpUndecoded undecoded_name = {0};
    if (name_idx > 0) {
        HpackHeaderField ihf;
        if (!burrow__hpack_decoder_at(d, name_idx, &ihf))
            return hp_invalid_index(name_idx);
        hf.name = ihf.name;
        name_from_table = name_idx > BURROW__HPACK_STATIC_LEN;
    } else {
        err = hp_read_string(d, p, len, &undecoded_name, &used);
        if (BURROW_FAILED(err))
            return err;
        p += used;
        len -= used;
    }
    HpUndecoded undecoded_value = {0};
    err = hp_read_string(d, p, len, &undecoded_value, &used);
    if (BURROW_FAILED(err))
        return err;
    p += used;
    len -= used;
    if (want_str) {
        if (name_idx == 0) {
            err = hp_decode_string(d, undecoded_name, 0, &hf.name);
            if (BURROW_FAILED(err))
                return err;
        }
        err = hp_decode_string(d, undecoded_value, 1, &hf.value);
        if (BURROW_FAILED(err))
            return err;
    }
    d->buf = p;
    d->buf_len = len;
    if (indexed) {
        /* Adding can evict the entry the name came from, so the name moves
         * somewhere that lasts until the field has been emitted. */
        if (name_from_table) {
            burrow__HpackBuf *b = &d->str[2];
            b->len = 0;
            b->failed = false;
            hp_put(d->a, b, hf.name.p, hf.name.len);
            if (b->failed)
                return burrow_err_out_of_memory;
            hf.name = str_from_bytes(b->p, hf.name.len);
        }
        if (!burrow__hpack_dynamic_table_add(&d->dyn_tab, hf))
            return burrow_err_out_of_memory;
    }
    hf.sensitive = it == HP_INDEXED_NEVER;
    return hp_call_emit(d, hf);
}

static Error hp_parse_dynamic_table_size_update(HpackDecoder *d) {
    /* RFC 7541, sec 4.2: the update MUST come at the start of the first header
     * block after the change to the size. */
    if (!d->first_field && d->dyn_tab.size > 0)
        return hp_fixed(&hp_dec_update_late);
    Int n = 0;
    Error err = BURROW_NO_ERROR;
    uint64_t size = burrow__hpack_read_var_int(5, d->buf, d->buf_len, &n, &err);
    if (BURROW_FAILED(err))
        return err;
    if (size > d->dyn_tab.allowed_max_size)
        return hp_fixed(&hp_dec_update_large);
    burrow__hpack_dynamic_table_set_max_size(&d->dyn_tab, (uint32_t)size);
    d->buf += n;
    d->buf_len -= n;
    return BURROW_NO_ERROR;
}

/* need_more when the bytes stop in the middle of the field, which leaves
 * d->buf where it was. Anything else that fails is the end of the block. */
static Error hp_parse_header_field_repr(HpackDecoder *d) {
    Byte b = d->buf[0];
    if ((b & 128) != 0)
        /* 6.1 Indexed Header Field. */
        return hp_parse_field_indexed(d);
    if ((b & 192) == 64)
        /* 6.2.1 Literal Header Field with Incremental Indexing, 01xxxxxx. */
        return hp_parse_field_literal(d, 6, HP_INDEXED_TRUE);
    if ((b & 240) == 0)
        /* 6.2.2 Literal Header Field without Indexing, 0000xxxx. */
        return hp_parse_field_literal(d, 4, HP_INDEXED_FALSE);
    if ((b & 240) == 16)
        /* 6.2.3 Literal Header Field never Indexed, 0001xxxx. */
        return hp_parse_field_literal(d, 4, HP_INDEXED_NEVER);
    if ((b & 224) == 32)
        /* 6.3 Dynamic Table Size Update, 001xxxxx. */
        return hp_parse_dynamic_table_size_update(d);
    return hp_fixed(&hp_dec_invalid);
}

Int burrow__hpack_decoder_write(HpackDecoder *d, Slice p, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (p.len == 0)
        /* Nothing to do, and no reason to go over a kept partial field again
         * looking for the end it still does not have. */
        return 0;
    /* Copy only if there is a partial field to put first. */
    bool from_save = d->save.len > 0;
    if (!from_save) {
        d->buf = (const Byte *)p.p;
        d->buf_len = p.len;
    } else {
        d->save.failed = false;
        hp_put(d->a, &d->save, p.p, p.len);
        if (d->save.failed) {
            BURROW_OUT(err, burrow_err_out_of_memory);
            return 0;
        }
        d->buf = d->save.p;
        d->buf_len = d->save.len;
        d->save.len = 0;
    }

    Error e = BURROW_NO_ERROR;
    while (d->buf_len > 0) {
        e = hp_parse_header_field_repr(d);
        if (hp_is(e, burrow__hpack_err_need_more)) {
            /* A last guard on how much is kept. The string reading above
             * should already have said a string was too long. */
            enum { VAR_INT_OVERHEAD = 8 };
            if (d->max_str_len != 0 &&
                (int64_t)d->buf_len >
                    2 * ((int64_t)d->max_str_len + VAR_INT_OVERHEAD)) {
                BURROW_OUT(err, burrow__hpack_err_string_length);
                return 0;
            }
            /* The rest may already be in save, past its start. */
            if (from_save) {
                memmove(d->save.p, d->buf, (size_t)d->buf_len);
                d->save.len = d->buf_len;
            } else {
                d->save.failed = false;
                hp_put(d->a, &d->save, d->buf, d->buf_len);
                if (d->save.failed) {
                    BURROW_OUT(err, burrow_err_out_of_memory);
                    return 0;
                }
            }
            return p.len;
        }
        d->first_field = false;
        if (BURROW_FAILED(e))
            break;
    }
    BURROW_OUT(err, e);
    return p.len;
}

Error burrow__hpack_decoder_close(HpackDecoder *d) {
    if (d->save.len > 0) {
        d->save.len = 0;
        return hp_fixed(&hp_dec_truncated);
    }
    d->first_field = true;
    return BURROW_NO_ERROR;
}

typedef struct HpCollect {
    Alloc *a;
    HpackHeaderFields out;
    bool failed;
} HpCollect;

static Str hp_clone(Alloc *a, Str s, bool *failed) {
    if (s.len == 0)
        return BURROW_STR_EMPTY;
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)s.len, 1);
    if (p == NULL) {
        *failed = true;
        return BURROW_STR_EMPTY;
    }
    memcpy(p, s.p, (size_t)s.len);
    return str_from_bytes(p, s.len);
}

static void hp_collect(void *ctx, HpackHeaderField f) {
    HpCollect *c = (HpCollect *)ctx;
    if (c->failed)
        return;
    HpackHeaderFields *o = &c->out;
    if (o->len == o->cap) {
        Int cap = o->cap == 0 ? 8 : o->cap * 2;
        HpackHeaderField *p = (HpackHeaderField *)mem_alloc_nozero(
            c->a, (size_t)cap * sizeof(HpackHeaderField), _Alignof(HpackHeaderField));
        if (p == NULL) {
            c->failed = true;
            return;
        }
        if (o->len > 0)
            memcpy(p, o->p, (size_t)o->len * sizeof(HpackHeaderField));
        if (o->p != NULL)
            mem_free(c->a, o->p, (size_t)o->cap * sizeof(HpackHeaderField),
                     _Alignof(HpackHeaderField));
        o->p = p;
        o->cap = cap;
    }
    HpackHeaderField g = {hp_clone(c->a, f.name, &c->failed),
                          hp_clone(c->a, f.value, &c->failed), f.sensitive};
    o->p[o->len++] = g;
}

HpackHeaderFields burrow__hpack_decoder_decode_full(HpackDecoder *d, Alloc *a, Slice p,
                                                    Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    HpCollect c = {a, {0}, false};
    HpackEmitFunc save_func = d->emit;
    void *save_ctx = d->emit_ctx;
    d->emit = hp_collect;
    d->emit_ctx = &c;
    Error e = BURROW_NO_ERROR;
    (void)burrow__hpack_decoder_write(d, p, &e);
    d->emit = save_func;
    d->emit_ctx = save_ctx;
    if (BURROW_OK(e))
        e = burrow__hpack_decoder_close(d);
    if (BURROW_OK(e) && c.failed)
        e = burrow_err_out_of_memory;
    if (BURROW_FAILED(e)) {
        burrow__hpack_header_fields_free(a, &c.out);
        BURROW_OUT(err, e);
        return (HpackHeaderFields){0};
    }
    return c.out;
}

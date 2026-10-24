/* index/suffixarray.
 *
 * Derived from Go's src/index/suffixarray/suffixarray.go.
 * Go source: go1.27.1.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/index/suffixarray.h"

#include <stdint.h>
#include <string.h>

#include "burrow/bytes.h"
#include "burrow/encoding/binary.h"
#include "burrow/mem/heap.h"
#include "burrow/panic.h"
#include "burrow/slices.h"
#include "burrow/sort.h"

#include "suffixarray_sais.h"
#include "suffixarray_sais2.h"

/* Can change for testing */
static Int max_data32 = INT32_MAX;

/* Not in Go: the tests set maxData32 through this, and get the old value
 * back. */
Int burrow__suffixarray_set_max_data32(Int n);
Int burrow__suffixarray_set_max_data32(Int n) {
    Int old = max_data32;
    max_data32 = n;
    return old;
}

/* Not in Go: TestNew32 and TestNew64 call text_32 and text_64 through these. */
bool burrow__suffixarray_text_32(Alloc *a, Slice text, Slice sa);
bool burrow__suffixarray_text_32(Alloc *a, Slice text, Slice sa) {
    SaisBytes t = {(const Byte *)text.p, text.len};
    SaisInt32s s = {(int32_t *)sa.p, sa.len};
    return text_32(a, t, s);
}

bool burrow__suffixarray_text_64(Alloc *a, Slice text, Slice sa);
bool burrow__suffixarray_text_64(Alloc *a, Slice text, Slice sa) {
    SaisBytes t = {(const Byte *)text.p, text.len};
    SaisInt64s s = {(int64_t *)sa.p, sa.len};
    return text_64(a, t, s);
}

/* [][]int, for FindAllIndex. */
static const Type sx_ints_desc = {
    {NULL, 0},
    {NULL, 0},
    KIND_SLICE,
    (uint32_t)sizeof(Slice),
    (uint16_t)_Alignof(Slice),
    0,
    0,
    NULL,
    NULL,
    TYPE_INT,
    NULL,
    0,
    0x73786931U, /* "sxi1" */
    NULL,
};

static Int sx_ints_len(const burrow__SuffixarrayInts *a) {
    return a->int32.len + a->int64.len;
}

static int64_t sx_ints_get(const burrow__SuffixarrayInts *a, Int i) {
    if (a->int32.p != NULL)
        return (int64_t)BURROW_AT(int32_t, a->int32, i);
    return BURROW_AT(int64_t, a->int64, i);
}

static void sx_ints_set(burrow__SuffixarrayInts *a, Int i, int64_t v) {
    if (a->int32.p != NULL)
        BURROW_AT(int32_t, a->int32, i) = (int32_t)v;
    else
        BURROW_AT(int64_t, a->int64, i) = v;
}

static burrow__SuffixarrayInts sx_ints_slice(const burrow__SuffixarrayInts *a, Int i,
                                             Int j) {
    burrow__SuffixarrayInts r;
    if (a->int32.p != NULL) {
        r.int32 = slice_sub(a->int32, i, j);
        r.int64 = slice_nil(TYPE_INT64);
    } else {
        r.int32 = slice_nil(TYPE_INT32);
        r.int64 = slice_sub(a->int64, i, j);
    }
    return r;
}

/* Gives back what slice_make made, which is nothing when it was empty. */
static void sx_free_slice(Alloc *a, Slice s, size_t size, size_t align) {
    if (a != NULL && s.p != NULL && s.cap > 0)
        mem_free(a, s.p, (size_t)s.cap * size, align);
}

static void sx_free_buffers(SuffixarrayIndex *x) {
    sx_free_slice(x->alloc, x->sa.int32, sizeof(int32_t), _Alignof(int32_t));
    sx_free_slice(x->alloc, x->sa.int64, sizeof(int64_t), _Alignof(int64_t));
    if (x->owns_data)
        sx_free_slice(x->alloc, x->data, 1, 1);
}

SuffixarrayIndex *suffixarray_new(Alloc *a, Slice data) {
    SuffixarrayIndex *ix = BURROW_NEW(a, SuffixarrayIndex);
    if (ix == NULL)
        return NULL;
    ix->data = data;
    ix->sa.int32 = slice_nil(TYPE_INT32);
    ix->sa.int64 = slice_nil(TYPE_INT64);
    ix->alloc = a;
    ix->owner = a;
    SaisBytes text = {(const Byte *)data.p, data.len};
    bool ok = false;
    if (data.len <= max_data32) {
        ix->sa.int32 = slice_make(a, TYPE_INT32, data.len, data.len);
        SaisInt32s sa = {(int32_t *)ix->sa.int32.p, data.len};
        ok = sa.p != NULL && text_32(a, text, sa);
    } else {
        ix->sa.int64 = slice_make(a, TYPE_INT64, data.len, data.len);
        SaisInt64s sa = {(int64_t *)ix->sa.int64.p, data.len};
        ok = sa.p != NULL && text_64(a, text, sa);
    }
    if (!ok) {
        suffixarray_index_free(ix);
        return NULL;
    }
    return ix;
}

void suffixarray_index_free(SuffixarrayIndex *x) {
    if (x == NULL)
        return;
    sx_free_buffers(x);
    Alloc *owner = x->owner;
    memset(x, 0, sizeof *x);
    if (owner != NULL)
        mem_free(owner, x, sizeof *x, _Alignof(SuffixarrayIndex));
}

/* writeInt writes an int x to w using buf to buffer the write. */
static Error sx_write_int(IoWriter w, Slice buf, Int x) {
    binary_put_varint(buf, (int64_t)x);
    Error err = BURROW_NO_ERROR;
    w.vt->write(w.data, slice_sub(buf, 0, BINARY_MAX_VARINT_LEN64), &err);
    return err;
}

/* readInt reads an int x from r using buf to buffer the read and returns x. */
static int64_t sx_read_int(IoReader r, Slice buf, Error *err) {
    Error e = BURROW_NO_ERROR;
    /* ok to continue with error */
    io_read_full(r, slice_sub(buf, 0, BINARY_MAX_VARINT_LEN64), &e);
    Int n = 0;
    int64_t x = binary_varint(buf, &n);
    BURROW_OUT(err, e);
    return x;
}

/* writeSlice writes data[:n] to w and returns n.
 * It uses buf to buffer the write. */
static Int sx_write_slice(IoWriter w, Slice buf, burrow__SuffixarrayInts data,
                          Error *err) {
    /* encode as many elements as fit into buf */
    Int n = 0;
    Int p = BINARY_MAX_VARINT_LEN64;
    Int m = sx_ints_len(&data);
    for (; n < m && p + BINARY_MAX_VARINT_LEN64 <= buf.len; n++)
        p += binary_put_uvarint(slice_sub(buf, p, buf.len),
                                (uint64_t)sx_ints_get(&data, n));

    /* update buffer size */
    binary_put_varint(buf, (int64_t)p);

    /* write buffer */
    Error e = BURROW_NO_ERROR;
    w.vt->write(w.data, slice_sub(buf, 0, p), &e);
    BURROW_OUT(err, e);
    return n;
}

static const Str sx_err_corrupted__text = {(const Byte *)"suffixarray: data corrupted",
                                           27};
static const Error sx_err_corrupted = {&burrow_sentinel_error_vt,
                                       &sx_err_corrupted__text};

/* Not in Go: TestIssue53352 and TestIndexCorruption compare with errCorrupted
 * through this. */
Error burrow__suffixarray_err_corrupted(void);
Error burrow__suffixarray_err_corrupted(void) {
    return sx_err_corrupted;
}

/* readSlice reads data[:n] from r and returns n; maxIndex is the length of the
 * suffix array. It uses buf to buffer the read. */
static Int sx_read_slice(IoReader r, Slice buf, burrow__SuffixarrayInts data,
                         uint64_t max_index, Error *err) {
    Int n = 0;

    /* read buffer size */
    Error e = BURROW_NO_ERROR;
    int64_t size64 = sx_read_int(r, buf, &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return n;
    }
    if ((int64_t)(Int)size64 != size64 || (Int)size64 < 0) {
        /* We never write chunks this big anyway. */
        BURROW_OUT(err, sx_err_corrupted);
        return 0;
    }
    Int size = (Int)size64;

    /* read buffer w/o the size */
    io_read_full(r, slice_sub(buf, BINARY_MAX_VARINT_LEN64, size), &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return n;
    }

    /* decode as many elements as present in buf */
    Int len = sx_ints_len(&data);
    for (Int p = BINARY_MAX_VARINT_LEN64; p < size; n++) {
        Int w = 0;
        uint64_t x = binary_uvarint(slice_sub(buf, p, buf.len), &w);
        /* - prevent index-out-of-bounds panic if there are more indices than
         *   expected (was go.dev/issue/53352)
         * - prevent index-out-of-bounds panic in a future Lookup
         *   by ensuring all indices x satisfy x < maxIndex */
        if (n >= len || x >= max_index) {
            BURROW_OUT(err, sx_err_corrupted);
            return n;
        }
        sx_ints_set(&data, n, (int64_t)x);
        p += w;
    }

    BURROW_OUT(err, BURROW_NO_ERROR);
    return n;
}

enum { SX_BUF_SIZE = 16 << 10 }; /* reasonable for BenchmarkSaveRestore */

static Error sx_read(SuffixarrayIndex *x, Alloc *a, IoReader r, Slice buf) {
    /* read length */
    Error err = BURROW_NO_ERROR;
    int64_t n64 = sx_read_int(r, buf, &err);
    if (BURROW_FAILED(err))
        return err;
    if ((int64_t)(Int)n64 != n64 || (Int)n64 < 0)
        return sx_err_corrupted;
    Int n = (Int)n64;

    /* allocate space */
    if ((Int)((uint64_t)n * 2u) < x->data.cap || x->data.cap < n ||
        (x->sa.int32.p != NULL && n > max_data32) ||
        (x->sa.int64.p != NULL && n <= max_data32)) {
        /* new data is significantly smaller or larger than
         * existing buffers - allocate new ones */
        Slice data = slice_make(a, TYPE_BYTE, n, n);
        burrow__SuffixarrayInts sa = {slice_nil(TYPE_INT32), slice_nil(TYPE_INT64)};
        if (n <= max_data32)
            sa.int32 = slice_make(a, TYPE_INT32, n, n);
        else
            sa.int64 = slice_make(a, TYPE_INT64, n, n);
        if (data.p == NULL || (sa.int32.p == NULL && sa.int64.p == NULL)) {
            sx_free_slice(a, data, 1, 1);
            sx_free_slice(a, sa.int32, sizeof(int32_t), _Alignof(int32_t));
            sx_free_slice(a, sa.int64, sizeof(int64_t), _Alignof(int64_t));
            return burrow_err_out_of_memory;
        }
        sx_free_buffers(x);
        x->data = data;
        x->sa = sa;
        x->alloc = a;
        x->owns_data = true;
    } else {
        /* re-use existing buffers */
        x->data = slice_sub(x->data, 0, n);
        x->sa = sx_ints_slice(&x->sa, 0, n);
    }

    /* read data */
    io_read_full(r, x->data, &err);
    if (BURROW_FAILED(err))
        return err;

    /* read index */
    burrow__SuffixarrayInts sa = x->sa;
    while (sx_ints_len(&sa) > 0) {
        Int k = sx_read_slice(r, buf, sa, (uint64_t)n, &err);
        if (BURROW_FAILED(err))
            return err;
        sa = sx_ints_slice(&sa, k, sx_ints_len(&sa));
    }
    return BURROW_NO_ERROR;
}

Error suffixarray_index_read(SuffixarrayIndex *x, Alloc *a, IoReader r) {
    /* buffer for all reads */
    Alloc *h = heap_allocator();
    Slice buf = slice_make(h, TYPE_BYTE, SX_BUF_SIZE, SX_BUF_SIZE);
    if (buf.p == NULL)
        return burrow_err_out_of_memory;
    Error err = sx_read(x, a, r, buf);
    sx_free_slice(h, buf, 1, 1);
    return err;
}

static Error sx_write(SuffixarrayIndex *x, IoWriter w, Slice buf) {
    /* write length */
    Error err = sx_write_int(w, buf, x->data.len);
    if (BURROW_FAILED(err))
        return err;

    /* write data */
    w.vt->write(w.data, x->data, &err);
    if (BURROW_FAILED(err))
        return err;

    /* write index */
    burrow__SuffixarrayInts sa = x->sa;
    while (sx_ints_len(&sa) > 0) {
        Int n = sx_write_slice(w, buf, sa, &err);
        if (BURROW_FAILED(err))
            return err;
        sa = sx_ints_slice(&sa, n, sx_ints_len(&sa));
    }
    return BURROW_NO_ERROR;
}

Error suffixarray_index_write(SuffixarrayIndex *x, IoWriter w) {
    /* buffer for all writes */
    Alloc *h = heap_allocator();
    Slice buf = slice_make(h, TYPE_BYTE, SX_BUF_SIZE, SX_BUF_SIZE);
    if (buf.p == NULL)
        return burrow_err_out_of_memory;
    Error err = sx_write(x, w, buf);
    sx_free_slice(h, buf, 1, 1);
    return err;
}

Slice suffixarray_index_bytes(SuffixarrayIndex *x) {
    return x->data;
}

static Slice sx_at(const SuffixarrayIndex *x, Int i) {
    return slice_sub(x->data, (Int)sx_ints_get(&x->sa, i), x->data.len);
}

typedef struct SxSearch {
    const SuffixarrayIndex *x;
    Slice s;
    Int i;
} SxSearch;

static bool sx_search_first(void *env, Int i) {
    const SxSearch *e = (const SxSearch *)env;
    return bytes_compare(sx_at(e->x, i), e->s) >= 0;
}

static bool sx_search_end(void *env, Int j) {
    const SxSearch *e = (const SxSearch *)env;
    return !bytes_has_prefix(sx_at(e->x, j + e->i), e->s);
}

/* lookupAll returns a slice into the matching region of the index.
 * The runtime is O(log(N)*len(s)). */
static burrow__SuffixarrayInts sx_lookup_all(const SuffixarrayIndex *x, Slice s) {
    /* find matching suffix index range [i:j]
     * find the first index where s would be the prefix */
    SxSearch e = {x, s, 0};
    Int n = sx_ints_len(&x->sa);
    Int i = sort_search(n, BURROW_FN(SortSearchFunc, sx_search_first, &e));
    /* starting at i, find the first index at which s is not a prefix */
    e.i = i;
    Int j = i + sort_search(n - i, BURROW_FN(SortSearchFunc, sx_search_end, &e));
    return sx_ints_slice(&x->sa, i, j);
}

Slice suffixarray_index_lookup(SuffixarrayIndex *x, Alloc *a, Slice s, Int n) {
    Slice result = slice_nil(TYPE_INT);
    if (s.len > 0 && n != 0) {
        burrow__SuffixarrayInts matches = sx_lookup_all(x, s);
        Int count = sx_ints_len(&matches);
        if (n < 0 || count < n)
            n = count;
        /* 0 <= n <= count */
        if (n > 0) {
            result = slice_make(a, TYPE_INT, n, n);
            Int *rp = (Int *)result.p;
            if (rp == NULL)
                return result;
            if (matches.int32.p != NULL) {
                const int32_t *m = (const int32_t *)matches.int32.p;
                for (Int i = 0; i < result.len; i++)
                    rp[i] = (Int)m[i];
            } else {
                const int64_t *m = (const int64_t *)matches.int64.p;
                for (Int i = 0; i < result.len; i++)
                    rp[i] = (Int)m[i];
            }
        }
    }
    return result;
}

/* n1 += 2 * (n - len(result)), which Go lets overflow. */
static Int sx_next_n1(Int n1, Int n, Int len) {
    return (Int)((uint64_t)n1 + 2u * ((uint64_t)n - (uint64_t)len));
}

static Slice sx_find_all_complete(SuffixarrayIndex *x, Alloc *a, Slice lit, Int n) {
    Alloc *h = heap_allocator();
    Slice result = slice_nil(&sx_ints_desc);

    /* Lookup returns indices that may belong to overlapping matches.
     * After eliminating them, we may end up with fewer than n matches.
     * If we don't have enough at the end, redo the search with an
     * increased value n1, but only if Lookup returned all the requested
     * indices in the first place (if it returned fewer than that then
     * there cannot be more). */
    for (Int n1 = n;; n1 = sx_next_n1(n1, n, result.len) /* overflow ok */) {
        Slice indices = suffixarray_index_lookup(x, h, lit, n1);
        if (indices.len == 0)
            return result;
        slices_sort(indices);
        Slice pairs = slice_make(a, TYPE_INT, 2 * indices.len, 2 * indices.len);
        result = slice_make(a, &sx_ints_desc, indices.len, indices.len);
        if (pairs.p == NULL || result.p == NULL) {
            sx_free_slice(h, indices, sizeof(Int), _Alignof(Int));
            return slice_nil(&sx_ints_desc);
        }
        Int *pp = (Int *)pairs.p;
        Slice *rp = (Slice *)result.p;
        Int count = 0;
        Int prev = 0;
        for (Int k = 0; k < indices.len; k++) {
            Int i = ((const Int *)indices.p)[k];
            if (count == n)
                break;
            /* ignore indices leading to overlapping matches */
            if (prev <= i) {
                Int j = 2 * count;
                pp[j + 0] = i;
                pp[j + 1] = i + lit.len;
                rp[count] = slice_sub(pairs, j, j + 2);
                count++;
                prev = i + lit.len;
            }
        }
        result = slice_sub(result, 0, count);
        Int got = indices.len;
        sx_free_slice(h, indices, sizeof(Int), _Alignof(Int));
        if (result.len >= n || got != n1) {
            /* found all matches or there's no chance to find more
             * (n and n1 can be negative) */
            break;
        }
    }
    if (result.len == 0)
        result = slice_nil(&sx_ints_desc);
    return result;
}

static Slice sx_find_all_prefix(SuffixarrayIndex *x, Alloc *a, const Regexp *re,
                                Slice lit, Int n) {
    Alloc *h = heap_allocator();
    Slice result = slice_nil(&sx_ints_desc);

    /* regexp has a non-empty literal prefix; Lookup(lit) computes
     * the indices of possible complete matches; use these as starting
     * points for anchored searches
     * (regexp "^" matches beginning of input, not beginning of line) */
    Str src = regexp_string(re);
    Byte *expr = (Byte *)mem_alloc_nozero(h, (size_t)src.len + 1, 1);
    if (expr == NULL)
        return result;
    expr[0] = '^';
    if (src.len > 0)
        memcpy(expr + 1, src.p, (size_t)src.len);
    Str anchored = {expr, src.len + 1};
    Regexp *r = regexp_must_compile(h, anchored); /* compiles because r compiled */
    if (r == NULL) {
        mem_free(h, expr, (size_t)src.len + 1, 1);
        return result;
    }

    /* same comment about Lookup applies here as in the loop above */
    for (Int n1 = n;; n1 = sx_next_n1(n1, n, result.len) /* overflow ok */) {
        Slice indices = suffixarray_index_lookup(x, h, lit, n1);
        if (indices.len == 0) {
            regexp_free(r);
            mem_free(h, expr, (size_t)src.len + 1, 1);
            return result;
        }
        slices_sort(indices);
        result = slice_sub(result, 0, 0);
        Int prev = 0;
        for (Int k = 0; k < indices.len; k++) {
            Int i = ((const Int *)indices.p)[k];
            if (result.len == n)
                break;
            /* anchored search - will not run off */
            Slice m = regexp_find_index(r, a, slice_sub(x->data, i, x->data.len));
            /* ignore indices leading to overlapping matches */
            if (m.p != NULL && prev <= i) {
                Int *mp = (Int *)m.p;
                mp[0] = i; /* correct m */
                mp[1] += i;
                result = slice_append(a, result, &m, 1);
                prev = mp[1];
            }
        }
        Int got = indices.len;
        sx_free_slice(h, indices, sizeof(Int), _Alignof(Int));
        if (result.len >= n || got != n1) {
            /* found all matches or there's no chance to find more
             * (n and n1 can be negative) */
            break;
        }
    }
    regexp_free(r);
    mem_free(h, expr, (size_t)src.len + 1, 1);
    if (result.len == 0)
        result = slice_nil(&sx_ints_desc);
    return result;
}

Slice suffixarray_index_find_all_index(SuffixarrayIndex *x, Alloc *a, const Regexp *r,
                                       Int n) {
    /* a non-empty literal prefix is used to determine possible
     * match start indices with Lookup */
    bool complete = false;
    Str prefix = regexp_literal_prefix(r, &complete);
    Slice lit =
        slice_from((void *)(uintptr_t)prefix.p, prefix.len, prefix.len, TYPE_BYTE);

    /* worst-case scenario: no literal prefix */
    if (prefix.len == 0)
        return regexp_find_all_index(r, a, x->data, n);

    /* if regexp is a literal just use Lookup and convert its
     * result into match pairs */
    if (complete)
        return sx_find_all_complete(x, a, lit, n);

    return sx_find_all_prefix(x, a, r, lit, n);
}

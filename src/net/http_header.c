/* Derived from Go's src/net/http/header.go, and cloneOrMakeHeader from
 * clone.go.
 * Go source: go1.27.1.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "http_internal.h"

#include "http_ascii.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/func.h"
#include "burrow/io.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/net/http.h"
#include "burrow/net/http/httptrace.h"
#include "burrow/net/textproto.h"
#include "burrow/slice.h"
#include "burrow/slices.h"
#include "burrow/strings.h"
#include "burrow/time.h"

#include "../xnet/httpguts.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

Str burrow__http_header_get(HttpHeader h, Str key) {
    if (h == NULL)
        return BURROW_STR_EMPTY;
    const Slice *v = (const Slice *)map_get(h, &key);
    if (v == NULL || v->len == 0)
        return BURROW_STR_EMPTY;
    return ((const Str *)v->p)[0];
}

bool burrow__http_header_has(HttpHeader h, Str key) {
    return h != NULL && map_get(h, &key) != NULL;
}

/* -------------------------------------------------------------------- Clone */

/* Where an empty value that is not nil points in a clone with no values at
 * all, as Go's make([]string, 0) is not nil. Nothing is ever written there. */
static const Str ht_no_values[1] = {{NULL, 0}};

HttpHeader http_header_clone(Alloc *a, HttpHeader h) {
    if (h == NULL)
        return NULL;
    /* Every value goes in one block, as Go shares one backing array. */
    Int nv = 0;
    MapIter it = map_iter(h);
    const void *k;
    void *v;
    while (map_next(&it, &k, &v))
        nv += ((const Slice *)v)->len;
    HttpHeader h2 = map_make(a, map_key_type(h), map_val_type(h), map_len(h));
    if (h2 == NULL)
        return NULL;
    Str *sv = (Str *)(uintptr_t)ht_no_values;
    if (nv > 0) {
        sv = (Str *)mem_alloc_nozero(a, (size_t)nv * sizeof(Str), _Alignof(Str));
        if (sv == NULL) {
            map_free(h2);
            return NULL;
        }
    }
    Int off = 0;
    it = map_iter(h);
    while (map_next(&it, &k, &v)) {
        const Slice *vv = (const Slice *)v;
        Slice s = slice_from(NULL, 0, 0, TYPE_STRING);
        if (vv->p != NULL) {
            if (vv->len > 0)
                memcpy(sv + off, vv->p, (size_t)vv->len * sizeof(Str));
            s = slice_from(sv + off, vv->len, vv->len, TYPE_STRING);
            off += vv->len;
        }
        if (!map_set(h2, k, &s)) {
            map_free(h2);
            if (nv > 0)
                mem_free(a, sv, (size_t)nv * sizeof(Str), _Alignof(Str));
            return NULL;
        }
    }
    return h2;
}

HttpHeader burrow__http_clone_or_make_header(Alloc *a, HttpHeader h) {
    if (h == NULL)
        return http_header_make(a);
    return http_header_clone(a, h);
}

/* ---------------------------------------------------------------- ParseTime */

Time http_parse_time(Alloc *a, Str text, Error *err) {
    const Str layouts[3] = {HTTP_TIME_FORMAT, TIME_RFC850, TIME_ANSIC};
    Time t = {0};
    for (int i = 0; i < 3; i++) {
        *err = BURROW_NO_ERROR;
        t = time_parse(a, layouts[i], text, err);
        if (BURROW_OK(*err))
            break;
    }
    return t;
}

/* -------------------------------------------------------------------- Write */

/* At most this many keys are sorted on the stack. Go keeps a pool of sorters
 * so that a write does not allocate, and this does the same for any header
 * that is not huge. */
#define HT_SORT_STACK 64

static int ht_key_cmp(void *env, const void *x, const void *y) {
    (void)env;
    return (int)strings_compare(*(const Str *)x, *(const Str *)y);
}

static bool ht_is_space(Byte b) {
    return b == ' ' || b == '\t' || b == '\n' || b == '\r';
}

/* v with each "\r" and "\n" written as a space, which is what Go's
 * headerNewlineToSpace replacer and TrimString do between them. v has been
 * trimmed already. */
static void ht_write_value(IoWriter w, Str v, Error *err) {
    Int start = 0;
    for (Int i = 0; i < v.len; i++) {
        if (v.p[i] != '\r' && v.p[i] != '\n')
            continue;
        if (i > start) {
            (void)io_write_string(w, str_from_bytes(v.p + start, i - start), err);
            if (BURROW_FAILED(*err))
                return;
        }
        (void)io_write_string(w, BURROW_S(" "), err);
        if (BURROW_FAILED(*err))
            return;
        start = i + 1;
    }
    if (v.len > start)
        (void)io_write_string(w, str_from_bytes(v.p + start, v.len - start), err);
}

static void ht_write_key(IoWriter w, Str key, const Slice *values, Error *err) {
    const Str *vs = (const Str *)values->p;
    for (Int j = 0; j < values->len; j++) {
        /* TrimString after the newlines are spaces is the same as trimming
         * every kind of space first, since they are all trimmed. */
        Str v = vs[j];
        Int lo = 0, hi = v.len;
        while (lo < hi && ht_is_space(v.p[lo]))
            lo++;
        while (hi > lo && ht_is_space(v.p[hi - 1]))
            hi--;
        (void)io_write_string(w, key, err);
        if (BURROW_OK(*err))
            (void)io_write_string(w, BURROW_S(": "), err);
        if (BURROW_OK(*err) && hi > lo)
            ht_write_value(w, str_from_bytes(v.p + lo, hi - lo), err);
        if (BURROW_OK(*err))
            (void)io_write_string(w, BURROW_S("\r\n"), err);
        if (BURROW_FAILED(*err))
            return;
    }
}

/* Tells trace about key, with its values as they went out: trimmed and with
 * each "\r" and "\n" a space. Go writes those back into the header, and this
 * makes a copy instead. */
static void ht_trace_key(const HttptraceClientTrace *trace, Str key,
                         const Slice *values) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    const Str *vs = (const Str *)values->p;
    Slice out = slice_make(a, TYPE_STRING, values->len, values->len);
    Str *os = (Str *)out.p;
    for (Int j = 0; j < values->len && os != NULL; j++) {
        Str v = vs[j];
        Int lo = 0, hi = v.len;
        while (lo < hi && ht_is_space(v.p[lo]))
            lo++;
        while (hi > lo && ht_is_space(v.p[hi - 1]))
            hi--;
        Byte *b = hi > lo ? (Byte *)mem_alloc_nozero(a, (size_t)(hi - lo), 1) : NULL;
        if (b == NULL) {
            os[j] = BURROW_STR_EMPTY;
            continue;
        }
        for (Int i = lo; i < hi; i++)
            b[i - lo] = v.p[i] == '\r' || v.p[i] == '\n' ? (Byte)' ' : v.p[i];
        os[j] = str_from_bytes(b, hi - lo);
    }
    if (os != NULL || values->len == 0)
        httptrace_client_trace_wrote_header_field(trace, key, out);
    arena_free(&ar);
}

static bool ht_excluded(Str key, Map *exclude, const Str *list, Int nlist) {
    if (exclude != NULL) {
        const bool *ex = (const bool *)map_get(exclude, &key);
        if (ex != NULL && *ex)
            return true;
    }
    for (Int i = 0; i < nlist; i++) {
        if (str_eq(key, list[i]))
            return true;
    }
    return false;
}

static Error ht_write_subset(HttpHeader h, IoWriter w, Map *exclude, const Str *list,
                             Int nlist, const HttptraceClientTrace *trace) {
    Error err = BURROW_NO_ERROR;
    if (!BURROW__HTTPTRACE_HAS(trace, wrote_header_field))
        trace = NULL;
    Int n = h == NULL ? 0 : map_len(h);
    if (n == 0)
        return err;
    Str stack[HT_SORT_STACK];
    Str *keys = stack;
    Alloc *a = NULL;
    if (n > HT_SORT_STACK) {
        a = burrow__map_allocator(h);
        keys = (Str *)mem_alloc_nozero(a, (size_t)n * sizeof(Str), _Alignof(Str));
        if (keys == NULL)
            return burrow_err_out_of_memory;
    }
    Int nk = 0;
    MapIter it = map_iter(h);
    const void *k;
    void *v;
    while (map_next(&it, &k, &v)) {
        Str key = *(const Str *)k;
        if (ht_excluded(key, exclude, list, nlist))
            continue;
        keys[nk++] = key;
    }
    slices_sort_func(slice_from(keys, nk, nk, TYPE_STRING),
                     BURROW_FN(SlicesCmpFunc, ht_key_cmp, NULL));
    for (Int i = 0; i < nk; i++) {
        if (!burrow__httpguts_valid_header_field_name(keys[i]))
            continue;
        const Slice *values = (const Slice *)map_get(h, &keys[i]);
        if (values == NULL)
            continue;
        ht_write_key(w, keys[i], values, &err);
        if (BURROW_FAILED(err))
            break;
        if (trace != NULL)
            ht_trace_key(trace, keys[i], values);
    }
    if (a != NULL)
        mem_free(a, keys, (size_t)n * sizeof(Str), _Alignof(Str));
    return err;
}

Error http_header_write_subset(HttpHeader h, IoWriter w, Map *exclude) {
    return ht_write_subset(h, w, exclude, NULL, 0, NULL);
}

Error burrow__http_header_write_except(HttpHeader h, IoWriter w, const Str *exclude,
                                       Int nexclude,
                                       const HttptraceClientTrace *trace) {
    return ht_write_subset(h, w, NULL, exclude, nexclude, trace);
}

Error http_header_write(HttpHeader h, IoWriter w) {
    return ht_write_subset(h, w, NULL, NULL, 0, NULL);
}

/* ------------------------------------------------------ foreachHeaderElement */

void burrow__http_foreach_header_element(Str v, void (*fn)(void *env, Str f),
                                         void *env) {
    v = textproto_trim_string(v);
    while (v.len > 0) {
        const Byte *comma = (const Byte *)memchr(v.p, ',', (size_t)v.len);
        Int n = comma != NULL ? (Int)(comma - v.p) : v.len;
        Str f = textproto_trim_string(str_from_bytes(v.p, n));
        if (f.len > 0)
            fn(env, f);
        if (comma == NULL)
            break;
        v = str_from_bytes(comma + 1, v.len - n - 1);
    }
}

/* ----------------------------------------------------------------- hasToken */

static bool ht_is_token_boundary(Byte b) {
    return b == ' ' || b == ',' || b == '\t';
}

bool burrow__http_has_token(Str v, Str token) {
    if (token.len > v.len || token.len == 0)
        return false;
    if (str_eq(v, token))
        return true;
    for (Int sp = 0; sp <= v.len - token.len; sp++) {
        /* Check that the first byte is right and the bytes either side are
         * boundaries before the full comparison, since that is the cheap part
         * and it fails most of the time. */
        Byte b = v.p[sp];
        if (b != token.p[0] && (b | 0x20) != token.p[0])
            continue;
        if (sp > 0 && !ht_is_token_boundary(v.p[sp - 1]))
            continue;
        Int end = sp + token.len;
        if (end != v.len && !ht_is_token_boundary(v.p[end]))
            continue;
        if (burrow__http_ascii_equal_fold(str_from_bytes(v.p + sp, token.len), token))
            return true;
    }
    return false;
}

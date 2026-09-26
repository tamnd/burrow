/* Derived from Go's src/internal/testhash/hash.go.
 * Go source: go1.27.1.
 *
 * The checks every hash in the standard library has to pass, shared by the
 * hash tests the way Go shares them through internal/testhash.
 * testhash_without_clone is TestHashWithoutClone, and testhash_with_clone is
 * TestHash, which adds the Clone check for a hash that is a hash.Cloner.
 *
 * The last two helpers are not from Go. Go's golden tables spell a long input
 * as strings.Repeat(rep, count) + tail, and testhash_repeat builds one of those
 * so the tables here can say it the same way. testhash_show cuts an input down
 * for an error message the way Go's tests do.
 *
 * Go seeds its random input from the clock and logs the seed, and so does this,
 * with a splitmix64 generator standing in for math/rand until that is ported.
 *
 * Copyright 2024 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_TESTS_TESTHASH_H
#define BURROW_TESTS_TESTHASH_H

#include "burrow/burrow.h"
#include "burrow/clock.h"
#include "burrow/hash.h"
#include "burrow/mem/arena.h"

#include <string.h>

typedef Hash (*TesthashMake)(Alloc *a);

typedef struct TesthashEnv {
    TesthashMake mh;
} TesthashEnv;

typedef struct TesthashRand {
    uint64_t s;
} TesthashRand;

static inline TesthashRand testhash_new_rand(TestingT *t) {
    TesthashRand r = {(uint64_t)burrow__nanotime()};
    testing_t_logf_v(t, "Deterministic RNG seed: 0x%x", (int64_t)r.s);
    return r;
}

static inline void testhash_read(TesthashRand *r, Slice p) {
    Byte *b = (Byte *)p.p;
    for (Int i = 0; i < p.len; i++) {
        uint64_t z = (r->s += 0x9e3779b97f4a7c15ULL);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        b[i] = (Byte)(z ^ (z >> 31));
    }
}

static inline bool testhash_equal(Slice a, Slice b) {
    return a.len == b.len && (a.len == 0 || memcmp(a.p, b.p, (size_t)a.len) == 0);
}

static inline Slice testhash_bytes(Alloc *a, Int n) {
    return slice_make(a, TYPE_BYTE, n, n);
}

static inline Slice testhash_clone(Alloc *a, Slice p) {
    if (slice_is_nil(p))
        return p;
    Slice c = testhash_bytes(a, p.len);
    slice_copy(c, p);
    return c;
}

static inline void testhash_write(TestingT *t, Alloc *a, Hash h, Slice p) {
    testing_t_helper(t);
    Slice before = testhash_clone(a, p);
    Error err = BURROW_NO_ERROR;
    Int n = hash_write(h, p, &err);
    if (BURROW_FAILED(err) || n != p.len)
        testing_t_errorf_v(t, "Write returned error; got (%v, %v), want (nil, %v)", err,
                           n, p.len);
    if (!testhash_equal(p, before))
        testing_t_errorf_v(t, "Write modified input slice; got %x, want %x", p, before);
}

static inline Slice testhash_sum(TestingT *t, Alloc *a, Hash h, Slice buff) {
    testing_t_helper(t);
    Slice test_buff = testhash_bytes(a, buff.len);
    slice_copy(test_buff, buff);
    Slice sum = hash_sum(a, h, buff);
    Slice test_sum = hash_sum(a, h, test_buff);
    if (!testhash_equal(sum, test_sum))
        testing_t_errorf_v(
            t, "successive calls to Sum yield different results; got %x, want %x", sum,
            test_sum);
    return sum;
}

static inline void testhash_sum_append(void *env, TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Hash h = ((TesthashEnv *)env)->mh(a);
    TesthashRand rng = testhash_new_rand(t);
    Slice empty_buff = testhash_bytes(a, 0);
    Slice short_buff = testhash_bytes(a, 1);
    ((Byte *)short_buff.p)[0] = 'a';
    Slice long_buff = testhash_bytes(a, hash_block_size(h) + 1);
    testhash_read(&rng, long_buff);
    Slice prefixes[] = {slice_nil(TYPE_BYTE), empty_buff, short_buff, long_buff};
    for (size_t i = 0; i < sizeof prefixes / sizeof prefixes[0]; i++) {
        Slice prefix = prefixes[i];
        hash_reset(h);
        Slice sum = testhash_sum(t, a, h, prefix);
        if (!testhash_equal(slice_sub(sum, 0, prefix.len), prefix))
            testing_t_errorf_v(
                t, "Sum alters passed buffer instead of appending; got %x, want %x",
                slice_sub(sum, 0, prefix.len), prefix);
        Slice expected_sum = testhash_sum(t, a, h, slice_nil(TYPE_BYTE));
        if (!testhash_equal(slice_sub(sum, prefix.len, sum.len), expected_sum))
            testing_t_errorf_v(
                t, "Sum behavior affected by data in the input buffer; got %x, want %x",
                slice_sub(sum, prefix.len, sum.len), expected_sum);
        if (sum.len - prefix.len != hash_size(h))
            testing_t_errorf_v(t,
                               "Sum appends number of bytes != Size; got %v , want %v",
                               sum.len - prefix.len, hash_size(h));
    }
    arena_free(&ar);
}

static inline void testhash_write_without_error(void *env, TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Hash h = ((TesthashEnv *)env)->mh(a);
    TesthashRand rng = testhash_new_rand(t);
    Slice short_slice = testhash_bytes(a, 1);
    ((Byte *)short_slice.p)[0] = 'a';
    Slice long_slice = testhash_bytes(a, hash_block_size(h) + 1);
    testhash_read(&rng, long_slice);
    Slice slices[] = {testhash_bytes(a, 0), short_slice, long_slice};
    for (size_t i = 0; i < sizeof slices / sizeof slices[0]; i++)
        testhash_write(t, a, h, slices[i]);
    arena_free(&ar);
}

static inline void testhash_reset_state(void *env, TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Hash h = ((TesthashEnv *)env)->mh(a);
    TesthashRand rng = testhash_new_rand(t);
    Slice empty_sum = testhash_sum(t, a, h, slice_nil(TYPE_BYTE));
    Slice write_ex = testhash_bytes(a, hash_block_size(h));
    testhash_read(&rng, write_ex);
    testhash_write(t, a, h, write_ex);
    hash_reset(h);
    Slice reset_sum = testhash_sum(t, a, h, slice_nil(TYPE_BYTE));
    if (!testhash_equal(empty_sum, reset_sum))
        testing_t_errorf_v(
            t, "Reset hash yields different Sum than new hash; got %x, want %x",
            empty_sum, reset_sum);
    arena_free(&ar);
}

static inline void testhash_out_of_bounds_read(void *env, TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Hash h = ((TesthashEnv *)env)->mh(a);
    Int block_size = hash_block_size(h);
    TesthashRand rng = testhash_new_rand(t);
    Slice msg = testhash_bytes(a, block_size);
    testhash_read(&rng, msg);
    testhash_write(t, a, h, msg);
    Slice expected_digest = testhash_sum(t, a, h, slice_nil(TYPE_BYTE));
    hash_reset(h);
    Slice buff = testhash_bytes(a, block_size * 3);
    Int end_of_prefix = block_size, start_of_suffix = block_size * 2;
    slice_copy(slice_sub(buff, end_of_prefix, start_of_suffix), msg);
    testhash_read(&rng, slice_sub(buff, 0, end_of_prefix));
    testhash_read(&rng, slice_sub(buff, start_of_suffix, buff.len));
    testhash_write(t, a, h, slice_sub(buff, end_of_prefix, start_of_suffix));
    Slice test_digest = testhash_sum(t, a, h, slice_nil(TYPE_BYTE));
    if (!testhash_equal(test_digest, expected_digest))
        testing_t_errorf_v(
            t, "Write affected by data outside of input slice bounds; got %x, want %x",
            test_digest, expected_digest);
    arena_free(&ar);
}

static inline void testhash_stateful_write(void *env, TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Hash h = ((TesthashEnv *)env)->mh(a);
    TesthashRand rng = testhash_new_rand(t);
    Slice prefix = testhash_bytes(a, hash_block_size(h));
    Slice suffix = testhash_bytes(a, hash_block_size(h));
    testhash_read(&rng, prefix);
    testhash_read(&rng, suffix);
    testhash_write(t, a, h, prefix);
    testhash_write(t, a, h, suffix);
    Slice serial_sum = testhash_sum(t, a, h, slice_nil(TYPE_BYTE));
    hash_reset(h);
    testhash_write(t, a, h, slice_append_slice(a, testhash_clone(a, prefix), suffix));
    Slice composite_sum = testhash_sum(t, a, h, slice_nil(TYPE_BYTE));
    if (!testhash_equal(composite_sum, serial_sum))
        testing_t_errorf_v(
            t,
            "two successive Write calls resulted in a different Sum than a single "
            "one; got %x, want %x",
            composite_sum, serial_sum);
    arena_free(&ar);
}

static inline Slice testhash_repeat(Alloc *a, const char *rep, Int rep_len, Int count,
                                    const char *tail) {
    Int tail_len = (Int)strlen(tail);
    Int n = rep_len * count + tail_len;
    Slice p = testhash_bytes(a, n);
    Byte *b = (Byte *)p.p;
    for (Int i = 0; i < count; i++)
        memcpy(b + i * rep_len, rep, (size_t)rep_len);
    memcpy(b + rep_len * count, tail, (size_t)tail_len);
    return p;
}

static inline Str testhash_show(Alloc *a, Slice p) {
    Str s = {(const Byte *)p.p, p.len};
    if (s.len <= 220)
        return s;
    Str head = {s.p, 100};
    Str tail = {s.p + s.len - 100, 100};
    return fmt_sprintf_v(a, "%s...%s", head, tail);
}

static inline void testhash_without_clone(TestingT *t, TesthashMake mh) {
    TesthashEnv env = {mh};
    testing_t_run(t, BURROW_S("SumAppend"),
                  BURROW_FN(TestingTFunc, testhash_sum_append, &env));
    testing_t_run(t, BURROW_S("WriteWithoutError"),
                  BURROW_FN(TestingTFunc, testhash_write_without_error, &env));
    testing_t_run(t, BURROW_S("ResetState"),
                  BURROW_FN(TestingTFunc, testhash_reset_state, &env));
    testing_t_run(t, BURROW_S("OutOfBoundsRead"),
                  BURROW_FN(TestingTFunc, testhash_out_of_bounds_read, &env));
    testing_t_run(t, BURROW_S("StatefulWrite"),
                  BURROW_FN(TestingTFunc, testhash_stateful_write, &env));
}

typedef HashCloner (*TesthashMakeCloner)(Alloc *a);

typedef struct TesthashClonerEnv {
    TesthashMakeCloner mh;
} TesthashClonerEnv;

static inline bool testhash_do_clone(TestingT *t, Alloc *a, HashCloner h,
                                     HashCloner *out) {
    Error err = BURROW_NO_ERROR;
    *out = hash_cloner_clone(a, h, &err);
    if (BURROW_FAILED(err) || out->vt == NULL) {
        testing_t_fatalf_v(t, "Clone failed: %v", err);
        return false;
    }
    return true;
}

/* Whether the results after cloning are consistent. */
static inline void testhash_clone_consistent(void *env, TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    HashCloner h = ((TesthashClonerEnv *)env)->mh(a);
    HashCloner h2, h3;
    Slice none = slice_nil(TYPE_BYTE);
    Byte pre[3] = {'t', 'm', 'p'};
    Byte suf[4] = {'t', 'm', 'p', '2'};
    Byte both[7] = {'t', 'm', 'p', 't', 'm', 'p', '2'};
    Slice prefix = slice_from(pre, 3, 3, TYPE_BYTE);
    Slice suffix = slice_from(suf, 4, 4, TYPE_BYTE);

    if (h.vt == NULL || !testhash_do_clone(t, a, h, &h3))
        goto done;
    Hash hh = {&h.vt->hash, h.data};
    Hash hh3 = {&h3.vt->hash, h3.data};
    testhash_write(t, a, hh, prefix);
    if (!testhash_do_clone(t, a, h, &h2))
        goto done;
    Hash hh2 = {&h2.vt->hash, h2.data};
    Slice prefix_sum = hash_sum(a, hh, none);
    if (!testhash_equal(prefix_sum, hash_sum(a, hh2, none)))
        testing_t_fatalf_v(t, "Clone results are inconsistent");
    testhash_write(t, a, hh, suffix);
    testhash_write(t, a, hh3, slice_from(both, 7, 7, TYPE_BYTE));
    Slice composite_sum = hash_sum(a, hh3, none);
    if (!testhash_equal(hash_sum(a, hh, none), composite_sum))
        testing_t_fatalf_v(t, "Clone results are inconsistent");
    if (!testhash_equal(hash_sum(a, hh2, none), prefix_sum))
        testing_t_fatalf_v(t, "Clone results are inconsistent");
    testhash_write(t, a, hh2, suffix);
    if (!testhash_equal(hash_sum(a, hh, none), composite_sum))
        testing_t_fatalf_v(t, "Clone results are inconsistent");
    if (!testhash_equal(hash_sum(a, hh2, none), composite_sum))
        testing_t_fatalf_v(t, "Clone results are inconsistent");
done:
    arena_free(&ar);
}

static inline void testhash_with_clone(TestingT *t, TesthashMake mh,
                                       TesthashMakeCloner mc) {
    testhash_without_clone(t, mh);
    TesthashClonerEnv env = {mc};
    testing_t_run(t, BURROW_S("Clone"),
                  BURROW_FN(TestingTFunc, testhash_clone_consistent, &env));
}

#endif /* BURROW_TESTS_TESTHASH_H */

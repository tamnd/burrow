/* Derived from the golden and large input tests Go repeats in
 * src/crypto/md5/md5_test.go, sha1/sha1_test.go, sha256/sha256_test.go and
 * sha512/sha512_test.go.
 * Go source: go1.27.1.
 *
 * Each of those files checks a table of inputs against known digests, first
 * with the one shot function, then by writing the whole input twice in a row
 * with a Reset between, then by writing it in two halves with a Sum in the
 * middle to show Sum does not disturb the state. MD5 adds four more passes
 * that write from a buffer moved along by one byte each time, and this runs
 * those for every hash since unaligned input is where a C port can go wrong.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_TESTS_TESTDIGEST_H
#define BURROW_TESTS_TESTDIGEST_H

#include "testhash.h"

#include "burrow/burrow.h"
#include "burrow/encoding.h"
#include "burrow/encoding/hex.h"
#include "burrow/hash.h"
#include "burrow/mem/arena.h"

#include <string.h>

/* half_state is the marshaled state after the first half of in is written,
 * which Go's TestGoldenMarshal checks. It holds NUL bytes, so it is a Str. */
typedef struct Golden {
    const char *out;
    const char *in;
    Str half_state;
} Golden;

/* A state that has seen more than 4GiB, and the sum it has to finish with. */
typedef struct UnmarshalTest {
    Str state;
    const char *sum;
} UnmarshalTest;

/* The one shot function under test, which writes the digest to out and
 * returns its length. */
typedef Int (*TestdigestSum)(Slice in, Byte *out);

static inline void testdigest_write_str(Hash h, const Byte *p, Int n) {
    Slice s = slice_from((void *)(uintptr_t)p, n, n, TYPE_BYTE);
    hash_write(h, s, NULL);
}

static inline void testdigest_golden(TestingT *t, const char *name,
                                     TestdigestSum one_shot, TesthashMake mh,
                                     const Golden *gold, size_t n) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Hash c = mh(a);
    for (size_t i = 0; i < n; i++) {
        const Golden *g = &gold[i];
        Str in = str_from_cstr(g->in);
        Str want = str_from_cstr(g->out);
        Byte one[64];
        Int one_len = one_shot(slice_from_str(a, in), one);
        Str s = hex_encode_to_string(a, slice_from(one, one_len, one_len, TYPE_BYTE));
        if (!str_eq(s, want)) {
            testing_t_fatalf_v(t, "Sum function: %s(%s) = %s want %s",
                               str_from_cstr(name), in, s, want);
            break;
        }
        Slice buf = testhash_bytes(a, in.len + 4);
        for (int j = 0; j < 3 + 4; j++) {
            if (j < 2) {
                testdigest_write_str(c, in.p, in.len);
            } else if (j == 2) {
                testdigest_write_str(c, in.p, in.len / 2);
                hash_sum(a, c, slice_nil(TYPE_BYTE));
                testdigest_write_str(c, in.p + in.len / 2, in.len - in.len / 2);
            } else {
                /* An unaligned write. */
                buf = slice_sub(buf, 1, buf.len);
                slice_copy(buf, slice_from((void *)(uintptr_t)in.p, in.len, in.len,
                                           TYPE_BYTE));
                hash_write(c, slice_sub(buf, 0, in.len), NULL);
            }
            s = hex_encode_to_string(a, hash_sum(a, c, slice_nil(TYPE_BYTE)));
            if (!str_eq(s, want)) {
                testing_t_fatalf_v(t, "%s[%d](%s) = %s want %s", str_from_cstr(name), j,
                                   in, s, want);
                goto out;
            }
            hash_reset(c);
        }
    }
out:
    arena_free(&ar);
}

/* Go's TestLarge and TestExtraLarge: n bytes of "0123456789" repeated,
 * written in pieces of 10, 100 and so on up to n, from four offsets. */
static inline void testdigest_large(TestingT *t, const char *name, TesthashMake mh,
                                    Int n, const char *ok) {
    enum { offsets = 4 };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice block = testhash_bytes(a, n + offsets);
    Byte *bp = (Byte *)block.p;
    Hash c = mh(a);
    Str want = str_from_cstr(ok);
    for (Int offset = 0; offset < offsets; offset++) {
        for (Int i = 0; i < n; i++)
            bp[offset + i] = (Byte)('0' + i % 10);
        for (Int block_size = 10; block_size <= n; block_size *= 10) {
            Int blocks = n / block_size;
            Slice b = slice_sub(block, offset, offset + block_size);
            hash_reset(c);
            for (Int i = 0; i < blocks; i++)
                hash_write(c, b, NULL);
            Str s = hex_encode_to_string(a, hash_sum(a, c, slice_nil(TYPE_BYTE)));
            if (!str_eq(s, want)) {
                testing_t_fatalf_v(t, "%s offset=%d, blockSize=%d = %s want %s",
                                   str_from_cstr(name), offset, block_size, s, want);
                goto out;
            }
        }
    }
out:
    arena_free(&ar);
}

static inline void testdigest_run_testhash(void *env, TestingT *t) {
    testhash_hash(t, *(TesthashMake *)env);
}

/* Go's TestGoldenMarshal for one table: marshal after half of each input,
 * unmarshal into a new hash, and finish both. name is what the messages call
 * the hash, and plus says whether they quote with %+q, as sha1's do. */
static inline void testdigest_golden_marshal(TestingT *t, const char *name, bool plus,
                                             TesthashMake mh, const Golden *gold,
                                             size_t n) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str hn = str_from_cstr(name);
    for (size_t i = 0; i < n; i++) {
        const Golden *g = &gold[i];
        Str in = str_from_cstr(g->in);
        Hash h = mh(a);
        Hash h2 = mh(a);
        Any ha = BURROW_ANY(TYPE_OF(Hash), &h);
        Any ha2 = BURROW_ANY(TYPE_OF(Hash), &h2);

        testdigest_write_str(h, in.p, in.len / 2);

        Error err = BURROW_NO_ERROR;
        Slice state = encoding_marshal_binary(a, ha, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "could not marshal: %v", err);
            continue;
        }

        Slice state_append =
            encoding_append_binary(a, ha, slice_make(a, TYPE_BYTE, 4, 32), &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "could not marshal: %v", err);
            continue;
        }
        state_append = slice_sub(state_append, 4, state_append.len);

        Str st = {(const Byte *)state.p, state.len};
        Str sa = {(const Byte *)state_append.p, state_append.len};
        if (!str_eq(st, g->half_state)) {
            if (plus)
                testing_t_errorf_v(t, "%s(%q) state = %+q, want %+q", hn, in, st,
                                   g->half_state);
            else
                testing_t_errorf_v(t, "%s(%q) state = %q, want %q", hn, in, st,
                                   g->half_state);
            continue;
        }

        if (!str_eq(sa, g->half_state)) {
            if (plus)
                testing_t_errorf_v(t, "%s(%q) stateAppend = %+q, want %+q", hn, in, sa,
                                   g->half_state);
            else
                testing_t_errorf_v(t, "%s(%q) stateAppend = %q, want %q", hn, in, sa,
                                   g->half_state);
            continue;
        }

        err = encoding_unmarshal_binary(a, ha2, state);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "could not unmarshal: %v", err);
            continue;
        }

        testdigest_write_str(h, in.p + in.len / 2, in.len - in.len / 2);
        testdigest_write_str(h2, in.p + in.len / 2, in.len - in.len / 2);

        Slice actual = hash_sum(a, h, slice_nil(TYPE_BYTE));
        Slice actual2 = hash_sum(a, h2, slice_nil(TYPE_BYTE));
        if (!testhash_equal(actual, actual2))
            testing_t_errorf_v(t, "%s(%q) = 0x%x != marshaled 0x%x", hn, in, actual,
                               actual2);
    }
    arena_free(&ar);
}

/* One subtest of a TestGoldenMarshal that runs a table per variant. */
typedef struct TestdigestMarshalCase {
    const char *name;
    const char *hash_name;
    TesthashMake mh;
    const Golden *gold;
    size_t n;
} TestdigestMarshalCase;

static inline void testdigest_run_golden_marshal(void *env, TestingT *t) {
    const TestdigestMarshalCase *c = (const TestdigestMarshalCase *)env;
    testdigest_golden_marshal(t, c->hash_name, false, c->mh, c->gold, c->n);
}

/* Go's safeSum: the sum, or the panic it raised as an error. */
static inline Slice testdigest_safe_sum(Alloc *a, Hash h, Error *err) {
    Slice sum = slice_nil(TYPE_BYTE);
    *err = BURROW_NO_ERROR;
    BURROW_TRY {
        sum = hash_sum(a, h, slice_nil(TYPE_BYTE));
    }
    BURROW_CATCH(r) {
        *err = fmt_errorf_v("sum panic: %s", panic_text(r));
    }
    BURROW_TRY_END;
    return sum;
}

/* Go's TestLargeHashes. */
static inline void testdigest_large_hashes(TestingT *t, TesthashMake mh,
                                           const UnmarshalTest *tests, size_t n) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < n; i++) {
        const UnmarshalTest *test = &tests[i];
        Hash h = mh(a);
        Slice state = slice_from((void *)(uintptr_t)test->state.p, test->state.len,
                                 test->state.len, TYPE_BYTE);
        Error err = encoding_unmarshal_binary(a, BURROW_ANY(TYPE_OF(Hash), &h), state);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "test %d could not unmarshal: %v", (Int)i, err);
            continue;
        }

        Slice sum = testdigest_safe_sum(a, h, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "test %d could not sum: %v", (Int)i, err);
            continue;
        }

        Str got = hex_encode_to_string(a, sum);
        if (!str_eq(got, str_from_cstr(test->sum)))
            testing_t_errorf_v(t, "test %d sum mismatch: expect %s got %x", (Int)i,
                               str_from_cstr(test->sum), sum);
    }
    arena_free(&ar);
}

/* Go's cryptotest.NoExtraMethods, on a hash it asserts to hash.Cloner the way
 * maybeCloner does: the exported methods of the hash's type that the
 * interface does not have have to be in allowed. */
static inline void testdigest_no_extra_methods(TestingT *t, TesthashMake mh,
                                               const char *const *allowed,
                                               size_t nallowed) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Hash h = mh(arena_allocator(&ar));
    const Type *it = hash_is_cloner(h) ? TYPE_OF(HashCloner) : TYPE_OF(Hash);
    const Type *ct = h.vt == NULL ? NULL : h.vt->writer.self_type;
    if (ct == NULL) {
        testing_t_fatalf_v(t, "argument must be a pointer to a non-nil interface");
        return;
    }
    for (uint16_t i = 0; i < ct->nmethod; i++) {
        Str m = ct->methods[i].name;
        if (m.len == 0 || m.p[0] < 'A' || m.p[0] > 'Z')
            continue;
        if (type_method_by_name(it, m) != NULL)
            continue;
        bool ok = false;
        for (size_t j = 0; j < nallowed && !ok; j++)
            ok = str_eq(m, str_from_cstr(allowed[j]));
        if (!ok)
            testing_t_errorf_v(t, "unexpected method %q", m);
    }
    arena_free(&ar);
}

/* Go's TestSize and TestBlockSize for one constructor. */
static inline void testdigest_sizes(TestingT *t, const char *name, TesthashMake mh,
                                    Int size, Int block_size) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Hash c = mh(arena_allocator(&ar));
    if (hash_size(c) != size)
        testing_t_errorf_v(t, "%s.Size = %d; want %d", str_from_cstr(name),
                           hash_size(c), size);
    if (hash_block_size(c) != block_size)
        testing_t_errorf_v(t, "%s.BlockSize = %d want %d", str_from_cstr(name),
                           hash_block_size(c), block_size);
    arena_free(&ar);
}

/* Go's benchmarkSize, the New half: Reset, Write and Sum into a buffer that
 * already has room, so the loop allocates nothing. */
static inline void testdigest_bench(TestingB *b, TesthashMake mh, Int size) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Hash h = mh(a);
    Slice buf = testhash_bytes(a, size);
    Slice sum = testhash_bytes(a, hash_size(h));
    testing_b_set_bytes(b, size);
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++) {
        hash_reset(h);
        hash_write(h, buf, NULL);
        hash_sum(a, h, slice_sub(sum, 0, 0));
    }
    arena_free(&ar);
}

#define NGOLD(g) (sizeof(g) / sizeof((g)[0]))

#endif /* BURROW_TESTS_TESTDIGEST_H */

/* The fuzzing engine: Go's src/internal/fuzz, less coverage guidance.
 *
 * The shape is Go's. The coordinator, which is the test binary run with
 * -test.fuzz, starts one worker process per -test.parallel, each the same
 * binary run again with -test.fuzzworker. It hands them inputs from the corpus
 * and they mutate each one and run the fuzz target on the results until one
 * fails or the time given runs out. A failing input is then minimized, by the
 * same worker, and written to testdata/fuzz.
 *
 * An input goes to a worker through a file both have mapped, the shared memory
 * of Go's mem.go. The worker writes nothing back that the coordinator could
 * not work out for itself: it reports how many inputs it tried, and the
 * coordinator, which saved the random generator's state in that same memory
 * before the call, replays the mutations that led to a failure. That is also
 * why the mutator and the generator have to be Go's to the bit, and why they
 * are here in full.
 *
 * Where Go uses goroutines, channels and a context, this uses one thread per
 * worker, a lock, and notes. The calls between the two processes go over a
 * pair of pipes as Go's do, in a small binary encoding rather than JSON,
 * because both ends are this file and nothing else ever reads it.
 *
 * Coverage is the part that is not here. Go instruments the package under test
 * and keeps an input that reaches new code, and without the instrumentation
 * this does what Go does when coverage is off: it fuzzes the corpus it has and
 * never adds to it. Go prints the same warning in that case.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "fuzz.h"

#include "corpus.h"

#include "burrow/atomic.h"
#include "burrow/core.h"
#include "burrow/fmt.h"
#include "burrow/lock.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/note.h"
#include "burrow/pal.h"
#include "burrow/panic.h"
#include "burrow/platform.h"
#include "burrow/slice.h"
#include "burrow/strconv.h"
#include "burrow/testing.h"
#include "burrow/thread.h"
#include "burrow/type.h"

#include <float.h>
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------- helpers */

static void *fuzz_must_alloc(size_t size, size_t align) {
    void *p = mem_alloc(heap_allocator(), size, align);
    if (p == NULL)
        panic_str(BURROW_S("testing: out of memory"));
    return p;
}

static void *fuzz_must_realloc(void *old, size_t old_size, size_t size, size_t align) {
    void *p = mem_realloc(heap_allocator(), old, old_size, size, align);
    if (p == NULL)
        panic_str(BURROW_S("testing: out of memory"));
    return p;
}

static void fuzz_free(void *p, size_t size, size_t align) {
    if (p != NULL)
        mem_free(heap_allocator(), p, size, align);
}

/* A heap copy of s, which fuzz_str_free gives back. */
static Str fuzz_str_dup(Str s) {
    if (s.len == 0)
        return (Str){0};
    Byte *p = (Byte *)fuzz_must_alloc((size_t)s.len, 1);
    memcpy(p, s.p, (size_t)s.len);
    return str_from_bytes(p, s.len);
}

static void fuzz_str_free(Str s) {
    if (s.len > 0)
        mem_free(heap_allocator(), (void *)(uintptr_t)s.p, (size_t)s.len, 1);
}

/* A growable byte buffer on the heap. */
typedef struct FuzzBuf {
    Byte *p;
    Int len;
    Int cap;
} FuzzBuf;

static void fuzz_buf_append(FuzzBuf *b, const void *p, Int n) {
    if (n <= 0)
        return;
    if (b->len + n > b->cap) {
        Int ncap = b->cap == 0 ? 64 : b->cap;
        while (ncap < b->len + n)
            ncap *= 2;
        b->p = (Byte *)fuzz_must_realloc(b->p, (size_t)b->cap, (size_t)ncap, 1);
        b->cap = ncap;
    }
    memcpy(b->p + b->len, p, (size_t)n);
    b->len += n;
}

static void fuzz_buf_free(FuzzBuf *b) {
    fuzz_free(b->p, (size_t)b->cap, 1);
    *b = (FuzzBuf){0};
}

/* os.Getenv, off the PAL's copy of the environment, which is UTF-8 on every
 * platform. Names do not care about case on Windows. */
static bool fuzz_getenv(const char *name, Str *out) {
    const char *const *env = pal_environ();
    size_t n = strlen(name);
    for (; env != NULL && *env != NULL; env++) {
        const char *e = *env;
        bool match = true;
        for (size_t i = 0; i < n && match; i++) {
            char a = e[i];
            char b = name[i];
#if defined(BURROW_OS_WINDOWS)
            if (a >= 'a' && a <= 'z')
                a = (char)(a - 'a' + 'A');
            if (b >= 'a' && b <= 'z')
                b = (char)(b - 'a' + 'A');
#endif
            match = a != '\0' && a == b;
        }
        if (match && e[n] == '=') {
            *out = str_from_cstr(e + n + 1);
            return true;
        }
    }
    return false;
}

/* ---------------------------------------------------------------- pcgRand */

/* Go's globalInc, which gives every generator in the process its own
 * stream. */
static uint64_t fuzz_global_inc;

#define FUZZ_PCG_MULTIPLIER 6364136223846793005ULL

/* godebugSeed: GODEBUG=fuzzseed=N fixes the seed, which is how Go's own tests
 * make a run repeat. */
static bool fuzz_godebug_seed(uint64_t *seed) {
    Str g;
    if (!fuzz_getenv("GODEBUG", &g))
        return false;
    static const char key[] = "fuzzseed=";
    Int from = 0;
    for (Int i = 0; i <= g.len; i++) {
        if (i < g.len && g.p[i] != ',')
            continue;
        Str f = str_from_bytes(g.p + from, i - from);
        from = i + 1;
        if (f.len < (Int)(sizeof key - 1) || memcmp(f.p, key, sizeof key - 1) != 0)
            continue;
        Error err = {0};
        Int v = strconv_atoi(
            str_from_bytes(f.p + sizeof key - 1, f.len - (Int)(sizeof key - 1)), &err);
        if (err.vt != NULL)
            panic_str(BURROW_S("malformed fuzzseed"));
        *seed = (uint64_t)(int64_t)v;
        return true;
    }
    return false;
}

static void fuzz_rand_step(burrow__FuzzRand *r) {
    r->state *= FUZZ_PCG_MULTIPLIER;
    r->state += r->inc;
}

void burrow__fuzz_rand_init(burrow__FuzzRand *r) {
    uint64_t now = (uint64_t)pal_clock_realtime();
    uint64_t seed;
    if (fuzz_godebug_seed(&seed))
        now = seed;
    uint64_t inc = burrow__atomic_add_u64(&fuzz_global_inc, 1) + 1;
    r->state = now;
    r->inc = (inc << 1) | 1;
    fuzz_rand_step(r);
    r->state += now;
    fuzz_rand_step(r);
}

uint32_t burrow__fuzz_rand_uint32(burrow__FuzzRand *r) {
    uint64_t x = r->state;
    fuzz_rand_step(r);
    uint32_t v = (uint32_t)(((x >> 18) ^ x) >> 27);
    unsigned rot = (unsigned)(x >> 59);
    return (v >> rot) | (v << ((32U - rot) & 31U));
}

/* Lemire's multiply and shift, which draws once even when n is 0. */
uint32_t burrow__fuzz_rand_uint32n(burrow__FuzzRand *r, uint32_t n) {
    uint32_t v = burrow__fuzz_rand_uint32(r);
    uint64_t prod = (uint64_t)v * (uint64_t)n;
    uint32_t low = (uint32_t)prod;
    if (low < n) {
        uint32_t thresh = (0U - n) % n;
        while (low < thresh) {
            v = burrow__fuzz_rand_uint32(r);
            prod = (uint64_t)v * (uint64_t)n;
            low = (uint32_t)prod;
        }
    }
    return (uint32_t)(prod >> 32);
}

Int burrow__fuzz_rand_intn(burrow__FuzzRand *r, Int n) {
    if ((int64_t)(uint32_t)n != (int64_t)n)
        panic_str(BURROW_S("large Intn"));
    return (Int)burrow__fuzz_rand_uint32n(r, (uint32_t)n);
}

bool burrow__fuzz_rand_bool(burrow__FuzzRand *r) {
    return (burrow__fuzz_rand_uint32(r) & 1) == 0;
}

/* ---------------------------------------------------------------- mutator */

static const int8_t fuzz_interesting8[] = {-128, -1, 0, 1, 16, 32, 64, 100, 127};

static const int16_t fuzz_interesting16[] = {
    -32768, -129, 128, 255, 256, 512, 1000, 1024, 4096, 32767,
    /* interesting8, which Go's init appends. */
    -128, -1, 0, 1, 16, 32, 64, 100, 127};

static const int32_t fuzz_interesting32[] = {
    -2147483647 - 1, -100663046, -32769, 32768, 65535, 65536, 100663045, 2147483647,
    /* interesting16, with interesting8 on the end of it. */
    -32768, -129, 128, 255, 256, 512, 1000, 1024, 4096, 32767, -128, -1, 0, 1, 16, 32,
    64, 100, 127};

#define FUZZ_LEN(a) ((Int)(sizeof(a) / sizeof((a)[0])))

void burrow__fuzz_mutator_init(burrow__FuzzMutator *m) {
    *m = (burrow__FuzzMutator){0};
    burrow__fuzz_rand_init(&m->r);
}

void burrow__fuzz_mutator_free(burrow__FuzzMutator *m) {
    fuzz_free(m->scratch, (size_t)m->scratch_cap, 1);
    for (Int i = 0; i < m->nstrs; i++)
        fuzz_free(m->strs[i], (size_t)m->str_caps[i], 1);
    fuzz_free((void *)m->strs, (size_t)m->nstrs * sizeof(Byte *), _Alignof(Byte *));
    fuzz_free(m->str_caps, (size_t)m->nstrs * sizeof(Int), _Alignof(Int));
    for (Int i = 0; i < m->nold; i++)
        fuzz_free(m->old[i], (size_t)m->old_caps[i], 1);
    fuzz_free((void *)m->old, (size_t)m->nold * sizeof(Byte *), _Alignof(Byte *));
    fuzz_free(m->old_caps, (size_t)m->nold * sizeof(Int), _Alignof(Int));
    *m = (burrow__FuzzMutator){0};
}

static Int fuzz_rand(burrow__FuzzMutator *m, Int n) {
    return burrow__fuzz_rand_intn(&m->r, n);
}

static Int fuzz_min(Int a, Int b) {
    return a < b ? a : b;
}

static Int fuzz_choose_len(burrow__FuzzMutator *m, Int n) {
    Int x = fuzz_rand(m, 100);
    if (x < 90)
        return fuzz_rand(m, fuzz_min(8, n)) + 1;
    if (x < 99)
        return fuzz_rand(m, fuzz_min(32, n)) + 1;
    return fuzz_rand(m, n) + 1;
}

static int64_t fuzz_mutate_int(burrow__FuzzMutator *m, int64_t v, int64_t max_value) {
    for (;;) {
        int64_t max = 100;
        if (fuzz_rand(m, 2) == 0) {
            if (v >= max_value)
                continue;
            if (v > 0 && max_value - v < max)
                max = max_value - v;
            return v + 1 + (int64_t)fuzz_rand(m, (Int)max);
        }
        if (v <= -max_value)
            continue;
        if (v < 0 && max_value + v < max)
            max = max_value + v;
        return v - 1 - (int64_t)fuzz_rand(m, (Int)max);
    }
}

static uint64_t fuzz_mutate_uint(burrow__FuzzMutator *m, uint64_t v,
                                 uint64_t max_value) {
    for (;;) {
        uint64_t max = 100;
        if (fuzz_rand(m, 2) == 0) {
            if (v >= max_value)
                continue;
            if (v > 0 && max_value - v < max)
                max = max_value - v;
            return v + (uint64_t)(1 + fuzz_rand(m, (Int)max));
        }
        if (v == 0)
            continue;
        if (v < max)
            max = v;
        return v - (uint64_t)(1 + fuzz_rand(m, (Int)max));
    }
}

static double fuzz_abs(double v) {
    return v < 0 ? -v : v;
}

/* int(max) in Go truncates, and max is never more than 100 here. */
static Int fuzz_trunc(double max) {
    return (Int)max;
}

static double fuzz_mutate_float(burrow__FuzzMutator *m, double v, double max_value) {
    for (;;) {
        double max;
        switch (fuzz_rand(m, 4)) {
        case 0:
            if (v >= max_value)
                continue;
            max = 100;
            if (v > 0 && max_value - v < max)
                max = max_value - v;
            return v + (double)(1 + fuzz_rand(m, fuzz_trunc(max)));
        case 1:
            if (v <= -max_value)
                continue;
            max = 100;
            if (v < 0 && max_value + v < max)
                max = max_value + v;
            return v - (double)(1 + fuzz_rand(m, fuzz_trunc(max)));
        case 2: {
            double abs_v = fuzz_abs(v);
            if (v == 0 || abs_v >= max_value)
                continue;
            max = 10;
            if (max_value / abs_v < max)
                max = max_value / abs_v;
            return v * (double)(1 + fuzz_rand(m, fuzz_trunc(max)));
        }
        default:
            if (v == 0)
                continue;
            return v / (double)(1 + fuzz_rand(m, 10));
        }
    }
}

/* ------------------------------------------------------ byte slice mutators
 *
 * mutators_byteslice.go. Each takes the bytes at b, len long with room for
 * cap, and answers with the new length, or -1 where Go's answers nil to say it
 * could do nothing with this input. Go's copy is memmove. */

typedef Int (*FuzzByteMutator)(burrow__FuzzMutator *m, Byte *b, Int len, Int cap);

static Int fuzz_remove_bytes(burrow__FuzzMutator *m, Byte *b, Int len, Int cap) {
    (void)cap;
    if (len <= 1)
        return -1;
    Int pos0 = fuzz_rand(m, len);
    Int pos1 = pos0 + fuzz_choose_len(m, len - pos0);
    memmove(b + pos0, b + pos1, (size_t)(len - pos1));
    return len - (pos1 - pos0);
}

static Int fuzz_insert_random_bytes(burrow__FuzzMutator *m, Byte *b, Int len, Int cap) {
    Int pos = fuzz_rand(m, len + 1);
    Int n = fuzz_choose_len(m, 1024);
    if (len + n >= cap)
        return -1;
    memmove(b + pos + n, b + pos, (size_t)(len - pos));
    for (Int i = 0; i < n; i++)
        b[pos + i] = (Byte)fuzz_rand(m, 256);
    return len + n;
}

static Int fuzz_duplicate_bytes(burrow__FuzzMutator *m, Byte *b, Int len, Int cap) {
    if (len <= 1)
        return -1;
    Int src = fuzz_rand(m, len);
    Int dst = fuzz_rand(m, len);
    while (dst == src)
        dst = fuzz_rand(m, len);
    Int n = fuzz_choose_len(m, len - src);
    if (len + n * 2 >= cap)
        return -1;
    Int end = len;
    memmove(b + end + n, b + src, (size_t)n);
    memmove(b + dst + n, b + dst, (size_t)(end - dst));
    memmove(b + dst, b + end + n, (size_t)n);
    return end + n;
}

static Int fuzz_overwrite_bytes(burrow__FuzzMutator *m, Byte *b, Int len, Int cap) {
    (void)cap;
    if (len <= 1)
        return -1;
    Int src = fuzz_rand(m, len);
    Int dst = fuzz_rand(m, len);
    while (dst == src)
        dst = fuzz_rand(m, len);
    Int n = fuzz_choose_len(m, len - src - 1);
    memmove(b + dst, b + src, (size_t)fuzz_min(len - dst, n));
    return len;
}

static Int fuzz_bit_flip(burrow__FuzzMutator *m, Byte *b, Int len, Int cap) {
    (void)cap;
    if (len == 0)
        return -1;
    Int pos = fuzz_rand(m, len);
    b[pos] ^= (Byte)(1U << fuzz_rand(m, 8));
    return len;
}

static Int fuzz_xor_byte(burrow__FuzzMutator *m, Byte *b, Int len, Int cap) {
    (void)cap;
    if (len == 0)
        return -1;
    Int pos = fuzz_rand(m, len);
    b[pos] ^= (Byte)(1 + fuzz_rand(m, 255));
    return len;
}

static Int fuzz_swap_byte(burrow__FuzzMutator *m, Byte *b, Int len, Int cap) {
    (void)cap;
    if (len <= 1)
        return -1;
    Int src = fuzz_rand(m, len);
    Int dst = fuzz_rand(m, len);
    while (dst == src)
        dst = fuzz_rand(m, len);
    Byte t = b[src];
    b[src] = b[dst];
    b[dst] = t;
    return len;
}

static Int fuzz_arith_u8(burrow__FuzzMutator *m, Byte *b, Int len, Int cap) {
    (void)cap;
    if (len == 0)
        return -1;
    Int pos = fuzz_rand(m, len);
    Byte v = (Byte)(fuzz_rand(m, 35) + 1);
    if (burrow__fuzz_rand_bool(&m->r))
        b[pos] = (Byte)(b[pos] + v);
    else
        b[pos] = (Byte)(b[pos] - v);
    return len;
}

/* binary.ByteOrder's Uint and PutUint over w bytes, little endian when le. */
static uint64_t fuzz_get(const Byte *p, int w, bool le) {
    uint64_t v = 0;
    for (int i = 0; i < w; i++)
        v |= (uint64_t)p[le ? i : w - 1 - i] << (8 * i);
    return v;
}

static void fuzz_put(Byte *p, int w, bool le, uint64_t v) {
    for (int i = 0; i < w; i++)
        p[le ? i : w - 1 - i] = (Byte)(v >> (8 * i));
}

static Int fuzz_arith(burrow__FuzzMutator *m, Byte *b, Int len, int w) {
    if (len < w)
        return -1;
    uint64_t v = (uint64_t)(fuzz_rand(m, 35) + 1);
    if (burrow__fuzz_rand_bool(&m->r))
        v = 0 - v;
    Int pos = fuzz_rand(m, len - (w - 1));
    bool le = burrow__fuzz_rand_bool(&m->r);
    fuzz_put(b + pos, w, le, fuzz_get(b + pos, w, le) + v);
    return len;
}

static Int fuzz_arith_u16(burrow__FuzzMutator *m, Byte *b, Int len, Int cap) {
    (void)cap;
    return fuzz_arith(m, b, len, 2);
}

static Int fuzz_arith_u32(burrow__FuzzMutator *m, Byte *b, Int len, Int cap) {
    (void)cap;
    return fuzz_arith(m, b, len, 4);
}

static Int fuzz_arith_u64(burrow__FuzzMutator *m, Byte *b, Int len, Int cap) {
    (void)cap;
    return fuzz_arith(m, b, len, 8);
}

static Int fuzz_interesting_u8(burrow__FuzzMutator *m, Byte *b, Int len, Int cap) {
    (void)cap;
    if (len == 0)
        return -1;
    Int pos = fuzz_rand(m, len);
    b[pos] = (Byte)fuzz_interesting8[fuzz_rand(m, FUZZ_LEN(fuzz_interesting8))];
    return len;
}

static Int fuzz_interesting_u16(burrow__FuzzMutator *m, Byte *b, Int len, Int cap) {
    (void)cap;
    if (len < 2)
        return -1;
    Int pos = fuzz_rand(m, len - 1);
    uint16_t v =
        (uint16_t)fuzz_interesting16[fuzz_rand(m, FUZZ_LEN(fuzz_interesting16))];
    fuzz_put(b + pos, 2, burrow__fuzz_rand_bool(&m->r), v);
    return len;
}

static Int fuzz_interesting_u32(burrow__FuzzMutator *m, Byte *b, Int len, Int cap) {
    (void)cap;
    if (len < 4)
        return -1;
    Int pos = fuzz_rand(m, len - 3);
    uint32_t v =
        (uint32_t)fuzz_interesting32[fuzz_rand(m, FUZZ_LEN(fuzz_interesting32))];
    fuzz_put(b + pos, 4, burrow__fuzz_rand_bool(&m->r), v);
    return len;
}

static Int fuzz_insert_constant_bytes(burrow__FuzzMutator *m, Byte *b, Int len,
                                      Int cap) {
    if (len <= 1)
        return -1;
    Int dst = fuzz_rand(m, len);
    Int n = fuzz_choose_len(m, 4096);
    if (len + n >= cap)
        return -1;
    memmove(b + dst + n, b + dst, (size_t)(len - dst));
    Byte rb = (Byte)fuzz_rand(m, 256);
    memset(b + dst, rb, (size_t)n);
    return len + n;
}

static Int fuzz_overwrite_constant_bytes(burrow__FuzzMutator *m, Byte *b, Int len,
                                         Int cap) {
    (void)cap;
    if (len <= 1)
        return -1;
    Int dst = fuzz_rand(m, len);
    Int n = fuzz_choose_len(m, len - dst);
    Byte rb = (Byte)fuzz_rand(m, 256);
    memset(b + dst, rb, (size_t)n);
    return len;
}

static Int fuzz_shuffle_bytes(burrow__FuzzMutator *m, Byte *b, Int len, Int cap) {
    (void)cap;
    if (len <= 1)
        return -1;
    Int dst = fuzz_rand(m, len);
    Int n = fuzz_choose_len(m, len - dst);
    if (n <= 2)
        return -1;
    for (Int i = n - 1; i > 0; i--) {
        Int j = fuzz_rand(m, i + 1);
        Byte t = b[dst + i];
        b[dst + i] = b[dst + j];
        b[dst + j] = t;
    }
    return len;
}

static Int fuzz_swap_bytes(burrow__FuzzMutator *m, Byte *b, Int len, Int cap) {
    if (len <= 1)
        return -1;
    Int src = fuzz_rand(m, len);
    Int dst = fuzz_rand(m, len);
    while (dst == src)
        dst = fuzz_rand(m, len);
    Int max = dst > src ? dst : src;
    Int n = fuzz_choose_len(m, len - max - 1);
    if ((src > dst && dst + n >= src) || (dst > src && src + n >= dst))
        return -1;
    if (len + n >= cap)
        return -1;
    Int end = len;
    memmove(b + end, b + dst, (size_t)n);
    memmove(b + dst, b + src, (size_t)n);
    memmove(b + src, b + end, (size_t)n);
    return end;
}

static const FuzzByteMutator fuzz_byte_mutators[] = {
    fuzz_remove_bytes,
    fuzz_insert_random_bytes,
    fuzz_duplicate_bytes,
    fuzz_overwrite_bytes,
    fuzz_bit_flip,
    fuzz_xor_byte,
    fuzz_swap_byte,
    fuzz_arith_u8,
    fuzz_arith_u16,
    fuzz_arith_u32,
    fuzz_arith_u64,
    fuzz_interesting_u8,
    fuzz_interesting_u16,
    fuzz_interesting_u32,
    fuzz_insert_constant_bytes,
    fuzz_overwrite_constant_bytes,
    fuzz_shuffle_bytes,
    fuzz_swap_bytes,
};

Int burrow__fuzz_mutate_bytes(burrow__FuzzMutator *m, Byte *b, Int len, Int cap) {
    for (;;) {
        FuzzByteMutator mut =
            fuzz_byte_mutators[fuzz_rand(m, FUZZ_LEN(fuzz_byte_mutators))];
        Int got = mut(m, b, len, cap);
        if (got >= 0)
            return got;
    }
}

static void fuzz_retire(burrow__FuzzMutator *m, Byte *p, Int cap) {
    if (p == NULL)
        return;
    Int n = m->nold;
    m->old =
        (Byte **)fuzz_must_realloc((void *)m->old, (size_t)n * sizeof(Byte *),
                                   (size_t)(n + 1) * sizeof(Byte *), _Alignof(Byte *));
    m->old_caps =
        (Int *)fuzz_must_realloc(m->old_caps, (size_t)n * sizeof(Int),
                                 (size_t)(n + 1) * sizeof(Int), _Alignof(Int));
    m->old[n] = p;
    m->old_caps[n] = cap;
    m->nold = n + 1;
}

/* Copies len bytes from p into the scratch buffer, first growing it to
 * max_per_val when it is smaller, as Go's append into a new slice does. */
static void fuzz_to_scratch(burrow__FuzzMutator *m, const Byte *p, Int len,
                            Int max_per_val) {
    if (m->scratch_cap < max_per_val) {
        Byte *s = (Byte *)fuzz_must_alloc((size_t)max_per_val, 1);
        memcpy(s, p, (size_t)len);
        fuzz_retire(m, m->scratch, m->scratch_cap);
        m->scratch = s;
        m->scratch_cap = max_per_val;
    } else {
        memmove(m->scratch, p, (size_t)len);
    }
}

/* The buffer a mutated string at index i is kept in, at least n long. Go
 * makes a new string each time. This keeps one per index and reuses it, so a
 * string from one call is only good until the next call changes that same
 * value. */
static Byte *fuzz_str_buf(burrow__FuzzMutator *m, Int i, Int n) {
    if (i >= m->nstrs) {
        Int total = i + 1;
        m->strs = (Byte **)fuzz_must_realloc(
            (void *)m->strs, (size_t)m->nstrs * sizeof(Byte *),
            (size_t)total * sizeof(Byte *), _Alignof(Byte *));
        m->str_caps =
            (Int *)fuzz_must_realloc(m->str_caps, (size_t)m->nstrs * sizeof(Int),
                                     (size_t)total * sizeof(Int), _Alignof(Int));
        for (Int k = m->nstrs; k < total; k++) {
            m->strs[k] = NULL;
            m->str_caps[k] = 0;
        }
        m->nstrs = total;
    }
    if (m->str_caps[i] < n) {
        /* Nothing points into the old one by now: the value it held was
         * copied into the scratch buffer before this was called. */
        Int ncap = m->str_caps[i] * 2 > 64 ? m->str_caps[i] * 2 : 64;
        if (ncap < n)
            ncap = n;
        fuzz_free(m->strs[i], (size_t)m->str_caps[i], 1);
        m->strs[i] = (Byte *)fuzz_must_alloc((size_t)ncap, 1);
        m->str_caps[i] = ncap;
    }
    return m->strs[i];
}

void burrow__fuzz_mutate(burrow__FuzzMutator *m, Any *vals, Int n, Int max_bytes) {
    Int max_per_val = max_bytes / n - 100;
    Int i = fuzz_rand(m, n);
    Any v = vals[i];
    const Type *t = v.t;
    void *d = v.data;
    if (t == TYPE_INT) {
        *(Int *)d = (Int)fuzz_mutate_int(m, (int64_t)*(Int *)d,
                                         sizeof(Int) == 8 ? INT64_MAX : INT32_MAX);
    } else if (t == TYPE_INT8) {
        *(int8_t *)d = (int8_t)fuzz_mutate_int(m, *(int8_t *)d, INT8_MAX);
    } else if (t == TYPE_INT16) {
        *(int16_t *)d = (int16_t)fuzz_mutate_int(m, *(int16_t *)d, INT16_MAX);
    } else if (t == TYPE_INT64) {
        *(int64_t *)d = fuzz_mutate_int(m, *(int64_t *)d, INT64_MAX);
    } else if (t == TYPE_UINT) {
        *(Uint *)d = (Uint)fuzz_mutate_uint(
            m, (uint64_t)*(Uint *)d, sizeof(Uint) == 8 ? UINT64_MAX : UINT32_MAX);
    } else if (t == TYPE_UINT16) {
        *(uint16_t *)d = (uint16_t)fuzz_mutate_uint(m, *(uint16_t *)d, UINT16_MAX);
    } else if (t == TYPE_UINT32) {
        *(uint32_t *)d = (uint32_t)fuzz_mutate_uint(m, *(uint32_t *)d, UINT32_MAX);
    } else if (t == TYPE_UINT64) {
        *(uint64_t *)d = fuzz_mutate_uint(m, *(uint64_t *)d, UINT64_MAX);
    } else if (t == TYPE_FLOAT32) {
        *(float *)d = (float)fuzz_mutate_float(m, (double)*(float *)d, (double)FLT_MAX);
    } else if (t == TYPE_FLOAT64) {
        *(double *)d = fuzz_mutate_float(m, *(double *)d, DBL_MAX);
    } else if (t == TYPE_BOOL) {
        if (fuzz_rand(m, 2) == 1)
            *(bool *)d = !*(bool *)d;
    } else if (t == TYPE_INT32) {
        *(int32_t *)d = (int32_t)fuzz_mutate_int(m, *(int32_t *)d, INT32_MAX);
    } else if (t == TYPE_UINT8) {
        *(uint8_t *)d = (uint8_t)fuzz_mutate_uint(m, *(uint8_t *)d, UINT8_MAX);
    } else if (t == TYPE_STRING) {
        Str s = *(Str *)d;
        if (s.len > max_per_val)
            panic_str(fmt_sprintf_v(error_allocator(),
                                    "cannot mutate bytes of length %d", s.len));
        fuzz_to_scratch(m, s.p, s.len, max_per_val);
        Int got = burrow__fuzz_mutate_bytes(m, m->scratch, s.len, m->scratch_cap);
        Byte *p = fuzz_str_buf(m, i, got);
        memcpy(p, m->scratch, (size_t)got);
        *(Str *)d = str_from_bytes(p, got);
    } else if (t == TYPE_BYTES) {
        Slice *s = (Slice *)d;
        if (s->len > max_per_val)
            panic_str(fmt_sprintf_v(error_allocator(),
                                    "cannot mutate bytes of length %d", s->len));
        fuzz_to_scratch(m, (const Byte *)s->p, s->len, max_per_val);
        Int got = burrow__fuzz_mutate_bytes(m, m->scratch, s->len, m->scratch_cap);
        s->p = m->scratch;
        s->len = got;
        s->cap = m->scratch_cap;
    } else {
        panic_str(
            fmt_sprintf_v(error_allocator(), "type not supported for mutating: %T", v));
    }
}

/* ---------------------------------------------------------- minimizeBytes */

void burrow__fuzz_minimize_bytes(Byte *v, Int *lenp, burrow__FuzzTry try_fn,
                                 burrow__FuzzShouldStop stop, void *env) {
    Int len = *lenp;
    Byte *tmp = (Byte *)fuzz_must_alloc((size_t)(len > 0 ? len : 1), 1);
    Int tmp_cap = len > 0 ? len : 1;

    /* Cutting from the end, in halving steps. */
    for (Int n = 1024; n != 0; n /= 2) {
        while (len > n) {
            if (stop(env))
                goto done;
            if (!try_fn(env, v, len - n))
                break;
            len -= n;
        }
    }

    /* Removing one byte at a time. */
    for (Int i = 0; i < len - 1; i++) {
        if (stop(env))
            goto done;
        memcpy(tmp, v, (size_t)i);
        memcpy(tmp + i, v + i + 1, (size_t)(len - i - 1));
        if (!try_fn(env, tmp, len - 1))
            continue;
        memmove(v + i, v + i + 1, (size_t)(len - i - 1));
        len--;
        i--;
    }

    /* Removing runs of bytes. */
    for (Int i = 0; i < len - 1; i++) {
        memcpy(tmp, v, (size_t)i);
        for (Int j = len; j > i + 1; j--) {
            if (stop(env))
                goto done;
            Int clen = len - j + i;
            memcpy(tmp + i, v + j, (size_t)(clen - i));
            if (!try_fn(env, tmp, clen))
                continue;
            memmove(v + i, v + j, (size_t)(len - j));
            len = clen;
            j = len;
        }
    }

    /* Swapping each byte for a printable one. */
    static const char printable[] = "012789ABCXYZabcxyz !\"#$%&'()*+,.";
    for (Int i = 0; i < len; i++) {
        if (stop(env))
            goto done;
        Byte b = v[i];
        for (size_t k = 0; k < sizeof printable - 1; k++) {
            v[i] = (Byte)printable[k];
            if (try_fn(env, v, len))
                break;
            v[i] = b;
        }
    }

done:
    *lenp = len;
    fuzz_free(tmp, (size_t)tmp_cap, 1);
}

/* ----------------------------------------------------------------- sha256
 *
 * crypto/sha256 is a package of its own and not written yet, and the corpus
 * file names need it, so this is the textbook one. */

static const uint32_t fuzz_sha_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
    0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
    0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
    0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
    0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
    0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2};

static uint32_t fuzz_rotr(uint32_t x, unsigned n) {
    return (x >> n) | (x << (32 - n));
}

static void fuzz_sha_block(uint32_t h[8], const Byte *p) {
    uint32_t w[64];
    for (Int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 |
               (uint32_t)p[4 * i + 2] << 8 | (uint32_t)p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 =
            fuzz_rotr(w[i - 15], 7) ^ fuzz_rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 =
            fuzz_rotr(w[i - 2], 17) ^ fuzz_rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6],
             k = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = k + (fuzz_rotr(e, 6) ^ fuzz_rotr(e, 11) ^ fuzz_rotr(e, 25)) +
                      ((e & f) ^ (~e & g)) + fuzz_sha_k[i] + w[i];
        uint32_t t2 = (fuzz_rotr(a, 2) ^ fuzz_rotr(a, 13) ^ fuzz_rotr(a, 22)) +
                      ((a & b) ^ (a & c) ^ (b & c));
        k = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += k;
}

void burrow__fuzz_sha256(const void *data, Int n, Byte out[32]) {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const Byte *p = (const Byte *)data;
    Int left = n;
    while (left >= 64) {
        fuzz_sha_block(h, p);
        p += 64;
        left -= 64;
    }
    Byte tail[128] = {0};
    memcpy(tail, p, (size_t)left);
    tail[left] = 0x80;
    Int blocks = left < 56 ? 1 : 2;
    uint64_t bits = (uint64_t)n * 8;
    for (int i = 0; i < 8; i++)
        tail[blocks * 64 - 1 - i] = (Byte)(bits >> (8 * i));
    for (Int i = 0; i < blocks; i++)
        fuzz_sha_block(h, tail + 64 * i);
    for (Int i = 0; i < 8; i++) {
        out[4 * i] = (Byte)(h[i] >> 24);
        out[4 * i + 1] = (Byte)(h[i] >> 16);
        out[4 * i + 2] = (Byte)(h[i] >> 8);
        out[4 * i + 3] = (Byte)h[i];
    }
}

/* The first n bytes of the sum of data, in lower case hex, into out, which
 * has room for 2n+1. */
static Str fuzz_sum_hex(Str data, Int n, char *out) {
    static const char hex[] = "0123456789abcdef";
    Byte sum[32];
    burrow__fuzz_sha256(data.p, data.len, sum);
    for (Int i = 0; i < n; i++) {
        out[2 * i] = hex[sum[i] >> 4];
        out[2 * i + 1] = hex[sum[i] & 15];
    }
    out[2 * n] = '\0';
    return str_from_bytes((const Byte *)out, 2 * n);
}

/* ---------------------------------------------------------- shared memory
 *
 * mem.go. A temporary file, mapped by the coordinator and by one worker, with
 * this header at the front and the input after it. */

typedef struct FuzzMemHeader {
    /* How many inputs the worker has run since the coordinator last zeroed
     * this. */
    int64_t count;
    int64_t value_len;
    /* The worker's generator as it was before the mutations that led to the
     * input it is on, for the coordinator to replay them. */
    uint64_t rand_state;
    uint64_t rand_inc;
    /* Set while minimizing, when the bytes after the header are the raw
     * value of the one argument being made smaller rather than a corpus
     * file. */
    uint8_t raw_in_mem;
} FuzzMemHeader;

/* workerSharedMemSize: the largest input a worker is handed. */
#define FUZZ_VALUE_SIZE ((Int)100 << 20)

/* chainedMutations: how many mutations are stacked on one input before the
 * worker goes back to the original. */
#define FUZZ_CHAINED 5

typedef struct FuzzMem {
    int64_t fd;
    void *region;
    int64_t size;
    /* The file's name, NUL terminated on the heap, in the coordinator, which
     * removes the file when it is done with it. */
    char *path;
    size_t path_size;
} FuzzMem;

static FuzzMemHeader *fuzz_hdr(FuzzMem *m) {
    return (FuzzMemHeader *)m->region;
}

static Byte *fuzz_mem_bytes(FuzzMem *m) {
    return (Byte *)m->region + sizeof(FuzzMemHeader);
}

static Int fuzz_mem_cap(FuzzMem *m) {
    return (Int)(m->size - (int64_t)sizeof(FuzzMemHeader));
}

/* valueRef: the value, borrowed from the mapping. */
static Str fuzz_mem_value(FuzzMem *m) {
    return str_from_bytes(fuzz_mem_bytes(m), (Int)fuzz_hdr(m)->value_len);
}

static void fuzz_mem_set_value(FuzzMem *m, Str v) {
    if (v.len > fuzz_mem_cap(m))
        panic_str(fmt_sprintf_v(error_allocator(),
                                "value length %d larger than shared memory capacity %d",
                                v.len, fuzz_mem_cap(m)));
    fuzz_hdr(m)->value_len = v.len;
    memcpy(fuzz_mem_bytes(m), v.p, (size_t)v.len);
}

/* os.TempDir. */
static Str fuzz_temp_dir(void) {
    Str d;
#if defined(BURROW_OS_WINDOWS)
    if (fuzz_getenv("TMP", &d) && d.len > 0)
        return d;
    if (fuzz_getenv("TEMP", &d) && d.len > 0)
        return d;
    if (fuzz_getenv("USERPROFILE", &d) && d.len > 0)
        return d;
    return BURROW_S("C:\\Windows");
#else
    if (fuzz_getenv("TMPDIR", &d) && d.len > 0)
        return d;
    return BURROW_S("/tmp");
#endif
}

#if defined(BURROW_OS_WINDOWS)
#define FUZZ_SEP_CH '\\'
#else
#define FUZZ_SEP_CH '/'
#endif

/* sharedMemTempFile: a new file under os.TempDir named as os.CreateTemp names
 * one, "fuzz-" and a random number, grown to size and mapped. */
static bool fuzz_mem_create(FuzzMem *m, int64_t value_size, Alloc *a, Str *err) {
    *m = (FuzzMem){.fd = PAL_INVALID_HANDLE};
    Str dir = fuzz_temp_dir();
    while (dir.len > 1 &&
           (dir.p[dir.len - 1] == '/' || dir.p[dir.len - 1] == FUZZ_SEP_CH))
        dir.len--;
    size_t cap = (size_t)dir.len + 32;
    char *path = (char *)fuzz_must_alloc(cap, 1);
    PalErrno e = PAL_OK;
    for (int tries = 0;; tries++) {
        uint32_t r = 0;
        if (!pal_random_bytes(&r, sizeof r, &e))
            r = (uint32_t)pal_clock_monotonic();
        snprintf(path, cap, "%.*s%cfuzz-%u", (int)dir.len, (const char *)dir.p,
                 FUZZ_SEP_CH, (unsigned)r);
        m->fd = pal_open(path, PAL_O_RDWR | PAL_O_CREATE | PAL_O_EXCL, 0600, &e);
        if (m->fd >= 0)
            break;
        if (e != PAL_EEXIST || tries == 10000) {
            *err = fmt_sprintf_v(a, "open %s: %s", path, pal_errno_string(e));
            fuzz_free(path, cap, 1);
            return false;
        }
    }
    m->path = path;
    m->path_size = cap;
    m->size = (int64_t)sizeof(FuzzMemHeader) + value_size;
    if (!pal_ftruncate(m->fd, m->size, &e)) {
        *err = fmt_sprintf_v(a, "truncate %s: %s", path, pal_errno_string(e));
        goto fail;
    }
    m->region = pal_mmap(m->fd, 0, m->size, PAL_PROT_READ | PAL_PROT_WRITE, &e);
    if (m->region == NULL) {
        *err = fmt_sprintf_v(a, "mmap: %s", pal_errno_string(e));
        goto fail;
    }
    return true;

fail:
    pal_close(m->fd, NULL);
    pal_unlink(path, NULL);
    fuzz_free(path, cap, 1);
    *m = (FuzzMem){.fd = PAL_INVALID_HANDLE};
    return false;
}

/* sharedMem.Close: unmapped, closed, and in the coordinator removed. */
static bool fuzz_mem_close(FuzzMem *m, Alloc *a, Str *err) {
    PalErrno e = PAL_OK;
    bool ok = true;
    if (m->region != NULL && !pal_munmap(m->region, m->size, &e)) {
        *err = fmt_sprintf_v(a, "munmap: %s", pal_errno_string(e));
        ok = false;
    }
    if (m->fd != PAL_INVALID_HANDLE && !pal_close(m->fd, &e) && ok) {
        *err = fmt_sprintf_v(a, "close: %s", pal_errno_string(e));
        ok = false;
    }
    if (m->path != NULL) {
        if (!pal_unlink(m->path, &e) && ok) {
            *err = fmt_sprintf_v(a, "remove %s: %s", m->path, pal_errno_string(e));
            ok = false;
        }
        fuzz_free(m->path, m->path_size, 1);
    }
    *m = (FuzzMem){.fd = PAL_INVALID_HANDLE};
    return ok;
}

/* ---------------------------------------------------------------- the wire
 *
 * A call is a kind byte and its arguments, and the answer is the response's
 * fields in order. Integers are eight bytes little endian and a string is a
 * four byte length and the bytes. */

enum { FUZZ_CALL_PING = 1, FUZZ_CALL_FUZZ = 2, FUZZ_CALL_MINIMIZE = 3 };

static void fuzz_put_u8(FuzzBuf *b, uint8_t v) {
    fuzz_buf_append(b, &v, 1);
}

static void fuzz_put_i64(FuzzBuf *b, int64_t v) {
    Byte p[8];
    fuzz_put(p, 8, true, (uint64_t)v);
    fuzz_buf_append(b, p, 8);
}

static void fuzz_put_str(FuzzBuf *b, Str s) {
    Byte p[4];
    fuzz_put(p, 4, true, (uint64_t)s.len);
    fuzz_buf_append(b, p, 4);
    fuzz_buf_append(b, s.p, s.len);
}

static bool fuzz_write_all(int64_t fd, const Byte *p, Int n, PalErrno *err) {
    while (n > 0) {
        int64_t got = pal_write(fd, p, n, err);
        if (got < 0) {
            if (*err == PAL_EINTR)
                continue;
            return false;
        }
        p += got;
        n -= (Int)got;
    }
    return true;
}

/* Reads a message field by field. The first failure sticks, and its text is
 * what encoding/json would have said: "EOF" when the stream ends before a
 * message starts, "unexpected EOF" when it ends inside one. */
typedef struct FuzzReader {
    int64_t fd;
    bool started;
    bool failed;
    bool eof;
    PalErrno err;
    Alloc *a;
} FuzzReader;

static bool fuzz_read(FuzzReader *r, void *dst, Int n) {
    Byte *p = (Byte *)dst;
    while (n > 0 && !r->failed) {
        int64_t got = pal_read(r->fd, p, n, &r->err);
        if (got < 0 && r->err == PAL_EINTR)
            continue;
        if (got <= 0) {
            r->failed = true;
            r->eof = got == 0;
            break;
        }
        r->started = true;
        p += got;
        n -= (Int)got;
    }
    if (r->failed)
        memset(p, 0, (size_t)n);
    return !r->failed;
}

static uint8_t fuzz_get_u8(FuzzReader *r) {
    uint8_t v = 0;
    fuzz_read(r, &v, 1);
    return v;
}

static int64_t fuzz_get_i64(FuzzReader *r) {
    Byte p[8];
    fuzz_read(r, p, 8);
    return (int64_t)fuzz_get(p, 8, true);
}

static Str fuzz_get_str(FuzzReader *r) {
    Byte p[4];
    if (!fuzz_read(r, p, 4))
        return (Str){0};
    Int n = (Int)fuzz_get(p, 4, true);
    if (n == 0)
        return (Str){0};
    Byte *s = (Byte *)mem_alloc(r->a, (size_t)n, 1);
    if (s == NULL)
        panic_str(BURROW_S("testing: out of memory"));
    if (!fuzz_read(r, s, n))
        return (Str){0};
    return str_from_bytes(s, n);
}

static Str fuzz_reader_error(FuzzReader *r) {
    if (r->eof)
        return r->started ? BURROW_S("unexpected EOF") : BURROW_S("EOF");
    return fmt_sprintf_v(r->a, "read: %s", pal_errno_string(r->err));
}

/* ------------------------------------------------------------ worker side */

/* Room for any value fuzzing allows, which is at most a Slice. */
typedef union FuzzCell {
    Str s;
    Slice b;
    int64_t i;
    uint64_t u;
    double f;
} FuzzCell;

/* The time a single run of the target may take before the worker gives up
 * on it and dies, which the coordinator reports as a hang. */
#define FUZZ_DEADLOCK_NS ((int64_t)60 * 1000000000)

typedef struct FuzzServer {
    int64_t in;
    int64_t out;
    FuzzMem mem;
    burrow__FuzzMutator m;
    burrow__FuzzRun run;
    void *env;

    /* The watchdog, Go's time.AfterFunc(60*time.Second) around each run. */
    burrow__Lock wd_mu;
    int64_t wd_deadline;
    burrow__Note wd_quit;
    burrow__Thread wd;
} FuzzServer;

static void fuzz_watchdog(void *arg) {
    FuzzServer *s = (FuzzServer *)arg;
    while (!burrow__note_sleep_timeout(&s->wd_quit, 1000000000)) {
        burrow__lock(&s->wd_mu);
        int64_t d = s->wd_deadline;
        burrow__unlock(&s->wd_mu);
        if (d != 0 && pal_clock_monotonic() > d) {
            static const char msg[] = "panic: deadlocked!\n\n";
            fwrite(msg, 1, sizeof msg - 1, stderr);
            fflush(stderr);
            pal_exit(2);
        }
    }
}

/* fuzzFn: one run of the target on vals, counted in the shared memory. */
static bool fuzz_server_run(FuzzServer *s, Any *vals, Int n, Alloc *a, int64_t *dur,
                            Str *msg) {
    fuzz_hdr(&s->mem)->count++;
    int64_t start = pal_clock_monotonic();
    burrow__lock(&s->wd_mu);
    s->wd_deadline = start + FUZZ_DEADLOCK_NS;
    burrow__unlock(&s->wd_mu);
    *msg = (Str){0};
    bool ok = s->run(s->env, vals, n, a, msg);
    burrow__lock(&s->wd_mu);
    s->wd_deadline = 0;
    burrow__unlock(&s->wd_mu);
    *dur = pal_clock_monotonic() - start;
    return ok;
}

static void fuzz_views_reset(Any *views, FuzzCell *cells, const Any *orig, Int n) {
    for (Int i = 0; i < n; i++) {
        memcpy(&cells[i], orig[i].data, orig[i].t->size);
        views[i] = (Any){orig[i].t, &cells[i]};
    }
}

/* workerServer.fuzz. */
static void fuzz_serve_fuzz(FuzzServer *s, FuzzReader *r, FuzzBuf *resp) {
    int64_t timeout = fuzz_get_i64(r);
    int64_t limit = fuzz_get_i64(r);
    bool warmup = fuzz_get_u8(r) != 0;
    if (r->failed)
        return;
    int64_t start = pal_clock_monotonic();
    FuzzMemHeader *h = fuzz_hdr(&s->mem);
    h->rand_state = s->m.r.state;
    h->rand_inc = s->m.r.inc;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str internal = {0};
    Str err = {0};
    int64_t interesting = 0;
    Any *orig = NULL;
    Int n = 0;
    Str uerr = {0};
    if (limit > 0 && h->count >= limit) {
        internal = fmt_sprintf_v(
            a, "mem.header().count %d already exceeds args.Limit %d", h->count, limit);
    } else if (!burrow__testing_corpus_unmarshal(a, fuzz_mem_value(&s->mem), &orig, &n,
                                                 &uerr)) {
        internal = uerr;
    } else {
        Any *views = (Any *)fuzz_must_alloc((size_t)n * sizeof(Any), _Alignof(Any));
        FuzzCell *cells = (FuzzCell *)fuzz_must_alloc((size_t)n * sizeof(FuzzCell),
                                                      _Alignof(FuzzCell));
        fuzz_views_reset(views, cells, orig, n);
        int64_t dur;
        if (warmup) {
            if (!fuzz_server_run(s, views, n, a, &dur, &err))
                err =
                    err.len > 0 ? err : BURROW_S("fuzz function failed with no input");
            else
                interesting = dur;
        } else {
            int64_t deadline = timeout != 0 ? start + timeout : 0;
            ArenaMark mark = arena_mark(&ar);
            while (deadline == 0 || pal_clock_monotonic() < deadline) {
                if (h->count % FUZZ_CHAINED == 0) {
                    fuzz_views_reset(views, cells, orig, n);
                    h->rand_state = s->m.r.state;
                    h->rand_inc = s->m.r.inc;
                }
                burrow__fuzz_mutate(&s->m, views, n, FUZZ_VALUE_SIZE);
                if (!fuzz_server_run(s, views, n, a, &dur, &err)) {
                    err = err.len > 0 ? err
                                      : BURROW_S("fuzz function failed with no input");
                    break;
                }
                arena_release(&ar, mark);
                if (limit > 0 && h->count >= limit)
                    break;
            }
        }
        fuzz_free(cells, (size_t)n * sizeof(FuzzCell), _Alignof(FuzzCell));
        fuzz_free(views, (size_t)n * sizeof(Any), _Alignof(Any));
        burrow__testing_values_free(orig, n);
    }
    fuzz_put_i64(resp, pal_clock_monotonic() - start);
    fuzz_put_i64(resp, interesting);
    fuzz_put_i64(resp, h->count);
    fuzz_put_str(resp, err);
    fuzz_put_str(resp, internal);
    arena_free(&ar);
}

/* What minimizeInput's closures share. */
typedef struct FuzzMin {
    FuzzServer *s;
    Any *vals;
    Int n;
    Int index;
    int64_t deadline;
    int64_t limit;
    Alloc *a;
    /* *bPtr in Go: the part of the shared memory the last candidate went
     * to. */
    Int mem_len;
    bool failed;
    Str msg;
} FuzzMin;

static bool fuzz_min_stop(void *env) {
    FuzzMin *c = (FuzzMin *)env;
    return (c->deadline != 0 && pal_clock_monotonic() >= c->deadline) ||
           (c->limit > 0 && fuzz_hdr(&c->s->mem)->count >= c->limit);
}

/* Points the value being minimized at n bytes at p. */
static void fuzz_min_set(FuzzMin *c, const Byte *p, Int n) {
    Any v = c->vals[c->index];
    if (v.t == TYPE_STRING) {
        *(Str *)v.data = str_from_bytes(p, n);
    } else {
        Slice *s = (Slice *)v.data;
        s->p = (void *)(uintptr_t)p;
        s->len = n;
        s->cap = n;
    }
}

/* tryMinimized. The candidate is copied into the shared memory first, so
 * that the coordinator can recover it if the run kills the worker. Go copies
 * it over the last candidate, which may be shorter, and so does this. */
static bool fuzz_min_try(void *env, const Byte *p, Int n) {
    FuzzMin *c = (FuzzMin *)env;
    Any v = c->vals[c->index];
    FuzzCell prev;
    memcpy(&prev, v.data, v.t->size);
    fuzz_min_set(c, p, n);
    memmove(fuzz_mem_bytes(&c->s->mem), p, (size_t)fuzz_min(c->mem_len, n));
    c->mem_len = n;
    fuzz_hdr(&c->s->mem)->value_len = n;
    int64_t dur;
    Str msg;
    if (!fuzz_server_run(c->s, c->vals, c->n, c->a, &dur, &msg)) {
        c->failed = true;
        c->msg = msg;
        return true;
    }
    memcpy(v.data, &prev, v.t->size);
    return false;
}

/* workerServer.minimize and minimizeInput. */
static void fuzz_serve_minimize(FuzzServer *s, FuzzReader *r, FuzzBuf *resp) {
    int64_t timeout = fuzz_get_i64(r);
    int64_t limit = fuzz_get_i64(r);
    int64_t index = fuzz_get_i64(r);
    if (r->failed)
        return;
    int64_t start = pal_clock_monotonic();
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Any *vals = NULL;
    Int n = 0;
    Str uerr = {0};
    if (!burrow__testing_corpus_unmarshal(a, fuzz_mem_value(&s->mem), &vals, &n, &uerr))
        panic_str(str_clone(error_allocator(), uerr));
    if (index < 0 || index >= n ||
        (vals[index].t != TYPE_STRING && vals[index].t != TYPE_BYTES))
        panic_str(BURROW_S("impossible"));
    FuzzMin c = {s,
                 vals,
                 n,
                 (Int)index,
                 timeout != 0 ? start + timeout : 0,
                 limit,
                 a,
                 (Int)fuzz_hdr(&s->mem)->value_len,
                 false,
                 {0}};
    FuzzMemHeader *h = fuzz_hdr(&s->mem);
    bool success = false;
    Any v = vals[index];
    FuzzCell orig;
    memcpy(&orig, v.data, v.t->size);
    int64_t dur;
    Str msg;
    if (!fuzz_min_stop(&c)) {
        if (fuzz_server_run(s, vals, n, a, &dur, &msg)) {
            /* It passes, so there is nothing to make smaller. */
        } else {
            h->raw_in_mem = 1;
            const Byte *p = v.t == TYPE_STRING ? orig.s.p : (const Byte *)orig.b.p;
            Int len = v.t == TYPE_STRING ? orig.s.len : orig.b.len;
            Int cap = len > 0 ? len : 1;
            Byte *buf = (Byte *)fuzz_must_alloc((size_t)cap, 1);
            memcpy(buf, p, (size_t)len);
            burrow__fuzz_minimize_bytes(buf, &len, fuzz_min_try, fuzz_min_stop, &c);
            fuzz_min_set(&c, buf, len);
            success = true;
            fuzz_mem_set_value(&s->mem, burrow__testing_corpus_marshal(a, vals, n));
            h->raw_in_mem = 0;
            memcpy(v.data, &orig, v.t->size);
            fuzz_free(buf, (size_t)cap, 1);
        }
    }
    memcpy(v.data, &orig, v.t->size);
    burrow__testing_values_free(vals, n);
    fuzz_put_u8(resp, success ? 1 : 0);
    fuzz_put_str(resp, success && c.failed ? c.msg : (Str){0});
    fuzz_put_i64(resp, pal_clock_monotonic() - start);
    arena_free(&ar);
}

/* getWorkerComm: the pipes and the memory the coordinator handed down. */
static bool fuzz_worker_comm(FuzzServer *s, Alloc *a, Str *err) {
    int64_t memfd;
#if defined(BURROW_OS_WINDOWS)
    Str v;
    if (!fuzz_getenv("BURROW_TEST_FUZZ_WORKER_HANDLES", &v)) {
        *err = BURROW_S("worker handles not set");
        return false;
    }
    int64_t hs[3] = {0};
    int k = 0;
    for (Int i = 0; i < v.len && k < 3; i++) {
        Byte ch = v.p[i];
        if (ch == ',') {
            k++;
            continue;
        }
        int d = ch >= '0' && ch <= '9'   ? ch - '0'
                : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10
                : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10
                                         : -1;
        if (d < 0)
            break;
        hs[k] = hs[k] * 16 + d;
    }
    if (k != 2) {
        *err = fmt_sprintf_v(a, "parsing BURROW_TEST_FUZZ_WORKER_HANDLES=%s", v);
        return false;
    }
    s->in = hs[0];
    s->out = hs[1];
    memfd = hs[2];
#else
    s->in = 3;
    s->out = 4;
    memfd = 5;
#endif
    PalStat st;
    PalErrno e = PAL_OK;
    if (!pal_fstat(memfd, &st, &e)) {
        *err = fmt_sprintf_v(a, "fstat: %s", pal_errno_string(e));
        return false;
    }
    s->mem = (FuzzMem){.fd = memfd, .size = st.size};
    if (st.size < (int64_t)sizeof(FuzzMemHeader)) {
        *err = BURROW_S("shared memory too small");
        return false;
    }
    s->mem.region = pal_mmap(memfd, 0, st.size, PAL_PROT_READ | PAL_PROT_WRITE, &e);
    if (s->mem.region == NULL) {
        *err = fmt_sprintf_v(a, "mmap: %s", pal_errno_string(e));
        return false;
    }
    return true;
}

bool burrow__fuzz_worker(burrow__FuzzRun fn, void *env, Alloc *a, Str *err) {
    FuzzServer s = {0};
    s.run = fn;
    s.env = env;
    if (!fuzz_worker_comm(&s, a, err))
        return false;
    burrow__fuzz_mutator_init(&s.m);
    burrow__note_init(&s.wd_quit);
    bool wd = burrow__thread_start(&s.wd, fuzz_watchdog, &s, 0);

    bool ok = true;
    FuzzBuf resp = {0};
    for (;;) {
        FuzzReader r = {.fd = s.in, .a = a};
        uint8_t kind = fuzz_get_u8(&r);
        if (r.failed) {
            if (!r.eof) {
                *err = fuzz_reader_error(&r);
                ok = false;
            }
            break;
        }
        resp.len = 0;
        switch (kind) {
        case FUZZ_CALL_PING:
            fuzz_put_u8(&resp, 0);
            break;
        case FUZZ_CALL_FUZZ:
            fuzz_serve_fuzz(&s, &r, &resp);
            break;
        case FUZZ_CALL_MINIMIZE:
            fuzz_serve_minimize(&s, &r, &resp);
            break;
        default:
            *err = BURROW_S("no arguments provided for any call");
            ok = false;
            break;
        }
        if (!ok)
            break;
        if (r.failed) {
            *err = fuzz_reader_error(&r);
            ok = false;
            break;
        }
        PalErrno e = PAL_OK;
        if (!fuzz_write_all(s.out, resp.p, resp.len, &e)) {
            *err = fmt_sprintf_v(a, "write: %s", pal_errno_string(e));
            ok = false;
            break;
        }
    }
    fuzz_buf_free(&resp);
    if (wd) {
        burrow__note_wake(&s.wd_quit);
        burrow__thread_join(&s.wd);
    }
    burrow__note_free(&s.wd_quit);
    burrow__fuzz_mutator_free(&s.m);
    pal_munmap(s.mem.region, s.mem.size, NULL);
    return ok;
}

/* ------------------------------------------------------- coordinator side */

/* CorpusEntry. The strings are on the heap and belong to whoever holds the
 * entry, except in the queues, which hold views of entries that live as long
 * as the coordinator does. */
typedef struct FuzzCorpusEntry {
    Str path;
    Str parent;
    Str data;
    int64_t generation;
    bool is_seed;
} FuzzCorpusEntry;

static FuzzCorpusEntry fuzz_centry_copy(FuzzCorpusEntry e) {
    return (FuzzCorpusEntry){fuzz_str_dup(e.path), fuzz_str_dup(e.parent),
                             fuzz_str_dup(e.data), e.generation, e.is_seed};
}

static void fuzz_centry_free(FuzzCorpusEntry *e) {
    fuzz_str_free(e->path);
    fuzz_str_free(e->parent);
    fuzz_str_free(e->data);
    *e = (FuzzCorpusEntry){0};
}

/* fuzzResult. Owns its entry and its message. */
typedef struct FuzzResult {
    FuzzCorpusEntry entry;
    Str crasher_msg;
    bool can_minimize;
    int64_t limit;
    int64_t count;
    int64_t total_duration;
    struct FuzzResult *next;
} FuzzResult;

static void fuzz_result_free(FuzzResult *r) {
    if (r == NULL)
        return;
    fuzz_centry_free(&r->entry);
    fuzz_str_free(r->crasher_msg);
    fuzz_free(r, sizeof *r, _Alignof(FuzzResult));
}

/* fuzzInput and fuzzMinimizeInput, which borrow their strings. */
typedef struct FuzzInput {
    FuzzCorpusEntry entry;
    int64_t timeout;
    int64_t limit;
    bool warmup;
} FuzzInput;

typedef struct FuzzMinInput {
    FuzzCorpusEntry entry;
    Str crasher_msg;
    int64_t timeout;
    int64_t limit;
} FuzzMinInput;

/* workerFuzzDuration and workerTimeoutDuration. */
#define FUZZ_WORKER_FUZZ_NS ((int64_t)100 * 1000000)
#define FUZZ_WORKER_TIMEOUT_NS ((int64_t)1000000000)

/* workerExitCode: what a worker exits with when F.Fuzz was used wrongly. */
#define FUZZ_WORKER_EXIT_CODE 70

enum { FUZZ_JOB_NONE, FUZZ_JOB_FUZZ, FUZZ_JOB_MINIMIZE };

struct FuzzCoord;

typedef struct FuzzWorker {
    struct FuzzCoord *c;
    burrow__Thread thread;
    bool started;
    /* Woken when the coordinator hands this worker something or wants it to
     * stop. */
    burrow__Note wake;

    /* Under c->mu. */
    int job;
    FuzzInput input;
    FuzzMinInput min_input;
    bool idle;
    bool in_call;
    bool done;
    bool collected;
    Str err;
    /* The process, or -1 when there is none running or it has been reaped. */
    int64_t pid;
    bool interrupted;
    /* What pal_wait said about the last process, for waitErr. */
    int32_t status;
    bool main_int;
    bool main_kill;

    /* The worker thread's own. */
    bool running;
    int64_t in_w;
    int64_t out_r;
    FuzzMem mem;
    burrow__FuzzMutator m;
} FuzzWorker;

typedef struct FuzzCoord {
    const burrow__FuzzOpts *opts;
    int parallel;
    Str argv0;
    burrow__Lock mu;
    burrow__Note ev;
    bool cancel;
    int64_t start_time;

    FuzzCorpusEntry *seeds;
    Int nseeds;
    FuzzCorpusEntry *corpus;
    Byte (*hashes)[32];
    Int ncorpus;
    Int corpus_cap;

    FuzzCorpusEntry *queue;
    Int qhead;
    Int qlen;
    Int qcap;
    bool min_queued;
    FuzzMinInput min_item;

    FuzzResult *results;
    FuzzResult *results_tail;

    int64_t count;
    int64_t count_waiting;
    int64_t count_last_log;
    int64_t time_last_log;
    int64_t duration;
    Int warmup_count;
    Int warmup_left;
    bool minimization_allowed;
    FuzzResult *crash_minimizing;

    FuzzWorker *workers;
} FuzzCoord;

static void fuzz_log(Str s) {
    fwrite(s.p, 1, (size_t)s.len, stderr);
    fflush(stderr);
}

/* ProcessState.String for a pal_wait status. */
static Str fuzz_wait_string(int32_t st, Alloc *a) {
    if (st >= 0)
        return fmt_sprintf_v(a, "exit status %d", (Int)st);
    int32_t neg = -st;
    bool core = neg >= 256;
    int32_t sig = core ? neg - 256 : neg;
    const char *name = NULL;
    switch (sig) {
    case PAL_SIGHUP:
        name = "hangup";
        break;
    case PAL_SIGINT:
        name = "interrupt";
        break;
    case PAL_SIGQUIT:
        name = "quit";
        break;
    case PAL_SIGILL:
        name = "illegal instruction";
        break;
#if defined(BURROW_OS_DARWIN)
    case PAL_SIGTRAP:
        name = "trace/BPT trap";
        break;
    case PAL_SIGABRT:
        name = "abort trap";
        break;
#else
    case PAL_SIGTRAP:
        name = "trace/breakpoint trap";
        break;
    case PAL_SIGABRT:
        name = "aborted";
        break;
#endif
    case PAL_SIGBUS:
        name = "bus error";
        break;
    case PAL_SIGFPE:
        name = "floating point exception";
        break;
    case PAL_SIGKILL:
        name = "killed";
        break;
    case PAL_SIGUSR1:
        name = "user defined signal 1";
        break;
    case PAL_SIGSEGV:
        name = "segmentation fault";
        break;
    case PAL_SIGUSR2:
        name = "user defined signal 2";
        break;
    case PAL_SIGPIPE:
        name = "broken pipe";
        break;
    case PAL_SIGALRM:
        name = "alarm clock";
        break;
    case PAL_SIGTERM:
        name = "terminated";
        break;
    default:
        break;
    }
    Str s = name != NULL ? fmt_sprintf_v(a, "signal: %s", name)
                         : fmt_sprintf_v(a, "signal: signal %d", (Int)sig);
    if (core)
        s = fmt_sprintf_v(a, "%s (core dumped)", s);
    return s;
}

/* The signal a status says ended the process, or 0. */
static int32_t fuzz_wait_signal(int32_t st) {
    if (st >= 0)
        return 0;
    return -st >= 256 ? -st - 256 : -st;
}

/* isCrashSignal. */
static bool fuzz_crash_signal(int32_t sig) {
    return sig == PAL_SIGILL || sig == PAL_SIGTRAP || sig == PAL_SIGABRT ||
           sig == PAL_SIGBUS || sig == PAL_SIGFPE || sig == PAL_SIGSEGV ||
           sig == PAL_SIGPIPE;
}

/* Reaps the worker's process if it has ended, under c->mu. Answers whether
 * there is no process left. */
static bool fuzz_reap_locked(FuzzWorker *w) {
    if (w->pid < 0)
        return true;
    int32_t st = 0;
    PalErrno e = PAL_OK;
    int64_t r = pal_wait(w->pid, &st, PAL_WAIT_NOHANG | PAL_WAIT_SIGNAL, &e);
    if (r == 0)
        return false;
    if (r < 0)
        st = -PAL_SIGKILL;
    w->status = st;
    w->pid = -1;
    return true;
}

static void fuzz_client_close(FuzzWorker *w) {
    if (w->in_w != PAL_INVALID_HANDLE)
        pal_close(w->in_w, NULL);
    if (w->out_r != PAL_INVALID_HANDLE)
        pal_close(w->out_r, NULL);
    w->in_w = PAL_INVALID_HANDLE;
    w->out_r = PAL_INVALID_HANDLE;
}

/* worker.stop: hangs up, which a worker takes as its cue to exit, and then
 * waits, interrupting and then killing it if it takes more than a second
 * each time. Answers the status, 0 for a clean exit. */
static int32_t fuzz_worker_stop(FuzzWorker *w) {
    FuzzCoord *c = w->c;
    fuzz_client_close(w);
    w->running = false;
#if defined(BURROW_OS_WINDOWS)
    int32_t sig = PAL_SIGKILL;
#else
    int32_t sig = PAL_SIGINT;
#endif
    int stage = 0;
    int64_t next = pal_clock_monotonic() + FUZZ_WORKER_TIMEOUT_NS;
    for (;;) {
        burrow__lock(&c->mu);
        bool gone = fuzz_reap_locked(w);
        int32_t st = w->status;
        if (!gone && pal_clock_monotonic() >= next) {
            w->interrupted = true;
            if (stage == 0) {
                pal_kill(w->pid, sig, NULL);
                stage = sig == PAL_SIGKILL ? 2 : 1;
            } else if (stage == 1) {
                pal_kill(w->pid, PAL_SIGKILL, NULL);
                stage = 2;
            } else {
                fuzz_log(BURROW_S("waiting for fuzzing process to terminate...\n"));
            }
            next = pal_clock_monotonic() + FUZZ_WORKER_TIMEOUT_NS;
        }
        burrow__unlock(&c->mu);
        if (gone)
            return st;
        pal_nanosleep(5000000);
    }
}

/* worker.start: the pipes, the process, and a fresh client. */
static bool fuzz_worker_start(FuzzWorker *w, Alloc *a, Str *err) {
    FuzzCoord *c = w->c;
    const burrow__FuzzOpts *o = c->opts;
    PalErrno e = PAL_OK;
    int64_t in[2];
    int64_t out[2];
    if (!pal_pipe(in, 0, &e)) {
        *err = fmt_sprintf_v(a, "pipe: %s", pal_errno_string(e));
        return false;
    }
    if (!pal_pipe(out, 0, &e)) {
        pal_close(in[0], NULL);
        pal_close(in[1], NULL);
        *err = fmt_sprintf_v(a, "pipe: %s", pal_errno_string(e));
        return false;
    }

    int nargs = o->argc + 2;
    const char **argv = (const char **)fuzz_must_alloc((size_t)nargs * sizeof(char *),
                                                       _Alignof(char *));
    char *path = (char *)fuzz_must_alloc((size_t)c->argv0.len + 1, 1);
    memcpy(path, c->argv0.p, (size_t)c->argv0.len);
    path[c->argv0.len] = '\0';
    argv[0] = path;
    argv[1] = "-test.fuzzworker";
    for (int i = 1; i < o->argc; i++)
        argv[i + 1] = o->argv[i];
    argv[nargs - 1] = NULL;

    const char *const *envp = NULL;
    const char **env = NULL;
    Int nenv = 0;
#if defined(BURROW_OS_WINDOWS)
    char handles[64];
    snprintf(handles, sizeof handles, "BURROW_TEST_FUZZ_WORKER_HANDLES=%llx,%llx,%llx",
             (unsigned long long)in[0], (unsigned long long)out[1],
             (unsigned long long)w->mem.fd);
    const char *const *cur = pal_environ();
    while (cur != NULL && cur[nenv] != NULL)
        nenv++;
    env = (const char **)fuzz_must_alloc((size_t)(nenv + 2) * sizeof(char *),
                                         _Alignof(char *));
    for (Int i = 0; i < nenv; i++)
        env[i] = cur[i];
    env[nenv] = handles;
    env[nenv + 1] = NULL;
    envp = env;
#endif
    /* exec.Cmd with nothing set for its standard files, which gives the
     * worker the null device for all three. */
#if defined(BURROW_OS_WINDOWS)
    const char *null_dev = "NUL";
#else
    const char *null_dev = "/dev/null";
#endif
    int64_t null_fd = pal_open(null_dev, PAL_O_RDWR, 0, &e);
    int64_t pid = -1;
    if (null_fd != PAL_INVALID_HANDLE) {
        int64_t fds[6] = {null_fd, null_fd, null_fd, in[0], out[1], w->mem.fd};
        PalSpawn req = {
            .path = path, .argv = argv, .envp = envp, .fds = fds, .nfds = 6};
        pid = pal_spawn(&req, &e);
        pal_close(null_fd, NULL);
    }
    pal_close(in[0], NULL);
    pal_close(out[1], NULL);
    if (env != NULL)
        fuzz_free((void *)env, (size_t)(nenv + 2) * sizeof(char *), _Alignof(char *));
    fuzz_free((void *)argv, (size_t)nargs * sizeof(char *), _Alignof(char *));
    if (pid < 0) {
        *err = fmt_sprintf_v(a, "fork/exec %s: %s", path, pal_errno_string(e));
        fuzz_free(path, (size_t)c->argv0.len + 1, 1);
        pal_close(in[1], NULL);
        pal_close(out[0], NULL);
        return false;
    }
    fuzz_free(path, (size_t)c->argv0.len + 1, 1);
    burrow__lock(&c->mu);
    w->pid = pid;
    w->status = 0;
    w->interrupted = false;
    w->main_int = false;
    w->main_kill = false;
    burrow__unlock(&c->mu);
    w->in_w = in[1];
    w->out_r = out[0];
    w->running = true;
    burrow__fuzz_mutator_free(&w->m);
    burrow__fuzz_mutator_init(&w->m);
    return true;
}

/* callLocked: sends the request in req and starts reading the answer. */
static bool fuzz_call(FuzzWorker *w, FuzzBuf *req, FuzzReader *r, Alloc *a, Str *err) {
    PalErrno e = PAL_OK;
    *r = (FuzzReader){.fd = w->out_r, .a = a};
    burrow__lock(&w->c->mu);
    w->in_call = true;
    burrow__unlock(&w->c->mu);
    if (!fuzz_write_all(w->in_w, req->p, req->len, &e)) {
        *err = fmt_sprintf_v(a, "write: %s", pal_errno_string(e));
        r->failed = true;
        return false;
    }
    return true;
}

static void fuzz_call_done(FuzzWorker *w) {
    burrow__lock(&w->c->mu);
    w->in_call = false;
    burrow__unlock(&w->c->mu);
}

/* workerClient.ping. */
static bool fuzz_client_ping(FuzzWorker *w, Alloc *a, Str *err) {
    FuzzBuf req = {0};
    fuzz_put_u8(&req, FUZZ_CALL_PING);
    FuzzReader r;
    bool ok = fuzz_call(w, &req, &r, a, err);
    fuzz_buf_free(&req);
    if (ok) {
        fuzz_get_u8(&r);
        if (r.failed) {
            *err = fuzz_reader_error(&r);
            ok = false;
        }
    }
    fuzz_call_done(w);
    return ok;
}

/* The name of an entry: the first four bytes of its data's hash, in hex. */
static Str fuzz_entry_name(Str data) {
    char hex[9];
    return fuzz_str_dup(fuzz_sum_hex(data, 4, hex));
}

typedef struct FuzzResp {
    int64_t total;
    int64_t count;
    Str err;
} FuzzResp;

/* workerClient.fuzz. Answers false when the call failed, with *err, and
 * *internal set when that is not the input's fault. */
static bool fuzz_client_fuzz(FuzzWorker *w, const FuzzInput *in, FuzzCorpusEntry *out,
                             FuzzResp *resp, bool *internal, Alloc *a, Str *err) {
    *out = (FuzzCorpusEntry){0};
    *resp = (FuzzResp){0};
    *internal = false;
    FuzzMemHeader *h = fuzz_hdr(&w->mem);
    h->count = 0;
    fuzz_mem_set_value(&w->mem, in->entry.data);

    FuzzBuf req = {0};
    fuzz_put_u8(&req, FUZZ_CALL_FUZZ);
    fuzz_put_i64(&req, in->timeout);
    fuzz_put_i64(&req, in->limit);
    fuzz_put_u8(&req, in->warmup ? 1 : 0);
    FuzzReader r;
    Str call_err = {0};
    bool call_ok = fuzz_call(w, &req, &r, a, &call_err);
    fuzz_buf_free(&req);
    Str internal_err = {0};
    if (call_ok) {
        resp->total = fuzz_get_i64(&r);
        fuzz_get_i64(&r);
        fuzz_get_i64(&r);
        resp->err = fuzz_get_str(&r);
        internal_err = fuzz_get_str(&r);
        if (r.failed) {
            call_ok = false;
            call_err = fuzz_reader_error(&r);
            *resp = (FuzzResp){0};
            internal_err = (Str){0};
        }
    }
    fuzz_call_done(w);
    if (internal_err.len > 0) {
        *internal = true;
        *err = internal_err;
        *resp = (FuzzResp){0};
        return false;
    }
    resp->count = h->count;
    if (!str_eq(in->entry.data, fuzz_mem_value(&w->mem))) {
        *internal = true;
        *err = BURROW_S("workerServer.fuzz modified input");
        *resp = (FuzzResp){0};
        return false;
    }
    if (!call_ok || resp->err.len > 0) {
        Any *vals;
        Int n;
        Str why;
        if (!burrow__testing_corpus_unmarshal(a, in->entry.data, &vals, &n, &why)) {
            *internal = true;
            *err =
                fmt_sprintf_v(a, "unmarshaling fuzz input value after call: %s", why);
            *resp = (FuzzResp){0};
            return false;
        }
        w->m.r.state = h->rand_state;
        w->m.r.inc = h->rand_inc;
        Any *views = (Any *)fuzz_must_alloc((size_t)n * sizeof(Any), _Alignof(Any));
        FuzzCell *cells = (FuzzCell *)fuzz_must_alloc((size_t)n * sizeof(FuzzCell),
                                                      _Alignof(FuzzCell));
        fuzz_views_reset(views, cells, vals, n);
        if (!in->warmup) {
            int64_t k = ((resp->count - 1) % FUZZ_CHAINED) + 1;
            for (int64_t i = 0; i < k; i++)
                burrow__fuzz_mutate(&w->m, views, n, fuzz_mem_cap(&w->mem));
        }
        Str data = burrow__testing_corpus_marshal(a, views, n);
        fuzz_free(cells, (size_t)n * sizeof(FuzzCell), _Alignof(FuzzCell));
        fuzz_free(views, (size_t)n * sizeof(Any), _Alignof(Any));
        burrow__testing_values_free(vals, n);
        out->data = fuzz_str_dup(data);
        out->path = fuzz_entry_name(data);
        out->parent = fuzz_str_dup(in->entry.path);
        out->generation = in->entry.generation + 1;
        out->is_seed = in->warmup && in->entry.is_seed;
    }
    if (!call_ok)
        *err = call_err;
    return call_ok;
}

typedef struct FuzzMinResp {
    int64_t count;
    int64_t duration;
    Str err;
} FuzzMinResp;

/* workerClient.minimize. *out is always set, and belongs to the caller. */
static bool fuzz_client_minimize(FuzzWorker *w, const FuzzMinInput *in,
                                 FuzzCorpusEntry *out, FuzzMinResp *resp, Alloc *a,
                                 Str *err) {
    *out = (FuzzCorpusEntry){0};
    *resp = (FuzzMinResp){0};
    FuzzMemHeader *h = fuzz_hdr(&w->mem);
    h->count = 0;
    fuzz_mem_set_value(&w->mem, in->entry.data);
    Any *vals;
    Int n;
    Str why;
    if (!burrow__testing_corpus_unmarshal(a, in->entry.data, &vals, &n, &why)) {
        *err = fmt_sprintf_v(a, "workerClient.minimize unmarshaling provided value: %s",
                             why);
        return false;
    }
    *out = fuzz_centry_copy(in->entry);
    int64_t timeout = in->timeout;
    int64_t limit = in->limit;
    bool ok = true;
    for (Int i = 0; i < n; i++) {
        if (vals[i].t != TYPE_STRING && vals[i].t != TYPE_BYTES)
            continue;
        FuzzBuf req = {0};
        fuzz_put_u8(&req, FUZZ_CALL_MINIMIZE);
        fuzz_put_i64(&req, timeout);
        fuzz_put_i64(&req, limit);
        fuzz_put_i64(&req, (int64_t)i);
        FuzzReader r;
        Str call_err = {0};
        bool call_ok = fuzz_call(w, &req, &r, a, &call_err);
        fuzz_buf_free(&req);
        bool wrote = false;
        if (call_ok) {
            wrote = fuzz_get_u8(&r) != 0;
            resp->err = fuzz_get_str(&r);
            resp->duration = fuzz_get_i64(&r);
            if (r.failed) {
                call_ok = false;
                call_err = fuzz_reader_error(&r);
            }
        }
        fuzz_call_done(w);
        if (!call_ok) {
            ok = false;
            *err = call_err;
            if (!h->raw_in_mem) {
                fuzz_centry_free(out);
                *out = fuzz_centry_copy(in->entry);
                *resp = (FuzzMinResp){0};
                burrow__testing_values_free(vals, n);
                return false;
            }
            /* The worker died on a candidate, which is in the memory raw. */
            Str raw = fuzz_mem_value(&w->mem);
            const Type *t = vals[i].t;
            burrow__testing_value_free(vals[i]);
            if (t == TYPE_STRING) {
                vals[i] = burrow__testing_value_copy(t, &raw);
            } else {
                Slice s = {(void *)(uintptr_t)raw.p, raw.len, raw.len, t->elem};
                vals[i] = burrow__testing_value_copy(t, &s);
            }
            fuzz_str_free(out->data);
            out->data = fuzz_str_dup(burrow__testing_corpus_marshal(a, vals, n));
            break;
        }
        if (wrote) {
            fuzz_str_free(out->data);
            out->data = fuzz_str_dup(fuzz_mem_value(&w->mem));
            burrow__testing_values_free(vals, n);
            if (!burrow__testing_corpus_unmarshal(a, out->data, &vals, &n, &why)) {
                fuzz_centry_free(out);
                *resp = (FuzzMinResp){0};
                *err = fmt_sprintf_v(
                    a, "workerClient.minimize unmarshaling minimized value: %s", why);
                return false;
            }
        }
        if (timeout != 0) {
            timeout -= resp->duration;
            if (timeout <= 0)
                break;
        }
        if (limit != 0) {
            limit -= h->count;
            if (limit <= 0)
                break;
        }
    }
    burrow__testing_values_free(vals, n);
    resp->count = h->count;
    fuzz_str_free(out->path);
    out->path = fuzz_entry_name(out->data);
    return ok;
}

static void fuzz_post(FuzzWorker *w, FuzzResult *res) {
    FuzzCoord *c = w->c;
    burrow__lock(&c->mu);
    res->next = NULL;
    if (c->results_tail != NULL)
        c->results_tail->next = res;
    else
        c->results = res;
    c->results_tail = res;
    burrow__unlock(&c->mu);
    burrow__note_wake(&c->ev);
}

static FuzzResult *fuzz_result_new(void) {
    FuzzResult *r =
        (FuzzResult *)fuzz_must_alloc(sizeof(FuzzResult), _Alignof(FuzzResult));
    *r = (FuzzResult){0};
    return r;
}

static bool fuzz_cancelled(FuzzCoord *c) {
    burrow__lock(&c->mu);
    bool v = c->cancel;
    burrow__unlock(&c->mu);
    return v;
}

/* worker.minimize. Always has a result to send back. */
static FuzzResult *fuzz_worker_minimize(FuzzWorker *w, const FuzzMinInput *in,
                                        Alloc *a) {
    FuzzResult *res = fuzz_result_new();
    res->limit = in->limit;
    FuzzCorpusEntry out;
    FuzzMinResp resp;
    Str err = {0};
    if (!fuzz_client_minimize(w, in, &out, &resp, a, &err)) {
        int32_t st = fuzz_worker_stop(w);
        burrow__lock(&w->c->mu);
        bool interrupted = w->interrupted || w->c->cancel;
        burrow__unlock(&w->c->mu);
        if (interrupted || fuzz_wait_signal(st) == PAL_SIGINT) {
            fuzz_centry_free(&out);
            res->entry = fuzz_centry_copy(in->entry);
            res->crasher_msg = fuzz_str_dup(in->crasher_msg);
            return res;
        }
        res->entry = out;
        res->crasher_msg = fuzz_str_dup(fmt_sprintf_v(
            a, "fuzzing process hung or terminated unexpectedly while minimizing: %s",
            err));
        res->count = resp.count;
        res->total_duration = resp.duration;
        return res;
    }
    if (in->crasher_msg.len > 0 && resp.err.len == 0) {
        /* The worker's minimize failing is Go's error return, and the input goes
         * back as it came. */
        fuzz_centry_free(&out);
        res->entry = fuzz_centry_copy(in->entry);
        res->crasher_msg = fuzz_str_dup(in->crasher_msg);
        return res;
    }
    res->entry = out;
    res->crasher_msg = fuzz_str_dup(resp.err);
    res->count = resp.count;
    res->total_duration = resp.duration;
    return res;
}

enum { FUZZ_EV_CANCEL, FUZZ_EV_JOB, FUZZ_EV_TERM };

/* worker.coordinate. Answers the error the worker stopped with, or an empty
 * string. */
static Str fuzz_worker_coordinate(FuzzWorker *w, Arena *ar) {
    FuzzCoord *c = w->c;
    Alloc *a = arena_allocator(ar);
    for (;;) {
        if (!w->running) {
            if (fuzz_cancelled(c))
                return (Str){0};
            Str err = {0};
            if (!fuzz_worker_start(w, a, &err))
                return err;
            if (!fuzz_client_ping(w, a, &err)) {
                fuzz_worker_stop(w);
                if (fuzz_cancelled(c))
                    return (Str){0};
                return fmt_sprintf_v(
                    a, "fuzzing process terminated without fuzzing: %s", err);
            }
        }

        /* What one call leaves in the arena is gone by the next, except for an
         * error, which ends the loop. */
        ArenaMark mark = arena_mark(ar);
        int ev;
        int job = FUZZ_JOB_NONE;
        FuzzInput input = {0};
        FuzzMinInput min_input = {0};
        burrow__lock(&c->mu);
        for (;;) {
            if (c->cancel) {
                ev = FUZZ_EV_CANCEL;
                break;
            }
            if (w->job != FUZZ_JOB_NONE) {
                ev = FUZZ_EV_JOB;
                job = w->job;
                input = w->input;
                min_input = w->min_input;
                w->job = FUZZ_JOB_NONE;
                break;
            }
            if (fuzz_reap_locked(w)) {
                ev = FUZZ_EV_TERM;
                break;
            }
            bool was_idle = w->idle;
            w->idle = true;
            burrow__note_clear(&w->wake);
            burrow__unlock(&c->mu);
            if (!was_idle)
                burrow__note_wake(&c->ev);
            burrow__note_sleep_timeout(&w->wake, 20000000);
            burrow__lock(&c->mu);
        }
        w->idle = false;
        burrow__unlock(&c->mu);

        if (ev == FUZZ_EV_CANCEL) {
            int32_t st = fuzz_worker_stop(w);
            burrow__lock(&c->mu);
            bool interrupted = w->interrupted;
            burrow__unlock(&c->mu);
            if (st != 0 && !interrupted && fuzz_wait_signal(st) != PAL_SIGINT)
                return fuzz_wait_string(st, a);
            return (Str){0};
        }
        if (ev == FUZZ_EV_TERM) {
            int32_t st = fuzz_worker_stop(w);
            if (st == 0 || fuzz_wait_signal(st) == PAL_SIGINT)
                return (Str){0};
            if (st == FUZZ_WORKER_EXIT_CODE)
                return fmt_sprintf_v(a,
                                     "fuzzing process exited unexpectedly due to an "
                                     "internal failure: %s",
                                     fuzz_wait_string(st, a));
            return fmt_sprintf_v(a,
                                 "fuzzing process hung or terminated unexpectedly: %s",
                                 fuzz_wait_string(st, a));
        }

        if (job == FUZZ_JOB_FUZZ) {
            FuzzCorpusEntry out;
            FuzzResp resp;
            bool internal;
            Str err = {0};
            bool can_minimize = true;
            Str crasher = {0};
            if (!fuzz_client_fuzz(w, &input, &out, &resp, &internal, a, &err)) {
                int32_t st = fuzz_worker_stop(w);
                burrow__lock(&c->mu);
                bool cancel = c->cancel;
                bool interrupted = w->interrupted;
                burrow__unlock(&c->mu);
                if (cancel) {
                    fuzz_centry_free(&out);
                    return (Str){0};
                }
                if (interrupted) {
                    fuzz_centry_free(&out);
                    return fmt_sprintf_v(a, "communicating with fuzzing process: %s",
                                         err);
                }
                int32_t sig = fuzz_wait_signal(st);
                if (sig != 0 && !fuzz_crash_signal(sig)) {
                    fuzz_centry_free(&out);
                    return fmt_sprintf_v(
                        a,
                        "fuzzing process terminated by unexpected signal; "
                        "no crash will be recorded: %s",
                        fuzz_wait_string(st, a));
                }
                if (internal) {
                    fuzz_centry_free(&out);
                    return err;
                }
                crasher = fmt_sprintf_v(
                    a, "fuzzing process hung or terminated unexpectedly: %s",
                    st == 0 ? BURROW_S("<nil>") : fuzz_wait_string(st, a));
                can_minimize = false;
            } else {
                crasher = resp.err;
            }
            FuzzResult *res = fuzz_result_new();
            res->limit = input.limit;
            res->count = resp.count;
            res->total_duration = resp.total;
            res->entry = out;
            res->crasher_msg = fuzz_str_dup(crasher);
            res->can_minimize = can_minimize;
            fuzz_post(w, res);
        } else {
            fuzz_post(w, fuzz_worker_minimize(w, &min_input, a));
        }
        arena_release(ar, mark);
    }
}

static void fuzz_worker_main(void *arg) {
    FuzzWorker *w = (FuzzWorker *)arg;
    FuzzCoord *c = w->c;
    /* A write to a worker that has died is an error to handle, not a reason for
     * the whole test binary to go. */
    pal_signal_mask(PAL_SIGPIPE, true, NULL);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str err = fuzz_worker_coordinate(w, &ar);
    if (fuzz_cancelled(c))
        err = (Str){0};
    if (w->running)
        fuzz_worker_stop(w);
    Str clean = {0};
    if (!fuzz_mem_close(&w->mem, a, &clean) && err.len == 0)
        err = clean;
    burrow__lock(&c->mu);
    w->err = fuzz_str_dup(err);
    w->done = true;
    burrow__unlock(&c->mu);
    arena_free(&ar);
    burrow__note_wake(&c->ev);
}

/* ------------------------------------------------------------ coordinator */

/* A path under dir, NUL terminated on the heap, for pal. */
static char *fuzz_cpath(Str s, size_t *size) {
    *size = (size_t)s.len + 1;
    char *p = (char *)fuzz_must_alloc(*size, 1);
    memcpy(p, s.p, (size_t)s.len);
    p[s.len] = '\0';
    return p;
}

static bool fuzz_is_sep(Byte ch) {
#if defined(BURROW_OS_WINDOWS)
    return ch == '/' || ch == '\\';
#else
    return ch == '/';
#endif
}

/* os.MkdirAll with mode 0777. */
static bool fuzz_mkdir_all(Str dir, Alloc *a, Str *err) {
    for (Int i = 1; i <= dir.len; i++) {
        if (i < dir.len && !fuzz_is_sep(dir.p[i]))
            continue;
        if (fuzz_is_sep(dir.p[i - 1]))
            continue;
#if defined(BURROW_OS_WINDOWS)
        if (i == 2 && dir.p[1] == ':')
            continue;
#endif
        Str part = str_from_bytes(dir.p, i);
        size_t size;
        char *p = fuzz_cpath(part, &size);
        PalErrno e = PAL_OK;
        bool ok = pal_mkdir(p, 0777, &e) || e == PAL_EEXIST;
        fuzz_free(p, size, 1);
        if (!ok) {
            *err = fmt_sprintf_v(a, "mkdir %s: %s", part, pal_errno_string(e));
            return false;
        }
    }
    return true;
}

/* writeToCorpus: the entry goes to dir under the first sixteen hex digits of
 * its hash, and its path becomes that file's. */
static bool fuzz_write_to_corpus(FuzzCorpusEntry *e, Str dir, Alloc *a, Str *err) {
    char hex[65];
    Str sum = fuzz_sum_hex(e->data, 32, hex);
    sum.len = 16;
    static const char sep[] = {FUZZ_SEP_CH, '\0'};
    Str path = fmt_sprintf_v(a, "%s%s%s", dir, sep, sum);
    fuzz_str_free(e->path);
    e->path = fuzz_str_dup(path);
    if (!fuzz_mkdir_all(dir, a, err))
        return false;
    size_t size;
    char *p = fuzz_cpath(path, &size);
    PalErrno pe = PAL_OK;
    bool ok = false;
    int64_t fd = pal_open(p, PAL_O_WRONLY | PAL_O_CREATE | PAL_O_TRUNC, 0666, &pe);
    if (fd == PAL_INVALID_HANDLE) {
        *err = fmt_sprintf_v(a, "open %s: %s", path, pal_errno_string(pe));
    } else {
        ok = fuzz_write_all(fd, e->data.p, e->data.len, &pe);
        if (!ok)
            *err = fmt_sprintf_v(a, "write %s: %s", path, pal_errno_string(pe));
        if (!pal_close(fd, &pe) && ok) {
            *err = fmt_sprintf_v(a, "close %s: %s", path, pal_errno_string(pe));
            ok = false;
        }
        if (!ok)
            pal_unlink(p, NULL);
    }
    fuzz_free(p, size, 1);
    return ok;
}

/* addCorpusEntries(false, e): e is copied in unless its data is already
 * there. */
static void fuzz_add_corpus(FuzzCoord *c, FuzzCorpusEntry e) {
    Byte h[32];
    burrow__fuzz_sha256(e.data.p, e.data.len, h);
    for (Int i = 0; i < c->ncorpus; i++)
        if (memcmp(c->hashes[i], h, 32) == 0)
            return;
    if (c->ncorpus == c->corpus_cap) {
        Int ncap = c->corpus_cap == 0 ? 8 : c->corpus_cap * 2;
        c->corpus = (FuzzCorpusEntry *)fuzz_must_realloc(
            c->corpus, (size_t)c->corpus_cap * sizeof(FuzzCorpusEntry),
            (size_t)ncap * sizeof(FuzzCorpusEntry), _Alignof(FuzzCorpusEntry));
        c->hashes = (Byte(*)[32])fuzz_must_realloc(
            c->hashes, (size_t)c->corpus_cap * 32, (size_t)ncap * 32, 1);
        c->corpus_cap = ncap;
    }
    c->corpus[c->ncorpus] = fuzz_centry_copy(e);
    memcpy(c->hashes[c->ncorpus], h, 32);
    c->ncorpus++;
}

static void fuzz_enqueue(FuzzCoord *c, FuzzCorpusEntry e) {
    if (c->qhead + c->qlen == c->qcap) {
        if (c->qhead > 0) {
            memmove(c->queue, c->queue + c->qhead,
                    (size_t)c->qlen * sizeof(FuzzCorpusEntry));
            c->qhead = 0;
        } else {
            Int ncap = c->qcap == 0 ? 8 : c->qcap * 2;
            c->queue = (FuzzCorpusEntry *)fuzz_must_realloc(
                c->queue, (size_t)c->qcap * sizeof(FuzzCorpusEntry),
                (size_t)ncap * sizeof(FuzzCorpusEntry), _Alignof(FuzzCorpusEntry));
            c->qcap = ncap;
        }
    }
    c->queue[c->qhead + c->qlen++] = e;
}

static int64_t fuzz_elapsed(FuzzCoord *c) {
    int64_t d = pal_clock_monotonic() - c->start_time;
    return (d + 500000000) / 1000000000 * 1000000000;
}

static bool fuzz_warmup_run(FuzzCoord *c) {
    return c->warmup_left > 0;
}

/* logStats. */
static void fuzz_log_stats(FuzzCoord *c) {
    Alloc *a = error_allocator();
    int64_t now = pal_clock_monotonic();
    char buf[32];
    Str el = burrow__testing_duration_string(fuzz_elapsed(c), buf);
    if (fuzz_warmup_run(c)) {
        fuzz_log(fmt_sprintf_v(
            a, "fuzz: elapsed: %s, testing seed corpus: %d/%d completed\n", el,
            c->warmup_count - c->warmup_left, c->warmup_count));
    } else if (c->crash_minimizing != NULL) {
        fuzz_log(fmt_sprintf_v(a, "fuzz: elapsed: %s, minimizing\n", el));
    } else {
        double secs = (double)(now - c->time_last_log) / 1e9;
        double rate = (double)(c->count - c->count_last_log) / secs;
        fuzz_log(fmt_sprintf_v(a, "fuzz: elapsed: %s, execs: %d (%.0f/sec)\n", el,
                               c->count, rate));
    }
    c->count_last_log = c->count;
    c->time_last_log = now;
}

/* peekInput. */
static bool fuzz_peek_input(FuzzCoord *c, FuzzInput *in) {
    int64_t limit = c->opts->limit;
    if (limit > 0 && c->count + c->count_waiting >= limit)
        return false;
    if (c->qlen == 0) {
        if (fuzz_warmup_run(c))
            return false;
        for (Int i = 0; i < c->ncorpus; i++)
            fuzz_enqueue(c, c->corpus[i]);
    }
    if (c->qlen == 0)
        panic_str(BURROW_S("input queue empty after refill"));
    *in = (FuzzInput){c->queue[c->qhead], FUZZ_WORKER_FUZZ_NS, 0, fuzz_warmup_run(c)};
    if (in->warmup) {
        in->limit = 1;
        return true;
    }
    if (limit > 0) {
        in->limit = limit / c->parallel;
        if (limit % c->parallel > 0)
            in->limit++;
        int64_t remaining = limit - c->count - c->count_waiting;
        if (in->limit > remaining)
            in->limit = remaining;
    }
    return true;
}

static bool fuzz_can_minimize(FuzzCoord *c) {
    int64_t limit = c->opts->limit;
    return c->minimization_allowed &&
           (limit == 0 || c->count + c->count_waiting < limit);
}

/* peekMinimizeInput. */
static bool fuzz_peek_minimize(FuzzCoord *c, FuzzMinInput *in) {
    if (!fuzz_can_minimize(c) || !c->min_queued)
        return false;
    const burrow__FuzzOpts *o = c->opts;
    *in = c->min_item;
    if (o->minimize_timeout > 0)
        in->timeout = o->minimize_timeout;
    if (o->minimize_limit > 0) {
        in->limit = o->minimize_limit;
    } else if (o->limit > 0) {
        if (in->crasher_msg.len > 0) {
            in->limit = o->limit;
        } else {
            in->limit = o->limit / c->parallel;
            if (o->limit % c->parallel > 0)
                in->limit++;
        }
    }
    if (o->limit > 0) {
        int64_t remaining = o->limit - c->count - c->count_waiting;
        if (in->limit > remaining)
            in->limit = remaining;
    }
    return true;
}

static FuzzWorker *fuzz_idle_worker(FuzzCoord *c) {
    for (int i = 0; i < c->parallel; i++) {
        FuzzWorker *w = &c->workers[i];
        if (w->started && w->idle && w->job == FUZZ_JOB_NONE && !w->done)
            return w;
    }
    return NULL;
}

/* The main loop's own state: what stop in CoordinateFuzzing closes over. */
typedef struct FuzzLoop {
    bool stopping;
    int64_t stop_at;
    bool have_err;
    Str err;
    Str crash_path;
    bool crash_written;
} FuzzLoop;

/* stop. err empty is nil. Under c->mu. */
static void fuzz_stop(FuzzCoord *c, FuzzLoop *l, Str err, Str crash_path) {
    if (err.len > 0 && !l->have_err) {
        l->have_err = true;
        l->err = fuzz_str_dup(err);
        l->crash_path = fuzz_str_dup(crash_path);
    }
    if (l->stopping)
        return;
    l->stopping = true;
    l->stop_at = pal_clock_monotonic();
    c->cancel = true;
    for (int i = 0; i < c->parallel; i++)
        if (c->workers[i].started)
            burrow__note_wake(&c->workers[i].wake);
}

/* What the main loop does with a result. Takes ownership of res. */
static void fuzz_handle_result(FuzzCoord *c, FuzzLoop *l, FuzzResult *res, Alloc *a) {
    if (l->stopping) {
        fuzz_result_free(res);
        return;
    }
    c->count += res->count;
    c->count_waiting -= res->limit;
    c->duration += res->total_duration;
    char buf[32];
    if (res->crasher_msg.len > 0) {
        if (fuzz_warmup_run(c) && res->entry.is_seed) {
            Str dir = c->opts->corpus_dir;
            Int i = dir.len;
            while (i > 0 && !fuzz_is_sep(dir.p[i - 1]))
                i--;
            Str parent = res->entry.parent;
            Int j = parent.len;
            while (j > 0 && !fuzz_is_sep(parent.p[j - 1]))
                j--;
            fuzz_log(fmt_sprintf_v(a,
                                   "failure while testing seed corpus entry: %s/%s\n",
                                   str_from_bytes(dir.p + i, dir.len - i),
                                   str_from_bytes(parent.p + j, parent.len - j)));
            fuzz_stop(c, l, res->crasher_msg, (Str){0});
            fuzz_result_free(res);
            return;
        }
        if (fuzz_can_minimize(c) && res->can_minimize) {
            if (c->crash_minimizing != NULL) {
                fuzz_result_free(res);
                return;
            }
            c->crash_minimizing = res;
            fuzz_log(fmt_sprintf_v(a, "fuzz: minimizing %d-byte failing input file\n",
                                   res->entry.data.len));
            c->min_item = (FuzzMinInput){res->entry, res->crasher_msg, 0, 0};
            c->min_queued = true;
            return;
        }
        if (!l->crash_written) {
            Str werr = {0};
            if (fuzz_write_to_corpus(&res->entry, c->opts->corpus_dir, a, &werr)) {
                l->crash_written = true;
                fuzz_stop(c, l, res->crasher_msg, res->entry.path);
            } else {
                fuzz_stop(c, l, werr, (Str){0});
            }
        }
    } else if (fuzz_warmup_run(c)) {
        c->warmup_left--;
        if (c->warmup_left == 0)
            fuzz_log(fmt_sprintf_v(
                a,
                "fuzz: elapsed: %s, testing seed corpus: %d/%d completed, "
                "now fuzzing with %d workers\n",
                burrow__testing_duration_string(fuzz_elapsed(c), buf), c->warmup_count,
                c->warmup_count, (Int)c->parallel));
    }
    if (res != c->crash_minimizing)
        fuzz_result_free(res);
}

/* The main loop's part in stopping: a worker still in a call a second after
 * stop is interrupted, and killed a second after that. worker.stop does the
 * same for one that is not in a call. Under c->mu. */
static void fuzz_escalate(FuzzCoord *c, const FuzzLoop *l, int64_t now) {
    for (int i = 0; i < c->parallel; i++) {
        FuzzWorker *w = &c->workers[i];
        if (!w->started || w->done || w->pid < 0 || !w->in_call)
            continue;
        if (!w->main_int && now >= l->stop_at + FUZZ_WORKER_TIMEOUT_NS) {
            w->main_int = true;
            w->interrupted = true;
#if defined(BURROW_OS_WINDOWS)
            pal_kill(w->pid, PAL_SIGKILL, NULL);
            w->main_kill = true;
#else
            pal_kill(w->pid, PAL_SIGINT, NULL);
#endif
        }
        if (!w->main_kill && now >= l->stop_at + 2 * FUZZ_WORKER_TIMEOUT_NS) {
            w->main_kill = true;
            pal_kill(w->pid, PAL_SIGKILL, NULL);
        }
    }
}

bool burrow__fuzz_coordinate(Alloc *a, const burrow__FuzzOpts *opts, Str *err,
                             Str *crash_path) {
    *err = (Str){0};
    *crash_path = (Str){0};
    FuzzCoord c = {0};
    c.opts = opts;
    c.parallel = opts->parallel > 0 ? opts->parallel : 1;
    if (opts->limit > 0 && (int64_t)c.parallel > opts->limit)
        c.parallel = (int)opts->limit;
    c.start_time = pal_clock_monotonic();
    c.time_last_log = c.start_time;
    burrow__note_init(&c.ev);

    /* exec.Command's LookPath, for a bare name. */
    const char *arg0 = opts->argc > 0 ? opts->argv[0] : "";
    Str argv0 = opts->exe.len > 0 ? opts->exe : str_from_cstr(arg0);
    bool has_sep = false;
    for (Int i = 0; i < argv0.len; i++)
        has_sep = has_sep || fuzz_is_sep(argv0.p[i]);
    if (!has_sep) {
        char found[4096];
        int64_t n = pal_exec_lookup(arg0, found, (int64_t)sizeof found, NULL);
        if (n > 0)
            argv0 = str_from_bytes((const Byte *)found, (Int)n);
    }
    c.argv0 = fuzz_str_dup(argv0);

    /* newCoordinator. */
    c.nseeds = opts->nseed;
    if (c.nseeds > 0)
        c.seeds = (FuzzCorpusEntry *)fuzz_must_alloc(
            (size_t)c.nseeds * sizeof(FuzzCorpusEntry), _Alignof(FuzzCorpusEntry));
    for (Int i = 0; i < c.nseeds; i++) {
        c.seeds[i] = (FuzzCorpusEntry){fuzz_str_dup(opts->seed[i].path),
                                       {0},
                                       fuzz_str_dup(opts->seed[i].data),
                                       0,
                                       true};
        fuzz_add_corpus(&c, c.seeds[i]);
    }
    for (Int i = 0; i < opts->ncache; i++)
        fuzz_add_corpus(
            &c,
            (FuzzCorpusEntry){opts->cache[i].path, {0}, opts->cache[i].data, 0, false});
    if (opts->minimize_limit > 0 || opts->minimize_timeout > 0)
        for (Int i = 0; i < opts->ntypes; i++)
            if (opts->types[i] == TYPE_STRING || opts->types[i] == TYPE_BYTES)
                c.minimization_allowed = true;
    fuzz_log(
        BURROW_S("warning: the test binary was not built with coverage "
                 "instrumentation, so fuzzing will run without coverage guidance and "
                 "may be inefficient\n"));
    c.warmup_count = c.nseeds;
    for (Int i = 0; i < c.nseeds; i++)
        fuzz_enqueue(&c, c.seeds[i]);
    c.warmup_left = c.warmup_count;
    if (c.ncorpus == 0) {
        fuzz_log(BURROW_S("warning: starting with empty corpus\n"));
        Int n = opts->ntypes;
        Any *vals = (Any *)fuzz_must_alloc((size_t)n * sizeof(Any), _Alignof(Any));
        FuzzCell *cells = (FuzzCell *)fuzz_must_alloc((size_t)n * sizeof(FuzzCell),
                                                      _Alignof(FuzzCell));
        memset(cells, 0, (size_t)n * sizeof(FuzzCell));
        for (Int i = 0; i < n; i++)
            vals[i] = (Any){opts->types[i], &cells[i]};
        Str data = burrow__testing_corpus_marshal(a, vals, n);
        fuzz_free(cells, (size_t)n * sizeof(FuzzCell), _Alignof(FuzzCell));
        fuzz_free(vals, (size_t)n * sizeof(Any), _Alignof(Any));
        char hex[9];
        fuzz_add_corpus(
            &c, (FuzzCorpusEntry){fuzz_sum_hex(data, 4, hex), {0}, data, 0, false});
    }

    FuzzLoop l = {0};
    int nalloc = c.parallel;
    c.workers = (FuzzWorker *)fuzz_must_alloc((size_t)c.parallel * sizeof(FuzzWorker),
                                              _Alignof(FuzzWorker));
    memset(c.workers, 0, (size_t)c.parallel * sizeof(FuzzWorker));
    int started = 0;
    Str start_err = {0};
    for (int i = 0; i < c.parallel; i++) {
        FuzzWorker *w = &c.workers[i];
        w->c = &c;
        w->pid = -1;
        w->in_w = PAL_INVALID_HANDLE;
        w->out_r = PAL_INVALID_HANDLE;
        if (!fuzz_mem_create(&w->mem, FUZZ_VALUE_SIZE, a, &start_err))
            break;
        burrow__note_init(&w->wake);
        burrow__fuzz_mutator_init(&w->m);
        w->started = true;
        started++;
    }
    if (start_err.len > 0) {
        /* newWorker failed, and Go returns before any worker has run. */
        for (int i = 0; i < started; i++) {
            fuzz_mem_close(&c.workers[i].mem, a, &(Str){0});
            burrow__note_free(&c.workers[i].wake);
            burrow__fuzz_mutator_free(&c.workers[i].m);
        }
        c.parallel = 0;
        l.have_err = true;
        l.err = fuzz_str_dup(start_err);
    } else {
        for (int i = 0; i < c.parallel; i++)
            if (!burrow__thread_start(&c.workers[i].thread, fuzz_worker_main,
                                      &c.workers[i], 0))
                panic_str(BURROW_S("testing: cannot start a fuzzing thread"));
    }

    int active = c.parallel;
    int64_t deadline = opts->timeout > 0 ? c.start_time + opts->timeout : 0;
    int64_t next_tick = c.start_time + (int64_t)3 * 1000000000;
    burrow__lock(&c.mu);
    if (active > 0)
        fuzz_log_stats(&c);
    while (active > 0) {
        burrow__note_clear(&c.ev);
        int64_t now = pal_clock_monotonic();
        if (opts->limit > 0 && c.count >= opts->limit)
            fuzz_stop(&c, &l, (Str){0}, (Str){0});
        if (!l.stopping && deadline != 0 && now >= deadline)
            fuzz_stop(&c, &l, (Str){0}, (Str){0});

        bool acted = false;
        for (int i = 0; i < c.parallel; i++) {
            FuzzWorker *w = &c.workers[i];
            if (w->done && !w->collected) {
                w->collected = true;
                fuzz_stop(&c, &l, w->err, (Str){0});
                active--;
                acted = true;
            }
        }
        if (active == 0)
            break;
        if (acted)
            continue;

        if (c.results != NULL) {
            FuzzResult *res = c.results;
            c.results = res->next;
            if (c.results == NULL)
                c.results_tail = NULL;
            fuzz_handle_result(&c, &l, res, a);
            continue;
        }

        FuzzInput in;
        FuzzWorker *w = fuzz_idle_worker(&c);
        if (w != NULL && c.crash_minimizing == NULL && !l.stopping &&
            fuzz_peek_input(&c, &in)) {
            w->input = in;
            w->job = FUZZ_JOB_FUZZ;
            w->idle = false;
            burrow__note_wake(&w->wake);
            c.qhead++;
            c.qlen--;
            if (c.qlen == 0)
                c.qhead = 0;
            c.count_waiting += in.limit;
            continue;
        }
        FuzzMinInput min;
        if (w != NULL && !l.stopping && fuzz_peek_minimize(&c, &min)) {
            w->min_input = min;
            w->job = FUZZ_JOB_MINIMIZE;
            w->idle = false;
            burrow__note_wake(&w->wake);
            c.min_queued = false;
            c.count_waiting += min.limit;
            continue;
        }

        if (now >= next_tick) {
            fuzz_log_stats(&c);
            while (next_tick <= now)
                next_tick += (int64_t)3 * 1000000000;
            continue;
        }
        int64_t wake = next_tick;
        if (!l.stopping && deadline != 0 && deadline < wake)
            wake = deadline;
        if (l.stopping) {
            fuzz_escalate(&c, &l, now);
            int64_t t1 = l.stop_at + FUZZ_WORKER_TIMEOUT_NS;
            int64_t t2 = l.stop_at + 2 * FUZZ_WORKER_TIMEOUT_NS;
            if (now < t1 && t1 < wake)
                wake = t1;
            else if (now < t2 && t2 < wake)
                wake = t2;
        }
        burrow__unlock(&c.mu);
        if (wake > now)
            burrow__note_sleep_timeout(&c.ev, wake - now);
        burrow__lock(&c.mu);
    }
    burrow__unlock(&c.mu);
    for (int i = 0; i < c.parallel; i++)
        burrow__thread_join(&c.workers[i].thread);
    if (c.parallel > 0)
        fuzz_log_stats(&c);

    /* The deferred write of a crash that was still being minimized. */
    if (c.crash_minimizing != NULL && !l.crash_written) {
        Str werr = {0};
        if (!fuzz_write_to_corpus(&c.crash_minimizing->entry, opts->corpus_dir, a,
                                  &werr)) {
            Str prev = l.have_err ? l.err : BURROW_S("%!w(<nil>)");
            Str both = fmt_sprintf_v(a, "%s\n%s", prev, werr);
            fuzz_str_free(l.err);
            fuzz_str_free(l.crash_path);
            l.err = fuzz_str_dup(both);
            l.crash_path = (Str){0};
            l.have_err = true;
        } else if (!l.have_err) {
            l.have_err = true;
            l.err = fuzz_str_dup(c.crash_minimizing->crasher_msg);
            l.crash_path = fuzz_str_dup(c.crash_minimizing->entry.path);
        }
    }

    bool ok = !l.have_err;
    if (l.have_err) {
        *err = str_clone(a, l.err);
        if (l.crash_path.len > 0)
            *crash_path = str_clone(a, l.crash_path);
    }
    fuzz_str_free(l.err);
    fuzz_str_free(l.crash_path);

    while (c.results != NULL) {
        FuzzResult *r = c.results;
        c.results = r->next;
        fuzz_result_free(r);
    }
    fuzz_result_free(c.crash_minimizing);
    for (int i = 0; i < c.parallel; i++) {
        FuzzWorker *w = &c.workers[i];
        fuzz_str_free(w->err);
        burrow__note_free(&w->wake);
        burrow__fuzz_mutator_free(&w->m);
    }
    fuzz_free(c.workers, (size_t)nalloc * sizeof(FuzzWorker), _Alignof(FuzzWorker));
    for (Int i = 0; i < c.ncorpus; i++)
        fuzz_centry_free(&c.corpus[i]);
    if (c.corpus != NULL) {
        fuzz_free(c.corpus, (size_t)c.corpus_cap * sizeof(FuzzCorpusEntry),
                  _Alignof(FuzzCorpusEntry));
        fuzz_free(c.hashes, (size_t)c.corpus_cap * 32, 1);
    }
    for (Int i = 0; i < c.nseeds; i++)
        fuzz_centry_free(&c.seeds[i]);
    if (c.seeds != NULL)
        fuzz_free(c.seeds, (size_t)c.nseeds * sizeof(FuzzCorpusEntry),
                  _Alignof(FuzzCorpusEntry));
    if (c.queue != NULL)
        fuzz_free(c.queue, (size_t)c.qcap * sizeof(FuzzCorpusEntry),
                  _Alignof(FuzzCorpusEntry));
    fuzz_str_free(c.argv0);
    burrow__note_free(&c.ev);
    return ok;
}

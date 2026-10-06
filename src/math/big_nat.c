/* math/big: unsigned integers as vectors of words, nat.go and natmul.go, and
 * the scratch memory everything under Int works in.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "big_internal.h"

#include "burrow/mem/heap.h"
#include "burrow/pal.h"
#include "burrow/panic.h"
#include "burrow/platform.h"

/* ---------------------------------------------------------------- scratch */

/* The scratch memory of one thread. heap is where the nat layer's vectors
 * come from and stack is Go's stk. Both go back to empty when a public
 * function returns, and back to the heap too when that function used more
 * than BIG_SCRATCH_KEEP, so that one huge computation does not pin its memory
 * to the thread for good. */
typedef struct BigScratch {
    Arena heap;
    Arena stack;
    bool used;
    size_t bytes;
    PalThreadExit on_exit;
} BigScratch;

#define BIG_SCRATCH_CHUNK ((size_t)16 * 1024)
#define BIG_SCRATCH_KEEP ((size_t)1024 * 1024)

/* The thread's scratch lives on the heap and the thread local is only a
 * pointer to it. The exit hook's node is inside the scratch, and it cannot be
 * in thread local storage itself, because macOS frees that storage from its own
 * thread exit destructor, which may run before the hook does. */
static BURROW_THREAD_LOCAL BigScratch *big_tls;

/* Gives a thread's scratch memory back as the thread exits. It does not touch
 * big_tls, which may already be gone by then. */
static void big_scratch_release(void *arg) {
    BigScratch *s = arg;
    arena_free(&s->heap);
    arena_free(&s->stack);
    mem_free(heap_allocator(), s, sizeof *s, _Alignof(BigScratch));
}

static BigScratch *big_scratch(void) {
    BigScratch *s = big_tls;
    if (s == NULL) {
        s = mem_alloc(heap_allocator(), sizeof *s, _Alignof(BigScratch));
        if (s == NULL)
            big_oom();
        arena_init(&s->heap, NULL, BIG_SCRATCH_CHUNK);
        arena_init(&s->stack, NULL, BIG_SCRATCH_CHUNK);
        big_tls = s;
        /* If the hook cannot be set up, the memory leaks when the thread
         * exits, which is better than refusing to compute. */
        s->on_exit.fn = big_scratch_release;
        s->on_exit.arg = s;
        (void)pal_thread_on_exit(&s->on_exit);
    }
    return s;
}

void big_oom(void) {
    panic_str(BURROW_S("math/big: out of memory"));
}

/* What an empty Nat points at. Nothing is ever read or written through it,
 * since its length and capacity are 0, but a pointer that is never NULL lets gcc
 * see that the words a caller indexes are always there. */
static BigWord big_no_words[1];

static BigWord *big_words(Arena *ar, BigScratch *s, Int n) {
    if (n <= 0)
        return big_no_words;
    size_t size = (size_t)n * sizeof(BigWord);
    if ((size_t)n > SIZE_MAX / sizeof(BigWord))
        big_oom();
    BigWord *p = mem_alloc_nozero(arena_allocator(ar), size, _Alignof(BigWord));
    if (p == NULL)
        big_oom();
    s->used = true;
    s->bytes += size;
    return p;
}

BigWord *big_alloc(Int n) {
    BigScratch *s = big_scratch();
    return big_words(&s->heap, s, n);
}

Nat big_stk_nat(Int n) {
    BigScratch *s = big_scratch();
    Nat r = {big_words(&s->stack, s, (n + 3) & ~(Int)3), n,
             n}; /* rounded up, as Go does */
    if (n > 0)
        r.p[0] = 0xfedcb; /* as Go does, to catch code that expects zeroes */
    return r;
}

Alloc *big_scratch_alloc(void) {
    BigScratch *s = big_scratch();
    s->used = true;
    return arena_allocator(&s->heap);
}

ArenaMark big_stk_save(void) {
    return arena_mark(&big_scratch()->stack);
}

void big_stk_restore(ArenaMark m) {
    arena_release(&big_scratch()->stack, m);
}

static void big_scratch_done(void) {
    BigScratch *s = big_tls;
    if (s == NULL || !s->used)
        return;
    if (s->bytes > BIG_SCRATCH_KEEP) {
        arena_free(&s->heap);
        arena_free(&s->stack);
        arena_init(&s->heap, NULL, BIG_SCRATCH_CHUNK);
        arena_init(&s->stack, NULL, BIG_SCRATCH_CHUNK);
    } else {
        arena_reset(&s->heap);
        arena_reset(&s->stack);
    }
    s->used = false;
    s->bytes = 0;
}

/* A public function that panicked part way through never reached its
 * big_leave, so the next one tidies up first. */
void big_enter(void) {
    big_scratch_done();
}

void big_leave(void) {
    big_scratch_done();
}

static Alloc *big_owner(Alloc *a) {
    return a != NULL ? a : heap_allocator();
}

/* A buffer of n words from a, with Go's four words of room to grow. */
static BigWord *big_own(Alloc *a, Int n, Int *cap) {
    Int c = n + 4;
    BigWord *p =
        mem_alloc_array(big_owner(a), (size_t)c, sizeof(BigWord), _Alignof(BigWord));
    if (p == NULL)
        big_oom();
    *cap = c;
    return p;
}

static void big_disown(Alloc *a, Nat x) {
    if (x.p != NULL && x.cap > 0)
        mem_free(big_owner(a), x.p, (size_t)x.cap * sizeof(BigWord), _Alignof(BigWord));
}

void big_reserve(Nat *abs, Alloc *a, Int n) {
    if (n <= abs->cap)
        return;
    Int cap;
    BigWord *p = big_own(a, n, &cap);
    if (abs->len > 0)
        memcpy(p, abs->p, (size_t)abs->len * sizeof(BigWord));
    big_disown(a, *abs);
    abs->p = p;
    abs->cap = cap;
}

void big_commit(Nat *abs, Alloc *a, Nat orig) {
    if (abs->p == orig.p && abs->cap == orig.cap)
        return;
    Nat r = *abs;
    if (r.len <= orig.cap) {
        nat_copy(nat_to(orig, r.len), r);
        abs->p = orig.p;
        abs->cap = orig.cap;
        return;
    }
    Int cap;
    BigWord *p = big_own(a, r.len, &cap);
    memcpy(p, r.p, (size_t)r.len * sizeof(BigWord));
    big_disown(a, orig);
    abs->p = p;
    abs->cap = cap;
}

void big_int_free(BigInt *x) {
    if (x == NULL)
        return;
    big_disown(x->a, x->abs);
    x->abs.p = NULL;
    x->abs.len = 0;
    x->abs.cap = 0;
    x->neg = false;
}

/* ------------------------------------------------------------------- nat.go */

const BigWord big_nat_one_w[1] = {1};
const BigWord big_nat_two_w[1] = {2};
const BigWord big_nat_five_w[1] = {5};
const BigWord big_nat_ten_w[1] = {10};

Nat nat_make(Nat z, Int n) {
    if (n <= z.cap)
        return nat_to(z, n);
    if (n == 1) {
        Nat r = {big_alloc(1), 1, 1};
        return r;
    }
    const Int e = 4; /* extra capacity */
    Nat r = {big_alloc(n + e), n, n + e};
    return r;
}

Nat nat_set_word(Nat z, BigWord x) {
    if (x == 0)
        return nat_to(z, 0);
    z = nat_make(z, 1);
    z.p[0] = x;
    return z;
}

Nat nat_set_uint64(Nat z, uint64_t x) {
    BigWord w = (BigWord)x;
    if ((uint64_t)w == x)
        return nat_set_word(z, w);
    z = nat_make(z, 2);
    z.p[1] = (BigWord)(x >> 32);
    z.p[0] = (BigWord)x;
    return z;
}

Nat nat_set(Nat z, Nat x) {
    z = nat_make(z, x.len);
    nat_copy(z, x);
    return z;
}

Nat nat_add(Nat z, Nat x, Nat y) {
    Int m = x.len;
    Int n = y.len;
    if (m < n)
        return nat_add(z, y, x);
    if (m == 0)
        return nat_to(z, 0);
    if (n == 0)
        return nat_set(z, x);
    z = nat_make(z, m + 1);
    BigWord c = big_add_vv(nat_to(z, n), nat_to(x, n), nat_to(y, n));
    if (m > n)
        c = big_add_vw(nat_slice(z, n, m), nat_from(x, n), c);
    z.p[m] = c;
    return nat_norm(z);
}

Nat nat_sub(Nat z, Nat x, Nat y) {
    Int m = x.len;
    Int n = y.len;
    if (m < n)
        panic_str(BURROW_S("underflow"));
    if (m == 0)
        return nat_to(z, 0);
    if (n == 0)
        return nat_set(z, x);
    z = nat_make(z, m);
    BigWord c = big_sub_vv(nat_to(z, n), nat_to(x, n), nat_to(y, n));
    if (m > n)
        c = big_sub_vw(nat_from(z, n), nat_from(x, n), c);
    if (c != 0)
        panic_str(BURROW_S("underflow"));
    return nat_norm(z);
}

int nat_cmp(Nat x, Nat y) {
    Int m = x.len;
    Int n = y.len;
    if (m != n || m == 0) {
        if (m < n)
            return -1;
        if (m > n)
            return 1;
        return 0;
    }
    Int i = m - 1;
    while (i > 0 && x.p[i] == y.p[i])
        i--;
    if (x.p[i] < y.p[i])
        return -1;
    if (x.p[i] > y.p[i])
        return 1;
    return 0;
}

/* montgomery: z = x*y*2**(-n*_W) mod m, for x, y and m of n words each and
 * k = -m**-1 mod 2**_W. The result may be m or more but fits in n words. */
Nat nat_montgomery(Nat z, Nat x, Nat y, Nat m, BigWord k, Int n) {
    if (x.len != n || y.len != n || m.len != n)
        panic_str(BURROW_S("math/big: mismatched montgomery number lengths"));
    z = nat_make(z, n * 2);
    nat_clear(z);
    BigWord c = 0;
    for (Int i = 0; i < n; i++) {
        BigWord d = y.p[i];
        Nat zi = nat_slice(z, i, n + i);
        BigWord c2 = big_add_mul_vvww(zi, zi, x, d, 0);
        BigWord t = z.p[i] * k;
        BigWord c3 = big_add_mul_vvww(zi, zi, m, t, 0);
        BigWord cx = c + c2;
        BigWord cy = cx + c3;
        z.p[n + i] = cy;
        c = (cx < c2 || cy < c3) ? 1 : 0;
    }
    if (c != 0)
        big_sub_vv(nat_to(z, n), nat_from(z, n), m);
    else
        memmove(z.p, z.p + n, (size_t)n * sizeof(BigWord));
    return nat_to(z, n);
}

/* addTo: z += x, where z is at least as long as x and the carry is dropped
 * off the top. */
void nat_add_to(Nat z, Nat x) {
    Int n = x.len;
    if (n > 0) {
        BigWord c = big_add_vv(nat_to(z, n), nat_to(z, n), nat_to(x, n));
        if (c != 0 && n < z.len)
            big_add_vw(nat_from(z, n), nat_from(z, n), c);
    }
}

/* mulRange: the product of the integers in [a, b]. */
Nat nat_mul_range(Nat z, uint64_t a, uint64_t b) {
    if (a == 0)
        return nat_set_uint64(z, 0);
    if (a > b)
        return nat_set_uint64(z, 1);
    if (a == b)
        return nat_set_uint64(z, a);
    if (a + 1 == b)
        return nat_mul(z, nat_set_uint64(NAT_NIL, a), nat_set_uint64(NAT_NIL, b));
    uint64_t m = a + (b - a) / 2; /* avoid overflow */
    return nat_mul(z, nat_mul_range(NAT_NIL, a, m), nat_mul_range(NAT_NIL, m + 1, b));
}

/* Used by crypto: only the size of x may show in the timing, so the top word
 * has every bit below its first one set before it is measured. */
Int nat_bit_len(Nat x) {
    Int i = x.len - 1;
    if (i >= 0) {
        Uint top = x.p[i];
        top |= top >> 1;
        top |= top >> 2;
        top |= top >> 4;
        top |= top >> 8;
        top |= top >> 16;
        top |= top >> 16 >> 16;
        return i * (Int)BIG_W + bits_len(top);
    }
    return 0;
}

Uint nat_trailing_zero_bits(Nat x) {
    if (x.len == 0)
        return 0;
    Uint i = 0;
    while (x.p[i] == 0)
        i++;
    return i * BIG_W + (Uint)bits_trailing_zeros(x.p[i]);
}

/* isPow2: whether x is a power of two, and which. */
bool nat_is_pow2(Nat x, Uint *n) {
    Uint i = 0;
    while (x.p[i] == 0)
        i++;
    if (i == (Uint)x.len - 1 && (x.p[i] & (x.p[i] - 1)) == 0) {
        *n = i * BIG_W + (Uint)bits_trailing_zeros(x.p[i]);
        return true;
    }
    *n = 0;
    return false;
}

Nat nat_lsh(Nat z, Nat x, Uint s) {
    if (s == 0) {
        if (nat_same(z, x))
            return z;
        if (!nat_alias(z, x))
            return nat_set(z, x);
    }
    Int m = x.len;
    if (m == 0)
        return nat_to(z, 0);
    Int n = m + (Int)(s / BIG_W);
    z = nat_make(z, n + 1);
    s %= BIG_W;
    if (s == 0) {
        memmove(z.p + (n - m), x.p, (size_t)m * sizeof(BigWord));
        z.p[n] = 0;
    } else {
        z.p[n] = big_lsh_vu(nat_slice(z, n - m, n), x, s);
    }
    nat_clear(nat_to(z, n - m));
    return nat_norm(z);
}

Nat nat_rsh(Nat z, Nat x, Uint s) {
    if (s == 0) {
        if (nat_same(z, x))
            return z;
        if (!nat_alias(z, x))
            return nat_set(z, x);
    }
    Int m = x.len;
    Int n = m - (Int)(s / BIG_W);
    if (s / BIG_W >= (Uint)m || n <= 0)
        return nat_to(z, 0);
    z = nat_make(z, n);
    s %= BIG_W;
    if (s == 0)
        memmove(z.p, x.p + (m - n), (size_t)n * sizeof(BigWord));
    else
        big_rsh_vu(z, nat_from(x, m - n), s);
    return nat_norm(z);
}

Nat nat_set_bit(Nat z, Nat x, Uint i, Uint b) {
    Int j = (Int)(i / BIG_W);
    BigWord m = (BigWord)1 << (i % BIG_W);
    Int n = x.len;
    switch (b) {
    case 0:
        z = nat_make(z, n);
        nat_copy(z, x);
        if (j >= n)
            return z; /* no need to grow */
        z.p[j] &= ~m;
        return nat_norm(z);
    case 1:
        if (j >= n) {
            z = nat_make(z, j + 1);
            nat_clear(nat_from(z, n));
        } else {
            z = nat_make(z, n);
        }
        nat_copy(z, x);
        z.p[j] |= m;
        return z; /* no need to normalize */
    default:
        break;
    }
    panic_str(BURROW_S("set bit is not 0 or 1"));
}

Uint nat_bit(Nat x, Uint i) {
    Uint j = i / BIG_W;
    if (j >= (Uint)x.len)
        return 0;
    return (Uint)(x.p[j] >> (i % BIG_W) & 1);
}

/* sticky: 1 if there is a one bit among the i least significant bits. */
Uint nat_sticky(Nat x, Uint i) {
    Uint j = i / BIG_W;
    if (j >= (Uint)x.len) {
        if (x.len == 0)
            return 0;
        return 1;
    }
    for (Uint k = 0; k < j; k++)
        if (x.p[k] != 0)
            return 1;
    Uint s = BIG_W - i % BIG_W;
    if (s < BIG_W && x.p[j] << s != 0)
        return 1;
    return 0;
}

Nat nat_and(Nat z, Nat x, Nat y) {
    Int m = x.len < y.len ? x.len : y.len;
    z = nat_make(z, m);
    for (Int i = 0; i < m; i++)
        z.p[i] = x.p[i] & y.p[i];
    return nat_norm(z);
}

/* trunc: z = x mod 2**n. */
Nat nat_trunc(Nat z, Nat x, Uint n) {
    Uint w = (n + BIG_W - 1) / BIG_W;
    if ((Uint)x.len < w)
        return nat_set(z, x);
    z = nat_make(z, (Int)w);
    nat_copy(z, x);
    if (n % BIG_W != 0)
        z.p[z.len - 1] &= ((BigWord)1 << (n % BIG_W)) - 1;
    return nat_norm(z);
}

Nat nat_and_not(Nat z, Nat x, Nat y) {
    Int m = x.len;
    Int n = y.len < m ? y.len : m;
    z = nat_make(z, m);
    for (Int i = 0; i < n; i++)
        z.p[i] = x.p[i] & ~y.p[i];
    if (m > n)
        memmove(z.p + n, x.p + n, (size_t)(m - n) * sizeof(BigWord));
    return nat_norm(z);
}

Nat nat_or(Nat z, Nat x, Nat y) {
    Int m = x.len;
    Int n = y.len;
    Nat s = x;
    if (m < n) {
        Int t = m;
        m = n;
        n = t;
        s = y;
    }
    z = nat_make(z, m);
    for (Int i = 0; i < n; i++)
        z.p[i] = x.p[i] | y.p[i];
    if (m > n)
        memmove(z.p + n, s.p + n, (size_t)(m - n) * sizeof(BigWord));
    return nat_norm(z);
}

Nat nat_xor(Nat z, Nat x, Nat y) {
    Int m = x.len;
    Int n = y.len;
    Nat s = x;
    if (m < n) {
        Int t = m;
        m = n;
        n = t;
        s = y;
    }
    z = nat_make(z, m);
    for (Int i = 0; i < n; i++)
        z.p[i] = x.p[i] ^ y.p[i];
    if (m > n)
        memmove(z.p + n, s.p + n, (size_t)(m - n) * sizeof(BigWord));
    return nat_norm(z);
}

/* random: a number in [0, limit), n the bit length of limit. */
Nat nat_random(Nat z, MathRandRand *rnd, Nat limit, Int n) {
    if (nat_alias(z, limit))
        z = NAT_NIL;    /* z is an alias for limit, cannot reuse */
    if (limit.len == 0) /* Go never asks for a number below 0 */
        return nat_to(z, 0);
    z = nat_make(z, limit.len);
    Uint msw = (Uint)(n % (Int)BIG_W);
    if (msw == 0)
        msw = BIG_W;
    BigWord mask = msw == BIG_W ? BIG_M : ((BigWord)1 << msw) - 1;
    for (;;) {
#if BURROW_INT_BITS == 64
        for (Int i = 0; i < z.len; i++) {
            /* Two statements, since C leaves the order of the operands of |
             * open and MSVC draws the high half first. */
            BigWord lo = (BigWord)math_rand_rand_uint32(rnd);
            z.p[i] = lo | (BigWord)math_rand_rand_uint32(rnd) << 32;
        }
#else
        for (Int i = 0; i < z.len; i++)
            z.p[i] = (BigWord)math_rand_rand_uint32(rnd);
#endif
        /* z has limit.len words and limit is not 0. */
        /* NOLINTNEXTLINE(clang-analyzer-core.NullDereference) */
        z.p[limit.len - 1] &= mask;
        if (nat_cmp(z, limit) < 0)
            break;
    }
    return nat_norm(z);
}

/* ----------------------------------------------------------------- expNN */

static Nat nat_exp_nn_montgomery_even(Nat z, Nat x, Nat y, Nat m);
static Nat nat_exp_nn_windowed(Nat z, Nat x, Nat y, Uint log_m);
static Nat nat_exp_nn_montgomery(Nat z, Nat x, Nat y, Nat m);

/* expNN: z = x**y mod m, or x**y when m is empty. */
Nat nat_exp_nn(Nat z, Nat x, Nat y, Nat m, bool slow) {
    if (nat_alias(z, x) || nat_alias(z, y))
        z = NAT_NIL; /* we cannot allow in-place modification of x or y */

    /* x**y mod 1 == 0 */
    if (m.len == 1 && m.p[0] == 1)
        return nat_set_word(z, 0);
    /* x**0 == 1 */
    if (y.len == 0)
        return nat_set_word(z, 1);
    /* 0**y = 0 */
    if (x.len == 0)
        return nat_set_word(z, 0);
    /* 1**y = 1 */
    if (x.len == 1 && x.p[0] == 1)
        return nat_set_word(z, 1);
    /* x**1 == x */
    if (y.len == 1 && y.p[0] == 1 && m.len == 0)
        return nat_set(z, x);
    if (y.len == 1 && y.p[0] == 1) /* m.len > 0 */
        return nat_rem(z, x, m);

    /* y > 1 */
    if (m.len != 0) {
        /* We likely end up being as long as the modulus. */
        z = nat_make(z, m.len);

        /* A large exponent takes Montgomery for an odd modulus, a 4 bit
         * window for a power of two, and the two combined through the
         * Chinese remainder theorem for the rest. */
        if (y.len > 1 && !slow) {
            if ((m.p[0] & 1) == 1)
                return nat_exp_nn_montgomery(z, x, y, m);
            Uint log_m;
            if (nat_is_pow2(m, &log_m))
                return nat_exp_nn_windowed(z, x, y, log_m);
            return nat_exp_nn_montgomery_even(z, x, y, m);
        }
    }

    z = nat_set(z, x);
    BigWord v = y.p[y.len - 1]; /* v > 0 because y is normalized and y > 0 */
    Uint shift = big_nlz(v) + 1;
    v = shift < BIG_W ? v << shift : 0;
    Nat q = NAT_NIL;

    const BigWord mask = (BigWord)1 << (BIG_W - 1);

    /* Walk the bits of the exponent one by one, squaring for each and
     * multiplying by x for each one. zz and r keep mul and div from
     * allocating, which they would do when their arguments alias. */
    Int w = (Int)BIG_W - (Int)shift;
    Nat zz = NAT_NIL, r = NAT_NIL;
    for (Int j = 0; j < w; j++) {
        zz = nat_sqr(zz, z);
        Nat t = zz;
        zz = z;
        z = t;

        if ((v & mask) != 0) {
            zz = nat_mul(zz, z, x);
            t = zz;
            zz = z;
            z = t;
        }

        if (m.len != 0) {
            zz = nat_div(zz, r, z, m, &r);
            Nat oq = q, oz = z, ozz = zz, orr = r;
            zz = oq;
            r = oz;
            q = ozz;
            z = orr;
        }

        v <<= 1;
    }

    for (Int i = y.len - 2; i >= 0; i--) {
        v = y.p[i];

        for (Uint j = 0; j < BIG_W; j++) {
            zz = nat_sqr(zz, z);
            Nat t = zz;
            zz = z;
            z = t;

            if ((v & mask) != 0) {
                zz = nat_mul(zz, z, x);
                t = zz;
                zz = z;
                z = t;
            }

            if (m.len != 0) {
                zz = nat_div(zz, r, z, m, &r);
                Nat oq = q, oz = z, ozz = zz, orr = r;
                zz = oq;
                r = oz;
                q = ozz;
                z = orr;
            }

            v <<= 1;
        }
    }

    return nat_norm(z);
}

/* expNNMontgomeryEven: x**y mod m for m = m1*m2 with m1 = 2**n and m2 odd,
 * from x**y mod m1 and x**y mod m2 and the Chinese remainder theorem. Koç,
 * "Montgomery Reduction with Even Modulus". */
static Nat nat_exp_nn_montgomery_even(Nat z, Nat x, Nat y, Nat m) {
    /* Split m = m1 * m2 where m1 = 2**n. */
    Uint n = nat_trailing_zero_bits(m);
    Nat m1 = nat_lsh(NAT_NIL, NAT_ONE, n);
    Nat m2 = nat_rsh(NAT_NIL, m, n);

    Nat z1 = nat_exp_nn(NAT_NIL, x, y, m1, false);
    Nat z2 = nat_exp_nn(NAT_NIL, x, y, m2, false);

    /* p = (z1 - z2) * m2**-1 (mod m1), z = z2 + p * m2. */
    z = nat_set(z, z2);

    /* (z1 - z2) mod m1, into z1. */
    z1 = nat_sub_mod_2n(z1, z1, z2, n);

    /* p = (z1 - z2) * m2**-1 (mod 2**n), into z2. */
    Nat m2inv = nat_mod_inverse(NAT_NIL, m2, m1);
    z2 = nat_mul(z2, z1, m2inv);
    z2 = nat_trunc(z2, z2, n);

    /* p * m2, into z1. */
    z = nat_add(z, z, nat_mul(z1, z2, m2));

    return z;
}

/* expNNWindowed: x**y mod 2**log_m, with a fixed 4 bit window. */
static Nat nat_exp_nn_windowed(Nat z, Nat x, Nat y, Uint log_m) {
    if (y.len <= 1)
        panic_str(BURROW_S("big: misuse of expNNWindowed"));
    if ((x.p[0] & 1) == 0) {
        /* x is even and y > log_m, so x**y is a multiple of 2**log_m. */
        return nat_set_word(z, 0);
    }
    if (log_m == 1)
        return nat_set_word(z, 1);

    ArenaMark mark = big_stk_save();
    Int w = (Int)((log_m + BIG_W - 1) / BIG_W);
    Nat zz = big_stk_nat(w);
    const BigWord *stk_zz = zz.p;

    enum { n = 4 };
    /* powers[i] contains x**i. */
    Nat powers[1 << n];
    for (int i = 0; i < 1 << n; i++)
        powers[i] = big_stk_nat(w);
    powers[0] = nat_set(powers[0], NAT_ONE);
    powers[1] = nat_trunc(powers[1], x, log_m);
    for (int i = 2; i < 1 << n; i += 2) {
        Nat *p2 = &powers[i / 2], *p = &powers[i], *p1 = &powers[i + 1];
        *p = nat_sqr(*p, *p2);
        *p = nat_trunc(*p, *p, log_m);
        *p1 = nat_mul(*p1, *p, x);
        *p1 = nat_trunc(*p1, *p1, log_m);
    }

    /* x**(2**(log_m-1)) = 1, so only the bottom log_m-1 bits of y matter. */
    Int i = y.len - 1;
    Int mtop =
        (Int)((log_m - 2) / BIG_W); /* the top word of N bits is the (N-1)/W'th */
    BigWord mmask = BIG_M;
    Uint mbits = (log_m - 1) & (BIG_W - 1);
    if (mbits != 0)
        mmask = ((BigWord)1 << mbits) - 1;
    if (i > mtop)
        i = mtop;
    bool advance = false;
    z = nat_set_word(z, 1);
    for (; i >= 0; i--) {
        BigWord yi = y.p[i];
        if (i == mtop)
            yi &= mmask;
        for (Uint j = 0; j < BIG_W; j += n) {
            if (advance) {
                /* Account for the 4 bits used in the previous round. */
                for (int k = 0; k < 4; k++) {
                    zz = nat_sqr(zz, z);
                    Nat t = zz;
                    zz = z;
                    z = t;
                    z = nat_trunc(z, z, log_m);
                }
            }

            zz = nat_mul(zz, z, powers[yi >> (BIG_W - n)]);
            Nat t = zz;
            zz = z;
            z = t;
            z = nat_trunc(z, z, log_m);

            yi <<= n;
            advance = true;
        }
    }

    z = nat_norm(z);
    /* The swaps come out even so z is the caller's vector again, but if it
     * were the stack one the restore would take it away. */
    if (z.p != NULL && z.p == stk_zz)
        z = nat_set(NAT_NIL, z);
    big_stk_restore(mark);
    return z;
}

/* expNNMontgomery: x**y mod m for an odd m, with a fixed 4 bit window over
 * the Montgomery form. */
static Nat nat_exp_nn_montgomery(Nat z, Nat x, Nat y, Nat m) {
    Int num_words = m.len;

    /* x and m have to be the same length. x may be m or more. */
    if (x.len > num_words) {
        Nat r;
        nat_div(NAT_NIL, NAT_NIL, x, m, &r);
        x = r;
        /* now x.len <= num_words, not guaranteed == */
    }
    if (x.len < num_words) {
        Nat rr = {big_alloc(num_words), num_words, num_words};
        nat_clear(rr);
        nat_copy(rr, x);
        x = rr;
    }

    /* k0 = -m**-1 mod 2**_W. Dumas, "On Newton-Raphson Iteration for
     * Multiplicative Inverses Modulo Prime Powers". */
    BigWord k0 = 2 - m.p[0];
    BigWord t = m.p[0] - 1;
    for (Uint i = 1; i < BIG_W; i <<= 1) {
        t *= t;
        k0 *= (t + 1);
    }
    k0 = 0 - k0;

    /* RR = 2**(2*_W*len(m)) mod m */
    Nat rr = nat_set_word(NAT_NIL, 1);
    Nat zz = nat_lsh(NAT_NIL, rr, (Uint)(2 * num_words) * BIG_W);
    nat_div(NAT_NIL, rr, zz, m, &rr);
    if (rr.len < num_words) {
        zz = nat_make(zz, num_words);
        nat_clear(zz);
        nat_copy(zz, rr);
        rr = zz;
    }
    /* one = 1, as long as m */
    Nat one = {big_alloc(num_words), num_words, num_words};
    nat_clear(one);
    one.p[0] = 1;

    enum { n = 4 };
    /* powers[i] contains x**i */
    Nat powers[1 << n];
    for (int i = 0; i < 1 << n; i++)
        powers[i] = NAT_NIL;
    powers[0] = nat_montgomery(powers[0], one, rr, m, k0, num_words);
    powers[1] = nat_montgomery(powers[1], x, rr, m, k0, num_words);
    for (int i = 2; i < 1 << n; i++)
        powers[i] =
            nat_montgomery(powers[i], powers[i - 1], powers[1], m, k0, num_words);

    /* z = 1, in Montgomery form */
    z = nat_make(z, num_words);
    nat_copy(z, powers[0]);

    zz = nat_make(zz, num_words);

    /* the same window, with Montgomery multiplications */
    for (Int i = y.len - 1; i >= 0; i--) {
        BigWord yi = y.p[i];
        for (Uint j = 0; j < BIG_W; j += n) {
            if (i != y.len - 1 || j != 0) {
                zz = nat_montgomery(zz, z, z, m, k0, num_words);
                z = nat_montgomery(z, zz, zz, m, k0, num_words);
                zz = nat_montgomery(zz, z, z, m, k0, num_words);
                z = nat_montgomery(z, zz, zz, m, k0, num_words);
            }
            zz = nat_montgomery(zz, z, powers[yi >> (BIG_W - n)], m, k0, num_words);
            Nat tmp = z;
            z = zz;
            zz = tmp;
            yi <<= n;
        }
    }
    /* back to a regular number */
    zz = nat_montgomery(zz, z, one, m, k0, num_words);

    /* One last reduction, just in case. See golang.org/issue/13907. */
    if (nat_cmp(zz, m) >= 0) {
        zz = nat_sub(zz, zz, m);
        if (nat_cmp(zz, m) >= 0)
            nat_div(NAT_NIL, NAT_NIL, zz, m, &zz);
    }

    return nat_norm(zz);
}

/* bytes: z big endian at the end of buf, returning the index of its first
 * byte. Panics when buf is too small. Used by crypto, so it must not leak
 * anything but the size of z through its timing. */
Int nat_bytes(Nat z, Slice buf) {
    Byte *b = buf.p;
    Int i = buf.len;
    for (Int k = 0; k < z.len; k++) {
        BigWord d = z.p[k];
        for (int j = 0; j < BIG_S; j++) {
            i--;
            if (i >= 0)
                b[i] = (Byte)d;
            else if ((Byte)d != 0)
                panic_str(BURROW_S("math/big: buffer too small to fit value"));
            d >>= 8;
        }
    }
    if (i < 0)
        i = 0;
    while (i < buf.len && b[i] == 0)
        i++;
    return i;
}

Nat nat_set_bytes(Nat z, Slice buf) {
    const Byte *b = buf.p;
    z = nat_make(z, (buf.len + BIG_S - 1) / BIG_S);
    Int i = buf.len;
    for (Int k = 0; i >= BIG_S; k++) {
        BigWord d = 0;
        for (int j = 0; j < BIG_S; j++)
            d = d << 8 | b[i - BIG_S + j];
        z.p[k] = d;
        i -= BIG_S;
    }
    if (i > 0) {
        BigWord d = 0;
        for (Uint s = 0; i > 0; s += 8) {
            d |= (BigWord)b[i - 1] << s;
            i--;
        }
        z.p[z.len - 1] = d;
    }
    return nat_norm(z);
}

/* sqrt: z = the floor of the square root of x. Brent and Zimmermann, Modern
 * Computer Arithmetic, algorithm 1.13. */
Nat nat_sqrt(Nat z, Nat x) {
    if (nat_cmp(x, NAT_ONE) <= 0)
        return nat_set(z, x);
    if (nat_alias(z, x))
        z = NAT_NIL;

    /* Start with a value known to be too large and repeat
     * z = (z + x/z)/2 until it stops getting smaller. */
    Nat z1 = z, z2 = NAT_NIL;
    z1 = nat_set_uint64(z1, 1);
    z1 = nat_lsh(z1, z1, (Uint)(nat_bit_len(x) + 1) / 2); /* must be >= sqrt(x) */
    for (int n = 0;; n++) {
        Nat r;
        z2 = nat_div(z2, NAT_NIL, x, z1, &r);
        z2 = nat_add(z2, z2, z1);
        z2 = nat_rsh(z2, z2, 1);
        if (nat_cmp(z2, z1) >= 0) {
            /* z1 is the answer. Which of z1 and z2 is z follows from the
             * number of rounds. */
            if ((n & 1) == 0)
                return z1;
            return nat_set(z, z1);
        }
        Nat t = z1;
        z1 = z2;
        z2 = t;
    }
}

/* subMod2N: z = (x - y) mod 2**n. */
Nat nat_sub_mod_2n(Nat z, Nat x, Nat y, Uint n) {
    if ((Uint)nat_bit_len(x) > n) {
        if (nat_alias(z, x))
            x = nat_trunc(x, x, n); /* ok to overwrite x in place */
        else
            x = nat_trunc(NAT_NIL, x, n);
    }
    if ((Uint)nat_bit_len(y) > n) {
        if (nat_alias(z, y))
            y = nat_trunc(y, y, n); /* ok to overwrite y in place */
        else
            y = nat_trunc(NAT_NIL, y, n);
    }
    if (nat_cmp(x, y) >= 0)
        return nat_sub(z, x, y);
    /* x - y < 0, and x - y mod 2**n = 1 + ^(y - x). */
    z = nat_sub(z, y, x);
    while ((Uint)z.len * BIG_W < n) {
        /* append(z, 0) */
        Int len = z.len;
        if (len < z.cap) {
            z.len++;
        } else {
            Nat t = nat_make(NAT_NIL, len + 1);
            nat_copy(t, z);
            z = t;
        }
        /* cap > len, so there is memory. */
        /* NOLINTNEXTLINE(clang-analyzer-core.NullDereference) */
        z.p[len] = 0;
    }
    for (Int i = 0; i < z.len; i++)
        z.p[i] = ~z.p[i];
    z = nat_trunc(z, z, n);
    return nat_add(z, z, NAT_ONE);
}

/* modInverse, through Int. */
Nat nat_mod_inverse(Nat z, Nat g, Nat n) {
    BigInt zi = {NULL, false, z};
    BigInt gi = {NULL, false, g};
    BigInt ni = {NULL, false, n};
    bi_mod_inverse(&zi, &gi, &ni);
    return zi.abs;
}

/* ---------------------------------------------------------------- natmul.go */

/* Operands with at least this many words use Karatsuba. */
#define BIG_KARATSUBA_THRESHOLD 40
#define BIG_BASIC_SQR_THRESHOLD 12
#define BIG_KARATSUBA_SQR_THRESHOLD 80

static void big_basic_mul(Nat z, Nat x, Nat y) {
    nat_clear(nat_to(z, x.len + y.len));
    for (Int i = 0; i < y.len; i++) {
        BigWord d = y.p[i];
        if (d != 0) {
            Nat zi = nat_slice(z, i, i + x.len);
            z.p[x.len + i] = big_add_mul_vvww(zi, zi, x, d, 0);
        }
    }
}

static void big_basic_sqr(Nat z, Nat x);
static void big_karatsuba(Nat z, Nat x, Nat y);
static void big_karatsuba_sqr(Nat z, Nat x);

Nat nat_mul_add_ww(Nat z, Nat x, BigWord y, BigWord r) {
    Int m = x.len;
    if (m == 0 || y == 0)
        return nat_set_word(z, r); /* result is r */
    z = nat_make(z, m + 1);
    z.p[m] = big_mul_add_vww(nat_to(z, m), x, y, r);
    return nat_norm(z);
}

Nat nat_mul(Nat z, Nat x, Nat y) {
    Int m = x.len;
    Int n = y.len;
    if (m < n)
        return nat_mul(z, y, x);
    if (m == 0 || n == 0)
        return nat_to(z, 0);
    if (n == 1)
        return nat_mul_add_ww(z, x, y.p[0], 0);
    /* m >= n > 1 */

    if (nat_alias(z, x) || nat_alias(z, y))
        z = NAT_NIL; /* z is an alias for x or y, cannot reuse */
    z = nat_make(z, m + n);

    if (n < BIG_KARATSUBA_THRESHOLD) {
        big_basic_mul(z, x, y);
        return nat_norm(z);
    }

    /* Karatsuba on the bottom n words of x, and the top words of x times y
     * added in, n words at a time. */
    big_karatsuba(nat_to(z, 2 * n), nat_to(x, n), y);

    if (n < m) {
        nat_clear(nat_from(z, 2 * n));
        ArenaMark mark = big_stk_save();
        Nat t = big_stk_nat(2 * n);
        for (Int i = n; i < m; i += n) {
            Int e = i + n < x.len ? i + n : x.len;
            t = nat_mul(t, nat_slice(x, i, e), y);
            nat_add_to(nat_from(z, i), t);
        }
        big_stk_restore(mark);
    }

    return nat_norm(z);
}

Nat nat_sqr(Nat z, Nat x) {
    Int n = x.len;
    if (n == 0)
        return nat_to(z, 0);
    if (n == 1) {
        BigWord d = x.p[0];
        z = nat_make(z, 2);
        z.p[1] = big_mul_ww(d, d, &z.p[0]);
        return nat_norm(z);
    }

    if (nat_alias(z, x))
        z = NAT_NIL; /* z is an alias for x, cannot reuse */
    z = nat_make(z, 2 * n);

    if (n < BIG_BASIC_SQR_THRESHOLD && n < BIG_KARATSUBA_SQR_THRESHOLD) {
        big_basic_mul(z, x, x);
        return nat_norm(z);
    }
    if (n < BIG_KARATSUBA_SQR_THRESHOLD) {
        big_basic_sqr(z, x);
        return nat_norm(z);
    }
    big_karatsuba_sqr(z, x);
    return nat_norm(z);
}

/* basicSqr: z = x*x, adding the products below the diagonal once and
 * doubling them, which is about twice as fast as basic_mul. */
static void big_basic_sqr(Nat z, Nat x) {
    Int n = x.len;
    if (n < BIG_BASIC_SQR_THRESHOLD) {
        big_basic_mul(z, x, x);
        return;
    }
    ArenaMark mark = big_stk_save();
    Nat t = big_stk_nat(2 * n);
    nat_clear(t);
    z.p[1] = big_mul_ww(x.p[0], x.p[0], &z.p[0]); /* the initial square */
    for (Int i = 1; i < n; i++) {
        BigWord d = x.p[i];
        /* z collects the squares x[i] * x[i] */
        z.p[2 * i + 1] = big_mul_ww(d, d, &z.p[2 * i]);
        /* t collects the products x[i] * x[j] where j < i */
        Nat ti = nat_slice(t, i, 2 * i);
        t.p[2 * i] = big_add_mul_vvww(ti, ti, nat_to(x, i), d, 0);
    }
    Nat tm = nat_slice(t, 1, 2 * n - 1);
    t.p[2 * n - 1] = big_lsh_vu(tm, tm, 1); /* double the j < i products */
    big_add_vv(z, z, t);                    /* combine the result */
    big_stk_restore(mark);
}

/* An Int over a piece of a vector, for karatsuba's signed middle term. */
static BigInt big_view(Nat abs) {
    BigInt r = {NULL, false, abs};
    return r;
}

/* karatsuba: z = x*y for x and y of n words and z of 2n. */
static void big_karatsuba(Nat z, Nat x, Nat y) {
    Int n = y.len;
    if (x.len != n || z.len != 2 * n)
        panic_str(BURROW_S("bad karatsuba length"));

    /* Switch to basic multiplication if numbers are odd or small. */
    if (n < BIG_KARATSUBA_THRESHOLD || n < 2) {
        big_basic_mul(z, x, y);
        return;
    }

    /* Let x = x1*b + x0 and y = y1*b + y0 with b = 2**(W*n2). Then
     *
     *     x*y = z2*b*b + z1*b + z0
     *     z2 = x1*y1, z0 = x0*y0
     *     z1 = (x1-x0)*(y0-y1) + z2 + z0
     *
     * which is three half size products instead of four. */
    Int n2 = (n + 1) / 2;
    BigInt x0 = big_view(nat_norm(nat_to(x, n2))),
           x1 = big_view(nat_norm(nat_from(x, n2)));
    BigInt y0 = big_view(nat_norm(nat_to(y, n2))),
           y1 = big_view(nat_norm(nat_from(y, n2)));
    BigInt z0 = big_view(nat_slice(z, 0, 2 * n2));
    BigInt z2 = big_view(nat_from(z, 2 * n2));

    /* Allocate temporary storage for z1; repurpose z0 to hold tx and ty. */
    ArenaMark mark = big_stk_save();
    BigInt z1 = big_view(big_stk_nat(2 * n2 + 1));
    BigInt tx = big_view(nat_slice(z, 0, n2));
    BigInt ty = big_view(nat_slice(z, n2, 2 * n2));

    bi_sub(&tx, &x0, &x1);
    bi_sub(&ty, &y1, &y0);
    z1.abs = nat_mul(z1.abs, tx.abs, ty.abs);
    z1.neg = z1.abs.len > 0 && tx.neg != ty.neg;

    nat_clear(z);
    z0.abs = nat_mul(z0.abs, x0.abs, y0.abs);
    z2.abs = nat_mul(z2.abs, x1.abs, y1.abs);
    bi_add(&z1, &z1, &z0);
    bi_add(&z1, &z1, &z2);
    nat_add_to(nat_from(z, n2), z1.abs);
    big_stk_restore(mark);
}

/* karatsubaSqr: z = x*x, the same with one fewer distinct product. */
static void big_karatsuba_sqr(Nat z, Nat x) {
    Int n = x.len;
    if (z.len != 2 * n)
        panic_str(BURROW_S("bad karatsubaSqr length"));

    if (n < BIG_KARATSUBA_SQR_THRESHOLD || n < 2) {
        big_basic_sqr(z, x);
        return;
    }

    Int n2 = (n + 1) / 2;
    BigInt x0 = big_view(nat_norm(nat_to(x, n2))),
           x1 = big_view(nat_norm(nat_from(x, n2)));
    BigInt z0 = big_view(nat_slice(z, 0, 2 * n2));
    BigInt z2 = big_view(nat_from(z, 2 * n2));

    ArenaMark mark = big_stk_save();
    BigInt z1 = big_view(big_stk_nat(2 * n2 + 1));
    BigInt tx = big_view(nat_slice(z, 0, n2));

    bi_sub(&tx, &x0, &x1);
    z1.abs = nat_sqr(z1.abs, tx.abs);
    z1.neg = true;

    nat_clear(z);
    z0.abs = nat_sqr(z0.abs, x0.abs);
    z2.abs = nat_sqr(z2.abs, x1.abs);
    bi_add(&z1, &z1, &z0);
    bi_add(&z1, &z1, &z2);
    nat_add_to(nat_from(z, n2), z1.abs);
    big_stk_restore(mark);
}

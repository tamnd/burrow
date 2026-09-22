/* Random numbers for the runtime's own use, seeded from the system.
 *
 * Not derived from Go's source. Go's runtime.rand is a chacha8 generator fed
 * from the kernel and refreshed periodically, which is more machinery than
 * anything in burrow needs yet. The contract is the same and the callers are
 * the same: map hash seeds, map iteration order, and anything else inside the
 * library that has to be unpredictable without asking the user for a source.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/runtime.h"

#include "burrow/pal.h"
#include "burrow/platform.h"

#include <stdbool.h>
#include <stdint.h>

/* Where the seed comes from is not this file's business any more. It is
 * pal_random_bytes, which is getrandom on Linux, getentropy on macOS and the
 * BSDs, and RtlGenRandom on Windows, each in its own file under src/pal/. What
 * used to be here was three of those spellings and the feature test macros each
 * one needed, which is exactly the shape the platform layer exists to remove. */

/* xoshiro256++, which is four words of state, no multiply, and passes the
 * statistical tests that matter for this use. The output is a sum of two
 * rotated words rather than one word of the state, so consecutive values do not
 * hand out the state the way a plain linear generator does. */
typedef struct RandState {
    uint64_t s[4];
    bool seeded;
} RandState;

/* Per thread, which is what makes this lock free. A generator shared between
 * threads would need an atomic, and it would be a place for two goroutines to
 * contend on every map insert over a value neither of them cares about the
 * exact bits of. */
static BURROW_THREAD_LOCAL RandState rand_state;

static uint64_t rotl(uint64_t x, unsigned k) {
    return (x << k) | (x >> (64 - k));
}

/* splitmix64, which is what xoshiro's author recommends for turning one seed
 * word into the four the generator needs. Handing the four words the same
 * value, or a value with most of its bits zero, gives a generator that takes a
 * long time to start looking random. */
static uint64_t splitmix64(uint64_t *x) {
    uint64_t z = (*x += 0x9e3779b97f4a7c15U);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9U;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebU;
    return z ^ (z >> 31);
}

static uint64_t system_seed(void) {
    uint64_t seed = 0;

    if (pal_random_bytes(&seed, sizeof seed, NULL) && seed != 0)
        return seed;

    /* Either there was nothing to ask or the ask failed. A failure here does
     * not stop the program. The caller wanted a hash seed, and a weak hash seed
     * is a hardening problem, while refusing to start is an availability
     * problem, which is worse.
     *
     * The mix below is not a substitute for entropy and is not presented as
     * one. It is the address of the thread's own state, the address of
     * something on the stack, and two clocks, which on a system with address
     * randomisation is a few tens of bits that differ between runs and on a
     * system without it is close to none. It keeps a map's iteration order from
     * being identical on every run, which is the property the library itself
     * depends on. It does not defend against somebody choosing keys on purpose,
     * and a port that needs that defence needs a real source in src/pal/
     * first. */
    {
        /* Two addresses from two different places, because a system that
         * randomises one of them does not always randomise the other. Both are
         * object pointers, since converting a function pointer to an integer is
         * something C does not define. */
        Uintptr state = (Uintptr)&rand_state;
        Uintptr frame = (Uintptr)&seed;
        seed ^= (uint64_t)state * 0x9e3779b97f4a7c15U;
        seed ^= (uint64_t)frame << 17;
    }

    /* Both clocks, because they are independent: the monotonic one says how
     * long this machine has been up and the wall clock says when the process
     * started, and a machine that gives away nothing through the first still
     * gives away the second. */
    seed ^= (uint64_t)pal_clock_monotonic() << 1;
    seed ^= (uint64_t)pal_clock_realtime() << 33;

    /* Zero is the one value the generator cannot be given, since an all zero
     * state produces all zero output for ever. */
    return seed == 0 ? 0x9e3779b97f4a7c15U : seed;
}

static void rand_seed(RandState *st) {
    uint64_t x = system_seed();
    st->s[0] = splitmix64(&x);
    st->s[1] = splitmix64(&x);
    st->s[2] = splitmix64(&x);
    st->s[3] = splitmix64(&x);
    st->seeded = true;
}

uint64_t runtime_rand64(void) {
    RandState *st = &rand_state;
    uint64_t result;
    uint64_t t;

    if (!st->seeded)
        rand_seed(st);

    result = rotl(st->s[0] + st->s[3], 23) + st->s[0];

    t = st->s[1] << 17;
    st->s[2] ^= st->s[0];
    st->s[3] ^= st->s[1];
    st->s[1] ^= st->s[2];
    st->s[0] ^= st->s[3];
    st->s[2] ^= t;
    st->s[3] = rotl(st->s[3], 45);

    return result;
}

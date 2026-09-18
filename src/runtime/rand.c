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

/* Before every include, because it only has an effect if <stdlib.h> has not
 * been seen yet and burrow's own headers are allowed to include it later.
 * _WIN32 rather than BURROW_OS_WINDOWS because platform.h is what defines that
 * and platform.h is an include. */
#ifdef _WIN32
#define _CRT_RAND_S
#endif

#include "burrow/runtime.h"

#include "burrow/platform.h"

#include <stdbool.h>
#include <stdint.h>

/* Where the seed comes from.
 *
 * On Linux it is getrandom. On every other Unix it is getentropy, which needs
 * no file descriptor, cannot be interrupted and cannot partially succeed, and
 * is the one call the BSDs and macOS all have.
 *
 * Linux is the exception because of musl. getentropy is declared there only
 * under _BSD_SOURCE or _GNU_SOURCE, neither of which is set when this library
 * is built as strict C11, and <sys/random.h> on musl does not declare it at
 * all. getrandom is declared by <sys/random.h> with no feature macro on musl
 * and on glibc both, so it is the call that works on every Linux libc without
 * giving up strict C11 and without reaching for _GNU_SOURCE. The price is that
 * it can be interrupted and can return short, which is what the loop in
 * system_seed is for, and that it needs a 3.17 kernel, which is 2014.
 *
 * On Windows it is rand_s, which is the CRT's wrapper over the system generator.
 * It needs _CRT_RAND_S defined before <stdlib.h>, which is what the block above
 * does, and it needs no library beyond the CRT. That last part is why it is used
 * here in preference to BCryptGenRandom: burrow links nothing today and that is
 * worth keeping. */
#if defined(BURROW_OS_LINUX)
#define BURROW_RAND_GETRANDOM 1
#include <errno.h>
#include <sys/random.h>
#include <sys/types.h> /* ssize_t, which getrandom returns */
#elif defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS) ||                           \
    defined(BURROW_OS_FREEBSD) || defined(BURROW_OS_NETBSD) ||                         \
    defined(BURROW_OS_DRAGONFLY) || defined(BURROW_OS_SOLARIS)
#define BURROW_RAND_GETENTROPY 1
#include <sys/random.h>
#elif defined(BURROW_OS_OPENBSD)
#define BURROW_RAND_GETENTROPY 1
#include <unistd.h>
#elif defined(BURROW_OS_WINDOWS)
#define BURROW_RAND_WINDOWS 1
#include <stdlib.h>
#else
/* Nowhere to ask. This is wasm, it is a freestanding target, and it is any
 * system somebody adds to platform.h before adding it here.
 *
 * The mix in system_seed is not a substitute for entropy and is not presented
 * as one. It is the address of the thread's own state, the address of something
 * on the stack, and the clock, which on a system with address randomisation is
 * a few tens of bits that differ between runs and on a system without it is
 * close to none. It keeps a map's iteration order from being identical on every run,
 * which is the property the library itself depends on. It does not defend
 * against somebody choosing keys on purpose, and a port that needs that defence
 * needs a real source here first. */
#define BURROW_RAND_NOTHING 1
#include <time.h>
#endif

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

#if defined(BURROW_RAND_GETRANDOM)
    {
        /* getrandom can come back with less than was asked for and can be cut
         * short by a signal, so it gets a loop where getentropy needs none. The
         * attempt count is a guard rather than a policy: a kernel that keeps
         * returning EINTR forever is a kernel this is not going to win against,
         * and falling through to the weak mix below is better than spinning.
         *
         * Zero flags, so no GRND_RANDOM and no GRND_NONBLOCK. This runs once
         * per thread at startup and the only case where it blocks is a machine
         * that has not gathered any entropy at all yet, where the right answer
         * is to wait rather than to take whatever is lying around. */
        unsigned char *p = (unsigned char *)&seed;
        size_t want = sizeof seed;
        int attempts = 0;

        while (want > 0 && attempts < 16) {
            ssize_t got = getrandom(p, want, 0);
            attempts++;
            if (got > 0) {
                p += (size_t)got;
                want -= (size_t)got;
                continue;
            }
            if (got < 0 && errno == EINTR)
                continue;
            break;
        }

        if (want == 0 && seed != 0)
            return seed;
    }
#elif defined(BURROW_RAND_GETENTROPY)
    if (getentropy(&seed, sizeof seed) == 0 && seed != 0)
        return seed;
#elif defined(BURROW_RAND_WINDOWS)
    {
        unsigned int lo = 0;
        unsigned int hi = 0;
        if (rand_s(&lo) == 0 && rand_s(&hi) == 0) {
            seed = ((uint64_t)hi << 32) | (uint64_t)lo;
            if (seed != 0)
                return seed;
        }
    }
#endif

    /* Either there was nothing to ask or the ask failed. A failure here does
     * not stop the program. The caller wanted a hash seed, and a weak hash seed
     * is a hardening problem, while refusing to start is an availability
     * problem, which is worse. */
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
#if defined(BURROW_RAND_NOTHING)
    seed ^= (uint64_t)time(NULL) << 1;
    seed ^= (uint64_t)clock() << 33;
#endif

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

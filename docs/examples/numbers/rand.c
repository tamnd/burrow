#include <stdio.h>
#include <string.h>

#include "burrow/burrow.h"
#include "burrow/math/rand.h"
#include "burrow/math/rand/v2.h"
#include "burrow/mem/arena.h"

static const char *deck[] = {"ace", "king", "queen", "jack", "ten"};

static void swap_cards(void *env, Int i, Int j) {
    const char **d = env;
    const char *t = d[i];
    d[i] = d[j];
    d[j] = t;
}

static void print_ints(const Int *n) {
    printf("%lld %lld %lld %lld %lld\n", (long long)n[0], (long long)n[1],
           (long long)n[2], (long long)n[3], (long long)n[4]);
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    // doc: global
    Int die = mathrand2_int_n(6) + 1; /* 1 to 6, different every run */
    double f = mathrand2_float64();   /* in [0, 1) */
    // doc: end
    printf("%d %d\n", die >= 1 && die <= 6, f >= 0 && f < 1);

    // doc: seeded
    Mathrand2PCG *pcg = mathrand2_new_pcg(a, 1, 2);
    Mathrand2Rand *r = mathrand2_new(a, mathrand2_pcg_as_source(pcg));
    Int n[5];
    for (int i = 0; i < 5; i++)
        n[i] = mathrand2_rand_int_n(r, 100); /* 76 61 78 79 23, every run */
    // doc: end
    print_ints(n);

    // doc: shuffle
    mathrand2_rand_shuffle(r, 5, BURROW_FN(SwapFunc, swap_cards, deck));
    // doc: end
    printf("%s %s %s %s %s\n", deck[0], deck[1], deck[2], deck[3], deck[4]);

    // doc: v1
    MathRandRand *old = math_rand_new(a, math_rand_new_source(a, 42));
    for (int i = 0; i < 5; i++)
        n[i] = math_rand_rand_intn(old, 100); /* 5 87 68 50 23, as Go's gives */
    // doc: end
    print_ints(n);

    // doc: chacha
    Byte seed[32];
    memcpy(seed, "ABCDEFGHIJKLMNOPQRSTUVWXYZ123456", 32);
    Mathrand2ChaCha8 *c = mathrand2_new_cha_cha8(a, seed);
    uint64_t first = mathrand2_cha_cha8_uint64(c);
    // doc: end
    printf("%#llx\n", (unsigned long long)first);

    arena_free(&ar);
    return 0;
}

/* Output:
1 1
76 61 78 79 23
ten queen jack king ace
5 87 68 50 23
0xb773b6063d4616a5
*/

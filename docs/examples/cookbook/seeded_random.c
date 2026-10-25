// doc: main
#define BURROW_SHORT 1
#include "burrow/burrow.h"
#include "burrow/math/rand/v2.h"

static void swap_names(void *env, Int i, Int j) {
    Str *names = env;
    Str t = names[i];
    names[i] = names[j];
    names[j] = t;
}

/* Rolls two dice a few times and deals out the names in a random order. With
 * the same seed it does the same thing every time, which is what a test or a
 * simulation you want to replay needs. */
static void play(Alloc *a, uint64_t seed) {
    Mathrand2PCG *pcg = mathrand2_new_pcg(a, seed, 0);
    Mathrand2Rand *r = mathrand2_new(a, mathrand2_pcg_as_source(pcg));
    fmt_printf_v("seed %d:", (Int)seed);
    for (int i = 0; i < 5; i++) {
        Int d1 = mathrand2_rand_int_n(r, 6) + 1, d2 = mathrand2_rand_int_n(r, 6) + 1;
        fmt_printf_v(" %d+%d", d1, d2);
    }
    Str names[] = {S("ada"), S("bob"), S("cy"), S("dee")};
    mathrand2_rand_shuffle(r, 4, FN(SwapFunc, swap_names, names));
    fmt_printf_v(", order %s %s %s %s\n", names[0], names[1], names[2], names[3]);
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    play(a, 1);
    play(a, 1);
    play(a, 2);

    /* The functions without a Rand use a source seeded at random, so they
     * give something new every run. None of these are fit for secrets: a
     * token or a key comes from crypto/rand. */
    Int roll = mathrand2_int_n(6) + 1;
    bool in_range = roll >= 1 && roll <= 6;
    fmt_printf_v("an unseeded roll is 1 to 6: %t\n", in_range);
    Str token = crypto_rand_text(a);
    fmt_printf_v("a token from crypto/rand has %d characters\n", token.len);
    arena_free(&ar);
}
// doc: end

/* Output:
seed 1: 4+1 5+1 5+4 5+4 2+1, order bob cy ada dee
seed 1: 4+1 5+1 5+4 5+4 2+1, order bob cy ada dee
seed 2: 5+6 3+2 1+5 1+4 6+3, order cy dee ada bob
an unseeded roll is 1 to 6: true
a token from crypto/rand has 26 characters
*/

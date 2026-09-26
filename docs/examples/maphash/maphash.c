#include <stdio.h>

#include "burrow/burrow.h"

static void oneshot(void) {
    // doc: oneshot
    MaphashSeed seed = maphash_make_seed();
    uint64_t a = maphash_string(seed, BURROW_S("hello"));
    uint64_t b = maphash_string(seed, BURROW_S("hello"));
    uint64_t c = maphash_string(maphash_make_seed(), BURROW_S("hello"));
    printf("same seed: %s\n", a == b ? "equal" : "different");
    printf("new seed: %s\n", a == c ? "equal" : "different");
    // doc: end
}

static void streaming(void) {
    // doc: stream
    MaphashHash h = {0};
    maphash_hash_write_string(&h, BURROW_S("hello, "), NULL);
    maphash_hash_write_string(&h, BURROW_S("world"), NULL);
    uint64_t sum = maphash_hash_sum64(&h);

    uint64_t whole = maphash_string(maphash_hash_seed(&h), BURROW_S("hello, world"));
    printf("pieces and whole: %s\n", sum == whole ? "equal" : "different");
    // doc: end
}

static void comparable(void) {
    // doc: comparable
    MaphashSeed seed = maphash_make_seed();
    Str x = BURROW_S("key");
    Byte copy[3] = {'k', 'e', 'y'};
    Str y = {copy, 3};
    uint64_t hx = maphash_comparable(seed, TYPE_STRING, &x);
    uint64_t hy = maphash_comparable(seed, TYPE_STRING, &y);
    printf("equal strings: %s\n", hx == hy ? "equal" : "different");
    // doc: end
}

int main(void) {
    oneshot();
    streaming();
    comparable();
    return 0;
}

/* Output:
same seed: equal
new seed: different
pieces and whole: equal
equal strings: equal
*/

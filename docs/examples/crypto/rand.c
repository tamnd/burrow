#include <stdint.h>
#include <stdio.h>

#include "burrow/burrow.h"

static void show_key(void) {
    // doc: read
    Byte key[32];
    crypto_rand_read(slice_from(key, 32, 32, TYPE_BYTE), NULL);
    // doc: end
    int zeros = 0;
    for (int i = 0; i < 32; i++)
        zeros += key[i] == 0;
    printf("%d\n", zeros < 32);
}

static void show_token(Alloc *a) {
    // doc: text
    Str token = crypto_rand_text(a);
    // doc: end
    printf("%d\n", (int)token.len);
}

static void show_dice(Alloc *a) {
    // doc: int
    BigInt *six = big_new_int(a, 6);
    BigInt *roll = crypto_rand_int(a, crypto_rand_reader, six, NULL);
    int64_t face = big_int_int64(roll) + 1;
    // doc: end
    printf("%d\n", face >= 1 && face <= 6);
}

static void show_prime(Alloc *a) {
    // doc: prime
    Error err;
    BigInt *p = crypto_rand_prime(a, crypto_rand_reader, 64, &err);
    // doc: end
    printf("%d %d\n", (int)big_int_bit_len(p), big_int_probably_prime(p, 20));
    crypto_rand_prime(a, crypto_rand_reader, 1, &err);
    printf("%.*s\n", (int)error_text(err).len, (const char *)error_text(err).p);
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    show_key();
    show_token(a);
    show_dice(a);
    show_prime(a);
    arena_free(&ar);
    return 0;
}

/* Output:
1
26
1
64 1
crypto/rand: prime size must be at least 2-bit
*/

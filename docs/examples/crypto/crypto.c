#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "burrow/burrow.h"

static Slice text(const char *s) {
    Int n = (Int)strlen(s);
    return slice_from((void *)(uintptr_t)s, n, n, TYPE_BYTE);
}

static void print_hex(Alloc *a, Slice b) {
    Str s = hex_encode_to_string(a, b);
    printf("%.*s\n", (int)s.len, (const char *)s.p);
}

static void by_number(Alloc *a) {
    // doc: hash
    CryptoHash h = CRYPTO_SHA256;
    Str name = crypto_hash_string(h, a);
    printf("%.*s, %d bytes\n", (int)name.len, (const char *)name.p,
           (int)crypto_hash_size(h));

    Hash d = crypto_hash_new(h, a);
    hash_write(d, text("abc"), NULL);
    print_hex(a, hash_sum(a, d, slice_nil(TYPE_BYTE)));
    // doc: end
}

static void available(Alloc *a) {
    // doc: available
    for (CryptoHash h = CRYPTO_MD4; h <= CRYPTO_SHA1; h++) {
        Str name = crypto_hash_string(h, a);
        printf("%.*s %s\n", (int)name.len, (const char *)name.p,
               crypto_hash_available(h) ? "yes" : "no");
    }
    // doc: end
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    by_number(a);
    available(a);
    arena_free(&ar);
    return 0;
}

/* Output:
SHA-256, 32 bytes
ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
MD4 no
MD5 yes
SHA-1 yes
*/

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

// doc: mac
static Slice sign(Alloc *a, Slice key, Slice message) {
    Hash mac = hmac_new(a, sha256_new, key);
    hash_write(mac, message, NULL);
    return hash_sum(a, mac, slice_nil(TYPE_BYTE));
}

static bool valid_mac(Alloc *a, Slice key, Slice message, Slice message_mac) {
    return hmac_equal(message_mac, sign(a, key, message));
}
// doc: end

static void mac(Alloc *a) {
    Slice key = text("my secret key");
    Slice tag = sign(a, key, text("pay bob 10"));
    print_hex(a, tag);
    printf("%d\n", valid_mac(a, key, text("pay bob 10"), tag));
    printf("%d\n", valid_mac(a, key, text("pay bob 99"), tag));
}

static void derive(Alloc *a) {
    Slice secret = text("the output of a key exchange");
    Slice salt = text("a random salt");
    // doc: hkdf
    Error err;
    Slice enc_key =
        hkdf_key(a, sha256_new, secret, salt, BURROW_S("encryption"), 16, &err);
    Slice mac_key = hkdf_key(a, sha256_new, secret, salt, BURROW_S("mac"), 16, &err);
    // doc: end
    print_hex(a, enc_key);
    print_hex(a, mac_key);

    hkdf_key(a, sha256_new, secret, salt, BURROW_S(""), 255 * 32 + 1, &err);
    printf("%.*s\n", (int)error_text(err).len, (const char *)error_text(err).p);
}

static void password(Alloc *a) {
    Slice salt = text("8 bytes!");
    // doc: pbkdf2
    Error err;
    Slice key =
        pbkdf2_key(a, sha256_new, BURROW_S("correct horse"), salt, 4096, 32, &err);
    if (BURROW_FAILED(err))
        return;
    // doc: end
    print_hex(a, key);
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    mac(a);
    derive(a);
    password(a);
    arena_free(&ar);
    return 0;
}

/* Output:
dac2bbf687c5038ce40f88b3f2c06a8554e7d773b57567829a48d145c5a02336
1
0
f1be524c671c3c3860e84cbdca3008e7
0732e779ded2e803f2dff3276ab29f0c
hkdf: requested key length too large
5190f725bf9273f9d7587638ba4d86633b0cf43a9e9c2b3587b2fd135050983c
*/

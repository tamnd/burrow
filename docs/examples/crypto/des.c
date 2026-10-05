#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "burrow/burrow.h"

static Slice text(const char *s) {
    Int n = (Int)strlen(s);
    return slice_from((void *)(uintptr_t)s, n, n, TYPE_BYTE);
}

static void print_text(Slice b) {
    printf("%.*s\n", (int)b.len, (const char *)b.p);
}

static void print_hex(Alloc *a, Slice b) {
    Str s = hex_encode_to_string(a, b);
    printf("%.*s\n", (int)s.len, (const char *)s.p);
}

static void print_error(Error err) {
    Str s = error_text(err);
    printf("%.*s\n", (int)s.len, (const char *)s.p);
}

static void triple_des(Alloc *a) {
    Error err;
    // doc: ede2
    /* Two key Triple DES, where the first key is used again at the end. */
    Slice ede2_key = text("example key 1234");
    Byte key[24];
    memcpy(key, ede2_key.p, 16);
    memcpy(key + 16, ede2_key.p, 8);
    CipherBlock block =
        des_new_triple_des_cipher(a, slice_from(key, 24, 24, TYPE_BYTE), &err);
    // doc: end
    if (BURROW_FAILED(err)) {
        print_error(err);
        return;
    }

    // doc: cbc
    Byte iv[DES_BLOCK_SIZE] = {0};
    Slice data = slice_make(a, TYPE_BYTE, 16, 16);
    memcpy(data.p, "exampleplaintext", 16);

    CipherBlockMode enc =
        cipher_new_cbc_encrypter(a, block, slice_from(iv, 8, 8, TYPE_BYTE));
    cipher_block_mode_crypt_blocks(enc, data, data);
    print_hex(a, data);

    CipherBlockMode dec =
        cipher_new_cbc_decrypter(a, block, slice_from(iv, 8, 8, TYPE_BYTE));
    cipher_block_mode_crypt_blocks(dec, data, data);
    print_text(data);
    // doc: end
}

static void rc4(Alloc *a) {
    Error err;
    // doc: rc4
    Rc4Cipher *c = rc4_new_cipher(a, text("Key"), &err);
    Slice data = slice_make(a, TYPE_BYTE, 9, 9);
    rc4_cipher_xor_key_stream(c, data, text("Plaintext"));
    print_hex(a, data);
    // doc: end
}

static void key_size(Alloc *a) {
    Error err;
    des_new_cipher(a, text("short"), &err);
    print_error(err);
    rc4_new_cipher(a, slice_nil(TYPE_BYTE), &err);
    print_error(err);
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    triple_des(a);
    rc4(a);
    key_size(a);
    arena_free(&ar);
    return 0;
}

/* Output:
c3b7f6b73a0ae2b8f9cb71b9f7e67fc7
exampleplaintext
bbf316e8d940af0ad3
crypto/des: invalid key size 5
crypto/rc4: invalid key size 0
*/

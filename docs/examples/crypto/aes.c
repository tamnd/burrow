#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "burrow/burrow.h"

static Slice text(const char *s) {
    Int n = (Int)strlen(s);
    return slice_from((void *)(uintptr_t)s, n, n, TYPE_BYTE);
}

static Slice unhex(Alloc *a, const char *s) {
    Error err;
    return hex_decode_string(a, str_from_cstr(s), &err);
}

static void print_text(Slice b) {
    printf("%.*s\n", (int)b.len, (const char *)b.p);
}

static void print_error(Error err) {
    Str s = error_text(err);
    printf("%.*s\n", (int)s.len, (const char *)s.p);
}

// doc: seal
static Slice encrypt(Alloc *a, Slice key, Slice plaintext, Error *err) {
    CipherBlock block = aes_new_cipher(a, key, err);
    if (BURROW_FAILED(*err))
        return slice_nil(TYPE_BYTE);
    CipherAEAD gcm = cipher_new_gcm(a, block, err);
    if (BURROW_FAILED(*err))
        return slice_nil(TYPE_BYTE);

    /* A fresh random nonce for every message, sent in front of it. */
    Slice nonce = slice_make(a, TYPE_BYTE, cipher_aead_nonce_size(gcm),
                             cipher_aead_nonce_size(gcm));
    crypto_rand_read(nonce, NULL);
    return cipher_aead_seal(gcm, a, nonce, nonce, plaintext, slice_nil(TYPE_BYTE));
}
// doc: end

// doc: open
static Slice decrypt(Alloc *a, Slice key, Slice message, Error *err) {
    CipherBlock block = aes_new_cipher(a, key, err);
    if (BURROW_FAILED(*err))
        return slice_nil(TYPE_BYTE);
    CipherAEAD gcm = cipher_new_gcm(a, block, err);
    if (BURROW_FAILED(*err))
        return slice_nil(TYPE_BYTE);

    Int n = cipher_aead_nonce_size(gcm);
    if (message.len < n) {
        *err = errors_new(a, BURROW_S("message too short"));
        return slice_nil(TYPE_BYTE);
    }
    return cipher_aead_open(gcm, a, slice_nil(TYPE_BYTE), slice_sub(message, 0, n),
                            slice_sub(message, n, message.len), slice_nil(TYPE_BYTE),
                            err);
}
// doc: end

static void gcm(Alloc *a) {
    Slice key =
        unhex(a, "6368616e676520746869732070617373776f726420746f206120736563726574");
    Error err;
    Slice message = encrypt(a, key, text("exampleplaintext"), &err);
    printf("%d bytes\n", (int)message.len);
    print_text(decrypt(a, key, message, &err));

    /* One flipped bit anywhere and Open says no. */
    ((Byte *)message.p)[20] ^= 1;
    decrypt(a, key, message, &err);
    print_error(err);
}

static void random_nonce(Alloc *a) {
    Slice key =
        unhex(a, "6368616e676520746869732070617373776f726420746f206120736563726574");
    Error err;
    // doc: random
    CipherBlock block = aes_new_cipher(a, key, &err);
    CipherAEAD gcm = cipher_new_gcm_with_random_nonce(a, block, &err);
    Slice sealed = cipher_aead_seal(gcm, a, slice_nil(TYPE_BYTE), slice_nil(TYPE_BYTE),
                                    text("exampleplaintext"), slice_nil(TYPE_BYTE));
    Slice opened = cipher_aead_open(gcm, a, slice_nil(TYPE_BYTE), slice_nil(TYPE_BYTE),
                                    sealed, slice_nil(TYPE_BYTE), &err);
    // doc: end
    printf("%d bytes\n", (int)sealed.len);
    print_text(opened);
}

static void cbc(Alloc *a) {
    Slice key = unhex(a, "6368616e676520746869732070617373");
    Slice ciphertext =
        unhex(a, "73c86d43a9d700a253a96c85b0f6b03ac9792e0e757f869cca306bd3cba1c62b");
    Error err;
    // doc: cbc
    CipherBlock block = aes_new_cipher(a, key, &err);
    Slice iv = slice_sub(ciphertext, 0, AES_BLOCK_SIZE);
    Slice data = slice_sub(ciphertext, AES_BLOCK_SIZE, ciphertext.len);
    CipherBlockMode mode = cipher_new_cbc_decrypter(a, block, iv);
    cipher_block_mode_crypt_blocks(mode, data, data);
    // doc: end
    print_text(data);
}

static void stream(Alloc *a) {
    Slice key = unhex(a, "6368616e676520746869732070617373");
    Error err;
    CipherBlock block = aes_new_cipher(a, key, &err);
    Byte iv[AES_BLOCK_SIZE] = {0};
    BytesReader in;
    bytes_reader_reset(&in, text("some secret text"));
    BytesBuffer out = BYTES_BUFFER(a);
    // doc: writer
    CipherStreamWriter w = {
        .s = cipher_new_ofb(a, block,
                            slice_from(iv, AES_BLOCK_SIZE, AES_BLOCK_SIZE, TYPE_BYTE)),
        .w = bytes_buffer_as_io_writer(&out),
    };
    io_copy(a, cipher_stream_writer_as_io_writer(&w), bytes_reader_as_io_reader(&in),
            &err);
    // doc: end
    Str s = hex_encode_to_string(a, bytes_buffer_bytes(&out));
    printf("%.*s\n", (int)s.len, (const char *)s.p);
}

static void key_size(Alloc *a) {
    Error err;
    aes_new_cipher(a, text("too short"), &err);
    print_error(err);
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    gcm(a);
    random_nonce(a);
    cbc(a);
    stream(a);
    key_size(a);
    arena_free(&ar);
    return 0;
}

/* Output:
44 bytes
exampleplaintext
cipher: message authentication failed
44 bytes
exampleplaintext
exampleplaintext
cf0495cc6f75dafc23948538e79904a9
crypto/aes: invalid key size 9
*/

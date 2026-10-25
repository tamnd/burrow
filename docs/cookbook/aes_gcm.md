# Encrypt a message with AES-GCM

AES-GCM encrypts and authenticates in one step, so a message that was changed after it was sealed fails to open instead of opening to something else. `cipher_new_gcm_with_random_nonce` makes a fresh random nonce for each message and puts it at the front of the sealed bytes, so there's no nonce to store or keep unique yourself. The cost is 28 bytes a message: 12 for the nonce and 16 for the tag.

<!-- example: ../examples/cookbook/aes_gcm.c#main -->
```c
#define BURROW_SHORT 1
#include "burrow/burrow.h"
#include "burrow/crypto/aes.h"
#include "burrow/crypto/cipher.h"
#include "burrow/crypto/rand.h"

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    /* A 32 byte key makes this AES-256. Keep it secret, and keep it: without
     * it nothing sealed with it can be opened again. */
    Error err = NO_ERROR;
    Slice key = slice_make(a, TYPE_BYTE, 32, 32);
    crypto_rand_read(key, &err);
    CipherBlock block = aes_new_cipher(a, key, &err);
    if (FAILED(err)) {
        fmt_println_v(err);
        return 1;
    }

    /* This GCM picks a new random nonce for every message and keeps it at the
     * front of the result, so there is no nonce to manage. Its overhead is the
     * nonce and the tag. */
    CipherAEAD gcm = cipher_new_gcm_with_random_nonce(a, block, &err);
    if (FAILED(err)) {
        fmt_println_v(err);
        return 1;
    }
    Slice none = slice_nil(TYPE_BYTE);
    Slice plain = BURROW_B("meet at the usual place, 9pm");
    Slice sealed = cipher_aead_seal(gcm, a, none, none, plain, none);
    Int overhead = cipher_aead_overhead(gcm);
    fmt_printf_v("%d bytes in, %d bytes sealed, %d of them overhead\n", plain.len,
                 sealed.len, overhead);

    Slice opened = cipher_aead_open(gcm, a, none, none, sealed, none, &err);
    if (FAILED(err)) {
        fmt_println_v(err);
        return 1;
    }
    Str text = str_from_bytes(opened.p, opened.len);
    fmt_printf_v("opened: %s\n", text);

    /* The tag covers every byte, so a message that was changed on the way does
     * not open at all, rather than opening to something else. */
    AT(Byte, sealed, sealed.len - 1) ^= 1;
    cipher_aead_open(gcm, a, none, none, sealed, none, &err);
    fmt_println_v("changed:", err);
    arena_free(&ar);
}
```

It prints:

```text
28 bytes in, 56 bytes sealed, 28 of them overhead
opened: meet at the usual place, 9pm
changed: cipher: message authentication failed
```

The program is [docs/examples/cookbook/aes_gcm.c](../examples/cookbook/aes_gcm.c). CI builds and runs it and checks that it prints this.

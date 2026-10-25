# Sign and verify with Ed25519

An Ed25519 key pair signs a message with the private key, and anyone with the public key can check the signature. Signing needs no randomness, so a key and a message always give the same signature. Changing a byte of either the message or the signature makes `ed25519_verify` return false.

<!-- example: ../examples/cookbook/ed25519_sign.c#main -->
```c
#define BURROW_SHORT 1
#include "burrow/burrow.h"
#include "burrow/crypto/ed25519.h"
#include "burrow/crypto/sha256.h"
#include "burrow/encoding/hex.h"

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    /* A real key comes from ed25519_generate_key with crypto/rand. This one
     * is made from a fixed seed so the program prints the same every time. */
    Sha256Sum256Ret seed = sha256_sum256(BURROW_B("a seed for the cookbook"));
    Slice seed_bytes =
        slice_from(seed.a, ED25519_SEED_SIZE, ED25519_SEED_SIZE, TYPE_BYTE);
    Ed25519PrivateKey priv = ed25519_new_key_from_seed(a, seed_bytes);
    Ed25519PublicKey pub = ed25519_private_key_public(priv, a);
    Str pub_hex = hex_encode_to_string(a, pub);
    fmt_println_v("public key:", pub_hex);

    /* Ed25519 needs no randomness to sign, so the same key and message give
     * the same signature. */
    Slice message = BURROW_B("pay bob 10 coins");
    Slice sig = ed25519_sign(a, priv, message);
    Str sig_hex = hex_encode_to_string(a, sig);
    fmt_println_v("signature:", sig_hex);

    /* Anyone with the public key can check it. Change one byte of the message
     * or of the signature and it no longer verifies. */
    bool good = ed25519_verify(pub, message, sig);
    bool changed = ed25519_verify(pub, BURROW_B("pay bob 99 coins"), sig);
    AT(Byte, sig, 0) ^= 1;
    bool bad_sig = ed25519_verify(pub, message, sig);
    fmt_printf_v("verifies: %t, other message: %t, damaged signature: %t\n", good,
                 changed, bad_sig);
    arena_free(&ar);
}
```

It prints:

```text
public key: bdc43d975a632f9c080ce8705074e6e6e24510c59955c43937fd5a124ae85520
signature: 5967f8031d1a777cd59703bc6bc8feb197b40a1b893f45d2c46606887f02efd8fad74aaaa2fe209451d4c7cc8d058911e72bd0eddf3f2d1f35ccd304c0b50e0e
verifies: true, other message: false, damaged signature: false
```

The program is [docs/examples/cookbook/ed25519_sign.c](../examples/cookbook/ed25519_sign.c). CI builds and runs it and checks that it prints this.

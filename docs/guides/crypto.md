# Cryptography

burrow's `crypto` packages are Go's, with the same algorithms, the same answers and the same panics. The hashes are covered in [hashing.md](hashing.md). This page covers the rest, one package at a time as they land.

## crypto

`burrow/crypto.h` is Go's top level `crypto` package. It names hash functions by number, so a signature or a certificate can say which hash it used without pulling in the package that implements it, and it has the interfaces every private key type fits: `CryptoSigner`, `CryptoDecrypter`, and the KEM pair `CryptoEncapsulator` and `CryptoDecapsulator`.

A `CryptoHash` gives the hash's name and digest size, and makes a new `Hash`:

<!-- example: ../examples/crypto/crypto.c#hash -->
```c
CryptoHash h = CRYPTO_SHA256;
Str name = crypto_hash_string(h, a);
printf("%.*s, %d bytes\n", (int)name.len, (const char *)name.p,
       (int)crypto_hash_size(h));

Hash d = crypto_hash_new(h, a);
hash_write(d, text("abc"), NULL);
print_hex(a, hash_sum(a, d, slice_nil(TYPE_BYTE)));
```

Go only has a hash when the program imports the package that registers it. burrow is one library, so MD5, SHA-1, the SHA-2 family and SHA-3 are always there. MD4, RIPEMD-160 and the BLAKE2 hashes are not in Go's standard library, and they stay unavailable until a program brings its own with `crypto_register_hash`. `crypto_hash_new` panics for a hash that is not available, so check first when the number comes from outside:

<!-- example: ../examples/crypto/crypto.c#available -->
```c
for (CryptoHash h = CRYPTO_MD4; h <= CRYPTO_SHA1; h++) {
    Str name = crypto_hash_string(h, a);
    printf("%.*s %s\n", (int)name.len, (const char *)name.p,
           crypto_hash_available(h) ? "yes" : "no");
}
```

`crypto_sign_message` signs a whole message with any `CryptoSigner`. It hashes the message with the hash the options name and hands the digest to the signer. A signer whose type has a `SignMessage` method in its method set gets the message as it is, which is how Go treats a `crypto.MessageSigner`. `burrow/crypto.h` shows the signature that method needs.

## crypto/subtle

`burrow/crypto/subtle.h` is what the other packages build on to keep secrets out of timing. Code that stops at the first wrong byte of a MAC, or branches on a bit of a key, takes a different time depending on the secret, and that difference can be measured from across a network. The functions here take the same time whatever the bytes are.

Checking a tag is the one most programs need:

<!-- example: ../examples/crypto/subtle.c#verify -->
```c
static bool tag_ok(Slice got, Slice want) {
    return subtle_constant_time_compare(got, want) == 1;
}
```

The time depends on the length and nothing else. Slices of different lengths compare unequal at once, which is fine because the length of a tag is not a secret.

The answers are `Int`s that are 1 or 0, as in Go, so one can be passed straight on as the choice in a select or a copy:

<!-- example: ../examples/crypto/subtle.c#select -->
```c
Int is_y = subtle_constant_time_byte_eq(secret, 'y');
Int n = subtle_constant_time_select(is_y, 10, 20);
```

<!-- example: ../examples/crypto/subtle.c#copy -->
```c
Byte key[4] = {1, 2, 3, 4};
Byte fallback[4] = {9, 9, 9, 9};
subtle_constant_time_copy(1 ^ is_y, bs(key, 4), bs(fallback, 4));
```

The copy writes every byte of `key` either way, so nothing about the choice shows in which memory was touched. Slices of different lengths panic with `subtle: slices have different lengths`, the same text as Go.

`subtle_xor_bytes` XORs two slices into a third and returns how many bytes it wrote, which is the shorter of the two inputs:

<!-- example: ../examples/crypto/subtle.c#xor -->
```c
Byte pad[5] = {0x10, 0x20, 0x30, 0x40, 0x50};
Byte msg[5] = {'h', 'e', 'l', 'l', 'o'};
Byte out[5];
Int n = subtle_xor_bytes(bs(out, 5), bs(msg, 5), bs(pad, 5));
```

The output may be one of the inputs, or apart from both. A destination that is too short, or one that overlaps an input without starting at the same place, panics before anything is written, as in Go.

### How the C stays constant time

Go has its compiler on its side here. It turns `x == y` in these functions into a single compare and set instruction, and nothing else. A C compiler makes no such promise, and is happy to turn a careful mask back into a branch when it can prove the two give the same answer. So burrow writes the comparisons as arithmetic on the bits and passes every mask through an empty asm statement, which tells GCC and Clang that the value could be anything and stops them reasoning past it. MSVC has no such statement, so the optimiser is turned off around these functions there. That makes them slower under MSVC and keeps them correct.

None of this helps code that branches on a secret itself, or uses one to pick which memory to read. The functions only keep their own promise.

### Data independent timing

Some processors have a mode in which the instructions that have it take the same time whatever their operands are. On arm64 it is DIT. `subtle_with_data_independent_timing` turns it on, runs a function, and turns it back off afterwards, including when the function panics:

<!-- example: ../examples/crypto/subtle.c#dit -->
```c
static void sign(void *env) {
    Slice *digest = env;
    /* Arithmetic on the key goes here. On arm64 it runs with DIT on, and so
     * does any goroutine it starts. */
    (void)digest;
}

static void sign_with_dit(Slice digest) {
    subtle_with_data_independent_timing(BURROW_FN(Func, sign, &digest));
}
```

The mode belongs to the thread, and a goroutine can move between threads, so the scheduler keeps a flag on the goroutine and sets the mode on whichever thread runs it, the way Go's runtime does. A goroutine started inside the function has the mode on too. Calls can be nested, and only the outermost one turns the mode off.

Where the processor has no such mode, which is everywhere but arm64 today, the function just runs. burrow looks for DIT on Linux, FreeBSD and Apple systems. Go also looks on OpenBSD, which burrow does not do yet, and neither looks on Windows.

`GODEBUG=dataindependenttiming=1` turns the mode on for every thread the runtime starts and leaves it on, which is Go's setting of the same name.

## crypto/rand

`burrow/crypto/rand.h` is where keys, nonces and tokens come from. The bytes come from the operating system every time, through getrandom on Linux, arc4random_buf or getentropy on macOS and the BSDs, RtlGenRandom on Windows and random_get on WASI, and burrow keeps no generator of its own that a fork could copy.

<!-- example: ../examples/crypto/rand.c#read -->
```c
Byte key[32];
crypto_rand_read(slice_from(key, 32, 32, TYPE_BYTE), NULL);
```

There is no error to check. As in Go, a read that fails ends the program with `fatal error: crypto/rand: failed to read random data`, because carrying on with a key that might not be random is worse than stopping. The `Error *` is there to match Go's signature and always comes back empty.

For something a person will see or type, `crypto_rand_text` gives 26 characters of base32, which is at least 128 bits:

<!-- example: ../examples/crypto/rand.c#text -->
```c
Str token = crypto_rand_text(a);
```

`crypto_rand_int` picks uniformly below a `BigInt`, which is the way to get a number in a range without the bias that taking a remainder adds:

<!-- example: ../examples/crypto/rand.c#int -->
```c
BigInt *six = big_new_int(a, 6);
BigInt *roll = crypto_rand_int(a, crypto_rand_reader, six, NULL);
int64_t face = big_int_int64(roll) + 1;
```

`crypto_rand_prime` makes a prime of an exact bit length:

<!-- example: ../examples/crypto/rand.c#prime -->
```c
Error err;
BigInt *p = crypto_rand_prime(a, crypto_rand_reader, 64, &err);
```

Both take a reader. Int reads from the one you give it, which is how a test makes it deterministic. Prime ignores it and uses the system generator, as Go has done since 1.26, unless `GODEBUG` has `cryptocustomrand=1`.

`crypto_rand_reader` is a variable, as `rand.Reader` is in Go, so a test can point it at a reader of its own. Do that before other threads start, because nothing synchronises it.

## crypto/hmac

`burrow/crypto/hmac.h` makes a message authentication code out of any hash and a key. The sender works out the MAC of a message and sends both, and the receiver, who has the same key, works it out again and compares:

<!-- example: ../examples/crypto/hmac.c#mac -->
```c
static Slice sign(Alloc *a, Slice key, Slice message) {
    Hash mac = hmac_new(a, sha256_new, key);
    hash_write(mac, message, NULL);
    return hash_sum(a, mac, slice_nil(TYPE_BYTE));
}

static bool valid_mac(Alloc *a, Slice key, Slice message, Slice message_mac) {
    return hmac_equal(message_mac, sign(a, key, message));
}
```

`hmac_new` takes the function that makes the hash rather than a hash, because it needs two of them, one inside the other. Any function of the shape `Hash f(Alloc *a)` will do, which `HashNewFunc` in `burrow/hash.h` names, and `sha256_new`, `sha512_new`, `sha1_new` and `md5_new` all are. The result is a `Hash` like any other, so writing, summing and resetting go through the usual calls.

Compare MACs with `hmac_equal`, never with `memcmp`. It is `subtle_constant_time_compare` underneath, so how long it takes does not say how much of a forged MAC was right.

Go's HMAC is also a `hash.Cloner` when the hash under it is one. A `Hash` here has no way to say that it is, so `hmac_new` does not offer cloning.

## crypto/hkdf

`burrow/crypto/hkdf.h` is RFC 5869. It turns a secret that is random enough but not in a usable shape, such as what a key exchange gives you, into as many keys as you need, telling them apart by an info string:

<!-- example: ../examples/crypto/hmac.c#hkdf -->
```c
Error err;
Slice enc_key =
    hkdf_key(a, sha256_new, secret, salt, BURROW_S("encryption"), 16, &err);
Slice mac_key = hkdf_key(a, sha256_new, secret, salt, BURROW_S("mac"), 16, &err);
```

`hkdf_key` is the two halves of the RFC in one call, and `hkdf_extract` and `hkdf_expand` are there for protocols that need them apart. Asking for more than 255 sums' worth of key fails with `hkdf: requested key length too large`, as in Go. HKDF assumes its secret is already hard to guess, so it is the wrong tool for a password.

## crypto/pbkdf2

`burrow/crypto/pbkdf2.h` is the one for passwords. It runs HMAC as many times as you ask, so that each guess costs an attacker that much work:

<!-- example: ../examples/crypto/hmac.c#pbkdf2 -->
```c
Error err;
Slice key =
    pbkdf2_key(a, sha256_new, BURROW_S("correct horse"), salt, 4096, 32, &err);
if (BURROW_FAILED(err))
    return;
```

The salt should be random, at least 8 bytes as the RFC recommends, different for each password and kept next to whatever the key protects. A higher iteration count makes each guess cost more and makes your own derivation slower by the same factor. A key length of zero or less, or one longer than the RFC allows, is an error rather than a panic.

None of the three has Go's FIPS 140-only mode, so the errors and panics that mode adds for short keys and unapproved hashes never happen here.

## crypto/aes and crypto/cipher

`burrow/crypto/aes.h` is the AES block cipher, and `burrow/crypto/cipher.h` is what turns a block cipher into something you can encrypt a message with. Most programs want one thing from the pair, AES-GCM, which encrypts and authenticates in one go:

<!-- example: ../examples/crypto/aes.c#seal -->
```c
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
```

The key has to be 16, 24 or 32 bytes, for AES-128, AES-192 or AES-256. Anything else fails with `crypto/aes: invalid key size 9` or whatever the length was. Never seal two messages with the same key and nonce: GCM loses both its secrecy and its authentication when that happens. Random 12 byte nonces are fine for up to about four billion messages a key.

Decrypting splits the nonce back off and hands the rest to Open:

<!-- example: ../examples/crypto/aes.c#open -->
```c
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
```

If a single bit of the message, the nonce or the additional data has changed, Open fails with `cipher: message authentication failed` and gives back nothing. It also zeroes whatever it had already written into `dst`, so you never see a plaintext that did not check out.

`cipher_new_gcm_with_random_nonce` does the nonce for you. Seal picks one and puts it in front, and Open takes it from there, so the nonce argument to both is empty:

<!-- example: ../examples/crypto/aes.c#random -->
```c
CipherBlock block = aes_new_cipher(a, key, &err);
CipherAEAD gcm = cipher_new_gcm_with_random_nonce(a, block, &err);
Slice sealed = cipher_aead_seal(gcm, a, slice_nil(TYPE_BYTE), slice_nil(TYPE_BYTE),
                                text("exampleplaintext"), slice_nil(TYPE_BYTE));
Slice opened = cipher_aead_open(gcm, a, slice_nil(TYPE_BYTE), slice_nil(TYPE_BYTE),
                                sealed, slice_nil(TYPE_BYTE), &err);
```

As in Go it wants an AES block and fails with `cipher: NewGCMWithRandomNonce requires aes.Block` for anything else. `cipher_new_gcm_with_nonce_size` and `cipher_new_gcm_with_tag_size` are there for protocols that fixed other sizes long ago. New code should not use them.

The older modes are all here too, CBC as a `CipherBlockMode` and CTR, OFB and CFB as a `CipherStream`. None of them authenticates anything, so they are for talking to things that already use them. CBC works on whole blocks in place:

<!-- example: ../examples/crypto/aes.c#cbc -->
```c
CipherBlock block = aes_new_cipher(a, key, &err);
Slice iv = slice_sub(ciphertext, 0, AES_BLOCK_SIZE);
Slice data = slice_sub(ciphertext, AES_BLOCK_SIZE, ciphertext.len);
CipherBlockMode mode = cipher_new_cbc_decrypter(a, block, iv);
cipher_block_mode_crypt_blocks(mode, data, data);
```

A stream goes in front of an `IoWriter` with `CipherStreamWriter`, or behind an `IoReader` with `CipherStreamReader`:

<!-- example: ../examples/crypto/aes.c#writer -->
```c
CipherStreamWriter w = {
    .s = cipher_new_ofb(a, block,
                        slice_from(iv, AES_BLOCK_SIZE, AES_BLOCK_SIZE, TYPE_BYTE)),
    .w = bytes_buffer_as_io_writer(&out),
};
io_copy(a, cipher_stream_writer_as_io_writer(&w), bytes_reader_as_io_reader(&in),
        &err);
```

Go's `StreamWriter.Close` closes the writer underneath if it is an `io.Closer`. C cannot ask an interface whether it is another one, so fill in the `closer` field when you want that, and leave it empty when you do not.

Where the processor has AES instructions, AES-NI on x86-64 or the Armv8 ones on arm64, the block cipher uses them, and CTR, CBC decryption and GCM hand them several blocks at a time. GCM's hash then runs on the carry-less multiply, PCLMULQDQ or PMULL. Without them burrow differs from Go on purpose: Go's portable AES looks up tables indexed by secret data, which can leak the key through the cache, while burrow's is bitsliced and takes the same time whatever the key and data are. The portable GHASH is Go's, which is already constant time.

## crypto/des and crypto/rc4

`burrow/crypto/des.h` is DES and Triple DES, and `burrow/crypto/rc4.h` is the RC4 stream cipher. All three are broken or close to it, and they are here only for old protocols and files that still use them. For anything new, use AES-GCM from the section above.

Triple DES takes a 24 byte key, three DES keys in a row. Two key Triple DES, which some old systems use, is the same thing with the first key repeated at the end:

<!-- example: ../examples/crypto/des.c#ede2 -->
```c
/* Two key Triple DES, where the first key is used again at the end. */
Slice ede2_key = text("example key 1234");
Byte key[24];
memcpy(key, ede2_key.p, 16);
memcpy(key + 16, ede2_key.p, 8);
CipherBlock block =
    des_new_triple_des_cipher(a, slice_from(key, 24, 24, TYPE_BYTE), &err);
```

The block works with any of the modes in `burrow/crypto/cipher.h`, just like an AES one, only with 8 byte blocks:

<!-- example: ../examples/crypto/des.c#cbc -->
```c
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
```

RC4 has no block and no IV. A key of 1 to 256 bytes gives an `Rc4Cipher`, and every call to `rc4_cipher_xor_key_stream` continues the key stream where the last one stopped:

<!-- example: ../examples/crypto/des.c#rc4 -->
```c
Rc4Cipher *c = rc4_new_cipher(a, text("Key"), &err);
Slice data = slice_make(a, TYPE_BYTE, 9, 9);
rc4_cipher_xor_key_stream(c, data, text("Plaintext"));
print_hex(a, data);
```

`rc4_cipher_as_cipher_stream` turns an `Rc4Cipher` into a `CipherStream`, so it can go into a `CipherStreamReader` or `CipherStreamWriter`. A DES key that is not 8 bytes fails with `crypto/des: invalid key size 5` or whatever the length was, and a Triple DES key that is not 24 bytes fails the same way. RC4 fails with `crypto/rc4: invalid key size 0` for an empty key or one longer than 256 bytes. Like Go's, none of these three is constant time. They look up tables with bits of the key, so on a shared machine the key can leak through the cache.

## crypto/ed25519

`burrow/crypto/ed25519.h` signs and verifies with Ed25519, RFC 8032. A private key is 64 bytes, the 32 byte seed followed by the public key, and both are plain byte slices. `ed25519_generate_key` makes a key pair from the system's random source when the reader is nil:

<!-- example: ../examples/crypto/ed25519.c#generate -->
```c
Error err = BURROW_NO_ERROR;
Ed25519PrivateKey priv;
Ed25519PublicKey pub = ed25519_generate_key(a, (IoReader){NULL, NULL}, &priv, &err);
if (BURROW_FAILED(err))
    return;
```

A seed always gives the same key, and Ed25519 signatures are deterministic, so the same key and message always give the same signature:

<!-- example: ../examples/crypto/ed25519.c#sign -->
```c
Ed25519PrivateKey priv =
    ed25519_new_key_from_seed(a, text("an ed25519 seed is 32 bytes long"));
Ed25519PublicKey pub = ed25519_private_key_public(priv, a);

Slice msg = text("The quick brown fox jumps over the lazy dog");
Slice sig = ed25519_sign(a, priv, msg);
```

`ed25519_verify` says whether a signature is good. It takes nothing to be secret, so it is fine to call on what a peer sent:

<!-- example: ../examples/crypto/ed25519.c#verify -->
```c
printf("%d\n", ed25519_verify(pub, msg, sig));
printf("%d\n", ed25519_verify(
                   pub, text("The quick brown fox jumps over the lazy cat"), sig));
```

`Ed25519Options` picks the other two variants of RFC 8032. A context string on its own gives Ed25519ctx, and a hash of `CRYPTO_SHA512` gives Ed25519ph, which signs a SHA-512 digest of the message rather than the message. A signature made with one context does not verify with another:

<!-- example: ../examples/crypto/ed25519.c#context -->
```c
Error err = BURROW_NO_ERROR;
Ed25519Options opts = {0, BURROW_S("Example_ed25519ctx")};
Slice ctx_sig =
    ed25519_private_key_sign(priv, a, (IoReader){NULL, NULL}, msg,
                             ed25519_options_as_signer_opts(&opts), &err);
if (BURROW_FAILED(err))
    return;
print_error(ed25519_verify_with_options(pub, msg, ctx_sig, &opts));

Ed25519Options other = {0, BURROW_S("another context")};
print_error(ed25519_verify_with_options(pub, msg, ctx_sig, &other));
```

`ed25519_private_key_signer` turns a private key into a `CryptoSigner`, for code that signs through the interface. Its public half comes back from `crypto_signer_public` as a `CryptoPublicKey` holding an `Ed25519PublicKey`, which `ed25519_public_key_equal` compares with. `ed25519_sign` panics if the private key is not 64 bytes, and `ed25519_verify` panics if the public key is not 32, as Go's do. Signing and key generation are constant time. Verifying is not, and does not need to be.

## crypto/ecdh

`burrow/crypto/ecdh.h` is Elliptic Curve Diffie-Hellman over P-256, P-384, P-521 and X25519. Two sides each make a private key, send each other the public half as bytes, and each gets the same shared secret from its own private key and the other's public key:

<!-- example: ../examples/crypto/ecdh.c#exchange -->
```c
Error err = BURROW_NO_ERROR;
const EcdhCurve *curve = ecdh_x25519();
EcdhPrivateKey *alice =
    ecdh_curve_generate_key(curve, a, (IoReader){NULL, NULL}, &err);
EcdhPrivateKey *bob =
    ecdh_curve_generate_key(curve, a, (IoReader){NULL, NULL}, &err);
if (BURROW_FAILED(err))
    return;

// Each side sends the other its public key, as bytes.
Slice alice_sends = ecdh_public_key_bytes(ecdh_private_key_public_key(alice), a);
Slice bob_sends = ecdh_public_key_bytes(ecdh_private_key_public_key(bob), a);

EcdhPublicKey *from_bob = ecdh_curve_new_public_key(curve, a, bob_sends, &err);
EcdhPublicKey *from_alice = ecdh_curve_new_public_key(curve, a, alice_sends, &err);
if (BURROW_FAILED(err))
    return;
Slice s1 = ecdh_private_key_ecdh(alice, a, from_bob, &err);
Slice s2 = ecdh_private_key_ecdh(bob, a, from_alice, &err);
if (BURROW_FAILED(err))
    return;
printf("%d %d\n", (int)s1.len, bytes_equal(s1, s2));
```

The curves are the four values `ecdh_p256`, `ecdh_p384`, `ecdh_p521` and `ecdh_x25519` return, so `==` tells whether two keys are on the same curve. Keys come from the allocator they are made with, in one block each, and `ecdh_private_key_free` and `ecdh_public_key_free` give them back. `ecdh_curve_new_private_key` and `ecdh_curve_new_public_key` take the encodings `ecdh_private_key_bytes` and `ecdh_public_key_bytes` give, so a fixed key gives a fixed result. This is the X25519 example of RFC 7748:

<!-- example: ../examples/crypto/ecdh.c#vector -->
```c
Error err = BURROW_NO_ERROR;
EcdhPrivateKey *k = ecdh_curve_new_private_key(
    ecdh_x25519(), a,
    unhex(a, "77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a"),
    &err);
EcdhPublicKey *peer = ecdh_curve_new_public_key(
    ecdh_x25519(), a,
    unhex(a, "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f"),
    &err);
if (BURROW_FAILED(err))
    return;
print_hex(a, ecdh_public_key_bytes(ecdh_private_key_public_key(k), a));
print_hex(a, ecdh_private_key_ecdh(k, a, peer, &err));
```

A shared secret is not a key yet. It is not uniformly random, and wants a key derivation function like HKDF first. A public key on another curve and an encoding that is not a point are errors with Go's messages:

<!-- example: ../examples/crypto/ecdh.c#errors -->
```c
Error err = BURROW_NO_ERROR;
EcdhPrivateKey *k =
    ecdh_curve_generate_key(ecdh_p256(), a, (IoReader){NULL, NULL}, &err);
EcdhPrivateKey *other =
    ecdh_curve_generate_key(ecdh_p384(), a, (IoReader){NULL, NULL}, &err);
if (BURROW_FAILED(err))
    return;
ecdh_private_key_ecdh(k, a, ecdh_private_key_public_key(other), &err);
print_error(err);

err = BURROW_NO_ERROR;
ecdh_curve_new_public_key(ecdh_p256(), a, unhex(a, "00"), &err);
print_error(err);
```

For the NIST curves a public key is the uncompressed point, a 4 then x and y, and the shared secret is the x coordinate of the shared point. Compressed points and the point at infinity are refused. For X25519 any 32 bytes are a public key, and a key of small order shows up as an error from `ecdh_private_key_ecdh`, which refuses to return a secret of all zeros. The field arithmetic is the same fiat-crypto code Go uses, and the scalar multiplications follow Go's constant time ones step for step.

## crypto/elliptic

`burrow/crypto/elliptic.h` is the old interface to the NIST curves, with points as `BigInt` coordinates. Go deprecates most of it: for key exchange use crypto/ecdh, and for signatures crypto/ecdsa. It is here for code that still speaks it, and it gives the same answers Go's does. An `EllipticCurve` is a vtable and a pointer, and `elliptic_p224`, `elliptic_p256`, `elliptic_p384` and `elliptic_p521` give the four curves. Each operation returns x and puts y in its last argument, both new from the allocator, and that argument can be NULL when only x is wanted:

<!-- example: ../examples/crypto/elliptic.c#points -->
```c
EllipticCurve p256 = elliptic_p256();
const EllipticCurveParams *params = elliptic_curve_params(p256);

// 2G, two ways.
uint8_t two[] = {2};
BigInt *y1;
BigInt *x1 =
    elliptic_curve_scalar_base_mult(p256, a, slice_from(two, 1, 1, TYPE_BYTE), &y1);
BigInt *y2;
BigInt *x2 = elliptic_curve_double(p256, a, params->gx, params->gy, &y2);
print_int(a, x1);
printf("%d %d\n", big_int_cmp(x1, x2) == 0 && big_int_cmp(y1, y2) == 0,
       elliptic_curve_is_on_curve(p256, x1, y1));

// G + 2G is 3G.
uint8_t three[] = {3};
BigInt *y3;
BigInt *x3 = elliptic_curve_add(p256, a, params->gx, params->gy, x1, y1, &y3);
BigInt *x4 = elliptic_curve_scalar_base_mult(
    p256, a, slice_from(three, 1, 1, TYPE_BYTE), NULL);
printf("%d\n", big_int_cmp(x3, x4) == 0);
```

`elliptic_marshal` writes the uncompressed form, a 4 then x and y, and `elliptic_marshal_compressed` the compressed form, a 2 or 3 for the sign of y then x. The unmarshal functions return NULL for anything that is not a point on the curve, and the point at infinity, (0, 0), is not one. Passing a point that is not on the curve to the operations or to the marshal functions panics with Go's message:

<!-- example: ../examples/crypto/elliptic.c#encoding -->
```c
EllipticCurve p256 = elliptic_p256();
const EllipticCurveParams *params = elliptic_curve_params(p256);

Slice full = elliptic_marshal(a, p256, params->gx, params->gy);
Slice small = elliptic_marshal_compressed(a, p256, params->gx, params->gy);
printf("%d %d\n", (int)full.len, (int)small.len);
print_hex(a, small);

BigInt *y;
BigInt *x = elliptic_unmarshal_compressed(a, p256, small, &y);
printf("%d\n", big_int_cmp(x, params->gx) == 0 && big_int_cmp(y, params->gy) == 0);

// A point that is not on the curve does not unmarshal.
x = elliptic_unmarshal(a, p256, unhex(a, "0400"), &y);
printf("%d %d\n", x == NULL, y == NULL);
```

The four curves use the same constant time field code as crypto/ecdh, with only the conversions to and from `BigInt` left variable time. An `EllipticCurveParams` other than the four curves' own params gets a generic implementation in math/big, which is what Go's tests compare the fast code against. A copy of the P-384 params is enough to get it:

<!-- example: ../examples/crypto/elliptic.c#generic -->
```c
// A copy of the params is the generic implementation of the same curve.
EllipticCurveParams copy = *elliptic_curve_params(elliptic_p384());
EllipticCurve slow = elliptic_curve_params_as_elliptic_curve(&copy);

Error err = BURROW_NO_ERROR;
BigInt *x, *y;
Slice priv =
    elliptic_generate_key(a, elliptic_p384(), (IoReader){NULL, NULL}, &x, &y, &err);
if (BURROW_FAILED(err))
    return;
BigInt *gy;
BigInt *gx = elliptic_curve_scalar_base_mult(slow, a, priv, &gy);
printf("%d %d\n", (int)priv.len,
       big_int_cmp(x, gx) == 0 && big_int_cmp(y, gy) == 0);
```

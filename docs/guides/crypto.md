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

## crypto/fips140

`burrow/crypto/fips140.h` answers whether the program is running in FIPS 140-3 mode. In burrow the answer is always no:

<!-- example: ../examples/crypto/fips140.c#enabled -->
```c
if (fips140_enabled())
    printf("FIPS 140-3 mode\n");
else
    printf("not in FIPS 140-3 mode\n");
```

Go ships a FIPS 140-3 module, a fixed set of its crypto packages that has been through validation, and `GODEBUG=fips140=on` makes a Go program use only that module, check its own code against a checksum when it starts and run the self tests the standard asks for. burrow's code follows the same module, but it has not been validated, and it does not claim to be.

So when `GODEBUG` asks for the mode with `fips140=on`, `only` or `debug`, `fips140_enabled` and `fips140_enforced` panic with `fips140: FIPS 140-3 mode is not supported by burrow`. That is what a Go program does at startup on a platform where the mode is not supported, and it is better than letting a program that asked for FIPS mode carry on without it. An unknown value panics as it does in Go, and `off` or no setting at all gives false. The setting is read the first time either function is called and never again.

`fips140_version` gives `"latest"`, which is what Go gives for a program that was not built against a frozen module. `fips140_without_enforcement` runs a function with strict enforcement off, and since enforcement is never on here, it just runs it:

<!-- example: ../examples/crypto/fips140.c#without -->
```c
fips140_without_enforcement(BURROW_FN(Func, legacy, NULL));
```

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

## crypto/ecdsa

`burrow/crypto/ecdsa.h` is the Elliptic Curve Digital Signature Algorithm of FIPS 186-5 over P-224, P-256, P-384 and P-521. A key is made for a curve, signs a digest, which is the hash of the message rather than the message, and the public half checks the signature. `ecdsa_sign_asn1` gives the DER encoding X.509 and TLS use:

<!-- example: ../examples/crypto/ecdsa.c#sign -->
```c
Error err = BURROW_NO_ERROR;
EcdsaPrivateKey *priv =
    ecdsa_generate_key(a, elliptic_p256(), (IoReader){NULL, NULL}, &err);
if (BURROW_FAILED(err))
    return;

Sha256Sum256Ret hash = sha256_sum256(text("hello, world"));
Slice h = slice_from(hash.a, 32, 32, TYPE_BYTE);

Slice sig = ecdsa_sign_asn1(a, (IoReader){NULL, NULL}, priv, h, &err);
if (BURROW_FAILED(err))
    return;

bool valid = ecdsa_verify_asn1(&priv->public_key, h, sig);
printf("signature verified: %s\n", valid ? "true" : "false");
```

The random source is ignored for the four NIST curves, as it is in Go from 1.26, and the bytes come from the system's generator, so a nil `IoReader` is the usual thing to pass. The nonce for each signature comes out of an HMAC_DRBG seeded from those bytes, the private key and the digest, so a broken generator does not give the key away. Two signatures of the same digest still differ. When a signature has to be the same every time, `ecdsa_private_key_sign` with a nil reader and the hash named in the options gives the deterministic signature of RFC 6979. This is the P-256 example of its appendix A.2.5:

<!-- example: ../examples/crypto/ecdsa.c#deterministic -->
```c
// The P-256 key of RFC 6979, appendix A.2.5.
Error err = BURROW_NO_ERROR;
EcdsaPrivateKey *priv = ecdsa_parse_raw_private_key(
    a, elliptic_p256(),
    unhex(a, "c9afa9d845ba75166b5c215767b1d6934e50c3db36e89b127b8a622b120f6721"),
    &err);
if (BURROW_FAILED(err))
    return;

Sha256Sum256Ret hash = sha256_sum256(text("sample"));
CryptoHash sha256 = CRYPTO_SHA256;
Slice sig = ecdsa_private_key_sign(priv, a, (IoReader){NULL, NULL},
                                   slice_from(hash.a, 32, 32, TYPE_BYTE),
                                   crypto_hash_as_signer_opts(&sha256), &err);
if (BURROW_FAILED(err))
    return;
print_hex(a, sig);
```

`ecdsa_public_key_bytes` gives the uncompressed point and `ecdsa_private_key_bytes` the scalar, as long as the curve's order. `ecdsa_parse_uncompressed_public_key` and `ecdsa_parse_raw_private_key` take them back and check them, and the `_equal` functions compare keys in constant time:

<!-- example: ../examples/crypto/ecdsa.c#encoding -->
```c
Error err = BURROW_NO_ERROR;
EcdsaPrivateKey *priv =
    ecdsa_generate_key(a, elliptic_p384(), (IoReader){NULL, NULL}, &err);
if (BURROW_FAILED(err))
    return;

Slice pub_bytes = ecdsa_public_key_bytes(&priv->public_key, a, &err);
Slice priv_bytes = ecdsa_private_key_bytes(priv, a, &err);
if (BURROW_FAILED(err))
    return;
printf("%d %d\n", (int)pub_bytes.len, (int)priv_bytes.len);

EcdsaPublicKey *pub =
    ecdsa_parse_uncompressed_public_key(a, elliptic_p384(), pub_bytes, &err);
EcdsaPrivateKey *back =
    ecdsa_parse_raw_private_key(a, elliptic_p384(), priv_bytes, &err);
if (BURROW_FAILED(err))
    return;
printf("%d %d\n",
       ecdsa_public_key_equal(&priv->public_key,
                              BURROW_ANY(TYPE_ECDSA_PUBLIC_KEY, pub)),
       ecdsa_private_key_equal(priv, BURROW_ANY(TYPE_ECDSA_PRIVATE_KEY, back)));
```

Keys and signatures come from the allocator they are made with, and `ecdsa_private_key_free` and `ecdsa_public_key_free` give a key back. `ecdsa_sign` and `ecdsa_verify` are the older interface with r and s as `BigInt` values, and `ecdsa_private_key_signer` makes a key a `CryptoSigner`. An empty digest, a scalar of the wrong length and an encoding that is not a point are errors with Go's messages:

<!-- example: ../examples/crypto/ecdsa.c#errors -->
```c
Error err = BURROW_NO_ERROR;
EcdsaPrivateKey *priv =
    ecdsa_generate_key(a, elliptic_p256(), (IoReader){NULL, NULL}, &err);
if (BURROW_FAILED(err))
    return;
ecdsa_sign_asn1(a, (IoReader){NULL, NULL}, priv, (Slice){0}, &err);
print_error(err);

err = BURROW_NO_ERROR;
ecdsa_parse_raw_private_key(a, elliptic_p256(), unhex(a, "0102"), &err);
print_error(err);

err = BURROW_NO_ERROR;
ecdsa_parse_uncompressed_public_key(a, elliptic_p256(), unhex(a, "00"), &err);
print_error(err);
```

## crypto/dsa

`burrow/crypto/dsa.h` is the Digital Signature Algorithm of FIPS 186-3. Go deprecates it and so does this: it is here for checking signatures made by old systems, and new code should use Ed25519. Keys are structs of `BigInt` pointers, the shared parameters p, q and g, then y for the public key and x for the private one, so a key from somewhere else can be put together with `big_int_set_string` or `big_int_set_bytes`. `dsa_sign` gives the signature as two integers, r and s. The hash is not cut down to the length of q for you, and FIPS 186-3 says it should be:

<!-- example: ../examples/crypto/dsa.c#sign -->
```c
DsaPrivateKey priv = test_key(a);

// q is 160 bits, so the SHA-256 hash is cut down to its first 20 bytes.
Sha256Sum256Ret sum = sha256_sum256(text("hello, world"));
Int n = big_int_bit_len(priv.public_key.parameters.q) / 8;
Slice hash = slice_from(sum.a, n, n, TYPE_BYTE);

Error err = BURROW_NO_ERROR;
BigInt *s;
BigInt *r = dsa_sign(a, (IoReader){NULL, NULL}, &priv, hash, &s, &err);
if (BURROW_FAILED(err))
    return;

printf("signature verified: %s\n",
       dsa_verify(&priv.public_key, hash, r, s) ? "true" : "false");
sum.a[0] ^= 1;
printf("other hash verified: %s\n",
       dsa_verify(&priv.public_key, hash, r, s) ? "true" : "false");
```

`dsa_generate_key` makes x and y for parameters that are already in the key. Making the parameters themselves with `dsa_generate_parameters` means finding two primes and can take several seconds. Many keys can share one set:

<!-- example: ../examples/crypto/dsa.c#generate -->
```c
// New keys for parameters someone already has. dsa_generate_parameters
// makes new ones, which takes seconds.
DsaPrivateKey shared = test_key(a);
DsaPrivateKey priv = {0};
priv.public_key.parameters = shared.public_key.parameters;

Error err = dsa_generate_key(&priv, a, (IoReader){NULL, NULL});
if (BURROW_FAILED(err))
    return;
printf("x < q: %s\n",
       big_int_cmp(priv.x, priv.public_key.parameters.q) < 0 ? "true" : "false");
printf("y < p: %s\n",
       big_int_cmp(priv.public_key.y, priv.public_key.parameters.p) < 0 ? "true"
                                                                        : "false");
```

The functions that only report an error return it, as Go's do. `dsa_sign` gives `dsa_err_invalid_public_key` for a key it cannot sign with, which a key made by other code can be, so check for it with `errors_is`:

<!-- example: ../examples/crypto/dsa.c#errors -->
```c
DsaParameters params = {0};
print_error(dsa_generate_parameters(&params, a, (IoReader){NULL, NULL}, 7));

DsaPrivateKey empty = {0};
print_error(dsa_generate_key(&empty, a, (IoReader){NULL, NULL}));

// A q whose length in bits is not a multiple of 8.
DsaPrivateKey bad = test_key(a);
bad.public_key.parameters.q = from_hex(a, "7F");
Error err = BURROW_NO_ERROR;
BigInt *s;
dsa_sign(a, (IoReader){NULL, NULL}, &bad, text("hash"), &s, &err);
print_error(err);
printf("%d\n", errors_is(err, dsa_err_invalid_public_key));
```

## crypto/mlkem

`burrow/crypto/mlkem.h` is ML-KEM, the key encapsulation method of FIPS 203 that was called Kyber. It is meant to hold up against a quantum computer. One side makes a decapsulation key and sends out the encapsulation key that goes with it. The other side uses that to make a shared key and a ciphertext, and sends back the ciphertext, which only the decapsulation key gets the shared key out of. Here Bob is a function, as he would be on another machine:

<!-- example: ../examples/crypto/mlkem.c#bob -->
```c
// Bob gets Alice's encapsulation key, makes a shared key with it, and sends
// back the ciphertext that carries it.
static Slice bob(Alloc *a, Slice encapsulation_key, Slice *shared_key) {
    Error err = BURROW_NO_ERROR;
    MlkemEncapsulationKey768 *ek =
        mlkem_new_encapsulation_key768(a, encapsulation_key, &err);
    if (BURROW_FAILED(err))
        return slice_nil(TYPE_BYTE);
    Slice ciphertext;
    *shared_key = mlkem_encapsulation_key768_encapsulate(ek, a, &ciphertext);
    mlkem_encapsulation_key768_free(ek);
    return ciphertext;
}
```

<!-- example: ../examples/crypto/mlkem.c#alice -->
```c
// Alice makes a key and sends Bob the encapsulation key.
Error err = BURROW_NO_ERROR;
MlkemDecapsulationKey768 *dk = mlkem_generate_key768(a, &err);
if (BURROW_FAILED(err))
    return;
Slice encapsulation_key = mlkem_encapsulation_key768_bytes(
    mlkem_decapsulation_key768_encapsulation_key(dk), a);

Slice bob_key;
Slice ciphertext = bob(a, encapsulation_key, &bob_key);

// Alice gets the shared key out of the ciphertext.
Slice alice_key = mlkem_decapsulation_key768_decapsulate(dk, a, ciphertext, &err);
if (BURROW_FAILED(err))
    return;
printf("%d %d %d %s\n", (int)encapsulation_key.len, (int)ciphertext.len,
       (int)alice_key.len, bytes_equal(alice_key, bob_key) ? "true" : "false");
mlkem_decapsulation_key768_free(dk);
```

There are two sizes, ML-KEM-768 and ML-KEM-1024, with the same functions under each number, and most programs want ML-KEM-768. Keys come from the allocator they are made with and go back with the `_free` function of their type. The encapsulation key of a decapsulation key is part of it, borrowed from it, and freed with it. Both kinds of key work out their matrices when they are made, so a decapsulation key is about 8 KB for ML-KEM-768 and 11 KB for ML-KEM-1024.

A decapsulation key is stored as its 64 byte seed, which `mlkem_decapsulation_key768_bytes` gives and `mlkem_new_decapsulation_key768` takes, so the same seed always gives the same key. Encapsulation takes its randomness from the system, and `burrow/crypto/mlkem/mlkemtest.h` has the version that takes it as an argument, for known answer tests. This is the known answer test Go runs in FIPS mode:

<!-- example: ../examples/crypto/mlkem.c#fixed -->
```c
// The same seed always gives the same key, and mlkemtest takes the
// randomness of an encapsulation as an argument.
Byte seed[MLKEM_SEED_SIZE], m[32];
for (int i = 0; i < 64; i++)
    seed[i] = (Byte)(0x01 + i);
for (int i = 0; i < 32; i++)
    m[i] = (Byte)(0x41 + i);

Error err = BURROW_NO_ERROR;
MlkemDecapsulationKey768 *dk = mlkem_new_decapsulation_key768(
    a, slice_from(seed, sizeof seed, sizeof seed, TYPE_BYTE), &err);
if (BURROW_FAILED(err))
    return;
Slice ciphertext;
Slice key = mlkemtest_encapsulate768(
    mlkem_decapsulation_key768_encapsulation_key(dk), a,
    slice_from(m, sizeof m, sizeof m, TYPE_BYTE), &ciphertext, &err);
if (BURROW_FAILED(err))
    return;
print_hex(a, key);
print_hex(a, mlkem_decapsulation_key768_decapsulate(dk, a, ciphertext, &err));
```

The keys fit the KEM interfaces of `burrow/crypto.h`, for code that should not care which KEM it has:

<!-- example: ../examples/crypto/mlkem.c#kem -->
```c
// Code that works with any KEM takes a CryptoDecapsulator.
Error err = BURROW_NO_ERROR;
MlkemDecapsulationKey1024 *dk = mlkem_generate_key1024(a, &err);
if (BURROW_FAILED(err))
    return;
CryptoDecapsulator d = mlkem_decapsulation_key1024_as_decapsulator(dk);

CryptoEncapsulator e = crypto_decapsulator_encapsulator(d);
CryptoEncapsulateResult r = crypto_encapsulator_encapsulate(e, a);
Slice key = crypto_decapsulator_decapsulate(d, a, r.ciphertext, &err);
if (BURROW_FAILED(err))
    return;
printf("%.*s %d %s\n", (int)d.vt->self_type->name.len,
       (const char *)d.vt->self_type->name.p, (int)r.ciphertext.len,
       bytes_equal(key, r.shared_key) ? "true" : "false");
mlkem_decapsulation_key1024_free(dk);
```

Keys and ciphertexts of the wrong length are errors, with Go's messages, and so is an encapsulation key with a coefficient that is not reduced. A ciphertext of the right length is never an error. One that was not made for the key, or was changed on the way, gives a shared key nobody else has, so the two sides find out when the first message under the key does not decrypt:

<!-- example: ../examples/crypto/mlkem.c#errors -->
```c
Error err = BURROW_NO_ERROR;
Byte zeros[MLKEM_ENCAPSULATION_KEY_SIZE768] = {0};
Byte ones[MLKEM_ENCAPSULATION_KEY_SIZE768];
memset(ones, 0xff, sizeof ones);

mlkem_new_decapsulation_key768(a, slice_from(zeros, 32, 32, TYPE_BYTE), &err);
print_error(err);
mlkem_new_encapsulation_key768(
    a, slice_from(ones, sizeof ones, sizeof ones, TYPE_BYTE), &err);
print_error(err);

MlkemDecapsulationKey768 *dk =
    mlkem_new_decapsulation_key768(a, slice_from(zeros, 64, 64, TYPE_BYTE), &err);
mlkem_decapsulation_key768_decapsulate(
    dk, a, slice_from(zeros, 100, 100, TYPE_BYTE), &err);
print_error(err);

// A ciphertext of the right length that was never made for this key is
// not an error. It gives a key nobody else has.
Slice key = mlkem_decapsulation_key768_decapsulate(
    dk, a,
    slice_from(zeros, MLKEM_CIPHERTEXT_SIZE768, MLKEM_CIPHERTEXT_SIZE768,
               TYPE_BYTE),
    &err);
print_error(err);
printf("%d\n", (int)key.len);
```

Everything that touches secret data is constant time, as Go's code is, and the shared key goes through the same constant time selection Go uses for a ciphertext that does not check out. Go runs a self test and checks each new key against itself only in FIPS mode, which burrow does not have, so it does neither.

## crypto/mldsa

`burrow/crypto/mldsa.h` is ML-DSA, the signature scheme of FIPS 204 that was called Dilithium. Like ML-KEM it is meant to hold up against a quantum computer, and its keys and signatures are much bigger than those of Ed25519. There are three parameter sets, `mldsa_mldsa44`, `mldsa_mldsa65` and `mldsa_mldsa87`, and most programs want ML-DSA-44:

<!-- example: ../examples/crypto/mldsa.c#sign -->
```c
// The signer makes a key and publishes the public key's bytes.
Error err = BURROW_NO_ERROR;
MldsaPrivateKey *sk = mldsa_generate_key(a, mldsa_mldsa44(), &err);
if (BURROW_FAILED(err))
    return;
Slice pub_bytes = mldsa_public_key_bytes(mldsa_private_key_public_key(sk), a);

// The context keeps signatures made for one purpose from passing for
// another. Sign and Verify have to be given the same one.
MldsaOptions opts = {BURROW_S("release manifest")};
Slice msg = text("burrow v0.3.0");
Slice sig = mldsa_private_key_sign(sk, a, (IoReader){0}, msg,
                                   mldsa_options_as_signer_opts(&opts), &err);
if (BURROW_FAILED(err))
    return;

// The verifier has only the public key's bytes.
MldsaPublicKey *pk = mldsa_new_public_key(a, mldsa_mldsa44(), pub_bytes, &err);
if (BURROW_FAILED(err))
    return;
printf("%d %d\n", (int)pub_bytes.len, (int)sig.len);
print_error(mldsa_verify(pk, msg, sig, &opts));
print_error(mldsa_verify(pk, msg, sig, NULL));
print_error(mldsa_verify(pk, text("burrow v0.3.1"), sig, &opts));
mldsa_public_key_free(pk);
mldsa_private_key_free(sk);
```

The last two lines show what a verifier sees when the context or the message is not the one that was signed. Keys come from the allocator they are made with and go back with `mldsa_private_key_free` and `mldsa_public_key_free`. The public key of a private key is part of it and goes with it. Signing works out a fresh value from the system's generator each time, so two signatures of the same message differ. `mldsa_private_key_sign_deterministic` leaves that out, and a private key is stored as its 32 byte seed, so the same seed and message always give the same signature:

<!-- example: ../examples/crypto/mldsa.c#deterministic -->
```c
// A private key is its 32 byte seed. The same seed gives the same key,
// and SignDeterministic gives the same signature of the same message.
Byte seed[MLDSA_PRIVATE_KEY_SIZE];
for (int i = 0; i < MLDSA_PRIVATE_KEY_SIZE; i++)
    seed[i] = (Byte)i;
Error err = BURROW_NO_ERROR;
MldsaPrivateKey *sk = mldsa_new_private_key(
    a, mldsa_mldsa65(), slice_from(seed, sizeof seed, sizeof seed, TYPE_BYTE),
    &err);
if (BURROW_FAILED(err))
    return;
Slice sig = mldsa_private_key_sign_deterministic(sk, a, text("hello"),
                                                 (CryptoSignerOpts){0}, &err);
if (BURROW_FAILED(err))
    return;
Sha256Sum256Ret sum = sha256_sum256(sig);
print_hex(a, slice_from(sum.a, sizeof sum.a, sizeof sum.a, TYPE_BYTE));
mldsa_private_key_free(sk);
```

A private key is a `CryptoSigner` too, for code that should not care which scheme it has. Its options can be nil, an `MldsaOptions` with a context, or `CRYPTO_MLDSA_MU` for a message that is the 64 byte μ the caller worked out itself, which RFC 9881 calls external μ:

<!-- example: ../examples/crypto/mldsa.c#signer -->
```c
// Code that works with any signature scheme takes a CryptoSigner.
Error err = BURROW_NO_ERROR;
MldsaPrivateKey *sk = mldsa_generate_key(a, mldsa_mldsa87(), &err);
if (BURROW_FAILED(err))
    return;
CryptoSigner s = mldsa_private_key_signer(sk);
Slice msg = text("any message");
Slice sig =
    crypto_signer_sign(s, a, (IoReader){0}, msg, (CryptoSignerOpts){0}, &err);
if (BURROW_FAILED(err))
    return;
CryptoPublicKey pub = crypto_signer_public(s);
printf("%.*s %d\n", (int)pub.t->name.len, (const char *)pub.t->name.p,
       (int)sig.len);
print_error(mldsa_verify(pub.data, msg, sig, NULL));
mldsa_private_key_free(sk);
```

Keys and signatures of the wrong length are errors with Go's messages, and so are a context longer than 255 bytes and options that ask for a hash:

<!-- example: ../examples/crypto/mldsa.c#errors -->
```c
Error err = BURROW_NO_ERROR;
Byte zeros[MLDSA_MLDSA44_PUBLIC_KEY_SIZE] = {0};
mldsa_new_private_key(a, mldsa_mldsa44(), slice_from(zeros, 16, 16, TYPE_BYTE),
                      &err);
print_error(err);
mldsa_new_public_key(a, mldsa_mldsa65(),
                     slice_from(zeros, sizeof zeros, sizeof zeros, TYPE_BYTE),
                     &err);
print_error(err);

err = BURROW_NO_ERROR;
MldsaPrivateKey *sk = mldsa_new_private_key(
    a, mldsa_mldsa44(), slice_from(zeros, 32, 32, TYPE_BYTE), &err);
MldsaOptions opts = {strings_repeat(a, BURROW_S("x"), 256)};
mldsa_private_key_sign_deterministic(sk, a, text("hello"),
                                     mldsa_options_as_signer_opts(&opts), &err);
print_error(err);
CryptoHash h = CRYPTO_SHA256;
mldsa_private_key_sign_deterministic(sk, a, text("hello"),
                                     crypto_hash_as_signer_opts(&h), &err);
print_error(err);
print_error(mldsa_verify(mldsa_private_key_public_key(sk), text("hello"),
                         slice_from(zeros, 100, 100, TYPE_BYTE), NULL));
mldsa_private_key_free(sk);
```

Everything that touches secret data is constant time, as Go's code is. Go runs a self test and checks each new key against itself only in FIPS mode, which burrow does not have, so it does neither.

## crypto/hpke

`burrow/crypto/hpke.h` is Hybrid Public Key Encryption, RFC 9180: a way to encrypt to someone's public key that is built out of a KEM, a KDF and an AEAD, each picked by its own handle. It has the DHKEMs over P-256, P-384, P-521 and X25519, ML-KEM-768 and ML-KEM-1024 on their own, and the post-quantum hybrids MLKEM768-X25519 (X-Wing), MLKEM768-P256 and MLKEM1024-P384. The KDFs are HKDF with SHA-256, SHA-384 and SHA-512, and SHAKE128 and SHAKE256. The AEADs are AES-128-GCM, AES-256-GCM and ChaCha20-Poly1305, plus the export-only one. This is the example Go's documentation has:

<!-- example: ../examples/crypto/hpke.c#exchange -->
```c
// In a real application, the private key would be stored and the
// public key bytes sent to the sender.
const HpkeKEM *kem = hpke_mlkem768_x25519();
const HpkeKDF *kdf = hpke_hkdfsha256();
const HpkeAEAD *aead = hpke_aes256_gcm();
Error err = BURROW_NO_ERROR;
HpkePrivateKey *k = hpke_kem_generate_key(kem, a, &err);
if (BURROW_FAILED(err))
    return;
Slice public_key_bytes = hpke_public_key_bytes(hpke_private_key_public_key(k), a);

// The sender parses the public key and seals a message to it.
HpkePublicKey *pk = hpke_kem_new_public_key(kem, a, public_key_bytes, &err);
if (BURROW_FAILED(err))
    return;
Slice ciphertext =
    hpke_seal(a, pk, kdf, aead, text("example"), text("|-()-|"), &err);
if (BURROW_FAILED(err))
    return;

// The recipient opens it with the private key.
Slice plaintext = hpke_open(a, k, kdf, aead, text("example"), ciphertext, &err);
if (BURROW_FAILED(err))
    return;
printf("Decrypted message: %.*s\n", (int)plaintext.len, (const char *)plaintext.p);
hpke_public_key_free(pk);
hpke_private_key_free(k);
```

`hpke_seal` and `hpke_open` are for one message, and put the encapsulated key in front of the ciphertext. For more than one, `hpke_new_sender` gives the encapsulated key to send and a sender that seals each message with the next nonce, and `hpke_new_recipient` takes that key and opens them in the same order. Each side can also export secrets from the context, which is all an export-only context does. Keys come from the allocator they are made with and go back with `hpke_private_key_free` and `hpke_public_key_free`. The public key of a private key is borrowed from it. The handles for KEMs, KDFs and AEADs are static and never freed.

`hpke_kem_derive_key_pair` makes the same key from the same input keying material every time, the way RFC 9180's test vectors do. This is the recipient of its first vector, which gets the first message out and exports a secret:

<!-- example: ../examples/crypto/hpke.c#derive -->
```c
// The recipient of RFC 9180's first test vector, A.1, derives its key
// from ikmR, and opens the first message the sender sent it.
const HpkeKEM *kem = hpke_dhkem(ecdh_x25519());
Error err = BURROW_NO_ERROR;
HpkePrivateKey *k = hpke_kem_derive_key_pair(
    kem, a,
    unhex(a, "6db9df30aa07dd42ee5e8181afdb977e538f5e1fec8a06223f33f7013e525037"),
    &err);
if (BURROW_FAILED(err))
    return;
print_hex(a, hpke_public_key_bytes(hpke_private_key_public_key(k), a));

Slice enc =
    unhex(a, "37fda3567bdbd628e88668c3c8d7e97d1d1253b6d4ea6d44c150f741f1bf4431");
HpkeRecipient *r =
    hpke_new_recipient(a, enc, k, hpke_hkdfsha256(), hpke_aes128_gcm(),
                       text("Ode on a Grecian Urn"), &err);
if (BURROW_FAILED(err))
    return;
Slice pt = hpke_recipient_open(
    r, a, text("Count-0"),
    unhex(a, "f938558b5d72f1a23810b4be2ab4f84331acc02fc97babc53a52ae8218a355a9"
             "6d8770ac83d07bea87e13c512a"),
    &err);
if (BURROW_FAILED(err))
    return;
printf("%.*s\n", (int)pt.len, (const char *)pt.p);
print_hex(a, hpke_recipient_export(r, a, BURROW_S(""), 32, &err));
hpke_private_key_free(k);
```

An export-only context seals and opens nothing:

<!-- example: ../examples/crypto/hpke.c#export -->
```c
// With ExportOnly the two sides share secrets and nothing else.
const HpkeKEM *kem = hpke_dhkem(ecdh_p256());
Error err = BURROW_NO_ERROR;
HpkePrivateKey *k = hpke_kem_generate_key(kem, a, &err);
if (BURROW_FAILED(err))
    return;
HpkeSender *s;
Slice enc = hpke_new_sender(a, hpke_private_key_public_key(k), hpke_hkdfsha256(),
                            hpke_export_only(), text("session"), &s, &err);
if (BURROW_FAILED(err))
    return;
HpkeRecipient *r = hpke_new_recipient(a, enc, k, hpke_hkdfsha256(),
                                      hpke_export_only(), text("session"), &err);
if (BURROW_FAILED(err))
    return;
Slice x = hpke_sender_export(s, a, BURROW_S("key"), 16, &err);
Slice y = hpke_recipient_export(r, a, BURROW_S("key"), 16, &err);
if (BURROW_FAILED(err))
    return;
printf("%d %d %s\n", (int)enc.len, (int)x.len,
       bytes_equal(x, y) ? "true" : "false");
hpke_sender_seal(s, a, slice_nil(TYPE_BYTE), text("hi"), &err);
print_error(err);
hpke_private_key_free(k);
```

Keys can also be made out of keys of the other packages: `hpke_new_dhkem_private_key` takes any `EcdhKeyExchanger`, `hpke_new_mlkem_private_key` any ML-KEM `CryptoDecapsulator`, and `hpke_new_hybrid_private_key` one of each. A private key made of parts that are not plain keys of those packages has no bytes to give back, which is an error from `hpke_private_key_bytes`, as it is in Go. An ID nobody supports, and a ciphertext that does not open, are errors with Go's messages:

<!-- example: ../examples/crypto/hpke.c#errors -->
```c
Error err = BURROW_NO_ERROR;
hpke_new_kem(0x0021, &err);
print_error(err);
err = BURROW_NO_ERROR;
hpke_new_aead(0xabcd, &err);
print_error(err);
err = BURROW_NO_ERROR;

HpkePrivateKey *k = hpke_kem_generate_key(hpke_dhkem(ecdh_x25519()), a, &err);
if (BURROW_FAILED(err))
    return;
Slice ct =
    hpke_seal(a, hpke_private_key_public_key(k), hpke_hkdfsha256(),
              hpke_cha_cha20_poly1305(), slice_nil(TYPE_BYTE), text("hi"), &err);
if (BURROW_FAILED(err))
    return;
((Byte *)ct.p)[ct.len - 1] ^= 1;
hpke_open(a, k, hpke_hkdfsha256(), hpke_cha_cha20_poly1305(), slice_nil(TYPE_BYTE),
          ct, &err);
print_error(err);
hpke_private_key_free(k);
```

Go's tests set package variables to make an encapsulation come out as a vector says. burrow has no mutable globals, so its tests reach into `src/crypto/hpke_internal.h` for a sender that takes that randomness as an argument instead, and nothing of it is in the public header.

## crypto/rsa

`burrow/crypto/rsa.h` is RSA as PKCS #1 has it, for signatures and for encryption. It is mostly there for the protocols and certificates that already use it. A new design does better with Ed25519 or ML-DSA for signatures and with ECDH or ML-KEM to agree on a key. There are two ways to sign. PSS is the newer one, and PKCS #1 v1.5 is the one most certificates use. Both sign a hash the caller has already worked out:

<!-- example: ../examples/crypto/rsa.c#sign -->
```c
Error err = BURROW_NO_ERROR;
RsaPrivateKey *priv = rsa_generate_key(a, (IoReader){0}, 2048, &err);
if (BURROW_FAILED(err))
    return;

// Both schemes sign the hash of the message, not the message itself.
Sha256Sum256Ret sum = sha256_sum256(text("burrow v0.3.0"));
Slice hashed = slice_from(sum.a, sizeof sum.a, sizeof sum.a, TYPE_BYTE);
Slice sig =
    rsa_sign_pss(a, crypto_rand_reader, priv, CRYPTO_SHA256, hashed, NULL, &err);
if (BURROW_FAILED(err))
    return;
printf("%d\n", (int)sig.len);
print_error(rsa_verify_pss(&priv->public_key, CRYPTO_SHA256, hashed, sig, NULL));

// PKCS #1 v1.5 needs no randomness and gives the same signature each time.
Slice old = rsa_sign_pkcs1_v15(a, (IoReader){0}, priv, CRYPTO_SHA256, hashed, &err);
if (BURROW_FAILED(err))
    return;
print_error(rsa_verify_pkcs1_v15(&priv->public_key, CRYPTO_SHA256, hashed, old));

// A signature of some other message does not verify.
Sha256Sum256Ret other = sha256_sum256(text("burrow v0.3.1"));
Slice other_hashed = slice_from(other.a, sizeof other.a, sizeof other.a, TYPE_BYTE);
print_error(
    rsa_verify_pss(&priv->public_key, CRYPTO_SHA256, other_hashed, sig, NULL));
rsa_private_key_free(priv, a);
```

A key is a struct of `BigInt` pointers, as in Go, and everything in it comes from the allocator it was made with. `rsa_private_key_free` gives back a key that `rsa_generate_key` made, and an arena does the same for a whole batch. `rsa_generate_key` takes an `IoReader` with nothing in it to mean the system's generator. Signing with PSS and encrypting with OAEP read the reader they are given, as Go's do, so they need a real one, and `crypto_rand_reader` is the one to pass. Given an empty reader they panic, the way Go does with nil.

For encryption, OAEP is the scheme to use, and PKCS #1 v1.5 encryption is only there for old protocols. RSA can only encrypt a message shorter than its key, so in practice it carries a key for a symmetric cipher. The label is bound into the ciphertext, and decrypting with a different one fails:

<!-- example: ../examples/crypto/rsa.c#encrypt -->
```c
Error err = BURROW_NO_ERROR;
RsaPrivateKey *priv = rsa_generate_key(a, (IoReader){0}, 2048, &err);
if (BURROW_FAILED(err))
    return;

// The label is not secret, but decrypting needs the same one.
Slice ct = rsa_encrypt_oaep(a, sha256_new(a), crypto_rand_reader, &priv->public_key,
                            text("a session key"), text("orders"), &err);
if (BURROW_FAILED(err))
    return;
printf("%d\n", (int)ct.len);
Slice pt = rsa_decrypt_oaep(a, sha256_new(a), (IoReader){0}, priv, ct,
                            text("orders"), &err);
if (BURROW_FAILED(err))
    return;
print_text(pt);
rsa_decrypt_oaep(a, sha256_new(a), (IoReader){0}, priv, ct, text("invoices"), &err);
print_error(err);
rsa_private_key_free(priv, a);
```

Keys shorter than 1024 bits are an error everywhere, as they are since Go 1.24, and the errors have Go's messages. Tests that need small keys can turn the check off with `GODEBUG=rsa1024min=0`:

<!-- example: ../examples/crypto/rsa.c#errors -->
```c
Error err = BURROW_NO_ERROR;
rsa_generate_key(a, (IoReader){0}, 512, &err);
print_error(err);

err = BURROW_NO_ERROR;
RsaPrivateKey *priv = rsa_generate_key(a, (IoReader){0}, 1024, &err);
if (BURROW_FAILED(err))
    return;
// OAEP with SHA-256 fits 128 - 2*32 - 2 = 62 bytes in a 1024 bit key.
Byte big[63] = {0};
rsa_encrypt_oaep(a, sha256_new(a), crypto_rand_reader, &priv->public_key,
                 slice_from(big, sizeof big, sizeof big, TYPE_BYTE), (Slice){0},
                 &err);
print_error(err);
err = BURROW_NO_ERROR;
rsa_sign_pkcs1_v15(a, (IoReader){0}, priv, CRYPTO_SHA256, text("not a hash"), &err);
print_error(err);
rsa_private_key_free(priv, a);
```

Everything that touches the private key is constant time, and so is checking the padding when decrypting. A key put together by hand should go through `rsa_private_key_precompute` before use, which checks it and works out the values that make the private key operations fast.

## crypto/x509/pkix

`burrow/crypto/x509/pkix.h` has the ASN.1 structures that certificates, CRLs and OCSP share: distinguished names, algorithm identifiers, extensions and the old CRL types. Each one has a type descriptor carrying Go's asn1 struct tags, so `encoding/asn1` reads and writes them with no extra code. A `PkixName` is the friendly form of a name, and `pkix_name_to_rdn_sequence` turns it into the sequence of RDNs that goes on the wire:

<!-- example: ../examples/crypto/pkix.c#name -->
```c
Str org = BURROW_S("Example Ltd");
PkixName n = {0};
n.common_name = BURROW_S("www.example.com");
n.organization = slice_append(a, slice_nil(TYPE_STRING), &org, 1);
PkixRDNSequence rdns = pkix_name_to_rdn_sequence(n, a);

Error err = BURROW_NO_ERROR;
Slice der = asn1_marshal(a, BURROW_ANY(TYPE_PKIX_RDN_SEQUENCE, &rdns), &err);
print(hex_encode_to_string(a, der));
```

Going the other way, unmarshal into a `PkixRDNSequence` and fill a `PkixName` from it. The string form follows RFC 2253, last RDN first, with the same escaping as Go:

<!-- example: ../examples/crypto/pkix.c#parse -->
```c
PkixRDNSequence back = slice_nil(TYPE_PKIX_RELATIVE_DISTINGUISHED_NAME_SET);
asn1_unmarshal(a, der, BURROW_ANY(TYPE_PKIX_RDN_SEQUENCE, &back), &err);
if (BURROW_FAILED(err))
    return;
PkixName parsed = {0};
pkix_name_fill_from_rdn_sequence(&parsed, a, &back);
print(pkix_name_string(parsed, a));
print(parsed.common_name);
printf("%d attributes\n", (int)parsed.names.len);
```

`names` keeps every attribute the parsed name had, including ones with no field of their own. `extra_names` is for the other direction: attributes put there are written into the name and win over a field of the same type. As in Go, a nil `extra_names` and an empty one are not the same thing when the name is printed, so leave it nil unless you mean to hide the uncommon attributes in `names`.

## crypto/x509

`burrow/crypto/x509.h` is Go's crypto/x509. It covers key formats, object identifiers, certificates, certificate requests and revocation lists; chain building comes next. Private keys go in and out of PKCS #8 as an `Any`, the same way Go uses `any`, so one call handles RSA, ECDSA, Ed25519, X25519 and ML-DSA keys. PKCS #1 and SEC 1 have calls of their own for RSA and EC keys:

<!-- example: ../examples/crypto/x509.c#pkcs8 -->
```c
Byte seed[ED25519_SEED_SIZE] = {0};
for (int i = 0; i < ED25519_SEED_SIZE; i++)
    seed[i] = (Byte)i;
Ed25519PrivateKey priv = ed25519_new_key_from_seed(
    a, slice_from(seed, sizeof seed, sizeof seed, TYPE_BYTE));

Error err = BURROW_NO_ERROR;
Slice der = x509_marshal_pkcs8_private_key(
    a, BURROW_ANY(TYPE_ED25519_PRIVATE_KEY, &priv), &err);
if (BURROW_FAILED(err))
    return;
PemBlock block = {.type = BURROW_S("PRIVATE KEY"), .bytes = der};
Slice pem = pem_encode_to_memory(a, &block);
printf("%.*s", (int)pem.len, (const char *)pem.p);
```

Parsing gives back an `Any` whose type says which kind of key it holds. Public keys use the SubjectPublicKeyInfo form through `x509_parse_pkix_public_key` and `x509_marshal_pkix_public_key`. Handing a key to the wrong parser fails with the same hint Go gives:

<!-- example: ../examples/crypto/x509.c#parse -->
```c
Slice rest;
PemBlock *b = pem_decode(a, pem, &rest);
Any key = x509_parse_pkcs8_private_key(a, b->bytes, &err);
if (BURROW_FAILED(err))
    return;
if (key.t == TYPE_ED25519_PRIVATE_KEY) {
    Ed25519PrivateKey *k = key.data;
    Ed25519PublicKey pub = ed25519_private_key_public(*k, a);
    Slice spki = x509_marshal_pkix_public_key(
        a, BURROW_ANY(TYPE_ED25519_PUBLIC_KEY, &pub), &err);
    print(hex_encode_to_string(a, spki));
}

// The wrong parser says which one to use.
x509_parse_pkcs1_private_key(a, b->bytes, &err);
print(error_text(err));
```

An `X509OID` holds the DER of an object identifier, so unlike `Asn1ObjectIdentifier` it has no limit on the size of an arc. `x509_parse_oid` reads the dotted form, and the text and binary marshal calls match Go's `encoding.TextMarshaler` and `encoding.BinaryMarshaler` methods:

<!-- example: ../examples/crypto/x509.c#oid -->
```c
Error err = BURROW_NO_ERROR;
X509OID oid = x509_parse_oid(a, BURROW_S("1.3.6.1.4.1.11129.2.4.2"), &err);
Slice der = x509_oid_marshal_binary(oid, a, &err);
print(hex_encode_to_string(a, der));

// Arcs past 64 bits are fine too.
oid = x509_parse_oid(a, BURROW_S("2.25.329800735698586629295641978511506172918"),
                     &err);
print(x509_oid_string(oid, a));

print(
    x509_oid_string(x509_ext_key_usage_oid(X509_EXT_KEY_USAGE_SERVER_AUTH, a), a));

x509_parse_oid(a, BURROW_S("1.2."), &err);
print(error_text(err));
```

`x509_create_certificate` signs a template with a parent's key, the same as Go's `CreateCertificate`. Fields left zero get Go's defaults: a CA with no subject key ID gets one from the SHA-256 of its public key, and the signature algorithm follows from the signer. A self-signed certificate is its own parent:

<!-- example: ../examples/crypto/x509.c#create -->
```c
Byte seed[ED25519_SEED_SIZE] = {0};
for (int i = 0; i < ED25519_SEED_SIZE; i++)
    seed[i] = (Byte)(i + 1);
Ed25519Signer ca_key;
CryptoSigner ca_signer = ed25519_private_key_signer(
    ed25519_new_key_from_seed(
        a, slice_from(seed, sizeof seed, sizeof seed, TYPE_BYTE)),
    &ca_key);

X509Certificate tmpl = {0};
tmpl.serial_number = big_new_int(a, 1);
tmpl.subject.common_name = BURROW_S("Example Root");
tmpl.not_before = time_date(2026, TIME_JANUARY, 1, 0, 0, 0, 0, time_utc_loc);
tmpl.not_after = time_date(2036, TIME_JANUARY, 1, 0, 0, 0, 0, time_utc_loc);
tmpl.key_usage = X509_KEY_USAGE_CERT_SIGN;
tmpl.basic_constraints_valid = true;
tmpl.is_ca = true;

// Self-signed, so the template is its own parent.
Error err = BURROW_NO_ERROR;
Slice der = x509_create_certificate(
    a, (IoReader){0}, &tmpl, &tmpl,
    BURROW_ANY(TYPE_ED25519_PUBLIC_KEY, &ca_key.pub), ca_signer, &err);
if (BURROW_FAILED(err))
    return;
printf("%d bytes\n", (int)der.len);
```

`x509_parse_certificate` reads the DER back into an `X509Certificate`, with every extension Go understands filled in and the raw bytes of each part kept next to it. `x509_certificate_check_signature_from` checks that a parent signed it:

<!-- example: ../examples/crypto/x509.c#certparse -->
```c
X509Certificate *ca = x509_parse_certificate(a, der, &err);
if (BURROW_FAILED(err))
    return;
print(pkix_name_string(ca->subject, a));
print(x509_signature_algorithm_string(ca->signature_algorithm, a));
print(hex_encode_to_string(a, ca->subject_key_id));
print(time_format(ca->not_after, a, TIME_RFC3339));
err = x509_certificate_check_signature_from(ca, ca);
printf("signed by itself: %s\n", BURROW_FAILED(err) ? "no" : "yes");
```

Certificate requests work the same way. The request carries the subject, the names and the public key, and is signed by the key it asks a certificate for:

<!-- example: ../examples/crypto/x509.c#request -->
```c
for (int i = 0; i < ED25519_SEED_SIZE; i++)
    seed[i] = (Byte)(0x80 + i);
Ed25519Signer leaf_key;
CryptoSigner leaf_signer = ed25519_private_key_signer(
    ed25519_new_key_from_seed(
        a, slice_from(seed, sizeof seed, sizeof seed, TYPE_BYTE)),
    &leaf_key);

Str host = BURROW_S("www.example.com");
X509CertificateRequest req = {0};
req.subject.common_name = host;
req.dns_names = slice_append(a, slice_nil(TYPE_STRING), &host, 1);
Slice csr_der =
    x509_create_certificate_request(a, (IoReader){0}, &req, leaf_signer, &err);
if (BURROW_FAILED(err))
    return;
X509CertificateRequest *csr = x509_parse_certificate_request(a, csr_der, &err);
if (BURROW_FAILED(err))
    return;
err = x509_certificate_request_check_signature(csr);
printf("request for %.*s: %s\n", (int)csr->subject.common_name.len,
       csr->subject.common_name.p, BURROW_FAILED(err) ? "bad" : "ok");
```

A CA then copies what it trusts from the request into a template and signs it. The authority key ID of the new certificate is the subject key ID of the parent:

<!-- example: ../examples/crypto/x509.c#issue -->
```c
// The CA signs a certificate for the key in the request.
X509Certificate leaf_tmpl = {0};
leaf_tmpl.serial_number = big_new_int(a, 2);
leaf_tmpl.subject = csr->subject;
leaf_tmpl.dns_names = csr->dns_names;
leaf_tmpl.not_before = tmpl.not_before;
leaf_tmpl.not_after = time_date(2027, TIME_JANUARY, 1, 0, 0, 0, 0, time_utc_loc);
leaf_tmpl.key_usage = X509_KEY_USAGE_DIGITAL_SIGNATURE;
X509ExtKeyUsage server = X509_EXT_KEY_USAGE_SERVER_AUTH;
leaf_tmpl.ext_key_usage = slice_append(a, slice_nil(TYPE_INT), &server, 1);
Slice leaf_der = x509_create_certificate(a, (IoReader){0}, &leaf_tmpl, ca,
                                         csr->public_key, ca_signer, &err);
if (BURROW_FAILED(err))
    return;
X509Certificate *leaf = x509_parse_certificate(a, leaf_der, &err);
if (BURROW_FAILED(err))
    return;
print(pkix_name_string(leaf->issuer, a));
print(hex_encode_to_string(a, leaf->authority_key_id));
```

`x509_create_revocation_list` and `x509_parse_revocation_list` do the same for CRLs. Parsing does not fail on an extension it does not know. When such an extension is marked critical it goes in `unhandled_critical_extensions`, as in Go, and verification turns the certificate down unless the caller handles it and takes it out of that list.

`x509_encrypt_pem_block` and `x509_decrypt_pem_block` handle the RFC 1423 `DEK-Info` encryption that old OpenSSL keys use. Go deprecates them because the scheme is weak and a wrong password is not always caught, and they are here for reading old files, not for writing new ones.

One GODEBUG setting from Go applies: `x509rsacrt=0` makes `x509_parse_pkcs1_private_key` work out the CRT values again when the ones in the key are wrong, instead of failing. It is read once from the `GODEBUG` environment variable.


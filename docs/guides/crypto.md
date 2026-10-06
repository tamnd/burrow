# Cryptography

burrow's `crypto` packages are Go's, with the same algorithms, the same answers and the same panics. The hashes are covered in [hashing.md](hashing.md). This page covers the rest, one package at a time as they land.

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

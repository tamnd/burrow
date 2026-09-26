# Hashing and checksums

`burrow/hash.h` is Go's `hash`: the interfaces that every hash function speaks. `hash/crc32` and `hash/crc64` are cyclic redundancy checks, `hash/adler32` is the checksum zlib uses, and `hash/fnv` is the Fowler, Noll and Vo family of non-cryptographic hashes. The cryptographic hashes `crypto/md5`, `crypto/sha1`, `crypto/sha256` and `crypto/sha512` implement the same interfaces, so code written against `Hash` takes any of them.

The checksums and FNV are not safe against someone choosing the input on purpose. They are for catching accidental corruption and for spreading keys across buckets. For that you want SHA-256 or SHA-512, which are covered [further down](#cryptographic-hashes).

## One call

When the data is all in memory at once, the checksum functions take a slice and return a number:

<!-- example: ../examples/hash/hash.c#oneshot -->
```c
Slice data = slice_from_str(a, BURROW_S("hello, world"));
uint32_t ieee = crc32_checksum_ieee(data);
uint32_t adler = adler32_checksum(data);
```

Both print the way Go prints them: `ffab723a` and `1d540489`.

A CRC is defined by its polynomial, and each one needs a table built from it. `crc32_make_table` builds one. Go keeps a single copy of the tables for IEEE and Castagnoli, and so does burrow, so for those two you can pass `NULL` for the allocator and get back a table that lives as long as the program:

<!-- example: ../examples/hash/hash.c#castagnoli -->
```c
const Crc32Table *c = crc32_make_table(NULL, CRC32_CASTAGNOLI);
uint32_t crc32c = crc32_checksum(data, c);
```

Castagnoli is the CRC-32C that iSCSI, ext4 and most modern storage formats use. Any other polynomial gets a table allocated from the allocator you pass. `crc32_ieee_table` is the IEEE table, ready to use.

`hash/crc64` is the same shape, with `CRC64_ISO` and `CRC64_ECMA` for the two standard polynomials:

<!-- example: ../examples/hash/hash.c#crc64 -->
```c
const Crc64Table *iso = crc64_make_table(a, CRC64_ISO);
uint64_t sum64 = crc64_checksum(data, iso);
```

## Streaming

For data that arrives in pieces, make a hash and write to it. `crc32_new_ieee` returns a `HashHash32`, which is Go's `hash.Hash32`: a `Hash` that can also give its sum as a `uint32_t`. The conversion to the narrower `Hash` is written out, since C will not do it for you:

<!-- example: ../examples/hash/hash.c#stream -->
```c
HashHash32 h = crc32_new_ieee(a);
Hash as_hash = hash_hash32_as_hash(h);
hash_write(as_hash, slice_from_str(a, BURROW_S("hello, ")), NULL);
hash_write(as_hash, slice_from_str(a, BURROW_S("world")), NULL);
uint32_t sum = hash_hash32_sum32(h);
```

The sum is the same `ffab723a` as the one call version. Sum does not change the state, so you can ask for it and keep writing.

If you would rather carry the running value yourself, `crc32_update` takes the CRC so far and returns the new one, with no hash object at all:

<!-- example: ../examples/hash/hash.c#update -->
```c
uint32_t crc =
    crc32_update(0, crc32_ieee_table, slice_from_str(a, BURROW_S("hello, ")));
crc = crc32_update(crc, crc32_ieee_table, slice_from_str(a, BURROW_S("world")));
```

Because a `Hash` starts with an `IoWriter`, it can go anywhere a writer can. Here `fmt` writes straight into the CRC without building the string first:

<!-- example: ../examples/hash/hash.c#writer -->
```c
IoWriter w = hash_as_io_writer(as_hash);
hash_reset(as_hash);
fmt_fprintf_v(w, "%s, %s", BURROW_S("hello"), BURROW_S("world"));
```

## Code that takes any hash

A function that takes a `Hash` works with all of them. `hash_sum` appends the sum to the slice you pass and returns the result, the same as Go's `Sum`, so passing a nil slice gets you a new one:

<!-- example: ../examples/hash/hash.c#generic -->
```c
static void print_sum(Alloc *a, Str name, Hash h, Slice data) {
    hash_reset(h);
    hash_write(h, data, NULL);
    Slice sum = hash_sum(a, h, slice_nil(TYPE_BYTE));
    printf(BURROW_STR_FMT ":", BURROW_STR_ARG(name));
    for (Int i = 0; i < sum.len; i++)
        printf(" %02x", ((const Byte *)sum.p)[i]);
    printf("\n");
}
```

The FNV hashes come in 32, 64 and 128 bit sizes, each in the original order of operations and the `a` variant, which spreads short inputs better and is the one to reach for. The 32 and 64 bit ones are also `HashHash32` and `HashHash64`, and the 128 bit ones are plain `Hash`:

<!-- example: ../examples/hash/hash.c#fnv -->
```c
print_sum(a, BURROW_S("fnv32a"), hash_hash32_as_hash(fnv_new32a(a)), data);
print_sum(a, BURROW_S("fnv64a"), hash_hash64_as_hash(fnv_new64a(a)), data);
print_sum(a, BURROW_S("fnv128a"), fnv_new128a(a), data);
```

That prints:

```text
fnv32a: 4d 0e a4 1d
fnv64a: 17 a1 a4 f2 67 be 63 3d
fnv128a: 4f 2e a2 0c f7 3d cc 0f f0 d6 a3 62 4c d2 66 05
```

## Cryptographic hashes

Each package has a one call function that returns the digest as a fixed size array wrapped in a struct, the way Go returns `[32]byte`. It is returned by value and nothing is allocated:

<!-- example: ../examples/hash/hash.c#sha256 -->
```c
Slice data = slice_from_str(a, BURROW_S("hello, world"));
Sha256Sum256Ret sum = sha256_sum256(data);
Str hex =
    hex_encode_to_string(a, slice_from(sum.a, SHA256_SIZE, SHA256_SIZE, TYPE_BYTE));
```

That is `09ca7e4eaa6e8ae9c7d261167129184883644d07dfba7cbfbc4c8a2e08360d5b`, the same as `sha256sum` prints.

The names follow Go's. `sha256_sum256` and `sha256_sum224` are SHA-256 and SHA-224. `sha512_sum512`, `sha512_sum384`, `sha512_sum512224` and `sha512_sum512256` are SHA-512, SHA-384, SHA-512/224 and SHA-512/256. `md5_sum` and `sha1_sum` are the old ones. Each size has a constant, `SHA256_SIZE`, `SHA512_SIZE384` and so on, and the block sizes are `MD5_BLOCK_SIZE`, `SHA1_BLOCK_SIZE`, `SHA256_BLOCK_SIZE` and `SHA512_BLOCK_SIZE`.

The streaming versions are a plain `Hash`, made from the allocator you pass:

<!-- example: ../examples/hash/hash.c#cryptostream -->
```c
Hash h = sha512_new512256(a);
hash_write(h, slice_from_str(a, BURROW_S("hello, ")), NULL);
hash_write(h, slice_from_str(a, BURROW_S("world")), NULL);
Slice digest = hash_sum(a, h, slice_nil(TYPE_BYTE));
```

And they go through the same generic code as the checksums above:

<!-- example: ../examples/hash/hash.c#cryptogeneric -->
```c
print_sum(a, BURROW_S("md5"), md5_new(a), data);
print_sum(a, BURROW_S("sha1"), sha1_new(a), data);
```

```text
md5: e4 d7 f1 b4 ed 2e 42 d1 58 98 f4 b2 7b 01 9d a4
sha1: b7 e2 3e c2 9a f2 2b 0b 4e 41 da 31 e8 68 d5 72 26 12 1c 84
```

MD5 and SHA-1 are broken for anything where an attacker picks the input. They are here because file formats and protocols still name them. Use SHA-256 or SHA-512 for new work.

## SHA-3 and SHAKE

`crypto/sha3` works a little differently, because Go's `sha3.SHA3` and `sha3.SHAKE` are concrete types whose zero value works. Here they are plain structs, `Sha3` and `Sha3SHAKE`, so one can sit on the stack with nothing allocated. A zero `Sha3` is SHA3-256:

<!-- example: ../examples/hash/hash.c#sha3 -->
```c
Sha3 h = {0};
sha3_write(&h, data, NULL);
Slice sum = sha3_sum(&h, a, slice_nil(TYPE_BYTE));
```

That prints `bfb3959527d7a3f2f09def2f6915452d55a8f122df9e164d6f31c7fcf6093e14`. `sha3_new224`, `sha3_new256`, `sha3_new384` and `sha3_new512` allocate one of a given size, and `sha3_sum224` and the rest are the one call versions, returned by value like the others above.

SHAKE128 and SHAKE256 are extendable output functions. You write the input and then read as many bytes of output as you want, in as many reads as you like. A zero `Sha3SHAKE` is SHAKE256:

<!-- example: ../examples/hash/hash.c#shake -->
```c
Sha3SHAKE x = {0};
sha3_shake_write(&x, data, NULL);
Byte out[16];
sha3_shake_read(&x, slice_from(out, 16, 16, TYPE_BYTE), NULL);
```

The 16 bytes are `7c9896ea84a2a1b80b2183a3f2b4e43c`, and reading 16 more carries on from there. `sha3_new_cshake128` and `sha3_new_cshake256` take a function name and a customisation string, which is cSHAKE from SP 800-185. Writing after the first read panics with Go's message, `sha3: Write after Read`.

To pass one to code that takes a `Hash`, `sha3_as_hash` wraps a pointer to it, and `sha3_shake_as_xof` gives a `HashXOF`:

<!-- example: ../examples/hash/hash.c#sha3generic -->
```c
print_sum(a, BURROW_S("sha3-224"), sha3_as_hash(sha3_new224(a)), data);
```

```text
sha3-224: 92 7b 36 2e af 84 a7 57 85 bb ec 33 70 d1 c9 71 13 49 e9 3f 11 04 ed a0 60 78 42 21
```

Unlike the older hashes, both types already have `MarshalBinary`, `AppendBinary` and `UnmarshalBinary`, as `sha3_marshal_binary` and so on, and `sha3_clone` returns a `HashCloner`. The saved state is byte for byte what Go saves, so a state written by a Go program can be picked up here and the other way round.

## maphash

`burrow/hash/maphash.h` is Go's `hash/maphash`: the hash the map itself uses, for building your own hash tables, Bloom filters and the like. It is fast and well mixed, and it is not stable. Every seed gives different hashes, a seed comes from `maphash_make_seed`, and nothing about the result survives the process, so never write one to disk or send it anywhere.

For a single string or byte slice, `maphash_string` and `maphash_bytes` take the seed and the data:

<!-- example: ../examples/maphash/maphash.c#oneshot -->
```c
MaphashSeed seed = maphash_make_seed();
uint64_t a = maphash_string(seed, BURROW_S("hello"));
uint64_t b = maphash_string(seed, BURROW_S("hello"));
uint64_t c = maphash_string(maphash_make_seed(), BURROW_S("hello"));
printf("same seed: %s\n", a == b ? "equal" : "different");
printf("new seed: %s\n", a == c ? "equal" : "different");
```

A zero `MaphashHash` is ready to use and picks a random seed the first time it needs one. Writing in pieces gives the same hash as writing everything at once, and `maphash_hash_seed` tells you which seed it picked, so you can hash other things the same way:

<!-- example: ../examples/maphash/maphash.c#stream -->
```c
MaphashHash h = {0};
maphash_hash_write_string(&h, BURROW_S("hello, "), NULL);
maphash_hash_write_string(&h, BURROW_S("world"), NULL);
uint64_t sum = maphash_hash_sum64(&h);

uint64_t whole = maphash_string(maphash_hash_seed(&h), BURROW_S("hello, world"));
printf("pieces and whole: %s\n", sum == whole ? "equal" : "different");
```

`MaphashHash` is also a `Hash`, a `HashHash64` and a `HashCloner` through `maphash_hash_as_hash`, `maphash_hash_as_hash64` and `maphash_hash_as_cloner`. It never fails to write, so the error out parameter can be `NULL`.

`maphash_comparable` is Go's `Comparable`. It takes a type descriptor and a pointer to a value, and two values that compare equal get the same hash even when their bytes differ: two strings with the same contents at different addresses, `0.0` and `-0.0`, a struct with padding. `maphash_write_comparable` does the same into a running `MaphashHash`.

<!-- example: ../examples/maphash/maphash.c#comparable -->
```c
MaphashSeed seed = maphash_make_seed();
Str x = BURROW_S("key");
Byte copy[3] = {'k', 'e', 'y'};
Str y = {copy, 3};
uint64_t hx = maphash_comparable(seed, TYPE_STRING, &x);
uint64_t hy = maphash_comparable(seed, TYPE_STRING, &y);
printf("equal strings: %s\n", hx == hy ? "equal" : "different");
```

Go 1.27 adds `Hasher`, the interface between a hash based container and its elements: a way to hash a value into a `MaphashHash` and a way to say whether two values are equal. It is how you key a container by something that cannot be a map key, or by a looser idea of equal, such as strings compared without case. Here it is `MaphashHasher`, a vtable with `hash` and `equal`, called through `maphash_hasher_hash` and `maphash_hasher_equal`. `MaphashComparableHasher` is the one whose equal is `==`, for a type given by its descriptor.

Go's package documentation shows it off with a Bloom filter that works for any element type, and here is the same filter. The filter only knows its elements through the hasher:

<!-- example: ../examples/maphash/bloom.c#filter -->
```c
typedef struct BloomFilter {
    MaphashHasher hasher;
    MaphashSeed *seeds; // each seed picks a hash function
    Int nseeds;
    Byte *bytes; // the bit vector
    Int nbytes;
} BloomFilter;

// reduce maps hash into [0, n), the way Lemire suggests instead of a modulo.
static uint64_t reduce(uint64_t hash, uint64_t n) {
    return (hash >> 32) * n >> 32;
}

static void locate(const BloomFilter *f, MaphashSeed seed, const void *v,
                   uint64_t *index, Byte *mask) {
    MaphashHash h = {0};
    maphash_hash_set_seed(&h, seed);
    maphash_hasher_hash(f->hasher, &h, v);
    uint64_t hash = maphash_hash_sum64(&h);
    *index = reduce(hash, (uint64_t)f->nbytes);
    *mask = (Byte)(1U << (hash % 8));
}

static void bloom_insert(BloomFilter *f, const void *v) {
    for (Int i = 0; i < f->nseeds; i++) {
        uint64_t index;
        Byte bit;
        locate(f, f->seeds[i], v, &index, &bit);
        f->bytes[index] |= bit;
    }
}

static bool bloom_contains(const BloomFilter *f, const void *v) {
    for (Int i = 0; i < f->nseeds; i++) {
        uint64_t index;
        Byte bit;
        locate(f, f->seeds[i], v, &index, &bit);
        if ((f->bytes[index] & bit) == 0)
            return false;
    }
    return true;
}
```

<!-- example: ../examples/maphash/bloom.c#use -->
```c
// A filter for 2 strings with a one in a billion false positive rate.
MaphashComparableHasher strs = {TYPE_STRING};
BloomFilter f = bloom_new(a, maphash_comparable_hasher_as_hasher(&strs), 2, 1e-9);

Str apple = BURROW_S("apple"), banana = BURROW_S("banana");
bloom_insert(&f, &apple);
bloom_insert(&f, &banana);

Str fruits[] = {BURROW_S("apple"), BURROW_S("banana"), BURROW_S("cherry")};
for (int i = 0; i < 3; i++)
    printf("Contains(\"%.*s\") = %s\n", (int)fruits[i].len,
           (const char *)fruits[i].p,
           bloom_contains(&f, &fruits[i]) ? "true" : "false");
```

A type Go cannot compare, such as a slice, panics with Go's message, `runtime error: hash of unhashable type []uint8`, and so does an `Any` holding one. A zero `MaphashSeed` is not a seed, and handing one in panics with `maphash: use of uninitialized Seed`.

The hash is burrow's runtime hash and not Go's, so the numbers differ from a Go program's, the same as they would between two runs of either. Its quality is checked by Go's smhasher tests, which look at collisions over sparse, cyclic, permuted and windowed keys and at how well each input bit flips each output bit. All of them pass.

## Memory

A hash made by one of the `_new` functions comes from the allocator you pass and holds no other memory, so it goes away with the arena. The IEEE and Castagnoli tables and their faster variants are built once, the first time they are used, from whichever thread gets there first. Nothing else is shared, and a single hash is not safe to write from two threads at once, the same as in Go.

## Hardware

SHA-1 and SHA-256 run on the SHA instructions when the processor has them, SHA-NI on x86 and the Armv8 SHA instructions on arm64, and SHA-512 runs on the Armv8.2 SHA-512 instructions. That makes them several times faster than the portable code. The choice is made at run time, the way Go makes it, so a program built for any x86-64 or arm64 machine still runs on one without the instructions, and the compiler does not need any flags for it.

As in Go, `GODEBUG` turns them off. `GODEBUG=cpu.all=off` runs the portable code for everything, and `cpu.sha` on x86 or `cpu.sha1`, `cpu.sha2` and `cpu.sha512` on arm64 turn off one at a time. It is read once, the first time a hash needs to know. Building with `-DBURROW_PUREGO` leaves the hardware code out altogether, like Go's `purego` build tag.

## What is not here yet

Apart from SHA-3, Go's hashes can save their state with `MarshalBinary` and pick it up again with `UnmarshalBinary`, and the newer ones can `Clone` themselves. Those need a way to ask a `Hash` whether it also implements another interface, which comes with the `encoding` package (#185). CRC-32 in Go uses the SSE 4.2 and ARMv8 CRC instructions when it can, and burrow uses slicing by 8 tables for now, which is what Go falls back to on machines without them. SHA-512 on x86 is the portable code too, where Go has an AVX2 version. SHA-3 is portable everywhere, and it is close anyway: its portable code runs at about 90% of the speed Go gets from the SHA-3 instructions on Apple chips, and at about twice the speed of Go's portable code.

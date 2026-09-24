# Hashing and checksums

`burrow/hash.h` is Go's `hash`: the interfaces that every hash function speaks. Four packages implement them so far. `hash/crc32` and `hash/crc64` are cyclic redundancy checks, `hash/adler32` is the checksum zlib uses, and `hash/fnv` is the Fowler, Noll and Vo family of non-cryptographic hashes. The cryptographic hashes under `crypto` will implement the same interfaces when they land, so code written against `Hash` today will take a SHA-256 later without changes.

None of these are safe against someone choosing the input on purpose. They are for catching accidental corruption and for spreading keys across buckets.

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

## Memory

A hash made by one of the `_new` functions comes from the allocator you pass and holds no other memory, so it goes away with the arena. The IEEE and Castagnoli tables and their faster variants are built once, the first time they are used, from whichever thread gets there first. Nothing else is shared, and a single hash is not safe to write from two threads at once, the same as in Go.

## What is not here yet

Go's hashes can save their state with `MarshalBinary` and pick it up again with `UnmarshalBinary`, and the newer ones can `Clone` themselves. Those need a way to ask a `Hash` whether it also implements another interface, which comes with the `encoding` package (#185). CRC-32 in Go uses the SSE 4.2 and ARMv8 CRC instructions when it can, and burrow uses slicing by 8 tables for now, which is what Go falls back to on machines without them.

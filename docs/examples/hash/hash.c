#include <stdio.h>

#include "burrow/burrow.h"
#include "burrow/mem/heap.h"

// doc: generic
static void print_sum(Alloc *a, Str name, Hash h, Slice data) {
    hash_reset(h);
    hash_write(h, data, NULL);
    Slice sum = hash_sum(a, h, slice_nil(TYPE_BYTE));
    printf(BURROW_STR_FMT ":", BURROW_STR_ARG(name));
    for (Int i = 0; i < sum.len; i++)
        printf(" %02x", ((const Byte *)sum.p)[i]);
    printf("\n");
}
// doc: end

static void oneshot(Alloc *a) {
    // doc: oneshot
    Slice data = slice_from_str(a, BURROW_S("hello, world"));
    uint32_t ieee = crc32_checksum_ieee(data);
    uint32_t adler = adler32_checksum(data);
    // doc: end
    printf("%08x %08x\n", ieee, adler);

    // doc: castagnoli
    const Crc32Table *c = crc32_make_table(NULL, CRC32_CASTAGNOLI);
    uint32_t crc32c = crc32_checksum(data, c);
    // doc: end
    printf("%08x\n", crc32c);

    // doc: crc64
    const Crc64Table *iso = crc64_make_table(a, CRC64_ISO);
    uint64_t sum64 = crc64_checksum(data, iso);
    // doc: end
    printf("%016llx\n", (unsigned long long)sum64);
}

static void streaming(Alloc *a) {
    // doc: stream
    HashHash32 h = crc32_new_ieee(a);
    Hash as_hash = hash_hash32_as_hash(h);
    hash_write(as_hash, slice_from_str(a, BURROW_S("hello, ")), NULL);
    hash_write(as_hash, slice_from_str(a, BURROW_S("world")), NULL);
    uint32_t sum = hash_hash32_sum32(h);
    // doc: end
    printf("%08x\n", sum);

    // doc: update
    uint32_t crc =
        crc32_update(0, crc32_ieee_table, slice_from_str(a, BURROW_S("hello, ")));
    crc = crc32_update(crc, crc32_ieee_table, slice_from_str(a, BURROW_S("world")));
    // doc: end
    printf("%08x\n", crc);

    // doc: writer
    IoWriter w = hash_as_io_writer(as_hash);
    hash_reset(as_hash);
    fmt_fprintf_v(w, "%s, %s", BURROW_S("hello"), BURROW_S("world"));
    // doc: end
    printf("%08x\n", hash_hash32_sum32(h));
}

static void fnv(Alloc *a) {
    Slice data = slice_from_str(a, BURROW_S("hello, world"));
    // doc: fnv
    print_sum(a, BURROW_S("fnv32a"), hash_hash32_as_hash(fnv_new32a(a)), data);
    print_sum(a, BURROW_S("fnv64a"), hash_hash64_as_hash(fnv_new64a(a)), data);
    print_sum(a, BURROW_S("fnv128a"), fnv_new128a(a), data);
    // doc: end
}

int main(void) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    oneshot(a);
    streaming(a);
    fnv(a);
    arena_free(&ar);
    return 0;
}

/* Output:
ffab723a 1d540489
6999a41f
16c45c0eb1d9c2ec
ffab723a
ffab723a
ffab723a
fnv32a: 4d 0e a4 1d
fnv64a: 17 a1 a4 f2 67 be 63 3d
fnv128a: 4f 2e a2 0c f7 3d cc 0f f0 d6 a3 62 4c d2 66 05
*/

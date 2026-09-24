#include <stdint.h>
#include <stdio.h>

#include "burrow/burrow.h"
#include "burrow/encoding/binary.h"
#include "burrow/mem/arena.h"

// doc: declare
BURROW_ARRAY_TYPE(Magic, uint8_t, 4);

#define HEADER_FIELDS(F, T)                                                            \
    F(T, Magic, Magic, "")                                                             \
    F(T, uint16_t, Version, "")                                                        \
    F(T, uint32_t, Count, "")                                                          \
    F(T, double, Scale, "")
BURROW_STRUCT(Header, HEADER_FIELDS);
// doc: end

BURROW_SLICE_TYPE(IntSlice, Int);

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    // doc: order
    Byte b[4];
    Slice buf = slice_from(b, 4, 4, TYPE_BYTE);
    binary_big_endian_put_uint32(buf, 0xcafe0102);
    uint32_t le = binary_little_endian_uint32(buf);
    Slice out = binary_big_endian_append_uint16(a, slice_nil(TYPE_BYTE), 0xbeef);
    // doc: end
    printf("bytes: %02x %02x %02x %02x\n", b[0], b[1], b[2], b[3]);
    printf("le: %#x\n", le);
    printf("append: %02x %02x\n", ((Byte *)out.p)[0], ((Byte *)out.p)[1]);

    // doc: write
    Header h = {{{'B', 'R', 'W', '1'}}, 2, 1000, 0.5};
    BytesBuffer w = BYTES_BUFFER(a);
    Error err = binary_write(a, bytes_buffer_as_io_writer(&w), binary_big_endian,
                             BURROW_ANY(TYPE_OF(Header), &h));
    // doc: end
    Slice wb = bytes_buffer_bytes(&w);
    printf("size: %lld, written: %lld\n",
           (long long)binary_size(BURROW_ANY(TYPE_OF(Header), &h)), (long long)wb.len);
    printf("bytes:");
    for (Int i = 0; i < wb.len; i++)
        printf(" %02x", ((Byte *)wb.p)[i]);
    printf("\n");

    // doc: read
    Header back;
    BytesReader r;
    bytes_reader_reset(&r, wb);
    err = binary_read(a, bytes_reader_as_io_reader(&r), binary_big_endian,
                      BURROW_ANY(TYPE_OF(Header), &back));
    // doc: end
    printf("read: %.4s v%u count %u scale %g\n", (const char *)back.Magic.v,
           (unsigned)back.Version, (unsigned)back.Count, back.Scale);

    // doc: varint
    Slice v = binary_append_uvarint(a, slice_nil(TYPE_BYTE), 300);
    v = binary_append_varint(a, v, -3);
    Int n1, n2;
    uint64_t x = binary_uvarint(v, &n1);
    int64_t y = binary_varint(slice_sub(v, n1, v.len), &n2);
    // doc: end
    printf("varint: %d bytes:", (int)v.len);
    for (Int i = 0; i < v.len; i++)
        printf(" %02x", ((Byte *)v.p)[i]);
    printf("\n");
    printf("back: %llu (%lld bytes), %lld (%lld bytes)\n", (unsigned long long)x,
           (long long)n1, (long long)y, (long long)n2);

    // doc: bad
    Int nums[2] = {1, 2};
    IntSlice ns = slice_from(nums, 2, 2, TYPE_INT);
    err = binary_write(a, bytes_buffer_as_io_writer(&w), binary_little_endian,
                       BURROW_ANY(TYPE_OF(IntSlice), &ns));
    // doc: end
    printf("err: " BURROW_STR_FMT "\n", BURROW_STR_ARG(error_text(err)));

    arena_free(&ar);
    return 0;
}

/* Output:
bytes: ca fe 01 02
le: 0x201feca
append: be ef
size: 18, written: 18
bytes: 42 52 57 31 00 02 00 00 03 e8 3f e0 00 00 00 00 00 00
read: BRW1 v2 count 1000 scale 0.5
varint: 3 bytes: ac 02 05
back: 300 (2 bytes), -3 (1 bytes)
err: binary.Write: some values are not fixed-sized in type []int
*/

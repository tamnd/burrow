// doc: main
#define BURROW_SHORT 1
#include "burrow/burrow.h"
#include "burrow/encoding/binary.h"
#include "burrow/hash/crc32.h"

/* The message with its CRC-32 after it, as 4 bytes, big endian. */
static Slice seal(Alloc *a, Slice msg) {
    return binary_big_endian_append_uint32(a, slices_clone(a, msg),
                                           crc32_checksum_ieee(msg));
}

/* Whether the last 4 bytes are the CRC-32 of the rest. */
static bool intact(Slice data) {
    if (data.len < 4)
        return false;
    Slice msg = slice_sub(data, 0, data.len - 4);
    uint32_t want = binary_big_endian_uint32(slice_sub(data, data.len - 4, data.len));
    return crc32_checksum_ieee(msg) == want;
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    /* A CRC catches the damage that disks and networks do, such as a flipped
     * bit or a lost byte. It does not stop someone changing the data on
     * purpose, since they can work out a new CRC as well. Use an HMAC for
     * that. */
    Slice msg = BURROW_B("hello world");
    uint32_t sum = crc32_checksum_ieee(msg);
    fmt_printf_v("crc32 of %q is %08x\n", msg, sum);

    Slice data = seal(a, msg);
    bool ok = intact(data);
    fmt_printf_v("%d bytes, intact: %t\n", data.len, ok);

    AT(Byte, data, 4) ^= 0x01; /* one bit, "hello" becomes "helln" */
    ok = intact(data);
    fmt_printf_v("after one bit flips, intact: %t\n", ok);

    ok = intact(slice_sub(data, 1, data.len));
    fmt_printf_v("with the first byte gone, intact: %t\n", ok);
    arena_free(&ar);
}
// doc: end

/* Output:
crc32 of "hello world" is 0d4a1185
15 bytes, intact: true
after one bit flips, intact: false
with the first byte gone, intact: false
*/

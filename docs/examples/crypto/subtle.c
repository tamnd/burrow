#include <stdio.h>

#include "burrow/burrow.h"

static Slice bs(Byte *p, Int n) {
    return slice_from(p, n, n, TYPE_BYTE);
}

// doc: verify
static bool tag_ok(Slice got, Slice want) {
    return subtle_constant_time_compare(got, want) == 1;
}
// doc: end

static void compare(void) {
    Byte want[4] = {0xde, 0xad, 0xbe, 0xef};
    Byte same[4] = {0xde, 0xad, 0xbe, 0xef};
    Byte last[4] = {0xde, 0xad, 0xbe, 0xee};
    printf("%d\n", tag_ok(bs(same, 4), bs(want, 4)));
    printf("%d\n", tag_ok(bs(last, 4), bs(want, 4)));
    printf("%d\n", tag_ok(bs(same, 2), bs(want, 4)));
}

static void select_and_copy(void) {
    Byte secret = 'y';
    // doc: select
    Int is_y = subtle_constant_time_byte_eq(secret, 'y');
    Int n = subtle_constant_time_select(is_y, 10, 20);
    // doc: end
    printf("%d\n", (int)n);

    // doc: copy
    Byte key[4] = {1, 2, 3, 4};
    Byte fallback[4] = {9, 9, 9, 9};
    subtle_constant_time_copy(1 ^ is_y, bs(key, 4), bs(fallback, 4));
    // doc: end
    printf("%d %d %d %d\n", key[0], key[1], key[2], key[3]);
}

static void xor_pad(void) {
    // doc: xor
    Byte pad[5] = {0x10, 0x20, 0x30, 0x40, 0x50};
    Byte msg[5] = {'h', 'e', 'l', 'l', 'o'};
    Byte out[5];
    Int n = subtle_xor_bytes(bs(out, 5), bs(msg, 5), bs(pad, 5));
    // doc: end
    printf("%d:", (int)n);
    for (Int i = 0; i < n; i++)
        printf(" %02x", out[i]);
    printf("\n");
}

// doc: dit
static void sign(void *env) {
    Slice *digest = env;
    /* Arithmetic on the key goes here. On arm64 it runs with DIT on, and so
     * does any goroutine it starts. */
    (void)digest;
}

static void sign_with_dit(Slice digest) {
    subtle_with_data_independent_timing(BURROW_FN(Func, sign, &digest));
}
// doc: end

static void body(void *env) {
    (void)env;
    compare();
    select_and_copy();
    xor_pad();
    Byte digest[32] = {0};
    sign_with_dit(bs(digest, 32));
    printf("done\n");
}

int main(void) {
    runtime_main(BURROW_FN(Func, body, NULL));
}

/* Output:
1
0
0
10
1 2 3 4
5: 78 45 5c 2c 3f
done
*/

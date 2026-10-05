#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "burrow/burrow.h"

static Slice unhex(Alloc *a, const char *s) {
    Error err = BURROW_NO_ERROR;
    return hex_decode_string(a, str_from_cstr(s), &err);
}

static void print_hex(Alloc *a, Slice b) {
    Str s = hex_encode_to_string(a, b);
    printf("%.*s\n", (int)s.len, (const char *)s.p);
}

static void print_int(Alloc *a, const BigInt *x) {
    Str s = big_int_text(x, a, 16);
    printf("%.*s\n", (int)s.len, (const char *)s.p);
}

static void points(Alloc *a) {
    // doc: points
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
    // doc: end
}

static void encoding(Alloc *a) {
    // doc: encoding
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
    // doc: end
}

static void generic(Alloc *a) {
    // doc: generic
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
    // doc: end
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    points(a);
    encoding(a);
    generic(a);
    arena_free(&ar);
    return 0;
}

/* Output:
7cf27b188d034f7e8a52380304b51ac3c08969e277f21b35a60b48fc47669978
1 1
1
65 33
036b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296
1
1 1
48 1
*/

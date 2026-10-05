#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "burrow/burrow.h"

static Slice text(const char *s) {
    Int n = (Int)strlen(s);
    return slice_from((void *)(uintptr_t)s, n, n, TYPE_BYTE);
}

static BigInt *from_hex(Alloc *a, const char *s) {
    BigInt *z = big_new_int(a, 0);
    bool ok;
    big_int_set_string(z, str_from_cstr(s), 16, &ok);
    return z;
}

static void print_error(Error err) {
    if (BURROW_OK(err)) {
        printf("ok\n");
        return;
    }
    Str s = error_text(err);
    printf("%.*s\n", (int)s.len, (const char *)s.p);
}

/* The 1024 bit parameters and key of Go's TestSignAndVerify. */
static DsaPrivateKey test_key(Alloc *a) {
    DsaPrivateKey k = {0};
    k.public_key.parameters.p = from_hex(
        a, "A9B5B793FB4785793D246BAE77E8FF63CA52F442DA763C440259919FE1BC1D6065A9350637"
           "A04F75A2F039401D49F08E066C4D275A5A65DA5684BC563C14289D7AB8A67163BFBF79D859"
           "72619AD2CFF55AB0EE77A9002B0EF96293BDD0F42685EBB2C66C327079F6C98000FBCB79AA"
           "CDE1BC6F9D5C7B1A97E3D9D54ED7951FEF");
    k.public_key.parameters.q = from_hex(a, "E1D3391245933D68A0714ED34BBCB7A1F422B9C1");
    k.public_key.parameters.g = from_hex(
        a, "634364FC25248933D01D1993ECABD0657CC0CB2CEED7ED2E3E8AECDFCDC4A25C3B15E9E3B1"
           "63ACA2984B5539181F3EFF1A5E8903D71D5B95DA4F27202B77D2C44B430BB53741A8D59A8F"
           "86887525C9F2A6A5980A195EAA7F2FF910064301DEF89D3AA213E1FAC7768D89365318E370"
           "AF54A112EFBA9246D9158386BA1B4EEFDA");
    k.public_key.y = from_hex(
        a, "32969E5780CFE1C849A1C276D7AEB4F38A23B591739AA2FE197349AEEBD31366AEE5EB7E6C"
           "6DDB7C57D02432B30DB5AA66D9884299FAA72568944E4EEDC92EA3FBC6F39F53412FBCC563"
           "208F7C15B737AC8910DBC2D9C9B8C001E72FDC40EB694AB1F06A5A2DBD18D9E36C66F31F56"
           "6742F11EC0A52E9F7B89355C02FB5D32D2");
    k.x = from_hex(a, "5078D4D29795CBE76D3AACFE48C9AF0BCDBEE91A");
    return k;
}

static void sign(Alloc *a) {
    // doc: sign
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
    // doc: end
}

static void generate(Alloc *a) {
    // doc: generate
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
    // doc: end
}

static void errors(Alloc *a) {
    // doc: errors
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
    // doc: end
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    sign(a);
    generate(a);
    errors(a);
    arena_free(&ar);
    return 0;
}

/* Output:
signature verified: true
other hash verified: false
x < q: true
y < p: true
crypto/dsa: invalid ParameterSizes
crypto/dsa: parameters not set up before generating key
crypto/dsa: invalid public key
1
*/

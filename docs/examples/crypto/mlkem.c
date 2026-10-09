#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "burrow/burrow.h"

static void print_hex(Alloc *a, Slice b) {
    Str s = hex_encode_to_string(a, b);
    printf("%.*s\n", (int)s.len, (const char *)s.p);
}

static void print_error(Error err) {
    if (BURROW_OK(err)) {
        printf("ok\n");
        return;
    }
    Str s = error_text(err);
    printf("%.*s\n", (int)s.len, (const char *)s.p);
}

// doc: bob
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
// doc: end

static void exchange(Alloc *a) {
    // doc: alice
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
    // doc: end
}

static void fixed(Alloc *a) {
    // doc: fixed
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
    // doc: end
}

static void kem(Alloc *a) {
    // doc: kem
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
    // doc: end
}

static void errors(Alloc *a) {
    // doc: errors
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
    // doc: end
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    exchange(a);
    fixed(a);
    kem(a);
    errors(a);
    arena_free(&ar);
    return 0;
}

/* Output:
1184 1088 32 true
5501fc523b745f41762a188de44a59b920f430146204ee4e793732396df7aa48
5501fc523b745f41762a188de44a59b920f430146204ee4e793732396df7aa48
DecapsulationKey1024 1568 true
mlkem: invalid seed length
mlkem: invalid polynomial encoding
mlkem: invalid ciphertext length
ok
32
*/

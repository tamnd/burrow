#include "burrow/testing/cryptotest.h"
#include "burrow/burrow.h"
#include "burrow/crypto/ecdsa.h"
#include "burrow/crypto/elliptic.h"
#include "burrow/crypto/rand.h"
#include "burrow/mem/arena.h"

// doc: seed
static void TestSameKeys(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    cryptotest_set_global_random(t, 1);
    Byte b[8];
    crypto_rand_read(slice_from(b, 8, 8, TYPE_BYTE), NULL);
    Slice first = slice_from(b, 8, 8, TYPE_BYTE);
    fmt_printf_v("first bytes: %x\n", first);

    Error err = BURROW_NO_ERROR;
    cryptotest_set_global_random(t, 7);
    EcdsaPrivateKey *k1 = ecdsa_generate_key(a, elliptic_p256(), (IoReader){0}, &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    cryptotest_set_global_random(t, 7);
    EcdsaPrivateKey *k2 = ecdsa_generate_key(a, elliptic_p256(), (IoReader){0}, &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    Any v = BURROW_ANY(TYPE_ECDSA_PRIVATE_KEY, k2);
    fmt_printf_v("same key both times: %t\n", ecdsa_private_key_equal(k1, v));
    arena_free(&ar);
}
// doc: end

#define TESTS(X) X(TestSameKeys)
TESTING_MAIN(TESTS)

/* Output:
first bytes: 6ae6783f4fbde91b
same key both times: true
PASS
*/

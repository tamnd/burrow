// doc: main
#define BURROW_SHORT 1
#include "burrow/burrow.h"
#include "burrow/crypto/hmac.h"
#include "burrow/crypto/sha256.h"
#include "burrow/encoding/hex.h"

/* The HMAC-SHA256 of message under key, as hex. */
static Str sign(Alloc *a, Slice key, Slice message) {
    Hash mac = hmac_new(a, sha256_new, key);
    hash_write(mac, message, NULL);
    return hex_encode_to_string(a, hash_sum(a, mac, slice_nil(TYPE_BYTE)));
}

/* Whether sig is the right signature for message. hmac_equal takes the same
 * time however many bytes match, so timing it gives nothing away. */
static bool verify(Alloc *a, Slice key, Slice message, Str sig) {
    Error err = NO_ERROR;
    Slice got = hex_decode_string(a, sig, &err);
    if (FAILED(err))
        return false;
    Hash mac = hmac_new(a, sha256_new, key);
    hash_write(mac, message, NULL);
    return hmac_equal(got, hash_sum(a, mac, slice_nil(TYPE_BYTE)));
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    /* The key and message from test case 2 of RFC 4231, so the signature
     * can be checked against the RFC. */
    Slice key = BURROW_B("Jefe");
    Slice message = BURROW_B("what do ya want for nothing?");
    Str sig = sign(a, key, message);
    fmt_println_v(sig);

    bool good = verify(a, key, message, sig);
    bool changed = verify(a, key, BURROW_B("what do ya want for something?"), sig);
    bool wrong_key = verify(a, BURROW_B("Jeff"), message, sig);
    fmt_printf_v("same message: %t\n", good);
    fmt_printf_v("changed message: %t\n", changed);
    fmt_printf_v("wrong key: %t\n", wrong_key);
    arena_free(&ar);
}
// doc: end

/* Output:
5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843
same message: true
changed message: false
wrong key: false
*/

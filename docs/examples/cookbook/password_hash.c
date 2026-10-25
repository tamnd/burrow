// doc: main
#define BURROW_SHORT 1
#include "burrow/burrow.h"

enum { salt_len = 16, key_len = 32, rounds = 600000 };

/* Turns a password into "salt$key", both in hex, ready to store. The salt is
 * random, so two people with the same password get different keys. */
static Str hash_password(Alloc *a, Str password, Error *err) {
    Slice salt = slice_make(a, TYPE_BYTE, salt_len, salt_len);
    crypto_rand_read(salt, err);
    if (FAILED(*err))
        return S("");
    Slice key = pbkdf2_key(a, sha256_new, password, salt, rounds, key_len, err);
    if (FAILED(*err))
        return S("");
    return fmt_sprintf_v(a, "%x$%x", salt, key);
}

/* Works out the key again from the stored salt and compares the two in
 * constant time, so how long it takes says nothing about where they differ. */
static bool check_password(Alloc *a, Str stored, Str password) {
    Str key_hex;
    bool found;
    Str salt_hex = strings_cut(stored, S("$"), &key_hex, &found);
    if (!found)
        return false;
    Error err = NO_ERROR;
    Slice salt = hex_decode_string(a, salt_hex, &err);
    if (FAILED(err))
        return false;
    Slice want = hex_decode_string(a, key_hex, &err);
    if (FAILED(err))
        return false;
    Slice got = pbkdf2_key(a, sha256_new, password, salt, rounds, key_len, &err);
    if (FAILED(err))
        return false;
    return subtle_constant_time_compare(got, want) == 1;
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    Error err = NO_ERROR;
    Str stored = hash_password(a, S("correct horse"), &err);
    if (FAILED(err)) {
        fmt_println_v(err);
        return 1;
    }
    Str again = hash_password(a, S("correct horse"), &err);
    if (FAILED(err)) {
        fmt_println_v(err);
        return 1;
    }
    fmt_println_v("stored", stored.len, "characters");
    bool differ = !str_eq(stored, again);
    fmt_printf_v("same password, different hash: %t\n", differ);

    bool right = check_password(a, stored, S("correct horse"));
    bool wrong = check_password(a, stored, S("correct horse!"));
    fmt_printf_v("right password: %t, wrong password: %t\n", right, wrong);
    arena_free(&ar);
}
// doc: end

/* Output:
stored 97 characters
same password, different hash: true
right password: true, wrong password: false
*/

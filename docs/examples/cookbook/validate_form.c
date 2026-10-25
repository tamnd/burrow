// doc: main
#define BURROW_SHORT 1
#include "burrow/burrow.h"

#define SIGNUP_FIELDS(F, T)                                                            \
    F(T, Str, Name, "")                                                                \
    F(T, Str, Email, "")                                                               \
    F(T, Int, Age, "")
BURROW_STRUCT(Signup, SIGNUP_FIELDS);

/* Sentinel errors that callers can test for with errors_is. */
BURROW_SENTINEL_ERROR(err_missing, "missing");
BURROW_SENTINEL_ERROR(err_invalid, "invalid");

/* Checks every field and reports every problem, not just the first, so
 * someone filling in a form can fix them all in one go. errors_join gives
 * NO_ERROR when every error in the list is NO_ERROR. */
static Error validate(Alloc *a, const Signup *s) {
    Slice errs = slice_make(a, TYPE_ERROR, 0, 3);
    if (s->Name.len == 0)
        errs = BURROW_APPEND(Error, a, errs, fmt_errorf_v("name: %w", err_missing));
    if (strings_count(s->Email, S("@")) != 1)
        errs = BURROW_APPEND(Error, a, errs,
                             fmt_errorf_v("email %q: %w", s->Email, err_invalid));
    if (s->Age < 13 || s->Age > 130)
        errs = BURROW_APPEND(
            Error, a, errs,
            fmt_errorf_v("age %d: %w, must be 13 to 130", s->Age, err_invalid));
    return errors_join(a, errs);
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    Signup good = {S("Ada"), S("ada@example.com"), 36};
    Error err = validate(a, &good);
    bool ok = !FAILED(err);
    fmt_printf_v("good form passes: %t\n", ok);

    /* The joined error prints one problem to a line, and errors_is looks
     * through all of them. */
    Signup bad = {S(""), S("ada.example.com"), 7};
    err = validate(a, &bad);
    fmt_println_v(err);
    bool missing = errors_is(err, err_missing);
    bool invalid = errors_is(err, err_invalid);
    fmt_printf_v("missing: %t, invalid: %t\n", missing, invalid);
    arena_free(&ar);
}
// doc: end

/* Output:
good form passes: true
name: missing
email "ada.example.com": invalid
age 7: invalid, must be 13 to 130
missing: true, invalid: true
*/

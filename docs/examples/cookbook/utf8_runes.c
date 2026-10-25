// doc: main
#define BURROW_SHORT 1
#include "burrow/burrow.h"

/* The longest start of s that is at most n bytes and does not cut a character
 * in half. Cutting at a plain byte count can leave the first byte or two of a
 * character at the end, which is not valid UTF-8 and prints as garbage. */
static Str cut_runes(Str s, Int n) {
    Int end = 0, i;
    Rune r;
    for (StrIter it = str_runes(s); str_next_rune(&it, &i, &r);) {
        if (i + utf8_rune_len(r) > n)
            break;
        end = i + utf8_rune_len(r);
    }
    return str_from_bytes(s.p, end);
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    /* A Str holds bytes, and outside ASCII one character takes two to four of
     * them. The escapes spell out "naive cafe" with accents and the two
     * characters of Tokyo. */
    Str s = S("na\xc3\xafve caf\xc3\xa9, \xe6\x9d\xb1\xe4\xba\xac");
    fmt_printf_v("%d bytes, %d runes\n", s.len, utf8_rune_count_in_string(s));

    /* The rune loop decodes as it goes and gives the byte offset each rune
     * starts at, which is what slicing needs. */
    Int i;
    Rune r;
    for (StrIter it = str_runes(s); str_next_rune(&it, &i, &r);) {
        if (r >= 0x80)
            fmt_printf_v("%2d: %c %U, %d bytes\n", i, r, r, utf8_rune_len(r));
    }

    Str cut = str_from_bytes(s.p, 16);
    bool cut_ok = utf8_valid_string(cut);
    Str safe = cut_runes(s, 16);
    bool safe_ok = utf8_valid_string(safe);
    fmt_printf_v("first 16 bytes valid: %t\n", cut_ok);
    fmt_printf_v("truncated to 16: %q, valid: %t\n", safe, safe_ok);
    fmt_println_v(strings_to_upper(a, s));
    arena_free(&ar);
}
// doc: end

/* Output:
20 bytes, 14 runes
 2: ï U+00EF, 2 bytes
10: é U+00E9, 2 bytes
14: 東 U+6771, 3 bytes
17: 京 U+4EAC, 3 bytes
first 16 bytes valid: false
truncated to 16: "naïve café, ", valid: true
NAÏVE CAFÉ, 東京
*/

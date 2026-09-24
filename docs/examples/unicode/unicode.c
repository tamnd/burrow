#include <stdio.h>

#include "burrow/burrow.h"
#include "burrow/unicode.h"

static void classify(void) {
    // doc: classify
    Str s = BURROW_S("Go 1.27, \xce\xb1\xce\xb2!");
    Int i;
    Rune r;
    for (StrIter it = str_runes(s); str_next_rune(&it, &i, &r);) {
        printf("%-7s", unicode_is_letter(r)  ? "letter"
                       : unicode_is_digit(r) ? "digit"
                       : unicode_is_space(r) ? "space"
                       : unicode_is_punct(r) ? "punct"
                                             : "other");
        printf(" U+%04X%s\n", (unsigned)r, unicode_is_upper(r) ? " upper" : "");
    }
    // doc: end
}

static void cases(void) {
    // doc: case
    Rune dz = 0x01C6; // the digraph dz with caron, as one lower case letter
    Rune upper = unicode_to_upper(dz);
    Rune title = unicode_to_title(dz);
    Rune lower = unicode_to_lower(upper);
    // doc: end
    printf("%04X %04X %04X\n", (unsigned)upper, (unsigned)title, (unsigned)lower);

    // doc: special
    Rune i_upper = unicode_to_upper('i');
    Rune tr_upper = unicode_special_case_to_upper(unicode_turkish_case, 'i');
    // doc: end
    printf("%04X %04X\n", (unsigned)i_upper, (unsigned)tr_upper);
}

static void fold(void) {
    // doc: fold
    printf("U+%04X", (unsigned)'k');
    for (Rune r = unicode_simple_fold('k'); r != 'k'; r = unicode_simple_fold(r))
        printf(" U+%04X", (unsigned)r);
    printf("\n");
    // doc: end
}

static void tables(void) {
    // doc: tables
    Rune alpha = 0x03B1;
    bool greek = unicode_is(unicode_greek, alpha);
    bool either = unicode_in_v(alpha, unicode_latin, unicode_cyrillic);
    // doc: end
    printf("%d %d\n", greek, either);

    // doc: lookup
    const UnicodeRangeTable *t =
        unicode_table_map_get(unicode_scripts, BURROW_S("Hiragana"));
    if (t != NULL && unicode_is(t, 0x3042))
        printf("U+3042 is Hiragana\n");
    // doc: end

    // doc: walk
    for (Int k = 0; k < unicode_categories.len; k++) {
        UnicodeNamedTable e = unicode_categories.p[k];
        if (e.name.len == 2 && unicode_is(e.table, 0x20AC))
            printf("U+20AC is in " BURROW_STR_FMT "\n", BURROW_STR_ARG(e.name));
    }
    // doc: end
}

int main(void) {
    classify();
    cases();
    fold();
    tables();
    printf("Unicode %s\n", UNICODE_VERSION);
    return 0;
}

/* Output:
letter  U+0047 upper
letter  U+006F
space   U+0020
digit   U+0031
punct   U+002E
digit   U+0032
digit   U+0037
punct   U+002C
space   U+0020
letter  U+03B1
letter  U+03B2
punct   U+0021
01C4 01C5 01C6
0049 0130
U+006B U+212A U+004B
1 0
U+3042 is Hiragana
U+20AC is in Sc
Unicode 17.0.0
*/

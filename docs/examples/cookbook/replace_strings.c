// doc: main
#define BURROW_SHORT 1
#include "burrow/burrow.h"

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    /* A replacer takes old and new strings in pairs and makes every
     * replacement in one pass over the text. Building it is the slow part, so
     * make one and use it many times. */
    static const Str fill[] = {
        BURROW_S_INIT("{name}"),
        BURROW_S_INIT("Ada"),
        BURROW_S_INIT("{day}"),
        BURROW_S_INIT("Tuesday"),
    };
    StringsReplacer *r =
        strings_new_replacer(a, slice_from((void *)(uintptr_t)fill, 4, 4, TYPE_STRING));
    fmt_println_v(strings_replacer_replace(r, a, S("Hi {name}, see you {day}.")));
    fmt_println_v(strings_replacer_replace(r, a, S("{name} again. {other} stays.")));

    /* Text a replacement puts in is never looked at again, so two words can
     * trade places. Doing it with strings_replace_all one word at a time
     * turns both into the same word. */
    static const Str swap[] = {
        BURROW_S_INIT("cat"),
        BURROW_S_INIT("dog"),
        BURROW_S_INIT("dog"),
        BURROW_S_INIT("cat"),
    };
    Str text = S("the cat chased the dog");
    StringsReplacer *sw =
        strings_new_replacer(a, slice_from((void *)(uintptr_t)swap, 4, 4, TYPE_STRING));
    fmt_println_v(strings_replacer_replace(sw, a, text));
    Str once = strings_replace_all(a, text, S("cat"), S("dog"));
    fmt_println_v(strings_replace_all(a, once, S("dog"), S("cat")));
    strings_replacer_free(r);
    strings_replacer_free(sw);
    arena_free(&ar);
}
// doc: end

/* Output:
Hi Ada, see you Tuesday.
Ada again. {other} stays.
the dog chased the cat
the cat chased the cat
*/

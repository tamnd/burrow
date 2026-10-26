// doc: hello
#define BURROW_SHORT 1
#include "burrow.h"

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    Str body = S("the quick brown fox");
    Slice words = strings_fields(a, body);

    for (Int i = 0; i < words.len; i++)
        fmt_println_v(AT(Str, words, i));

    arena_free(&ar);
    return 0;
}
// doc: end

/* Output:
the
quick
brown
fox
*/

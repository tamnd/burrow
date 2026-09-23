// doc: rule
#include <stdio.h>

#include "burrow/burrow.h"

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    Str name = str_clone(a, BURROW_S("squares"));
    Map *squares = map_make(a, TYPE_INT, TYPE_INT, 0);
    for (Int i = 0; i < 1000; i++) {
        Int sq = i * i;
        map_set(squares, &i, &sq);
    }
    printf(BURROW_STR_FMT " has %lld entries\n", BURROW_STR_ARG(name),
           (long long)map_len(squares));

    arena_free(&ar);
    return 0;
}
// doc: end

/* Output:
squares has 1000 entries
*/

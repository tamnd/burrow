#define BURROW_SHORT 1
#include <stdio.h>

#include "burrow/burrow.h"

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str name = S("gopher");
    Str path = S("usr/local/bin");

    // doc: memory
    Str upper = strings_to_upper(a, name);
    Slice parts = strings_split(a, path, S("/"));
    // doc: end

    printf(STR_FMT " has %lld parts\n", STR_ARG(upper), (long long)parts.len);
    arena_free(&ar);
    return 0;
}

/* Output:
GOPHER has 3 parts
*/

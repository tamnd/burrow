#include "burrow/burrow.h"

static void versions(Alloc *a) {
    // doc: version
    Str pairs[][2] = {
        {BURROW_S("go1.21"), BURROW_S("go1.21rc1")},
        {BURROW_S("go1.21rc1"), BURROW_S("go1.21.0")},
        {BURROW_S("go1.9"), BURROW_S("go1.10")},
        {BURROW_S("go1.20"), BURROW_S("go1.20.0")},
    };
    for (int i = 0; i < 4; i++)
        fmt_println_v(pairs[i][0], pairs[i][1],
                      version_compare(pairs[i][0], pairs[i][1]));

    Str vs[] = {BURROW_S("go1.22.3"), BURROW_S("go1.23rc1"), BURROW_S("go1"),
                BURROW_S("1.22")};
    for (int i = 0; i < 4; i++)
        fmt_printf_v("%-10s valid=%v lang=%q\n", vs[i], version_is_valid(vs[i]),
                     version_lang(a, vs[i]));
    // doc: end
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    versions(arena_allocator(&ar));
    arena_free(&ar);
    return 0;
}

/* Output:
go1.21 go1.21rc1 -1
go1.21rc1 go1.21.0 -1
go1.9 go1.10 -1
go1.20 go1.20.0 0
go1.22.3   valid=true lang="go1.22"
go1.23rc1  valid=true lang="go1.23"
go1        valid=true lang="go1"
1.22       valid=false lang=""
*/

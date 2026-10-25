/* strings, strconv and time with no fmt and no goroutines, which is the
 * smallest useful program and one Go can't build: its runtime always comes
 * along. */
#include <stdio.h>

#include "burrow/burrow.h"

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    double f = strconv_parse_float(BURROW_S("3.25"), 64, &err);
    Str up = strings_to_upper(a, BURROW_S("hello"));
    Time t = time_parse(a, TIME_RFC3339, BURROW_S("2026-03-08T14:30:00-05:00"), &err);
    Str s = time_format(t, a, TIME_KITCHEN);
    printf("%g " BURROW_STR_FMT " " BURROW_STR_FMT "\n", f, BURROW_STR_ARG(up),
           BURROW_STR_ARG(s));
    arena_free(&ar);
    return 0;
}

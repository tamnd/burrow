#include <stdio.h>

#include "burrow/burrow.h"

// doc: zero-return
static Slice nothing(void) {
    return BURROW_ZERO(Slice);
}

static bool unnamed(Str name) {
    return str_eq(name, BURROW_ZERO(Str));
}
// doc: end

// doc: cut
typedef struct StringsCutRet {
    Str before;
    Str after;
    bool found;
} StringsCutRet;
// doc: end

int main(void) {
    // doc: zero
    Str s = {0};
    Slice parts = {0};
    Error err = {0};
    // doc: end

    StringsCutRet cut = {0};
    printf("empty string: %s\n", s.len == 0 ? "yes" : "no");
    printf("nil slice: %s\n",
           slice_is_nil(parts) && slice_is_nil(nothing()) ? "yes" : "no");
    printf("no error: %s\n", BURROW_OK(err) ? "yes" : "no");
    printf("unnamed: %s\n", unnamed(s) ? "yes" : "no");
    printf("found: %s\n", cut.found ? "yes" : "no");
    return 0;
}

/* Output:
empty string: yes
nil slice: yes
no error: yes
unnamed: yes
found: no
*/

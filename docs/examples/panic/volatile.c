#include <stdio.h>

#include "burrow/burrow.h"

static void step(Int i) {
    if (i == 3)
        panic_str(BURROW_S("step 3 has no case for this input"));
}

int main(void) {
    Int n = 10;
    // doc: body
    volatile Int done = 0;
    BURROW_TRY {
        for (Int i = 0; i < n; i++) {
            step(i);
            done++;
        }
    }
    BURROW_CATCH(p) {
        Str why = panic_text(p);
        printf("stopped after %lld: %.*s\n", (long long)done, (int)why.len,
               (const char *)why.p);
    }
    BURROW_TRY_END;
    // doc: end
    return 0;
}

/* Output:
stopped after 3: step 3 has no case for this input
*/

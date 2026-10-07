#include <stdio.h>

#include "burrow/burrow.h"

static void legacy(void *env) {
    (void)env;
    printf("running the legacy code\n");
}

int main(void) {
    // doc: enabled
    if (fips140_enabled())
        printf("FIPS 140-3 mode\n");
    else
        printf("not in FIPS 140-3 mode\n");
    // doc: end

    Str v = fips140_version();
    printf("module version %.*s\n", (int)v.len, (const char *)v.p);
    printf("enforced: %s\n", fips140_enforced() ? "yes" : "no");

    // doc: without
    fips140_without_enforcement(BURROW_FN(Func, legacy, NULL));
    // doc: end
    return 0;
}

/* Output:
not in FIPS 140-3 mode
module version latest
enforced: no
running the legacy code
*/

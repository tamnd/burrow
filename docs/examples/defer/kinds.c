#include <stdio.h>
#include <stdlib.h>

#include "burrow/burrow.h"

static void close_file(void *f) {
    fclose(f);
    printf("closed the file\n");
}

static void report(void *what) {
    printf("cleaned up %s\n", (const char *)what);
}

int main(void) {
    FILE *f = fopen("scratch.txt", "w");
    char *buf = malloc(64);
    Func cleanup = BURROW_FN(Func, report, "the rest");

    BURROW_SCOPE {
        // doc: kinds
        BURROW_DEFER(close_file, f); /* the common case */
        BURROW_DEFER(free, buf);     /* a C function that fits already */
        BURROW_DEFER_FUNC(cleanup);  /* a Func you already have */
        // doc: end
    }
    BURROW_SCOPE_END;
    return 0;
}

/* Output:
cleaned up the rest
closed the file
*/

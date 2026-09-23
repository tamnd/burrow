#include <stdio.h>

#include "burrow/burrow.h"

static void say(void *word) {
    printf("%s\n", (const char *)word);
}

int main(void) {
    // doc: lifo
    BURROW_SCOPE {
        BURROW_DEFER(say, "a");
        BURROW_DEFER(say, "b");
        BURROW_DEFER(say, "c");
    }
    BURROW_SCOPE_END;
    /* prints c b a */
    // doc: end

    // doc: argument
    BURROW_SCOPE {
        const char *word = "first";
        BURROW_DEFER(say, word);
        word = "second";
    }
    BURROW_SCOPE_END;
    /* prints first */
    // doc: end

    // doc: nested
    BURROW_SCOPE {
        BURROW_DEFER(say, "outer");
        BURROW_SCOPE {
            BURROW_DEFER(say, "inner");
        }
        BURROW_SCOPE_END;
        say("between");
    }
    BURROW_SCOPE_END;
    /* prints inner, between, outer */
    // doc: end
    return 0;
}

/* Output:
c
b
a
first
inner
between
outer
*/

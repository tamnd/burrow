#include <stdio.h>

#include "burrow/burrow.h"

static void log_crash(Str msg) {
    printf("the runtime stopped it: %.*s\n", (int)msg.len, (const char *)msg.p);
}

static void divide(Int x, Int y) {
    printf("%lld\n", (long long)int_div(x, y));
}

static void run(Int x, Int y) {
    BURROW_TRY {
        divide(x, y);
    }
    // doc: catch
    BURROW_CATCH(p) {
        const RuntimeError *re = runtime_error_from(p);
        if (re == NULL)
            panic(p);
        log_crash(re->message);
    }
    // doc: end
    BURROW_TRY_END;
}

int main(void) {
    run(10, 2);
    run(10, 0);

    // A panic the program raised on purpose is not a runtime error, so the
    // catch block above sends it on outward, and this one gets it.
    BURROW_TRY {
        BURROW_TRY {
            panic_str(BURROW_S("on purpose"));
        }
        BURROW_CATCH(p) {
            const RuntimeError *re = runtime_error_from(p);
            if (re == NULL)
                panic(p);
            log_crash(re->message);
        }
        BURROW_TRY_END;
    }
    BURROW_CATCH(p) {
        Str text = panic_text(p);
        printf("the program stopped it: %.*s\n", (int)text.len, (const char *)text.p);
    }
    BURROW_TRY_END;
    return 0;
}

/* Output:
5
the runtime stopped it: runtime error: integer divide by zero
the program stopped it: on purpose
*/

#include <stdio.h>

#include "burrow/burrow.h"

static void log_crash(Str msg) {
    printf("caught: %.*s\n", (int)msg.len, (const char *)msg.p);
}

// A request is an index here, and a request past the end is the bug.
static void handle(Int request) {
    Int parts[3] = {10, 20, 30};
    Slice s = slice_from(parts, 3, 3, TYPE_INT);
    printf("part %lld\n", (long long)*(Int *)slice_at(s, request));
}

static void serve(Int request) {
    // doc: try
    BURROW_TRY {
        handle(request);
    }
    BURROW_CATCH(p) {
        log_crash(panic_text(p));
    }
    BURROW_TRY_END;
    // doc: end
}

static void serve_runtime_errors_only(Int request) {
    BURROW_TRY {
        handle(request);
    }
    // doc: runtime
    BURROW_CATCH(p) {
        const RuntimeError *re = runtime_error_from(p);
        if (re != NULL)
            log_crash(re->message);
        else
            panic(p);
    }
    // doc: end
    BURROW_TRY_END;
}

int main(void) {
    serve(1);
    serve(5);
    serve_runtime_errors_only(7);
    return 0;
}

/* Output:
part 20
caught: runtime error: index out of range [5] with length 3
caught: runtime error: index out of range [7] with length 3
*/

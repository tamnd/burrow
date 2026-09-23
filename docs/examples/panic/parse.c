#include <stdio.h>

#include "burrow/burrow.h"

// A parser that panics on input it has no case for, which is the kind of code
// that wants one recovery point at the top rather than a check on every line.
static void parse(Str input) {
    if (input.len == 0)
        panic_str(BURROW_S("nothing to parse"));
    printf("parsed %.*s\n", (int)input.len, (const char *)input.p);
}

static void log_bad_input(Str why) {
    printf("bad input: %.*s\n", (int)why.len, (const char *)why.p);
}

static Error parse_or_fail(Alloc *a, Str input) {
    Error err = BURROW_NO_ERROR;
    // doc: try
    BURROW_TRY {
        parse(input);
    }
    BURROW_CATCH(p) {
        log_bad_input(panic_text(p));
        err = errors_new(a, BURROW_S("bad input"));
    }
    BURROW_TRY_END;
    // doc: end
    return err;
}

int main(void) {
    Arena arena;
    arena_init(&arena, heap_allocator(), 0);
    Alloc *a = arena_allocator(&arena);

    Error err = parse_or_fail(a, BURROW_S("x = 1"));
    printf("failed: %s\n", BURROW_FAILED(err) ? "yes" : "no");
    err = parse_or_fail(a, BURROW_S(""));
    Str msg = error_message(err);
    printf("failed: %.*s\n", (int)msg.len, (const char *)msg.p);

    arena_free(&arena);
    return 0;
}

/* Output:
parsed x = 1
failed: no
bad input: nothing to parse
failed: bad input
*/

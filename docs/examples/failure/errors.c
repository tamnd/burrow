#include <stdio.h>

#include "burrow/burrow.h"

static Error load(Alloc *a) {
    // doc: read-file
    Error err;
    Slice data = os_read_file(a, BURROW_S("config.json"), &err);
    if (BURROW_FAILED(err))
        return err;
    // doc: end
    printf("read %lld bytes\n", (long long)data.len);
    return BURROW_NO_ERROR;
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Error err = load(arena_allocator(&ar));
    Str msg = error_text(err);
    printf("failed: %.*s\n", (int)msg.len, (const char *)msg.p);
    arena_free(&ar);
    return 0;
}

/* Output:
failed: open config.json: no such file or directory
*/

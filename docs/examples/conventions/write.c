#include <stdio.h>

#include "burrow/burrow.h"

static Error save(Str path, Slice data) {
    // doc: write-file
    Error err = os_write_file(path, data, 0644);
    if (BURROW_FAILED(err))
        return err;
    // doc: end
    printf("wrote %lld bytes to %.*s\n", (long long)data.len, (int)path.len,
           (const char *)path.p);
    return BURROW_NO_ERROR;
}

int main(void) {
    static Byte text[] = "{}\n";
    Int n = (Int)sizeof text - 1;
    Error err = save(BURROW_S("config.json"), slice_from(text, n, n, TYPE_BYTE));
    if (BURROW_FAILED(err)) {
        Str msg = error_text(err);
        printf("failed: %.*s\n", (int)msg.len, (const char *)msg.p);
    }
    return 0;
}

/* Output:
wrote 3 bytes to config.json
*/

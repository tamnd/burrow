#include <stdio.h>

#include "burrow/burrow.h"

// strconv is not ported yet, so this is a cut down strconv_atoi with the
// signature the real one will have. When the real one lands this file stops
// compiling, which is the reminder to delete the stand-in.
BURROW_SENTINEL_ERROR(strconv_err_syntax, "invalid syntax");

// doc: out
static Int strconv_atoi(Str s, Error *err) {
    Int n = 0;
    if (s.len == 0) {
        BURROW_OUT(err, strconv_err_syntax);
        return 0;
    }
    for (Int i = 0; i < s.len; i++) {
        if (s.p[i] < '0' || s.p[i] > '9') {
            BURROW_OUT(err, strconv_err_syntax);
            return 0;
        }
        n = n * 10 + (s.p[i] - '0');
    }
    return n;
}
// doc: end

// doc: declarations
Int strconv_atoi(Str s, Error *err);
Slice os_read_file(Alloc *a, Str name, Error *err);
// doc: end

static Error port(Str s) {
    // doc: call
    Error err = BURROW_NO_ERROR;
    Int n = strconv_atoi(s, &err);
    if (BURROW_FAILED(err))
        return err;
    // doc: end
    printf("port %lld\n", (long long)n);
    return BURROW_NO_ERROR;
}

static void port_or_zero(Str s) {
    // doc: call-null
    Int n = strconv_atoi(s, NULL);
    // doc: end
    printf("port %lld\n", (long long)n);
}

int main(void) {
    Error err = port(BURROW_S("8080"));
    if (BURROW_OK(err))
        err = port(BURROW_S("http"));
    Str msg = error_text(err);
    printf("failed: %.*s\n", (int)msg.len, (const char *)msg.p);
    port_or_zero(BURROW_S("http"));
    return 0;
}

/* Output:
port 8080
failed: invalid syntax
port 0
*/

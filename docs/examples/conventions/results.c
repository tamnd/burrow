#include <stdio.h>

#include "burrow/burrow.h"

// parse_digits is a cut down strconv_atoi that only knows digits, to show the
// writing side of an out parameter. The calls use the real strconv_atoi.

// doc: out
static Int parse_digits(Str s, Error *err) {
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
    printf("digits %lld\n", (long long)parse_digits(BURROW_S("42"), NULL));
    return 0;
}

/* Output:
port 8080
failed: strconv.Atoi: parsing "http": invalid syntax
port 0
digits 42
*/

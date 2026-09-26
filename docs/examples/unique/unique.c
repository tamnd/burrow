#include <stdio.h>

#include "burrow/burrow.h"

// doc: decl
#define ENDPOINT_FIELDS(F, T)                                                          \
    F(T, Str, host, "")                                                                \
    F(T, Int, port, "")
BURROW_STRUCT(Endpoint, ENDPOINT_FIELDS);
// doc: end

int main(void) {
    // doc: str
    char buf[] = "eth0";
    Str zone = str_from_bytes((const Byte *)buf, 4);
    UniqueHandle h = UNIQUE_MAKE(Str, &zone);
    buf[3] = '1'; /* the handle has its own copy of the bytes */

    Str again = BURROW_S("eth0");
    UniqueHandle g = UNIQUE_MAKE(Str, &again);
    Str back = UNIQUE_VALUE(Str, h);
    printf("%d %.*s\n", unique_handle_eq(h, g), (int)back.len, back.p); /* 1 eth0 */
    // doc: end

    // doc: struct
    Endpoint e1 = {BURROW_S("example.com"), 443};
    Endpoint e2 = {BURROW_S("example.com"), 443};
    Endpoint e3 = {BURROW_S("example.com"), 80};
    UniqueHandle h1 = UNIQUE_MAKE(Endpoint, &e1);
    UniqueHandle h2 = UNIQUE_MAKE(Endpoint, &e2);
    UniqueHandle h3 = UNIQUE_MAKE(Endpoint, &e3);
    printf("%d %d\n", unique_handle_eq(h1, h2), unique_handle_eq(h1, h3)); /* 1 0 */
    printf("%lld\n", (long long)UNIQUE_VALUE(Endpoint, h3).port);          /* 80 */
    // doc: end
    return 0;
}

/* Output:
1 eth0
1 0
80
*/

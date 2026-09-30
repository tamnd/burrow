#include <stdio.h>

#include "burrow/burrow.h"

// doc: decl
#define ORDER_FIELDS(F, T)                                                             \
    F(T, Uuid, ID, "json:\"id\"")                                                      \
    F(T, Int, Qty, "json:\"qty\"")
BURROW_STRUCT(Order, ORDER_FIELDS);
// doc: end

int main(void) {
    Alloc *a = heap_allocator();

    {
        // doc: new
        Uuid id = uuid_new();
        Str s = uuid_string(id, a);
        printf("%d %c\n", (int)s.len, s.p[14]); /* 36 4 */
        // doc: end
    }

    {
        // doc: parse
        Error err;
        Uuid u =
            uuid_parse_str(BURROW_S("{F81D4FAE-7DEC-11D0-A765-00A0C91E6BF6}"), &err);
        Str s = uuid_string(u, a);
        printf("%.*s\n", (int)s.len, s.p); /* f81d4fae-7dec-11d0-a765-00a0c91e6bf6 */

        (void)uuid_parse_str(BURROW_S("f81d4fae-7dec-11d0-a765"), &err);
        Str msg = error_text(err);
        printf("%.*s\n", (int)msg.len, msg.p); /* invalid uuid */
        // doc: end
    }

    {
        // doc: v7
        Uuid first = uuid_new_v7();
        Uuid second = uuid_new_v7();
        printf("%d\n", (int)uuid_cmp(first, second)); /* -1 */
        // doc: end
    }

    {
        // doc: json
        Order o = {uuid_must_parse(BURROW_S("f81d4fae-7dec-11d0-a765-00a0c91e6bf6")),
                   3};
        Error err = BURROW_NO_ERROR;
        Slice out = json_marshal(a, BURROW_ANY(TYPE_OF(Order), &o), &err);
        printf("%.*s\n", (int)out.len, (const char *)out.p);
        // doc: end
    }
    return 0;
}

/* Output:
36 4
f81d4fae-7dec-11d0-a765-00a0c91e6bf6
invalid uuid
-1
{"id":"f81d4fae-7dec-11d0-a765-00a0c91e6bf6","qty":3}
*/

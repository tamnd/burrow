#include <stdio.h>

#include "burrow/burrow.h"
#include "burrow/encoding/pem.h"
#include "burrow/mem/arena.h"

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    // doc: decode
    Slice rest = BURROW_B("Some text before the block\n"
                          "-----BEGIN MESSAGE-----\n"
                          "Proc-Type: 4,ENCRYPTED\n"
                          "Comment: hello there\n"
                          "\n"
                          "aGVsbG8sIHdvcmxk\n"
                          "-----END MESSAGE-----\n"
                          "and some after\n");
    PemBlock *b = pem_decode(a, rest, &rest);
    const Str *comment = BURROW_MAP_GET(Str, Str, b->headers, BURROW_S("Comment"));
    // doc: end
    printf("type: " BURROW_STR_FMT "\n", BURROW_STR_ARG(b->type));
    printf("headers: %d\n", (int)map_len(b->headers));
    printf("comment: " BURROW_STR_FMT "\n", BURROW_STR_ARG(*comment));
    printf("bytes: %.*s\n", (int)b->bytes.len, (const char *)b->bytes.p);
    printf("rest: %.*s", (int)rest.len, (const char *)rest.p);

    // doc: encode
    Map *h = map_make(a, TYPE_STRING, TYPE_STRING, 2);
    BURROW_MAP_SET(Str, Str, h, BURROW_S("Name"), BURROW_S("demo"));
    BURROW_MAP_SET(Str, Str, h, BURROW_S("Proc-Type"), BURROW_S("4,ENCRYPTED"));
    PemBlock out = {
        .type = BURROW_S("MESSAGE"), .headers = h, .bytes = BURROW_B("hello, world")};
    Slice text = pem_encode_to_memory(a, &out);
    // doc: end
    printf("%.*s", (int)text.len, (const char *)text.p);

    arena_free(&ar);
    return 0;
}

/* Output:
type: MESSAGE
headers: 2
comment: hello there
bytes: hello, world
rest: and some after
-----BEGIN MESSAGE-----
Proc-Type: 4,ENCRYPTED
Name: demo

aGVsbG8sIHdvcmxk
-----END MESSAGE-----
*/

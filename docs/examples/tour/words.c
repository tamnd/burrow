// doc: words
#define BURROW_SHORT 1
#include "burrow/burrow.h"

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = os_write_file(S("poem.txt"),
                              BURROW_B("the cat saw the dog\nand the dog ran\n"), 0644);

    Slice data = os_read_file(a, S("poem.txt"), &err);
    if (FAILED(err)) {
        fmt_println_v(err);
        return 1;
    }
    Slice words = strings_fields(a, str_from_bytes(data.p, data.len));
    Map *counts = map_make(a, TYPE_STRING, TYPE_INT, 0);
    for (Int i = 0; i < words.len; i++) {
        Int *n = MAP_GET(Str, Int, counts, AT(Str, words, i));
        MAP_SET(Str, Int, counts, AT(Str, words, i), n != NULL ? *n + 1 : 1);
    }
    fmt_printf_v("%d words: %v\n", words.len, counts);
    arena_free(&ar);
}
// doc: end

/* Output:
9 words: map[and:1 cat:1 dog:2 ran:1 saw:1 the:3]
*/

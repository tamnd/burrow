#include <stdio.h>

#include "burrow/burrow.h"
#include "burrow/container/list.h"
#include "burrow/container/ring.h"
#include "burrow/mem/arena.h"

/* Prints each value with a space before all but the first. */
static void print_value(void *env, Any v) {
    bool *first = env;
    if (!*first)
        fmt_print_v(" ");
    fmt_print_v(v);
    *first = false;
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    // doc: list
    List l = LIST(a);
    ListElement *four = list_push_back(&l, BURROW_ANY_OF(4));
    list_push_front(&l, BURROW_ANY_OF("one"));
    list_insert_before(&l, BURROW_ANY_OF(3.5), four);
    list_move_to_back(&l, list_front(&l));

    for (ListElement *e = list_front(&l); e != NULL; e = list_element_next(e))
        fmt_println_v(e->value); /* 3.5, then 4, then one */
    // doc: end

    // doc: remove
    Any v = list_remove(&l, four);
    fmt_println_v("removed", v, "and", list_len(&l), "left"); /* removed 4 and 2 left */
    // doc: end

    // doc: ring
    Ring *r = ring_new(a, 4);
    for (int i = 1; i <= 4; i++) {
        r->value = any_box(a, BURROW_ANY_OF(i * 10));
        r = ring_next(r);
    }
    r = ring_move(r, 2);
    bool first = true;
    ring_do(r, BURROW_FN(RingDoFunc, print_value, &first)); /* 30 40 10 20 */
    printf("\n");
    // doc: end

    arena_free(&ar);
    return 0;
}

/* Output:
3.5
4
one
removed 4 and 2 left
30 40 10 20
*/

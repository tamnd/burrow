#include <stdio.h>

#include "burrow/burrow.h"

typedef struct Blob {
    Int id;
    Int size;
} Blob;

static Blob config = {1, 64};

int main(void) {
    Alloc *a = heap_allocator();

    {
        // doc: basic
        Blob *b = BURROW_NEW(a, Blob);
        WeakPointer w = weak_make(b);
        printf("%d\n", weak_pointer_value(w) == b); /* 1 */
        mem_free(a, b, sizeof(Blob), _Alignof(Blob));
        printf("%d\n", weak_pointer_value(w) == NULL); /* 1 */
        // doc: end
    }

    {
        // doc: arena
        Arena ar;
        arena_init(&ar, NULL, 0);
        Blob *b = BURROW_NEW(arena_allocator(&ar), Blob);
        WeakPointer w = weak_make(b);
        arena_reset(&ar);
        printf("%d\n", weak_pointer_value(w) == NULL); /* 1 */
        arena_free(&ar);
        // doc: end
    }

    {
        // doc: eq
        Blob *b = BURROW_NEW(a, Blob);
        WeakPointer w1 = weak_make(b);
        WeakPointer w2 = weak_make(b);
        WeakPointer wid = weak_make(&b->id);
        WeakPointer wsize = weak_make(&b->size);
        printf("%d %d\n", weak_pointer_eq(w1, w2), weak_pointer_eq(wid, wsize)); /* 1 0 */
        mem_free(a, b, sizeof(Blob), _Alignof(Blob));
        // doc: end
    }

    {
        // doc: global
        WeakPointer w = weak_make(&config);
        Blob *c = weak_pointer_value(w);
        printf("%d\n", (int)c->size); /* 64 */
        // doc: end
    }
    return 0;
}

/* Output:
1
1
1
1 0
64
*/

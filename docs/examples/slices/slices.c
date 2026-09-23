#include <stdio.h>

#include "burrow/burrow.h"
#include "burrow/mem/heap.h"

static void print(const char *name, Slice s) {
    printf("%s len %lld cap %lld:", name, (long long)s.len, (long long)s.cap);
    for (Int i = 0; i < s.len; i++)
        printf(" %lld", (long long)BURROW_AT(Int, s, i));
    printf("\n");
}

static void make(Alloc *a) {
    // doc: make
    Slice xs = slice_make(a, TYPE_INT, 0, 16);
    // doc: end
    print("make", xs);
}

static void from(void) {
    // doc: from
    Int backing[4] = {10, 20, 30, 40};
    Slice s = slice_from(backing, 4, 4, TYPE_INT);
    // doc: end
    print("from", s);
}

static void at(Alloc *a) {
    Str words[2] = {BURROW_S("first"), BURROW_S("second")};
    Slice parts = slice_from(words, 2, 2, TYPE_STRING);
    Slice xs = slice_make(a, TYPE_INT, 3, 3);
    Int i = 1;

    // doc: at
    Str f = BURROW_AT(Str, parts, i);
    BURROW_AT(Int, xs, 0) = 42; /* it is an lvalue, so this is xs[0] = 42 */
    // doc: end
    printf("at %.*s\n", (int)f.len, (const char *)f.p);
    print("at", xs);
}

static void sub(void) {
    Int backing[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    Slice s = slice_from(backing, 8, 8, TYPE_INT);

    // doc: sub
    Slice m = slice_sub(s, 2, 5);     /* s[2:5]    */
    Slice n = slice_sub3(s, 2, 5, 6); /* s[2:5:6]  */
    // doc: end
    print("m", m);
    print("n", n);
}

static void append(Alloc *a) {
    Slice xs = slice_nil(TYPE_INT);
    Slice ys = slice_make(a, TYPE_INT, 1, 1);
    Int elems[2] = {1, 2};
    Int n = 2;

    // doc: append
    xs = slice_append(a, xs, elems, n);
    xs = BURROW_APPEND(Int, a, xs, 42); /* one value, no temporary */
    ys = slice_append_slice(a, ys, xs); /* append(ys, xs...) */
    // doc: end
    print("xs", xs);
    print("ys", ys);
}

static void share(Alloc *a) {
    Int v = 99;

    // doc: share
    Slice base = slice_make(a, TYPE_INT, 8, 8);
    Slice head = slice_sub(base, 0, 3); /* len 3, cap 8 */

    head = slice_append(a, head, &v, 1); /* writes base[3] */
    // doc: end
    print("base", base);
    print("head", head);
}

static void copy(Alloc *a) {
    Int from[3] = {1, 2, 3};
    Slice src = slice_from(from, 3, 3, TYPE_INT);
    Slice dst = slice_make(a, TYPE_INT, 2, 2);
    Slice bytes = slice_make(a, TYPE_BYTE, 3, 3);
    Str s = BURROW_S("hello");

    // doc: copy
    Int n = slice_copy(dst, src);     /* copy(dst, src) */
    Int m = slice_copy_str(bytes, s); /* copy(b, s) */
    // doc: end
    printf("copied %lld and %lld: %.*s\n", (long long)n, (long long)m, (int)bytes.len,
           (const char *)bytes.p);
    print("dst", dst);
}

static void bytes(Alloc *a) {
    Str s = BURROW_S("gopher");

    // doc: bytes
    Slice b = slice_from_str(a, s);  /* []byte(s) */
    Str back = str_from_slice(a, b); /* string(b) */
    // doc: end
    printf("%lld bytes, back to %.*s\n", (long long)b.len, (int)back.len,
           (const char *)back.p);
}

int main(void) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);

    make(a);
    from();
    at(a);
    sub();
    append(a);
    share(a);
    copy(a);
    bytes(a);
    arena_free(&ar);
    return 0;
}

/* Output:
make len 0 cap 16:
from len 4 cap 4: 10 20 30 40
at second
at len 3 cap 3: 42 0 0
m len 3 cap 6: 2 3 4
n len 3 cap 4: 2 3 4
xs len 3 cap 4: 1 2 42
ys len 4 cap 4: 0 1 2 42
base len 8 cap 8: 0 0 0 99 0 0 0 0
head len 4 cap 8: 0 0 0 99
copied 2 and 3: hel
dst len 2 cap 2: 1 2
6 bytes, back to gopher
*/

#include <math.h>
#include <stdio.h>

#include "burrow/burrow.h"
#include "burrow/mem/heap.h"

static void print_ints(Slice s) {
    for (Int i = 0; i < s.len; i++)
        printf(i == 0 ? "%lld" : " %lld", (long long)BURROW_AT(Int, s, i));
    printf("\n");
}

typedef struct Person {
    Str name;
    Int age;
} Person;

static const Type person_type = {BURROW_S_INIT("Person"),
                                 BURROW_S_INIT("main"),
                                 KIND_STRUCT,
                                 sizeof(Person),
                                 _Alignof(Person),
                                 0,
                                 0,
                                 NULL,
                                 NULL,
                                 NULL,
                                 NULL,
                                 0,
                                 0,
                                 NULL};

// doc: cmpfunc
/* Go's func(a, b Person) int, by age and then by name. */
static int by_age(void *env, const void *x, const void *y) {
    const Person *a = x, *b = y;
    return cmp_or(cmp_compare(a->age, b->age), cmp_compare(a->name, b->name));
}
// doc: end

static bool is_even(void *env, const void *v) {
    return *(const Int *)v % 2 == 0;
}

static bool print_chunk(void *env, const void *v) {
    print_ints(*(const Slice *)v);
    return true;
}

int main(void) {
    Alloc *a = heap_allocator();

    // doc: cmp
    printf("%d %d\n", cmp_compare(2, 10),
           cmp_compare(BURROW_S("b"), BURROW_S("a"))); /* -1 1 */
    printf("%d\n", cmp_less((double)NAN, -1.0));       /* 1 */
    Str name = cmp_or(BURROW_S(""), BURROW_S("guest"));
    printf("%.*s\n", (int)name.len, name.p); /* guest */
    // doc: end

    // doc: basics
    Int xs[] = {3, 1, 4, 1, 5, 9, 2, 6};
    Slice s = slice_from(xs, 8, 8, TYPE_INT);
    slices_sort(s);
    print_ints(s); /* 1 1 2 3 4 5 6 9 */

    Int five = 5;
    bool found;
    Int i = slices_binary_search(s, &five, &found);
    printf("%lld %d\n", (long long)i, found); /* 5 1 */
    printf("%lld %lld\n", (long long)*(const Int *)slices_min(s),
           (long long)*(const Int *)slices_max(s)); /* 1 9 */
    // doc: end

    // doc: edit
    s = slices_compact(s);
    print_ints(s); /* 1 2 3 4 5 6 9 */
    Int more[] = {7, 8};
    s = slices_insert(a, s, 6, more, 2);
    print_ints(s); /* 1 2 3 4 5 6 7 8 9 */
    s = slices_delete_func(s, BURROW_FN(SlicesPredFunc, is_even, NULL));
    print_ints(s); /* 1 3 5 7 9 */
    // doc: end
    mem_free(a, s.p, (size_t)s.cap * sizeof(Int), _Alignof(Int));

    // doc: func
    Person people[] = {
        {BURROW_S_INIT("Gopher"), 13},
        {BURROW_S_INIT("Alice"), 55},
        {BURROW_S_INIT("Bob"), 24},
        {BURROW_S_INIT("Alice"), 20},
    };
    Slice ps = slice_from(people, 4, 4, &person_type);
    slices_sort_func(ps, BURROW_FN(SlicesCmpFunc, by_age, NULL));
    for (Int k = 0; k < 4; k++)
        printf("%.*s %lld\n", (int)people[k].name.len, people[k].name.p,
               (long long)people[k].age);
    // doc: end

    // doc: chunk
    Int ys[] = {1, 2, 3, 4, 5};
    IterSeq chunks = slices_chunk(a, slice_from(ys, 5, 5, TYPE_INT), 2);
    chunks.f(chunks.env, BURROW_FN(IterYield, print_chunk, NULL));
    slices_seq_free(a, chunks);
    // doc: end
    return 0;
}

/* Output:
-1 1
1
guest
1 1 2 3 4 5 6 9
5 1
1 9
1 2 3 4 5 6 9
1 2 3 4 5 6 7 8 9
1 3 5 7 9
Gopher 13
Alice 20
Bob 24
Alice 55
1 2
3 4
5
*/

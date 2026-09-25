#include <stdio.h>

#include "burrow/burrow.h"
#include "burrow/sort.h"

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

/* The less for sort_slice. env is the slice's first element. */
static bool by_age(void *env, Int i, Int j) {
    Person *p = env;
    return p[i].age < p[j].age;
}

/* A SortInterface over an array of Int that sorts by the last digit. */
typedef struct Digits {
    Int *xs;
    Int n;
} Digits;

static Int digits_len(void *self) {
    return ((Digits *)self)->n;
}
static bool digits_less(void *self, Int i, Int j) {
    Digits *d = self;
    return d->xs[i] % 10 < d->xs[j] % 10;
}
static void digits_swap(void *self, Int i, Int j) {
    Digits *d = self;
    Int t = d->xs[i];
    d->xs[i] = d->xs[j];
    d->xs[j] = t;
}
static const SortInterfaceVT digits_vt = {NULL, digits_len, digits_less, digits_swap};

/* The cmp for sort_find, comparing a target against each name in turn. */
typedef struct Lookup {
    Str *names;
    Str target;
} Lookup;

static int cmp_name(void *env, Int i) {
    Lookup *l = env;
    return str_cmp(l->target, l->names[i]);
}

static void print_ints(const Int *xs, int n) {
    for (int i = 0; i < n; i++)
        printf(i ? " %lld" : "%lld", (long long)xs[i]);
    printf("\n");
}

int main(void) {
    // doc: builtin
    Int xs[] = {5, 2, 6, 3, 1, 4};
    Slice s = slice_from(xs, 6, 6, TYPE_INT);
    sort_ints(s);
    print_ints(xs, 6);                                   /* 1 2 3 4 5 6 */
    printf("%lld\n", (long long)sort_search_ints(s, 4)); /* 3 */
    // doc: end

    // doc: slice
    Person people[] = {
        {BURROW_S_INIT("Gopher"), 7},
        {BURROW_S_INIT("Alice"), 55},
        {BURROW_S_INIT("Vera"), 24},
        {BURROW_S_INIT("Bob"), 75},
    };
    Slice ps = slice_from(people, 4, 4, &person_type);
    sort_slice(ps, BURROW_FN(SortLessFunc, by_age, people));
    for (int i = 0; i < 4; i++)
        printf("%.*s %lld\n", (int)people[i].name.len, people[i].name.p,
               (long long)people[i].age);
    // doc: end

    // doc: interface
    Int ys[] = {31, 12, 41, 22, 51, 11};
    Digits d = {ys, 6};
    SortInterface data = {&digits_vt, &d};
    sort_stable(data);
    print_ints(ys, 6); /* 31 41 51 11 12 22 */

    SortReverse r = sort_reverse(data);
    sort_stable(sort_reverse_as_sort_interface(&r));
    print_ints(ys, 6); /* 12 22 31 41 51 11 */
    // doc: end

    // doc: find
    Str names[] = {BURROW_S_INIT("ann"), BURROW_S_INIT("bob"), BURROW_S_INIT("cat")};
    Lookup l = {names, BURROW_S("bob")};
    bool found;
    Int i = sort_find(3, BURROW_FN(SortFindFunc, cmp_name, &l), &found);
    printf("%lld %s\n", (long long)i, found ? "true" : "false"); /* 1 true */
    l.target = BURROW_S("bea");
    i = sort_find(3, BURROW_FN(SortFindFunc, cmp_name, &l), &found);
    printf("%lld %s\n", (long long)i, found ? "true" : "false"); /* 1 false */
    // doc: end
    return 0;
}

/* Output:
1 2 3 4 5 6
3
Gopher 7
Vera 24
Alice 55
Bob 75
31 41 51 11 12 22
12 22 31 41 51 11
1 true
1 false
*/

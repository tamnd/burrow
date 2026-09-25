# Sorting

`burrow/sort.h` is Go's `sort` package. The algorithms are Go's own, pdqsort for `sort_sort` and `sort_slice`, and insertion sort plus SymMerge for the stable sorts, and they make every comparison and every swap in the order Go makes them. So when two elements compare equal and the sort is not stable, they still come out in the same order they would in Go, and a program ported from Go prints the same thing.

## Slices of builtins

`sort_ints`, `sort_float64s` and `sort_strings` sort a `Slice` of `Int`, `double` or `Str` in increasing order. They compare the values directly instead of going through a less function, so they are the fastest way to sort. A NaN goes before every other float.

<!-- example: ../examples/sort/sort.c#builtin -->
```c
Int xs[] = {5, 2, 6, 3, 1, 4};
Slice s = slice_from(xs, 6, 6, TYPE_INT);
sort_ints(s);
print_ints(xs, 6);                                   /* 1 2 3 4 5 6 */
printf("%lld\n", (long long)sort_search_ints(s, 4)); /* 3 */
```

`sort_search_ints` and its two siblings return where a value would go in a sorted slice, which is the index of the first element that is not less than it. `sort_ints_are_sorted` and the rest check the order without changing anything.

## Any slice with a less function

`sort_slice` is Go's `sort.Slice`. It takes the slice and a `SortLessFunc` that says whether element `i` goes before element `j`, and it swaps whole elements itself, using the element size from the slice's type. Go's version takes `any` and panics at run time when it is not a slice, and this one takes a `Slice`, so that mistake does not compile.

<!-- example: ../examples/sort/sort.c#slice -->
```c
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
```

The less function gets the environment from `BURROW_FN` as its first argument, which is how it reaches the elements. `sort_slice_stable` keeps equal elements in the order they came in, and `sort_slice_is_sorted` checks.

## sort.Interface

Anything with a length, a less and a swap can be sorted, whether or not it is in one piece of memory. That is `SortInterface`, a vtable and a data pointer, the same shape as every other interface in burrow:

<!-- example: ../examples/sort/sort.c#interface -->
```c
Int ys[] = {31, 12, 41, 22, 51, 11};
Digits d = {ys, 6};
SortInterface data = {&digits_vt, &d};
sort_stable(data);
print_ints(ys, 6); /* 31 41 51 11 12 22 */

SortReverse r = sort_reverse(data);
sort_stable(sort_reverse_as_sort_interface(&r));
print_ints(ys, 6); /* 12 22 31 41 51 11 */
```

`sort_reverse` turns the less around. Go's `Reverse` allocates and returns an `Interface`, and here the `SortReverse` comes back by value and `sort_reverse_as_sort_interface` makes the interface from it, so nothing is allocated. It borrows `r`, so `r` has to outlive the sort.

`SortIntSlice`, `SortFloat64Slice` and `SortStringSlice` are Go's `IntSlice`, `Float64Slice` and `StringSlice`, with the same methods, and `sort_int_slice_as_sort_interface` and its siblings give the interface for one.

## Binary search

`sort_search(n, f)` is the smallest index in `[0, n)` where `f` is true, assuming `f` is false and then true, and `n` if it is never true. `sort_find(n, cmp, &found)` is the same idea with a three way compare: the first index where `cmp` is zero or negative, with `found` set when it is exactly zero. Go returns the two results together, and here the second comes back through a pointer that may be `NULL`.

<!-- example: ../examples/sort/sort.c#find -->
```c
Str names[] = {BURROW_S_INIT("ann"), BURROW_S_INIT("bob"), BURROW_S_INIT("cat")};
Lookup l = {names, BURROW_S("bob")};
bool found;
Int i = sort_find(3, BURROW_FN(SortFindFunc, cmp_name, &l), &found);
printf("%lld %s\n", (long long)i, found ? "true" : "false"); /* 1 true */
l.target = BURROW_S("bea");
i = sort_find(3, BURROW_FN(SortFindFunc, cmp_name, &l), &found);
printf("%lld %s\n", (long long)i, found ? "true" : "false"); /* 1 false */
```

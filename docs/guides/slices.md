# Slices

A slice is a pointer, a length, a capacity, and the element's type descriptor.

<!-- not compiled: the definition in burrow/slice.h, shown for reference -->
```c
typedef struct Slice {
    void *p;
    Int len;
    Int cap;
    const Type *elem;
} Slice;
```

Go's header is three words and this one is four. The fourth is what buys the whole thing. With the element descriptor in hand, one implementation of `append` works for every element type, and `reflect`, `fmt` and `encoding/json` can see into a slice that nobody told them about.

C has no templates, so the choice was between carrying a descriptor pointer and generating a slice type per element with a macro. The macro version cannot be passed to a function that does not already know the element type, which rules out about half the standard library. One word, pointing at a static object, initialised by the linker.

The fields are public. Go's `len` and `cap` are not function calls either, and half the loops in this library are `for (Int i = 0; i < s.len; i++)`. Read them freely. Write them only if you are sure, because nothing checks that `len` is still inside `cap` afterwards.

## Making one

<!-- example: ../examples/slices/slices.c#make -->
```c
Slice xs = slice_make(a, TYPE_INT, 0, 16);
```

That is `make([]int, 0, 16)`. The memory is zeroed, because Go's zero value rule is the language and not a convention, and code ported from Go assumes it in places where it never says so.

`len < 0`, `cap < 0` or `len > cap` panics with the message Go panics with. None of those is a condition a caller can handle and all of them mean the arithmetic that produced them was already wrong.

Over memory you already have, which does not copy and does not take ownership:

<!-- example: ../examples/slices/slices.c#from -->
```c
Int backing[4] = {10, 20, 30, 40};
Slice s = slice_from(backing, 4, 4, TYPE_INT);
```

That is the bridge from a C array, and it is how you hand burrow a stack buffer.

## Reading and writing

`slice_at` returns a pointer, because C cannot return a value whose type is only known at runtime. `BURROW_AT` turns it back into the value:

<!-- example: ../examples/slices/slices.c#at -->
```c
Str f = BURROW_AT(Str, parts, i);
BURROW_AT(Int, xs, 0) = 42; /* it is an lvalue, so this is xs[0] = 42 */
```

The `T` you pass is not checked against the element descriptor at compile time, because there is nothing at compile time to check it against. Its size is checked at runtime, and a `T` of the wrong size gets you the element the descriptor says is there, read as the type you asked for. It is the same class of mistake as a wrong `printf` format and it has the same flavour of consequence, so pass the type the slice actually holds.

Indexing is bounds checked against `len`, not against `cap`, and out of range panics. This is not optional and not behind a build flag. Go's tests depend on the failure, code written against Go relies on never reading past the end, and a version that trusted the caller would be a different language with the same spelling.

## Nil and empty

Go keeps these apart and the difference is visible from outside:

```go
var a []int          // nil,   marshals to null
b := []int{}         // empty, marshals to []
```

So burrow keeps them apart too. `slice_nil(TYPE_INT)` is the nil slice, `slice_make(a, TYPE_INT, 0, 0)` is the empty one, and `slice_is_nil` tells them apart. It looks like a distinction without a difference right up to the afternoon you are diffing JSON output against a Go service.

## Reslicing

<!-- example: ../examples/slices/slices.c#sub -->
```c
Slice m = slice_sub(s, 2, 5);     /* s[2:5]    */
Slice n = slice_sub3(s, 2, 5, 6); /* s[2:5:6]  */
```

Neither copies. The result points into the same backing array, which is why slicing is free and why writing through one is visible through the other.

Two things about the bounds catch people out, and both of them are Go's behaviour rather than ours.

The capacity of `s[lo:hi]` is `cap - lo` and not `hi - lo`. A three element slice cut out of a sixteen element array still has thirteen of capacity behind it.

The bounds are checked against `cap` and not against `len`. `s[:cap(s)]` is legal and is how you get at the spare capacity on purpose. Reslicing past `len` is a feature, and it is exactly why the three index form exists: `s[lo:hi:max]` clips the capacity so that appending has to allocate instead of writing into somebody else's elements.

## Append

<!-- example: ../examples/slices/slices.c#append -->
```c
xs = slice_append(a, xs, elems, n);
xs = BURROW_APPEND(Int, a, xs, 42); /* one value, no temporary */
ys = slice_append_slice(a, ys, xs); /* append(ys, xs...) */
```

Always assign the result back. `append` may or may not have moved the backing array, and the old header does not know which.

Here is the part worth reading twice, because it is the behaviour people rely on without being able to state it.

When the capacity is already there, the new elements are written into the existing backing array. Every other slice over that array sees them:

<!-- example: ../examples/slices/slices.c#share -->
```c
Slice base = slice_make(a, TYPE_INT, 8, 8);
Slice head = slice_sub(base, 0, 3); /* len 3, cap 8 */

head = slice_append(a, head, &v, 1); /* writes base[3] */
```

When the capacity is not there, a new array is allocated and the old one is left alone, so the same two slices now disagree about what the data is.

Go behaves exactly this way, Go's tests depend on it, and a version of burrow that quietly always copied would be a nicer library that runs ported Go code incorrectly. So it does what Go does.

`append(s, s...)` works, for the same reason it works in Go: the copy happens after the allocation, so the old array is still there while it is being read.

The spare capacity after a grow is zeroed, which Go also does and has to, because the next thing somebody writes is `s = s[:cap(s)]` and reads a zero value out of it.

## How the capacity grows

A faithful port of Go's `nextslicecap`. Double below 256 elements, then approach 1.25x through a formula that makes the transition smooth rather than a step.

```
len:  1  2  3  4  5  6  7  8  9 ...
cap:  1  2  4  4  8  8  8  8 16 ...
```

That is measured from go1.27.1 and it is what burrow produces.

There is one difference and it is deliberate. Go runs a second step called `roundupsize`, which rounds the byte count up to one of its allocator's size classes, and burrow does not. So the two agree wherever the rounding is a no-op and part company where it is not:

| elements before | Go 1.27.1 `cap` after | burrow `cap` after |
| --- | --- | --- |
| 4 | 8 | 8 |
| 256 | 512 | 512 |
| 512 | 848 | 832 |
| 832 | 1280 | 1232 |

`nextslicecap` gives 832 and 1232 in both cases. The extra 16 and 48 elements are Go rounding 6656 bytes up to the 6784 byte class and 9856 up to 10240.

The reason not to copy that step is that the size classes are a property of Go's allocator and burrow's allocator is whichever one you passed in. Rounding a request up to a class that your arena does not have buys nothing and wastes up to an eighth of the allocation. Go's spec says nothing about capacity growth, so nothing correct depends on this, but it is a real difference and it is written down here rather than discovered.

If you are counting allocations against a Go program, count the growth steps and not the capacities. Those match.

## What it costs

`BURROW_AT` and `BURROW_APPEND` are not thin wrappers around `slice_at` and `slice_append`. They expand to an inline fast path that is handed `sizeof(T)` at the call site, and that one fact changes the generated code twice over. The copy becomes a single store rather than a call into `memcpy` with a length nothing can see. And the whole thing inlines, so the four word header no longer goes out to the stack on the way into a call and back through a return buffer on the way out.

The size is checked against the element descriptor at runtime. A size that does not match, a nil pointer, a full slice or an index out of range all fall through to `slice_at` and `slice_append`, so the bounds checks, the failure messages and the growth arithmetic are still written exactly once. Passing a `T` of the wrong size is a performance mistake and not a correctness one.

Appending 1024 ints one at a time into a slice whose capacity is already there, appending 1024 into a nil slice and letting it grow, and 64 bounds checked reads. Median of five runs at `-time 0.2`, spreads under 3% unless noted.

M1 laptop, clang, against go1.27.1 darwin/arm64:

| | out of line | inline | Go |
| --- | --- | --- | --- |
| 1024 appends, capacity in hand | 6779 ns | 3700 ns | 738 ns |
| 1024 appends, growing | 7000 ns | 3990 ns | 1769 ns |
| 64 reads | 57.4 ns | 36.0 ns | 17.6 ns |

An AMD EPYC, gcc 13.3.0, pinned to one core. There is no Go on that machine, so this pair is burrow against burrow:

| | out of line | inline |
| --- | --- | --- |
| 1024 appends, capacity in hand | 19044 ns | 1360 ns |
| 1024 appends, growing | 19687 ns | 2020 ns |
| 64 reads | 211 ns | 82 ns |

Fourteen times on one machine and not quite twice on the other, for the same change, which is worth understanding rather than averaging. A four word struct is over the limit for being passed in registers, so the out of line version writes the header out as four eight byte stores and reads it back as two sixteen byte loads. On that AMD core a sixteen byte load overlapping two eight byte stores cannot be forwarded from the store buffer and has to wait for the cache, twice per append, on the dependency chain. Apple's core forwards it. So x86-64 was paying a stall that arm64 was not, and removing the call removed the stall.

What is left on arm64 is that clang still keeps the header in a stack slot even after inlining, because the slow path returns its result through memory and both paths have to agree on where the answer is. That is [issue 18](https://github.com/tamnd/burrow/issues/18) and it is not fixed here.

Two things follow for code using this. Use the macro for a single element of a type you know, always. Call `slice_append` directly for a bulk append, where the per element cost is divided by the count and the call is free: 1024 elements in blocks of 64 costs 450 ns here, which is a third of what the same elements cost one at a time, and a third of what Go charges for the same bulk append.

## Copying

<!-- example: ../examples/slices/slices.c#copy -->
```c
Int n = slice_copy(dst, src);     /* copy(dst, src) */
Int m = slice_copy_str(bytes, s); /* copy(b, s) */
```

Both return the number of elements copied, which is the smaller of the two lengths, and both handle overlap, because `copy(s, s[1:])` is how you delete an element and Go promises it works.

The element sizes have to match. Sizes rather than descriptor identity, because nothing stops you declaring your own `uint8` descriptor and a copy between that and `TYPE_BYTE` is meaningful. A copy between different sizes is not, and it is a bug in the caller rather than a condition to return zero for.

## Strings and byte slices

<!-- example: ../examples/slices/slices.c#bytes -->
```c
Slice b = slice_from_str(a, s);  /* []byte(s) */
Str back = str_from_slice(a, b); /* string(b) */
```

Both copy, which is what both conversions do in Go. If you want the cheap version, `s.p` and `s.len` are right there and you already know whether the lifetime works out.

## Zero sized elements

`[]struct{}` is a real thing in Go and it never allocates, because every zero sized object shares one address. burrow does the same, so a slice of a zero sized type has a non NULL pointer, costs no memory, and grows without ever calling the allocator.

## The slices package

`burrow/slices.h` is Go's `slices` package, all of it. Go writes each function once over a type parameter, and here each one takes a `Slice` and reads the element type from its descriptor, so the same `slices_sort` sorts an `Int` slice, a `double` slice and a `Str` slice. A value goes in and comes out by pointer, since C has no way to pass a value whose type is only known at run time.

<!-- example: ../examples/slicespkg/slicespkg.c#basics -->
```c
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
```

The functions Go limits to `cmp.Ordered`, which are `slices_sort`, `slices_is_sorted`, `slices_binary_search`, `slices_compare`, `slices_min` and `slices_max`, work when the element is an integer of any width, a float or a `Str`. Each of those has its own compiled copy of the algorithm, picked once per call from the element's kind and size, so sorting a million `Int` values does not go through a function pointer per comparison. Any other element type panics, which is as close as C gets to Go refusing to compile it. Floats follow Go, with a NaN before every other value.

`slices_equal`, `slices_index` and `slices_contains` are the ones Go limits to `comparable`. They use the type's own equality, so two `Str` values with the same bytes are equal wherever the bytes live, and a NaN is never equal to anything.

The editing functions work in place when they can, the same way Go's do, and the ones that might grow the slice take an allocator:

<!-- example: ../examples/slicespkg/slicespkg.c#edit -->
```c
s = slices_compact(s);
print_ints(s); /* 1 2 3 4 5 6 9 */
Int more[] = {7, 8};
s = slices_insert(a, s, 6, more, 2);
print_ints(s); /* 1 2 3 4 5 6 7 8 9 */
s = slices_delete_func(s, BURROW_FN(SlicesPredFunc, is_even, NULL));
print_ints(s); /* 1 3 5 7 9 */
```

As in Go, `slices_delete`, `slices_compact` and the rest zero the elements they leave behind past the new length, so a slice of pointers does not keep dead objects reachable. `slices_insert` and `slices_replace` accept values that point into the slice itself and get the answer Go gets.

Every function with a `_func` suffix takes a burrow function value whose arguments point at elements, and those work for any element type, structs included. A comparison returns a negative number, zero or a positive number, and `cmp_or` makes the usual "by this, then by that" order short:

<!-- example: ../examples/slicespkg/slicespkg.c#cmpfunc -->
```c
/* Go's func(a, b Person) int, by age and then by name. */
static int by_age(void *env, const void *x, const void *y) {
    const Person *a = x, *b = y;
    return cmp_or(cmp_compare(a->age, b->age), cmp_compare(a->name, b->name));
}
```

<!-- example: ../examples/slicespkg/slicespkg.c#func -->
```c
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
```

`slices_sort_stable_func` keeps equal elements in their original order, and `slices_min_func`, `slices_max_func` and `slices_binary_search_func` take the same kind of comparison.

The iterator functions return the `IterSeq` and `IterSeq2` values from `burrow/iter.h`. `slices_all`, `slices_backward` and `slices_values` take a pointer to the slice and allocate nothing, so the slice has to stay put until the sequence is done. `slices_chunk` also has to remember `n`, so it takes a small block from an allocator, and `slices_seq_free` gives it back:

<!-- example: ../examples/slicespkg/slicespkg.c#chunk -->
```c
Int ys[] = {1, 2, 3, 4, 5};
IterSeq chunks = slices_chunk(a, slice_from(ys, 5, 5, TYPE_INT), 2);
chunks.f(chunks.env, BURROW_FN(IterYield, print_chunk, NULL));
slices_seq_free(a, chunks);
```

`slices_collect`, `slices_sorted` and their siblings go the other way, from a sequence to a new slice.

## cmp

`burrow/cmp.h` is Go's `cmp` package. `cmp_compare`, `cmp_less` and `cmp_or` are macros that pick a function by the type of the first argument, so they read like Go's generic versions:

<!-- example: ../examples/slicespkg/slicespkg.c#cmp -->
```c
printf("%d %d\n", cmp_compare(2, 10),
       cmp_compare(BURROW_S("b"), BURROW_S("a"))); /* -1 1 */
printf("%d\n", cmp_less((double)NAN, -1.0));       /* 1 */
Str name = cmp_or(BURROW_S(""), BURROW_S("guest"));
printf("%.*s\n", (int)name.len, name.p); /* guest */
```

The second argument is converted to the first one's type, the way an untyped constant is in Go. Behind the macros are plain functions such as `cmp_compare_int64` and `cmp_less_str`, one per ordered type, to call directly when the type is known. `cmp_or` takes up to sixteen values and answers the first one that is not zero. `CmpOrdered` is Go's `Ordered` constraint written as a list macro, for code that wants to generate something once per ordered type.

## What is not here yet

A way to ask for the descriptor of `[]T`. A slice of slices works already, since a `Slice` is just a value with a size and an alignment, but you have to write the `[]T` descriptor out yourself. The function that builds one on demand belongs to `reflect`, which is where Go puts it too.

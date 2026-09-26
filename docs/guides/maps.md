# Maps

A map is a hash table that grows by itself, hands out its entries in a different order every time, and compares keys by value rather than by address.

<!-- example: ../examples/maps/maps.c#tour -->
```c
Map *counts = map_make(a, TYPE_STRING, TYPE_INT, 0);

BURROW_MAP_SET(Str, Int, counts, BURROW_S("the"), 1);

Int *n = BURROW_MAP_GET(Str, Int, counts, BURROW_S("the"));
if (n != NULL)
    (*n)++;
```

That is `map[string]int`, and the three properties in the first sentence are the three things Go's map does that most C hash tables do not.

`Map` is opaque and lives on the heap, which is what Go's map is as well. A Go map variable is a pointer to a header the runtime owns, which is why passing one to a function lets that function change it, and why a map is the one composite in Go that is not copied on assignment. `Map *` behaves the same way for the same reason.

## The allocator

`map_make` takes an allocator and the map remembers it. This is the one place the library bends the rule that every allocating function takes an allocator first.

An insert can grow the table, so `map_set` can allocate. The alternative is an allocator parameter on the hottest operation the container has, and a caller who passes a different one on the second call gets a table with half its memory from somewhere else. Storing it once keeps the property the rule exists for, which is that everything a map allocated came from one allocator that the caller chose.

The last argument is `make(map[K]V, hint)`: how many entries you expect. It sizes the table so that filling it to the hint does not rehash. Zero means no guess, and a map made that way allocates nothing at all beyond the header until the first insert, because `make(map[K]V)` in code that then puts nothing in it is common.

## Keys and values go by pointer

A map holds values of a type it only learns at runtime, so `map_get` and `map_set` deal in `void *`:

<!-- example: ../examples/maps/maps.c#raw -->
```c
Str word = BURROW_S("the");
Int one = 1;
map_set(counts, &word, &one);
```

The macros put the static typing back at the call site, which is where the types are known almost every time:

<!-- example: ../examples/maps/maps.c#macros -->
```c
BURROW_MAP_SET(Str, Int, counts, BURROW_S("the"), 1);
Int *n = BURROW_MAP_GET(Str, Int, counts, BURROW_S("the"));
bool have = BURROW_MAP_HAS(Str, counts, BURROW_S("the"));
BURROW_MAP_DEL(Str, counts, BURROW_S("the"));
```

Types first, then the container, then the values, which is the same shape as `BURROW_AT` and `BURROW_APPEND`. The key and the value are put into one element arrays inside the macro, which is what lets you pass a literal without naming a temporary.

Both the key and the value are copied into the table, so a key built on the stack is a fine key. For a `Str` key that means the two words of the header are copied and the bytes are not, so the bytes still have to outlive the map. A key built out of a buffer you are about to reuse needs a copy first, the same as it does in Go when you convert a `[]byte` to a `string`.

## Reading

<!-- example: ../examples/maps/maps.c#get -->
```c
Int *v = map_get(m, &key);         /* m[key], or NULL when it is not there */
bool ok = map_get2(m, &key, &out); /* v, ok := m[key] */
```

`map_get` returns a pointer into the table, so writing through it changes the entry. That is how `m[k]++` is spelled here and it is worth knowing that Go does not let you do it at all. The reason Go does not is exactly the hazard you now own: the pointer stops being valid at the next insert into that map, because growing the table moves every entry. Take the value out if you are going to keep it.

`map_get2` copies the value out, which is the safe version, and writes the zero value when the key is absent so that the C reads like the Go it came from. Pass `NULL` for the output to ask only whether the key is there.

A `NULL` map reads as empty rather than crashing, because a nil map in Go does. `map_len`, `map_get`, `map_get2`, `map_del`, `map_clear` and iteration all treat it as a map with nothing in it. Writing to one panics with Go's message, `assignment to entry in nil map`, since quietly accepting the write would lose data instead of reporting it.

## Writing

<!-- example: ../examples/maps/maps.c#set -->
```c
if (!map_set(m, &key, &val))
    return errors_new(a, BURROW_S("out of memory"));
```

`map_set` returns false when the table needed to grow and the allocator said no, and the map is unchanged in that case. Go's assignment cannot fail because Go stops the world instead, and a library in C has to hand the decision back. If your allocator cannot fail, ignore the result.

The value may be `NULL`, which stores the zero value. Go has no way to write that and does not need one, since `m[k] = V{}` says it, but here it saves naming a temporary and for a set it is the only thing you would ever pass:

<!-- example: ../examples/maps/maps.c#seen -->
```c
Map *seen = map_make(a, TYPE_STRING, TYPE_BOOL, 0);
map_set(seen, &word, NULL);
if (map_get(seen, &word) != NULL) {
    printf("seen " BURROW_STR_FMT " before\n", BURROW_STR_ARG(word));
}
```

That is `map[string]bool`, which is one byte per entry. Go's set is usually `map[string]struct{}`, which is none, and a map with a zero sized value type works here today and costs nothing per entry. There is just no descriptor to name for `struct{}` until structs land, so write the set with a `bool` for now and the cost is one byte you will get back later.

Setting a key that is already there replaces the value and keeps the stored key. That is Go's behaviour and it is observable for a key type whose equality ignores something its bits hold, which is exactly what a float key is: store `-0.0` first and the key stays negative no matter how many times you write to it through `0.0`.

## Deleting, and clearing

<!-- example: ../examples/maps/maps.c#del -->
```c
map_del(m, &key); /* delete(m, key) */
map_clear(m);     /* clear(m) */
```

`map_clear` empties the map and keeps the memory, the same as Go's builtin, so a map you are about to refill should be cleared rather than remade.

`map_free` hands the memory back to the allocator the map came from and leaves the pointer dangling, so it is the last thing you do with one. It is the first per object free function in the library and there is a reason it exists here when `Str` and `Slice` have none: those two hand you the pointer and the size, so `mem_free` takes them directly. A map is opaque and reallocates behind your back, so nothing outside `map.c` can name the pointer or the size to give back. Arena users can keep ignoring it.

## Iteration

<!-- example: ../examples/maps/maps.c#iter -->
```c
const void *k;
void *v;
for (MapIter it = map_iter(m); map_next(&it, &k, &v);) {
    printf(BURROW_STR_FMT " = %lld\n", BURROW_STR_ARG(*(const Str *)k),
           (long long)*(Int *)v);
}
```

That is `for k, v := range m`. Either pointer may be `NULL` if you only want the other one. `MapIter` is a value you keep on the stack, and its fields are visible only because C has no other way to let you declare one.

The order is randomised per iterator and it is randomised on purpose. Go does this and it is a feature: a program that depends on map order is already broken, and the kind thing is to break it on the first run on the author's own machine rather than on the day somebody adds a key. If you want an order, take the keys out and sort them.

Deleting during the walk is allowed and is the common case. An entry you delete before the walk reaches it will not be produced. Inserting during the walk is allowed too, and a new entry may or may not turn up, which is exactly what Go's specification says.

With one exception, and it is the one real deviation in this type. If an insert makes the table grow while an iterator is live, every entry moves, and `map_next` stops the program with `map grew during iteration` instead of producing an arbitrary answer. Go keeps the old table alive for the iterator and can afford to because it has a collector. Doing that here means either handing out pointers into freed memory or never freeing it, and the pattern is already unspecified in Go, so this is the honest version of the same program. It is entry fifteen in [the ledger](../ledger.md).

## Which keys are allowed

Any comparable type, which is Go's rule. A slice, a map and a function are not comparable, and neither is a struct containing one. Go catches that at compile time and says `invalid map key type`. A type descriptor only exists at runtime, so `map_make` catches it there and stops the program, which is the earliest the same mistake can be caught and beats hashing the bytes of a slice header.

Two corners of IEEE 754 come along with float keys, and both of them are Go's behaviour rather than something this library chose.

A negative zero and a positive zero are equal, so they are one key:

<!-- example: ../examples/maps/maps.c#zero -->
```c
BURROW_MAP_SET(double, Int, m, -0.0, 7);
BURROW_MAP_HAS(double, m, 0.0); /* true, and len is 1 */
```

A NaN is not equal to itself, so a NaN key goes in and can never be found again, can never be deleted, and two of them are two entries:

<!-- example: ../examples/maps/maps.c#nan -->
```c
map_set(m, &nan, &one);
map_set(m, &nan, &one);
map_len(m);       /* 2 */
map_get(m, &nan); /* NULL */
```

Go behaves exactly this way and it catches everybody out once. Do not use a NaN as a key. `map_clear` is the only way to get rid of one.

Complex keys follow both rules componentwise, since Go defines complex equality as the real parts being equal and the imaginary parts being equal.

`Str` keys compare by their bytes rather than by their pointer, which is what makes a map keyed by string useful, and it is the reason `TypeOps` exists at all. Two `Str` values built from different buffers holding the same bytes are one key.

## What is inside

A Swiss table, which is the structure Go moved to in 1.24 and which Go took from Abseil.

A table is an array of groups. A group is eight control bytes followed by eight slots, and a slot is a key followed by a value. A control byte is `0x80` when its slot has never been used, `0xFE` when its slot held an entry that was deleted, and otherwise the low seven bits of the key's hash.

So the eight control bytes of a group are one 64 bit word, and the question "which of these eight slots might hold my key" is a few instructions of ordinary integer arithmetic on that word, with no branches and no key comparisons. A lookup that misses usually reads one cache line and compares nothing at all. The rest of the hash picks the group to start at, and a probe that finds neither a match nor an unused slot moves on by a triangular step, 1 then 3 then 6 then 10, which is what guarantees every group gets visited once when the group count is a power of two.

The table fills to seven eighths before it grows.

Deleting is where open addressing charges you. If some other slot in the group has never been used, the deleted slot can go back to never used, because a probe passing through will stop there anyway. If the whole group is in use, the slot has to stay in the way as a tombstone, or a probe that ran through this group on the way to somewhere else would stop early and miss the key it was looking for.

Tombstones are the reason `map_set` does not simply double whenever it runs out of room. If the live entries alone would fit in the current size, it rebuilds at the same size and gets the space back, and only a map that is genuinely full doubles. Without that, filling and emptying a map over and over, which is what a cache does, grows it without bound while it holds nothing.

### Where this differs from Go

Go's table is a directory of fixed size tables and it grows by splitting one of them, so no single insert ever has to touch the whole map. This one is a single table that doubles and reinserts everything.

Amortised that is the same work, and it is what Abseil does. What you get instead is a pause on the insert that grows, proportional to the size of the map. What you also get is the iterator rule above, since an entry that moved is an iterator that cannot be kept.

The other difference is the hash. Go's is AES accelerated where the chip has the instruction, and burrow's is a multiply and fold hash in the style of wyhash that costs two multiplies for any key up to sixteen bytes. Nothing observable depends on which hash it is.

Each map gets its own seed from the runtime's random generator, so two maps holding the same keys have different layouts, and so a program cannot be fed keys that all land in one group. That generator is the reason `runtime_rand64` exists and the reason the map is the first type in the library that needs a source of entropy at startup.

## The maps package

Go's `maps` package is here as `maps_*`, in `burrow/maps.h`. Go's functions are generic over the key and value types and these read both from the `Map`, so one function serves every map. A NULL map reads as empty in all of them, the way a nil map does.

<!-- example: ../examples/mapspkg/mapspkg.c#clone -->
```c
Map *m = map_make(a, TYPE_STRING, TYPE_INT, 0);
BURROW_MAP_SET(Str, Int, m, BURROW_S("one"), 1);
BURROW_MAP_SET(Str, Int, m, BURROW_S("two"), 2);
BURROW_MAP_SET(Str, Int, m, BURROW_S("three"), 3);

Map *c = maps_clone(a, m);
printf("%d\n", maps_equal(m, c)); /* 1 */
BURROW_MAP_SET(Str, Int, c, BURROW_S("four"), 4);
printf("%d\n", maps_equal(m, c)); /* 0 */
```

`maps_clone` copies the table as it stands rather than inserting each entry again, which is what Go's runtime does too. It is also on the core type as `map_clone`. `maps_equal` compares values with `type_equal`, so a map holding a NaN is not equal to itself, as in Go. `maps_equal_func` takes the comparison as a function, and the two value types may differ.

The functions that take a function take a closure, the same `BURROW_FUNC` shape the rest of the library uses. The key and the value come in by pointer:

<!-- example: ../examples/mapspkg/mapspkg.c#pred -->
```c
/* Go's func(k string, v int) bool, true for the odd values. */
static bool is_odd(void *env, const void *k, const void *v) {
    return *(const Int *)v % 2 != 0;
}
```

<!-- example: ../examples/mapspkg/mapspkg.c#delete -->
```c
maps_delete_func(c, BURROW_FN(MapsPredFunc, is_odd, NULL));
print_map(a, c); /* four:4 two:2 */
```

`maps_copy` sets every entry of one map in another, and `maps_insert` does the same from a sequence. Both return false when the map needed to grow and the allocator said no, which is the answer `map_set` gives, where Go's versions cannot fail.

<!-- example: ../examples/mapspkg/mapspkg.c#copy -->
```c
maps_copy(c, m);
print_map(a, c); /* four:4 one:1 three:3 two:2 */
```

`maps_all`, `maps_keys` and `maps_values` are the three sequences, in the map's random order, and `maps_collect` builds a new map from a sequence of pairs. The sequences hold only the map and allocate nothing. Sorting the keys is `slices_sorted` over `maps_keys`, which is how the examples here print a map in a stable order.

<!-- example: ../examples/mapspkg/mapspkg.c#iter -->
```c
Map *d = maps_collect(a, TYPE_STRING, TYPE_INT, maps_all(m));
print_map(a, d); /* one:1 three:3 two:2 */
```

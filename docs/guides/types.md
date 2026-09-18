# Type descriptors

Go's standard library leans on the type system far more than it looks like it does from the outside.

`fmt.Println` prints whatever you hand it because it can ask the value what it is. `encoding/json` marshals a struct nobody wrote any code for. `sort.Slice` sorts a slice of anything. `database/sql` scans a row into whichever fields you passed. None of that is possible in C unless the types describe themselves, so in burrow they do.

A type descriptor is a `const Type` sitting in read only memory, one per type, shared by everything that mentions that type. Pointing at one costs a word. Initialising one costs nothing, because it was initialised by the linker. A program that uses a dozen types pays for a dozen descriptors and a program that uses none pays only for the handful of builtins something else in the library dragged in.

```c
const Type *t = TYPE_INT;
printf("%u bytes, aligned to %u\n", t->size, t->align);
```

## The builtins

Every type the Go language itself has comes with a descriptor already written:

```c
TYPE_BOOL
TYPE_INT     TYPE_INT8   TYPE_INT16   TYPE_INT32   TYPE_INT64
TYPE_UINT    TYPE_UINT8  TYPE_UINT16  TYPE_UINT32  TYPE_UINT64  TYPE_UINTPTR
TYPE_FLOAT32 TYPE_FLOAT64
TYPE_COMPLEX64 TYPE_COMPLEX128
TYPE_STRING
TYPE_UNSAFE_POINTER
```

`TYPE_BYTE` and `TYPE_RUNE` exist too, and they are macros for `TYPE_UINT8` and `TYPE_INT32` rather than descriptors of their own. That is what Go does. `reflect.TypeOf(byte(0)).Kind()` is `reflect.Uint8` and its `String()` is `"uint8"`, so a byte in burrow prints as a uint8 as well. It looks wrong for about a second and then you remember that `byte` is a spelling of `uint8` in Go and not a separate type.

`TYPE_INT->size` is eight on a sixty four bit machine and four on a thirty two bit one, because Go's `int` follows the pointer width and so does burrow's `Int`.

## Kinds

A kind is the broad category a type falls into, and there are twenty six of them, numbered exactly the way `reflect.Kind` numbers them.

The numbering matters more than it looks. Kind values get printed by `fmt`, compared in test code, and switched on in code ported from Go. A descriptor set that numbered them differently would produce a library that behaved like Go right up until somebody printed one.

```c
if (t->kind == KIND_SLICE) { ... }

Str k = kind_name(t->kind);   /* "slice" */
```

`kind_name` on a value outside the range gives you `"invalid"` rather than reading off the end of a table, because a kind can arrive from a cast and the useful answer to a bad one is a word, not a crash.

Three questions get asked about kinds constantly, so they are spelled out once here rather than written by hand in twenty places and got subtly wrong in one of them:

```c
bool kind_is_signed(Kind k);     /* KIND_INT through KIND_INT64 */
bool kind_is_unsigned(Kind k);   /* KIND_UINT through KIND_UINTPTR */
bool kind_is_float(Kind k);      /* KIND_FLOAT32 and KIND_FLOAT64 */
```

Signed and unsigned exclude the float kinds, which is the grouping `reflect` uses.

## Names

```c
Str n = type_name(t);
```

`"int"`, or `"Point"` for a named struct. It borrows from the descriptor, which is static, so the result outlives anything you might do with it and there is nothing to free.

A type also carries a package path, which is empty for the builtins and is what makes `image.Point` and `geom.Point` different types rather than the same one:

```c
t->pkg_path   /* BURROW_S("image") */
```

Unnamed composite types do not store their spelling. A `[]int` would need a string built at instantiation and there is no good place to put it, so for now `type_name` on one falls back to the kind name and you get `"slice"` rather than `"[]int"`. That changes when the composite type constructors land and have somewhere to build the real name.

## Operations

Four things you want to do to a value whose type you only know at runtime:

```c
bool     type_equal(const Type *t, const void *a, const void *b);
uint64_t type_hash(const Type *t, const void *p, uint64_t seed);
void     type_copy(const Type *t, void *dst, const void *src);
void     type_zero(const Type *t, void *p);
```

Use these rather than `memcmp` and `memcpy`. That is the whole reason they exist.

For most types the byte level operation is the right one, and that is exactly what these do: `memcmp` over `size` bytes, a hash of those bytes, `memcpy`, `memset` to zero. For some types it is wrong, and `Str` is the first example you hit. Two `Str` values with different pointers and the same bytes are one string in Go. `memcmp` on the struct says they differ. A map keyed by string built on `memcmp` would lose every lookup that did not happen to use the identical pointer.

So a descriptor can carry an ops table:

```c
typedef struct TypeOps {
    bool (*equal)(const void *a, const void *b);
    uint64_t (*hash)(const void *p, uint64_t seed);
    void (*copy)(void *dst, const void *src);
    void (*zero)(void *p);
} TypeOps;
```

Every entry is optional and `NULL` means take the default. `TYPE_STRING` fills in `equal` and `hash` and leaves `copy` and `zero` alone, because copying a `Str` really is copying sixteen bytes and zeroing one really is writing sixteen zero bytes.

The hash is currently FNV-1a, sixty four bit, seeded. It is not the hash Go uses. Go's runtime hash is AES accelerated where the chip has the instruction and something else where it does not, and its output is deliberately unstable between runs so that nobody can build a program that depends on map ordering. Nothing observable in burrow depends on which hash this is, only on it being a hash, so it stays the simple one until the map lands and has an opinion about throughput. The seed is folded in at the start rather than mixed in at the end, so two values differing only in a late byte still land in different buckets.

## Comparability

```c
bool type_is_comparable(const Type *t);
```

Slices, maps and functions cannot be compared with `==` in Go. A struct or an array cannot be compared when any part of it cannot.

Go answers this at compile time and rejects your program. burrow has to answer it at runtime, because a map with an uncomparable key type is a program that has to fail somewhere, and the moment it is handed the key type is the earliest place it can find out.

## Fields and methods

```c
const Field  *f = type_field_by_name(t, BURROW_S("X"));
const Method *m = type_method_by_name(t, BURROW_S("String"));
```

Field lookup is a linear scan. Structs have a handful of fields and a linear walk over a contiguous array beats anything cleverer at that size, by a margin that is not close.

Method lookup is a binary search, because methods in a descriptor are sorted by name the way Go sorts them, and a type with a large method set is a real thing that happens.

A field carries its name, its struct tag exactly as written and unparsed, its type, its byte offset, and whether it is exported and whether it is embedded:

```c
typedef struct Field {
    Str name;
    Str tag;
    const Type *type;
    uint32_t offset;
    bool exported;
    bool anonymous;
} Field;
```

The tag is unparsed on purpose. `encoding/json`, `encoding/xml` and the rest each parse it their own way and Go does not centralise that either.

A method carries a thunk:

```c
void (*thunk)(void *recv, void **args, void **rets);
```

One shape of function pointer, whatever the method's real signature is, because a dynamic call needs a single calling convention to go through. Receiver, an array of pointers to the arguments, an array of pointers to where the results go. That is enough for `net/rpc` and for `text/template`, and it is what sits underneath `reflect.Value.Call`.

## The hash field

Every descriptor has a `hash`, a number that differs between different types and is stable within one build. It turns a type assertion into an integer comparison rather than a string compare, and it seeds a map's hashing.

It is not stable across builds and nothing may write it to disk or send it over a wire. Go's is not stable either, for the same reason: it comes out of the layout of one particular binary.

## Writing your own

You will not write a descriptor by hand. That is what the `STRUCT` macro is for, and it is not here yet.

What is here is the shape of a descriptor and the descriptors for the types the language already has, which is what was needed first, because `Slice` carries a pointer to the descriptor of its element type and could not be written until there was something for it to point at.

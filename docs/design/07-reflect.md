# 07 — Reflection: type descriptors without a compiler

`fmt.Printf("%v", x)`. `json.Marshal(x)`. `json.Unmarshal(b, &x)`.
`template.Execute(w, data)`. `rows.Scan(&a, &b)`. `gob.Encode(x)`.
`xml.Unmarshal`. `slog.Any`. `testing/quick`. Nine of the most-used entry
points in the standard library, and every one of them needs to know, at
runtime, the shape of a type the library has never seen.

Go's compiler emits that information for every type. C emits none. This is the
hardest wall in the project, and this document climbs it.

## 1. What is actually required

Reading the stdlib's reflection consumers back to front, the requirement set is
narrower than "full reflection":

| Capability | Needed by | Hard? |
| --- | --- | --- |
| Type kind, size, alignment | everything | no |
| Struct field list: name, type, offset, tag | json, xml, gob, sql, template | no |
| Exported vs unexported field | json, xml, gob | no |
| Element type of slice/array/map/pointer/chan | everything | no |
| Read a field's value given a base pointer | encoders, `fmt`, template | no |
| Write a field's value | decoders, `Scan` | no |
| Construct a zero value of a type | decoders | no |
| Grow a slice, insert into a map, generically | decoders | no |
| Method set enumeration + dynamic call | `net/rpc`, template method calls | **yes** |
| Interface satisfaction check at runtime | `fmt` (`Stringer`), json (`Marshaler`) | moderate |
| `reflect.MakeFunc`, `reflect.New` of arbitrary type | `testing/quick`, rpc | **yes** |
| Struct tag parsing | json, xml, sql, template | no |

Only three rows are hard, and two of them (`MakeFunc`, dynamic call) are needed
by exactly two packages. Everything else is a data-description problem, and
data can be described.

## 2. The descriptor

```c
typedef enum {
    KIND_INVALID, KIND_BOOL,
    KIND_INT, KIND_INT8, KIND_INT16, KIND_INT32, KIND_INT64,
    KIND_UINT, KIND_UINT8, /* … */ KIND_UINTPTR,
    KIND_FLOAT32, KIND_FLOAT64, KIND_COMPLEX64, KIND_COMPLEX128,
    KIND_ARRAY, KIND_CHAN, KIND_FUNC, KIND_INTERFACE,
    KIND_MAP, KIND_POINTER, KIND_SLICE, KIND_STRING,
    KIND_STRUCT, KIND_UNSAFE_POINTER,
} Kind;

typedef struct {
    Str          name;        /* "X" */
    Str          tag;         /* `json:"x,omitempty"` */
    const Type  *type;
    uint32_t        offset;
} Field;

typedef struct {
    Str          name;        /* "String" */
    const Type  *ftype;       /* func descriptor */
    void          (*thunk)(void *recv, void **args, void **rets);
} Method;

struct Type {
    Str            name;        /* "Point" */
    Str            pkg_path;    /* "image" */
    Kind           kind;
    uint32_t          size;
    uint16_t          align;
    uint16_t          nfield, nmethod;
    const Field   *fields;      /* struct */
    const Method  *methods;
    const Type    *elem;        /* slice/array/ptr/chan/map-value */
    const Type    *key;         /* map */
    uint32_t          len;         /* array */
    uint32_t          hash;        /* identity, for maps and type assertions */
    const TypeOps *ops;        /* equal, hash, copy, zero — optional */
};
```

Whether a field is exported and whether it is embedded are not stored. Both are
answers to questions about the name, both are `field_is_exported` and
`field_is_embedded` instead, and a stored copy of something derivable is a
stored copy that can be wrong. Go's `reflect` treats them as questions too.

One static `const` struct per type, in rodata, shared. A program using twelve
types pays for twelve descriptors. `reflect` is then ordinary library code over
these — all 265 declarations of it — and so are `fmt`, `encoding/json` and the
rest.

## 3. Declaring a type: the X-macro DSL

The descriptor has to come from somewhere, and the choice of *where* determines
whether `burrow` keeps its "`cc burrow.c`, no build step" promise. So the
primary mechanism requires no external tool at all:

```c
#define POINT_FIELDS(F, T)                    \
    F(T, Int, X, "json:\"x\"")                \
    F(T, Int, Y, "json:\"y\"")                \
    F(T, Str, Label, "json:\"label,omitempty\"")

BURROW_STRUCT(Point, POINT_FIELDS);
```

`BURROW_STRUCT` expands twice over the field list: once to emit the struct
definition, once to emit the descriptor with `offsetof` for each field. The
declaration and the metadata cannot drift because there is one source for both
— which is the property that makes this better than annotations plus a
generator, not merely cheaper.

The list takes two parameters rather than one. `F` is the thing being done to
each line and `T` is the struct being declared, because a descriptor entry needs
`offsetof(T, field)` and C gives a macro no way to bind `T` for the lines below
it. Passing it in means the type's name is written once, at the `BURROW_STRUCT`
call, rather than on every line where it could be got wrong during a rename.
The semicolon is the caller's, since a macro that ate one would leave a stray
semicolon at file scope, which `-Wpedantic -Werror` rejects.

Resulting usage:

```c
Point p = { .X = 3, .Y = 4, .Label = BURROW_S("origin") };

fmt_printf(BURROW_S("%v\n"), BURROW_ANY(Point, &p));
/* {3 4 origin} */

Slice j = json_marshal(a, BURROW_ANY(Point, &p), &err);
/* {"x":3,"y":4,"label":"origin"} */

Point q = {0};
err = json_unmarshal(a, j, BURROW_ANY(Point, &q));
```

That is the target ergonomic, and it is within a line or two of Go.

Companion macros cover the rest of the type space:

```c
BURROW_STRUCT(T, FIELDS)            /* struct + descriptor */
BURROW_SLICE_TYPE(Name, T)          /* descriptor for []T */
BURROW_ARRAY_TYPE(Name, T, N)       /* descriptor for [N]T */
BURROW_PTR_TYPE(Name, T)
BURROW_MAP_TYPE(Name, K, V)
BURROW_METHOD(T, Name, fn)          /* registers a method + thunk */
BURROW_IMPLEMENTS(T, FmtStringer, adapter)
```

The four composite macros take the new type's name first, because C gives no
way to derive one and `TYPE_OF` needs a name to paste. Go does not have this
problem: `[]int` is both a type and its own spelling. The convention that reads
best is the Go name with the punctuation written out, so `IntSlice`, `PointPtr`,
`StrIntMap`, but nothing enforces it.

Each macro has a `_DECL` form. The plain macro defines objects, so it belongs in
exactly one translation unit; the `_DECL` form declares only, for a type whose
struct has to live in a header. `BURROW_STRUCT` is `BURROW_STRUCT_DECL` followed
by `BURROW_STRUCT_DEFINE`, both driven by the same list.

**`BURROW_ENUM` and `BURROW_ALIAS` are not implemented and are not scheduled.**
Both need the underlying type's `Kind` in the descriptor, and the only way to
reach it from a type's name is `TYPE_OF(U)->kind`, which is a load rather than a
constant expression and therefore cannot appear in the static initialiser these
descriptors are. The two ways out are both worse than the gap: spell the kind at
the call site, which adds a second place to state something the compiler already
knows and can disagree with the type; or fill it in during a pass at startup,
which is exactly the registration this approach exists to avoid. A named integer
type still works today by declaring it as its underlying type. It loses the
value names in `%v`, which is what `BURROW_ENUM` was for, and that is the whole
of what is missing.

Every one of `burrow`'s own ~1,900 public struct types is declared this way, so
the DSL is exercised across the entire library before any user sees it. If it
is awkward, we find out at scale and early.

**Ugliness, acknowledged.** The `F(...)` list form is not idiomatic C and it is
the least attractive thing in the whole design. It buys: zero build steps, zero
external tools, amalgamation compatibility, no possibility of drift, and
descriptors in rodata with no runtime registration cost. That trade is worth
making, and §4 gives the alternative to anyone who disagrees.

## 4. The generator, for people who want plain structs

```c
/* burrow:reflect */
typedef struct {
    Int X    BURROW_TAG("json:\"x\"");
    Int Y    BURROW_TAG("json:\"y\"");
    Str Label BURROW_TAG("json:\"label,omitempty\"");
} Point;
```

`burrow-gen reflect src/*.h -o src/reflect_gen.c` parses these with libclang
and emits the descriptors. Natural declarations, one build step, one heavy
build-time dependency (libclang) that is *not* required to build `burrow`
itself or to use the DSL.

Both paths produce byte-identical descriptors, and the same test suite runs
against both. Neither is deprecated; the DSL is the default because of the
no-build-step promise, and the generator exists because a large existing
codebase will not rewrite its structs.

A third path for the truly reluctant: **`burrow-gen reflect --from-go`**
ingests a Go type declaration and emits both the C struct and its descriptor.
For someone porting a Go program, this is the natural direction of travel, and
it is the same machinery the conformance harness uses to translate Go's tests.
→ [14](14-conformance.md) §3

## 5. Registration and type identity

Descriptors are static, so there is nothing to register for reflection to work
on a value you hold. A registry exists for two narrower purposes:

- **Name → type lookup**, needed by `encoding/gob` (which encodes type names on
  the wire), `net/rpc`, and `template`'s `.Method` calls.
- **Type identity across translation units**, so that `type_of(Point)` in
  two `.c` files is the same pointer. Handled by making descriptors
  `extern const` with a canonical definition emitted once, plus a `hash` field
  for cases where pointer identity cannot be relied on (shared libraries).

```c
BURROW_REGISTER_TYPE(Point);                    /* file scope; gob/rpc need this */
const Type *t = type_by_name(BURROW_S("main.Point"));
```

`BURROW_REGISTER_TYPE` puts one pointer in a section of its own and costs
nothing else. The linker gathers every such pointer into one run and hands the
bounds over as two symbols, so the first lookup can read the whole list at once
and build the table. Every object format burrow targets can do this, though no
two of them spell it the same way:

| Format | Section | Bounds |
| --- | --- | --- |
| ELF | `burrowtype` | `__start_burrowtype` and `__stop_burrowtype`, from the linker |
| Mach-O | `__DATA,__burrowtype` | `section$start$` and `section$end$` asm labels |
| PE | `.brwt$b` | objects of our own in `.brwt$a` and `.brwt$z`, which sort either side |

A compiler with no sections but with `__attribute__((constructor))` gets a
constructor per type instead, which calls `type_register` before `main`. A
compiler with neither gets a `_Static_assert` telling the caller to register by
hand, because failing at compile time is better than a lookup that silently
returns nothing.

The table is built on first use rather than during static init, because a
constructor cannot take a lock that another constructor might be initialising.
It is a power of two open addressed table keyed on the package path, a dot and
the type name hashed together. The key is never materialised anywhere: a
descriptor keeps its package path and its name apart, and a query is split at
the last dot rather than the first, since an import path has dots in it and a
type name cannot.

Registration is not write-once, because a shared library can arrive after
`main` has started. So the table is behind a read-write lock rather than being
lock-free. Lookups take the read lock, which is the case that matters, and
`type_register` takes the write lock. Registering the same descriptor twice is
fine, and registering two different descriptors under one name fails rather
than picking a winner.

`type_same` answers identity. It is pointer equality first, which is the answer
within one link. When that fails it compares kind, size, name and package path,
which is the shared library case where the same type was compiled twice. Two
unnamed types are never the same, on purpose: an anonymous struct has no
identity to compare, and saying so is more useful than guessing from a layout
that two unrelated types could share.

One thing worth knowing if you touch the section walk. clang's address
sanitizer puts a redzone around every global, including globals in a section of
your own, and that turns the packed run of pointers into one with holes in it.
The entries are declared `no_sanitize_address` to stop it. gcc needs nothing,
because it already leaves a global with its own section alone, and it rejects
the attribute anywhere but on a function, which is why this is asked for by
compiler rather than through `__has_attribute`. The attribute does nothing in a
build without the sanitizer, and it is not decoration, so please do not tidy it
away. → [03](03-c-dialect.md) §5

## 6. Methods and dynamic calls

A method is two things at once. It is data, so that something can ask a type
what it can do, and it is a call made with a signature the caller does not know
until it runs. net/rpc is handed a method name off a socket, `text/template` is
handed one out of a template, and neither of them can write the call, so the
call has to already be there.

What is already there is a **thunk**: one function of a fixed shape, emitted
next to the method, that takes the receiver, an array of pointers to the
arguments and an array of pointers to where the results go.

```c
static Int point_sum(Point *p);
static void point_move(Point *p, Int dx, Int dy);

#define POINT_SIG_Move(IN, OUT)  IN(0, Int) IN(1, Int)
#define POINT_SIG_Sum(IN, OUT)   OUT(Int)

#define POINT_METHODS(M, T)                 \
    M(T, Move, point_move, POINT_SIG_Move)  \
    M(T, Sum, point_sum, POINT_SIG_Sum)

BURROW_STRUCT_DECL(Point, POINT_FIELDS);
/* the two functions, written by hand */
BURROW_STRUCT_DEFINE_METHODS(Point, POINT_FIELDS, POINT_METHODS);
```

and then, with nothing in hand but a name:

```c
const Method *m = type_method_by_name(TYPE_OF(Point), BURROW_S("Sum"));
Int out;
void *rets[] = {&out};
method_call(m, &p, NULL, rets);
```

This covers every call `reflect.Value.Call` has to make on a registered method,
which is every call net/rpc and `text/template` make. No libffi, no runtime code
generation, and nothing for the caller to write.

### Three things it asks of you, and why

**The parameter's position is written out.** `IN` takes it alongside the type.
The preprocessor cannot count: turning a list into 0, 1, 2 needs either a fixed
table of arities or a counter incremented inside an expression whose evaluation
order is undefined. Writing the number is the honest version, it is one per
parameter in a list you are already writing, and it lands in the descriptor
where a test can hold you to it.

**There is at most one result.** A C function returns one value, and a burrow
function returning several returns a struct of them, the same as everywhere else
in the library. So a method returning a value and an error is one `OUT` naming
the struct of the two, and `NumOut` counts what C counts rather than what Go
would.

**The receiver is a pointer.** A dynamic call arrives holding a pointer to the
value either way, and a method wanting a copy can take one on its first line.
What this gives up is Go's distinction between the method set of `T` and of
`*T`, which is a statement about what satisfies an interface, and in C an
interface is satisfied by a vtable filled in by hand. Nothing in the library can
tell the difference.

### Where a signature lives

A signature is a `Type` of `KIND_FUNC`. It uses `fields` for both halves,
parameters first and then results, with `nfield` the total and `len` the number
of parameters, read through `type_num_in`, `type_in`, `type_num_out` and
`type_out`. One array rather than two because a descriptor has one pointer for a
list of fields, and adding a second would grow every descriptor of every kind to
pay for a kind most programs never reflect on.

### Method order

`type_method_by_name` is a linear scan. It was a binary search, on the strength
of the array being sorted by name the way Go sorts it, and the array is sorted
by name only if whoever wrote the list happened to write it in order. The
preprocessor cannot sort any more than it can count, so nothing enforces it, and
a binary search over a nearly sorted array finds most of what it looks for and
silently misses the rest, which is the worst way for a lookup to be wrong. At a
handful of short names a scan wins on branch prediction anyway.

The order still matters, because it is the order a caller enumerating a type's
methods sees. `type_methods_sorted` is there for a type's own test to say out
loud that its list is in Go's order.

### What is still not covered

`reflect.MakeFunc`, which synthesises a function pointer with an arbitrary
signature that plain C code can call directly. That genuinely needs runtime code
generation or libffi. `reflect_make_func` returns a `Func` usable from burrow
through the thunk protocol but **not** a raw C function pointer, and returns
`reflect_err_no_trampoline` if a raw pointer is asked for. An optional
`BURROW_ENABLE_LIBFFI=1` build lifts the restriction. This affects
`testing/quick`'s `quick.Check` on function values and nothing else in the
standard library.

## 7. The boundary, stated plainly

> **`reflect` is complete over types declared with `BURROW_STRUCT`/`BURROW_ENUM`/…
> or generated by `burrow-gen`. It cannot see an arbitrary, undeclared C
> struct, because that information does not exist in a C binary.**

`reflect_type_of` on an unknown pointer does not guess. `Any` carries an
explicit descriptor, so there is no way to construct one without a descriptor
in the first place — the failure is caught at the construction site, at compile
time, rather than deep inside `json.Marshal`:

```c
Any bad = BURROW_ANY(SomeUndeclaredStruct, &x);
/* compile error: no type_of(SomeUndeclaredStruct) */
```

That is the right failure mode. The user adds four lines of `BURROW_STRUCT` and
moves on. Compare the alternative — a runtime "cannot reflect on this type"
error at the bottom of a call stack — and the compile-time failure is clearly
better, which makes this limitation substantially less painful in practice than
it sounds in the abstract.

For the specific case of **marshalling a type you cannot modify** (a third-party
struct), three escapes, in order of preference: declare a descriptor separately
with `BURROW_STRUCT_EXTERNAL(T, FIELDS)` which emits only metadata against an
existing declaration; implement `json.Marshaler` by hand, which needs no
reflection at all; or use `jsontext` (Go 1.27's streaming token API) directly,
which is reflection-free by design and is one of the reasons `encoding/json/v2`
is a welcome addition here.

## 8. Struct tags

Go's struct tags are a convention over a string, parsed by each consumer. We
port `reflect.StructTag`'s `Get`/`Lookup` exactly, and the tag string format is
identical, which means **every Go struct tag in existing code works unchanged**
— `json:"name,omitempty"`, `xml:"ns url"`, `db:"col"`, `validate:"required"`.
This is a small detail with a large effect on how portable a port feels.

## 9. Ordering, and the gate

`reflect` blocks `fmt`, and `fmt` blocks everyone's ability to debug anything,
so this is the first Tier 0 subsystem after the core types. Sequence:

1. `Type` and the ops table; primitive descriptors.
2. `BURROW_STRUCT` and the composite type macros; `offsetof` correctness tests
   on all Tier A platforms (padding and alignment differ, and this is where
   big-endian s390x earns its CI slot). `BURROW_ENUM` and `BURROW_ALIAS` came
   out of this step for the reason given in §3.
3. `reflect`'s read side: `TypeOf`, `ValueOf`, `Kind`, `Field`, `Len`, `Index`,
   `MapKeys`, `Interface`, `String`.
4. `fmt`'s `%v`, `%+v`, `%#v`, `%T`. **This is the gate for Tier 0.**
5. `reflect`'s write side: `Set*`, `New`, `MakeSlice`, `MakeMap`, `Append`,
   `Elem`, `Addr`, `CanSet`.
6. `encoding/json` round-trip on a nested struct with tags, slices, maps and
   pointers. **This is the gate for Tier 1's format packages.**
7. Methods, thunks, `Value.Call`. Unblocks `net/rpc` and `text/template`.
8. The libclang generator; differential test that it produces identical
   descriptors to the DSL.

Step 4 is the milestone that makes the project feel real: once
`fmt_printf("%+v", myStruct)` prints what Go would print, byte for byte,
the reflection design is proven and 40 packages become unblocked at once.

## 10. Cost accounting

The thing to check is that we have not accidentally built something
unaffordable for the embedded use case that motivates C in the first place.

| | Cost |
| --- | --- |
| Per type declared | ~64 bytes descriptor + ~40 bytes per field, all rodata |
| Per method registered | ~40 bytes + a ~30-byte thunk, in text |
| Runtime registration | zero for reflection; one linker-section walk for the name registry |
| `reflect` code size | ~40 KB, and **only if linked** |
| If you never use reflection | descriptors for unused types are dropped by `--gc-sections`; `reflect`, `fmt`'s `%v` path, `json` all absent |
| `fmt.Printf("%d", n)` without reflection | a `BURROW_PRINT_NO_REFLECT` build gives a printf-alike with no descriptors at all, ~6 KB |

The last row matters: a firmware user who wants `strings`, `time` and
`encoding/hex` pays nothing for any of this, because the amalgamation generator
never emits it. → [15](15-build-deploy.md) §5

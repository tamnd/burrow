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

```
tools/burrow-gen reflect point.h -o point_gen.c --header point_gen.h -Iinclude
```

`tools/burrow-gen` reads the header with libclang and writes the descriptors.
Natural declarations, one build step, and one heavy build time dependency that
is not required to build burrow, to use the DSL, or to compile what the
generator emits. It is required to run the generator, once, when your structs
change.

Neither path is deprecated and neither is preferred. The DSL is the default
because of the no build step promise, and this exists because a large existing
codebase will not restructure its headers to get `%v` to work.

### It does not compute offsets

The generated file is full of `offsetof`, `sizeof` and `_Alignof`. The generator
works out which fields exist and what they are called, and nothing else.

That is not laziness. A generator that computed 12 for an offset would be
computing it for whatever machine the generator ran on, and the entire reason
this library is careful about layout is that the answer differs between the
machines it has to run on. Leaving the arithmetic to the compiler that is
actually building for the target is the only answer that is right everywhere,
and it has the pleasant side effect that a generated file can be committed and
then compiled for a target the generator has never seen.

`tests/gen_test.c` checks this on a struct whose second field is padded, so a
generator that started guessing would be caught rather than accidentally right.

### Two markers, one meaning

A block comment reading `burrow:reflect` above the typedef is the marker the
example uses, because it leaves the declaration alone. `BURROW_REFLECT` after
the typedef name does the same thing, for anyone whose tooling does not preserve
comments. Both expand to nothing in an ordinary build, as does `BURROW_TAG`, so
the struct a normal compiler sees is exactly the struct that was written.

The tag has to be a macro rather than a comment because a comment attaches to a
declaration and there is one declaration for the whole struct, while a tag
belongs to a field.

### What it refuses

A field's type has to be spellable as a single identifier, because `TYPE_OF`
pastes its argument onto a prefix. `char *name` is refused with a message saying
so and pointing at the line, and the fix is a typedef. This is the same
restriction the DSL has, for the same reason, and it is better as a message from
the generator than as a paste error in generated code.

A bit field is refused, because it has no address and therefore no `offsetof`. A
marked struct with no typedef over it is refused, because `struct Foo` is two
tokens and the descriptor could not be named after it. A marked typedef that is
not a struct is refused, because there are no fields to describe.

### It agrees with the DSL, and the test says so

`tests/gen/shapes.h` holds plain annotated structs, `tests/gen/shapes_gen.c` is
what the generator made of them, and `tests/gen_test.c` declares the same fields
again through the DSL and compares the two descriptors member by member. Kind,
size, alignment, field count, and every field's name, tag, type pointer and
offset.

The generated file is committed rather than built, so that building burrow never
needs libclang and a contributor without one can still run the whole suite.
`tools/check-gen.sh` regenerates it and diffs, and says so and passes on a
machine with no libclang. The output is stable across libclang versions:
libclang 18 on Linux and libclang 23 on macOS produce the same bytes.

### The binding

`tools/burrow-gen` needs Python 3 and a libclang shared library. It does not
need `pip install`, because the libclang binding is about a hundred lines of
`ctypes` at the bottom of the script. The full binding on PyPI is a fine piece
of work and it brings a hundred megabyte wheel with its own copy of libclang,
and a code generator that makes you build a virtualenv first is a code generator
people work around.

### Still to come

**`burrow-gen reflect --from-go`** would ingest a Go type declaration and emit
both the C struct and its descriptor. For someone porting a Go program that is
the natural direction of travel, and it is the same machinery the conformance
harness wants for translating Go's tests. It is not built.
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
const Type *t = type_by_name(BURROW_S("main.Point"), &err);
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

Both functions take an `Error *` and both of them fill it in, because the three
ways they can fail are three different situations for the caller. Section 7 is
about why.

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

> **`reflect` is complete over types declared with `BURROW_STRUCT`, generated by
> `burrow-gen`, or described by hand. It cannot see a C struct that nobody
> described, because that description does not exist in a C binary.**

Go's reflect never hits this wall, because the Go compiler writes a descriptor
for every type whether anybody asked for one or not. A C compiler writes none,
and no amount of cleverness at runtime gets the field names back out of a
binary that never had them. So the question is not whether burrow has a
boundary. It is where the boundary is, whether you find out at compile time or
at three in the morning, and what the library does when it reaches one.

### Asking about a value cannot reach it

An `Any` carries a descriptor, so there is no way to build one without having a
descriptor first:

```c
Any bad = BURROW_ANY(SomeUndescribedStruct, &x);
/* link error: burrow_type_SomeUndescribedStruct */
```

That is the failure mode to want. It names the missing thing, it happens at the
line that made the mistake, and the fix is a field list. Compare it to the
alternative, which is a runtime "cannot reflect on this type" arriving from the
bottom of `json_marshal` with no indication of which field started it.

### Asking by name can reach it, so it says which wall it hit

`type_by_name` is the one entry point that takes something from outside the
program. A name arrives off a socket or out of a config file, and the answer
can be no for reasons the caller needs to tell apart. So it takes an `Error *`,
the same way every other fallible function in burrow does, and fills it in:

| Answer | Error | What it usually means |
| --- | --- | --- |
| `NULL` | `type_err_not_registered` | The name is fine and this program was not built with that type. A version skew between two peers. |
| `NULL` | `type_err_name_invalid` | Not a type name at all: empty, a bare dot, a trailing dot, a leading one. A decoder that read a length wrong. |
| a descriptor | no error | |

Those two get fixed by different people. The first is a deployment to look at
and the second is a stream to stop trusting, and a caller holding nothing but a
`NULL` cannot tell which one it has. That is the whole argument for the out
parameter. It is optional, like every out parameter in the library, so a caller
who only wants to know whether there is a type passes `NULL` and reads the
pointer.

`type_register` is the same shape. It returns false for a conflict, for a
descriptor with no name to register it under, and for a table that could not
grow, and those are `type_err_conflict`, `type_err_name_invalid` and
`burrow_err_out_of_memory` respectively. A plugin loader can retry the third
one and cannot do anything useful about the first, which is exactly why one
bool for all three was not enough.

The rule underneath all of it: a reflect function that cannot answer returns
nothing and says why. It does not return its best guess. A registry that
matched on the last path element when the full name missed would find
`your.Config` for a stream asking about `theirs.Config`, and the program would
then decode someone else's bytes into it. That is not a hypothetical class of
bug, it is most of the deserialisation CVEs ever written.

`type_qualified_name` follows the same rule for a smaller reason. A name too
long for the buffer gives back the empty string rather than the first `cap`
bytes of itself, because a truncated qualified name is still a well formed
qualified name and would go straight back through a lookup as a question about
some other type.

### The way out, for a struct you cannot modify

The awkward case is a third-party struct: you want to marshal it, it lives in
somebody else's header, and no maintainer is going to take a patch that wraps
their type in a macro from a reflection library. Three ways out, in order of
preference.

Describe it from outside. `BURROW_STRUCT_DECL` emits the struct and the
descriptor's declaration; `BURROW_STRUCT_DEFINE` emits only the descriptor, and
nothing stops you pointing it at a struct that already exists:

```c
typedef struct vendor_rect VendorRect;   /* TYPE_OF pastes one identifier */

#define VENDOR_RECT_FIELDS(F, T)              \
    F(T, int32_t, w, "json:\"width\"")        \
    F(T, int32_t, h, "json:\"height\"")

BURROW_STRUCT_DEFINE(VendorRect, VENDOR_RECT_FIELDS);
```

The offsets come from `offsetof` on their struct, so this is their layout and
not a copy of it that can drift. What it cannot do is see a field the header
does not expose, and it will not compile if you name one that is not there.
There is a test on this in `tests/declare_test.c`, because an escape hatch
nobody compiles is an escape hatch that quietly stops working.

Or point `burrow-gen` at their header, which writes the same thing for you and
is the better answer when there are forty of them rather than one.

Or write the marshaller by hand, which needs no reflection at all, and is what
you were going to end up doing for the type with the union in it anyway.

## 8. Struct tags

A struct tag is one string on a field that holds several things at once, one per
package that cares about it. The format is a space separated list of
`key:"value"` pairs:

```
json:"id,omitempty" xml:"id,attr" db:"user_id"
```

`encoding/json` reads the `json` one, `encoding/xml` reads the `xml` one, and
neither has to know the other is there. The format is Go's exactly, which means
**every Go struct tag in existing code works unchanged**. That is a small detail
with a large effect on how portable a port feels, because a tag is the one piece
of a Go struct definition people copy across by hand.

Two functions, in `burrow/type.h`:

```c
BURROW_OWNS(value) bool tag_lookup(Alloc *a, Str tag, Str key, Str *value);
BURROW_OWNS(ret)   Str  tag_get(Alloc *a, Str tag, Str key);
```

`tag_lookup` is the one to reach for. It separates "the key is not there" from
"the key is there and its value is empty", and those mean different things:
`json:""` is a field that asked for the default name, and no `json` key at all
is a field that never opted in. `tag_get` is the convenience form for when you
do not need to tell those apart.

### Nothing validates the tag

A tag that does not parse is a tag nobody finds a key in, rather than an error.
That is Go's behaviour and it is the right one, because the place a malformed
tag is noticed is deep inside a marshaller that has nothing useful to do with an
error and no way to point at the line that caused it. The scan stops at the
first pair that is not in the format, so a good pair behind a bad one is not
reachable either.

The one case worth knowing about, because it catches people out in Go too: pairs
are separated by a space and by nothing else. A tab between two pairs ends the
scan and everything after it is invisible.

### Why the value is copied

Go returns a slice of the tag itself when the value has no escapes in it. It can
do that because a Go string is immutable and a tag lives as long as the program
does. burrow copies instead, always, and takes an allocator to do it.

The reason is the ownership scheme. The alternative is a function that sometimes
owns what it returns and sometimes borrows it, decided by whether the tag
happened to contain a backslash. There is no way to annotate that with
`BURROW_OWNS`/`BURROW_BORROWS`, no way for a caller to know which one it got,
and every other function in this library answers that question the same way
every time it is called. A copy of a short string, on an arena, in code that
caches its answer per type anyway, is not worth the hole that would put in it.

The value is a Go string literal with its quotes on, so getting the string out
means undoing the escapes: `\n`, `\xNN`, three digit octal, `\uHHHH`,
`\UHHHHHHHH` and the rest. That is `strconv.Unquote` for the double quoted case,
and it lives in `src/runtime/tag.c` for now because `strconv` is a later
milestone and this is needed today. It moves when `strconv` lands.

The unquoter runs twice, once with a NULL destination to measure and once to
write. That costs a second pass over a string that is almost always under thirty
bytes, and it buys an allocation of exactly the right size, so the `Str`'s
length and its allocation agree and the caller can free it. Sizing by an upper
bound instead would hand back a `Str` that cannot be freed correctly.

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

# 04 — Core types: mapping Go's type system onto C

Go's standard library is expressed in Go's type system. Before any package can
be ported, every construct that appears in a stdlib signature needs exactly one
C representation, chosen once. This document fixes those representations.

The test of this document is mechanical translatability: given a Go
declaration, a competent porter (or an agent) should be able to produce the C
declaration without a judgement call. Where a judgement call remains, it is
flagged.

## 1. The inventory of constructs

Every type that appears in a Go stdlib exported signature, and its fate:

| Go construct | C representation | §|
| --- | --- | --- |
| `bool`, `int8`…`uint64`, `float32/64` | `bool`, `int8_t`…`uint64_t`, `float`/`double` | 2 |
| `int`, `uint`, `uintptr` | `Int`, `Uint`, `Uintptr` | 2 |
| `complex64`, `complex128` | `Complex64`, `Complex128` structs | 2 |
| `byte`, `rune` | `Byte` (`uint8_t`), `Rune` (`int32_t`) | 2 |
| `string` | `Str` — `{const uint8_t *p; Int len;}` | 3 |
| `[]T` | `Slice` — `{void *p; Int len, cap; const Type *elem;}` | 4 |
| `[N]T` | plain C array in structs; `array_ref` at boundaries | 4 |
| `map[K]V` | `Map *` — opaque, descriptor-driven | 4 |
| `chan T` | `Chan *` — opaque | [06](06-runtime.md) §5 |
| `interface{...}` | vtable struct `{const TIfaceVT *vt; void *data;}` | 5 |
| `any` / `interface{}` | `Any` — `{const Type *t; void *data;}` | 5 |
| `error` | `Error` — an interface value, nullable | 6 |
| `func(...)` | `FuncT` — `{fnptr f; void *env;}` closure pair | 7 |
| `struct{...}` | C `struct`, field-for-field, with descriptor | 8 |
| embedded fields | anonymous member + promoted accessor macros | 8 |
| methods | `pkg_type_method(recv, ...)` free functions | 8 |
| pointer receivers | `Type *` first argument | 8 |
| multiple returns | out-parameters, error last→returned | 9 |
| variadic `...T` | `Slice` + `_v` va_list variant | 9 |
| type parameters | `_Generic` dispatch + explicit-type macros | [08](08-naming-abi.md) §5 |
| `iota` const blocks | `enum` or `#define`, per width | 10 |
| goroutine-local (`context`) | explicit `Context *` argument, as in Go | [06](06-runtime.md) §7 |

## 2. Numeric types

```c
typedef uint8_t  Byte;
typedef int32_t  Rune;

#if BURROW_PTR_BITS == 64
typedef int64_t  Int;
typedef uint64_t Uint;
#else
typedef int32_t  Int;
typedef uint32_t Uint;
#endif
typedef uintptr_t Uintptr;

typedef struct { float  re, im; } Complex64;
typedef struct { double re, im; } Complex128;
```

`Int` matching Go's platform-dependent `int` is the right call even though a
fixed `int64_t` would be simpler. Go's `len()` returns `int`, `strings.Index`
returns `int`, and the overflow characteristics of 32-bit `int` are observable
in tests on 32-bit platforms (`strconv`, `bytes.Repeat`'s overflow check,
`slices.Grow`'s capacity math). Fidelity requires matching the width.

Complex numbers get structs rather than C99 `_Complex` because MSVC does not
support `_Complex` in C mode and because Go's `complex128` has defined
behaviour on infinities and NaNs that `math/cmplx`'s tests pin. `math/cmplx` is
ported as arithmetic over these structs, not delegated to `<complex.h>`.

The arithmetic itself shipped as `burrow/num.h`, and the scope of that header is
narrow on purpose: it covers the operations where C is undefined, or where C is
defined and disagrees with Go, and nothing else. That is five cases. Signed
overflow, which is undefined in C and wraps in Go, so `int_add`, `int_sub`,
`int_mul` and `int_neg` exist for every signed width. Division by zero and
`MinInt / -1`, which fault on x86 and which Go answers, one with a message and
one with the wrapped value. Shifts at or past the width of the type, which are
undefined in C and which x86 answers with the low six bits of the count, while
Go gives zero or a sign extension. A negative shift count, which Go treats as a
run-time error. And float to integer conversion out of range, which is the one
case Go's own specification calls implementation dependent.

That last one needs a decision rather than a port, because there is no single
Go answer to copy: amd64 gives `MinInt64` for both `int64(1e300)` and
`int64(NaN)` while arm64 saturates and maps NaN to zero, and both are conformant
Go. burrow saturates and maps NaN to zero everywhere, matching arm64, wasm and
Rust. It is not a ledger entry, since the spec permits the answer and burrow is
conformant either way, but it is called out in the header, the guide and the
tests, because it is the one place a program ported from amd64 Go can change
behaviour.

Unsigned wrapping, float arithmetic, comparisons, the bitwise operators and
narrowing conversions are all the same in both languages, so they stay as the
plain operator and the header says so. The unsigned families still get the same
nine function names so that generic code does not have to know which half of the
table it is in, and those expand to the operator with the integer promotions
pinned down, which matters below 32 bits where `uint16_t * uint16_t` otherwise
promotes to a signed `int` and can overflow it.

`Int` and `Uint` get their own named functions rather than macro aliases to the
`int64` set, so that a 32-bit build gets 32-bit wrapping without a second code
path. The `_Generic` selection macros deliberately omit them, because on any
given platform they are a duplicate association with whichever fixed-width type
they already are.

## 3. Strings

```c
typedef struct { const Byte *p; Int len; } Str;
```

A two-word value, passed and returned by value. Not NUL-terminated. Immutable
by contract — `p` is `const` and no `burrow` function writes through it.

This is the single most consequential decision in the API, and the temptation
to use `char *` must be resisted absolutely. Go strings can contain NUL bytes;
`strings.Split("a\x00b", "\x00")` is meaningful; `os.ReadFile` on a binary file
produces bytes that are legal in a Go `string`. Every place a `char *` sneaks
in, a truncation bug follows.

Interop is explicit, in both directions, and both directions are cheap:

```c
#define BURROW_S(lit)  ((Str){ (const Byte *)(lit), sizeof(lit) - 1 })

Str str_from_cstr(const char *s);                  // O(n), no alloc, borrows
Str str_from_bytes(const void *p, Int n);        // no alloc, borrows
char  *str_to_cstr(Alloc *a, Str s);             // allocates, NUL-terminates
bool   str_has_nul(Str s);                          // for safe cstr conversion
int    str_cmp(Str a, Str b);                    // -1/0/+1, byte order
bool   str_eq(Str a, Str b);
```

`BURROW_S("literal")` is the workhorse and compiles to a compile-time constant
struct with no function call and no `strlen`. It is the reason the API stays
pleasant:

```c
if (strings_has_prefix(path, BURROW_S("/api/"))) { ... }
```

**Immutability and aliasing.** Go's compiler guarantees strings are immutable;
C cannot. The rule is contractual: functions taking `Str` never write to it
and never retain it beyond the call unless documented. Functions *returning*
`Str` state whether the result aliases an input (borrowed, valid as long as
the input) or is freshly allocated from the passed allocator (owned). This is
recorded in a machine-readable annotation on every declaration and checked by
the conformance harness under ASan. → [05](05-memory.md) §4

`strings.Builder`, `bytes.Buffer` and friends port directly; `Str` from a
builder is borrowed from the builder's arena.

**The range loop.** Go's `for i, r := range s` decodes UTF-8, and it is the one
piece of string behaviour that lives in the language rather than in a package,
so it has to be spelled here rather than in `unicode/utf8`. The shape is the
`MapIter` shape, which is the only one C offers for an iterator that has to
survive across calls:

```c
typedef struct StrIter { Str s; Int i; } StrIter;

StrIter str_runes(Str s);
bool    str_next_rune(StrIter *it, Int *index, Rune *r);
```

The index is the byte offset the rune started at, not a count of runes, because
that is what Go's loop yields and what a caller needs in order to reslice.
Both out-parameters are nullable, per §9. The implementation lives in
`src/core/str.c` and calls into `utf8_decode_rune_in_string`, so `core.h`
declares the loop without including `utf8.h` and the header order stays acyclic.
Invalid bytes yield `UTF8_RUNE_ERROR` and advance one byte, which is what makes
a loop over corrupt input terminate. → [09](09-packages-pure.md) §3

## 4. Slices, arrays and maps

```c
typedef struct {
    void          *p;
    Int         len;
    Int         cap;
    const Type *elem;   /* element descriptor: size, align, kind, ops */
} Slice;
```

Four words instead of Go's three. The `elem` descriptor is what makes generic
slice operations possible without templates, and what lets `reflect`, `fmt` and
`encoding/json` see into a slice they were handed. It is a pointer to a static
descriptor, so the cost is one word and zero initialisation work.

```c
Slice slice_make(Alloc *a, const Type *elem, Int len, Int cap);
void    *slice_at(Slice s, Int i);          /* bounds-checked, panics */
Slice slice_sub(Slice s, Int lo, Int hi);       /* s[lo:hi] */
Slice slice_sub3(Slice s, Int lo, Int hi, Int max);
Slice slice_append(Alloc *a, Slice s, const void *elems, Int n);
Int   slice_copy(Slice dst, Slice src);
```

Typed access is via macros that recover static typing where it is available:

```c
#define BURROW_AT(T, s, i)      (*(T *)slice_at((s), (i)))
#define BURROW_APPEND(T, a, s, v) \
    (slice_append((a), (s), (T[]){ (v) }, 1))

Slice parts = strings_split(arena, line, BURROW_S(","));
for (Int i = 0; i < parts.len; i++) {
    Str f = BURROW_AT(Str, parts, i);
    ...
}
```

Append semantics match Go's exactly, including the aliasing behaviour that
makes `append` subtle: if `cap` suffices, the backing array is written in place
and the old slice header now sees changed data. This is faithfully reproduced
because Go programs (and Go's tests) depend on it, growth factor included —
`slices.Grow`'s and `append`'s capacity progression is test-visible.

**Arrays.** Go's `[N]T` is a value type. Inside a C struct it is a plain C
array, which has the right layout but the wrong copy semantics (C arrays do not
assign). Where a stdlib signature takes or returns an array by value — `[32]byte`
from `sha256.Sum256`, `[16]byte` from `md5.Sum` — it becomes a small named
struct wrapping the array, which *does* assign:

```c
typedef struct { Byte a[32]; } Sha256Sum256Ret;
Sha256Sum256Ret sha256_sum256(Slice data);
```

Ugly in the type name, correct in behaviour, and the `BURROW_SHORT` layer supplies
`sha256_Sum256`. → [08](08-naming-abi.md) §6

**Maps.**

```c
typedef struct Map Map;   /* opaque */

Map *map_make(Alloc *a, const Type *key, const Type *val, Int hint);
void   *map_get(Map *m, const void *key);          /* NULL if absent */
bool    map_get2(Map *m, const void *key, void *out_val);
bool    map_set(Map *m, const void *key, const void *val);
void    map_del(Map *m, const void *key);
void    map_clear(Map *m);
void    map_free(Map *m);
Int     map_len(const Map *m);

MapIter map_iter(Map *m);
bool    map_next(MapIter *it, const void **key, void **val);
```

Implemented as Swiss tables, matching Go 1.24+, because the iteration-order
randomisation, growth behaviour and `hash/maphash` seeding are observable and
because it is simply the right data structure. The per-map hash seed comes from
the process seed exactly as Go's does, which means iteration order is randomised
and `burrow` will surface the same "don't depend on map order" bugs Go does —
deliberately, since silently stable order would let users write code that breaks
when they port back.

`Map` is opaque and heap-allocated (Go's `map` is also a pointer under the
hood), so `map` values are reference-like in C exactly as in Go.

Three signatures above differ from what this document first wrote down, and each
difference is forced by C rather than chosen:

- `map_set` returns `bool`. Go's assignment cannot fail because the runtime
  stops the world on an allocation failure. A library that takes an allocator
  from the caller has to hand that decision back, so `false` means the table
  needed to grow and the allocator refused, and the map is unchanged.
- The iterator is a caller-owned value, `MapIter it = map_iter(m)`, not a
  cursor the map holds. Go's `range` hides the same state in the frame. Making
  it a struct the caller declares is what allows two live iterators over one
  map, which Go allows, without the map paying for a list of them.
- `map_free` exists, and it is the first per-object free in the library.
  → [05](05-memory.md) §2.

One behavioural deviation, and it is the only one in the core types. Go's table
is a directory of fixed-size tables and grows by splitting one, so a growth
leaves every other entry where it was and an iterator survives it. This
implementation is a single table that doubles and reinserts, which is what
Abseil does and what makes the lookup path as tight as it is. The cost is that
a growth moves every entry, so an iterator cannot survive one. Go's
specification already leaves the result of inserting during a range
unspecified, so rather than produce an arbitrary answer, `map_next` calls
`runtime_throw` with `map grew during iteration`. Keeping the old table alive
for the iterator is the Go answer and it needs a collector to decide when the
old table dies; without one the choice is a dangling pointer or a leak, and
reporting the program's existing bug beats both.

The table stores its `Alloc *`, which is the documented exception to the
allocator-passing rule. → [05](05-memory.md) §2.

## 5. Interfaces

Go's interfaces are the backbone of the library: `io.Reader`, `io.Writer`,
`sort.Interface`, `fmt.Stringer`, `error`, `http.Handler`, `net.Conn`,
`fs.FS`. There are two distinct cases and they get two distinct
representations.

**Non-empty interfaces → explicit vtable pair.**

```c
typedef struct IoReaderVT {
    const Type *self_type;
    Int (*read)(void *self, Slice p, Error *err);
} IoReaderVT;

typedef struct IoReader {
    const IoReaderVT *vt;
    void *data;
} IoReader;
```

Two words, nil-able (`vt == NULL`), passed by value. Calling is a macro so it
reads like a method:

```c
#define BURROW_CALL(iface, m, ...) ((iface).vt->m((iface).data, __VA_ARGS__))

Int n = BURROW_CALL(r, read, buf, &err);
```

There is a `BURROW_CALL0` next to it for methods that take no arguments, because
C99 requires at least one argument for the ellipsis and `__VA_OPT__`, which
would avoid the second macro, is C23. `Close`, `String` and `Len` are common
enough that living without it was worse.

Implementing an interface is a static vtable plus a constructor — the libgit2
pattern, explicit and greppable:

```c
static Int mything_read(void *self, Slice p, Error *err) { ... }
static const IoReaderVT mything_reader_vt = { &mything_type, mything_read };

IoReader mything_as_io_reader(MyThing *t) {
    IoReader r = { &mything_reader_vt, t };
    return r;
}
```

Every stdlib type that satisfies an interface gets a generated
`pkg_type_as_io_reader(...)` adapter and a static vtable, so composing
`bufio.NewReader(os.Stdin)` is as short in C as in Go:

```c
BufioReader *br = bufio_new_reader(a, os_stdin_as_io_reader());
```

**Interface embedding** (`io.ReadWriter` = `Reader` + `Writer`) becomes vtable
struct embedding, with the embedded vtables held as named members:

```c
typedef struct IoReadWriterVT {
    IoReaderVT reader;
    IoWriterVT writer;
} IoReadWriterVT;
```

Down-converting is the address of a member, with no cast and no assumption about
layout: `(IoReader){&rw.vt->reader, rw.data}`. The library writes those out as
`io_read_writer_as_io_reader` and friends, since C has no implicit conversion
step.

This section originally called for the outer vtable to be a prefix-compatible
superset reached by a pointer cast, checked by a generated static assertion on
offsets. That was changed when the code was written, for a reason worth
recording: a prefix cast works for whichever interface is embedded first and
quietly does not for the second, because the second one's function pointers sit
at an offset the cast knows nothing about. The static assertion would have had
to be right about every combination, and the thing it was protecting was not
buying anything, since the member address compiles to the same instruction the
cast does.

Combination to combination conversion is not offered at all, so there is no
`io_read_write_closer_as_io_read_writer`. The value is two words with nowhere to
keep a vtable and a function cannot return a pointer to one it made on its
stack, so the only way to produce it is the layout assumption above. The answer
is to go back to the concrete type, which has an `_as_` adapter for every
interface it satisfies.

**Type assertions and type switches.** These need the dynamic type, which
non-empty interface values do not carry. Solution: every generated vtable's
first member is a `const Type *self_type` slot, so:

```c
OsFile *f = iface_assert(BURROW_IFACE(r), TYPE_OS_FILE);  /* NULL if it is not */
```

Zero cost for the common case, and a full answer when needed. `BURROW_IFACE` is
the cast to the generic `Iface`, and it is defined behaviour rather than a trick
because C guarantees that a pointer to a struct points at its first member. A
vtable may leave `self_type` as `NULL`, which means the type declines to be
asserted to, the same thing an unexported type gets you in Go.

**Empty interface → `Any`.**

```c
typedef struct Any { const Type *t; void *data; } Any;

#define BURROW_ANY(t, ptr)      ((Any){ (t), (void *)(ptr) })
#define BURROW_ANY_VAL(t, T, v) ((Any){ (t), (void *)(T[]){(v)} })
```

`Any` is what `fmt.Printf`'s variadic arguments, `encoding/json.Marshal`'s
parameter, `sync.Map`'s values and `context.WithValue`'s value become. It
carries a descriptor, which is why [07](07-reflect.md) is a prerequisite for
`fmt`.

**Note on boxing.** Go's interface values sometimes store small values directly
and sometimes point to heap copies, invisibly. `Any` and interface values
always hold a pointer, so the caller must ensure the pointee outlives the call.
For literals, `BURROW_ANY_VAL(TYPE_INT, Int, 42)` uses a compound literal whose
lifetime is the enclosing block, which is correct for the overwhelmingly common
`Printf` case and is documented. `any_box(a, v)` copies into an allocator when
the value must escape, through the descriptor's copy operation and no deeper,
so boxing an `Any` holding a `Slice` copies the header and not the elements.

`any_equal` is Go's `==` on two interface values, including the part where
comparing two uncomparable values is a run time failure rather than a compile
error, because the static type on both sides is `any` and neither compiler can
see inside. `TYPE_ANY` exists and hashes the dynamic type along with the value,
which is what makes `map[any]T` behave as Go's does.

## 6. Errors

```c
typedef struct ErrorVT {
    const Type *self_type;
    Str    (*message)(const void *self);
    Error  (*unwrap)(const void *self);         /* NULL if not wrapping */
    Slice  (*unwrap_multi)(const void *self);   /* the Unwrap() []error form */
    bool   (*is)(const void *self, Error target);
    const void *(*as)(const void *self, const Type *target);
} ErrorVT;

typedef struct { const ErrorVT *vt; const void *data; } Error;

#define BURROW_NO_ERROR  ((Error){ NULL, NULL })
#define BURROW_FAILED(e) ((e).vt != NULL)
#define BURROW_OK(e)     ((e).vt == NULL)
```

Four things in there differ from the first draft of this section, and all four
came out of writing the code:

- `data` is `const void *`, so a sentinel can live in read only memory with no
  cast anywhere. The alternative was throwing `const` away a few hundred times
  across the library.
- `as` returns the pointer rather than filling one in and returning a bool. In C
  the pointer is the bool, `void *` converts to any object pointer so there is no
  cast at the call site, and both of Go's panics (nil target, non pointer target)
  stop existing.
- The message method is `message`, and the function is `error_message`. The
  mechanical rule gives `error_error`, which is the one place the naming rules
  produce a name that says the same word twice. → [08](08-naming-abi.md) R2
- The out of memory sentinel is `burrow_err_out_of_memory`, not
  `errors_err_out_of_memory`, because it is not a Go symbol and the coverage
  round trip maps every `errors_` symbol back to Go's API manifest.

`message` also takes no allocator, so an error builds its message at
construction time and printing one cannot fail. Go builds it in `Error()` on
demand, which saves an allocation when nobody prints; on an error path the
unfailing print is worth more.

`Error` is an interface value with `Unwrap`, `Is` and `As` promoted into the
vtable rather than discovered by type assertion. Go discovers them
dynamically; putting them in the vtable is faster, and `errors.Is`/`As`/
`Unwrap` behave identically from the outside, including `Unwrap() []error`
multi-error trees (the second slot, `unwrap_multi`).

Two slots for one Go method is the one place the vtable is wider than the
interface. A Go type cannot satisfy both forms, because it has a single method
of that name, so a vtable with both filled in has no Go equivalent and no Go
behaviour to be faithful to. `errors_is`, `errors_as` and `errors_unwrap` all
take the chain form in that case, since it is the first arm of Go's type switch,
which keeps the three of them answering the same question the same way.

Three constructors cover almost everything:

```c
Error errors_new(Alloc *a, Str text);
Error fmt_errorf(Alloc *a, Str format, ...);   /* %w supported */
extern const ErrorVT burrow_sentinel_error_vt;    /* for package vars */
```

Sentinel errors — `io.EOF`, `os.ErrNotExist`, `sql.ErrNoRows`, and there are
several hundred across the library — are **static, immortal and allocation
free**:

```c
BURROW_SENTINEL_ERROR(io_eof, "EOF");
```

which expands to a file-scope `const` struct plus an `extern` declaration in
the header. `BURROW_FAILED(err) && errors_is(err, io_eof)` needs no allocator
and no cleanup, which matters because error paths must never themselves be able
to fail.

**Error handling with allocators.** An error can be constructed only if an
allocator is available, and error paths are exactly where allocation is least
welcome. Two mitigations: sentinels need none, and every allocator has a
reserved emergency block so that constructing an error message never fails.
If even that is exhausted, `errors_new` and `errors_join` return a static
`burrow_err_out_of_memory`. Allocation failure is therefore never a crash and
never a silent nil error. → [05](05-memory.md) §7

## 7. Function values and closures

```c
typedef struct ReadFn {
    Int (*f)(void *env, Slice p, Error *err);
    void *env;
} ReadFn;
```

Every function type in a stdlib signature, `http.HandlerFunc`, `sort.Slice`'s
`less`, `filepath.WalkFunc`, `slices.SortFunc`'s comparator, `sync.Once.Do`'s
`f`, becomes a named `{fnptr, env}` pair. A bare function pointer would be
cheaper and would make every callback that needs captured state require a
global, which is the classic C API mistake. The `env` word is non-negotiable.

`BURROW_FUNC` declares one, and `BURROW_FUNC0` is the same thing for a
signature that takes nothing, since C99 has no way to write a variadic macro
accepting an empty argument list. The parameters written at the declaration are
the ones the caller writes; the macro puts the `env` in front of them.

```c
BURROW_FUNC(Filter, bool, Str s);
BURROW_FUNC0(Func, void);
```

`Func` is Go's `func()` and ships declared, spelled without a package under R5
of [08](08-naming-abi.md) because it is a builtin type and not any package's.

The three rules are the interface rules from section 5, for the reason that a
function value is an interface with one unnamed method. The function pointer is
first, so a zeroed value is nil and `BURROW_FUNC_IS_NIL` asks. The env goes in
first at the call, which `BURROW_CALLF` and `BURROW_CALLF0` do, and the target
declares the parameter even when it ignores it: calling through a pointer of a
different type is undefined behaviour and traps under wasm or control flow
integrity, so nothing casts the pointer. And the env has to outlive the value,
which is the one part Go does silently by moving a captured variable to the
heap and the one part nothing here can see.

```c
Filter f = BURROW_FN(Filter, has_prefix, &ctx);
bool keep = BURROW_CALLF(f, line);
```

Go closures capturing variables become an explicit context struct, which is
mechanical and is what the porting guide covers. Capture by reference comes out
of it for free, since the env is a pointer. Not as pretty as Go, entirely
workable.

## 8. Structs, methods and embedding

Structs port field-for-field, in declaration order, with a
`BURROW_STRUCT` declaration macro that also emits the type descriptor
([07](07-reflect.md) §3). Field names keep Go's capitalisation; unexported Go
fields become fields with a `_` suffix and are excluded from the descriptor's
public view, preserving `encoding/json`'s "unexported fields are skipped" rule.

Methods become free functions with the receiver first:

| Go | C |
| --- | --- |
| `func (b *Builder) WriteString(s string) (int, error)` | `Int strings_builder_write_string(StringsBuilder *b, Str s, Error *err)` |
| `func (t Time) Add(d Duration) Time` | `Time time_add(Time t, Duration d)` |

Value receivers take the value, pointer receivers take the pointer — matching
Go means matching the mutation semantics for free.

**Embedding** is the one place a judgement call survives. Go's
`type Buffer struct { io.Reader }` promotes `Read` onto `Buffer`. C has
anonymous struct members (C11) which handles *field* promotion, but method
promotion needs generated forwarders. Decision: embedded fields become named
members whose name is the embedded type's name (as Go does), plus generated
forwarding functions for every promoted method:

```c
typedef struct {
    IoReader io_Reader;     /* embedded */
    Int       n;
} IoLimitedReader;

/* generated */
static inline Int io_limited_reader_read(IoLimitedReader *l, Slice p, Error *e);
```

Generated by the same tool that emits descriptors, from the same declaration.

## 9. Multiple returns and variadics

Go's `(T, error)` is the library's dominant signature shape. The rule:

> **The first result is the return value. Every subsequent result is an
> out-parameter, in order, at the end of the parameter list. `error` is always
> last. Out-parameters may be `NULL` to discard.**

```go
func Atoi(s string) (int, error)
func ReadFile(name string) ([]byte, error)
func (m *Map) Load(key any) (value any, ok bool)
func ParseFloat(s string, bitSize int) (float64, error)
```

```c
Int   strconv_atoi(Str s, Error *err);
Slice os_read_file(Alloc *a, Str name, Error *err);
Any   sync_map_load(SyncMap *m, Any key, bool *ok);
double   strconv_parse_float(Str s, Int bitSize, Error *err);
```

When Go returns three or more meaningful values, or when the first result is
itself an out-shaped aggregate, a named result struct is generated instead —
consistent, if slightly verbose:

```c
typedef struct { Str before; Str after; bool found; } StringsCutRet;
```

Functions with no meaningful first result return `Error` directly, which
makes the common check natural:

```c
Error err = os_write_file(path, data, 0644);
if (BURROW_FAILED(err)) return err;
```

**What shipped.** The rule above, unchanged, plus `BURROW_OUT(p, v)` for the
writing side of it:

```c
if (min > buf.len) {
    BURROW_OUT(err, io_err_short_buffer);
    return 0;
}
```

It writes through the pointer if there is one and does nothing if there is not.
The alternative was `if (err != NULL) *err = ...` at every site, which is three
lines of noise around one assignment and which gets forgotten, and a forgotten
check is a crash in a caller who did nothing wrong. The pointer appears twice in
the expansion, so the macro's contract is that it takes a pointer variable and
not an expression with a side effect in it. `src/io/io.c` is the first user and
every function that grows a second result is expected to use it.

The `NULL`-is-allowed half of the rule has to hold without exceptions to be
worth having. A caller who wants only the first result should not have to
declare a variable to throw away, and a rule with holes in it gets looked up
every time instead of learned once.

**Variadics.** Go's `...T` is a slice. The C form takes a `Slice`, and a
`va_list` convenience variant is generated for the handful of cases where
call-site brevity matters (`fmt.*`, `log.*`, `errors.Join`, `append`):

```c
Int fmt_printf(Str format, ...);          /* Any arguments */
Int fmt_vprintf(Str format, va_list ap);
Int fmt_printf_s(Str format, Slice args);  /* slice of Any */
```

The `...` form takes `Any` values, built with `BURROW_ANY`, and terminates on an
argument count derived from the format string — as Go's does, including the
`%!(EXTRA ...)` and `%!v(MISSING)` diagnostics, which are test-pinned.

## 10. Constants

Go's `iota` blocks become enums where the values fit `int` and the type is
`int`-like, and `#define`d typed constants otherwise (`uint64` flags,
`time.Duration` values, `os.FileMode` bits):

```c
typedef Int64 Duration;
#define TIME_NANOSECOND  ((Duration)1)
#define TIME_MICROSECOND ((Duration)1000)
#define TIME_MILLISECOND ((Duration)1000000)
#define TIME_SECOND      ((Duration)1000000000)
```

`syscall` and `debug/*` contribute ~12,000 constants between them and are
generated wholesale from Go's source by a translator, never hand-typed.
→ [10](10-packages-os.md) §5

## 11. The zero value

Go guarantees every type has a useful zero value, and the library leans on it
hard: `var buf bytes.Buffer` works, `var mu sync.Mutex` works, `sync.WaitGroup`
works, a nil map reads as empty, a nil slice appends correctly.

C's `= {0}` gives the same bit pattern, so this transfers — **on the condition
that no type requires non-zero initialisation.** That is a real constraint on
every design decision in Tier 0, and it is worth the cost:

- `SyncMutex` must be usable as `{0}`, so it cannot be a
  `pthread_mutex_t` (whose initialiser is not portably all-zero). It is a
  futex-style atomic word with a slow path. → [06](06-runtime.md) §4
- `BytesBuffer` zero value must be a valid empty buffer that allocates
  lazily from the allocator it is handed on first write.
- A `NULL` `Map *` must read as an empty map (`map_len(NULL) == 0`,
  `map_get(NULL, k) == NULL`) and panic on write, exactly as Go's nil map
  does.
- A zero `Slice` is a valid nil slice: `len == cap == 0`, `p == NULL`,
  appendable. `elem == NULL` is tolerated by `append` when an element
  descriptor is supplied.

`BURROW_ZERO(T)` is provided for clarity, and expands to a compound literal, so
it is for the places that need a value rather than an initialiser: a `return`, a
call argument, a comparison. Something with static storage still gets `= {0}`
written out by hand, which is the same bits and is a constant expression.

The plan here was a generated static assertion per public struct, checking that
the zero value passes an `is_valid_zero` predicate. That does not work and is
not what shipped. C has no way to evaluate `str_is_empty` or `map_len` at
compile time, and a static assertion over the bytes of a struct would only be
restating `= {0}` back to itself. What stands in for it is a test,
`the_zero_value_of_every_type_is_the_useful_one` in `tests/core_test.c`, which
takes the zero value of every public type and exercises it: the empty `Str`, the
nil `Slice`, the succeeding `Error`, the nil `Map` read through `map_len(NULL)`
and `map_get(NULL, k)`, the nil interface, the nil `Func`. It grows by a few
lines whenever a type lands, and that growth is part of landing a type.

One qualification on the `Slice` entry above. A zeroed `Slice` has no element
descriptor, and `slice_append` needs to know how big an element is, so the zero
value is the one that cannot do everything its non-zero form can.
`slice_nil(TYPE_INT)` is the nil slice that can be appended to. Allocators are
the other qualification and are not really one: an `Arena` holds memory rather
than describing it, so it has `arena_init` for the same reason a file has
`open`.

## 12. What this buys

A worked example — `net/http`'s hello world, in Go and in `burrow`:

```go
http.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
    fmt.Fprintf(w, "hello, %s", r.URL.Path)
})
log.Fatal(http.ListenAndServe(":8080", nil))
```

```c
#include <burrow/net/http.h>
#include <burrow/fmt.h>
#include <burrow/log.h>

static void hello(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    fmt_fprintf(http_response_writer_as_io_writer(w),
                   BURROW_S("hello, %s"), BURROW_ANY(Str, &r->URL->Path));
}

int main(void) {
    http_handle_func(BURROW_S("/"), BURROW_HANDLER(hello, NULL));
    log_fatal(http_listen_and_serve(BURROW_S(":8080"), BURROW_NIL_HANDLER));
}
```

Eleven lines against five. Longer, obviously — but it is recognisably the same
program, every name is derivable from the Go name, and there is no context
object, no init call, no error-code checking on the happy path, and no manual
memory management visible at all (the server owns per-request arenas;
[05](05-memory.md) §6). That is the ergonomic bar the rest of the design has to
clear.

# 05 — Memory: allocator passing, arenas, and ownership

Go's standard library allocates without ceremony because a garbage collector
exists. Reproducing that library in C without a GC is the central design
problem of the whole project — larger than the scheduler, larger than
`reflect`. This document settles it.

## 1. Why the obvious answers fail

**Per-object `free`.** Document ownership at every boundary, expose
`X_free()` for every type. This is what most C libraries do and it does not
scale to 23,730 declarations. Consider `strings.Split`: it returns a slice
whose *header and backing array are freshly allocated* but whose *elements are
sub-slices aliasing the input string*. So the caller frees one thing and must
not free the others, and must keep the input alive. Now multiply by the whole
library. Every function needs a bespoke ownership paragraph, every caller needs
to have read it, and a single mistake is a use-after-free. Worse, it cannot be
checked mechanically, so the conformance harness cannot catch regressions.

**A bolt-on tracing GC as the only mode.** Link Boehm, match Go's signatures
exactly, done. This is genuinely attractive and it is the wrong *default* for a
library. A tracing collector forces its runtime onto every consumer — the
collector has to be initialised by the host and needs to see the real stack —
which breaks `burrow` as an embeddable component, breaks it inside another
language's runtime (the FFI case that is half the point), and breaks it on
freestanding targets. Conservative collectors also identify pointers by
heuristic, so behaviour can shift with OS version, machine memory or a change
in how the compiler spills registers. Acceptable in a program you own end to
end; not acceptable as the only option in a library other people embed.

**Reference counting everywhere.** Deterministic, FFI-friendly, plays well
with C — genuinely the right answer for some libraries. Wrong here for two
reasons. It does not handle cycles, and Go's library creates them casually
(`http.Request` ↔ `http.Response`, `go/ast` parent links, `net/http`'s
`Transport` graph). And it changes the API: every getter must declare whether
it returns a borrowed or owned reference, and every caller must `ref`/`unref` —
which is the per-object problem again, with extra instructions in every user
loop.

## 2. The rule

> **Every `burrow` function that allocates memory for data takes a
> `Alloc *a` as its first parameter. There are no exceptions.**

This is Zig's convention, and it is the only formulation that survives contact
with 180 packages, because it is a single sentence that a porter can apply
mechanically and a reviewer can check by looking at a signature.

```c
Slice strings_split(Alloc *a, Str s, Str sep);
Str   strings_to_upper(Alloc *a, Str s);
Slice os_read_file(Alloc *a, Str name, Error *err);
Map  *map_make(Alloc *a, const Type *k, const Type *v, Int hint);

/* no allocation → no allocator */
bool     strings_has_prefix(Str s, Str prefix);
Int   strings_index(Str s, Str sub);
Str   strings_trim_space(Str s);          /* returns a sub-slice */
Int   strconv_atoi(Str s, Error *err);
```

Three things fall out of it immediately, and they are the payoff:

1. **Allocation is visible in the signature.** You can tell from the header
   whether a call can allocate, which is information Go's API hides and which
   matters enormously in the contexts where people reach for C.
2. **Ownership is uniform.** Everything a call allocated came from `a`. There
   is nothing else to know, and nothing to document per function.
3. **The policy is the caller's.** The same `burrow` build serves a web server
   that wants per-request arenas, a firmware image that wants a fixed pool, and
   a script that wants Boehm GC.

### The three carve-outs

Stated here, in full, so that "no exceptions" stays true elsewhere.

**Errors do not use the caller's allocator.** Error paths must not be able to
fail and must not require the caller to have an allocator in scope. `Error`
values are allocated from a per-goroutine **error arena** owned by the runtime:

```c
Int   strconv_atoi(Str s, Error *err);        /* no Alloc */
Error fmt_errorf(Str format, ...);               /* no Alloc */
```

The error arena has a documented lifetime: an error is valid until the
enclosing **error scope** is popped, and scopes are pushed/popped by the
runtime at natural boundaries (goroutine start/end, one HTTP request,
one `testing.T` run) or explicitly by the user:

```c
BURROW_ERROR_SCOPE {
    Error err = do_work();
    if (BURROW_FAILED(err)) log_it(err);
}   /* errors from this scope are reclaimed here */

/* to outlive the scope: */
Error kept = error_retain(a, err);   /* deep-copies into a */
```

Sentinel errors (`io_eof` and several hundred others) are static and live
forever, so the overwhelmingly common `errors_is(err, io_eof)` path
touches no arena at all. Every error arena reserves an emergency block so
constructing a message never fails; if even that is exhausted,
`errors_err_out_of_memory` — itself static — is returned.

**Objects with an explicit lifetime keep their constructor/destructor pair.**
Where Go itself has a `Close`, a `Stop` or a finaliser — `os.File`, `net.Conn`,
`http.Server`, `sql.DB`, `time.Timer`, `exec.Cmd` — the C version keeps the
same paired API, because the resource is an OS handle and not memory. These
still take an allocator for their memory; `Close` releases the *handle*, and
the memory goes when the arena goes. This matches Go, where forgetting `Close`
leaks an fd regardless of the GC.

**A growable container stores the allocator it was made with.** Today that is
exactly one type, `Map`. An insert can grow the table, so `map_set` allocates,
and the rule as written would put an `Alloc *` on the hottest operation a hash
table has:

```c
Map *m = map_make(a, TYPE_STRING, TYPE_INT, 0);
bool ok = map_set(m, &key, &val);   /* no Alloc, and it can still allocate */
```

The signature hides an allocation, which is carve-out 1 above given up, and it
is worth being clear that is the cost. What is bought is the property the rule
exists for. A caller who passes a different allocator on the second insert gets
a table with half its memory from one place and half from another, which is an
ownership question per insert instead of per map, and neither the caller nor a
reviewer can see it in a signature either. Storing it once means everything a
map allocated came from the one allocator its `map_make` was given, which is
point 2 of the payoff, kept.

`Slice` is the type that shows why this is a carve-out and not a new rule.
`slice_append` takes an allocator because a `Slice` is a value the caller
holds, four words they can copy, alias and sub-slice, and there is nowhere to
put a fifth word without making every slice in the library bigger for the
benefit of the calls that grow. A `Map` is a pointer to a header that is
already opaque, so one more field in it costs nothing per use and nothing per
copy, because a map is not copied.

`map_free` follows from the same asymmetry. `Str` and `Slice` need no free
function, since they hand you the pointer and the size and `mem_free` takes
both. Nothing outside `map.c` can name a map's pointer or its size, so the
free has to live there. It is the first per-object free in the library and the
argument in §1 against per-object `free` still holds: this one is safe to have
because there is exactly one thing to free, the map itself, there is no
aliasing question, and arena and GC users can carry on ignoring it.

## 3. The allocator interface

```c
typedef struct Alloc Alloc;

typedef struct AllocVT {
    /* All sizes in bytes; align is a power of two. Returns NULL on failure. */
    void *(*alloc)  (void *self, size_t size, size_t align);
    void *(*realloc)(void *self, void *p, size_t old, size_t nsz, size_t align);
    void  (*free)   (void *self, void *p, size_t size, size_t align);
    /* Optional. NULL means "not supported". */
    void  (*reset)  (void *self);
    AllocStats (*stats)(void *self);
} AllocVT;

struct Alloc { const AllocVT *vt; void *self; };
```

Deliberately the same shape as an interface value ([04](04-core-types.md) §5),
so users can write their own with no special machinery. `free` receives the
size and alignment, which is what lets pool and arena implementations be fast
and what lets the tracking allocator detect mismatches.

## 4. Ownership annotations, and why they are machine-readable

Even with a uniform allocator, one question remains per function: does a
returned `Str`/`Slice` *alias its input* or is it *freshly allocated*?
Go's GC makes the distinction invisible; in C it decides whether the input can
be freed.

So every declaration carries an annotation, in the header, in a form the
tooling parses:

```c
BURROW_OWNS(ret)        Str   strings_to_upper(Alloc *a, Str s);
BURROW_BORROWS(ret, s)  Str   strings_trim_space(Str s);
BURROW_STATIC(ret)      const char *burrow_version(void);
BURROW_OWNS(ret) BURROW_BORROWS(ret.elem, s)
                    Slice strings_split(Alloc *a, Str s, Str sep);
BURROW_RETAINS(w)       Int   io_copy(IoWriter w, IoReader r, Error *e);
```

They live in `burrow/own.h`, which is a header of its own rather than a section
of `mem.h` because `platform.h` and `version.h` have functions to annotate and
no business depending on the allocator interface to do it.

`BURROW_STATIC` is the third answer to the lifetime question and it is not a
weaker `BURROW_BORROWS`. It says the result has static storage duration or is
nil, so there is nothing to free and nothing it can outlive. A borrow has to
name what it came from, and a version string or a type descriptor has nothing to
name. Writing `BURROW_BORROWS(ret)` with no source for those would have been
indistinguishable from somebody forgetting to fill in the source, which is
exactly the silence the annotations exist to remove.

`BURROW_OWNS` and `BURROW_BORROWS` can both appear on one declaration, and on
the growable containers they do. `slice_append` writes into the array it was
given when there is capacity and allocates a bigger one when there is not, and
the caller cannot tell which happened. So the caller has to keep the input alive
and also free the result, and annotating only one of the two would have told
them to do half of that.

All four expand to nothing by default. Three consumers read them:

- **The doc generator**, which turns them into the lifetime sentence in every
  function's documentation. Nobody writes those sentences by hand.
- **The conformance harness**, which generates an ASan test per annotated
  function: free the input, then touch the output, and require a report exactly
  when `BURROW_BORROWS` says the output aliases the input. A wrong annotation is a
  test failure, which is the property per-object ownership schemes can never
  have. → [14](14-conformance.md) §6
- **An optional clang plugin** for users who want static checking in their own
  code.

`strings.Split`'s awkward case — owned header, borrowed elements — is expressed
precisely and checked, rather than buried in prose.

Two checks keep them honest, and they answer different questions.
`tools/check-annotations.sh` is structural and runs in `make check`: a
declaration whose return type carries a pointer needs an `OWNS`, a `BORROWS` or
a `STATIC` naming `ret`, and every name inside an annotation has to be `ret` or
a parameter of that same declaration. That catches the declaration added without
one and the annotation that was right until somebody renamed the parameter.
Whether an annotation is *true* needs the memory to exist, so it is checked at
runtime in `tests/lifetime_test.c` using the tracking allocator: `OWNS` means the
live block count went up, `BORROWS` and `STATIC` mean it did not, and where the
borrow is into a buffer the result has to be inside that buffer.

## 5. The backends

One API, five implementations, chosen by the user at the call site (not at
build time, except where noted).

### `arena` — the default

Bump-pointer allocation from chunked regions. `alloc` is a pointer increment
and a bounds check; `free` is a no-op except for the most recent allocation
(which unwinds, making `append`-in-a-loop efficient); `reset` reclaims
everything at once.

```c
arena ar; arena_init(&ar, NULL /* parent */, 64*1024);
Alloc *a = arena_alloc(&ar);
... do work ...
arena_free(&ar);          /* everything above is gone, cannot leak */
```

This is the mode that makes the rule pleasant instead of merely tractable.
"Parse a config file, build a structure, extract three values, throw the rest
away" needs zero per-object bookkeeping and cannot leak. It is also
substantially faster than `malloc` for the allocation-heavy text processing
that a large fraction of the library does.

Arenas nest. A child arena can be reset independently and is reclaimed with its
parent, which gives request scoping for free.

### `heap` — `malloc`/`free` passthrough

For long-lived objects, for interop with code that will `free()` the result,
and for users who want exactly what they are used to. Every allocation must be
individually freed, and the `BURROW_OWNS`/`BURROW_BORROWS` annotations become
load-bearing rather than informational. This is the mode where the library is
least pleasant and most familiar.

### `gc` — Boehm-backed

```c
#define BURROW_ENABLE_BOEHM 1      /* build flag; links -lgc */
Alloc *a = gc_alloc();       /* pass this everywhere, never free */
```

Signatures are unchanged — you still pass an allocator, it just happens to be
collecting. This restores Go's exact ergonomics for people who want them:
scripts, prototypes, whole programs that `burrow` owns. It is also what makes
`weak.Pointer`, `runtime.SetFinalizer` and `runtime.AddCleanup` fully
functional rather than approximated. → [01](01-scope.md) §6

Opt-in, off by default, never used by `burrow`'s internals, and never required
to run the test suite — so `burrow` never inherits Boehm's portability surface
as a hard dependency.

### `track` — the debugging allocator

Wraps any other allocator. Records size, alignment, sequence number and call
site per allocation; reports leaks, double frees, wild frees, size and align
mismatches and write-after-free. Faults arrive through a callback as a struct,
so a test asserts on a field rather than parsing text. Used by the test suite,
which is how ownership annotations get validated. Not for production.

Write-after-free needs the freed block to still be readable and still be ours,
so a free poisons the block and holds it in a quarantine instead of passing the
free down, and the poison is checked when the block is finally evicted. That
finds the write late rather than at the moment it happened and does not find
reads at all, which is the price of working on every platform without touching
the page tables. AddressSanitizer covers the reads; this covers the ownership
mistakes AddressSanitizer cannot see, because the interface tells it what the
sizes were supposed to be. The two are complementary and CI runs both.

### `fixed` — a caller-supplied buffer

```c
Byte buf[8192];
Alloc *a = fixed_alloc(buf, sizeof buf);
```

No dynamic allocation at all. Returns `NULL` when exhausted, which surfaces as
`err_out_of_memory`. This is what makes `burrow` usable in firmware and in
allocation-free hot paths, and it is why `alloc` returning `NULL` must be a
handled condition everywhere rather than an abort.

Plus a `mimalloc` shim as an optional drop-in for `heap`. → [02](02-landscape.md) §4

## 6. Where arenas come from in practice

The rule "pass an allocator" raises the practical question of where a user gets
one, and the answer differs by context. `burrow` supplies the right one in each.

| Context | Arena | Lifetime |
| --- | --- | --- |
| `main` | `Alloc *a = arena_alloc(main_arena())` | process |
| A goroutine | `goroutine_arena()` | the goroutine |
| An HTTP request handler | `r->arena` | the request, reset and reused across requests on the same connection |
| A `testing.T` | `t->arena` | one test, per-subtest children |
| A `sql.Rows` scan | `rows->arena` | reset per `Next()` |
| A `fs.WalkDir` callback | `walk->arena` | per entry |
| `time.AfterFunc` callback | its own goroutine arena | the callback |

The HTTP case is the one that proves the design. Go's `net/http` allocates per
request and relies on the GC; `burrow`'s gives each request an arena that is
reset — not freed — when the response completes. So a server handles a million
requests with zero allocator calls to the OS after warm-up, no leaks possible,
and no per-object cleanup code in any handler. That is strictly better than
what Go does, and it is a direct consequence of the explicit-allocator design
rather than a special case bolted on.

Handlers that need data to outlive the request copy it explicitly into a
longer-lived allocator — which is a visible, greppable act rather than an
invisible GC-rooted reference, and is exactly the bug class that leaks memory
in production Go services.

## 7. Failure policy

`alloc` can return `NULL`, and on a fixed allocator it routinely will. The
policy, uniform across the library:

1. **Library internals check every allocation.** No exceptions, enforced by
   `tools/check-alloc.sh` rather than by discipline.
2. **Allocation failure surfaces as `burrow_err_out_of_memory`** through the
   normal error return, in any function that returns an `Error`.
3. **In functions with no error return** — and Go has plenty, e.g.
   `strings.ToUpper` — failure raises a panic
   (`panic(burrow_err_out_of_memory)`), which is recoverable and which unwinds
   the defer chain. This matches Go's behaviour for out-of-memory, which is also
   a non-returnable condition, and it means `strings_to_upper`'s signature does
   not have to grow an error parameter for a condition that essentially never
   happens on a hosted platform. This one waits on the runtime, since there is
   no panic to raise yet.
4. **A per-allocator OOM hook** lets fixed-pool users handle it without
   panicking.
5. **Never abort, never `exit`.** A library that kills the host process is not
   embeddable. `tools/check-banned.sh` is what makes that true rather than
   intended.

### The hook

```c
typedef bool (*OomFunc)(void *ctx, size_t size, size_t align);

void mem_set_oom(Alloc *a, OomFunc fn, void *ctx);
```

The handler is told what was asked for and answers whether it is worth asking
again. Returning true retries the allocation once, which is the answer for a
handler that just made room. Returning false lets the `NULL` through to the
caller, which is the answer for a handler that only wanted to log it. Not
returning at all is also allowed, by `longjmp` or by ending the process, and
that is the host's decision rather than this library's.

Once, not in a loop. A handler that answers true without having freed anything
would spin forever, and above one there is no number of attempts anybody can
defend.

The two fields live on `Alloc` rather than on `AllocVT` because a vtable is
shared by every allocator using it and this is a decision about one allocator.
That grows `Alloc` from two words to four, which is the one visible cost, and it
buys a hook that works on an allocator somebody else wrote without that
allocator knowing the hook exists. `mem_alloc` is the only funnel, so there is
exactly one place where a refusal is noticed.

It fires for a request an allocator refused and for nothing else. A zero sized
request is not a failure, and neither is an element count that overflows when
multiplied by the element size, because no amount of free memory would have
satisfied that one.

`heap_allocator()` hands back a single shared object, so a handler set on it is
set for everything in the process that allocates from the heap. Every other
backend is an object you made, and the handler belongs to that object.

## 8. What Go's GC does that we must explicitly replace

| Go GC service | `burrow` answer |
| --- | --- |
| Reclaiming unreachable data | Arena scope, or the chosen allocator |
| Keeping interface/`any` payloads alive | Caller's responsibility, annotated; `any_box` to copy into an allocator |
| Making `append` growth safe | `slice_append` takes the allocator; old backing array is freed only by `heap`, retained by arenas (so stale slice headers stay valid — safer than Go) |
| `SetFinalizer` / `AddCleanup` | Real under `gc`; arena-teardown callbacks otherwise (`arena_defer`) |
| `weak.Pointer` | Real under `gc`; a checked generational handle otherwise |
| Escape analysis (stack vs heap) | Manual: the porting rules say value types stay on the stack, and `Str`/`Slice` are values |
| Keeping cyclic graphs alive | Arena scope handles cycles trivially — a real advantage over refcounting |
| `runtime.GC()`, `GOGC`, pacing | No-ops under arena; real under `gc`; `MemStats` reports the active allocator's real numbers with `NumGC == 0` |

The cyclic-graph line is worth dwelling on: the graphs that make refcounting
unworkable (`go/ast`, `http.Request`, `net/http.Transport`) are exactly the
ones that are naturally scoped — one parse, one request, one client lifetime.
Arena allocation handles them with no cycle detection and no bookkeeping. The
hard case for arenas, conversely, is a long-lived cache with per-item eviction;
for that, use `heap` or `gc` for the cache and arenas for everything
around it. Mixing allocators per-object is legal and expected.

## 9. Cost

Measured against Go, per the benchmark plan in [16](16-milestones.md) §5. The
expectations that the design is being held to:

| Workload | Expectation vs Go |
| --- | --- |
| String/text processing with an arena | 1.2–2× faster (bump allocation, no write barriers, single-TU inlining) |
| Same workload with `heap` | 0.8–1.1× (malloc-bound) |
| Same workload with `gc` | 0.7–1.0× (Boehm is not Go's collector) |
| JSON decode into a registered struct | 0.9–1.3× |
| HTTP server, per-request arena | 1.1–1.5× on allocation-heavy handlers |
| Anything dominated by syscalls or crypto | ≈1.0× |

If the arena path is not faster than Go on text processing, something is wrong
with the implementation, not the design — that path has no write barriers, no
GC assist, no safepoints and full cross-package inlining.

## 10. The one-paragraph summary for the README

> `burrow` never allocates behind your back. Any function that allocates takes
> an allocator as its first argument, and everything it allocated belongs to
> that allocator. Use an arena and throw it away when you are done — that is
> the intended style, it cannot leak, and it is faster than `malloc`. Use
> `heap` if you want `malloc`/`free`. Use `gc` if you want Go's
> ergonomics exactly and do not mind linking a collector. Use `fixed` if
> you have no heap at all. Errors are the exception: they come from the
> runtime, they are valid until the end of the enclosing error scope, and you
> never free them.

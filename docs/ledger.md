# The fidelity ledger

burrow's claim is that it is the Go standard library, complete, with nothing missing. This page is the only qualification on that claim and it is meant to be read before the claim is believed.

Everything here is a declaration that cannot be a one to one port, because it is a property of the Go compiler and toolchain rather than of a library, or because it needs a garbage collector to mean what Go means by it. Every one of them has a defined substitute and every one of them says plainly whether the substitute is as capable as the original. Eleven of the fifteen are full capability replacements. Four are genuine semantic differences and those are the ones worth your attention.

Out of 23,730 exported declarations, fifteen. That is the whole asterisk.

A symbol with an entry here links to it from its reference page, and the entry links back, so you meet the caveat at the point where it matters rather than afterwards. Adding a sixteenth entry is allowed and it needs the same argument the first fifteen got.

## Full capability substitutes

| Go | Why it cannot port directly | What burrow does |
| --- | --- | --- |
| `unsafe`, all of it | Compiler intrinsics: `Sizeof`, `Offsetof`, `Alignof`, `Pointer`, `Slice`, `String`, `Add` | `sizeof`, `offsetof` and `_Alignof` are native C and do the same thing. `unsafe_slice` and `unsafe_string` are real functions. Nothing is lost. |
| `embed.FS` | `//go:embed` is a compiler directive | `BURROW_EMBED_FILE(sym, path)` using C23's `#embed`, with `burrow-gen embed` emitting a C array for C11 toolchains that do not have it. Produces a real `EmbedFs` that satisfies `Fs`. |
| `runtime/cgo` | It is the boundary between Go and C, and in C there is no boundary | The header exists and all five declarations are no-ops that report success. |
| `runtime/race` | Needs compiler instrumentation | Maps to ThreadSanitizer. Full functionality, and it needs a rebuild, which is exactly the deal Go's `-race` offers. |
| `runtime/coverage` | Needs compiler instrumentation | Maps to `-fprofile-instr-generate` and gcov, and writes `covmeta` and `covcounters` files that `go tool covdata` can read. |
| `iter.Seq`, `iter.Seq2` | Range over function is compiler syntax | Function pointer and yield callback types, plus `BURROW_RANGE(...)` which gives you `for` loop syntax over an `IterSeq`. The ergonomics differ, the capability does not. |
| `runtime.MemStats` GC fields | There is no Go garbage collector here | Filled in with the active allocator's real statistics. The field layout is identical so existing tooling parses it. `NumGC` is zero under an arena, which is true rather than a placeholder. |
| `structs.HostLayout` | Compiler layout directive | A zero size marker struct. C layout is already host layout, so this is correct by doing nothing. |
| `go/build` toolchain queries | Shells out to the `go` command | Works when a Go toolchain is on `PATH` and returns `build_err_no_toolchain` when it is not. The pure parts, constraint evaluation and file matching, are complete either way. |
| `testing/synctest` | Needs the Go scheduler's idle detection | Real, because burrow owns its scheduler. Arguably cleaner than Go's, since the idle condition is something we can observe directly. |
| `reflect` over registered types | C has no runtime type information | Complete for any type declared through the descriptor macro or the generator. See the next table for what happens otherwise. |

## Genuine differences

These four do not do what Go does, and no amount of engineering will change that.

### Inserting into a map while ranging over it

Go's map is a directory of smaller tables and it grows by splitting one of them, so an insert that grows the map leaves the other entries where they are and a `range` already in progress keeps working. burrow's map is a single Swiss table that doubles and reinserts, which is what Abseil does and what makes the lookup path as tight as it is, and a growth therefore moves every entry.

So an insert that grows the table while an iterator is live makes `map_next` stop the program with `map grew during iteration`. Deleting during a range is fine, and so is an insert that does not grow the table, which is the overwhelming majority of them.

Go's own specification says the entries produced after an insert during a range are unspecified, so a program this catches is a program that was already relying on something Go does not promise. What Go does have is a collector, which lets it keep the displaced table alive for exactly as long as some iterator can still see it. Without one, the two available answers are handing out pointers into memory that was freed, or never freeing it, and reporting the bug beats both.

If you need to insert while walking, collect the keys first and walk those. The rest of the iteration rules, which are Go's, are in [guides/maps.md](guides/maps.md).

### `plugin`

Go's `plugin` package requires Go's dynamic linking and, more importantly, type identity that holds across separately compiled objects. C has neither.

burrow gives you `plugin_open` and `plugin_lookup` backed by `dlopen` and `LoadLibrary`. Type identity checks become descriptor hash checks, which catch a mismatched struct layout but will not catch two structurally identical types that were meant to be distinct. If you rely on `plugin` for type safety rather than for loading code, burrow is weaker than Go and you should know that before you depend on it.

### `weak.Pointer`, `runtime.SetFinalizer`, `runtime.AddCleanup`

These need a tracing collector to mean what Go means by them.

Under the Boehm allocator backend they are real and behave as Go does. Under the arena and malloc backends, `weak.Pointer` is a checked handle into a generational slot table, which works and gives you the safety property you wanted, but the liveness rules are different: a weak pointer is cleared when its arena is torn down rather than when the object becomes unreachable. Finalizers run at arena teardown for the same reason.

This is documented per backend rather than once, because the answer genuinely depends on which allocator you chose.

### `reflect` on an unregistered type

`reflect` is complete over types declared with `STRUCT(...)` or produced by `burrow-gen reflect`. Handed a pointer to a plain C struct it has never seen, `reflect_type_of` returns `NULL` and an error you can diagnose, rather than guessing.

Go can reflect over any type because the compiler emits type information for all of them. burrow has no compiler, so it can only see what was declared to it. This is the one place where the port is structurally less capable than the original and there is no version of this project where it is not.

### `simd/archsimd`

Deferred to after 1.0 rather than substituted. It is new in Go 1.27, its API is explicitly unstable, and porting an unstable compiler intrinsic surface is work that gets thrown away. Tracked as a gap rather than solved, and it is the one entry here that is expected to leave this page eventually.
